// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM64 架构启动与平台接入主线
 * 基于 arch/arm/kernel/setup.c 演化而来
 *
 * Copyright (C) 1995-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/cache.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/panic_notifier.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/efi.h>
#include <linux/psci.h>
#include <linux/sched/task.h>
#include <linux/scs.h>
#include <linux/mm.h>

#include <asm/acpi.h>
#include <asm/fixmap.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/elf.h>
#include <asm/cpufeature.h>
#include <asm/cpu_ops.h>
#include <asm/kasan.h>
#include <asm/numa.h>
#include <asm/rsi.h>
#include <asm/scs.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/efi.h>
#include <asm/xen/hypervisor.h>
#include <asm/mmu_context.h>

static int num_standard_resources;
static struct resource *standard_resources;

phys_addr_t __fdt_pointer __initdata;
u64 mmu_enabled_at_boot __initdata;

/*
 * 标准内存资源描述
 *
 * setup_arch() 走到后半段时，会把"内核代码段 / 内核数据段 / 系统 RAM / reserved"
 * 这些资源注册进 iomem_resource，供后续 /proc/iomem 和资源冲突检查使用。
 */
static struct resource mem_res[] = {
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	}
};

#define kernel_code mem_res[0]
#define kernel_data mem_res[1]

/*
 * 记录内核入口时 x0..x3 的原始值。
 * 对 ARM64 启动协议来说，正常只要求 x0 携带 FDT 指针，x1~x3 应为 0。
 */
u64 __cacheline_aligned boot_args[4];

void __init smp_setup_processor_id(void)
{
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;

	/* 在 boot CPU 上最早建立"逻辑 CPU 0 -> 物理 MPIDR"的映射关系。
	 * 这是后续 SMP、IPI、CPU 拓扑和热插拔路径的基础。
	 */
	set_cpu_logical_map(0, mpidr);

	pr_info("Booting Linux on physical CPU 0x%010lx [0x%08x]\n",
		(unsigned long)mpidr, read_cpuid_id());
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

struct mpidr_hash mpidr_hash;
/**
 * smp_build_mpidr_hash - 预计算 MPIDR 压缩哈希所需的移位参数
 *
 * 目标是把分层的 MPIDR affinity 字段压缩成一个线性索引。
 * 这里得到的哈希并不一定最小，但必须无碰撞，便于后续快速定位 CPU。
 */
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity, fs[4], bits[4], ls;
	u64 mask = 0;
	/*
	 * Pre-scan the list of MPIDRS and filter out bits that do
	 * not contribute to affinity levels, ie they never toggle.
	 */
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits %#llx\n", mask);
	/*
	 * Find and stash the last and first bit set at all affinity levels to
	 * check how many bits are required to represent them.
	 */
	for (i = 0; i < 4; i++) {
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		/*
		 * Find the MSB bit and LSB bits position
		 * to determine how many bits are required
		 * to express the affinity level.
		 */
		ls = fls(affinity);
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
		bits[i] = ls - fs[i];
	}
	/*
	 * An index can be created from the MPIDR_EL1 by isolating the
	 * significant bits at each affinity level and by shifting
	 * them in order to compress the 32 bits values space to a
	 * compressed set of values. This is equivalent to hashing
	 * the MPIDR_EL1 through shifting and ORing. It is a collision free
	 * hash though not minimal since some levels might contain a number
	 * of CPUs that is not an exact power of 2 and their bit
	 * representation might contain holes, eg MPIDR_EL1[7:0] = {0x2, 0x80}.
	 */
	mpidr_hash.shift_aff[0] = MPIDR_LEVEL_SHIFT(0) + fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_SHIFT(1) + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = MPIDR_LEVEL_SHIFT(2) + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.shift_aff[3] = MPIDR_LEVEL_SHIFT(3) +
				  fs[3] - (bits[2] + bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[3] + bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] aff3[%u] mask[%#llx] bits[%u]\n",
		mpidr_hash.shift_aff[0],
		mpidr_hash.shift_aff[1],
		mpidr_hash.shift_aff[2],
		mpidr_hash.shift_aff[3],
		mpidr_hash.mask,
		mpidr_hash.bits);
	/*
	 * 4x is an arbitrary value used to warn on a hash table much bigger
	 * than expected on most systems.
	 */
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");
}

