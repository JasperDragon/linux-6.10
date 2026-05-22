/*
 * Copyright (c) 2006-2008 Intel Corporation
 * Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
 *
 * DRM core CRTC related functions
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
 *
 * Authors:
 *      Keith Packard
 *	Eric Anholt <eric@anholt.net>
 *      Dave Airlie <airlied@linux.ie>
 *      Jesse Barnes <jesse.barnes@intel.com>
 */

/*
 * DRM 连接器探测辅助函数
 *
 * 本文件提供了连接器输出探测的辅助函数。核心功能是实现了
 * &drm_connector_funcs.fill_modes 接口的
 * drm_helper_probe_single_connector_modes() 函数。
 *
 * 此外，还提供了：
 *   - 通过工作项定期轮询连接器状态的机制（output_poll_execute）
 *   - 通用的热插拔中断处理（无需驱动追踪每个连接器的 HPD 中断）
 *   - 连接器状态检测辅助函数
 *   - EDID 读取和模式填充的默认实现
 *
 * 探测流程：
 *   1. 对连接器进行状态检测（调用 detect 回调）
 *   2. 获取显示模式（通过 get_modes 回调或添加 EDID 模式）
 *   3. 添加内核命令行指定的模式
 *   4. 验证所有模式的有效性
 *   5. 移除无效的模式
 *   6. 对剩余模式进行排序
 *
 * 轮询机制：
 *   驱动可以为连接器设置 DRM_CONNECTOR_POLL_CONNECT/DISCONNECT 标志，
 *   辅助框架会定期（默认 10 秒）轮询这些连接器的状态变化。
 */

#include <linux/export.h>

#include <linux/export.h>
#include <linux/moduleparam.h>

#include <drm/drm_bridge.h>
#include <drm/drm_client_event.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_sysfs.h>

#include "drm_crtc_helper_internal.h"

/**
 * DOC: output probing helper overview
 *
 * This library provides some helper code for output probing. It provides an
 * implementation of the core &drm_connector_funcs.fill_modes interface with
 * drm_helper_probe_single_connector_modes().
 *
 * It also provides support for polling connectors with a work item and for
 * generic hotplug interrupt handling where the driver doesn't or cannot keep
 * track of a per-connector hpd interrupt.
 *
 * This helper library can be used independently of the modeset helper library.
 * Drivers can also overwrite different parts e.g. use their own hotplug
 * handling code to avoid probing unrelated outputs.
 *
 * The probe helpers share the function table structures with other display
 * helper libraries. See &struct drm_connector_helper_funcs for the details.
 */

static bool drm_kms_helper_poll = true;
module_param_named(poll, drm_kms_helper_poll, bool, 0600);

static enum drm_mode_status
drm_mode_validate_flag(const struct drm_display_mode *mode,
		       int flags)
{
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) &&
	    !(flags & DRM_MODE_FLAG_INTERLACE))
		return MODE_NO_INTERLACE;

	if ((mode->flags & DRM_MODE_FLAG_DBLSCAN) &&
	    !(flags & DRM_MODE_FLAG_DBLSCAN))
		return MODE_NO_DBLESCAN;

	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) &&
	    !(flags & DRM_MODE_FLAG_3D_MASK))
		return MODE_NO_STEREO;

	return MODE_OK;
}

static int
drm_mode_validate_pipeline(struct drm_display_mode *mode,
			   struct drm_connector *connector,
			   struct drm_modeset_acquire_ctx *ctx,
			   enum drm_mode_status *status)
{
	struct drm_device *dev = connector->dev;
	struct drm_encoder *encoder;
	int ret;

	/* Step 1: Validate against connector */
	ret = drm_connector_mode_valid(connector, mode, ctx, status);
	if (ret || *status != MODE_OK)
		return ret;

	/* Step 2: Validate against encoders and crtcs */
	drm_connector_for_each_possible_encoder(connector, encoder) {
		struct drm_bridge *bridge;
		struct drm_crtc *crtc;

		*status = drm_encoder_mode_valid(encoder, mode);
		if (*status != MODE_OK) {
			/* No point in continuing for crtc check as this encoder
			 * will not accept the mode anyway. If all encoders
			 * reject the mode then, at exit, ret will not be
			 * MODE_OK. */
			continue;
		}

		bridge = drm_bridge_chain_get_first_bridge(encoder);
		*status = drm_bridge_chain_mode_valid(bridge,
						      &connector->display_info,
						      mode);
		drm_bridge_put(bridge);
		if (*status != MODE_OK) {
			/* There is also no point in continuing for crtc check
			 * here. */
			continue;
		}

		drm_for_each_crtc(crtc, dev) {
			if (!drm_encoder_crtc_ok(encoder, crtc))
				continue;

			*status = drm_crtc_mode_valid(crtc, mode);
			if (*status == MODE_OK) {
				/* If we get to this point there is at least
				 * one combination of encoder+crtc that works
				 * for this mode. Lets return now. */
				return 0;
			}
		}
	}

	return 0;
}

static int drm_helper_probe_add_cmdline_mode(struct drm_connector *connector)
{
	struct drm_cmdline_mode *cmdline_mode;
	struct drm_display_mode *mode;

	cmdline_mode = &connector->cmdline_mode;
	if (!cmdline_mode->specified)
		return 0;

	/* Only add a GTF mode if we find no matching probed modes */
	list_for_each_entry(mode, &connector->probed_modes, head) {
		if (mode->hdisplay != cmdline_mode->xres ||
		    mode->vdisplay != cmdline_mode->yres)
			continue;

		if (cmdline_mode->refresh_specified) {
			/* The probed mode's vrefresh is set until later */
			if (drm_mode_vrefresh(mode) != cmdline_mode->refresh)
				continue;
		}

		/* Mark the matching mode as being preferred by the user */
		mode->type |= DRM_MODE_TYPE_USERDEF;
		return 0;
	}

	mode = drm_mode_create_from_cmdline_mode(connector->dev,
						 cmdline_mode);
	if (mode == NULL)
		return 0;

	drm_mode_probed_add(connector, mode);
	return 1;
}

