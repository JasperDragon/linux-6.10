// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 Noralf Trønnes
 */

/*
 * DRM 简单显示管线（Simple Display Pipeline）辅助函数
 *
 * 本文件为简单显示硬件提供了一个精简的 KMS 驱动框架。对于不需要复杂
 * 多 CRTC、多平面或高级特性的简单显示控制器（如嵌入式系统的显示控制器、
 * 简单的 USB 显示适配器等），可以使用此框架快速实现 KMS 驱动。
 *
 * 该框架通过 struct drm_simple_display_pipe 将 CRTC、主平面（primary plane）
 * 和编码器（encoder）组合成一个单一的数据结构，简化了管线的初始化和操作。
 *
 * 主要功能：
 *   - drm_simple_display_pipe_init：初始化简单显示管线
 *   - drm_simple_encoder_init：初始化简单编码器
 *   - drm_simple_display_pipe_attach_bridge：附加桥接器
 *
 * 驱动通过实现 &drm_simple_display_pipe_funcs 回调来提供硬件特定的操作，
 * 如 enable/disable/update/check 等。
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

static const struct drm_encoder_funcs drm_simple_encoder_funcs_cleanup = {
	.destroy = drm_encoder_cleanup,
};

/**
 * drm_simple_encoder_init - 初始化简单编码器
 * @dev: DRM 设备
 * @encoder: 要初始化的编码器
 * @encoder_type: 编码器类型（如 DRM_MODE_ENCODER_NONE）
 *
 * 初始化一个简化版的编码器，使用默认的清理函数 drm_encoder_cleanup。
 * 这是 drm_simple_display_pipe 框架的一部分，适用于简单显示管线。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_simple_encoder_init(struct drm_device *dev,
			    struct drm_encoder *encoder,
			    int encoder_type)
{
	return drm_encoder_init(dev, encoder,
				&drm_simple_encoder_funcs_cleanup,
				encoder_type, NULL);
}
EXPORT_SYMBOL(drm_simple_encoder_init);

void *__drmm_simple_encoder_alloc(struct drm_device *dev, size_t size,
				  size_t offset, int encoder_type)
{
	return __drmm_encoder_alloc(dev, size, offset, NULL, encoder_type,
				    NULL);
}
EXPORT_SYMBOL(__drmm_simple_encoder_alloc);

static enum drm_mode_status
drm_simple_kms_crtc_mode_valid(struct drm_crtc *crtc,
			       const struct drm_display_mode *mode)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->mode_valid)
		/* Anything goes */
		return MODE_OK;

	return pipe->funcs->mode_valid(pipe, mode);
}

static int drm_simple_kms_crtc_check(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (!crtc_state->enable)
		goto out;

	ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
	if (ret)
		return ret;

out:
	return drm_atomic_add_affected_planes(state, crtc);
}

static void drm_simple_kms_crtc_enable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct drm_plane *plane;
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->enable)
		return;

	plane = &pipe->plane;
	pipe->funcs->enable(pipe, crtc->state, plane->state);
}

static void drm_simple_kms_crtc_disable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->disable)
		return;

	pipe->funcs->disable(pipe);
}

static const struct drm_crtc_helper_funcs drm_simple_kms_crtc_helper_funcs = {
	.mode_valid = drm_simple_kms_crtc_mode_valid,
	.atomic_check = drm_simple_kms_crtc_check,
	.atomic_enable = drm_simple_kms_crtc_enable,
	.atomic_disable = drm_simple_kms_crtc_disable,
};

static void drm_simple_kms_crtc_reset(struct drm_crtc *crtc)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->reset_crtc)
		return drm_atomic_helper_crtc_reset(crtc);

	return pipe->funcs->reset_crtc(pipe);
}

static struct drm_crtc_state *drm_simple_kms_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->duplicate_crtc_state)
		return drm_atomic_helper_crtc_duplicate_state(crtc);

	return pipe->funcs->duplicate_crtc_state(pipe);
}

