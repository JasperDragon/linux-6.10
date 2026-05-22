// SPDX-License-Identifier: GPL-2.0 OR MIT
/**************************************************************************
 *
 * Copyright (c) 2018 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Deepak Rawat <drawat@vmware.com>
 * Rob Clark <robdclark@gmail.com>
 *
 **************************************************************************/

/*
 * DRM 脏区域（Damage）辅助函数
 *
 * 本文件实现了脏区域跟踪和处理的辅助函数。脏区域是指帧缓冲区中自上次刷新以来
 * 内容发生变化的区域。通过跟踪脏区域，驱动可以只更新屏幕上的变化部分，
 * 而不需要重新传输整个帧缓冲区，从而节省带宽和功耗。
 *
 * 主要功能：
 *   - drm_atomic_helper_check_plane_damage：在 atomic check 阶段验证脏区域
 *   - drm_atomic_helper_dirtyfb：实现 DIRTYFB IOCTL，处理用户空间的脏区域通知
 *   - drm_atomic_helper_damage_iter_init/next：脏区域迭代器，用于遍历损坏区域
 *   - drm_atomic_helper_damage_merged：合并所有脏区域为一个矩形
 *
 * 脏区域信息存储在 &drm_plane_state.fb_damage_clips 属性中，
 * 当该属性为 NULL 时意味着需要进行全平面更新。
 */

#include <linux/export.h>

#include <drm/drm_atomic.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>

static void convert_clip_rect_to_rect(const struct drm_clip_rect *src,
				      struct drm_mode_rect *dest,
				      uint32_t num_clips, uint32_t src_inc)
{
	while (num_clips > 0) {
		dest->x1 = src->x1;
		dest->y1 = src->y1;
		dest->x2 = src->x2;
		dest->y2 = src->y2;
		src += src_inc;
		dest++;
		num_clips--;
	}
}

/**
 * drm_atomic_helper_check_plane_damage - 在 atomic_check 中验证平面脏区域
 * @state: 驱动状态对象
 * @plane_state: 需要验证脏区域的平面状态
 *
 * 此辅助函数确保在进行全模式设置（full modeset）时丢弃平面状态中的脏区域信息。
 * 如果驱动有其他理由需要执行全平面更新而不是处理个别脏区域，
 * 也应当在此处处理。
 *
 * 注意：&drm_plane_state.fb_damage_clips == NULL 表示应当执行全平面更新。
 * 这也确保当没有脏区域信息时，辅助迭代器返回 &drm_plane_state.src 作为脏区域。
 */
void drm_atomic_helper_check_plane_damage(struct drm_atomic_state *state,
					  struct drm_plane_state *plane_state)
{
	struct drm_crtc_state *crtc_state;

	if (plane_state->crtc) {
		crtc_state = drm_atomic_get_new_crtc_state(state,
							   plane_state->crtc);

		if (WARN_ON(!crtc_state))
			return;

		if (drm_atomic_crtc_needs_modeset(crtc_state)) {
			drm_property_blob_put(plane_state->fb_damage_clips);
			plane_state->fb_damage_clips = NULL;
		}
	}
}
EXPORT_SYMBOL(drm_atomic_helper_check_plane_damage);

/**
 * drm_atomic_helper_dirtyfb - 实现 dirtyfb 的辅助函数
 * @fb: DRM 帧缓冲
 * @file_priv: IOCTL 调用的 DRM 文件句柄
 * @flags: 脏帧缓冲区注释标志
 * @color: 注释填充颜色
 * @clips: 脏区域矩形数组
 * @num_clips: clips 数组中的裁剪矩形数量
 *
 * 使用脏区域（damage）接口实现 &drm_framebuffer_funcs.dirty 回调的辅助函数。
 * 如果 num_clips 为 0，则执行全平面更新。这与 DIRTYFB IOCTL 的预期行为一致。
 *
 * 这是一个阻塞实现。当前驱动和用户空间在实现 DIRTYFB IOCTL 时期望如此，
 * 作为对用户空间的速率限制机制，确保渲染不会超前于新数据的上传。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_atomic_helper_dirtyfb(struct drm_framebuffer *fb,
			      struct drm_file *file_priv, unsigned int flags,
			      unsigned int color, struct drm_clip_rect *clips,
			      unsigned int num_clips)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_property_blob *damage = NULL;
	struct drm_mode_rect *rects = NULL;
	struct drm_atomic_state *state;
	struct drm_plane *plane;
	int ret = 0;

	/*
	 * When called from ioctl, we are interruptible, but not when called
	 * internally (ie. defio worker)
	 */
	drm_modeset_acquire_init(&ctx,
		file_priv ? DRM_MODESET_ACQUIRE_INTERRUPTIBLE : 0);

	state = drm_atomic_state_alloc(fb->dev);
	if (!state) {
		ret = -ENOMEM;
		goto out_drop_locks;
	}
	state->acquire_ctx = &ctx;

	if (clips) {
		uint32_t inc = 1;

		if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) {
			inc = 2;
			num_clips /= 2;
		}

		rects = kzalloc_objs(*rects, num_clips);
		if (!rects) {
			ret = -ENOMEM;
			goto out;
		}

		convert_clip_rect_to_rect(clips, rects, num_clips, inc);
		damage = drm_property_create_blob(fb->dev,
						  num_clips * sizeof(*rects),
						  rects);
		if (IS_ERR(damage)) {
			ret = PTR_ERR(damage);
			damage = NULL;
			goto out;
		}
	}

