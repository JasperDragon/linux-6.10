// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Procedures for maintaining information about logical memory blocks.
 *
 * Peter Bergner, IBM Corp.	June 2001.
 * Copyright (C) 2001 Peter Bergner.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/poison.h>
#include <linux/pfn.h>
#include <linux/debugfs.h>
#include <linux/kmemleak.h>
#include <linux/seq_file.h>
#include <linux/memblock.h>
#include <linux/mutex.h>
#include <linux/string_helpers.h>

#ifdef CONFIG_KEXEC_HANDOVER
#include <linux/libfdt.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/memblock.h>
#endif /* CONFIG_KEXEC_HANDOVER */

#include <asm/sections.h>
#include <linux/io.h>

#include "internal.h"

#define INIT_MEMBLOCK_REGIONS			128
#define INIT_PHYSMEM_REGIONS			4

#ifndef INIT_MEMBLOCK_RESERVED_REGIONS
# define INIT_MEMBLOCK_RESERVED_REGIONS		INIT_MEMBLOCK_REGIONS
#endif

#ifndef INIT_MEMBLOCK_MEMORY_REGIONS
#define INIT_MEMBLOCK_MEMORY_REGIONS		INIT_MEMBLOCK_REGIONS
#endif

/*
 * ============================================================================
 * memblock.c — 内核启动早期物理内存管理器
 * ============================================================================
 *
 * 【定位】memblock 是 Linux 最早期的内存分配器，在 buddy allocator（伙伴分配器）
 * 启动之前运行。它直接管理物理地址空间，使用简单但高效的区域（region）模型。
 *
 * 【为什么需要 memblock】
 *   内核启动早期，buddy allocator 尚未初始化：没有页表、没有 freelist、
 *   没有 percpu 缓存。但内核需要分配 page table、percpu 区域、SPARSEMEM
 *   元数据等。memblock 就是为满足这些"boot time"分配需求而生的。
 *
 * 【数据模型：三个区域集合】
 *
 *   struct memblock {
 *       .memory     → "哪些物理内存可用"（从 firmware/DT 获取）
 *       .reserved   → "哪些已被占用"（内核镜像、initrd、已分配块等）
 *       .physmem    → "实际存在的物理内存"（不考虑 mem= 限制，仅部分架构)
 *   }
 *
 *   空闲内存 = memory 中不在 reserved 中的部分
 *   每个集合最多 128 个区域（初始），可通过 memblock_double_array() 动态扩容。
 *
 * 【核心算法：__next_mem_range() 迭代器】
 *
 *   绝大多数 memblock 操作基于此迭代器。它在 memory 和 reserved 两个
 *   有序数组间做双指针归并扫描，找出 memory 中未被 reserved 覆盖的区域。
 *
 *   原理（类似归并排序的 merge 步骤）：
 *     idx_a 遍历 memory, idx_b 遍历 reserved (两者都按地址有序)
 *     - 若 memory 完全在 reserved 之前 → 返回整块 memory, idx_a++
 *     - 若 memory 完全在 reserved 之后 → idx_b++ (跳过此保留区)
 *     - 若有重叠 → 返回重叠前的部分, 推进先结束的那一侧
 *
 *   __next_mem_range() 被封装为多个 for_each_* 宏：
 *   - for_each_free_mem_range()    — 遍历空闲区域（memory \ reserved）
 *   - for_each_mem_range()         — 遍历所有 memory（忽略 reserved）
 *   - for_each_mem_range_rev()     — 反向遍历（top-down 分配使用）
 *   - for_each_reserved_mem_range() — 遍历 reserved
 *
 * 【分配策略：top-down vs bottom-up】
 *
 *   top-down（默认）: 从高地址向下搜索 → 减少低地址碎片
 *   bottom-up:       从低地址向上搜索 → 避免与 initrd/kernel 区域冲突
 *   可通过 memblock_set_bottom_up(true) 切换。
 *
 * 【区域合并与有序性保证】
 *   每次 memblock_add/reserve 时自动合并相邻/重叠区域，保证 regions[]
 *   数组始终按 base 地址有序且无重叠。这是 __next_mem_range() 算法
 *   正确性的基础。
 *
 * 【生命周期】
 *   setup_arch()
 *     └→ memblock_add()         — 注册物理内存
 *     └→ memblock_reserve()     — 保留内核/initrd/DTB 等
 *   paging_init() → memblock_alloc() — 分配 page table
 *   mm_init()
 *     └→ memblock_free_all()    — 将空闲内存释放给 buddy allocator
 *     └→ memblock_discard()     — 释放 memblock 元数据自身
 *
 * 【关键全局变量】
 *   struct memblock memblock  — 全局单例（__initdata_memblock，init 完成后释放）
 *   max_pfn / max_low_pfn     — 最大页框号
 *   memblock.current_limit    — 分配地址上限（=MEMBLOCK_ALLOC_ANYWHERE 表示无限制）
 */

/**
 * DOC: memblock overview
 *
 * Memblock is a method of managing memory regions during the early
 * boot period when the usual kernel memory allocators are not up and
 * running.
 *
 * Memblock views the system memory as collections of contiguous
 * regions. There are several types of these collections:
 *
 * * ``memory`` - describes the physical memory available to the
 *   kernel; this may differ from the actual physical memory installed
 *   in the system, for instance when the memory is restricted with
 *   ``mem=`` command line parameter
 * * ``reserved`` - describes the regions that were allocated
 * * ``physmem`` - describes the actual physical memory available during
 *   boot regardless of the possible restrictions and memory hot(un)plug;
 *   the ``physmem`` type is only available on some architectures.
 *
 * Each region is represented by struct memblock_region that
 * defines the region extents, its attributes and NUMA node id on NUMA
 * systems. Every memory type is described by the struct memblock_type
 * which contains an array of memory regions along with
 * the allocator metadata. The "memory" and "reserved" types are nicely
 * wrapped with struct memblock. This structure is statically
 * initialized at build time. The region arrays are initially sized to
 * %INIT_MEMBLOCK_MEMORY_REGIONS for "memory" and
 * %INIT_MEMBLOCK_RESERVED_REGIONS for "reserved". The region array
 * for "physmem" is initially sized to %INIT_PHYSMEM_REGIONS.
 * The memblock_allow_resize() enables automatic resizing of the region
 * arrays during addition of new regions. This feature should be used
 * with care so that memory allocated for the region array will not
 * overlap with areas that should be reserved, for example initrd.
 *
 * The early architecture setup should tell memblock what the physical
 * memory layout is by using memblock_add() or memblock_add_node()
 * functions. The first function does not assign the region to a NUMA
 * node and it is appropriate for UMA systems. Yet, it is possible to
 * use it on NUMA systems as well and assign the region to a NUMA node
 * later in the setup process using memblock_set_node(). The
 * memblock_add_node() performs such an assignment directly.
 *
 * Once memblock is setup the memory can be allocated using one of the
 * API variants:
 *
 * * memblock_phys_alloc*() - these functions return the **physical**
 *   address of the allocated memory
 * * memblock_alloc*() - these functions return the **virtual** address
 *   of the allocated memory.
 *
 * Note, that both API variants use implicit assumptions about allowed
 * memory ranges and the fallback methods. Consult the documentation
 * of memblock_alloc_internal() and memblock_alloc_range_nid()
 * functions for more elaborate description.
 *
 * As the system boot progresses, the architecture specific mem_init()
 * function frees all the memory to the buddy page allocator.
 *
 * Unless an architecture enables %CONFIG_ARCH_KEEP_MEMBLOCK, the
 * memblock data structures (except "physmem") will be discarded after the
 * system initialization completes.
 */

#ifndef CONFIG_NUMA
struct pglist_data __refdata contig_page_data;
EXPORT_SYMBOL(contig_page_data);
#endif

unsigned long max_low_pfn;
unsigned long min_low_pfn;
unsigned long max_pfn;
unsigned long long max_possible_pfn;

#ifdef CONFIG_MEMBLOCK_KHO_SCRATCH
/* When set to true, only allocate from MEMBLOCK_KHO_SCRATCH ranges */
static bool kho_scratch_only;
#else
#define kho_scratch_only false
#endif

static struct memblock_region memblock_memory_init_regions[INIT_MEMBLOCK_MEMORY_REGIONS] __initdata_memblock;
static struct memblock_region memblock_reserved_init_regions[INIT_MEMBLOCK_RESERVED_REGIONS] __initdata_memblock;
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
static struct memblock_region memblock_physmem_init_regions[INIT_PHYSMEM_REGIONS];
#endif

/*
 * memblock 全局单例的静态初始化。
 * __initdata_memblock 标记表示该结构体内存在内核初始化完成后可被丢弃
 * （除非配置了 CONFIG_ARCH_KEEP_MEMBLOCK）。
 */
struct memblock memblock __initdata_memblock = {
	/*
	 * .memory 成员描述系统中所有可用的物理内存。
	 * 这是 memblock 分配器的"资源池"——所有分配都从这里取。
	 * regions 初始指向静态分配的 memblock_memory_init_regions[128]，
	 * 当 region 数量超过 128 时会被动态扩容。
	 */
	.memory.regions		= memblock_memory_init_regions,
	.memory.max		= INIT_MEMBLOCK_MEMORY_REGIONS,
	.memory.name		= "memory",

	/*
	 * .reserved 成员跟踪已被占用的物理内存区域。
	 * 包括：内核镜像、initrd、设备树、页表等已分配的内存。
	 * 空闲内存 = memory 中不在 reserved 中的部分。
	 * 同样初始使用静态数组，后续可动态扩容。
	 */
	.reserved.regions	= memblock_reserved_init_regions,
	.reserved.max		= INIT_MEMBLOCK_RESERVED_REGIONS,
	.reserved.name		= "reserved",

	/*
	 * bottom_up = false 表示默认采用 top-down 分配策略。
	 * top-down：从高地址向低地址分配，有利于减少低地址碎片，
	 * 并确保低地址区域留给 DMA 等有特殊地址要求的设备。
	 * 可通过 memblock_set_bottom_up(true) 切换为 bottom-up。
	 */
	.bottom_up		= false,
	/*
	 * current_limit 限制分配的地址上限。
	 * MEMBLOCK_ALLOC_ANYWHERE (~0) 表示无限制。
	 * 内核启动过程中，memblock_set_current_limit() 会将其
	 * 调整为实际可访问的物理地址范围（如 lowmem 上限）。
	 */
	.current_limit		= MEMBLOCK_ALLOC_ANYWHERE,
};

#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
struct memblock_type physmem = {
	.regions		= memblock_physmem_init_regions,
	.max			= INIT_PHYSMEM_REGIONS,
	.name			= "physmem",
};
#endif

/*
 * keep a pointer to &memblock.memory in the text section to use it in
 * __next_mem_range() and its helpers.
 *  For architectures that do not keep memblock data after init, this
 * pointer will be reset to NULL at memblock_discard()
 */
static __refdata struct memblock_type *memblock_memory = &memblock.memory;

#define for_each_memblock_type(i, memblock_type, rgn)			\
	for (i = 0, rgn = &memblock_type->regions[0];			\
	     i < memblock_type->cnt;					\
	     i++, rgn = &memblock_type->regions[i])

#define memblock_dbg(fmt, ...)						\
	do {								\
		if (memblock_debug)					\
			pr_info(fmt, ##__VA_ARGS__);			\
	} while (0)

static int memblock_debug __initdata_memblock;
static bool system_has_some_mirror __initdata_memblock;
static int memblock_can_resize __initdata_memblock;
static int memblock_memory_in_slab __initdata_memblock;
static int memblock_reserved_in_slab __initdata_memblock;

bool __init_memblock memblock_has_mirror(void)
{
	return system_has_some_mirror;
}

static enum memblock_flags __init_memblock choose_memblock_flags(void)
{
	/* skip non-scratch memory for kho early boot allocations */
	if (kho_scratch_only)
		return MEMBLOCK_KHO_SCRATCH;

	return system_has_some_mirror ? MEMBLOCK_MIRROR : MEMBLOCK_NONE;
}

/* adjust *@size so that (@base + *@size) doesn't overflow, return new size */
static inline phys_addr_t memblock_cap_size(phys_addr_t base, phys_addr_t *size)
{
	return *size = min(*size, PHYS_ADDR_MAX - base);
}

/*
 * Address comparison utilities
 */
unsigned long __init_memblock
memblock_addrs_overlap(phys_addr_t base1, phys_addr_t size1, phys_addr_t base2,
		       phys_addr_t size2)
{
	return ((base1 < (base2 + size2)) && (base2 < (base1 + size1)));
}

bool __init_memblock memblock_overlaps_region(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size)
{
	unsigned long i;

	memblock_cap_size(base, &size);

	for (i = 0; i < type->cnt; i++)
		if (memblock_addrs_overlap(base, size, type->regions[i].base,
					   type->regions[i].size))
			return true;
	return false;
}

/**
 * __memblock_find_range_bottom_up - find free area utility in bottom-up
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Utility called from memblock_find_in_range_node(), find free area bottom-up.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock
__memblock_find_range_bottom_up(phys_addr_t start, phys_addr_t end,
				phys_addr_t size, phys_addr_t align, int nid,
				enum memblock_flags flags)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	/*
	 * 从低地址到高地址遍历所有空闲内存区域。
	 * for_each_free_mem_range 内部调用 __next_mem_range()，使用双指针归并算法
	 * 扫描 memory 数组和 reserved 数组，生成 "memory 中有而 reserved 中没有" 的区间。
	 */
	for_each_free_mem_range(i, nid, flags, &this_start, &this_end, NULL) {
		/*
		 * 将当前空闲区间裁剪到 [start, end) 范围内。
		 * clamp(x, lo, hi) 返回 lo <= x <= hi 的值。
		 * 如果区间完全在 [start, end) 之外，裁剪后 this_start >= this_end，
		 * 后续的检查会自动跳过。
		 */
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		/*
		 * 以对齐要求向上取整起始地址。
		 * 例如: this_start=0x1000, align=0x1000 → cand=0x1000
		 *       this_start=0x1100, align=0x1000 → cand=0x2000
		 */
		cand = round_up(this_start, align);
		/*
		 * 检查对齐后的候选地址是否仍在区间内，
		 * 并且剩余空间是否足够容纳要求的 size。
		 * 注意: cand < this_end 防止整数溢出；this_end - cand >= size 保证空间足够。
		 */
		if (cand < this_end && this_end - cand >= size)
			return cand;
	}

	/* 未找到合适区域，返回 0（分配失败） */
	return 0;
}

