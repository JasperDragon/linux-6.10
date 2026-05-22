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
 * DRM 传统 CRTC 模式设置辅助函数
 *
 * 本文件提供了一组基于回调的传统 CRTC 模式设置辅助函数。它们实现了
 * DRM 核心接口（如 set_config、dpms）与驱动硬件操作之间的中间层。
 * 这些函数将通用的模式设置操作分解为一系列驱动回调调用
 * （prepare、mode_set、commit、dpms、disable 等），
 * 使驱动可以分别实现每个阶段的硬件操作。
 *
 * 主要功能：
 *   - drm_crtc_helper_set_config：实现 &drm_crtc_funcs.set_config 回调
 *   - drm_crtc_helper_set_mode：内部辅助函数，执行实际的模式设置序列
 *   - drm_helper_connector_dpms：实现 &drm_connector_funcs.dpms 回调
 *   - drm_helper_resume_force_mode：从挂起状态恢复时强制恢复模式设置
 *   - drm_helper_force_disable_all：强制关闭所有已启用的 CRTC
 *   - drm_helper_disable_unused_functions：禁用未使用的编码器和 CRTC
 *
 * 重要说明：
 *   这些是传统的模式设置辅助函数，新的驱动必须使用原子模式设置框架。
 *   这些函数与原子辅助函数共享相同的函数表结构（&drm_crtc_helper_funcs、
 *   &drm_encoder_helper_funcs、&drm_connector_helper_funcs）。
 */

#include <linux/export.h>

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/dynamic_debug.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "drm_crtc_helper_internal.h"

DECLARE_DYNDBG_CLASSMAP(drm_debug_classes, DD_CLASS_TYPE_DISJOINT_BITS, 0,
			"DRM_UT_CORE",
			"DRM_UT_DRIVER",
			"DRM_UT_KMS",
			"DRM_UT_PRIME",
			"DRM_UT_ATOMIC",
			"DRM_UT_VBL",
			"DRM_UT_STATE",
			"DRM_UT_LEASE",
			"DRM_UT_DP",
			"DRM_UT_DRMRES");

/**
 * DOC: overview
 *
 * The CRTC modeset helper library provides a default set_config implementation
 * in drm_crtc_helper_set_config(). Plus a few other convenience functions using
 * the same callbacks which drivers can use to e.g. restore the modeset
 * configuration on resume with drm_helper_resume_force_mode().
 *
 * Note that this helper library doesn't track the current power state of CRTCs
 * and encoders. It can call callbacks like &drm_encoder_helper_funcs.dpms even
 * though the hardware is already in the desired state. This deficiency has been
 * fixed in the atomic helpers.
 *
 * The driver callbacks are mostly compatible with the atomic modeset helpers,
 * except for the handling of the primary plane: Atomic helpers require that the
 * primary plane is implemented as a real standalone plane and not directly tied
 * to the CRTC state. For easier transition this library provides functions to
 * implement the old semantics required by the CRTC helpers using the new plane
 * and atomic helper callbacks.
 *
 * Drivers are strongly urged to convert to the atomic helpers (by way of first
 * converting to the plane helpers). New drivers must not use these functions
 * but need to implement the atomic interface instead, potentially using the
 * atomic helpers for that.
 *
 * These legacy modeset helpers use the same function table structures as
 * all other modesetting helpers. See the documentation for struct
 * &drm_crtc_helper_funcs, &struct drm_encoder_helper_funcs and struct
 * &drm_connector_helper_funcs.
 */

/**
 * drm_helper_encoder_in_use - 检查给定的编码器是否被使用
 * @encoder: 要检查的编码器
 *
 * 检查在当前模式设置输出配置中，@encoder 是否被任何连接器使用。
 * 这不表示编码器实际已启用（DPMS 状态独立跟踪）。
 * 仅适用于传统（非原子）驱动。
 *
 * 返回值：
 * 如果 @encoder 被使用返回 true，否则返回 false。
 */
bool drm_helper_encoder_in_use(struct drm_encoder *encoder)
{
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	struct drm_device *dev = encoder->dev;

	drm_WARN_ON(dev, drm_drv_uses_atomic_modeset(dev));

	/*
	 * We can expect this mutex to be locked if we are not panicking.
	 * Locking is currently fubar in the panic handler.
	 */
	if (!oops_in_progress) {
		drm_WARN_ON(dev, !mutex_is_locked(&dev->mode_config.mutex));
		drm_WARN_ON(dev, !drm_modeset_is_locked(&dev->mode_config.connection_mutex));
	}


	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		if (connector->encoder == encoder) {
			drm_connector_list_iter_end(&conn_iter);
			return true;
		}
	}
	drm_connector_list_iter_end(&conn_iter);
	return false;
}
EXPORT_SYMBOL(drm_helper_encoder_in_use);

