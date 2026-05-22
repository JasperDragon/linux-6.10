// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright (c) 2023 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
 */

/*
 * 文件名: drm_draw.c
 *
 * 中文描述: DRM 基本绘图辅助函数
 *
 * 本文件提供了一组底层绘图辅助函数，用于在 DRM 帧缓冲区上执行基本的像素操作。
 * 这些函数主要用于内核崩溃（panic）时的屏幕显示，以及类似的非正常模式下的
 * 图形输出场景。
 *
 * 核心功能：
 *   1. 颜色格式转换 (drm_draw_can_convert_from_xrgb8888 /
 *      drm_draw_color_from_xrgb8888) - 检查/执行从 XRGB8888 到其他颜色格式的转换
 *   2. Blit 操作 (drm_draw_blit16/24/32) - 将单色位图数据绘制到彩色帧缓冲区
 *   3. Fill 操作 (drm_draw_fill16/24/32) - 用指定颜色填充帧缓冲区区域
 *
 * 支持的输出格式包括 RGB565、RGBA5551、XRGB1555、ARGB1555、RGB888、
 * XRGB8888、ARGB8888、XBGR8888、ABGR8888、XRGB2101010、ARGB2101010、
 * ABGR2101010 等。
 */

#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/export.h>
#include <linux/iosys-map.h>
#include <linux/types.h>

#include <drm/drm_fourcc.h>

#include "drm_draw_internal.h"
#include "drm_format_internal.h"

/**
 * drm_draw_can_convert_from_xrgb8888 - 检查是否可从 XRGB8888 转换到指定格式
 *
 * 检查 drm_draw 模块是否支持从 XRGB8888 格式到指定目标格式的颜色转换。
 * 支持的格式包括 RGB565、RGBA5551、XRGB1555、ARGB1555、RGB888、
 * XRGB8888、ARGB8888、XBGR8888、ABGR8888 以及 10 位深色格式。
 */
 * @format: format
 *
 * Returns:
 * True if XRGB8888 can be converted to the specified format, false otherwise.
 */
bool drm_draw_can_convert_from_xrgb8888(u32 format)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ABGR2101010:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL(drm_draw_can_convert_from_xrgb8888);

/**
 * drm_draw_color_from_xrgb8888 - 将一个像素从 XRGB8888 转换为指定格式
 *
 * 将单个 XRGB8888 格式的像素颜色值转换为目标格式。这个函数主要用于
 * 内核崩溃（panic）时的屏幕显示场景，需要将预设的颜色值（通常为
 * XRGB8888 格式）转换为帧缓冲区的实际像素格式。
 */
 * @color: input color, in xrgb8888 format
 * @format: output format
 *
 * Returns:
 * Color in the format specified, casted to u32.
 * Or 0 if the format is not supported.
 */
u32 drm_draw_color_from_xrgb8888(u32 color, u32 format)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
		return drm_pixel_xrgb8888_to_rgb565(color);
	case DRM_FORMAT_RGBA5551:
		return drm_pixel_xrgb8888_to_rgba5551(color);
	case DRM_FORMAT_XRGB1555:
		return drm_pixel_xrgb8888_to_xrgb1555(color);
	case DRM_FORMAT_ARGB1555:
		return drm_pixel_xrgb8888_to_argb1555(color);
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_XRGB8888:
		return color;
	case DRM_FORMAT_ARGB8888:
		return drm_pixel_xrgb8888_to_argb8888(color);
	case DRM_FORMAT_XBGR8888:
		return drm_pixel_xrgb8888_to_xbgr8888(color);
	case DRM_FORMAT_ABGR8888:
		return drm_pixel_xrgb8888_to_abgr8888(color);
	case DRM_FORMAT_XRGB2101010:
		return drm_pixel_xrgb8888_to_xrgb2101010(color);
	case DRM_FORMAT_ARGB2101010:
		return drm_pixel_xrgb8888_to_argb2101010(color);
	case DRM_FORMAT_ABGR2101010:
		return drm_pixel_xrgb8888_to_abgr2101010(color);
	default:
		WARN_ONCE(1, "Can't convert to %p4cc\n", &format);
		return 0;
	}
}
EXPORT_SYMBOL(drm_draw_color_from_xrgb8888);

/*
 * drm_draw_blit16 - 将单色位图绘制到 16bpp 帧缓冲区
 * @dmap: 目标帧缓冲区映射
 * @dpitch: 目标帧缓冲区每行字节数
 * @sbuf8: 源单色位图数据（8 像素/字节）
 * @spitch: 源数据每行字节数
 * @height: 绘制区域高度
 * @width: 绘制区域宽度
 * @scale: 缩放比例（源图比目标小 scale 倍）
 * @fg16: 前景色（16 位目标格式）
 *
 * 遍历目标区域的每个像素，检查对应的源单色位图像素是否为前景色，
 * 如果是则将前景色写入目标缓冲区。
 */
