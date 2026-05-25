/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/smp.h>

	.text
/*
 * ARM64 CPU 挂起与恢复底层汇编
 *
 * 本文件实现了 CPU 挂起 (suspend) 和恢复 (resume) 的关键底层逻辑:
 *   1. __cpu_suspend_enter — 挂起前保存被调用方寄存器到 sleep_stack_data
 *   2. cpu_resume — 恢复时恒等映射入口, 重新初始化并开启 MMU
 *   3. _cpu_resume — MMU 开启后, 恢复上下文并返回
 *
 * 挂起流程:
 *   __cpu_suspend_enter 保存 x19~x28, x29, lr, sp
 *   → MPIDR 哈希找到 per-CPU 槽位
 *   → 调用 cpu_do_suspend (SoC 特定) → 返回 1
 *
 * 恢复流程:
 *   cpu_resume (idmap) → init_kernel_el → __cpu_setup
 *   → enable_mmu → _cpu_resume → 恢复上下文 → 返回 0
 */

/*
 * Implementation of MPIDR_EL1 hash algorithm through shifting
 * and OR'ing.
 *
 * @dst: register containing hash result
 * @rs0: register containing affinity level 0 bit shift
 * @rs1: register containing affinity level 1 bit shift
 * @rs2: register containing affinity level 2 bit shift
 * @rs3: register containing affinity level 3 bit shift
 * @mpidr: register containing MPIDR_EL1 value
 * @mask: register containing MPIDR mask
 *
 * Pseudo C-code:
 *
 *u32 dst;
 *
 *compute_mpidr_hash(u32 rs0, u32 rs1, u32 rs2, u32 rs3, u64 mpidr, u64 mask) {
 *	u32 aff0, aff1, aff2, aff3;
 *	u64 mpidr_masked = mpidr & mask;
 *	aff0 = mpidr_masked & 0xff;
 *	aff1 = mpidr_masked & 0xff00;
 *	aff2 = mpidr_masked & 0xff0000;
 *	aff3 = mpidr_masked & 0xff00000000;
 *	dst = (aff0 >> rs0 | aff1 >> rs1 | aff2 >> rs2 | aff3 >> rs3);
 *}
 * Input registers: rs0, rs1, rs2, rs3, mpidr, mask
 * Output register: dst
 * Note: input and output registers must be disjoint register sets
         (eg: a macro instance with mpidr = x1 and dst = x1 is invalid)
 */
	.macro compute_mpidr_hash dst, rs0, rs1, rs2, rs3, mpidr, mask
	and	\mpidr, \mpidr, \mask		// mask out MPIDR bits
	and	\dst, \mpidr, #0xff		// mask=aff0
	lsr	\dst ,\dst, \rs0		// dst=aff0>>rs0
	and	\mask, \mpidr, #0xff00		// mask = aff1
	lsr	\mask ,\mask, \rs1
	orr	\dst, \dst, \mask		// dst|=(aff1>>rs1)
	and	\mask, \mpidr, #0xff0000	// mask = aff2
	lsr	\mask ,\mask, \rs2
	orr	\dst, \dst, \mask		// dst|=(aff2>>rs2)
	and	\mask, \mpidr, #0xff00000000	// mask = aff3
	lsr	\mask ,\mask, \rs3
	orr	\dst, \dst, \mask		// dst|=(aff3>>rs3)
	.endm
/*
 * __cpu_suspend_enter: 挂起前保存 CPU 上下文
 *
 * 将当前 CPU 的被调用方保存寄存器保存到提供的 sleep_stack_data 区域，
 * 并将其位置发布到 sleep_save_stash (按 MPIDR 哈希索引)，供 cpu_resume 使用。
 *
 * 关键设计: 挂起/恢复路径的返回区分
 *   cpu_resume 恢复保存的状态后"返回"，由于 lr 也被保存/恢复了，
 *   看起来就像是从这个函数返回。调用者通过返回值区分:
 *     非零 → 正常挂起路径
 *     零   → 恢复路径
 *
 * 参数:
 *   x0 = struct sleep_stack_data 指针
 *
 * 保存顺序:
 *   SLEEP_STACK_DATA_CALLEE_REGS:
 *     x29, lr, x19~x28 (被调用方保存寄存器)
 *   SLEEP_STACK_DATA_SYSTEM_REGS:
 *     sp (栈指针)
 */
