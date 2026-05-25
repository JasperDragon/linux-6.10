/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM64 异常入口/退出与系统调用底层汇编代码
 *
 * 本文件实现了 ARM64 架构下完整的异常处理底层逻辑，包括:
 *   1. 异常向量表 (vectors)          -- 所有异常类型的跳转入口
 *   2. kernel_entry / kernel_exit     -- 异常上下文的保存与恢复宏
 *   3. 系统调用处理                   -- el0_sync 等路径
 *   4. 进程切换 (cpu_switch_to)       -- 任务上下文切换
 *   5. 中断栈切换 (call_on_irq_stack) -- 从任务栈切换到中断栈
 *   6. KPTI 熔断 (trampoline)         -- KAISER/KPTI 页表切换缓解
 *   7. Spectre/BHB 缓解               -- 分支预测历史缓解
 *   8. SDEI 处理                      -- 软件委托异常处理
 *
 * 异常流程概览:
 *   发生异常 → 硬件查 vectors[] → kernel_ventry 栈溢出检测
 *   → kernel_entry 保存 pt_regs → C 语言 handler
 *   → ret_to_kernel / ret_to_user → kernel_exit 恢复 → eret
 *
 * 寄存器使用约定:
 *   tsk (x28)  -- 始终指向当前 task_struct
 *   sp_el0      -- 用户态时指向 current task_struct
 *
 * Copyright (C) 2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/alternative.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/asm_pointer_auth.h>
#include <asm/bug.h>
#include <asm/cpufeature.h>
#include <asm/errno.h>
#include <asm/esr.h>
#include <asm/irq.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/scs.h>
#include <asm/stacktrace/frame.h>
#include <asm/thread_info.h>
#include <asm/asm-uaccess.h>
#include <asm/unistd.h>

/*
 * clear_gp_regs: 清零通用寄存器 x0~x29
 *
 * 从用户态 (EL0) 陷入内核时调用，防止用户态寄存器值意外泄露给内核代码。
 * 这是 arm64 内核安全基线的组成部分，确保内核不因遗留的用户寄存器值而泄漏信息。
 */
	.macro	clear_gp_regs
	.irp	n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
	mov	x\n, xzr
	.endr
	.endm

/*
 * kernel_ventry: 异常向量入口宏
 *
 * 每个异常向量表条目 (4 字节对齐到 128 字节槽) 的开头代码。
 * 负责：
 *   1. EL0 入口时清理 tpidrro_el0（KPTI trampoline 清理）
 *   2. 在栈上为 pt_regs 预留空间
 *   3. 检测内核栈溢出 —— 通过检查 SP 在 THREAD_SHIFT 对齐位上的值
 *   4. 如果溢出，切换到 overflow_stack 并继续处理
 *
 * 参数:
 *   el      - 异常来源异常级别 (0=EL0, 1=EL1)
 *   ht      - SP 使用模式 (t=EL0 stack, h=EL1 stack)
 *   regsize - 指令集宽度 (32/64)
 *   label   - 异常类型 (sync/irq/fiq/error)
 *
 * 栈溢出检测算法:
 *   由于任务栈和中断栈都是 THREAD_SHIFT 对齐的，SP 的第 THREAD_SHIFT 位
 *   正常应为 0。如果为 1，说明栈指针溢出或异常。
 */
	.macro kernel_ventry, el:req, ht:req, regsize:req, label:req
	.align 7
.Lventry_start\@:
	.if	\el == 0
	/*
	 * This must be the first instruction of the EL0 vector entries. It is
	 * skipped by the trampoline vectors, to trigger the cleanup.
	 */
	b	.Lskip_tramp_vectors_cleanup\@
	.if	\regsize == 64
	mrs	x30, tpidrro_el0
	msr	tpidrro_el0, xzr
	.else
	mov	x30, xzr
	.endif
.Lskip_tramp_vectors_cleanup\@:
	.endif

	sub	sp, sp, #PT_REGS_SIZE
	/*
	 * Test whether the SP has overflowed, without corrupting a GPR.
	 * Task and IRQ stacks are aligned so that SP & (1 << THREAD_SHIFT)
	 * should always be zero.
	 */
	add	sp, sp, x0			// sp' = sp + x0
	sub	x0, sp, x0			// x0' = sp' - x0 = (sp + x0) - x0 = sp
	tbnz	x0, #THREAD_SHIFT, 0f
	sub	x0, sp, x0			// x0'' = sp' - x0' = (sp + x0) - sp = x0
	sub	sp, sp, x0			// sp'' = sp' - x0 = (sp + x0) - x0 = sp
	b	el\el\ht\()_\regsize\()_\label

0:
	/*
	 * Either we've just detected an overflow, or we've taken an exception
	 * while on the overflow stack. Either way, we won't return to
	 * userspace, and can clobber EL0 registers to free up GPRs.
	 */

	/* Stash the original SP (minus PT_REGS_SIZE) in tpidr_el0. */
	msr	tpidr_el0, x0

	/* Recover the original x0 value and stash it in tpidrro_el0 */
	sub	x0, sp, x0
	msr	tpidrro_el0, x0

	/* Switch to the overflow stack */
	adr_this_cpu sp, overflow_stack + OVERFLOW_STACK_SIZE, x0

	/*
	 * Check whether we were already on the overflow stack. This may happen
	 * after panic() re-enables interrupts.
	 */
	mrs	x0, tpidr_el0			// sp of interrupted context
	sub	x0, sp, x0			// delta with top of overflow stack
	tst	x0, #~(OVERFLOW_STACK_SIZE - 1)	// within range?
	b.ne	__bad_stack			// no? -> bad stack pointer

	/* We were already on the overflow stack. Restore sp/x0 and carry on. */
	sub	sp, sp, x0
	mrs	x0, tpidrro_el0
	b	el\el\ht\()_\regsize\()_\label
.org .Lventry_start\@ + 128	// Did we overflow the ventry slot?
	.endm

/*
 * tramp_alias: 计算 trampoline 空间中某符号的虚拟地址
 *
 * KPTI 开启时，trampoline 代码位于 .entry.tramp.text 段，
 * 该段会被映射到 TRAMP_VALIAS 虚拟地址。
 * 此宏将符号相对于 .entry.tramp.text 的偏移映射到 trampoline 虚拟地址空间。
 *
 * 由于 trampoline 是位置无关的，不能直接用 adr_l，
 * 需要通过绝对寻址构造完整 64 位地址。
 *
 * 破坏: x0-x3 (由调用者负责保存/恢复)
 */
	.macro	tramp_alias, dst, sym
	.set	.Lalias\@, TRAMP_VALIAS + \sym - .entry.tramp.text
	movz	\dst, :abs_g2_s:.Lalias\@
	movk	\dst, :abs_g1_nc:.Lalias\@
	movk	\dst, :abs_g0_nc:.Lalias\@
	.endm

	/*
	 * apply_ssbd: 应用 SSBD (Speculative Store Bypass Disable) 缓解
	 *
	 * SSBD 用于缓解 Spectre Variant 4 (Speculative Store Bypass) 漏洞。
	 * 该宏会调用固件 (通过 SMC/HVC) 来启用或禁用 SSB 缓解。
	 *
	 * 从用户态进入内核时关闭 (state=1)，返回用户态时恢复 (state=0)。
	 * 但当前线程设置了 TIF_SSBD 标志时不强制执行。
	 *
	 * 此宏会破坏 x0-x3，调用者需自行保存/恢复。
	 *
	 * 参数:
	 *   state - ARM_SMCCC_ARCH_WORKAROUND_2 的状态值 (1=禁用, 0=启用)
	 *   tmp1, tmp2 - 临时寄存器
	 */
	.macro	apply_ssbd, state, tmp1, tmp2
