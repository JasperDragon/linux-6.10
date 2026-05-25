/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 ARM Ltd.
 * Copyright (C) 2013 Linaro.
 *
 * This code is based on glibc cortex strings work originally authored by Linaro
 * be found @
 *
 * http://bazaar.launchpad.net/~linaro-toolchain-dev/cortex-strings/trunk/
 * files/head:/src/aarch64/
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/cache.h>

/*
 * Fill in the buffer with character c (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - buf
 *	x1 - c
 *	x2 - n
 * Returns:
 *	x0 - buf
 *
 * 中文说明：
 * 功能：将内存缓冲区填充为指定字符（标准 C 库 memset 函数）。
 * 参数：x0 - 缓冲区起始地址，x1 - 填充字符（取低 8 位），x2 - 填充字节数。
 * 返回：x0 - 缓冲区起始地址（不变）。
 * 算法：分为五层优化路径：
 *   (1) 小量填充（<=15 字节）：按 8/4/2/1 字节递次处理，无需对齐。
 *   (2) 对齐调整：对于 >=16 字节的填充，先将目标地址 16 字节对齐。
 *   (3) 中等填充（<64 字节）：按 48/32/16 字节递减处理，
 *       最后用 stp 从结尾写入 16 字节处理尾部。
 *   (4) 大量填充（>=64 字节）：主循环每次 64 字节，使用 stp 指令对。
 *   (5) 清零优化：当填充字符为 0 时，使用 DC ZVA（Data Cache Zero
 *       by Virtual Address）按缓存行高效清零。
 *   (6) MOPS 指令扩展（ARMv8.8+）：使用 setp/setm/sete 三条顺序指令
 *       由硬件高效完成填充。
 */

dstin		.req	x0
val_x		.req	x1
val		.req	w1
count		.req	x2
tmp1		.req	x3
tmp1w		.req	w3
tmp2		.req	x4
tmp2w		.req	w4
zva_len_x	.req	x5
zva_len		.req	w5
zva_bits_x	.req	x6

A_l		.req	x7
A_lw		.req	w7
dst		.req	x8
tmp3w		.req	w9
tmp3		.req	x9

SYM_FUNC_START_LOCAL(__pi_memset_generic)
	mov	dst, dstin	/* Preserve return value.  */
	// 中文：将 val 的低 8 位扩展到 64 位，使整个寄存器充满相同的字节值
and	A_lw, val, #255
	orr	A_lw, A_lw, A_lw, lsl #8
	orr	A_lw, A_lw, A_lw, lsl #16
	orr	A_l, A_l, A_l, lsl #32	// 64 位填充值准备完成，A_l 中每个字节都是 val

	cmp	count, #15
	b.hi	.Lover16_proc
	/*All store maybe are non-aligned..*/
// 中文：以下小量填充支持非对齐地址，使用 str/strh/strb 逐级处理
	tbz	count, #3, 1f
	str	A_l, [dst], #8
1:
	tbz	count, #2, 2f
	str	A_lw, [dst], #4
2:
	tbz	count, #1, 3f
	strh	A_lw, [dst], #2
3:
	tbz	count, #0, 4f
	strb	A_lw, [dst]
4:
	ret

.Lover16_proc:
	/*Whether  the start address is aligned with 16.*/
// 中文：检查目标地址是否已 16 字节对齐，若未对齐则先写入前 16 字节并调整
	neg	tmp2, dst
	ands	tmp2, tmp2, #15
	b.eq	.Laligned
/*
* The count is not less than 16, we can use stp to store the start 16 bytes,
* then adjust the dst aligned with 16.This process will make the current
* memory address at alignment boundary.
* 中文：写入前 16 字节（可能非对齐），然后调整 dst 到 16 字节对齐
*/
	stp	A_l, A_l, [dst] /*non-aligned store..*/
	/*make the dst aligned..*/
	sub	count, count, tmp2
	add	dst, dst, tmp2

.Laligned:
	cbz	A_l, .Lzero_mem

.Ltail_maybe_long:
	cmp	count, #64
	b.ge	.Lnot_short
.Ltail63:
	ands	tmp1, count, #0x30
	b.eq	3f
	cmp	tmp1w, #0x20
	b.eq	1f
	b.lt	2f
	stp	A_l, A_l, [dst], #16
1:
	stp	A_l, A_l, [dst], #16
2:
	stp	A_l, A_l, [dst], #16