/**
 * __memblock_find_range_top_down - find free area utility, in top-down
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Utility called from memblock_find_in_range_node(), find free area top-down.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock
__memblock_find_range_top_down(phys_addr_t start, phys_addr_t end,
			       phys_addr_t size, phys_addr_t align, int nid,
			       enum memblock_flags flags)
{
	phys_addr_t this_start, this_end, cand;
	u64 i;

	/*
	 * 从高地址到低地址反向遍历所有空闲内存区域。
	 * for_each_free_mem_range_reverse 内部调用 __next_mem_range_rev()，
	 * 与 bottom-up 方向相反：从 memory 数组的末尾开始向前扫描。
	 *
	 * 与 bottom-up 的关键区别：
	 *   1. 迭代方向相反（反向）
	 *   2. 使用 round_down 而非 round_up（从区间尾部向前对齐）
	 *   3. 先检查区间是否有足够空间（this_end < size 则跳过）
	 */
	for_each_free_mem_range_reverse(i, nid, flags, &this_start, &this_end,
					NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);

		/*
		 * 如果区间长度本身就不够 size，直接跳过。
		 * （top-down 特有检查：bottom-up 不提前检查总长度，
		 *  而是通过 round_up 后的 cand < this_end 隐式检查）
		 */
		if (this_end < size)
			continue;

		/*
		 * 从区间尾部向前对齐：cand = (this_end - size) 向下对齐到 align。
		 * 例如: this_end=0x5000, size=0x1000, align=0x1000
		 *       → cand = round_down(0x4000, 0x1000) = 0x4000
		 * 这样分配的区域 [cand, cand+size) 会紧贴区间尾部，
		 * 有利于减少低地址碎片。
		 */
		cand = round_down(this_end - size, align);
		/*
		 * 检查候选地址是否 >= 区间起始地址（即对齐后的候选地址仍在区间内）。
		 * 如果 cand < this_start，说明对齐后候选地址落在了区间之前，
		 * 此区间不满足条件，继续扫描下一个区间。
		 */
		if (cand >= this_start)
			return cand;
	}

	return 0;
}

/**
 * memblock_find_in_range_node - find free area in given range and node
 * @size: size of free area to find
 * @align: alignment of free area to find
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @flags: pick from blocks based on memory attributes
 *
 * Find @size free area aligned to @align in the specified range and node.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock memblock_find_in_range_node(phys_addr_t size,
					phys_addr_t align, phys_addr_t start,
					phys_addr_t end, int nid,
					enum memblock_flags flags)
{
	/* pump up @end */
	if (end == MEMBLOCK_ALLOC_ACCESSIBLE ||
	    end == MEMBLOCK_ALLOC_NOLEAKTRACE)
		end = memblock.current_limit;

	/* avoid allocating the first page */
	start = max_t(phys_addr_t, start, PAGE_SIZE);
	end = max(start, end);

	if (memblock_bottom_up())
		return __memblock_find_range_bottom_up(start, end, size, align,
						       nid, flags);
	else
		return __memblock_find_range_top_down(start, end, size, align,
						      nid, flags);
}

/**
 * memblock_find_in_range - find free area in given range
 * @start: start of candidate range
 * @end: end of candidate range, can be %MEMBLOCK_ALLOC_ANYWHERE or
 *       %MEMBLOCK_ALLOC_ACCESSIBLE
 * @size: size of free area to find
 * @align: alignment of free area to find
 *
 * Find @size free area aligned to @align in the specified range.
 *
 * Return:
 * Found address on success, 0 on failure.
 */
static phys_addr_t __init_memblock memblock_find_in_range(phys_addr_t start,
					phys_addr_t end, phys_addr_t size,
					phys_addr_t align)
{
	phys_addr_t ret;
	enum memblock_flags flags = choose_memblock_flags();

again:
	ret = memblock_find_in_range_node(size, align, start, end,
					    NUMA_NO_NODE, flags);

	if (!ret && (flags & MEMBLOCK_MIRROR)) {
		pr_warn_ratelimited("Could not allocate %pap bytes of mirrored memory\n",
			&size);
		flags &= ~MEMBLOCK_MIRROR;
		goto again;
	}

	return ret;
}

static void __init_memblock memblock_remove_region(struct memblock_type *type, unsigned long r)
{
	/* 从 total_size 中减去被移除 region 的大小 */
	type->total_size -= type->regions[r].size;
	/*
	 * 使用 memmove 将 r+1 之后的所有 region 向前移动一个位置，
	 * 覆盖掉第 r 个 region。memmove 处理了重叠问题（当 src 和 dst
	 * 区域重叠时也能正确工作）。
	 */
	memmove(&type->regions[r], &type->regions[r + 1],
		(type->cnt - (r + 1)) * sizeof(type->regions[r]));
	type->cnt--;

	/* 如果数组变空了，将其重置为一个空的初始状态 */
	if (type->cnt == 0) {
		WARN_ON(type->total_size != 0);
		type->regions[0].base = 0;
		type->regions[0].size = 0;
		type->regions[0].flags = 0;
		memblock_set_region_node(&type->regions[0], MAX_NUMNODES);
	}
}

#ifndef CONFIG_ARCH_KEEP_MEMBLOCK
/**
 * memblock_discard - discard memory and reserved arrays if they were allocated
 */
void __init memblock_discard(void)
{
	phys_addr_t size;
	void *addr;

	if (memblock.reserved.regions != memblock_reserved_init_regions) {
		addr = memblock.reserved.regions;
		size = PAGE_ALIGN(sizeof(struct memblock_region) *
				  memblock.reserved.max);
		if (memblock_reserved_in_slab)
			kfree(addr);
		else
			memblock_free(addr, size);
	}

	if (memblock.memory.regions != memblock_memory_init_regions) {
		addr = memblock.memory.regions;
		size = PAGE_ALIGN(sizeof(struct memblock_region) *
				  memblock.memory.max);
		if (memblock_memory_in_slab)
			kfree(addr);
		else
			memblock_free(addr, size);
	}

	memblock_memory = NULL;
}
#endif

/**
 * memblock_double_array - double the size of the memblock regions array
 * @type: memblock type of the regions array being doubled
 * @new_area_start: starting address of memory range to avoid overlap with
 * @new_area_size: size of memory range to avoid overlap with
 *
 * Double the size of the @type regions array. If memblock is being used to
 * allocate memory for a new reserved regions array and there is a previously
 * allocated memory range [@new_area_start, @new_area_start + @new_area_size]
 * waiting to be reserved, ensure the memory used by the new array does
 * not overlap.
 *
 * Return:
 * 0 on success, -1 on failure.
 */
/*
 * memblock_double_array — 将 memblock region 数组容量翻倍
 *
 * 这是 memblock 中最微妙的自引用分配场景：
 * memblock 自身的 region 数组满了需要扩容，但扩容所需的新数组
 * 又需要通过 memblock 来分配。
 *
 * 尤其是当 reserved 数组满时情况最复杂：
 *   1. 新数组需要被 reserve 以避免后续分配覆盖
 *   2. 但旧的 reserved 数组已满，无法插入新的 reserve 记录！
 *   3. 解法：先 find（不 reserve），再 memcpy 切换指针，
 *      最后 reserve（此时新数组已就位，操作可正常进行）。
 */
static int __init_memblock memblock_double_array(struct memblock_type *type,
						phys_addr_t new_area_start,
						phys_addr_t new_area_size)
{
	struct memblock_region *new_array, *old_array;
	phys_addr_t old_alloc_size, new_alloc_size;
	phys_addr_t old_size, new_size, addr, new_end;
	int use_slab = slab_is_available();
	int *in_slab;

	/* We don't allow resizing until we know about the reserved regions
	 * of memory that aren't suitable for allocation
	 */
	if (!memblock_can_resize)
		panic("memblock: cannot resize %s array\n", type->name);

	/* Calculate new doubled size */
	old_size = type->max * sizeof(struct memblock_region);
	new_size = old_size << 1;
	/*
	 * We need to allocated new one align to PAGE_SIZE,
	 *   so we can free them completely later.
	 */
	old_alloc_size = PAGE_ALIGN(old_size);
	new_alloc_size = PAGE_ALIGN(new_size);

	/* Retrieve the slab flag */
	if (type == &memblock.memory)
		in_slab = &memblock_memory_in_slab;
	else
		in_slab = &memblock_reserved_in_slab;

	/*
	 * === 分配新数组 ===
	 *
	 * 如果 slab 分配器已就绪（use_slab == true），使用 kmalloc。
	 * kmalloc 分配的内存由 slab 管理，memblock 无需追踪，
	 * 完美避免了自引用问题。
	 *
	 * 如果 slab 尚未就绪，需要通过 memblock 自身来找空闲内存：
	 * 先 memblock_find_in_range （只查找，不 reserve），
	 * 后续再手动 reserve。
	 */
	if (use_slab) {
		/* slab 可用时直接用 kmalloc，简单可靠 */
		new_array = kmalloc(new_size, GFP_KERNEL);
		addr = new_array ? __pa(new_array) : 0;
	} else {
		/* only exclude range when trying to double reserved.regions */
		/*
		 * 只有在扩容 reserved 数组时才需要排除 new_area 范围。
		 * new_area 是调用者（memblock_add_range）正在添加的新区域，
		 * 避免新数组与它重叠。如果扩容的是 memory 数组则无需排除。
		 */
		if (type != &memblock.reserved)
			new_area_start = new_area_size = 0;

		/*
		 * 先尝试在 new_area 之后分配（避免与正在添加的新区域重叠）。
		 * 如果失败且 new_area_size > 0，再尝试在 new_area 之前分配。
		 */
		addr = memblock_find_in_range(new_area_start + new_area_size,
						memblock.current_limit,
						new_alloc_size, PAGE_SIZE);
		if (!addr && new_area_size)
			addr = memblock_find_in_range(0,
				min(new_area_start, memblock.current_limit),
				new_alloc_size, PAGE_SIZE);

		if (addr) {
			/* The memory may not have been accepted, yet. */
			accept_memory(addr, new_alloc_size);

			new_array = __va(addr);
		} else {
			new_array = NULL;
		}
	}
	if (!addr) {
		pr_err("memblock: Failed to double %s array from %ld to %ld entries !\n",
		       type->name, type->max, type->max * 2);
		return -1;
	}

	new_end = addr + new_size - 1;
	memblock_dbg("memblock: %s is doubled to %ld at [%pa-%pa]",
			type->name, type->max * 2, &addr, &new_end);

	/*
	 * Found space, we now need to move the array over before we add the
	 * reserved region since it may be our reserved array itself that is
	 * full.
	 */
	/*
	 * === 数据迁移与指针切换 ===
	 *
	 * 这是整个扩容流程的核心步骤，顺序至关重要：
	 *
	 * 1. memcpy: 将旧数组全部数据复制到新数组
	 * 2. memset: 清空新数组的后半部分（新增的容量，所有字段为 0）
	 * 3. old_array = type->regions: 保存旧指针以便后续释放
	 * 4. type->regions = new_array: 切换为新数组（此时新数组正式生效）
	 * 5. type->max <<= 1: 容量翻倍
	 *
	 * 为什么在 reserve 之前就必须切换指针？
	 *   如果是 reserved 数组扩容，只有新指针生效后，
	 *   type 才有足够的空闲槽位来接收后续的 reserve 操作。
	 *   这是典型的"提着鞋带把自己拉起来"（bootstrapping）技巧。
	 */
	memcpy(new_array, type->regions, old_size);
	memset(new_array + type->max, 0, old_size);
	old_array = type->regions;
	type->regions = new_array;
	type->max <<= 1;

	/* 根据旧数组的来源选择释放方式 */
	if (*in_slab)
		kfree(old_array);
	else if (old_array != memblock_memory_init_regions &&
		 old_array != memblock_reserved_init_regions)
		memblock_free(old_array, old_alloc_size);

	/*
	 * Reserve the new array if that comes from the memblock.  Otherwise, we
	 * needn't do it
	 */
	/*
	 * 如果新数组来自 memblock 分配（非 slab），必须立即 reserve。
	 * 否则后续的 memblock_alloc() 可能分配到这块内存，导致数据覆盖！
	 *
	 * 使用 BUG_ON 是因为：如果这里 reserve 失败，系统无法继续安全运行
	 * （新数组的内存完整性无法保证）。
	 *
	 * 注意：此时新数组已切换为生效状态，所以 reserve 操作
	 * 能够正常插入记录——这正是上面先切换指针的原因。
	 */
	if (!use_slab)
		BUG_ON(memblock_reserve_kern(addr, new_alloc_size));

	/* Update slab flag */
	/*
	 * 记录新数组的分配方式，供后续操作参考：
	 * - memblock_discard(): 根据此标志选择 kfree 或 memblock_free
	 * - 再次扩容时：根据此标志决定下次的分配策略
	 */
	*in_slab = use_slab;

	return 0;
}

