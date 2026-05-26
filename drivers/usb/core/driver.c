// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/usb/core/driver.c - most of the driver model stuff for usb
 *
 * (C) Copyright 2005 Greg Kroah-Hartman <gregkh@suse.de>
 *
 * based on drivers/usb/usb.c which had the following copyrights:
 *	(C) Copyright Linus Torvalds 1999
 *	(C) Copyright Johannes Erdfelt 1999-2001
 *	(C) Copyright Andreas Gal 1999
 *	(C) Copyright Gregory P. Smith 1999
 *	(C) Copyright Deti Fliegl 1999 (new USB architecture)
 *	(C) Copyright Randy Dunlap 2000
 *	(C) Copyright David Brownell 2000-2004
 *	(C) Copyright Yggdrasil Computing, Inc. 2000
 *		(usb_device_id matching changes by Adam J. Richter)
 *	(C) Copyright Greg Kroah-Hartman 2002-2003
 *
 * Released under the GPLv2 only.
 *
 * NOTE! This is not actually a driver at all, rather this is
 * just a collection of helper routines that implement the
 * matching, probing, releasing, suspending and resuming for
 * real drivers.
 *
 */

/*
 * driver.c -- USB 设备/接口驱动绑定核心
 *
 * 本文件实现了 USB 子系统与 Linux 设备驱动模型 (device driver model)
 * 之间的核心桥接层。它不是一个真正的驱动，而是一组辅助例程的集合，
 * 负责处理 USB 设备/接口与驱动之间的匹配(matching)、探测(probe)、
 * 断开(disconnect)、挂起/恢复(suspend/resume)以及运行时 PM 管理。
 *
 * ==================== 两层匹配架构 ====================
 * USB 驱动模型在 Linux 设备模型中存在两个层次的匹配:
 *
 *   [1] 设备级匹配 (struct usb_device_driver)
 *      匹配目标是 struct usb_device 本身。设备级驱动注册时其 probe
 *      回调被设为 usb_probe_device。匹配通过 usb_device_match_id()
 *      比较 usb_device_id 表中的 idVendor/idProduct/bcdDevice/
 *      bDeviceClass 等设备描述符字段。关键的特殊设备级驱动是
 *      usb_generic_driver, 它不匹配特定 VID/PID, 而是作为"万能回退"
 *      驱动, 负责为每个 USB 设备创建 usbfs 设备节点、管理配置选择
 *      以及协调接口级驱动的绑定。
 *
 *   [2] 接口级匹配 (struct usb_driver)
 *      匹配目标是 struct usb_interface。绝大多数 USB 功能驱动
 *      (HID、Mass Storage、CDC ACM、音频、视频等)都属于这一层。
 *      接口级匹配通过 usb_match_id() 比较接口描述符中的
 *      bInterfaceClass/bInterfaceSubClass/bInterfaceProtocol 等字段。
 *      接口级驱动的 probe 回调被设为 usb_probe_interface。
 *
 *  调用路径: 当 Linux 设备核心注册新设备或新驱动时, bus_type.match
 *  回调(即 usb_device_match())被自动调用。它根据 dev 的类型分派:
 *    - dev 是 usb_device     → 走设备级匹配(usb_device_driver)
 *    - dev 是 usb_interface  → 走接口级匹配(usb_driver)
 *
 * ==================== probe 生命周期 ====================
 *   usb_new_device()
 *     → usb_probe_device()             [设备级 probe]
 *       → usb_generic_driver_probe()   [通用层: 配置解析 + 接口注册]
 *         → usb_probe_interface()      [接口级 probe, 对每个接口]
 *           → driver->probe(intf, id)  [具体驱动 probe 回调]
 *
 *   usb_disconnect() 和 usb_unbind_interface() 完成对称的清理流程。
 *
 * ==================== 关键子系统 ====================
 *  - unbind/bind sysfs: 通过 new_id / remove_id sysfs 属性文件,
 *    用户空间可动态添加或移除 USB 设备 ID, 触发 driver_attach()
 *    重新探测, 实现无需重启的热插拔驱动绑定。
 *
 *  - 接口声明(claim): usb_driver_claim_interface() 允许一个驱动
 *    在 probe() 中"认领"同一设备上的多个接口(如音频设备需要同时
 *    绑定控制接口和数据接口), 绕过标准 match→probe 流程直接绑定。
 *
 *  - autosuspend 集成: probe 时根据驱动是否声明 supports_autosuspend
 *    来决定如何初始化运行时 PM。支持 autosuspend 的驱动启用 runtime
 *    PM 并允许设备在空闲时自动挂起; 不支持的则保持设备活跃以防止
 *    意外挂起。
 *
 *  - 强制解绑与重绑定: 当 reset_resume 或 suspend/resume 不被支持时,
 *    usb_forced_unbind_intf() + usb_rebind_intf() 机制先解绑接口,
 *    然后在系统 resume 完成后重新探测驱动。
 *
 * ==================== 相关文件 ====================
 *  - drivers/usb/core/generic.c : usb_generic_driver 的具体实现
 *  - drivers/usb/core/usb.h     : USB 核心内部头文件
 *  - include/linux/usb.h        : USB 子系统公共头文件
 *  - include/linux/device.h     : Linux 设备模型核心接口
 */

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/usb.h>
#include <linux/usb/quirks.h>
#include <linux/usb/hcd.h>

#include "usb.h"


/*
 * Adds a new dynamic USBdevice ID to this driver,
 * and cause the driver to probe for all devices again.
 */
ssize_t usb_store_new_id(struct usb_dynids *dynids,
			 const struct usb_device_id *id_table,
			 struct device_driver *driver,
			 const char *buf, size_t count)
{
	struct usb_dynid *dynid;
	u32 idVendor = 0;
	u32 idProduct = 0;
	unsigned int bInterfaceClass = 0;
	u32 refVendor, refProduct;
	int fields = 0;
	int retval = 0;

	fields = sscanf(buf, "%x %x %x %x %x", &idVendor, &idProduct,
			&bInterfaceClass, &refVendor, &refProduct);
	if (fields < 2)
		return -EINVAL;

	dynid = kzalloc_obj(*dynid);
	if (!dynid)
		return -ENOMEM;

	INIT_LIST_HEAD(&dynid->node);
	dynid->id.idVendor = idVendor;
	dynid->id.idProduct = idProduct;
	dynid->id.match_flags = USB_DEVICE_ID_MATCH_DEVICE;
	if (fields > 2 && bInterfaceClass) {
		if (bInterfaceClass > 255) {
			retval = -EINVAL;
			goto fail;
		}

		dynid->id.bInterfaceClass = (u8)bInterfaceClass;
		dynid->id.match_flags |= USB_DEVICE_ID_MATCH_INT_CLASS;
	}

	if (fields > 4) {
		const struct usb_device_id *id = id_table;

		if (!id) {
			retval = -ENODEV;
			goto fail;
		}

		for (; id->match_flags; id++)
			if (id->idVendor == refVendor && id->idProduct == refProduct)
				break;

		if (id->match_flags) {
			dynid->id.driver_info = id->driver_info;
		} else {
			retval = -ENODEV;
			goto fail;
		}
	}

	mutex_lock(&usb_dynids_lock);
	list_add_tail(&dynid->node, &dynids->list);
	mutex_unlock(&usb_dynids_lock);

	retval = driver_attach(driver);

	if (retval)
		return retval;
	return count;

fail:
	kfree(dynid);
	return retval;
}
EXPORT_SYMBOL_GPL(usb_store_new_id);

ssize_t usb_show_dynids(struct usb_dynids *dynids, char *buf)
{
	struct usb_dynid *dynid;
	size_t count = 0;

	guard(mutex)(&usb_dynids_lock);
	list_for_each_entry(dynid, &dynids->list, node)
		if (dynid->id.bInterfaceClass != 0)
			count += sysfs_emit_at(buf, count, "%04x %04x %02x\n",
					   dynid->id.idVendor, dynid->id.idProduct,
					   dynid->id.bInterfaceClass);
		else
			count += sysfs_emit_at(buf, count, "%04x %04x\n",
					   dynid->id.idVendor, dynid->id.idProduct);
	return count;
}
EXPORT_SYMBOL_GPL(usb_show_dynids);

static ssize_t new_id_show(struct device_driver *driver, char *buf)
{
	struct usb_driver *usb_drv = to_usb_driver(driver);

	return usb_show_dynids(&usb_drv->dynids, buf);
}

static ssize_t new_id_store(struct device_driver *driver,
			    const char *buf, size_t count)
{
	struct usb_driver *usb_drv = to_usb_driver(driver);

	return usb_store_new_id(&usb_drv->dynids, usb_drv->id_table, driver, buf, count);
}
static DRIVER_ATTR_RW(new_id);

/*
 * Remove a USB device ID from this driver
 */
static ssize_t remove_id_store(struct device_driver *driver, const char *buf,
			       size_t count)
{
	struct usb_dynid *dynid, *n;
	struct usb_driver *usb_driver = to_usb_driver(driver);
	u32 idVendor;
	u32 idProduct;
	int fields;

	fields = sscanf(buf, "%x %x", &idVendor, &idProduct);
	if (fields < 2)
		return -EINVAL;

	guard(mutex)(&usb_dynids_lock);
	list_for_each_entry_safe(dynid, n, &usb_driver->dynids.list, node) {
		struct usb_device_id *id = &dynid->id;

		if ((id->idVendor == idVendor) &&
		    (id->idProduct == idProduct)) {
			list_del(&dynid->node);
			kfree(dynid);
			break;
		}
	}
	return count;
}

static ssize_t remove_id_show(struct device_driver *driver, char *buf)
{
	return new_id_show(driver, buf);
}
static DRIVER_ATTR_RW(remove_id);

static int usb_create_newid_files(struct usb_driver *usb_drv)
{
	int error = 0;

	if (usb_drv->no_dynamic_id)
		goto exit;

	if (usb_drv->probe != NULL) {
		error = driver_create_file(&usb_drv->driver,
					   &driver_attr_new_id);
		if (error == 0) {
			error = driver_create_file(&usb_drv->driver,
					&driver_attr_remove_id);
			if (error)
				driver_remove_file(&usb_drv->driver,
						&driver_attr_new_id);
		}
	}
exit:
	return error;
}

static void usb_remove_newid_files(struct usb_driver *usb_drv)
{
	if (usb_drv->no_dynamic_id)
		return;

	if (usb_drv->probe != NULL) {
		driver_remove_file(&usb_drv->driver,
				&driver_attr_remove_id);
		driver_remove_file(&usb_drv->driver,
				   &driver_attr_new_id);
	}
}

