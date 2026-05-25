/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FP/SIMD state saving and restoring
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

/*
 * ============================================================================
 * entry-fpsimd.S — ARM64 FP/SIMD/SVE/SME 状态保存/恢复
 * ============================================================================
 *
 * 本文件提供浮点和向量扩展的上下文切换底层汇编函数。
 * 实际指令由 asm/fpsimdmacros.h 中的宏生成。
 *
 * 支持的处理扩展：
 * ┌───────────────────────────────────────────────────────────────┐
 * │ 扩展   │ 寄存器                  │ VL (向量长度)               │
 * ├───────────────────────────────────────────────────────────────┤
 * │ FPSIMD │ V0-V31 (128-bit)       │ 固定 128-bit               │
 * │ SVE    │ Z0-Z31 + P0-P15 + FFR  │ 128~2048-bit (VQ×128)     │
 * │ SME    │ ZA (矩阵存储) + ZT0     │ 同上                       │
 * └───────────────────────────────────────────────────────────────┘
 *
 * fpsimd_save_state / fpsimd_load_state:
 *   基础 FPSIMD/NEON 上下文切换，每次上下文切换都调用。
 *
 * sve_save_state / sve_load_state:
 *   SVE (Scalable Vector Extension) 上下文保存/恢复。
 *   参数 x2 控制 FFR (First Fault Register) 的保存。
 *
 * sve_get_vl / sve_set_vq:
 *   读取/设置 SVE 向量长度（VL = (VQ+1) × 16 字节）。
 *
 * sve_flush_live:
 *   将 Z 寄存器（除低 128-bit 外）和 P 寄存器清零。
 *   用于 exec() 时确保新进程不会泄漏前任的 SVE 数据。
 *
 * sme_save_state / sme_load_state:
 *   SME (Scalable Matrix Extension) ZA 和 ZT 状态保存/恢复。
 *   SME 新增 ZA 存储（2D 寄存器矩阵）和 ZT0（LUT 加速）。
 */

#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/fpsimdmacros.h>

/*
 * Save the FP registers.
 *
 * x0 - pointer to struct fpsimd_state
 */
SYM_FUNC_START(fpsimd_save_state)
	fpsimd_save x0, 8
	ret
SYM_FUNC_END(fpsimd_save_state)

/*
 * Load the FP registers.
 *
 * x0 - pointer to struct fpsimd_state
 */
SYM_FUNC_START(fpsimd_load_state)
	fpsimd_restore x0, 8
	ret
SYM_FUNC_END(fpsimd_load_state)

#ifdef CONFIG_ARM64_SVE

/*
 * Save the SVE state
 *
 * x0 - pointer to buffer for state
 * x1 - pointer to storage for FPSR
 * x2 - Save FFR if non-zero
 */
SYM_FUNC_START(sve_save_state)
	sve_save 0, x1, x2, 3
	ret
SYM_FUNC_END(sve_save_state)

/*
 * Load the SVE state
 *
 * x0 - pointer to buffer for state
 * x1 - pointer to storage for FPSR
 * x2 - Restore FFR if non-zero
 */
SYM_FUNC_START(sve_load_state)
	sve_load 0, x1, x2, 4
	ret
SYM_FUNC_END(sve_load_state)

SYM_FUNC_START(sve_get_vl)
	_sve_rdvl	0, 1
	ret
SYM_FUNC_END(sve_get_vl)

SYM_FUNC_START(sve_set_vq)
	sve_load_vq x0, x1, x2
	ret
SYM_FUNC_END(sve_set_vq)

/*
 * Zero all SVE registers but the first 128-bits of each vector
 *
 * VQ must already be configured by caller, any further updates of VQ
 * will need to ensure that the register state remains valid.
 *
 * x0 = include FFR?
 * x1 = VQ - 1
 */
SYM_FUNC_START(sve_flush_live)
	cbz		x1, 1f	// A VQ-1 of 0 is 128 bits so no extra Z state
	sve_flush_z
1:	sve_flush_p
	tbz		x0, #0, 2f
	sve_flush_ffr
2:	ret
SYM_FUNC_END(sve_flush_live)

#endif /* CONFIG_ARM64_SVE */

#ifdef CONFIG_ARM64_SME

SYM_FUNC_START(sme_get_vl)
	_sme_rdsvl	0, 1
	ret
SYM_FUNC_END(sme_get_vl)

SYM_FUNC_START(sme_set_vq)
	sme_load_vq x0, x1, x2
	ret
SYM_FUNC_END(sme_set_vq)

/*
 * Save the ZA and ZT state
 *
 * x0 - pointer to buffer for state
 * x1 - number of ZT registers to save
 */
SYM_FUNC_START(sme_save_state)
	_sme_rdsvl	2, 1		// x2 = VL/8
	sme_save_za 0, x2, 12		// Leaves x0 pointing to the end of ZA

	cbz	x1, 1f
	_str_zt 0
1:
	ret
SYM_FUNC_END(sme_save_state)

/*
 * Load the ZA and ZT state
 *
 * x0 - pointer to buffer for state
 * x1 - number of ZT registers to save
 */
SYM_FUNC_START(sme_load_state)
	_sme_rdsvl	2, 1		// x2 = VL/8
	sme_load_za 0, x2, 12		// Leaves x0 pointing to the end of ZA

	cbz	x1, 1f
	_ldr_zt 0
1:
	ret
SYM_FUNC_END(sme_load_state)

#endif /* CONFIG_ARM64_SME */
