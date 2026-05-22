// SPDX-License-Identifier: GPL-2.0 or MIT

/*
 * 文件名: drm_client_sysrq.c
 *
 * 中文描述: DRM 客户端 SysRq 支持
 *
 * 本文件实现了通过 SysRq 快捷键（Magic SysRq）恢复 DRM 客户端显示的功能。
 * 当显示器出现显示异常或控制台被破坏时，用户可以通过 SysRq+v 组合键
 * 触发所有已注册 DRM 设备的客户端恢复操作。
 *
 * 核心功能：
 *   1. SysRq 处理器 - 通过系统工作队列异步执行客户端恢复
 *   2. drm_client_sysrq_register() - 注册 DRM 设备到 SysRq 恢复列表
 *   3. drm_client_sysrq_unregister() - 从 SysRq 恢复列表中移除设备
 *
 * 首次注册设备时同时注册 SysRq 键 'v' 的处理函数，最后一个设备注销时
 * 自动注销该 SysRq 键。
 */

#include <linux/sysrq.h>

#include <drm/drm_client_event.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

#ifdef CONFIG_MAGIC_SYSRQ
static LIST_HEAD(drm_client_sysrq_dev_list);
static DEFINE_MUTEX(drm_client_sysrq_dev_lock);

/*
 * drm_client_sysrq_restore_work_fn - SysRq 恢复工作函数
 * @ignored: 忽略的工作结构体参数
 *
 * 在工作队列上下文中执行所有已注册 DRM 设备的客户端恢复操作。
 * 跳过已经关闭电源的设备。紧急恢复模式下不进行错误报告。
 */
static void drm_client_sysrq_restore_work_fn(struct work_struct *ignored)
{
	struct drm_device *dev;

	guard(mutex)(&drm_client_sysrq_dev_lock);

	list_for_each_entry(dev, &drm_client_sysrq_dev_list, client_sysrq_list) {
		if (dev->switch_power_state == DRM_SWITCH_POWER_OFF)
			continue;

		drm_client_dev_restore(dev, true);
	}
}

static DECLARE_WORK(drm_client_sysrq_restore_work, drm_client_sysrq_restore_work_fn);

static void drm_client_sysrq_restore_handler(u8 ignored)
{
	schedule_work(&drm_client_sysrq_restore_work);
}

static const struct sysrq_key_op drm_client_sysrq_restore_op = {
	.handler = drm_client_sysrq_restore_handler,
	.help_msg = "force-fb(v)",
	.action_msg = "Restore framebuffer console",
};

/*
 * drm_client_sysrq_register - 注册 DRM 设备到 SysRq 恢复列表
 * @dev: DRM 设备
 *
 * 将 DRM 设备添加到 SysRq 恢复列表中。如果是列表中的第一个设备，
 * 同时注册 SysRq 键 'v' 的处理函数，用户可按 Alt+SysRq+v 触发恢复。
 */
void drm_client_sysrq_register(struct drm_device *dev)
{
	guard(mutex)(&drm_client_sysrq_dev_lock);

	if (list_empty(&drm_client_sysrq_dev_list))
		register_sysrq_key('v', &drm_client_sysrq_restore_op);

	list_add(&dev->client_sysrq_list, &drm_client_sysrq_dev_list);
}

/*
 * drm_client_sysrq_unregister - 从 SysRq 恢复列表移除 DRM 设备
 * @dev: DRM 设备
 *
 * 从 SysRq 恢复列表中移除 DRM 设备。如果列表变空，同时注销
 * SysRq 键 'v' 的处理函数。
 */
void drm_client_sysrq_unregister(struct drm_device *dev)
{
	guard(mutex)(&drm_client_sysrq_dev_lock);

	/* remove device from global restore list */
	if (!drm_WARN_ON(dev, list_empty(&dev->client_sysrq_list)))
		list_del(&dev->client_sysrq_list);

	/* no devices left; unregister key */
	if (list_empty(&drm_client_sysrq_dev_list))
		unregister_sysrq_key('v', &drm_client_sysrq_restore_op);
}
#endif