alternative_cb	ARM64_ALWAYS_SYSTEM, spectre_v4_patch_fw_mitigation_enable
	b	.L__asm_ssbd_skip\@		// Patched to NOP
alternative_cb_end
	ldr_this_cpu	\tmp2, arm64_ssbd_callback_required, \tmp1
	cbz	\tmp2,	.L__asm_ssbd_skip\@
	ldr	\tmp2, [tsk, #TSK_TI_FLAGS]
	tbnz	\tmp2, #TIF_SSBD, .L__asm_ssbd_skip\@
	mov	w0, #ARM_SMCCC_ARCH_WORKAROUND_2
	mov	w1, #\state
alternative_cb	ARM64_ALWAYS_SYSTEM, smccc_patch_fw_mitigation_conduit
	nop					// Patched to SMC/HVC #0
alternative_cb_end
.L__asm_ssbd_skip\@:
	.endm

	/*
	 * check_mte_async_tcf: 检查 MTE 异步标签检查故障
	 *
	 * MTE (Memory Tagging Extension) 异步标签检查故障仅在 ASYNC (2)
	 * 或 ASYM (3) 模式下发生。这两种模式下 SCTLR_EL1.TCF0 的 bit 1 被置位。
	 *
	 * 如果检测到异步故障 (TFSRE0_EL1.TF0 置位)，
	 * 则在当前线程的 TIF 标志中设置 _TIF_MTE_ASYNC_FAULT，
	 * 以便在返回用户态时上报 SIGSEGV。
	 *
	 * 破坏: \tmp, \ti_flags
	 */
	.macro check_mte_async_tcf, tmp, ti_flags, thread_sctlr
#ifdef CONFIG_ARM64_MTE
	.arch_extension lse
alternative_if_not ARM64_MTE
	b	1f
alternative_else_nop_endif
	/*
	 * Asynchronous tag check faults are only possible in ASYNC (2) or
	 * ASYM (3) modes. In each of these modes bit 1 of SCTLR_EL1.TCF0 is
	 * set, so skip the check if it is unset.
	 */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	mrs_s	\tmp, SYS_TFSRE0_EL1
	tbz	\tmp, #SYS_TFSR_EL1_TF0_SHIFT, 1f
	/* Asynchronous TCF occurred for TTBR0 access, set the TI flag */
	mov	\tmp, #_TIF_MTE_ASYNC_FAULT
	add	\ti_flags, tsk, #TSK_TI_FLAGS
	stset	\tmp, [\ti_flags]
1:
#endif
	.endm

	/*
	 * clear_mte_async_tcf: 清除 MTE 异步标签检查故障
	 *
	 * 在返回用户态前，清除已记录的 MTE 异步故障 (写 TFSRE0_EL1=0)。
	 * 与 check_mte_async_tcf 对称，仅在 ASYNC/ASYM 模式下执行。
	 * 需要 DSB ISH 保证之前的存储对故障记录的可见性。
	 */
	.macro clear_mte_async_tcf thread_sctlr
#ifdef CONFIG_ARM64_MTE
alternative_if ARM64_MTE
	/* See comment in check_mte_async_tcf above. */
	tbz	\thread_sctlr, #(SCTLR_EL1_TCF0_SHIFT + 1), 1f
	dsb	ish
	msr_s	SYS_TFSRE0_EL1, xzr
1:
alternative_else_nop_endif
#endif
	.endm

/*
 * mte_set_gcr: 设置 MTE GCR_EL1 (标签排除寄存器)
 *
 * 从 thread.mte_ctrl 中提取 GCR 用户排除位，写入 GCR_EL1。
 * GCR_EL1.RRND 位同时被置位以启用随机标签生成。
 */
	.macro mte_set_gcr, mte_ctrl, tmp
#ifdef CONFIG_ARM64_MTE
	ubfx	\tmp, \mte_ctrl, #MTE_CTRL_GCR_USER_EXCL_SHIFT, #16
	orr	\tmp, \tmp, #SYS_GCR_EL1_RRND
	msr_s	SYS_GCR_EL1, \tmp
#endif
	.endm

/*
 * mte_set_kernel_gcr: 设置内核 MTE GCR (KASAN HW 标签模式)
 *
 * 仅在 CONFIG_KASAN_HW_TAGS 且 KASAN 已启用时执行。
 * 将 GCR_EL1 设置为 KERNEL_GCR_EL1，使内核能够分配自己的标签。
 * alternative_cb 机制允许在 KASAN 未启用时跳过。
 */
	.macro mte_set_kernel_gcr, tmp, tmp2
#ifdef CONFIG_KASAN_HW_TAGS
alternative_cb	ARM64_ALWAYS_SYSTEM, kasan_hw_tags_enable
	b	1f
alternative_cb_end
	mov	\tmp, KERNEL_GCR_EL1
	msr_s	SYS_GCR_EL1, \tmp
1:
#endif
	.endm

/*
 * mte_set_user_gcr: 设置用户态 MTE GCR
 *
 * 在返回用户态前，从当前任务的 thread.mte_ctrl 中恢复用户态
 * 的 GCR_EL1 值，使 MTE 标签分配与用户态配置一致。
 */
	.macro mte_set_user_gcr, tsk, tmp, tmp2
#ifdef CONFIG_KASAN_HW_TAGS
alternative_cb	ARM64_ALWAYS_SYSTEM, kasan_hw_tags_enable
	b	1f
alternative_cb_end
	ldr	\tmp, [\tsk, #THREAD_MTE_CTRL]

	mte_set_gcr \tmp, \tmp2
1:
#endif
	.endm

/*
 * kernel_entry: 异常上下文保存宏
 *
 * 在异常向量入口处调用，将发生异常时的 CPU 寄存器状态保存到栈上的
 * struct pt_regs 中。这是 C 语言异常处理函数能看到完整上下文的基础。
 *
 * 保存顺序（与 struct pt_regs 定义一致）:
 *   x0~x29 → 通用寄存器
 *   lr, sp → 链接寄存器和异常前栈指针
 *   elr_el1, spsr_el1 → 异常返回地址和处理器状态
 *   (仅 EL0) syscallno → 系统调用号
 *
 * 参数:
 *   el      - 异常来源 (0=EL0, 1=EL1)
 *   regsize - 指令集宽度 (32=AArch32, 64=AArch64)
 *
 * EL0 进入时额外操作:
 *   1. 清零用户寄存器 (clear_gp_regs) 防止信息泄露
 *   2. 保存 sp_el0 → 加载 entry_task
 *   3. 处理 Single Step 调试
 *   4. 检查 MTE 异步故障
 *   5. 安装内核 PAC 密钥
 *   6. 应用 SSBD 缓解
 *   7. 设置内核 MTE GCR
 *   8. 加载影子调用栈 (SCS)
 *
 * EL1 进入时额外操作:
 *   1. 记录异常前 SP (x21 = SP + PT_REGS_SIZE)
 *   2. 通过 sp_el0 获取当前任务
 *
 * 宏展开后的关键寄存器:
 *   x20 - ICC_PMR_EL1  (伪 NMI 时使用)
 *   x21 - 异常前的 SP (EL0: sp_el0, EL1: aborted SP)
 *   x22 - elr_el1 (异常返回地址)
 *   x23 - spsr_el1 (异常前 PSTATE)
 *   x28/tsk - current task_struct
 */
	.macro	kernel_entry, el, regsize = 64
	/*
	 * ★ EL0 → EL1 异常入口: 从用户态进入时的安全检查
	 */

	/*
	 * 先将所有通用寄存器保存到栈上的 pt_regs 结构体中
	 * 以下 stp 指令依次保存 x0~x29
	 */
	.if	\el == 0
	alternative_insn nop, SET_PSTATE_DIT(1), ARM64_HAS_DIT	// EL0 入口: 启用数据独立定时
	.endif
	.if	\regsize == 32
	mov	w0, w0				// 32位系统调用: 清零 x0 高 32 位
	.endif
	stp	x0, x1, [sp, #16 * 0]		// 保存 x0, x1 到 pt_regs
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]	// 保存 x28 (tsk), x29 (fp)

	.if	\el == 0
	clear_gp_regs			// 清零用户寄存器
	mrs	x21, sp_el0
	ldr_this_cpu	tsk, __entry_task, x20
	msr	sp_el0, tsk

	/*
	 * Ensure MDSCR_EL1.SS is clear, since we can unmask debug exceptions
	 * when scheduling.
	 */
	ldr	x19, [tsk, #TSK_TI_FLAGS]
	disable_step_tsk x19, x20

	/* Check for asynchronous tag check faults in user space */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	check_mte_async_tcf x22, x23, x0

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * Enable IA for in-kernel PAC if the task had it disabled. Although
	 * this could be implemented with an unconditional MRS which would avoid
	 * a load, this was measured to be slower on Cortex-A75 and Cortex-A76.
	 *
	 * Install the kernel IA key only if IA was enabled in the task. If IA
	 * was disabled on kernel exit then we would have left the kernel IA
	 * installed so there is no need to install it again.
	 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_kernel_nosync tsk, x20, x22, x23
	b	2f
1:
	mrs	x0, sctlr_el1
	orr	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	apply_ssbd 1, x22, x23

	mte_set_kernel_gcr x22, x23

	/*
	 * Any non-self-synchronizing system register updates required for
	 * kernel entry should be placed before this point.
	 */
alternative_if ARM64_MTE
	isb
	b	1f
alternative_else_nop_endif
alternative_if ARM64_HAS_ADDRESS_AUTH
	isb
alternative_else_nop_endif
1:

	scs_load_current_base
	.else
	add	x21, sp, #PT_REGS_SIZE
	get_current_task tsk
	.endif /* \el == 0 */
	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]

	/*
	 * Create a metadata frame record. The unwinder will use this to
	 * identify and unwind exception boundaries.
	 */
	stp	xzr, xzr, [sp, #S_STACKFRAME]
	.if \el == 0
	mov	x0, #FRAME_META_TYPE_FINAL
	.else
	mov	x0, #FRAME_META_TYPE_PT_REGS
	.endif
	str	x0, [sp, #S_STACKFRAME_TYPE]
	add	x29, sp, #S_STACKFRAME

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
	bl	__swpan_entry_el\el
alternative_else_nop_endif
#endif

	stp	x22, x23, [sp, #S_PC]

	/* Not in a syscall by default (el0_svc overwrites for real syscall) */
	.if	\el == 0
	mov	w21, #NO_SYSCALL
	str	w21, [sp, #S_SYSCALLNO]
	.endif

#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_save\@
alternative_else_nop_endif

	mrs_s	x20, SYS_ICC_PMR_EL1
	str	w20, [sp, #S_PMR]
	mov	x20, #GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET
	msr_s	SYS_ICC_PMR_EL1, x20

.Lskip_pmr_save\@:
#endif

	/*
	 * Registers that may be useful after this macro is invoked:
	 *
	 * x20 - ICC_PMR_EL1
	 * x21 - aborted SP
	 * x22 - aborted PC
	 * x23 - aborted PSTATE
	*/
	.endm

/*
 * kernel_exit: 异常上下文恢复宏
 *
 * 从栈上的 pt_regs 结构中恢复所有通用寄存器并返回被中断的执行流。
 * 是 kernel_entry 的逆操作。
 *
 * 参数:
 *   el - 返回目标异常级别 (0=EL0, 1=EL1)
 *
 * 恢复顺序:
 *   1. 恢复 ICC_PMR_EL1 (伪 NMI)
 *   2. 加载 ELR_EL1 和 SPSR_EL1
 *   3. SW PAN 退出处理 (EL1)
 *   4. 恢复用户态 SP (EL0)
 *   5. 处理 MTE 异步标签故障清除 (EL0)
 *   6. 安装用户态 PAC 密钥 (EL0)
 *   7. 设置用户态 MTE GCR (EL0)
 *   8. 应用 SSBD 恢复 (EL0)
 *   9. 写 ELR_EL1, SPSR_EL1
 *   10. 从栈恢复 x0~x29
 *   11. KPTI trampoline 退出 (EL0, KPTI开启)
 *   12. eret 返回
 *
 * Spectre 缓解:
 *   EL0 返回前执行 TLBI VALE1 防止预测加载敏感数据
 *   EL1 返回前执行 DMB SY 确保 device/NC 读取完成
 */
	.macro	kernel_exit, el
	.if	\el != 0
	disable_daif
	.endif

#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_restore\@
alternative_else_nop_endif

	ldr	w20, [sp, #S_PMR]
	msr_s	SYS_ICC_PMR_EL1, x20

	/* Ensure priority change is seen by redistributor */
alternative_if_not ARM64_HAS_GIC_PRIO_RELAXED_SYNC
	dsb	sy
alternative_else_nop_endif

.Lskip_pmr_restore\@:
#endif

	ldp	x21, x22, [sp, #S_PC]		// load ELR, SPSR

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
alternative_if_not ARM64_HAS_PAN
	bl	__swpan_exit_el\el
alternative_else_nop_endif
#endif

	.if	\el == 0
	ldr	x23, [sp, #S_SP]		// load return stack pointer
	msr	sp_el0, x23
	tst	x22, #PSR_MODE32_BIT		// native task?
	b.eq	3f

#ifdef CONFIG_ARM64_ERRATUM_845719
alternative_if ARM64_WORKAROUND_845719
#ifdef CONFIG_PID_IN_CONTEXTIDR
	mrs	x29, contextidr_el1
	msr	contextidr_el1, x29
#else
	msr contextidr_el1, xzr
#endif
alternative_else_nop_endif
#endif
3:
	/* Ignore asynchronous tag check faults in the uaccess routines */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	clear_mte_async_tcf x0

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * IA was enabled for in-kernel PAC. Disable it now if needed, or
	 * alternatively install the user's IA. All other per-task keys and
	 * SCTLR bits were updated on task switch.
	 *
	 * No kernel C function calls after this.
	 */
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_user tsk, x0, x1, x2
	b	2f
1:
	mrs	x0, sctlr_el1
	bic	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	mte_set_user_gcr tsk, x0, x1

	apply_ssbd 0, x0, x1
	.endif

	msr	elr_el1, x21			// set up the return data
	msr	spsr_el1, x22
	ldp	x0, x1, [sp, #16 * 0]
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]

	.if	\el == 0
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	alternative_insn "b .L_skip_tramp_exit_\@", nop, ARM64_UNMAP_KERNEL_AT_EL0

	msr	far_el1, x29

	ldr_this_cpu	x30, this_cpu_vector, x29
	tramp_alias	x29, tramp_exit
	msr		vbar_el1, x30		// install vector table
	ldr		lr, [sp, #S_LR]		// restore x30
	add		sp, sp, #PT_REGS_SIZE	// restore sp
	br		x29

.L_skip_tramp_exit_\@:
#endif
	.endif

	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE		// restore sp

	.if \el == 0
	/* This must be after the last explicit memory access */
alternative_if ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD
	tlbi	vale1, xzr
	dsb	nsh
alternative_else_nop_endif
	.else
	/* Ensure any device/NC reads complete */
	alternative_insn nop, "dmb sy", ARM64_WORKAROUND_1508412
	.endif

	eret
	sb
	.endm

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * __swpan_entry_el1: 软件模拟 PAN (Privileged Access Never) 入口
	 *
	 * 在没有硬件 PAN 支持的 ARMv8.0 CPU 上，通过操作 TTBR0_EL1 来模拟 PAN。
	 * 从 EL1 进入异常时，将 TTBR0_EL1 切换为保留 ASID (禁用用户态映射访问)，
	 * 并在 SPSR 中设置模拟的 PAN 位。
	 *
	 * 注意: 来自 EL0 的异常不需要此操作，因为用户态访问总是启用的。
	 *
	 * 与 ARMv8.1 硬件 PAN 的区别:
	 * 软件 PAN 禁用所有 TTBR0_EL1 访问，而硬件 PAN 仅禁用用户映射访问。
	 */

	/*
	 * (English original:)
	 * Set the TTBR0 PAN bit in SPSR. When the exception is taken from
	 * EL0, there is no need to check the state of TTBR0_EL1 since
	 * accesses are always enabled.
	 * Note that the meaning of this bit differs from the ARMv8.1 PAN
	 * feature as all TTBR0_EL1 accesses are disabled, not just those to
	 * user mappings.
	 */
SYM_CODE_START_LOCAL(__swpan_entry_el1)
	mrs	x21, ttbr0_el1
	tst	x21, #TTBRx_EL1_ASID_MASK	// Check for the reserved ASID
	orr	x23, x23, #PSR_PAN_BIT		// Set the emulated PAN in the saved SPSR
	b.eq	1f				// TTBR0 access already disabled
	and	x23, x23, #~PSR_PAN_BIT		// Clear the emulated PAN in the saved SPSR
SYM_INNER_LABEL(__swpan_entry_el0, SYM_L_LOCAL)
	__uaccess_ttbr0_disable x21
1:	ret
SYM_CODE_END(__swpan_entry_el1)

	/*
	 * Restore access to TTBR0_EL1. If returning to EL0, no need for SPSR
	 * PAN bit checking.
	 */
SYM_CODE_START_LOCAL(__swpan_exit_el1)
	tbnz	x22, #22, 1f			// Skip re-enabling TTBR0 access if the PSR_PAN_BIT is set
	__uaccess_ttbr0_enable x0, x1
1:	and	x22, x22, #~PSR_PAN_BIT		// ARMv8.0 CPUs do not understand this bit
	ret
SYM_CODE_END(__swpan_exit_el1)

SYM_CODE_START_LOCAL(__swpan_exit_el0)
	__uaccess_ttbr0_enable x0, x1
	/*
	 * Enable errata workarounds only if returning to user. The only
	 * workaround currently required for TTBR0_EL1 changes are for the
	 * Cavium erratum 27456 (broadcast TLBI instructions may cause I-cache
	 * corruption).
	 */
	b	post_ttbr_update_workaround
SYM_CODE_END(__swpan_exit_el0)
#endif

/* GPRs used by entry code */
tsk	.req	x28		// current thread_info

	.text

/*
 * vectors: ARM64 异常向量表
 *
 * ARM64 架构使用 4 级异常向量表，每级包含 4 个条目 (每个 128 字节):
 *
 *   基址 + 0x000: EL1t Sync   (EL1, SP_EL0, 同步)
 *   基址 + 0x080: EL1t IRQ    (EL1, SP_EL0, IRQ)
 *   基址 + 0x100: EL1t FIQ    (EL1, SP_EL0, FIQ)
 *   基址 + 0x180: EL1t Error  (EL1, SP_EL0, 错误)
 *   基址 + 0x200: EL1h Sync   (EL1, SP_EL1, 同步)
 *   基址 + 0x280: EL1h IRQ    (EL1, SP_EL1, IRQ)
 *   基址 + 0x300: EL1h FIQ    (EL1, SP_EL1, FIQ)
 *   基址 + 0x380: EL1h Error  (EL1, SP_EL1, 错误)
 *   基址 + 0x400: EL0 64 Sync (EL0, AArch64, 同步)
 *   基址 + 0x480: EL0 64 IRQ  (EL0, AArch64, IRQ)
 *   基址 + 0x500: EL0 64 FIQ  (EL0, AArch64, FIQ)
 *   基址 + 0x580: EL0 64 Err  (EL0, AArch64, 错误)
 *   基址 + 0x600: EL0 32 Sync (EL0, AArch32, 同步)
 *   基址 + 0x680: EL0 32 IRQ  (EL0, AArch32, IRQ)
 *   基址 + 0x700: EL0 32 FIQ  (EL0, AArch32, FIQ)
 *   基址 + 0x800: EL0 32 Err  (EL0, AArch32, 错误)
 *
 * 向量表基址通过 VBAR_EL1 寄存器设置。
 * 硬件根据 "异常级别 + SP 选择 + 异常类型" 自动索引到对应条目。
 *
 * 'h' (SP_EL1) 只有 EL1 使用，'t' (SP_EL0) 在 EL1 仅有理论意义。
 * 系统调用 (SVC) 从 EL0 进入，属于 Synchronous EL0。
 */
	.pushsection ".entry.text", "ax"

	.align	11
SYM_CODE_START(vectors)
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1t
	kernel_ventry	1, t, 64, error		// Error EL1t

	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h

	kernel_ventry	0, t, 64, sync		// Synchronous 64-bit EL0
	kernel_ventry	0, t, 64, irq		// IRQ 64-bit EL0
	kernel_ventry	0, t, 64, fiq		// FIQ 64-bit EL0
	kernel_ventry	0, t, 64, error		// Error 64-bit EL0

	kernel_ventry	0, t, 32, sync		// Synchronous 32-bit EL0
	kernel_ventry	0, t, 32, irq		// IRQ 32-bit EL0
	kernel_ventry	0, t, 32, fiq		// FIQ 32-bit EL0
	kernel_ventry	0, t, 32, error		// Error 32-bit EL0
SYM_CODE_END(vectors)

SYM_CODE_START_LOCAL(__bad_stack)
	/*
	 * We detected an overflow in kernel_ventry, which switched to the
	 * overflow stack. Stash the exception regs, and head to our overflow
	 * handler.
	 */

	/* Restore the original x0 value */
	mrs	x0, tpidrro_el0

	/*
	 * Store the original GPRs to the new stack. The orginal SP (minus
	 * PT_REGS_SIZE) was stashed in tpidr_el0 by kernel_ventry.
	 */
	sub	sp, sp, #PT_REGS_SIZE
	kernel_entry 1
	mrs	x0, tpidr_el0
	add	x0, x0, #PT_REGS_SIZE
	str	x0, [sp, #S_SP]

	/* Stash the regs for handle_bad_stack */
	mov	x0, sp

	/* Time to die */
	bl	handle_bad_stack
	ASM_BUG()
SYM_CODE_END(__bad_stack)


/*
 * entry_handler: 异常处理函数调用宏
 *
 * 为指定的异常类型生成完整处理流程:
 *   1. kernel_entry → 保存上下文到栈
 *   2. mov x0, sp → 传递 pt_regs 指针
 *   3. bl XXX_handler → 调用 C 语言处理函数
 *   4. ret_to_user / ret_to_kernel → 根据异常来源决定返回路径
 *
 * 宏名格式: el{el}{ht}_{regsize}_{label}
 * 例如: el0t_64_sync   (EL0, AArch64, 同步 = 系统调用)
 *       el1h_64_irq    (EL1, SP_EL1, IRQ = 中断)
 */
	.macro entry_handler el:req, ht:req, regsize:req, label:req
SYM_CODE_START_LOCAL(el\el\ht\()_\regsize\()_\label)
	kernel_entry \el, \regsize
	mov	x0, sp
	bl	el\el\ht\()_\regsize\()_\label\()_handler
	.if \el == 0
	b	ret_to_user
	.else
	b	ret_to_kernel
	.endif
SYM_CODE_END(el\el\ht\()_\regsize\()_\label)
	.endm

/*
 * Early exception handlers
 */
	entry_handler	1, t, 64, sync
	entry_handler	1, t, 64, irq
	entry_handler	1, t, 64, fiq
	entry_handler	1, t, 64, error

	entry_handler	1, h, 64, sync
	entry_handler	1, h, 64, irq
	entry_handler	1, h, 64, fiq
	entry_handler	1, h, 64, error

	entry_handler	0, t, 64, sync
	entry_handler	0, t, 64, irq
	entry_handler	0, t, 64, fiq
	entry_handler	0, t, 64, error

	entry_handler	0, t, 32, sync
	entry_handler	0, t, 32, irq
	entry_handler	0, t, 32, fiq
	entry_handler	0, t, 32, error

SYM_CODE_START_LOCAL(ret_to_kernel)
	kernel_exit 1
SYM_CODE_END(ret_to_kernel)

SYM_CODE_START_LOCAL(ret_to_user)
	ldr	x19, [tsk, #TSK_TI_FLAGS]	// re-check for single-step
	enable_step_tsk x19, x2
#ifdef CONFIG_KSTACK_ERASE
	bl	stackleak_erase_on_task_stack
#endif
	kernel_exit 0
SYM_CODE_END(ret_to_user)

	.popsection				// .entry.text

	/*
 * tramp_map_kernel: KPTI trampoline → 内核页表切换
 *
 * 从 tramp_pg_dir (最小用户态映射) 切换到 swapper_pg_dir (完整内核映射)。
 * 通过将 ttbr1_el1 加上 TRAMP_SWAPPER_OFFSET 并清除 USER_ASID_FLAG 实现。
 *
 * 对于 QCOM_FALKOR_ERRATUM_1003，还需要执行 TLBI 以清除页表缓存。
 */
// Move from tramp_pg_dir to swapper_pg_dir
	.macro tramp_map_kernel, tmp
	mrs	\tmp, ttbr1_el1
	add	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET
	bic	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1003
alternative_if ARM64_WORKAROUND_QCOM_FALKOR_E1003
	/* ASID already in \tmp[63:48] */
	movk	\tmp, #:abs_g2_nc:(TRAMP_VALIAS >> 12)
	movk	\tmp, #:abs_g1_nc:(TRAMP_VALIAS >> 12)
	/* 2MB boundary containing the vectors, so we nobble the walk cache */
	movk	\tmp, #:abs_g0_nc:((TRAMP_VALIAS & ~(SZ_2M - 1)) >> 12)
	isb
	tlbi	vae1, \tmp
	dsb	nsh
alternative_else_nop_endif
#endif /* CONFIG_QCOM_FALKOR_ERRATUM_1003 */
	.endm

	/*
 * tramp_unmap_kernel: 内核页表 → KPTI trampoline 切换
 *
 * 返回用户态前，从 swapper_pg_dir 切换回 tramp_pg_dir，
 * 移除内核映射以防止 Meltdown 类攻击。
 * 通过设置 USER_ASID_FLAG 并减去 TRAMP_SWAPPER_OFFSET 实现。
 *
 * 注意: 不会执行 post_ttbr_update_workaround，
 * 因为该 workaround (Cavium ThunderX) 要求 KPTI 关闭，与 KPTI 互斥。
 */
// Move from swapper_pg_dir to tramp_pg_dir
	.macro tramp_unmap_kernel, tmp
	mrs	\tmp, ttbr1_el1
	sub	\tmp, \tmp, #TRAMP_SWAPPER_OFFSET
	orr	\tmp, \tmp, #USER_ASID_FLAG
	msr	ttbr1_el1, \tmp
	/*
	 * We avoid running the post_ttbr_update_workaround here because
	 * it's only needed by Cavium ThunderX, which requires KPTI to be
	 * disabled.
	 */
	.endm

	.macro		tramp_data_read_var	dst, var
#ifdef CONFIG_RELOCATABLE
	ldr		\dst, .L__tramp_data_\var
	.ifndef		.L__tramp_data_\var
	.pushsection	".entry.tramp.rodata", "a", %progbits
	.align		3
.L__tramp_data_\var:
	.quad		\var
	.popsection
	.endif
#else
	/*
	 * As !RELOCATABLE implies !RANDOMIZE_BASE the address is always a
	 * compile time constant (and hence not secret and not worth hiding).
	 *
	 * As statically allocated kernel code and data always live in the top
	 * 47 bits of the address space we can sign-extend bit 47 and avoid an
	 * instruction to load the upper 16 bits (which must be 0xFFFF).
	 */
	movz		\dst, :abs_g2_s:\var
	movk		\dst, :abs_g1_nc:\var
	movk		\dst, :abs_g0_nc:\var
#endif
	.endm

#define BHB_MITIGATION_NONE	0
#define BHB_MITIGATION_LOOP	1
#define BHB_MITIGATION_FW	2
#define BHB_MITIGATION_INSN	3

	.macro tramp_ventry, vector_start, regsize, kpti, bhb
	.align	7
1:
	.if	\regsize == 64
	msr	tpidrro_el0, x30	// Restored in kernel_ventry
	.endif

	.if	\bhb == BHB_MITIGATION_LOOP
	/*
	 * This sequence must appear before the first indirect branch. i.e. the
	 * ret out of tramp_ventry. It appears here because x30 is free.
	 */
	__mitigate_spectre_bhb_loop	x30
	.endif // \bhb == BHB_MITIGATION_LOOP

	.if	\bhb == BHB_MITIGATION_INSN
	clearbhb
	isb
	.endif // \bhb == BHB_MITIGATION_INSN

	.if	\kpti == 1
	/*
	 * Defend against branch aliasing attacks by pushing a dummy
	 * entry onto the return stack and using a RET instruction to
	 * enter the full-fat kernel vectors.
	 */
	bl	2f
	b	.
2:
	tramp_map_kernel	x30
alternative_insn isb, nop, ARM64_WORKAROUND_QCOM_FALKOR_E1003
	tramp_data_read_var	x30, vectors
alternative_if_not ARM64_WORKAROUND_CAVIUM_TX2_219_PRFM
	prfm	plil1strm, [x30, #(1b - \vector_start)]
alternative_else_nop_endif

	msr	vbar_el1, x30
	isb
	.else
	adr_l	x30, vectors
	.endif // \kpti == 1

	.if	\bhb == BHB_MITIGATION_FW
	/*
	 * The firmware sequence must appear before the first indirect branch.
	 * i.e. the ret out of tramp_ventry. But it also needs the stack to be
	 * mapped to save/restore the registers the SMC clobbers.
	 */
	__mitigate_spectre_bhb_fw
	.endif // \bhb == BHB_MITIGATION_FW

	add	x30, x30, #(1b - \vector_start + 4)
	ret
.org 1b + 128	// Did we overflow the ventry slot?
	.endm

	.macro	generate_tramp_vector,	kpti, bhb
.Lvector_start\@:
	.space	0x400

	.rept	4
	tramp_ventry	.Lvector_start\@, 64, \kpti, \bhb
	.endr
	.rept	4
	tramp_ventry	.Lvector_start\@, 32, \kpti, \bhb
	.endr
	.endm

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * Exception vectors trampoline.
 * The order must match __bp_harden_el1_vectors and the
 * arm64_bp_harden_el1_vectors enum.
 */
	.pushsection ".entry.tramp.text", "ax"
	.align	11
SYM_CODE_START_LOCAL_NOALIGN(tramp_vectors)
#ifdef CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_LOOP
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_FW
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_INSN
#endif /* CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY */
	generate_tramp_vector	kpti=1, bhb=BHB_MITIGATION_NONE
SYM_CODE_END(tramp_vectors)

SYM_CODE_START_LOCAL(tramp_exit)
	tramp_unmap_kernel	x29
	mrs		x29, far_el1		// restore x29
	eret
	sb
SYM_CODE_END(tramp_exit)
	.popsection				// .entry.tramp.text
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Exception vectors for spectre mitigations on entry from EL1 when
 * kpti is not in use.
 */
	.macro generate_el1_vector, bhb
.Lvector_start\@:
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1h
	kernel_ventry	1, t, 64, error		// Error EL1t

	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h

	.rept	4
	tramp_ventry	.Lvector_start\@, 64, 0, \bhb
	.endr
	.rept 4
	tramp_ventry	.Lvector_start\@, 32, 0, \bhb
	.endr
	.endm

/* The order must match tramp_vecs and the arm64_bp_harden_el1_vectors enum. */
	.pushsection ".entry.text", "ax"
	.align	11
/*
 * __bp_harden_el1_vectors: EL1 异常向量表 (Spectre BHB 缓解)
 *
 * 当 KPTI 未启用但需要 Spectre BHB 缓解时使用。
 * 仅包含 EL1 条目 (EL0 条目使用 tramp_ventry 处理)。
 * 根据 BHB 缓解策略提供多组向量表，由 __bp_harden_el1_vectors 和
 * arm64_bp_harden_el1_vectors 枚举索引。
 *
 * 顺序必须与 tramp_vecs 和 arm64_bp_harden_el1_vectors 枚举一致。
 */
SYM_CODE_START(__bp_harden_el1_vectors)
#ifdef CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY
	generate_el1_vector	bhb=BHB_MITIGATION_LOOP
	generate_el1_vector	bhb=BHB_MITIGATION_FW
	generate_el1_vector	bhb=BHB_MITIGATION_INSN
#endif /* CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY */
SYM_CODE_END(__bp_harden_el1_vectors)
	.popsection


/*
 * cpu_switch_to: ARM64 进程切换核心函数
 *
 * 保存当前进程 (prev) 的被调用方保存寄存器 (x19~x30, fp, sp)，
 * 恢复下一个进程 (next) 的这些寄存器。
 *
 * 调用约定:
 *   x0 = prev (上一个 task_struct，必须保持到切换完成)
 *   x1 = next (下一个 task_struct)
 *   prev 和 next 保证不同
 *
 * 保存/恢复的内容:
 *   - 被调用方保存寄存器 x19~x28
 *   - x29 (帧指针), x9 (SP, 通过栈帧间接保存)
 *   - lr (x30, 链接寄存器)
 *   - sp_el0 (用户态 SP / current task 指针)
 *   - PAC 密钥 (ptrauth_keys_install_kernel)
 *   - 影子调用栈 (SCS)
 *
 * 注意:
 *   - 需要先调用 save_and_disable_daif 保存并屏蔽中断，
 *     然后在切换完成后再调用 restore_irq 恢复。
 *   - THREAD_CPU_CONTEXT 定义了 thread_struct 中保存上下文的偏移。
 */
SYM_FUNC_START(cpu_switch_to)
	save_and_disable_daif x11
	mov	x10, #THREAD_CPU_CONTEXT
	add	x8, x0, x10
	mov	x9, sp
	stp	x19, x20, [x8], #16		// store callee-saved registers
	stp	x21, x22, [x8], #16
	stp	x23, x24, [x8], #16
	stp	x25, x26, [x8], #16
	stp	x27, x28, [x8], #16
	stp	x29, x9, [x8], #16
	str	lr, [x8]
	add	x8, x1, x10
	ldp	x19, x20, [x8], #16		// restore callee-saved registers
	ldp	x21, x22, [x8], #16
	ldp	x23, x24, [x8], #16
	ldp	x25, x26, [x8], #16
	ldp	x27, x28, [x8], #16
	ldp	x29, x9, [x8], #16
	ldr	lr, [x8]
	mov	sp, x9
	msr	sp_el0, x1
	ptrauth_keys_install_kernel x1, x8, x9, x10
	scs_save x0
	scs_load_current
	restore_irq x11
	ret
SYM_FUNC_END(cpu_switch_to)
NOKPROBE(cpu_switch_to)

/*
 * ret_from_fork: 新创建进程 (fork/clone) 的首次返回路径
 *
 * 新进程通过 fork/clone 系统调用创建时，其 pt_regs 中保存的 PC
 * 指向此函数。这是该进程第一次真正"运行"时的入口。
 *
 * 流程:
 *   1. schedule_tail() → 完成调度器的收尾工作
 *   2. 如果是内核线程 (x19 != 0):
 *      - x20 = 参数, x19 = 函数指针 → blr x19 执行内核线程函数
 *   3. 用户态进程:
 *      - asm_exit_to_user_mode() → 退出内核模式
 *      - ret_to_user → 返回用户空间
 */
SYM_CODE_START(ret_from_fork)
	bl	schedule_tail
	cbz	x19, 1f				// not a kernel thread
	mov	x0, x20
	blr	x19
1:	get_current_task tsk
	mov	x0, sp
	bl	asm_exit_to_user_mode
	b	ret_to_user
SYM_CODE_END(ret_from_fork)
NOKPROBE(ret_from_fork)

/*
 * call_on_irq_stack: 切换到中断栈执行指定函数
 *
 * 在 Linux 内核中，中断处理需要运行在独立的中断栈 (irq_stack) 上，
 * 而不是当前任务的内核栈，以避免栈溢出并确保中断处理的独立性。
 *
 * 实现细节:
 *   1. 保存当前 SP 和 LR 到栈帧
 *   2. 加载当前 CPU 的中断栈指针 (per-CPU irq_stack_ptr)
 *   3. 切换到中断栈并调用 func(regs)
 *   4. 函数返回后，从栈帧恢复原始 SP 和 LR
 *   5. 如果启用了 SCS (影子调用栈)，同时切换到中断的影子调用栈
 *
 * 参数:
 *   regs - pt_regs 指针
 *   func - 要在中断栈上执行的函数
 *
 * 注意: 调用前后都需要 save_and_disable_irq / restore_irq
 * 以避免在切换栈的过程中再次被中断打断。
 */
SYM_FUNC_START(call_on_irq_stack)
	save_and_disable_daif x9
#ifdef CONFIG_SHADOW_CALL_STACK
	get_current_task x16
	scs_save x16
	ldr_this_cpu scs_sp, irq_shadow_call_stack_ptr, x17
#endif

	/* Create a frame record to save our LR and SP (implicit in FP) */
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

	ldr_this_cpu x16, irq_stack_ptr, x17

	/* Move to the new stack and call the function there */
	add	sp, x16, #IRQ_STACK_SIZE
	restore_irq x9
	blr	x1

	save_and_disable_daif x9
	/*
	 * Restore the SP from the FP, and restore the FP and LR from the frame
	 * record.
	 */
	mov	sp, x29
	ldp	x29, x30, [sp], #16
	scs_load_current
	restore_irq x9
	ret
SYM_FUNC_END(call_on_irq_stack)
NOKPROBE(call_on_irq_stack)

#ifdef CONFIG_ARM_SDE_INTERFACE

#include <asm/sdei.h>
#include <uapi/linux/arm_sdei.h>

/*
 * sdei_handler_exit: SDEI 退出宏
 *
 * SDEI (Software Delegated Exception Interface)
 * 允许固件将特定事件委托给 OS 处理。完成后通过 SMC 或 HVC 通知固件。
 *
 * SDEI_EXIT_SMC → SMC 指令 (EL3)
 * 其他 → HVC 指令 (EL2 或 hypervisor)
 */
.macro sdei_handler_exit exit_mode
	/* On success, this call never returns... */
	cmp	\exit_mode, #SDEI_EXIT_SMC
	b.ne	99f
	smc	#0
	b	.
99:	hvc	#0
	b	.
.endm

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
/*
 * The regular SDEI entry point may have been unmapped along with the rest of
 * the kernel. This trampoline restores the kernel mapping to make the x1 memory
 * argument accessible.
 *
 * This clobbers x4, __sdei_handler() will restore this from firmware's
 * copy.
 */
.pushsection ".entry.tramp.text", "ax"
SYM_CODE_START(__sdei_asm_entry_trampoline)
	mrs	x4, ttbr1_el1
	tbz	x4, #USER_ASID_BIT, 1f

	tramp_map_kernel tmp=x4
	isb
	mov	x4, xzr

	/*
	 * Remember whether to unmap the kernel on exit.
	 */
1:	str	x4, [x1, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]
	tramp_data_read_var     x4, __sdei_asm_handler
	br	x4
SYM_CODE_END(__sdei_asm_entry_trampoline)
NOKPROBE(__sdei_asm_entry_trampoline)

/*
 * Make the exit call and restore the original ttbr1_el1
 *
 * x0 & x1: setup for the exit API call
 * x2: exit_mode
 * x4: struct sdei_registered_event argument from registration time.
 */
SYM_CODE_START(__sdei_asm_exit_trampoline)
	ldr	x4, [x4, #(SDEI_EVENT_INTREGS + S_SDEI_TTBR1)]
	cbnz	x4, 1f

	tramp_unmap_kernel	tmp=x4

1:	sdei_handler_exit exit_mode=x2
SYM_CODE_END(__sdei_asm_exit_trampoline)
NOKPROBE(__sdei_asm_exit_trampoline)
.popsection		// .entry.tramp.text
#endif /* CONFIG_UNMAP_KERNEL_AT_EL0 */

/*
 * Software Delegated Exception entry point.
 *
 * x0: Event number
 * x1: struct sdei_registered_event argument from registration time.
 * x2: interrupted PC
 * x3: interrupted PSTATE
 * x4: maybe clobbered by the trampoline
 *
 * Firmware has preserved x0->x17 for us, we must save/restore the rest to
 * follow SMC-CC. We save (or retrieve) all the registers as the handler may
 * want them.
 */
SYM_CODE_START(__sdei_asm_handler)
	stp     x2, x3, [x1, #SDEI_EVENT_INTREGS + S_PC]
	stp     x4, x5, [x1, #SDEI_EVENT_INTREGS + 16 * 2]
	stp     x6, x7, [x1, #SDEI_EVENT_INTREGS + 16 * 3]
	stp     x8, x9, [x1, #SDEI_EVENT_INTREGS + 16 * 4]
	stp     x10, x11, [x1, #SDEI_EVENT_INTREGS + 16 * 5]
	stp     x12, x13, [x1, #SDEI_EVENT_INTREGS + 16 * 6]
	stp     x14, x15, [x1, #SDEI_EVENT_INTREGS + 16 * 7]
	stp     x16, x17, [x1, #SDEI_EVENT_INTREGS + 16 * 8]
	stp     x18, x19, [x1, #SDEI_EVENT_INTREGS + 16 * 9]
	stp     x20, x21, [x1, #SDEI_EVENT_INTREGS + 16 * 10]
	stp     x22, x23, [x1, #SDEI_EVENT_INTREGS + 16 * 11]
	stp     x24, x25, [x1, #SDEI_EVENT_INTREGS + 16 * 12]
	stp     x26, x27, [x1, #SDEI_EVENT_INTREGS + 16 * 13]
	stp     x28, x29, [x1, #SDEI_EVENT_INTREGS + 16 * 14]
	mov	x4, sp
	stp     lr, x4, [x1, #SDEI_EVENT_INTREGS + S_LR]

	mov	x19, x1

	/* 记录注册事件，供 crash_smp_send_stop() 在紧急停机时使用 */
	ldrb	w4, [x19, #SDEI_EVENT_PRIORITY]
	cbnz	w4, 1f
	adr_this_cpu dst=x5, sym=sdei_active_normal_event, tmp=x6
	b	2f
1:	adr_this_cpu dst=x5, sym=sdei_active_critical_event, tmp=x6
2:	str	x19, [x5]

	/*
	 * entry.S may have been using sp as a scratch register, find whether
	 * this is a normal or critical event and switch to the appropriate
	 * stack for this CPU.
	 */
	cbnz	w4, 1f
	ldr_this_cpu dst=x5, sym=sdei_stack_normal_ptr, tmp=x6
	b	2f
1:	ldr_this_cpu dst=x5, sym=sdei_stack_critical_ptr, tmp=x6
2:	mov	x6, #SDEI_STACK_SIZE
	add	x5, x5, x6
	mov	sp, x5

#ifdef CONFIG_SHADOW_CALL_STACK
	/* 普通事件和紧急事件使用各自的影子调用栈 */
	cbnz	w4, 3f
	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_normal_ptr, tmp=x6
	b	4f
3:	ldr_this_cpu dst=scs_sp, sym=sdei_shadow_call_stack_critical_ptr, tmp=x6
4:
#endif

	/*
	 * We may have interrupted userspace, or a guest, or exit-from or
	 * return-to either of these. We can't trust sp_el0, restore it.
	 */
	mrs	x28, sp_el0
	ldr_this_cpu	dst=x0, sym=__entry_task, tmp=x1
	msr	sp_el0, x0

	/* If we interrupted the kernel point to the previous stack/frame. */
	and     x0, x3, #0xc
	mrs     x1, CurrentEL
	cmp     x0, x1
	csel	x29, x29, xzr, eq	// fp, or zero
	csel	x4, x2, xzr, eq		// elr, or zero

	stp	x29, x4, [sp, #-16]!
	mov	x29, sp

	add	x0, x19, #SDEI_EVENT_INTREGS
	mov	x1, x19
	bl	__sdei_handler

	msr	sp_el0, x28
	/* restore regs >x17 that we clobbered */
	mov	x4, x19         // keep x4 for __sdei_asm_exit_trampoline
	ldp	x28, x29, [x4, #SDEI_EVENT_INTREGS + 16 * 14]
	ldp	x18, x19, [x4, #SDEI_EVENT_INTREGS + 16 * 9]
	ldp	lr, x1, [x4, #SDEI_EVENT_INTREGS + S_LR]
	mov	sp, x1

	mov	x1, x0			// address to complete_and_resume
	/* x0 = (x0 <= SDEI_EV_FAILED) ?
	 * EVENT_COMPLETE:EVENT_COMPLETE_AND_RESUME
	 */
	cmp	x0, #SDEI_EV_FAILED
	mov_q	x2, SDEI_1_0_FN_SDEI_EVENT_COMPLETE
	mov_q	x3, SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME
	csel	x0, x2, x3, ls

	ldr_l	x2, sdei_exit_mode

	/* 清除 crash_smp_send_stop() 可见的已注册事件标记 */
	ldrb	w3, [x4, #SDEI_EVENT_PRIORITY]
	cbnz	w3, 1f
	adr_this_cpu dst=x5, sym=sdei_active_normal_event, tmp=x6
	b	2f
1:	adr_this_cpu dst=x5, sym=sdei_active_critical_event, tmp=x6
2:	str	xzr, [x5]

alternative_if_not ARM64_UNMAP_KERNEL_AT_EL0
	sdei_handler_exit exit_mode=x2
alternative_else_nop_endif

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	tramp_alias	dst=x5, sym=__sdei_asm_exit_trampoline
	br	x5
#endif
SYM_CODE_END(__sdei_asm_handler)
NOKPROBE(__sdei_asm_handler)

SYM_CODE_START(__sdei_handler_abort)
	mov_q	x0, SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME
	adr	x1, 1f
	ldr_l	x2, sdei_exit_mode
	sdei_handler_exit exit_mode=x2
	// exit the handler and jump to the next instruction.
	// Exit will stomp x0-x17, PSTATE, ELR_ELx, and SPSR_ELx.
1:	ret
SYM_CODE_END(__sdei_handler_abort)
NOKPROBE(__sdei_handler_abort)
#endif /* CONFIG_ARM_SDE_INTERFACE */
