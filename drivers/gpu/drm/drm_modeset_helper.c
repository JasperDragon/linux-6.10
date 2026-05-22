/*
 * Copyright (c) 2016 Intel Corporation
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
 * DRM 模式设置辅助函数
 *
 * 本文件提供 DRM 模式设置（modeset）的一些通用辅助函数，这些函数
 * 不适合放在 DRM 模式设置辅助库的其他文件中。
 *
 * 主要功能：
 *   1. drm_helper_move_panel_connectors_to_head() - 将内置面板连接器
 *      （eDP/LVDS/DSI）移到连接器列表的前面，确保用户空间将内置面板
 *      识别为主显示器（用于显示登录屏幕等）。
 *   2. drm_helper_mode_fill_fb_struct() - 填充 framebuffer 的元数据
 *      字段，可在驱动的 fb_create 回调中使用。
 *   3. drm_crtc_init() - 向后兼容的 CRTC 初始化函数，自动创建一个
 *      默认的主平面。建议新驱动使用原子接口的初始化方式。
 *   4. drm_mode_config_helper_suspend/resume() - 模式设置的挂起/恢复
 *      辅助函数，统一处理输出轮询、fbdev 和原子状态。
 */

#include <linux/export.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_client_event.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

/**
 * DOC: aux kms helpers
 *
 * This helper library contains various one-off functions which don't really fit
 * anywhere else in the DRM modeset helper library.
 */

/**
 * drm_helper_move_panel_connectors_to_head() - 将面板连接器移动到连接器列表前端
 * @dev: 要操作的 DRM 设备
 *
 * 某些用户空间程序假定第一个已连接的连接器是主显示器，并会在其上
 * 显示登录屏幕等内容。对于笔记本电脑，这应该是内置面板。使用此函数
 * 可以将所有（eDP/LVDS/DSI）面板连接器排序到连接器列表的前面，
 * 而不必费心按照正确的顺序初始化它们。
 *
 * 适用场景：
 *   在设备初始化的最后阶段调用此函数，确保内置面板始终位于
 *   连接器列表的前列，从而被用户空间识别为主显示器。
 */
void drm_helper_move_panel_connectors_to_head(struct drm_device *dev)
void drm_helper_move_panel_connectors_to_head(struct drm_device *dev)
{
	struct drm_connector *connector, *tmp;
	struct list_head panel_list;

	INIT_LIST_HEAD(&panel_list);

	spin_lock_irq(&dev->mode_config.connector_list_lock);
	list_for_each_entry_safe(connector, tmp,
				 &dev->mode_config.connector_list, head) {
		if (connector->connector_type == DRM_MODE_CONNECTOR_LVDS ||
		    connector->connector_type == DRM_MODE_CONNECTOR_eDP ||
		    connector->connector_type == DRM_MODE_CONNECTOR_DSI)
			list_move_tail(&connector->head, &panel_list);
	}

	list_splice(&panel_list, &dev->mode_config.connector_list);
	spin_unlock_irq(&dev->mode_config.connector_list_lock);
}
EXPORT_SYMBOL(drm_helper_move_panel_connectors_to_head);

/**
 * drm_helper_mode_fill_fb_struct - 填充 framebuffer 元数据
 * @dev: DRM 设备
 * @fb: 要填充的 drm_framebuffer 对象
 * @info: 像素格式信息
 * @mode_cmd: 来自用户空间 fb 创建请求的元数据
 *
 * 该辅助函数可以在驱动的 fb_create 回调中使用，用于预填充 framebuffer
 * 的元数据字段，包括设备指针、格式信息、分辨率、每平面行宽和偏移、
 * 修饰符和标志等。
 */
 * @dev: DRM device
 * @fb: drm_framebuffer object to fill out
 * @info: pixel format information
 * @mode_cmd: metadata from the userspace fb creation request
 *
 * This helper can be used in a drivers fb_create callback to pre-fill the fb's
 * metadata fields.
 */
void drm_helper_mode_fill_fb_struct(struct drm_device *dev,
				    struct drm_framebuffer *fb,
				    const struct drm_format_info *info,
				    const struct drm_mode_fb_cmd2 *mode_cmd)
{
	int i;

	fb->dev = dev;
	fb->format = info;
	fb->width = mode_cmd->width;
	fb->height = mode_cmd->height;
	for (i = 0; i < 4; i++) {
		fb->pitches[i] = mode_cmd->pitches[i];
		fb->offsets[i] = mode_cmd->offsets[i];
	}
	fb->modifier = mode_cmd->modifier[0];
	fb->flags = mode_cmd->flags;
}
EXPORT_SYMBOL(drm_helper_mode_fill_fb_struct);

/*
 * This is the minimal list of formats that seem to be safe for modeset use
 * with all current DRM drivers.  Most hardware can actually support more
 * formats than this and drivers may specify a more accurate list when
 * creating the primary plane.
 */
static const uint32_t safe_modeset_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const struct drm_plane_funcs primary_plane_funcs = {
	DRM_PLANE_NON_ATOMIC_FUNCS,
};

