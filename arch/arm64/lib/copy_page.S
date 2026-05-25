/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/assembler.h>
#include <asm/page.h>
#include <asm/cpufeature.h>
#include <asm/alternative.h>

/*
 * Copy a page from src to dest (both are page aligned)
 *
 * Parameters:
 *	x0 - dest
 *	x1 - src
 *
 * 中文说明：
 * 功能：将一页（PAGE_SIZE 大小）数据从源地址复制到目标地址。
 * 参数：x0 - 目标页面地址，x1 - 源页面地址（均需页对齐）。
 * 算法：提供两种复制路径：
 *   (1) MOPS 指令扩展（ARMv8.8+）：使用 cpypwn/cpymwn/cpyewn 三条顺序指令，
 *       由硬件高效完成页面复制。
 *   (2) 软件流水线路径：采用双缓冲（double-buffering）技术，每次加载 128 字节
 *       到寄存器后立即写入，通过交错加载/存储隐藏内存延迟。
 *       使用 STNP（非暂存对）写入以减少缓存污染。
 */
SYM_FUNC_START(__pi_copy_page)
#ifdef CONFIG_AS_HAS_MOPS
	.arch_extension mops
alternative_if_not ARM64_HAS_MOPS
	b	.Lno_mops
alternative_else_nop_endif

	mov	x2, #PAGE_SIZE
	cpypwn	[x0]!, [x1]!, x2!	// MOPS 复制前向探针：准备复制操作
	cpymwn	[x0]!, [x1]!, x2!	// MOPS 复制主循环：硬件自动执行大块复制
	cpyewn	[x0]!, [x1]!, x2!	// MOPS 复制后向收尾：处理剩余字节
	ret
.Lno_mops:
#endif
	ldp	x2, x3, [x1]		// 预加载 128 字节（16 个寄存器 × 8 字节）
	ldp	x4, x5, [x1, #16]
	ldp	x6, x7, [x1, #32]
	ldp	x8, x9, [x1, #48]
	ldp	x10, x11, [x1, #64]
	ldp	x12, x13, [x1, #80]
	ldp	x14, x15, [x1, #96]
	ldp	x16, x17, [x1, #112]

	add	x0, x0, #256		// 预偏移目标地址，配合后续负偏移访问
	add	x1, x1, #128		// 源地址前进 128 字节，开始流水线循环
1:
	tst	x0, #(PAGE_SIZE - 1)	// 检查是否已复制完整个页

	stnp	x2, x3, [x0, #-256]	// 写入上轮预加载的 128 字节（使用负偏移回退到正确位置）
	ldp	x2, x3, [x1]		// 同时加载下一块数据，实现加载/存储交错流水线
	stnp	x4, x5, [x0, #16 - 256]	// 交替存储之前加载的数据...
	ldp	x4, x5, [x1, #16]		// ...同时加载下一轮所需的数据
	stnp	x6, x7, [x0, #32 - 256]
	ldp	x6, x7, [x1, #32]
	stnp	x8, x9, [x0, #48 - 256]
	ldp	x8, x9, [x1, #48]
	stnp	x10, x11, [x0, #64 - 256]
	ldp	x10, x11, [x1, #64]
	stnp	x12, x13, [x0, #80 - 256]
	ldp	x12, x13, [x1, #80]
	stnp	x14, x15, [x0, #96 - 256]
	ldp	x14, x15, [x1, #96]
	stnp	x16, x17, [x0, #112 - 256]
	ldp	x16, x17, [x1, #112]

	add	x0, x0, #128		// 目标地址前进 128 字节
	add	x1, x1, #128		// 源地址前进 128 字节

	b.ne	1b			// 若未到页尾则继续循环

	stnp	x2, x3, [x0, #-256]	// 将最后一批预加载的 128 字节写入目标
	stnp	x4, x5, [x0, #16 - 256]
	stnp	x6, x7, [x0, #32 - 256]
	stnp	x8, x9, [x0, #48 - 256]
	stnp	x10, x11, [x0, #64 - 256]
	stnp	x12, x13, [x0, #80 - 256]
	stnp	x14, x15, [x0, #96 - 256]
	stnp	x16, x17, [x0, #112 - 256]

	ret
SYM_FUNC_END(__pi_copy_page)
SYM_FUNC_ALIAS(copy_page, __pi_copy_page)
EXPORT_SYMBOL(copy_page)