static void usb_free_dynids(struct usb_driver *usb_drv)
{
	struct usb_dynid *dynid, *n;

	guard(mutex)(&usb_dynids_lock);
	list_for_each_entry_safe(dynid, n, &usb_drv->dynids.list, node) {
		list_del(&dynid->node);
		kfree(dynid);
	}
}

static const struct usb_device_id *usb_match_dynamic_id(struct usb_interface *intf,
							const struct usb_driver *drv)
{
	struct usb_dynid *dynid;

	guard(mutex)(&usb_dynids_lock);
	list_for_each_entry(dynid, &drv->dynids.list, node) {
		if (usb_match_one_id(intf, &dynid->id)) {
			return &dynid->id;
		}
	}
	return NULL;
}


/*
 * usb_probe_device - USB 设备级 probe 入口
 *
 * 由 Linux 设备模型核心在驱动匹配成功后调用。这是 USB 设备级驱动
 * (usb_device_driver) 的 probe 分发点。
 *
 * 处理流程:
 *   1) 如果驱动不支持 autosuspend, 先调用 usb_autoresume_device()
 *      增加设备的引用计数, 防止设备在 probe 过程中被意外挂起。
 *   2) 如果驱动设置了 generic_subclass 标志(即它是 usb_generic_driver
 *      的子类), 先调用 usb_generic_driver_probe() 执行通用 probe 逻辑
 *      (配置解析、接口注册等)。
 *   3) 调用驱动特定的 udriver->probe(udev) 回调。
 *   4) 如果特定驱动返回 -ENODEV, 且该驱动拥有 id_table 或 match 函数,
 *      则标记 use_generic_driver = 1 并返回 -EPROBE_DEFER, 以便后续
 *      由 usb_generic_driver 重新探测。
 *
 * 关键设计: usb_generic_driver 是 USB 子系统的"万能回退"驱动。当所有
 * 专用设备级驱动都无法匹配时, 系统会最终尝试 usb_generic_driver。该
 * 驱动负责创建设备文件系统节点 (usbfs) 和进行基本的配置管理。
 *
 * 注意: usb_generic_driver 本身也是一个 usb_device_driver, 它的 probe
 * 回调也是 usb_probe_device, 但它的 generic_subclass 标志为 false,
 * 因此不会递归调用 usb_generic_driver_probe。
 */
/* called from driver core with dev locked */
static int usb_probe_device(struct device *dev)
{
	struct usb_device_driver *udriver = to_usb_device_driver(dev->driver);
	struct usb_device *udev = to_usb_device(dev);
	int error = 0;

	dev_dbg(dev, "%s\n", __func__);

	/* TODO: Add real matching code */

	/* The device should always appear to be in use
	 * unless the driver supports autosuspend.
	 */
	if (!udriver->supports_autosuspend)
		error = usb_autoresume_device(udev);
	if (error)
		return error;

	if (udriver->generic_subclass)
		error = usb_generic_driver_probe(udev);
	if (error)
		return error;

	/* Probe the USB device with the driver in hand, but only
	 * defer to a generic driver in case the current USB
	 * device driver has an id_table or a match function; i.e.,
	 * when the device driver was explicitly matched against
	 * a device.
	 *
	 * If the device driver does not have either of these,
	 * then we assume that it can bind to any device and is
	 * not truly a more specialized/non-generic driver, so a
	 * return value of -ENODEV should not force the device
	 * to be handled by the generic USB driver, as there
	 * can still be another, more specialized, device driver.
	 *
	 * This accommodates the usbip driver.
	 *
	 * TODO: What if, in the future, there are multiple
	 * specialized USB device drivers for a particular device?
	 * In such cases, there is a need to try all matching
	 * specialised device drivers prior to setting the
	 * use_generic_driver bit.
	 */
	if (udriver->probe)
		error = udriver->probe(udev);
	else if (!udriver->generic_subclass)
		error = -EINVAL;
	if (error == -ENODEV && udriver != &usb_generic_driver &&
	    (udriver->id_table || udriver->match)) {
		udev->use_generic_driver = 1;
		return -EPROBE_DEFER;
	}
	return error;
}

/*
 * usb_unbind_device - USB 设备级解绑
 *
 * 由 Linux 设备模型核心在驱动断开时调用。是 usb_probe_device 的逆操作。
 *
 * 处理流程:
 *   1) 调用 udriver->disconnect(udev) 通知驱动设备已断开。
 *   2) 如果驱动是 generic_subclass (如 usb_generic_driver 的子类),
 *      调用 usb_generic_driver_disconnect() 执行通用清理。
 *   3) 如果驱动不支持 autosuspend, 调用 usb_autosuspend_device()
 *      减少设备的引用计数, 恢复正常的 autosuspend 状态。
 */
/* called from driver core with dev locked */
static int usb_unbind_device(struct device *dev)
{
	struct usb_device *udev = to_usb_device(dev);
	struct usb_device_driver *udriver = to_usb_device_driver(dev->driver);

	if (udriver->disconnect)
		udriver->disconnect(udev);
	if (udriver->generic_subclass)
		usb_generic_driver_disconnect(udev);
	if (!udriver->supports_autosuspend)
		usb_autosuspend_device(udev);
	return 0;
}

/*
 * usb_probe_interface - USB 接口级 probe 入口
 *
 * 由 Linux 设备模型核心在 usb_driver 匹配接口成功后调用。这是绝大多数
 * USB 功能驱动(HID、Mass Storage、CDC ACM等)的 probe 分发点。
 *
 * 处理流程:
 *   1) 检查设备/接口的授权状态, 未授权的接口拒绝绑定。
 *   2) 通过 usb_match_dynamic_id() 和 usb_match_id() 查找匹配的
 *      usb_device_id 条目。
 *   3) 调用 usb_autoresume_device() 防止 probe 过程中设备被挂起。
 *   4) 设置接口状态为 USB_INTERFACE_BINDING, 初始化运行时 PM。
 *   5) 如果驱动要求禁用 LPM(disable_hub_initiated_lpm), 则尝试禁用之。
 *   6) 如果之前有延迟的 altsetting 0 切换请求(intf->needs_altsetting0),
 *      在此执行 usb_set_interface()。
 *   7) 调用 driver->probe(intf, id) 执行具体的驱动 probe。
 *   8) 成功后接口状态设为 USB_INTERFACE_BOUND, 失败则回退到
 *      USB_INTERFACE_UNBOUND。
 *
 * 授权检查: USB 核心支持设备级和接口级的授权控制。未授权的设备或接口
 * 不会被任何驱动绑定, 这是 USB 安全模型的一部分(Linux USB 授权框架)。
 */
/* called from driver core with dev locked */
static int usb_probe_interface(struct device *dev)
{
	struct usb_driver *driver = to_usb_driver(dev->driver);
	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct usb_device_id *id;
	int error = -ENODEV;
	int lpm_disable_error = -ENODEV;

	dev_dbg(dev, "%s\n", __func__);

	intf->needs_binding = 0;

	if (usb_device_is_owned(udev))
		return error;

	if (udev->authorized == 0) {
		dev_info(&intf->dev, "Device is not authorized for usage\n");
		return error;
	} else if (intf->authorized == 0) {
		dev_info(&intf->dev, "Interface %d is not authorized for usage\n",
				intf->altsetting->desc.bInterfaceNumber);
		return error;
	}

	id = usb_match_dynamic_id(intf, driver);
	if (!id)
		id = usb_match_id(intf, driver->id_table);
	if (!id)
		return error;

	dev_dbg(dev, "%s - got id\n", __func__);

	error = usb_autoresume_device(udev);
	if (error)
		return error;

	intf->condition = USB_INTERFACE_BINDING;

	/* Probed interfaces are initially active.  They are
	 * runtime-PM-enabled only if the driver has autosuspend support.
	 * They are sensitive to their children's power states.
	 */
	pm_runtime_set_active(dev);
	pm_suspend_ignore_children(dev, false);
	if (driver->supports_autosuspend)
		pm_runtime_enable(dev);

	/* If the new driver doesn't allow hub-initiated LPM, and we can't
	 * disable hub-initiated LPM, then fail the probe.
	 *
	 * Otherwise, leaving LPM enabled should be harmless, because the
	 * endpoint intervals should remain the same, and the U1/U2 timeouts
	 * should remain the same.
	 *
	 * If we need to install alt setting 0 before probe, or another alt
	 * setting during probe, that should also be fine.  usb_set_interface()
	 * will attempt to disable LPM, and fail if it can't disable it.
	 */
	if (driver->disable_hub_initiated_lpm) {
		lpm_disable_error = usb_unlocked_disable_lpm(udev);
		if (lpm_disable_error) {
			dev_err(&intf->dev, "%s Failed to disable LPM for driver %s\n",
				__func__, driver->name);
			error = lpm_disable_error;
			goto err;
		}
	}

	/* Carry out a deferred switch to altsetting 0 */
	if (intf->needs_altsetting0) {
		error = usb_set_interface(udev, intf->altsetting[0].
				desc.bInterfaceNumber, 0);
		if (error < 0)
			goto err;
		intf->needs_altsetting0 = 0;
	}

	error = driver->probe(intf, id);
	if (error)
		goto err;

	intf->condition = USB_INTERFACE_BOUND;

	/* If the LPM disable succeeded, balance the ref counts. */
	if (!lpm_disable_error)
		usb_unlocked_enable_lpm(udev);

	usb_autosuspend_device(udev);
	return error;

 err:
	usb_set_intfdata(intf, NULL);
	intf->needs_remote_wakeup = 0;
	intf->condition = USB_INTERFACE_UNBOUND;

	/* If the LPM disable succeeded, balance the ref counts. */
	if (!lpm_disable_error)
		usb_unlocked_enable_lpm(udev);

	/* Unbound interfaces are always runtime-PM-disabled and -suspended */
	if (driver->supports_autosuspend)
		pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

	usb_autosuspend_device(udev);
	return error;
}

