/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2021, Arm Limited.
 *
 * Adapted from the original at:
 * https://github.com/ARM-software/optimized-routines/blob/afd6244a1f8d9229/string/aarch64/memcpy.S
 */

#include <linux/linkage.h>
#include <asm/assembler.h>

/* Assumptions:
 *
 * ARMv8-a, AArch64, unaligned accesses.
 *
 * 中文说明：
 * 功能：内存复制（memcpy）和内存移动（memmove，支持重叠区域）。
 *       同时支持 memcpy 和 memmove 两个标准 C 库函数。
 * 参数：x0 - 目标地址（dstin），x1 - 源地址（src），x2 - 复制字节数（count）。
 * 返回：x0 - 目标地址（与标准 memcpy/memmove 语义一致）。
 * 算法：按复制大小分为三个主要路径：
 *   (1) 小量（0-32 字节）：使用无分支（branchless）序列，通过 ldp/stp 组合
 *       从两端同时加载/存储，避免分支预测失败。
 *   (2) 中等（33-128 字节）：一次性加载所有数据到寄存器再写入，
 *       通过成对加载（ldp）和存储（stp）最大化吞吐量。
 *   (3) 大量（>128 字节）：使用软件流水线循环，每次处理 64 字节。
 *       目标地址 16 字节对齐以减少非对齐访问。
 *       重叠检测后选择正向或反向复制。
 *   (4) MOPS 指令扩展（ARMv8.8+）：使用 cpyp/cpym/cpye 三条顺序指令，
 *       硬件高效完成复制。
 *
 * 关键优化：通过 ldp/stp 指令对和预加载技术隐藏内存延迟，
 *   循环中交错执行加载和存储以实现流水线并行。
 *
 * 注意：此实现也作为 memmove 使用，通过检测 dst-src 重叠自动选择复制方向。
 */

#define L(label) .L ## label

#define dstin	x0
#define src	x1
#define count	x2
#define dst	x3
#define srcend	x4
#define dstend	x5
#define A_l	x6
#define A_lw	w6
#define A_h	x7
#define B_l	x8
#define B_lw	w8
#define B_h	x9
#define C_l	x10
#define C_lw	w10
#define C_h	x11
#define D_l	x12
#define D_h	x13
#define E_l	x14
#define E_h	x15
#define F_l	x16
#define F_h	x17
#define G_l	count
#define G_h	dst
#define H_l	src
#define H_h	srcend
#define tmp1	x14

/* This implementation handles overlaps and supports both memcpy and memmove
   from a single entry point.  It uses unaligned accesses and branchless
   sequences to keep the code small, simple and improve performance.

   Copies are split into 3 main cases: small copies of up to 32 bytes, medium
   copies of up to 128 bytes, and large copies.  The overhead of the overlap
   check is negligible since it is only required for large copies.

   Large copies use a software pipelined loop processing 64 bytes per iteration.
   The destination pointer is 16-byte aligned to minimize unaligned accesses.
   The loop tail is handled by always copying 64 bytes from the end.
*/

SYM_FUNC_START_LOCAL(__pi_memcpy_generic)
	add	srcend, src, count
	add	dstend, dstin, count
	cmp	count, 128
	b.hi	L(copy_long)
	cmp	count, 32
	b.hi	L(copy32_128)

	/* Small copies: 0..32 bytes.  */
// 中文：0-32 字节小量复制 — 从两端同时加载/存储，避免分支预测失败
	cmp	count, 16
	b.lo	L(copy16)
	ldp	A_l, A_h, [src]
	ldp	D_l, D_h, [srcend, -16]
	stp	A_l, A_h, [dstin]
	stp	D_l, D_h, [dstend, -16]
	ret

	/* Copy 8-15 bytes.  */
// 中文：8-15 字节复制 — 同样从两端加载/存储，无分支
L(copy16):
	tbz	count, 3, L(copy8)
	ldr	A_l, [src]
	ldr	A_h, [srcend, -8]
	str	A_l, [dstin]
	str	A_h, [dstend, -8]
	ret

	.p2align 3
	/* Copy 4-7 bytes.  */
