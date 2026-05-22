// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/init.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/cache.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/gfp.h>
#include <linux/math.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/efi.h>
#include <linux/swiotlb.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/crash_dump.h>
#include <linux/hugetlb.h>
#include <linux/acpi_iort.h>
#include <linux/kmemleak.h>
#include <linux/execmem.h>

#include <asm/boot.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/kvm_host.h>
#include <asm/memory.h>
#include <asm/numa.h>
#include <asm/rsi.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <linux/sizes.h>
#include <asm/tlb.h>
#include <asm/alternative.h>
#include <asm/xen/swiotlb-xen.h>

/*
 * ARM64 早期内存初始化主线。
 *
 * 这一层的职责不是“把所有内存管理都建好”，而是先完成几个关键决策：
 * - 线性映射实际能覆盖多少物理内存
 * - PAGE_OFFSET 需要对应哪一段物理地址(memstart_addr)
 * - kernel/initrd/crashkernel/CMA 这些早期保留区如何落到 memblock
 * - 何时从 memblock 过渡到 buddy/slab 等正式页分配体系
 */

/*
 * 在 arm64_memblock_init() 真正计算出线性映射基址之前，任何对
 * memstart_addr 的误用都应尽早暴露出来，因此先给一个不可能是有效
 * 物理地址的哨兵值。
 */
s64 memstart_addr __ro_after_init = -1;
EXPORT_SYMBOL(memstart_addr);

/*
 * arm64_dma_phys_limit 表示常规 DMA 直接可达的最高物理地址上界。
 * 如果同时启用 ZONE_DMA 和 ZONE_DMA32：
 * - ZONE_DMA 优先覆盖平台声明的低地址 DMA 可达窗口
 * - ZONE_DMA32 覆盖剩余的 32-bit 可达内存
 * 若平台没有额外限制，二者通常退化成“低 32-bit RAM”这一传统布局。
 */
phys_addr_t __ro_after_init arm64_dma_phys_limit;

/*
 * 为了让线性映射尽可能使用 block/contiguous 映射，物理内存基址会向下
 * 对齐到更适合页表大块映射的粒度。这样做的目标是减少页表级数和 TLB 压力，
 * 而不是单纯追求“与 DRAM 起始地址完全相同”。
 */
#if defined(CONFIG_ARM64_4K_PAGES)
#define ARM64_MEMSTART_SHIFT		PUD_SHIFT
#elif defined(CONFIG_ARM64_16K_PAGES)
#define ARM64_MEMSTART_SHIFT		CONT_PMD_SHIFT
#else
#define ARM64_MEMSTART_SHIFT		PMD_SHIFT
#endif

/*
 * sparsemem + vmemmap 会把 struct page 数组与线性映射建立固定换算关系，
 * 因此 memstart_addr 除了要利于 block mapping，还必须满足 vmemmap 的
 * 对齐约束。
 */
#if ARM64_MEMSTART_SHIFT < SECTION_SIZE_BITS
#define ARM64_MEMSTART_ALIGN	(1UL << SECTION_SIZE_BITS)
#else
#define ARM64_MEMSTART_ALIGN	(1UL << ARM64_MEMSTART_SHIFT)
#endif

/*
 * 解析 crashkernel= 参数并在 memblock 中预留 kdump 捕获内核所需的内存。
 *
 * 必须赶在 buddy allocator 接管之前完成预留，这样 kdump 内核加载时才能
 * 从 memblock reserved 区中找到一块确保不被常规分配污染的物理连续内存。
 */
static void __init arch_reserve_crashkernel(void)
{
	unsigned long long low_size = 0;
	unsigned long long crash_base, crash_size;
	bool high = false;
	int ret;

	if (!IS_ENABLED(CONFIG_CRASH_RESERVE))
		return;

	ret = parse_crashkernel(boot_command_line, memblock_phys_mem_size(),
				&crash_size, &crash_base,
				&low_size, NULL, &high);
	if (ret)
		return;

	reserve_crashkernel_generic(crash_size, crash_base, low_size, high);
}

static phys_addr_t __init max_zone_phys(phys_addr_t zone_limit)
{
	return min(zone_limit, memblock_end_of_DRAM() - 1) + 1;
}

/*
 * 把 DMA/DMA32/NORMAL 各 zone 的 PFN 上界写入 max_zone_pfns[]。
 *
 * 这些界限被后续 free_area_init() 用来切分 buddy allocator 的 zone 边界：
 * - ZONE_DMA: 平台声明的低端 DMA 可达窗口
 * - ZONE_DMA32: 所有 32-bit 设备可寻址的内存
 * - ZONE_NORMAL: 剩余全部可用内存
 */
