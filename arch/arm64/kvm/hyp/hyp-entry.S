/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2015-2018 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

//=============================================================================
// hyp-entry.S — KVM 虚拟机监控器（Hypervisor）异常向量与入口处理
//
// 本文件实现了 KVM 在 EL2（异常级别 2）上的异常向量表和入口处理逻辑。
// 主要功能包括：
//   - 异常向量表定义（__kvm_hyp_vector 和 __bp_harden_hyp_vecs）
//   - Guest 陷入 EL2 时的同步/中断/错误处理
//   - Spectre 变通方案的 SMCCC 调用处理
//   - 非法异常返回的检测与恢复
//   - 分支预测加固向量（Spectre BHB/SMCCC）
//
// 异常向量布局（AArch64）：
//   EL2t: 使用 SP_EL0 的 EL2 异常（当前异常级使用 SP0）
//   EL2h: 使用 SP_ELx 的 EL2 异常（当前异常级使用当前 SP）
//   EL1:  来自 EL1 的异常（64位和32位各一组）
// 每组包含：同步、IRQ、FIQ、SError 四种异常类型
//=============================================================================

#include <linux/arm-smccc.h>
#include <linux/linkage.h>

#include <asm/alternative.h>
#include <asm/assembler.h>
#include <asm/cpufeature.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/mmu.h>
#include <asm/spectre.h>

//=============================================================================
// save_caller_saved_regs_vect / restore_caller_saved_regs_vect
// 在异常向量入口/出口处保存和恢复所有调用者保存寄存器（x2~x17）
// x0 和 x1 已在向量入口序列中保存，此处不再重复处理
// 使用栈的前递减/后递增方式（! 后缀表示先减后存）
//=============================================================================

