// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/usb/core/sysfs.c
 *
 * (C) Copyright 2002 David Brownell
 * (C) Copyright 2002,2004 Greg Kroah-Hartman
 * (C) Copyright 2002,2004 IBM Corp.
 *
 * All of the sysfs file attributes for usb devices and interfaces.
 *
 * Released under the GPLv2 only.
 */

/*
 * sysfs.c - USB 设备和接口的 sysfs 属性文件实现
 *
 * 本文件实现了 USB 子系统中所有 sysfs 属性文件的显示/存储回调函数。
 * 这些属性文件暴露在 /sys/bus/usb/devices/ 和 /sys/class/usb/ 目录下，
 * 用户空间可以通过读写这些文件来查询和控制 USB 设备的状态。
 * 属性通过 attribute_group 组织，由驱动核心在设备/接口注册时自动创建。
 *
 * 设备级属性 (dev_attrs) 包括:
 *   - 基本信息: speed, devnum, busnum, devpath, version, maxchild
 *   - 设备身份: idVendor, idProduct, bcdDevice, bDeviceClass 等
 *   - 配置信息: bConfigurationValue, bNumInterfaces, bMaxPower, configuration
 *   - 电源管理: persist, autosuspend, connected_duration, active_duration
 *   - 链路电源管理: usb2_hardware_lpm, usb3_hardware_lpm_u1/u2
 *   - 授权控制: authorized, authorized_default
 *   - 诊断信息: quirks, avoid_reset_quirk, urbnum, ltm_capable
 *   - 字符串描述符: manufacturer, product, serial
 *   - 二进制描述符: descriptors, bos_descriptors
 *
 * 接口级属性 (intf_attrs) 包括:
 *   - bInterfaceNumber, bAlternateSetting, bNumEndpoints
 *   - bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol
 *   - modalias, interface, supports_autosuspend, authorized
 *   - 接口关联描述符 (IAD): iad_bFirstInterface 等
 *   - wireless_status
 *
 * 属性组通过 usb_device_groups[] 和 usb_interface_groups[] 组织，
 * 分别在 usb_device_type 和 usb_if_device_type 中注册，
 * 由驱动核心在设备/接口注册时自动创建。
 */


#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/string.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/quirks.h>
#include <linux/of.h>
#include "usb.h"

/* Active configuration fields */
#define usb_actconfig_show(field, format_string)			\
static ssize_t field##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct usb_device *udev;					\
	struct usb_host_config *actconfig;				\
	ssize_t rc;							\
									\
	udev = to_usb_device(dev);					\
	rc = usb_lock_device_interruptible(udev);			\
	if (rc < 0)							\
		return -EINTR;						\
	actconfig = udev->actconfig;					\
	if (actconfig)							\
		rc = sysfs_emit(buf, format_string,			\
				actconfig->desc.field);			\
	usb_unlock_device(udev);					\
	return rc;							\
}									\

#define usb_actconfig_attr(field, format_string)		\
	usb_actconfig_show(field, format_string)		\
	static DEVICE_ATTR_RO(field)

usb_actconfig_attr(bNumInterfaces, "%2d\n");
usb_actconfig_attr(bmAttributes, "%2x\n");

/*
 * bMaxPower - 显示当前配置的最大总线供电电流
 * 返回设备当前配置所需的最大总线供电电流（单位 mA）。
 * 该值来自 USB 配置描述符的 bMaxPower 字段（每个单位代表 2mA）。
 * 对于总线供电的设备，此值反映了设备从 USB 总线获取的最大电流。
 */
static ssize_t bMaxPower_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usb_device *udev;
	struct usb_host_config *actconfig;
	ssize_t rc;

	udev = to_usb_device(dev);
	rc = usb_lock_device_interruptible(udev);
	if (rc < 0)
		return -EINTR;
	actconfig = udev->actconfig;
	if (actconfig)
		rc = sysfs_emit(buf, "%dmA\n", usb_get_max_power(udev, actconfig));
	usb_unlock_device(udev);
	return rc;
}
static DEVICE_ATTR_RO(bMaxPower);

/*
 * configuration - 显示当前配置的描述字符串
 * 返回当前激活配置的描述字符串，该字符串从配置描述符的 iConfiguration 字段
 * 指向的字符串描述符中读取。配置字符串描述了该配置的用途（如"高功耗模式"等）。
 */
static ssize_t configuration_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usb_device *udev;
	struct usb_host_config *actconfig;
	ssize_t rc;

	udev = to_usb_device(dev);
	rc = usb_lock_device_interruptible(udev);
	if (rc < 0)
		return -EINTR;
	actconfig = udev->actconfig;
	if (actconfig && actconfig->string)
		rc = sysfs_emit(buf, "%s\n", actconfig->string);
	usb_unlock_device(udev);
	return rc;
}
static DEVICE_ATTR_RO(configuration);

/* configuration value is always present, and r/w */
usb_actconfig_show(bConfigurationValue, "%u\n");

/*
 * bConfigurationValue - 读取/设置当前激活的配置值
 * 读取时返回当前激活配置的 bConfigurationValue。
 * 写入时切换设备到指定的配置（值为 -1 表示使设备进入未配置状态）。
 * 这是 sysfs 中少数的可写属性之一，允许用户空间动态切换 USB 设备的配置，
 * 从而改变设备的功能和行为（如切换电源模式或启用/禁用某些接口）。
 */
static ssize_t bConfigurationValue_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct usb_device	*udev = to_usb_device(dev);
	int			config, value, rc;

	if (sscanf(buf, "%d", &config) != 1 || config < -1 || config > 255)
		return -EINVAL;
	rc = usb_lock_device_interruptible(udev);
	if (rc < 0)
		return -EINTR;
	value = usb_set_configuration(udev, config);
	usb_unlock_device(udev);
	return (value < 0) ? value : count;
}
static DEVICE_ATTR_IGNORE_LOCKDEP(bConfigurationValue, S_IRUGO | S_IWUSR,
		bConfigurationValue_show, bConfigurationValue_store);

