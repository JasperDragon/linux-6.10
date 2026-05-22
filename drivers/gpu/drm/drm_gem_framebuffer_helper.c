// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drm gem framebuffer helper functions
 *
 * Copyright (C) 2017 Noralf Trønnes
 */

/**
 * DOC: GEM 帧缓冲辅助函数概述 (中文)
 *
 * 该文件实现了 GEM 对象到帧缓冲（framebuffer）的桥接辅助函数，为不子类化
 * &drm_framebuffer 且使用 &drm_gem_object 作为后端存储的驱动程序提供支持。
 *
 * 核心功能包括：
 *   - 创建、初始化和销毁基于 GEM 对象的帧缓冲
 *   - 为帧缓冲创建 GEM 句柄（handle），支持 GETFB IOCTL
 *   - 将帧缓冲的缓冲区对象映射/取消映射到内核地址空间
 *   - 管理 CPU 访问 DMA-BUF 导入缓冲区的同步
 *   - 支持 AFBC（Arm Frame Buffer Compression）帧缓冲的尺寸验证
 *
 * 简单驱动程序可直接使用 drm_gem_fb_create() 作为 &drm_mode_config_funcs.fb_create
 * 回调，无需额外定制。需要脏区域跟踪的驱动可使用 drm_gem_fb_create_with_dirty()。
 */

#include <linux/export.h>
#include <linux/slab.h>
#include <linux/module.h>

#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

MODULE_IMPORT_NS("DMA_BUF");

#define AFBC_HEADER_SIZE		16
#define AFBC_TH_LAYOUT_ALIGNMENT	8
#define AFBC_HDR_ALIGN			64
#define AFBC_SUPERBLOCK_PIXELS		256
#define AFBC_SUPERBLOCK_ALIGNMENT	128
#define AFBC_TH_BODY_START_ALIGNMENT	4096

/**
 * DOC: overview
 *
 * This library provides helpers for drivers that don't subclass
 * &drm_framebuffer and use &drm_gem_object for their backing storage.
 *
 * Drivers without additional needs to validate framebuffers can simply use
 * drm_gem_fb_create() and everything is wired up automatically. Other drivers
 * can use all parts independently.
 */

/**
 * drm_gem_fb_get_obj() - 获取帧缓冲后端的 GEM 对象
 *
 * 中文: 返回指定帧缓冲和平面索引对应的 GEM 对象指针。该函数不获取额外的
 * 引用计数，仅返回 &drm_frambuffer 已持有的对象指针。如果平面索引超出范围
 * 或对应平面没有 GEM 对象，返回 NULL 并触发警告。
 *
 * @fb: 帧缓冲
 * @plane: 平面索引
 *
 * Returns:
 * Pointer to &drm_gem_object for the given framebuffer and plane index or NULL
 * if it does not exist.
 */
struct drm_gem_object *drm_gem_fb_get_obj(struct drm_framebuffer *fb,
					  unsigned int plane)
{
	struct drm_device *dev = fb->dev;

	if (drm_WARN_ON_ONCE(dev, plane >= ARRAY_SIZE(fb->obj)))
		return NULL;
	else if (drm_WARN_ON_ONCE(dev, !fb->obj[plane]))
		return NULL;

	return fb->obj[plane];
}
EXPORT_SYMBOL_GPL(drm_gem_fb_get_obj);

static int
drm_gem_fb_init(struct drm_device *dev,
		 struct drm_framebuffer *fb,
		 const struct drm_format_info *info,
		 const struct drm_mode_fb_cmd2 *mode_cmd,
		 struct drm_gem_object **obj, unsigned int num_planes,
		 const struct drm_framebuffer_funcs *funcs)
{
	unsigned int i;
	int ret;

	drm_helper_mode_fill_fb_struct(dev, fb, info, mode_cmd);

	for (i = 0; i < num_planes; i++)
		fb->obj[i] = obj[i];

	ret = drm_framebuffer_init(dev, fb, funcs);
	if (ret)
		drm_err(dev, "Failed to init framebuffer: %d\n", ret);

	return ret;
}