/**
 * memblock_merge_regions - merge neighboring compatible regions
 * @type: memblock type to scan
 * @start_rgn: start scanning from (@start_rgn - 1)
 * @end_rgn: end scanning at (@end_rgn - 1)
 * Scan @type and merge neighboring compatible regions in [@start_rgn - 1, @end_rgn)
 */
static void __init_memblock memblock_merge_regions(struct memblock_type *type,
						   unsigned long start_rgn,
						   unsigned long end_rgn)
{
	int i = 0;
	if (start_rgn)
		i = start_rgn - 1;
	end_rgn = min(end_rgn, type->cnt - 1);
	while (i < end_rgn) {
		struct memblock_region *this = &type->regions[i];
		struct memblock_region *next = &type->regions[i + 1];

		if (this->base + this->size != next->base ||
		    memblock_get_region_node(this) !=
		    memblock_get_region_node(next) ||
		    this->flags != next->flags) {
			BUG_ON(this->base + this->size > next->base);
			i++;
			continue;
		}

		this->size += next->size;
		/* move forward from next + 1, index of which is i + 2 */
		memmove(next, next + 1, (type->cnt - (i + 2)) * sizeof(*next));
		type->cnt--;
		end_rgn--;
	}
}

/**
 * memblock_insert_region - insert new memblock region
 * @type:	memblock type to insert into
 * @idx:	index for the insertion point
 * @base:	base address of the new region
 * @size:	size of the new region
 * @nid:	node id of the new region
 * @flags:	flags of the new region
 *
 * Insert new memblock region [@base, @base + @size) into @type at @idx.
 * @type must already have extra room to accommodate the new region.
 */
static void __init_memblock memblock_insert_region(struct memblock_type *type,
						   int idx, phys_addr_t base,
						   phys_addr_t size,
						   int nid,
						   enum memblock_flags flags)
{
	struct memblock_region *rgn = &type->regions[idx];

	BUG_ON(type->cnt >= type->max);
	memmove(rgn + 1, rgn, (type->cnt - idx) * sizeof(*rgn));
	rgn->base = base;
	rgn->size = size;
	rgn->flags = flags;
	memblock_set_region_node(rgn, nid);
	type->cnt++;
	type->total_size += size;
}

/**
 * memblock_add_range - add new memblock region
 * @type: memblock type to add new region into
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 * @flags: flags of the new region
 *
 * Add new memblock region [@base, @base + @size) into @type.  The new region
 * is allowed to overlap with existing ones - overlaps don't affect already
 * existing regions.  @type is guaranteed to be minimal (all neighbouring
 * compatible regions are merged) after the addition.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
static int __init_memblock memblock_add_range(struct memblock_type *type,
				phys_addr_t base, phys_addr_t size,
				int nid, enum memblock_flags flags)
{
	bool insert = false;
	phys_addr_t obase = base;
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int idx, nr_new, start_rgn = -1, end_rgn;
	struct memblock_region *rgn;

	if (!size)
		return 0;

	/* 空数组特例：直接初始化第一个 region */
	if (type->regions[0].size == 0) {
		WARN_ON(type->cnt != 0 || type->total_size);
		type->regions[0].base = base;
		type->regions[0].size = size;
		type->regions[0].flags = flags;
		memblock_set_region_node(&type->regions[0], nid);
		type->total_size = size;
		type->cnt = 1;
		return 0;
	}

	/*
	 * 判断数组容量是否足够直接插入。
	 *
	 * 最坏情况：新区域与所有现有 region 都重叠，
	 * 每个重叠处最多产生一个新插入，总共需要 type->cnt + 1 个空槽。
	 *
	 * 技巧：如果 type->cnt * 2 + 1 <= type->max，说明空闲槽
	 * 至少为 cnt+1（因为 max - cnt >= cnt+1），足够直接插入。
	 * 否则需要先扩容再插入。
	 *
	 * 为什么是 type->cnt * 2 + 1 而不是 type->cnt + 1？
	 * 因为 type->max 可能只比 type->cnt 大一点，不够容纳 cnt+1 个新区域。
	 * 用 cnt*2+1 来比较可以快速判断是否足够宽松。
	 */
	if (type->cnt * 2 + 1 <= type->max)
		insert = true;

repeat:
	/*
	 * 两遍扫描设计（two-pass approach）：
	 *
	 * 第一遍 (insert == false):
	 *   只做统计，数出实际需要插入多少个 region (nr_new)，
	 *   不修改 regions 数组内容。
	 *
	 * 第二遍 (insert == true):
	 *   根据第一遍的统计结果，执行实际的插入操作。
	 *
	 * 如果第一遍发现容量不足（nr_new 太大），调用 memblock_double_array()
	 * 扩容后 goto repeat 重新执行第二遍。
	 */
	base = obase;
	nr_new = 0;

	for_each_memblock_type(idx, type, rgn) {
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		/*
		 * 当前 region 完全在新区域之后：
		 * regions 数组按地址升序排列，后面的 region 地址更大，
		 * 不再可能与新区域重叠，break。
		 */
		if (rbase >= end)
			break;
		/*
		 * 当前 region 完全在新区域之前：
		 * 尚未重叠，继续检查下一个 region。
		 */
		if (rend <= base)
			continue;
		/*
		 * 到此：rbase < end 且 rend > base，即重叠。
		 *
		 * 如果 rbase > base，则 [base, rbase) 是新区域中
		 * 没有被当前 region 覆盖的头部部分，需要单独插入。
		 */
		if (rbase > base) {
#ifdef CONFIG_NUMA
			WARN_ON(nid != memblock_get_region_node(rgn));
#endif
			WARN_ON(flags != MEMBLOCK_NONE && flags != rgn->flags);
			nr_new++;
			if (insert) {
				if (start_rgn == -1)
					start_rgn = idx;
				end_rgn = idx + 1;
				/*
				 * 在 idx 位置插入 [base, rbase) 区间，
				 * 插入后 idx++ 因为后面的 region 都被推后了一位，
				 * 需要跳过刚刚插入的 region 和原来的 rgn。
				 */
				memblock_insert_region(type, idx++, base,
						       rbase - base, nid,
						       flags);
			}
		}
		/*
		 * 推进 base 越过当前 region 的覆盖范围。
		 * 这是"截断"算法的关键步骤：
		 * 每次处理一个重叠后，将新区域的起始地址向前推进，
		 * 逐步消耗新区域，直到完全处理完。
		 */
		base = min(rend, end);
	}

	/* 遍历完所有现有 region 后，如果 base 仍未到达 end，插入剩余尾部 */
	if (base < end) {
		nr_new++;
		if (insert) {
			if (start_rgn == -1)
				start_rgn = idx;
			end_rgn = idx + 1;
			memblock_insert_region(type, idx, base, end - base,
					       nid, flags);
		}
	}

	if (!nr_new)
		return 0;

	/*
	 * 根据 insert 标志判断当前是哪一遍：
	 * - 第一遍 (!insert): 扩容后跳回 repeat 执行第二遍
	 * - 第二遍 (insert):  合并相邻兼容 region 后返回
	 */
	if (!insert) {
		while (type->cnt + nr_new > type->max)
			if (memblock_double_array(type, obase, size) < 0)
				return -ENOMEM;
		insert = true;
		goto repeat;
	} else {
		/*
		 * 合并相邻的兼容 region。
		 * start_rgn - 1 用于向前合并（新插入的 region 可能和前一个
		 * region 相邻且兼容。end_rgn 用于向后合并（新插入的尾部
		 * 可能和下一个 region 相邻且兼容）。
		 *
		 * memblock_merge_regions 会检查三个条件：
		 * 1. 地址是否相邻（this->base + this->size == next->base）
		 * 2. NUMA 节点是否相同
		 * 3. flags 是否相同
		 * 全部满足才合并。
		 */
		memblock_merge_regions(type, start_rgn, end_rgn);
		return 0;
	}
}

/**
 * memblock_add_node - add new memblock region within a NUMA node
 * @base: base address of the new region
 * @size: size of the new region
 * @nid: nid of the new region
 * @flags: flags of the new region
 *
 * Add new memblock region [@base, @base + @size) to the "memory"
 * type. See memblock_add_range() description for mode details
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_add_node(phys_addr_t base, phys_addr_t size,
				      int nid, enum memblock_flags flags)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] nid=%d flags=%x %pS\n", __func__,
		     &base, &end, nid, flags, (void *)_RET_IP_);

	return memblock_add_range(&memblock.memory, base, size, nid, flags);
}

/**
 * memblock_add - add new memblock region
 * @base: base address of the new region
 * @size: size of the new region
 *
 * Add new memblock region [@base, @base + @size) to the "memory"
 * type. See memblock_add_range() description for mode details
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_add(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	return memblock_add_range(&memblock.memory, base, size, MAX_NUMNODES, 0);
}

/**
 * memblock_validate_numa_coverage - check if amount of memory with
 * no node ID assigned is less than a threshold
 * @threshold_bytes: maximal memory size that can have unassigned node
 * ID (in bytes).
 *
 * A buggy firmware may report memory that does not belong to any node.
 * Check if amount of such memory is below @threshold_bytes.
 *
 * Return: true on success, false on failure.
 */
bool __init_memblock memblock_validate_numa_coverage(unsigned long threshold_bytes)
{
	unsigned long nr_pages = 0;
	unsigned long start_pfn, end_pfn, mem_size_mb;
	int nid, i;

	/* calculate lost page */
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		if (!numa_valid_node(nid))
			nr_pages += end_pfn - start_pfn;
	}

	if ((nr_pages << PAGE_SHIFT) > threshold_bytes) {
		mem_size_mb = memblock_phys_mem_size() / SZ_1M;
		pr_err("NUMA: no nodes coverage for %luMB of %luMB RAM\n",
		       (nr_pages << PAGE_SHIFT) / SZ_1M, mem_size_mb);
		return false;
	}

	return true;
}


