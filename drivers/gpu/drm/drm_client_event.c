// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright 2018 Noralf Trønnes
 */

/*
 * 文件名: drm_client_event.c
 *
 * 中文描述: DRM 客户端事件处理
 *
 * 本文件实现了 DRM 客户端（drm_client）的事件管理功能。DRM 客户端是
 * DRM 子系统中的一种抽象，代表一个使用 DRM 设备的内部消费者（如 fbdev
 * 模拟层、内核显示处理器等）。
 *
 * 核心功能：
 *   1. 客户端注销 (drm_client_dev_unregister) - 注销所有客户端并释放资源
 *   2. 热插拔事件 (drm_client_dev_hotplug) - 通知客户端显示器热插拔事件
 *   3. 恢复处理 (drm_client_dev_restore) - 在系统恢复或模式设置后恢复客户端状态
 *   4. 挂起处理 (drm_client_dev_suspend) - 暂停所有客户端活动
 *   5. 恢复处理 (drm_client_dev_resume) - 恢复所有客户端活动
 *   6. DebugFS 接口 - 通过 debugfs 显示已注册的客户端列表
 *
 * 回调机制：每个客户端通过 drm_client_funcs 结构体注册 hotplug、suspend、
 * resume、restore、unregister 等回调函数。
 */

#include <linux/export.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>

#include <drm/drm_client.h>
#include <drm/drm_client_event.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

/**
 * drm_client_dev_unregister - 注销所有客户端
 * @dev: DRM 设备
 *
 * 释放所有已注册的 DRM 客户端。遍历客户列表，对每个客户端调用
 * 其 unregister 回调函数（如果已注册），否则调用 drm_client_release()
 * 进行默认释放。回调函数负责释放所有资源，包括客户端自身。
 *
 * drm_dev_unregister() 会调用此函数。使用它的驱动无需自行调用。
 */
void drm_client_dev_unregister(struct drm_device *dev)
{
	struct drm_client_dev *client, *tmp;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	mutex_lock(&dev->clientlist_mutex);
	list_for_each_entry_safe(client, tmp, &dev->clientlist, list) {
		list_del(&client->list);
		/*
		 * Unregistering consumes and frees the client.
		 */
		if (client->funcs && client->funcs->unregister)
			client->funcs->unregister(client);
		else
			drm_client_release(client);
	}
	mutex_unlock(&dev->clientlist_mutex);
}
EXPORT_SYMBOL(drm_client_dev_unregister);

/*
 * drm_client_hotplug - 向单个客户端发送热插拔事件
 * @client: DRM 客户端
 *
 * 调用客户端的热插拔回调。如果客户端处于挂起状态，则仅设置
 * hotplug_pending 标志，待恢复时再处理。如果之前的 hotplug
 * 失败，则不再重复尝试。
 */
static void drm_client_hotplug(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	int ret;

	if (!client->funcs || !client->funcs->hotplug)
		return;

	if (client->hotplug_failed)
		return;

	if (client->suspended) {
		client->hotplug_pending = true;
		return;
	}

	client->hotplug_pending = false;
	ret = client->funcs->hotplug(client);
	drm_dbg_kms(dev, "%s: ret=%d\n", client->name, ret);
	if (ret)
		client->hotplug_failed = true;
}

/**
 * drm_client_dev_hotplug - 向客户端发送热插拔事件
 * @dev: DRM 设备
 *
 * 遍历所有已注册的客户端，调用每个客户端的 hotplug 回调函数
 * 通知其显示器连接状态发生变化。对于已挂起的客户端，标记
 * hotplug_pending 标志，待恢复时再处理。
 *
 * drm_kms_helper_hotplug_event() 会调用此函数。使用它的驱动
 * 无需自行调用。
 */
void drm_client_dev_hotplug(struct drm_device *dev)
{
	struct drm_client_dev *client;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	if (!dev->mode_config.num_connector) {
		drm_dbg_kms(dev, "No connectors found, will not send hotplug events!\n");
		return;
	}

	mutex_lock(&dev->clientlist_mutex);
	list_for_each_entry(client, &dev->clientlist, list)
		drm_client_hotplug(client);
	mutex_unlock(&dev->clientlist_mutex);
}
EXPORT_SYMBOL(drm_client_dev_hotplug);