.macro save_caller_saved_regs_vect
	/* x0 and x1 were saved in the vector entry */
	stp	x2, x3,   [sp, #-16]!
	stp	x4, x5,   [sp, #-16]!
	stp	x6, x7,   [sp, #-16]!
	stp	x8, x9,   [sp, #-16]!
	stp	x10, x11, [sp, #-16]!
	stp	x12, x13, [sp, #-16]!
	stp	x14, x15, [sp, #-16]!
	stp	x16, x17, [sp, #-16]!
.endm

// 从栈中恢复 x16~x17, x14~x15, ..., x2~x3, x0~x1
// 恢复顺序与保存顺序相反（栈的后递增方式）
.macro restore_caller_saved_regs_vect
	ldp	x16, x17, [sp], #16
	ldp	x14, x15, [sp], #16
	ldp	x12, x13, [sp], #16
	ldp	x10, x11, [sp], #16
	ldp	x8, x9,   [sp], #16
	ldp	x6, x7,   [sp], #16
	ldp	x4, x5,   [sp], #16
	ldp	x2, x3,   [sp], #16
	ldp	x0, x1,   [sp], #16
.endm

	.text

//=============================================================================
// EL1 异常处理入口 — Guest 陷入 EL2 时的同步/中断/错误处理
//
// 处理流程：
//   1. el1_sync: 读取 ESR_EL2 分析异常原因
//      - HVC64/HVC32 调用：检查是否为 SMCCC ARCH_WORKAROUND 调用
//        * ARM_SMCCC_ARCH_WORKAROUND_1: 快速返回 Guest（主机已应用变通方案）
//        * ARM_SMCCC_ARCH_WORKAROUND_2/3: 类似处理
//      - 其他原因：转发到 __guest_exit 处理
//   2. el1_irq/el1_fiq: 物理中断/快速中断，退出 Guest 交给主机处理
//   3. el1_error: SError 系统错误，退出 Guest 交给主机处理
//=============================================================================
el1_sync:				// Guest trapped into EL2

	mrs	x0, esr_el2
	ubfx	x0, x0, #ESR_ELx_EC_SHIFT, #ESR_ELx_EC_WIDTH
	cmp	x0, #ESR_ELx_EC_HVC64
	ccmp	x0, #ESR_ELx_EC_HVC32, #4, ne
	b.ne	el1_trap

	/*
	 * Fastest possible path for ARM_SMCCC_ARCH_WORKAROUND_1.
	 * The workaround has already been applied on the host,
	 * so let's quickly get back to the guest. We don't bother
	 * restoring x1, as it can be clobbered anyway.
	 */
	ldr	x1, [sp]				// Guest's x0
	eor	w1, w1, #ARM_SMCCC_ARCH_WORKAROUND_1
	cbz	w1, wa_epilogue

	/* ARM_SMCCC_ARCH_WORKAROUND_2 handling */
	eor	w1, w1, #(ARM_SMCCC_ARCH_WORKAROUND_1 ^ \
			  ARM_SMCCC_ARCH_WORKAROUND_2)
	cbz	w1, wa_epilogue

	eor	w1, w1, #(ARM_SMCCC_ARCH_WORKAROUND_2 ^ \
			  ARM_SMCCC_ARCH_WORKAROUND_3)
	cbnz	w1, el1_trap

wa_epilogue:
	mov	x0, xzr
	add	sp, sp, #16
	eret
	sb

el1_trap:
	get_vcpu_ptr	x1, x0
	mov	x0, #ARM_EXCEPTION_TRAP
	b	__guest_exit

el1_irq:
el1_fiq:
	get_vcpu_ptr	x1, x0
	mov	x0, #ARM_EXCEPTION_IRQ
	b	__guest_exit

el1_error:
	get_vcpu_ptr	x1, x0
	mov	x0, #ARM_EXCEPTION_EL1_SERROR
	b	__guest_exit

//=============================================================================
// EL2 异常处理入口 — EL2 自身异常处理
// el2_sync: EL2 同步异常
//   - 检查是否为非法异常返回（spsr_el2 bit 20 = IL 位）
//   - 如果是 IL 异常：尝试恢复，通知主机发生非法异常返回
//   - 其他情况：调用 kvm_unexpected_el2_exception 处理
// el2_error: EL2 SError 错误
//   - 调用 kvm_unexpected_el2_exception 记录并处理
//=============================================================================

el2_sync:
	/* Check for illegal exception return */
	mrs	x0, spsr_el2
	tbnz	x0, #20, 1f

	save_caller_saved_regs_vect
	stp     x29, x30, [sp, #-16]!
	bl	kvm_unexpected_el2_exception
	ldp     x29, x30, [sp], #16
	restore_caller_saved_regs_vect

	eret

1:
	/* Let's attempt a recovery from the illegal exception return */
	get_vcpu_ptr	x1, x0
	mov	x0, #ARM_EXCEPTION_IL
	b	__guest_exit


el2_error:
	save_caller_saved_regs_vect
	stp     x29, x30, [sp, #-16]!

	bl	kvm_unexpected_el2_exception

	ldp     x29, x30, [sp], #16
	restore_caller_saved_regs_vect

	eret
	sb

//=============================================================================
// invalid_vector 宏 — 为不应该发生的异常生成简单入口（直接跳转到 panic）
// 这些向量在正常操作中永远不会被触发。如果触发，表明系统出现严重错误
// 覆盖的无效向量：EL2t 的所有四种异常 + EL2h 的 IRQ 和 FIQ
//=============================================================================

.macro invalid_vector	label, target = __guest_exit_panic
	.align	2
SYM_CODE_START_LOCAL(\label)
	b \target
SYM_CODE_END(\label)
.endm

	/* None of these should ever happen */
	invalid_vector	el2t_sync_invalid
	invalid_vector	el2t_irq_invalid
	invalid_vector	el2t_fiq_invalid
	invalid_vector	el2t_error_invalid
	invalid_vector	el2h_irq_invalid
	invalid_vector	el2h_fiq_invalid

	.ltorg

//=============================================================================
// 向量入口生成宏 — valid_vect / invalid_vect / check_preamble_length
//
// check_preamble_length: 编译时验证向量入口前导序列（preamble）大小
//   kvm_patch_vector_branch() 在运行时替换前导序列的跳转目标，
//   需要确保前导长度与预期一致
//
// valid_vect: 生成有效向量入口
//   序列：esb → 保存 x0/x1 → bti j → 跳转
//   - esb: 错误同步屏障，用于 RAS（可靠性、可用性、可服务性）
//   - bti j: 分支目标识别，用于控制流完整性（CFI）
//
// invalid_vect: 生成无效（不应被触发）向量的入口
//   序列：nop → 保存 x0/x1 → bti j → 跳转
//   与 valid_vect 的区别仅在于 esb 被替换为 nop
//=============================================================================

	.align 11

.macro check_preamble_length start, end
/* kvm_patch_vector_branch() generates code that jumps over the preamble. */
.if ((\end-\start) != KVM_VECTOR_PREAMBLE)
	.error "KVM vector preamble length mismatch"
.endif
.endm

.macro valid_vect target
	.align 7
661:
	esb
	stp	x0, x1, [sp, #-16]!
662:
	/*
	 * spectre vectors __bp_harden_hyp_vecs generate br instructions at runtime
	 * that jump at offset 8 at __kvm_hyp_vector.
	 * As hyp .text is guarded section, it needs bti j.
	 */
	bti j
	b	\target

check_preamble_length 661b, 662b
.endm

.macro invalid_vect target
	.align 7
661:
	nop
	stp	x0, x1, [sp, #-16]!
662:
	/* Check valid_vect */
	bti j
	b	\target

check_preamble_length 661b, 662b
.endm

//=============================================================================
// __kvm_hyp_vector — KVM Hypervisor 主异常向量表
//
// AArch64 异常向量表布局（从低地址到高地址，每个向量 0x80 字节对齐）：
//   偏移 0x000:  EL2t 同步（使用 SP_EL0 的 EL2 -> 无效，不应发生）
//   偏移 0x080:  EL2t IRQ     （无效）
//   偏移 0x100:  EL2t FIQ     （无效）
//   偏移 0x180:  EL2t Error   （无效）
//   偏移 0x200:  EL2h 同步（使用 SP_ELx 的 EL2 -> 有效，处理 EL2 异常）
//   偏移 0x280:  EL2h IRQ     （无效）
//   偏移 0x300:  EL2h FIQ     （无效）
//   偏移 0x380:  EL2h Error   （有效，处理 EL2 SError）
//   偏移 0x400:  EL1 64-bit 同步（有效，Guest 陷入处理）
//   偏移 0x480:  EL1 64-bit IRQ（有效，物理中断）
//   偏移 0x500:  EL1 64-bit FIQ（有效）
//   偏移 0x580:  EL1 64-bit Error（有效，SError）
//   偏移 0x600:  EL1 32-bit 同步（有效）
//   偏移 0x680:  EL1 32-bit IRQ（有效）
//   偏移 0x700:  EL1 32-bit FIQ（有效）
//   偏移 0x780:  EL1 32-bit Error（有效）
//=============================================================================

SYM_CODE_START(__kvm_hyp_vector)
	invalid_vect	el2t_sync_invalid	// Synchronous EL2t
	invalid_vect	el2t_irq_invalid	// IRQ EL2t
	invalid_vect	el2t_fiq_invalid	// FIQ EL2t
	invalid_vect	el2t_error_invalid	// Error EL2t

	valid_vect	el2_sync		// Synchronous EL2h
	invalid_vect	el2h_irq_invalid	// IRQ EL2h
	invalid_vect	el2h_fiq_invalid	// FIQ EL2h
	valid_vect	el2_error		// Error EL2h

	valid_vect	el1_sync		// Synchronous 64-bit EL1
	valid_vect	el1_irq			// IRQ 64-bit EL1
	valid_vect	el1_fiq			// FIQ 64-bit EL1
	valid_vect	el1_error		// Error 64-bit EL1

	valid_vect	el1_sync		// Synchronous 32-bit EL1
	valid_vect	el1_irq			// IRQ 32-bit EL1
	valid_vect	el1_fiq			// FIQ 32-bit EL1
	valid_vect	el1_error		// Error 32-bit EL1
SYM_CODE_END(__kvm_hyp_vector)

//=============================================================================
// Spectre v2 / BHB 缓解措施 — 分支预测加固向量
//
// spectrev2_smccc_wa1_smc:
//   通过 SMC 调用固件执行 Spectre v2 变通方案（ARCH_WORKAROUND_1/3）
//   - 保存 x0~x3 到栈
//   - 使用 alternative_cb 机制在运行时修补 WORKAROUND_3 值
//   - 执行 SMC #0 请求固件处理
//   - 恢复 x0, x2, x3（x1 可以被固件篡改）
//
// hyp_ventry: 生成 bp hardening 向量表的单个入口
//   参数：
//     indirect: 是否支持间接向量（Spectre v3a 变通方案）
//     spectrev2: 是否支持 Spectre v2 SMCCC 变通方案
//   当 indirect=1 时，NOP 序列在运行时被替换为直接跳转代码，
//   避免使用容易被 Spectre v3a 攻击的间接分支
//=============================================================================

.macro spectrev2_smccc_wa1_smc
	sub	sp, sp, #(8 * 4)
	stp	x2, x3, [sp, #(8 * 0)]
	stp	x0, x1, [sp, #(8 * 2)]
	alternative_cb ARM64_ALWAYS_SYSTEM, spectre_bhb_patch_wa3
	/* Patched to mov WA3 when supported */
	mov	w0, #ARM_SMCCC_ARCH_WORKAROUND_1
	alternative_cb_end
	smc	#0
	ldp	x2, x3, [sp, #(8 * 0)]
	add	sp, sp, #(8 * 2)
.endm

.macro hyp_ventry	indirect, spectrev2
	.align	7
1:	esb
	.if \spectrev2 != 0
	spectrev2_smccc_wa1_smc
	.else
	stp	x0, x1, [sp, #-16]!
	mitigate_spectre_bhb_loop	x0
	mitigate_spectre_bhb_clear_insn
	.endif
	.if \indirect != 0
	alternative_cb ARM64_ALWAYS_SYSTEM, kvm_patch_vector_branch
	/*
	 * For ARM64_SPECTRE_V3A configurations, these NOPs get replaced with:
	 *
	 * movz	x0, #(addr & 0xffff)
	 * movk	x0, #((addr >> 16) & 0xffff), lsl #16
	 * movk	x0, #((addr >> 32) & 0xffff), lsl #32
	 * br	x0
	 *
	 * Where:
	 * addr = kern_hyp_va(__kvm_hyp_vector) + vector-offset + KVM_VECTOR_PREAMBLE.
	 * See kvm_patch_vector_branch for details.
	 */
	nop
	nop
	nop
	nop
	alternative_cb_end
	.endif
	b	__kvm_hyp_vector + (1b - 0b + KVM_VECTOR_PREAMBLE)
.endm

.macro generate_vectors	indirect, spectrev2
0:
	.rept 16
	hyp_ventry	\indirect, \spectrev2
	.endr
	.org 0b + SZ_2K		// Safety measure
.endm

//=============================================================================
// __bp_harden_hyp_vecs — 分支预测加固的 HYP 向量表
//
// 包含三组向量（每组 16 个入口，对应 16 个异常类型）：
//   1. HYP_VECTOR_SPECTRE_DIRECT:  直接向量 + Spectre v2 SMCCC
//   2. HYP_VECTOR_INDIRECT:        间接向量（运行时修补为直接跳转）
//   3. HYP_VECTOR_SPECTRE_INDIRECT: 间接向量 + Spectre v2 SMCCC
//
// 每组通过 generate_vectors 宏生成 16 个 hyp_ventry 入口，
// 总共占用 __BP_HARDEN_HYP_VECS_SZ 字节空间。
// 运行时根据 CPU 特性选择合适的向量组。
//=============================================================================

	.align	11
SYM_CODE_START(__bp_harden_hyp_vecs)
	generate_vectors indirect = 0, spectrev2 = 1 // HYP_VECTOR_SPECTRE_DIRECT
	generate_vectors indirect = 1, spectrev2 = 0 // HYP_VECTOR_INDIRECT
	generate_vectors indirect = 1, spectrev2 = 1 // HYP_VECTOR_SPECTRE_INDIRECT
1:	.org __bp_harden_hyp_vecs + __BP_HARDEN_HYP_VECS_SZ
	.org 1b
SYM_CODE_END(__bp_harden_hyp_vecs)
