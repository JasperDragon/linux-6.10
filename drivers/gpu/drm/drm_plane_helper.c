/*
 * Copyright (C) 2014 Intel Corporation
 *
 * DRM universal plane helper functions
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
 * DRM 通用平面（plane）辅助函数
 *
 * 本文件实现了主平面（primary plane）的辅助函数，用于在传统 CRTC 模式设置接口
 * 之上支持主平面操作。由于传统的 &drm_mode_config_funcs.set_config 接口将
 * 主平面与 CRTC 状态绑定在一起，用户空间无法直接禁用主平面。
 *
 * 这些辅助函数为过渡性支持而存在。新的驱动应当实现完整的原子化平面支持，
 * 并使用 atomic helpers。
 *
 * 主要内容：
 *   - drm_plane_helper_update_primary：更新主平面的辅助函数
 *   - drm_plane_helper_disable_primary：禁用主平面的辅助函数
 *   - drm_plane_helper_destroy：主平面销毁辅助函数
 */

#include <linux/export.h>
#include <linux/list.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#define SUBPIXEL_MASK 0xffff

/**
 * DOC: overview
 *
 * This helper library contains helpers to implement primary plane support on
 * top of the normal CRTC configuration interface.
 * Since the legacy &drm_mode_config_funcs.set_config interface ties the primary
 * plane together with the CRTC state this does not allow userspace to disable
 * the primary plane itself. The default primary plane only expose XRBG8888 and
 * ARGB8888 as valid pixel formats for the attached framebuffer.
 *
 * Drivers are highly recommended to implement proper support for primary
 * planes, and newly merged drivers must not rely upon these transitional
 * helpers.
 *
 * The plane helpers share the function table structures with other helpers,
 * specifically also the atomic helpers. See &struct drm_plane_helper_funcs for
 * the details.
 */

/*
 * Returns the connectors currently associated with a CRTC.  This function
 * should be called twice:  once with a NULL connector list to retrieve
 * the list size, and once with the properly allocated list to be filled in.
 */
