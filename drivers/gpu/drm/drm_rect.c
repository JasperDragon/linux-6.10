/*
 * Copyright (C) 2011-2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * DRM 矩形数学辅助 - 中文注释补充
 *
 * 本文件提供 DRM 子系统使用的矩形运算工具函数。矩形（drm_rect）
 * 在显示合成中广泛用于描述源区域（framebuffer 内的裁剪区域）
 * 和目标区域（CRTC 上的显示区域）。
 *
 * 主要功能：
 *   1. drm_rect_intersect：计算两个矩形的交集
 *   2. drm_rect_clip_scaled：按比例缩放裁剪（保持源和目标之间的缩放因子）
 *   3. drm_rect_calc_hscale/vscale：计算水平和垂直缩放因子
 *   4. drm_rect_rotate/rotate_inv：矩形坐标旋转和逆旋转
 *   5. drm_rect_debug_print：矩形调试输出
 *
 * 坐标使用整数表示，坐标区间为 [x1, x2) 左闭右开。
 * 源矩形坐标使用 16.16 定点格式以支持亚像素精度。
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>

#include <drm/drm_mode.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

/**
 * drm_rect_intersect - intersect two rectangles
 * @r1: first rectangle
 * @r2: second rectangle
 *
 * Calculate the intersection of rectangles @r1 and @r2.
 * @r1 will be overwritten with the intersection.
 *
 * RETURNS:
 * %true if rectangle @r1 is still visible after the operation,
 * %false otherwise.
 */
/*
 * 中文说明：计算两个矩形的交集，结果覆盖写入 r1。
 * 如果交集为空（不可见），返回 false。
 * 坐标区间为 [x1, x2) 左闭右开。
 */
bool drm_rect_intersect(struct drm_rect *r1, const struct drm_rect *r2)
{
	r1->x1 = max(r1->x1, r2->x1);
	r1->y1 = max(r1->y1, r2->y1);
	r1->x2 = min(r1->x2, r2->x2);
	r1->y2 = min(r1->y2, r2->y2);

	return drm_rect_visible(r1);
}
EXPORT_SYMBOL(drm_rect_intersect);

static u32 clip_scaled(int src, int dst, int *clip)
{
	u64 tmp;

	if (dst == 0)
		return 0;

	/* Only clip what we have. Keeps the result bounded. */
	*clip = min(*clip, dst);

	tmp = mul_u32_u32(src, dst - *clip);

	/*
	 * Round toward 1.0 when clipping so that we don't accidentally
	 * change upscaling to downscaling or vice versa.
	 */
	if (src < (dst << 16))
		return DIV_ROUND_UP_ULL(tmp, dst);
	else
		return DIV_ROUND_DOWN_ULL(tmp, dst);
}

/**
 * drm_rect_clip_scaled - perform a scaled clip operation
 * @src: source window rectangle
 * @dst: destination window rectangle
 * @clip: clip rectangle
 *
 * Clip rectangle @dst by rectangle @clip. Clip rectangle @src by
 * the corresponding amounts, retaining the vertical and horizontal scaling
 * factors from @src to @dst.
 *
 * RETURNS:
 * %true if rectangle @dst is still visible after being clipped,
 * %false otherwise.
 */
/*
 * 中文说明：按比例缩放裁剪。当目标矩形（dst）被裁剪时，
 * 源矩形（src）按相同的缩放比例进行调整，保证缩放因子不变。
 * 用于在保证缩放比例的前提下进行显示区域裁剪。
 * 裁剪方向为向 1.0 方向舍入，避免改变缩放性质（放大/缩小）。
 */
bool drm_rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst,
			  const struct drm_rect *clip)
{
	int diff;

	diff = clip->x1 - dst->x1;
	if (diff > 0) {
		u32 new_src_w = clip_scaled(drm_rect_width(src),
					    drm_rect_width(dst), &diff);

		src->x1 = src->x2 - new_src_w;
		dst->x1 += diff;
	}
	diff = clip->y1 - dst->y1;
	if (diff > 0) {
		u32 new_src_h = clip_scaled(drm_rect_height(src),
					    drm_rect_height(dst), &diff);

		src->y1 = src->y2 - new_src_h;
		dst->y1 += diff;
	}
	diff = dst->x2 - clip->x2;
	if (diff > 0) {
		u32 new_src_w = clip_scaled(drm_rect_width(src),
					    drm_rect_width(dst), &diff);

		src->x2 = src->x1 + new_src_w;
		dst->x2 -= diff;
	}
	diff = dst->y2 - clip->y2;
	if (diff > 0) {
		u32 new_src_h = clip_scaled(drm_rect_height(src),
					    drm_rect_height(dst), &diff);

		src->y2 = src->y1 + new_src_h;
		dst->y2 -= diff;
	}

	return drm_rect_visible(dst);
}
EXPORT_SYMBOL(drm_rect_clip_scaled);

