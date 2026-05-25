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

/*
 * ============================================================================
 * copy_template.S — arm64 内存拷贝模板（copy_from_user / copy_to_user / memcpy 共用）
 * ============================================================================
 *
 * 本文件是 arm64 内存拷贝的通用汇编模板，通过 C 预处理器宏参数化实现代码复用。
 *
 * 文件本身不定义函数，而是提供通用的拷贝算法。调用方（copy_from_user.S、
 * copy_to_user.S、memcpy.S）通过 #define 不同宏后 #include 本文件来生成
 * 各自的实现：
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │ 调用方            │ ldr 指令      │ str 指令      │ 额外宏    │
 * ├─────────────────────────────────────────────────────────────┤
 * │ copy_from_user.S  │ ldr1=ldtr     │ str1=str      │ USER()    │
 * │ copy_to_user.S    │ ldr1=ldr      │ str1=sttr     │ USER()    │
 * │ memcpy.S          │ ldr1=ldr      │ str1=str      │ (无)      │
 * └─────────────────────────────────────────────────────────────┘
 *
 * copy_from_user 使用 LDTR（非特权加载）：当源地址是非法用户地址时触发
 * 缺页异常，通过异常表（__ex_table）安全返回 -EFAULT，而非内核崩溃。
 * copy_to_user 使用 STTR（非特权存储）：对非法目标地址的处理同理。
 * memcpy 使用标准 LDR/STR，假设源和目标的虚拟地址均有效（内核态）。
 *
 * 算法概要（按数据大小选择不同策略）：
 *
 *   count < 16  → .Ltiny15:  按 8/4/2/1 字节逐次拷贝（尾部处理）
 *   16 ≤ count < 64 → .Ltail63: 先对齐源地址，然后 ldp/stp 16字节批量拷贝
 *   64 ≤ count < 128 → 展开 4× ldp + 2× stp interleaved + 2× stp + 跳尾
 *   count ≥ 128 → .Lcpy_body_large: 预取 64B → 交错 load/store 循环
 *      每轮: 存储上一轮 64B + 加载下一轮 64B（流水线优化）
 *
 * 关键优化技巧：
 *   - .Lcpy_body_large 对齐到 L1_CACHE_SHIFT（64B），确保循环在一行 I-cache 内
 *   - load 和 store 交错：利用 AArch64 的乱序执行和存储转发
 *   - MOPS 替代方案：ARMv8.8+ FEAT_MOPS 提供硬件加速的 cpy1 指令
 *   - memmove 友好：始终以递增地址顺序访问，避免 src/dst 重叠时覆盖源数据
 */


/*
 * Copy a buffer from src to dest (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - dest
 *	x1 - src
 *	x2 - n
 * Returns:
 *	x0 - dest
 */
dstin	.req	x0
src	.req	x1
count	.req	x2
tmp1	.req	x3
tmp1w	.req	w3
tmp2	.req	x4
tmp2w	.req	w4
dst	.req	x6

A_l	.req	x7
A_h	.req	x8
B_l	.req	x9
B_h	.req	x10
C_l	.req	x11
C_h	.req	x12
D_l	.req	x13
D_h	.req	x14

	mov	dst, dstin

#ifdef CONFIG_AS_HAS_MOPS
alternative_if_not ARM64_HAS_MOPS
	b	.Lno_mops
alternative_else_nop_endif
	cpy1	dst, src, count
	b	.Lexitfunc
.Lno_mops:
#endif

	cmp	count, #16
	/*When memory length is less than 16, the accessed are not aligned.*/
	b.lo	.Ltiny15

	neg	tmp2, src
	ands	tmp2, tmp2, #15/* Bytes to reach alignment. */
	b.eq	.LSrcAligned
	sub	count, count, tmp2
	/*
	* Copy the leading memory data from src to dst in an increasing
	* address order.By this way,the risk of overwriting the source
	* memory data is eliminated when the distance between src and
	* dst is less than 16. The memory accesses here are alignment.
	*/
	tbz	tmp2, #0, 1f
	ldrb1	tmp1w, src, #1
	strb1	tmp1w, dst, #1
1:
	tbz	tmp2, #1, 2f
	ldrh1	tmp1w, src, #2
	strh1	tmp1w, dst, #2
