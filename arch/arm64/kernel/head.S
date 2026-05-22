/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 最底层 CPU 初始化入口
 * 基于 arch/arm/kernel/head.S 演化而来
 *
 * Copyright (C) 1994-2002 Russell King
 * Copyright (C) 2003-2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/pgtable.h>

#include <asm/asm_pointer_auth.h>
#include <asm/assembler.h>
#include <asm/boot.h>
#include <asm/bug.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/cache.h>
#include <asm/cputype.h>
#include <asm/el2_setup.h>
#include <asm/elf.h>
#include <asm/image.h>
#include <asm/kernel-pgtable.h>
#include <asm/kvm_arm.h>
#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>
#include <asm/page.h>
#include <asm/scs.h>
#include <asm/smp.h>
#include <asm/sysreg.h>
#include <asm/stacktrace/frame.h>
#include <asm/thread_info.h>
#include <asm/virt.h>

#include "efi-header.S"

#if (PAGE_OFFSET & 0x1fffff) != 0
#error PAGE_OFFSET must be at least 2MB aligned
#endif

/*
 * ============================================================================
 *  ARM64 内核启动入口文件
 * ============================================================================
 *
 * 整体启动流程:
 *   primary_entry                         -- 主核入口 (MMU=off)
 *     ├─ record_mmu_state                 -- 记录 MMU/缓存 初始状态
 *     ├─ preserve_boot_args               -- 保存 bootloader 传入的参数 (x0~x3)
 *     ├─ __pi_create_init_idmap           -- 在物理地址空间创建初始恒等映射
 *     ├─ 缓存维护 (根据 MMU 状态)
 *     ├─ init_kernel_el                   -- 确定异常级别并初始化
 *     ├─ __cpu_setup                      -- CPU 特定初始化 (TCR/MAIR 等)
 *     └─ __primary_switch                 -- MMU 开启 + 内核映射 + 重定位
 *           ├─ __enable_mmu               -- 加载 TTBR，开启 MMU
 *           ├─ __pi_early_map_kernel      -- 映射内核镜像、FDT、清除 BSS
 *           └─ __primary_switched         -- MMU 开启后第一个虚拟地址
 *                 ├─ init_cpu_task        -- 初始化任务上下文 (sp_el0, stack, per-CPU)
 *                 ├─ 设置 VBAR_EL1        -- 加载异常向量表
 *                 ├─ 保存 FDT 指针、虚拟偏移
 *                 ├─ kasan_early_init     -- KASAN 早期初始化
 *                 ├─ finalise_el2         -- 确定最终 EL2 配置 (VHE)
 *                 └─ start_kernel()       -- 进入 C 代码!
 *
 * 从核启动 (SMP):
 *   secondary_holding_pen                 -- 从核轮询等待释放信号
 *     └─ secondary_startup               -- 从核正式启动
 *           ├─ __cpu_secondary_check52bitva  -- [可选] 52位 VA检查
 *           ├─ __cpu_setup               -- CPU 初始化
 *           ├─ __enable_mmu              -- 开启 MMU
 *           └─ __secondary_switched      -- 加载任务 + 进入 secondary_start_kernel
 *
 * 被调用方保存寄存器 (callee-saved registers) 的使用约定:
 *   寄存器    使用范围                        用途
 *   x19       primary_entry() .. start_kernel()       MMU 是否在入口时开启
 *   x20       primary_entry() .. __primary_switch()    CPU 启动模式 (EL1/EL2)
 *   x21       primary_entry() .. start_kernel()       FDT 物理地址 (来自 x0)
 */