#ifdef CONFIG_OF
static ssize_t devspec_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct device_node *of_node = dev->of_node;

	return sysfs_emit(buf, "%pOF\n", of_node);
}
static DEVICE_ATTR_RO(devspec);
#endif

/* String fields */
#define usb_string_attr(name)						\
static ssize_t  name##_show(struct device *dev,				\
		struct device_attribute *attr, char *buf)		\
{									\
	struct usb_device *udev;					\
	int retval;							\
									\
	udev = to_usb_device(dev);					\
	retval = usb_lock_device_interruptible(udev);			\
	if (retval < 0)							\
		return -EINTR;						\
	retval = sysfs_emit(buf, "%s\n", udev->name);			\
	usb_unlock_device(udev);					\
	return retval;							\
}									\
static DEVICE_ATTR_RO(name)

/*
 * manufacturer / product / serial - USB 字符串描述符属性
 * 显示从 USB 设备读取的 iManufacturer、iProduct 和 iSerial 字符串。
 * 字符串在设备枚举时从设备固件中读取并缓存在 udev 结构体中。
 * 如果设备未提供相应字符串描述符，对应的 sysfs 文件将自动隐藏
 * （通过 dev_string_attrs_are_visible() 控制可见性）。
 * 这些字符串是设备识别和 udev 规则匹配的重要依据。
 */
usb_string_attr(product);
usb_string_attr(manufacturer);
usb_string_attr(serial);

/*
 * speed - 显示 USB 设备的连接速度
 * 返回设备当前协商的 USB 速度等级（单位 Mbps）：
 *   - 1.5   (USB 1.x Low-Speed)
 *   - 12    (USB 1.x Full-Speed / USB 2.0 兼容)
 *   - 480   (USB 2.0 High-Speed)
 *   - 5000  (USB 3.x SuperSpeed / SuperSpeedPlus Gen 1)
 *   - 10000 (USB 3.1 SuperSpeedPlus Gen 2)
 *   - 20000 (USB 3.2 SuperSpeedPlus Gen 2x2)
 * 速度取决于主机控制器、线缆质量和设备能力三方的协商结果。
 */
static ssize_t speed_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct usb_device *udev;
	char *speed;

	udev = to_usb_device(dev);

	switch (udev->speed) {
	case USB_SPEED_LOW:
		speed = "1.5";
		break;
	case USB_SPEED_UNKNOWN:
	case USB_SPEED_FULL:
		speed = "12";
		break;
	case USB_SPEED_HIGH:
		speed = "480";
		break;
	case USB_SPEED_SUPER:
		speed = "5000";
		break;
	case USB_SPEED_SUPER_PLUS:
		if (udev->ssp_rate == USB_SSP_GEN_2x2)
			speed = "20000";
		else
			speed = "10000";
		break;
	default:
		speed = "unknown";
	}
	return sysfs_emit(buf, "%s\n", speed);
}
static DEVICE_ATTR_RO(speed);

static ssize_t rx_lanes_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", udev->rx_lanes);
}
static DEVICE_ATTR_RO(rx_lanes);

static ssize_t tx_lanes_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", udev->tx_lanes);
}
static DEVICE_ATTR_RO(tx_lanes);

static ssize_t busnum_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", udev->bus->busnum);
}
static DEVICE_ATTR_RO(busnum);

/*
 * devnum - 显示 USB 设备地址编号
 * 返回该设备在 USB 总线上的地址编号（范围 1-127）。
 * 地址编号由 USB 子系统在设备枚举时动态分配，每次枚举可能不同。
 * 每个 USB 设备在一条总线上有唯一的地址，主机通过此地址与设备通信。
 */
static ssize_t devnum_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", udev->devnum);
}
static DEVICE_ATTR_RO(devnum);

static ssize_t devpath_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%s\n", udev->devpath);
}
static DEVICE_ATTR_RO(devpath);

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct usb_device *udev;
	u16 bcdUSB;

	udev = to_usb_device(dev);
	bcdUSB = le16_to_cpu(udev->descriptor.bcdUSB);
	return sysfs_emit(buf, "%2x.%02x\n", bcdUSB >> 8, bcdUSB & 0xff);
}
static DEVICE_ATTR_RO(version);

static ssize_t maxchild_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", udev->maxchild);
}
static DEVICE_ATTR_RO(maxchild);

static ssize_t quirks_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "0x%x\n", udev->quirks);
}
static DEVICE_ATTR_RO(quirks);

/*
 * avoid_reset_quirk - 复位变通标志
 * 读取时返回设备是否已设置 USB_QUIRK_RESET 标志。
 * 写入 1 设置该标志，写入 0 清除该标志。
 * 设置此标志后，USB 核心将避免对该设备执行复位操作，
 * 用于解决某些存在复位兼容性问题的设备（复位后无法正常工作）。
 * 此属性为可读写，允许用户空间根据需要动态调整。
 */
static ssize_t avoid_reset_quirk_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", !!(udev->quirks & USB_QUIRK_RESET));
}

static ssize_t avoid_reset_quirk_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct usb_device	*udev = to_usb_device(dev);
	bool			val;
	int			rc;

	if (kstrtobool(buf, &val) != 0)
		return -EINVAL;
	rc = usb_lock_device_interruptible(udev);
	if (rc < 0)
		return -EINTR;
	if (val)
		udev->quirks |= USB_QUIRK_RESET;
	else
		udev->quirks &= ~USB_QUIRK_RESET;
	usb_unlock_device(udev);
	return count;
}
static DEVICE_ATTR_RW(avoid_reset_quirk);