/**
 * memblock_isolate_range - isolate given range into disjoint memblocks
 * @type: memblock type to isolate range for
 * @base: base of range to isolate
 * @size: size of range to isolate
 * @start_rgn: out parameter for the start of isolated region
 * @end_rgn: out parameter for the end of isolated region
 *
 * Walk @type and ensure that regions don't cross the boundaries defined by
 * [@base, @base + @size).  Crossing regions are split at the boundaries,
 * which may create at most two more regions.  The index of the first
 * region inside the range is returned in *@start_rgn and the index of the
 * first region after the range is returned in *@end_rgn.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
static int __init_memblock memblock_isolate_range(struct memblock_type *type,
					phys_addr_t base, phys_addr_t size,
					int *start_rgn, int *end_rgn)
{
	phys_addr_t end = base + memblock_cap_size(base, &size);
	int idx;
	struct memblock_region *rgn;

	*start_rgn = *end_rgn = 0;

	if (!size)
		return 0;

	/*
	 * 隔离操作最多会创建 2 个新 region（从下方切入 + 从上方切出），
	 * 确保数组有至少 2 个空闲槽位。
	 */
	while (type->cnt + 2 > type->max)
		if (memblock_double_array(type, base, size) < 0)
			return -ENOMEM;

	/*
	 * === region 分割逻辑 ===
	 *
	 * 对于 [base, end) 范围内的每个 region，有 3 种情况：
	 *
	 * 情况 A (rbase < base)：region 从范围下方切入
	 *   原始: [---rbase-----------rend---)
	 *   范围:        [---base--------end---)
	 *   分割后: [A---) [B-------------)
	 *   → region 被分成两部分，A 部分留在原地，
	 *     B 部分缩小后继续参与后续检查。
	 *   操作：更新 rgn（原地缩小为后半段），插入前半段。
	 *
	 * 情况 B (rend > end)：region 从范围上方切出
	 *   原始: [---rbase-----------rend---)
	 *   范围: [---base--------end---)
	 *   分割后: [-------------B) [---A---)
	 *   → region 被分成两部分，A 部分留在原地，
	 *     B 部分通过 idx-- 重做一次以正确记录。
	 *   操作：更新 rgn（原地缩小为后半段），插入前半段后 idx--。
	 *
	 * 情况 C (rbase >= base && rend <= end)：region 完全在范围内
	 *   记录其索引到 start_rgn/end_rgn。
	 */
	for_each_memblock_type(idx, type, rgn) {
		phys_addr_t rbase = rgn->base;
		phys_addr_t rend = rbase + rgn->size;

		if (rbase >= end)
			break;
		if (rend <= base)
			continue;

		if (rbase < base) {
			/*
			 * 情况 A：region 从下方切入 [base, end)
			 *
			 * 原始 region: [rbase, rend)
			 * 目标范围:     [base,  end)
			 *
			 * 将原始 region 分裂为：
			 *   下半段 [rbase, base) → 新插入的 region（在 base 之前）
			 *   上半段 [base, rend)   → 原地缩小后的 rgn（在 base 之后）
			 *
			 * idx 不推进，因为分裂后 idx+1 是刚插入的下半段，
			 * 继续到 idx+2 处理下一个原始 region。
			 */
			rgn->base = base;
			rgn->size -= base - rbase;
			type->total_size -= base - rbase;
			memblock_insert_region(type, idx, rbase, base - rbase,
					       memblock_get_region_node(rgn),
					       rgn->flags);
		} else if (rend > end) {
			/*
			 * 情况 B：region 从上方切出 [base, end)
			 *
			 * 原始 region: [rbase, rend)
			 * 目标范围:     [base,  end)
			 *
			 * 将原始 region 分裂为：
			 *   下半段 [rbase, end)   → 新插入的 region（在 end 之前）
			 *   上半段 [end,   rend)  → 原地缩小后的 rgn（在 end 之后）
			 *
			 * idx-- 是关键：因为刚插入的 [rbase, end) 完全在范围内，
			 * 需要记录它。idx-- 后重新进入循环体，会在 else 分支中
			 * 记录它（rbase >= base && rend <= end）。
			 */
			rgn->base = end;
			rgn->size -= end - rbase;
			type->total_size -= end - rbase;
			memblock_insert_region(type, idx--, rbase, end - rbase,
					       memblock_get_region_node(rgn),
					       rgn->flags);
		} else {
			/*
			 * 情况 C：region 完全在 [base, end) 范围内
			 * 记录 start_rgn（第一次遇到）和 end_rgn（每次更新）。
			 * end_rgn 是"第一个超出范围的索引"（半开区间）。
			 */
			if (!*end_rgn)
				*start_rgn = idx;
			*end_rgn = idx + 1;
		}
	}

	return 0;
}

static int __init_memblock memblock_remove_range(struct memblock_type *type,
					  phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = end_rgn - 1; i >= start_rgn; i--)
		memblock_remove_region(type, i);
	return 0;
}

int __init_memblock memblock_remove(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	return memblock_remove_range(&memblock.memory, base, size);
}

static unsigned long __free_reserved_area(phys_addr_t start, phys_addr_t end,
					  int poison)
{
	unsigned long pages = 0, pfn;

	if (deferred_pages_enabled()) {
		WARN(1, "Cannot free reserved memory because of deferred initialization of the memory map");
		return 0;
	}

	for_each_valid_pfn(pfn, PFN_UP(start), PFN_DOWN(end)) {
		struct page *page = pfn_to_page(pfn);
		void *direct_map_addr;

		/*
		 * 'direct_map_addr' might be different from the kernel virtual
		 * address because some architectures use aliases.
		 * Going via physical address, pfn_to_page() and page_address()
		 * ensures that we get a _writeable_ alias for the memset().
		 */
		direct_map_addr = page_address(page);
		/*
		 * Perform a kasan-unchecked memset() since this memory
		 * has not been initialized.
		 */
		direct_map_addr = kasan_reset_tag(direct_map_addr);
		if ((unsigned int)poison <= 0xFF)
			memset(direct_map_addr, poison, PAGE_SIZE);

		free_reserved_page(page);
		pages++;
	}
	return pages;
}

unsigned long free_reserved_area(void *start, void *end, int poison, const char *s)
{
	phys_addr_t start_pa, end_pa;
	unsigned long pages;

	/*
	 * end is the first address past the region and it may be beyond what
	 * __pa() or __pa_symbol() can handle.
	 * Use the address included in the range for the conversion and add back
	 * 1 afterwards.
	 */
	if (__is_kernel((unsigned long)start)) {
		start_pa = __pa_symbol(start);
		end_pa = __pa_symbol(end - 1) + 1;
	} else {
		start_pa = __pa(start);
		end_pa = __pa(end - 1) + 1;
	}

	if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK)) {
		if (start_pa < end_pa)
			memblock_remove_range(&memblock.reserved,
					      start_pa, end_pa - start_pa);
	}

	pages = __free_reserved_area(start_pa, end_pa, poison);
	if (pages && s)
		pr_info("Freeing %s memory: %ldK\n", s, K(pages));

	return pages;
}

/**
 * memblock_free - free boot memory allocation
 * @ptr: starting address of the  boot memory allocation
 * @size: size of the boot memory block in bytes
 *
 * Free boot memory block previously allocated by memblock_alloc_xx() API.
 * If called after the buddy allocator is available, the memory is released to
 * the buddy allocator.
 */
void __init_memblock memblock_free(void *ptr, size_t size)
{
	if (ptr)
		memblock_phys_free(__pa(ptr), size);
}

/**
 * memblock_phys_free - free boot memory block
 * @base: phys starting address of the  boot memory block
 * @size: size of the boot memory block in bytes
 *
 * Free boot memory block previously allocated by memblock_phys_alloc_xx() API.
 * If called after the buddy allocator is available, the memory is released to
 * the buddy allocator.
 */
int __init_memblock memblock_phys_free(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;
	int ret;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	kmemleak_free_part_phys(base, size);
	ret = memblock_remove_range(&memblock.reserved, base, size);

	if (slab_is_available())
		__free_reserved_area(base, base + size, -1);

	return ret;
}

int __init_memblock __memblock_reserve(phys_addr_t base, phys_addr_t size,
				       int nid, enum memblock_flags flags)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] nid=%d flags=%x %pS\n", __func__,
		     &base, &end, nid, flags, (void *)_RET_IP_);

	return memblock_add_range(&memblock.reserved, base, size, nid, flags);
}

#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
int __init_memblock memblock_physmem_add(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t end = base + size - 1;

	memblock_dbg("%s: [%pa-%pa] %pS\n", __func__,
		     &base, &end, (void *)_RET_IP_);

	return memblock_add_range(&physmem, base, size, MAX_NUMNODES, 0);
}
#endif

#ifdef CONFIG_MEMBLOCK_KHO_SCRATCH
__init void memblock_set_kho_scratch_only(void)
{
	kho_scratch_only = true;
}

__init void memblock_clear_kho_scratch_only(void)
{
	kho_scratch_only = false;
}

__init void memmap_init_kho_scratch_pages(void)
{
	phys_addr_t start, end;
	unsigned long pfn;
	int nid;
	u64 i;

	if (!IS_ENABLED(CONFIG_DEFERRED_STRUCT_PAGE_INIT))
		return;

	/*
	 * Initialize struct pages for free scratch memory.
	 * The struct pages for reserved scratch memory will be set up in
	 * memmap_init_reserved_pages()
	 */
	__for_each_mem_range(i, &memblock.memory, NULL, NUMA_NO_NODE,
			     MEMBLOCK_KHO_SCRATCH, &start, &end, &nid) {
		for (pfn = PFN_UP(start); pfn < PFN_DOWN(end); pfn++)
			init_deferred_page(pfn, nid);
	}
}
#endif

/**
 * memblock_setclr_flag - set or clear flag for a memory region
 * @type: memblock type to set/clear flag for
 * @base: base address of the region
 * @size: size of the region
 * @set: set or clear the flag
 * @flag: the flag to update
 *
 * This function isolates region [@base, @base + @size), and sets/clears flag
 *
 * Return: 0 on success, -errno on failure.
 */
static int __init_memblock memblock_setclr_flag(struct memblock_type *type,
				phys_addr_t base, phys_addr_t size, int set, int flag)
{
	int i, ret, start_rgn, end_rgn;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++) {
		struct memblock_region *r = &type->regions[i];

		if (set)
			r->flags |= flag;
		else
			r->flags &= ~flag;
	}

	memblock_merge_regions(type, start_rgn, end_rgn);
	return 0;
}

/**
 * memblock_mark_hotplug - Mark hotpluggable memory with flag MEMBLOCK_HOTPLUG.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_mark_hotplug(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 1, MEMBLOCK_HOTPLUG);
}

/**
 * memblock_clear_hotplug - Clear flag MEMBLOCK_HOTPLUG for a specified region.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_clear_hotplug(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 0, MEMBLOCK_HOTPLUG);
}

/**
 * memblock_mark_mirror - Mark mirrored memory with flag MEMBLOCK_MIRROR.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_mark_mirror(phys_addr_t base, phys_addr_t size)
{
	if (!mirrored_kernelcore)
		return 0;

	system_has_some_mirror = true;

	return memblock_setclr_flag(&memblock.memory, base, size, 1, MEMBLOCK_MIRROR);
}

/**
 * memblock_mark_nomap - Mark a memory region with flag MEMBLOCK_NOMAP.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * The memory regions marked with %MEMBLOCK_NOMAP will not be added to the
 * direct mapping of the physical memory. These regions will still be
 * covered by the memory map. The struct page representing NOMAP memory
 * frames in the memory map will be PageReserved()
 *
 * Note: if the memory being marked %MEMBLOCK_NOMAP was allocated from
 * memblock, the caller must inform kmemleak to ignore that memory
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_mark_nomap(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 1, MEMBLOCK_NOMAP);
}

/**
 * memblock_clear_nomap - Clear flag MEMBLOCK_NOMAP for a specified region.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_clear_nomap(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 0, MEMBLOCK_NOMAP);
}

/**
 * memblock_reserved_mark_noinit - Mark a reserved memory region with flag
 * MEMBLOCK_RSRV_NOINIT
 *
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * The struct pages for the reserved regions marked %MEMBLOCK_RSRV_NOINIT will
 * not be fully initialized to allow the caller optimize their initialization.
 *
 * When %CONFIG_DEFERRED_STRUCT_PAGE_INIT is enabled, setting this flag
 * completely bypasses the initialization of struct pages for such region.
 *
 * When %CONFIG_DEFERRED_STRUCT_PAGE_INIT is disabled, struct pages in this
 * region will be initialized with default values but won't be marked as
 * reserved.
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_reserved_mark_noinit(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.reserved, base, size, 1,
				    MEMBLOCK_RSRV_NOINIT);
}

/**
 * memblock_reserved_mark_kern - Mark a reserved memory region with flag
 * MEMBLOCK_RSRV_KERN
 *
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
int __init_memblock memblock_reserved_mark_kern(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.reserved, base, size, 1,
				    MEMBLOCK_RSRV_KERN);
}

/**
 * memblock_mark_kho_scratch - Mark a memory region as MEMBLOCK_KHO_SCRATCH.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Only memory regions marked with %MEMBLOCK_KHO_SCRATCH will be considered
 * for allocations during early boot with kexec handover.
 *
 * Return: 0 on success, -errno on failure.
 */
__init int memblock_mark_kho_scratch(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 1,
				    MEMBLOCK_KHO_SCRATCH);
}

/**
 * memblock_clear_kho_scratch - Clear MEMBLOCK_KHO_SCRATCH flag for a
 * specified region.
 * @base: the base phys addr of the region
 * @size: the size of the region
 *
 * Return: 0 on success, -errno on failure.
 */
__init int memblock_clear_kho_scratch(phys_addr_t base, phys_addr_t size)
{
	return memblock_setclr_flag(&memblock.memory, base, size, 0,
				    MEMBLOCK_KHO_SCRATCH);
}

static bool should_skip_region(struct memblock_type *type,
			       struct memblock_region *m,
			       int nid, int flags)
{
	int m_nid = memblock_get_region_node(m);

	/* we never skip regions when iterating memblock.reserved or physmem */
	if (type != memblock_memory)
		return false;

	/* only memory regions are associated with nodes, check it */
	if (numa_valid_node(nid) && nid != m_nid)
		return true;

	/* skip hotpluggable memory regions if needed */
	if (movable_node_is_enabled() && memblock_is_hotpluggable(m) &&
	    !(flags & MEMBLOCK_HOTPLUG))
		return true;

	/* if we want mirror memory skip non-mirror memory regions */
	if ((flags & MEMBLOCK_MIRROR) && !memblock_is_mirror(m))
		return true;

	/* skip nomap memory unless we were asked for it explicitly */
	if (!(flags & MEMBLOCK_NOMAP) && memblock_is_nomap(m))
		return true;

	/* skip driver-managed memory unless we were asked for it explicitly */
	if (!(flags & MEMBLOCK_DRIVER_MANAGED) && memblock_is_driver_managed(m))
		return true;

	/*
	 * In early alloc during kexec handover, we can only consider
	 * MEMBLOCK_KHO_SCRATCH regions for the allocations
	 */
	if ((flags & MEMBLOCK_KHO_SCRATCH) && !memblock_is_kho_scratch(m))
		return true;

	return false;
}