/**
 * drm_helper_crtc_in_use - 检查给定的 CRTC 是否在模式配置中被使用
 * @crtc: 要检查的 CRTC
 *
 * 检查在当前模式设置输出配置中，@crtc 是否被任何连接器使用。
 * 这不表示 CRTC 实际已启用（DPMS 状态独立跟踪）。
 * 仅适用于传统（非原子）驱动。
 *
 * 返回值：
 * 如果 @crtc 被使用返回 true，否则返回 false。
 */
bool drm_helper_crtc_in_use(struct drm_crtc *crtc)
{
	struct drm_encoder *encoder;
	struct drm_device *dev = crtc->dev;

	drm_WARN_ON(dev, drm_drv_uses_atomic_modeset(dev));

	/*
	 * We can expect this mutex to be locked if we are not panicking.
	 * Locking is currently fubar in the panic handler.
	 */
	if (!oops_in_progress)
		drm_WARN_ON(dev, !mutex_is_locked(&dev->mode_config.mutex));

	drm_for_each_encoder(encoder, dev)
		if (encoder->crtc == crtc && drm_helper_encoder_in_use(encoder))
			return true;
	return false;
}
EXPORT_SYMBOL(drm_helper_crtc_in_use);

static void
drm_encoder_disable(struct drm_encoder *encoder)
{
	const struct drm_encoder_helper_funcs *encoder_funcs = encoder->helper_private;

	if (!encoder_funcs)
		return;

	if (encoder_funcs->disable)
		(*encoder_funcs->disable)(encoder);
	else if (encoder_funcs->dpms)
		(*encoder_funcs->dpms)(encoder, DRM_MODE_DPMS_OFF);
}

static void __drm_helper_disable_unused_functions(struct drm_device *dev)
{
	struct drm_encoder *encoder;
	struct drm_crtc *crtc;

	drm_warn_on_modeset_not_all_locked(dev);

	drm_for_each_encoder(encoder, dev) {
		if (!drm_helper_encoder_in_use(encoder)) {
			drm_encoder_disable(encoder);
			/* disconnect encoder from any connector */
			encoder->crtc = NULL;
		}
	}

	drm_for_each_crtc(crtc, dev) {
		const struct drm_crtc_helper_funcs *crtc_funcs = crtc->helper_private;

		crtc->enabled = drm_helper_crtc_in_use(crtc);
		if (!crtc->enabled) {
			if (crtc_funcs->disable)
				(*crtc_funcs->disable)(crtc);
			else
				(*crtc_funcs->dpms)(crtc, DRM_MODE_DPMS_OFF);
			crtc->primary->fb = NULL;
		}
	}
}

/**
 * drm_helper_disable_unused_functions - 禁用未使用的对象
 * @dev: DRM 设备
 *
 * 遍历 @dev 的整个模式设置配置。此函数会移除未使用编码器的 CRTC 链接
 * 和已断开连接器的编码器链接。然后禁用所有未使用的编码器和 CRTC，
 * 优先调用 disable 回调，如果不可用则调用 dpms 回调（设为 DRM_MODE_DPMS_OFF）。
 *
 * 注意：
 * 此函数属于传统模式设置辅助库，与原子驱动不兼容。
 * 原子辅助框架保证不会在已禁用的函数上调用 ->disable()，
 * 也不会在已启用的函数上调用 ->enable()。
 * 但 drm_helper_disable_unused_functions() 不考虑这些保证，
 * 无条件地在未使用的函数上调用 disable 钩子。
 */
void drm_helper_disable_unused_functions(struct drm_device *dev)
{
	drm_WARN_ON(dev, drm_drv_uses_atomic_modeset(dev));

	drm_modeset_lock_all(dev);
	__drm_helper_disable_unused_functions(dev);
	drm_modeset_unlock_all(dev);
}
EXPORT_SYMBOL(drm_helper_disable_unused_functions);

/*
 * Check the CRTC we're going to map each output to vs. its current
 * CRTC.  If they don't match, we have to disable the output and the CRTC
 * since the driver will have to re-route things.
 */