static int drm_calc_scale(int src, int dst)
{
	int scale = 0;

	if (WARN_ON(src < 0 || dst < 0))
		return -EINVAL;

	if (dst == 0)
		return 0;

	if (src > (dst << 16))
		return DIV_ROUND_UP(src, dst);
	else
		scale = src / dst;

	return scale;
}

/**
 * drm_rect_calc_hscale - calculate the horizontal scaling factor
 * @src: source window rectangle
 * @dst: destination window rectangle
 * @min_hscale: minimum allowed horizontal scaling factor
 * @max_hscale: maximum allowed horizontal scaling factor
 *
 * Calculate the horizontal scaling factor as
 * (@src width) / (@dst width).
 *
 * If the scale is below 1 << 16, round down. If the scale is above
 * 1 << 16, round up. This will calculate the scale with the most
 * pessimistic limit calculation.
 *
 * RETURNS:
 * The horizontal scaling factor, or errno of out of limits.
 */
/*
 * 中文说明：计算水平缩放因子 = src_width / dst_width。
 * 当缩小（src > dst）时向上取整，放大时向下取整，
 * 以使用最悲观的限制检查。结果在 [min_hscale, max_hscale] 之外返回 -ERANGE。
 */
int drm_rect_calc_hscale(const struct drm_rect *src,
			 const struct drm_rect *dst,
			 int min_hscale, int max_hscale)
{
	int src_w = drm_rect_width(src);
	int dst_w = drm_rect_width(dst);
	int hscale = drm_calc_scale(src_w, dst_w);

	if (hscale < 0 || dst_w == 0)
		return hscale;

	if (hscale < min_hscale || hscale > max_hscale)
		return -ERANGE;

	return hscale;
}
EXPORT_SYMBOL(drm_rect_calc_hscale);

/**
 * drm_rect_calc_vscale - calculate the vertical scaling factor
 * @src: source window rectangle
 * @dst: destination window rectangle
 * @min_vscale: minimum allowed vertical scaling factor
 * @max_vscale: maximum allowed vertical scaling factor
 *
 * Calculate the vertical scaling factor as
 * (@src height) / (@dst height).
 *
 * If the scale is below 1 << 16, round down. If the scale is above
 * 1 << 16, round up. This will calculate the scale with the most
 * pessimistic limit calculation.
 *
 * RETURNS:
 * The vertical scaling factor, or errno of out of limits.
 */
/*
 * 中文说明：计算垂直缩放因子 = src_height / dst_height。
 * 与 drm_rect_calc_hscale() 逻辑相同，计算垂直方向的缩放。
 */
int drm_rect_calc_vscale(const struct drm_rect *src,
			 const struct drm_rect *dst,
			 int min_vscale, int max_vscale)
{
	int src_h = drm_rect_height(src);
	int dst_h = drm_rect_height(dst);
	int vscale = drm_calc_scale(src_h, dst_h);

	if (vscale < 0 || dst_h == 0)
		return vscale;

	if (vscale < min_vscale || vscale > max_vscale)
		return -ERANGE;

	return vscale;
}
EXPORT_SYMBOL(drm_rect_calc_vscale);

/**
 * drm_rect_debug_print - print the rectangle information
 * @prefix: prefix string
 * @r: rectangle to print
 * @fixed_point: rectangle is in 16.16 fixed point format
 */
/*
 * 中文说明：调试打印矩形信息。支持普通整数格式
 * 和 16.16 定点格式两种输出方式，方便调试平面源矩形
 * （亚像素精度）和目标矩形。
 */
void drm_rect_debug_print(const char *prefix, const struct drm_rect *r, bool fixed_point)
{
	if (fixed_point)
		DRM_DEBUG_KMS("%s" DRM_RECT_FP_FMT "\n", prefix, DRM_RECT_FP_ARG(r));
	else
		DRM_DEBUG_KMS("%s" DRM_RECT_FMT "\n", prefix, DRM_RECT_ARG(r));
}
EXPORT_SYMBOL(drm_rect_debug_print);