/**
 * drm_gem_fb_destroy - 释放基于 GEM 的帧缓冲
 *
 * 中文: 释放基于 GEM 对象的帧缓冲及其后端缓冲区。该函数逐一释放每个平面的
 * GEM 对象引用，然后调用 drm_framebuffer_cleanup() 清理帧缓冲，最后释放
 * 帧缓冲结构体本身。驱动程序可将其用作 &drm_framebuffer_funcs->destroy 回调。
 *
 * @fb: 要释放的帧缓冲
 */
void drm_gem_fb_destroy(struct drm_framebuffer *fb)
{
	unsigned int i;

	for (i = 0; i < fb->format->num_planes; i++)
		drm_gem_object_put(fb->obj[i]);

	drm_framebuffer_cleanup(fb);
	kfree(fb);
}
EXPORT_SYMBOL(drm_gem_fb_destroy);

/**
 * drm_gem_fb_create_handle - 为基于 GEM 的帧缓冲创建句柄
 *
 * 中文: 为帧缓冲后端的 GEM 对象创建句柄。驱动程序可将其用作
 * &drm_framebuffer_funcs->create_handle 回调。GETFB IOCTL 会调用此回调。
 * 该函数仅为第一个平面（plane 0）的 GEM 对象创建句柄。
 *
 * @fb: 帧缓冲
 * @file: 注册句柄的 DRM 文件
 * @handle: 返回创建的句柄
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_fb_create_handle(struct drm_framebuffer *fb, struct drm_file *file,
			     unsigned int *handle)
{
	return drm_gem_handle_create(file, fb->obj[0], handle);
}
EXPORT_SYMBOL(drm_gem_fb_create_handle);

/**
 * drm_gem_fb_init_with_funcs() - 使用自定义函数表初始化基于 GEM 的帧缓冲
 *
 * 中文: 为需要自定义帧缓冲回调的驱动程序设置 &drm_framebuffer_funcs。
 * 该函数执行像素格式验证、GEM 对象查找、缓冲区大小验证，然后初始化帧缓冲。
 * 如果不需要自定义回调，应使用 drm_gem_fb_create()。
 * 注意：缓冲区大小验证仅针对一般情况，驱动开发者应确保其适用于自己的硬件。
 *
 * @dev: DRM 设备
 * @fb: 帧缓冲对象
 * @file: 持有帧缓冲后端 GEM 句柄的 DRM 文件
 * @info: 像素格式信息
 * @mode_cmd: 用户空间帧缓冲创建请求的元数据
 * @funcs: 新帧缓冲对象要使用的函数表
 *
 * Returns:
 * Zero or a negative error code.
 */