/*
 * urbnum - 显示当前活跃 URB 数量
 * 返回该 USB 设备当前未完成的 URB（USB Request Block）数量。
 * URB 是 USB 核心与主机控制器驱动之间传输数据的基本单位，
 * 每个 URB 代表一次或一组 USB 数据传输事务。
 * 该计数器可用于诊断设备是否处于繁忙状态、是否存在 URB 泄漏
 * 或驱动是否正确提交和回收了 URB。
 */
static ssize_t urbnum_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct usb_device *udev;

	udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", atomic_read(&udev->urbnum));
}
static DEVICE_ATTR_RO(urbnum);

static ssize_t ltm_capable_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (usb_device_supports_ltm(to_usb_device(dev)))
		return sysfs_emit(buf, "%s\n", "yes");
	return sysfs_emit(buf, "%s\n", "no");
}
static DEVICE_ATTR_RO(ltm_capable);

#ifdef	CONFIG_PM

static ssize_t persist_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct usb_device *udev = to_usb_device(dev);

	return sysfs_emit(buf, "%d\n", udev->persist_enabled);
}

static ssize_t persist_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct usb_device *udev = to_usb_device(dev);
	bool value;
	int rc;

	/* Hubs are always enabled for USB_PERSIST */
	if (udev->descriptor.bDeviceClass == USB_CLASS_HUB)
		return -EPERM;

	if (kstrtobool(buf, &value) != 0)
		return -EINVAL;

	rc = usb_lock_device_interruptible(udev);
	if (rc < 0)
		return -EINTR;
	udev->persist_enabled = !!value;
	usb_unlock_device(udev);
	return count;
}
static DEVICE_ATTR_RW(persist);

static int add_persist_attributes(struct device *dev)
{
	int rc = 0;

	if (is_usb_device(dev)) {
		struct usb_device *udev = to_usb_device(dev);

		/* Hubs are automatically enabled for USB_PERSIST,
		 * no point in creating the attribute file.
		 */
		if (udev->descriptor.bDeviceClass != USB_CLASS_HUB)
			rc = sysfs_add_file_to_group(&dev->kobj,
					&dev_attr_persist.attr,
					power_group_name);
	}
	return rc;
}

static void remove_persist_attributes(struct device *dev)
{
	sysfs_remove_file_from_group(&dev->kobj,
			&dev_attr_persist.attr,
			power_group_name);
}

static ssize_t connected_duration_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct usb_device *udev = to_usb_device(dev);

	return sysfs_emit(buf, "%u\n",
			jiffies_to_msecs(jiffies - udev->connect_time));
}
static DEVICE_ATTR_RO(connected_duration);

/*
 * If the device is resumed, the last time the device was suspended has
 * been pre-subtracted from active_duration.  We add the current time to
 * get the duration that the device was actually active.
 *
 * If the device is suspended, the active_duration is up-to-date.
 */
static ssize_t active_duration_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct usb_device *udev = to_usb_device(dev);
	int duration;

	if (udev->state != USB_STATE_SUSPENDED)
		duration = jiffies_to_msecs(jiffies + udev->active_duration);
	else
		duration = jiffies_to_msecs(udev->active_duration);
	return sysfs_emit(buf, "%u\n", duration);
}
static DEVICE_ATTR_RO(active_duration);

static ssize_t autosuspend_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", dev->power.autosuspend_delay / 1000);
}

static ssize_t autosuspend_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	int value;

	if (sscanf(buf, "%d", &value) != 1 || value >= INT_MAX/1000 ||
			value <= -INT_MAX/1000)
		return -EINVAL;

	pm_runtime_set_autosuspend_delay(dev, value * 1000);
	return count;
}
static DEVICE_ATTR_RW(autosuspend);

static const char on_string[] = "on";
static const char auto_string[] = "auto";

static void warn_level(void)
{
	static int level_warned;

	if (!level_warned) {
		level_warned = 1;
		printk(KERN_WARNING "WARNING! power/level is deprecated; "
				"use power/control instead\n");
	}
}

static ssize_t level_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct usb_device *udev = to_usb_device(dev);
	const char *p = auto_string;

	warn_level();
	if (udev->state != USB_STATE_SUSPENDED && !udev->dev.power.runtime_auto)
		p = on_string;
	return sysfs_emit(buf, "%s\n", p);
}

static ssize_t level_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct usb_device *udev = to_usb_device(dev);
	int len = count;
	char *cp;
	int rc = count;
	int rv;

	warn_level();
	cp = memchr(buf, '\n', count);
	if (cp)
		len = cp - buf;

	rv = usb_lock_device_interruptible(udev);
	if (rv < 0)
		return -EINTR;

	if (len == sizeof on_string - 1 &&
			strncmp(buf, on_string, len) == 0)
		usb_disable_autosuspend(udev);

	else if (len == sizeof auto_string - 1 &&
			strncmp(buf, auto_string, len) == 0)
		usb_enable_autosuspend(udev);

	else
		rc = -EINVAL;

	usb_unlock_device(udev);
	return rc;
}
static DEVICE_ATTR_RW(level);

static ssize_t usb2_hardware_lpm_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct usb_device *udev = to_usb_device(dev);
	const char *p;

	if (udev->usb2_hw_lpm_allowed == 1)
		p = "enabled";
	else
		p = "disabled";

	return sysfs_emit(buf, "%s\n", p);
}

static ssize_t usb2_hardware_lpm_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct usb_device *udev = to_usb_device(dev);
	bool value;
	int ret;

	ret = usb_lock_device_interruptible(udev);
	if (ret < 0)
		return -EINTR;

	ret = kstrtobool(buf, &value);

	if (!ret) {
		udev->usb2_hw_lpm_allowed = value;
		if (value)
			ret = usb_enable_usb2_hardware_lpm(udev);
		else
			ret = usb_disable_usb2_hardware_lpm(udev);
	}

	usb_unlock_device(udev);

	if (!ret)
		return count;

	return ret;
}
static DEVICE_ATTR_RW(usb2_hardware_lpm);

