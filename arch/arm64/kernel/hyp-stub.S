/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hypervisor stub
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author:	Marc Zyngier <marc.zyngier@arm.com>
 */

/*
 * ============================================================================
 * hyp-stub.S — ARM64 Hypervisor Stub（管理程序桩代码）
 * ============================================================================
 *
 * 本文件实现内核启动早期安装在 EL2 的 "桩" 异常向量表。
 * 在 KVM 正式接管之前，此桩代码处理所有 EL2 异常。
 *
 * 【架构角色】
 *
 *   内核启动时运行在 EL2（如果 CPU 支持 EL2），通过此桩代码：
 *   1. 设置最小化的 EL2 异常向量表（大部分向量为 infinite loop）
 *   2. 仅处理 HVC 调用（通过 elx_sync 向量）
 *   3. 支持切换到 VHE（Virtualization Host Extensions）模式
 *   4. 最终被 KVM 的完整异常向量表替换
 *
 * 【异常向量表布局】：.align 11（2KB 对齐，硬件要求）
 *
 *   VBAR_EL2 地址     SP    目标异常级别    向量
 *   ─────────────  ─────  ─────────────  ─────
 *   +0x000         EL2t  当前 EL (SP0)   el2_sync_invalid   — 无效（无限循环）
 *   +0x080         EL2t  当前 EL (SP0)   el2_irq_invalid
 *   +0x100         EL2t  当前 EL (SP0)   el2_fiq_invalid
 *   +0x180         EL2t  当前 EL (SP0)   el2_error_invalid
 *   +0x200         EL2h  当前 EL (SPx)   elx_sync            — 唯一有效的HVC入口！
 *   +0x280         EL2h  当前 EL (SPx)   el2_irq_invalid
 *   +0x300         EL2h  当前 EL (SPx)   el2_fiq_invalid
 *   +0x380         EL2h  当前 EL (SPx)   el2_error_invalid
 *   +0x400         EL1   低 EL (AArch64) elx_sync
 *   +0x480         EL1   低 EL (AArch64) el1_irq_invalid
 *   +0x500         EL1   低 EL (AArch64) el1_fiq_invalid
 *   +0x580         EL1   低 EL (AArch64) el1_error_invalid
 *   +0x600         EL1   低 EL (AArch32) 全部 invalid
 *   +0x700         EL1   低 EL (AArch32) 全部 invalid
 *
 * 【elx_sync — HVC 服务调度器】
 *
 *   根据 x0 中的命令码执行不同操作：
 *   ┌──────────────────────┬─────────────────────────────────────────┐
 *   │ HVC_SET_VECTORS      │ 设置 VBAR_EL2（安装 KVM 向量表）       │
 *   │ HVC_FINALISE_EL2     │ 切换到 VHE 模式（见 __finalise_el2）    │
 *   │ HVC_GET_ICH_VTR_EL2  │ 读取 GIC 虚拟化能力寄存器              │
 *   │ HVC_SOFT_RESTART     │ 软重启 CPU（kexec 使用）               │
 *   │ HVC_RESET_VECTORS    │ 重置向量表（目前为空操作）             │
 *   │ 其他                 │ 返回 HVC_STUB_ERR（kvm_call_hyp 滥用）│
 *   └──────────────────────┴─────────────────────────────────────────┘
 *
 * 【__finalise_el2 — VHE 启用流程】
 *
 *   1. 开启 EL2 stage-1 MMU（使用 EL1 的页表/MAIR/TCR 映射）
 *   2. 将 CPACR_EL1 (FP)、VBAR_EL1 (EL1 异常向量) 传递到 EL2
 *   3. 保持 SPE/TRBE 在 EL2 运行，禁止 EL1 访问
 *   4. 禁用 EL1 stage-1 MMU（EL1 不再需要，因为 host 运行在 EL2）
 *   5. 通过修改 SPSR_EL1 的 mode 位 hack 异常返回，从 EL2 继续执行
 *
 *   VHE 模式下 Host 运行在 EL2，Guest 运行在 EL1/EL0，
 *   避免了传统的 nVHE 模式中 Host ↔ Guest 切换的开销。
 *
 * 【__hyp_set_vectors / __hyp_reset_vectors】
 *
 *   这两个函数通过 HVC_SET_VECTORS / HVC_RESET_VECTORS 从 EL1 发起
 *   对 VBAR_EL2 的操作（EL1 不能直接写 EL2 系统寄存器）。
 */

