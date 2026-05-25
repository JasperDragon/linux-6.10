/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>
#include <asm/cache.h>

/*
 * Copy to user space from a kernel buffer (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - to
 *	x1 - from
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 *
 * 中文说明：
 * 功能：将数据从内核缓冲区复制到用户空间。与 copy_from_user 对称：
 *       源地址（内核空间）使用普通 ldr 指令读取，
 *       目标地址（用户空间）使用带异常处理的 sttr 系列指令写入。
 * 参数：x0 - 目标用户地址，x1 - 源内核地址，x2 - 复制字节数。
 * 返回：x0 - 未能复制的字节数（0 表示全部成功）。
 * 算法：提供两种路径：
 *   (1) MOPS 指令扩展：使用 cpyfpwt/cpyfmwt/cpyfewt 写入用户空间，
 *       硬件自动处理异常。
 *   (2) copy_template.S 通用模板：使用 strb1/strh1/str1/stp1 宏
 *       （内含带异常处理的存储指令）。
 * 异常处理：当用户空间写入触发异常时，跳转到修复点计算未复制字节数。
 */
	.macro ldrb1 reg, ptr, val
	ldrb  \reg, [\ptr], \val
	.endm

	.macro strb1 reg, ptr, val
	user_ldst 9998f, sttrb, \reg, \ptr, \val
	.endm

	.macro ldrh1 reg, ptr, val
	ldrh  \reg, [\ptr], \val
	.endm

	.macro strh1 reg, ptr, val
	user_ldst 9997f, sttrh, \reg, \ptr, \val
	.endm

	.macro ldr1 reg, ptr, val
	ldr \reg, [\ptr], \val
	.endm

	.macro str1 reg, ptr, val
	user_ldst 9997f, sttr, \reg, \ptr, \val
	.endm

	.macro ldp1 reg1, reg2, ptr, val
	ldp \reg1, \reg2, [\ptr], \val
	.endm

	.macro stp1 reg1, reg2, ptr, val
	user_stp 9997f, \reg1, \reg2, \ptr, \val
	.endm

	// 宏：使用 MOPS 指令复制到用户空间（带异常处理，写入用户空间）
	.macro cpy1 dst, src, count
	.arch_extension mops
	USER_CPY(9997f, 1, cpyfpwt [\dst]!, [\src]!, \count!)	// MOPS 复制前向探针（写入用户空间）
	USER_CPY(9996f, 1, cpyfmwt [\dst]!, [\src]!, \count!)	// MOPS 复制主循环
	USER_CPY(9996f, 1, cpyfewt [\dst]!, [\src]!, \count!)	// MOPS 复制收尾
	.endm

end	.req	x5		// 复制结束地址（dst + n），用于异常时计算未复制字节数
srcin	.req	x15		// 保留原始内核源地址，用于异常后重试
SYM_FUNC_START(__arch_copy_to_user)
	add	end, x0, x2		// 计算复制结束地址 = 目标用户地址 + 复制长度
	mov	srcin, x1		// 保存原始内核源地址
#include "copy_template.S"
	mov	x0, #0
	ret

	// 异常修复点：当用户空间存储指令触发 page fault 时执行
9996:	b.cs	9997f		// MOPS 完成码：carry=1 表示全部传输完成
	// Registers are in Option A format
	add	dst, dst, count		// Option A 格式：恢复 dst 到未完成位置
9997:	cmp	dst, dstin		// 检查是否一个字节都未写入（dst 是否还在起点）
	b.ne	9998f			// 已写入部分数据则跳转计算返回值
	// 若完全未写入任何字节，尝试单字节方式（可能跨页边界需要折半处理）
	ldrb	tmp1w, [srcin]
USER(9998f, sttrb tmp1w, [dst])
	add	dst, dst, #1		// 前进 1 字节，继续处理剩余数据
9998:	sub	x0, end, dst			// 计算未能复制的字节数并返回
	ret
SYM_FUNC_END(__arch_copy_to_user)
EXPORT_SYMBOL(__arch_copy_to_user)