static void
drm_crtc_prepare_encoders(struct drm_device *dev)
{
	const struct drm_encoder_helper_funcs *encoder_funcs;
	struct drm_encoder *encoder;

	drm_for_each_encoder(encoder, dev) {
		encoder_funcs = encoder->helper_private;
		if (!encoder_funcs)
			continue;

		/* Disable unused encoders */
		if (encoder->crtc == NULL)
			drm_encoder_disable(encoder);
	}
}

/**
 * drm_crtc_helper_set_mode - 内部辅助函数：设置显示模式
 * @crtc: 要编程的 CRTC
 * @mode: 要使用的模式
 * @x: 表面的水平偏移
 * @y: 表面的垂直偏移
 * @old_fb: 旧的帧缓冲，用于清理
 *
 * 尝试在 @crtc 上设置 @mode。在尝试设置之前，给 @crtc 及其关联的连接器
 * 一个修复或拒绝模式的机会。
 *
 * 模式设置序列（按顺序）：
 *   1. 调用所有关联编码器的 mode_fixup 回调
 *   2. 调用 CRTC 的 mode_fixup 回调
 *   3. 调用所有关联编码器的 prepare 回调（准备阶段）
 *   4. 调用 CRTC 的 prepare 回调
 *   5. 调用 CRTC 的 mode_set 回调（设置 PLL 和模式参数）
 *   6. 调用所有关联编码器的 mode_set 回调
 *   7. 调用 CRTC 的 commit 回调（提交/激活）
 *   8. 调用所有关联编码器的 commit 回调
 *   9. 计算并存储 VBLANK 和交换完成时间戳所需的常量
 *
 * 如果任何步骤失败，会回滚到之前保存的状态。
 *
 * 返回值：
 * 成功返回 true，失败返回 false。
 */
bool drm_crtc_helper_set_mode(struct drm_crtc *crtc,
			      struct drm_display_mode *mode,
			      int x, int y,
			      struct drm_framebuffer *old_fb)
{
	struct drm_device *dev = crtc->dev;
	struct drm_display_mode *adjusted_mode, saved_mode, saved_hwmode;
	const struct drm_crtc_helper_funcs *crtc_funcs = crtc->helper_private;
	const struct drm_encoder_helper_funcs *encoder_funcs;
	int saved_x, saved_y;
	bool saved_enabled;
	struct drm_encoder *encoder;
	bool ret = true;

	drm_WARN_ON(dev, drm_drv_uses_atomic_modeset(dev));

	drm_warn_on_modeset_not_all_locked(dev);

	saved_enabled = crtc->enabled;
	crtc->enabled = drm_helper_crtc_in_use(crtc);
	if (!crtc->enabled)
		return true;

	adjusted_mode = drm_mode_duplicate(dev, mode);
	if (!adjusted_mode) {
		crtc->enabled = saved_enabled;
		return false;
	}

	drm_mode_init(&saved_mode, &crtc->mode);
	drm_mode_init(&saved_hwmode, &crtc->hwmode);
	saved_x = crtc->x;
	saved_y = crtc->y;

	/* Update crtc values up front so the driver can rely on them for mode
	 * setting.
	 */
	drm_mode_copy(&crtc->mode, mode);
	crtc->x = x;
	crtc->y = y;

	/* Pass our mode to the connectors and the CRTC to give them a chance to
	 * adjust it according to limitations or connector properties, and also
	 * a chance to reject the mode entirely.
	 */
	drm_for_each_encoder(encoder, dev) {

		if (encoder->crtc != crtc)
			continue;

		encoder_funcs = encoder->helper_private;
		if (!encoder_funcs)
			continue;

		if (encoder_funcs->mode_fixup) {
			if (!(ret = encoder_funcs->mode_fixup(encoder, mode,
							      adjusted_mode))) {
				drm_dbg_kms(dev, "[ENCODER:%d:%s] mode fixup failed\n",
					    encoder->base.id, encoder->name);
				goto done;
			}
		}
	}

	if (crtc_funcs->mode_fixup) {
		if (!(ret = crtc_funcs->mode_fixup(crtc, mode,
						adjusted_mode))) {
			drm_dbg_kms(dev, "[CRTC:%d:%s] mode fixup failed\n",
				    crtc->base.id, crtc->name);
			goto done;
		}
	}
	drm_dbg_kms(dev, "[CRTC:%d:%s]\n", crtc->base.id, crtc->name);

	drm_mode_copy(&crtc->hwmode, adjusted_mode);

	/* Prepare the encoders and CRTCs before setting the mode. */
	drm_for_each_encoder(encoder, dev) {

		if (encoder->crtc != crtc)
			continue;

		encoder_funcs = encoder->helper_private;
		if (!encoder_funcs)
			continue;

		/* Disable the encoders as the first thing we do. */
		if (encoder_funcs->prepare)
			encoder_funcs->prepare(encoder);
	}

