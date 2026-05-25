/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/kernel/entry-common.S
 *
 *  Copyright (C) 2000 Russell King
 *
 * 概述: entry-common.S -- ARM32系统调用及返回路径公共代码
 * 本文件实现系统调用的分发处理和内核态/用户态切换。
 * 主要内容:
 *   vector_swi        - 软件中断(SWI/SVC)入口,系统调用总入口
 *   ret_fast_syscall  - 系统调用快速返回路径(无追踪时使用)
 *   ret_slow_syscall  - 系统调用慢速返回路径(需处理信号等)
 *   ret_to_user       - 返回用户空间的通用路径
 *   ret_from_fork     - 新创建进程的首次返回路径
 *   系统调用表(sys_call_table)的构建
 *   兼容OABI的包装函数
 */

#include <asm/assembler.h>
#include <asm/unistd.h>
#include <asm/ftrace.h>
#include <asm/unwind.h>
#include <asm/page.h>
#ifdef CONFIG_AEABI
#include <asm/unistd-oabi.h>
#endif

		.equ	NR_syscalls, __NR_syscalls

#include "entry-header.S"

saved_psr	.req	r8
#if defined(CONFIG_TRACE_IRQFLAGS) || defined(CONFIG_CONTEXT_TRACKING_USER)
saved_pc	.req	r9
#define TRACE(x...) x
#else
saved_pc	.req	lr
#define TRACE(x...)
#endif

/*
 * 系统调用快速返回路径 - ret_fast_syscall
 * 当不启用tracing、context tracking和rseq debug时,使用此快速路径。
 * 尽可能少做工作,提高系统调用返回性能。
 * 如果检查到需要额外处理(如信号),则fall through到work_pending。
 */
		.section .entry.text,"ax",%progbits
		.align	5
#if !(IS_ENABLED(CONFIG_TRACE_IRQFLAGS) || IS_ENABLED(CONFIG_CONTEXT_TRACKING_USER) || \
	IS_ENABLED(CONFIG_DEBUG_RSEQ))
/*
 * This is the fast syscall return path.  We do as little as possible here,
 * such as avoiding writing r0 to the stack.  We only use this path if we
 * have tracing, context tracking and rseq debug disabled - the overheads
 * from those features make this path too inefficient.
 */