void __init arch_zone_limits_init(unsigned long *max_zone_pfns)
{
	phys_addr_t __maybe_unused dma32_phys_limit =
		max_zone_phys(DMA_BIT_MASK(32));

#ifdef CONFIG_ZONE_DMA
	max_zone_pfns[ZONE_DMA] = PFN_DOWN(max_zone_phys(zone_dma_limit));
#endif
#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(dma32_phys_limit);
#endif
	max_zone_pfns[ZONE_NORMAL] = max_pfn;
}

/*
 * 综合 DT/ACPI 的 DMA 范围信息，计算出 arm64_dma_phys_limit。
 *
 * 这个值是后续 CMA 保留区选址、swiotlb 启用判断、以及 page allocator zone
 * 划分的核心依据。优先级从高到低为：
 * 1. 固件声明的 ZONE_DMA 上界 (DT dma-ranges / ACPI IORT)
 * 2. DMA_BIT_MASK(32) = 4GB (ZONE_DMA32)
 * 3. PHYS_MASK + 1 (无限制)
 */
static void __init dma_limits_init(void)
{
	phys_addr_t __maybe_unused acpi_zone_dma_limit;
	phys_addr_t __maybe_unused dt_zone_dma_limit;
	phys_addr_t __maybe_unused dma32_phys_limit =
		max_zone_phys(DMA_BIT_MASK(32));

#ifdef CONFIG_ZONE_DMA
	acpi_zone_dma_limit = acpi_iort_dma_get_max_cpu_address();
	dt_zone_dma_limit = of_dma_get_max_cpu_address(NULL);
	zone_dma_limit = min(dt_zone_dma_limit, acpi_zone_dma_limit);
	/*
	 * 固件给出的 dma-ranges/ACPI 信息描述的是”总线窗口”，但具体设备的 DMA
	 * 能力可能更弱。很多驱动依然默认低 32-bit RAM 上存在 DMA zone，所以当
	 * 平台低地址确实有 RAM 时，保守地保留这块低端 DMA 区域。
	 */
	if (memblock_start_of_DRAM() < U32_MAX)
		zone_dma_limit = min(zone_dma_limit, U32_MAX);
	arm64_dma_phys_limit = max_zone_phys(zone_dma_limit);
#endif
#ifdef CONFIG_ZONE_DMA32
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = dma32_phys_limit;
#endif
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = PHYS_MASK + 1;
}

int pfn_is_map_memory(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);

	/* avoid false positives for bogus PFNs, see comment in pfn_valid() */
	if (PHYS_PFN(addr) != pfn)
		return 0;

	return memblock_is_map_memory(addr);
}
EXPORT_SYMBOL(pfn_is_map_memory);

static phys_addr_t memory_limit __ro_after_init = PHYS_ADDR_MAX;

/*
 * 解析 "mem=" 启动参数。这里限制的是内核可见的内存上界，而不是去改固件
 * 上报的真实内存布局。
 */
static int __init early_mem(char *p)
{
	if (!p)
		return 1;

	memory_limit = memparse(p, &p) & PAGE_MASK;
	pr_notice("Memory limited to %lldMB\n", memory_limit >> 20);

	return 0;
}
early_param("mem", early_mem);