/**
 * __next_mem_range - next function for for_each_free_mem_range() etc.
 * @idx: pointer to u64 loop variable
 * @nid: node selector, %NUMA_NO_NODE for all nodes
 * @flags: pick from blocks based on memory attributes
 * @type_a: pointer to memblock_type from where the range is taken
 * @type_b: pointer to memblock_type which excludes memory from being taken
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Find the first area from *@idx which matches @nid, fill the out
 * parameters, and update *@idx for the next iteration.  The lower 32bit of
 * *@idx contains index into type_a and the upper 32bit indexes the
 * areas before each region in type_b.	For example, if type_b regions
 * look like the following,
 *
 *	0:[0-16), 1:[32-48), 2:[128-130)
 *
 * The upper 32bit indexes the following regions.
 *
 *	0:[0-0), 1:[16-32), 2:[48-128), 3:[130-MAX)
 *
 * As both region arrays are sorted, the function advances the two indices
 * in lockstep and returns each intersection.
 */
void __next_mem_range(u64 *idx, int nid, enum memblock_flags flags,
		      struct memblock_type *type_a,
		      struct memblock_type *type_b, phys_addr_t *out_start,
		      phys_addr_t *out_end, int *out_nid)
{
	/*
	 * idx 编码为 64 位整数：
	 *   低 32 位 (idx_a): type_a (memory) 中的当前 region 索引
	 *   高 32 位 (idx_b): type_b (reserved) 中的"间隙"索引
	 *
	 * idx_b 的设计很巧妙：它不仅索引 reserved region 本身，
	 * 还索引 region 之间的"间隙"。见上方的 kernel-doc 图示。
	 */
	int idx_a = *idx & 0xffffffff;
	int idx_b = *idx >> 32;

	/*
	 * === 外层循环：遍历 type_a (memory) 的每个 region ===
	 *
	 * 对每个 memory region [m_start, m_end)，在内层寻找
	 * 其中不被任何 reserved region 覆盖的子区间。
	 */
	for (; idx_a < type_a->cnt; idx_a++) {
		struct memblock_region *m = &type_a->regions[idx_a];

		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;
		int	    m_nid = memblock_get_region_node(m);

		/* 根据 nid、flags 判断是否跳过此 region */
		if (should_skip_region(type_a, m, nid, flags))
			continue;

		/*
		 * type_b == NULL：只遍历 type_a，不排除任何区域
		 * 用于 for_each_mem_region() 等简单遍历场景
		 */
		if (!type_b) {
			if (out_start)
				*out_start = m_start;
			if (out_end)
				*out_end = m_end;
			if (out_nid)
				*out_nid = m_nid;
			idx_a++;
			*idx = (u32)idx_a | (u64)idx_b << 32;
			return;
		}

		/*
		 * === 内层循环：遍历 type_b (reserved) 的"间隙" ===
		 *
		 * 核心思想：将 reserved region 数组重新解释为一组"空闲间隙"。
		 * idx_b 从 0 到 cnt（含），每个值对应一个间隙：
		 *
		 *   idx_b = 0:      [0,                rgn[0].base)
		 *   idx_b = k:      [rgn[k-1].end,      rgn[k].base)   (0<k<cnt)
		 *   idx_b = cnt:    [rgn[cnt-1].end,    PHYS_ADDR_MAX)
		 *
		 * 然后 memory region 和这些"间隙"做交集运算，
		 * 得到的就是 memory 中有而 reserved 中没有的"空闲内存"。
		 *
		 * 这是经典的变体双指针归并算法：
		 * 一个指针（idx_a）遍历 memory 数组，
		 * 另一个指针（idx_b）遍历 reserved 的间隙数组，
		 * 在 lockstep 中推进先结束的那一侧。
		 */
		for (; idx_b < type_b->cnt + 1; idx_b++) {
			struct memblock_region *r;
			phys_addr_t r_start;
			phys_addr_t r_end;

			r = &type_b->regions[idx_b];
			r_start = idx_b ? r[-1].base + r[-1].size : 0;
			r_end = idx_b < type_b->cnt ?
				r->base : PHYS_ADDR_MAX;

			/*
			 * 情况 A：reserved 间隙已完全在当前 memory region 之后
			 *
			 * 图示：memory [---m_start---m_end)
			 *       间隙             [---r_start---r_end)
			 *
			 * r_start >= m_end：此间隙及其后的所有间隙
			 * 都在当前 memory region 之外，跳出内层循环
			 * 推进到下一个 memory region（idx_a++）。
			 */
			if (r_start >= m_end)
				break;

			/*
			 * 情况 B：memory 与 reserved 间隙相交
			 *
			 * 图示：memory [---m_start--------m_end---)
			 *       间隙         [---r_start---r_end)
			 *       空闲     [---max-------min---)
			 *
			 * 交集 [max(m_start, r_start), min(m_end, r_end))
			 * 就是一段空闲物理内存——memory 中有而 reserved 中没有。
			 */
			if (m_start < r_end) {
				if (out_start)
					*out_start =
						max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = m_nid;

				/*
				 * 双指针归并核心规则：谁先结束就推进谁
				 *
				 * - memory region 先结束 (m_end <= r_end)：
				 *   当前 memory region 已被完全遍历，
				 *   下次推进 idx_a 到下一个 memory region，
				 *   idx_b 保持不变。
				 *
				 * - reserved 间隙先结束 (r_end < m_end)：
				 *   当前间隙已被完全遍历，
				 *   memory region 还有剩余部分未处理，
				 *   下次推进 idx_b 到下一个间隙，
				 *   idx_a 保持不变。
				 */
				if (m_end <= r_end)
					idx_a++;
				else
					idx_b++;
				*idx = (u32)idx_a | (u64)idx_b << 32;
				return;
			}
			/*
			 * 情况 C：reserved 间隙完全在当前 memory 之前
			 * （即 m_start >= r_end），无需处理，idx_b++ 继续。
			 */
		}
	}

	/* idx == ULLONG_MAX 是迭代终止信号 */
	*idx = ULLONG_MAX;
}

/**
 * __next_mem_range_rev - generic next function for for_each_*_range_rev()
 *
 * @idx: pointer to u64 loop variable
 * @nid: node selector, %NUMA_NO_NODE for all nodes
 * @flags: pick from blocks based on memory attributes
 * @type_a: pointer to memblock_type from where the range is taken
 * @type_b: pointer to memblock_type which excludes memory from being taken
 * @out_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @out_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @out_nid: ptr to int for nid of the range, can be %NULL
 *
 * Finds the next range from type_a which is not marked as unsuitable
 * in type_b.
 *
 * Reverse of __next_mem_range().
 */
void __init_memblock __next_mem_range_rev(u64 *idx, int nid,
					  enum memblock_flags flags,
					  struct memblock_type *type_a,
					  struct memblock_type *type_b,
					  phys_addr_t *out_start,
					  phys_addr_t *out_end, int *out_nid)
{
	int idx_a = *idx & 0xffffffff;
	int idx_b = *idx >> 32;

	if (*idx == (u64)ULLONG_MAX) {
		idx_a = type_a->cnt - 1;
		if (type_b != NULL)
			idx_b = type_b->cnt;
		else
			idx_b = 0;
	}

	for (; idx_a >= 0; idx_a--) {
		struct memblock_region *m = &type_a->regions[idx_a];

		phys_addr_t m_start = m->base;
		phys_addr_t m_end = m->base + m->size;
		int m_nid = memblock_get_region_node(m);

		if (should_skip_region(type_a, m, nid, flags))
			continue;

		if (!type_b) {
			if (out_start)
				*out_start = m_start;
			if (out_end)
				*out_end = m_end;
			if (out_nid)
				*out_nid = m_nid;
			idx_a--;
			*idx = (u32)idx_a | (u64)idx_b << 32;
			return;
		}

		/* scan areas before each reservation */
		for (; idx_b >= 0; idx_b--) {
			struct memblock_region *r;
			phys_addr_t r_start;
			phys_addr_t r_end;

			r = &type_b->regions[idx_b];
			r_start = idx_b ? r[-1].base + r[-1].size : 0;
			r_end = idx_b < type_b->cnt ?
				r->base : PHYS_ADDR_MAX;
			/*
			 * if idx_b advanced past idx_a,
			 * break out to advance idx_a
			 */

			if (r_end <= m_start)
				break;
			/* if the two regions intersect, we're done */
			if (m_end > r_start) {
				if (out_start)
					*out_start = max(m_start, r_start);
				if (out_end)
					*out_end = min(m_end, r_end);
				if (out_nid)
					*out_nid = m_nid;
				if (m_start >= r_start)
					idx_a--;
				else
					idx_b--;
				*idx = (u32)idx_a | (u64)idx_b << 32;
				return;
			}
		}
	}
	/* signal end of iteration */
	*idx = ULLONG_MAX;
}

/*
 * Common iterator interface used to define for_each_mem_pfn_range().
 */
void __init_memblock __next_mem_pfn_range(int *idx, int nid,
				unsigned long *out_start_pfn,
				unsigned long *out_end_pfn, int *out_nid)
{
	struct memblock_type *type = &memblock.memory;
	struct memblock_region *r;
	int r_nid;

	while (++*idx < type->cnt) {
		r = &type->regions[*idx];
		r_nid = memblock_get_region_node(r);

		if (PFN_UP(r->base) >= PFN_DOWN(r->base + r->size))
			continue;
		if (!numa_valid_node(nid) || nid == r_nid)
			break;
	}
	if (*idx >= type->cnt) {
		*idx = -1;
		return;
	}

	if (out_start_pfn)
		*out_start_pfn = PFN_UP(r->base);
	if (out_end_pfn)
		*out_end_pfn = PFN_DOWN(r->base + r->size);
	if (out_nid)
		*out_nid = r_nid;
}

/**
 * memblock_set_node - set node ID on memblock regions
 * @base: base of area to set node ID for
 * @size: size of area to set node ID for
 * @type: memblock type to set node ID for
 * @nid: node ID to set
 *
 * Set the nid of memblock @type regions in [@base, @base + @size) to @nid.
 * Regions which cross the area boundaries are split as necessary.
 *
 * Return:
 * 0 on success, -errno on failure.
 */
int __init_memblock memblock_set_node(phys_addr_t base, phys_addr_t size,
				      struct memblock_type *type, int nid)
{
#ifdef CONFIG_NUMA
	int start_rgn, end_rgn;
	int i, ret;

	ret = memblock_isolate_range(type, base, size, &start_rgn, &end_rgn);
	if (ret)
		return ret;

	for (i = start_rgn; i < end_rgn; i++)
		memblock_set_region_node(&type->regions[i], nid);

	memblock_merge_regions(type, start_rgn, end_rgn);
#endif
	return 0;
}

/**
 * memblock_alloc_range_nid - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @start: the lower bound of the memory region to allocate (phys address)
 * @end: the upper bound of the memory region to allocate (phys address)
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @exact_nid: control the allocation fall back to other nodes
 *
 * The allocation is performed from memory region limited by
 * memblock.current_limit if @end == %MEMBLOCK_ALLOC_ACCESSIBLE.
 *
 * If the specified node can not hold the requested memory and @exact_nid
 * is false, the allocation falls back to any node in the system.
 *
 * For systems with memory mirroring, the allocation is attempted first
 * from the regions with mirroring enabled and then retried from any
 * memory region.
 *
 * In addition, function using kmemleak_alloc_phys for allocated boot
 * memory block, it is never reported as leaks.
 *
 * Return:
 * Physical address of allocated memory block on success, %0 on failure.
 */