	drm_crtc_prepare_encoders(dev);

	crtc_funcs->prepare(crtc);

	/* Set up the DPLL and any encoders state that needs to adjust or depend
	 * on the DPLL.
	 */
	ret = !crtc_funcs->mode_set(crtc, mode, adjusted_mode, x, y, old_fb);
	if (!ret)
	    goto done;

	drm_for_each_encoder(encoder, dev) {

		if (encoder->crtc != crtc)
			continue;

		encoder_funcs = encoder->helper_private;
		if (!encoder_funcs)
			continue;

		drm_dbg_kms(dev, "[ENCODER:%d:%s] set [MODE:%s]\n",
			    encoder->base.id, encoder->name, mode->name);
		if (encoder_funcs->mode_set)
			encoder_funcs->mode_set(encoder, mode, adjusted_mode);
	}

	/* Now enable the clocks, plane, pipe, and connectors that we set up. */
	crtc_funcs->commit(crtc);

	drm_for_each_encoder(encoder, dev) {

		if (encoder->crtc != crtc)
			continue;

		encoder_funcs = encoder->helper_private;
		if (!encoder_funcs)
			continue;

		if (encoder_funcs->commit)
			encoder_funcs->commit(encoder);
	}

	/* Calculate and store various constants which
	 * are later needed by vblank and swap-completion
	 * timestamping. They are derived from true hwmode.
	 */
	drm_calc_timestamping_constants(crtc, &crtc->hwmode);

	/* FIXME: add subpixel order */
done:
	drm_mode_destroy(dev, adjusted_mode);
	if (!ret) {
		crtc->enabled = saved_enabled;
		drm_mode_copy(&crtc->mode, &saved_mode);
		drm_mode_copy(&crtc->hwmode, &saved_hwmode);
		crtc->x = saved_x;
		crtc->y = saved_y;
	}

	return ret;
}
EXPORT_SYMBOL(drm_crtc_helper_set_mode);

/**
 * drm_crtc_helper_atomic_check() - 检查 CRTC 原子状态的辅助函数
 * @crtc: 要检查的 CRTC
 * @state: 原子状态对象
 *
 * 为只连接了一个主平面的 CRTC 提供默认的 CRTC 状态检查处理函数。
 * 这是简单帧缓冲 CRTC 的常见情况。
 *
 * 如果 CRTC 已启用，此函数会检查是否配置了有效的主平面，
 * 确保主平面有有效的帧缓冲。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_crtc_helper_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!new_crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(new_crtc_state);
}
EXPORT_SYMBOL(drm_crtc_helper_atomic_check);

static void
drm_crtc_helper_disable(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_connector *connector;
	struct drm_encoder *encoder;

	/* Decouple all encoders and their attached connectors from this crtc */
	drm_for_each_encoder(encoder, dev) {
		struct drm_connector_list_iter conn_iter;

		if (encoder->crtc != crtc)
			continue;

		drm_connector_list_iter_begin(dev, &conn_iter);
		drm_for_each_connector_iter(connector, &conn_iter) {
			if (connector->encoder != encoder)
				continue;

			connector->encoder = NULL;

			/*
			 * drm_helper_disable_unused_functions() ought to be
			 * doing this, but since we've decoupled the encoder
			 * from the connector above, the required connection
			 * between them is henceforth no longer available.
			 */
			connector->dpms = DRM_MODE_DPMS_OFF;

			/* we keep a reference while the encoder is bound */
			drm_connector_put(connector);
		}
		drm_connector_list_iter_end(&conn_iter);
	}

	__drm_helper_disable_unused_functions(dev);
}

/*
 * For connectors that support multiple encoders, either the
 * .atomic_best_encoder() or .best_encoder() operation must be implemented.
 */
struct drm_encoder *
drm_connector_get_single_encoder(struct drm_connector *connector)
{
	struct drm_encoder *encoder;

	drm_WARN_ON(connector->dev, hweight32(connector->possible_encoders) > 1);
	drm_connector_for_each_possible_encoder(connector, encoder)
		return encoder;

	return NULL;
}

