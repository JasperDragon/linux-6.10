// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * DOC: DRM 伙伴分配器打印辅助函数概述 (中文)
 *
 * 该文件提供 DRM 特定的伙伴分配器（Buddy Allocator）打印辅助函数。
 * 伙伴分配器是一种高效的内存分配算法，通过将内存块反复对半分裂来
 * 管理空闲内存，分配和释放时可以合并相邻的空闲块以减小碎片。
 *
 * 该文件的函数主要用于调试和状态查看：
 *   - drm_buddy_block_print()：打印单个内存块的信息
 *   - drm_buddy_print()：打印整个伙伴分配器的状态，包括每个 order
 *     的空闲块数量和大小
 *
 * 底层伙伴分配器算法实现在通用 GPU 伙伴分配器（gpu_buddy）中，
 * 此文件为其提供 DRM 风格的打印输出集成。
 */

#include <kunit/test-bug.h>

#include <linux/export.h>
#include <linux/kmemleak.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include <linux/gpu_buddy.h>
#include <drm/drm_buddy.h>
#include <drm/drm_print.h>

/**
 * drm_buddy_block_print - 打印伙伴分配器内存块信息
 *
 * 中文: 打印单个伙伴分配器内存块的详细信息，包括块的起始地址、结束地址
 * 和大小。使用 DRM printer 进行格式化输出，便于集成到 debugfs 中。
 *
 * @mm: DRM 伙伴分配器管理器
 * @block: DRM 伙伴分配器内存块
 * @p: 使用的 DRM printer
 */
void drm_buddy_block_print(struct gpu_buddy *mm,
			   struct gpu_buddy_block *block,
			   struct drm_printer *p)
{
	u64 start = gpu_buddy_block_offset(block);
	u64 size = gpu_buddy_block_size(mm, block);

	drm_printf(p, "%#018llx-%#018llx: %llu\n", start, start + size, size);
}
EXPORT_SYMBOL(drm_buddy_block_print);

/**
 * drm_buddy_print - 打印伙伴分配器完整状态
 *
 * 中文: 打印 DRM 伙伴分配器的完整状态信息。首先输出分配器的总体信息：
 * chunk_size（最小块大小）、总大小、空闲大小和 clear_free 大小。
 * 然后从最高 order 到最低 order 遍历所有空闲树，统计每个 order 的空闲块
 * 数量和总大小，并格式化输出。对于超过 1MiB 的大小使用 MiB 单位，
 * 否则使用 KiB 单位。便于集成到 debugfs 中进行内存分配调试。
 *
 * @mm: DRM 伙伴分配器管理器
 * @p: 使用的 DRM printer
 */
void drm_buddy_print(struct gpu_buddy *mm, struct drm_printer *p)
{
	int order;

	drm_printf(p, "chunk_size: %lluKiB, total: %lluMiB, free: %lluMiB, clear_free: %lluMiB\n",
		   mm->chunk_size >> 10, mm->size >> 20, mm->avail >> 20, mm->clear_avail >> 20);

	for (order = mm->max_order; order >= 0; order--) {
		struct gpu_buddy_block *block, *tmp;
		struct rb_root *root;
		u64 count = 0, free;
		unsigned int tree;

		for_each_free_tree(tree) {
			root = &mm->free_trees[tree][order];

			rbtree_postorder_for_each_entry_safe(block, tmp, root, rb) {
				BUG_ON(!gpu_buddy_block_is_free(block));
				count++;
			}
		}

		drm_printf(p, "order-%2d ", order);

		free = count * (mm->chunk_size << order);
		if (free < SZ_1M)
			drm_printf(p, "free: %8llu KiB", free >> 10);
		else
			drm_printf(p, "free: %8llu MiB", free >> 20);

		drm_printf(p, ", blocks: %llu\n", count);
	}
}
EXPORT_SYMBOL(drm_buddy_print);

MODULE_DESCRIPTION("DRM-specific GPU Buddy Allocator Print Helpers");
MODULE_LICENSE("Dual MIT/GPL");