phys_addr_t __init memblock_alloc_range_nid(phys_addr_t size,
					phys_addr_t align, phys_addr_t start,
					phys_addr_t end, int nid,
					bool exact_nid)
{
	enum memblock_flags flags = choose_memblock_flags();
	phys_addr_t found;

	/*
	 * 安全检查：如果在 slab 分配器已可用后调用 memblock 分配，
	 * 说明调用时机可能不正确。此时 memblock 可能已被释放。
	 * 直接回退到 kzalloc_node 分配。
	 */
	if (WARN_ON_ONCE(slab_is_available())) {
		void *vaddr = kzalloc_node(size, GFP_NOWAIT, nid);

		return vaddr ? virt_to_phys(vaddr) : 0;
	}

	/*
	 * 对齐参数为 0 时，使用 SMP_CACHE_BYTES 作为默认对齐。
	 * 这是为了保证分配的内存与缓存行对齐，避免伪共享问题。
	 * powerpc 早期启动时不能使用 WARN，故用 dump_stack。
	 */
	if (!align) {
		/* Can't use WARNs this early in boot on powerpc */
		dump_stack();
		align = SMP_CACHE_BYTES;
	}

again:
	/*
	 * === 核心分配逻辑（三步重试策略） ===
	 *
	 * 第一步：在指定节点上查找空闲内存
	 * memblock_find_in_range_node 会选择合适的 bottom-up 或
	 * top-down 搜索策略，在 [start, end) 范围内寻找符合条件的空闲区域。
	 * 找到后用 __memblock_reserve 立即标记为已保留。
	 *
	 * __memblock_reserve 返回 0 表示 reserve 成功。
	 */
	found = memblock_find_in_range_node(size, align, start, end, nid,
					    flags);
	if (found && !__memblock_reserve(found, size, nid, MEMBLOCK_RSRV_KERN))
		goto done;

	/*
	 * 第二步（NUMA 回退）：
	 * 如果在指定节点上分配失败，且 exact_nid == false（允许跨节点），
	 * 放松 NUMA 约束，尝试在任何节点上分配（NUMA_NO_NODE）。
	 */
	if (numa_valid_node(nid) && !exact_nid) {
		found = memblock_find_in_range_node(size, align, start,
						    end, NUMA_NO_NODE,
						    flags);
		if (found && !memblock_reserve_kern(found, size))
			goto done;
	}

	/*
	 * 第三步（MIRROR 回退）：
	 * 如果请求的是 mirrored memory 但没有找到，
	 * 去除 MEMBLOCK_MIRROR 标志，重试非 mirrored 内存。
	 * 这是最后的 fallback，因为 mirrored 内存通常更稀缺。
	 */
	if (flags & MEMBLOCK_MIRROR) {
		flags &= ~MEMBLOCK_MIRROR;
		pr_warn_ratelimited("Could not allocate %pap bytes of mirrored memory\n",
			&size);
		goto again;
	}

	/* 所有尝试都失败，返回 0 */
	return 0;

done:
	/*
	 * kmemleak 跟踪：除非明确要求不跟踪（MEMBLOCK_ALLOC_NOLEAKTRACE），
	 * 否则将分配通知 kmemleak。memblock 分配的块不会被报告为泄漏，
	 * 因为它们通常只通过物理地址访问，而 kmemleak 不扫描物理地址。
	 */
	if (end != MEMBLOCK_ALLOC_NOLEAKTRACE)
		kmemleak_alloc_phys(found, size, 0);

	/*
	 * 机密虚拟机平台（Intel TDX, AMD SEV-SNP 等）要求内存在使用前
	 * 先被"接受"（accept）。对于这些平台，内存必须经过特定的
	 * 安全转换后才能被内核访问。accept_memory() 处理此转换。
	 */
	accept_memory(found, size);

	return found;
}

/**
 * memblock_phys_alloc_range - allocate a memory block inside specified range
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @start: the lower bound of the memory region to allocate (physical address)
 * @end: the upper bound of the memory region to allocate (physical address)
 *
 * Allocate @size bytes in the between @start and @end.
 *
 * Return: physical address of the allocated memory block on success,
 * %0 on failure.
 */
phys_addr_t __init memblock_phys_alloc_range(phys_addr_t size,
					     phys_addr_t align,
					     phys_addr_t start,
					     phys_addr_t end)
{
	memblock_dbg("%s: %llu bytes align=0x%llx from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, &start, &end,
		     (void *)_RET_IP_);
	return memblock_alloc_range_nid(size, align, start, end, NUMA_NO_NODE,
					false);
}

/**
 * memblock_phys_alloc_try_nid - allocate a memory block from specified NUMA node
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Allocates memory block from the specified NUMA node. If the node
 * has no available memory, attempts to allocated from any node in the
 * system.
 *
 * Return: physical address of the allocated memory block on success,
 * %0 on failure.
 */
phys_addr_t __init memblock_phys_alloc_try_nid(phys_addr_t size, phys_addr_t align, int nid)
{
	return memblock_alloc_range_nid(size, align, 0,
					MEMBLOCK_ALLOC_ACCESSIBLE, nid, false);
}

/**
 * memblock_alloc_internal - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region to allocate (phys address)
 * @max_addr: the upper bound of the memory region to allocate (phys address)
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 * @exact_nid: control the allocation fall back to other nodes
 *
 * Allocates memory block using memblock_alloc_range_nid() and
 * converts the returned physical address to virtual.
 *
 * The @min_addr limit is dropped if it can not be satisfied and the allocation
 * will fall back to memory below @min_addr. Other constraints, such
 * as node and mirrored memory will be handled again in
 * memblock_alloc_range_nid().
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
static void * __init memblock_alloc_internal(
				phys_addr_t size, phys_addr_t align,
				phys_addr_t min_addr, phys_addr_t max_addr,
				int nid, bool exact_nid)
{
	phys_addr_t alloc;

	/*
	 * 限制 max_addr 不超过 current_limit。
	 * current_limit 是 memblock 分配的上限防火墙，
	 * 通常由架构代码在早期启动时设置（例如限制在 lowmem 范围内）。
	 * 这样可以防止分配器越界分配到不可访问的物理地址。
	 */
	if (max_addr > memblock.current_limit)
		max_addr = memblock.current_limit;

	/*
	 * 第一轮尝试：在 [min_addr, max_addr) 范围内按指定 nid 分配。
	 *
	 * memblock_alloc_range_nid 内部实现了三级回退：
	 *   1. 指定节点分配
	 *   2. 放松 NUMA 约束（任何节点）
	 *   3. 放松 mirror 要求
	 */
	alloc = memblock_alloc_range_nid(size, align, min_addr, max_addr, nid,
					exact_nid);

	/*
	 * 第二轮尝试（回退）：降低 min_addr 到 0。
	 *
	 * 如果第一轮失败且 min_addr > 0（即有下限约束），
	 * 说明在 [min_addr, max_addr) 范围内没有足够的空间。
	 * 放宽下限到 0，让分配器可以在更低地址寻找空间。
	 *
	 * 这是地址范围松弛策略：先尝试偏好范围，
	 * 失败后扩大到允许范围。
	 */
	if (!alloc && min_addr)
		alloc = memblock_alloc_range_nid(size, align, 0, max_addr, nid,
						exact_nid);

	/* 所有尝试都失败，返回 NULL */
	if (!alloc)
		return NULL;

	/*
	 * 将物理地址转换为内核虚拟地址。
	 * 物理内存已被直接映射（在大多数架构上），
	 * phys_to_virt 只是做一个简单的偏移量转换（PAGE_OFFSET）。
	 * 调用者可以直接使用返回的虚拟地址访问分配的内存。
	 */
	return phys_to_virt(alloc);
}

/**
 * memblock_alloc_exact_nid_raw - allocate boot memory block on the exact node
 * without zeroing memory
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %MEMBLOCK_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. Does not zero allocated memory.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
void * __init memblock_alloc_exact_nid_raw(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);

	return memblock_alloc_internal(size, align, min_addr, max_addr, nid,
				       true);
}

/**
 * memblock_alloc_try_nid_raw - allocate boot memory block without zeroing
 * memory and without panicking
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %MEMBLOCK_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. Does not zero allocated memory, does not panic if request
 * cannot be satisfied.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
void * __init memblock_alloc_try_nid_raw(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);

	return memblock_alloc_internal(size, align, min_addr, max_addr, nid,
				       false);
}

/**
 * memblock_alloc_try_nid - allocate boot memory block
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @min_addr: the lower bound of the memory region from where the allocation
 *	  is preferred (phys address)
 * @max_addr: the upper bound of the memory region from where the allocation
 *	      is preferred (phys address), or %MEMBLOCK_ALLOC_ACCESSIBLE to
 *	      allocate only from memory limited by memblock.current_limit value
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * Public function, provides additional debug information (including caller
 * info), if enabled. This function zeroes the allocated memory.
 *
 * Return:
 * Virtual address of allocated memory block on success, NULL on failure.
 */
void * __init memblock_alloc_try_nid(
			phys_addr_t size, phys_addr_t align,
			phys_addr_t min_addr, phys_addr_t max_addr,
			int nid)
{
	void *ptr;

	memblock_dbg("%s: %llu bytes align=0x%llx nid=%d from=%pa max_addr=%pa %pS\n",
		     __func__, (u64)size, (u64)align, nid, &min_addr,
		     &max_addr, (void *)_RET_IP_);
	ptr = memblock_alloc_internal(size, align,
					   min_addr, max_addr, nid, false);
	if (ptr)
		memset(ptr, 0, size);

	return ptr;
}

/**
 * __memblock_alloc_or_panic - Try to allocate memory and panic on failure
 * @size: size of memory block to be allocated in bytes
 * @align: alignment of the region and block's size
 * @func: caller func name
 *
 * This function attempts to allocate memory using memblock_alloc,
 * and in case of failure, it calls panic with the formatted message.
 * This function should not be used directly, please use the macro memblock_alloc_or_panic.
 */
void *__init __memblock_alloc_or_panic(phys_addr_t size, phys_addr_t align,
				       const char *func)
{
	void *addr = memblock_alloc(size, align);

	if (unlikely(!addr))
		panic("%s: Failed to allocate %pap bytes\n", func, &size);
	return addr;
}

/*
 * Remaining API functions
 */

phys_addr_t __init_memblock memblock_phys_mem_size(void)
{
	return memblock.memory.total_size;
}

phys_addr_t __init_memblock memblock_reserved_size(void)
{
	return memblock.reserved.total_size;
}

phys_addr_t __init_memblock memblock_reserved_kern_size(phys_addr_t limit, int nid)
{
	struct memblock_region *r;
	phys_addr_t total = 0;

	for_each_reserved_mem_region(r) {
		phys_addr_t size = r->size;

		if (r->base > limit)
			break;

		if (r->base + r->size > limit)
			size = limit - r->base;

		if (nid == memblock_get_region_node(r) || !numa_valid_node(nid))
			if (r->flags & MEMBLOCK_RSRV_KERN)
				total += size;
	}

	return total;
}

/**
 * memblock_estimated_nr_free_pages - return estimated number of free pages
 * from memblock point of view
 *
 * During bootup, subsystems might need a rough estimate of the number of free
 * pages in the whole system, before precise numbers are available from the
 * buddy. Especially with CONFIG_DEFERRED_STRUCT_PAGE_INIT, the numbers
 * obtained from the buddy might be very imprecise during bootup.
 *
 * Return:
 * An estimated number of free pages from memblock point of view.
 */
unsigned long __init memblock_estimated_nr_free_pages(void)
{
	return PHYS_PFN(memblock_phys_mem_size() -
			memblock_reserved_kern_size(MEMBLOCK_ALLOC_ANYWHERE, NUMA_NO_NODE));
}

/* lowest address */
phys_addr_t __init_memblock memblock_start_of_DRAM(void)
{
	return memblock.memory.regions[0].base;
}

phys_addr_t __init_memblock memblock_end_of_DRAM(void)
{
	int idx = memblock.memory.cnt - 1;

	return (memblock.memory.regions[idx].base + memblock.memory.regions[idx].size);
}

static phys_addr_t __init_memblock __find_max_addr(phys_addr_t limit)
{
	phys_addr_t max_addr = PHYS_ADDR_MAX;
	struct memblock_region *r;

	/*
	 * translate the memory @limit size into the max address within one of
	 * the memory memblock regions, if the @limit exceeds the total size
	 * of those regions, max_addr will keep original value PHYS_ADDR_MAX
	 */
	for_each_mem_region(r) {
		if (limit <= r->size) {
			max_addr = r->base + limit;
			break;
		}
		limit -= r->size;
	}

	return max_addr;
}

void __init memblock_enforce_memory_limit(phys_addr_t limit)
{
	phys_addr_t max_addr;

	if (!limit)
		return;

	max_addr = __find_max_addr(limit);

	/* @limit exceeds the total size of the memory, do nothing */
	if (max_addr == PHYS_ADDR_MAX)
		return;

	/* truncate both memory and reserved regions */
	memblock_remove_range(&memblock.memory, max_addr,
			      PHYS_ADDR_MAX);
	memblock_remove_range(&memblock.reserved, max_addr,
			      PHYS_ADDR_MAX);
}

void __init memblock_cap_memory_range(phys_addr_t base, phys_addr_t size)
{
	int start_rgn, end_rgn;
	int i, ret;

	if (!size)
		return;

	if (!memblock_memory->total_size) {
		pr_warn("%s: No memory registered yet\n", __func__);
		return;
	}

	ret = memblock_isolate_range(&memblock.memory, base, size,
						&start_rgn, &end_rgn);
	if (ret)
		return;

	/* remove all the MAP regions */
	for (i = memblock.memory.cnt - 1; i >= end_rgn; i--)
		if (!memblock_is_nomap(&memblock.memory.regions[i]))
			memblock_remove_region(&memblock.memory, i);

	for (i = start_rgn - 1; i >= 0; i--)
		if (!memblock_is_nomap(&memblock.memory.regions[i]))
			memblock_remove_region(&memblock.memory, i);

	/* truncate the reserved regions */
	memblock_remove_range(&memblock.reserved, 0, base);
	memblock_remove_range(&memblock.reserved,
			base + size, PHYS_ADDR_MAX);
}

void __init memblock_mem_limit_remove_map(phys_addr_t limit)
{
	phys_addr_t max_addr;

	if (!limit)
		return;

	max_addr = __find_max_addr(limit);

	/* @limit exceeds the total size of the memory, do nothing */
	if (max_addr == PHYS_ADDR_MAX)
		return;

	memblock_cap_memory_range(0, max_addr);
}

