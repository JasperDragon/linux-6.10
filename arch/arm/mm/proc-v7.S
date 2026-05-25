/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/mm/proc-v7.S
 *
 *  Copyright (C) 2001 Deep Blue Solutions Ltd.
 *
 *  This is the "shell" of the ARMv7 processor support.
 */
// ============================================================================
// 文件概述: ARMv7 处理器初始化与设置汇编实现
// 该文件是 ARMv7 架构处理器支持的"核心外壳"，负责 CPU 的初始化、
// 缓存/TLB/MMU 的设置、勘误表(Errata)工作区修复以及处理器功能表定义。
// 这是内核 MM 子系统的关键组成部分。
// 主要内容:
//   1. 处理器生命周期管理: init/fin(初始化/关闭)、reset(软重启)、
//      do_idle(空闲)、do_suspend/do_resume(挂起/恢复)
//   2. MM 相关操作: switch_mm(地址空间切换)、set_pte_ext(页表设置)、
//      dcache_clean_area(D-cache 区域清理)
//   3. // ============================================================================
// __v7_setup: ARMv7 处理器核心初始化（主入口）
//   1. 刷新 L1 数据缓存
//   2. 读取 MIDR 识别处理器型号/版本
//   3. 根据主部件号跳转到对应 Errata 修复
//   4. 最终配置: I-cache/BTB/TLB 失效, 设置 TTBCR/TTBRx/MAIR
// 保留: r1,r2,r4,r5,r9,r13; r4/r5=TTBR0, r8=TTBR1, r9=MIDR
// ============================================================================
__v7_setup: 核心初始化函数, 配置 TLB、缓存、MMU 状态,
//      写入 MAIR0/MAIR1(PRRR/NMRR)内存属性寄存器
//   4. 各 ARM 内核的 Errata 修复:
//      - Cortex-A8/Cortex-A9/Cortex-A15/Cortex-A12/Cortex-A17
//      - Marvell PJ4B
//   5. __v7_proc 宏: 定义处理器信息结构(proc info list)
// 分支预测硬化:
//   - 提供 bpiall/iciallu 变体的 switch_mm 用于 Spectre 修复
//   - PSCI SMC/HVC 调用实现固件级别的分支预测管理
//   - 在 big.Little 系统中函数表保存在 .rodata 段
// 关键寄存器配置: SCTLR(系统控制)、ACTLR(辅助控制)、
// PRRR/NMRR(内存属性)、TTBCR/TTBRx(页表基址)
// ============================================================================
#include <linux/arm-smccc.h>
#include <linux/cfi_types.h>
#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/pgtable.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/hwcap.h>
#include <asm/pgtable-hwdef.h>
#include <asm/page.h>

#include "proc-macros.S"

#ifdef CONFIG_ARM_LPAE
#include "proc-v7-3level.S"
#else
#include "proc-v7-2level.S"
#endif

.arch armv7-a

SYM_TYPED_FUNC_START(cpu_v7_proc_init)
	ret	lr
SYM_FUNC_END(cpu_v7_proc_init)

// --------------------------------------------------------------------------
// cpu_v7_proc_fin: 处理器关闭/清理
// 关闭 MMU 和缓存（I-cache + D-cache）为软重启做准备。
// 清除 SCTLR 的 I 位（bit 12, I-cache）和 C/A 位（bit 2/1, D-cache/对齐）。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(cpu_v7_proc_fin)
	mrc	p15, 0, r0, c1, c0, 0		@ ctrl register
	bic	r0, r0, #0x1000			@ ...i............
	bic	r0, r0, #0x0006			@ .............ca.
	mcr	p15, 0, r0, c1, c0, 0		@ disable caches
	ret	lr
SYM_FUNC_END(cpu_v7_proc_fin)

/*
 *	cpu_v7_reset(loc, hyp)
 *
 *	Perform a soft reset of the system.  Put the CPU into the
 *	same state as it would be if it had been reset, and branch
 *	to what would be the reset vector.
 *
 *	- loc   - location to jump to for soft reset
 *	- hyp   - indicate if restart occurs in HYP mode
 *
 *	This code must be executed using a flat identity mapping with
 *      caches disabled.
 */
	.align	5
	.pushsection	.idmap.text, "ax"