/**
 * drm_crtc_helper_set_config - 从用户空间设置新配置
 * @set: 模式设置配置
 * @ctx: 锁获取上下文（此处未使用）
 *
 * 为使用传统 CRTC 辅助函数的驱动实现 &drm_crtc_funcs.set_config 回调。
 *
 * 执行流程：
 *   1. 为每个连接器选择最佳编码器（通过 best_encoder 回调）
 *   2. 调用 mode_fixup 回调调整或拒绝请求的模式
 *   3. 如果新模式与当前模式相同但帧缓冲不同，调用 mode_set_base
 *   4. 如果新模式与当前模式不同，执行完整的模式设置序列
 *      （prepare -> mode_set -> commit）
 *   5. 禁用所有未使用的函数
 *   6. 如果任何步骤失败，回滚到之前保存的配置
 *
 * 此函数已废弃。新的驱动必须实现原子模式设置支持，
 * 应使用 drm_atomic_helper_set_config() 替代。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_crtc_helper_set_config(struct drm_mode_set *set,
			       struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *dev;
	struct drm_crtc **save_encoder_crtcs, *new_crtc;
	struct drm_encoder **save_connector_encoders, *new_encoder, *encoder;
	bool mode_changed = false; /* if true do a full mode set */
	bool fb_changed = false; /* if true and !mode_changed just do a flip */
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	int count = 0, ro, fail = 0;
	const struct drm_crtc_helper_funcs *crtc_funcs;
	struct drm_mode_set save_set;
	int ret;
	int i;

	BUG_ON(!set);
	BUG_ON(!set->crtc);
	BUG_ON(!set->crtc->helper_private);

	/* Enforce sane interface api - has been abused by the fb helper. */
	BUG_ON(!set->mode && set->fb);
	BUG_ON(set->fb && set->num_connectors == 0);

	crtc_funcs = set->crtc->helper_private;

	dev = set->crtc->dev;

	drm_dbg_kms(dev, "\n");

	drm_WARN_ON(dev, drm_drv_uses_atomic_modeset(dev));

	if (!set->mode)
		set->fb = NULL;

	if (set->fb) {
		drm_dbg_kms(dev, "[CRTC:%d:%s] [FB:%d] #connectors=%d (x y) (%i %i)\n",
			    set->crtc->base.id, set->crtc->name,
			    set->fb->base.id,
			    (int)set->num_connectors, set->x, set->y);
	} else {
		drm_dbg_kms(dev, "[CRTC:%d:%s] [NOFB]\n",
			    set->crtc->base.id, set->crtc->name);
		drm_crtc_helper_disable(set->crtc);
		return 0;
	}

	drm_warn_on_modeset_not_all_locked(dev);

	/*
	 * Allocate space for the backup of all (non-pointer) encoder and
	 * connector data.
	 */
	save_encoder_crtcs = kzalloc_objs(struct drm_crtc *,
					  dev->mode_config.num_encoder);
	if (!save_encoder_crtcs)
		return -ENOMEM;

	save_connector_encoders = kzalloc_objs(struct drm_encoder *,
					       dev->mode_config.num_connector);
	if (!save_connector_encoders) {
		kfree(save_encoder_crtcs);
		return -ENOMEM;
	}

	/*
	 * Copy data. Note that driver private data is not affected.
	 * Should anything bad happen only the expected state is
	 * restored, not the drivers personal bookkeeping.
	 */
	count = 0;
	drm_for_each_encoder(encoder, dev) {
		save_encoder_crtcs[count++] = encoder->crtc;
	}

	count = 0;
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter)
		save_connector_encoders[count++] = connector->encoder;
	drm_connector_list_iter_end(&conn_iter);

	save_set.crtc = set->crtc;
	save_set.mode = &set->crtc->mode;
	save_set.x = set->crtc->x;
	save_set.y = set->crtc->y;
	save_set.fb = set->crtc->primary->fb;

	/* We should be able to check here if the fb has the same properties
	 * and then just flip_or_move it */
	if (set->crtc->primary->fb != set->fb) {
		/* If we have no fb then treat it as a full mode set */
		if (set->crtc->primary->fb == NULL) {
			drm_dbg_kms(dev, "[CRTC:%d:%s] no fb, full mode set\n",
				    set->crtc->base.id, set->crtc->name);
			mode_changed = true;
		} else if (set->fb->format != set->crtc->primary->fb->format) {
			mode_changed = true;
		} else
			fb_changed = true;
	}

	if (set->x != set->crtc->x || set->y != set->crtc->y)
		fb_changed = true;

	if (!drm_mode_equal(set->mode, &set->crtc->mode)) {
		drm_dbg_kms(dev, "[CRTC:%d:%s] modes are different, full mode set:\n",
			    set->crtc->base.id, set->crtc->name);
		drm_dbg_kms(dev, DRM_MODE_FMT "\n", DRM_MODE_ARG(&set->crtc->mode));
		drm_dbg_kms(dev, DRM_MODE_FMT "\n", DRM_MODE_ARG(set->mode));
		mode_changed = true;
	}

	/* take a reference on all unbound connectors in set, reuse the
	 * already taken reference for bound connectors
	 */
	for (ro = 0; ro < set->num_connectors; ro++) {
		if (set->connectors[ro]->encoder)
			continue;
		drm_connector_get(set->connectors[ro]);
	}

	/* a) traverse passed in connector list and get encoders for them */
	count = 0;
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		const struct drm_connector_helper_funcs *connector_funcs =
			connector->helper_private;
		new_encoder = connector->encoder;
		for (ro = 0; ro < set->num_connectors; ro++) {
			if (set->connectors[ro] == connector) {
				if (connector_funcs->best_encoder)
					new_encoder = connector_funcs->best_encoder(connector);
				else
					new_encoder = drm_connector_get_single_encoder(connector);

				/* if we can't get an encoder for a connector
				   we are setting now - then fail */
				if (new_encoder == NULL)
					/* don't break so fail path works correct */
					fail = 1;

				if (connector->dpms != DRM_MODE_DPMS_ON) {
					drm_dbg_kms(dev, "[CONNECTOR:%d:%s] DPMS not on, full mode switch\n",
						    connector->base.id, connector->name);
					mode_changed = true;
				}

				break;
			}
		}

		if (new_encoder != connector->encoder) {
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] encoder changed, full mode switch\n",
				    connector->base.id, connector->name);
			mode_changed = true;
			/* If the encoder is reused for another connector, then
			 * the appropriate crtc will be set later.
			 */
			if (connector->encoder)
				connector->encoder->crtc = NULL;
			connector->encoder = new_encoder;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	if (fail) {
		ret = -EINVAL;
		goto fail;
	}

	count = 0;
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		if (!connector->encoder)
			continue;

		if (connector->encoder->crtc == set->crtc)
			new_crtc = NULL;
		else
			new_crtc = connector->encoder->crtc;

		for (ro = 0; ro < set->num_connectors; ro++) {
			if (set->connectors[ro] == connector)
				new_crtc = set->crtc;
		}

		/* Make sure the new CRTC will work with the encoder */
		if (new_crtc &&
		    !drm_encoder_crtc_ok(connector->encoder, new_crtc)) {
			ret = -EINVAL;
			drm_connector_list_iter_end(&conn_iter);
			goto fail;
		}
		if (new_crtc != connector->encoder->crtc) {
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] CRTC changed, full mode switch\n",
				    connector->base.id, connector->name);
			mode_changed = true;
			connector->encoder->crtc = new_crtc;
		}
		if (new_crtc) {
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] to [CRTC:%d:%s]\n",
				    connector->base.id, connector->name,
				    new_crtc->base.id, new_crtc->name);
		} else {
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] to [NOCRTC]\n",
				    connector->base.id, connector->name);
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	/* mode_set_base is not a required function */
	if (fb_changed && !crtc_funcs->mode_set_base)
		mode_changed = true;

	if (mode_changed) {
		if (drm_helper_crtc_in_use(set->crtc)) {
			drm_dbg_kms(dev, "[CRTC:%d:%s] attempting to set mode from userspace: " DRM_MODE_FMT "\n",
				    set->crtc->base.id, set->crtc->name, DRM_MODE_ARG(set->mode));
			set->crtc->primary->fb = set->fb;
			if (!drm_crtc_helper_set_mode(set->crtc, set->mode,
						      set->x, set->y,
						      save_set.fb)) {
				drm_err(dev, "[CRTC:%d:%s] failed to set mode\n",
					set->crtc->base.id, set->crtc->name);
				set->crtc->primary->fb = save_set.fb;
				ret = -EINVAL;
				goto fail;
			}
			drm_dbg_kms(dev, "[CRTC:%d:%s] Setting connector DPMS state to on\n",
				    set->crtc->base.id, set->crtc->name);
			for (i = 0; i < set->num_connectors; i++) {
				drm_dbg_kms(dev, "\t[CONNECTOR:%d:%s] set DPMS on\n", set->connectors[i]->base.id,
					    set->connectors[i]->name);
				set->connectors[i]->funcs->dpms(set->connectors[i], DRM_MODE_DPMS_ON);
			}
		}
		__drm_helper_disable_unused_functions(dev);
	} else if (fb_changed) {
		set->crtc->x = set->x;
		set->crtc->y = set->y;
		set->crtc->primary->fb = set->fb;
		ret = crtc_funcs->mode_set_base(set->crtc,
						set->x, set->y, save_set.fb);
		if (ret != 0) {
			set->crtc->x = save_set.x;
			set->crtc->y = save_set.y;
			set->crtc->primary->fb = save_set.fb;
			goto fail;
		}
	}

	kfree(save_connector_encoders);
	kfree(save_encoder_crtcs);
	return 0;