ret_fast_syscall:
__ret_fast_syscall:
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	disable_irq_notrace			@ 关闭IRQ(不追踪)
	ldr	r1, [tsk, #TI_FLAGS]		@ 重新检查线程标志
	movs	r1, r1, lsl #16			@ 提取低16位(TIF_WORK_MASK)
	bne	fast_work_pending		@ 有工作需要处理,进入慢速路径

	restore_user_regs fast = 1, offset = S_OFF	@ 快速恢复用户寄存器并返回
 UNWIND(.fnend		)
ENDPROC(ret_fast_syscall)

	/* Ok, we need to do extra processing, enter the slow path. */
fast_work_pending:
	str	r0, [sp, #S_R0+S_OFF]!		@ 保存返回的r0到栈
	/* fall through to work_pending */
#else
/*
 * ret_fast_syscall替代实现(启用tracing/context tracking/rseq debug时使用)
 * 由于需要调用C函数,先将r0保存到栈以避免在C函数调用中保存寄存器。
 */
ret_fast_syscall:
__ret_fast_syscall:
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	str	r0, [sp, #S_R0 + S_OFF]!	@ 保存返回的r0
#if IS_ENABLED(CONFIG_DEBUG_RSEQ)
	/* do_rseq_syscall needs interrupts enabled. */
	mov	r0, sp				@ 'regs'指针
	bl	do_rseq_syscall			@ 处理rseq系统调用事件
#endif
	disable_irq_notrace			@ 关闭IRQ(不追踪)
	ldr	r1, [tsk, #TI_FLAGS]		@ 重新检查线程标志
	movs	r1, r1, lsl #16			@ 提取TIF_WORK_MASK
	beq	no_work_pending			@ 无需工作,直接返回
 UNWIND(.fnend		)
ENDPROC(ret_fast_syscall)

	/* Slower path - fall through to work_pending */
#endif

	tst	r1, #_TIF_SYSCALL_WORK		@ 检查系统调用追踪相关标志
	bne	__sys_trace_return_nosave	@ 需要追踪,进入追踪返回路径
slow_work_pending:
	mov	r0, sp				@ 'regs'参数
	mov	r2, why				@ 'syscall'参数
	bl	do_work_pending			@ 处理待处理工作(信号、调度等)
	cmp	r0, #0				@ 是否需要重启系统调用?
	beq	no_work_pending			@ 不需要,直接返回
	movlt	scno, #(__NR_restart_syscall - __NR_SYSCALL_BASE)	@ 需要重启
	str	scno, [tsk, #TI_ABI_SYSCALL]	@ 确保追踪器看到更新
	ldmia	sp, {r0 - r6}			@ 重新加载参数寄存器
	b	local_restart			@ 跳转到系统调用分发
ENDPROC(ret_fast_syscall)

/*
 * 系统调用慢速返回路径 - ret_slow_syscall / ret_to_user
 * 用于需要额外处理(如信号传递、进程调度)时的系统调用返回。
 * 也用于中断/异常返回用户空间。
 */
ENTRY(ret_to_user)
ret_slow_syscall:
#if IS_ENABLED(CONFIG_DEBUG_RSEQ)
	/* do_rseq_syscall needs interrupts enabled. */
	enable_irq_notrace			@ 使能IRQ(不追踪)
	mov	r0, sp				@ 'regs'指针
	bl	do_rseq_syscall			@ 处理rseq系统调用事件
#endif
	disable_irq_notrace			@ 关闭IRQ(不追踪)
ENTRY(ret_to_user_from_irq)
	ldr	r1, [tsk, #TI_FLAGS]		@ 加载线程标志
	movs	r1, r1, lsl #16			@ 提取TIF_WORK_MASK
	bne	slow_work_pending		@ 有工作待处理
no_work_pending:
	asm_trace_hardirqs_on save = 0		@ 标记hardirq开启(tracing)

	ct_user_enter save = 0			@ 标记进入用户态(context tracking)

#ifdef CONFIG_KSTACK_ERASE
	bl	stackleak_erase_on_task_stack	@ 擦除内核栈上的敏感数据
#endif
	restore_user_regs fast = 0, offset = 0	@ 恢复用户寄存器并返回
ENDPROC(ret_to_user_from_irq)
ENDPROC(ret_to_user)

/*
 * ret_from_fork: 新创建进程的首次返回路径
 * 子进程首次运行时从此处开始执行。
 * 调用schedule_tail完成调度收尾工作,
 * 然后判断是否需要执行内核线程函数。
 */
ENTRY(ret_from_fork)
	bl	schedule_tail			@ 完成调度收尾
	cmp	r5, #0				@ 是否内核线程?
	movne	r0, r4				@ 是,加载内核线程参数
	badrne	lr, 1f				@ 设置返回地址
	retne	r5				@ 跳转到内核线程函数
1:	get_thread_info tsk			@ 获取当前线程信息
	b	ret_slow_syscall		@ 返回用户空间(或退出内核线程)
ENDPROC(ret_from_fork)

/*=============================================================================
 * SWI handler - 软件中断处理(System Call入口)
 *-----------------------------------------------------------------------------
 */

		.align	5
#ifdef CONFIG_HARDEN_BRANCH_HISTORY
/*
 * vector_bhb_loop8_swi: BHB缓解变体 - 循环8次
 * 在系统调用入口执行8次空跳转,刷新分支历史缓冲区(BHB),
 * 缓解Spectre变种BHB安全漏洞。
 */
ENTRY(vector_bhb_loop8_swi)
	sub	sp, sp, #PT_REGS_SIZE		@ 分配pt_regs空间
	stmia	sp, {r0 - r12}			@ 保存r0-r12寄存器
	mov	r8, #8				@ 设置循环计数=8
1:	b	2f				@ 空跳转
2:	subs	r8, r8, #1			@ 递减计数
	bne	1b				@ 循环8次
	dsb	nsh				@ 数据同步屏障(非共享)
	isb					@ 指令同步屏障
	b	3f				@ 跳转到公共处理
ENDPROC(vector_bhb_loop8_swi)

		.align	5
/*
 * vector_bhb_bpiall_swi: BHB缓解变体 - BPIALL
 * 通过BPIALL操作刷新分支预测器来缓解BHB漏洞。
 */
ENTRY(vector_bhb_bpiall_swi)
	sub	sp, sp, #PT_REGS_SIZE		@ 分配pt_regs空间
	stmia	sp, {r0 - r12}			@ 保存r0-r12寄存器
	mcr	p15, 0, r8, c7, c5, 6	@ BPIALL (刷新分支预测器)
	isb					@ 指令同步屏障
	b	3f				@ 跳转到公共处理
ENDPROC(vector_bhb_bpiall_swi)
#endif
	.align	5
/*
 * vector_swi: 系统调用主入口
 * 保存所有用户寄存器到内核栈,构造pt_regs结构。
 * 根据ABI类型(EABI/OABI)获取系统调用号和系统调用表。
 * EABI: 系统调用号在r7中
 * OABI: 系统调用号在SWI指令的低24位中
 */
ENTRY(vector_swi)
#ifdef CONFIG_CPU_V7M
	v7m_exception_entry			@ Cortex-M系列: 使用v7m异常入口
#else
	sub	sp, sp, #PT_REGS_SIZE		@ 分配pt_regs结构空间
	stmia	sp, {r0 - r12}			@ 保存r0 - r12到栈
3:
 ARM(	add	r8, sp, #S_PC		)
 ARM(	stmdb	r8, {sp, lr}^		)	@ 保存用户态SP和LR
 THUMB(	mov	r8, sp			)
 THUMB(	store_user_sp_lr r8, r10, S_SP	)	@ Thumb: 保存用户SP和LR
	mrs	saved_psr, spsr			@ 读取进入SVC前的CPSR
 TRACE(	mov	saved_pc, lr		)
	str	saved_pc, [sp, #S_PC]		@ 保存用户态PC
	str	saved_psr, [sp, #S_PSR]		@ 保存用户态CPSR
	str	r0, [sp, #S_OLD_R0]		@ 保存原始r0
#endif
	reload_current r10, ip			@ 重新加载current指针
	zero_fp					@ 清零帧指针
	alignment_trap r10, ip, cr_alignment	@ 配置对齐陷阱
	asm_trace_hardirqs_on save=0		@ 标记hardirq开启(tracing)
	enable_irq_notrace			@ 使能IRQ(系统调用执行期间可中断)
	ct_user_exit save=0			@ 标记退出用户态(context tracking)

	/*
	 * 获取系统调用号 - Get the system call number.
	 */

#if defined(CONFIG_OABI_COMPAT)

	/*
	 * OABI兼容模式: 根据SWI指令内容判断是EABI还是OABI调用。
	 * EABI调用: SWI指令参数为0,系统调用号在r7中
	 * OABI调用: SWI指令参数包含系统调用号
	 */
#ifdef CONFIG_ARM_THUMB
	tst	saved_psr, #PSR_T_BIT		@ 检查Thumb模式?
	movne	r10, #0				@ Thumb模式下OABI不支持
 USER(	ldreq	r10, [saved_pc, #-4]	)	@ ARM模式: 读取SWI指令
#else
 USER(	ldr	r10, [saved_pc, #-4]	)	@ 读取SWI指令
#endif
 ARM_BE8(rev	r10, r10)			@ BE8模式: 交换字节序

#elif defined(CONFIG_AEABI)

	/*
	 * 纯EABI模式: 系统调用号始终在r7(scno)中。
	 */

#elif defined(CONFIG_ARM_THUMB)
	/* Legacy ABI only, possibly thumb mode. */
	tst	saved_psr, #PSR_T_BIT		@ 检查Thumb模式?
	addne	scno, r7, #__NR_SYSCALL_BASE	@ Thumb: 系统调用号在r7中
 USER(	ldreq	scno, [saved_pc, #-4]	)	@ ARM: 从SWI指令获取

#else
	/* Legacy ABI only. */
 USER(	ldr	scno, [saved_pc, #-4]	)	@ 从SWI指令获取系统调用号
#endif

	/* saved_psr and saved_pc are now dead */

	uaccess_disable tbl			@ 关闭用户空间内存访问
	get_thread_info tsk			@ 获取当前线程信息

	adr	tbl, sys_call_table		@ 加载系统调用表基地址

#if defined(CONFIG_OABI_COMPAT)
	/*
	 * OABI兼容模式: 根据SWI参数判断ABI类型并选择对应的系统调用表。
	 * SWI参数为0: EABI调用,使用sys_call_table
	 * SWI参数非0: OABI调用,使用sys_oabi_call_table
	 */
	bics	r10, r10, #0xff000000		@ 提取SWI指令的低24位(系统调用号)
	strne	r10, [tsk, #TI_ABI_SYSCALL]	@ OABI: 保存系统调用号
	streq	scno, [tsk, #TI_ABI_SYSCALL]	@ EABI: 保存系统调用号(r7)
	eorne	scno, r10, #__NR_OABI_SYSCALL_BASE	@ OABI: 转换系统调用号
	ldrne	tbl, =sys_oabi_call_table	@ OABI: 加载OABI兼容调用表
#elif !defined(CONFIG_AEABI)
	bic	scno, scno, #0xff000000		@ 屏蔽SWI指令操作码
	str	scno, [tsk, #TI_ABI_SYSCALL]	@ 保存系统调用号
	eor	scno, scno, #__NR_SYSCALL_BASE	@ 检查OS编号(确保是Linux调用)
#else
	str	scno, [tsk, #TI_ABI_SYSCALL]	@ EABI: 保存系统调用号
#endif
	/*
	 * 重新加载可能被tracing/context tracking破坏的寄存器。
	 */
 TRACE(	ldmia	sp, {r0 - r3}		)

	/*
	 * local_restart: 系统调用分发执行点
	 * 检查是否需要追踪(tracing),然后调用对应的系统调用函数。
	 * 也是系统调用重启时的跳转目标。
	 */
local_restart:
	ldr	r10, [tsk, #TI_FLAGS]		@ 检查线程标志(是否追踪系统调用)
	stmdb	sp!, {r4, r5}			@ 将第5、6个参数压栈

	tst	r10, #_TIF_SYSCALL_WORK		@ 是否需要追踪?
	bne	__sys_trace			@ 需要追踪,进入追踪路径

	invoke_syscall tbl, scno, r10, __ret_fast_syscall	@ 调用系统调用

	add	r1, sp, #S_OFF			@ 调整栈指针
2:	cmp	scno, #(__ARM_NR_BASE - __NR_SYSCALL_BASE)	@ ARM特定系统调用?
	eor	r0, scno, #__NR_SYSCALL_BASE	@ 恢复原始系统调用号
	bcs	arm_syscall			@ ARM特定系统调用
	mov	why, #0				@ 不再是真正的系统调用
	b	sys_ni_syscall			@ 未实现系统调用

#if defined(CONFIG_OABI_COMPAT) || !defined(CONFIG_AEABI)
	/*
	 * 读取SWI指令时发生页错误: 无法返回-EFAULT,
	 * 返回到SWI指令重新执行,触发用户态缺页处理。
	 */
9001:
	sub	lr, saved_pc, #4		@ 返回到SWI指令
	str	lr, [sp, #S_PC]			@ 修正保存的PC
	get_thread_info tsk			@ 获取当前线程信息
	b	ret_fast_syscall			@ 从快速路径返回
#endif
ENDPROC(vector_swi)
	.ltorg

	/*
	 * __sys_trace: 系统调用追踪慢速路径
	 * 调用syscall_trace_enter通知追踪器,
	 * 执行系统调用后调用syscall_trace_exit。
	 * 此路径也用于ptrace调试场景。
	 */
__sys_trace:
	add	r0, sp, #S_OFF			@ pt_regs指针
	bl	syscall_trace_enter		@ 通知追踪器(可能修改系统调用号)
	mov	scno, r0			@ 获取(可能被修改的)系统调用号
	invoke_syscall tbl, scno, r10, __sys_trace_return, reload=1	@ 执行系统调用
	cmp	scno, #-1			@ 跳过系统调用?
	bne	2b				@ 未跳过,转到正常检查
	add	sp, sp, #S_OFF			@ 跳过系统调用,恢复栈

__sys_trace_return_nosave:
	enable_irq_notrace			@ 使能IRQ(不追踪)
	mov	r0, sp				@ pt_regs指针
	bl	syscall_trace_exit		@ 通知追踪器系统调用结束
	b	ret_slow_syscall		@ 从慢速路径返回

__sys_trace_return:
	str	r0, [sp, #S_R0 + S_OFF]!	@ 保存返回的r0
	mov	r0, sp				@ pt_regs指针
	bl	syscall_trace_exit		@ 通知追踪器系统调用结束
	b	ret_slow_syscall		@ 从慢速路径返回

/*
 * 系统调用表构建宏 - syscall table construction macros
 * syscall_table_start: 开始定义系统调用表
 * syscall: 在指定序号处插入系统调用函数指针,
 *          自动填充未使用序号为sys_ni_syscall
 * syscall_table_end: 结束系统调用表,填充剩余未用序号
 */
	.macro	syscall_table_start, sym
	.equ	__sys_nr, 0
	.type	\sym, #object
ENTRY(\sym)
	.endm

	.macro	syscall, nr, func
	.ifgt	__sys_nr - \nr
	.error	"Duplicated/unorded system call entry"
	.endif
	.rept	\nr - __sys_nr
	.long	sys_ni_syscall			@ 填充未使用序号
	.endr
	.long	\func				@ 插入系统调用函数指针
	.equ	__sys_nr, \nr + 1
	.endm

	.macro	syscall_table_end, sym
	.ifgt	__sys_nr - __NR_syscalls
	.error	"System call table too big"
	.endif
	.rept	__NR_syscalls - __sys_nr
	.long	sys_ni_syscall			@ 填充剩余未使用序号
	.endr
	.size	\sym, . - \sym
	.endm

#define __SYSCALL_WITH_COMPAT(nr, native, compat)	__SYSCALL(nr, native)
#define __SYSCALL(nr, func) syscall nr, func

/*
 * 原生ABI系统调用表声明 - Native ABI syscall table
 * 根据配置选择EABI或OABI的调用表。
 */
	syscall_table_start sys_call_table
#ifdef CONFIG_AEABI
#include <calls-eabi.S>			@ EABI系统调用表
#else
#include <calls-oabi.S>			@ OABI系统调用表
#endif
	syscall_table_end sys_call_table

/*============================================================================
 * Special system call wrappers - 特殊系统调用包装函数
 * 这些包装函数处理参数布局差异或ABI兼容性问题。
 */
@ r0 = syscall number
@ r8 = syscall table
sys_syscall:
		bic	scno, r0, #__NR_OABI_SYSCALL_BASE	@ 提取系统调用号
		cmp	scno, #__NR_syscall - __NR_SYSCALL_BASE	@ 检查范围下界
		cmpne	scno, #NR_syscalls		@ 检查范围上界
#ifdef CONFIG_CPU_SPECTRE
		movhs	scno, #0			@ 越界: 清0(spectre缓解)
		csdb
#endif
		stmialo	sp, {r5, r6}		@ 未越界: 将r5,r6压栈(第5、6参数)
		movlo	r0, r1			@ 参数重排: r1->r0
		movlo	r1, r2
		movlo	r2, r3
		movlo	r3, r4
		ldrlo	pc, [tbl, scno, lsl #2]	@ 跳转到系统调用函数
		b	sys_ni_syscall		@ 越界: 返回未实现
ENDPROC(sys_syscall)

/*
 * sys_sigreturn_wrapper: sigreturn系统调用包装
 * 传递pt_regs指针,防止系统调用重启。
 */
sys_sigreturn_wrapper:
		add	r0, sp, #S_OFF		@ pt_regs指针
		mov	why, #0			@ 防止系统调用重启处理
		b	sys_sigreturn
ENDPROC(sys_sigreturn_wrapper)

/*
 * sys_rt_sigreturn_wrapper: rt_sigreturn系统调用包装
 * 传递pt_regs指针,防止系统调用重启。
 */
sys_rt_sigreturn_wrapper:
		add	r0, sp, #S_OFF		@ pt_regs指针
		mov	why, #0			@ 防止系统调用重启处理
		b	sys_rt_sigreturn
ENDPROC(sys_rt_sigreturn_wrapper)

/*
 * sys_statfs64_wrapper: statfs64系统调用包装
 * 兼容不同版本的statfs64结构体大小。
 */
sys_statfs64_wrapper:
		teq	r1, #88			@ 旧版结构体大小?
		moveq	r1, #84			@ 调整为实际大小
		b	sys_statfs64
ENDPROC(sys_statfs64_wrapper)

/*
 * sys_fstatfs64_wrapper: fstatfs64系统调用包装
 * 兼容不同版本的fstatfs64结构体大小。
 */
sys_fstatfs64_wrapper:
		teq	r1, #88			@ 旧版结构体大小?
		moveq	r1, #84			@ 调整为实际大小
		b	sys_fstatfs64
ENDPROC(sys_fstatfs64_wrapper)

/*
 * sys_mmap2: mmap2系统调用包装
 * 传入的偏移量以4KB为单位,直接传递给sys_mmap_pgoff。
 */
sys_mmap2:
		str	r5, [sp, #4]		@ 将偏移量参数压栈
		b	sys_mmap_pgoff
ENDPROC(sys_mmap2)

#ifdef CONFIG_OABI_COMPAT

/*
 * OABI兼容包装函数 - OABI compatibility wrappers
 * 旧ABI(OABI)的参数传递方式与EABI不同,
 * 这些包装函数调整参数布局使其兼容EABI的实现。
 * OABI将64位参数拆分到两个寄存器中,
 * 需要重新组合后才能传递给EABI系统调用函数。
 */

sys_oabi_pread64:
		stmia	sp, {r3, r4}		@ 将64位偏移量(r3,r4)压栈
		b	sys_pread64
ENDPROC(sys_oabi_pread64)

sys_oabi_pwrite64:
		stmia	sp, {r3, r4}		@ 将64位偏移量(r3,r4)压栈
		b	sys_pwrite64
ENDPROC(sys_oabi_pwrite64)

sys_oabi_truncate64:
		mov	r3, r2			@ 重新排列64位参数
		mov	r2, r1			@ r1:r2 = 64位长度
		b	sys_truncate64
ENDPROC(sys_oabi_truncate64)

sys_oabi_ftruncate64:
		mov	r3, r2			@ 重新排列64位参数
		mov	r2, r1
		b	sys_ftruncate64
ENDPROC(sys_oabi_ftruncate64)

sys_oabi_readahead:
		str	r3, [sp]		@ 将第4个参数压栈
		mov	r3, r2			@ 重新排列参数
		mov	r2, r1
		b	sys_readahead
ENDPROC(sys_oabi_readahead)

/*
 * OABI兼容系统调用表 - Old ABI syscall table
 * 为旧ABI(OABI)二进制程序提供的第二套系统调用表。
 * 使用兼容的系统调用条目处理参数布局差异。
 */
	syscall_table_start sys_oabi_call_table
#undef __SYSCALL_WITH_COMPAT
#define __SYSCALL_WITH_COMPAT(nr, native, compat)	__SYSCALL(nr, compat)
#include <calls-oabi.S>
	syscall_table_end sys_oabi_call_table

#endif
