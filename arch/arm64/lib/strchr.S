/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/lib/strchr.S
 *
 * Copyright (C) 1995-2000 Russell King
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Find the first occurrence of a character in a string.
 *
 * Parameters:
 *	x0 - str
 *	x1 - c
 * Returns:
 *	x0 - address of first occurrence of 'c' or 0
 *
 * 中文说明：
 * 功能：在字符串中查找第一个匹配的字符（标准 C 库 strchr 函数）。
 * 参数：x0 - 字符串起始地址，x1 - 要查找的字符。
 * 返回：x0 - 第一个匹配字符的地址，若未找到则返回 0。
 * 算法：简单的逐字节扫描，每次加载 1 字节进行比较。
 *   使用 ccmp（条件比较）将字符匹配和 NUL 终止符检查合并为一条分支，
 *   减少分支指令数量。
 * 注意：这是一个简单、紧凑的实现，未使用字级并行的优化技术。
 */
SYM_FUNC_START(__pi_strchr)
	// 中文：主循环 — 逐字节扫描，ccmp 合并字符匹配和 NUL 终止两个检查条件
and	w1, w1, #0xff
1:	ldrb	w2, [x0], #1
	cmp	w2, w1
	ccmp	w2, wzr, #4, ne
	b.ne	1b
	sub	x0, x0, #1
	cmp	w2, w1
	csel	x0, x0, xzr, eq
	ret
SYM_FUNC_END(__pi_strchr)

SYM_FUNC_ALIAS_WEAK(strchr, __pi_strchr)
EXPORT_SYMBOL_NOKASAN(strchr)