fail:
	/* Restore all previous data. */
	count = 0;
	drm_for_each_encoder(encoder, dev) {
		encoder->crtc = save_encoder_crtcs[count++];
	}

	count = 0;
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter)
		connector->encoder = save_connector_encoders[count++];
	drm_connector_list_iter_end(&conn_iter);

	/* after fail drop reference on all unbound connectors in set, let
	 * bound connectors keep their reference
	 */
	for (ro = 0; ro < set->num_connectors; ro++) {
		if (set->connectors[ro]->encoder)
			continue;
		drm_connector_put(set->connectors[ro]);
	}

	/* Try to restore the config */
	if (mode_changed &&
	    !drm_crtc_helper_set_mode(save_set.crtc, save_set.mode, save_set.x,
				      save_set.y, save_set.fb))
		drm_err(dev, "failed to restore config after modeset failure\n");

	kfree(save_connector_encoders);
	kfree(save_encoder_crtcs);
	return ret;
}
EXPORT_SYMBOL(drm_crtc_helper_set_config);

static int drm_helper_choose_encoder_dpms(struct drm_encoder *encoder)
{
	int dpms = DRM_MODE_DPMS_OFF;
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	struct drm_device *dev = encoder->dev;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter)
		if (connector->encoder == encoder)
			if (connector->dpms < dpms)
				dpms = connector->dpms;
	drm_connector_list_iter_end(&conn_iter);

	return dpms;
}

