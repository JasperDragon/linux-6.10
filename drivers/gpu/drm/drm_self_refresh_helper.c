// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2019 Google, Inc.
 *
 * Authors:
 * Sean Paul <seanpaul@chromium.org>
 */

/*
 * DRM 面板自刷新（Self Refresh）辅助函数
 *
 * 本文件实现了面板自刷新（SR）的辅助框架，利用 DRM 原子接口帮助驱动
 * 轻松实现面板自刷新支持。适用于在不需要更新显示内容时，让面板进入
 * 低功耗的自刷新模式（如 PSR - Panel Self Refresh）。
 *
 * 核心机制：
 *   - 驱动在加载/卸载时通过 drm_self_refresh_helper_init/cleanup 初始化和清理
 *   - 连接器将 &drm_connector_state.self_refresh_aware 设置为 true 表示支持 SR
 *   - 辅助框架监控显示活动，在空闲时自动触发进入 SR 模式
 *   - 有任何原子更新影响处于 SR 状态的管线时，SR 会自动退出
 *   - 使用 EWMA（指数加权移动平均）来计算进入/退出 SR 的延迟时间
 *
 * 在 SR 期间，驱动可以选择完全关闭 CRTC/编码器/桥接器硬件，或检查
 * &drm_crtc_state.self_refresh_active 来决定部分关闭以节省功耗。
 */

#include <linux/average.h>
#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <drm/drm_self_refresh_helper.h>

/**
 * DOC: overview
 *
 * This helper library provides an easy way for drivers to leverage the atomic
 * framework to implement panel self refresh (SR) support. Drivers are
 * responsible for initializing and cleaning up the SR helpers on load/unload
 * (see &drm_self_refresh_helper_init/&drm_self_refresh_helper_cleanup).
 * The connector is responsible for setting
 * &drm_connector_state.self_refresh_aware to true at runtime if it is SR-aware
 * (meaning it knows how to initiate self refresh on the panel).
 *
 * Once a crtc has enabled SR using &drm_self_refresh_helper_init, the
 * helpers will monitor activity and call back into the driver to enable/disable
 * SR as appropriate. The best way to think about this is that it's a DPMS
 * on/off request with &drm_crtc_state.self_refresh_active set in crtc state
 * that tells you to disable/enable SR on the panel instead of power-cycling it.
 *
 * During SR, drivers may choose to fully disable their crtc/encoder/bridge
 * hardware (in which case no driver changes are necessary), or they can inspect
 * &drm_crtc_state.self_refresh_active if they want to enter low power mode
 * without full disable (in case full disable/enable is too slow).
 *
 * SR will be deactivated if there are any atomic updates affecting the
 * pipe that is in SR mode. If a crtc is driving multiple connectors, all
 * connectors must be SR aware and all will enter/exit SR mode at the same time.
 *
 * If the crtc and connector are SR aware, but the panel connected does not
 * support it (or is otherwise unable to enter SR), the driver should fail
 * atomic_check when &drm_crtc_state.self_refresh_active is true.
 */

#define SELF_REFRESH_AVG_SEED_MS 200

DECLARE_EWMA(psr_time, 4, 4)

struct drm_self_refresh_data {
	struct drm_crtc *crtc;
	struct delayed_work entry_work;

	struct mutex avg_mutex;
	struct ewma_psr_time entry_avg_ms;
	struct ewma_psr_time exit_avg_ms;
};

static void drm_self_refresh_helper_entry_work(struct work_struct *work)
{
	struct drm_self_refresh_data *sr_data = container_of(
				to_delayed_work(work),
				struct drm_self_refresh_data, entry_work);
	struct drm_crtc *crtc = sr_data->crtc;
	struct drm_device *dev = crtc->dev;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_connector *conn;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	int i, ret = 0;

	drm_modeset_acquire_init(&ctx, 0);

	state = drm_atomic_state_alloc(dev);
	if (!state) {
		ret = -ENOMEM;
		goto out_drop_locks;
	}

retry:
	state->acquire_ctx = &ctx;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}

	if (!crtc_state->enable)
		goto out;

	ret = drm_atomic_add_affected_connectors(state, crtc);
	if (ret)
		goto out;

	for_each_new_connector_in_state(state, conn, conn_state, i) {
		if (!conn_state->self_refresh_aware)
			goto out;
	}

	crtc_state->active = false;
	crtc_state->self_refresh_active = true;

	ret = drm_atomic_commit(state);
	if (ret)
		goto out;

out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		ret = drm_modeset_backoff(&ctx);
		if (!ret)
			goto retry;
	}

	drm_atomic_state_put(state);

out_drop_locks:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}

/**
 * drm_self_refresh_helper_update_avg_times - 更新 CRTC 的自刷新平均时间
 * @state: 刚应用到硬件的状态
 * @commit_time_ms: 此次提交完成所花费的时间（毫秒）
 * @new_self_refresh_mask: 在新状态中 self_refresh_active 为 true 的 CRTC 位掩码
 *
 * 在 &drm_mode_config_funcs.atomic_commit_tail 之后调用，此函数在自刷新
 * 状态转换时更新进入/退出自刷新的平均时间。这些平均值将用于计算
 * 在显示活动结束后延迟多久才进入自刷新模式。
 *
 * 使用 EWMA（指数加权移动平均）算法来计算平均时间，能自适应地
 * 跟踪面板性能的变化。
 */
void
drm_self_refresh_helper_update_avg_times(struct drm_atomic_state *state,
					 unsigned int commit_time_ms,
					 unsigned int new_self_refresh_mask)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	int i;

	for_each_old_crtc_in_state(state, crtc, old_crtc_state, i) {
		bool new_self_refresh_active = new_self_refresh_mask & BIT(i);
		struct drm_self_refresh_data *sr_data = crtc->self_refresh_data;
		struct ewma_psr_time *time;

		if (old_crtc_state->self_refresh_active ==
		    new_self_refresh_active)
			continue;

		if (new_self_refresh_active)
			time = &sr_data->entry_avg_ms;
		else
			time = &sr_data->exit_avg_ms;

		mutex_lock(&sr_data->avg_mutex);
		ewma_psr_time_add(time, commit_time_ms);
		mutex_unlock(&sr_data->avg_mutex);
	}
}
EXPORT_SYMBOL(drm_self_refresh_helper_update_avg_times);

/**
 * drm_self_refresh_helper_alter_state - 在自刷新退出时修改原子状态
 * @state: 当前正在检查的状态
 *
 * 在 atomic check 结束时调用。此函数检查状态中与自刷新退出不兼容的标志
 * 并进行修改。虽然这看起来有点"欺骗性"（用户空间期望一件事而实际做了另一件），
 * 但为了将自刷新完全对用户空间透明，这是必要的。
 *
 * 具体操作包括：
 *   - 如果状态中有 CRTC 处于自刷新状态而请求了异步更新，则强制回退到同步模式
 *   - 对于所有不在自刷新状态的 CRTC，重新调度自刷新进入定时器
 *
 * 最后，此函数会重新调度自刷新进入的工作任务，使得在期望的延迟后
 * 可以重新进入 PSR（面板自刷新）模式。
 */
void drm_self_refresh_helper_alter_state(struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i;

	if (state->async_update || !state->allow_modeset) {
		for_each_old_crtc_in_state(state, crtc, crtc_state, i) {
			if (crtc_state->self_refresh_active) {
				state->async_update = false;
				state->allow_modeset = true;
				break;
			}
		}
	}

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct drm_self_refresh_data *sr_data;
		unsigned int delay;

		/* Don't trigger the entry timer when we're already in SR */
		if (crtc_state->self_refresh_active)
			continue;

		sr_data = crtc->self_refresh_data;
		if (!sr_data)
			continue;

		mutex_lock(&sr_data->avg_mutex);
		delay = (ewma_psr_time_read(&sr_data->entry_avg_ms) +
			 ewma_psr_time_read(&sr_data->exit_avg_ms)) * 2;
		mutex_unlock(&sr_data->avg_mutex);

		mod_delayed_work(system_percpu_wq, &sr_data->entry_work,
				 msecs_to_jiffies(delay));
	}
}
EXPORT_SYMBOL(drm_self_refresh_helper_alter_state);

/**
 * drm_self_refresh_helper_init - 初始化 CRTC 的自刷新辅助框架
 * @crtc: 支持自刷新显示器的 CRTC
 *
 * 为给定的 CRTC 初始化自刷新辅助框架。分配并初始化自刷新数据结构，
 * 包含进入/退出延迟的平均时间计算器和延迟进入工作队列。
 *
 * 初始化时会用种子值（200ms）预填充 EWMA 平均值，使平均值在一开始
 * 就是非零的合理值。随着时间的推移，平均值会收敛到真实值。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_self_refresh_helper_init(struct drm_crtc *crtc)
{
	struct drm_self_refresh_data *sr_data = crtc->self_refresh_data;

	/* Helper is already initialized */
	if (WARN_ON(sr_data))
		return -EINVAL;

	sr_data = kzalloc_obj(*sr_data);
	if (!sr_data)
		return -ENOMEM;

	INIT_DELAYED_WORK(&sr_data->entry_work,
			  drm_self_refresh_helper_entry_work);
	sr_data->crtc = crtc;
	mutex_init(&sr_data->avg_mutex);
	ewma_psr_time_init(&sr_data->entry_avg_ms);
	ewma_psr_time_init(&sr_data->exit_avg_ms);

	/*
	 * Seed the averages so they're non-zero (and sufficiently large
	 * for even poorly performing panels). As time goes on, this will be
	 * averaged out and the values will trend to their true value.
	 */
	ewma_psr_time_add(&sr_data->entry_avg_ms, SELF_REFRESH_AVG_SEED_MS);
	ewma_psr_time_add(&sr_data->exit_avg_ms, SELF_REFRESH_AVG_SEED_MS);

	crtc->self_refresh_data = sr_data;
	return 0;
}
EXPORT_SYMBOL(drm_self_refresh_helper_init);

/**
 * drm_self_refresh_helper_cleanup - 清理 CRTC 的自刷新辅助框架
 * @crtc: 要清理的 CRTC
 *
 * 取消调度尚未执行的进入自刷新工作任务，释放自刷新数据结构，
 * 并将 CRTC 的 self_refresh_data 设置为 NULL。
 * 应当与 drm_self_refresh_helper_init() 成对调用。
 */
void drm_self_refresh_helper_cleanup(struct drm_crtc *crtc)
{
	struct drm_self_refresh_data *sr_data = crtc->self_refresh_data;

	/* Helper is already uninitialized */
	if (!sr_data)
		return;

	crtc->self_refresh_data = NULL;

	cancel_delayed_work_sync(&sr_data->entry_work);
	kfree(sr_data);
}
EXPORT_SYMBOL(drm_self_refresh_helper_cleanup);