SYM_TYPED_FUNC_START(cpu_v7_reset)
	mrc	p15, 0, r2, c1, c0, 0		@ ctrl register
	bic	r2, r2, #0x1			@ ...............m
 THUMB(	bic	r2, r2, #1 << 30 )		@ SCTLR.TE (Thumb exceptions)
	mcr	p15, 0, r2, c1, c0, 0		@ disable MMU
	isb
#ifdef CONFIG_ARM_VIRT_EXT
	teq	r1, #0
	bne	__hyp_soft_restart
#endif
	bx	r0
SYM_FUNC_END(cpu_v7_reset)
	.popsection

/*
 *	cpu_v7_do_idle()
 *
 *	Idle the processor (eg, wait for interrupt).
 *
 *	IRQs are already disabled.
 */
SYM_TYPED_FUNC_START(cpu_v7_do_idle)
	dsb					@ WFI may enter a low-power mode
	wfi
	ret	lr
SYM_FUNC_END(cpu_v7_do_idle)

// --------------------------------------------------------------------------
// cpu_v7_dcache_clean_area: 清理 D-cache 指定区域（用于页表修改后）
// SMP: MP 扩展意味着硬件页表遍历(PTW)能自动维护缓存一致性，无需操作。
// UP:  需要逐缓存行 clean D-cache，确保页表修改写回内存。
// 参数: r0=地址, r1=大小
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(cpu_v7_dcache_clean_area)
	ALT_SMP(W(nop))			@ MP extensions imply L1 PTW
	ALT_UP_B(1f)
	ret	lr
1:	dcache_line_size r2, r3
2:	mcr	p15, 0, r0, c7, c10, 1		@ clean D entry
	add	r0, r0, r2
	subs	r1, r1, r2
	bhi	2b
	dsb	ishst
	ret	lr
SYM_FUNC_END(cpu_v7_dcache_clean_area)

// ============================================================================
// 分支预测硬化（Spectre Variant 2 缓解措施）
// 通过 PSCI SMC/HVC 调用固件进行分支预测器维护，
// 在进程切换时刷新分支预测器（BTB/BPD）。
// cpu_v7_smc_switch_mm: 使用 SMC 调用（安全监视器）进行切换
// cpu_v7_hvc_switch_mm: 使用 HVC 调用（虚拟化扩展）进行切换
// cpu_v7_iciallu_switch_mm: 仅 ICIALLU（用于 Cortex-A15）
// cpu_v7_bpiall_switch_mm: 仅 BPIALL（用于 Cortex-A8/A9）
// ============================================================================
#if defined(CONFIG_ARM_PSCI) && defined(CONFIG_HARDEN_BRANCH_PREDICTOR)
	.arch_extension sec
SYM_TYPED_FUNC_START(cpu_v7_smc_switch_mm)
	stmfd	sp!, {r0 - r3}
	movw	r0, #:lower16:ARM_SMCCC_ARCH_WORKAROUND_1
	movt	r0, #:upper16:ARM_SMCCC_ARCH_WORKAROUND_1
	smc	#0
	ldmfd	sp!, {r0 - r3}
	b	cpu_v7_switch_mm
SYM_FUNC_END(cpu_v7_smc_switch_mm)
	.arch_extension virt
SYM_TYPED_FUNC_START(cpu_v7_hvc_switch_mm)
	stmfd	sp!, {r0 - r3}
	movw	r0, #:lower16:ARM_SMCCC_ARCH_WORKAROUND_1
	movt	r0, #:upper16:ARM_SMCCC_ARCH_WORKAROUND_1
	hvc	#0
	ldmfd	sp!, {r0 - r3}
	b	cpu_v7_switch_mm
SYM_FUNC_END(cpu_v7_hvc_switch_mm)
#endif

// --------------------------------------------------------------------------
// cpu_v7_iciallu_switch_mm: 通过 ICIALLU 刷新分支预测器后切换地址空间
// 用于 Cortex-A15 的分支预测硬化，先使整个 I-cache 失效，
// 再跳转到 cpu_v7_switch_mm 设置新页表。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(cpu_v7_iciallu_switch_mm)
	mov	r3, #0
	mcr	p15, 0, r3, c7, c5, 0		@ ICIALLU
	b	cpu_v7_switch_mm
SYM_FUNC_END(cpu_v7_iciallu_switch_mm)

// --------------------------------------------------------------------------
// cpu_v7_bpiall_switch_mm: 通过 BPIALL 刷新分支预测器后切换地址空间
// 用于 Cortex-A8/A9 的分支预测硬化，先使整个 BTAC/BTB 失效，
// 再跳转到 cpu_v7_switch_mm 设置新页表。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(cpu_v7_bpiall_switch_mm)
	mov	r3, #0
	mcr	p15, 0, r3, c7, c5, 6		@ flush BTAC/BTB
	b	cpu_v7_switch_mm
SYM_FUNC_END(cpu_v7_bpiall_switch_mm)

	string	cpu_v7_name, "ARMv7 Processor"
	.align

// ============================================================================
// CPU 挂起/恢复支持
// 派生自 arch/arm/mach-s5pv210/sleep.S
// 挂起时保存以下 CPU 关键状态到内存:
//   - FCSE/PID (r4, CP15 c13)
//   - 用户只读线程 ID (r5, CP15 c13)
//   - 域 ID (r6, CP15 c3)
//   - TTB 0/1 (r5/r7, CP15 c2) - 页表基址
//   - TTB 控制寄存器 (r11, CP15 c2)
//   - 系统控制寄存器 (r8, CP15 c1)
//   - 辅助控制寄存器 (r9, CP15 c1)
//   - 协处理器访问控制 (r10, CP15 c1)
// 恢复时逆序恢复, 最后跳转到 cpu_resume_mmu 重新使能 MMU。
// Cortex-A9 MP 额外保存诊断和电源管理寄存器。
// Marvell PJ4B 额外保存功能模式/调试模式控制寄存器。
// ============================================================================
/* Suspend/resume support: derived from arch/arm/mach-s5pv210/sleep.S */
.globl	cpu_v7_suspend_size
.equ	cpu_v7_suspend_size, 4 * 9
#ifdef CONFIG_ARM_CPU_SUSPEND
SYM_TYPED_FUNC_START(cpu_v7_do_suspend)
	stmfd	sp!, {r4 - r11, lr}
	mrc	p15, 0, r4, c13, c0, 0	@ FCSE/PID
	mrc	p15, 0, r5, c13, c0, 3	@ User r/o thread ID
	stmia	r0!, {r4 - r5}
#ifdef CONFIG_MMU
	mrc	p15, 0, r6, c3, c0, 0	@ Domain ID
#ifdef CONFIG_ARM_LPAE
	mrrc	p15, 1, r5, r7, c2	@ TTB 1
#else
	mrc	p15, 0, r7, c2, c0, 1	@ TTB 1
#endif
	mrc	p15, 0, r11, c2, c0, 2	@ TTB control register
#endif
	mrc	p15, 0, r8, c1, c0, 0	@ Control register
	mrc	p15, 0, r9, c1, c0, 1	@ Auxiliary control register
	mrc	p15, 0, r10, c1, c0, 2	@ Co-processor access control
	stmia	r0, {r5 - r11}
	ldmfd	sp!, {r4 - r11, pc}
SYM_FUNC_END(cpu_v7_do_suspend)

SYM_TYPED_FUNC_START(cpu_v7_do_resume)
	mov	ip, #0
	mcr	p15, 0, ip, c7, c5, 0	@ invalidate I cache
	mcr	p15, 0, ip, c13, c0, 1	@ set reserved context ID
	ldmia	r0!, {r4 - r5}
	mcr	p15, 0, r4, c13, c0, 0	@ FCSE/PID
	mcr	p15, 0, r5, c13, c0, 3	@ User r/o thread ID
	ldmia	r0, {r5 - r11}
#ifdef CONFIG_MMU
	mcr	p15, 0, ip, c8, c7, 0	@ invalidate TLBs
	mcr	p15, 0, r6, c3, c0, 0	@ Domain ID
#ifdef CONFIG_ARM_LPAE
	mcrr	p15, 0, r1, ip, c2	@ TTB 0
	mcrr	p15, 1, r5, r7, c2	@ TTB 1
#else
	ALT_SMP(orr	r1, r1, #TTB_FLAGS_SMP)
	ALT_UP(orr	r1, r1, #TTB_FLAGS_UP)
	mcr	p15, 0, r1, c2, c0, 0	@ TTB 0
	mcr	p15, 0, r7, c2, c0, 1	@ TTB 1
#endif
	mcr	p15, 0, r11, c2, c0, 2	@ TTB control register
	ldr	r4, =PRRR		@ PRRR
	ldr	r5, =NMRR		@ NMRR
	mcr	p15, 0, r4, c10, c2, 0	@ write PRRR
	mcr	p15, 0, r5, c10, c2, 1	@ write NMRR
#endif	/* CONFIG_MMU */
	mrc	p15, 0, r4, c1, c0, 1	@ Read Auxiliary control register
	teq	r4, r9			@ Is it already set?
	mcrne	p15, 0, r9, c1, c0, 1	@ No, so write it
	mcr	p15, 0, r10, c1, c0, 2	@ Co-processor access control
	isb
	dsb
	mov	r0, r8			@ control register
	b	cpu_resume_mmu
SYM_FUNC_END(cpu_v7_do_resume)
#endif

.globl	cpu_ca9mp_suspend_size
.equ	cpu_ca9mp_suspend_size, cpu_v7_suspend_size + 4 * 2
#ifdef CONFIG_ARM_CPU_SUSPEND
SYM_TYPED_FUNC_START(cpu_ca9mp_do_suspend)
	stmfd	sp!, {r4 - r5}
	mrc	p15, 0, r4, c15, c0, 1		@ Diagnostic register
	mrc	p15, 0, r5, c15, c0, 0		@ Power register
	stmia	r0!, {r4 - r5}
	ldmfd	sp!, {r4 - r5}
	b	cpu_v7_do_suspend
SYM_FUNC_END(cpu_ca9mp_do_suspend)

SYM_TYPED_FUNC_START(cpu_ca9mp_do_resume)
	ldmia	r0!, {r4 - r5}
	mrc	p15, 0, r10, c15, c0, 1		@ Read Diagnostic register
	teq	r4, r10				@ Already restored?
	mcrne	p15, 0, r4, c15, c0, 1		@ No, so restore it
	mrc	p15, 0, r10, c15, c0, 0		@ Read Power register
	teq	r5, r10				@ Already restored?
	mcrne	p15, 0, r5, c15, c0, 0		@ No, so restore it
	b	cpu_v7_do_resume
SYM_FUNC_END(cpu_ca9mp_do_resume)
#endif

#ifdef CONFIG_CPU_PJ4B
	globl_equ	cpu_pj4b_switch_mm,     cpu_v7_switch_mm
	globl_equ	cpu_pj4b_set_pte_ext,	cpu_v7_set_pte_ext
	globl_equ	cpu_pj4b_proc_init,	cpu_v7_proc_init
	globl_equ	cpu_pj4b_proc_fin, 	cpu_v7_proc_fin
	globl_equ	cpu_pj4b_reset,	   	cpu_v7_reset
#ifdef CONFIG_PJ4B_ERRATA_4742
SYM_TYPED_FUNC_START(cpu_pj4b_do_idle)
	dsb					@ WFI may enter a low-power mode
	wfi
	dsb					@barrier
	ret	lr
SYM_FUNC_END(cpu_pj4b_do_idle)
#else
	globl_equ	cpu_pj4b_do_idle,  	cpu_v7_do_idle
#endif
	globl_equ	cpu_pj4b_dcache_clean_area,	cpu_v7_dcache_clean_area
#ifdef CONFIG_ARM_CPU_SUSPEND
SYM_TYPED_FUNC_START(cpu_pj4b_do_suspend)
	stmfd	sp!, {r6 - r10}
	mrc	p15, 1, r6, c15, c1, 0  @ save CP15 - extra features
	mrc	p15, 1, r7, c15, c2, 0	@ save CP15 - Aux Func Modes Ctrl 0
	mrc	p15, 1, r8, c15, c1, 2	@ save CP15 - Aux Debug Modes Ctrl 2
	mrc	p15, 1, r9, c15, c1, 1  @ save CP15 - Aux Debug Modes Ctrl 1
	mrc	p15, 0, r10, c9, c14, 0  @ save CP15 - PMC
	stmia	r0!, {r6 - r10}
	ldmfd	sp!, {r6 - r10}
	b cpu_v7_do_suspend
SYM_FUNC_END(cpu_pj4b_do_suspend)

SYM_TYPED_FUNC_START(cpu_pj4b_do_resume)
	ldmia	r0!, {r6 - r10}
	mcr	p15, 1, r6, c15, c1, 0  @ restore CP15 - extra features
	mcr	p15, 1, r7, c15, c2, 0	@ restore CP15 - Aux Func Modes Ctrl 0
	mcr	p15, 1, r8, c15, c1, 2	@ restore CP15 - Aux Debug Modes Ctrl 2
	mcr	p15, 1, r9, c15, c1, 1  @ restore CP15 - Aux Debug Modes Ctrl 1
	mcr	p15, 0, r10, c9, c14, 0  @ restore CP15 - PMC
	b cpu_v7_do_resume
SYM_FUNC_END(cpu_pj4b_do_resume)
#endif
.globl	cpu_pj4b_suspend_size
.equ	cpu_pj4b_suspend_size, cpu_v7_suspend_size + 4 * 5

#endif

	@
	@ Invoke the v7_invalidate_l1() function, which adheres to the AAPCS
	@ rules, and so it may corrupt registers that we need to preserve.
	@
	// do_invalidate_l1: 安全的 L1 缓存失效宏
	// 保存 r1/r2/lr 后调用 v7_invalidate_l1（会破坏 r0-r3/ip/lr），
	// 调用后恢复原始值，避免影响上层调用者。
	.macro	do_invalidate_l1
	mov	r6, r1
	mov	r7, r2
	mov	r10, lr
	bl	v7_invalidate_l1		@ corrupts {r0-r3, ip, lr}
	mov	r1, r6
	mov	r2, r7
	mov	lr, r10
	.endm

/*
 *	__v7_setup
 *
 *	Initialise TLB, Caches, and MMU state ready to switch the MMU
 *	on.  Return in r0 the new CP15 C1 control register setting.
 *
 *	r1, r2, r4, r5, r9, r13 must be preserved - r13 is not a stack
 *	r4: TTBR0 (low word)
 *	r5: TTBR0 (high word if LPAE)
 *	r8: TTBR1
 *	r9: Main ID register
 *
 *	This should be able to cover all ARMv7 cores.
 *
 *	It is assumed that:
 *	- cache type register is implemented
 */
// ============================================================================
// __v7_ca5mp/ca9mp/cr7mp/cr8mp_setup: Cortex-A5/A9/R7/R8 初始化入口
// 这些核心需要使 Cache/TLB 操作广播到所有核心(ACTLR[0]=1)
// 然后使能 SMP 模式(ACTLR[6]=1)
// ----------------------------------------------------------------------------
// __v7_ca7mp/ca12mp/ca15mp/b15mp/ca17mp_setup: 新一代 Cortex 初始化入口
// 这些核心不依赖软件广播(硬件维护一致性)
// 只需刷新 L1 缓存并使能 SMP 模式
// ============================================================================
__v7_ca5mp_setup:
__v7_ca9mp_setup:
__v7_cr7mp_setup:
__v7_cr8mp_setup:
	do_invalidate_l1
	mov	r10, #(1 << 0)			@ Cache/TLB ops broadcasting
	b	1f
__v7_ca7mp_setup:
__v7_ca12mp_setup:
__v7_ca15mp_setup:
__v7_b15mp_setup:
__v7_ca17mp_setup:
	do_invalidate_l1
	mov	r10, #0
1:
#ifdef CONFIG_SMP
	orr	r10, r10, #(1 << 6)		@ Enable SMP/nAMP mode
	ALT_SMP(mrc	p15, 0, r0, c1, c0, 1)
	ALT_UP(mov	r0, r10)		@ fake it for UP
	orr	r10, r10, r0			@ Set required bits
	teq	r10, r0				@ Were they already set?
	mcrne	p15, 0, r10, c1, c0, 1		@ No, update register
#endif
	b	__v7_setup_cont

/*
 * Errata:
 *  r0, r10 available for use
 *  r1, r2, r4, r5, r9, r13: must be preserved
 *  r3: contains MIDR rX number in bits 23-20
 *  r6: contains MIDR rXpY as 8-bit XY number
 *  r9: MIDR
 */
// ============================================================================
// Cortex-A8 勘误表工作区
//   Errata 430973: 在 r1p* 设置 IBE(Imprecise Bus Error)位
//   Errata 458693: 在 r2p0 设置 L1NEON 和 PLDNOP 位
//   Errata 460075: 在 r2p0 禁止 L2 缓存的 Write Allocate
// ============================================================================
__ca8_errata:
#if defined(CONFIG_ARM_ERRATA_430973) && !defined(CONFIG_ARCH_MULTIPLATFORM)
	teq	r3, #0x00100000			@ only present in r1p*
	mrceq	p15, 0, r0, c1, c0, 1		@ read aux control register
	orreq	r0, r0, #(1 << 6)		@ set IBE to 1
	mcreq	p15, 0, r0, c1, c0, 1		@ write aux control register
#endif
#ifdef CONFIG_ARM_ERRATA_458693
	teq	r6, #0x20			@ only present in r2p0
	mrceq	p15, 0, r0, c1, c0, 1		@ read aux control register
	orreq	r0, r0, #(1 << 5)		@ set L1NEON to 1
	orreq	r0, r0, #(1 << 9)		@ set PLDNOP to 1
	mcreq	p15, 0, r0, c1, c0, 1		@ write aux control register
#endif
#ifdef CONFIG_ARM_ERRATA_460075
	teq	r6, #0x20			@ only present in r2p0
	mrceq	p15, 1, r0, c9, c0, 2		@ read L2 cache aux ctrl register
	tsteq	r0, #1 << 22
	orreq	r0, r0, #(1 << 22)		@ set the Write Allocate disable bit
	mcreq	p15, 1, r0, c9, c0, 2		@ write the L2 cache aux ctrl register
#endif
	b	__errata_finish

// ============================================================================
// Cortex-A9 勘误表工作区
//   Errata 742230: r2p2 之前, 设置诊断寄存器 bit 4（L2 缓存问题）
//   Errata 742231: r2p0-r2p2, 设置诊断寄存器 bit 12/22（L2 缓存问题）
//   Errata 743622: r2p*, 设置诊断寄存器 bit 6（L2 缓存问题）
//   Errata 751472: r3p0 之前（SMP）, 设置诊断寄存器 bit 11（延时加载问题）
// ============================================================================
__ca9_errata:
#ifdef CONFIG_ARM_ERRATA_742230
	cmp	r6, #0x22			@ only present up to r2p2
	mrcle	p15, 0, r0, c15, c0, 1		@ read diagnostic register
	orrle	r0, r0, #1 << 4			@ set bit #4
	mcrle	p15, 0, r0, c15, c0, 1		@ write diagnostic register
#endif
#ifdef CONFIG_ARM_ERRATA_742231
	teq	r6, #0x20			@ present in r2p0
	teqne	r6, #0x21			@ present in r2p1
	teqne	r6, #0x22			@ present in r2p2
	mrceq	p15, 0, r0, c15, c0, 1		@ read diagnostic register
	orreq	r0, r0, #1 << 12		@ set bit #12
	orreq	r0, r0, #1 << 22		@ set bit #22
	mcreq	p15, 0, r0, c15, c0, 1		@ write diagnostic register
#endif
#ifdef CONFIG_ARM_ERRATA_743622
	teq	r3, #0x00200000			@ only present in r2p*
	mrceq	p15, 0, r0, c15, c0, 1		@ read diagnostic register
	orreq	r0, r0, #1 << 6			@ set bit #6
	mcreq	p15, 0, r0, c15, c0, 1		@ write diagnostic register
#endif
#if defined(CONFIG_ARM_ERRATA_751472) && defined(CONFIG_SMP)
	ALT_SMP(cmp r6, #0x30)			@ present prior to r3p0
	ALT_UP_B(1f)
	mrclt	p15, 0, r0, c15, c0, 1		@ read diagnostic register
	orrlt	r0, r0, #1 << 11		@ set bit #11
	mcrlt	p15, 0, r0, c15, c0, 1		@ write diagnostic register
1:
#endif
	b	__errata_finish

// ============================================================================
// Cortex-A15 勘误表工作区
//   Errata 773022: r0p4 之前(UP), 在 ACTLR 中禁用循环缓冲器(bit 1)
// ============================================================================
__ca15_errata:
#ifdef CONFIG_ARM_ERRATA_773022
	cmp	r6, #0x4			@ only present up to r0p4
	mrcle	p15, 0, r0, c1, c0, 1		@ read aux control register
	orrle	r0, r0, #1 << 1			@ disable loop buffer
	mcrle	p15, 0, r0, c1, c0, 1		@ write aux control register
#endif
	b	__errata_finish

// ============================================================================
// Cortex-A12 勘误表工作区
//   Errata 818325/852422: 设置诊断寄存器 bit 12（处理器上电/复位问题）
//   Errata 821420: 设置内部特性寄存器 bit 1（数据一致性问题）
//   Errata 825619: 设置诊断寄存器 bit 24（AWCACHE 问题）
//   Errata 857271: 设置诊断寄存器 bit 10/11（TLB 问题）
// ============================================================================
__ca12_errata:
#ifdef CONFIG_ARM_ERRATA_818325_852422
	mrc	p15, 0, r10, c15, c0, 1		@ read diagnostic register
	orr	r10, r10, #1 << 12		@ set bit #12
	mcr	p15, 0, r10, c15, c0, 1		@ write diagnostic register
#endif
#ifdef CONFIG_ARM_ERRATA_821420
	mrc	p15, 0, r10, c15, c0, 2		@ read internal feature reg
	orr	r10, r10, #1 << 1		@ set bit #1
	mcr	p15, 0, r10, c15, c0, 2		@ write internal feature reg
#endif
#ifdef CONFIG_ARM_ERRATA_825619
	mrc	p15, 0, r10, c15, c0, 1		@ read diagnostic register
	orr	r10, r10, #1 << 24		@ set bit #24
	mcr	p15, 0, r10, c15, c0, 1		@ write diagnostic register
#endif
#ifdef CONFIG_ARM_ERRATA_857271
	mrc	p15, 0, r10, c15, c0, 1		@ read diagnostic register
	orr	r10, r10, #3 << 10		@ set bits #10 and #11
	mcr	p15, 0, r10, c15, c0, 1		@ write diagnostic register
#endif
	b	__errata_finish

// ============================================================================
// Cortex-A17 勘误表工作区
//   Errata 852421: r1p2 之前, 设置诊断寄存器 bit 24（上电复位问题）
//   Errata 852423: r1p2 之前, 设置诊断寄存器 bit 12（AWCACHE 问题）
//   Errata 857272: 设置诊断寄存器 bit 10/11（TLB 问题，与 A12 相同）
// ============================================================================
__ca17_errata:
#ifdef CONFIG_ARM_ERRATA_852421
	cmp	r6, #0x12			@ only present up to r1p2
	mrcle	p15, 0, r10, c15, c0, 1		@ read diagnostic register
	orrle	r10, r10, #1 << 24		@ set bit #24
	mcrle	p15, 0, r10, c15, c0, 1		@ write diagnostic register
#endif
#ifdef CONFIG_ARM_ERRATA_852423
	cmp	r6, #0x12			@ only present up to r1p2
	mrcle	p15, 0, r10, c15, c0, 1		@ read diagnostic register
	orrle	r10, r10, #1 << 12		@ set bit #12
	mcrle	p15, 0, r10, c15, c0, 1		@ write diagnostic register
#endif
#ifdef CONFIG_ARM_ERRATA_857272
	mrc	p15, 0, r10, c15, c0, 1		@ read diagnostic register
	orr	r10, r10, #3 << 10		@ set bits #10 and #11
	mcr	p15, 0, r10, c15, c0, 1		@ write diagnostic register
#endif
	b	__errata_finish

// ============================================================================
// Marvell PJ4B 处理器初始化
// PJ4B 是 Marvell 的 ARMv7 兼容处理器核心, 需要配置多个辅助调试/功能寄存器:
//   1. 禁用静态分支预测、启用内部奇偶校验、禁止 Clean Line 数据传输
//   2. 禁用快速 LDR、不交错写和监听数据、禁止 Critical Word First
//   3. 设置 SMP 模式(加入一致性域)、L1 奇偶校验、广播缓存/TLB 维护
//   4. 配置 WFI/WFE 响应 DVM 请求
// ============================================================================
__v7_pj4b_setup:
#ifdef CONFIG_CPU_PJ4B

/* Auxiliary Debug Modes Control 1 Register */
#define PJ4B_STATIC_BP (1 << 2) /* Enable Static BP */
#define PJ4B_INTER_PARITY (1 << 8) /* Disable Internal Parity Handling */
#define PJ4B_CLEAN_LINE (1 << 16) /* Disable data transfer for clean line */

/* Auxiliary Debug Modes Control 2 Register */
#define PJ4B_FAST_LDR (1 << 23) /* Disable fast LDR */
#define PJ4B_SNOOP_DATA (1 << 25) /* Do not interleave write and snoop data */
#define PJ4B_CWF (1 << 27) /* Disable Critical Word First feature */
#define PJ4B_OUTSDNG_NC (1 << 29) /* Disable outstanding non cacheable rqst */
#define PJ4B_L1_REP_RR (1 << 30) /* L1 replacement - Strict round robin */
#define PJ4B_AUX_DBG_CTRL2 (PJ4B_SNOOP_DATA | PJ4B_CWF |\
			    PJ4B_OUTSDNG_NC | PJ4B_L1_REP_RR)

/* Auxiliary Functional Modes Control Register 0 */
#define PJ4B_SMP_CFB (1 << 1) /* Set SMP mode. Join the coherency fabric */
#define PJ4B_L1_PAR_CHK (1 << 2) /* Support L1 parity checking */
#define PJ4B_BROADCAST_CACHE (1 << 8) /* Broadcast Cache and TLB maintenance */

/* Auxiliary Debug Modes Control 0 Register */
#define PJ4B_WFI_WFE (1 << 22) /* WFI/WFE - serve the DVM and back to idle */

	/* Auxiliary Debug Modes Control 1 Register */
	mrc	p15, 1,	r0, c15, c1, 1
	orr     r0, r0, #PJ4B_CLEAN_LINE
	orr     r0, r0, #PJ4B_INTER_PARITY
	bic	r0, r0, #PJ4B_STATIC_BP
	mcr	p15, 1,	r0, c15, c1, 1

	/* Auxiliary Debug Modes Control 2 Register */
	mrc	p15, 1,	r0, c15, c1, 2
	bic	r0, r0, #PJ4B_FAST_LDR
	orr	r0, r0, #PJ4B_AUX_DBG_CTRL2
	mcr	p15, 1,	r0, c15, c1, 2

	/* Auxiliary Functional Modes Control Register 0 */
	mrc	p15, 1,	r0, c15, c2, 0
#ifdef CONFIG_SMP
	orr	r0, r0, #PJ4B_SMP_CFB
#endif
	orr	r0, r0, #PJ4B_L1_PAR_CHK
	orr	r0, r0, #PJ4B_BROADCAST_CACHE
	mcr	p15, 1,	r0, c15, c2, 0

	/* Auxiliary Debug Modes Control 0 Register */
	mrc	p15, 1,	r0, c15, c1, 0
	orr	r0, r0, #PJ4B_WFI_WFE
	mcr	p15, 1,	r0, c15, c1, 0

#endif /* CONFIG_CPU_PJ4B */

// ============================================================================
// __v7_setup: ARMv7 处理器核心初始化（主入口）
//   1. 刷新 L1 数据缓存
//   2. 读取 MIDR 识别处理器型号/版本
//   3. 根据主部件号跳转到对应 Errata 修复
//   4. 最终配置: I-cache/BTB/TLB 失效, 设置 TTBCR/TTBRx/MAIR
// 保留: r1,r2,r4,r5,r9,r13; r4/r5=TTBR0, r8=TTBR1, r9=MIDR
// ============================================================================
__v7_setup:
	do_invalidate_l1

__v7_setup_cont:
	and	r0, r9, #0xff000000		@ ARM?
	teq	r0, #0x41000000
	bne	__errata_finish
	and	r3, r9, #0x00f00000		@ variant
	and	r6, r9, #0x0000000f		@ revision
	orr	r6, r6, r3, lsr #20-4		@ combine variant and revision
	ubfx	r0, r9, #4, #12			@ primary part number

	/* Cortex-A8 Errata */
	ldr	r10, =0x00000c08		@ Cortex-A8 primary part number
	teq	r0, r10
	beq	__ca8_errata

	/* Cortex-A9 Errata */
	ldr	r10, =0x00000c09		@ Cortex-A9 primary part number
	teq	r0, r10
	beq	__ca9_errata

	/* Cortex-A12 Errata */
	ldr	r10, =0x00000c0d		@ Cortex-A12 primary part number
	teq	r0, r10
	beq	__ca12_errata

	/* Cortex-A17 Errata */
	ldr	r10, =0x00000c0e		@ Cortex-A17 primary part number
	teq	r0, r10
	beq	__ca17_errata

	/* Cortex-A15 Errata */
	ldr	r10, =0x00000c0f		@ Cortex-A15 primary part number
	teq	r0, r10
	beq	__ca15_errata

// ============================================================================
// __errata_finish: Errata 修复完成后的统一处理
//   1. 使 I-cache + BTB 失效
//   2. 使 I + D TLBs 失效（MMU 开启时）
//   3. 设置 TTBCR + TTBRx（v7_ttb_setup 宏）
//   4. 写入 MAIR0/MAIR1（PRRR/NMRR）
//   5. DSB 等待所有维护操作完成
//   6. 配置 ThumbEE（如果硬件支持且未禁用）
//   7. 读取 v7_crval 确定 SCTLR 的控制位掩码，返回给调用者
// ============================================================================
__errata_finish:
	mov	r10, #0
	mcr	p15, 0, r10, c7, c5, 0		@ I+BTB cache invalidate
#ifdef CONFIG_MMU
	mcr	p15, 0, r10, c8, c7, 0		@ invalidate I + D TLBs
	v7_ttb_setup r10, r4, r5, r8, r3	@ TTBCR, TTBRx setup
	ldr	r3, =PRRR			@ PRRR
	ldr	r6, =NMRR			@ NMRR
	mcr	p15, 0, r3, c10, c2, 0		@ write PRRR
	mcr	p15, 0, r6, c10, c2, 1		@ write NMRR
#endif
	dsb					@ Complete invalidations
#ifndef CONFIG_ARM_THUMBEE
	mrc	p15, 0, r0, c0, c1, 0		@ read ID_PFR0 for ThumbEE
	and	r0, r0, #(0xf << 12)		@ ThumbEE enabled field
	teq	r0, #(1 << 12)			@ check if ThumbEE is present
	bne	1f
	mov	r3, #0
	mcr	p14, 6, r3, c1, c0, 0		@ Initialize TEEHBR to 0
	mrc	p14, 6, r0, c0, c0, 0		@ load TEECR
	orr	r0, r0, #1			@ set the 1st bit in order to
	mcr	p14, 6, r0, c0, c0, 0		@ stop userspace TEEHBR access
1:
#endif
	adr	r3, v7_crval
	ldmia	r3, {r3, r6}
 ARM_BE8(orr	r6, r6, #1 << 25)		@ big-endian page tables
#ifdef CONFIG_SWP_EMULATE
	orr     r3, r3, #(1 << 10)              @ set SW bit in "clear"
	bic     r6, r6, #(1 << 10)              @ clear it in "mmuset"
#endif
   	mrc	p15, 0, r0, c1, c0, 0		@ read control register
	bic	r0, r0, r3			@ clear bits them
	orr	r0, r0, r6			@ set them
 THUMB(	orr	r0, r0, #1 << 30	)	@ Thumb exceptions
	ret	lr				@ return to head.S:__ret
ENDPROC(__v7_setup)

	__INITDATA

	.weak cpu_v7_bugs_init

	@ define struct processor (see <asm/proc-fns.h> and proc-macros.S)
	define_processor_functions v7, dabort=v7_early_abort, pabort=v7_pabort, suspend=1, bugs=cpu_v7_bugs_init

#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
	@ generic v7 bpiall on context switch
	globl_equ	cpu_v7_bpiall_proc_init,	cpu_v7_proc_init
	globl_equ	cpu_v7_bpiall_proc_fin,		cpu_v7_proc_fin
	globl_equ	cpu_v7_bpiall_reset,		cpu_v7_reset
	globl_equ	cpu_v7_bpiall_do_idle,		cpu_v7_do_idle
	globl_equ	cpu_v7_bpiall_dcache_clean_area, cpu_v7_dcache_clean_area
	globl_equ	cpu_v7_bpiall_set_pte_ext,	cpu_v7_set_pte_ext
	globl_equ	cpu_v7_bpiall_suspend_size,	cpu_v7_suspend_size
#ifdef CONFIG_ARM_CPU_SUSPEND
	globl_equ	cpu_v7_bpiall_do_suspend,	cpu_v7_do_suspend
	globl_equ	cpu_v7_bpiall_do_resume,	cpu_v7_do_resume
#endif
	define_processor_functions v7_bpiall, dabort=v7_early_abort, pabort=v7_pabort, suspend=1, bugs=cpu_v7_bugs_init

#define HARDENED_BPIALL_PROCESSOR_FUNCTIONS v7_bpiall_processor_functions
#else
#define HARDENED_BPIALL_PROCESSOR_FUNCTIONS v7_processor_functions
#endif

#ifndef CONFIG_ARM_LPAE
	@ Cortex-A8 - always needs bpiall switch_mm implementation
	globl_equ	cpu_ca8_proc_init,	cpu_v7_proc_init
	globl_equ	cpu_ca8_proc_fin,	cpu_v7_proc_fin
	globl_equ	cpu_ca8_reset,		cpu_v7_reset
	globl_equ	cpu_ca8_do_idle,	cpu_v7_do_idle
	globl_equ	cpu_ca8_dcache_clean_area, cpu_v7_dcache_clean_area
	globl_equ	cpu_ca8_set_pte_ext,	cpu_v7_set_pte_ext
	globl_equ	cpu_ca8_switch_mm,	cpu_v7_bpiall_switch_mm
	globl_equ	cpu_ca8_suspend_size,	cpu_v7_suspend_size
#ifdef CONFIG_ARM_CPU_SUSPEND
	globl_equ	cpu_ca8_do_suspend,	cpu_v7_do_suspend
	globl_equ	cpu_ca8_do_resume,	cpu_v7_do_resume
#endif
	define_processor_functions ca8, dabort=v7_early_abort, pabort=v7_pabort, suspend=1, bugs=cpu_v7_ca8_ibe

	@ Cortex-A9 - needs more registers preserved across suspend/resume
	@ and bpiall switch_mm for hardening
	globl_equ	cpu_ca9mp_proc_init,	cpu_v7_proc_init
	globl_equ	cpu_ca9mp_proc_fin,	cpu_v7_proc_fin
	globl_equ	cpu_ca9mp_reset,	cpu_v7_reset
	globl_equ	cpu_ca9mp_do_idle,	cpu_v7_do_idle
	globl_equ	cpu_ca9mp_dcache_clean_area, cpu_v7_dcache_clean_area
#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
	globl_equ	cpu_ca9mp_switch_mm,	cpu_v7_bpiall_switch_mm
#else
	globl_equ	cpu_ca9mp_switch_mm,	cpu_v7_switch_mm
#endif
	globl_equ	cpu_ca9mp_set_pte_ext,	cpu_v7_set_pte_ext
	define_processor_functions ca9mp, dabort=v7_early_abort, pabort=v7_pabort, suspend=1, bugs=cpu_v7_bugs_init
#endif

	@ Cortex-A15 - needs iciallu switch_mm for hardening
	globl_equ	cpu_ca15_proc_init,	cpu_v7_proc_init
	globl_equ	cpu_ca15_proc_fin,	cpu_v7_proc_fin
	globl_equ	cpu_ca15_reset,		cpu_v7_reset
	globl_equ	cpu_ca15_do_idle,	cpu_v7_do_idle
	globl_equ	cpu_ca15_dcache_clean_area, cpu_v7_dcache_clean_area
#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
	globl_equ	cpu_ca15_switch_mm,	cpu_v7_iciallu_switch_mm
#else
	globl_equ	cpu_ca15_switch_mm,	cpu_v7_switch_mm
#endif
	globl_equ	cpu_ca15_set_pte_ext,	cpu_v7_set_pte_ext
	globl_equ	cpu_ca15_suspend_size,	cpu_v7_suspend_size
	globl_equ	cpu_ca15_do_suspend,	cpu_v7_do_suspend
	globl_equ	cpu_ca15_do_resume,	cpu_v7_do_resume
	define_processor_functions ca15, dabort=v7_early_abort, pabort=v7_pabort, suspend=1, bugs=cpu_v7_ca15_ibe
#ifdef CONFIG_CPU_PJ4B
	define_processor_functions pj4b, dabort=v7_early_abort, pabort=v7_pabort, suspend=1
#endif

	.section ".rodata"

	string	cpu_arch_name, "armv7"
	string	cpu_elf_name, "v7"
	.align

	.section ".proc.info.init", "a"

	/*
	 * Standard v7 proc info content
	 */
// ============================================================================
// __v7_proc 宏: 定义 ARMv7 处理器信息结构体
// 为每个支持的 ARMv7 处理器生成 proc_info_list 条目, 内核启动时由链接脚本
// 遍历 .proc.info.init 段来匹配当前 CPU 并调用对应的初始化函数。
// 该结构体包含 MMU 页表属性、处理器能力标志、以及各种函数表指针。
// 各参数:
//   name: 结构体名称
//   initfunc: CPU 初始化入口(__v7_setup 或特定核心版本)
//   mm_mmuflags: 内存区域页表附加标志
//   io_mmuflags: I/O 区域页表附加标志
//   hwcaps: 硬件能力(HWCAP_*)标志位
//   proc_fns: 处理器函数表
//   cache_fns: 缓存函数表(默认为 v7_cache_fns, B15 使用 b15_cache_fns)
// ============================================================================
.macro __v7_proc name, initfunc, mm_mmuflags = 0, io_mmuflags = 0, hwcaps = 0, proc_fns = v7_processor_functions, cache_fns = v7_cache_fns
	ALT_SMP(.long	PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | \
			PMD_SECT_AF | PMD_FLAGS_SMP | \mm_mmuflags)
	ALT_UP(.long	PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_AP_READ | \
			PMD_SECT_AF | PMD_FLAGS_UP | \mm_mmuflags)
	.long	PMD_TYPE_SECT | PMD_SECT_AP_WRITE | \
		PMD_SECT_AP_READ | PMD_SECT_AF | \io_mmuflags
	initfn	\initfunc, \name
	.long	cpu_arch_name
	.long	cpu_elf_name
	.long	HWCAP_SWP | HWCAP_HALF | HWCAP_THUMB | HWCAP_FAST_MULT | \
		HWCAP_EDSP | HWCAP_TLS | \hwcaps
	.long	cpu_v7_name
	.long	\proc_fns
	.long	v7wbi_tlb_fns
	.long	v6_user_fns
	.long	\cache_fns
.endm

#ifndef CONFIG_ARM_LPAE
	/*
	 * ARM Ltd. Cortex A5 processor.
	 */
	.type   __v7_ca5mp_proc_info, #object
// ============================================================================
// 处理器信息列表: 每个条目定义处理器 ID 匹配掩码和对应的初始化函数。
// 匹配规则: CPU 的 MIDR 值 & mask == required_id。
// 内核启动时从 __proc_info_begin 到 __proc_info_end 遍历匹配。
// 涵盖的处理器: A5, A9, A8, PJ4B, R7, R8, A7, A12, A15, B15, A17, A73, A75, Krait
// ============================================================================
__v7_ca5mp_proc_info:
	.long	0x410fc050
	.long	0xff0ffff0
	__v7_proc __v7_ca5mp_proc_info, __v7_ca5mp_setup
	.size	__v7_ca5mp_proc_info, . - __v7_ca5mp_proc_info

	/*
	 * ARM Ltd. Cortex A9 processor.
	 */
	.type   __v7_ca9mp_proc_info, #object
__v7_ca9mp_proc_info:
	.long	0x410fc090
	.long	0xff0ffff0
	__v7_proc __v7_ca9mp_proc_info, __v7_ca9mp_setup, proc_fns = ca9mp_processor_functions
	.size	__v7_ca9mp_proc_info, . - __v7_ca9mp_proc_info

	/*
	 * ARM Ltd. Cortex A8 processor.
	 */
	.type	__v7_ca8_proc_info, #object
__v7_ca8_proc_info:
	.long	0x410fc080
	.long	0xff0ffff0
	__v7_proc __v7_ca8_proc_info, __v7_setup, proc_fns = ca8_processor_functions
	.size	__v7_ca8_proc_info, . - __v7_ca8_proc_info

#endif	/* CONFIG_ARM_LPAE */

	/*
	 * Marvell PJ4B processor.
	 */
#ifdef CONFIG_CPU_PJ4B
	.type   __v7_pj4b_proc_info, #object
__v7_pj4b_proc_info:
	.long	0x560f5800
	.long	0xff0fff00
	__v7_proc __v7_pj4b_proc_info, __v7_pj4b_setup, proc_fns = pj4b_processor_functions
	.size	__v7_pj4b_proc_info, . - __v7_pj4b_proc_info
#endif

	/*
	 * ARM Ltd. Cortex R7 processor.
	 */
	.type	__v7_cr7mp_proc_info, #object
__v7_cr7mp_proc_info:
	.long	0x410fc170
	.long	0xff0ffff0
	__v7_proc __v7_cr7mp_proc_info, __v7_cr7mp_setup
	.size	__v7_cr7mp_proc_info, . - __v7_cr7mp_proc_info

	/*
	 * ARM Ltd. Cortex R8 processor.
	 */
	.type	__v7_cr8mp_proc_info, #object
__v7_cr8mp_proc_info:
	.long	0x410fc180
	.long	0xff0ffff0
	__v7_proc __v7_cr8mp_proc_info, __v7_cr8mp_setup
	.size	__v7_cr8mp_proc_info, . - __v7_cr8mp_proc_info

	/*
	 * ARM Ltd. Cortex A7 processor.
	 */
	.type	__v7_ca7mp_proc_info, #object
__v7_ca7mp_proc_info:
	.long	0x410fc070
	.long	0xff0ffff0
	__v7_proc __v7_ca7mp_proc_info, __v7_ca7mp_setup
	.size	__v7_ca7mp_proc_info, . - __v7_ca7mp_proc_info

	/*
	 * ARM Ltd. Cortex A12 processor.
	 */
	.type	__v7_ca12mp_proc_info, #object
__v7_ca12mp_proc_info:
	.long	0x410fc0d0
	.long	0xff0ffff0
	__v7_proc __v7_ca12mp_proc_info, __v7_ca12mp_setup, proc_fns = HARDENED_BPIALL_PROCESSOR_FUNCTIONS
	.size	__v7_ca12mp_proc_info, . - __v7_ca12mp_proc_info

	/*
	 * ARM Ltd. Cortex A15 processor.
	 */
	.type	__v7_ca15mp_proc_info, #object
__v7_ca15mp_proc_info:
	.long	0x410fc0f0
	.long	0xff0ffff0
	__v7_proc __v7_ca15mp_proc_info, __v7_ca15mp_setup, proc_fns = ca15_processor_functions
	.size	__v7_ca15mp_proc_info, . - __v7_ca15mp_proc_info

	/*
	 * Broadcom Corporation Brahma-B15 processor.
	 */
	.type	__v7_b15mp_proc_info, #object
__v7_b15mp_proc_info:
	.long	0x420f00f0
	.long	0xff0ffff0
	__v7_proc __v7_b15mp_proc_info, __v7_b15mp_setup, proc_fns = ca15_processor_functions, cache_fns = b15_cache_fns
	.size	__v7_b15mp_proc_info, . - __v7_b15mp_proc_info

	/*
	 * ARM Ltd. Cortex A17 processor.
	 */
	.type	__v7_ca17mp_proc_info, #object
__v7_ca17mp_proc_info:
	.long	0x410fc0e0
	.long	0xff0ffff0
	__v7_proc __v7_ca17mp_proc_info, __v7_ca17mp_setup, proc_fns = HARDENED_BPIALL_PROCESSOR_FUNCTIONS
	.size	__v7_ca17mp_proc_info, . - __v7_ca17mp_proc_info

	/* ARM Ltd. Cortex A73 processor */
	.type	__v7_ca73_proc_info, #object
__v7_ca73_proc_info:
	.long	0x410fd090
	.long	0xff0ffff0
	__v7_proc __v7_ca73_proc_info, __v7_setup, proc_fns = HARDENED_BPIALL_PROCESSOR_FUNCTIONS
	.size	__v7_ca73_proc_info, . - __v7_ca73_proc_info

	/* ARM Ltd. Cortex A75 processor */
	.type	__v7_ca75_proc_info, #object
__v7_ca75_proc_info:
	.long	0x410fd0a0
	.long	0xff0ffff0
	__v7_proc __v7_ca75_proc_info, __v7_setup, proc_fns = HARDENED_BPIALL_PROCESSOR_FUNCTIONS
	.size	__v7_ca75_proc_info, . - __v7_ca75_proc_info

	/*
	 * Qualcomm Inc. Krait processors.
	 */
	.type	__krait_proc_info, #object
// ------------------------------------------------------------------------
// Qualcomm Krait 处理器（高通定制 ARMv7 核心）
// Krait 的 MIDR 不报告 SDIV/UDIV 和 fused multiply 支持位，
// 但硬件实际支持。手动添加 HWCAP_IDIV 和 HWCAP_VFPv4。
// ------------------------------------------------------------------------
__krait_proc_info:
	.long	0x510f0400		@ Required ID value
	.long	0xff0ffc00		@ Mask for ID
	/*
	 * Some Krait processors don't indicate support for SDIV and UDIV
	 * instructions in the ARM instruction set, even though they actually
	 * do support them. They also don't indicate support for fused multiply
	 * instructions even though they actually do support them.
	 */
	__v7_proc __krait_proc_info, __v7_setup, hwcaps = HWCAP_IDIV | HWCAP_VFPv4
	.size	__krait_proc_info, . - __krait_proc_info

	/*
	 * Match any ARMv7 processor core.
	 */
	.type	__v7_proc_info, #object
// ------------------------------------------------------------------------
// 通用 ARMv7 处理器匹配条目（兜底匹配）
// 匹配规则: MIDR & 0x000f0000 == 0x000f0000
// 作为最后的手段, 匹配所有未被具体条目匹配的 ARMv7 核心。
// ------------------------------------------------------------------------
__v7_proc_info:
	.long	0x000f0000		@ Required ID value
	.long	0x000f0000		@ Mask for ID
	__v7_proc __v7_proc_info, __v7_setup
	.size	__v7_proc_info, . - __v7_proc_info
