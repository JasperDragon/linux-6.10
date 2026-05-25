/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CPU reset routines
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2015 Huawei Futurewei Technologies.
 */

/*
 * ============================================================================
 * cpu-reset.S — ARM64 CPU 软复位（kexec 使用）
 * ============================================================================
 *
 * cpu_soft_restart(el2_switch, entry, arg0, arg1, arg2)：
 *   将 CPU 恢复到类似硬件复位的状态，然后跳转到新的内核入口。
 *
 * 执行流程（运行在 idmap 映射下，因为将关闭 MMU）：
 *   1. 关闭 MMU：写 INIT_SCTLR_EL1_MMU_OFF 到 SCTLR_EL1
 *      （禁用 EL1/EL0 stage-1 地址翻译，或 EL2 的 E2H=1 时禁用 EL2/EL0）
 *   2. ISB 同步：确保 MMU 关闭操作完成
 *   3. 若 el2_switch=1：通过 HVC_SOFT_RESTART 进入 EL2 再复位
 *      （hyp-stub.S 的 elx_sync 处理此 HVC，直接跳转到 entry）
 *   4. 若 el2_switch=0：直接 br x8 跳转到 entry(arg0, arg1, arg2)
 *
 * 典型使用场景：kexec 跳转到新内核时，需要 CPU 处于"刚复位"状态。
 */

#include <linux/linkage.h>
#include <linux/cfi_types.h>
#include <asm/assembler.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

.text
.pushsection    .idmap.text, "a"

/*
 * cpu_soft_restart(el2_switch, entry, arg0, arg1, arg2)
 *
 * @el2_switch: Flag to indicate a switch to EL2 is needed.
 * @entry: Location to jump to for soft reset.
 * arg0: First argument passed to @entry. (relocation list)
 * arg1: Second argument passed to @entry.(physical kernel entry)
 * arg2: Third argument passed to @entry. (physical dtb address)
 *
 * Put the CPU into the same state as it would be if it had been reset, and
 * branch to what would be the reset vector. It must be executed with the
 * flat identity mapping.
 */
SYM_TYPED_FUNC_START(cpu_soft_restart)
	mov_q	x12, INIT_SCTLR_EL1_MMU_OFF
	pre_disable_mmu_workaround
	/*
	 * either disable EL1&0 translation regime or disable EL2&0 translation
	 * regime if HCR_EL2.E2H == 1
	 */
	msr	sctlr_el1, x12
	isb

	cbz	x0, 1f				// el2_switch?
	mov	x0, #HVC_SOFT_RESTART
	hvc	#0				// no return

1:	mov	x8, x1				// entry
	mov	x0, x2				// arg0
	mov	x1, x3				// arg1
	mov	x2, x4				// arg2
	br	x8
SYM_FUNC_END(cpu_soft_restart)

.popsection