/*
 * Kernel 启动入口约束
 * -------------------
 *
 * 入口要求：
 *   - MMU = off
 *   - D-cache = off
 *   - I-cache = on 或 off 均可
 *   - x0 = FDT blob 的物理地址
 *
 * 另外要注意，若干被调用方保存寄存器会在 MMU 开启前承担“启动期变量寄存器”的角色。
 * 它们的具体分配和生命周期，在各入口例程的注释里说明。
 */
	__HEAD
	/*
	 * DO NOT MODIFY. Image header expected by Linux boot-loaders.
	 *
	 * 内核镜像头部 (Image header)，bootloader 通过此头部识别内核镜像。
	 * 结构:
	 *   [0]   efi_signature_nop   - PE/COFF 签名 NOP (EFI 启动标识)
	 *   [4]   b primary_entry     - 跳转指令，跳到内核入口
	 *   [8]   .quad 0             - Image 在 RAM 中的加载偏移 (小端)
	 *   [16]  _kernel_size_le     - 内核镜像有效大小 (小端)
	 *   [24]  _kernel_flags_le    - 内核标志 (小端，如 PAGE_SIZE, 字节序)
	 *   [32]  .quad 0 (×3)        - 保留字段
	 *   [56]  ARM64_IMAGE_MAGIC   - 魔数 "ARM\x64" 用于校验
	 *   [60]  PE header offset    - PE 头部偏移 (用于 EFI 启动)
	 */
	efi_signature_nop			// special NOP to identity as PE/COFF executable
	b	primary_entry			// branch to kernel start, magic
	.quad	0				// Image load offset from start of RAM, little-endian
	le64sym	_kernel_size_le			// Effective size of kernel image, little-endian
	le64sym	_kernel_flags_le		// Informative flags, little-endian
	.quad	0				// reserved
	.quad	0				// reserved
	.quad	0				// reserved
	.ascii	ARM64_IMAGE_MAGIC		// Magic number
	.long	.Lpe_header_offset		// Offset to the PE header.

	__EFI_PE_HEADER

	/*
	 * .idmap.text 段：恒等映射文本段
	 *
	 * 这里的代码必须能同时跨越“MMU 关闭”和“MMU 刚开启”两个阶段执行，
	 * 因为此时 PC 不能因为地址空间切换而失效。
	 * 内核通过初始 idmap 保证这段代码满足 VA == PA，
	 * 所有 MMU 切换前后紧邻执行的代码都必须放在这里。
	 */
	.section ".idmap.text","a"

	/*
	 * 主核低级启动路径里会占用以下被调用方保存通用寄存器：
	 *
	 *  寄存器   生命周期范围                         用途
	 *  x19      primary_entry() .. start_kernel()   记录进入时 MMU 是否已开启
	 *  x20      primary_entry() .. __primary_switch() 记录 CPU 启动模式
	 *  x21      primary_entry() .. start_kernel()   保存 bootloader 传入的 FDT 指针
	 */
SYM_CODE_START(primary_entry)
	/*
	 * primary_entry: 主核入口点
	 * 输入: x0 = FDT 物理地址
	 * 状态: MMU=off, D-cache=off
	 */
	bl	record_mmu_state		// ★ 步骤1: 记录 MMU/缓存/字节序初始状态
	bl	preserve_boot_args		// ★ 步骤2: 保存 bootloader 参数 (FDT地址存入x21)

	/*
	 * 设置早期启动栈 (early_init_stack)
	 * 此时 MMU 仍未开启，使用物理地址访问栈
	 */
	adrp	x1, early_init_stack
	mov	sp, x1
	mov	x29, xzr			// fp=0，终止帧链回溯

	/*
	 * ★ 步骤3: 创建初始恒等映射 (identity mapping)
	 * 输入: x0 = __pi_init_idmap_pg_dir (初始页表目录物理地址)
	 *       x1 = 0 (不使用)
	 *
	 * 此函数在 MMU 关闭的情况下运行 (PI = position independent)，
	 * 为内核镜像区域建立恒等映射:
	 *   - .head.text ~ __initdata_begin → 只读可执行映射
	 *   - __initdata_begin ~ _end       → 读写数据映射
	 * 返回: x0 = 已使用的页表区域末尾地址
	 */
	adrp	x0, __pi_init_idmap_pg_dir
	mov	x1, xzr
	bl	__pi_create_init_idmap

	/*
	 * ★ 步骤4: 缓存维护
	 * 如果页表是通过 non-cacheable 访问填充的 (MMU=off)，
	 * 需要将页表缓存行无效化以清除可能被投机加载的脏行。
	 *
	 * 如果进入时 MMU 和缓存已开启，则需要将恒等映射区域的
	 * 代码 clean 到 PoC (Point of Coherency)，以确保在 MMU
	 * 关闭后可以安全执行这些代码。
	 */
	cbnz	x19, 0f				// x19!=0 表示进入时 MMU 已开启
	/*
	 * MMU 关闭路径: 无效化初始页表区域的 D-cache
	 * 范围: [__pi_init_idmap_pg_dir, x0)  (x0 是 create_init_idmap 的返回值)
	 */
	dmb     sy
	mov	x1, x0				// end of used region
	adrp    x0, __pi_init_idmap_pg_dir
	adr_l	x2, dcache_inval_poc
	blr	x2
	b	1f

	/*
	 * MMU 开启路径: 将恒等映射区域的代码 Clean 到 PoC
	 * 以确保关闭 MMU 后仍能正确取指
	 * 范围: [__idmap_text_start, __idmap_text_end)
	 */
0:	adrp	x0, __idmap_text_start
	adr_l	x1, __idmap_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

	/*
	 * ★ 步骤5: 初始化异常级别
	 * 输入: x0 = x19 (MMU 进入时状态)
	 *
	 * 确定当前异常级别 (EL1 或 EL2)，配置 CPU 运行在
	 * 内核支持的最高可达异常级别。
	 * - 如果在 EL2: 配置 EL2 后，通过 ERET 降到 EL1
	 * - 如果在 EL1: 直接配置 EL1
	 * 返回: w0 = BOOT_CPU_MODE_EL1 或 BOOT_CPU_MODE_EL2
	 */
