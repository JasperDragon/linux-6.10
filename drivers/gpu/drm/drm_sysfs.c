// SPDX-License-Identifier: GPL-2.0-only

/*
 * drm_sysfs.c - Modifications to drm_sysfs_class.c to support
 *               extra sysfs attribute from DRM. Normal drm_sysfs_class
 *               does not allow adding attributes.
 *
 * Copyright (c) 2004 Jon Smirl <jonsmirl@gmail.com>
 * Copyright (c) 2003-2004 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2003-2004 IBM Corp.
 */

/*
 * DRM Sysfs 接口 - 为 DRM 设备和连接器提供 sysfs 文件系统支持
 *
 * 本文件实现了 DRM 框架的 sysfs 接口，通过 sysfs 文件系统暴露 DRM 设备
 * 和连接器的属性。主要功能包括：
 *
 *   设备类管理：
 *     - drm_sysfs_init() / drm_sysfs_destroy() - DRM 设备类的创建和销毁
 *     - drm_sysfs_minor_alloc() - 分配给设备节点的 sysfs 设备结构
 *
 *   连接器属性（通过 /sys/class/drm/cardN-connector/）：
 *     - status：连接器状态（connected/disconnected）
 *     - enabled：编码器是否已连接
 *     - dpms：DPMS 电源状态
 *     - edid：读取 EDID 原始数据
 *     - modes：列出支持的显示模式
 *
 *   事件通知：
 *     - drm_sysfs_hotplug_event() - 发送 HOTPLUG=1 的 uevent
 *     - drm_sysfs_connector_hotplug_event() - 连接器热插拔事件
 *     - drm_sysfs_connector_property_event() - 连接器属性变更事件
 *     - drm_sysfs_lease_event() - 租赁变更事件
 *
 *   其他：Type-C 连接器关联、引导显示设备标识等
 */

#include <linux/acpi.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/i2c.h>
#include <linux/kdev_t.h>
#include <linux/pci.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <drm/drm_accel.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_property.h>
#include <drm/drm_sysfs.h>

#include <asm/video.h>

#include "drm_internal.h"
#include "drm_crtc_internal.h"

#define to_drm_minor(d) dev_get_drvdata(d)
#define to_drm_connector(d) dev_get_drvdata(d)

/**
 * DOC: overview
 *
 * DRM provides very little additional support to drivers for sysfs
 * interactions, beyond just all the standard stuff. Drivers who want to expose
 * additional sysfs properties and property groups can attach them at either
 * &drm_device.dev or &drm_connector.kdev.
 *
 * Registration is automatically handled when calling drm_dev_register(), or
 * drm_connector_register() in case of hot-plugged connectors. Unregistration is
 * also automatically handled by drm_dev_unregister() and
 * drm_connector_unregister().
 */

static struct device_type drm_sysfs_device_minor = {
	.name = "drm_minor"
};

static struct device_type drm_sysfs_device_connector = {
	.name = "drm_connector",
};

struct class *drm_class;

#ifdef CONFIG_ACPI
static bool drm_connector_acpi_bus_match(struct device *dev)
{
	return dev->type == &drm_sysfs_device_connector;
}

static struct acpi_device *drm_connector_acpi_find_companion(struct device *dev)
{
	struct drm_connector *connector = to_drm_connector(dev);

	return to_acpi_device_node(connector->fwnode);
}

static struct acpi_bus_type drm_connector_acpi_bus = {
	.name = "drm_connector",
	.match = drm_connector_acpi_bus_match,
	.find_companion = drm_connector_acpi_find_companion,
};

static void drm_sysfs_acpi_register(void)
{
	register_acpi_bus_type(&drm_connector_acpi_bus);
}

static void drm_sysfs_acpi_unregister(void)
{
	unregister_acpi_bus_type(&drm_connector_acpi_bus);
}
#else
static void drm_sysfs_acpi_register(void) { }
static void drm_sysfs_acpi_unregister(void) { }
#endif

static char *drm_devnode(const struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "dri/%s", dev_name(dev));
}

