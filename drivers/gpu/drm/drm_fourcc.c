/*
 * Copyright (c) 2016 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * DRM core format related functions
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/*
 * DRM 像素格式辅助 - 中文注释补充
 *
 * 本文件提供 DRM 像素格式（FourCC）的查询和转换辅助函数。
 * DRM 使用四字符码（FourCC）唯一标识像素格式，每个格式定义了
 * 颜色分量在内存中的排列方式。
 *
 * 主要功能：
 *   1. legacy fb 格式转换：将传统的 bpp/depth 描述转换为 FourCC 格式
 *   2. 格式信息查询：通过 drm_format_info() 获取格式的详细描述
 *      （包括平面数、每像素字节数、色度采样、是否有 alpha 通道等）
 *   3. 格式信息辅助：计算块宽度、块高度、每像素位数、最小 pitch
 *
 * 支持的格式涵盖：
 *   - 索引色格式（C1/C2/C4/C8）
 *   - RGB 格式（各种位深的 RGB/BGR/XRGB/ARGB 等）
 *   - YUV 格式（平面/半平面/打包格式，各种采样率）
 *   - 高动态范围格式（HDR，如 10/12/16 位格式）
 *   - 自定义块格式（block-based 压缩格式）
 */

#include <linux/bug.h>
#include <linux/ctype.h>
#include <linux/export.h>
#include <linux/kernel.h>

#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>

/**
 * drm_mode_legacy_fb_format - compute drm fourcc code from legacy description
 * @bpp: bits per pixels
 * @depth: bit depth per pixel
 *
 * Computes a drm fourcc pixel format code for the given @bpp/@depth values.
 */
/*
 * 中文说明：将传统的 bpp（每像素位数）和 depth（颜色深度）描述
 * 转换为 DRM FourCC 像素格式码。用于处理遗留 fb 驱动和
 * 传统用户空间的格式请求。
 */
