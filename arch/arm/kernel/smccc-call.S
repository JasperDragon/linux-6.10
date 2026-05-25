/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015, Linaro Limited
 */

/*
 * ============================================================================
 * smccc-call.S — ARM32 SMCCC (SMC Calling Convention) 调用包装器
 * ============================================================================
 *
 * ARM32 的 SMCCC 实现与 ARM64 类似，但调用约定不同：
 *   - 参数: r0-r3 = 寄存器传递，r4-r7 = 栈传递（通过 push/pop 管理）
 *   - 前4个参数 (a0-a3) 在 r0-r3 中
 *   - 后4个参数 (a4-a7) 在 r4-r7 中（从栈上加载）
 *   - 返回值 r0-r3 存入 *res (r12 指向)
 *
 * SMC (__SMC(0)): 进入 Secure Monitor (ARM32 安全扩展)
 * HVC (__HVC(0)): 进入 Hypervisor (ARM32 虚拟化扩展)
 *
 * 关键差异于 ARM64:
 *   - ARM32 只有 4 个参数寄存器 (r0-r3)，额外参数需从栈加载
 *   - 使用 bx lr 返回而非 ret
 *   - 需要显式 .save 展开表注释（UNWIND）
 */
#include <linux/arm-smccc.h>

#include <asm/asm-offsets.h>
#include <asm/opcodes-sec.h>
#include <asm/opcodes-virt.h>
#include <asm/unwind.h>

	/*
	 * Wrap c macros in asm macros to delay expansion until after the
	 * SMCCC asm macro is expanded.
	 */
	.macro SMCCC_SMC
	__SMC(0)
	.endm

	.macro SMCCC_HVC
	__HVC(0)
	.endm

	.macro SMCCC instr
UNWIND(	.fnstart)
	mov	r12, sp
	push	{r4-r7}
UNWIND(	.save	{r4-r7})
	ldm	r12, {r4-r7}
	\instr
	ldr	r4, [sp, #36]
	cmp	r4, #0
	beq	1f			// No quirk structure
	ldr     r5, [r4, #ARM_SMCCC_QUIRK_ID_OFFS]
	cmp     r5, #ARM_SMCCC_QUIRK_QCOM_A6
	bne	1f			// No quirk present
	str	r6, [r4, #ARM_SMCCC_QUIRK_STATE_OFFS]
1:	pop	{r4-r7}
	ldr	r12, [sp, #(4 * 4)]
	stm	r12, {r0-r3}
	bx	lr
UNWIND(	.fnend)
	.endm

/*
 * void smccc_smc(unsigned long a0, unsigned long a1, unsigned long a2,
 *		  unsigned long a3, unsigned long a4, unsigned long a5,
 *		  unsigned long a6, unsigned long a7, struct arm_smccc_res *res,
 *		  struct arm_smccc_quirk *quirk)
 */
ENTRY(__arm_smccc_smc)
	SMCCC SMCCC_SMC
ENDPROC(__arm_smccc_smc)

/*
 * void smccc_hvc(unsigned long a0, unsigned long a1, unsigned long a2,
 *		  unsigned long a3, unsigned long a4, unsigned long a5,
 *		  unsigned long a6, unsigned long a7, struct arm_smccc_res *res,
 *		  struct arm_smccc_quirk *quirk)
 */
ENTRY(__arm_smccc_hvc)
	SMCCC SMCCC_HVC
ENDPROC(__arm_smccc_hvc)
