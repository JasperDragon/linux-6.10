// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2020 - 2021 Red Hat, Inc.
 *
 * Authors:
 * Hans de Goede <hdegoede@redhat.com>
 */

/*
 * DRM 隐私屏幕（Privacy Screen）核心
 *
 * 本文件实现了隐私屏幕功能的 DRM 核心支持。隐私屏幕是一种特殊
 * 的显示功能，允许用户在屏幕上覆盖一层可切换的过滤层，限制屏幕
 * 的可视角度，防止旁人窥视屏幕内容，从而保护用户隐私。
 *
 * 架构设计：
 *   本模块采用三层架构设计：
 *     1. 机器特定层（drm_privacy_screen_machine.h）：非 KMS 驱动
 *        （如 platform/x86 驱动）通过此接口注册隐私屏幕设备
 *     2. 消费者层（drm_privacy_screen_consumer.h）：KMS 驱动通过
 *        此接口获取和使用隐私屏幕功能
 *     3. 驱动层（drm_privacy_screen_driver.h）：实际硬件驱动的
 *        注册接口
 *
 * 与 DRM 连接器属性的集成：
 *   KMS 驱动应使用 drm_connector_attach_privacy_screen_provider()
 *   和 drm_connector_update_privacy_screen() 辅助函数来实现标准
 *   的隐私屏幕连接器属性，参见：
 *   :ref:`Standard Connector Properties<standard_connector_properties>`
 *
 * 状态管理：
 *   隐私屏幕维护两种状态：
 *     - sw_state（软件状态）：驱动期望设置的状态
 *     - hw_state（硬件状态）：硬件实际处于的状态
 *   当硬件状态被锁定时（如被 BIOS 锁定），软件状态更改会被暂存，
 *   直到硬件解锁后才会生效。
 */

#include <linux/device.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <drm/drm_privacy_screen_machine.h>
#include <drm/drm_privacy_screen_consumer.h>
#include <drm/drm_privacy_screen_driver.h>
#include "drm_internal.h"

/**
 * DOC: overview
 *
 * This class allows non KMS drivers, from e.g. drivers/platform/x86 to
 * register a privacy-screen device, which the KMS drivers can then use
 * to implement the standard privacy-screen properties, see
 * :ref:`Standard Connector Properties<standard_connector_properties>`.
 *
 * KMS drivers using a privacy-screen class device are advised to use the
 * drm_connector_attach_privacy_screen_provider() and
 * drm_connector_update_privacy_screen() helpers for dealing with this.
 */

#define to_drm_privacy_screen(dev) \
	container_of(dev, struct drm_privacy_screen, dev)

static DEFINE_MUTEX(drm_privacy_screen_lookup_lock);
static LIST_HEAD(drm_privacy_screen_lookup_list);

static DEFINE_MUTEX(drm_privacy_screen_devs_lock);
static LIST_HEAD(drm_privacy_screen_devs);

/*** drm_privacy_screen_machine.h functions ***/

/**
 * drm_privacy_screen_lookup_add - 添加隐私屏幕静态查找条目
 * @lookup: 要添加的查找条目
 *
 * 将条目添加到静态隐私屏幕查找列表中。注意，传入的
 * &struct drm_privacy_screen_lookup 中的 &struct list_head 会被添加到
 * 隐私屏幕核心拥有的列表中。因此，传入的 &struct drm_privacy_screen_lookup
 * 在通过 drm_privacy_screen_lookup_remove() 从列表中移除之前，
 * 不能被释放。
 *
 * 此函数供平台特定代码（如 x86 平台检测代码）使用，用于注册
 * 已知的隐私屏幕提供者信息。
 */
void drm_privacy_screen_lookup_add(struct drm_privacy_screen_lookup *lookup)
{
	mutex_lock(&drm_privacy_screen_lookup_lock);
	list_add(&lookup->list, &drm_privacy_screen_lookup_list);
	mutex_unlock(&drm_privacy_screen_lookup_lock);
}
EXPORT_SYMBOL(drm_privacy_screen_lookup_add);

/**
 * drm_privacy_screen_lookup_remove - 移除隐私屏幕静态查找条目
 * @lookup: 要移除的查找条目
 *
 * 从静态隐私屏幕查找列表中移除之前通过 drm_privacy_screen_lookup_add()
 * 添加的条目。
 *
 * 通常在系统关闭或模块卸载时调用，以确保资源被正确清理。
 */
void drm_privacy_screen_lookup_remove(struct drm_privacy_screen_lookup *lookup)
{
	mutex_lock(&drm_privacy_screen_lookup_lock);
	list_del(&lookup->list);
	mutex_unlock(&drm_privacy_screen_lookup_lock);
}
EXPORT_SYMBOL(drm_privacy_screen_lookup_remove);