SYM_FUNC_START(__cpu_suspend_enter)
	stp	x29, lr, [x0, #SLEEP_STACK_DATA_CALLEE_REGS]
	stp	x19, x20, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+16]
	stp	x21, x22, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+32]
	stp	x23, x24, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+48]
	stp	x25, x26, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+64]
	stp	x27, x28, [x0,#SLEEP_STACK_DATA_CALLEE_REGS+80]

	/* 将 SP (栈指针) 保存到 cpu_suspend_ctx 中 */
	mov	x2, sp
	str	x2, [x0, #SLEEP_STACK_DATA_SYSTEM_REGS + CPU_CTX_SP]

	/* 读取 mpidr_hash 结构: 计算本 CPU 在 sleep_save_stash 中的索引 */
	ldr_l	x1, sleep_save_stash
	mrs	x7, mpidr_el1
	adr_l	x9, mpidr_hash
	ldr	x10, [x9, #MPIDR_HASH_MASK]
	/*
	 * Following code relies on the struct mpidr_hash
	 * members size.
	 */
	ldp	w3, w4, [x9, #MPIDR_HASH_SHIFTS]
	ldp	w5, w6, [x9, #(MPIDR_HASH_SHIFTS + 8)]
	compute_mpidr_hash x8, x3, x4, x5, x6, x7, x10
	add	x1, x1, x8, lsl #3

	str	x0, [x1]
	add	x0, x0, #SLEEP_STACK_DATA_SYSTEM_REGS
	stp	x29, lr, [sp, #-16]!
	bl	cpu_do_suspend
	ldp	x29, lr, [sp], #16
	mov	x0, #1
	ret
SYM_FUNC_END(__cpu_suspend_enter)

	.pushsection ".idmap.text", "a"
SYM_CODE_START(cpu_resume)
	mov	x0, xzr
	bl	init_kernel_el
	mov	x19, x0			// preserve boot mode
	bl	__cpu_setup
	/* 提前开启 MMU, 以便通过虚拟地址访问 sleep_save_stash */
	adrp	x1, swapper_pg_dir
	adrp	x2, idmap_pg_dir
	bl	__enable_mmu
	ldr	x8, =_cpu_resume
	br	x8
SYM_CODE_END(cpu_resume)
	.ltorg
	.popsection

SYM_FUNC_START(_cpu_resume)
	mov	x0, x19
	bl	finalise_el2

	mrs	x1, mpidr_el1
	adr_l	x8, mpidr_hash		/* x8 = struct mpidr_hash 虚拟地址 */

	/* 读取 mpidr_hash 的各位成员 (掩码 + 4 个移位量) */
	ldr	x2, [x8, #MPIDR_HASH_MASK]
	ldp	w3, w4, [x8, #MPIDR_HASH_SHIFTS]
	ldp	w5, w6, [x8, #(MPIDR_HASH_SHIFTS + 8)]
	compute_mpidr_hash x7, x3, x4, x5, x6, x1, x2

	/* x7 = 哈希索引, 用于从 sleep_save_stash 数组中取出上下文指针 */
	ldr_l	x0, sleep_save_stash
	ldr	x0, [x0, x7, lsl #3]
	add	x29, x0, #SLEEP_STACK_DATA_CALLEE_REGS
	add	x0, x0, #SLEEP_STACK_DATA_SYSTEM_REGS
	/* 从挂起时保存的上下文中恢复栈指针 SP */
	ldr	x2, [x0, #CPU_CTX_SP]
	mov	sp, x2
	/*
	 * cpu_do_resume 期望 x0 指向上下文地址 (包含 SoC 特定的系统寄存器)
	 */
	bl	cpu_do_resume

#if defined(CONFIG_KASAN) && defined(CONFIG_KASAN_STACK)
	mov	x0, sp
	bl	kasan_unpoison_task_stack_below	// KASAN: 取消栈底区域的毒化标记
#endif

	/* 从保存的上下文恢复被调用方保存寄存器 x19~x28, x29, lr */
	ldp	x19, x20, [x29, #16]
	ldp	x21, x22, [x29, #32]
	ldp	x23, x24, [x29, #48]
	ldp	x25, x26, [x29, #64]
	ldp	x27, x28, [x29, #80]
	ldp	x29, lr, [x29]
	mov	x0, #0
	ret
SYM_FUNC_END(_cpu_resume)