enum drm_mode_status drm_crtc_mode_valid(struct drm_crtc *crtc,
					 const struct drm_display_mode *mode)
{
	const struct drm_crtc_helper_funcs *crtc_funcs = crtc->helper_private;

	if (!crtc_funcs || !crtc_funcs->mode_valid)
		return MODE_OK;

	return crtc_funcs->mode_valid(crtc, mode);
}

enum drm_mode_status drm_encoder_mode_valid(struct drm_encoder *encoder,
					    const struct drm_display_mode *mode)
{
	const struct drm_encoder_helper_funcs *encoder_funcs =
		encoder->helper_private;

	if (!encoder_funcs || !encoder_funcs->mode_valid)
		return MODE_OK;

	return encoder_funcs->mode_valid(encoder, mode);
}

int
drm_connector_mode_valid(struct drm_connector *connector,
			 const struct drm_display_mode *mode,
			 struct drm_modeset_acquire_ctx *ctx,
			 enum drm_mode_status *status)
{
	const struct drm_connector_helper_funcs *connector_funcs =
		connector->helper_private;
	int ret = 0;

	if (!connector_funcs)
		*status = MODE_OK;
	else if (connector_funcs->mode_valid_ctx)
		ret = connector_funcs->mode_valid_ctx(connector, mode, ctx,
						      status);
	else if (connector_funcs->mode_valid)
		*status = connector_funcs->mode_valid(connector, mode);
	else
		*status = MODE_OK;

	return ret;
}

static void drm_kms_helper_disable_hpd(struct drm_device *dev)
{
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		const struct drm_connector_helper_funcs *funcs =
			connector->helper_private;

		if (funcs && funcs->disable_hpd)
			funcs->disable_hpd(connector);
	}
	drm_connector_list_iter_end(&conn_iter);
}

static bool drm_kms_helper_enable_hpd(struct drm_device *dev)
{
	bool poll = false;
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		const struct drm_connector_helper_funcs *funcs =
			connector->helper_private;

		if (funcs && funcs->enable_hpd)
			funcs->enable_hpd(connector);

		if (connector->polled & (DRM_CONNECTOR_POLL_CONNECT |
					 DRM_CONNECTOR_POLL_DISCONNECT))
			poll = true;
	}
	drm_connector_list_iter_end(&conn_iter);

	return poll;
}

#define DRM_OUTPUT_POLL_PERIOD (10*HZ)
static void reschedule_output_poll_work(struct drm_device *dev)
{
	unsigned long delay = DRM_OUTPUT_POLL_PERIOD;

	if (dev->mode_config.delayed_event)
		/*
		 * FIXME:
		 *
		 * Use short (1s) delay to handle the initial delayed event.
		 * This delay should not be needed, but Optimus/nouveau will
		 * fail in a mysterious way if the delayed event is handled as
		 * soon as possible like it is done in
		 * drm_helper_probe_single_connector_modes() in case the poll
		 * was enabled before.
		 */
		delay = HZ;

	schedule_delayed_work(&dev->mode_config.output_poll_work, delay);
}

/**
 * drm_kms_helper_poll_enable - 重新启用输出轮询
 * @dev: DRM 设备
 *
 * 在使用 drm_kms_helper_poll_disable() 临时禁用输出轮询后
 * （例如在 suspend/resume 期间），此函数重新启用输出轮询工作。
 *
 * 驱动可以从其设备恢复（resume）实现中调用此辅助函数。
 * 即使输出轮询尚未启用，调用此函数也不是错误。
 *
 * 如果设备轮询从未被初始化过，此调用会触发警告并返回。
 *
 * 注意：启用和禁用轮询的调用必须严格有序，
 * 当仅在 suspend/resume 回调中调用时自动满足此要求。
 */
void drm_kms_helper_poll_enable(struct drm_device *dev)
{
	if (drm_WARN_ON_ONCE(dev, !dev->mode_config.poll_enabled) ||
	    !drm_kms_helper_poll || dev->mode_config.poll_running)
		return;

	if (drm_kms_helper_enable_hpd(dev) ||
	    dev->mode_config.delayed_event)
		reschedule_output_poll_work(dev);

	dev->mode_config.poll_running = true;
}
EXPORT_SYMBOL(drm_kms_helper_poll_enable);

/**
 * drm_kms_helper_poll_reschedule - 重新调度输出轮询工作
 * @dev: DRM 设备
 *
 * 在启用某个连接器的轮询后，重新调度输出轮询工作。
 *
 * 驱动在通过设置 drm_connector::polled 中的
 * %DRM_CONNECTOR_POLL_CONNECT / %DRM_CONNECTOR_POLL_DISCONNECT 标志
 * 来启用连接器的轮询后，必须调用此辅助函数。
 * 注意：如果通过清除这些标志禁用了某个连接器的轮询，且所有其他
 * 连接器的轮询也被禁用，输出轮询工作会自动停止。
 *
 * 只有在调用了 drm_kms_helper_poll_init() / drm_kms_helper_poll_enable()
 * 启用轮询后才能调用此函数。
 */
void drm_kms_helper_poll_reschedule(struct drm_device *dev)
{
	if (dev->mode_config.poll_running)
		reschedule_output_poll_work(dev);
}
EXPORT_SYMBOL(drm_kms_helper_poll_reschedule);