1:	mov	x0, x19
	bl	init_kernel_el			// w0=cpu_boot_mode
	mov	x20, x0				// x20 保存启动模式

	/*
	 * ★ 步骤6: CPU 特定设置
	 * 配置 TCR (Translation Control Register)、MAIR (Memory Attribute
	 * Indirection Register)、SCTLR 等系统寄存器。
	 * 返回后 CPU 已准备好开启 MMU。
	 * 实现位于 arch/arm64/mm/proc.S
	 */
	bl	__cpu_setup			// initialise processor

	/*
	 * ★ 步骤7: 主核切换
	 * 开启 MMU，映射内核，重定位到虚拟地址空间，
	 * 最终跳转到 __primary_switched
	 */
	b	__primary_switch
SYM_CODE_END(primary_entry)

	__INIT
/*
 * record_mmu_state: 记录 MMU 和缓存的初始状态
 *
 * 结果存入 x19:
 *   x19 = 0:                     MMU 关闭 (正常冷启动)
 *   x19 = SCTLR_ELx_M:           MMU 开启 (如 EFI 启动)
 *   x19 = SCTLR_ELx_M|SCTLR_ELx_C: MMU 和缓存都开启
 *
 * 如果当前字节序与内核不一致，还会修正字节序。
 * 注意: 修正字节序后 MMU 必须关闭，否则恒等映射会按错误的字节序解释。
 */
SYM_CODE_START_LOCAL(record_mmu_state)
	mrs	x19, CurrentEL			// 读取当前异常级别
	cmp	x19, #CurrentEL_EL2
	mrs	x19, sctlr_el1			// 先读 SCTLR_EL1
	b.ne	0f				// 不在 EL2 → 使用 EL1 的值
	mrs	x19, sctlr_el2			// 在 EL2 → 使用 SCTLR_EL2
0:
	/*
	 * 字节序检查
	 * CPU_LE (小端): 如果 EE 位为 1，说明当前是大端，需修正
	 * CPU_BE (大端): 如果 EE 位为 0，说明当前是小端，需修正
	 */
