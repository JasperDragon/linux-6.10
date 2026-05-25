/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020 ARM Ltd.
 */
// 中文说明：
// MTE（Memory Tagging Extension）相关汇编函数。
// 功能：提供页级别 MTE 标签的清除、复制、保存、恢复，
//   以及用户空间与内核空间之间的标签传输。
// 主要指令：
//   STGM/LDGM：多标签存储/加载，在单次操作中传输多个标签
//   STZ2G/DZ GZVA：清零数据并同时设置标签
//   STG/LDG：单标签存储/加载
// 标签粒度：（MTE_GRANULE_SIZE）每 16 字节一个 4 位标签

#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>
#include <asm/mte.h>
#include <asm/page.h>
#include <asm/sysreg.h>

	.arch	armv8.5-a+memtag

/*
 * multitag_transfer_size - set \reg to the block size that is accessed by the
 * LDGM/STGM instructions.
 * 中文：宏 — 获取 LDGM/STGM 指令的单次传输块大小，
 * 从 GMID_EL1 系统寄存器读取粒度，计算为 4 << BS（默认 64 字节）。
 */
	.macro	multitag_transfer_size, reg, tmp
	mrs_s	\reg, SYS_GMID_EL1
	ubfx	\reg, \reg, #GMID_EL1_BS_SHIFT, #GMID_EL1_BS_WIDTH
	mov	\tmp, #4
	lsl	\reg, \tmp, \reg
	.endm

/*
 * Clear the tags in a page
 *   x0 - address of the page to be cleared
 * 中文：清除整个页面的 MTE 标签。使用 STGM xzr 将零标签写入每个传输块。
 */
SYM_FUNC_START(mte_clear_page_tags)
	multitag_transfer_size x1, x2
1:	stgm	xzr, [x0]
	add	x0, x0, x1
	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b
	ret
SYM_FUNC_END(mte_clear_page_tags)

/*
 * Zero the page and tags at the same time
 *
 * Parameters:
 *	x0 - address to the beginning of the page
 * 中文：同时清零一页的数据和 MTE 标签。优先使用 DC GZVA 指令，
 * 不支持时使用 STZ2G 软件回退路径。
 */
SYM_FUNC_START(mte_zero_clear_page_tags)
	and	x0, x0, #(1 << MTE_TAG_SHIFT) - 1	// clear the tag
	mrs	x1, dczid_el0
	tbnz	x1, #4, 2f	// Branch if DC GZVA is prohibited
	and	w1, w1, #0xf
	mov	x2, #4
	lsl	x1, x2, x1

1:	dc	gzva, x0
	add	x0, x0, x1
	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b
	ret

2:	stz2g	x0, [x0], #(MTE_GRANULE_SIZE * 2)
	tst	x0, #(PAGE_SIZE - 1)
	b.ne	2b
	ret
SYM_FUNC_END(mte_zero_clear_page_tags)

/*
 * Copy the tags from the source page to the destination one
 *   x0 - address of the destination page
 *   x1 - address of the source page
 * 中文：将源页面的 MTE 标签复制到目标页面。使用 LDGM/STGM 逐块传输。
 */
SYM_FUNC_START(mte_copy_page_tags)
	mov	x2, x0
	mov	x3, x1
	multitag_transfer_size x5, x6
1:	ldgm	x4, [x3]
	stgm	x4, [x2]
	add	x2, x2, x5
	add	x3, x3, x5
	tst	x2, #(PAGE_SIZE - 1)
	b.ne	1b
	ret
SYM_FUNC_END(mte_copy_page_tags)

/*
 * Read tags from a user buffer (one tag per byte) and set the corresponding
 * tags at the given kernel address. Used by PTRACE_POKEMTETAGS.
 *   x0 - kernel address (to)
 *   x1 - user buffer (from)
 *   x2 - number of tags/bytes (n)
 * Returns:
 *   x0 - number of tags read/set
 * 中文：从用户缓冲区读取标签值设置到内核地址。每字节一标签，PTRACE_POKEMTETAGS 使用。
 */
SYM_FUNC_START(mte_copy_tags_from_user)
	mov	x3, x1
	cbz	x2, 2f
1:
USER(2f, ldtrb	w4, [x1])
	lsl	x4, x4, #MTE_TAG_SHIFT
	stg	x4, [x0], #MTE_GRANULE_SIZE
	add	x1, x1, #1
	subs	x2, x2, #1
	b.ne	1b

	// exception handling and function return
2:	sub	x0, x1, x3		// update the number of tags set
	ret
SYM_FUNC_END(mte_copy_tags_from_user)

/*
 * Get the tags from a kernel address range and write the tag values to the
 * given user buffer (one tag per byte). Used by PTRACE_PEEKMTETAGS.
 *   x0 - user buffer (to)
 *   x1 - kernel address (from)
 *   x2 - number of tags/bytes (n)
 * Returns:
 *   x0 - number of tags read/set
 * 中文：从内核地址读取标签值写入用户缓冲区。每字节一标签，PTRACE_PEEKMTETAGS 使用。
 */
SYM_FUNC_START(mte_copy_tags_to_user)
	mov	x3, x0
	cbz	x2, 2f
1:
	ldg	x4, [x1]
	ubfx	x4, x4, #MTE_TAG_SHIFT, #MTE_TAG_SIZE
USER(2f, sttrb	w4, [x0])
	add	x0, x0, #1
	add	x1, x1, #MTE_GRANULE_SIZE
	subs	x2, x2, #1
	b.ne	1b

	// exception handling and function return
2:	sub	x0, x0, x3		// update the number of tags copied
	ret
SYM_FUNC_END(mte_copy_tags_to_user)

/*
 * Save the tags in a page
 *   x0 - page address
 *   x1 - tag storage, MTE_PAGE_TAG_STORAGE bytes
 * 中文：将整个页面的 MTE 标签保存到缓冲区。
 * 每 16 个标签（覆盖 256 字节）合并为一个 64 位值存入缓冲区。
 */
SYM_FUNC_START(mte_save_page_tags)
	multitag_transfer_size x7, x5
1:
	mov	x2, #0
2:
	ldgm	x5, [x0]
	orr	x2, x2, x5
	add	x0, x0, x7
	tst	x0, #0xFF		// 16 tag values fit in a register,
	b.ne	2b			// which is 16*16=256 bytes

	str	x2, [x1], #8

	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b

	ret
SYM_FUNC_END(mte_save_page_tags)

/*
 * Restore the tags in a page
 *   x0 - page address
 *   x1 - tag storage, MTE_PAGE_TAG_STORAGE bytes
 * 中文：从缓冲区恢复整个页面的 MTE 标签。
 * 每 64 位值包含 16 个标签，用 STGM 逐一写入每个传输块。
 */
SYM_FUNC_START(mte_restore_page_tags)
	multitag_transfer_size x7, x5
1:
	ldr	x2, [x1], #8
2:
	stgm	x2, [x0]
	add	x0, x0, x7
	tst	x0, #0xFF
	b.ne	2b

	tst	x0, #(PAGE_SIZE - 1)
	b.ne	1b

	ret
SYM_FUNC_END(mte_restore_page_tags)