/*
 * drm_client_dev_restore - 恢复客户端显示状态
 * @dev: DRM 设备
 * @force: 是否强制恢复
 *
 * 在系统从休眠恢复或模式设置改变后调用，通知各客户端恢复显示。
 * 遍历所有客户端，调用其 restore 回调。第一个成功恢复（返回 0）
 * 的客户端获得"恢复权"，后续客户端不再执行恢复操作。
 *
 * 此函数被 drm_client_dev_restore() 或其变体所调用。
 */
void drm_client_dev_restore(struct drm_device *dev, bool force)
{
	struct drm_client_dev *client;
	int ret;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return;

	mutex_lock(&dev->clientlist_mutex);
	list_for_each_entry(client, &dev->clientlist, list) {
		if (!client->funcs || !client->funcs->restore)
			continue;

		ret = client->funcs->restore(client, force);
		drm_dbg_kms(dev, "%s: ret=%d\n", client->name, ret);
		if (!ret) /* The first one to return zero gets the privilege to restore */
			break;
	}
	mutex_unlock(&dev->clientlist_mutex);
}

/*
 * drm_client_suspend - 暂停单个客户端
 * @client: DRM 客户端
 *
 * 调用客户端的 suspend 回调并设置 suspended 标志。
 * 如果客户端已经挂起，将触发 WARN 警告并跳过。
 */
static int drm_client_suspend(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	int ret = 0;

	if (drm_WARN_ON_ONCE(dev, client->suspended))
		return 0;

	if (client->funcs && client->funcs->suspend)
		ret = client->funcs->suspend(client);
	drm_dbg_kms(dev, "%s: ret=%d\n", client->name, ret);

	client->suspended = true;

	return ret;
}

/*
 * drm_client_dev_suspend - 暂停所有客户端
 * @dev: DRM 设备
 *
 * 在系统进入睡眠前调用，遍历所有客户端并暂停它们。
 */
void drm_client_dev_suspend(struct drm_device *dev)
{
	struct drm_client_dev *client;

	mutex_lock(&dev->clientlist_mutex);
	list_for_each_entry(client, &dev->clientlist, list) {
		if (!client->suspended)
			drm_client_suspend(client);
	}
	mutex_unlock(&dev->clientlist_mutex);
}
EXPORT_SYMBOL(drm_client_dev_suspend);

/*
 * drm_client_resume - 恢复单个客户端
 * @client: DRM 客户端
 *
 * 调用客户端的 resume 回调，清除 suspended 标志。
 * 如果挂起期间有待处理的热插拔事件，立即执行热插拔处理。
 */
static int drm_client_resume(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	int ret = 0;

	if (drm_WARN_ON_ONCE(dev, !client->suspended))
		return 0;

	if (client->funcs && client->funcs->resume)
		ret = client->funcs->resume(client);
	drm_dbg_kms(dev, "%s: ret=%d\n", client->name, ret);

	client->suspended = false;

	if (client->hotplug_pending)
		drm_client_hotplug(client);

	return ret;
}

/*
 * drm_client_dev_resume - 恢复所有客户端
 * @dev: DRM 设备
 *
 * 在系统从睡眠唤醒后调用，遍历所有已挂起的客户端并恢复它们。
 */
void drm_client_dev_resume(struct drm_device *dev)
{
	struct drm_client_dev *client;

	mutex_lock(&dev->clientlist_mutex);
	list_for_each_entry(client, &dev->clientlist, list) {
		if  (client->suspended)
			drm_client_resume(client);
	}
	mutex_unlock(&dev->clientlist_mutex);
}
EXPORT_SYMBOL(drm_client_dev_resume);

#ifdef CONFIG_DEBUG_FS
static int drm_client_debugfs_internal_clients(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct drm_printer p = drm_seq_file_printer(m);
	struct drm_client_dev *client;

	mutex_lock(&dev->clientlist_mutex);
	list_for_each_entry(client, &dev->clientlist, list)
		drm_printf(&p, "%s\n", client->name);
	mutex_unlock(&dev->clientlist_mutex);

	return 0;
}

static const struct drm_debugfs_info drm_client_debugfs_list[] = {
	{ "internal_clients", drm_client_debugfs_internal_clients, 0 },
};

void drm_client_debugfs_init(struct drm_device *dev)
{
	drm_debugfs_add_files(dev, drm_client_debugfs_list,
			      ARRAY_SIZE(drm_client_debugfs_list));
}
#endif