/* Helper which handles bridge ordering around encoder dpms */
static void drm_helper_encoder_dpms(struct drm_encoder *encoder, int mode)
{
	const struct drm_encoder_helper_funcs *encoder_funcs;

	encoder_funcs = encoder->helper_private;
	if (!encoder_funcs)
		return;

	if (encoder_funcs->dpms)
		encoder_funcs->dpms(encoder, mode);
}

static int drm_helper_choose_crtc_dpms(struct drm_crtc *crtc)
{
	int dpms = DRM_MODE_DPMS_OFF;
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	struct drm_device *dev = crtc->dev;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter)
		if (connector->encoder && connector->encoder->crtc == crtc)
			if (connector->dpms < dpms)
				dpms = connector->dpms;
	drm_connector_list_iter_end(&conn_iter);

	return dpms;
}

/**
 * drm_helper_connector_dpms() - 连接器 DPMS 辅助实现
 * @connector: 受影响的连接器
 * @mode: DPMS 模式
 *
 * 为使用传统 CRTC 辅助函数的驱动实现 &drm_connector_funcs.dpms 回调。
 *
 * 这是 CRTC 辅助框架提供的主要 DPMS 辅助函数。它计算输出网格中所有
 * 编码器和 CRTC 的所需 DPMS 状态，然后调用驱动提供的
 * &drm_crtc_helper_funcs.dpms 和 &drm_encoder_helper_funcs.dpms 回调。
 *
 * 转换顺序：
 *   - 从关到开（mode < old_dpms）：先 CRTC 后编码器
 *   - 从开到关（mode > old_dpms）：先编码器后 CRTC
 *   这种顺序确保了正确的电源序列。
 *
 * 此函数已废弃。新的驱动必须实现原子模式设置支持，
 * 在原子框架中 DPMS 由 DRM 核心直接处理。
 *
 * 返回值：
 * 始终返回 0。
 */