static int detect_connector_status(struct drm_connector *connector,
				   struct drm_modeset_acquire_ctx *ctx,
				   bool force)
{
	const struct drm_connector_helper_funcs *funcs = connector->helper_private;

	if (funcs->detect_ctx)
		return funcs->detect_ctx(connector, ctx, force);
	else if (connector->funcs->detect)
		return connector->funcs->detect(connector, force);

	return connector_status_connected;
}

static enum drm_connector_status
drm_helper_probe_detect_ctx(struct drm_connector *connector, bool force)
{
	struct drm_modeset_acquire_ctx ctx;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);

retry:
	ret = drm_modeset_lock(&connector->dev->mode_config.connection_mutex, &ctx);
	if (!ret)
		ret = detect_connector_status(connector, &ctx, force);

	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	}

	if (WARN_ON(ret < 0))
		ret = connector_status_unknown;

	if (ret != connector->status)
		connector->epoch_counter += 1;

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}

/**
 * drm_helper_probe_detect - 探测连接器状态
 * @connector: 要探测的连接器
 * @ctx: 锁获取上下文，或 NULL 表示让此函数自行处理锁定
 * @force: 是否执行破坏性（destructive）探测操作
 *
 * 调用连接器的 detect 回调来检测其状态。
 * 如果 @ctx 不为 NULL，调用者负责锁定管理，此函数可能返回 -EDEADLK
 * 表示需要回退（backoff）并重试。
 * 如果 @ctx 为 NULL，此函数自行处理所有锁定。
 */
int
drm_helper_probe_detect(struct drm_connector *connector,
			struct drm_modeset_acquire_ctx *ctx,
			bool force)
{
	struct drm_device *dev = connector->dev;
	int ret;

	if (!ctx)
		return drm_helper_probe_detect_ctx(connector, force);

	ret = drm_modeset_lock(&dev->mode_config.connection_mutex, ctx);
	if (ret)
		return ret;

	ret = detect_connector_status(connector, ctx, force);

	if (ret != connector->status)
		connector->epoch_counter += 1;

	return ret;
}
EXPORT_SYMBOL(drm_helper_probe_detect);

static int drm_helper_probe_get_modes(struct drm_connector *connector)
{
	const struct drm_connector_helper_funcs *connector_funcs =
		connector->helper_private;
	int count;

	count = connector_funcs->get_modes(connector);

	/* The .get_modes() callback should not return negative values. */
	if (count < 0) {
		drm_err(connector->dev, ".get_modes() returned %pe\n",
			ERR_PTR(count));
		count = 0;
	}

	/*
	 * Fallback for when DDC probe failed in drm_get_edid() and thus skipped
	 * override/firmware EDID.
	 */
	if (count == 0 && connector->status == connector_status_connected)
		count = drm_edid_override_connector_update(connector);

	return count;
}

static int __drm_helper_update_and_validate(struct drm_connector *connector,
					    uint32_t maxX, uint32_t maxY,
					    struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	int mode_flags = 0;
	int ret;

	drm_connector_list_update(connector);

	if (connector->interlace_allowed)
		mode_flags |= DRM_MODE_FLAG_INTERLACE;
	if (connector->doublescan_allowed)
		mode_flags |= DRM_MODE_FLAG_DBLSCAN;
	if (connector->stereo_allowed)
		mode_flags |= DRM_MODE_FLAG_3D_MASK;

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->status != MODE_OK)
			continue;

		mode->status = drm_mode_validate_driver(dev, mode);
		if (mode->status != MODE_OK)
			continue;

		mode->status = drm_mode_validate_size(mode, maxX, maxY);
		if (mode->status != MODE_OK)
			continue;

		mode->status = drm_mode_validate_flag(mode, mode_flags);
		if (mode->status != MODE_OK)
			continue;

		mode->status = drm_mode_validate_ycbcr420(mode, connector);
		if (mode->status != MODE_OK)
			continue;

		ret = drm_mode_validate_pipeline(mode, connector, ctx,
						 &mode->status);
		if (ret) {
			drm_dbg_kms(dev,
				    "drm_mode_validate_pipeline failed: %d\n",
				    ret);

			if (drm_WARN_ON_ONCE(dev, ret != -EDEADLK))
				mode->status = MODE_ERROR;
			else
				return -EDEADLK;
		}
	}

	return 0;
}

/**
 * drm_helper_probe_single_connector_modes - 获取完整的显示模式集合
 * @connector: 要探测的连接器
 * @maxX: 模式的最大宽度
 * @maxY: 模式的最大高度
 *
 * 基于 @connector 在 &drm_connector_helper_funcs 中实现辅助回调，
 * 尝试检测所有有效的显示模式。模式首先被添加到连接器的 probed_modes 列表，
 * 然后经过验证和筛选（基于有效性和 @maxX/@maxY 参数），
 * 最后放入正常的 modes 列表中。
 *
 * 这是 &drm_connector_funcs.fill_modes() vfunc 的通用实现，
 * 供使用 CRTC 辅助函数进行输出模式过滤和检测的驱动使用。
 *
 * 基本流程：
 *
 * 1. 将连接器 modes 列表中现有模式标记为过期（stale）
 *
 * 2. 向 probed_modes 列表添加新模式，按以下优先级从单个来源添加：
 *    - &drm_connector_helper_funcs.get_modes vfunc
 *    - 如果连接器状态为 connected，自动添加标准 VESA DMT 模式
 *      （最高 1024x768，通过 drm_add_modes_noedid()）
 *    - 最后添加内核命令行（video=...）指定的模式
 *      （通过 drm_helper_probe_add_cmdline_mode()）
 *
 * 3. 模式从 probed_modes 列表移动到 modes 列表，重复项合并
 *
 * 4. 所有非过期模式进行验证：
 *    - drm_mode_validate_basic()：基本完整性检查
 *    - drm_mode_validate_size()：筛选超过 @maxX/@maxY 的模式
 *    - drm_mode_validate_flag()：检查连接器能力标志
 *    - drm_connector_helper_funcs.mode_valid/mode_valid_ctx：
 *      驱动和/或接收端特定检查
 *    - drm_crtc_helper_funcs.mode_valid、drm_bridge_funcs.mode_valid、
 *      drm_encoder_helper_funcs.mode_valid：驱动和/或源端特定检查
 *
 * 5. 移除所有状态不为 OK 的模式
 *
 * 对于 DisplayPort 连接器，如果所有模式都被移除，会尝试添加
 * 640x480 @60Hz 作为故障安全模式（符合 DP 规范 5.2.1.2 节要求）。
 *
 * 返回值：
 * 在 @connector 上找到的模式数量。
 */
