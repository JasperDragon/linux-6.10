/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/lib/strrchr.S
 *
 * Copyright (C) 1995-2000 Russell King
 * Copyright (C) 2013 ARM Ltd.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/*
 * Find the last occurrence of a character in a string.
 *
 * Parameters:
 *	x0 - str
 *	x1 - c
 * Returns:
 *	x0 - address of last occurrence of 'c' or 0
 *
 * 中文说明：
 * 功能：在字符串中查找最后一个匹配的字符（标准 C 库 strrchr 函数）。
 * 参数：x0 - 字符串起始地址，x1 - 要查找的字符。
 * 返回：x0 - 最后一个匹配字符的地址，若未找到则返回 0。
 * 算法：使用 x3（初始化为 0）保存最后一次匹配的位置。
 *   逐字节遍历整个字符串，每次匹配到目标字符时更新 x3。
 *   遇到 NUL 终止符后，返回 x3 中保存的最后一个匹配位置。
 * 注意：这是一个简单的逐字节实现，完整扫描整个字符串以确保找到最后一个匹配。
 */
SYM_FUNC_START(__pi_strrchr)
	mov	x3, #0
	and	w1, w1, #0xff
1:	ldrb	w2, [x0], #1
	cbz	w2, 2f
	cmp	w2, w1
	b.ne	1b
	sub	x3, x0, #1
	b	1b
2:	mov	x0, x3
	ret
SYM_FUNC_END(__pi_strrchr)
SYM_FUNC_ALIAS_WEAK(strrchr, __pi_strrchr)
EXPORT_SYMBOL_NOKASAN(strrchr)