static ssize_t usb2_lpm_l1_timeout_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct usb_device *udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", udev->l1_params.timeout);
}

static ssize_t usb2_lpm_l1_timeout_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct usb_device *udev = to_usb_device(dev);
	u16 timeout;

	if (kstrtou16(buf, 0, &timeout))
		return -EINVAL;

	udev->l1_params.timeout = timeout;

	return count;
}
static DEVICE_ATTR_RW(usb2_lpm_l1_timeout);

static ssize_t usb2_lpm_besl_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct usb_device *udev = to_usb_device(dev);
	return sysfs_emit(buf, "%d\n", udev->l1_params.besl);
}

static ssize_t usb2_lpm_besl_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct usb_device *udev = to_usb_device(dev);
	u8 besl;

	if (kstrtou8(buf, 0, &besl) || besl > 15)
		return -EINVAL;

	udev->l1_params.besl = besl;

	return count;
}
static DEVICE_ATTR_RW(usb2_lpm_besl);

static ssize_t usb3_hardware_lpm_u1_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct usb_device *udev = to_usb_device(dev);
	const char *p;
	int rc;

	rc = usb_lock_device_interruptible(udev);
	if (rc < 0)
		return -EINTR;

	if (udev->usb3_lpm_u1_enabled)
		p = "enabled";
	else
		p = "disabled";

	usb_unlock_device(udev);

	return sysfs_emit(buf, "%s\n", p);
}
static DEVICE_ATTR_RO(usb3_hardware_lpm_u1);

static ssize_t usb3_hardware_lpm_u2_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct usb_device *udev = to_usb_device(dev);
	const char *p;
	int rc;

	rc = usb_lock_device_interruptible(udev);
	if (rc < 0)
		return -EINTR;

	if (udev->usb3_lpm_u2_enabled)
		p = "enabled";
	else
		p = "disabled";

	usb_unlock_device(udev);

	return sysfs_emit(buf, "%s\n", p);
}
static DEVICE_ATTR_RO(usb3_hardware_lpm_u2);

static struct attribute *usb2_hardware_lpm_attr[] = {
	&dev_attr_usb2_hardware_lpm.attr,
	&dev_attr_usb2_lpm_l1_timeout.attr,
	&dev_attr_usb2_lpm_besl.attr,
	NULL,
};
static const struct attribute_group usb2_hardware_lpm_attr_group = {
	.name	= power_group_name,
	.attrs	= usb2_hardware_lpm_attr,
};

static struct attribute *usb3_hardware_lpm_attr[] = {
	&dev_attr_usb3_hardware_lpm_u1.attr,
	&dev_attr_usb3_hardware_lpm_u2.attr,
	NULL,
};
static const struct attribute_group usb3_hardware_lpm_attr_group = {
	.name	= power_group_name,
	.attrs	= usb3_hardware_lpm_attr,
};

static struct attribute *power_attrs[] = {
	&dev_attr_autosuspend.attr,
	&dev_attr_level.attr,
	&dev_attr_connected_duration.attr,
	&dev_attr_active_duration.attr,
	NULL,
};
static const struct attribute_group power_attr_group = {
	.name	= power_group_name,
	.attrs	= power_attrs,
};

static int add_power_attributes(struct device *dev)
{
	int rc = 0;

	if (is_usb_device(dev)) {
		struct usb_device *udev = to_usb_device(dev);
		rc = sysfs_merge_group(&dev->kobj, &power_attr_group);
		if (udev->usb2_hw_lpm_capable == 1)
			rc = sysfs_merge_group(&dev->kobj,
					&usb2_hardware_lpm_attr_group);
		if ((udev->speed == USB_SPEED_SUPER ||
		     udev->speed == USB_SPEED_SUPER_PLUS) &&
				udev->lpm_capable == 1)
			rc = sysfs_merge_group(&dev->kobj,
					&usb3_hardware_lpm_attr_group);
	}

	return rc;
}

static void remove_power_attributes(struct device *dev)
{
	sysfs_unmerge_group(&dev->kobj, &usb3_hardware_lpm_attr_group);
	sysfs_unmerge_group(&dev->kobj, &usb2_hardware_lpm_attr_group);
	sysfs_unmerge_group(&dev->kobj, &power_attr_group);
}

#else

#define add_persist_attributes(dev)	0
#define remove_persist_attributes(dev)	do {} while (0)

#define add_power_attributes(dev)	0
#define remove_power_attributes(dev)	do {} while (0)

#endif	/* CONFIG_PM */


/* Descriptor fields */
#define usb_descriptor_attr_le16(field, format_string)			\
static ssize_t								\
field##_show(struct device *dev, struct device_attribute *attr,	\
		char *buf)						\
{									\
	struct usb_device *udev;					\
									\
	udev = to_usb_device(dev);					\
	return sysfs_emit(buf, format_string,				\
			le16_to_cpu(udev->descriptor.field));		\
}									\
static DEVICE_ATTR_RO(field)

/*
 * idVendor / idProduct / bcdDevice - 设备身份标识
 * 这些属性直接来自 USB 设备描述符，分别表示：
 *   - idVendor:  USB-IF 分配的厂商 ID（16 位，十六进制）
 *   - idProduct: 厂商定义的产品 ID（16 位，十六进制）
 *   - bcdDevice: 设备版本号（BCD 编码，如 0x0100 表示 1.00）
 * 用户空间通过这些属性唯一识别 USB 设备类型，
 * 是 udev 规则匹配、设备分类和权限控制的重要依据。
 */
usb_descriptor_attr_le16(idVendor, "%04x\n");
usb_descriptor_attr_le16(idProduct, "%04x\n");
usb_descriptor_attr_le16(bcdDevice, "%04x\n");

#define usb_descriptor_attr(field, format_string)			\
static ssize_t								\
field##_show(struct device *dev, struct device_attribute *attr,	\
		char *buf)						\
{									\
	struct usb_device *udev;					\
									\
	udev = to_usb_device(dev);					\
	return sysfs_emit(buf, format_string, udev->descriptor.field);	\
}									\
static DEVICE_ATTR_RO(field)