static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
	int size = 0;
	void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
	const char *name;

	/* 先用 early fixmap 临时映射 FDT。
	 * 这一步发生在正式 ioremap/vmalloc 尚未完全可用之前。
	 */
	if (dt_virt)
		memblock_reserve(dt_phys, size);

	/*
	 * dt_virt is a fixmap address, hence __pa(dt_virt) can't be used.
	 * Pass dt_phys directly.
	 */
	if (!early_init_dt_scan(dt_virt, dt_phys)) {
		pr_crit("\n"
			"Error: invalid device tree blob: PA=%pa, VA=%px, size=%d bytes\n"
			"The dtb must be 8-byte aligned and must not exceed 2 MB in size.\n"
			"\nPlease check your bootloader.\n",
			&dt_phys, dt_virt, size);

		/*
		 * Note that in this _really_ early stage we cannot even BUG()
		 * or oops, so the least terrible thing to do is cpu_relax(),
		 * or else we could end-up printing non-initialized data, etc.
		 */
		while (true)
			cpu_relax();
	}

	/* 早期修补完成后，把 FDT 重新映射成只读，避免后续误写。 */
	fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);

	name = of_flat_dt_get_machine_name();
	if (!name)
		return;

	pr_info("Machine model: %s\n", name);
	dump_stack_set_arch_desc("%s (DT)", name);
}

static void __init request_standard_resources(void)
{
	struct memblock_region *region;
	struct resource *res;
	unsigned long i = 0;
	size_t res_size;

	/* 先把内核代码段和数据段作为独立系统 RAM 资源插入。 */
	kernel_code.start   = __pa_symbol(_text);
	kernel_code.end     = __pa_symbol(__init_begin - 1);
	kernel_data.start   = __pa_symbol(_sdata);
	kernel_data.end     = __pa_symbol(_end - 1);
	insert_resource(&iomem_resource, &kernel_code);
	insert_resource(&iomem_resource, &kernel_data);

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc_or_panic(res_size, SMP_CACHE_BYTES);

	/* 再把 memblock 当前认定的所有内存区，按"System RAM / reserved"注册到 iomem 树。 */
	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (memblock_is_nomap(region)) {
			res->name  = "reserved";
			res->flags = IORESOURCE_MEM;
			res->start = __pfn_to_phys(memblock_region_reserved_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_reserved_end_pfn(region)) - 1;
		} else {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
			res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		}

		insert_resource(&iomem_resource, res);
	}
}

static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	/* 该步骤把 memblock 级别的"reserved"概念同步细化进 iomem resource 树。
	 * 这样用户态看到的不只是大块 System RAM，而是已经扣除了保留区的资源分裂结果。
	 */
	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

u64 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

u64 cpu_logical_map(unsigned int cpu)
{
	return __cpu_logical_map[cpu];
}