/*** drm_privacy_screen_consumer.h functions ***/

static struct drm_privacy_screen *drm_privacy_screen_get_by_name(
	const char *name)
{
	struct drm_privacy_screen *priv;
	struct device *dev = NULL;

	mutex_lock(&drm_privacy_screen_devs_lock);

	list_for_each_entry(priv, &drm_privacy_screen_devs, list) {
		if (strcmp(dev_name(&priv->dev), name) == 0) {
			dev = get_device(&priv->dev);
			break;
		}
	}

	mutex_unlock(&drm_privacy_screen_devs_lock);

	return dev ? to_drm_privacy_screen(dev) : NULL;
}

/**
 * drm_privacy_screen_get - 获取隐私屏幕提供者
 * @dev: 要获取隐私屏幕提供者的消费者设备
 * @con_id: （视频）连接器名称
 *
 * 获取连接到 @dev 和 @con_id 参数描述的显示器上的隐私屏幕提供者。
 *
 * 查找算法（借鉴自时钟框架）：
 *   使用模糊匹配方式：
 *     - ID 为 NULL 的条目被视为通配符
 *     - 如果条目有设备 ID，则必须匹配
 *     - 如果条目有连接器 ID，则必须匹配
 *   然后选择最具体的条目，优先级顺序为：
 *     设备+连接器 > 仅设备 > 仅连接器
 *
 * 返回：
 * * 成功时返回指向 &struct drm_privacy_screen 的指针
 * * 未找到匹配时返回 ERR_PTR(-ENODEV)
 * * 有匹配但尚未注册时返回 ERR_PTR(-EPROBE_DEFER)
 */
struct drm_privacy_screen *drm_privacy_screen_get(struct device *dev,
						  const char *con_id)
{
	const char *dev_id = dev ? dev_name(dev) : NULL;
	struct drm_privacy_screen_lookup *l;
	struct drm_privacy_screen *priv;
	const char *provider = NULL;
	int match, best = -1;

	/*
	 * For now we only support using a static lookup table, which is
	 * populated by the drm_privacy_screen_arch_init() call. This should
	 * be extended with device-tree / fw_node lookup when support is added
	 * for device-tree using hardware with a privacy-screen.
	 *
	 * The lookup algorithm was shamelessly taken from the clock
	 * framework:
	 *
	 * We do slightly fuzzy matching here:
	 *  An entry with a NULL ID is assumed to be a wildcard.
	 *  If an entry has a device ID, it must match
	 *  If an entry has a connection ID, it must match
	 * Then we take the most specific entry - with the following order
	 * of precedence: dev+con > dev only > con only.
	 */
	mutex_lock(&drm_privacy_screen_lookup_lock);

	list_for_each_entry(l, &drm_privacy_screen_lookup_list, list) {
		match = 0;

		if (l->dev_id) {
			if (!dev_id || strcmp(l->dev_id, dev_id))
				continue;

			match += 2;
		}

		if (l->con_id) {
			if (!con_id || strcmp(l->con_id, con_id))
				continue;

			match += 1;
		}

		if (match > best) {
			provider = l->provider;
			best = match;
		}
	}

	mutex_unlock(&drm_privacy_screen_lookup_lock);

	if (!provider)
		return ERR_PTR(-ENODEV);

	priv = drm_privacy_screen_get_by_name(provider);
	if (!priv)
		return ERR_PTR(-EPROBE_DEFER);

	return priv;
}
EXPORT_SYMBOL(drm_privacy_screen_get);

/**
 * drm_privacy_screen_put - 释放隐私屏幕引用
 * @priv: 要释放的隐私屏幕引用
 *
 * 释放通过 drm_privacy_screen_get() 获取的隐私屏幕提供者引用。
 * 如果传入 NULL 或 ERR_PTR，则该函数不执行任何操作。
 */
void drm_privacy_screen_put(struct drm_privacy_screen *priv)
{
	if (IS_ERR_OR_NULL(priv))
		return;

	put_device(&priv->dev);
}
EXPORT_SYMBOL(drm_privacy_screen_put);