void __init arm64_memblock_init(void)
{
	s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);

	/*
	 * 先确定“线性映射理论上最多能覆盖多少物理内存”。后续的 memblock 裁剪、
	 * initrd 可达性检查、memstart_addr 选取都依赖这个窗口。
	 *
	 * 特殊情况：52-bit VA + KVM nVHE 时，ID map 的放置可能让可安全使用的
	 * 线性映射窗口缩到 51 bit。这里直接把线性映射上限压到 51 bit，避免在
	 * 后续页表建立时落入这个角落问题。
	 */
	if (IS_ENABLED(CONFIG_KVM) && vabits_actual == 52 &&
	    is_hyp_mode_available() && !is_kernel_in_hyp_mode()) {
		pr_info("Capping linear region to 51 bits for KVM in nVHE mode on LVA capable hardware.\n");
		linear_region_size = min_t(u64, linear_region_size, BIT(51));
	}

	/* 超出 CPU/内核支持的物理地址位宽的内存，必须在最早阶段就从 memblock 移除。 */
	memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

	/*
	 * 选择 PAGE_OFFSET 对应的物理基址。它不一定等于 DRAM 起点，而是“在线性映射
	 * 窗口内、并满足大页映射和 vmemmap 对齐要求”的一个折中值。
	 */
	memstart_addr = round_down(memblock_start_of_DRAM(),
				   ARM64_MEMSTART_ALIGN);

	if ((memblock_end_of_DRAM() - memstart_addr) > linear_region_size)
		pr_warn("Memory doesn't fit in the linear mapping, VA_BITS too small\n");

	/*
	 * 把线性映射无法覆盖的内存从 memblock 可见范围中裁掉，但要避免把高地址
	 * 加载的内核镜像本体一并裁掉。所以裁剪下界至少要覆盖到 _end。
	 */
	memblock_remove(max_t(u64, memstart_addr + linear_region_size,
			__pa_symbol(_end)), ULLONG_MAX);
	if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
		/* 若高端仍溢出，则整体把线性映射窗口上移，同时保持对齐要求不变。 */
		memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
					 ARM64_MEMSTART_ALIGN);
		memblock_remove(0, memstart_addr);
	}

	/*
	 * 若内核按 52-bit VA 构建，但硬件实际只能提供更小的 VA 空间，线性映射
	 * 需要重新落到可达的低位 VA 区间。由于 memstart_addr 表示 PAGE_OFFSET
	 * 对应的物理地址，这里通过“减去 VA 位宽差值”来完成平移。
	 */
	if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
		memstart_addr -= _PAGE_OFFSET(vabits_actual) - _PAGE_OFFSET(52);

	/*
	 * 应用 mem= 限制。注意不能因为用户裁了内存就把当前内核镜像本体裁掉，
	 * 所以内核所在区间必须重新加回 memblock 映射集合。
	 */
	if (memory_limit != PHYS_ADDR_MAX) {
		memblock_mem_limit_remove_map(memory_limit);
		memblock_add(__pa_symbol(_text), (resource_size_t)(_end - _text));
	}

	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/*
		 * initrd 也必须能通过线性映射访问；如果前面的裁剪让它掉出了窗口，就
		 * 需要把对应物理区间重新加入 memblock。若本来就在窗口内，这里不会有
		 * 实际效果。
		 */
		phys_addr_t base = phys_initrd_start & PAGE_MASK;
		resource_size_t size = PAGE_ALIGN(phys_initrd_start + phys_initrd_size) - base;

		/*
		 * 即使要“加回” initrd，也不能突破线性映射窗口。bootloader 有责任把
		 * kernel 和 initrd 放得足够近，否则内核只能放弃 initrd。
		 */
		if (WARN(base < memblock_start_of_DRAM() ||
			 base + size > memblock_start_of_DRAM() +
				       linear_region_size,
			"initrd not fully accessible via the linear mapping -- please check your bootloader ...\n")) {
			phys_initrd_size = 0;
		} else {
			memblock_add(base, size);
			memblock_clear_nomap(base, size);
			memblock_reserve(base, size);
		}
	}

	/*
	 * 到这里，memblock 中保留下来的区间才是“早期内核真正打算使用的物理空间”。
	 * 接下来要把内核镜像、initrd、初始页表等关键对象明确标成 reserved，
	 * 防止后续 bootmem/buddy 初始化时把它们当成普通页释放出去。
	 */
	memblock_reserve(__pa_symbol(_text), _end - _text);
	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/* 通用 initrd 代码使用的是虚拟地址，因此这里补上线性映射下的 VA。 */
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	}

	/* 扫描 FDT 的 reserved-memory 节点，并同步到 memblock reserved 集合。 */
	early_init_fdt_scan_reserved_mem();
}