static int typec_connector_bind(struct device *dev,
				struct device *typec_connector, void *data)
{
	int ret;

	ret = sysfs_create_link(&dev->kobj, &typec_connector->kobj, "typec_connector");
	if (ret)
		return ret;

	ret = sysfs_create_link(&typec_connector->kobj, &dev->kobj, "drm_connector");
	if (ret)
		sysfs_remove_link(&dev->kobj, "typec_connector");

	return ret;
}

static void typec_connector_unbind(struct device *dev,
				   struct device *typec_connector, void *data)
{
	sysfs_remove_link(&typec_connector->kobj, "drm_connector");
	sysfs_remove_link(&dev->kobj, "typec_connector");
}

static const struct component_ops typec_connector_ops = {
	.bind = typec_connector_bind,
	.unbind = typec_connector_unbind,
};

static CLASS_ATTR_STRING(version, S_IRUGO, "drm 1.1.0 20060810");

/**
 * drm_sysfs_init - 初始化 sysfs 辅助功能
 *
 * 创建 DRM 设备类（drm class），该设备类是所有其他顶层 DRM sysfs
 * 对象的隐式父节点。DRM 设备、连接器等对象都会在 /sys/class/drm/
 * 下创建相应的子目录。
 *
 * 必须调用 drm_sysfs_destroy() 来释放分配的资源。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int drm_sysfs_init(void)
{
	int err;

	drm_class = class_create("drm");
	if (IS_ERR(drm_class))
		return PTR_ERR(drm_class);

	err = class_create_file(drm_class, &class_attr_version.attr);
	if (err) {
		class_destroy(drm_class);
		drm_class = NULL;
		return err;
	}

	drm_class->devnode = drm_devnode;

	drm_sysfs_acpi_register();
	return 0;
}

/**
 * drm_sysfs_destroy - 销毁 DRM 设备类
 *
 * 销毁由 drm_sysfs_init() 创建的 DRM 设备类（drm class），
 * 包括移除 ACPI 注册和版本属性文件，最后释放类对象。
 * 此函数在模块卸载时调用，以清理 sysfs 相关资源。
 */
void drm_sysfs_destroy(void)
{
	if (IS_ERR_OR_NULL(drm_class))
		return;
	drm_sysfs_acpi_unregister();
	class_remove_file(drm_class, &class_attr_version.attr);
	class_destroy(drm_class);
	drm_class = NULL;
}

static void drm_sysfs_release(struct device *dev)
{
	kfree(dev);
}

/*
 * Connector properties
 */
static ssize_t status_store(struct device *device,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct drm_connector *connector = to_drm_connector(device);
	struct drm_device *dev = connector->dev;
	enum drm_connector_force old_force;
	int ret;

	ret = mutex_lock_interruptible(&dev->mode_config.mutex);
	if (ret)
		return ret;

	old_force = connector->force;

	if (sysfs_streq(buf, "detect"))
		connector->force = 0;
	else if (sysfs_streq(buf, "on"))
		connector->force = DRM_FORCE_ON;
	else if (sysfs_streq(buf, "on-digital"))
		connector->force = DRM_FORCE_ON_DIGITAL;
	else if (sysfs_streq(buf, "off"))
		connector->force = DRM_FORCE_OFF;
	else
		ret = -EINVAL;

	if (old_force != connector->force || !connector->force) {
		drm_dbg_kms(dev, "[CONNECTOR:%d:%s] force updated from %d to %d or reprobing\n",
			    connector->base.id, connector->name,
			    old_force, connector->force);

		connector->funcs->fill_modes(connector,
					     dev->mode_config.max_width,
					     dev->mode_config.max_height);
	}

	mutex_unlock(&dev->mode_config.mutex);

	return ret ? ret : count;
}

static ssize_t status_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	enum drm_connector_status status;

	status = READ_ONCE(connector->status);

	return sysfs_emit(buf, "%s\n",
			  drm_get_connector_status_name(status));
}

static ssize_t dpms_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	int dpms;

	dpms = READ_ONCE(connector->dpms);

	return sysfs_emit(buf, "%s\n", drm_get_dpms_name(dpms));
}

static ssize_t enabled_show(struct device *device,
			    struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	bool enabled;

	enabled = READ_ONCE(connector->encoder);

	return sysfs_emit(buf, enabled ? "enabled\n" : "disabled\n");
}

static ssize_t edid_show(struct file *filp, struct kobject *kobj,
			 const struct bin_attribute *attr, char *buf, loff_t off,
			 size_t count)
{
	struct device *connector_dev = kobj_to_dev(kobj);
	struct drm_connector *connector = to_drm_connector(connector_dev);
	ssize_t ret;

	ret = drm_edid_connector_property_show(connector, buf, off, count);

	return ret;
}

static ssize_t modes_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);
	struct drm_display_mode *mode;
	int written = 0;

	mutex_lock(&connector->dev->mode_config.mutex);
	list_for_each_entry(mode, &connector->modes, head) {
		written += scnprintf(buf + written, PAGE_SIZE - written, "%s\n",
				    mode->name);
	}
	mutex_unlock(&connector->dev->mode_config.mutex);

	return written;
}

static ssize_t connector_id_show(struct device *device,
				 struct device_attribute *attr,
				 char *buf)
{
	struct drm_connector *connector = to_drm_connector(device);

	return sysfs_emit(buf, "%d\n", connector->base.id);
}

static DEVICE_ATTR_RW(status);
static DEVICE_ATTR_RO(enabled);
static DEVICE_ATTR_RO(dpms);
static DEVICE_ATTR_RO(modes);
static DEVICE_ATTR_RO(connector_id);

static struct attribute *connector_dev_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_enabled.attr,
	&dev_attr_dpms.attr,
	&dev_attr_modes.attr,
	&dev_attr_connector_id.attr,
	NULL
};

static const struct bin_attribute edid_attr = {
	.attr.name = "edid",
	.attr.mode = 0444,
	.size = 0,
	.read = edid_show,
};

static const struct bin_attribute *const connector_bin_attrs[] = {
	&edid_attr,
	NULL
};

static const struct attribute_group connector_dev_group = {
	.attrs = connector_dev_attrs,
	.bin_attrs = connector_bin_attrs,
};

static const struct attribute_group *connector_dev_groups[] = {
	&connector_dev_group,
	NULL
};

int drm_sysfs_connector_add(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct device *kdev;
	int r;

	if (connector->kdev)
		return 0;

	kdev = kzalloc_obj(*kdev);
	if (!kdev)
		return -ENOMEM;

	device_initialize(kdev);
	kdev->class = drm_class;
	kdev->type = &drm_sysfs_device_connector;
	kdev->parent = dev->primary->kdev;
	kdev->groups = connector_dev_groups;
	kdev->release = drm_sysfs_release;
	dev_set_drvdata(kdev, connector);

	r = dev_set_name(kdev, "card%d-%s", dev->primary->index, connector->name);
	if (r)
		goto err_free;

	drm_dbg_kms(dev, "[CONNECTOR:%d:%s] adding connector to sysfs\n",
		    connector->base.id, connector->name);

	r = device_add(kdev);
	if (r) {
		drm_err(dev, "failed to register connector device: %d\n", r);
		goto err_free;
	}

	connector->kdev = kdev;

	if (dev_fwnode(kdev)) {
		r = component_add(kdev, &typec_connector_ops);
		if (r)
			drm_err(dev, "failed to add component to create link to typec connector\n");
	}

	return 0;

err_free:
	put_device(kdev);
	return r;
}

int drm_sysfs_connector_add_late(struct drm_connector *connector)
{
	if (connector->ddc)
		return sysfs_create_link(&connector->kdev->kobj,
					 &connector->ddc->dev.kobj, "ddc");

	return 0;
}

void drm_sysfs_connector_remove_early(struct drm_connector *connector)
{
	if (connector->ddc)
		sysfs_remove_link(&connector->kdev->kobj, "ddc");
}