static int get_connectors_for_crtc(struct drm_crtc *crtc,
				   struct drm_connector **connector_list,
				   int num_connectors)
{
	struct drm_device *dev = crtc->dev;
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	int count = 0;

	/*
	 * Note: Once we change the plane hooks to more fine-grained locking we
	 * need to grab the connection_mutex here to be able to make these
	 * checks.
	 */
	WARN_ON(!drm_modeset_is_locked(&dev->mode_config.connection_mutex));

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		if (connector->encoder && connector->encoder->crtc == crtc) {
			if (connector_list != NULL && count < num_connectors)
				*(connector_list++) = connector;

			count++;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	return count;
}

static int drm_plane_helper_check_update(struct drm_plane *plane,
					 struct drm_crtc *crtc,
					 struct drm_framebuffer *fb,
					 struct drm_rect *src,
					 struct drm_rect *dst,
					 unsigned int rotation,
					 int min_scale,
					 int max_scale,
					 bool can_position,
					 bool can_update_disabled,
					 bool *visible)
{
	struct drm_plane_state plane_state = {
		.plane = plane,
		.crtc = crtc,
		.fb = fb,
		.src_x = src->x1,
		.src_y = src->y1,
		.src_w = drm_rect_width(src),
		.src_h = drm_rect_height(src),
		.crtc_x = dst->x1,
		.crtc_y = dst->y1,
		.crtc_w = drm_rect_width(dst),
		.crtc_h = drm_rect_height(dst),
		.rotation = rotation,
	};
	struct drm_crtc_state crtc_state = {
		.crtc = crtc,
		.enable = crtc->enabled,
		.mode = crtc->mode,
	};
	int ret;

	ret = drm_atomic_helper_check_plane_state(&plane_state, &crtc_state,
						  min_scale, max_scale,
						  can_position,
						  can_update_disabled);
	if (ret)
		return ret;

	*src = plane_state.src;
	*dst = plane_state.dst;
	*visible = plane_state.visible;

	return 0;
}

/**
 * drm_plane_helper_update_primary - 更新主平面的辅助函数
 * @plane: 要更新的平面
 * @crtc: 平面的新 CRTC
 * @fb: 平面的新帧缓冲
 * @crtc_x: 在 CRTC 中的 X 坐标
 * @crtc_y: 在 CRTC 中的 Y 坐标
 * @crtc_w: 在 CRTC 中的宽度
 * @crtc_h: 在 CRTC 中的高度
 * @src_x: 源图像中的 X 坐标（16.16 固定点数）
 * @src_y: 源图像中的 Y 坐标（16.16 固定点数）
 * @src_w: 源图像中的宽度（16.16 固定点数）
 * @src_h: 源图像中的高度（16.16 固定点数）
 * @ctx: 模式设置锁上下文
 *
 * 验证给定参数并更新主平面。该函数通过查找与 CRTC 关联的连接器并调用
 * set_config 来实现平面更新。它是传统非原子模式设置的过渡性辅助函数。
 *
 * 此函数仅适用于非原子模式设置。新的驱动不应使用此函数，
 * 而应实现完整的原子平面支持。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_plane_helper_update_primary(struct drm_plane *plane, struct drm_crtc *crtc,
				    struct drm_framebuffer *fb,
				    int crtc_x, int crtc_y,
				    unsigned int crtc_w, unsigned int crtc_h,
				    uint32_t src_x, uint32_t src_y,
				    uint32_t src_w, uint32_t src_h,
				    struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_mode_set set = {
		.crtc = crtc,
		.fb = fb,
		.mode = &crtc->mode,
		.x = src_x >> 16,
		.y = src_y >> 16,
	};
	struct drm_rect src = {
		.x1 = src_x,
		.y1 = src_y,
		.x2 = src_x + src_w,
		.y2 = src_y + src_h,
	};
	struct drm_rect dest = {
		.x1 = crtc_x,
		.y1 = crtc_y,
		.x2 = crtc_x + crtc_w,
		.y2 = crtc_y + crtc_h,
	};
	struct drm_device *dev = plane->dev;
	struct drm_connector **connector_list;
	int num_connectors, ret;
	bool visible;

	if (drm_WARN_ON_ONCE(dev, drm_drv_uses_atomic_modeset(dev)))
		return -EINVAL;

	ret = drm_plane_helper_check_update(plane, crtc, fb,
					    &src, &dest,
					    DRM_MODE_ROTATE_0,
					    DRM_PLANE_NO_SCALING,
					    DRM_PLANE_NO_SCALING,
					    false, false, &visible);
	if (ret)
		return ret;

	if (!visible)
		/*
		 * Primary plane isn't visible.  Note that unless a driver
		 * provides their own disable function, this will just
		 * wind up returning -EINVAL to userspace.
		 */
		return plane->funcs->disable_plane(plane, ctx);

	/* Find current connectors for CRTC */
	num_connectors = get_connectors_for_crtc(crtc, NULL, 0);
	BUG_ON(num_connectors == 0);
	connector_list = kzalloc_objs(*connector_list, num_connectors);
	if (!connector_list)
		return -ENOMEM;
	get_connectors_for_crtc(crtc, connector_list, num_connectors);

	set.connectors = connector_list;
	set.num_connectors = num_connectors;

	/*
	 * We call set_config() directly here rather than using
	 * drm_mode_set_config_internal.  We're reprogramming the same
	 * connectors that were already in use, so we shouldn't need the extra
	 * cross-CRTC fb refcounting to accommodate stealing connectors.
	 * drm_mode_setplane() already handles the basic refcounting for the
	 * framebuffers involved in this operation.
	 */
	ret = crtc->funcs->set_config(&set, ctx);

	kfree(connector_list);
	return ret;
}
EXPORT_SYMBOL(drm_plane_helper_update_primary);

/**
 * drm_plane_helper_disable_primary - 禁用主平面的辅助函数
 * @plane: 要禁用的平面
 * @ctx: 模式设置锁上下文
 *
 * 此辅助函数在尝试禁用主平面时始终返回错误（-EINVAL）。
 * 因为在传统模式设置模型中，主平面与 CRTC 紧密绑定，不能独立禁用。
 * 要禁用显示输出，应当禁用整个 CRTC 而非单独禁用它主平面。
 *
 * 此函数仅适用于非原子模式设置。新的驱动不应使用此函数。
 *
 * 返回值：
 * 始终返回 -EINVAL 错误码。
 */
int drm_plane_helper_disable_primary(struct drm_plane *plane,
				     struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *dev = plane->dev;

	drm_WARN_ON_ONCE(dev, drm_drv_uses_atomic_modeset(dev));

	return -EINVAL;
}
EXPORT_SYMBOL(drm_plane_helper_disable_primary);

/**
 * drm_plane_helper_destroy() - 销毁主平面的辅助函数
 * @plane: 要销毁的平面
 *
 * 提供主平面的默认销毁处理函数。在 CRTC 销毁过程中被调用。
 * 该函数会清理平面资源（通过 drm_plane_cleanup）并释放平面结构体内存。
 *
 * 注意：此函数使用 kfree 释放平面结构体，因此仅适用于通过 kzalloc/kzalloc_obj
 * 分配的平面对象。如果驱动使用其他分配方式，应提供自定义的销毁函数。
 */
void drm_plane_helper_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
	kfree(plane);
}
EXPORT_SYMBOL(drm_plane_helper_destroy);