void __init bootmem_init(void)
{
	unsigned long min, max;

	/*
	 * 这里开始从“memblock 描述的物理布局”过渡到“页分配器认识的 PFN 范围”。
	 * min/max pfn 会被后续 zone、buddy、CMA、swiotlb 等多个子系统复用。
	 */
	min = PFN_UP(memblock_start_of_DRAM());
	max = PFN_DOWN(memblock_end_of_DRAM());

	/* 如启用 memtest，在正式放开页分配前先做一轮早期内存可用性检查。 */
	early_memtest(min << PAGE_SHIFT, max << PAGE_SHIFT);

	max_pfn = max_low_pfn = max;
	min_low_pfn = min;

	/* NUMA 拓扑需要在 zone 建立前确定好节点边界。 */
	arch_numa_init();

	/* nVHE/受保护虚拟化可能需要在普通页分配前先扣出一块 hypervisor 内存。 */
	kvm_hyp_reserve();
	dma_limits_init();

	/*
	 * CMA 需要知道 DMA 可达上界后才能决定保留区应该落在哪个物理窗口内。
	 */
	dma_contiguous_reserve(arm64_dma_phys_limit);

	/*
	 * crashkernel 必须先在 memblock 中预留出来，否则后面的资源树导出会把它
	 * 误当成普通 RAM。
	 */
	arch_reserve_crashkernel();

	/* 早期 bring-up 阶段保留完整 memblock 转储，便于核对内存裁剪和保留区。 */
	memblock_dump_all();
}

/*
 * 把 BSS 中预分配的 empty_zero_page 注册为全局零页。
 * 后续 COW、anonymous page fault 等路径都会通过 __zero_page 引用它。
 */
void __init arch_setup_zero_pages(void)
{
	__zero_page = phys_to_page(__pa_symbol(empty_zero_page));
}

/*
 * 架构层内存管理前置初始化：
 * - 判断是否需要 swiotlb：物理内存超过 DMA 可达上限时，必须为低端设备准备 bounce buffer
 * - Realm 世界强制启用 swiotlb
 * - 未对齐 kmalloc bounce 也需要一小块 swiotlb 池
 */