/*
 * setup_arch() — ARM64 架构初始化主入口。
 *
 * 这是 start_kernel() 回调的第一个架构级钩子，也是整个内核启动链路中
 * 最重要的架构函数之一。它的职责是把"汇编刚跳进 C 代码、只有最小页表和
 * FDT 指针"的早期状态，逐步建立为"通用内核可理解的完整平台描述"。
 *
 * 整个函数可以按依赖关系划分为七个阶段，阶段之间有严格的先后顺序：
 *
 * ┌─────────────────────────────────────────────────────────┐
 * │ 阶段一：内核镜像元信息 + KASLR + 最早期映射               │
 * │   setup_initial_init_mm → kaslr_init → early_fixmap     │
 * │   → early_ioremap → setup_machine_fdt                   │
 * │   此时：MMU=on, 仅有恒等映射+fixmap, 无完整页表            │
 * ├─────────────────────────────────────────────────────────┤
 * │ 阶段二：命令行 + CPU 异常级 + 固件                        │
 * │   jump_label_init → parse_early_param → DAIF 恢复        │
 * │   → cpu_uninstall_idmap → xen/efi 早期探测               │
 * │   此时：memblock 尚未建立, 仅通过 FDT 知道了物理内存范围     │
 * ├─────────────────────────────────────────────────────────┤
 * │ 阶段三：memblock 构建 + 正式页表 (最关键的分水岭)          │
 * │   arm64_memblock_init → paging_init                     │
 * │   此时：正式 swapper_pg_dir 就绪, 线性映射可用,            │
 * │         ID map 也已建好供次级 CPU 使用                     │
 * ├─────────────────────────────────────────────────────────┤
 * │ 阶段四：ACPI/DT + bootmem + KASAN + 资源树               │
 * │   acpi_table_upgrade → acpi_boot_table_init              │
 * │   → unflatten_device_tree → bootmem_init                 │
 * │   → kasan_init → request_standard_resources              │
 * │   此时：buddy allocator zone 划分完成, KASAN 在线          │
 * ├─────────────────────────────────────────────────────────┤
 * │ 阶段五：PSCI + CPU 操作集 + SMP 拓扑                      │
 * │   psci_dt/acpi_init → arm64_rsi_init                    │
 * │   → init_bootcpu_ops → smp_init_cpus                     │
 * │   → smp_build_mpidr_hash                                │
 * │   此时：cpu_logical_map 填充完毕, PSCI 可用,              │
 * │         后续 SMP bringup 的前置条件全部就绪                 │
 * ├─────────────────────────────────────────────────────────┤
 * │ 阶段六：安全加固收尾                                      │
 * │   TTBR0 PAN 初始化                                      │
 * │   此时：init_task 的 uaccess 保护已建立                    │
 * ├─────────────────────────────────────────────────────────┤
 * │ 阶段七：启动协议合规性检查                                 │
 * │   boot_args[1..3] 非零检查                               │
 * │   此时：发现问题可以安全报错（printk 已可用）               │
 * └─────────────────────────────────────────────────────────┘
 *
 * 调用约定：
 *   输入: cmdline_p — 输出参数，指向 bootloader 传入的命令行
 *   环境: 关中断 (IRQ/FIQ masked), 关抢占, boot CPU 单线程
 *   返回: command_line 指向 boot_command_line
 *         系统具备完整页表、memblock→zone 过渡、CPU 拓扑
 *         但 IRQ 仍然关闭，SMP 尚未 bringup
 */
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
	/*
	 * ═══════════════════════════════════════════════════════════
	 * 阶段一：内核镜像元信息 + KASLR + 最早期映射
	 * ═══════════════════════════════════════════════════════════
	 *
	 * 此时系统状态：
	 *   - MMU 已由 head.S 的 __primary_switch 打开
	 *   - 仅有恒等映射 (idmap) + 内核镜像映射
	 *   - 没有 fixmap，无法临时映射任意物理页
	 *   - 没有 ioremap，无法访问 MMIO
	 *   - 没有 memblock 视图，不知道系统有多少内存
	 *
	 * 本阶段的每个步骤都在"能做的事情极其有限"的前提下，
	 * 逐步扩大内核的能力边界。
	 */

	/*
	 * 1.1 初始化 init_mm 的边界字段。
	 *
	 * init_mm 是 0 号进程 (idle) 的地址空间描述符，也是内核线程的
	 * 默认 mm。这里把内核镜像的代码/数据/BSS 边界填入，后续页表操作、
	 * /proc/kcore 等都要依赖这些值。
	 *
	 * 注意：此时 init_mm->pgd 仍然是 head.S 中建立的早期 pgdir
	 * (init_pg_dir)，尚未切换到正式的 swapper_pg_dir。
	 */
	setup_initial_init_mm(_text, _etext, _edata, _end);

	/*
	 * 1.2 把 bootloader 传入的命令行指针交给调用方 (start_kernel)。
	 *
	 * boot_command_line 由 head.S 的 __primary_switched 从 FDT 的
	 * /chosen/bootargs 节点中复制而来（或从 .dtb 填充的默认值）。
	 */
	*cmdline_p = boot_command_line;

	/*
	 * 1.3 KASLR — 内核地址空间布局随机化。
	 *
	 * 必须在所有依赖固定 KIMAGE_VADDR 的操作之前完成：
	 *   - 如果 KASLR 启用了，kaslr_init() 会修改 KIMAGE_VADDR
	 *   - 后续所有 fixmap/FDT/ioremap 的虚拟地址都基于最终的 KIMAGE_VADDR
	 *   - 因此这是 setup_arch() 中第一个实质性操作
	 * 如果没启用 CONFIG_RANDOMIZE_BASE，此函数为空。
	 */
	kaslr_init();

	/*
	 * 1.4 建立 early fixmap — 编译期预留的固定虚拟映射窗口。
	 *
	 * fixmap 使用 BSS 中预分配的静态页表 (bm_pud/bm_pmd/bm_pte)，
	 * 因为此时 memblock 尚未初始化，无法动态分配页表页。
	 *
	 * fixmap 建立后，以下操作才变得可能：
	 *   - 临时映射 FDT 物理页 (fixmap_remap_fdt)
	 *   - early ioremap（映射外设寄存器进行早期调试）
	 *   - early console / earlyprintk
	 */
	early_fixmap_init();

	/*
	 * 1.5 建立 early ioremap — 基于 fixmap 的临时 MMIO 映射。
	 *
	 * 在正式 vmalloc/ioremap 可用之前，驱动和架构代码需要访问
	 * 少量 MMIO 寄存器（如 UART、GIC 等），early ioremap 提供
	 * 一个基于 fixmap 槽位的临时映射机制。
	 */
	early_ioremap_init();

	/*
	 * 1.6 通过 FDT 接入平台信息。
	 *
	 * 这是 ARM64 启动中第一个"信息大入口"：
	 *   - 用 fixmap_remap_fdt() 把 FDT 物理地址映射到 fixmap 虚拟窗口
	 *   - early_init_dt_scan() 解析 /chosen (bootargs, initrd, ...)
	 *   - 解析 /memory 节点 → 填充 memblock (memblock_add)
	 *   - 解析 /reserved-memory → 标记保留区
	 *   - 提取 machine model name
	 *
	 * 完成后 memblock 中已经有"物理内存的原始视图"，
	 * 但尚未进行裁剪和对齐调整（那是 arm64_memblock_init 的工作）。
	 */
	setup_machine_fdt(__fdt_pointer);

	/*
	 * ═══════════════════════════════════════════════════════════
	 * 阶段二：命令行解析 + CPU 异常级配置 + 固件早期探测
	 * ═══════════════════════════════════════════════════════════
	 *
	 * 现在 fixmap 和 FDT 都可用，memblock 中有原始内存视图。
	 * 下一步是让内核能正确处理命令行参数，并建立基本的异常处理环境。
	 */

	/*
	 * 2.1 重新初始化 static key（分支优化基础设施）。
	 *
	 * head.S 中已经调用过一次 jump_label_init()，但那时还没有
	 * 解析 FDT，不知道平台支持哪些 CPU feature。
	 * 现在 cpufeature 框架已经通过 FDT 获取了部分信息，
	 * 需要重新初始化确保后续 feature 检测能正确使用 static branch。
	 */
	jump_label_init();

	/*
	 * 2.2 解析 early param。
	 *
	 * early param 是必须在内核启动最早阶段就生效的参数，
	 * 通过 early_param() 宏注册（不同于普通的 __setup()）。
	 *
	 * 为什么在这里解析？
	 *   - mem= 等参数必须在 arm64_memblock_init() 裁剪内存前生效
	 *   - earlyprintk/earlycon 等参数要尽早配置控制台
	 *   - cpufeature 相关的命令行开关需要在此生效
	 *
	 * 这是第一次解析（还会有第二次完整解析）。
	 */
	parse_early_param();

	/*
	 * 2.3 初始化 Shadow Call Stack（如果启用）。
	 *
	 * SCS 是 ARM64 的返回地址保护机制，使用独立的 shadow stack
	 * 存储返回地址。这里根据内核参数确定是否启用并分配 SCS 区域。
	 */
	dynamic_scs_init();

	/*
	 * 2.4 恢复 DAIF 中 Debug 和 SError 的可见性。
	 *
	 * head.S 入口时 DAIF 全部屏蔽 (所有异步异常被阻塞)。
	 * 现在已经有了基本的异常向量表 (VBAR_EL1)，且 fixmap 可以
	 * 临时映射寄存器做诊断，因此可以安全地放开 Debug 和 SError。
	 *
	 * IRQ/FIQ 仍然保持关闭——根中断控制器尚未初始化，
	 * 此时收到中断无法正确处理。
	 */
	local_daif_restore(DAIF_PROCCTX_NOIRQ);

	/*
	 * 2.5 卸载早期恒等映射。
	 *
	 * 早期恒等映射 (idmap) 是 head.S 为了在 MMU 开关前后过渡
	 * 而建立的。现在内核已经通过正式映射运行，不再需要 TTBR0
	 * 上有任何有效条目。把它指向 zero page 可以：
	 *   - 避免 CPU 投机访问到残留的地址
	 *   - 为后续 KPTI 做准备（TTBR0 将被用于用户空间）
	 */
	cpu_uninstall_idmap();

	/*
	 * 2.6 Xen 半虚拟化早期初始化。
	 *
	 * 探测是否在 Xen dom0/domU 下运行，如果是则建立与 hypervisor
	 * 的通信通道。必须在内存管理完全建立前完成。
	 */
	xen_early_init();

	/*
	 * 2.7 EFI/UEFI 固件初始化。
	 *
	 * 探测 EFI 运行时服务、EFI 内存映射表等。
	 * 如果系统是通过 EFI stub 启动的，这里会接管 EFI 内存映射
	 * 并与 FDT 的内存信息合并。
	 */
	efi_init();

	/*
	 * 2.8 bootloader/固件合规性检查。
	 *
	 * 非 EFI 启动时执行两项检查：
	 *   - 内核镜像对齐：必须满足 MIN_KIMG_ALIGN (通常是 SEGMENT_ALIGN)
	 *   - MMU 状态：ARM64 Linux 启动协议要求 MMU=off 进入内核
	 *     如果发现 MMU 已开启，说明 bootloader 违反了协议，
	 *     打上 TAINT_FIRMWARE_WORKAROUND 标记继续运行
	 */
	if (!efi_enabled(EFI_BOOT)) {
		if ((u64)_text % MIN_KIMG_ALIGN)
			pr_warn(FW_BUG "Kernel image misaligned at boot, please fix your bootloader!");
		WARN_TAINT(mmu_enabled_at_boot, TAINT_FIRMWARE_WORKAROUND,
			   FW_BUG "Booted with MMU enabled!");
	}

	/*
	 * ═══════════════════════════════════════════════════════════
	 * 阶段三：memblock 视图重建 + 正式页表建立 (关键分水岭)
	 * ═══════════════════════════════════════════════════════════
	 *
	 * 这是 setup_arch() 中最关键的一段：
	 *   - arm64_memblock_init() 对原始物理内存视图进行 ARM64 特定的裁剪
	 *   - paging_init() 建立正式的 swapper_pg_dir 页表和线性映射
	 *
	 * 完成本阶段后，内核才真正拥有了"完整可用的内存管理基础设施"。
	 */

	/*
	 * 3.1 ARM64 memblock 初始化 — 裁剪和调整物理内存视图。
	 *
	 * 输入：FDT /memory 节点填充的原始 memblock 视图
	 *
	 * 主要工作：
	 *   a) 确定线性映射窗口大小 (linear_region_size = PAGE_END - PAGE_OFFSET)
	 *      - 52-bit VA + KVM nVHE 时可能需要压缩到 51 bit
	 *   b) 移除超出 CPU 物理地址位宽的 high memory
	 *   c) 选择 memstart_addr — PAGE_OFFSET 对应的物理基址
	 *      - 向下对齐到 ARM64_MEMSTART_ALIGN (通常 1GB)
	 *      - 同时满足 vmemmap 对齐要求
	 *   d) 裁剪掉线性映射窗口无法覆盖的物理内存
	 *      - 注意保护内核镜像所在的物理区间
	 *      - 若高端溢出则整体上移线性映射窗口
	 *   e) 处理 52-bit VA 构建运行在更小 VA 硬件上的退化场景
	 *   f) 应用 mem= 限制，但保护内核镜像本体
	 *   g) 校验 initrd 是否在线性映射可达范围内
	 *      - 不可达则放弃 initrd (WARN + disable)
	 *   h) reserve 内核镜像、initrd
	 *   i) 同步 FDT reserved-memory 节点到 memblock reserved 集合
	 *
	 * 输出：memblock 中只保留"内核真正打算使用的物理内存",
	 *      关键区域已被标记为 reserved
	 */
	arm64_memblock_init();

	/*
	 * 3.2 建立正式页表和线性映射。
	 *
	 * paging_init() 做四件事：
	 *   a) map_mem(swapper_pg_dir) — 遍历 memblock，为所有可用物理内存
	 *      在 PAGE_OFFSET 处建立线性映射
	 *      - 优先使用 2MB block mapping
	 *      - 内核镜像区单独用更严格的权限映射
	 *   b) memblock_allow_resize() — 允许 memblock 后续扩容
	 *   c) create_idmap() — 为 __idmap_text 建立恒等映射
	 *   d) declare_kernel_vmas() — 把 vmlinux 各段登记进内核 VMA 元数据
	 *
	 * 完成后：swapper_pg_dir 已在 TTBR1_EL1 中激活，
	 *         内核可以通过虚拟地址访问所有线性映射范围内的物理内存，
	 *         buddy/slab 等正式内存管理机制的前置条件已满足。
	 */
	paging_init();

	/*
	 * ═══════════════════════════════════════════════════════════
	 * 阶段四：ACPI/DT 设备枚举 + bootmem 过渡 + KASAN + 资源树
	 * ═══════════════════════════════════════════════════════════
	 *
	 * 页表已就绪，memlock 视图已稳定。
	 * 下一步是把 memblock 信息转成 buddy allocator 可用的 zone 划分，
	 * 同时完成设备平台的枚举 (ACPI 或 DT)。
	 */

	/*
	 * 3.3 ACPI 表升级 (initrd override)。
	 *
	 * 如果 initrd 中包含了 ACPI 表的 override 数据，
	 * 在这里把它们应用到内存中的 ACPI 表。
	 * 这会影响到后续 acpi_boot_table_init() 看到的表内容。
	 */
	acpi_table_upgrade();

	/*
	 * 4.1 ACPI 启动表初始化 或 展开设备树。
	 *
	 * ARM64 的平台设备枚举在 ACPI 和 DT 之间二选一：
	 *   - ACPI 可用 (acpi_disabled == false):
	 *     acpi_boot_table_init() 解析 ACPI 表 (MADT, SRAT, etc.)
	 *     后续设备枚举通过 ACPI namespace
	 *   - ACPI 不可用 (acpi_disabled == true):
	 *     跳过 ACPI, 转而调用 unflatten_device_tree()
	 *     把 FDT 平面结构展开为内核 device_node 树
	 *
	 * 判定依据：bootloader 是否传入了 ACPI RSDP，以及
	 * 内核命令行中是否有 "acpi=off"。
	 */
	acpi_boot_table_init();

	if (acpi_disabled)
		unflatten_device_tree();

	/*
	 * 4.2 bootmem 初始化 — 从 memblock 过渡到 buddy allocator。
	 *
	 * 这是 memblock → 正式内存管理的交接点：
	 *   a) 计算 min/max PFN (页框号)
	 *   b) early_memtest (如启用)
	 *   c) arch_numa_init() — 建立 NUMA 节点边界
	 *   d) kvm_hyp_reserve() — 为 KVM hypervisor 预留内存
	 *   e) dma_limits_init() — 确定 arm64_dma_phys_limit
	 *   f) dma_contiguous_reserve() — CMA 保留区
	 *   g) arch_reserve_crashkernel() — crashkernel 保留区
	 *   h) memblock_dump_all() — 打印最终 memblock 布局
	 *
	 * 注意：此时还没有真正调用 free_area_init() 去初始化
	 * buddy allocator。那是 mm_core_init() 在 start_kernel()
	 * 更后面的位置做的。
	 * bootmem_init() 只是做好了 zone 划分的前置准备。
	 */
	bootmem_init();

	/*
	 * 4.3 KASAN (Kernel Address SANitizer) 初始化。
	 *
	 * KASAN 的启动依赖链很长：
	 *   - 需要正式页表 (paging_init) 来建立 shadow 映射
	 *   - 需要 memblock zone 信息 (bootmem_init) 来知道物理内存范围
	 *   - shadow 内存的分配仍走 memblock (此时 buddy 还没完全上线)
	 *
	 * 所以 KASAN 必须紧接在 bootmem_init() 之后、
	 * 但早于任何可能触发 KASAN 检查的大量分配之前初始化。
	 */
	kasan_init();

	/*
	 * 4.4 注册标准资源 — 把内核代码/数据段和 System RAM 插入 iomem 资源树。
	 *
	 * 这会建立 /proc/iomem 中可见的资源层次：
	 *   - "Kernel code": _text → __init_begin
	 *   - "Kernel data": _sdata → _end
	 *   - "System RAM": 每块可用物理内存区域
	 *
	 * reserve_memblock_reserved_regions() 会作为 arch_initcall
	 * 在更后面把 reserved 区域从 System RAM 中细分出来。
	 */
	request_standard_resources();

	/*
	 * ═══════════════════════════════════════════════════════════
	 * 阶段五：PSCI + CPU 操作集 + SMP 拓扑
	 * ═══════════════════════════════════════════════════════════
	 *
	 * 内存管理已可用，设备信息已就绪。
	 * 现在可以初始化 CPU 电源管理接口和 SMP 拓扑。
	 */

	/*
	 * 4.5 重置 early ioremap — 切回常规 ioremap 语义。
	 *
	 * 正式页表和 buddy allocator 已经可用，不再需要 fixmap 槽位
	 * 充当临时 ioremap。此后 __set_fixmap 仅用于 fixmap 的原始用途
	 * (FDT, KPTI trampoline 等)。
	 */
	early_ioremap_reset();

	/*
	 * 5.1 初始化 PSCI (Power State Coordination Interface)。
	 *
	 * PSCI 是 ARM64 的 CPU 电源管理标准接口，负责：
	 *   - CPU_ON: 启动次级 CPU
	 *   - CPU_OFF: 关闭 CPU
	 *   - SYSTEM_RESET/SYSTEM_OFF: 系统复位/关机
	 *
	 * 根据平台使用 DT 还是 ACPI 选择对应的初始化方式：
	 *   - DT: psci_dt_init() 从 /psci 节点获取方法和版本
	 *   - ACPI: psci_acpi_init() 从 ACPI 表获取 PSCI 信息
	 */
	if (acpi_disabled)
		psci_dt_init();
	else
		psci_acpi_init();

	/*
	 * 5.2 ARM64 Realm Service Interface (如果运行在 CCA Realm 中)。
	 */
	arm64_rsi_init();

	/*
	 * 5.3 初始化 boot CPU 操作集。
	 *
	 * 把当前 CPU (boot CPU) 的操作回调绑定好：
	 *   - cpu_die: CPU 热拔出时调用
	 *   - cpu_kill: 确认 CPU 已停止
	 *   - cpu_disable: 检查 CPU 是否支持热拔出
	 *
	 * 后续 smp_init_cpus() 会为每个 possible CPU 绑定对应的 ops。
	 */
	init_bootcpu_ops();

	/*
	 * 5.4 初始化 SMP CPU 拓扑。
	 *
	 * 从 DT (cpu@N 节点) 或 ACPI (MADT GICC 条目) 中解析所有
	 * possible CPU 的硬件 ID (MPIDR)，填充到 cpu_logical_map[]。
	 *
	 * 此时只做登记，不真正启动次级 CPU。
	 * 实际的 SMP bringup 在后面 kernel_init_freeable() 的
	 * smp_prepare_cpus() + smp_init() 中完成。
	 */
	smp_init_cpus();

	/*
	 * 5.5 预计算 MPIDR→线性索引的哈希参数。
	 *
	 * MPIDR 是分层的 affinity 值 (Aff3.Aff2.Aff1.Aff0)，
	 * 不直接适合做数组索引。这里通过扫描所有 possible CPU
	 * 的 MPIDR，找到一个无碰撞的压缩函数，把 32-bit MPIDR
	 * 哈希成紧凑的线性 CPU 编号。
	 */
	smp_build_mpidr_hash();

	/*
	 * ═══════════════════════════════════════════════════════════
	 * 阶段六：安全加固收尾
	 * ═══════════════════════════════════════════════════════════
	 */

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * 6.1 软件 PAN (Privileged Access Never) 初始化。
	 *
	 * ARM64 PAN 阻止内核态访问用户空间地址 (防止漏洞利用)。
	 * 当硬件 PAN 不可用时，内核通过软件 PAN 模拟：
	 * 在 TTBR0_EL1 中放入一个保留页表 (reserved_pg_dir)，
	 * 其所有条目都无效。这样内核态下任何对用户地址的访问
	 * 都会触发 translation fault。
	 *
	 * 设置 init_task 的初始 ttbr0 为 reserved_pg_dir，
	 * 确保在 boot 线程的上下文中 uaccess 被默认阻止。
	 */
	init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
