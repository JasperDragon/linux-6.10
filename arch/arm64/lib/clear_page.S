/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/assembler.h>
#include <asm/page.h>

/*
 * Clear page @dest
 *
 * Parameters:
 *	x0 - dest
 *
 * 中文说明：
 * 功能：将一页（PAGE_SIZE 大小）内存清零。
 * 参数：x0 - 目标页面的起始地址（页对齐）。
 * 算法：提供三种清零路径：
 *   (1) MOPS 指令集扩展（ARMv8.8+）：使用 setpn/setmn/seten 三条顺序指令，
 *       由硬件高效完成大块内存清零。
 *   (2) DC ZVA（Data Cache Zero by Virtual Address）：通过缓存零地址指令，
 *       按缓存行大小逐行清零，性能最优。
 *   (3) 软件回退路径：使用 STNP 指令对（非暂存对）以 64 字节块清零，
 *       适用于不支持 DC ZVA 的场景。
 */
SYM_FUNC_START(__pi_clear_page)
#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
alternative_if_not ARM64_HAS_MOPS
	b	.Lno_mops
alternative_else_nop_endif

	mov	x1, #PAGE_SIZE
	setpn	[x0]!, x1!, xzr
	setmn	[x0]!, x1!, xzr
	seten	[x0]!, x1!, xzr
	ret
.Lno_mops:
#endif
	mrs	x1, dczid_el0
	tbnz	x1, #4, 2f	/* Branch if DC ZVA is prohibited */
	// 中文：若 dczid_el0.DZP == 1，表示 DC ZVA 指令被禁止，跳转到软件清零路径
	and	w1, w1, #0xf
	mov	x2, #4
	lsl	x1, x2, x1
	// 中文：解码缓存行大小，x1 = 4 << (dczid_el0[3:0])，得到实际缓存行字节数

1:	dc	zva, x0
	add	x0, x0, x1
	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b
	// 中文：DC ZVA 循环 — 每次将一个缓存行清零，遍历整个页面
	ret

2:	stnp	xzr, xzr, [x0]		// 软件回退：使用 STNP 每次写入 64 字节（4 对 xzr）
	stnp	xzr, xzr, [x0, #16]
	stnp	xzr, xzr, [x0, #32]
	stnp	xzr, xzr, [x0, #48]
	add	x0, x0, #64
	tst	x0, #(PAGE_SIZE - 1)
	b.ne	2b			// 循环直到整个页清零
	ret
SYM_FUNC_END(__pi_clear_page)
SYM_FUNC_ALIAS(clear_page, __pi_clear_page)
EXPORT_SYMBOL(clear_page)