static int __init_memblock memblock_search(struct memblock_type *type, phys_addr_t addr)
{
	unsigned int left = 0, right = type->cnt;

	do {
		unsigned int mid = (right + left) / 2;

		if (addr < type->regions[mid].base)
			right = mid;
		else if (addr >= (type->regions[mid].base +
				  type->regions[mid].size))
			left = mid + 1;
		else
			return mid;
	} while (left < right);
	return -1;
}

bool __init_memblock memblock_is_reserved(phys_addr_t addr)
{
	return memblock_search(&memblock.reserved, addr) != -1;
}

bool __init_memblock memblock_is_memory(phys_addr_t addr)
{
	return memblock_search(&memblock.memory, addr) != -1;
}

bool __init_memblock memblock_is_map_memory(phys_addr_t addr)
{
	int i = memblock_search(&memblock.memory, addr);

	if (i == -1)
		return false;
	return !memblock_is_nomap(&memblock.memory.regions[i]);
}

int __init_memblock memblock_search_pfn_nid(unsigned long pfn,
			 unsigned long *start_pfn, unsigned long *end_pfn)
{
	struct memblock_type *type = &memblock.memory;
	int mid = memblock_search(type, PFN_PHYS(pfn));

	if (mid == -1)
		return NUMA_NO_NODE;

	*start_pfn = PFN_DOWN(type->regions[mid].base);
	*end_pfn = PFN_DOWN(type->regions[mid].base + type->regions[mid].size);

	return memblock_get_region_node(&type->regions[mid]);
}

/**
 * memblock_is_region_memory - check if a region is a subset of memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base + @size) is a subset of a memory block.
 *
 * Return:
 * 0 if false, non-zero if true
 */
bool __init_memblock memblock_is_region_memory(phys_addr_t base, phys_addr_t size)
{
	int idx = memblock_search(&memblock.memory, base);
	phys_addr_t end = base + memblock_cap_size(base, &size);

	if (idx == -1)
		return false;
	return (memblock.memory.regions[idx].base +
		 memblock.memory.regions[idx].size) >= end;
}

/**
 * memblock_is_region_reserved - check if a region intersects reserved memory
 * @base: base of region to check
 * @size: size of region to check
 *
 * Check if the region [@base, @base + @size) intersects a reserved
 * memory block.
 *
 * Return:
 * True if they intersect, false if not.
 */
bool __init_memblock memblock_is_region_reserved(phys_addr_t base, phys_addr_t size)
{
	return memblock_overlaps_region(&memblock.reserved, base, size);
}

void __init_memblock memblock_trim_memory(phys_addr_t align)
{
	phys_addr_t start, end, orig_start, orig_end;
	struct memblock_region *r;

	for_each_mem_region(r) {
		orig_start = r->base;
		orig_end = r->base + r->size;
		start = round_up(orig_start, align);
		end = round_down(orig_end, align);

		if (start == orig_start && end == orig_end)
			continue;

		if (start < end) {
			r->base = start;
			r->size = end - start;
		} else {
			memblock_remove_region(&memblock.memory,
					       r - memblock.memory.regions);
			r--;
		}
	}
}

void __init_memblock memblock_set_current_limit(phys_addr_t limit)
{
	memblock.current_limit = limit;
}

phys_addr_t __init_memblock memblock_get_current_limit(void)
{
	return memblock.current_limit;
}

static void __init_memblock memblock_dump(struct memblock_type *type)
{
	phys_addr_t base, end, size;
	enum memblock_flags flags;
	int idx;
	struct memblock_region *rgn;

	pr_info(" %s.cnt  = 0x%lx\n", type->name, type->cnt);

	for_each_memblock_type(idx, type, rgn) {
		char nid_buf[32] = "";

		base = rgn->base;
		size = rgn->size;
		end = base + size - 1;
		flags = rgn->flags;
#ifdef CONFIG_NUMA
		if (numa_valid_node(memblock_get_region_node(rgn)))
			snprintf(nid_buf, sizeof(nid_buf), " on node %d",
				 memblock_get_region_node(rgn));
#endif
		pr_info(" %s[%#x]\t[%pa-%pa], %pa bytes%s flags: %#x\n",
			type->name, idx, &base, &end, &size, nid_buf, flags);
	}
}

static void __init_memblock __memblock_dump_all(void)
{
	pr_info("MEMBLOCK configuration:\n");
	pr_info(" memory size = %pa reserved size = %pa\n",
		&memblock.memory.total_size,
		&memblock.reserved.total_size);

	memblock_dump(&memblock.memory);
	memblock_dump(&memblock.reserved);
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
	memblock_dump(&physmem);
#endif
}

void __init_memblock memblock_dump_all(void)
{
	if (memblock_debug)
		__memblock_dump_all();
}

void __init memblock_allow_resize(void)
{
	memblock_can_resize = 1;
}

static int __init early_memblock(char *p)
{
	if (p && strstr(p, "debug"))
		memblock_debug = 1;
	return 0;
}
early_param("memblock", early_memblock);

static void __init free_memmap(unsigned long start_pfn, unsigned long end_pfn)
{
	struct page *start_pg, *end_pg;
	phys_addr_t pg, pgend;

	/*
	 * Convert start_pfn/end_pfn to a struct page pointer.
	 */
	start_pg = pfn_to_page(start_pfn - 1) + 1;
	end_pg = pfn_to_page(end_pfn - 1) + 1;

	/*
	 * Convert to physical addresses, and round start upwards and end
	 * downwards.
	 */
	pg = PAGE_ALIGN(__pa(start_pg));
	pgend = PAGE_ALIGN_DOWN(__pa(end_pg));

	/*
	 * If there are free pages between these, free the section of the
	 * memmap array.
	 */
	if (pg < pgend)
		memblock_phys_free(pg, pgend - pg);
}

/*
 * The mem_map array can get very big.  Free the unused area of the memory map.
 */
static void __init free_unused_memmap(void)
{
	unsigned long start, end, prev_end = 0;
	int i;

	if (!IS_ENABLED(CONFIG_HAVE_ARCH_PFN_VALID) ||
	    IS_ENABLED(CONFIG_SPARSEMEM_VMEMMAP))
		return;

	/*
	 * This relies on each bank being in address order.
	 * The banks are sorted previously in bootmem_init().
	 */
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, NULL) {
#ifdef CONFIG_SPARSEMEM
		/*
		 * Take care not to free memmap entries that don't exist
		 * due to SPARSEMEM sections which aren't present.
		 */
		start = min(start, ALIGN(prev_end, PAGES_PER_SECTION));
#endif
		/*
		 * Align down here since many operations in VM subsystem
		 * presume that there are no holes in the memory map inside
		 * a pageblock
		 */
		start = pageblock_start_pfn(start);

		/*
		 * If we had a previous bank, and there is a space
		 * between the current bank and the previous, free it.
		 */
		if (prev_end && prev_end < start)
			free_memmap(prev_end, start);

		/*
		 * Align up here since many operations in VM subsystem
		 * presume that there are no holes in the memory map inside
		 * a pageblock
		 */
		prev_end = pageblock_align(end);
	}

#ifdef CONFIG_SPARSEMEM
	if (!IS_ALIGNED(prev_end, PAGES_PER_SECTION)) {
		prev_end = pageblock_align(end);
		free_memmap(prev_end, ALIGN(prev_end, PAGES_PER_SECTION));
	}
#endif
}

static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
	int order;

	while (start < end) {
		/*
		 * Free the pages in the largest chunks alignment allows.
		 *
		 * __ffs() behaviour is undefined for 0. start == 0 is
		 * MAX_PAGE_ORDER-aligned, set order to MAX_PAGE_ORDER for
		 * the case.
		 */
		if (start)
			order = min_t(int, MAX_PAGE_ORDER, __ffs(start));
		else
			order = MAX_PAGE_ORDER;

		while (start + (1UL << order) > end)
			order--;

		memblock_free_pages(start, order);

		start += (1UL << order);
	}
}

static unsigned long __init __free_memory_core(phys_addr_t start,
				 phys_addr_t end)
{
	unsigned long start_pfn = PFN_UP(start);
	unsigned long end_pfn = PFN_DOWN(end);

	if (!IS_ENABLED(CONFIG_HIGHMEM) && end_pfn > max_low_pfn)
		end_pfn = max_low_pfn;

	if (start_pfn >= end_pfn)
		return 0;

	__free_pages_memory(start_pfn, end_pfn);

	return end_pfn - start_pfn;
}

/*
 * Initialised pages do not have PageReserved set. This function is called
 * for each reserved range and marks the pages PageReserved.
 * When deferred initialization of struct pages is enabled it also ensures
 * that struct pages are properly initialised.
 */
static void __init memmap_init_reserved_range(phys_addr_t start,
					      phys_addr_t end, int nid)
{
	unsigned long pfn;

	for_each_valid_pfn(pfn, PFN_DOWN(start), PFN_UP(end)) {
		struct page *page = pfn_to_page(pfn);

		init_deferred_page(pfn, nid);

		/*
		 * no need for atomic set_bit because the struct
		 * page is not visible yet so nobody should
		 * access it yet.
		 */
		__SetPageReserved(page);
	}
}

static void __init memmap_init_reserved_pages(void)
{
	struct memblock_region *region;
	phys_addr_t start, end;
	int nid;
	unsigned long max_reserved;

	/*
	 * set nid on all reserved pages and also treat struct
	 * pages for the NOMAP regions as PageReserved
	 */
repeat:
	max_reserved = memblock.reserved.max;
	for_each_mem_region(region) {
		nid = memblock_get_region_node(region);
		start = region->base;
		end = start + region->size;

		if (memblock_is_nomap(region))
			memmap_init_reserved_range(start, end, nid);

		memblock_set_node(start, region->size, &memblock.reserved, nid);
	}
	/*
	 * 'max' is changed means memblock.reserved has been doubled its
	 * array, which may result a new reserved region before current
	 * 'start'. Now we should repeat the procedure to set its node id.
	 */
	if (max_reserved != memblock.reserved.max)
		goto repeat;

	/*
	 * initialize struct pages for reserved regions that don't have
	 * the MEMBLOCK_RSRV_NOINIT flag set
	 */
	for_each_reserved_mem_region(region) {
		if (!memblock_is_reserved_noinit(region)) {
			nid = memblock_get_region_node(region);
			start = region->base;
			end = start + region->size;

			if (!numa_valid_node(nid))
				nid = early_pfn_to_nid(PFN_DOWN(start));

			memmap_init_reserved_range(start, end, nid);
		}
	}
}

static unsigned long __init free_low_memory_core_early(void)
{
	unsigned long count = 0;
	phys_addr_t start, end;
	u64 i;

	memblock_clear_hotplug(0, -1);

	memmap_init_reserved_pages();

	/*
	 * We need to use NUMA_NO_NODE instead of NODE_DATA(0)->node_id
	 *  because in some case like Node0 doesn't have RAM installed
	 *  low ram will be on Node1
	 */
	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end,
				NULL)
		count += __free_memory_core(start, end);

	return count;
}

static int reset_managed_pages_done __initdata;

static void __init reset_node_managed_pages(pg_data_t *pgdat)
{
	struct zone *z;

	for (z = pgdat->node_zones; z < pgdat->node_zones + MAX_NR_ZONES; z++)
		atomic_long_set(&z->managed_pages, 0);
}

void __init reset_all_zones_managed_pages(void)
{
	struct pglist_data *pgdat;

	if (reset_managed_pages_done)
		return;

	for_each_online_pgdat(pgdat)
		reset_node_managed_pages(pgdat);

	reset_managed_pages_done = 1;
}

/**
 * memblock_free_all - release free pages to the buddy allocator
 */
void __init memblock_free_all(void)
{
	unsigned long pages;

	free_unused_memmap();
	reset_all_zones_managed_pages();

	memblock_clear_kho_scratch_only();
	pages = free_low_memory_core_early();
	totalram_pages_add(pages);
}

/* Keep a table to reserve named memory */
#define RESERVE_MEM_MAX_ENTRIES		8
#define RESERVE_MEM_NAME_SIZE		16
struct reserve_mem_table {
	char			name[RESERVE_MEM_NAME_SIZE];
	phys_addr_t		start;
	phys_addr_t		size;
};
static struct reserve_mem_table reserved_mem_table[RESERVE_MEM_MAX_ENTRIES];
static int reserved_mem_count;
static DEFINE_MUTEX(reserve_mem_lock);

/* Add wildcard region with a lookup name */
static void __init reserved_mem_add(phys_addr_t start, phys_addr_t size,
				   const char *name)
{
	struct reserve_mem_table *map;

	map = &reserved_mem_table[reserved_mem_count++];
	map->start = start;
	map->size = size;
	strscpy(map->name, name);
}

static struct reserve_mem_table *reserve_mem_find_by_name_nolock(const char *name)
{
	struct reserve_mem_table *map;
	int i;

	for (i = 0; i < reserved_mem_count; i++) {
		map = &reserved_mem_table[i];
		if (!map->size)
			continue;
		if (strcmp(name, map->name) == 0)
			return map;
	}
	return NULL;
}