void drm_draw_blit16(struct iosys_map *dmap, unsigned int dpitch,
		     const u8 *sbuf8, unsigned int spitch,
		     unsigned int height, unsigned int width,
		     unsigned int scale, u16 fg16)
{
	unsigned int y, x;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			if (drm_draw_is_pixel_fg(sbuf8, spitch, x / scale, y / scale))
				iosys_map_wr(dmap, y * dpitch + x * sizeof(u16), u16, fg16);
}
EXPORT_SYMBOL(drm_draw_blit16);

/*
 * drm_draw_blit24 - 将单色位图绘制到 24bpp 帧缓冲区
 * @dmap: 目标帧缓冲区映射
 * @dpitch: 目标帧缓冲区每行字节数
 * @sbuf8: 源单色位图数据（8 像素/字节）
 * @spitch: 源数据每行字节数
 * @height: 绘制区域高度
 * @width: 绘制区域宽度
 * @scale: 缩放比例
 * @fg32: 前景色（低 24 位为 BGR 格式）
 *
 * 24bpp 格式特殊之处在于每个像素占用 3 个字节，需要分别写入
 * 蓝、绿、红分量。
 */
void drm_draw_blit24(struct iosys_map *dmap, unsigned int dpitch,
		     const u8 *sbuf8, unsigned int spitch,
		     unsigned int height, unsigned int width,
		     unsigned int scale, u32 fg32)
{
	unsigned int y, x;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			u32 off = y * dpitch + x * 3;

			if (drm_draw_is_pixel_fg(sbuf8, spitch, x / scale, y / scale)) {
				/* write blue-green-red to output in little endianness */
				iosys_map_wr(dmap, off, u8, (fg32 & 0x000000FF) >> 0);
				iosys_map_wr(dmap, off + 1, u8, (fg32 & 0x0000FF00) >> 8);
				iosys_map_wr(dmap, off + 2, u8, (fg32 & 0x00FF0000) >> 16);
			}
		}
	}
}
EXPORT_SYMBOL(drm_draw_blit24);

/*
 * drm_draw_blit32 - 将单色位图绘制到 32bpp 帧缓冲区
 * @dmap: 目标帧缓冲区映射
 * @dpitch: 目标帧缓冲区每行字节数
 * @sbuf8: 源单色位图数据（8 像素/字节）
 * @spitch: 源数据每行字节数
 * @height: 绘制区域高度
 * @width: 绘制区域宽度
 * @scale: 缩放比例
 * @fg32: 前景色（32 位目标格式）
 */
void drm_draw_blit32(struct iosys_map *dmap, unsigned int dpitch,
		     const u8 *sbuf8, unsigned int spitch,
		     unsigned int height, unsigned int width,
		     unsigned int scale, u32 fg32)
{
	unsigned int y, x;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			if (drm_draw_is_pixel_fg(sbuf8, spitch, x / scale, y / scale))
				iosys_map_wr(dmap, y * dpitch + x * sizeof(u32), u32, fg32);
}
EXPORT_SYMBOL(drm_draw_blit32);

/*
 * drm_draw_fill16 - 用指定颜色填充 16bpp 帧缓冲区区域
 * @dmap: 目标帧缓冲区映射
 * @dpitch: 目标每行字节数
 * @height: 填充区域高度
 * @width: 填充区域宽度
 * @color: 填充颜色（16 位格式）
 *
 * 用指定颜色填充帧缓冲区中的矩形区域。
 */
void drm_draw_fill16(struct iosys_map *dmap, unsigned int dpitch,
		     unsigned int height, unsigned int width,
		     u16 color)
{
	unsigned int y, x;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			iosys_map_wr(dmap, y * dpitch + x * sizeof(u16), u16, color);
}
EXPORT_SYMBOL(drm_draw_fill16);

/*
 * drm_draw_fill24 - 用指定颜色填充 24bpp 帧缓冲区区域
 * @dmap: 目标帧缓冲区映射
 * @dpitch: 目标每行字节数
 * @height: 填充区域高度
 * @width: 填充区域宽度
 * @color: 填充颜色（低 24 位为 BGR）
 */
void drm_draw_fill24(struct iosys_map *dmap, unsigned int dpitch,
		     unsigned int height, unsigned int width,
		     u32 color)
{
	unsigned int y, x;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned int off = y * dpitch + x * 3;

			/* write blue-green-red to output in little endianness */
			iosys_map_wr(dmap, off, u8, (color & 0x000000FF) >> 0);
			iosys_map_wr(dmap, off + 1, u8, (color & 0x0000FF00) >> 8);
			iosys_map_wr(dmap, off + 2, u8, (color & 0x00FF0000) >> 16);
		}
	}
}
EXPORT_SYMBOL(drm_draw_fill24);

/*
 * drm_draw_fill32 - 用指定颜色填充 32bpp 帧缓冲区区域
 * @dmap: 目标帧缓冲区映射
 * @dpitch: 目标每行字节数
 * @height: 填充区域高度
 * @width: 填充区域宽度
 * @color: 填充颜色（32 位格式）
 */
void drm_draw_fill32(struct iosys_map *dmap, unsigned int dpitch,
		     unsigned int height, unsigned int width,
		     u32 color)
{
	unsigned int y, x;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			iosys_map_wr(dmap, y * dpitch + x * sizeof(u32), u32, color);
}
EXPORT_SYMBOL(drm_draw_fill32);