/**
 * drm_rect_rotate - Rotate the rectangle
 * @r: rectangle to be rotated
 * @width: Width of the coordinate space
 * @height: Height of the coordinate space
 * @rotation: Transformation to be applied
 *
 * Apply @rotation to the coordinates of rectangle @r.
 *
 * @width and @height combined with @rotation define
 * the location of the new origin.
 *
 * @width correcsponds to the horizontal and @height
 * to the vertical axis of the untransformed coordinate
 * space.
 */
/*
 * 中文说明：对矩形应用旋转/镜像变换。先进行镜像（X/Y 轴反射），
 * 再进行旋转（0/90/180/270 度）。变换后的坐标原点位置由
 * width、height 和 rotation 共同决定。
 */
void drm_rect_rotate(struct drm_rect *r,
		     int width, int height,
		     unsigned int rotation)
{
	struct drm_rect tmp;

	if (rotation & (DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)) {
		tmp = *r;

		if (rotation & DRM_MODE_REFLECT_X) {
			r->x1 = width - tmp.x2;
			r->x2 = width - tmp.x1;
		}

		if (rotation & DRM_MODE_REFLECT_Y) {
			r->y1 = height - tmp.y2;
			r->y2 = height - tmp.y1;
		}
	}

	switch (rotation & DRM_MODE_ROTATE_MASK) {
	case DRM_MODE_ROTATE_0:
		break;
	case DRM_MODE_ROTATE_90:
		tmp = *r;
		r->x1 = tmp.y1;
		r->x2 = tmp.y2;
		r->y1 = width - tmp.x2;
		r->y2 = width - tmp.x1;
		break;
	case DRM_MODE_ROTATE_180:
		tmp = *r;
		r->x1 = width - tmp.x2;
		r->x2 = width - tmp.x1;
		r->y1 = height - tmp.y2;
		r->y2 = height - tmp.y1;
		break;
	case DRM_MODE_ROTATE_270:
		tmp = *r;
		r->x1 = height - tmp.y2;
		r->x2 = height - tmp.y1;
		r->y1 = tmp.x1;
		r->y2 = tmp.x2;
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(drm_rect_rotate);

/**
 * drm_rect_rotate_inv - Inverse rotate the rectangle
 * @r: rectangle to be rotated
 * @width: Width of the coordinate space
 * @height: Height of the coordinate space
 * @rotation: Transformation whose inverse is to be applied
 *
 * Apply the inverse of @rotation to the coordinates
 * of rectangle @r.
 *
 * @width and @height combined with @rotation define
 * the location of the new origin.
 *
 * @width correcsponds to the horizontal and @height
 * to the vertical axis of the original untransformed
 * coordinate space, so that you never have to flip
 * them when doing a rotatation and its inverse.
 * That is, if you do ::
 *
 *     drm_rect_rotate(&r, width, height, rotation);
 *     drm_rect_rotate_inv(&r, width, height, rotation);
 *
 * you will always get back the original rectangle.
 */
/*
 * 中文说明：对矩形应用旋转/镜像的逆变换。先进行逆旋转，
 * 再进行逆镜像。与 drm_rect_rotate() 配合使用可实现无损变换，
 * 即 rotate(rotate_inv(r)) == r。width 和 height 始终对应原始
 * 未变换坐标空间。
 */
void drm_rect_rotate_inv(struct drm_rect *r,
			 int width, int height,
			 unsigned int rotation)
{
	struct drm_rect tmp;

	switch (rotation & DRM_MODE_ROTATE_MASK) {
	case DRM_MODE_ROTATE_0:
		break;
	case DRM_MODE_ROTATE_90:
		tmp = *r;
		r->x1 = width - tmp.y2;
		r->x2 = width - tmp.y1;
		r->y1 = tmp.x1;
		r->y2 = tmp.x2;
		break;
	case DRM_MODE_ROTATE_180:
		tmp = *r;
		r->x1 = width - tmp.x2;
		r->x2 = width - tmp.x1;
		r->y1 = height - tmp.y2;
		r->y2 = height - tmp.y1;
		break;
	case DRM_MODE_ROTATE_270:
		tmp = *r;
		r->x1 = tmp.y1;
		r->x2 = tmp.y2;
		r->y1 = height - tmp.x2;
		r->y2 = height - tmp.x1;
		break;
	default:
		break;
	}

	if (rotation & (DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)) {
		tmp = *r;

		if (rotation & DRM_MODE_REFLECT_X) {
			r->x1 = width - tmp.x2;
			r->x2 = width - tmp.x1;
		}

		if (rotation & DRM_MODE_REFLECT_Y) {
			r->y1 = height - tmp.y2;
			r->y2 = height - tmp.y1;
		}
	}
}
EXPORT_SYMBOL(drm_rect_rotate_inv);