int drm_helper_probe_single_connector_modes(struct drm_connector *connector,
					    uint32_t maxX, uint32_t maxY)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	int count = 0, ret;
	enum drm_connector_status old_status;
	struct drm_modeset_acquire_ctx ctx;

	WARN_ON(!mutex_is_locked(&dev->mode_config.mutex));

	drm_modeset_acquire_init(&ctx, 0);

	drm_dbg_kms(dev, "[CONNECTOR:%d:%s]\n", connector->base.id,
		    connector->name);

retry:
	ret = drm_modeset_lock(&dev->mode_config.connection_mutex, &ctx);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	} else
		WARN_ON(ret < 0);

	/* set all old modes to the stale state */
	list_for_each_entry(mode, &connector->modes, head)
		mode->status = MODE_STALE;

	old_status = connector->status;

	if (connector->force) {
		if (connector->force == DRM_FORCE_ON ||
		    connector->force == DRM_FORCE_ON_DIGITAL)
			connector->status = connector_status_connected;
		else
			connector->status = connector_status_disconnected;
		if (connector->funcs->force)
			connector->funcs->force(connector);
	} else {
		ret = drm_helper_probe_detect(connector, &ctx, true);

		if (ret == -EDEADLK) {
			drm_modeset_backoff(&ctx);
			goto retry;
		} else if (WARN(ret < 0, "Invalid return value %i for connector detection\n", ret))
			ret = connector_status_unknown;

		connector->status = ret;
	}

	/*
	 * Normally either the driver's hpd code or the poll loop should
	 * pick up any changes and fire the hotplug event. But if
	 * userspace sneaks in a probe, we might miss a change. Hence
	 * check here, and if anything changed start the hotplug code.
	 */
	if (old_status != connector->status) {
		drm_dbg_kms(dev, "[CONNECTOR:%d:%s] status updated from %s to %s\n",
			    connector->base.id, connector->name,
			    drm_get_connector_status_name(old_status),
			    drm_get_connector_status_name(connector->status));

		/*
		 * The hotplug event code might call into the fb
		 * helpers, and so expects that we do not hold any
		 * locks. Fire up the poll struct instead, it will
		 * disable itself again.
		 */
		dev->mode_config.delayed_event = true;
		if (dev->mode_config.poll_enabled)
			mod_delayed_work(system_percpu_wq,
					 &dev->mode_config.output_poll_work,
					 0);
	}

	/*
	 * Re-enable polling in case the global poll config changed but polling
	 * is still initialized.
	 */
	if (dev->mode_config.poll_enabled)
		drm_kms_helper_poll_enable(dev);

	if (connector->status == connector_status_disconnected) {
		drm_dbg_kms(dev, "[CONNECTOR:%d:%s] disconnected\n",
			    connector->base.id, connector->name);
		drm_connector_update_edid_property(connector, NULL);
		drm_mode_prune_invalid(dev, &connector->modes, false);
		goto exit;
	}

	count = drm_helper_probe_get_modes(connector);

	if (count == 0 && (connector->status == connector_status_connected ||
			   connector->status == connector_status_unknown)) {
		count = drm_add_modes_noedid(connector, 1024, 768);

		/*
		 * Section 4.2.2.6 (EDID Corruption Detection) of the DP 1.4a
		 * Link CTS specifies that 640x480 (the official "failsafe"
		 * mode) needs to be the default if there's no EDID.
		 */
		if (connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
			drm_set_preferred_mode(connector, 640, 480);
	}
	count += drm_helper_probe_add_cmdline_mode(connector);
	if (count != 0) {
		ret = __drm_helper_update_and_validate(connector, maxX, maxY, &ctx);
		if (ret == -EDEADLK) {
			drm_modeset_backoff(&ctx);
			goto retry;
		}
	}

	drm_mode_prune_invalid(dev, &connector->modes, true);

	/*
	 * Displayport spec section 5.2.1.2 ("Video Timing Format") says that
	 * all detachable sinks shall support 640x480 @60Hz as a fail safe
	 * mode. If all modes were pruned, perhaps because they need more
	 * lanes or a higher pixel clock than available, at least try to add
	 * in 640x480.
	 */
	if (list_empty(&connector->modes) &&
	    connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort) {
		count = drm_add_modes_noedid(connector, 640, 480);
		ret = __drm_helper_update_and_validate(connector, maxX, maxY, &ctx);
		if (ret == -EDEADLK) {
			drm_modeset_backoff(&ctx);
			goto retry;
		}
		drm_mode_prune_invalid(dev, &connector->modes, true);
	}

exit:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	if (list_empty(&connector->modes))
		return 0;

	drm_mode_sort(&connector->modes);

	drm_dbg_kms(dev, "[CONNECTOR:%d:%s] probed modes:\n",
		    connector->base.id, connector->name);

	list_for_each_entry(mode, &connector->modes, head) {
		drm_mode_set_crtcinfo(mode, CRTC_INTERLACE_HALVE_V);
		drm_dbg_kms(dev, "Probed mode: " DRM_MODE_FMT "\n",
			    DRM_MODE_ARG(mode));
	}

	return count;
}
EXPORT_SYMBOL(drm_helper_probe_single_connector_modes);