/*
 * usb_unbind_interface - USB 接口级解绑
 *
 * 由 Linux 设备模型核心在驱动与接口断开时调用。是 usb_probe_interface
 * 的逆操作, 执行完整的接口清理流程。
 *
 * 处理流程:
 *   1) 设置接口状态为 USB_INTERFACE_UNBINDING, 防止并发操作。
 *   2) 调用 usb_autoresume_device() 确保设备在解绑过程中处于活跃状态。
 *   3) 如果驱动要求禁用 LPM, 尝试禁用。
 *   4) 除非驱动声明了 soft_unbind 且设备仍存在, 否则终止接口上所有
 *      未完成的 URB (usb_disable_interface) --- 这是为了防止驱动
 *      在 disconnect 后还有未完成的 URB 回调访问已释放的数据。
 *   5) 调用 driver->disconnect(intf) 通知驱动执行清理。
 *   6) 释放流资源(usb_free_streams), 每个关联了 USB 流(批量流)的端点
 *      都需要单独释放。
 *   7) 恢复 altsetting 0: 如果接口已在 altsetting 0 则重新使能;
 *      否则执行 Set-Interface 回退。如果设备处于挂起状态或正在
 *      系统休眠转换中, 则延迟切换 (needs_altsetting0 = 1)。
 *   8) 清理接口数据, 设置接口状态为 USB_INTERFACE_UNBOUND。
 *   9) 重新使能 LPM, 恢复运行时 PM 状态。
 *
 * soft_unbind: 支持 soft_unbind 的驱动允许在设备仍存在时跳过 URB 终止,
 * 这对于某些需要优雅断开、不丢失数据的场景(如 usbnet)非常有用。
 */
/* called from driver core with dev locked */
static int usb_unbind_interface(struct device *dev)
{
	struct usb_driver *driver = to_usb_driver(dev->driver);
	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_host_endpoint *ep, **eps = NULL;
	struct usb_device *udev;
	int i, j, error, r;
	int lpm_disable_error = -ENODEV;

	intf->condition = USB_INTERFACE_UNBINDING;

	/* Autoresume for set_interface call below */
	udev = interface_to_usbdev(intf);
	error = usb_autoresume_device(udev);

	/* If hub-initiated LPM policy may change, attempt to disable LPM until
	 * the driver is unbound.  If LPM isn't disabled, that's fine because it
	 * wouldn't be enabled unless all the bound interfaces supported
	 * hub-initiated LPM.
	 */
	if (driver->disable_hub_initiated_lpm)
		lpm_disable_error = usb_unlocked_disable_lpm(udev);

	/*
	 * Terminate all URBs for this interface unless the driver
	 * supports "soft" unbinding and the device is still present.
	 */
	if (!driver->soft_unbind || udev->state == USB_STATE_NOTATTACHED)
		usb_disable_interface(udev, intf, false);

	driver->disconnect(intf);

	/* Free streams */
	for (i = 0, j = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		ep = &intf->cur_altsetting->endpoint[i];
		if (ep->streams == 0)
			continue;
		if (j == 0) {
			eps = kmalloc_array(USB_MAXENDPOINTS, sizeof(void *),
				      GFP_KERNEL);
			if (!eps)
				break;
		}
		eps[j++] = ep;
	}
	if (j) {
		usb_free_streams(intf, eps, j, GFP_KERNEL);
		kfree(eps);
	}

	/* Reset other interface state.
	 * We cannot do a Set-Interface if the device is suspended or
	 * if it is prepared for a system sleep (since installing a new
	 * altsetting means creating new endpoint device entries).
	 * When either of these happens, defer the Set-Interface.
	 */
	if (intf->cur_altsetting->desc.bAlternateSetting == 0) {
		/* Already in altsetting 0 so skip Set-Interface.
		 * Just re-enable it without affecting the endpoint toggles.
		 */
		usb_enable_interface(udev, intf, false);
	} else if (!error && !intf->dev.power.is_prepared) {
		r = usb_set_interface(udev, intf->altsetting[0].
				desc.bInterfaceNumber, 0);
		if (r < 0)
			intf->needs_altsetting0 = 1;
	} else {
		intf->needs_altsetting0 = 1;
	}
	usb_set_intfdata(intf, NULL);

	intf->condition = USB_INTERFACE_UNBOUND;
	intf->needs_remote_wakeup = 0;

	/* Attempt to re-enable USB3 LPM, if the disable succeeded. */
	if (!lpm_disable_error)
		usb_unlocked_enable_lpm(udev);

	/* Unbound interfaces are always runtime-PM-disabled and -suspended */
	if (driver->supports_autosuspend)
		pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

	if (!error)
		usb_autosuspend_device(udev);

	return 0;
}

static void usb_shutdown_interface(struct device *dev)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_driver *driver;

	if (!dev->driver)
		return;

	driver = to_usb_driver(dev->driver);
	if (driver->shutdown)
		driver->shutdown(intf);
}

/*
 * usb_driver_claim_interface - 驱动声明(claim)绑定一个接口
 *
 * 这是接口声明机制的实现。允许一个驱动在 probe() 中主动"认领"
 * 同一个 USB 设备上的额外接口, 绕过标准 match→probe 流程。
 *
 * 典型用例: 音频设备 (audio) 和 CDC ACM 设备, 它们的一个物理功能
 * 需要跨多个接口配合工作。驱动 probe 第一个接口时, 通过此函数
 * 主动绑定附加接口。
 *
 * 与标准 probe 的区别:
 *   - 标准流程: 设备核心发现接口 → 调用 usb_device_match() →
 *     usb_probe_interface() → driver->probe()
 *   - Claim 流程: 驱动主动调用 → device_bind_driver() 直接绑定
 *
 * 调用者必须持有设备锁 (device lock)。在驱动 probe() 中调用时
 * 锁已被持有, 无需额外操作; 在其他上下文中调用时需显式获取锁。
 *
 * 返回: 0 表示成功, -EBUSY 表示接口已被其他驱动绑定
 */
/**
 * usb_driver_claim_interface - bind a driver to an interface
 * @driver: the driver to be bound
 * @iface: the interface to which it will be bound; must be in the
 *	usb device's active configuration
 * @data: driver data associated with that interface
 *
 * This is used by usb device drivers that need to claim more than one
 * interface on a device when probing (audio and acm are current examples).
 * No device driver should directly modify internal usb_interface or
 * usb_device structure members.
 *
 * Callers must own the device lock, so driver probe() entries don't need
 * extra locking, but other call contexts may need to explicitly claim that
 * lock.
 *
 * Return: 0 on success.
 */
int usb_driver_claim_interface(struct usb_driver *driver,
				struct usb_interface *iface, void *data)
{
	struct device *dev;
	int retval = 0;

	if (!iface)
		return -ENODEV;

	dev = &iface->dev;
	if (dev->driver)
		return -EBUSY;

	/* reject claim if interface is not authorized */
	if (!iface->authorized)
		return -ENODEV;

	dev->driver = &driver->driver;
	usb_set_intfdata(iface, data);
	iface->needs_binding = 0;

	iface->condition = USB_INTERFACE_BOUND;

	/* Claimed interfaces are initially inactive (suspended) and
	 * runtime-PM-enabled, but only if the driver has autosuspend
	 * support.  Otherwise they are marked active, to prevent the
	 * device from being autosuspended, but left disabled.  In either
	 * case they are sensitive to their children's power states.
	 */
	pm_suspend_ignore_children(dev, false);
	if (driver->supports_autosuspend)
		pm_runtime_enable(dev);
	else
		pm_runtime_set_active(dev);

	/* if interface was already added, bind now; else let
	 * the future device_add() bind it, bypassing probe()
	 */
	if (device_is_registered(dev))
		retval = device_bind_driver(dev);

	if (retval) {
		dev->driver = NULL;
		usb_set_intfdata(iface, NULL);
		iface->needs_remote_wakeup = 0;
		iface->condition = USB_INTERFACE_UNBOUND;

		/*
		 * Unbound interfaces are always runtime-PM-disabled
		 * and runtime-PM-suspended
		 */
		if (driver->supports_autosuspend)
			pm_runtime_disable(dev);
		pm_runtime_set_suspended(dev);
	}

	return retval;
}
EXPORT_SYMBOL_GPL(usb_driver_claim_interface);

/*
 * usb_driver_release_interface - 驱动释放(release)一个接口
 *
 * 与 usb_driver_claim_interface 对应的逆操作。允许驱动在无需等待
 * disconnect() 调用的情况下主动释放之前声明的接口。
 *
 * 处理流程:
 *   1) 检查接口是否确实被本驱动持有, 防止错误释放。
 *   2) 检查接口状态是否为 USB_INTERFACE_BOUND, 防止在 disconnect()
 *      中重复释放。
 *   3) 如果接口已注册到设备模型, 通过 device_release_driver() 走
 *      标准驱动核心释放路径(会调用 usb_unbind_interface)。
 *   4) 如果接口尚未注册, 直接调用 usb_unbind_interface() 手动清理。
 *
 * 注意: 此调用是同步的, 不能在中断上下文中使用。调用者必须持有
 * 设备锁。
 */
/**
 * usb_driver_release_interface - unbind a driver from an interface
 * @driver: the driver to be unbound
 * @iface: the interface from which it will be unbound
 *
 * This can be used by drivers to release an interface without waiting
 * for their disconnect() methods to be called.  In typical cases this
 * also causes the driver disconnect() method to be called.
 *
 * This call is synchronous, and may not be used in an interrupt context.
 * Callers must own the device lock, so driver disconnect() entries don't
 * need extra locking, but other call contexts may need to explicitly claim
 * that lock.
 */
void usb_driver_release_interface(struct usb_driver *driver,
					struct usb_interface *iface)
{
	struct device *dev = &iface->dev;

	/* this should never happen, don't release something that's not ours */
	if (!dev->driver || dev->driver != &driver->driver)
		return;

	/* don't release from within disconnect() */
	if (iface->condition != USB_INTERFACE_BOUND)
		return;
	iface->condition = USB_INTERFACE_UNBINDING;

	/* Release via the driver core only if the interface
	 * has already been registered
	 */
	if (device_is_registered(dev)) {
		device_release_driver(dev);
	} else {
		device_lock(dev);
		usb_unbind_interface(dev);
		dev->driver = NULL;
		device_unlock(dev);
	}
}
EXPORT_SYMBOL_GPL(usb_driver_release_interface);

/*
 * usb_match_device - 用 usb_device_id 匹配 USB 设备描述符
 *
 * 这是设备级匹配的核心函数。它将 usb_device_id 中的各个字段与
 * USB 设备描述符(device descriptor)中的对应字段逐项比较。
 *
 * 匹配通过 match_flags 位掩码控制:
 *  - USB_DEVICE_ID_MATCH_VENDOR   : 比较 idVendor
 *  - USB_DEVICE_ID_MATCH_PRODUCT  : 比较 idProduct
 *  - USB_DEVICE_ID_MATCH_DEV_LO   : 比较 bcdDevice 下限
 *  - USB_DEVICE_ID_MATCH_DEV_HI   : 比较 bcdDevice 上限
 *  - USB_DEVICE_ID_MATCH_DEV_CLASS: 比较 bDeviceClass
 *  - 等等
 *
 * 每个匹配标志位独立控制对应的字段比较。只有当 match_flags 中
 * 设置了某标志位时, 相应的字段比较才会执行。如果任何已启用的
 * 比较失败, 函数立即返回 0 (不匹配)。所有已启用的比较都通过
 * 则返回 1 (匹配)。
 *
 * 返回: 0 = 不匹配, 1 = 匹配
 */
