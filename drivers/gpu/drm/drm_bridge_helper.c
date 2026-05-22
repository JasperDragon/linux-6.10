// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * DRM 桥接辅助函数
 *
 * 本文件提供 DRM 桥接（bridge）相关的辅助函数。在 DRM 显示子系统中，
 * 桥接是一种连接显示控制器和显示设备的抽象层，通常用于表示外部显示
 * 转换芯片（如 HDMI 转换器、DP 转换器等）或特殊显示路径。
 *
 * 桥接模型允许将显示管道分解为多个可组合的组件，每个组件可以独立地
 * 进行配置和管理。这种设计提高了代码的复用性，使得不同的显示控制器
 * 和显示设备可以灵活组合。
 *
 * 当前本文件提供了重置桥接管道的辅助函数，用于在需要时对整个显示
 * 管道进行电源周期重置操作。
 */

#include <linux/export.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_helper.h>
#include <drm/drm_modeset_lock.h>

/**
 * drm_bridge_helper_reset_crtc - 重置桥接器对应的显示管道
 * @bridge: 要重置的 DRM 桥接器
 * @ctx: 锁获取上下文
 *
 * 该函数用于重置指定桥接器对应的整个显示管道。它将电源周期循环所有
 * 从 CRTC 到连接器之间桥接器所连接的活跃组件。
 *
 * 该函数依赖于 drm_atomic_helper_reset_crtc()，因此相同的限制条件
 * 同样适用。
 *
 * 返回：
 * 成功返回 0，失败返回负错误码。如果返回 EDEADLK，则需要重启整个
 * 原子操作序列。
 *
 * 使用场景：
 * 当显示硬件出现异常或需要重新初始化显示管道时，可以调用此函数
 * 来执行完整的重置操作，确保所有组件重新进入已知的良好状态。
 */
int drm_bridge_helper_reset_crtc(struct drm_bridge *bridge,
				 struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_connector *connector;
	struct drm_encoder *encoder = bridge->encoder;
	struct drm_device *dev = encoder->dev;
	struct drm_crtc *crtc;
	int ret;

	ret = drm_modeset_lock(&dev->mode_config.connection_mutex, ctx);
	if (ret)
		return ret;

	connector = drm_atomic_get_connector_for_encoder(encoder, ctx);
	if (IS_ERR(connector)) {
		ret = PTR_ERR(connector);
		goto out;
	}

	if (!connector->state) {
		ret = -EINVAL;
		goto out;
	}

	crtc = connector->state->crtc;
	ret = drm_atomic_helper_reset_crtc(crtc, ctx);
	if (ret)
		goto out;

out:
	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	return ret;
}
EXPORT_SYMBOL(drm_bridge_helper_reset_crtc);