// 中文：4-7 字节复制 — 从两端加载 32 位值再写入
L(copy8):
	tbz	count, 2, L(copy4)
	ldr	A_lw, [src]
	ldr	B_lw, [srcend, -4]
	str	A_lw, [dstin]
	str	B_lw, [dstend, -4]
	ret

	/* Copy 0..3 bytes using a branchless sequence.  */
// 中文：0-3 字节复制 — 无分支序列，通过 lsr 计算中间偏移一次性加载三个字节
L(copy4):
	cbz	count, L(copy0)
	lsr	tmp1, count, 1
	ldrb	A_lw, [src]
	ldrb	C_lw, [srcend, -1]
	ldrb	B_lw, [src, tmp1]
	strb	A_lw, [dstin]
	strb	B_lw, [dstin, tmp1]
	strb	C_lw, [dstend, -1]
L(copy0):
	ret

	.p2align 4
	/* Medium copies: 33..128 bytes.  */
// 中文：33-128 字节中等复制 — 一次性加载所有数据到寄存器，再全部写入
L(copy32_128):
	ldp	A_l, A_h, [src]
	ldp	B_l, B_h, [src, 16]
	ldp	C_l, C_h, [srcend, -32]
	ldp	D_l, D_h, [srcend, -16]
	cmp	count, 64
	b.hi	L(copy128)
	stp	A_l, A_h, [dstin]
	stp	B_l, B_h, [dstin, 16]
	stp	C_l, C_h, [dstend, -32]
	stp	D_l, D_h, [dstend, -16]
	ret

	.p2align 4
	/* Copy 65..128 bytes.  */
// 中文：65-128 字节复制 — 额外加载中间 64 字节，拼接成完整的 128 字节传输
L(copy128):
	ldp	E_l, E_h, [src, 32]
	ldp	F_l, F_h, [src, 48]
	cmp	count, 96
	b.ls	L(copy96)
	ldp	G_l, G_h, [srcend, -64]
	ldp	H_l, H_h, [srcend, -48]
	stp	G_l, G_h, [dstend, -64]
	stp	H_l, H_h, [dstend, -48]
L(copy96):
	stp	A_l, A_h, [dstin]
	stp	B_l, B_h, [dstin, 16]
	stp	E_l, E_h, [dstin, 32]
	stp	F_l, F_h, [dstin, 48]
	stp	C_l, C_h, [dstend, -32]
	stp	D_l, D_h, [dstend, -16]
	ret

	.p2align 4
	/* Copy more than 128 bytes.  */
// 中文：超过 128 字节的大块复制 — 使用软件流水线循环，每次 64 字节
L(copy_long):
	/* Use backwards copy if there is an overlap.  */
// 中文：如果 dst 和 src 有重叠且 dst > src，使用反向复制避免数据被覆盖
	sub	tmp1, dstin, src
	cbz	tmp1, L(copy0)
	cmp	tmp1, count
	b.lo	L(copy_long_backwards)

	/* Copy 16 bytes and then align dst to 16-byte alignment.  */
// 中文：先复制 16 字节，然后将目标地址对齐到 16 字节边界以优化后续访问

	ldp	D_l, D_h, [src]
	and	tmp1, dstin, 15
	bic	dst, dstin, 15
	sub	src, src, tmp1
	add	count, count, tmp1	/* Count is now 16 too large.  */
	ldp	A_l, A_h, [src, 16]
	stp	D_l, D_h, [dstin]
	ldp	B_l, B_h, [src, 32]
	ldp	C_l, C_h, [src, 48]
	ldp	D_l, D_h, [src, 64]!
	subs	count, count, 128 + 16	/* Test and readjust count.  */
	b.ls	L(copy64_from_end)

L(loop64):
	// 中文：主流水线循环 — 交错执行 64 字节的存储和加载（软件流水线技术，隐藏内存延迟）
	stp	A_l, A_h, [dst, 16]
	ldp	A_l, A_h, [src, 16]
	stp	B_l, B_h, [dst, 32]
	ldp	B_l, B_h, [src, 32]
	stp	C_l, C_h, [dst, 48]
	ldp	C_l, C_h, [src, 48]
	stp	D_l, D_h, [dst, 64]!
	ldp	D_l, D_h, [src, 64]!
	subs	count, count, 64
	b.hi	L(loop64)

	/* Write the last iteration and copy 64 bytes from the end.  */
