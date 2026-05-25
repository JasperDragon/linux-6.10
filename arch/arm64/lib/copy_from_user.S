/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>
#include <asm/cache.h>

/*
 * Copy from user space to a kernel buffer (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - to
 *	x1 - from
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 *
 * 中文说明：
 * 功能：将数据从用户空间复制到内核缓冲区。与 copy_to_user 的区别在于：
 *       源地址（用户空间）的读取使用带异常处理的 ldtr 系列指令，
 *       而目标地址（内核空间）使用普通 str 指令。
 * 参数：x0 - 目标内核地址，x1 - 源用户地址，x2 - 复制字节数。
 * 返回：x0 - 未能复制的字节数（0 表示全部成功）。
 * 算法：提供两种路径：
 *   (1) MOPS 指令扩展：使用 cpyfprt/cpyfmrt/cpyfert 从用户空间复制，
 *       硬件自动处理页边界和异常。
 *   (2) copy_template.S 通用模板：针对不同大小和对齐情况优化，
 *       使用 ldrb1/ldrh1/ldr1/ldp1 宏（内含带异常处理的加载指令）。
 * 异常处理：当用户空间访问触发异常时，跳转到 9996/9997/9998 修复点，
 *       计算并返回尚未复制的字节数。
 */

	.macro ldrb1 reg, ptr, val
	user_ldst 9998f, ldtrb, \reg, \ptr, \val
	.endm

	.macro strb1 reg, ptr, val
	strb \reg, [\ptr], \val
	.endm

	.macro ldrh1 reg, ptr, val
	user_ldst 9997f, ldtrh, \reg, \ptr, \val
	.endm

	.macro strh1 reg, ptr, val
	strh \reg, [\ptr], \val
	.endm

	.macro ldr1 reg, ptr, val
	user_ldst 9997f, ldtr, \reg, \ptr, \val
	.endm

	.macro str1 reg, ptr, val
	str \reg, [\ptr], \val
	.endm

	.macro ldp1 reg1, reg2, ptr, val
	user_ldp 9997f, \reg1, \reg2, \ptr, \val
	.endm

	.macro stp1 reg1, reg2, ptr, val
	stp \reg1, \reg2, [\ptr], \val
	.endm

	// 宏：使用 MOPS 指令从用户空间复制（带异常处理，从用户读取）
	.macro cpy1 dst, src, count
	.arch_extension mops
	USER_CPY(9997f, 0, cpyfprt [\dst]!, [\src]!, \count!)	// MOPS 复制前向探针
	USER_CPY(9996f, 0, cpyfmrt [\dst]!, [\src]!, \count!)	// MOPS 复制主循环
	USER_CPY(9996f, 0, cpyfert [\dst]!, [\src]!, \count!)	// MOPS 复制收尾
	.endm

end	.req	x5		// 复制结束地址（dst + n），用于异常时计算未复制字节数
srcin	.req	x15		// 保留原始源地址，用于异常后的单字节重试
SYM_FUNC_START(__arch_copy_from_user)
	add	end, x0, x2		// 计算复制结束地址 = 目标地址 + 复制长度
	mov	srcin, x1		// 保存原始用户空间源地址
#include "copy_template.S"
	mov	x0, #0				// Nothing to copy
	ret

	// 异常修复点：当用户空间加载指令触发 page fault 时执行
	// 这些标签（9996/9997/9998）由 copy_template 和 user_ldst/user_ldp/USER_CPY 宏引用
9996:	b.cs	9997f			// MOPS 完成码：若 carry=1 表示全部传输完成
	// Registers are in Option A format
	add	dst, dst, count		// 在 Option A 格式下，恢复 dst 到未完成位置
9997:	cmp	dst, dstin		// 检查是否一个字节都未复制（dst 是否还在起点）
	b.ne	9998f
	// Before being absolutely sure we couldn't copy anything, try harder
USER(9998f, ldtrb tmp1w, [srcin])
	strb	tmp1w, [dst], #1	// 写入内核空间并前进 1 字节
9998:	sub	x0, end, dst			// bytes not copied
	ret
SYM_FUNC_END(__arch_copy_from_user)
EXPORT_SYMBOL(__arch_copy_from_user)