/*
 * bDeviceClass / bDeviceSubClass / bDeviceProtocol - USB 标准设备分类
 * 这些属性来自 USB 设备描述符，用于标识设备所属的标准类别：
 *   - bDeviceClass:     基类代码（0x00 表示由接口描述符定义，0x09 表示 Hub）
 *   - bDeviceSubClass:  子类代码（进一步细分设备类型）
 *   - bDeviceProtocol:  协议代码（指定设备使用的协议）
 *
 * bNumConfigurations - 设备支持的配置描述符数量
 * bMaxPacketSize0 - 端点 0 的最大包大小（字节）
 */
usb_descriptor_attr(bDeviceClass, "%02x\n");
usb_descriptor_attr(bDeviceSubClass, "%02x\n");
usb_descriptor_attr(bDeviceProtocol, "%02x\n");
usb_descriptor_attr(bNumConfigurations, "%d\n");
usb_descriptor_attr(bMaxPacketSize0, "%d\n");


/*
 * authorized - 设备授权状态控制
 *
 * 读取时返回设备的授权状态（1 = 已授权，0 = 未授权）。
 * 写入 0 使设备去授权（deauthorize），设备将不可用且不会响应任何驱动；
 * 写入 1 授权设备，使其可被 USB 驱动正常绑定。
 *
 * 这是 USB 安全机制的核心接口：
 *   - 未授权的设备不会被任何 USB 驱动绑定或访问
 *   - 可用于实现 USB 设备白名单/黑名单（USB 防火墙）
 *   - 默认授权策略由 authorized_default 控制
 *   - 典型应用: 在授权前检查设备身份，阻止未识别的 USB 设备
 */
static ssize_t authorized_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct usb_device *usb_dev = to_usb_device(dev);
	return sysfs_emit(buf, "%u\n", usb_dev->authorized);
}

/*
 * Authorize a device to be used in the system
 *
 * Writing a 0 deauthorizes the device, writing a 1 authorizes it.
 */
static ssize_t authorized_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t size)
{
	ssize_t result;
	struct usb_device *usb_dev = to_usb_device(dev);
	bool val;

	if (kstrtobool(buf, &val) != 0)
		result = -EINVAL;
	else if (val)
		result = usb_authorize_device(usb_dev);
	else
		result = usb_deauthorize_device(usb_dev);
	return result < 0 ? result : size;
}
static DEVICE_ATTR_IGNORE_LOCKDEP(authorized, S_IRUGO | S_IWUSR,
				  authorized_show, authorized_store);

/* "Safely remove a device" */
static ssize_t remove_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct usb_device *udev = to_usb_device(dev);
	int rc = 0;

	usb_lock_device(udev);
	if (udev->state != USB_STATE_NOTATTACHED) {

		/* To avoid races, first unconfigure and then remove */
		usb_set_configuration(udev, -1);
		rc = usb_remove_device(udev);
	}
	if (rc == 0)
		rc = count;
	usb_unlock_device(udev);
	return rc;
}
static DEVICE_ATTR_IGNORE_LOCKDEP(remove, S_IWUSR, NULL, remove_store);


static struct attribute *dev_attrs[] = {
	/* current configuration's attributes */
	&dev_attr_configuration.attr,
	&dev_attr_bNumInterfaces.attr,
	&dev_attr_bConfigurationValue.attr,
	&dev_attr_bmAttributes.attr,
	&dev_attr_bMaxPower.attr,
	/* device attributes */
	&dev_attr_urbnum.attr,
	&dev_attr_idVendor.attr,
	&dev_attr_idProduct.attr,
	&dev_attr_bcdDevice.attr,
	&dev_attr_bDeviceClass.attr,
	&dev_attr_bDeviceSubClass.attr,
	&dev_attr_bDeviceProtocol.attr,
	&dev_attr_bNumConfigurations.attr,
	&dev_attr_bMaxPacketSize0.attr,
	&dev_attr_speed.attr,
	&dev_attr_rx_lanes.attr,
	&dev_attr_tx_lanes.attr,
	&dev_attr_busnum.attr,
	&dev_attr_devnum.attr,
	&dev_attr_devpath.attr,
	&dev_attr_version.attr,
	&dev_attr_maxchild.attr,
	&dev_attr_quirks.attr,
	&dev_attr_avoid_reset_quirk.attr,
	&dev_attr_authorized.attr,
	&dev_attr_remove.attr,
	&dev_attr_ltm_capable.attr,
#ifdef CONFIG_OF
	&dev_attr_devspec.attr,
#endif
	NULL,
};
static const struct attribute_group dev_attr_grp = {
	.attrs = dev_attrs,
};

/* When modifying this list, be sure to modify dev_string_attrs_are_visible()
 * accordingly.
 */
static struct attribute *dev_string_attrs[] = {
	&dev_attr_manufacturer.attr,
	&dev_attr_product.attr,
	&dev_attr_serial.attr,
	NULL
};

static umode_t dev_string_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct usb_device *udev = to_usb_device(dev);

	if (a == &dev_attr_manufacturer.attr) {
		if (udev->manufacturer == NULL)
			return 0;
	} else if (a == &dev_attr_product.attr) {
		if (udev->product == NULL)
			return 0;
	} else if (a == &dev_attr_serial.attr) {
		if (udev->serial == NULL)
			return 0;
	}
	return a->mode;
}

static const struct attribute_group dev_string_attr_grp = {
	.attrs =	dev_string_attrs,
	.is_visible =	dev_string_attrs_are_visible,
};

/* Binary descriptors */

