/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
 *
 * Copyright (C) 2017-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <linux/linkage.h>

#include <asm/assembler.h>

// 中文说明：
// 128 位（TImode）整数移位操作函数，用于编译器生成 __int128 类型的移位操作。
// AArch64 架构原生只支持 64 位操作，128 位移位需要拆分为高/低 64 位处理。
// 所有函数使用统一接口：x0=低 64 位，x1=高 64 位，x2=移位位数。
// 当移位 < 64 时，数据在高低位之间交叉迁移；
// 当移位 >= 64 时，整体搬移到另一半，原位填 0 或符号位。

// __ashlti3：128 位算术左移
//   参数：x0=低 64 位，x1=高 64 位，x2=左移位数
//   返回：x0=结果低 64 位，x1=结果高 64 位
//   算法：shift<64 时低位左移+高位左移+低位溢出移入高位
//         shift>=64 时低位移到高位，低位补 0
SYM_FUNC_START(__ashlti3)
	cbz	x2, 1f
	mov	x3, #64
	sub	x3, x3, x2
	cmp	x3, #0
	b.le	2f
	// 中文：左移 < 64 位的情况
lsl	x1, x1, x2
	lsr	x3, x0, x3
	lsl	x2, x0, x2
	orr	x1, x1, x3
	mov	x0, x2
1:
	ret
2:
	neg	w1, w3
	mov	x2, #0
	lsl	x1, x0, x1
	mov	x0, x2
	ret
SYM_FUNC_END(__ashlti3)
EXPORT_SYMBOL(__ashlti3)

// __ashrti3：128 位算术右移（保留符号位）
SYM_FUNC_START(__ashrti3)
	cbz	x2, 1f
	mov	x3, #64
	sub	x3, x3, x2
	cmp	x3, #0
	b.le	2f
	lsr	x0, x0, x2
	lsl	x3, x1, x3
	asr	x2, x1, x2
	orr	x0, x0, x3
	mov	x1, x2
1:
	ret
2:
	neg	w0, w3
	// 中文：移位 >= 64：高位全符号位
asr	x2, x1, #63
	asr	x0, x1, x0
	mov	x1, x2
	ret
SYM_FUNC_END(__ashrti3)
EXPORT_SYMBOL(__ashrti3)

// __lshrti3：128 位逻辑右移（高位补 0）
SYM_FUNC_START(__lshrti3)
	cbz	x2, 1f
	mov	x3, #64
	sub	x3, x3, x2
	cmp	x3, #0
	b.le	2f
	lsr	x0, x0, x2
	lsl	x3, x1, x3
	lsr	x2, x1, x2
	orr	x0, x0, x3
	mov	x1, x2
1:
	ret
2:
	neg	w0, w3
	mov	x2, #0
	lsr	x0, x1, x0
	mov	x1, x2
	ret
SYM_FUNC_END(__lshrti3)
EXPORT_SYMBOL(__lshrti3)