/**
 * drm_crtc_init - 传统 CRTC 初始化函数（向后兼容）
 * @dev: DRM 设备
 * @crtc: 要初始化的 CRTC 对象
 * @funcs: 新 CRTC 的回调函数
 *
 * 初始化 CRTC 对象，并附带一个默认辅助提供的主平面，不创建光标平面。
 *
 * 注意，此函数对硬件限制做出了一些假设，这些假设可能不适用于所有硬件：
 *   1. 主平面不能重新定位
 *   2. 主平面不能缩放
 *   3. 主平面必须覆盖整个 CRTC
 *   4. 不支持亚像素定位
 *   5. 如果 CRTC 已启用，主平面必须始终开启
 *
 * 这纯粹是为旧驱动提供的向后兼容辅助函数。新驱动应自行实现主平面，
 * 原子驱动必须这样做。
 *
 * 返回：
 * 成功返回 0，失败返回错误码。
 */
 * @dev: DRM device
 * @crtc: CRTC object to init
 * @funcs: callbacks for the new CRTC
 *
 * Initialize a CRTC object with a default helper-provided primary plane and no
 * cursor plane.
 *
 * Note that we make some assumptions about hardware limitations that may not be
 * true for all hardware:
 *
 * 1. Primary plane cannot be repositioned.
 * 2. Primary plane cannot be scaled.
 * 3. Primary plane must cover the entire CRTC.
 * 4. Subpixel positioning is not supported.
 * 5. The primary plane must always be on if the CRTC is enabled.
 *
 * This is purely a backwards compatibility helper for old drivers. Drivers
 * should instead implement their own primary plane. Atomic drivers must do so.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drm_crtc_init(struct drm_device *dev, struct drm_crtc *crtc,
		  const struct drm_crtc_funcs *funcs)
{
	struct drm_plane *primary;
	int ret;

	/* possible_crtc's will be filled in later by crtc_init */
	primary = __drm_universal_plane_alloc(dev, sizeof(*primary), 0, 0,
					      &primary_plane_funcs,
					      safe_modeset_formats,
					      ARRAY_SIZE(safe_modeset_formats),
					      NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (IS_ERR(primary))
		return PTR_ERR(primary);

	/*
	 * Remove the format_default field from drm_plane when dropping
	 * this helper.
	 */
	primary->format_default = true;

	ret = drm_crtc_init_with_planes(dev, crtc, primary, NULL, funcs, NULL);
	if (ret)
		goto err_drm_plane_cleanup;

	return 0;

err_drm_plane_cleanup:
	drm_plane_cleanup(primary);
	kfree(primary);
	return ret;
}
EXPORT_SYMBOL(drm_crtc_init);

/**
 * drm_mode_config_helper_suspend - 模式设置挂起辅助函数
 * @dev: DRM 设备
 *
 * 此辅助函数负责处理模式设置方面的挂起操作。它会：
 *   1. 如果已初始化输出轮询，则禁用它
 *   2. 挂起 fbdev（如果已使用）
 *   3. 最后调用 drm_atomic_helper_suspend() 保存原子状态
 *
 * 如果挂起失败，fbdev 和输出轮询会被重新启用。
 *
 * 返回：
 * 成功返回 0，失败返回负错误码。
 *
 * 另请参见：
 * drm_kms_helper_poll_disable() 和 drm_client_dev_suspend()
 */
 * @dev: DRM device
 *
 * This helper function takes care of suspending the modeset side. It disables
 * output polling if initialized, suspends fbdev if used and finally calls
 * drm_atomic_helper_suspend().
 * If suspending fails, fbdev and polling is re-enabled.
 *
 * Returns:
 * Zero on success, negative error code on error.
 *
 * See also:
 * drm_kms_helper_poll_disable() and drm_client_dev_suspend().
 */
int drm_mode_config_helper_suspend(struct drm_device *dev)
{
	struct drm_atomic_state *state;

	if (!dev)
		return 0;
	/*
	 * Don't disable polling if it was never initialized
	 */
	if (dev->mode_config.poll_enabled)
		drm_kms_helper_poll_disable(dev);

	drm_client_dev_suspend(dev);
	state = drm_atomic_helper_suspend(dev);
	if (IS_ERR(state)) {
		drm_client_dev_resume(dev);

		/*
		 * Don't enable polling if it was never initialized
		 */
		if (dev->mode_config.poll_enabled)
			drm_kms_helper_poll_enable(dev);

		return PTR_ERR(state);
	}

	dev->mode_config.suspend_state = state;

	return 0;
}
EXPORT_SYMBOL(drm_mode_config_helper_suspend);

/**
 * drm_mode_config_helper_resume - 模式设置恢复辅助函数
 * @dev: DRM 设备
 *
 * 此辅助函数负责处理模式设置方面的恢复操作。它会：
 *   1. 调用 drm_atomic_helper_resume() 恢复原子状态
 *   2. 恢复 fbdev（如果已使用）
 *   3. 如果已初始化输出轮询，则启用它
 *
 * 返回：
 * 成功返回 0，失败返回负错误码。
 *
 * 另请参见：
 * drm_client_dev_resume() 和 drm_kms_helper_poll_enable()
 */
 * @dev: DRM device
 *
 * This helper function takes care of resuming the modeset side. It calls
 * drm_atomic_helper_resume(), resumes fbdev if used and enables output polling
 * if initiaized.
 *
 * Returns:
 * Zero on success, negative error code on error.
 *
 * See also:
 * drm_client_dev_resume() and drm_kms_helper_poll_enable().
 */
int drm_mode_config_helper_resume(struct drm_device *dev)
{
	int ret;

	if (!dev)
		return 0;

	if (WARN_ON(!dev->mode_config.suspend_state))
		return -EINVAL;

	ret = drm_atomic_helper_resume(dev, dev->mode_config.suspend_state);
	if (ret)
		DRM_ERROR("Failed to resume (%d)\n", ret);
	dev->mode_config.suspend_state = NULL;

	drm_client_dev_resume(dev);

	/*
	 * Don't enable polling if it is not initialized
	 */
	if (dev->mode_config.poll_enabled)
		drm_kms_helper_poll_enable(dev);

	return ret;
}
EXPORT_SYMBOL(drm_mode_config_helper_resume);