int drm_gem_fb_init_with_funcs(struct drm_device *dev,
			       struct drm_framebuffer *fb,
			       struct drm_file *file,
			       const struct drm_format_info *info,
			       const struct drm_mode_fb_cmd2 *mode_cmd,
			       const struct drm_framebuffer_funcs *funcs)
{
	struct drm_gem_object *objs[DRM_FORMAT_MAX_PLANES];
	unsigned int i;
	int ret;

	if (drm_drv_uses_atomic_modeset(dev) &&
	    !drm_any_plane_has_format(dev, mode_cmd->pixel_format,
				      mode_cmd->modifier[0])) {
		drm_dbg_kms(dev, "Unsupported pixel format %p4cc / modifier 0x%llx\n",
			    &mode_cmd->pixel_format, mode_cmd->modifier[0]);
		return -EINVAL;
	}

	for (i = 0; i < info->num_planes; i++) {
		unsigned int width = drm_format_info_plane_width(info, mode_cmd->width, i);
		unsigned int height = drm_format_info_plane_height(info, mode_cmd->height, i);
		unsigned int min_size;

		objs[i] = drm_gem_object_lookup(file, mode_cmd->handles[i]);
		if (!objs[i]) {
			drm_dbg_kms(dev, "Failed to lookup GEM object\n");
			ret = -ENOENT;
			goto err_gem_object_put;
		}

		min_size = (height - 1) * mode_cmd->pitches[i]
			 + drm_format_info_min_pitch(info, i, width)
			 + mode_cmd->offsets[i];

		if (objs[i]->size < min_size) {
			drm_dbg_kms(dev,
				    "GEM object size (%zu) smaller than minimum size (%u) for plane %d\n",
				    objs[i]->size, min_size, i);
			drm_gem_object_put(objs[i]);
			ret = -EINVAL;
			goto err_gem_object_put;
		}
	}

	ret = drm_gem_fb_init(dev, fb, info, mode_cmd, objs, i, funcs);
	if (ret)
		goto err_gem_object_put;

	return 0;

err_gem_object_put:
	while (i > 0) {
		--i;
		drm_gem_object_put(objs[i]);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_fb_init_with_funcs);

/**
 * drm_gem_fb_create_with_funcs() - 使用自定义函数表创建帧缓冲
 *
 * 中文: 分配帧缓冲结构体并使用给定的自定义函数表调用
 * drm_gem_fb_init_with_funcs() 完成初始化。适用于需要自定义
 * &drm_framebuffer_funcs 的驱动程序。如果不需要自定义函数表，
 * 应使用更简洁的 drm_gem_fb_create()。
 *
 * @dev: DRM 设备
 * @file: 持有帧缓冲后端 GEM 句柄的 DRM 文件
 * @info: 像素格式信息
 * @mode_cmd: 用户空间帧缓冲创建请求的元数据
 * @funcs: 新帧缓冲对象要使用的函数表
 *
 * Returns:
 * Pointer to a &drm_framebuffer on success or an error pointer on failure.
 */
struct drm_framebuffer *
drm_gem_fb_create_with_funcs(struct drm_device *dev, struct drm_file *file,
			     const struct drm_format_info *info,
			     const struct drm_mode_fb_cmd2 *mode_cmd,
			     const struct drm_framebuffer_funcs *funcs)
{
	struct drm_framebuffer *fb;
	int ret;

	fb = kzalloc_obj(*fb);
	if (!fb)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_fb_init_with_funcs(dev, fb, file, info, mode_cmd, funcs);
	if (ret) {
		kfree(fb);
		return ERR_PTR(ret);
	}

	return fb;
}
EXPORT_SYMBOL_GPL(drm_gem_fb_create_with_funcs);

static const struct drm_framebuffer_funcs drm_gem_fb_funcs = {
	.destroy	= drm_gem_fb_destroy,
	.create_handle	= drm_gem_fb_create_handle,
};

/**
 * drm_gem_fb_create() - 创建基于 GEM 的帧缓冲（核心辅助函数）
 *
 * 中文: 创建一个由 &drm_mode_fb_cmd2 描述的帧缓冲对象。
 * 此函数是 &drm_mode_config_funcs.fb_create 回调的标准实现。
 * 它会验证像素格式与可用平面是否匹配、检查缓冲区大小是否足够，
 * 然后调用 drm_gem_fb_init_with_funcs() 完成初始化。
 * 对于有特殊对齐或 pitch 要求的硬件，驱动程序应在调用此函数前进行检查。
 * 如果需要帧缓冲刷新功能，应使用 drm_gem_fb_create_with_dirty()。
 *
 * @dev: DRM 设备
 * @file: 持有帧缓冲后端 GEM 句柄的 DRM 文件
 * @info: 像素格式信息
 * @mode_cmd: 用户空间帧缓冲创建请求的元数据
 *
 * Returns:
 * Pointer to a &drm_framebuffer on success or an error pointer on failure.
 */
struct drm_framebuffer *
drm_gem_fb_create(struct drm_device *dev, struct drm_file *file,
		  const struct drm_format_info *info,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_gem_fb_create_with_funcs(dev, file, info, mode_cmd,
					    &drm_gem_fb_funcs);
}
EXPORT_SYMBOL_GPL(drm_gem_fb_create);

static const struct drm_framebuffer_funcs drm_gem_fb_funcs_dirtyfb = {
	.destroy	= drm_gem_fb_destroy,
	.create_handle	= drm_gem_fb_create_handle,
	.dirty		= drm_atomic_helper_dirtyfb,
};

/**
 * drm_gem_fb_create_with_dirty() - 创建支持脏区域跟踪的帧缓冲
 *
 * 中文: 创建带脏区域回调（dirty callback）的帧缓冲对象。与 drm_gem_fb_create()
 * 的区别在于，该函数设置 drm_atomic_helper_dirtyfb() 作为 dirty 回调，
 * 使帧缓冲刷新可以通过原子机制进行。驱动程序还应在所有平面上调用
 * drm_plane_enable_fb_damage_clips() 以允许用户空间通过 ATOMIC IOCTL
 * 使用损坏区域剪辑（damage clips）。
 *
 * @dev: DRM 设备
 * @file: 持有帧缓冲后端 GEM 句柄的 DRM 文件
 * @info: 像素格式信息
 * @mode_cmd: 用户空间帧缓冲创建请求的元数据
 *
 * Returns:
 * Pointer to a &drm_framebuffer on success or an error pointer on failure.
 */
struct drm_framebuffer *
drm_gem_fb_create_with_dirty(struct drm_device *dev, struct drm_file *file,
			     const struct drm_format_info *info,
			     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_gem_fb_create_with_funcs(dev, file, info, mode_cmd,
					    &drm_gem_fb_funcs_dirtyfb);
}
EXPORT_SYMBOL_GPL(drm_gem_fb_create_with_dirty);

/**
 * drm_gem_fb_vmap - 将所有帧缓冲 BO 映射到内核地址空间
 *
 * 中文: 将给定帧缓冲的所有缓冲区对象（BO）映射到内核地址空间，并将映射
 * 存储在 struct iosys_map 数组中。如果某个 BO 的映射操作失败，函数会自动
 * 取消已建立的所有映射。如果需要访问 BO 中存储的实际数据，应传入 @data
 * 参数，它会返回考虑帧缓冲 offsets 字段后的数据地址。
 * 映射应通过 drm_gem_fb_vunmap() 取消。
 *
 * @fb: 帧缓冲
 * @map: 返回每个 BO 的映射地址
 * @data: 返回每个 BO 的数据地址（可为 NULL）
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_gem_fb_vmap(struct drm_framebuffer *fb, struct iosys_map *map,
		    struct iosys_map *data)
{
	struct drm_gem_object *obj;
	unsigned int i;
	int ret;

	for (i = 0; i < fb->format->num_planes; ++i) {
		obj = drm_gem_fb_get_obj(fb, i);
		if (!obj) {
			ret = -EINVAL;
			goto err_drm_gem_vunmap;
		}
		ret = drm_gem_vmap(obj, &map[i]);
		if (ret)
			goto err_drm_gem_vunmap;
	}

	if (data) {
		for (i = 0; i < fb->format->num_planes; ++i) {
			memcpy(&data[i], &map[i], sizeof(data[i]));
			if (iosys_map_is_null(&data[i]))
				continue;
			iosys_map_incr(&data[i], fb->offsets[i]);
		}
	}

	return 0;

err_drm_gem_vunmap:
	while (i) {
		--i;
		obj = drm_gem_fb_get_obj(fb, i);
		if (!obj)
			continue;
		drm_gem_vunmap(obj, &map[i]);
	}
	return ret;
}
EXPORT_SYMBOL(drm_gem_fb_vmap);

/**
 * drm_gem_fb_vunmap - 从内核地址空间取消帧缓冲 BO 的映射
 *
 * 中文: 取消给定帧缓冲的所有缓冲区对象的内核地址空间映射。
 * 该函数与 drm_gem_fb_vmap() 配对使用。遍历每个平面的 GEM 对象，
 * 跳过空映射后调用 drm_gem_vunmap() 解除映射。
 *
 * @fb: 帧缓冲
 * @map: drm_gem_fb_vmap() 返回的映射地址数组
 */
void drm_gem_fb_vunmap(struct drm_framebuffer *fb, struct iosys_map *map)
{
	unsigned int i = fb->format->num_planes;
	struct drm_gem_object *obj;

	while (i) {
		--i;
		obj = drm_gem_fb_get_obj(fb, i);
		if (!obj)
			continue;
		if (iosys_map_is_null(&map[i]))
			continue;
		drm_gem_vunmap(obj, &map[i]);
	}
}
EXPORT_SYMBOL(drm_gem_fb_vunmap);

static void __drm_gem_fb_end_cpu_access(struct drm_framebuffer *fb, enum dma_data_direction dir,
					unsigned int num_planes)
{
	struct dma_buf_attachment *import_attach;
	struct drm_gem_object *obj;
	int ret;

	while (num_planes) {
		--num_planes;
		obj = drm_gem_fb_get_obj(fb, num_planes);
		if (!obj)
			continue;
		import_attach = obj->import_attach;
		if (!drm_gem_is_imported(obj))
			continue;
		ret = dma_buf_end_cpu_access(import_attach->dmabuf, dir);
		if (ret)
			drm_err(fb->dev, "dma_buf_end_cpu_access(%u, %d) failed: %d\n",
				ret, num_planes, dir);
	}
}

/**
 * drm_gem_fb_begin_cpu_access - 为 CPU 访问准备 GEM 缓冲区对象
 *
 * 中文: 准备帧缓冲的 GEM 缓冲区对象以供 CPU 访问。在内核中访问 BO 数据
 * 前必须调用此函数。对于 dma-buf 导入的 BO，该函数调用 dma_buf_begin_cpu_access()
 * 确保缓存一致性。访问完成后应调用 drm_gem_fb_end_cpu_access()。
 *
 * @fb: 帧缓冲
 * @dir: 访问模式（DMA 数据方向）
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_gem_fb_begin_cpu_access(struct drm_framebuffer *fb, enum dma_data_direction dir)
{
	struct dma_buf_attachment *import_attach;
	struct drm_gem_object *obj;
	unsigned int i;
	int ret;

	for (i = 0; i < fb->format->num_planes; ++i) {
		obj = drm_gem_fb_get_obj(fb, i);
		if (!obj) {
			ret = -EINVAL;
			goto err___drm_gem_fb_end_cpu_access;
		}
		import_attach = obj->import_attach;
		if (!drm_gem_is_imported(obj))
			continue;
		ret = dma_buf_begin_cpu_access(import_attach->dmabuf, dir);
		if (ret)
			goto err___drm_gem_fb_end_cpu_access;
	}

	return 0;

err___drm_gem_fb_end_cpu_access:
	__drm_gem_fb_end_cpu_access(fb, dir, i);
	return ret;
}
EXPORT_SYMBOL(drm_gem_fb_begin_cpu_access);

/**
 * drm_gem_fb_end_cpu_access - 通知 CPU 对 GEM 缓冲区对象的访问结束
 *
 * 中文: 通知对帧缓冲的 GEM 缓冲区对象的 CPU 访问已结束。此函数必须与
 * drm_gem_fb_begin_cpu_access() 配对调用。对于 dma-buf 导入的 BO，
 * 调用 dma_buf_end_cpu_access() 完成缓存同步。
 *
 * @fb: 帧缓冲
 * @dir: 访问模式
 */
void drm_gem_fb_end_cpu_access(struct drm_framebuffer *fb, enum dma_data_direction dir)
{
	__drm_gem_fb_end_cpu_access(fb, dir, fb->format->num_planes);
}
EXPORT_SYMBOL(drm_gem_fb_end_cpu_access);

// TODO Drop this function and replace by drm_format_info_bpp() once all
// DRM_FORMAT_* provide proper block info in drivers/gpu/drm/drm_fourcc.c
static __u32 drm_gem_afbc_get_bpp(struct drm_device *dev,
				  const struct drm_format_info *info,
				  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	switch (info->format) {
	case DRM_FORMAT_YUV420_8BIT:
		return 12;
	case DRM_FORMAT_YUV420_10BIT:
		return 15;
	case DRM_FORMAT_VUY101010:
		return 30;
	default:
		return drm_format_info_bpp(info, 0);
	}
}

static int drm_gem_afbc_min_size(struct drm_device *dev,
				 const struct drm_format_info *info,
				 const struct drm_mode_fb_cmd2 *mode_cmd,
				 struct drm_afbc_framebuffer *afbc_fb)
{
	__u32 n_blocks, w_alignment, h_alignment, hdr_alignment;
	/* remove bpp when all users properly encode cpp in drm_format_info */
	__u32 bpp;

	switch (mode_cmd->modifier[0] & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK) {
	case AFBC_FORMAT_MOD_BLOCK_SIZE_16x16:
		afbc_fb->block_width = 16;
		afbc_fb->block_height = 16;
		break;
	case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8:
		afbc_fb->block_width = 32;
		afbc_fb->block_height = 8;
		break;
	/* no user exists yet - fall through */
	case AFBC_FORMAT_MOD_BLOCK_SIZE_64x4:
	case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8_64x4:
	default:
		drm_dbg_kms(dev, "Invalid AFBC_FORMAT_MOD_BLOCK_SIZE: %lld.\n",
			    mode_cmd->modifier[0]
			    & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK);
		return -EINVAL;
	}

	/* tiled header afbc */
	w_alignment = afbc_fb->block_width;
	h_alignment = afbc_fb->block_height;
	hdr_alignment = AFBC_HDR_ALIGN;
	if (mode_cmd->modifier[0] & AFBC_FORMAT_MOD_TILED) {
		w_alignment *= AFBC_TH_LAYOUT_ALIGNMENT;
		h_alignment *= AFBC_TH_LAYOUT_ALIGNMENT;
		hdr_alignment = AFBC_TH_BODY_START_ALIGNMENT;
	}

	afbc_fb->aligned_width = ALIGN(mode_cmd->width, w_alignment);
	afbc_fb->aligned_height = ALIGN(mode_cmd->height, h_alignment);
	afbc_fb->offset = mode_cmd->offsets[0];

	bpp = drm_gem_afbc_get_bpp(dev, info, mode_cmd);
	if (!bpp) {
		drm_dbg_kms(dev, "Invalid AFBC bpp value: %d\n", bpp);
		return -EINVAL;
	}

	n_blocks = (afbc_fb->aligned_width * afbc_fb->aligned_height)
		   / AFBC_SUPERBLOCK_PIXELS;
	afbc_fb->afbc_size = ALIGN(n_blocks * AFBC_HEADER_SIZE, hdr_alignment);
	afbc_fb->afbc_size += n_blocks * ALIGN(bpp * AFBC_SUPERBLOCK_PIXELS / 8,
					       AFBC_SUPERBLOCK_ALIGNMENT);

	return 0;
}

/**
 * drm_gem_fb_afbc_init() - 为支持 AFBC 的驱动程序初始化帧缓冲
 *
 * 中文: 供支持 AFBC（Arm Frame Buffer Compression，ARM 帧缓冲压缩）的
 * 驱动程序使用，完成 struct drm_afbc_framebuffer 的准备和验证工作。
 * 必须在分配该结构体并调用 drm_gem_fb_init_with_funcs() 之后调用。
 * 如果调用失败，调用者负责释放 afbc_fb->base.obj 中的对象。
 * 该函数计算 AFBC 头部大小和主体大小的最小值，并验证 GEM 对象大小是否足够。
 *
 * @dev: DRM 设备
 * @afbc_fb: AFBC 特有的帧缓冲
 * @info: 像素格式信息
 * @mode_cmd: 用户空间帧缓冲创建请求的元数据
 *
 * Returns:
 * Zero on success or a negative error value on failure.
 */
int drm_gem_fb_afbc_init(struct drm_device *dev,
			 const struct drm_format_info *info,
			 const struct drm_mode_fb_cmd2 *mode_cmd,
			 struct drm_afbc_framebuffer *afbc_fb)
{
	struct drm_gem_object **objs;
	int ret;

	objs = afbc_fb->base.obj;

	ret = drm_gem_afbc_min_size(dev, info, mode_cmd, afbc_fb);
	if (ret < 0)
		return ret;

	if (objs[0]->size < afbc_fb->afbc_size) {
		drm_dbg_kms(dev, "GEM object size (%zu) smaller than minimum afbc size (%u)\n",
			    objs[0]->size, afbc_fb->afbc_size);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gem_fb_afbc_init);