/**
 * reserve_mem_find_by_name - Find reserved memory region with a given name
 * @name: The name that is attached to a reserved memory region
 * @start: If found, holds the start address
 * @size: If found, holds the size of the address.
 *
 * @start and @size are only updated if @name is found.
 *
 * Returns: 1 if found or 0 if not found.
 */
int reserve_mem_find_by_name(const char *name, phys_addr_t *start, phys_addr_t *size)
{
	struct reserve_mem_table *map;

	guard(mutex)(&reserve_mem_lock);
	map = reserve_mem_find_by_name_nolock(name);
	if (!map)
		return 0;

	*start = map->start;
	*size = map->size;
	return 1;
}
EXPORT_SYMBOL_GPL(reserve_mem_find_by_name);

/**
 * reserve_mem_release_by_name - Release reserved memory region with a given name
 * @name: The name that is attached to a reserved memory region
 *
 * Forcibly release the pages in the reserved memory region so that those memory
 * can be used as free memory. After released the reserved region size becomes 0.
 *
 * Returns: 1 if released or 0 if not found.
 */
int reserve_mem_release_by_name(const char *name)
{
	char buf[RESERVE_MEM_NAME_SIZE + 12];
	struct reserve_mem_table *map;
	void *start, *end;

	guard(mutex)(&reserve_mem_lock);
	map = reserve_mem_find_by_name_nolock(name);
	if (!map)
		return 0;

	start = phys_to_virt(map->start);
	end = start + map->size;
	snprintf(buf, sizeof(buf), "reserve_mem:%s", name);
	free_reserved_area(start, end, 0, buf);
	map->size = 0;

	return 1;
}

#ifdef CONFIG_KEXEC_HANDOVER

static int __init reserved_mem_preserve(void)
{
	unsigned int nr_preserved = 0;
	int err;

	for (unsigned int i = 0; i < reserved_mem_count; i++, nr_preserved++) {
		struct reserve_mem_table *map = &reserved_mem_table[i];
		struct page *page = phys_to_page(map->start);
		unsigned int nr_pages = map->size >> PAGE_SHIFT;

		err = kho_preserve_pages(page, nr_pages);
		if (err)
			goto err_unpreserve;
	}

	return 0;

err_unpreserve:
	for (unsigned int i = 0; i < nr_preserved; i++) {
		struct reserve_mem_table *map = &reserved_mem_table[i];
		struct page *page = phys_to_page(map->start);
		unsigned int nr_pages = map->size >> PAGE_SHIFT;

		kho_unpreserve_pages(page, nr_pages);
	}

	return err;
}

static int __init prepare_kho_fdt(void)
{
	struct page *fdt_page;
	void *fdt;
	int err;

	fdt_page = alloc_page(GFP_KERNEL);
	if (!fdt_page) {
		err = -ENOMEM;
		goto err_report;
	}

	fdt = page_to_virt(fdt_page);
	err = kho_preserve_pages(fdt_page, 1);
	if (err)
		goto err_free_fdt;

	err |= fdt_create(fdt, PAGE_SIZE);
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");
	err |= fdt_property_string(fdt, "compatible", MEMBLOCK_KHO_NODE_COMPATIBLE);

	for (unsigned int i = 0; !err && i < reserved_mem_count; i++) {
		struct reserve_mem_table *map = &reserved_mem_table[i];

		err |= fdt_begin_node(fdt, map->name);
		err |= fdt_property_string(fdt, "compatible", RESERVE_MEM_KHO_NODE_COMPATIBLE);
		err |= fdt_property(fdt, "start", &map->start, sizeof(map->start));
		err |= fdt_property(fdt, "size", &map->size, sizeof(map->size));
		err |= fdt_end_node(fdt);
	}
	err |= fdt_end_node(fdt);
	err |= fdt_finish(fdt);

	if (err)
		goto err_unpreserve_fdt;

	err = kho_add_subtree(MEMBLOCK_KHO_FDT, fdt, fdt_totalsize(fdt));
	if (err)
		goto err_unpreserve_fdt;

	err = reserved_mem_preserve();
	if (err)
		goto err_remove_subtree;

	return 0;

err_remove_subtree:
	kho_remove_subtree(fdt);
err_unpreserve_fdt:
	kho_unpreserve_pages(fdt_page, 1);
err_free_fdt:
	put_page(fdt_page);
err_report:
	pr_err("failed to prepare memblock FDT for KHO: %d\n", err);

	return err;
}

static int __init reserve_mem_init(void)
{
	int err;

	if (!kho_is_enabled() || !reserved_mem_count)
		return 0;

	err = prepare_kho_fdt();
	if (err)
		return err;
	return err;
}
late_initcall(reserve_mem_init);

static void *__init reserve_mem_kho_retrieve_fdt(void)
{
	phys_addr_t fdt_phys;
	static void *fdt;
	int err;

	if (fdt)
		return fdt;

	err = kho_retrieve_subtree(MEMBLOCK_KHO_FDT, &fdt_phys, NULL);
	if (err) {
		if (err != -ENOENT)
			pr_warn("failed to retrieve FDT '%s' from KHO: %d\n",
				MEMBLOCK_KHO_FDT, err);
		return NULL;
	}

	fdt = phys_to_virt(fdt_phys);

	err = fdt_node_check_compatible(fdt, 0, MEMBLOCK_KHO_NODE_COMPATIBLE);
	if (err) {
		pr_warn("FDT '%s' is incompatible with '%s': %d\n",
			MEMBLOCK_KHO_FDT, MEMBLOCK_KHO_NODE_COMPATIBLE, err);
		fdt = NULL;
	}

	return fdt;
}

static bool __init reserve_mem_kho_revive(const char *name, phys_addr_t size,
					  phys_addr_t align)
{
	int err, len_start, len_size, offset;
	const phys_addr_t *p_start, *p_size;
	const void *fdt;

	fdt = reserve_mem_kho_retrieve_fdt();
	if (!fdt)
		return false;

	offset = fdt_subnode_offset(fdt, 0, name);
	if (offset < 0) {
		pr_warn("FDT '%s' has no child '%s': %d\n",
			MEMBLOCK_KHO_FDT, name, offset);
		return false;
	}
	err = fdt_node_check_compatible(fdt, offset, RESERVE_MEM_KHO_NODE_COMPATIBLE);
	if (err) {
		pr_warn("Node '%s' is incompatible with '%s': %d\n",
			name, RESERVE_MEM_KHO_NODE_COMPATIBLE, err);
		return false;
	}

	p_start = fdt_getprop(fdt, offset, "start", &len_start);
	p_size = fdt_getprop(fdt, offset, "size", &len_size);
	if (!p_start || len_start != sizeof(*p_start) || !p_size ||
	    len_size != sizeof(*p_size)) {
		return false;
	}

	if (*p_start & (align - 1)) {
		pr_warn("KHO reserve-mem '%s' has wrong alignment (0x%lx, 0x%lx)\n",
			name, (long)align, (long)*p_start);
		return false;
	}

	if (*p_size != size) {
		pr_warn("KHO reserve-mem '%s' has wrong size (0x%lx != 0x%lx)\n",
			name, (long)*p_size, (long)size);
		return false;
	}

	reserved_mem_add(*p_start, size, name);
	pr_info("Revived memory reservation '%s' from KHO\n", name);

	return true;
}
#else
static bool __init reserve_mem_kho_revive(const char *name, phys_addr_t size,
					  phys_addr_t align)
{
	return false;
}
#endif /* CONFIG_KEXEC_HANDOVER */

/*
 * Parse reserve_mem=nn:align:name
 */
static int __init reserve_mem(char *p)
{
	phys_addr_t start, size, align, tmp;
	char *name;
	char *oldp;
	int len;

	if (!p)
		goto err_param;

	/* Check if there's room for more reserved memory */
	if (reserved_mem_count >= RESERVE_MEM_MAX_ENTRIES) {
		pr_err("reserve_mem: no more room for reserved memory\n");
		return -EBUSY;
	}

	oldp = p;
	size = memparse(p, &p);
	if (!size || p == oldp)
		goto err_param;

	if (*p != ':')
		goto err_param;

	align = memparse(p+1, &p);
	if (*p != ':')
		goto err_param;

	/*
	 * memblock_phys_alloc() doesn't like a zero size align,
	 * but it is OK for this command to have it.
	 */
	if (align < SMP_CACHE_BYTES)
		align = SMP_CACHE_BYTES;

	name = p + 1;
	len = strlen(name);

	/* name needs to have length but not too big */
	if (!len || len >= RESERVE_MEM_NAME_SIZE)
		goto err_param;

	/* Make sure that name has text */
	for (p = name; *p; p++) {
		if (!isspace(*p))
			break;
	}
	if (!*p)
		goto err_param;

	/* Make sure the name is not already used */
	if (reserve_mem_find_by_name(name, &start, &tmp)) {
		pr_err("reserve_mem: name \"%s\" was already used\n", name);
		return -EBUSY;
	}

	/* Pick previous allocations up from KHO if available */
	if (reserve_mem_kho_revive(name, size, align))
		return 1;

	/* TODO: Allocation must be outside of scratch region */
	start = memblock_phys_alloc(size, align);
	if (!start) {
		pr_err("reserve_mem: memblock allocation failed\n");
		return -ENOMEM;
	}

	reserved_mem_add(start, size, name);

	return 1;
err_param:
	pr_err("reserve_mem: empty or malformed parameter\n");
	return -EINVAL;
}
__setup("reserve_mem=", reserve_mem);

#ifdef CONFIG_DEBUG_FS
#ifdef CONFIG_ARCH_KEEP_MEMBLOCK
static const char * const flagname[] = {
	[ilog2(MEMBLOCK_HOTPLUG)] = "HOTPLUG",
	[ilog2(MEMBLOCK_MIRROR)] = "MIRROR",
	[ilog2(MEMBLOCK_NOMAP)] = "NOMAP",
	[ilog2(MEMBLOCK_DRIVER_MANAGED)] = "DRV_MNG",
	[ilog2(MEMBLOCK_RSRV_NOINIT)] = "RSV_NIT",
	[ilog2(MEMBLOCK_RSRV_KERN)] = "RSV_KERN",
	[ilog2(MEMBLOCK_KHO_SCRATCH)] = "KHO_SCRATCH",
};

static int memblock_debug_show(struct seq_file *m, void *private)
{
	struct memblock_type *type = m->private;
	struct memblock_region *reg;
	int i, j, nid;
	unsigned int count = ARRAY_SIZE(flagname);
	phys_addr_t end;

	for (i = 0; i < type->cnt; i++) {
		reg = &type->regions[i];
		end = reg->base + reg->size - 1;
		nid = memblock_get_region_node(reg);

		seq_printf(m, "%4d: ", i);
		seq_printf(m, "%pa..%pa ", &reg->base, &end);
		if (numa_valid_node(nid))
			seq_printf(m, "%4d ", nid);
		else
			seq_printf(m, "%4c ", 'x');
		if (reg->flags) {
			for (j = 0; j < count; j++) {
				if (reg->flags & (1U << j)) {
					seq_printf(m, "%s\n", flagname[j]);
					break;
				}
			}
			if (j == count)
				seq_printf(m, "%s\n", "UNKNOWN");
		} else {
			seq_printf(m, "%s\n", "NONE");
		}
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(memblock_debug);

static inline void memblock_debugfs_expose_arrays(struct dentry *root)
{
	debugfs_create_file("memory", 0444, root,
			    &memblock.memory, &memblock_debug_fops);
	debugfs_create_file("reserved", 0444, root,
			    &memblock.reserved, &memblock_debug_fops);
#ifdef CONFIG_HAVE_MEMBLOCK_PHYS_MAP
	debugfs_create_file("physmem", 0444, root, &physmem,
			    &memblock_debug_fops);
#endif
}

#else

static inline void memblock_debugfs_expose_arrays(struct dentry *root) { }

#endif /* CONFIG_ARCH_KEEP_MEMBLOCK */

static int memblock_reserve_mem_show(struct seq_file *m, void *private)
{
	struct reserve_mem_table *map;
	char txtsz[16];

	guard(mutex)(&reserve_mem_lock);
	for (int i = 0; i < reserved_mem_count; i++) {
		map = &reserved_mem_table[i];
		if (!map->size)
			continue;

		memset(txtsz, 0, sizeof(txtsz));
		string_get_size(map->size, 1, STRING_UNITS_2, txtsz, sizeof(txtsz));
		seq_printf(m, "%s\t\t(%s)\n", map->name, txtsz);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(memblock_reserve_mem);

static int __init memblock_init_debugfs(void)
{
	struct dentry *root;

	if (!IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK) && !reserved_mem_count)
		return 0;

	root = debugfs_create_dir("memblock", NULL);

	if (reserved_mem_count)
		debugfs_create_file("reserve_mem_param", 0444, root, NULL,
				    &memblock_reserve_mem_fops);

	memblock_debugfs_expose_arrays(root);
	return 0;
}
__initcall(memblock_init_debugfs);

#endif /* CONFIG_DEBUG_FS */
