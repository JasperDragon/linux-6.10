// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drm kms/fb dma helper functions
 *
 * Copyright (C) 2012 Analog Devices Inc.
 *   Author: Lars-Peter Clausen <lars@metafoo.de>
 *
 * Based on udl_fbdev.c
 *  Copyright (C) 2012 Red Hat
 */

/*
 * 中文注释: DRM DMA 帧缓冲辅助函数 (DMA Framebuffer Helper)
 *
 * 本文件提供了基于 DMA-contiguous 帧缓冲的辅助函数, 适用于使用
 * DMA API 分配连续内存的 DRM 驱动程序。根据平台不同, 这些缓冲区
 * 可能是通过 IOMMU 映射的物理非连续内存, 或是通过 CMA 等机制分配
 * 的物理连续内存。
 *
 * 主要功能:
 *   1. drm_fb_dma_get_gem_obj: 获取帧缓冲指定平面的 DMA GEM 对象
 *   2. drm_fb_dma_get_gem_addr: 计算帧缓冲指定平面的 DMA (总线)
 *      地址, 考虑像素格式的块布局 (block width/height) 和子采样
 *      因子 (hsub/vsub)
 *   3. drm_fb_dma_sync_non_coherent: 同步非一致性 (non-coherent)
 *      内存的 DMA 缓冲区, 确保 CPU 写入的数据对设备可见
 *   4. drm_fb_dma_get_scanout_buffer: 获取 panic 处理程序的扫描输出
 *      缓冲区, 在系统崩溃时显示 panic 屏幕
 *
 * 这些函数通常从 CRTC 或 PLANE 的回调函数中调用。
 */

#include <drm/drm_damage_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_panic.h>
#include <drm/drm_plane.h>

#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/module.h>

/**
 * DOC: framebuffer dma helper functions
 *
 * Provides helper functions for creating a DMA-contiguous framebuffer.
 *
 * Depending on the platform, the buffers may be physically non-contiguous and
 * mapped through an IOMMU or a similar mechanism, or allocated from
 * physically-contiguous memory (using, for instance, CMA or a pool of memory
 * reserved at early boot). This is handled behind the scenes by the DMA mapping
 * API.
 *
 * drm_gem_fb_create() is used in the &drm_mode_config_funcs.fb_create
 * callback function to create a DMA-contiguous framebuffer.
 */

/**
 * 中文注释: 获取帧缓冲的 DMA GEM 对象
 * 根据帧缓冲和平面索引返回对应的 DMA GEM 对象。通常从 CRTC 的
 * 回调函数中调用, 用于获取帧缓冲各平面 (如 Y/Cb/Cr) 对应的
 * GEM 对象。
 *
 * drm_fb_dma_get_gem_obj() - Get DMA GEM object for framebuffer
 * @fb: The framebuffer
 * @plane: Which plane
 *
 * Return the DMA GEM object for given framebuffer.
 *
 * This function will usually be called from the CRTC callback functions.
 */
struct drm_gem_dma_object *drm_fb_dma_get_gem_obj(struct drm_framebuffer *fb,
						  unsigned int plane)
{
	struct drm_gem_object *gem;

	gem = drm_gem_fb_get_obj(fb, plane);
	if (!gem)
		return NULL;

	return to_drm_gem_dma_obj(gem);
}
EXPORT_SYMBOL_GPL(drm_fb_dma_get_gem_obj);

/**
 * 中文注释: 获取帧缓冲的 DMA 总线地址
 * 根据帧缓冲、平面状态和平面索引计算 DMA 总线地址。对于像素格式
 * 按块 (block) 分组的情况, 此函数考虑:
 *   - 块宽度 (block_w) 和块高度 (block_h)
 *   - 色度子采样因子 (hsub, vsub) 对于非第一平面
 *   - 源裁剪 (src_x, src_y) 计算出实际数据起始位置
 * 最终地址 = GEM 对象 DMA 基址 + 平面偏移 + 行偏移 + 块内偏移。
 * 通常从 PLANE 的回调函数中调用。
 *
 * drm_fb_dma_get_gem_addr() - Get DMA (bus) address for framebuffer, for pixel
 * formats where values are grouped in blocks this will get you the beginning of
 * the block
 * @fb: The framebuffer
 * @state: Which state of drm plane
 * @plane: Which plane
 * Return the DMA GEM address for given framebuffer.
 *
 * This function will usually be called from the PLANE callback functions.
 */