/**
 * drm_kms_helper_hotplug_event - 触发 KMS 热插拔事件
 * @dev: 连接器状态发生变化的 DRM 设备
 *
 * 向用户空间发送 uevent，同时调用客户端热插拔函数（最常用于通知
 * fbdev 模拟代码更新 fbcon 输出配置）。
 *
 * 驱动在检测到连接器状态变化时，应从其热插拔处理代码中调用此函数。
 * 注意：此函数不自行执行任何输出检测（不像 drm_helper_hpd_irq_event()），
 * 驱动应已经完成了检测工作。
 *
 * 必须从进程上下文中调用，且不持有任何模式设置锁。
 *
 * 如果只有单个连接器发生变化，建议使用
 * drm_kms_helper_connector_hotplug_event() 以获得更细粒度的事件通知。
 */
void drm_kms_helper_hotplug_event(struct drm_device *dev)
{
	drm_sysfs_hotplug_event(dev);
	drm_client_dev_hotplug(dev);
}
EXPORT_SYMBOL(drm_kms_helper_hotplug_event);

/**
 * drm_kms_helper_connector_hotplug_event - 触发 KMS 连接器热插拔事件
 * @connector: 发生变化的 DRM 连接器
 *
 * 与 drm_kms_helper_hotplug_event() 功能相同，但只为单个连接器
 * 触发更细粒度的 uevent。当驱动可以精确定位是哪个连接器发生变化时，
 * 应优先使用此函数。
 */
void drm_kms_helper_connector_hotplug_event(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;

	drm_sysfs_connector_hotplug_event(connector);
	drm_client_dev_hotplug(dev);
}
EXPORT_SYMBOL(drm_kms_helper_connector_hotplug_event);

static void output_poll_execute(struct work_struct *work)
{
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct drm_device *dev = container_of(delayed_work, struct drm_device, mode_config.output_poll_work);
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	enum drm_connector_status old_status;
	bool repoll = false, changed;
	u64 old_epoch_counter;

	if (!dev->mode_config.poll_enabled)
		return;

	/* Pick up any changes detected by the probe functions. */
	changed = dev->mode_config.delayed_event;
	dev->mode_config.delayed_event = false;

	if (!drm_kms_helper_poll) {
		if (dev->mode_config.poll_running) {
			drm_kms_helper_disable_hpd(dev);
			dev->mode_config.poll_running = false;
		}
		goto out;
	}

	if (!mutex_trylock(&dev->mode_config.mutex)) {
		repoll = true;
		goto out;
	}

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		/* Ignore forced connectors. */
		if (connector->force)
			continue;

		/* Ignore HPD capable connectors and connectors where we don't
		 * want any hotplug detection at all for polling. */
		if (!connector->polled || connector->polled == DRM_CONNECTOR_POLL_HPD)
			continue;

		old_status = connector->status;
		/* if we are connected and don't want to poll for disconnect
		   skip it */
		if (old_status == connector_status_connected &&
		    !(connector->polled & DRM_CONNECTOR_POLL_DISCONNECT))
			continue;

		repoll = true;

		old_epoch_counter = connector->epoch_counter;
		connector->status = drm_helper_probe_detect(connector, NULL, false);
		if (old_epoch_counter != connector->epoch_counter) {
			const char *old, *new;

			/*
			 * The poll work sets force=false when calling detect so
			 * that drivers can avoid to do disruptive tests (e.g.
			 * when load detect cycles could cause flickering on
			 * other, running displays). This bears the risk that we
			 * flip-flop between unknown here in the poll work and
			 * the real state when userspace forces a full detect
			 * call after receiving a hotplug event due to this
			 * change.
			 *
			 * Hence clamp an unknown detect status to the old
			 * value.
			 */
			if (connector->status == connector_status_unknown) {
				connector->status = old_status;
				continue;
			}

			old = drm_get_connector_status_name(old_status);
			new = drm_get_connector_status_name(connector->status);

			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] status updated from %s to %s\n",
				    connector->base.id, connector->name,
				    old, new);
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] epoch counter %llu -> %llu\n",
				    connector->base.id, connector->name,
				    old_epoch_counter, connector->epoch_counter);

			changed = true;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	mutex_unlock(&dev->mode_config.mutex);

out:
	if (changed)
		drm_kms_helper_hotplug_event(dev);

	if (repoll)
		schedule_delayed_work(delayed_work, DRM_OUTPUT_POLL_PERIOD);
}

/**
 * drm_kms_helper_is_poll_worker - 判断当前任务是否为输出轮询工作线程
 *
 * 确定当前任务是否是输出轮询工作线程。可用于为输出轮询与其他上下文
 * 选择不同的代码路径。
 *
 * 一个典型的使用场景是避免输出轮询工作线程和自动挂起工作线程之间的
 * 死锁：后者在调用 drm_kms_helper_poll_disable() 时等待轮询完成，
 * 而前者在连接器 ->detect 钩子中调用 pm_runtime_get_sync() 时
 * 等待运行时挂起完成。
 */
bool drm_kms_helper_is_poll_worker(void)
{
	struct work_struct *work = current_work();

	return work && work->func == output_poll_execute;
}
EXPORT_SYMBOL(drm_kms_helper_is_poll_worker);

