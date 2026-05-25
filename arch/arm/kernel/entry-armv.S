/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/kernel/entry-armv.S
 *
 *  Copyright (C) 1996,1997,1998 Russell King.
 *  ARM700 fix by Matthew Godbolt (linux-user@willothewisp.demon.co.uk)
 *  nommu support by Hyok S. Choi (hyok.choi@samsung.com)
 *
 *  Low-level vector interface routines
 *
 * 概述: entry-armv.S -- ARM32异常向量底层处理代码
 * 本文件实现ARM架构的异常向量分发和处理机制。
 * 当CPU发生异常(中断、数据/指令中止、未定义指令、FIQ等)时,
 * 硬件自动跳转到异常向量表,通过vector_stub框架分发:
 *   - 用户态(USR模式)发起的异常 -> __xxx_usr 处理路径
 *   - 内核态(SVC模式)发起的异常 -> __xxx_svc 处理路径
 * 同时还包含:
 *   - 进程上下文切换(__switch_to)
 *   - KUSER帮助函数(用户空间原子操作辅助)
 *   - VMAP栈溢出处理(__bad_stack)
 *   - FIQ作为NMI的处理支持
 *
 *  Note:  there is a StrongARM bug in the STMIA rn, {regs}^ instruction
 *  that causes it to save wrong values...  Be aware!
 */

#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/page.h>
#include <asm/glue-df.h>
#include <asm/glue-pf.h>
#include <asm/vfpmacros.h>
#include <asm/thread_notify.h>
#include <asm/unwind.h>
#include <asm/unistd.h>
#include <asm/tls.h>
#include <asm/system_info.h>
#include <asm/uaccess-asm.h>
#include <asm/kasan_def.h>

#include "entry-header.S"
#include <asm/probes.h>

#ifdef CONFIG_HAVE_LD_DEAD_CODE_DATA_ELIMINATION
#define RELOC_TEXT_NONE .reloc  .text, R_ARM_NONE, .
#else
#define RELOC_TEXT_NONE
#endif

/*
 * irq_handler: 中断处理宏
 * @from_user: 是否来自用户态(1=用户态, 0=内核态)
 *
 * 内核态中断时,检查当前是否已在IRQ栈上。
 * 如果是,则直接在当前栈上处理(避免栈溢出);
 * 否则切换到每CPU的IRQ栈。
 */
	.macro	irq_handler, from_user:req
	mov	r1, sp				@ 保存当前栈指针
	ldr_this_cpu r2, irq_stack_ptr, r2, r3	@ 加载每CPU IRQ栈指针
	.if	\from_user == 0
	@
	@ If we took the interrupt while running in the kernel, we may already
	@ be using the IRQ stack, so revert to the original value in that case.
	@ 如果在内核中收到中断,可能已经在使用IRQ栈,
	@ 需要检查是否已在IRQ栈上,避免重复切换。
	@
	subs	r3, r2, r1		@ SP above bottom of IRQ stack?
	rsbscs	r3, r3, #THREAD_SIZE	@ ... and below the top?
#ifdef CONFIG_VMAP_STACK
	ldr_va	r3, high_memory, cc	@ End of the linear region
	cmpcc	r3, r1			@ Stack pointer was below it?
#endif
	bcc	0f			@ If not, switch to the IRQ stack
	mov	r0, r1			@ 已在IRQ栈上,直接处理
	bl	generic_handle_arch_irq
	b	1f
0:
	.endif

	mov_l	r0, generic_handle_arch_irq
	bl	call_with_stack		@ 切换到IRQ栈并处理中断
1:
	.endm

/*
 * pabt_helper: 指令预取中止(Prefectch Abort)处理辅助宏
 * 调用处理器相关的预取中止处理函数。
 * 需要: r2=pt_regs, r4=出错地址, r5=出错时的PSR
 */
	.macro	pabt_helper
	@ PABORT handler takes pt_regs in r2, fault address in r4 and psr in r5
#ifdef MULTI_PABORT
	ldr_va	ip, processor, offset=PROCESSOR_PABT_FUNC
	bl_r	ip
#else
	bl	CPU_PABORT_HANDLER
#endif
	.endm

/*
 * dabt_helper: 数据中止(Data Abort)处理辅助宏
 * 调用处理器相关的数据中止处理函数。
 * 需要: r2=pt_regs, r4=出错地址, r5=出错时的PSR
 * 返回: r0=出错地址, r1=错误状态寄存器
 */
	.macro	dabt_helper

	@
	@ Call the processor-specific abort handler:
	@
	@  r2 - pt_regs
	@  r4 - aborted context pc
	@  r5 - aborted context psr
	@
	@ The abort handler must return the aborted address in r0, and
	@ the fault status register in r1.  r9 must be preserved.
	@
#ifdef MULTI_DABORT
	ldr_va	ip, processor, offset=PROCESSOR_DABT_FUNC
	bl_r	ip
#else
	bl	CPU_DABORT_HANDLER
#endif
	.endm

	.section	.entry.text,"ax",%progbits