/*
* The last store length is less than 16,use stp to write last 16 bytes.
* It will lead some bytes written twice and the access is non-aligned.
*/
3:
	ands	count, count, #15
	cbz	count, 4f
	add	dst, dst, count
	stp	A_l, A_l, [dst, #-16]	/* Repeat some/all of last store. */
4:
	ret

	/*
	* Critical loop. Start at a new cache line boundary. Assuming
	* 64 bytes per line, this ensures the entire loop is in one line.
	*/
	.p2align	L1_CACHE_SHIFT
.Lnot_short:
	sub	dst, dst, #16/* Pre-bias.  */
	sub	count, count, #64
1:
	stp	A_l, A_l, [dst, #16]
	stp	A_l, A_l, [dst, #32]
	stp	A_l, A_l, [dst, #48]
	stp	A_l, A_l, [dst, #64]!
	subs	count, count, #64
	b.ge	1b
	tst	count, #0x3f
	add	dst, dst, #16
	b.ne	.Ltail63
.Lexitfunc:
	ret

	/*
	* For zeroing memory, check to see if we can use the ZVA feature to
	* zero entire 'cache' lines.
	*/
.Lzero_mem:
	cmp	count, #63
	b.le	.Ltail63
	/*
	* For zeroing small amounts of memory, it's not worth setting up
	* the line-clear code.
	*/
	cmp	count, #128
	b.lt	.Lnot_short /*count is at least  128 bytes*/

	mrs	tmp1, dczid_el0
	tbnz	tmp1, #4, .Lnot_short
	mov	tmp3w, #4
	and	zva_len, tmp1w, #15	/* Safety: other bits reserved.  */
	lsl	zva_len, tmp3w, zva_len

	ands	tmp3w, zva_len, #63
	/*
	* ensure the zva_len is not less than 64.
	* It is not meaningful to use ZVA if the block size is less than 64.
	*/
	b.ne	.Lnot_short
.Lzero_by_line:
	/*
	* Compute how far we need to go to become suitably aligned. We're
	* already at quad-word alignment.
	*/
	cmp	count, zva_len_x
	b.lt	.Lnot_short		/* Not enough to reach alignment.  */
	sub	zva_bits_x, zva_len_x, #1
	neg	tmp2, dst
	ands	tmp2, tmp2, zva_bits_x
	b.eq	2f			/* Already aligned.  */
	/* Not aligned, check that there's enough to copy after alignment.*/
	sub	tmp1, count, tmp2
	/*
	* grantee the remain length to be ZVA is bigger than 64,
	* avoid to make the 2f's process over mem range.*/
	cmp	tmp1, #64
	ccmp	tmp1, zva_len_x, #8, ge	/* NZCV=0b1000 */
	b.lt	.Lnot_short
	/*
	* We know that there's at least 64 bytes to zero and that it's safe
	* to overrun by 64 bytes.
	*/
	mov	count, tmp1
1:
	stp	A_l, A_l, [dst]
	stp	A_l, A_l, [dst, #16]
	stp	A_l, A_l, [dst, #32]
	subs	tmp2, tmp2, #64
	stp	A_l, A_l, [dst, #48]
	add	dst, dst, #64
	b.ge	1b
	/* We've overrun a bit, so adjust dst downwards.*/
	add	dst, dst, tmp2
2:
	sub	count, count, zva_len_x
3:
	dc	zva, dst
	add	dst, dst, zva_len_x
	subs	count, count, zva_len_x
	b.ge	3b
	ands	count, count, zva_bits_x
	b.ne	.Ltail_maybe_long
	ret
SYM_FUNC_END(__pi_memset_generic)

#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
SYM_FUNC_START(__pi_memset)
alternative_if_not ARM64_HAS_MOPS
	b	__pi_memset_generic
alternative_else_nop_endif

	mov	dst, dstin
	setp	[dst]!, count!, val_x
	setm	[dst]!, count!, val_x
	sete	[dst]!, count!, val_x
	ret
SYM_FUNC_END(__pi_memset)
#else
SYM_FUNC_ALIAS(__pi_memset, __pi_memset_generic)
#endif

SYM_FUNC_ALIAS(__memset, __pi_memset)
EXPORT_SYMBOL(__memset)

SYM_FUNC_ALIAS_WEAK(memset, __pi_memset)
EXPORT_SYMBOL(memset)