/**
 * drm_kms_helper_poll_disable - 禁用输出轮询
 * @dev: DRM 设备
 *
 * 禁用输出轮询工作。驱动可以从其设备挂起（suspend）实现中调用此函数。
 * 即使输出轮询尚未启用或已禁用，调用此函数也不是错误。
 * 轮询通过调用 drm_kms_helper_poll_enable() 重新启用。
 *
 * 如果轮询从未被初始化过，此调用会触发警告并返回。
 *
 * 注意：启用和禁用轮询的调用必须严格有序，
 * 当仅在 suspend/resume 回调中调用时自动满足此要求。
 */
void drm_kms_helper_poll_disable(struct drm_device *dev)
{
	if (drm_WARN_ON(dev, !dev->mode_config.poll_enabled))
		return;

	if (dev->mode_config.poll_running)
		drm_kms_helper_disable_hpd(dev);

	cancel_delayed_work_sync(&dev->mode_config.output_poll_work);

	dev->mode_config.poll_running = false;
}
EXPORT_SYMBOL(drm_kms_helper_poll_disable);

/**
 * drm_kms_helper_poll_init - 初始化并启用输出轮询
 * @dev: DRM 设备
 *
 * 初始化并为 @dev 启用输出轮询支持。对于没有可靠热插拔硬件支持的驱动，
 * 可以使用此辅助基础设施定期轮询连接器的连接状态变化。
 *
 * 驱动可以通过设置 DRM_CONNECTOR_POLL_CONNECT 和 DRM_CONNECTOR_POLL_DISCONNECT
 * 标志来控制哪些连接器被轮询。对于探测带电输出可能导致视觉失真的连接器，
 * 驱动不应设置 DRM_CONNECTOR_POLL_DISCONNECT 标志以避免此问题。
 * 没有标志或仅设置了 DRM_CONNECTOR_POLL_HPD 的连接器被轮询逻辑完全忽略。
 *
 * 注意：如果热插拔中断已知不可靠，一个连接器可以同时被轮询和通过热插拔
 * 处理函数探测。
 */
void drm_kms_helper_poll_init(struct drm_device *dev)
{
	INIT_DELAYED_WORK(&dev->mode_config.output_poll_work, output_poll_execute);
	dev->mode_config.poll_enabled = true;

	drm_kms_helper_poll_enable(dev);
}
EXPORT_SYMBOL(drm_kms_helper_poll_init);

/**
 * drm_kms_helper_poll_fini - 禁用输出轮询并清理
 * @dev: DRM 设备
 *
 * 禁用输出轮询并清理相关资源。调用 drm_kms_helper_poll_disable()
 * 并设置 poll_enabled = false。
 */
void drm_kms_helper_poll_fini(struct drm_device *dev)
{
	if (!dev->mode_config.poll_enabled)
		return;

	drm_kms_helper_poll_disable(dev);

	dev->mode_config.poll_enabled = false;
}
EXPORT_SYMBOL(drm_kms_helper_poll_fini);

static void drm_kms_helper_poll_init_release(struct drm_device *dev, void *res)
{
	drm_kms_helper_poll_fini(dev);
}

/**
 * drmm_kms_helper_poll_init - 初始化并启用输出轮询（托管版本）
 * @dev: DRM 设备
 *
 * 类似于 drm_kms_helper_poll_init()，但使用 DRM 托管机制
 * （devres managed），当 DRM 设备销毁时轮询会自动清理。
 * 使用 drmm_add_action_or_reset() 注册了清理回调。
 *
 * 详见 drm_kms_helper_poll_init() 的文档。
 */
void drmm_kms_helper_poll_init(struct drm_device *dev)
{
	int ret;

	drm_kms_helper_poll_init(dev);

	ret = drmm_add_action_or_reset(dev, drm_kms_helper_poll_init_release, dev);
	if (ret)
		drm_warn(dev, "Connector status will not be updated, error %d\n", ret);
}
EXPORT_SYMBOL(drmm_kms_helper_poll_init);

static bool check_connector_changed(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	enum drm_connector_status old_status;
	u64 old_epoch_counter;

	/* Only handle HPD capable connectors. */
	drm_WARN_ON(dev, !(connector->polled & DRM_CONNECTOR_POLL_HPD));

	drm_WARN_ON(dev, !mutex_is_locked(&dev->mode_config.mutex));

	old_status = connector->status;
	old_epoch_counter = connector->epoch_counter;
	connector->status = drm_helper_probe_detect(connector, NULL, false);

	if (old_epoch_counter == connector->epoch_counter) {
		drm_dbg_kms(dev, "[CONNECTOR:%d:%s] Same epoch counter %llu\n",
			    connector->base.id,
			    connector->name,
			    connector->epoch_counter);

		return false;
	}

	drm_dbg_kms(dev, "[CONNECTOR:%d:%s] status updated from %s to %s\n",
		    connector->base.id,
		    connector->name,
		    drm_get_connector_status_name(old_status),
		    drm_get_connector_status_name(connector->status));

	drm_dbg_kms(dev, "[CONNECTOR:%d:%s] Changed epoch counter %llu => %llu\n",
		    connector->base.id,
		    connector->name,
		    old_epoch_counter,
		    connector->epoch_counter);

	return true;
}

/**
 * drm_connector_helper_hpd_irq_event - 单连接器热插拔事件处理
 * @connector: DRM 连接器
 *
 * 对设置了 DRM_CONNECTOR_POLL_HPD 标志的连接器执行一次检测循环。
 *
 * 此辅助函数适用于能够追踪单个连接器热插拔中断的驱动。
 * 如果需要为所有连接器发送热插拔事件或无法按连接器追踪
 * 热插拔中断，应使用 drm_helper_hpd_irq_event()。
 *
 * 此函数必须在进程上下文中调用，且不持有任何模式设置锁。
 * 如果热插拔中断已知不可靠，连接器可以同时被轮询和通过热插拔处理函数探测。
 *
 * 返回值：
 * 表示连接器状态是否发生变化的布尔值。
 */