/*
 * 非法模式处理 - Invalid mode handlers
 * 当异常来自不期望的处理器模式时,使用这些处理程序。
 * 保存寄存器到栈上,然后跳转到common_invalid进行统一处理。
 */
	.macro	inv_entry, reason
	sub	sp, sp, #PT_REGS_SIZE		@ 在栈上分配pt_regs空间
 ARM(	stmib	sp, {r1 - lr}		)	@ ARM: 先增后存,保存r1-lr
 THUMB(	stmia	sp, {r0 - r12}		)	@ Thumb: 保存r0-r12
 THUMB(	str	sp, [sp, #S_SP]		)	@ Thumb: 保存SP
 THUMB(	str	lr, [sp, #S_LR]		)	@ Thumb: 保存LR
	mov	r1, #\reason			@ 设置错误原因码
	.endm

__pabt_invalid:
	inv_entry BAD_PREFETCH			@ 非法预取中止: 原因=BAD_PREFETCH
	b	common_invalid
ENDPROC(__pabt_invalid)

__dabt_invalid:
	inv_entry BAD_DATA			@ 非法数据中止: 原因=BAD_DATA
	b	common_invalid
ENDPROC(__dabt_invalid)

__irq_invalid:
	inv_entry BAD_IRQ			@ 非法IRQ: 原因=BAD_IRQ
	b	common_invalid
ENDPROC(__irq_invalid)

__und_invalid:
	inv_entry BAD_UNDEFINSTR		@ 非法未定义指令: 原因=BAD_UNDEFINSTR
	@ fall through to common_invalid

@
@ common_invalid - generic code for failed exception (re-entrant version of handlers)
@ common_invalid: 所有非法异常的统一处理入口,构造pt_regs后调用bad_mode
@
common_invalid:
	zero_fp

	ldmia	r0, {r4 - r6}			@ 从模式栈加载r0, lr, spsr
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r7, #-1			@  ""   ""    ""        ""
	str	r4, [sp]		@ save preserved r0 (保存原始r0)
	stmia	r0, {r5 - r7}		@ lr_<exception>,
					@ cpsr_<exception>, "old_r0"
					@ 保存异常LR、CPSR和old_r0

	mov	r0, sp
	b	bad_mode			@ 调用C语言的bad_mode处理
ENDPROC(__und_invalid)

/*
 * SVC模式处理程序 - SVC mode handlers
 * 处理发生在内核态(SVC模式)的异常。
 * svc_entry宏负责保存内核寄存器上下文到栈上。
 */

#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5)
#define SPFIX(code...) code
#else
#define SPFIX(code...)
#endif

/*
 * svc_entry: SVC模式异常入口宏
 * @stack_hole: 额外栈空间(用于kprobes等)
 * @trace: 是否追踪irq标志
 * @uaccess: 是否处理用户空间访问状态
 * @overflow_check: 是否检查栈溢出
 *
 * 保存发生异常时的内核寄存器状态到栈上,构造pt_regs结构。
 */
	.macro	svc_entry, stack_hole=0, trace=1, uaccess=1, overflow_check=1
 UNWIND(.fnstart		)
	sub	sp, sp, #(SVC_REGS_SIZE + \stack_hole)	@ 分配栈空间
 THUMB(	add	sp, r1		)	@ get SP in a GPR without
 THUMB(	sub	r1, sp, r1	)	@ using a temp register

	.if	\overflow_check
 UNWIND(.save	{r0 - pc}	)
	do_overflow_check (SVC_REGS_SIZE + \stack_hole)	@ 检查内核栈溢出
	.endif

#ifdef CONFIG_THUMB2_KERNEL
	tst	r1, #4			@ test stack pointer alignment (检查栈指针对齐)
	sub	r1, sp, r1		@ restore original R1 (恢复原始R1)
	sub	sp, r1			@ restore original SP (恢复原始SP)
#else
 SPFIX(	tst	sp, #4		)	@ 检查SP是否8字节对齐(EABI要求)
#endif
 SPFIX(	subne	sp, sp, #4	)	@ 不对齐则调整,使SP 8字节对齐

 ARM(	stmib	sp, {r1 - r12}	)	@ ARM: 先增后存, 保存r1-r12
 THUMB(	stmia	sp, {r0 - r12}	)	@ No STMIB in Thumb-2

	ldmia	r0, {r3 - r5}		@ 从模式栈加载r0, lr, spsr
	add	r7, sp, #S_SP		@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""      ""       ""
	add	r2, sp, #(SVC_REGS_SIZE + \stack_hole)	@ 计算原始SP值
 SPFIX(	addne	r2, r2, #4	)	@ 如果之前调整过SP,需要补偿
	str	r3, [sp]		@ save the "real" r0 copied
					@ from the exception stack
					@ 保存真正的r0(来自异常栈)

	mov	r3, lr			@ lr_svc

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@ 现在填充栈上的剩余字段:
	@  r2 - sp_svc (SVC模式栈指针)
	@  r3 - lr_svc (SVC模式返回地址)
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception> (异常发生时的CPSR)
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h) (原始r0)
	@
	stmia	r7, {r2 - r6}		@ 保存SP、LR、异常LR、SPSR、old_r0

	get_thread_info tsk		@ 获取当前线程信息
	uaccess_entry tsk, r0, r1, r2, \uaccess	@ 保存/恢复用户空间访问状态

	.if \trace
#ifdef CONFIG_TRACE_IRQFLAGS
	bl	trace_hardirqs_off		@ 追踪IRQ关闭事件
#endif
	.endif
	.endm

	.align	5
/*
 * __dabt_svc: 内核态数据中止处理
 * 保存上下文后,调用dabt_helper获取错误信息,然后svc_exit返回。
 */
__dabt_svc:
	svc_entry uaccess=0			@ 保存内核寄存器(关闭uaccess追踪)
	mov	r2, sp				@ pt_regs指针
	dabt_helper				@ 调用数据中止处理函数
 THUMB(	ldr	r5, [sp, #S_PSR]	)	@ potentially updated CPSR
	svc_exit r5				@ return from exception (返回)
 UNWIND(.fnend		)
ENDPROC(__dabt_svc)

	.align	5
/*
 * __irq_svc: 内核态IRQ中断处理
 * 保存上下文后调用generic_handle_arch_irq处理中断。
 * 如果启用了抢占,在中断返回前检查是否需要抢占调度。
 */
__irq_svc:
	svc_entry				@ 保存内核寄存器
	irq_handler from_user=0			@ 处理中断(来自内核态)

#ifdef CONFIG_PREEMPTION
	ldr	r8, [tsk, #TI_PREEMPT]		@ get preempt count (获取抢占计数)
	ldr	r0, [tsk, #TI_FLAGS]		@ get flags (获取线程标志)
	teq	r8, #0				@ if preempt count != 0
	movne	r0, #0				@ force flags to 0 (不可抢占)
	tst	r0, #_TIF_NEED_RESCHED		@ 需要重新调度?
	blne	svc_preempt			@ 是, 执行抢占调度
#endif

	svc_exit r5, irq = 1			@ return from exception
 UNWIND(.fnend		)
ENDPROC(__irq_svc)

	.ltorg

#ifdef CONFIG_PREEMPTION
/*
 * svc_preempt: 内核态抢占调度
 * 调用preempt_schedule_irq执行抢占调度。
 * 返回后检查是否再次需要调度,循环直到不需要为止。
 */
svc_preempt:
	mov	r8, lr
1:	bl	preempt_schedule_irq		@ irq en/disable is done inside
	ldr	r0, [tsk, #TI_FLAGS]		@ get new tasks TI_FLAGS
	tst	r0, #_TIF_NEED_RESCHED
	reteq	r8				@ go again (不需调度则返回)
	b	1b				@ 仍需调度,继续
#endif

/*
 * __und_fault: 未定义指令处理辅助函数
 * 修正PC指针,使其指向真正的故障指令。
 * ARM指令: PC已指向下一条指令,需减4
 * Thumb指令: PC指向指令后半部分,需减2
 * r1中存放了修正值(4或2), r0=pt_regs
 */
__und_fault:
	@ Correct the PC such that it is pointing at the instruction
	@ which caused the fault.  If the faulting instruction was ARM
	@ the PC will be pointing at the next instruction, and have to
	@ subtract 4.  Otherwise, it is Thumb, and the PC will be
	@ pointing at the second half of the Thumb instruction.  We
	@ have to subtract 2.
	ldr	r2, [r0, #S_PC]		@ 加载保存的PC
	sub	r2, r2, r1			@ 减去指令修正值
	str	r2, [r0, #S_PC]		@ 写回修正后的PC
	b	do_undefinstr			@ 调用C语言处理函数
ENDPROC(__und_fault)

	.align	5
/*
 * __und_svc: 内核态未定义指令异常处理
 * 如果kprobes启用了,需要预留额外栈空间用于模拟指令。
 */
__und_svc:
#ifdef CONFIG_KPROBES
	@ If a kprobe is about to simulate a "stmdb sp..." instruction,
	@ it obviously needs free stack space which then will belong to
	@ the saved context.
	svc_entry MAX_STACK_SIZE		@ 为kprobe预留额外栈空间
#else
	svc_entry
#endif

	mov	r1, #4				@ PC correction to apply (ARM指令修正值=4)
 THUMB(	tst	r5, #PSR_T_BIT		)	@ exception taken in Thumb mode?
 THUMB(	movne	r1, #2			)	@ if so, fix up PC correction (Thumb修正值=2)
	mov	r0, sp				@ struct pt_regs *regs
	bl	__und_fault			@ 调用未定义指令处理

__und_svc_finish:
	get_thread_info tsk
	ldr	r5, [sp, #S_PSR]		@ Get SVC cpsr
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__und_svc)

	.align	5
/*
 * __pabt_svc: 内核态预取中止处理
 */
__pabt_svc:
	svc_entry
	mov	r2, sp				@ regs
	pabt_helper				@ 调用预取中止处理
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__pabt_svc)

	.align	5
/*
 * __fiq_svc: 内核态FIQ快速中断处理(作为NMI)
 * 使用SVC栈处理FIQ,使其行为类似于x86的NMI。
 */
__fiq_svc:
	svc_entry trace=0			@ 保存上下文,不追踪irq
	mov	r0, sp				@ struct pt_regs *regs
	bl	handle_fiq_as_nmi		@ 作为NMI处理FIQ
	svc_exit_via_fiq			@ 通过FIQ路径返回
 UNWIND(.fnend		)
ENDPROC(__fiq_svc)

/*
 * Abort模式FIQ处理 - FIQ in Abort mode
 * 在Abort模式下收到FIQ时,需要额外保存/恢复lr_abt和spsr_abt,
 * 以支持嵌套的abort处理。
 */
	.align 5
__fiq_abt:
	svc_entry trace=0

 ARM(	msr	cpsr_c, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )	@ 切换到Abort模式
 THUMB( mov	r0, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	mov	r1, lr		@ Save lr_abt (保存Abort模式返回地址)
	mrs	r2, spsr	@ Save spsr_abt, abort is now safe (保存Abort模式SPSR)
 ARM(	msr	cpsr_c, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )	@ 切回SVC模式
 THUMB( mov	r0, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	stmfd	sp!, {r1 - r2}			@ 保存lr_abt和spsr_abt到SVC栈

	add	r0, sp, #8			@ struct pt_regs *regs
	bl	handle_fiq_as_nmi		@ 作为NMI处理FIQ

	ldmfd	sp!, {r1 - r2}			@ 恢复lr_abt和spsr_abt
 ARM(	msr	cpsr_c, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )	@ 切回Abort模式
 THUMB( mov	r0, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	mov	lr, r1		@ Restore lr_abt, abort is unsafe
	msr	spsr_cxsf, r2	@ Restore spsr_abt
 ARM(	msr	cpsr_c, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )	@ 切回SVC模式
 THUMB( mov	r0, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )

	svc_exit_via_fiq			@ 通过FIQ路径返回
 UNWIND(.fnend		)
ENDPROC(__fiq_abt)

/*
 * 用户态异常处理程序 - User mode handlers
 * 处理来自用户态(USR模式)的异常。
 * usr_entry宏负责保存用户寄存器上下文到内核栈。
 * EABI要求sp_svc始终保持64位对齐。
 */

#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5) && (PT_REGS_SIZE & 7)
#error "sizeof(struct pt_regs) must be a multiple of 8"
#endif

/*
 * usr_entry: 用户态异常入口宏
 * @trace: 是否追踪irq标志
 * @uaccess: 是否关闭用户空间访问
 *
 * 保存用户态寄存器到内核栈,构造pt_regs结构。
 * 关闭用户空间访问(设置域或PAN),启用对齐陷阱。
 */
	.macro	usr_entry, trace=1, uaccess=1
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)	@ don't unwind the user space (用户空间不可unwind)
	sub	sp, sp, #PT_REGS_SIZE		@ 分配pt_regs空间
 ARM(	stmib	sp, {r1 - r12}	)		@ 保存r1-r12
 THUMB(	stmia	sp, {r0 - r12}	)		@ Thumb: 保存r0-r12

 ATRAP(	mrc	p15, 0, r7, c1, c0, 0)		@ 读取SCTLR(对齐陷阱状态)
 ATRAP(	ldr_va	r8, cr_alignment)		@ 加载预期的对齐陷阱值

	ldmia	r0, {r3 - r5}			@ 从异常模式栈加载r0, lr, spsr
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""     ""        ""

	str	r3, [sp]		@ save the "real" r0 copied
					@ from the exception stack
					@ 保存真正的r0

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@ 现在填充栈上的剩余字段:
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception>
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h)
	@
	@ Also, separately save sp_usr and lr_usr
	@ 同时单独保存用户态的SP和LR
	@
	stmia	r0, {r4 - r6}			@ 保存异常LR、SPSR、old_r0
 ARM(	stmdb	r0, {sp, lr}^			)	@ 保存用户SP和LR(使用^后缀,用户态寄存器)
 THUMB(	store_user_sp_lr r0, r1, S_SP - S_PC	)	@ Thumb: 保存用户SP和LR

	.if \uaccess
	uaccess_disable ip			@ 关闭用户空间内存访问
	.endif

	@ Enable the alignment trap while in kernel mode
	@ 在内核模式下启用对齐陷阱
 ATRAP(	teq	r8, r7)				@ 当前对齐陷阱状态是否匹配?
 ATRAP( mcrne	p15, 0, r8, c1, c0, 0)		@ 不匹配则写入正确值

	reload_current r7, r8			@ 重新加载current指针

	@
	@ Clear FP to mark the first stack frame
	@ 清零帧指针,标记栈帧起始
	@
	zero_fp

	.if	\trace
#ifdef CONFIG_TRACE_IRQFLAGS
	bl	trace_hardirqs_off		@ 追踪IRQ关闭事件
#endif
	ct_user_exit save = 0			@ 标记退出用户态
	.endif
	.endm

/*
 * kuser_cmpxchg_check: 检查用户态cmpxchg原子操作是否需要修复
 * 如果用户态的原子比较交换操作在临界区被中断,
 * 需要修正PC使其重新执行。
 */
	.macro	kuser_cmpxchg_check
#if !defined(CONFIG_CPU_32v6K) && defined(CONFIG_KUSER_HELPERS)
#ifndef CONFIG_MMU
#warning "NPTL on non MMU needs fixing"
#else
	@ Make sure our user space atomic helper is restarted
	@ if it was interrupted in a critical region.  Here we
	@ perform a quick test inline since it should be false
	@ 99.9999% of the time.  The rest is done out of line.
	ldr	r0, =TASK_SIZE
	cmp	r4, r0
	blhs	kuser_cmpxchg64_fixup		@ 如果PC在用户空间,检查是否需要修复
#endif
#endif
	.endm

	.align	5
/*
 * __dabt_usr: 用户态数据中止处理
 */
__dabt_usr:
	usr_entry uaccess=0			@ 保存用户寄存器
	kuser_cmpxchg_check			@ 检查cmpxchg修复
	mov	r2, sp
	dabt_helper				@ 调用数据中止处理
	b	ret_from_exception		@ 从中止返回
 UNWIND(.fnend		)
ENDPROC(__dabt_usr)

	.align	5
/*
 * __irq_usr: 用户态IRQ中断处理
 */
__irq_usr:
	usr_entry
	kuser_cmpxchg_check
	irq_handler from_user=1			@ 处理中断(来自用户态)
	get_thread_info tsk
	mov	why, #0
	b	ret_to_user_from_irq		@ 返回用户态
 UNWIND(.fnend		)
ENDPROC(__irq_usr)

	.ltorg

	.align	5
/*
 * __und_usr: 用户态未定义指令异常处理
 * 需要先使能IRQ,因为读取用户指令可能触发页错误。
 * 检查Thumb/ARM模式,正确修正PC值后调用__und_fault。
 */
__und_usr:
	usr_entry uaccess=0

	@ IRQs must be enabled before attempting to read the instruction from
	@ user space since that could cause a page/translation fault if the
	@ page table was modified by another CPU.
	@ 读取用户空间指令前必须先开IRQ,因为其他CPU修改页表后
	@ 可能触发页/转换错误。
	enable_irq

	tst	r5, #PSR_T_BIT			@ Thumb mode?
	mov	r1, #2				@ set insn size to 2 for Thumb
	bne	0f				@ handle as Thumb undef exception
#ifdef CONFIG_FPE_NWFPE
	adr	r9, ret_from_exception
	bl	call_fpe			@ returns via R9 on success (FPE仿真)
#endif
	mov	r1, #4				@ set insn size to 4 for ARM
0:	mov	r0, sp
	uaccess_disable ip			@ 关闭用户空间访问
	bl	__und_fault
	b	ret_from_exception
 UNWIND(.fnend)
ENDPROC(__und_usr)

	.align	5
/*
 * __pabt_usr: 用户态预取中止处理
 * 处理完后fall through到ret_from_exception返回用户态。
 */
__pabt_usr:
	usr_entry
	mov	r2, sp				@ regs
	pabt_helper
 UNWIND(.fnend		)
	/* fall through */
/*
 * ret_from_exception: 从中止异常返回用户态的公共入口
 */
ENTRY(ret_from_exception)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	get_thread_info tsk
	mov	why, #0
	b	ret_to_user
 UNWIND(.fnend		)
ENDPROC(__pabt_usr)
ENDPROC(ret_from_exception)

	.align	5
/*
 * __fiq_usr: 用户态FIQ快速中断处理(作为NMI)
 */
__fiq_usr:
	usr_entry trace=0
	kuser_cmpxchg_check
	mov	r0, sp				@ struct pt_regs *regs
	bl	handle_fiq_as_nmi
	get_thread_info tsk
	restore_user_regs fast = 0, offset = 0
 UNWIND(.fnend		)
ENDPROC(__fiq_usr)

/*
 * __switch_to: 进程上下文切换
 * 保存当前进程的寄存器,恢复下一个进程的寄存器。
 * 这是ARM架构中进程调度的核心汇编代码。
 *
 * 输入:
 *   r0 = previous task_struct (前一个进程的task_struct)
 *   r1 = previous thread_info (前一个进程的thread_info)
 *   r2 = next thread_info (下一个进程的thread_info)
 * 前后进程保证不是同一个。
 */
ENTRY(__switch_to)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	add	ip, r1, #TI_CPU_SAVE		@ 指向previous的CPU保存区
 ARM(	stmia	ip!, {r4 - sl, fp, sp, lr} )	@ Store most regs on stack (保存r4-lr到previous)
 THUMB(	stmia	ip!, {r4 - sl, fp}	   )	@ Store most regs on stack
 THUMB(	str	sp, [ip], #4		   )	@ Thumb: 单独保存SP
 THUMB(	str	lr, [ip], #4		   )	@ Thumb: 单独保存LR
	ldr	r4, [r2, #TI_TP_VALUE]		@ 加载next的TLS值(低32位)
	ldr	r5, [r2, #TI_TP_VALUE + 4]	@ 加载next的TLS值(高32位)
#ifdef CONFIG_CPU_USE_DOMAINS
	mrc	p15, 0, r6, c3, c0, 0		@ Get domain register (读取域寄存器)
	str	r6, [r1, #TI_CPU_DOMAIN]	@ Save old domain register (保存旧域)
	ldr	r6, [r2, #TI_CPU_DOMAIN]	@ 加载next的域寄存器值
#endif
	switch_tls r1, r4, r5, r3, r7		@ 切换TLS(线程本地存储)寄存器
#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_SMP) && \
    !defined(CONFIG_STACKPROTECTOR_PER_TASK)
	ldr	r8, =__stack_chk_guard		@ 加载栈保护canary地址
	.if (TSK_STACK_CANARY > IMM12_MASK)
	add	r9, r2, #TSK_STACK_CANARY & ~IMM12_MASK
	ldr	r9, [r9, #TSK_STACK_CANARY & IMM12_MASK]
	.else
	ldr	r9, [r2, #TSK_STACK_CANARY & IMM12_MASK]	@ 加载next的canary值
	.endif
#endif
	mov	r7, r2				@ Preserve 'next' (保存next指针)
#ifdef CONFIG_CPU_USE_DOMAINS
	mcr	p15, 0, r6, c3, c0, 0		@ Set domain register (设置next的域)
#endif
	mov	r5, r0
	add	r4, r2, #TI_CPU_SAVE		@ 指向next的CPU保存区
	ldr	r0, =thread_notify_head
	mov	r1, #THREAD_NOTIFY_SWITCH
	bl	atomic_notifier_call_chain	@ 通知线程切换事件
#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_SMP) && \
    !defined(CONFIG_STACKPROTECTOR_PER_TASK)
	str	r9, [r8]			@ 更新栈保护canary
#endif
	mov	r0, r5
#if !defined(CONFIG_THUMB2_KERNEL) && !defined(CONFIG_VMAP_STACK)
	set_current r7, r8			@ 设置current指针
	ldmia	r4, {r4 - sl, fp, sp, pc}	@ Load all regs saved previously (恢复next的所有寄存器)
#else
	mov	r1, r7
	ldmia	r4, {r4 - sl, fp, ip, lr}	@ 恢复next的寄存器(不含SP和PC)
#ifdef CONFIG_VMAP_STACK
	@
	@ Do a dummy read from the new stack while running from the old one so
	@ that we can rely on do_translation_fault() to fix up any stale PMD
	@ entries covering the vmalloc region.
	@ 在旧栈上对目标栈做一次哑读,触发do_translation_fault()修正
	@ vmalloc区域可能过时的PMD条目。
	@
	ldr	r2, [ip]			@ 哑读新栈的地址
#ifdef CONFIG_KASAN_VMALLOC
	@ Also dummy read from the KASAN shadow memory for the new stack if we
	@ are using KASAN
	mov_l	r2, KASAN_SHADOW_OFFSET
	add	r2, r2, ip, lsr #KASAN_SHADOW_SCALE_SHIFT
	ldr	r2, [r2]			@ 哑读KASAN shadow内存
#endif
#endif

	@ When CONFIG_THREAD_INFO_IN_TASK=n, the update of SP itself is what
	@ effectuates the task switch, as that is what causes the observable
	@ values of current and current_thread_info to change. When
	@ CONFIG_THREAD_INFO_IN_TASK=y, setting current (and therefore
	@ current_thread_info) is done explicitly, and the update of SP just
	@ switches us to another stack, with few other side effects. In order
	@ to prevent this distinction from causing any inconsistencies, let's
	@ keep the 'set_current' call as close as we can to the update of SP.
	set_current r1, r2			@ 设置current指针
	mov	sp, ip				@ 切换栈指针(完成真正的上下文切换)
	ret	lr				@ 返回到next进程的LR地址
#endif
 UNWIND(.fnend		)
ENDPROC(__switch_to)

#ifdef CONFIG_VMAP_STACK
	.text
	.align	2
/*
 * __bad_stack: 内核栈溢出处理
 * 检测到内核栈溢出后,切换到每CPU的overflow栈,
 * 保存溢出的上下文,调用handle_bad_stack。
 * 由于只有少量寄存器可用,使用PC相对寻址加载overflow栈地址。
 */
__bad_stack:
	@
	@ We've just detected an overflow. We need to load the address of this
	@ CPU's overflow stack into the stack pointer register. We have only one
	@ scratch register so let's use a sequence of ADDs including one
	@ involving the PC, and decorate them with PC-relative group
	@ relocations. As these are ARM only, switch to ARM mode first.
	@
	@ We enter here with IP clobbered and its value stashed on the mode
	@ stack.
	@
THUMB(	bx	pc		)	@ Thumb->ARM模式切换
THUMB(	nop			)
THUMB(	.arm			)
	ldr_this_cpu_armv6 ip, overflow_stack_ptr	@ 加载overflow栈指针

	str	sp, [ip, #-4]!			@ Preserve original SP value (保存原SP)
	mov	sp, ip				@ Switch to overflow stack (切换到溢出栈)
	pop	{ip}				@ Original SP in IP (原SP值存入IP)

#if defined(CONFIG_UNWINDER_FRAME_POINTER) && defined(CONFIG_CC_IS_GCC)
	mov	ip, ip				@ mov expected by unwinder
	push	{fp, ip, lr, pc}		@ GCC flavor frame record (GCC格式帧记录)
#else
	str	ip, [sp, #-8]!			@ store original SP (保存原SP)
	push	{fpreg, lr}			@ Clang flavor frame record (Clang格式帧记录)
#endif
UNWIND( ldr	ip, [r0, #4]	)		@ load exception LR (加载异常LR)
UNWIND( str	ip, [sp, #12]	)		@ store in the frame record (存入帧记录)
	ldr	ip, [r0, #12]			@ reload IP (重新加载IP)

	@ Store the original GPRs to the new stack.
	svc_entry uaccess=0, overflow_check=0	@ 在overflow栈上保存寄存器

UNWIND( .save   {sp, pc}	)
UNWIND( .save   {fpreg, lr}	)
UNWIND( .setfp  fpreg, sp	)

	ldr	fpreg, [sp, #S_SP]		@ Add our frame record
						@ to the linked list
						@ 将帧记录链接到链表
#if defined(CONFIG_UNWINDER_FRAME_POINTER) && defined(CONFIG_CC_IS_GCC)
	ldr	r1, [fp, #4]			@ reload SP at entry
	add	fp, fp, #12
#else
	ldr	r1, [fpreg, #8]
#endif
	str	r1, [sp, #S_SP]			@ store in pt_regs (存入pt_regs)

	@ Stash the regs for handle_bad_stack
	mov	r0, sp

	@ Time to die (调用处理函数,系统即将崩溃)
	bl	handle_bad_stack
	nop
UNWIND( .fnend			)
ENDPROC(__bad_stack)
#endif

	__INIT

/*
 * 用户空间辅助函数 - User helpers
 * 每个段32字节对齐,将被移动到高向量页的顶部。
 * 这些是内核为用户空间提供的小型辅助函数,
 * 用于实现高效的原子操作、内存屏障、TLS读取等。
 * 参见 Documentation/arch/arm/kernel_user_helpers.rst
 */
 THUMB(	.arm	)

	.macro	usr_ret, reg
#ifdef CONFIG_ARM_THUMB
	bx	\reg			@ Thumb: 分支并切换模式
#else
	ret	\reg			@ ARM: 返回
#endif
	.endm

/*
 * kuser_pad: 用undefined instruction填充到指定大小
 * 每个kuser slot必须恰好32字节(或64字节等)。
 */
	.macro	kuser_pad, sym, size
	.if	(. - \sym) & 3
	.rept	4 - (. - \sym) & 3
	.byte	0
	.endr
	.endif
	.rept	(\size - (. - \sym)) / 4
	.word	0xe7fddef1		@ undefined instruction (undefined指令填充)
	.endr
	.endm

#ifdef CONFIG_KUSER_HELPERS
	.align	5
	.globl	__kuser_helper_start
__kuser_helper_start:

/*
 * Due to the length of some sequences, __kuser_cmpxchg64 spans 2 regular
 * kuser "slots", therefore 0xffff0f80 is not used as a valid entry point.
 * 某些序列较长,__kuser_cmpxchg64占用2个slot,所以0xffff0f80不是有效入口。
 */

/*
 * __kuser_cmpxchg64: 64位原子比较交换 (地址 0xffff0f60)
 * 用于用户空间实现64位原子操作。
 * 在ARMv6K及以上使用LDREXD/STREXD指令;
 * 在UP系统上使用禁中断+普通load/store。
 */
__kuser_cmpxchg64:				@ 0xffff0f60

#if defined(CONFIG_CPU_32v6K)

	stmfd	sp!, {r4, r5, r6, r7}
	ldrd	r4, r5, [r0]			@ load old val (加载旧值)
	ldrd	r6, r7, [r1]			@ load new val (加载新值)
	smp_dmb	arm
1:	ldrexd	r0, r1, [r2]			@ load current val (加载当前值)
	eors	r3, r0, r4			@ compare with oldval (1)
	eorseq	r3, r1, r5			@ compare with oldval (2)
	strexdeq r3, r6, r7, [r2]		@ store newval if eq (相等则存储)
	teqeq	r3, #1				@ success? (成功?)
	beq	1b				@ if no then retry (失败则重试)
	smp_dmb	arm
	rsbs	r0, r3, #0			@ set returned val and C flag
	ldmfd	sp!, {r4, r5, r6, r7}
	usr_ret	lr

#elif !defined(CONFIG_SMP)

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg64
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle of
	 * the critical sequence.  The same strategy as for cmpxchg is used.
	 * 唯一可能破坏原子性的是IRQ或数据中止导致调度。
	 * 异常处理程序会检查是否在临界区内,如果是则重置PC。
	 */
	stmfd	sp!, {r4, r5, r6, lr}
	ldmia	r0, {r4, r5}			@ load old val
	ldmia	r1, {r6, lr}			@ load new val
1:	ldmia	r2, {r0, r1}			@ load current val
	eors	r3, r0, r4			@ compare with oldval (1)
	eorseq	r3, r1, r5			@ compare with oldval (2)
2:	stmiaeq	r2, {r6, lr}			@ store newval if eq
	rsbs	r0, r3, #0			@ set return val and C flag
	ldmfd	sp!, {r4, r5, r6, pc}

	.text
/*
 * kuser_cmpxchg64_fixup: cmpxchg64中断修复函数
 * 如果PC在临界区(1b-2b之间),将保存的PC重置为1b。
 */
kuser_cmpxchg64_fixup:
	@ Called from kuser_cmpxchg_fixup.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0f60 + (1b - __kuser_cmpxchg64)))
	subs	r8, r4, r7
	rsbscs	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]			@ 修正PC到临界区起始
#if __LINUX_ARM_ARCH__ < 6
	bcc	kuser_cmpxchg32_fixup		@ 也需要检查32位cmpxchg
#endif
	ret	lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else
#error "incoherent kernel configuration"
#endif

	kuser_pad __kuser_cmpxchg64, 64

/*
 * __kuser_memory_barrier: 内存屏障 (地址 0xffff0fa0)
 * 提供用户空间可调用的DMB(数据内存屏障)操作。
 */
__kuser_memory_barrier:				@ 0xffff0fa0
	smp_dmb	arm
	usr_ret	lr

	kuser_pad __kuser_memory_barrier, 32

/*
 * __kuser_cmpxchg: 32位原子比较交换 (地址 0xffff0fc0)
 * ARMv6以下使用禁中断+LDREX/STREX模拟原子操作;
 * ARMv6及以上使用LDREX/STREX指令。
 */
__kuser_cmpxchg:				@ 0xffff0fc0

#if __LINUX_ARM_ARCH__ < 6

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle
	 * of the critical sequence.  To prevent this, code is added to
	 * the IRQ and data abort exception handlers to set the pc back
	 * to the beginning of the critical section if it is found to be
	 * within that critical section (see kuser_cmpxchg_fixup).
	 */
1:	ldr	r3, [r2]			@ load current val (加载当前值)
	subs	r3, r3, r0			@ compare with oldval (与旧值比较)
2:	streq	r1, [r2]			@ store newval if eq (相等则存储新值)
	rsbs	r0, r3, #0			@ set return val and C flag
	usr_ret	lr

	.text
/*
 * kuser_cmpxchg32_fixup: 32位cmpxchg中断修复函数
 */
kuser_cmpxchg32_fixup:
	@ Called from kuser_cmpxchg_check macro.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0fc0 + (1b - __kuser_cmpxchg)))
	subs	r8, r4, r7
	rsbscs	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]			@ 修正PC到临界区起始
	ret	lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else

	smp_dmb	arm
1:	ldrex	r3, [r2]			@ 独占加载当前值
	subs	r3, r3, r0			@ 与旧值比较
	strexeq	r3, r1, [r2]			@ 相等则独占存储新值
	teqeq	r3, #1				@ store succeeded?
	beq	1b				@ 失败则重试
	rsbs	r0, r3, #0
	/* beware -- each __kuser slot must be 8 instructions max */
	ALT_SMP(b	__kuser_memory_barrier)	@ SMP: 需要内存屏障
	ALT_UP(usr_ret	lr)			@ UP: 直接返回

#endif

	kuser_pad __kuser_cmpxchg, 32

/*
 * __kuser_get_tls: 读取线程本地存储(TLS) (地址 0xffff0fe0)
 * 支持软件和硬件TLS两种方式。
 */
__kuser_get_tls:				@ 0xffff0fe0
	ldr	r0, [pc, #(16 - 8)]	@ read TLS, set in kuser_get_tls_init
	usr_ret	lr
	mrc	p15, 0, r0, c13, c0, 3	@ 0xffff0fe8 hardware TLS code (硬件TLS)
	kuser_pad __kuser_get_tls, 16
	.rep	3
	.word	0			@ 0xffff0ff0 software TLS value, then
	.endr				@ pad up to __kuser_helper_version

__kuser_helper_version:				@ 0xffff0ffc
	.word	((__kuser_helper_end - __kuser_helper_start) >> 5)

	.globl	__kuser_helper_end
__kuser_helper_end:

#endif

 THUMB(	.thumb	)

/*
 * Vector stubs - 异常向量存根
 *
 * This code is copied to 0xffff1000 so we can use branches in the
 * vectors, rather than ldr's.  Note that this code must not exceed
 * a page size.
 * 这段代码被复制到0xffff1000,使得向量表可以使用分支指令而非加载指令。
 * 注意: 代码不能超过一个页面大小。
 *
 * Common stub entry macro:
 *   Enter in IRQ mode, spsr = SVC/USR CPSR, lr = SVC/USR PC
 *
 * SP points to a minimal amount of processor-private memory, the address
 * of which is copied into r0 for the mode specific abort handler.
 *
 * vector_stub是异常分发的核心宏:
 * - 从异常模式保存r0、LR、SPSR到模式栈
 * - 切换到SVC模式
 * - 根据异常发生前的模式(SPSR的低4位)查找分支表
 * - 跳转到对应的usr/svc处理程序
 */
	.macro	vector_stub, name, mode, correction=0
	.align	5
#ifdef CONFIG_HARDEN_BRANCH_HISTORY
/*
 * BHB(分支历史缓冲区)安全漏洞的缓解变体1: BPIALL
 */
vector_bhb_bpiall_\name:
	mcr	p15, 0, r0, c7, c5, 6	@ BPIALL (刷新分支预测器)
	@ isb not needed due to "movs pc, lr" in the vector stub
	@ which gives a "context synchronisation".
#endif

vector_\name:
	.if \correction
	sub	lr, lr, #\correction		@ 修正LR,使其指向真正的故障指令
	.endif

	@ Save r0, lr_<exception> (parent PC)
	stmia	sp, {r0, lr}		@ save r0, lr (保存r0和异常LR到模式栈)

	@ Save spsr_<exception> (parent CPSR)
.Lvec_\name:
	mrs	lr, spsr			@ 读取异常时的CPSR
	str	lr, [sp, #8]		@ save spsr (保存SPSR到模式栈)

	@
	@ Prepare for SVC32 mode.  IRQs remain disabled.
	@ 准备切换到SVC模式, IRQ保持关闭。
	@
	mrs	r0, cpsr
	eor	r0, r0, #(\mode ^ SVC_MODE | PSR_ISETSTATE)	@ 将当前模式替换为SVC模式
	msr	spsr_cxsf, r0

	@
	@ the branch table must immediately follow this code
	@ 分支表必须紧跟在代码之后,用于根据异常发生前的模式分发
	@
	and	lr, lr, #0x0f			@ 提取异常发生前的处理器模式(低4位)
 THUMB(	adr	r0, 1f			)	@ Thumb: 获取分支表地址
 THUMB(	ldr	lr, [r0, lr, lsl #2]	)	@ Thumb: 加载处理程序地址
	mov	r0, sp				@ r0 = 模式栈上的寄存器保存区
 ARM(	ldr	lr, [pc, lr, lsl #2]	)	@ ARM: 从分支表加载处理程序地址
	movs	pc, lr			@ branch to handler in SVC mode (切换到SVC并跳转)
ENDPROC(vector_\name)

#ifdef CONFIG_HARDEN_BRANCH_HISTORY
/*
 * BHB安全漏洞的缓解变体2: 循环8次(环回)
 */
	.subsection 1
	.align 5
vector_bhb_loop8_\name:
	.if \correction
	sub	lr, lr, #\correction
	.endif

	@ Save r0, lr_<exception> (parent PC)
	stmia	sp, {r0, lr}

	@ bhb workaround (BHB缓解: 执行8次空跳转刷新分支历史)
	mov	r0, #8
3:	W(b)	. + 4
	subs	r0, r0, #1
	bne	3b
	dsb	nsh
	@ isb not needed due to "movs pc, lr" in the vector stub
	@ which gives a "context synchronisation".
	b	.Lvec_\name
ENDPROC(vector_bhb_loop8_\name)
	.previous
#endif

	.align	2
	@ handler addresses follow this label (处理程序地址表跟在此标签后)
1:
	.endm

	.section .stubs, "ax", %progbits
	@ These need to remain at the start of the section so that
	@ they are in range of the 'SWI' entries in the vector tables
	@ located 4k down.
.L__vector_swi:
	.word	vector_swi			@ SWI/系统调用处理程序地址
#ifdef CONFIG_HARDEN_BRANCH_HISTORY
.L__vector_bhb_loop8_swi:
	.word	vector_bhb_loop8_swi
.L__vector_bhb_bpiall_swi:
	.word	vector_bhb_bpiall_swi
#endif

/*
 * vector_rst: 复位向量
 * 执行软件中断0(系统调用0 - 重启)
 */
vector_rst:
 ARM(	swi	SYS_ERROR0	)
 THUMB(	svc	#0		)
 THUMB(	nop			)
	b	vector_und

/*
 * Interrupt dispatcher - IRQ中断分发器
 * 根据异常发生前的模式,分发到__irq_usr或__irq_svc。
 */
	vector_stub	irq, IRQ_MODE, 4

	.long	__irq_usr			@  0  (USR_26 / USR_32) 用户态
	.long	__irq_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__irq_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__irq_svc			@  3  (SVC_26 / SVC_32) 内核态
	.long	__irq_invalid			@  4
	.long	__irq_invalid			@  5
	.long	__irq_invalid			@  6
	.long	__irq_invalid			@  7
	.long	__irq_invalid			@  8
	.long	__irq_invalid			@  9
	.long	__irq_invalid			@  a
	.long	__irq_invalid			@  b
	.long	__irq_invalid			@  c
	.long	__irq_invalid			@  d
	.long	__irq_invalid			@  e
	.long	__irq_invalid			@  f

/*
 * Data abort dispatcher - 数据中止分发器
 * 在ABT模式下进入, spsr=用户态CPSR, lr=用户态PC
 */
	vector_stub	dabt, ABT_MODE, 8

	.long	__dabt_usr			@  0  (USR_26 / USR_32) 用户态
	.long	__dabt_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__dabt_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__dabt_svc			@  3  (SVC_26 / SVC_32) 内核态
	.long	__dabt_invalid			@  4
	.long	__dabt_invalid			@  5
	.long	__dabt_invalid			@  6
	.long	__dabt_invalid			@  7
	.long	__dabt_invalid			@  8
	.long	__dabt_invalid			@  9
	.long	__dabt_invalid			@  a
	.long	__dabt_invalid			@  b
	.long	__dabt_invalid			@  c
	.long	__dabt_invalid			@  d
	.long	__dabt_invalid			@  e
	.long	__dabt_invalid			@  f

/*
 * Prefetch abort dispatcher - 预取中止分发器
 * 在ABT模式下进入, spsr=用户态CPSR, lr=用户态PC
 */
	vector_stub	pabt, ABT_MODE, 4

	.long	__pabt_usr			@  0 (USR_26 / USR_32) 用户态
	.long	__pabt_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__pabt_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__pabt_svc			@  3 (SVC_26 / SVC_32) 内核态
	.long	__pabt_invalid			@  4
	.long	__pabt_invalid			@  5
	.long	__pabt_invalid			@  6
	.long	__pabt_invalid			@  7
	.long	__pabt_invalid			@  8
	.long	__pabt_invalid			@  9
	.long	__pabt_invalid			@  a
	.long	__pabt_invalid			@  b
	.long	__pabt_invalid			@  c
	.long	__pabt_invalid			@  d
	.long	__pabt_invalid			@  e
	.long	__pabt_invalid			@  f

/*
 * Undef instr entry dispatcher - 未定义指令分发器
 * 在UND模式下进入, spsr=SVC/USR CPSR, lr=SVC/USR PC
 */
	vector_stub	und, UND_MODE

	.long	__und_usr			@  0 (USR_26 / USR_32) 用户态
	.long	__und_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__und_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__und_svc			@  3 (SVC_26 / SVC_32) 内核态
	.long	__und_invalid			@  4
	.long	__und_invalid			@  5
	.long	__und_invalid			@  6
	.long	__und_invalid			@  7
	.long	__und_invalid			@  8
	.long	__und_invalid			@  9
	.long	__und_invalid			@  a
	.long	__und_invalid			@  b
	.long	__und_invalid			@  c
	.long	__und_invalid			@  d
	.long	__und_invalid			@  e
	.long	__und_invalid			@  f

	.align	5

/*=============================================================================
 * Address exception handler - 地址异常处理
 *-----------------------------------------------------------------------------
 * These aren't too critical.
 * (they're not supposed to happen, and won't happen in 32-bit data mode).
 * 不太关键(在32位模式下不应该发生)。
 */

vector_addrexcptn:
	b	vector_addrexcptn		@ 无限循环(不应到达此处)

/*=============================================================================
 * FIQ "NMI" handler - FIQ快速中断处理(作为NMI)
 *-----------------------------------------------------------------------------
 * Handle a FIQ using the SVC stack allowing FIQ act like NMI on x86
 * systems. This must be the last vector stub, so lets place it in its own
 * subsection.
 * 使用SVC栈处理FIQ,使其行为类似于x86系统的NMI。
 * 必须是最后一个向量存根,放在独立的subsection中。
 */
	.subsection 2
	vector_stub	fiq, FIQ_MODE, 4

	.long	__fiq_usr			@  0  (USR_26 / USR_32) 用户态
	.long	__fiq_svc			@  1  (FIQ_26 / FIQ_32)
	.long	__fiq_svc			@  2  (IRQ_26 / IRQ_32)
	.long	__fiq_svc			@  3  (SVC_26 / SVC_32) 内核态
	.long	__fiq_svc			@  4
	.long	__fiq_svc			@  5
	.long	__fiq_svc			@  6
	.long	__fiq_abt			@  7  (ABT_26/ABT_32) Abort模式
	.long	__fiq_svc			@  8
	.long	__fiq_svc			@  9
	.long	__fiq_svc			@  a
	.long	__fiq_svc			@  b
	.long	__fiq_svc			@  c
	.long	__fiq_svc			@  d
	.long	__fiq_svc			@  e
	.long	__fiq_svc			@  f

	.globl	vector_fiq			@ FIQ向量全局符号

/*
 * 异常向量表 - Exception vectors
 * 位于0xffff0000(高向量)或0x00000000(低向量)处。
 * 正常情况下使用W(b)分支到向量存根。
 * SWI使用W(ldr)加载系统调用处理程序地址。
 */
	.section .vectors, "ax", %progbits
	RELOC_TEXT_NONE
	W(b)	vector_rst			@ 0x00: 复位
	W(b)	vector_und			@ 0x04: 未定义指令
ARM(	.reloc	., R_ARM_LDR_PC_G0, .L__vector_swi		)
THUMB(	.reloc	., R_ARM_THM_PC12, .L__vector_swi		)
	W(ldr)	pc, .				@ 0x08: 软件中断(SWI/系统调用)
	W(b)	vector_pabt			@ 0x0C: 预取中止
	W(b)	vector_dabt			@ 0x10: 数据中止
	W(b)	vector_addrexcptn		@ 0x14: 地址异常
	W(b)	vector_irq			@ 0x18: IRQ中断
	W(b)	vector_fiq			@ 0x1C: FIQ快速中断

#ifdef CONFIG_HARDEN_BRANCH_HISTORY
/*
 * BHB缓解变体1的向量表 - 循环8次
 */
	.section .vectors.bhb.loop8, "ax", %progbits
	RELOC_TEXT_NONE
	W(b)	vector_rst
	W(b)	vector_bhb_loop8_und
ARM(	.reloc	., R_ARM_LDR_PC_G0, .L__vector_bhb_loop8_swi	)
THUMB(	.reloc	., R_ARM_THM_PC12, .L__vector_bhb_loop8_swi	)
	W(ldr)	pc, .
	W(b)	vector_bhb_loop8_pabt
	W(b)	vector_bhb_loop8_dabt
	W(b)	vector_addrexcptn
	W(b)	vector_bhb_loop8_irq
	W(b)	vector_bhb_loop8_fiq

/*
 * BHB缓解变体2的向量表 - BPIALL
 */
	.section .vectors.bhb.bpiall, "ax", %progbits
	RELOC_TEXT_NONE
	W(b)	vector_rst
	W(b)	vector_bhb_bpiall_und
ARM(	.reloc	., R_ARM_LDR_PC_G0, .L__vector_bhb_bpiall_swi	)
THUMB(	.reloc	., R_ARM_THM_PC12, .L__vector_bhb_bpiall_swi	)
	W(ldr)	pc, .
	W(b)	vector_bhb_bpiall_pabt
	W(b)	vector_bhb_bpiall_dabt
	W(b)	vector_addrexcptn
	W(b)	vector_bhb_bpiall_irq
	W(b)	vector_bhb_bpiall_fiq
#endif

	.data
	.align	2

/*
 * cr_alignment: 对齐陷阱控制寄存器值
 * 在内核态启用/禁用对齐陷阱时使用。
 */
	.globl	cr_alignment
cr_alignment:
	.space	4