2:
	tbz	tmp2, #2, 3f
	ldr1	tmp1w, src, #4
	str1	tmp1w, dst, #4
3:
	tbz	tmp2, #3, .LSrcAligned
	ldr1	tmp1, src, #8
	str1	tmp1, dst, #8

.LSrcAligned:
	cmp	count, #64
	b.ge	.Lcpy_over64
	/*
	* Deal with small copies quickly by dropping straight into the
	* exit block.
	*/
.Ltail63:
	/*
	* Copy up to 48 bytes of data. At this point we only need the
	* bottom 6 bits of count to be accurate.
	*/
	ands	tmp1, count, #0x30
	b.eq	.Ltiny15
	cmp	tmp1w, #0x20
	b.eq	1f
	b.lt	2f
	ldp1	A_l, A_h, src, #16
	stp1	A_l, A_h, dst, #16
1:
	ldp1	A_l, A_h, src, #16
	stp1	A_l, A_h, dst, #16
2:
	ldp1	A_l, A_h, src, #16
	stp1	A_l, A_h, dst, #16
.Ltiny15:
	/*
	* Prefer to break one ldp/stp into several load/store to access
	* memory in an increasing address order,rather than to load/store 16
	* bytes from (src-16) to (dst-16) and to backward the src to aligned
	* address,which way is used in original cortex memcpy. If keeping
	* the original memcpy process here, memmove need to satisfy the
	* precondition that src address is at least 16 bytes bigger than dst
	* address,otherwise some source data will be overwritten when memove
	* call memcpy directly. To make memmove simpler and decouple the
	* memcpy's dependency on memmove, withdrew the original process.
	*/
	tbz	count, #3, 1f
	ldr1	tmp1, src, #8
	str1	tmp1, dst, #8
1:
	tbz	count, #2, 2f
	ldr1	tmp1w, src, #4
	str1	tmp1w, dst, #4
2:
	tbz	count, #1, 3f
	ldrh1	tmp1w, src, #2
	strh1	tmp1w, dst, #2
3:
	tbz	count, #0, .Lexitfunc
	ldrb1	tmp1w, src, #1
	strb1	tmp1w, dst, #1

	b	.Lexitfunc

.Lcpy_over64:
	subs	count, count, #128
	b.ge	.Lcpy_body_large
	/*
	* Less than 128 bytes to copy, so handle 64 here and then jump
	* to the tail.
	*/
	ldp1	A_l, A_h, src, #16
	stp1	A_l, A_h, dst, #16
	ldp1	B_l, B_h, src, #16
	ldp1	C_l, C_h, src, #16
	stp1	B_l, B_h, dst, #16
	stp1	C_l, C_h, dst, #16
	ldp1	D_l, D_h, src, #16
	stp1	D_l, D_h, dst, #16

	tst	count, #0x3f
	b.ne	.Ltail63
	b	.Lexitfunc

	/*
	* Critical loop.  Start at a new cache line boundary.  Assuming
	* 64 bytes per line this ensures the entire loop is in one line.
	*/
	.p2align	L1_CACHE_SHIFT
.Lcpy_body_large:
	/* pre-get 64 bytes data. */
	ldp1	A_l, A_h, src, #16
	ldp1	B_l, B_h, src, #16
	ldp1	C_l, C_h, src, #16
	ldp1	D_l, D_h, src, #16
1:
	/*
	* interlace the load of next 64 bytes data block with store of the last
	* loaded 64 bytes data.
	*/
	stp1	A_l, A_h, dst, #16
	ldp1	A_l, A_h, src, #16
	stp1	B_l, B_h, dst, #16
	ldp1	B_l, B_h, src, #16
	stp1	C_l, C_h, dst, #16
	ldp1	C_l, C_h, src, #16
	stp1	D_l, D_h, dst, #16
	ldp1	D_l, D_h, src, #16
	subs	count, count, #64
	b.ge	1b
	stp1	A_l, A_h, dst, #16
	stp1	B_l, B_h, dst, #16
	stp1	C_l, C_h, dst, #16
	stp1	D_l, D_h, dst, #16

	tst	count, #0x3f
	b.ne	.Ltail63
.Lexitfunc:
