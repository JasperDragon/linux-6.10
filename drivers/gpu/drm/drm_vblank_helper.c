// SPDX-License-Identifier: MIT

/*
 * 文件名: drm_vblank_helper.c
 *
 * 中文描述: VBlank（垂直消隐期）辅助函数库
 *
 * 本文件为 DRM 驱动程序提供了 VBlank 相关的辅助回调函数实现。VBlank 是显示设备
 * 在帧与帧之间切换时的垂直消隐期，驱动程序常在此时间段内执行安全的状态更新。
 *
 * 主要功能包括：
 *   1. Atomic Flush 回调 - 在 atomic 事务提交时发送 VBlank 事件
 *   2. Atomic Enable/Disable 回调 - 在 CRTC 启用/禁用时控制 VBlank
 *   3. VBlank 定时器控制 - 基于定时器的 VBlank 启用/禁用实现
 *   4. VBlank 时间戳获取 - 基于定时器超时时间的 VBlank 时间戳
 *
 * 驱动程序可通过 DRM_CRTC_VBLANK_TIMER_FUNCS 和 DRM_CRTC_HELPER_VBLANK_FUNCS
 * 宏来便捷地初始化这些回调函数。
 */

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

/**
 * DOC: overview
 *
 * The vblank helper library provides functions for supporting vertical
 * blanking in DRM drivers.
 *
 * For vblank timers, several callback implementations are available.
 * Drivers enable support for vblank timers by setting the vblank callbacks
 * in struct &drm_crtc_funcs to the helpers provided by this library. The
 * initializer macro DRM_CRTC_VBLANK_TIMER_FUNCS does this conveniently.
 * The driver further has to send the VBLANK event from its atomic_flush
 * callback and control vblank from the CRTC's atomic_enable and atomic_disable
 * callbacks. The callbacks are located in struct &drm_crtc_helper_funcs.
 * The vblank helper library provides implementations of these callbacks
 * for drivers without further requirements. The initializer macro
 * DRM_CRTC_HELPER_VBLANK_FUNCS sets them coveniently.
 *
 * Once the driver enables vblank support with drm_vblank_init(), each
 * CRTC's vblank timer fires according to the programmed display mode. By
 * default, the vblank timer invokes drm_crtc_handle_vblank(). Drivers with
 * more specific requirements can set their own handler function in
 * struct &drm_crtc_helper_funcs.handle_vblank_timeout.
 */

/*
 * VBLANK helpers
 */

/**
 * drm_crtc_vblank_atomic_flush - CRTC atomic_flush 回调实现
 *
 * 在 atomic 事务的 flush 阶段触发。此函数获取 CRTC 状态中的待发送
 * VBlank 事件，并尝试将其装载（arm）到下一个 VBlank 中断时发送。
 * 如果 VBlank 不可用（如 CRTC 已关闭），则立即发送事件。
 *
 * 这是 drm_crtc_helper_funcs.atomic_flush 的默认实现，适用于只需要
 * 在 atomic flush 时发送 VBlank 事件的 CRTC。
 */
 * @crtc: The CRTC
 * @state: The atomic state to apply
 *
 * The helper drm_crtc_vblank_atomic_flush() implements atomic_flush of
 * struct drm_crtc_helper_funcs for CRTCs that only need to send out a
 * VBLANK event.
 *
 * See also struct &drm_crtc_helper_funcs.atomic_flush.
 */
void drm_crtc_vblank_atomic_flush(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct drm_device *dev = crtc->dev;
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event;

	spin_lock_irq(&dev->event_lock);

	event = crtc_state->event;
	crtc_state->event = NULL;

	if (event) {
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
	}

	spin_unlock_irq(&dev->event_lock);
}
EXPORT_SYMBOL(drm_crtc_vblank_atomic_flush);

/**
 * drm_crtc_vblank_atomic_enable - CRTC atomic_enable 回调实现
 *
 * 在 CRTC 启用时调用，使能 VBlank 中断。这是 drm_crtc_helper_funcs.
 * atomic_enable 的默认实现，适用于只需要在 CRTC 启用时开启 VBlank 的驱动。
 *
 * 内部调用 drm_crtc_vblank_on()，该函数会启用 VBlank 计数并开始
 * 产生 VBlank 中断事件。
 */
 * @crtc: The CRTC
 * @state: The atomic state
 *
 * The helper drm_crtc_vblank_atomic_enable() implements atomic_enable
 * of struct drm_crtc_helper_funcs for CRTCs the only need to enable VBLANKs.
 *
 * See also struct &drm_crtc_helper_funcs.atomic_enable.
 */
void drm_crtc_vblank_atomic_enable(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	drm_crtc_vblank_on(crtc);
}
EXPORT_SYMBOL(drm_crtc_vblank_atomic_enable);

/**
 * drm_crtc_vblank_atomic_disable - CRTC atomic_disable 回调实现
 *
 * 在 CRTC 禁用时调用，关闭 VBlank 中断。这是 drm_crtc_helper_funcs.
 * atomic_disable 的默认实现，适用于只需要在 CRTC 禁用时关闭 VBlank 的驱动。
 *
 * 内部调用 drm_crtc_vblank_off()，该函数会立即关闭 VBlank 并
 * 等待任何正在进行的 VBlank 操作完成。
 */
 * @crtc: The CRTC
 * @state: The atomic state
 *
 * The helper drm_crtc_vblank_atomic_disable() implements atomic_disable
 * of struct drm_crtc_helper_funcs for CRTCs the only need to disable VBLANKs.
 *
 * See also struct &drm_crtc_funcs.atomic_disable.
 */
void drm_crtc_vblank_atomic_disable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	drm_crtc_vblank_off(crtc);
}
EXPORT_SYMBOL(drm_crtc_vblank_atomic_disable);

/*
 * VBLANK timer
 */

/**
 * drm_crtc_vblank_helper_enable_vblank_timer - 基于定时器的 VBlank 启用回调
 *
 * 为需要 VBlank 定时器的 CRTC 实现 enable_vblank 回调。
 * 首次调用时设置定时器，定时器在当前帧时长到期后触发。
 *
 * 适用于那些没有硬件 VBlank 中断，需要通过定时器模拟 VBlank 的驱动。
 */
 * @crtc: The CRTC
 *
 * The helper drm_crtc_vblank_helper_enable_vblank_timer() implements
 * enable_vblank of struct drm_crtc_helper_funcs for CRTCs that require
 * a VBLANK timer. It sets up the timer on the first invocation. The
 * started timer expires after the current frame duration. See struct
 * &drm_vblank_crtc.framedur_ns.
 *
 * See also struct &drm_crtc_helper_funcs.enable_vblank.
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_crtc_vblank_helper_enable_vblank_timer(struct drm_crtc *crtc)
{
	return drm_crtc_vblank_start_timer(crtc);
}
EXPORT_SYMBOL(drm_crtc_vblank_helper_enable_vblank_timer);

/**
 * drm_crtc_vblank_helper_disable_vblank_timer - 基于定时器的 VBlank 禁用回调
 *
 * 为需要 VBlank 定时器的 CRTC 实现 disable_vblank 回调。
 * 取消正在运行的 VBlank 定时器。
 */
 * @crtc: The CRTC
 *
 * The helper drm_crtc_vblank_helper_disable_vblank_timer() implements
 * disable_vblank of struct drm_crtc_funcs for CRTCs that require a
 * VBLANK timer.
 *
 * See also struct &drm_crtc_helper_funcs.disable_vblank.
 */
void drm_crtc_vblank_helper_disable_vblank_timer(struct drm_crtc *crtc)
{
	drm_crtc_vblank_cancel_timer(crtc);
}
EXPORT_SYMBOL(drm_crtc_vblank_helper_disable_vblank_timer);

/**
 * drm_crtc_vblank_helper_get_vblank_timestamp_from_timer - 基于定时器的 VBlank 时间戳获取
 *
 * 为需要 VBlank 定时器的 CRTC 实现 get_vblank_timestamp 回调。
 * 根据定时器的超时时间返回 VBlank 时间戳。
 *
 * 当硬件无法提供精确的 VBlank 时间戳时，此函数提供一个合理的
 * 时间估计值。
 */
 * @crtc: The CRTC
 * @max_error: Maximum acceptable error
 * @vblank_time: Returns the next vblank timestamp
 * @in_vblank_irq: True is called from drm_crtc_handle_vblank()
 *
 * The helper drm_crtc_helper_get_vblank_timestamp_from_timer() implements
 * get_vblank_timestamp of struct drm_crtc_funcs for CRTCs that require a
 * VBLANK timer. It returns the timestamp according to the timer's expiry
 * time.
 *
 * See also struct &drm_crtc_funcs.get_vblank_timestamp.
 *
 * Returns:
 * True on success, or false otherwise.
 */
bool drm_crtc_vblank_helper_get_vblank_timestamp_from_timer(struct drm_crtc *crtc,
							    int *max_error,
							    ktime_t *vblank_time,
							    bool in_vblank_irq)
{
	drm_crtc_vblank_get_vblank_timeout(crtc, vblank_time);

	return true;
}
EXPORT_SYMBOL(drm_crtc_vblank_helper_get_vblank_timestamp_from_timer);