// 中文：当剩余不足 64 字节时，将上次循环的数据写入并额外从末尾复制 64 字节
// 这样既处理了尾部数据，又避免了对剩余长度进行逐字节检查
L(copy64_from_end):
	ldp	E_l, E_h, [srcend, -64]
	stp	A_l, A_h, [dst, 16]
	ldp	A_l, A_h, [srcend, -48]
	stp	B_l, B_h, [dst, 32]
	ldp	B_l, B_h, [srcend, -32]
	stp	C_l, C_h, [dst, 48]
	ldp	C_l, C_h, [srcend, -16]
	stp	D_l, D_h, [dst, 64]
	stp	E_l, E_h, [dstend, -64]
	stp	A_l, A_h, [dstend, -48]
	stp	B_l, B_h, [dstend, -32]
	stp	C_l, C_h, [dstend, -16]
	ret

	.p2align 4

		/* 反向复制 — 当 dst 在 src 之后且区域重叠时，从尾到头复制避免覆盖。 */
		/* Copy 16 bytes and then align dst to 16-byte alignment. */
L(copy_long_backwards):
	ldp	D_l, D_h, [srcend, -16]
	and	tmp1, dstend, 15
	sub	srcend, srcend, tmp1
	sub	count, count, tmp1
	ldp	A_l, A_h, [srcend, -16]
	stp	D_l, D_h, [dstend, -16]
	ldp	B_l, B_h, [srcend, -32]
	ldp	C_l, C_h, [srcend, -48]
	ldp	D_l, D_h, [srcend, -64]!
	sub	dstend, dstend, tmp1
	subs	count, count, 128
	b.ls	L(copy64_from_start)

L(loop64_backwards):
	stp	A_l, A_h, [dstend, -16]
	ldp	A_l, A_h, [srcend, -16]
	stp	B_l, B_h, [dstend, -32]
	ldp	B_l, B_h, [srcend, -32]
	stp	C_l, C_h, [dstend, -48]
	ldp	C_l, C_h, [srcend, -48]
	stp	D_l, D_h, [dstend, -64]!
	ldp	D_l, D_h, [srcend, -64]!
	subs	count, count, 64
	b.hi	L(loop64_backwards)

	/* Write the last iteration and copy 64 bytes from the start.  */
L(copy64_from_start):
	ldp	G_l, G_h, [src, 48]
	stp	A_l, A_h, [dstend, -16]
	ldp	A_l, A_h, [src, 32]
	stp	B_l, B_h, [dstend, -32]
	ldp	B_l, B_h, [src, 16]
	stp	C_l, C_h, [dstend, -48]
	ldp	C_l, C_h, [src]
	stp	D_l, D_h, [dstend, -64]
	stp	G_l, G_h, [dstin, 48]
	stp	A_l, A_h, [dstin, 32]
	stp	B_l, B_h, [dstin, 16]
	stp	C_l, C_h, [dstin]
	ret
SYM_FUNC_END(__pi_memcpy_generic)

#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
SYM_FUNC_START(__pi_memcpy)
alternative_if_not ARM64_HAS_MOPS
	b	__pi_memcpy_generic
alternative_else_nop_endif

	mov	dst, dstin
	cpyp	[dst]!, [src]!, count!	// MOPS 复制前向探针：准备复制操作
	cpym	[dst]!, [src]!, count!	// MOPS 复制主循环：硬件自动完成大块复制
	cpye	[dst]!, [src]!, count!	// MOPS 复制收尾：处理剩余字节
	ret
SYM_FUNC_END(__pi_memcpy)
#else
SYM_FUNC_ALIAS(__pi_memcpy, __pi_memcpy_generic)
#endif

SYM_FUNC_ALIAS(__memcpy, __pi_memcpy)
EXPORT_SYMBOL(__memcpy)
SYM_FUNC_ALIAS_WEAK(memcpy, __memcpy)
EXPORT_SYMBOL(memcpy)

SYM_FUNC_ALIAS(__pi_memmove, __pi_memcpy)

SYM_FUNC_ALIAS(__memmove, __pi_memmove)
EXPORT_SYMBOL(__memmove)
SYM_FUNC_ALIAS_WEAK(memmove, __memmove)
EXPORT_SYMBOL(memmove)
