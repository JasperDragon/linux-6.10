// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * 中文注释: DRM GEM TTM 辅助函数 (GEM TTM Helper)
 *
 * 本文件实现了基于 TTM (Translation Table Maps) 的 GEM 缓冲区对象
 * 辅助函数。TTM 是 DRM 子系统中的通用内存管理框架, 支持多种内存
 * 类型 (系统内存、TT 页表内存、VRAM 视频内存等) 和对象迁移。
 *
 * 提供的功能:
 *   - drm_gem_ttm_print_info: 打印 TTM 缓冲区对象信息 (用于 debugfs)
 *   - drm_gem_ttm_vmap / vunmap: TTM 缓冲区对象的内核态映射管理
 *   - drm_gem_ttm_mmap: TTM 缓冲区对象的用户态映射 (mmap)
 *   - drm_gem_ttm_dumb_map_offset: Dumb 缓冲区的偏移量查询
 *
 * 这些函数通常被用作 drm_gem_object_funcs 中的回调函数, 是 TTM 驱动
 * 实现标准 GEM 接口的通用构建块。
 */

#include <linux/export.h>
#include <linux/module.h>

#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_print.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>

/**
 * DOC: overview
 *
 * This library provides helper functions for gem objects backed by
 * ttm.
 */

/**
 * 中文注释: 打印 TTM 缓冲区对象信息 (用于 debugfs)
 * 输出 TTM 缓冲区对象的放置位置 (placement) 信息, 包括内存类型
 * (system/tt/vram/priv) 和缓存属性 (cached/uncached/wc) 等。
 * 如果缓冲区有总线偏移 (bus.offset), 也会一并输出。
 * 可用作 drm_gem_object_funcs.print_info 回调函数。
 *
 * drm_gem_ttm_print_info() - Print &ttm_buffer_object info for debugfs
 * @p: DRM printer
 * @indent: Tab indentation level
 * @gem: GEM object
 *
 * This function can be used as &drm_gem_object_funcs.print_info
 * callback.
 */
void drm_gem_ttm_print_info(struct drm_printer *p, unsigned int indent,
			    const struct drm_gem_object *gem)
{
	static const char * const plname[] = {
		[ TTM_PL_SYSTEM ] = "system",
		[ TTM_PL_TT     ] = "tt",
		[ TTM_PL_VRAM   ] = "vram",
		[ TTM_PL_PRIV   ] = "priv",

		[ 16 ]            = "cached",
		[ 17 ]            = "uncached",
		[ 18 ]            = "wc",
		[ 19 ]            = "contig",

		[ 21 ]            = "pinned", /* NO_EVICT */
		[ 22 ]            = "topdown",
	};
	const struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(gem);

	drm_printf_indent(p, indent, "placement=");
	drm_print_bits(p, bo->resource->placement, plname, ARRAY_SIZE(plname));
	drm_printf(p, "\n");

	if (bo->resource->bus.is_iomem)
		drm_printf_indent(p, indent, "bus.offset=%lx\n",
				  (unsigned long)bo->resource->bus.offset);
}
EXPORT_SYMBOL(drm_gem_ttm_print_info);

/**
 * 中文注释: 映射 TTM 缓冲区对象到内核地址空间
 * 通过 ttm_bo_vmap() 将 GEM/TTM 缓冲区映射到内核地址空间。
 * 映射结果通过 struct iosys_map 返回, 该结构可以表示系统内存
 * 或 I/O 内存的虚拟地址。可用作 drm_gem_object_funcs.vmap 回调。
 *
 * drm_gem_ttm_vmap() - vmap &ttm_buffer_object
 * @gem: GEM object.
 * @map: [out] returns the dma-buf mapping.
 *
 * Maps a GEM object with ttm_bo_vmap(). This function can be used as
 * &drm_gem_object_funcs.vmap callback.
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_gem_ttm_vmap(struct drm_gem_object *gem,
		     struct iosys_map *map)
{
	struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(gem);

	return ttm_bo_vmap(bo, map);
}
EXPORT_SYMBOL(drm_gem_ttm_vmap);

/**
 * 中文注释: 取消映射 TTM 缓冲区对象
 * 通过 ttm_bo_vunmap() 取消之前通过 drm_gem_ttm_vmap() 建立的内核
 * 映射。可用作 drm_gem_object_funcs.vunmap 回调。
 *
 * drm_gem_ttm_vunmap() - vunmap &ttm_buffer_object
 * @gem: GEM object.
 * @map: dma-buf mapping.
 *
 * Unmaps a GEM object with ttm_bo_vunmap(). This function can be used as
 * &drm_gem_object_funcs.vmap callback.
 */
void drm_gem_ttm_vunmap(struct drm_gem_object *gem,
			struct iosys_map *map)
{
	struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(gem);

	ttm_bo_vunmap(bo, map);
}
EXPORT_SYMBOL(drm_gem_ttm_vunmap);

/**
 * 中文注释: 映射 TTM 缓冲区对象到用户地址空间 (mmap)
 * 通过 ttm_bo_mmap_obj() 将 TTM 缓冲区对象映射到用户空间。
 * TTM 拥有自己的对象引用计数, 因此需要在映射后释放 GEM 引用,
 * 避免双重计数。可用作 drm_gem_object_funcs.mmap 回调。
 *
 * drm_gem_ttm_mmap() - mmap &ttm_buffer_object
 * @gem: GEM object.
 * @vma: vm area.
 *
 * This function can be used as &drm_gem_object_funcs.mmap
 * callback.
 */
int drm_gem_ttm_mmap(struct drm_gem_object *gem,
		     struct vm_area_struct *vma)
{
	struct ttm_buffer_object *bo = drm_gem_ttm_of_gem(gem);
	int ret;

	ret = ttm_bo_mmap_obj(vma, bo);
	if (ret < 0)
		return ret;

	/*
	 * ttm has its own object refcounting, so drop gem reference
	 * to avoid double accounting counting.
	 */
	drm_gem_object_put(gem);

	return 0;
}
EXPORT_SYMBOL(drm_gem_ttm_mmap);

/**
 * 中文注释: 实现 TTM 驱动的 dumb_map_offset 回调
 * 通过 GEM 句柄查找对象, 并返回其 VMA 偏移节点地址 (即用户空间
 * mmap 时使用的偏移量)。TTM 在内部管理这些偏移量的分配。
 * 适用于基于 TTM 的 GEM 驱动程序实现 dumb 缓冲区的偏移查询。
 *
 * drm_gem_ttm_dumb_map_offset() - Implements struct &drm_driver.dumb_map_offset
 * @file:	DRM file pointer.
 * @dev:	DRM device.
 * @handle:	GEM handle
 * @offset:	Returns the mapping's memory offset on success
 *
 * Provides an implementation of struct &drm_driver.dumb_map_offset for
 * TTM-based GEM drivers. TTM allocates the offset internally and
 * drm_gem_ttm_dumb_map_offset() returns it for dumb-buffer implementations.
 *
 * See struct &drm_driver.dumb_map_offset.
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_gem_ttm_dumb_map_offset(struct drm_file *file, struct drm_device *dev,
				uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem;

	gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return -ENOENT;

	*offset = drm_vma_node_offset_addr(&gem->vma_node);

	drm_gem_object_put(gem);

	return 0;
}
EXPORT_SYMBOL(drm_gem_ttm_dumb_map_offset);

MODULE_DESCRIPTION("DRM gem ttm helpers");
MODULE_LICENSE("GPL");