static void drm_simple_kms_crtc_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->destroy_crtc_state)
		drm_atomic_helper_crtc_destroy_state(crtc, state);
	else
		pipe->funcs->destroy_crtc_state(pipe, state);
}

static int drm_simple_kms_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->enable_vblank)
		return 0;

	return pipe->funcs->enable_vblank(pipe);
}

static void drm_simple_kms_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(crtc, struct drm_simple_display_pipe, crtc);
	if (!pipe->funcs || !pipe->funcs->disable_vblank)
		return;

	pipe->funcs->disable_vblank(pipe);
}

static const struct drm_crtc_funcs drm_simple_kms_crtc_funcs = {
	.reset = drm_simple_kms_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_simple_kms_crtc_duplicate_state,
	.atomic_destroy_state = drm_simple_kms_crtc_destroy_state,
	.enable_vblank = drm_simple_kms_crtc_enable_vblank,
	.disable_vblank = drm_simple_kms_crtc_disable_vblank,
};

static int drm_simple_kms_plane_atomic_check(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state,
									     plane);
	struct drm_simple_display_pipe *pipe;
	struct drm_crtc_state *crtc_state;
	int ret;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	crtc_state = drm_atomic_get_new_crtc_state(state,
						   &pipe->crtc);

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret)
		return ret;

	if (!plane_state->visible)
		return 0;

	if (!pipe->funcs || !pipe->funcs->check)
		return 0;

	return pipe->funcs->check(pipe, plane_state, crtc_state);
}

static void drm_simple_kms_plane_atomic_update(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_plane_state *old_pstate = drm_atomic_get_old_plane_state(state,
									    plane);
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->update)
		return;

	pipe->funcs->update(pipe, old_pstate);
}

static int drm_simple_kms_plane_prepare_fb(struct drm_plane *plane,
					   struct drm_plane_state *state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->prepare_fb) {
		if (WARN_ON_ONCE(!drm_core_check_feature(plane->dev, DRIVER_GEM)))
			return 0;

		WARN_ON_ONCE(pipe->funcs && pipe->funcs->cleanup_fb);

		return drm_gem_plane_helper_prepare_fb(plane, state);
	}

	return pipe->funcs->prepare_fb(pipe, state);
}

static void drm_simple_kms_plane_cleanup_fb(struct drm_plane *plane,
					    struct drm_plane_state *state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->cleanup_fb)
		return;

	pipe->funcs->cleanup_fb(pipe, state);
}

static int drm_simple_kms_plane_begin_fb_access(struct drm_plane *plane,
						struct drm_plane_state *new_plane_state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->begin_fb_access)
		return 0;

	return pipe->funcs->begin_fb_access(pipe, new_plane_state);
}

static void drm_simple_kms_plane_end_fb_access(struct drm_plane *plane,
					       struct drm_plane_state *new_plane_state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->end_fb_access)
		return;

	pipe->funcs->end_fb_access(pipe, new_plane_state);
}

static bool drm_simple_kms_format_mod_supported(struct drm_plane *plane,
						uint32_t format,
						uint64_t modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR;
}

static const struct drm_plane_helper_funcs drm_simple_kms_plane_helper_funcs = {
	.prepare_fb = drm_simple_kms_plane_prepare_fb,
	.cleanup_fb = drm_simple_kms_plane_cleanup_fb,
	.begin_fb_access = drm_simple_kms_plane_begin_fb_access,
	.end_fb_access = drm_simple_kms_plane_end_fb_access,
	.atomic_check = drm_simple_kms_plane_atomic_check,
	.atomic_update = drm_simple_kms_plane_atomic_update,
};

static void drm_simple_kms_plane_reset(struct drm_plane *plane)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->reset_plane)
		return drm_atomic_helper_plane_reset(plane);

	return pipe->funcs->reset_plane(pipe);
}