static ssize_t
descriptors_read(struct file *filp, struct kobject *kobj,
		const struct bin_attribute *attr,
		char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct usb_device *udev = to_usb_device(dev);
	size_t nleft = count;
	size_t srclen, n;
	int cfgno;
	void *src;

	/* The binary attribute begins with the device descriptor.
	 * Following that are the raw descriptor entries for all the
	 * configurations (config plus subsidiary descriptors).
	 */
	for (cfgno = -1; cfgno < udev->descriptor.bNumConfigurations &&
			nleft > 0; ++cfgno) {
		if (cfgno < 0) {
			src = &udev->descriptor;
			srclen = sizeof(struct usb_device_descriptor);
		} else {
			src = udev->rawdescriptors[cfgno];
			srclen = le16_to_cpu(udev->config[cfgno].desc.
					wTotalLength);
		}
		if (off < srclen) {
			n = min(nleft, srclen - (size_t) off);
			memcpy(buf, src + off, n);
			nleft -= n;
			buf += n;
			off = 0;
		} else {
			off -= srclen;
		}
	}
	return count - nleft;
}
static const BIN_ATTR_RO(descriptors, 18 + 65535); /* dev descr + max-size raw descriptor */

static ssize_t
bos_descriptors_read(struct file *filp, struct kobject *kobj,
		const struct bin_attribute *attr,
		char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct usb_device *udev = to_usb_device(dev);
	struct usb_host_bos *bos = udev->bos;
	struct usb_bos_descriptor *desc;
	size_t desclen, n = 0;

	if (bos) {
		desc = bos->desc;
		desclen = le16_to_cpu(desc->wTotalLength);
		if (off < desclen) {
			n = min(count, desclen - (size_t) off);
			memcpy(buf, (void *) desc + off, n);
		}
	}
	return n;
}
static const BIN_ATTR_RO(bos_descriptors, 65535); /* max-size BOS */

/* When modifying this list, be sure to modify dev_bin_attrs_are_visible()
 * accordingly.
 */
static const struct bin_attribute *const dev_bin_attrs[] = {
	&bin_attr_descriptors,
	&bin_attr_bos_descriptors,
	NULL
};

static umode_t dev_bin_attrs_are_visible(struct kobject *kobj,
		const struct bin_attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct usb_device *udev = to_usb_device(dev);

	/*
	 * There's no need to check if the descriptors attribute should
	 * be visible because all devices have a device descriptor. The
	 * bos_descriptors attribute should be visible if and only if
	 * the device has a BOS, so check if it exists here.
	 */
	if (a == &bin_attr_bos_descriptors) {
		if (udev->bos == NULL)
			return 0;
	}
	return a->attr.mode;
}

static const struct attribute_group dev_bin_attr_grp = {
	.bin_attrs =	dev_bin_attrs,
	.is_bin_visible =	dev_bin_attrs_are_visible,
};

const struct attribute_group *usb_device_groups[] = {
	&dev_attr_grp,
	&dev_string_attr_grp,
	&dev_bin_attr_grp,
	NULL
};

/*
 * Show & store the current value of authorized_default
 */
static ssize_t authorized_default_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct usb_device *rh_usb_dev = to_usb_device(dev);
	struct usb_bus *usb_bus = rh_usb_dev->bus;
	struct usb_hcd *hcd;

	hcd = bus_to_hcd(usb_bus);
	return sysfs_emit(buf, "%u\n", hcd->dev_policy);
}

static ssize_t authorized_default_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	ssize_t result;
	unsigned int val;
	struct usb_device *rh_usb_dev = to_usb_device(dev);
	struct usb_bus *usb_bus = rh_usb_dev->bus;
	struct usb_hcd *hcd;

	hcd = bus_to_hcd(usb_bus);
	result = sscanf(buf, "%u\n", &val);
	if (result == 1) {
		hcd->dev_policy = val <= USB_DEVICE_AUTHORIZE_INTERNAL ?
			val : USB_DEVICE_AUTHORIZE_ALL;
		result = size;
	} else {
		result = -EINVAL;
	}
	return result;
}
static DEVICE_ATTR_RW(authorized_default);

/*
 * interface_authorized_default_show - show default authorization status
 * for USB interfaces
 *
 * note: interface_authorized_default is the default value
 *       for initializing the authorized attribute of interfaces
 */
static ssize_t interface_authorized_default_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usb_device *usb_dev = to_usb_device(dev);
	struct usb_hcd *hcd = bus_to_hcd(usb_dev->bus);

	return sysfs_emit(buf, "%u\n", !!HCD_INTF_AUTHORIZED(hcd));
}

/*
 * interface_authorized_default_store - store default authorization status
 * for USB interfaces
 *
 * note: interface_authorized_default is the default value
 *       for initializing the authorized attribute of interfaces
 */
static ssize_t interface_authorized_default_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct usb_device *usb_dev = to_usb_device(dev);
	struct usb_hcd *hcd = bus_to_hcd(usb_dev->bus);
	int rc = count;
	bool val;

	if (kstrtobool(buf, &val) != 0)
		return -EINVAL;

	if (val)
		set_bit(HCD_FLAG_INTF_AUTHORIZED, &hcd->flags);
	else
		clear_bit(HCD_FLAG_INTF_AUTHORIZED, &hcd->flags);

	return rc;
}
static DEVICE_ATTR_RW(interface_authorized_default);

/* Group all the USB bus attributes */
static struct attribute *usb_bus_attrs[] = {
		&dev_attr_authorized_default.attr,
		&dev_attr_interface_authorized_default.attr,
		NULL,
};

static const struct attribute_group usb_bus_attr_group = {
	.name = NULL,	/* we want them in the same directory */
	.attrs = usb_bus_attrs,
};


static int add_default_authorized_attributes(struct device *dev)
{
	int rc = 0;

	if (is_usb_device(dev))
		rc = sysfs_create_group(&dev->kobj, &usb_bus_attr_group);

	return rc;
}

static void remove_default_authorized_attributes(struct device *dev)
{
	if (is_usb_device(dev)) {
		sysfs_remove_group(&dev->kobj, &usb_bus_attr_group);
	}
}

