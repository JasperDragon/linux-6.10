/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Arm Ltd.
 */

#include <linux/linkage.h>
#include <asm/asm-uaccess.h>

	.text

/* Prototype: int __arch_clear_user(void *addr, size_t sz)
 * Purpose  : clear some user memory
 * Params   : addr - user memory address to clear
 *          : sz   - number of bytes to clear
 * Returns  : number of bytes NOT cleared
 *
 * Alignment fixed up by hardware.
 *
 * 中文说明：
 * 功能：将用户空间的一段内存清零。
 * 参数：x0 - 用户空间起始地址，x1 - 要清零的字节数。
 * 返回：x0 - 未能清零的字节数（0 表示全部成功）。
 * 算法：提供两种路径：
 *   (1) MOPS 指令扩展：使用 setpt/setmt/setet 三条顺序指令，硬件高效清零。
 *   (2) 软件路径：先处理 8 字节对齐块（使用 sttr xzr），
 *       再处理尾部剩余的 0-7 字节（按 4/2/1 字节递次处理）。
 *       所有用户空间写入均使用带异常处理的 sttr 系列指令。
 * 异常处理：当用户空间写入触发异常时，通过 6f/7f/8f/9f 修复点
 *       计算并返回未清零的字节数。
 */

SYM_FUNC_START(__arch_clear_user)
	add	x2, x0, x1		// x2 = addr + sz，保存结束地址，用于异常时计算返回值

#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
alternative_if_not ARM64_HAS_MOPS
	b	.Lno_mops
alternative_else_nop_endif

USER(9f, setpt	[x0]!, x1!, xzr)	// MOPS 清零前向探针：向用户空间写入零（异常 => 9f）
USER(6f, setmt	[x0]!, x1!, xzr)	// MOPS 清零主循环：硬件高效逐块写入零（异常 => 6f）
USER(6f, setet	[x0]!, x1!, xzr)	// MOPS 清零收尾：处理剩余字节（异常 => 6f）
	mov	x0, #0
	ret
.Lno_mops:
#endif

	subs	x1, x1, #8		// 预减 8，检查是否至少 8 字节
	b.mi	2f			// 不足 8 字节则跳转到尾部处理

1:	.p2align 4			// 主循环 16 字节对齐以提升性能
USER(9f, sttr	xzr, [x0])		// 向用户空间写入 8 字节零（异常 => 9f）
	add	x0, x0, #8		// 前进 8 字节
	subs	x1, x1, #8		// 剩余长度减 8
	b.hi	1b			// 若还有至少 9 字节则继续循环
USER(9f, sttr	xzr, [x2, #-8])	// 写入最后 8 字节（从结束地址倒退 8 字节，避免跨页问题）
	mov	x0, #0			// 全部清零，返回 0
	ret

	// 尾部处理：处理剩余 0-7 字节
2:	tbz	x1, #2, 3f		// 测试 bit2：是否有 4 字节需要处理
USER(9f, sttr	wzr, [x0])		// 写入 4 字节零（用户空间）
USER(8f, sttr	wzr, [x2, #-4])	// 从末尾写入 4 字节（处理可能的跨边界情况）
	mov	x0, #0
	ret

3:	tbz	x1, #1, 4f		// 测试 bit1：是否有 2 字节需要处理
USER(9f, sttrh	wzr, [x0])		// 写入 2 字节零
4:	tbz	x1, #0, 5f		// 测试 bit0：是否有 1 字节需要处理
USER(7f, sttrb	wzr, [x2, #-1])		// 写入最后 1 字节零
5:	mov	x0, #0
	ret

	// 异常修复点：当用户空间 sttr 类指令触发 page fault 时执行
	// 标签 6f/7f/8f/9f 由 USER() 宏和 MOPS 指令引用
6:	b.cs	9f		// MOPS 完成码检查：carry=1 表示全部完成，跳转到 9f 返回 0
	// Registers are in Option A format
	add	x0, x0, x1	// 恢复指针：x0 += x1（剩余计数），用于计算未清零字节数
	b	9f
7:	sub	x0, x2, #5	// sttrb 单字节异常调整：从结束地址倒退 5 字节校正偏移
8:	add	x0, x0, #4	// sttr 4 字节异常调整：前进 4 字节（对应第 2 个字）
9:	sub	x0, x2, x0
	ret
SYM_FUNC_END(__arch_clear_user)
EXPORT_SYMBOL(__arch_clear_user)
