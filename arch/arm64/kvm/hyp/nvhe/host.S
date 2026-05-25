/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 - Google Inc
 * Author: Andrew Scull <ascull@google.com>
 */

//=============================================================================
// host.S — nVHE（非虚拟化扩展）主机异常处理与上下文管理
//
// 本文件实现 KVM 在 nVHE 模式（即 VHE 不可用时）下 EL2 异常处理逻辑。
// nVHE 模式下主机和 Guest 处于不同的异常级别，需要显式保存/恢复上下文。
//
// 主要功能包括：
//   - __host_exit: 从 Guest 退出到主机时保存主机上下文并处理陷阱
//   - __host_enter: 重新进入主机上下文
//   - __hyp_do_panic: Hyp 状态下的 panic 处理（准备参数后交给主机）
//   - __host_hvc: 处理主机发起的 HVC 调用（含 stub 调用转发）
//   - __kvm_hyp_host_vector: 主机异常向量表
//   - __kvm_hyp_host_forward_smc: 将 SMC 调用转发给安全监控器
//
// ptrauth（指针认证）处理：
//   在受保护模式下（ARM64_KVM_PROTECTED_MODE），需要在主机和 hyp 之间
//   切换 ptrauth key，因为两者的地址空间使用了不同的 key
//=============================================================================

#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_ptrauth.h>

	.text

//=============================================================================
// __host_exit — 从 Guest 退出到主机时的上下文保存和陷阱处理
//
// 此函数在每次从 Guest 模式退出到 EL2（由于异常或中断）时被调用。
// 负责：
//   1. 获取主机上下文结构体指针
//   2. 将主机的寄存器状态保存到 kvm_cpu_context 结构中
//   3. 处理 ptrauth（指针认证）key 的切换（受保护模式下）
//   4. 调用 handle_trap() 处理具体异常原因
//   5. 陷阱处理完成后，通过子标签恢复主机状态
//
// 子标签入口点：
//   __host_enter_restore_full:       完全恢复后返回主机
//   __host_enter_for_panic:          panic 场景（x0-7 可作参数）
//   __host_enter_without_restoring:  直接 ERET 不恢复（最简路径）
//=============================================================================