/**
 * drm_privacy_screen_set_sw_state - 设置隐私屏幕的软件状态
 * @priv: 要设置软件状态的隐私屏幕
 * @sw_state: 要设置的新软件状态值
 *
 * 设置隐私屏幕的软件状态。如果隐私屏幕未处于硬件锁定状态，
 * 则实际的硬件状态会立即更新为新值。如果隐私屏幕处于硬件
 * 锁定状态，则新的软件状态会被记住作为解除锁定后应设置的
 * 目标状态。
 *
 * 根据 DRM 连接器属性文档，在硬件状态被锁定时设置软件状态
 * 是允许的。此时除了保存新状态以便解锁后生效外，不执行其他操作。
 * 同样，如果硬件已处于目标状态，也跳过设置操作。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int drm_privacy_screen_set_sw_state(struct drm_privacy_screen *priv,
				    enum drm_privacy_screen_status sw_state)
{
	int ret = 0;

	mutex_lock(&priv->lock);

	if (!priv->ops) {
		ret = -ENODEV;
		goto out;
	}

	/*
	 * As per the DRM connector properties documentation, setting the
	 * sw_state while the hw_state is locked is allowed. In this case
	 * it is a no-op other then storing the new sw_state so that it
	 * can be honored when the state gets unlocked.
	 * Also skip the set if the hw already is in the desired state.
	 */
	if (priv->hw_state >= PRIVACY_SCREEN_DISABLED_LOCKED ||
	    priv->hw_state == sw_state) {
		priv->sw_state = sw_state;
		goto out;
	}

	ret = priv->ops->set_sw_state(priv, sw_state);
out:
	mutex_unlock(&priv->lock);
	return ret;
}
EXPORT_SYMBOL(drm_privacy_screen_set_sw_state);

/**
 * drm_privacy_screen_get_state - 获取隐私屏幕当前状态
 * @priv: 要获取状态的隐私屏幕
 * @sw_state_ret: 用于存储当前软件状态的地址
 * @hw_state_ret: 用于存储当前硬件状态的地址
 *
 * 获取隐私屏幕的当前状态，包括软件状态和硬件状态。
 * 该函数通过互斥锁保证读取的一致性。
 */
void drm_privacy_screen_get_state(struct drm_privacy_screen *priv,
				  enum drm_privacy_screen_status *sw_state_ret,
				  enum drm_privacy_screen_status *hw_state_ret)
{
	mutex_lock(&priv->lock);
	*sw_state_ret = priv->sw_state;
	*hw_state_ret = priv->hw_state;
	mutex_unlock(&priv->lock);
}
EXPORT_SYMBOL(drm_privacy_screen_get_state);

/**
 * drm_privacy_screen_register_notifier - 注册通知器
 * @priv: 要注册通知器的隐私屏幕
 * @nb: 要注册的通知器块
 *
 * 注册一个通知器到隐私屏幕，以便在隐私屏幕状态被外部（非本驱动）
 * 改变时得到通知。例如，硬件自身可能因为用户按下热键而改变状态。
 *
 * 通知器在不持有锁的情况下被调用。可以使用 drm_privacy_screen_get_state()
 * 函数获取新的硬件状态和软件状态。隐私屏幕的 &struct drm_privacy_screen
 * 指针会作为 notifier_block 的 notifier_call 的 ``void *data`` 参数传递。
 *
 * 注意：通过 drm_privacy_screen_set_sw_state() 进行的更改不会触发通知，
 * 通知仅用于外部状态变化。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int drm_privacy_screen_register_notifier(struct drm_privacy_screen *priv,
					 struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&priv->notifier_head, nb);
}
EXPORT_SYMBOL(drm_privacy_screen_register_notifier);

/**
 * drm_privacy_screen_unregister_notifier - 注销通知器
 * @priv: 要注销通知器的隐私屏幕
 * @nb: 要注销的通知器块
 *
 * 注销之前通过 drm_privacy_screen_register_notifier() 注册的通知器。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int drm_privacy_screen_unregister_notifier(struct drm_privacy_screen *priv,
					   struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&priv->notifier_head, nb);
}
EXPORT_SYMBOL(drm_privacy_screen_unregister_notifier);

/*** drm_privacy_screen_driver.h functions ***/

static ssize_t sw_state_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct drm_privacy_screen *priv = to_drm_privacy_screen(dev);
	const char * const sw_state_names[] = {
		"Disabled",
		"Enabled",
	};
	ssize_t ret;

	mutex_lock(&priv->lock);

	if (!priv->ops)
		ret = -ENODEV;
	else if (WARN_ON(priv->sw_state >= ARRAY_SIZE(sw_state_names)))
		ret = -ENXIO;
	else
		ret = sprintf(buf, "%s\n", sw_state_names[priv->sw_state]);

	mutex_unlock(&priv->lock);
	return ret;
}
/*
 * RO: Do not allow setting the sw_state through sysfs, this MUST be done
 * through the drm_properties on the drm_connector.
 */
static DEVICE_ATTR_RO(sw_state);

static ssize_t hw_state_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct drm_privacy_screen *priv = to_drm_privacy_screen(dev);
	const char * const hw_state_names[] = {
		"Disabled",
		"Enabled",
		"Disabled, locked",
		"Enabled, locked",
	};
	ssize_t ret;

	mutex_lock(&priv->lock);

	if (!priv->ops)
		ret = -ENODEV;
	else if (WARN_ON(priv->hw_state >= ARRAY_SIZE(hw_state_names)))
		ret = -ENXIO;
	else
		ret = sprintf(buf, "%s\n", hw_state_names[priv->hw_state]);

	mutex_unlock(&priv->lock);
	return ret;
}
static DEVICE_ATTR_RO(hw_state);