bool drm_connector_helper_hpd_irq_event(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	bool changed;

	mutex_lock(&dev->mode_config.mutex);
	changed = check_connector_changed(connector);
	mutex_unlock(&dev->mode_config.mutex);

	if (changed) {
		drm_kms_helper_connector_hotplug_event(connector);
		drm_dbg_kms(dev, "[CONNECTOR:%d:%s] Sent hotplug event\n",
			    connector->base.id,
			    connector->name);
	}

	return changed;
}
EXPORT_SYMBOL(drm_connector_helper_hpd_irq_event);

/**
 * drm_helper_hpd_irq_event - 全局热插拔事件处理
 * @dev: DRM 设备
 *
 * 对所有设置了 DRM_CONNECTOR_POLL_HPD 标志的连接器执行一次检测循环。
 * 其他连接器被忽略（避免重新探测固定面板）。
 *
 * 此辅助函数适用于无法或不需要按连接器追踪热插拔中断的驱动。
 *
 * 如果只有一个连接器状态发生了变化，会触发针对该连接器的细粒度 uevent；
 * 如果有多个连接器变化，则触发全局热插拔事件。
 *
 * 支持单独热插拔中断和更细粒度检测逻辑的驱动应使用
 * drm_connector_helper_hpd_irq_event() 或直接调用 drm_kms_helper_hotplug_event()。
 *
 * 此函数必须在进程上下文中调用，且不持有任何模式设置锁。
 *
 * 返回值：
 * 表示是否有连接器状态发生变化的布尔值。
 */
bool drm_helper_hpd_irq_event(struct drm_device *dev)
{
	struct drm_connector *connector, *first_changed_connector = NULL;
	struct drm_connector_list_iter conn_iter;
	int changed = 0;

	if (!dev->mode_config.poll_enabled)
		return false;

	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		/* Only handle HPD capable connectors. */
		if (!(connector->polled & DRM_CONNECTOR_POLL_HPD))
			continue;

		if (check_connector_changed(connector)) {
			if (!first_changed_connector) {
				drm_connector_get(connector);
				first_changed_connector = connector;
			}

			changed++;
		}
	}
	drm_connector_list_iter_end(&conn_iter);
	mutex_unlock(&dev->mode_config.mutex);

	if (changed == 1)
		drm_kms_helper_connector_hotplug_event(first_changed_connector);
	else if (changed > 0)
		drm_kms_helper_hotplug_event(dev);

	if (first_changed_connector)
		drm_connector_put(first_changed_connector);

	return changed;
}
EXPORT_SYMBOL(drm_helper_hpd_irq_event);

/**
 * drm_crtc_helper_mode_valid_fixed - 验证显示模式（固定模式版本）
 * @crtc: CRTC
 * @mode: 要验证的模式
 * @fixed_mode: 显示硬件的固定模式
 *
 * 验证给定的模式是否与硬件固定的显示模式匹配。
 * 适用于只有一种固定分辨率的显示硬件（如许多嵌入式 LCD 面板）。
 *
 * 返回值：
 * 成功返回 MODE_OK，否则返回其他模式状态码
 * （如 MODE_ONE_SIZE、MODE_ONE_WIDTH、MODE_ONE_HEIGHT）。
 */
enum drm_mode_status drm_crtc_helper_mode_valid_fixed(struct drm_crtc *crtc,
						      const struct drm_display_mode *mode,
						      const struct drm_display_mode *fixed_mode)
{
	if (mode->hdisplay != fixed_mode->hdisplay && mode->vdisplay != fixed_mode->vdisplay)
		return MODE_ONE_SIZE;
	else if (mode->hdisplay != fixed_mode->hdisplay)
		return MODE_ONE_WIDTH;
	else if (mode->vdisplay != fixed_mode->vdisplay)
		return MODE_ONE_HEIGHT;

	return MODE_OK;
}
EXPORT_SYMBOL(drm_crtc_helper_mode_valid_fixed);

/**
 * drm_connector_helper_get_modes_fixed - 为连接器复制固定显示模式
 * @connector: 连接器
 * @fixed_mode: 显示硬件的固定模式
 *
 * 为连接器复制一个固定显示模式。只支持单一固定模式的显示硬件驱动
 * 可以在其连接器的 get_modes 辅助函数中使用此函数。
 *
 * 复制的模式会被标记为 DRM_MODE_TYPE_PREFERRED（首选模式），
 * 如果提供了物理尺寸信息，还会更新连接器的显示信息。
 *
 * 返回值：
 * 创建的模式数量（成功返回 1，失败返回 0）。
 */
int drm_connector_helper_get_modes_fixed(struct drm_connector *connector,
					 const struct drm_display_mode *fixed_mode)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(dev, fixed_mode);
	if (!mode) {
		drm_err(dev, "Failed to duplicate mode " DRM_MODE_FMT "\n",
			DRM_MODE_ARG(fixed_mode));
		return 0;
	}

	if (mode->name[0] == '\0')
		drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	if (mode->width_mm)
		connector->display_info.width_mm = mode->width_mm;
	if (mode->height_mm)
		connector->display_info.height_mm = mode->height_mm;

	return 1;
}
EXPORT_SYMBOL(drm_connector_helper_get_modes_fixed);