SYM_FUNC_START(__host_exit)
	get_host_ctxt	x0, x1

	/* Store the host regs x2 and x3 */
	stp	x2, x3,   [x0, #CPU_XREG_OFFSET(2)]

	/* Retrieve the host regs x0-x1 from the stack */
	ldp	x2, x3, [sp], #16	// x0, x1

	/* Store the host regs x0-x1 and x4-x17 */
	stp	x2, x3,   [x0, #CPU_XREG_OFFSET(0)]
	stp	x4, x5,   [x0, #CPU_XREG_OFFSET(4)]
	stp	x6, x7,   [x0, #CPU_XREG_OFFSET(6)]
	stp	x8, x9,   [x0, #CPU_XREG_OFFSET(8)]
	stp	x10, x11, [x0, #CPU_XREG_OFFSET(10)]
	stp	x12, x13, [x0, #CPU_XREG_OFFSET(12)]
	stp	x14, x15, [x0, #CPU_XREG_OFFSET(14)]
	stp	x16, x17, [x0, #CPU_XREG_OFFSET(16)]

	/* Store the host regs x18-x29, lr */
	save_callee_saved_regs x0

	/* Save the host context pointer in x29 across the function call */
	mov	x29, x0

#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL
alternative_if_not ARM64_HAS_ADDRESS_AUTH
b __skip_pauth_save
alternative_else_nop_endif

alternative_if ARM64_KVM_PROTECTED_MODE
	/* Save kernel ptrauth keys. */
	add x18, x29, #CPU_APIAKEYLO_EL1
	ptrauth_save_state x18, x19, x20

	/* Use hyp keys. */
	adr_this_cpu x18, kvm_hyp_ctxt, x19
	add x18, x18, #CPU_APIAKEYLO_EL1
	ptrauth_restore_state x18, x19, x20
	isb
alternative_else_nop_endif
__skip_pauth_save:
#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */

	bl	handle_trap

// __host_enter_restore_full — 完整恢复主机上下文并返回主机
// 这也是 __host_enter() 的入口点（通过 b 指令跳转过来）
// 恢复顺序：ptrauth keys → x0-x17 → x18-x29+lr → ERET
__host_enter_restore_full:
	/* Restore kernel keys. */
#ifdef CONFIG_ARM64_PTR_AUTH_KERNEL
alternative_if_not ARM64_HAS_ADDRESS_AUTH
b __skip_pauth_restore
alternative_else_nop_endif

alternative_if ARM64_KVM_PROTECTED_MODE
	add x18, x29, #CPU_APIAKEYLO_EL1
	ptrauth_restore_state x18, x19, x20
alternative_else_nop_endif
__skip_pauth_restore:
#endif /* CONFIG_ARM64_PTR_AUTH_KERNEL */

	/* Restore host regs x0-x17 */
	ldp	x0, x1,   [x29, #CPU_XREG_OFFSET(0)]
	ldp	x2, x3,   [x29, #CPU_XREG_OFFSET(2)]
	ldp	x4, x5,   [x29, #CPU_XREG_OFFSET(4)]
	ldp	x6, x7,   [x29, #CPU_XREG_OFFSET(6)]

	/* x0-7 are use for panic arguments */
__host_enter_for_panic:
	ldp	x8, x9,   [x29, #CPU_XREG_OFFSET(8)]
	ldp	x10, x11, [x29, #CPU_XREG_OFFSET(10)]
	ldp	x12, x13, [x29, #CPU_XREG_OFFSET(12)]
	ldp	x14, x15, [x29, #CPU_XREG_OFFSET(14)]
	ldp	x16, x17, [x29, #CPU_XREG_OFFSET(16)]

	/* Restore host regs x18-x29, lr */
	restore_callee_saved_regs x29

	/* Do not touch any register after this! */
__host_enter_without_restoring:
	eret
	sb
SYM_FUNC_END(__host_exit)

//=============================================================================
// __host_enter — 从 EL2 重新进入主机上下文
// 参数：x0 = struct kvm_cpu_context*（主机上下文指针）
// 直接将上下文指针保存在 x29 中，然后跳转到 __host_enter_restore_full
// 由该标签处完成完整的寄存器恢复和 ERET 返回主机
//=============================================================================
/*
 * void __noreturn __host_enter(struct kvm_cpu_context *host_ctxt);
 */
SYM_FUNC_START(__host_enter)
	mov	x29, x0
	b	__host_enter_restore_full
SYM_FUNC_END(__host_enter)

//=============================================================================
// __hyp_do_panic — Hyp 状态下的 panic 处理
// 参数：
//   x0 = struct kvm_cpu_context*（主机上下文，可为 NULL）
//   x1 = SPSR_EL2 值
//   x2 = ELR_EL2 值
//   x3 = PAR_EL1 值
//
// 功能：
//   1. 构造返回主机的异常帧：设置 SPSR（掩蔽所有异常，EL1h 模式）
//   2. 设置返回地址为 nvhe_hyp_panic_handler
//   3. 如果定义了 PKVM_DISABLE_STAGE2_ON_PANIC，禁用 stage-2 页表
//   4. 加载各种错误寄存器值（ESR、FAR、HPFAR）作为 panic 参数
//   5. 如果 host_ctxt 非空，恢复主机上下文后再 panic
//    否则直接 ERET（不恢复）
//=============================================================================
/*
 * void __noreturn __hyp_do_panic(struct kvm_cpu_context *host_ctxt, u64 spsr,
 * 				  u64 elr, u64 par);
 */
SYM_FUNC_START(__hyp_do_panic)
	/* Prepare and exit to the host's panic function. */
	mov	lr, #(PSR_F_BIT | PSR_I_BIT | PSR_A_BIT | PSR_D_BIT |\
		      PSR_MODE_EL1h)
	msr	spsr_el2, lr
	adr_l	lr, nvhe_hyp_panic_handler
	hyp_kimg_va lr, x6
	msr	elr_el2, lr

	mov	x29, x0

#ifdef PKVM_DISABLE_STAGE2_ON_PANIC
	/* Ensure host stage-2 is disabled */
	mrs	x0, hcr_el2
	bic	x0, x0, #HCR_VM
	msr_hcr_el2 x0
	tlbi	vmalls12e1
	dsb	nsh
#endif

	/* Load the panic arguments into x0-7 */
	mrs	x0, esr_el2
	mov	x4, x3
	mov	x3, x2
	hyp_pa	x3, x6
	get_vcpu_ptr x5, x6
	mrs	x6, far_el2
	mrs	x7, hpfar_el2

	/* Enter the host, conditionally restoring the host context. */
	cbz	x29, __host_enter_without_restoring
	b	__host_enter_for_panic
SYM_FUNC_END(__hyp_do_panic)

//=============================================================================
// __host_hvc — 处理主机发起的 HVC 调用
//
// 当主机执行 HVC 指令陷入 EL2 时，被 host_el1_sync_vect 调用。
// 逻辑流程：
//   1. 从栈中加载主机传递的 HVC 参数（x0, x1），暂不修复栈指针
//   2. 受保护模式下不支持 stub 调用，直接退出到 __host_exit
//   3. 否则检查是否为 stub HVC 调用（编号 < HVC_STUB_HCALL_NR）
//      - stub 调用：跳转到 idmap 地址的 __kvm_handle_stub_hvc
//      - 非 stub 调用：退出到 __host_exit
//=============================================================================
SYM_FUNC_START(__host_hvc)
	ldp	x0, x1, [sp]		// Don't fixup the stack yet

	/* No stub for you, sonny Jim */
alternative_if ARM64_KVM_PROTECTED_MODE
	b	__host_exit
alternative_else_nop_endif

	/* Check for a stub HVC call */
	cmp	x0, #HVC_STUB_HCALL_NR
	b.hs	__host_exit

	add	sp, sp, #16
	/*
	 * Compute the idmap address of __kvm_handle_stub_hvc and
	 * jump there.
	 *
	 * Preserve x0-x4, which may contain stub parameters.
	 */
	adr_l	x5, __kvm_handle_stub_hvc
	hyp_pa	x5, x6
	br	x5
SYM_FUNC_END(__host_hvc)

//=============================================================================
// 主机异常向量入口宏
//
// host_el1_sync_vect: 主机 EL1 同步异常向量的快速入口
//   检查 ESR_EL2 异常类：
//   - HVC64 调用 → __host_hvc
//   - 其他异常 → __host_exit（完整保存上下文）
//
// invalid_host_el2_vect: 不应触发的 EL2 异常入口
//   含栈溢出检测（利用 nVHE 栈对齐特性，NVHE_STACK_SHIFT 位应为 1）
//   栈有效 → hyp_panic / 栈溢出 → overflow_stack + hyp_panic_bad_stack
//
// invalid_host_el1_vect: 不应触发的 EL1 异常入口
//   收集异常现场（SPSR, ELR, PAR）→ __hyp_do_panic
//=============================================================================

.macro host_el1_sync_vect
	.align 7
.L__vect_start\@:
	stp	x0, x1, [sp, #-16]!
	mrs	x0, esr_el2
	ubfx	x0, x0, #ESR_ELx_EC_SHIFT, #ESR_ELx_EC_WIDTH
	cmp	x0, #ESR_ELx_EC_HVC64
	b.eq	__host_hvc
	b	__host_exit
.L__vect_end\@:
.if ((.L__vect_end\@ - .L__vect_start\@) > 0x80)
	.error "host_el1_sync_vect larger than vector entry"
.endif
.endm

.macro invalid_host_el2_vect
	.align 7

	/*
	 * Test whether the SP has overflowed, without corrupting a GPR.
	 * nVHE hypervisor stacks are aligned so that the NVHE_STACK_SHIFT bit
	 * of SP should always be 1.
	 */
	add	sp, sp, x0			// sp' = sp + x0
	sub	x0, sp, x0			// x0' = sp' - x0 = (sp + x0) - x0 = sp
	tbz	x0, #NVHE_STACK_SHIFT, .L__hyp_sp_overflow\@
	sub	x0, sp, x0			// x0'' = sp' - x0' = (sp + x0) - sp = x0
	sub	sp, sp, x0			// sp'' = sp' - x0 = (sp + x0) - x0 = sp

	/*
	 * The panic may not be clean if the exception is taken before the host
	 * context has been saved by __host_exit or after the hyp context has
	 * been partially clobbered by __host_enter.
	 */
	b	hyp_panic

.L__hyp_sp_overflow\@:
	/* Switch to the overflow stack */
	adr_this_cpu sp, overflow_stack + OVERFLOW_STACK_SIZE, x0

	b	hyp_panic_bad_stack
	ASM_BUG()
.endm

.macro invalid_host_el1_vect
	.align 7
	mov	x0, xzr		/* restore_host = false */
	mrs	x1, spsr_el2
	mrs	x2, elr_el2
	mrs	x3, par_el1
	b	__hyp_do_panic
.endm

/*
 * The host vector does not use an ESB instruction in order to avoid consuming
 * SErrors that should only be consumed by the host. Guest entry is deferred by
 * __guest_enter if there are any pending asynchronous exceptions so hyp will
 * always return to the host without having consumerd host SErrors.
 *
 * CONFIG_KVM_INDIRECT_VECTORS is not applied to the host vectors because the
 * host knows about the EL2 vectors already, and there is no point in hiding
 * them.
//=============================================================================
// __kvm_hyp_host_vector — nVHE 主机异常向量表
//
// 布局说明：
//   EL2t 和 EL2h 的所有异常 → invalid_host_el2_vect（含栈溢出检测）
//   EL1 同步异常 → host_el1_sync_vect（HVC 调用处理）
//   EL1 IRQ/FIQ/Error → invalid_host_el1_vect（直接 panic）
//
// 设计要点：
//   1. 不使用 ESB 指令：避免消耗应由主机处理的 SError
//      Guest 入口（__guest_enter）在有挂起异步异常时会推迟进入
//   2. 不应用 KVM_INDIRECT_VECTORS：主机已知 EL2 向量地址
//
// 向量布局（每个 0x80 字节对齐）：
//   0x000-0x180: EL2t 四种异常
//   0x200-0x300: EL2h 四种异常
//   0x400-0x580: EL1 64-bit 四种异常
//   0x600-0x780: EL1 32-bit 四种异常
//=============================================================================
 */
	.align 11
SYM_CODE_START(__kvm_hyp_host_vector)
	invalid_host_el2_vect			// Synchronous EL2t
	invalid_host_el2_vect			// IRQ EL2t
	invalid_host_el2_vect			// FIQ EL2t
	invalid_host_el2_vect			// Error EL2t

	invalid_host_el2_vect			// Synchronous EL2h
	invalid_host_el2_vect			// IRQ EL2h
	invalid_host_el2_vect			// FIQ EL2h
	invalid_host_el2_vect			// Error EL2h

	host_el1_sync_vect			// Synchronous 64-bit EL1/EL0
	invalid_host_el1_vect			// IRQ 64-bit EL1/EL0
	invalid_host_el1_vect			// FIQ 64-bit EL1/EL0
	invalid_host_el1_vect			// Error 64-bit EL1/EL0

	host_el1_sync_vect			// Synchronous 32-bit EL1/EL0
	invalid_host_el1_vect			// IRQ 32-bit EL1/EL0
	invalid_host_el1_vect			// FIQ 32-bit EL1/EL0
	invalid_host_el1_vect			// Error 32-bit EL1/EL0
SYM_CODE_END(__kvm_hyp_host_vector)

//=============================================================================
// __kvm_hyp_host_forward_smc — 将 SMC 调用转发给安全监控器（Secure Monitor）
//
// 从 kvm_cpu_context 结构中加载所有 SMCCC 参数寄存器（x0-x17），
// 执行 SMC #0 调用，然后将结果写回到同一个 context 结构。
//
// 注意：使用 x18 保存上下文指针，因为 x18 在 SMCCC 约定下是被调用者保存的，
// 但在 AAPCS64 中不是。这样标准 SMC 调用不会破坏上下文指针。
//
// 假设使用 SMCCC 1.2 或更早版本（x0-x17 传递参数）。
//=============================================================================
/*
 * Forward SMC with arguments in struct kvm_cpu_context, and
 * store the result into the same struct. Assumes SMCCC 1.2 or older.
 *
 * x0: struct kvm_cpu_context*
 */
SYM_CODE_START(__kvm_hyp_host_forward_smc)
	/*
	 * Use x18 to keep the pointer to the host context because
	 * x18 is callee-saved in SMCCC but not in AAPCS64.
	 */
	mov	x18, x0

	ldp	x0, x1,   [x18, #CPU_XREG_OFFSET(0)]
	ldp	x2, x3,   [x18, #CPU_XREG_OFFSET(2)]
	ldp	x4, x5,   [x18, #CPU_XREG_OFFSET(4)]
	ldp	x6, x7,   [x18, #CPU_XREG_OFFSET(6)]
	ldp	x8, x9,   [x18, #CPU_XREG_OFFSET(8)]
	ldp	x10, x11, [x18, #CPU_XREG_OFFSET(10)]
	ldp	x12, x13, [x18, #CPU_XREG_OFFSET(12)]
	ldp	x14, x15, [x18, #CPU_XREG_OFFSET(14)]
	ldp	x16, x17, [x18, #CPU_XREG_OFFSET(16)]

	smc	#0

	stp	x0, x1,   [x18, #CPU_XREG_OFFSET(0)]
	stp	x2, x3,   [x18, #CPU_XREG_OFFSET(2)]
	stp	x4, x5,   [x18, #CPU_XREG_OFFSET(4)]
	stp	x6, x7,   [x18, #CPU_XREG_OFFSET(6)]
	stp	x8, x9,   [x18, #CPU_XREG_OFFSET(8)]
	stp	x10, x11, [x18, #CPU_XREG_OFFSET(10)]
	stp	x12, x13, [x18, #CPU_XREG_OFFSET(12)]
	stp	x14, x15, [x18, #CPU_XREG_OFFSET(14)]
	stp	x16, x17, [x18, #CPU_XREG_OFFSET(16)]

	ret
SYM_CODE_END(__kvm_hyp_host_forward_smc)