CPU_LE( tbnz	x19, #SCTLR_ELx_EE_SHIFT, 1f	)
CPU_BE( tbz	x19, #SCTLR_ELx_EE_SHIFT, 1f	)
	/*
	 * 字节序匹配: 提取 M 位 (MMU 状态) 和 C 位 (D-cache 状态)
	 *   x19 = 0:                MMU=off, D-cache=off
	 *   x19 = SCTLR_ELx_M:      MMU=on,  D-cache=off
	 *   x19 = SCTLR_ELx_M|C:    MMU=on,  D-cache=on
	 */
	tst	x19, #SCTLR_ELx_C		// Z := (C == 0)
	and	x19, x19, #SCTLR_ELx_M		// isolate M bit
	csel	x19, xzr, x19, eq		// clear x19 if Z
	ret

	/*
	 * 字节序不匹配: 修正字节序
	 * 翻转 EE 位，清除 M 位（必须关闭 MMU），然后写回 SCTLR
	 */
1:	eor	x19, x19, #SCTLR_ELx_EE
	bic	x19, x19, #SCTLR_ELx_M
	b.ne	2f
	pre_disable_mmu_workaround
	msr	sctlr_el2, x19
	b	3f
2:	pre_disable_mmu_workaround
	msr	sctlr_el1, x19
3:	isb
	mov	x19, xzr			// 强制表示 MMU=off
	ret
SYM_CODE_END(record_mmu_state)

/*
 * preserve_boot_args: 保存 bootloader 传入的参数
 *
 * 将 x0~x3 保存到 boot_args[] 数组中。
 * x0 = FDT 物理地址 → x21  (被调用方保存，启动全程使用)
 *
 * 如果 MMU 关闭 (x19==0)，还需要 DMB 后无效化 boot_args 所在缓存行，
 * 确保后续 MMU 开启后通过虚拟地址能读到正确的值。
 */
SYM_CODE_START_LOCAL(preserve_boot_args)
	mov	x21, x0				// x21=FDT (被调用方保存，boot全程使用)

	adr_l	x0, boot_args			// 将 x0~x3 保存到 boot_args[]
	stp	x21, x1, [x0]			// boot_args[0]=FDT, [1]=x1
	stp	x2, x3, [x0, #16]		// boot_args[2]=x2,  [3]=x3

	cbnz	x19, 0f				// MMU 开启则跳过缓存无效化
	dmb	sy				// MMU 关闭时需要 DMB
						// 确保之前的存储对 dc ivac 可见

	add	x1, x0, #0x20			// 4 × 8 bytes
	b	dcache_inval_poc		// tail call: 无效化 boot_args 的缓存行
0:	str_l   x19, mmu_enabled_at_boot, x0	// 保存 MMU 启动状态到全局变量
	ret
SYM_CODE_END(preserve_boot_args)

	/*
	 * init_cpu_task: 初始化 CPU 任务上下文
	 *
	 * 为当前 CPU 设置:
	 * 1. sp_el0 → 指向任务结构体 (用于用户态访问)
	 * 2. 栈指针 → task.stack + THREAD_SIZE - PT_REGS_SIZE
	 * 3. 帧记录 → 创建最终帧 (final frame)，栈回溯的终点
	 * 4. SCS (Shadow Call Stack) → 加载影子调用栈
	 * 5. per-CPU 偏移 → set_this_cpu_offset
	 *
	 * 输入:
	 *   tsk  = 任务结构体指针
	 *   tmp1, tmp2 = 临时寄存器
	 */
	.macro	init_cpu_task tsk, tmp1, tmp2
	msr	sp_el0, \tsk			// 设置用户态栈指针指向 task_struct

	ldr	\tmp1, [\tsk, #TSK_STACK]	// \tmp1 = task->stack
	add	sp, \tmp1, #THREAD_SIZE		// sp = 栈顶 (栈向下增长)
	sub	sp, sp, #PT_REGS_SIZE		// 预留 pt_regs 空间 (栈底)

	/*
	 * 创建最终帧记录 (final frame record)
	 * 栈回溯器 (unwinder) 通过栈底位置识别最终帧
	 * 此处 x29(fp) 和 x30(lr) 均为 0，标识调用链的终点
	 */
	stp	xzr, xzr, [sp, #S_STACKFRAME]
	mov	\tmp1, #FRAME_META_TYPE_FINAL
	str	\tmp1, [sp, #S_STACKFRAME_TYPE]
	add	x29, sp, #S_STACKFRAME		// fp 指向帧记录

	scs_load_current			// [可选] 加载影子调用栈

	/*
	 * 设置 per-CPU 偏移量
	 * 内核通过 this_cpu_offset 访问 per-CPU 变量:
	 *   per_cpu_var = base + this_cpu_offset[cpu]
	 */
	adr_l	\tmp1, __per_cpu_offset
	ldr	w\tmp2, [\tsk, #TSK_TI_CPU]	// w\tmp2 = CPU number
	ldr	\tmp1, [\tmp1, \tmp2, lsl #3]	// \tmp1 = __per_cpu_offset[cpu]
	set_this_cpu_offset \tmp1
	.endm

/*
 * __primary_switched: MMU 开启后的第一个虚拟地址
 *
 * 此时:
 * - MMU 已开启，运行在虚拟地址空间
 * - 恒等映射仍然存在 (idmap_pg_dir)
 * - 内核已被映射到 swapper_pg_dir
 *
 * 输入:
 *   x0 = __pa(KERNEL_START)   -- 内核物理起始地址
 */
SYM_FUNC_START_LOCAL(__primary_switched)
	adr_l	x4, init_task			// x4 = &init_task (swapper进程)
	init_cpu_task x4, x5, x6		// ★ 初始化 init_task 的 CPU 上下文

	/*
	 * ★ 加载异常向量表
	 * 将虚拟地址空间中的异常向量表基址写入 VBAR_EL1
	 */
	adr_l	x8, vectors			// load VBAR_EL1 with virtual
	msr	vbar_el1, x8			// vector table address
	isb

	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

	/*
	 * ★ 保存关键全局变量
	 */
	str_l	x21, __fdt_pointer, x5		// 保存 FDT 指针供后续 setup_arch 使用

	adrp	x4, _text			// 计算内核虚拟地址偏移:
	sub	x4, x4, x0			//   kimage_voffset = virt_addr - phys_addr
	str_l	x4, kimage_voffset, x5		// 供后续 phys_to_virt/virt_to_phys 使用

	/*
	 * ★ 设置 CPU 启动模式标志
	 * 将 boot_cpu_mode 写入 __boot_cpu_mode
	 */
	mov	x0, x20
	bl	set_cpu_boot_mode_flag

#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	bl	kasan_early_init		// ★ KASAN 早期初始化 (阴影内存映射)
#endif
	mov	x0, x20
	bl	finalise_el2			// ★ 最终化 EL2 配置: 如果支持 VHE 则启用
	ldp	x29, x30, [sp], #16
	bl	start_kernel			// ★★★ 进入 C 代码! init/main.c
	ASM_BUG()				// start_kernel 不应返回，否则触发 BUG
SYM_FUNC_END(__primary_switched)

/*
 * end early head section, begin head code that is also used for
 * hotplug and needs to have the same protections as the text region
 */
	.section ".idmap.text","a"

/*
 * init_kernel_el: 初始化内核异常级别
 *
 * 功能: 从 EL2 或 EL1 启动，配置 CPU 运行在内核支持的最高可达异常级别。
 * 如果从 EL2 降到 EL1，先配 EL2 再配 EL1。
 *
 * 注意: 由于不能保证 ERET 同步写回系统寄存器 (SCTLR_ELx.EOS 可能为0)，
 * 在 ERET 前插入 ISB 进行同步。
 *
 * 输入: x0 = 是否从主启动路径调用且 MMU 已开启
 * 返回: w0 = BOOT_CPU_MODE_EL1 或 BOOT_CPU_MODE_EL2
 *       高32位可能包含上下文标志 (如 BOOT_CPU_FLAG_E2H)
 *
 * 流程:
 *   CurrentEL==EL2? → init_el2 (配EL2 → ERET到EL1)
 *   CurrentEL==EL1? → init_el1 (直接配EL1 → ERET到EL1)
 */
SYM_FUNC_START(init_kernel_el)
	mrs	x1, CurrentEL
	cmp	x1, #CurrentEL_EL2
	b.eq	init_el2			// 在 EL2 → 初始化 EL2

	/*
	 * init_el1: 在 EL1 启动的初始化
	 *
	 * 1. 将 SCTLR_EL1 设为已知安全状态 (MMU=off)
	 * 2. 设置 SPSR_EL1 为 EL1 初始 PSTATE (所有异常屏蔽)
	 * 3. 设置 ELR_EL1 为返回地址 (lr)
	 * 4. 通过 ERET "返回"到 EL1 (实际上就是停留在 EL1)
	 * 5. 返回 BOOT_CPU_MODE_EL1
	 */
SYM_INNER_LABEL(init_el1, SYM_L_LOCAL)
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF	// 安全默认: MMU=off, 对齐检查=off
	pre_disable_mmu_workaround
	msr	sctlr_el1, x0
	isb
	mov_q	x0, INIT_PSTATE_EL1		// 初始处理器状态: D/A/I/F 全屏蔽
	msr	spsr_el1, x0			// ERET 时将此值加载到 PSTATE
	msr	elr_el1, lr			// ERET 返回地址 = 调用者
	mov	w0, #BOOT_CPU_MODE_EL1		// 返回 BOOT_CPU_MODE_EL1
	eret

	/*
	 * init_el2: 在 EL2 启动的初始化
	 *
	 * 流程:
	 * 1. 设置 ELR_EL2 为返回地址
	 * 2. [MMU=on] 将 HYP 代码 Clean 到 PoC
	 * 3. 将 SCTLR_EL2 设为安全状态 (MMU=off)
	 * 4. 配置 HCR_EL2 (Hypervisor Configuration Register)
	 * 5. 配置 EL2 系统寄存器 (通过 init_el2_state 宏)
	 * 6. 安装 HYP 桩向量表 (__hyp_stub_vectors)
	 * 7. 配置 SCTLR_EL1 (通过 VHE 路径或直接)
	 * 8. 设置 SPSR_EL2 为 EL1 模式
	 * 9. ERET → 降到 EL1
	 * 10. 返回 BOOT_CPU_MODE_EL2 (可能带 E2H 标志)
	 */
SYM_INNER_LABEL(init_el2, SYM_L_LOCAL)
	msr	elr_el2, lr			// ERET 返回地址

	/*
	 * 如果以 MMU 开启的状态进入 EL2 (如 EFI 启动)，
	 * 需要将 HYP 代码 Clean 到 PoC，以确保关闭 MMU 后
	 * 可以安全执行。
	 * 范围: [__hyp_idmap_text_start, __hyp_text_end)
	 */
	cbz	x0, 0f				// MMU=off 则跳过
	adrp	x0, __hyp_idmap_text_start
	adr_l	x1, __hyp_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

	mov_q	x0, INIT_SCTLR_EL2_MMU_OFF	// 安全默认
	pre_disable_mmu_workaround
	msr	sctlr_el2, x0
	isb
0:
	/*
	 * 配置 HCR_EL2:
	 * HCR_HOST_NVHE_FLAGS: 将大部分异常路由到 EL2 (非 VHE 模式)
	 * HCR_ATA: 允许 EL1 使用地址认证 (PAC)
	 */
	init_el2_hcr	HCR_HOST_NVHE_FLAGS | HCR_ATA
	init_el2_state				// 初始化 EL2 系统寄存器

	/* Hypervisor stub: 安装 EL2 异常向量表 */
	adr_l	x0, __hyp_stub_vectors
	msr	vbar_el2, x0
	isb

	mov_q	x1, INIT_SCTLR_EL1_MMU_OFF	// 准备 EL1 的 SCTLR

	/*
	 * 检查是否支持 VHE (Virtualization Host Extensions)
	 * VHE 允许内核直接运行在 EL2，减少虚拟化开销。
	 *
	 * HCR_E2H=1 → VHE 路径: SCTLR_EL1 通过 SCTLR_EL12 设置 ("影子"访问)
	 * HCR_E2H=0 → 非VHE路径: 直接写 SCTLR_EL1
	 */
	mrs	x0, hcr_el2
	and	x0, x0, #HCR_E2H
	cbz	x0, 2f

	/* VHE 路径：通过 _EL12 访问器间接编程“来宾视角”的 SCTLR_EL1。 */
	msr_s	SYS_SCTLR_EL12, x1
	mov	x2, #BOOT_CPU_FLAG_E2H		// 标记: 支持 VHE
	b	3f

2:
	/* 非 VHE 路径：直接由 EL2 写入真正的 SCTLR_EL1。 */
	msr	sctlr_el1, x1
	mov	x2, xzr				// 无 E2H 标志
3:
	mov	x0, #INIT_PSTATE_EL1		// SPSR: 将目标设为 EL1 (D/A/I/F 屏蔽)
	msr	spsr_el2, x0

	mov	w0, #BOOT_CPU_MODE_EL2		// 返回 BOOT_CPU_MODE_EL2
	orr	x0, x0, x2			// 合并 E2H 标志
	eret					// ★ 通过 ERET 降到 EL1，继续执行内核常规 EL1 路径
SYM_FUNC_END(init_kernel_el)

	/*
	 * secondary_holding_pen：从核“等候区”
	 *
	 * 很多平台会先把所有从核停在这里，等主核完成页表、内存和 SMP 基础设施后，
	 * 再逐个放行。
	 *
	 * 机制很简单：
	 *   - 每个从核读出自己的 MPIDR 硬件 ID
	 *   - 轮询 secondary_holding_pen_release
	 *   - 当全局值等于自己的 MPIDR 时，转入 secondary_startup
	 *
	 * 主核通常在 smp_prepare_cpus()/__cpu_up() 相关路径里写这个全局变量来放行从核。
	 */
SYM_FUNC_START(secondary_holding_pen)
	mov	x0, xzr				// MMU=off 调用 init_kernel_el
	bl	init_kernel_el			// w0=cpu_boot_mode
	mrs	x2, mpidr_el1			// 读取自己的 MPIDR (CPU 唯一ID)
	mov_q	x1, MPIDR_HWID_BITMASK
	and	x2, x2, x1			// 提取硬件 ID
	adr_l	x3, secondary_holding_pen_release
pen:	ldr	x4, [x3]			// 读取释放信号
	cmp	x4, x2				// 是释放我吗?
	b.eq	secondary_startup		// 是 → 启动
	wfe					// Wait For Event：低功耗等待事件
	b	pen				// 继续轮询
SYM_FUNC_END(secondary_holding_pen)

	/*
	 * secondary_entry：从核直接入口
	 *
	 * 主要用于 CPU hotplug 这类动态上线场景。
	 * 与 secondary_holding_pen 不同，它不需要先在“等候区”轮询，
	 * 而是直接走从核启动主线。
	 */
SYM_FUNC_START(secondary_entry)
	mov	x0, xzr				// MMU=off
	bl	init_kernel_el			// w0=cpu_boot_mode
	b	secondary_startup
SYM_FUNC_END(secondary_entry)

	/*
	 * secondary_startup：从核通用启动路径
	 *
	 * 输入：
	 *   x0 = cpu_boot_mode
	 *
	 * 这是所有“真正被放行的从核”都会走的统一主线。
	 * 重点不在于建立新页表，而在于：
	 *   - 复用主核已经准备好的内核页表
	 *   - 做每个 CPU 自己必须做的体系结构初始化
	 *   - 安全地切进虚拟地址空间
	 */
SYM_FUNC_START_LOCAL(secondary_startup)
	mov	x20, x0				// preserve boot mode

#ifdef CONFIG_ARM64_VA_BITS_52
	/*
	 * 52 位 VA 检查：
	 * 如果整个系统最终选择了 52 位虚拟地址，但这个从核本身不支持，
	 * 就不能让它继续加入系统，只能把它标成 stuck 并停住。
	 */
alternative_if ARM64_HAS_VA52
	bl	__cpu_secondary_check52bitva
alternative_else_nop_endif
#endif

	bl	__cpu_setup			// initialise processor

	/*
	 * 从核不重新造页表，而是直接复用主核已经建立好的两份关键页表：
	 *   x1 = swapper_pg_dir  → TTBR1，提供高地址内核映射
	 *   x2 = idmap_pg_dir    → TTBR0，提供切换阶段的恒等映射
	 */
	adrp	x1, swapper_pg_dir
	adrp	x2, idmap_pg_dir
	bl	__enable_mmu			// ★ 开启 MMU
	ldr	x8, =__secondary_switched	// MMU 开启后的目标虚拟地址
	br	x8
SYM_FUNC_END(secondary_startup)

	.text
/*
 * __secondary_switched：从核开启 MMU 后落到的第一个虚拟地址
 *
 * 从这里开始：
 *   - 从核已经进入虚拟地址空间
 *   - 页表与主核一致
 *   - 可以开始装载每 CPU 上下文、异常向量和 secondary task
 */
SYM_FUNC_START_LOCAL(__secondary_switched)
	mov	x0, x20
	bl	set_cpu_boot_mode_flag		// 记录启动模式

	mov	x0, x20
	bl	finalise_el2			// 最终化 EL2 配置

	str_l	xzr, __early_cpu_boot_status, x3	// 清除启动状态标志
	adr_l	x5, vectors
	msr	vbar_el1, x5			// 加载异常向量表
	isb

	/*
	 * 取出主核预先为该从核准备好的 task_struct。
	 * secondary_data.task 通常由 __cpu_up() 之类的主核路径提前填好。
	 */
	adr_l	x0, secondary_data
	ldr	x2, [x0, #CPU_BOOT_TASK]	// x2 = secondary_data.task
	cbz	x2, __secondary_too_slow	// 任务还未准备好 → 卡住

	init_cpu_task x2, x1, x3		// 初始化 CPU 任务上下文

#ifdef CONFIG_ARM64_PTR_AUTH
	ptrauth_keys_init_cpu x2, x3, x4, x5	// 初始化指针认证密钥
#endif

	bl	secondary_start_kernel		// ★ 进入从核 C 入口
	ASM_BUG()
SYM_FUNC_END(__secondary_switched)

/*
 * __secondary_too_slow: 从核任务未就绪
 * 主核尚未设置 secondary_data.task，该从核进入低功耗等待循环。
 */
SYM_FUNC_START_LOCAL(__secondary_too_slow)
	wfe
	wfi
	b	__secondary_too_slow
SYM_FUNC_END(__secondary_too_slow)

/*
 * set_cpu_boot_mode_flag：记录 CPU 的实际启动模式
 *
 * 输入：
 *   w0 = BOOT_CPU_MODE_EL1 或 BOOT_CPU_MODE_EL2
 *
 * __boot_cpu_mode 是个 64 位槽位，拆成两个 32 位使用：
 *   [31:0]  保存一种启动模式
 *   [63:32] 在不同 CPU 启动模式不一致时可保存另一种模式，用于冲突检测
 *
 * 之所以分开写，是因为这里不依赖非对齐原子访问语义。
 */
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
	adr_l	x1, __boot_cpu_mode
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f
	add	x1, x1, #4			// EL2 → 写入高32位
1:	str	w0, [x1]			// 保存启动模式
	ret
SYM_FUNC_END(set_cpu_boot_mode_flag)

/*
 * update_early_cpu_boot_status：更新从核早期启动状态并强制对外可见
 *
 * 宏参数：
 *   status: 要写入的状态值
 *   tmp1, tmp2: 临时寄存器
 *
 * 这里使用 DMB + DC IVAC，确保即便当前还在 MMU 关闭或缓存状态不稳定的阶段，
 * 主核或其他观察者之后也能可靠看到这个状态。
 */
	.macro	update_early_cpu_boot_status status, tmp1, tmp2
	mov	\tmp2, #\status
	adr_l	\tmp1, __early_cpu_boot_status
	str	\tmp2, [\tmp1]
	dmb	sy				// 确保 store 对所有观察者可见
	dc	ivac, \tmp1			// 无效化可能存在的过期缓存行
	.endm

/*
 * __enable_mmu：真正执行“打开 MMU”这件事
 *
 * 输入：
 *   x0 = 目标 SCTLR_EL1 值
 *   x1 = TTBR1_EL1 对应页表（通常是 swapper_pg_dir）
 *   x2 = TTBR0_EL1 对应页表（通常是 idmap 根页表）
 *
 * 前置要求：
 *   - 调用者必须位于 .idmap.text 中
 *   - 这样在 SCTLR_EL1.M 由 0 切 1 的瞬间，PC 依然能被恒等映射命中
 *
 * 该函数只做几件极关键的事：
 *   1. 检查当前 CPU 是否支持内核所选内存颗粒大小
 *   2. 装载 TTBR0/TTBR1
 *   3. 写 SCTLR_EL1，真正把 MMU 打开
 */
	.section ".idmap.text","a"
SYM_FUNC_START(__enable_mmu)
	/*
	 * ★ 内存颗粒大小支持检查
	 * 读取 ID_AA64MMFR0_EL1 的 TGRAN 字段，检查是否支持
	 * 内核配置的内存颗粒大小 (4KB/16KB/64KB)
	 */
	mrs	x3, ID_AA64MMFR0_EL1
	ubfx	x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
	b.lt    __no_granule_support		// 不支持 → 卡住 CPU
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
	b.gt    __no_granule_support		// 不支持 → 卡住 CPU

	/*
	 * 加载页表基址:
	 * TTBR0_EL1: ID map (恒等映射, 低虚拟地址)
	 * TTBR1_EL1: swapper_pg_dir (内核映射, 高虚拟地址)
	 *
	 * phys_to_ttbr 将物理地址转换为 TTBR 格式 (设置 CnP 等属性位)
	 */
	phys_to_ttbr x2, x2
	msr	ttbr0_el1, x2			// load TTBR0
	load_ttbr1 x1, x1, x3

	/*
	 * ★ 写 SCTLR_EL1：MMU 就在这一刻真正打开
	 *
	 * set_sctlr_el1 会顺带处理相关 errata 规避。
	 * 指令提交后：
	 *   - 地址翻译正式生效
	 *   - 低地址仍由 TTBR0 的 idmap 承接
	 *   - 高地址内核映射由 TTBR1 承接
	 *   - 当前 PC 不会丢，因为仍处在被恒等映射覆盖的 .idmap.text 中
	 */
	set_sctlr_el1	x0

	ret
SYM_FUNC_END(__enable_mmu)

#ifdef CONFIG_ARM64_VA_BITS_52
/*
 * __cpu_secondary_check52bitva: 检查从核是否支持 52 位 VA
 *
 * 52 位虚拟地址需要 LPA2 支持:
 * - 非 LPA2: 检查 ID_AA64MMFR2_EL1.VARange 是否非零
 * - LPA2:   检查是否支持 LPA2 颗粒大小 (TGRAN_LPA2)
 *
 * 如果不支持，将该 CPU 标记为 STUCK 并卡住。
 */
SYM_FUNC_START(__cpu_secondary_check52bitva)
#ifndef CONFIG_ARM64_LPA2
	mrs_s	x0, SYS_ID_AA64MMFR2_EL1
	and	x0, x0, ID_AA64MMFR2_EL1_VARange_MASK
	cbnz	x0, 2f				// 支持 → 正常返回
#else
	mrs	x0, id_aa64mmfr0_el1
	sbfx	x0, x0, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
	cmp	x0, #ID_AA64MMFR0_EL1_TGRAN_LPA2
	b.ge	2f				// 支持 → 正常返回
#endif

	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_52_BIT_VA, x0, x1
1:	wfe
	wfi
	b	1b

2:	ret
SYM_FUNC_END(__cpu_secondary_check52bitva)
#endif

/*
 * __no_granule_support: 内存颗粒大小不支持
 *
 * 当前 CPU 不支持内核配置的内存颗粒大小 (4KB/16KB/64KB)。
 * 标记 CPU 为 STUCK 并进入低功耗死循环。
 */
SYM_FUNC_START_LOCAL(__no_granule_support)
	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2
1:
	wfe
	wfi
	b	1b
SYM_FUNC_END(__no_granule_support)

/*
 * __primary_switch：主核从“物理地址世界”切进“内核虚拟地址世界”的桥
 *
 * 这个函数仍运行在 .idmap.text 中，因此它自己不会因 MMU 切换而失联。
 *
 * 主核路径与从核路径最大的不同在于：
 *   - 主核不仅要打开 MMU
 *   - 还要完成内核镜像、FDT、BSS、临时页表到最终页表的过渡
 *
 * 可以把它理解成：
 *   1. 先用最小可运行页表把 MMU 打开
 *   2. 再把正式内核地址空间真正搭好
 *   3. 最后跳到 __primary_switched，进入稳定虚拟地址阶段
 */
SYM_FUNC_START_LOCAL(__primary_switch)
	/*
	 * 先让 TTBR1 指向 reserved_pg_dir，而不是一上来就用最终页表。
	 * 这样做是为了在“MMU 刚打开但正式映射还没铺好”时，保持过渡阶段可控。
	 */
	adrp	x1, reserved_pg_dir
	adrp	x2, __pi_init_idmap_pg_dir
	bl	__enable_mmu			// ★ 开启 MMU

	/*
	 * MMU 已开启后，重新以虚拟地址方式设置 early_init_stack。
	 * 从这里开始，后续栈访问已经处于内核虚拟地址语义下。
	 */
	adrp	x1, early_init_stack
	mov	sp, x1
	mov	x29, xzr

	mov	x0, x20				// pass the full boot status
	mov	x1, x21				// pass the FDT (物理地址)
	/*
	 * ★ 真正把内核地址空间搭起来
	 * __pi_early_map_kernel 负责完成：
	 *   - FDT 映射
	 *   - BSS 清零
	 *   - KASLR 相关早期处理
	 *   - 内核各段映射
	 *   - 从临时 init_pg_dir 过渡到最终 swapper_pg_dir
	 *   - 必要时重建更适合最终运行态的映射形式
	 *
	 * 这一步做完后，系统才算真正拥有“稳定的内核虚拟地址空间”。
	 */
	bl	__pi_early_map_kernel		// Map and relocate the kernel

	/*
	 * ★ 跳转到 __primary_switched
	 * x0 = KERNEL_START 的物理地址 (__pa)
	 */
	ldr	x8, =__primary_switched
	adrp	x0, KERNEL_START		// __pa(KERNEL_START)
	br	x8
SYM_FUNC_END(__primary_switch)