retry:
	drm_for_each_plane(plane, fb->dev) {
		struct drm_plane_state *plane_state;

		ret = drm_modeset_lock(&plane->mutex, state->acquire_ctx);
		if (ret)
			goto out;

		if (plane->state->fb != fb) {
			drm_modeset_unlock(&plane->mutex);
			continue;
		}

		plane_state = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			goto out;
		}

		drm_property_replace_blob(&plane_state->fb_damage_clips,
					  damage);
	}

	ret = drm_atomic_commit(state);

out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}

	drm_property_blob_put(damage);
	kfree(rects);
	drm_atomic_state_put(state);

out_drop_locks:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;

}
EXPORT_SYMBOL(drm_atomic_helper_dirtyfb);

/**
 * drm_atomic_helper_damage_iter_init - 初始化脏区域迭代器
 * @iter: 要初始化的迭代器
 * @old_state: 旧的平面状态，用于验证
 * @state: 要遍历脏区域的平面状态
 *
 * 初始化一个迭代器，该迭代器将平面脏区域 &drm_plane_state.fb_damage_clips
 * 裁剪到平面源区域 &drm_plane_state.src 内。
 *
 * 在以下情况下返回完整的平面源区域作为脏区域：
 *   - 用户空间未提供脏区域信息
 *   - 驱动丢弃了脏区域信息（希望执行全平面更新）
 *   - 平面源区域发生变化
 *
 * 如果平面不可见或不需要更新，第一次调用 iter_next 将返回 false。
 * 注意：此辅助函数使用裁剪后的 &drm_plane_state.src，因此调用者应确保
 * 之前已经调用了 drm_atomic_helper_check_plane_state()。
 */
void
drm_atomic_helper_damage_iter_init(struct drm_atomic_helper_damage_iter *iter,
				   const struct drm_plane_state *old_state,
				   const struct drm_plane_state *state)
{
	struct drm_rect src;
	memset(iter, 0, sizeof(*iter));

	if (!state || !state->crtc || !state->fb || !state->visible)
		return;

	iter->clips = (struct drm_rect *)drm_plane_get_damage_clips(state);
	iter->num_clips = drm_plane_get_damage_clips_count(state);

	/* Round down for x1/y1 and round up for x2/y2 to catch all pixels */
	src = drm_plane_state_src(state);

	iter->plane_src.x1 = src.x1 >> 16;
	iter->plane_src.y1 = src.y1 >> 16;
	iter->plane_src.x2 = (src.x2 >> 16) + !!(src.x2 & 0xFFFF);
	iter->plane_src.y2 = (src.y2 >> 16) + !!(src.y2 & 0xFFFF);

	if (!iter->clips || state->ignore_damage_clips ||
	    !drm_rect_equals(&state->src, &old_state->src)) {
		iter->clips = NULL;
		iter->num_clips = 0;
		iter->full_update = true;
	}
}
EXPORT_SYMBOL(drm_atomic_helper_damage_iter_init);

/**
 * drm_atomic_helper_damage_iter_next - 推进脏区域迭代器
 * @iter: 要推进的迭代器
 * @rect: 返回在帧缓冲坐标中、裁剪到平面源区域内的矩形
 *
 * 由于平面源区域使用 16.16 定点数表示，而脏区域是整数，此迭代器对与平面
 * 源区域相交的裁剪矩形进行舍入处理。x1/y1 向下取整，x2/y2 向上取整。
 * 对于返回完整平面源区域作为脏区域的情况也采用相同的舍入处理。
 * 此迭代器会自动跳过位于平面源区域之外的脏区域。
 *
 * 返回值：
 * 如果输出有效返回 true，如果遍历结束返回 false。
 *
 * 如果第一次调用就返回 false，则表示无需更新该平面。
 */
bool
drm_atomic_helper_damage_iter_next(struct drm_atomic_helper_damage_iter *iter,
				   struct drm_rect *rect)
{
	bool ret = false;

	if (iter->full_update) {
		*rect = iter->plane_src;
		iter->full_update = false;
		return true;
	}

	while (iter->curr_clip < iter->num_clips) {
		*rect = iter->clips[iter->curr_clip];
		iter->curr_clip++;

		if (drm_rect_intersect(rect, &iter->plane_src)) {
			ret = true;
			break;
		}
	}

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_damage_iter_next);

/**
 * drm_atomic_helper_damage_merged - 合并平面脏区域
 * @old_state: 旧的平面状态，用于验证
 * @state: 要遍历脏区域的平面状态
 * @rect: 返回合并后的脏区域矩形
 *
 * 将所有有效的平面脏区域合并为一个矩形并返回。这在驱动只需要知道
 * 脏区域的整体边界而不需要逐区域处理时非常有用。
 *
 * 详细说明请参见：drm_atomic_helper_damage_iter_init() 和
 * drm_atomic_helper_damage_iter_next()。
 *
 * 返回值：
 * 如果有有效的平面脏区域返回 true，否则返回 false。
 */
bool drm_atomic_helper_damage_merged(const struct drm_plane_state *old_state,
				     const struct drm_plane_state *state,
				     struct drm_rect *rect)
{
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;
	bool valid = false;

	rect->x1 = INT_MAX;
	rect->y1 = INT_MAX;
	rect->x2 = 0;
	rect->y2 = 0;

	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		rect->x1 = min(rect->x1, clip.x1);
		rect->y1 = min(rect->y1, clip.y1);
		rect->x2 = max(rect->x2, clip.x2);
		rect->y2 = max(rect->y2, clip.y2);
		valid = true;
	}

	return valid;
}
EXPORT_SYMBOL(drm_atomic_helper_damage_merged);