void drm_sysfs_connector_remove(struct drm_connector *connector)
{
	if (!connector->kdev)
		return;

	if (dev_fwnode(connector->kdev))
		component_del(connector->kdev, &typec_connector_ops);

	drm_dbg_kms(connector->dev,
		    "[CONNECTOR:%d:%s] removing connector from sysfs\n",
		    connector->base.id, connector->name);

	device_unregister(connector->kdev);
	connector->kdev = NULL;
}

void drm_sysfs_lease_event(struct drm_device *dev)
{
	char *event_string = "LEASE=1";
	char *envp[] = { event_string, NULL };

	drm_dbg_lease(dev, "generating lease event\n");

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}

/*
 * drm_sysfs_hotplug_event - 生成 DRM 热插拔 uevent 事件
 * @dev: DRM 设备
 *
 * 向用户空间发送 DRM 设备的热插拔事件 uevent，设置环境变量
 * HOTPLUG=1。用户空间的设备管理器（如 systemd-udev）接收到此
 * 事件后会触发相应的处理逻辑。
 *
 * 新的连接器状态变更应使用 drm_sysfs_connector_hotplug_event()。
 */
/**
 * drm_sysfs_hotplug_event - generate a DRM uevent
 * @dev: DRM device
 *
 * Send a uevent for the DRM device specified by @dev.  Currently we only
 * set HOTPLUG=1 in the uevent environment, but this could be expanded to
 * deal with other types of events.
 *
 * Any new uapi should be using the drm_sysfs_connector_status_event()
 * for uevents on connector status change.
 */
void drm_sysfs_hotplug_event(struct drm_device *dev)
{
	char *event_string = "HOTPLUG=1";
	char *envp[] = { event_string, NULL };

	drm_dbg_kms(dev, "generating hotplug event\n");

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(drm_sysfs_hotplug_event);

/*
 * drm_sysfs_connector_hotplug_event - 生成连接器热插拔 uevent 事件
 * @connector: 发生变化的连接器
 *
 * 向用户空间发送指定连接器的热插拔事件 uevent，包含环境变量
 * HOTPLUG=1 和 CONNECTOR=<connector_id>。用户空间通过监听此事件
 * 可以及时更新显示配置。
 */
/**
 * drm_sysfs_connector_hotplug_event - generate a DRM uevent for any connector
 * change
 * @connector: connector which has changed
 *
 * Send a uevent for the DRM connector specified by @connector. This will send
 * a uevent with the properties HOTPLUG=1 and CONNECTOR.
 */
void drm_sysfs_connector_hotplug_event(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	char hotplug_str[] = "HOTPLUG=1", conn_id[21];
	char *envp[] = { hotplug_str, conn_id, NULL };

	snprintf(conn_id, sizeof(conn_id),
		 "CONNECTOR=%u", connector->base.id);

	drm_dbg_kms(connector->dev,
		    "[CONNECTOR:%d:%s] generating connector hotplug event\n",
		    connector->base.id, connector->name);

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(drm_sysfs_connector_hotplug_event);

/*
 * drm_sysfs_connector_property_event - 生成连接器属性变更 uevent 事件
 * @connector: 属性发生变化的连接器
 * @property: 发生变化的连接器属性
 *
 * 向用户空间发送指定连接器属性的变更事件 uevent，包含 HOTPLUG=1、
 * CONNECTOR=<id> 和 PROPERTY=<id> 环境变量。用于通知用户空间
 * 连接器的某个属性值已发生变化，需要重新读取。
 */
/**
 * drm_sysfs_connector_property_event - generate a DRM uevent for connector
 * property change
 * @connector: connector on which property changed
 * @property: connector property which has changed.
 *
 * Send a uevent for the specified DRM connector and property.  Currently we
 * set HOTPLUG=1 and connector id along with the attached property id
 * related to the change.
 */
void drm_sysfs_connector_property_event(struct drm_connector *connector,
					struct drm_property *property)
{
	struct drm_device *dev = connector->dev;
	char hotplug_str[] = "HOTPLUG=1", conn_id[21], prop_id[21];
	char *envp[4] = { hotplug_str, conn_id, prop_id, NULL };

	WARN_ON(!drm_mode_obj_find_prop_id(&connector->base,
					   property->base.id));

	snprintf(conn_id, ARRAY_SIZE(conn_id),
		 "CONNECTOR=%u", connector->base.id);
	snprintf(prop_id, ARRAY_SIZE(prop_id),
		 "PROPERTY=%u", property->base.id);

	drm_dbg_kms(connector->dev,
		    "[CONNECTOR:%d:%s] generating connector property event for [PROP:%d:%s]\n",
		    connector->base.id, connector->name,
		    property->base.id, property->name);

	kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(drm_sysfs_connector_property_event);

static ssize_t boot_display_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	return sysfs_emit(buf, "1\n");
}
static DEVICE_ATTR_RO(boot_display);

static struct attribute *display_attrs[] = {
	&dev_attr_boot_display.attr,
	NULL
};

static umode_t boot_display_visible(struct kobject *kobj,
				    struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj)->parent;

	if (dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);

		if (video_is_primary_device(&pdev->dev))
			return a->mode;
	}

	return 0;
}