static struct attribute *drm_privacy_screen_attrs[] = {
	&dev_attr_sw_state.attr,
	&dev_attr_hw_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(drm_privacy_screen);

static struct device_type drm_privacy_screen_type = {
	.name = "privacy_screen",
	.groups = drm_privacy_screen_groups,
};

static void drm_privacy_screen_device_release(struct device *dev)
{
	struct drm_privacy_screen *priv = to_drm_privacy_screen(dev);

	kfree(priv);
}

/**
 * drm_privacy_screen_register - 注册隐私屏幕
 * @parent: 隐私屏幕的父设备
 * @ops: 指向 &struct drm_privacy_screen_ops 的操作函数指针
 * @data: 隐私屏幕提供者拥有的私有数据
 *
 * 创建并注册一个隐私屏幕设备。该函数会：
 *   1. 分配并初始化隐私屏幕结构体
 *   2. 设置互斥锁和通知器链表
 *   3. 初始化设备结构体并设置设备名
 *   4. 调用驱动的 get_hw_state 获取初始硬件状态
 *   5. 注册设备到系统设备模型
 *   6. 将隐私屏幕添加到全局设备列表
 *
 * 返回：
 * * 成功时返回指向创建的隐私屏幕的指针
 * * 失败时返回 ERR_PTR(errno)
 */
struct drm_privacy_screen *drm_privacy_screen_register(
	struct device *parent, const struct drm_privacy_screen_ops *ops,
	void *data)
{
	struct drm_privacy_screen *priv;
	int ret;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	mutex_init(&priv->lock);
	BLOCKING_INIT_NOTIFIER_HEAD(&priv->notifier_head);

	priv->dev.class = drm_class;
	priv->dev.type = &drm_privacy_screen_type;
	priv->dev.parent = parent;
	priv->dev.release = drm_privacy_screen_device_release;
	dev_set_name(&priv->dev, "privacy_screen-%s", dev_name(parent));
	priv->drvdata = data;
	priv->ops = ops;

	priv->ops->get_hw_state(priv);

	ret = device_register(&priv->dev);
	if (ret) {
		put_device(&priv->dev);
		return ERR_PTR(ret);
	}

	mutex_lock(&drm_privacy_screen_devs_lock);
	list_add(&priv->list, &drm_privacy_screen_devs);
	mutex_unlock(&drm_privacy_screen_devs_lock);

	return priv;
}
EXPORT_SYMBOL(drm_privacy_screen_register);

/**
 * drm_privacy_screen_unregister - 注销隐私屏幕
 * @priv: 要注销的隐私屏幕
 *
 * 注销之前通过 drm_privacy_screen_register() 注册的隐私屏幕。
 * 该函数会：
 *   1. 从全局设备列表中移除设备
 *   2. 清空驱动数据和操作函数指针
 *   3. 注销系统设备
 *
 * 可以传入 NULL 或 ERR_PTR，此时函数不执行任何操作。
 */
void drm_privacy_screen_unregister(struct drm_privacy_screen *priv)
{
	if (IS_ERR_OR_NULL(priv))
		return;

	mutex_lock(&drm_privacy_screen_devs_lock);
	list_del(&priv->list);
	mutex_unlock(&drm_privacy_screen_devs_lock);

	mutex_lock(&priv->lock);
	priv->drvdata = NULL;
	priv->ops = NULL;
	mutex_unlock(&priv->lock);

	device_unregister(&priv->dev);
}
EXPORT_SYMBOL(drm_privacy_screen_unregister);

/**
 * drm_privacy_screen_call_notifier_chain - 通知消费者状态已更改
 * @priv: 要触发通知的隐私屏幕
 *
 * 隐私屏幕提供者驱动可以在外部状态更改时调用此函数。例如，硬件
 * 自身可能因为用户按下热键而改变状态。
 *
 * 调用此函数时不能持有隐私屏幕锁。驱动必须在调用此函数之前更新
 * sw_state 和 hw_state 以反映新状态。
 *
 * 驱动在收到外部状态更改事件时的预期行为：
 *   1. 获取锁
 *   2. 更新 sw_state 和 hw_state
 *   3. 释放锁
 *   4. 调用 drm_privacy_screen_call_notifier_chain()
 */
void drm_privacy_screen_call_notifier_chain(struct drm_privacy_screen *priv)
{
	blocking_notifier_call_chain(&priv->notifier_head, 0, priv);
}
EXPORT_SYMBOL(drm_privacy_screen_call_notifier_chain);