#endif

	/*
	 * ═══════════════════════════════════════════════════════════
	 * 阶段七：启动协议合规性检查
	 * ═══════════════════════════════════════════════════════════
	 *
	 * setup_arch() 的最后一步：验证 bootloader 是否遵循了
	 * ARM64 Linux 启动协议。此时 printk 已可用，可以安全报错。
	 */

	/*
	 * 7.1 x1-x3 非零检查。
	 *
	 * ARM64 Linux 启动协议明确规定：
	 *   - x0 = FDT 物理地址 (必须)
	 *   - x1 = x2 = x3 = 0 (保留，必须为零)
	 *
	 * 非零通常意味着：
	 *   - bootloader 使用了旧版启动协议 (如 ARM32 风格)
	 *   - bootloader 有 bug
	 *   - 内核镜像格式不正确 (如缺少 PE/COFF header)
	 *
	 * 此时内核仍然继续运行，但打印警告帮助诊断。
	 */
	if (boot_args[1] || boot_args[2] || boot_args[3]) {
		pr_err("WARNING: x1-x3 nonzero in violation of boot protocol:\n"
			"\tx1: %016llx\n\tx2: %016llx\n\tx3: %016llx\n"
			"This indicates a broken bootloader or old kernel\n",
			boot_args[1], boot_args[2], boot_args[3]);
	}
}