void __init arch_mm_preinit(void)
{
	unsigned int flags = SWIOTLB_VERBOSE;
	bool swiotlb = max_pfn > PFN_DOWN(arm64_dma_phys_limit);

	if (is_realm_world()) {
		swiotlb = true;
		flags |= SWIOTLB_FORCE;
	}

	if (IS_ENABLED(CONFIG_DMA_BOUNCE_UNALIGNED_KMALLOC) && !swiotlb) {
		/*
		 * 即使平台整体不需要传统的低端 DMA bounce，若启用了未对齐 kmalloc
		 * bounce，也至少保留一小块 swiotlb 缓冲给这类场景使用。
		 */
		unsigned long size =
			DIV_ROUND_UP(memblock_phys_mem_size(), 1024);
		swiotlb_adjust_size(min(swiotlb_size_or_default(), size));
		swiotlb = true;
	}

	swiotlb_init(swiotlb, flags);

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can be
	 * detected at build time already.
	 */
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(TASK_SIZE_32 > DEFAULT_MAP_WINDOW_64);
#endif

	/*
	 * Selected page table levels should match when derived from
	 * scratch using the virtual address range and page size.
	 */
	BUILD_BUG_ON(ARM64_HW_PGTABLE_LEVELS(CONFIG_ARM64_VA_BITS) !=
		     CONFIG_PGTABLE_LEVELS);

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get anywhere without
		 * overcommit, so turn it on by default.
		 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

bool page_alloc_available __ro_after_init;

void __init mem_init(void)
{
	/* buddy/page allocator 在这一刻之后才算真正可以被通用代码使用。 */
	page_alloc_available = true;
	swiotlb_update_mem_attributes();
}

void free_initmem(void)
{
	void *lm_init_begin = lm_alias(__init_begin);
	void *lm_init_end = lm_alias(__init_end);

	WARN_ON(!IS_ALIGNED((unsigned long)lm_init_begin, PAGE_SIZE));
	WARN_ON(!IS_ALIGNED((unsigned long)lm_init_end, PAGE_SIZE));

	free_reserved_area(lm_init_begin, lm_init_end,
			   POISON_FREE_INITMEM, "unused kernel");
	/*
	 * 释放 __init 段后，还要把对应线性映射拆掉，但保留 VM 区间占位，避免模块
	 * 再复用这段 VA 空间。否则 kallsyms 等依赖固定布局的代码会遇到歧义。
	 */
	vunmap_range((u64)__init_begin, (u64)__init_end);
}

void dump_mem_limit(void)
{
	if (memory_limit != PHYS_ADDR_MAX) {
		pr_emerg("Memory Limit: %llu MB\n", memory_limit >> 20);
	} else {
		pr_emerg("Memory Limit: none\n");
	}
}

#ifdef CONFIG_EXECMEM
static u64 module_direct_base __ro_after_init = 0;
static u64 module_plt_base __ro_after_init = 0;

/*
 * Choose a random page-aligned base address for a window of 'size' bytes which
 * entirely contains the interval [start, end - 1].
 */
static u64 __init random_bounding_box(u64 size, u64 start, u64 end)
{
	u64 max_pgoff, pgoff;

	if ((end - start) >= size)
		return 0;

	max_pgoff = (size - (end - start)) / PAGE_SIZE;
	pgoff = get_random_u32_inclusive(0, max_pgoff);

	return start - pgoff * PAGE_SIZE;
}

/*
 * Modules may directly reference data and text anywhere within the kernel
 * image and other modules. References using PREL32 relocations have a +/-2G
 * range, and so we need to ensure that the entire kernel image and all modules
 * fall within a 2G window such that these are always within range.
 *
 * Modules may directly branch to functions and code within the kernel text,
 * and to functions and code within other modules. These branches will use
 * CALL26/JUMP26 relocations with a +/-128M range. Without PLTs, we must ensure
 * that the entire kernel text and all module text falls within a 128M window
 * such that these are always within range. With PLTs, we can expand this to a
 * 2G window.
 *
 * We chose the 128M region to surround the entire kernel image (rather than
 * just the text) as using the same bounds for the 128M and 2G regions ensures
 * by construction that we never select a 128M region that is not a subset of
 * the 2G region. For very large and unusual kernel configurations this means
 * we may fall back to PLTs where they could have been avoided, but this keeps
 * the logic significantly simpler.
 */
static int __init module_init_limits(void)
{
	u64 kernel_end = (u64)_end;
	u64 kernel_start = (u64)_text;
	u64 kernel_size = kernel_end - kernel_start;

	/*
	 * The default modules region is placed immediately below the kernel
	 * image, and is large enough to use the full 2G relocation range.
	 */
	BUILD_BUG_ON(KIMAGE_VADDR != MODULES_END);
	BUILD_BUG_ON(MODULES_VSIZE < SZ_2G);

	if (!kaslr_enabled()) {
		if (kernel_size < SZ_128M)
			module_direct_base = kernel_end - SZ_128M;
		if (kernel_size < SZ_2G)
			module_plt_base = kernel_end - SZ_2G;
	} else {
		u64 min = kernel_start;
		u64 max = kernel_end;

		if (IS_ENABLED(CONFIG_RANDOMIZE_MODULE_REGION_FULL)) {
			pr_info("2G module region forced by RANDOMIZE_MODULE_REGION_FULL\n");
		} else {
			module_direct_base = random_bounding_box(SZ_128M, min, max);
			if (module_direct_base) {
				min = module_direct_base;
				max = module_direct_base + SZ_128M;
			}
		}

		module_plt_base = random_bounding_box(SZ_2G, min, max);
	}

	pr_info("%llu pages in range for non-PLT usage",
		module_direct_base ? (SZ_128M - kernel_size) / PAGE_SIZE : 0);
	pr_info("%llu pages in range for PLT usage",
		module_plt_base ? (SZ_2G - kernel_size) / PAGE_SIZE : 0);

	return 0;
}

static struct execmem_info execmem_info __ro_after_init;

struct execmem_info __init *execmem_arch_setup(void)
{
	unsigned long fallback_start = 0, fallback_end = 0;
	unsigned long start = 0, end = 0;

	module_init_limits();

	/*
	 * Where possible, prefer to allocate within direct branch range of the
	 * kernel such that no PLTs are necessary.
	 */
	if (module_direct_base) {
		start = module_direct_base;
		end = module_direct_base + SZ_128M;

		if (module_plt_base) {
			fallback_start = module_plt_base;
			fallback_end = module_plt_base + SZ_2G;
		}
	} else if (module_plt_base) {
		start = module_plt_base;
		end = module_plt_base + SZ_2G;
	}

	execmem_info = (struct execmem_info){
		.ranges = {
			[EXECMEM_DEFAULT] = {
				.start	= start,
				.end	= end,
				.pgprot	= PAGE_KERNEL,
				.alignment = 1,
				.fallback_start	= fallback_start,
				.fallback_end	= fallback_end,
			},
			[EXECMEM_KPROBES] = {
				.start	= VMALLOC_START,
				.end	= VMALLOC_END,
				.pgprot	= PAGE_KERNEL_ROX,
				.alignment = 1,
			},
			[EXECMEM_BPF] = {
				.start	= VMALLOC_START,
				.end	= VMALLOC_END,
				.pgprot	= PAGE_KERNEL,
				.alignment = 1,
			},
		},
	};

	return &execmem_info;
}
#endif /* CONFIG_EXECMEM */