dma_addr_t drm_fb_dma_get_gem_addr(struct drm_framebuffer *fb,
				   struct drm_plane_state *state,
				   unsigned int plane)
{
	struct drm_gem_dma_object *obj;
	dma_addr_t dma_addr;
	u8 h_div = 1, v_div = 1;
	u32 block_w = drm_format_info_block_width(fb->format, plane);
	u32 block_h = drm_format_info_block_height(fb->format, plane);
	u32 block_size = fb->format->char_per_block[plane];
	u32 sample_x;
	u32 sample_y;
	u32 block_start_y;
	u32 num_hblocks;

	obj = drm_fb_dma_get_gem_obj(fb, plane);
	if (!obj)
		return 0;

	dma_addr = obj->dma_addr + fb->offsets[plane];

	if (plane > 0) {
		h_div = fb->format->hsub;
		v_div = fb->format->vsub;
	}

	sample_x = (state->src_x >> 16) / h_div;
	sample_y = (state->src_y >> 16) / v_div;
	block_start_y = (sample_y / block_h) * block_h;
	num_hblocks = sample_x / block_w;

	dma_addr += fb->pitches[plane] * block_start_y;
	dma_addr += block_size * num_hblocks;

	return dma_addr;
}
EXPORT_SYMBOL_GPL(drm_fb_dma_get_gem_addr);

/**
 * 中文注释: 同步非一致性内存的 GEM 对象
 * 适用于使用 damage clips 且 DMA GEM 对象由非一致性 (non-coherent)
 * 内存支持的驱动程序。在平面的 .atomic_update 钩子中调用此函数,
 * 通过 dma_sync_single_for_device() 确保 CPU 写入帧缓冲区的数据
 * 已刷新到设备的可见范围。只同步受损区域 (damage clips) 对应的行,
 * 而非整个帧缓冲, 以提高性能。
 *
 * drm_fb_dma_sync_non_coherent - Sync GEM object to non-coherent backing
 *	memory
 * @drm: DRM device
 * @old_state: Old plane state
 * @state: New plane state
 *
 * This function can be used by drivers that use damage clips and have
 * DMA GEM objects backed by non-coherent memory. Calling this function
 * in a plane's .atomic_update ensures that all the data in the backing
 * memory have been written to RAM.
 */
void drm_fb_dma_sync_non_coherent(struct drm_device *drm,
				  struct drm_plane_state *old_state,
				  struct drm_plane_state *state)
{
	const struct drm_format_info *finfo = state->fb->format;
	struct drm_atomic_helper_damage_iter iter;
	const struct drm_gem_dma_object *dma_obj;
	unsigned int offset, i;
	struct drm_rect clip;
	dma_addr_t daddr;
	size_t nb_bytes;

	for (i = 0; i < finfo->num_planes; i++) {
		dma_obj = drm_fb_dma_get_gem_obj(state->fb, i);
		if (!dma_obj->map_noncoherent)
			continue;

		daddr = drm_fb_dma_get_gem_addr(state->fb, state, i);
		drm_atomic_helper_damage_iter_init(&iter, old_state, state);

		drm_atomic_for_each_plane_damage(&iter, &clip) {
			/* Ignore x1/x2 values, invalidate complete lines */
			offset = clip.y1 * state->fb->pitches[i];

			nb_bytes = (clip.y2 - clip.y1) * state->fb->pitches[i];
			dma_sync_single_for_device(drm->dev, daddr + offset,
						   nb_bytes, DMA_TO_DEVICE);
		}
	}
}
EXPORT_SYMBOL_GPL(drm_fb_dma_sync_non_coherent);

/**
 * 中文注释: 提供 panic 处理程序的扫描输出缓冲区
 * 通用的 get_scanout_buffer() 实现, 适用于使用 drm_fb_dma_helper
 * 的驱动程序。在系统 panic 时提供当前帧缓冲区的信息, 以便 panic
 * 屏幕显示在屏幕上。
 * 注意: 此函数不会在 panic 上下文中调用 vmap, 因此驱动程序必须
 * 确保主平面已被 vmap, 否则 panic 屏幕无法显示。
 * 仅支持线性修饰符 (DRM_FORMAT_MOD_LINEAR) 和非导入 (非 dma-buf)
 * 的 GEM 对象。
 *
 * drm_fb_dma_get_scanout_buffer - Provide a scanout buffer in case of panic
 * @plane: DRM primary plane
 * @sb: scanout buffer for the panic handler
 * Returns: 0 or negative error code
 *
 * Generic get_scanout_buffer() implementation, for drivers that uses the
 * drm_fb_dma_helper. It won't call vmap in the panic context, so the driver
 * should make sure the primary plane is vmapped, otherwise the panic screen
 * won't get displayed.
 */
int drm_fb_dma_get_scanout_buffer(struct drm_plane *plane,
				  struct drm_scanout_buffer *sb)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_framebuffer *fb;

	if (!plane->state || !plane->state->fb)
		return -EINVAL;

	fb = plane->state->fb;
	/* Only support linear modifier */
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR)
		return -ENODEV;

	dma_obj = drm_fb_dma_get_gem_obj(fb, 0);

	/* Buffer should be accessible from the CPU */
	if (drm_gem_is_imported(&dma_obj->base))
		return -ENODEV;

	/* Buffer should be already mapped to CPU */
	if (!dma_obj->vaddr)
		return -ENODEV;

	iosys_map_set_vaddr(&sb->map[0], dma_obj->vaddr);
	sb->format = fb->format;
	sb->height = fb->height;
	sb->width = fb->width;
	sb->pitch[0] = fb->pitches[0];
	return 0;
}
EXPORT_SYMBOL(drm_fb_dma_get_scanout_buffer);