/* returns 0 if no match, 1 if match */
int usb_match_device(struct usb_device *dev, const struct usb_device_id *id)
{
	if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
	    id->idVendor != le16_to_cpu(dev->descriptor.idVendor))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) &&
	    id->idProduct != le16_to_cpu(dev->descriptor.idProduct))
		return 0;

	/* No need to test id->bcdDevice_lo != 0, since 0 is never
	   greater than any unsigned number. */
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_LO) &&
	    (id->bcdDevice_lo > le16_to_cpu(dev->descriptor.bcdDevice)))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_HI) &&
	    (id->bcdDevice_hi < le16_to_cpu(dev->descriptor.bcdDevice)))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) &&
	    (id->bDeviceClass != dev->descriptor.bDeviceClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) &&
	    (id->bDeviceSubClass != dev->descriptor.bDeviceSubClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) &&
	    (id->bDeviceProtocol != dev->descriptor.bDeviceProtocol))
		return 0;

	return 1;
}

/*
 * usb_match_one_id_intf - 用 usb_device_id 匹配 USB 接口描述符
 *
 * 这是接口级匹配的第二步, 在 usb_match_device() 通过之后被调用,
 * 用于匹配合适的接口描述符字段。
 *
 * 本函数实现了一条重要的 USB 规范规则:
 *   如果设备的 bDeviceClass 是 USB_CLASS_VENDOR_SPEC (0xFF),
 *   则接口的 class/subclass/protocol 匹配将被跳过, 除非匹配记录
 *   同时指定了 Vendor ID。这是因为厂商特定设备的接口类含义也是
 *   厂商定义的, 不能按标准类解释。
 *
 * 匹配字段包括:
 *  - USB_DEVICE_ID_MATCH_INT_CLASS     : bInterfaceClass
 *  - USB_DEVICE_ID_MATCH_INT_SUBCLASS  : bInterfaceSubClass
 *  - USB_DEVICE_ID_MATCH_INT_PROTOCOL  : bInterfaceProtocol
 *  - USB_DEVICE_ID_MATCH_INT_NUMBER    : bInterfaceNumber
 *
 * 返回: 0 = 不匹配, 1 = 匹配
 */
/* returns 0 if no match, 1 if match */
int usb_match_one_id_intf(struct usb_device *dev,
			  struct usb_host_interface *intf,
			  const struct usb_device_id *id)
{
	/* The interface class, subclass, protocol and number should never be
	 * checked for a match if the device class is Vendor Specific,
	 * unless the match record specifies the Vendor ID. */
	if (dev->descriptor.bDeviceClass == USB_CLASS_VENDOR_SPEC &&
			!(id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
			(id->match_flags & (USB_DEVICE_ID_MATCH_INT_CLASS |
				USB_DEVICE_ID_MATCH_INT_SUBCLASS |
				USB_DEVICE_ID_MATCH_INT_PROTOCOL |
				USB_DEVICE_ID_MATCH_INT_NUMBER)))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS) &&
	    (id->bInterfaceClass != intf->desc.bInterfaceClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) &&
	    (id->bInterfaceSubClass != intf->desc.bInterfaceSubClass))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) &&
	    (id->bInterfaceProtocol != intf->desc.bInterfaceProtocol))
		return 0;

	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_NUMBER) &&
	    (id->bInterfaceNumber != intf->desc.bInterfaceNumber))
		return 0;

	return 1;
}

/*
 * usb_match_one_id - 组合设备级 + 接口级匹配
 *
 * 这是接口级匹配的主要入口, 按顺序执行两级匹配:
 *   1) usb_match_device() - 匹配设备描述符(Vendor/Product/DeviceClass等)
 *   2) usb_match_one_id_intf() - 匹配接口描述符(InterfaceClass/Protocol等)
 *
 * 只有两级都通过才返回匹配成功。
 *
 * 返回: 0 = 不匹配, 1 = 匹配
 */
/* returns 0 if no match, 1 if match */
int usb_match_one_id(struct usb_interface *interface,
		     const struct usb_device_id *id)
{
	struct usb_host_interface *intf;
	struct usb_device *dev;

	/* proc_connectinfo in devio.c may call us with id == NULL. */
	if (id == NULL)
		return 0;

	intf = interface->cur_altsetting;
	dev = interface_to_usbdev(interface);

	if (!usb_match_device(dev, id))
		return 0;

	return usb_match_one_id_intf(dev, intf, id);
}
EXPORT_SYMBOL_GPL(usb_match_one_id);

/*
 * usb_match_id - 遍历 usb_device_id 表, 寻找第一个匹配项
 *
 * 在驱动的静态 id_table 中顺序查找, 对每个条目调用
 * usb_match_one_id() 进行匹配。第一条匹配的条目被返回。
 *
 * 查找终止条件: 当遇到 id->idVendor == 0 && id->idProduct == 0
 * && id->bDeviceClass == 0 && id->bInterfaceClass == 0 &&
 * id->driver_info == 0 的全零条目时停止。
 *
 * 特殊的"全零+driver_info"条目: 如果只有 driver_info 非零,
 * 则表示驱动希望检查所有设备和接口, 通常用于使用额外逻辑
 * 来确定是否绑定的场景。
 */
/**
 * usb_match_id - find first usb_device_id matching device or interface
 * @interface: the interface of interest
 * @id: array of usb_device_id structures, terminated by zero entry
 *
 * usb_match_id searches an array of usb_device_id's and returns
 * the first one matching the device or interface, or null.
 * This is used when binding (or rebinding) a driver to an interface.
 * Most USB device drivers will use this indirectly, through the usb core,
 * but some layered driver frameworks use it directly.
 * These device tables are exported with MODULE_DEVICE_TABLE, through
 * modutils, to support the driver loading functionality of USB hotplugging.
 *
 * Return: The first matching usb_device_id, or %NULL.
 *
 * What Matches:
 *
 * The "match_flags" element in a usb_device_id controls which
 * members are used.  If the corresponding bit is set, the
 * value in the device_id must match its corresponding member
 * in the device or interface descriptor, or else the device_id
 * does not match.
 *
 * "driver_info" is normally used only by device drivers,
 * but you can create a wildcard "matches anything" usb_device_id
 * as a driver's "modules.usbmap" entry if you provide an id with
 * only a nonzero "driver_info" field.  If you do this, the USB device
 * driver's probe() routine should use additional intelligence to
 * decide whether to bind to the specified interface.
 *
 * What Makes Good usb_device_id Tables:
 *
 * The match algorithm is very simple, so that intelligence in
 * driver selection must come from smart driver id records.
 * Unless you have good reasons to use another selection policy,
 * provide match elements only in related groups, and order match
 * specifiers from specific to general.  Use the macros provided
 * for that purpose if you can.
 *
 * The most specific match specifiers use device descriptor
 * data.  These are commonly used with product-specific matches;
 * the USB_DEVICE macro lets you provide vendor and product IDs,
 * and you can also match against ranges of product revisions.
 * These are widely used for devices with application or vendor
 * specific bDeviceClass values.
 *
 * Matches based on device class/subclass/protocol specifications
 * are slightly more general; use the USB_DEVICE_INFO macro, or
 * its siblings.  These are used with single-function devices
 * where bDeviceClass doesn't specify that each interface has
 * its own class.
 *
 * Matches based on interface class/subclass/protocol are the
 * most general; they let drivers bind to any interface on a
 * multiple-function device.  Use the USB_INTERFACE_INFO
 * macro, or its siblings, to match class-per-interface style
 * devices (as recorded in bInterfaceClass).
 *
 * Note that an entry created by USB_INTERFACE_INFO won't match
 * any interface if the device class is set to Vendor-Specific.
 * This is deliberate; according to the USB spec the meanings of
 * the interface class/subclass/protocol for these devices are also
 * vendor-specific, and hence matching against a standard product
 * class wouldn't work anyway.  If you really want to use an
 * interface-based match for such a device, create a match record
 * that also specifies the vendor ID.  (Unforunately there isn't a
 * standard macro for creating records like this.)
 *
 * Within those groups, remember that not all combinations are
 * meaningful.  For example, don't give a product version range
 * without vendor and product IDs; or specify a protocol without
 * its associated class and subclass.
 */