static inline bool cpu_can_disable(unsigned int cpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_can_disable)
		return ops->cpu_can_disable(cpu);
#endif
	return false;
}

bool arch_cpu_is_hotpluggable(int num)
{
	return cpu_can_disable(num);
}

static void dump_kernel_offset(void)
{
	const unsigned long offset = kaslr_offset();

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE) && offset > 0) {
		pr_emerg("Kernel Offset: 0x%lx from 0x%lx\n",
			 offset, KIMAGE_VADDR);
		pr_emerg("PHYS_OFFSET: 0x%llx\n", PHYS_OFFSET);
	} else {
		pr_emerg("Kernel Offset: disabled\n");
	}
}

static int arm64_panic_block_dump(struct notifier_block *self,
				  unsigned long v, void *p)
{
	dump_kernel_offset();
	dump_cpu_features();
	dump_mem_limit();
	return 0;
}

static struct notifier_block arm64_panic_block = {
	.notifier_call = arm64_panic_block_dump
};

static int __init register_arm64_panic_block(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &arm64_panic_block);
	return 0;
}
device_initcall(register_arm64_panic_block);

static int __init check_mmu_enabled_at_boot(void)
{
	if (!efi_enabled(EFI_BOOT) && mmu_enabled_at_boot)
		panic("Non-EFI boot detected with MMU and caches enabled");
	return 0;
}
device_initcall_sync(check_mmu_enabled_at_boot);