/*
 * usb_create_sysfs_dev_files - 创建 USB 设备的 sysfs 属性文件
 * @udev: 要创建 sysfs 文件的 USB 设备
 *
 * 在设备注册时由 usb_bus_notify 通知处理器调用。
 * 创建该设备所需的额外 sysfs 属性组（核心通用属性由
 * usb_device_groups[] 通过驱动模型自动创建）：
 *   - 持久化属性（persist）：控制系统挂起后是否保留设备配置
 *   - 电源管理属性（power）：autosuspend、connected_duration 等
 *   - 根集线器授权默认值（authorized_default / interface_authorized_default）：
 *     控制后续接入该根集线器端口的设备的默认授权策略
 *
 * 返回 0 表示成功，负值表示错误。失败时自动回滚已创建的属性。
 */
int usb_create_sysfs_dev_files(struct usb_device *udev)
{
	struct device *dev = &udev->dev;
	int retval;

	retval = add_persist_attributes(dev);
	if (retval)
		goto error;

	retval = add_power_attributes(dev);
	if (retval)
		goto error;

	if (is_root_hub(udev)) {
		retval = add_default_authorized_attributes(dev);
		if (retval)
			goto error;
	}
	return retval;

error:
	usb_remove_sysfs_dev_files(udev);
	return retval;
}

/*
 * usb_remove_sysfs_dev_files - 移除 USB 设备的 sysfs 属性文件
 * @udev: 要移除 sysfs 文件的 USB 设备
 *
 * 在设备注销时由 usb_bus_notify 通知处理器调用。
 * 移除 usb_create_sysfs_dev_files() 创建的所有属性组，
 * 与创建顺序相反（后创建的先移除）：
 *   1. 根集线器的授权默认属性
 *   2. 电源管理属性组（含 LPM 子组）
 *   3. 持久化属性
 */
void usb_remove_sysfs_dev_files(struct usb_device *udev)
{
	struct device *dev = &udev->dev;

	if (is_root_hub(udev))
		remove_default_authorized_attributes(dev);

	remove_power_attributes(dev);
	remove_persist_attributes(dev);
}

/* Interface Association Descriptor fields */
#define usb_intf_assoc_attr(field, format_string)			\
static ssize_t								\
iad_##field##_show(struct device *dev, struct device_attribute *attr,	\
		char *buf)						\
{									\
	struct usb_interface *intf = to_usb_interface(dev);		\
									\
	return sysfs_emit(buf, format_string,				\
			intf->intf_assoc->field); 			\
}									\
static DEVICE_ATTR_RO(iad_##field)

usb_intf_assoc_attr(bFirstInterface, "%02x\n");
usb_intf_assoc_attr(bInterfaceCount, "%02d\n");
usb_intf_assoc_attr(bFunctionClass, "%02x\n");
usb_intf_assoc_attr(bFunctionSubClass, "%02x\n");
usb_intf_assoc_attr(bFunctionProtocol, "%02x\n");

/* Interface fields */
#define usb_intf_attr(field, format_string)				\
static ssize_t								\
field##_show(struct device *dev, struct device_attribute *attr,		\
		char *buf)						\
{									\
	struct usb_interface *intf = to_usb_interface(dev);		\
									\
	return sysfs_emit(buf, format_string,				\
			intf->cur_altsetting->desc.field); 		\
}									\
static DEVICE_ATTR_RO(field)

usb_intf_attr(bInterfaceNumber, "%02x\n");
usb_intf_attr(bAlternateSetting, "%2d\n");
usb_intf_attr(bNumEndpoints, "%02x\n");
usb_intf_attr(bInterfaceClass, "%02x\n");
usb_intf_attr(bInterfaceSubClass, "%02x\n");
usb_intf_attr(bInterfaceProtocol, "%02x\n");

static ssize_t interface_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct usb_interface *intf;
	char *string;

	intf = to_usb_interface(dev);
	string = READ_ONCE(intf->cur_altsetting->string);
	if (!string)
		return 0;
	return sysfs_emit(buf, "%s\n", string);
}
static DEVICE_ATTR_RO(interface);

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct usb_interface *intf;
	struct usb_device *udev;
	struct usb_host_interface *alt;

	intf = to_usb_interface(dev);
	udev = interface_to_usbdev(intf);
	alt = READ_ONCE(intf->cur_altsetting);

	return sysfs_emit(buf,
			"usb:v%04Xp%04Xd%04Xdc%02Xdsc%02Xdp%02X"
			"ic%02Xisc%02Xip%02Xin%02X\n",
			le16_to_cpu(udev->descriptor.idVendor),
			le16_to_cpu(udev->descriptor.idProduct),
			le16_to_cpu(udev->descriptor.bcdDevice),
			udev->descriptor.bDeviceClass,
			udev->descriptor.bDeviceSubClass,
			udev->descriptor.bDeviceProtocol,
			alt->desc.bInterfaceClass,
			alt->desc.bInterfaceSubClass,
			alt->desc.bInterfaceProtocol,
			alt->desc.bInterfaceNumber);
}
static DEVICE_ATTR_RO(modalias);

static ssize_t supports_autosuspend_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	int s;

	s = device_lock_interruptible(dev);
	if (s < 0)
		return -EINTR;
	/* Devices will be autosuspended even when an interface isn't claimed */
	s = (!dev->driver || to_usb_driver(dev->driver)->supports_autosuspend);
	device_unlock(dev);

	return sysfs_emit(buf, "%u\n", s);
}
static DEVICE_ATTR_RO(supports_autosuspend);

/*
 * interface_authorized_show - show authorization status of an USB interface
 * 1 is authorized, 0 is deauthorized
 */
static ssize_t interface_authorized_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf = to_usb_interface(dev);

	return sysfs_emit(buf, "%u\n", intf->authorized);
}