#include <linux/init.h>
#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/el2_setup.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/ptrace.h>
#include <asm/virt.h>

	.text
	.pushsection	.hyp.text, "ax"

	.align 11

SYM_CODE_START(__hyp_stub_vectors)
	ventry	el2_sync_invalid		// Synchronous EL2t
	ventry	el2_irq_invalid			// IRQ EL2t
	ventry	el2_fiq_invalid			// FIQ EL2t
	ventry	el2_error_invalid		// Error EL2t

	ventry	elx_sync			// Synchronous EL2h
	ventry	el2_irq_invalid			// IRQ EL2h
	ventry	el2_fiq_invalid			// FIQ EL2h
	ventry	el2_error_invalid		// Error EL2h

	ventry	elx_sync			// Synchronous 64-bit EL1
	ventry	el1_irq_invalid			// IRQ 64-bit EL1
	ventry	el1_fiq_invalid			// FIQ 64-bit EL1
	ventry	el1_error_invalid		// Error 64-bit EL1

	ventry	el1_sync_invalid		// Synchronous 32-bit EL1
	ventry	el1_irq_invalid			// IRQ 32-bit EL1
	ventry	el1_fiq_invalid			// FIQ 32-bit EL1
	ventry	el1_error_invalid		// Error 32-bit EL1
SYM_CODE_END(__hyp_stub_vectors)

	.align 11

SYM_CODE_START_LOCAL(elx_sync)
	cmp	x0, #HVC_SET_VECTORS
	b.ne	1f
	msr	vbar_el2, x1
	b	9f

1:	cmp	x0, #HVC_FINALISE_EL2
	b.eq	__finalise_el2

	cmp	x0, #HVC_GET_ICH_VTR_EL2
	b.ne	2f
	mrs_s	x1, SYS_ICH_VTR_EL2
	b	9f

2:	cmp	x0, #HVC_SOFT_RESTART
	b.ne	3f
	mov	x0, x2
	mov	x2, x4
	mov	x4, x1
	mov	x1, x3
	br	x4				// no return

3:	cmp	x0, #HVC_RESET_VECTORS
	beq	9f				// Nothing to reset!

	/* Someone called kvm_call_hyp() against the hyp-stub... */
	mov_q	x0, HVC_STUB_ERR
	eret

9:	mov	x0, xzr
	eret
SYM_CODE_END(elx_sync)

SYM_CODE_START_LOCAL(__finalise_el2)
	finalise_el2_state

	// nVHE? No way! Give me the real thing!
	// Sanity check: MMU *must* be off
	mrs	x1, sctlr_el2
	tbnz	x1, #0, 1f

	// Needs to be VHE capable, obviously
	check_override id_aa64mmfr1 ID_AA64MMFR1_EL1_VH_SHIFT 0f 1f x1 x2

0:	// Check whether we only want the hypervisor to run VHE, not the kernel
	adr_l	x1, arm64_sw_feature_override
	ldr	x2, [x1, FTR_OVR_VAL_OFFSET]
	ldr	x1, [x1, FTR_OVR_MASK_OFFSET]
	and	x2, x2, x1
	ubfx	x2, x2, #ARM64_SW_FEATURE_OVERRIDE_HVHE, #4
	cbz	x2, 2f

1:	mov_q	x0, HVC_STUB_ERR
	eret
2:
	// Engage the VHE magic!
	mov_q	x0, HCR_HOST_VHE_FLAGS
	msr_hcr_el2 x0

	// Use the EL1 allocated stack, per-cpu offset
	mrs	x0, sp_el1
	mov	sp, x0
	mrs	x0, tpidr_el1
	msr	tpidr_el2, x0

	// FP configuration, vectors
	mrs_s	x0, SYS_CPACR_EL12
	msr	cpacr_el1, x0
	mrs_s	x0, SYS_VBAR_EL12
	msr	vbar_el1, x0

	// Use EL2 translations for SPE & TRBE and disable access from EL1
	mrs	x0, mdcr_el2
	bic	x0, x0, #MDCR_EL2_E2PB_MASK
	bic	x0, x0, #MDCR_EL2_E2TB_MASK
	msr	mdcr_el2, x0

	// Transfer the MM state from EL1 to EL2
	mrs_s	x0, SYS_TCR_EL12
	msr	tcr_el1, x0
	mrs_s	x0, SYS_TTBR0_EL12
	msr	ttbr0_el1, x0
	mrs_s	x0, SYS_TTBR1_EL12
	msr	ttbr1_el1, x0
	mrs_s	x0, SYS_MAIR_EL12
	msr	mair_el1, x0
	mrs	x1, REG_ID_AA64MMFR3_EL1
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_TCRX_SHIFT, #4
	cbz	x1, .Lskip_tcr2
	mrs	x0, REG_TCR2_EL12
	msr	REG_TCR2_EL1, x0

	// Transfer permission indirection state
	mrs	x1, REG_ID_AA64MMFR3_EL1
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_S1PIE_SHIFT, #4
	cbz	x1, .Lskip_indirection
	mrs	x0, REG_PIRE0_EL12
	msr	REG_PIRE0_EL1, x0
	mrs	x0, REG_PIR_EL12
	msr	REG_PIR_EL1, x0