static const struct attribute_group display_attr_group = {
	.attrs = display_attrs,
	.is_visible = boot_display_visible,
};

static const struct attribute_group *card_dev_groups[] = {
	&display_attr_group,
	NULL
};

struct device *drm_sysfs_minor_alloc(struct drm_minor *minor)
{
	const char *minor_str;
	struct device *kdev;
	int r;

	kdev = kzalloc_obj(*kdev);
	if (!kdev)
		return ERR_PTR(-ENOMEM);

	device_initialize(kdev);

	if (minor->type == DRM_MINOR_ACCEL) {
		minor_str = "accel%d";
		accel_set_device_instance_params(kdev, minor->index);
	} else {
		if (minor->type == DRM_MINOR_RENDER)
			minor_str = "renderD%d";
		else
			minor_str = "card%d";

		kdev->devt = MKDEV(DRM_MAJOR, minor->index);
		kdev->class = drm_class;
		kdev->groups = card_dev_groups;
		kdev->type = &drm_sysfs_device_minor;
	}

	kdev->parent = minor->dev->dev;
	kdev->release = drm_sysfs_release;
	dev_set_drvdata(kdev, minor);

	r = dev_set_name(kdev, minor_str, minor->index);
	if (r < 0)
		goto err_free;

	return kdev;

err_free:
	put_device(kdev);
	return ERR_PTR(r);
}

/*
 * drm_class_device_register - 在 DRM sysfs 类中注册新设备
 * @dev: 要注册的设备
 *
 * 在 DRM sysfs 类中注册一个新的 struct device。主要用于 TTM
 * （透明传输管理）子系统，为其全局设置提供 sysfs 入口点。
 * 普通驱动不应使用此函数。
 *
 * 返回：0 表示成功，负错误码表示失败。
 */
/**
 * drm_class_device_register - register new device with the DRM sysfs class
 * @dev: device to register
 *
 * Registers a new &struct device within the DRM sysfs class. Essentially only
 * used by ttm to have a place for its global settings. Drivers should never use
 * this.
 */
int drm_class_device_register(struct device *dev)
{
	if (!drm_class || IS_ERR(drm_class))
		return -ENOENT;

	dev->class = drm_class;
	return device_register(dev);
}
EXPORT_SYMBOL_GPL(drm_class_device_register);

/*
 * drm_class_device_unregister - 从 DRM sysfs 类中注销设备
 * @dev: 要注销的设备
 *
 * 从 DRM sysfs 类中注销指定的 struct device。与 drm_class_device_register()
 * 对应，主要用于 TTM 子系统的 sysfs 入口清理。
 */
/**
 * drm_class_device_unregister - unregister device with the DRM sysfs class
 * @dev: device to unregister
 *
 * Unregisters a &struct device from the DRM sysfs class. Essentially only used
 * by ttm to have a place for its global settings. Drivers should never use
 * this.
 */
void drm_class_device_unregister(struct device *dev)
{
	return device_unregister(dev);
}
EXPORT_SYMBOL_GPL(drm_class_device_unregister);