/**
 * drm_connector_helper_get_modes - 读取 EDID 并更新连接器
 * @connector: 连接器
 *
 * 使用 drm_edid_read() 读取 EDID（要求设置了 connector->ddc），
 * 并根据 EDID 内容更新连接器的显示信息和模式列表。
 *
 * 此函数可用作连接器辅助函数 .get_modes() 钩子的默认实现，
* 适用于不需要特殊处理的驱动。它展示了自定义 .get_modes() 钩子
 * 在 EDID 读取和连接器更新方面应遵循的模式。
 *
 * 返回值：
 * 添加的模式数量。
 */
int drm_connector_helper_get_modes(struct drm_connector *connector)
{
	const struct drm_edid *drm_edid;
	int count;

	drm_edid = drm_edid_read(connector);

	/*
	 * Unconditionally update the connector. If the EDID was read
	 * successfully, fill in the connector information derived from the
	 * EDID. Otherwise, if the EDID is NULL, clear the connector
	 * information.
	 */
	drm_edid_connector_update(connector, drm_edid);

	count = drm_edid_connector_add_modes(connector);

	drm_edid_free(drm_edid);

	return count;
}
EXPORT_SYMBOL(drm_connector_helper_get_modes);

/**
 * drm_connector_helper_tv_get_modes - 填充 TV 连接器的可用模式
 * @connector: 连接器
 *
 * 根据支持的电视制式（NTSC/PAL/SECAM 等）和内核命令行指定的默认模式，
 * 填充 TV 连接器的可用显示模式。
 *
 * 此函数可用作 TV 连接器辅助函数 .get_modes() 钩子的默认实现，
 * 适用于不需要特殊处理的驱动。
 *
 * 会根据驱动支持的电视制式自动选择 NTSC（480i）或 PAL（576i）模式，
 * 并考虑内核命令行中指定的电视制式参数。
 *
 * 返回值：
 * 添加到连接器的模式数量。
 */
int drm_connector_helper_tv_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct drm_property *tv_mode_property =
		dev->mode_config.tv_mode_property;
	struct drm_cmdline_mode *cmdline = &connector->cmdline_mode;
	unsigned int ntsc_modes = BIT(DRM_MODE_TV_MODE_NTSC) |
		BIT(DRM_MODE_TV_MODE_NTSC_443) |
		BIT(DRM_MODE_TV_MODE_NTSC_J) |
		BIT(DRM_MODE_TV_MODE_PAL_M);
	unsigned int pal_modes = BIT(DRM_MODE_TV_MODE_PAL) |
		BIT(DRM_MODE_TV_MODE_PAL_N) |
		BIT(DRM_MODE_TV_MODE_SECAM);
	unsigned int tv_modes[2] = { UINT_MAX, UINT_MAX };
	unsigned int i, supported_tv_modes = 0;

	if (!tv_mode_property)
		return 0;

	for (i = 0; i < tv_mode_property->num_values; i++)
		supported_tv_modes |= BIT(tv_mode_property->values[i]);

	if (((supported_tv_modes & ntsc_modes) &&
	     (supported_tv_modes & pal_modes)) ||
	    (supported_tv_modes & BIT(DRM_MODE_TV_MODE_MONOCHROME))) {
		uint64_t default_mode;

		if (drm_object_property_get_default_value(&connector->base,
							  tv_mode_property,
							  &default_mode))
			return 0;

		if (cmdline->tv_mode_specified)
			default_mode = cmdline->tv_mode;

		if (BIT(default_mode) & ntsc_modes) {
			tv_modes[0] = DRM_MODE_TV_MODE_NTSC;
			tv_modes[1] = DRM_MODE_TV_MODE_PAL;
		} else {
			tv_modes[0] = DRM_MODE_TV_MODE_PAL;
			tv_modes[1] = DRM_MODE_TV_MODE_NTSC;
		}
	} else if (supported_tv_modes & ntsc_modes) {
		tv_modes[0] = DRM_MODE_TV_MODE_NTSC;
	} else if (supported_tv_modes & pal_modes) {
		tv_modes[0] = DRM_MODE_TV_MODE_PAL;
	} else {
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(tv_modes); i++) {
		struct drm_display_mode *mode;

		if (tv_modes[i] == DRM_MODE_TV_MODE_NTSC)
			mode = drm_mode_analog_ntsc_480i(dev);
		else if (tv_modes[i] == DRM_MODE_TV_MODE_PAL)
			mode = drm_mode_analog_pal_576i(dev);
		else
			break;
		if (!mode)
			return i;
		if (!i)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
	}

	return i;
}
EXPORT_SYMBOL(drm_connector_helper_tv_get_modes);

/**
 * drm_connector_helper_detect_from_ddc - 通过 DDC 读取 EDID 并检测连接器状态
 * @connector: 连接器
 * @ctx: 锁获取上下文
 * @force: 是否执行破坏性操作
 *
 * 通过读取 EDID 来检测连接器状态（使用 drm_probe_ddc() 进行检测，
 * 要求设置了 connector->ddc）。
 *
 * 这是一种非破坏性的检测方法，适用于简单的 DDC 检测场景。
 * 如果 DDC 通信成功，假定连接器已连接；否则假定为已断开。
 *
 * 返回值：
 * 由 enum drm_connector_status 定义的连接器状态
 * （connector_status_connected / disconnected / unknown）。
 */
int drm_connector_helper_detect_from_ddc(struct drm_connector *connector,
					 struct drm_modeset_acquire_ctx *ctx,
					 bool force)
{
	struct i2c_adapter *ddc = connector->ddc;

	if (!ddc)
		return connector_status_unknown;

	if (drm_probe_ddc(ddc))
		return connector_status_connected;

	return connector_status_disconnected;
}
EXPORT_SYMBOL(drm_connector_helper_detect_from_ddc);