.Lskip_indirection:
.Lskip_tcr2:

	isb

	// Hack the exception return to stay at EL2
	mrs	x0, spsr_el1
	and	x0, x0, #~PSR_MODE_MASK
	mov	x1, #PSR_MODE_EL2h
	orr	x0, x0, x1
	msr	spsr_el1, x0

	b	enter_vhe
SYM_CODE_END(__finalise_el2)

	// At the point where we reach enter_vhe(), we run with
	// the MMU off (which is enforced by __finalise_el2()).
	// We thus need to be in the idmap, or everything will
	// explode when enabling the MMU.

	.pushsection	.idmap.text, "ax"

SYM_CODE_START_LOCAL(enter_vhe)
	// Invalidate TLBs before enabling the MMU
	tlbi	vmalle1
	dsb	nsh
	isb

	// Enable the EL2 S1 MMU, as set up from EL1
	mrs_s	x0, SYS_SCTLR_EL12
	set_sctlr_el1	x0

	// Disable the EL1 S1 MMU for a good measure
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	msr_s	SYS_SCTLR_EL12, x0

	mov	x0, xzr

	eret
SYM_CODE_END(enter_vhe)

	.popsection

.macro invalid_vector	label
SYM_CODE_START_LOCAL(\label)
	b \label
SYM_CODE_END(\label)
.endm

	invalid_vector	el2_sync_invalid
	invalid_vector	el2_irq_invalid
	invalid_vector	el2_fiq_invalid
	invalid_vector	el2_error_invalid
	invalid_vector	el1_sync_invalid
	invalid_vector	el1_irq_invalid
	invalid_vector	el1_fiq_invalid
	invalid_vector	el1_error_invalid

	.popsection

/*
 * __hyp_set_vectors: Call this after boot to set the initial hypervisor
 * vectors as part of hypervisor installation.  On an SMP system, this should
 * be called on each CPU.
 *
 * x0 must be the physical address of the new vector table, and must be
 * 2KB aligned.
 *
 * Before calling this, you must check that the stub hypervisor is installed
 * everywhere, by waiting for any secondary CPUs to be brought up and then
 * checking that is_hyp_mode_available() is true.
 *
 * If not, there is a pre-existing hypervisor, some CPUs failed to boot, or
 * something else went wrong... in such cases, trying to install a new
 * hypervisor is unlikely to work as desired.
 *
 * When you call into your shiny new hypervisor, sp_el2 will contain junk,
 * so you will need to set that to something sensible at the new hypervisor's
 * initialisation entry point.
 */

SYM_FUNC_START(__hyp_set_vectors)
	mov	x1, x0
	mov	x0, #HVC_SET_VECTORS
	hvc	#0
	ret
SYM_FUNC_END(__hyp_set_vectors)

SYM_FUNC_START(__hyp_reset_vectors)
	mov	x0, #HVC_RESET_VECTORS
	hvc	#0
	ret
SYM_FUNC_END(__hyp_reset_vectors)

/*
 * Entry point to finalise EL2 and switch to VHE if deemed capable
 *
 * w0: boot mode, as returned by init_kernel_el()
 */
SYM_FUNC_START(finalise_el2)
	// Need to have booted at EL2
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f

	// and still be at EL1
	mrs	x0, CurrentEL
	cmp	x0, #CurrentEL_EL1
	b.ne	1f

	mov	x0, #HVC_FINALISE_EL2
	hvc	#0
1:
	ret
SYM_FUNC_END(finalise_el2)