uint32_t drm_mode_legacy_fb_format(uint32_t bpp, uint32_t depth)
{
	uint32_t fmt = DRM_FORMAT_INVALID;

	switch (bpp) {
	case 1:
		if (depth == 1)
			fmt = DRM_FORMAT_C1;
		break;

	case 2:
		if (depth == 2)
			fmt = DRM_FORMAT_C2;
		break;

	case 4:
		if (depth == 4)
			fmt = DRM_FORMAT_C4;
		break;

	case 8:
		if (depth == 8)
			fmt = DRM_FORMAT_C8;
		break;

	case 16:
		switch (depth) {
		case 15:
			fmt = DRM_FORMAT_XRGB1555;
			break;
		case 16:
			fmt = DRM_FORMAT_RGB565;
			break;
		default:
			break;
		}
		break;

	case 24:
		if (depth == 24)
			fmt = DRM_FORMAT_RGB888;
		break;

	case 32:
		switch (depth) {
		case 24:
			fmt = DRM_FORMAT_XRGB8888;
			break;
		case 30:
			fmt = DRM_FORMAT_XRGB2101010;
			break;
		case 32:
			fmt = DRM_FORMAT_ARGB8888;
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}

	return fmt;
}
EXPORT_SYMBOL(drm_mode_legacy_fb_format);

/**
 * drm_driver_legacy_fb_format - compute drm fourcc code from legacy description
 * @dev: DRM device
 * @bpp: bits per pixels
 * @depth: bit depth per pixel
 *
 * Computes a drm fourcc pixel format code for the given @bpp/@depth values.
 * Unlike drm_mode_legacy_fb_format() this looks at the drivers mode_config,
 * and depending on the &drm_mode_config.quirk_addfb_prefer_host_byte_order flag
 * it returns little endian byte order or host byte order framebuffer formats.
 */
/*
 * 中文说明：驱动级别的 legacy fb 格式转换。在基本转换基础上，
 * 考虑驱动的 quirks 标志：
 *   - quirk_addfb_prefer_host_byte_order：优先使用主机字节序
 *   - quirk_addfb_prefer_xbgr_30bpp：使用 XBGR2101010 替代 XRGB2101010
 */
uint32_t drm_driver_legacy_fb_format(struct drm_device *dev,
				     uint32_t bpp, uint32_t depth)
{
	uint32_t fmt = drm_mode_legacy_fb_format(bpp, depth);

	if (dev->mode_config.quirk_addfb_prefer_host_byte_order) {
		if (fmt == DRM_FORMAT_XRGB8888)
			fmt = DRM_FORMAT_HOST_XRGB8888;
		if (fmt == DRM_FORMAT_ARGB8888)
			fmt = DRM_FORMAT_HOST_ARGB8888;
		if (fmt == DRM_FORMAT_RGB565)
			fmt = DRM_FORMAT_HOST_RGB565;
		if (fmt == DRM_FORMAT_XRGB1555)
			fmt = DRM_FORMAT_HOST_XRGB1555;
	}

	if (dev->mode_config.quirk_addfb_prefer_xbgr_30bpp &&
	    fmt == DRM_FORMAT_XRGB2101010)
		fmt = DRM_FORMAT_XBGR2101010;

	return fmt;
}
EXPORT_SYMBOL(drm_driver_legacy_fb_format);

/**
 * drm_driver_color_mode_format - Compute DRM 4CC code from color mode
 * @dev: DRM device
 * @color_mode: command-line color mode
 *
 * Computes a DRM 4CC pixel format code for the given color mode using
 * drm_driver_color_mode(). The color mode is in the format used and the
 * kernel command line. It specifies the number of bits per pixel
 * and color depth in a single value.
 *
 * Useful in fbdev emulation code, since that deals in those values. The
 * helper does not consider YUV or other complicated formats. This means
 * only legacy formats are supported (fmt->depth is a legacy field), but
 * the framebuffer emulation can only deal with such formats, specifically
 * RGB/BGA formats.
 */
uint32_t drm_driver_color_mode_format(struct drm_device *dev, unsigned int color_mode)
{
	switch (color_mode) {
	case 15:
		return drm_driver_legacy_fb_format(dev, 16, 15);
	case 32:
		return drm_driver_legacy_fb_format(dev, 32, 24);
	default:
		return drm_driver_legacy_fb_format(dev, color_mode, color_mode);
	}
}
EXPORT_SYMBOL(drm_driver_color_mode_format);

/*
 * Internal function to query information for a given format. See
 * drm_format_info() for the public API.
 */
const struct drm_format_info *__drm_format_info(u32 format)
{
	static const struct drm_format_info formats[] = {
		{ .format = DRM_FORMAT_C1,		.depth = 1,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 8, }, .block_h = { 1, }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
		{ .format = DRM_FORMAT_C2,		.depth = 2,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 4, }, .block_h = { 1, }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
		{ .format = DRM_FORMAT_C4,		.depth = 4,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 2, }, .block_h = { 1, }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
		{ .format = DRM_FORMAT_C8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1, .is_color_indexed = true },
		{ .format = DRM_FORMAT_D1,		.depth = 1,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 8, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_D2,		.depth = 2,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 4, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_D4,		.depth = 4,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 2, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_D8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_R1,		.depth = 1,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 8, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_R2,		.depth = 2,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 4, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_R4,		.depth = 4,  .num_planes = 1,
		  .char_per_block = { 1, }, .block_w = { 2, }, .block_h = { 1, }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_R8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_R10,		.depth = 10, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_R12,		.depth = 12, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGB332,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGR233,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XRGB4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGB565,		.depth = 16, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGR565,		.depth = 16, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
#ifdef __BIG_ENDIAN
		{ .format = DRM_FORMAT_XRGB1555 | DRM_FORMAT_BIG_ENDIAN, .depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGB565 | DRM_FORMAT_BIG_ENDIAN, .depth = 16, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
#endif
		{ .format = DRM_FORMAT_RGB888,		.depth = 24, .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGR888,		.depth = 24, .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XRGB8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGB565_A8,	.depth = 24, .num_planes = 2, .cpp = { 2, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGR565_A8,	.depth = 24, .num_planes = 2, .cpp = { 2, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGB161616,	.depth = 0,
		  .num_planes = 1, .char_per_block = { 6, 0, 0 },
		  .block_w = { 1, 0, 0 }, .block_h = { 1, 0, 0 },
		  .hsub = 1, .vsub = 1, .has_alpha = false },
		{ .format = DRM_FORMAT_BGR161616,	.depth = 0,
		  .num_planes = 1, .char_per_block = { 6, 0, 0 },
		  .block_w = { 1, 0, 0 }, .block_h = { 1, 0, 0 },
		  .hsub = 1, .vsub = 1, .has_alpha = false },
		{ .format = DRM_FORMAT_ARGB8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_AXBXGXRX106106106106, .depth = 0, .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGB888_A8,	.depth = 32, .num_planes = 2, .cpp = { 3, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGR888_A8,	.depth = 32, .num_planes = 2, .cpp = { 3, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XBGR8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBX8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRX8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_YUV410,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU410,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV411,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU411,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV420,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU420,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV422,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU422,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV444,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU444,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV12,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_NV21,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_NV16,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV61,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV24,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV42,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YUYV,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVYU,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_UYVY,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_VYUY,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_XYUV8888,	.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_VUY888,          .depth = 0,  .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_AYUV,		.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_Y210,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y212,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y216,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y410,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_Y412,            .depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_Y416,            .depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_XVYU2101010,	.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_XVYU12_16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_XVYU16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y0L0,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_X0L0,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_Y0L2,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_X0L2,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_P010,            .depth = 0,  .num_planes = 2,
		  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_P012,		.depth = 0,  .num_planes = 2,
		  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
		   .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_P016,		.depth = 0,  .num_planes = 2,
		  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_P210,		.depth = 0,
		  .num_planes = 2, .char_per_block = { 2, 4, 0 },
		  .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
		  .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_VUY101010,	.depth = 0,
		  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 1, .vsub = 1,
		  .is_yuv = true },
		{ .format = DRM_FORMAT_YUV420_8BIT,     .depth = 0,
		  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 2, .vsub = 2,
		  .is_yuv = true },
		{ .format = DRM_FORMAT_YUV420_10BIT,    .depth = 0,
		  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 2, .vsub = 2,
		  .is_yuv = true },
		{ .format = DRM_FORMAT_NV15,		.depth = 0,
		  .num_planes = 2, .char_per_block = { 5, 5, 0 },
		  .block_w = { 4, 2, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
		  .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_NV20,		.depth = 0,
		  .num_planes = 2, .char_per_block = { 5, 5, 0 },
		  .block_w = { 4, 2, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
		  .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV30,		.depth = 0,
		  .num_planes = 2, .char_per_block = { 5, 5, 0 },
		  .block_w = { 4, 2, 0 }, .block_h = { 1, 1, 0 }, .hsub = 1,
		  .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Q410,		.depth = 0,
		  .num_planes = 3, .char_per_block = { 2, 2, 2 },
		  .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 }, .hsub = 1,
		  .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Q401,		.depth = 0,
		  .num_planes = 3, .char_per_block = { 2, 2, 2 },
		  .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 }, .hsub = 1,
		  .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_P030,            .depth = 0,  .num_planes = 2,
		  .char_per_block = { 4, 8, 0 }, .block_w = { 3, 3, 0 }, .block_h = { 1, 1, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_S010,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_S210,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 2, .vsub = 1, .is_yuv = true},
		{ .format = DRM_FORMAT_S410,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 1, .vsub = 1, .is_yuv = true},
		{ .format = DRM_FORMAT_S012,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_S212,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 2, .vsub = 1, .is_yuv = true},
		{ .format = DRM_FORMAT_S412,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 1, .vsub = 1, .is_yuv = true},
		{ .format = DRM_FORMAT_S016,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_S216,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 2, .vsub = 1, .is_yuv = true},
		{ .format = DRM_FORMAT_S416,            .depth = 0,  .num_planes = 3,
		  .char_per_block = { 2, 2, 2 }, .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 },
		  .hsub = 1, .vsub = 1, .is_yuv = true},
	};

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); ++i) {
		if (formats[i].format == format)
			return &formats[i];
	}

	return NULL;
}

/**
 * drm_format_info - query information for a given format
 * @format: pixel format (DRM_FORMAT_*)
 *
 * The caller should only pass a supported pixel format to this function.
 * Unsupported pixel formats will generate a warning in the kernel log.
 *
 * Returns:
 * The instance of struct drm_format_info that describes the pixel format, or
 * NULL if the format is unsupported.
 */
/*
 * 中文说明：查询给定 FourCC 像素格式的详细信息。
 * 返回的信息包括：平面数、每像素字节数、块大小、
 * 色度采样率、是否有 alpha 通道、是否为 YUV 等。
 * 如果格式不支持，会触发 WARN 并返回 NULL。
 */
const struct drm_format_info *drm_format_info(u32 format)
{
	const struct drm_format_info *info;

	info = __drm_format_info(format);
	WARN_ON(!info);
	return info;
}
EXPORT_SYMBOL(drm_format_info);

/**
 * drm_get_format_info - query information for a given framebuffer configuration
 * @dev: DRM device
 * @pixel_format: pixel format (DRM_FORMAT_*)
 * @modifier: modifier
 *
 * Returns:
 * The instance of struct drm_format_info that describes the pixel format, or
 * NULL if the format is unsupported.
 */
/*
 * 中文说明：获取给定像素格式和修饰符（modifier）对应的格式信息。
 * 优先使用驱动的 get_format_info 回调（可处理自定义格式和修饰符），
 * 若驱动未实现则回退到标准 drm_format_info() 查询。
 */
const struct drm_format_info *
drm_get_format_info(struct drm_device *dev,
		    u32 pixel_format, u64 modifier)
{
	const struct drm_format_info *info = NULL;

	if (dev->mode_config.funcs->get_format_info)
		info = dev->mode_config.funcs->get_format_info(pixel_format,
							       modifier);

	if (!info)
		info = drm_format_info(pixel_format);

	return info;
}
EXPORT_SYMBOL(drm_get_format_info);

/**
 * drm_format_info_block_width - width in pixels of block.
 * @info: pixel format info
 * @plane: plane index
 *
 * Returns:
 * The width in pixels of a block, depending on the plane index.
 */
/*
 * 中文说明：获取指定平面的块宽度（像素单位）。
 * 对于非块格式返回 1。块格式（如压缩纹理）使用
 * block_w/block_h 描述一个压缩块覆盖的像素区域。
 */
unsigned int drm_format_info_block_width(const struct drm_format_info *info,
					 int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	if (!info->block_w[plane])
		return 1;
	return info->block_w[plane];
}
EXPORT_SYMBOL(drm_format_info_block_width);

/**
 * drm_format_info_block_height - height in pixels of a block
 * @info: pixel format info
 * @plane: plane index
 *
 * Returns:
 * The height in pixels of a block, depending on the plane index.
 */
/*
 * 中文说明：获取指定平面的块高度（像素单位）。
 * 对于非块格式返回 1。与 drm_format_info_block_width() 配合使用。
 */
unsigned int drm_format_info_block_height(const struct drm_format_info *info,
					  int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	if (!info->block_h[plane])
		return 1;
	return info->block_h[plane];
}
EXPORT_SYMBOL(drm_format_info_block_height);

/**
 * drm_format_info_bpp - number of bits per pixel
 * @info: pixel format info
 * @plane: plane index
 *
 * Returns:
 * The actual number of bits per pixel, depending on the plane index.
 */
/*
 * 中文说明：计算指定平面的每像素位数（bpp）。
 * 通过 char_per_block / (block_width * block_height) * 8 计算得出。
 * 对于 YUV 半平面格式，chroma 平面的 bpp 会反映出采样率的影响。
 */
unsigned int drm_format_info_bpp(const struct drm_format_info *info, int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	return info->char_per_block[plane] * 8 /
	       (drm_format_info_block_width(info, plane) *
		drm_format_info_block_height(info, plane));
}
EXPORT_SYMBOL(drm_format_info_bpp);

/**
 * drm_format_info_min_pitch - computes the minimum required pitch in bytes
 * @info: pixel format info
 * @plane: plane index
 * @buffer_width: buffer width in pixels
 *
 * Returns:
 * The minimum required pitch in bytes for a buffer by taking into consideration
 * the pixel format information and the buffer width.
 */
/*
 * 中文说明：计算给定宽度下至少需要的 pitch（行跨度，字节数）。
 * 考虑了块格式的对齐要求：pitch = ceil(width * char_per_block /
 * (block_width * block_height))。用于 framebuffer 创建时的校验。
 */
uint64_t drm_format_info_min_pitch(const struct drm_format_info *info,
				   int plane, unsigned int buffer_width)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	return DIV_ROUND_UP_ULL((u64)buffer_width * info->char_per_block[plane],
			    drm_format_info_block_width(info, plane) *
			    drm_format_info_block_height(info, plane));
}
EXPORT_SYMBOL(drm_format_info_min_pitch);