/*
 * interface_authorized_store - authorize or deauthorize an USB interface
 */
static ssize_t interface_authorized_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct usb_interface *intf = to_usb_interface(dev);
	bool val;
	struct kernfs_node *kn;

	if (kstrtobool(buf, &val) != 0)
		return -EINVAL;

	if (val) {
		usb_authorize_interface(intf);
	} else {
		/*
		 * Prevent deadlock if another process is concurrently
		 * trying to unregister intf.
		 */
		kn = sysfs_break_active_protection(&dev->kobj, &attr->attr);
		if (kn) {
			usb_deauthorize_interface(intf);
			sysfs_unbreak_active_protection(kn);
		}
	}

	return count;
}
static struct device_attribute dev_attr_interface_authorized =
		__ATTR(authorized, S_IRUGO | S_IWUSR,
		interface_authorized_show, interface_authorized_store);

static struct attribute *intf_attrs[] = {
	&dev_attr_bInterfaceNumber.attr,
	&dev_attr_bAlternateSetting.attr,
	&dev_attr_bNumEndpoints.attr,
	&dev_attr_bInterfaceClass.attr,
	&dev_attr_bInterfaceSubClass.attr,
	&dev_attr_bInterfaceProtocol.attr,
	&dev_attr_modalias.attr,
	&dev_attr_supports_autosuspend.attr,
	&dev_attr_interface_authorized.attr,
	NULL,
};
static const struct attribute_group intf_attr_grp = {
	.attrs = intf_attrs,
};

static struct attribute *intf_assoc_attrs[] = {
	&dev_attr_iad_bFirstInterface.attr,
	&dev_attr_iad_bInterfaceCount.attr,
	&dev_attr_iad_bFunctionClass.attr,
	&dev_attr_iad_bFunctionSubClass.attr,
	&dev_attr_iad_bFunctionProtocol.attr,
	NULL,
};

static umode_t intf_assoc_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct usb_interface *intf = to_usb_interface(dev);

	if (intf->intf_assoc == NULL)
		return 0;
	return a->mode;
}

static const struct attribute_group intf_assoc_attr_grp = {
	.attrs =	intf_assoc_attrs,
	.is_visible =	intf_assoc_attrs_are_visible,
};

static ssize_t wireless_status_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct usb_interface *intf;

	intf = to_usb_interface(dev);
	if (intf->wireless_status == USB_WIRELESS_STATUS_DISCONNECTED)
		return sysfs_emit(buf, "%s\n", "disconnected");
	return sysfs_emit(buf, "%s\n", "connected");
}
static DEVICE_ATTR_RO(wireless_status);

static struct attribute *intf_wireless_status_attrs[] = {
	&dev_attr_wireless_status.attr,
	NULL
};

static umode_t intf_wireless_status_attr_is_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct usb_interface *intf = to_usb_interface(dev);

	if (a != &dev_attr_wireless_status.attr ||
	    intf->wireless_status != USB_WIRELESS_STATUS_NA)
		return a->mode;
	return 0;
}

static const struct attribute_group intf_wireless_status_attr_grp = {
	.attrs =	intf_wireless_status_attrs,
	.is_visible =	intf_wireless_status_attr_is_visible,
};

int usb_update_wireless_status_attr(struct usb_interface *intf)
{
	struct device *dev = &intf->dev;
	int ret;

	ret = sysfs_update_group(&dev->kobj, &intf_wireless_status_attr_grp);
	if (ret < 0)
		return ret;

	sysfs_notify(&dev->kobj, NULL, "wireless_status");
	kobject_uevent(&dev->kobj, KOBJ_CHANGE);

	return 0;
}

const struct attribute_group *usb_interface_groups[] = {
	&intf_attr_grp,
	&intf_assoc_attr_grp,
	&intf_wireless_status_attr_grp,
	NULL
};

/*
 * usb_create_sysfs_intf_files - 创建 USB 接口的 sysfs 属性文件
 * @intf: 要创建 sysfs 文件的 USB 接口
 *
 * 在 USB 接口注册时由 usb_bus_notify 通知处理器调用。
 * 本函数主要检查并创建 interface 字符串属性（来自接口描述符的 iInterface 字段）。
 *
 * 如果设备未提供接口描述字符串，或标记有 USB_QUIRK_CONFIG_INTF_STRINGS 标志，
 * 则不会尝试读取字符串描述符，也不会创建 interface 属性文件。
 * 接口的标准属性（bInterfaceNumber、modalias 等）由 usb_interface_groups[]
 * 自动创建，无需在此处理。
 *
 * 如果 intf 已经在注销过程中（unregistering 标志已设置），则跳过创建。
 */
void usb_create_sysfs_intf_files(struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt = intf->cur_altsetting;

	if (intf->sysfs_files_created || intf->unregistering)
		return;

	if (!alt->string && !(udev->quirks & USB_QUIRK_CONFIG_INTF_STRINGS))
		alt->string = usb_cache_string(udev, alt->desc.iInterface);
	if (alt->string && device_create_file(&intf->dev, &dev_attr_interface)) {
		/* This is not a serious error */
		dev_dbg(&intf->dev, "interface string descriptor file not created\n");
	}
	intf->sysfs_files_created = 1;
}

/*
 * usb_remove_sysfs_intf_files - 移除 USB 接口的 sysfs 属性文件
 * @intf: 要移除 sysfs 文件的 USB 接口
 *
 * 在 USB 接口注销时由 usb_bus_notify 通知处理器调用。
 * 移除 usb_create_sysfs_intf_files() 创建的 interface 字符串属性文件。
 * 如果 intf 的 sysfs_files_created 标志未设置，说明从未创建过，则直接返回。
 */
void usb_remove_sysfs_intf_files(struct usb_interface *intf)
{
	if (!intf->sysfs_files_created)
		return;

	device_remove_file(&intf->dev, &dev_attr_interface);
	intf->sysfs_files_created = 0;
}
