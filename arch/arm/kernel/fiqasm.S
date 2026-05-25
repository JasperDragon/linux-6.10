/*
 *  linux/arch/arm/kernel/fiqasm.S
 *
 *  Derived from code originally in linux/arch/arm/kernel/fiq.c:
 *
 *  Copyright (C) 1998 Russell King
 *  Copyright (C) 1998, 1999 Phil Blundell
 *  Copyright (C) 2011, Linaro Limited
 *
 *  FIQ support written by Philip Blundell <philb@gnu.org>, 1998.
 */

/*
 * ============================================================================
 * FIQ (Fast Interrupt reQuest) 底层汇编支持
 * ============================================================================
 *
 * FIQ 是 ARM 的快速中断，具有专用的 r8-r12 寄存器（banked），
 * 相比 IRQ 更快速，因为无需保存这些寄存器。
 *
 * __set_fiq_regs / __get_fiq_regs:
 *   设置/读取 FIQ 模式下 banked 寄存器 (r8-r13) 的值。
 *   通过 SPSR 切换模式后直接访问 banked 寄存器，然后恢复原模式。
 *
 * __set_fiq_handler:
 *   设置 FIQ 处理函数入口地址，复制到 FIQ 向量的跳转地址处。
 *
 * FIQ vs IRQ: FIQ 有独立的 banked r8-r12 和 sp (r13), lr (r14)，
 *   无需栈帧操作即可瞬时进入处理函数，适合极低延迟场景。
 */
 *
 *  FIQ support re-written by Russell King to be more generic
 *
 *  v7/Thumb-2 compatibility modifications by Linaro Limited, 2011.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Taking an interrupt in FIQ mode is death, so both these functions
 * disable irqs for the duration.
 */

ENTRY(__set_fiq_regs)
	mov	r2, #PSR_I_BIT | PSR_F_BIT | FIQ_MODE
	mrs	r1, cpsr
	msr	cpsr_c, r2	@ select FIQ mode
	mov	r0, r0		@ avoid hazard prior to ARMv4
	ldmia	r0!, {r8 - r12}
	ldr	sp, [r0], #4
	ldr	lr, [r0]
	msr	cpsr_c, r1	@ return to SVC mode
	mov	r0, r0		@ avoid hazard prior to ARMv4
	ret	lr
ENDPROC(__set_fiq_regs)

ENTRY(__get_fiq_regs)
	mov	r2, #PSR_I_BIT | PSR_F_BIT | FIQ_MODE
	mrs	r1, cpsr
	msr	cpsr_c, r2	@ select FIQ mode
	mov	r0, r0		@ avoid hazard prior to ARMv4
	stmia	r0!, {r8 - r12}
	str	sp, [r0], #4
	str	lr, [r0]
	msr	cpsr_c, r1	@ return to SVC mode
	mov	r0, r0		@ avoid hazard prior to ARMv4
	ret	lr
ENDPROC(__get_fiq_regs)