const struct usb_device_id *usb_match_id(struct usb_interface *interface,
					 const struct usb_device_id *id)
{
	/* proc_connectinfo in devio.c may call us with id == NULL. */
	if (id == NULL)
		return NULL;

	/* It is important to check that id->driver_info is nonzero,
	   since an entry that is all zeroes except for a nonzero
	   id->driver_info is the way to create an entry that
	   indicates that the driver want to examine every
	   device and interface. */
	for (; id->idVendor || id->idProduct || id->bDeviceClass ||
	       id->bInterfaceClass || id->driver_info; id++) {
		if (usb_match_one_id(interface, id))
			return id;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(usb_match_id);

const struct usb_device_id *usb_device_match_id(struct usb_device *udev,
				const struct usb_device_id *id)
{
	if (!id)
		return NULL;

	for (; id->idVendor || id->idProduct ; id++) {
		if (usb_match_device(udev, id))
			return id;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(usb_device_match_id);

bool usb_driver_applicable(struct usb_device *udev,
			   const struct usb_device_driver *udrv)
{
	if (udrv->id_table && udrv->match)
		return usb_device_match_id(udev, udrv->id_table) != NULL &&
		       udrv->match(udev);

	if (udrv->id_table)
		return usb_device_match_id(udev, udrv->id_table) != NULL;

	if (udrv->match)
		return udrv->match(udev);

	return false;
}

/*
 * usb_device_match - USB bus_type.match 回调, 两层匹配分发器
 *
 * 这是 USB 总线注册到 Linux 设备模型核心的 match 回调函数。
 * 每当系统中注册了新的设备或新的驱动时, 设备核心会遍历所有
 * 未匹配的设备-驱动对, 调用此函数判断是否匹配。
 *
 * 这是 USB 两层匹配架构的核心分发点:
 *
 *   1) 当 dev 是 usb_device (设备级):
 *       - 首先检查 drv 是否为 usb_device_driver (即 probe 回调是
 *         usb_probe_device 的驱动)。接口级驱动(usb_driver) 不会
 *         被考虑。
 *       - 如果驱动没有 id_table 和 match 函数, 则无条件返回 1,
 *         让驱动的 probe 函数自行决定是否绑定。
 *       - 否则通过 usb_driver_applicable() 检查设备是否在该驱动
 *         的 id_table 中或通过 match 函数。
 *
 *   2) 当 dev 是 usb_interface (接口级):
 *       - 检查 drv 是否是接口级驱动(非 usb_device_driver)。
 *       - 通过 usb_match_id() 在驱动的静态 id_table 中查找匹配项。
 *       - 如果没有静态匹配, 再通过 usb_match_dynamic_id() 检查
 *         动态添加的 ID (通过 sysfs new_id 接口)。
 *
 * 这种设计使得一个 USB 设备的多个接口可以由不同的驱动分别绑定,
 * 实现了接口级的功能复用。例如, 一个复合 USB 设备可以同时拥有
 * HID 接口(由 usbhid 驱动)和 Mass Storage 接口(由 usb-storage 驱动)。
 */
static int usb_device_match(struct device *dev, const struct device_driver *drv)
{
	/* devices and interfaces are handled separately */
	if (is_usb_device(dev)) {
		struct usb_device *udev;
		const struct usb_device_driver *udrv;

		/* interface drivers never match devices */
		if (!is_usb_device_driver(drv))
			return 0;

		udev = to_usb_device(dev);
		udrv = to_usb_device_driver(drv);

		/* If the device driver under consideration does not have a
		 * id_table or a match function, then let the driver's probe
		 * function decide.
		 */
		if (!udrv->id_table && !udrv->match)
			return 1;

		return usb_driver_applicable(udev, udrv);

	} else if (is_usb_interface(dev)) {
		struct usb_interface *intf;
		const struct usb_driver *usb_drv;
		const struct usb_device_id *id;

		/* device drivers never match interfaces */
		if (is_usb_device_driver(drv))
			return 0;

		intf = to_usb_interface(dev);
		usb_drv = to_usb_driver(drv);

		id = usb_match_id(intf, usb_drv->id_table);
		if (id)
			return 1;

		id = usb_match_dynamic_id(intf, usb_drv);
		if (id)
			return 1;
	}

	return 0;
}

static int usb_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct usb_device *usb_dev;

	if (is_usb_device(dev)) {
		usb_dev = to_usb_device(dev);
	} else if (is_usb_interface(dev)) {
		const struct usb_interface *intf = to_usb_interface(dev);

		usb_dev = interface_to_usbdev(intf);
	} else {
		return 0;
	}

	if (usb_dev->devnum < 0) {
		/* driver is often null here; dev_dbg() would oops */
		pr_debug("usb %s: already deleted?\n", dev_name(dev));
		return -ENODEV;
	}
	if (!usb_dev->bus) {
		pr_debug("usb %s: bus removed?\n", dev_name(dev));
		return -ENODEV;
	}

	/* per-device configurations are common */
	if (add_uevent_var(env, "PRODUCT=%x/%x/%x",
			   le16_to_cpu(usb_dev->descriptor.idVendor),
			   le16_to_cpu(usb_dev->descriptor.idProduct),
			   le16_to_cpu(usb_dev->descriptor.bcdDevice)))
		return -ENOMEM;

	/* class-based driver binding models */
	if (add_uevent_var(env, "TYPE=%d/%d/%d",
			   usb_dev->descriptor.bDeviceClass,
			   usb_dev->descriptor.bDeviceSubClass,
			   usb_dev->descriptor.bDeviceProtocol))
		return -ENOMEM;

	return 0;
}

static int __usb_bus_reprobe_drivers(struct device *dev, void *data)
{
	struct usb_device_driver *new_udriver = data;
	struct usb_device *udev;
	int ret;

	/* Don't reprobe if current driver isn't usb_generic_driver */
	if (dev->driver != &usb_generic_driver.driver)
		return 0;

	udev = to_usb_device(dev);
	if (!usb_driver_applicable(udev, new_udriver))
		return 0;

	ret = device_reprobe(dev);
	if (ret && ret != -EPROBE_DEFER)
		dev_err(dev, "Failed to reprobe device (error %d)\n", ret);

	return 0;
}

bool is_usb_device_driver(const struct device_driver *drv)
{
	return drv->probe == usb_probe_device;
}

/*
 * usb_register_device_driver - 注册 USB 设备级驱动
 *
 * 在 USB 核心中注册一个 usb_device_driver。设备级驱动匹配的是
 * USB 设备本身, 而非接口。
 *
 * 注册流程:
 *   1) 设置 driver.bus = &usb_bus_type (指向 USB 总线)
 *   2) 设置 driver.probe = usb_probe_device (统一 probe 入口)
 *   3) 设置 driver.remove = usb_unbind_device (统一 remove 入口)
 *   4) 调用 driver_register() 注册到 Linux 设备模型核心
 *   5) 注册完成后立即遍历所有已有设备, 调用 __usb_bus_reprobe_drivers
 *      检查是否有设备可以更好地由这个新驱动服务(即替代当前绑定的
 *      usb_generic_driver)。
 *
 * usb_generic_driver 是在 USB 核心初始化时注册的一个特殊设备级驱动,
 * 它不匹配特定 VID/PID, 而是作为万能回退驱动。当新的专用设备级驱动
 * 注册时, 核心会检查哪些设备目前由 usb_generic_driver 服务且更适合
 * 由新驱动接管, 触发重新探测。
 */
/**
 * usb_register_device_driver - register a USB device (not interface) driver
 * @new_udriver: USB operations for the device driver
 * @owner: module owner of this driver.
 *
 * Registers a USB device driver with the USB core.  The list of
 * unattached devices will be rescanned whenever a new driver is
 * added, allowing the new driver to attach to any recognized devices.
 *
 * Return: A negative error code on failure and 0 on success.
 */
int usb_register_device_driver(struct usb_device_driver *new_udriver,
		struct module *owner)
{
	int retval = 0;

	if (usb_disabled())
		return -ENODEV;

	new_udriver->driver.name = new_udriver->name;
	new_udriver->driver.bus = &usb_bus_type;
	new_udriver->driver.probe = usb_probe_device;
	new_udriver->driver.remove = usb_unbind_device;
	new_udriver->driver.owner = owner;
	new_udriver->driver.dev_groups = new_udriver->dev_groups;

	retval = driver_register(&new_udriver->driver);

	if (!retval) {
		pr_info("%s: registered new device driver %s\n",
			usbcore_name, new_udriver->name);
		/*
		 * Check whether any device could be better served with
		 * this new driver
		 */
		bus_for_each_dev(&usb_bus_type, NULL, new_udriver,
				 __usb_bus_reprobe_drivers);
	} else {
		pr_err("%s: error %d registering device driver %s\n",
			usbcore_name, retval, new_udriver->name);
	}

	return retval;
}
EXPORT_SYMBOL_GPL(usb_register_device_driver);

/*
 * usb_deregister_device_driver - 注销 USB 设备级驱动
 *
 * usb_register_device_driver 的逆操作。通过 driver_unregister()
 * 从 Linux 设备模型核心中移除该驱动。
 */
/**
 * usb_deregister_device_driver - unregister a USB device (not interface) driver
 * @udriver: USB operations of the device driver to unregister
 * Context: must be able to sleep
 *
 * Unlinks the specified driver from the internal USB driver list.
 */
void usb_deregister_device_driver(struct usb_device_driver *udriver)
{
	pr_info("%s: deregistering device driver %s\n",
			usbcore_name, udriver->name);

	driver_unregister(&udriver->driver);
}
EXPORT_SYMBOL_GPL(usb_deregister_device_driver);

/*
 * usb_register_driver - 注册 USB 接口级驱动
 *
 * 在 USB 核心中注册一个 usb_driver(接口级驱动)。这是绝大多数
 * USB 驱动使用的注册函数, 被 usb_register() 宏所封装。
 *
 * 注册流程:
 *   1) 设置 driver.bus = &usb_bus_type (指向 USB 总线)
 *   2) 设置 driver.probe = usb_probe_interface (接口级 probe 入口)
 *   3) 设置 driver.remove = usb_unbind_interface (接口级 remove 入口)
 *   4) 设置 driver.shutdown = usb_shutdown_interface (关机回调)
 *   5) 初始化 dynids 链表(用于 sysfs new_id 动态添加 ID)
 *   6) 调用 driver_register() 注册到 Linux 设备模型核心
 *   7) 创建 new_id 和 remove_id sysfs 属性文件, 允许用户空间
 *      动态管理该驱动支持的设备 ID 列表
 *
 * 与 usb_register_device_driver 的区别:
 *   - 本函数注册的是接口级驱动(匹配接口)
 *   - usb_register_device_driver 注册的是设备级驱动(匹配设备)
 *
 * 注意: 在大多数情况下, USB 驱动应使用 usb_register() 宏,
 * 它会自动处理 module 所有权和模块名。
 */
/**
 * usb_register_driver - register a USB interface driver
 * @new_driver: USB operations for the interface driver
 * @owner: module owner of this driver.
 * @mod_name: module name string
 *
 * Registers a USB interface driver with the USB core.  The list of
 * unattached interfaces will be rescanned whenever a new driver is
 * added, allowing the new driver to attach to any recognized interfaces.
 *
 * Return: A negative error code on failure and 0 on success.
 *
 * NOTE: if you want your driver to use the USB major number, you must call
 * usb_register_dev() to enable that functionality.  This function no longer
 * takes care of that.
 */
int usb_register_driver(struct usb_driver *new_driver, struct module *owner,
			const char *mod_name)
{
	int retval = 0;

	if (usb_disabled())
		return -ENODEV;

	new_driver->driver.name = new_driver->name;
	new_driver->driver.bus = &usb_bus_type;
	new_driver->driver.probe = usb_probe_interface;
	new_driver->driver.remove = usb_unbind_interface;
	new_driver->driver.shutdown = usb_shutdown_interface;
	new_driver->driver.owner = owner;
	new_driver->driver.mod_name = mod_name;
	new_driver->driver.dev_groups = new_driver->dev_groups;
	INIT_LIST_HEAD(&new_driver->dynids.list);

	retval = driver_register(&new_driver->driver);
	if (retval)
		goto out;

	retval = usb_create_newid_files(new_driver);
	if (retval)
		goto out_newid;

	pr_info("%s: registered new interface driver %s\n",
			usbcore_name, new_driver->name);

	return 0;

out_newid:
	driver_unregister(&new_driver->driver);
out:
	pr_err("%s: error %d registering interface driver %s\n",
		usbcore_name, retval, new_driver->name);
	return retval;
}
EXPORT_SYMBOL_GPL(usb_register_driver);

/*
 * usb_deregister - 注销 USB 接口级驱动
 *
 * usb_register_driver 的逆操作。执行清理:
 *   1) 删除 new_id / remove_id sysfs 属性文件
 *   2) 通过 driver_unregister() 从设备模型核心中移除驱动
 *   3) 释放动态添加的设备 ID 列表 (dynids)
 *
 * 注意: 如果驱动之前调用了 usb_register_dev() 注册了 USB 主设备号,
 * 还需要额外调用 usb_deregister_dev() 进行清理。
 */
/**
 * usb_deregister - unregister a USB interface driver
 * @driver: USB operations of the interface driver to unregister
 * Context: must be able to sleep
 *
 * Unlinks the specified driver from the internal USB driver list.
 *
 * NOTE: If you called usb_register_dev(), you still need to call
 * usb_deregister_dev() to clean up your driver's allocated minor numbers,
 * this * call will no longer do it for you.
 */
void usb_deregister(struct usb_driver *driver)
{
	pr_info("%s: deregistering interface driver %s\n",
			usbcore_name, driver->name);

	usb_remove_newid_files(driver);
	driver_unregister(&driver->driver);
	usb_free_dynids(driver);
}
EXPORT_SYMBOL_GPL(usb_deregister);

/* Forced unbinding of a USB interface driver, either because
 * it doesn't support pre_reset/post_reset/reset_resume or
 * because it doesn't support suspend/resume.
 *
 * The caller must hold @intf's device's lock, but not @intf's lock.
 */
void usb_forced_unbind_intf(struct usb_interface *intf)
{
	struct usb_driver *driver = to_usb_driver(intf->dev.driver);

	dev_dbg(&intf->dev, "forced unbind\n");
	usb_driver_release_interface(driver, intf);

	/* Mark the interface for later rebinding */
	intf->needs_binding = 1;
}

/*
 * Unbind drivers for @udev's marked interfaces.  These interfaces have
 * the needs_binding flag set, for example by usb_resume_interface().
 *
 * The caller must hold @udev's device lock.
 */
static void unbind_marked_interfaces(struct usb_device *udev)
{
	struct usb_host_config	*config;
	int			i;
	struct usb_interface	*intf;

	config = udev->actconfig;
	if (config) {
		for (i = 0; i < config->desc.bNumInterfaces; ++i) {
			intf = config->interface[i];
			if (intf->dev.driver && intf->needs_binding)
				usb_forced_unbind_intf(intf);
		}
	}
}

/* Delayed forced unbinding of a USB interface driver and scan
 * for rebinding.
 *
 * The caller must hold @intf's device's lock, but not @intf's lock.
 *
 * Note: Rebinds will be skipped if a system sleep transition is in
 * progress and the PM "complete" callback hasn't occurred yet.
 */
static void usb_rebind_intf(struct usb_interface *intf)
{
	int rc;

	/* Delayed unbind of an existing driver */
	if (intf->dev.driver)
		usb_forced_unbind_intf(intf);

	/* Try to rebind the interface */
	if (!intf->dev.power.is_prepared) {
		intf->needs_binding = 0;
		rc = device_attach(&intf->dev);
		if (rc < 0 && rc != -EPROBE_DEFER)
			dev_warn(&intf->dev, "rebind failed: %d\n", rc);
	}
}

/*
 * Rebind drivers to @udev's marked interfaces.  These interfaces have
 * the needs_binding flag set.
 *
 * The caller must hold @udev's device lock.
 */
static void rebind_marked_interfaces(struct usb_device *udev)
{
	struct usb_host_config	*config;
	int			i;
	struct usb_interface	*intf;

	config = udev->actconfig;
	if (config) {
		for (i = 0; i < config->desc.bNumInterfaces; ++i) {
			intf = config->interface[i];
			if (intf->needs_binding)
				usb_rebind_intf(intf);
		}
	}
}

/*
 * Unbind all of @udev's marked interfaces and then rebind all of them.
 * This ordering is necessary because some drivers claim several interfaces
 * when they are first probed.
 *
 * The caller must hold @udev's device lock.
 */
void usb_unbind_and_rebind_marked_interfaces(struct usb_device *udev)
{
	unbind_marked_interfaces(udev);
	rebind_marked_interfaces(udev);
}

#ifdef CONFIG_PM

/* Unbind drivers for @udev's interfaces that don't support suspend/resume
 * There is no check for reset_resume here because it can be determined
 * only during resume whether reset_resume is needed.
 *
 * The caller must hold @udev's device lock.
 */
static void unbind_no_pm_drivers_interfaces(struct usb_device *udev)
{
	struct usb_host_config	*config;
	int			i;
	struct usb_interface	*intf;
	struct usb_driver	*drv;

	config = udev->actconfig;
	if (config) {
		for (i = 0; i < config->desc.bNumInterfaces; ++i) {
			intf = config->interface[i];

			if (intf->dev.driver) {
				drv = to_usb_driver(intf->dev.driver);
				if (!drv->suspend || !drv->resume)
					usb_forced_unbind_intf(intf);
			}
		}
	}
}

static int usb_suspend_device(struct usb_device *udev, pm_message_t msg)
{
	struct usb_device_driver	*udriver;
	int				status = 0;

	if (udev->state == USB_STATE_NOTATTACHED ||
			udev->state == USB_STATE_SUSPENDED)
		goto done;

	/* For devices that don't have a driver, we do a generic suspend. */
	if (udev->dev.driver)
		udriver = to_usb_device_driver(udev->dev.driver);
	else {
		udev->do_remote_wakeup = 0;
		udriver = &usb_generic_driver;
	}
	if (udriver->suspend)
		status = udriver->suspend(udev, msg);
	if (status == 0 && udriver->generic_subclass)
		status = usb_generic_driver_suspend(udev, msg);

 done:
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	return status;
}

static int usb_resume_device(struct usb_device *udev, pm_message_t msg)
{
	struct usb_device_driver	*udriver;
	int				status = 0;

	if (udev->state == USB_STATE_NOTATTACHED)
		goto done;

	/* Can't resume it if it doesn't have a driver. */
	if (udev->dev.driver == NULL) {
		status = -ENOTCONN;
		goto done;
	}

	/* Non-root devices on a full/low-speed bus must wait for their
	 * companion high-speed root hub, in case a handoff is needed.
	 */
	if (!PMSG_IS_AUTO(msg) && udev->parent && udev->bus->hs_companion)
		device_pm_wait_for_dev(&udev->dev,
				&udev->bus->hs_companion->root_hub->dev);

	if (udev->quirks & USB_QUIRK_RESET_RESUME)
		udev->reset_resume = 1;

	udriver = to_usb_device_driver(udev->dev.driver);
	if (udriver->generic_subclass)
		status = usb_generic_driver_resume(udev, msg);
	if (status == 0 && udriver->resume)
		status = udriver->resume(udev, msg);

 done:
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	return status;
}

static int usb_suspend_interface(struct usb_device *udev,
		struct usb_interface *intf, pm_message_t msg)
{
	struct usb_driver	*driver;
	int			status = 0;

	if (udev->state == USB_STATE_NOTATTACHED ||
			intf->condition == USB_INTERFACE_UNBOUND)
		goto done;
	driver = to_usb_driver(intf->dev.driver);

	/* at this time we know the driver supports suspend */
	status = driver->suspend(intf, msg);
	if (status && !PMSG_IS_AUTO(msg))
		dev_err(&intf->dev, "suspend error %d\n", status);

 done:
	dev_vdbg(&intf->dev, "%s: status %d\n", __func__, status);
	return status;
}

static int usb_resume_interface(struct usb_device *udev,
		struct usb_interface *intf, pm_message_t msg, int reset_resume)
{
	struct usb_driver	*driver;
	int			status = 0;

	if (udev->state == USB_STATE_NOTATTACHED)
		goto done;

	/* Don't let autoresume interfere with unbinding */
	if (intf->condition == USB_INTERFACE_UNBINDING)
		goto done;

	/* Can't resume it if it doesn't have a driver. */
	if (intf->condition == USB_INTERFACE_UNBOUND) {

		/* Carry out a deferred switch to altsetting 0 */
		if (intf->needs_altsetting0 && !intf->dev.power.is_prepared) {
			usb_set_interface(udev, intf->altsetting[0].
					desc.bInterfaceNumber, 0);
			intf->needs_altsetting0 = 0;
		}
		goto done;
	}

	/* Don't resume if the interface is marked for rebinding */
	if (intf->needs_binding)
		goto done;
	driver = to_usb_driver(intf->dev.driver);

	if (reset_resume) {
		if (driver->reset_resume) {
			status = driver->reset_resume(intf);
			if (status)
				dev_err(&intf->dev, "%s error %d\n",
						"reset_resume", status);
		} else {
			intf->needs_binding = 1;
			dev_dbg(&intf->dev, "no reset_resume for driver %s?\n",
					driver->name);
		}
	} else {
		status = driver->resume(intf);
		if (status)
			dev_err(&intf->dev, "resume error %d\n", status);
	}

done:
	dev_vdbg(&intf->dev, "%s: status %d\n", __func__, status);

	/* Later we will unbind the driver and/or reprobe, if necessary */
	return status;
}

/**
 * usb_suspend_both - suspend a USB device and its interfaces
 * @udev: the usb_device to suspend
 * @msg: Power Management message describing this state transition
 *
 * This is the central routine for suspending USB devices.  It calls the
 * suspend methods for all the interface drivers in @udev and then calls
 * the suspend method for @udev itself.  When the routine is called in
 * autosuspend, if an error occurs at any stage, all the interfaces
 * which were suspended are resumed so that they remain in the same
 * state as the device, but when called from system sleep, all error
 * from suspend methods of interfaces and the non-root-hub device itself
 * are simply ignored, so all suspended interfaces are only resumed
 * to the device's state when @udev is root-hub and its suspend method
 * returns failure.
 *
 * Autosuspend requests originating from a child device or an interface
 * driver may be made without the protection of @udev's device lock, but
 * all other suspend calls will hold the lock.  Usbcore will insure that
 * method calls do not arrive during bind, unbind, or reset operations.
 * However drivers must be prepared to handle suspend calls arriving at
 * unpredictable times.
 *
 * This routine can run only in process context.
 *
 * Return: 0 if the suspend succeeded.
 */
static int usb_suspend_both(struct usb_device *udev, pm_message_t msg)
{
	int			status = 0;
	int			i = 0, n = 0;
	struct usb_interface	*intf;
	bool			offload_active = false;

	if (udev->state == USB_STATE_NOTATTACHED ||
			udev->state == USB_STATE_SUSPENDED)
		goto done;

	usb_offload_set_pm_locked(udev, true);
	if (msg.event == PM_EVENT_SUSPEND && usb_offload_check(udev)) {
		dev_dbg(&udev->dev, "device offloaded, skip suspend.\n");
		offload_active = true;
	}

	/* Suspend all the interfaces and then udev itself */
	if (udev->actconfig) {
		n = udev->actconfig->desc.bNumInterfaces;
		for (i = n - 1; i >= 0; --i) {
			intf = udev->actconfig->interface[i];
			/*
			 * Don't suspend interfaces with remote wakeup while
			 * the controller is active. This preserves pending
			 * interrupt urbs, allowing interrupt events to be
			 * handled during system suspend.
			 */
			if (offload_active && intf->needs_remote_wakeup) {
				dev_dbg(&intf->dev,
					"device offloaded, skip suspend.\n");
				continue;
			}
			status = usb_suspend_interface(udev, intf, msg);

			/* Ignore errors during system sleep transitions */
			if (!PMSG_IS_AUTO(msg))
				status = 0;
			if (status != 0)
				break;
		}
	}
	if (status == 0) {
		if (!offload_active)
			status = usb_suspend_device(udev, msg);

		/*
		 * Ignore errors from non-root-hub devices during
		 * system sleep transitions.  For the most part,
		 * these devices should go to low power anyway when
		 * the entire bus is suspended.
		 */
		if (udev->parent && !PMSG_IS_AUTO(msg))
			status = 0;

		/*
		 * If the device is inaccessible, don't try to resume
		 * suspended interfaces and just return the error.
		 */
		if (status && status != -EBUSY) {
			int err;
			u16 devstat;

			err = usb_get_std_status(udev, USB_RECIP_DEVICE, 0,
						 &devstat);
			if (err) {
				dev_err(&udev->dev,
					"Failed to suspend device, error %d\n",
					status);
				goto done;
			}
		}
	}

	/* If the suspend failed, resume interfaces that did get suspended */
	if (status != 0) {
		if (udev->actconfig) {
			msg.event ^= (PM_EVENT_SUSPEND | PM_EVENT_RESUME);
			while (++i < n) {
				intf = udev->actconfig->interface[i];
				usb_resume_interface(udev, intf, msg, 0);
			}
		}

	/* If the suspend succeeded then prevent any more URB submissions
	 * and flush any outstanding URBs.
	 */
	} else {
		udev->can_submit = 0;
		if (!offload_active) {
			for (i = 0; i < 16; ++i) {
				usb_hcd_flush_endpoint(udev, udev->ep_out[i]);
				usb_hcd_flush_endpoint(udev, udev->ep_in[i]);
			}
		}
	}

 done:
	if (status != 0)
		usb_offload_set_pm_locked(udev, false);
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	return status;
}

/**
 * usb_resume_both - resume a USB device and its interfaces
 * @udev: the usb_device to resume
 * @msg: Power Management message describing this state transition
 *
 * This is the central routine for resuming USB devices.  It calls the
 * resume method for @udev and then calls the resume methods for all
 * the interface drivers in @udev.
 *
 * Autoresume requests originating from a child device or an interface
 * driver may be made without the protection of @udev's device lock, but
 * all other resume calls will hold the lock.  Usbcore will insure that
 * method calls do not arrive during bind, unbind, or reset operations.
 * However drivers must be prepared to handle resume calls arriving at
 * unpredictable times.
 *
 * This routine can run only in process context.
 *
 * Return: 0 on success.
 */
static int usb_resume_both(struct usb_device *udev, pm_message_t msg)
{
	int			status = 0;
	int			i;
	struct usb_interface	*intf;
	bool			offload_active = false;

	if (udev->state == USB_STATE_NOTATTACHED) {
		status = -ENODEV;
		goto done;
	}
	udev->can_submit = 1;
	if (msg.event == PM_EVENT_RESUME)
		offload_active = usb_offload_check(udev);

	/* Resume the device */
	if (udev->state == USB_STATE_SUSPENDED || udev->reset_resume) {
		if (!offload_active)
			status = usb_resume_device(udev, msg);
		else
			dev_dbg(&udev->dev,
				"device offloaded, skip resume.\n");
	}

	/* Resume the interfaces */
	if (status == 0 && udev->actconfig) {
		for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
			intf = udev->actconfig->interface[i];
			/*
			 * Interfaces with remote wakeup aren't suspended
			 * while the controller is active. This preserves
			 * pending interrupt urbs, allowing interrupt events
			 * to be handled during system suspend.
			 */
			if (offload_active && intf->needs_remote_wakeup) {
				dev_dbg(&intf->dev,
					"device offloaded, skip resume.\n");
				continue;
			}
			usb_resume_interface(udev, intf, msg,
					udev->reset_resume);
		}
	}
	usb_mark_last_busy(udev);

 done:
	dev_vdbg(&udev->dev, "%s: status %d\n", __func__, status);
	usb_offload_set_pm_locked(udev, false);
	if (!status)
		udev->reset_resume = 0;
	return status;
}

static void choose_wakeup(struct usb_device *udev, pm_message_t msg)
{
	int	w;

	/*
	 * For FREEZE/QUIESCE, disable remote wakeups so no interrupts get
	 * generated.
	 */
	if (msg.event == PM_EVENT_FREEZE || msg.event == PM_EVENT_QUIESCE) {
		w = 0;

	} else {
		/*
		 * Enable remote wakeup if it is allowed, even if no interface
		 * drivers actually want it.
		 */
		w = device_may_wakeup(&udev->dev);
	}

	/*
	 * If the device is autosuspended with the wrong wakeup setting,
	 * autoresume now so the setting can be changed.
	 */
	if (udev->state == USB_STATE_SUSPENDED && w != udev->do_remote_wakeup)
		pm_runtime_resume(&udev->dev);
	udev->do_remote_wakeup = w;
}

/* The device lock is held by the PM core */
int usb_suspend(struct device *dev, pm_message_t msg)
{
	struct usb_device	*udev = to_usb_device(dev);
	int r;

	unbind_no_pm_drivers_interfaces(udev);

	/* From now on we are sure all drivers support suspend/resume
	 * but not necessarily reset_resume()
	 * so we may still need to unbind and rebind upon resume
	 */
	choose_wakeup(udev, msg);
	r = usb_suspend_both(udev, msg);
	if (r)
		return r;

	if (udev->quirks & USB_QUIRK_DISCONNECT_SUSPEND)
		usb_port_disable(udev);

	return 0;
}

/* The device lock is held by the PM core */
int usb_resume_complete(struct device *dev)
{
	struct usb_device *udev = to_usb_device(dev);

	/* For PM complete calls, all we do is rebind interfaces
	 * whose needs_binding flag is set
	 */
	if (udev->state != USB_STATE_NOTATTACHED)
		rebind_marked_interfaces(udev);
	return 0;
}

/* The device lock is held by the PM core */
int usb_resume(struct device *dev, pm_message_t msg)
{
	struct usb_device	*udev = to_usb_device(dev);
	int			status;

	/* For all calls, take the device back to full power and
	 * tell the PM core in case it was autosuspended previously.
	 * Unbind the interfaces that will need rebinding later,
	 * because they fail to support reset_resume.
	 * (This can't be done in usb_resume_interface()
	 * above because it doesn't own the right set of locks.)
	 */
	status = usb_resume_both(udev, msg);
	if (status == 0) {
		pm_runtime_disable(dev);
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);
		unbind_marked_interfaces(udev);
	}

	/* Avoid PM error messages for devices disconnected while suspended
	 * as we'll display regular disconnect messages just a bit later.
	 */
	if (status == -ENODEV || status == -ESHUTDOWN)
		status = 0;
	return status;
}

/**
 * usb_enable_autosuspend - allow a USB device to be autosuspended
 * @udev: the USB device which may be autosuspended
 *
 * This routine allows @udev to be autosuspended.  An autosuspend won't
 * take place until the autosuspend_delay has elapsed and all the other
 * necessary conditions are satisfied.
 *
 * The caller must hold @udev's device lock.
 */
void usb_enable_autosuspend(struct usb_device *udev)
{
	pm_runtime_allow(&udev->dev);
}
EXPORT_SYMBOL_GPL(usb_enable_autosuspend);

/**
 * usb_disable_autosuspend - prevent a USB device from being autosuspended
 * @udev: the USB device which may not be autosuspended
 *
 * This routine prevents @udev from being autosuspended and wakes it up
 * if it is already autosuspended.
 *
 * The caller must hold @udev's device lock.
 */
void usb_disable_autosuspend(struct usb_device *udev)
{
	pm_runtime_forbid(&udev->dev);
}
EXPORT_SYMBOL_GPL(usb_disable_autosuspend);

/**
 * usb_autosuspend_device - delayed autosuspend of a USB device and its interfaces
 * @udev: the usb_device to autosuspend
 *
 * This routine should be called when a core subsystem is finished using
 * @udev and wants to allow it to autosuspend.  Examples would be when
 * @udev's device file in usbfs is closed or after a configuration change.
 *
 * @udev's usage counter is decremented; if it drops to 0 and all the
 * interfaces are inactive then a delayed autosuspend will be attempted.
 * The attempt may fail (see autosuspend_check()).
 *
 * The caller must hold @udev's device lock.
 *
 * This routine can run only in process context.
 */
void usb_autosuspend_device(struct usb_device *udev)
{
	int	status;

	usb_mark_last_busy(udev);
	status = pm_runtime_put_sync_autosuspend(&udev->dev);
	dev_vdbg(&udev->dev, "%s: cnt %d -> %d\n",
			__func__, atomic_read(&udev->dev.power.usage_count),
			status);
}

/**
 * usb_autoresume_device - immediately autoresume a USB device and its interfaces
 * @udev: the usb_device to autoresume
 *
 * This routine should be called when a core subsystem wants to use @udev
 * and needs to guarantee that it is not suspended.  No autosuspend will
 * occur until usb_autosuspend_device() is called.  (Note that this will
 * not prevent suspend events originating in the PM core.)  Examples would
 * be when @udev's device file in usbfs is opened or when a remote-wakeup
 * request is received.
 *
 * @udev's usage counter is incremented to prevent subsequent autosuspends.
 * However if the autoresume fails then the usage counter is re-decremented.
 *
 * The caller must hold @udev's device lock.
 *
 * This routine can run only in process context.
 *
 * Return: 0 on success. A negative error code otherwise.
 */
int usb_autoresume_device(struct usb_device *udev)
{
	int	status;

	status = pm_runtime_resume_and_get(&udev->dev);
	dev_vdbg(&udev->dev, "%s: cnt %d -> %d\n",
			__func__, atomic_read(&udev->dev.power.usage_count),
			status);
	return status;
}

/**
 * usb_autopm_put_interface - decrement a USB interface's PM-usage counter
 * @intf: the usb_interface whose counter should be decremented
 *
 * This routine should be called by an interface driver when it is
 * finished using @intf and wants to allow it to autosuspend.  A typical
 * example would be a character-device driver when its device file is
 * closed.
 *
 * The routine decrements @intf's usage counter.  When the counter reaches
 * 0, a delayed autosuspend request for @intf's device is attempted.  The
 * attempt may fail (see autosuspend_check()).
 *
 * This routine can run only in process context.
 */
void usb_autopm_put_interface(struct usb_interface *intf)
{
	struct usb_device	*udev = interface_to_usbdev(intf);
	int			status;

	usb_mark_last_busy(udev);
	status = pm_runtime_put_sync(&intf->dev);
	dev_vdbg(&intf->dev, "%s: cnt %d -> %d\n",
			__func__, atomic_read(&intf->dev.power.usage_count),
			status);
}
EXPORT_SYMBOL_GPL(usb_autopm_put_interface);

/**
 * usb_autopm_put_interface_async - decrement a USB interface's PM-usage counter
 * @intf: the usb_interface whose counter should be decremented
 *
 * This routine does much the same thing as usb_autopm_put_interface():
 * It decrements @intf's usage counter and schedules a delayed
 * autosuspend request if the counter is <= 0.  The difference is that it
 * does not perform any synchronization; callers should hold a private
 * lock and handle all synchronization issues themselves.
 *
 * Typically a driver would call this routine during an URB's completion
 * handler, if no more URBs were pending.
 *
 * This routine can run in atomic context.
 */
void usb_autopm_put_interface_async(struct usb_interface *intf)
{
	struct usb_device	*udev = interface_to_usbdev(intf);

	usb_mark_last_busy(udev);
	pm_runtime_put(&intf->dev);
	dev_vdbg(&intf->dev, "%s: cnt %d\n",
			__func__, atomic_read(&intf->dev.power.usage_count));
}
EXPORT_SYMBOL_GPL(usb_autopm_put_interface_async);

/**
 * usb_autopm_put_interface_no_suspend - decrement a USB interface's PM-usage counter
 * @intf: the usb_interface whose counter should be decremented
 *
 * This routine decrements @intf's usage counter but does not carry out an
 * autosuspend.
 *
 * This routine can run in atomic context.
 */
void usb_autopm_put_interface_no_suspend(struct usb_interface *intf)
{
	struct usb_device	*udev = interface_to_usbdev(intf);

	usb_mark_last_busy(udev);
	pm_runtime_put_noidle(&intf->dev);
}
EXPORT_SYMBOL_GPL(usb_autopm_put_interface_no_suspend);

/**
 * usb_autopm_get_interface - increment a USB interface's PM-usage counter
 * @intf: the usb_interface whose counter should be incremented
 *
 * This routine should be called by an interface driver when it wants to
 * use @intf and needs to guarantee that it is not suspended.  In addition,
 * the routine prevents @intf from being autosuspended subsequently.  (Note
 * that this will not prevent suspend events originating in the PM core.)
 * This prevention will persist until usb_autopm_put_interface() is called
 * or @intf is unbound.  A typical example would be a character-device
 * driver when its device file is opened.
 *
 * @intf's usage counter is incremented to prevent subsequent autosuspends.
 * However if the autoresume fails then the counter is re-decremented.
 *
 * This routine can run only in process context.
 *
 * Return: 0 on success.
 */
int usb_autopm_get_interface(struct usb_interface *intf)
{
	int	status;

	status = pm_runtime_resume_and_get(&intf->dev);
	dev_vdbg(&intf->dev, "%s: cnt %d -> %d\n",
			__func__, atomic_read(&intf->dev.power.usage_count),
			status);
	return status;
}
EXPORT_SYMBOL_GPL(usb_autopm_get_interface);

/**
 * usb_autopm_get_interface_async - increment a USB interface's PM-usage counter
 * @intf: the usb_interface whose counter should be incremented
 *
 * This routine does much the same thing as
 * usb_autopm_get_interface(): It increments @intf's usage counter and
 * queues an autoresume request if the device is suspended.  The
 * differences are that it does not perform any synchronization (callers
 * should hold a private lock and handle all synchronization issues
 * themselves), and it does not autoresume the device directly (it only
 * queues a request).  After a successful call, the device may not yet be
 * resumed.
 *
 * This routine can run in atomic context.
 *
 * Return: 0 on success. A negative error code otherwise.
 */
int usb_autopm_get_interface_async(struct usb_interface *intf)
{
	int	status;

	status = pm_runtime_get(&intf->dev);
	if (status < 0 && status != -EINPROGRESS)
		pm_runtime_put_noidle(&intf->dev);
	dev_vdbg(&intf->dev, "%s: cnt %d -> %d\n",
			__func__, atomic_read(&intf->dev.power.usage_count),
			status);
	if (status > 0 || status == -EINPROGRESS)
		status = 0;
	return status;
}
EXPORT_SYMBOL_GPL(usb_autopm_get_interface_async);

/**
 * usb_autopm_get_interface_no_resume - increment a USB interface's PM-usage counter
 * @intf: the usb_interface whose counter should be incremented
 *
 * This routine increments @intf's usage counter but does not carry out an
 * autoresume.
 *
 * This routine can run in atomic context.
 */
void usb_autopm_get_interface_no_resume(struct usb_interface *intf)
{
	struct usb_device	*udev = interface_to_usbdev(intf);

	usb_mark_last_busy(udev);
	pm_runtime_get_noresume(&intf->dev);
}
EXPORT_SYMBOL_GPL(usb_autopm_get_interface_no_resume);

/* Internal routine to check whether we may autosuspend a device. */
static int autosuspend_check(struct usb_device *udev)
{
	int			w, i;
	struct usb_interface	*intf;

	if (udev->state == USB_STATE_NOTATTACHED)
		return -ENODEV;

	/* Fail if autosuspend is disabled, or any interfaces are in use, or
	 * any interface drivers require remote wakeup but it isn't available.
	 */
	w = 0;
	if (udev->actconfig) {
		for (i = 0; i < udev->actconfig->desc.bNumInterfaces; i++) {
			intf = udev->actconfig->interface[i];

			/* We don't need to check interfaces that are
			 * disabled for runtime PM.  Either they are unbound
			 * or else their drivers don't support autosuspend
			 * and so they are permanently active.
			 */
			if (intf->dev.power.disable_depth)
				continue;
			if (atomic_read(&intf->dev.power.usage_count) > 0)
				return -EBUSY;
			w |= intf->needs_remote_wakeup;

			/* Don't allow autosuspend if the device will need
			 * a reset-resume and any of its interface drivers
			 * doesn't include support or needs remote wakeup.
			 */
			if (udev->quirks & USB_QUIRK_RESET_RESUME) {
				struct usb_driver *driver;

				driver = to_usb_driver(intf->dev.driver);
				if (!driver->reset_resume ||
						intf->needs_remote_wakeup)
					return -EOPNOTSUPP;
			}
		}
	}
	if (w && !device_can_wakeup(&udev->dev)) {
		dev_dbg(&udev->dev, "remote wakeup needed for autosuspend\n");
		return -EOPNOTSUPP;
	}

	/*
	 * If the device is a direct child of the root hub and the HCD
	 * doesn't handle wakeup requests, don't allow autosuspend when
	 * wakeup is needed.
	 */
	if (w && udev->parent == udev->bus->root_hub &&
			bus_to_hcd(udev->bus)->cant_recv_wakeups) {
		dev_dbg(&udev->dev, "HCD doesn't handle wakeup requests\n");
		return -EOPNOTSUPP;
	}

	udev->do_remote_wakeup = w;
	return 0;
}

int usb_runtime_suspend(struct device *dev)
{
	struct usb_device	*udev = to_usb_device(dev);
	int			status;

	/* A USB device can be suspended if it passes the various autosuspend
	 * checks.  Runtime suspend for a USB device means suspending all the
	 * interfaces and then the device itself.
	 */
	if (autosuspend_check(udev) != 0)
		return -EAGAIN;

	status = usb_suspend_both(udev, PMSG_AUTO_SUSPEND);

	/* Allow a retry if autosuspend failed temporarily */
	if (status == -EAGAIN || status == -EBUSY)
		usb_mark_last_busy(udev);

	/*
	 * The PM core reacts badly unless the return code is 0,
	 * -EAGAIN, or -EBUSY, so always return -EBUSY on an error
	 * (except for root hubs, because they don't suspend through
	 * an upstream port like other USB devices).
	 */
	if (status != 0 && udev->parent)
		return -EBUSY;
	return status;
}

int usb_runtime_resume(struct device *dev)
{
	struct usb_device	*udev = to_usb_device(dev);
	int			status;

	/* Runtime resume for a USB device means resuming both the device
	 * and all its interfaces.
	 */
	status = usb_resume_both(udev, PMSG_AUTO_RESUME);
	return status;
}

int usb_runtime_idle(struct device *dev)
{
	struct usb_device	*udev = to_usb_device(dev);

	/* An idle USB device can be suspended if it passes the various
	 * autosuspend checks.
	 */
	if (autosuspend_check(udev) == 0)
		pm_runtime_autosuspend(dev);
	/* Tell the core not to suspend it, though. */
	return -EBUSY;
}

static int usb_set_usb2_hardware_lpm(struct usb_device *udev, int enable)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);
	int ret = -EPERM;

	if (hcd->driver->set_usb2_hw_lpm) {
		ret = hcd->driver->set_usb2_hw_lpm(hcd, udev, enable);
		if (!ret)
			udev->usb2_hw_lpm_enabled = enable;
	}

	return ret;
}

int usb_enable_usb2_hardware_lpm(struct usb_device *udev)
{
	if (!udev->usb2_hw_lpm_capable ||
	    !udev->usb2_hw_lpm_allowed ||
	    udev->usb2_hw_lpm_enabled)
		return 0;

	return usb_set_usb2_hardware_lpm(udev, 1);
}

int usb_disable_usb2_hardware_lpm(struct usb_device *udev)
{
	if (!udev->usb2_hw_lpm_enabled)
		return 0;

	return usb_set_usb2_hardware_lpm(udev, 0);
}

#endif /* CONFIG_PM */

const struct bus_type usb_bus_type = {
	.name =		"usb",
	.match =	usb_device_match,
	.uevent =	usb_uevent,
	.need_parent_lock =	true,
};