static struct drm_plane_state *drm_simple_kms_plane_duplicate_state(struct drm_plane *plane)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->duplicate_plane_state)
		return drm_atomic_helper_plane_duplicate_state(plane);

	return pipe->funcs->duplicate_plane_state(pipe);
}

static void drm_simple_kms_plane_destroy_state(struct drm_plane *plane,
					       struct drm_plane_state *state)
{
	struct drm_simple_display_pipe *pipe;

	pipe = container_of(plane, struct drm_simple_display_pipe, plane);
	if (!pipe->funcs || !pipe->funcs->destroy_plane_state)
		drm_atomic_helper_plane_destroy_state(plane, state);
	else
		pipe->funcs->destroy_plane_state(pipe, state);
}

static const struct drm_plane_funcs drm_simple_kms_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_simple_kms_plane_reset,
	.atomic_duplicate_state	= drm_simple_kms_plane_duplicate_state,
	.atomic_destroy_state	= drm_simple_kms_plane_destroy_state,
	.format_mod_supported   = drm_simple_kms_format_mod_supported,
};

/**
 * drm_simple_display_pipe_attach_bridge - 为简单显示管线附加桥接器
 * @pipe: 简单显示管线
 * @bridge: 要附加的桥接器
 *
 * 将一个 DRM 桥接器（bridge）附加到简单显示管线的编码器上。
 * 桥接器用于连接编码器与连接器之间的外部显示芯片（如 HDMI 转换器、
 * LVDS 发送器等）。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_simple_display_pipe_attach_bridge(struct drm_simple_display_pipe *pipe,
					  struct drm_bridge *bridge)
{
	return drm_bridge_attach(&pipe->encoder, bridge, NULL, 0);
}
EXPORT_SYMBOL(drm_simple_display_pipe_attach_bridge);

/**
 * drm_simple_display_pipe_init - 初始化简单显示管线
 * @dev: DRM 设备
 * @pipe: 简单显示管线
 * @funcs: 显示管线操作函数（可选）
 * @formats: 支持的像素格式列表
 * @format_count: 格式数量
 * @format_modifiers: 格式修饰符列表（可选）
 * @connector: 连接器（可选，可以为 NULL）
 *
 * 初始化一个包含 CRTC、主平面和编码器的简单显示管线。此函数会：
 *   1. 初始化主平面（primary plane）并注册到 DRM 核心
 *   2. 初始化 CRTC 并注册到 DRM 核心
 *   3. 初始化编码器（encoder）
 *   4. 如果提供了连接器，将编码器连接到连接器
 *
 * 适用于嵌入式系统和简单显示控制器的驱动开发。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_simple_display_pipe_init(struct drm_device *dev,
				 struct drm_simple_display_pipe *pipe,
				 const struct drm_simple_display_pipe_funcs *funcs,
				 const uint32_t *formats, unsigned int format_count,
				 const uint64_t *format_modifiers,
				 struct drm_connector *connector)
{
	struct drm_encoder *encoder = &pipe->encoder;
	struct drm_plane *plane = &pipe->plane;
	struct drm_crtc *crtc = &pipe->crtc;
	int ret;

	pipe->connector = connector;
	pipe->funcs = funcs;

	drm_plane_helper_add(plane, &drm_simple_kms_plane_helper_funcs);
	ret = drm_universal_plane_init(dev, plane, 0,
				       &drm_simple_kms_plane_funcs,
				       formats, format_count,
				       format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_crtc_helper_add(crtc, &drm_simple_kms_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(dev, crtc, plane, NULL,
					&drm_simple_kms_crtc_funcs, NULL);
	if (ret)
		return ret;

	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drm_simple_encoder_init(dev, encoder, DRM_MODE_ENCODER_NONE);
	if (ret || !connector)
		return ret;

	return drm_connector_attach_encoder(connector, encoder);
}
EXPORT_SYMBOL(drm_simple_display_pipe_init);

MODULE_DESCRIPTION("Helpers for drivers for simple display hardware");
MODULE_LICENSE("GPL");