int drm_helper_connector_dpms(struct drm_connector *connector, int mode)
{
	struct drm_encoder *encoder = connector->encoder;
	struct drm_crtc *crtc = encoder ? encoder->crtc : NULL;
	int old_dpms, encoder_dpms = DRM_MODE_DPMS_OFF;

	drm_WARN_ON(connector->dev, drm_drv_uses_atomic_modeset(connector->dev));

	if (mode == connector->dpms)
		return 0;

	old_dpms = connector->dpms;
	connector->dpms = mode;

	if (encoder)
		encoder_dpms = drm_helper_choose_encoder_dpms(encoder);

	/* from off to on, do crtc then encoder */
	if (mode < old_dpms) {
		if (crtc) {
			const struct drm_crtc_helper_funcs *crtc_funcs = crtc->helper_private;

			if (crtc_funcs->dpms)
				(*crtc_funcs->dpms) (crtc,
						     drm_helper_choose_crtc_dpms(crtc));
		}
		if (encoder)
			drm_helper_encoder_dpms(encoder, encoder_dpms);
	}

	/* from on to off, do encoder then crtc */
	if (mode > old_dpms) {
		if (encoder)
			drm_helper_encoder_dpms(encoder, encoder_dpms);
		if (crtc) {
			const struct drm_crtc_helper_funcs *crtc_funcs = crtc->helper_private;

			if (crtc_funcs->dpms)
				(*crtc_funcs->dpms) (crtc,
						     drm_helper_choose_crtc_dpms(crtc));
		}
	}

	return 0;
}
EXPORT_SYMBOL(drm_helper_connector_dpms);

/**
 * drm_helper_resume_force_mode - 强制恢复模式设置配置
 * @dev: 要恢复的 DRM 设备
 *
 * 使用模式设置辅助函数的驱动可以使用此函数强制恢复模式设置配置，
 * 例如在恢复（resume）时，或者当其他因素可能破坏硬件状态时
 * （如某些过于激进的旧 BIOS）。
 *
 * 此辅助函数不提供错误返回值，因为恢复旧的配置因为资源分配问题
 * 而失败的情况不应该发生——驱动之前已经成功设置了这些配置。
 * 因此这基本上等同于几次 dpms 开启调用。
 *
 * 如果只是简单恢复旧的配置可能会导致失败（例如在恢复时因顺序不同
 * 导致共享资源分配的微小差异），驱动需要使用自己的恢复逻辑。
 *
 * 此函数已废弃。新的驱动应实现原子模式设置并使用原子的
 * suspend/resume 辅助函数。
 *
 * 另请参见：
 * drm_atomic_helper_suspend(), drm_atomic_helper_resume()
 */
void drm_helper_resume_force_mode(struct drm_device *dev)
{
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	const struct drm_crtc_helper_funcs *crtc_funcs;
	int encoder_dpms;
	bool ret;

	drm_WARN_ON(dev, drm_drv_uses_atomic_modeset(dev));

	drm_modeset_lock_all(dev);
	drm_for_each_crtc(crtc, dev) {

		if (!crtc->enabled)
			continue;

		ret = drm_crtc_helper_set_mode(crtc, &crtc->mode,
					       crtc->x, crtc->y, crtc->primary->fb);

		/* Restoring the old config should never fail! */
		if (ret == false)
			drm_err(dev, "failed to set mode on crtc %p\n", crtc);

		/* Turn off outputs that were already powered off */
		if (drm_helper_choose_crtc_dpms(crtc)) {
			drm_for_each_encoder(encoder, dev) {

				if(encoder->crtc != crtc)
					continue;

				encoder_dpms = drm_helper_choose_encoder_dpms(
							encoder);

				drm_helper_encoder_dpms(encoder, encoder_dpms);
			}

			crtc_funcs = crtc->helper_private;
			if (crtc_funcs->dpms)
				(*crtc_funcs->dpms) (crtc,
						     drm_helper_choose_crtc_dpms(crtc));
		}
	}

	/* disable the unused connectors while restoring the modesetting */
	__drm_helper_disable_unused_functions(dev);
	drm_modeset_unlock_all(dev);
}
EXPORT_SYMBOL(drm_helper_resume_force_mode);

/**
 * drm_helper_force_disable_all - 强制关闭所有已启用的 CRTC
 * @dev: 要关闭 CRTC 的 DRM 设备
 *
 * 驱动可以在卸载时调用此函数，以确保所有显示器关闭，
 * GPU 处于一致的、低功耗状态。会获取模式设置锁。
 *
 * 注意：此函数仅适用于非原子的传统驱动。
 * 对于原子版本，请查看 drm_atomic_helper_shutdown()。
 *
 * 返回值：
 * 成功返回 0，失败返回错误码。
 */
int drm_helper_force_disable_all(struct drm_device *dev)
{
	struct drm_crtc *crtc;
	int ret = 0;

	drm_modeset_lock_all(dev);
	drm_for_each_crtc(crtc, dev)
		if (crtc->enabled) {
			struct drm_mode_set set = {
				.crtc = crtc,
			};

			ret = drm_mode_set_config_internal(&set);
			if (ret)
				goto out;
		}
out:
	drm_modeset_unlock_all(dev);
	return ret;
}
EXPORT_SYMBOL(drm_helper_force_disable_all);
