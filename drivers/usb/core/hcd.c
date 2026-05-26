// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright Linus Torvalds 1999
 * (C) Copyright Johannes Erdfelt 1999-2001
 * (C) Copyright Andreas Gal 1999
 * (C) Copyright Gregory P. Smith 1999
 * (C) Copyright Deti Fliegl 1999
 * (C) Copyright Randy Dunlap 2000
 * (C) Copyright David Brownell 2000-2002
 */

/*
 * hcd.c -- USB Host Controller Driver (HCD) 框架核心
 *
 * 版权所有 (C) Linus Torvalds 1999, Johannes Erdfelt 1999-2001,
 * Andreas Gal 1999, Gregory P. Smith 1999, Deti Fliegl 1999,
 * Randy Dunlap 2000, David Brownell 2000-2002
 *
 * --------------------------------------------------------------------------
 * 概述:
 *
 * HCD (Host Controller Driver) 框架是 Linux USB 子系统的核心枢纽,
 * 位于 USB Core 和具体的主机控制器硬件 (如 xHCI、EHCI、OHCI、UHCI) 之间。
 * 它通过 struct hc_driver 操作集实现硬件无关的 URB 管理, 使得上层的
 * USB 核心和驱动程序无需关心底层硬件的具体差异.
 *
 * --------------------------------------------------------------------------
 * 关键职责:
 *
 * 1. URB 生命周期管理 (usb_hcd_submit_urb / usb_hcd_giveback_urb)
 *    - URB 提交: usb_hcd_submit_urb() → map_urb_for_dma() →
 *      hc_driver->urb_enqueue() → (硬件处理) → usb_hcd_giveback_urb()
 *    - URB 完成回调: 使用 tasklet (BH, bottom half) 机制延迟到软中断上下文,
 *      避免在硬件中断处理函数中直接调用 urb->complete()
 *
 * 2. 根 HUB (Root Hub) 模拟
 *    - 每个主机控制器都有一个软件模拟的虚拟根 HUB
 *    - 根 HUB 的端口状态通过 hc_driver->hub_status_data() /
 *      hc_driver->hub_control() 与硬件交互
 *    - 根 HUB 控制传输同步执行 (rh_call_control)
 *    - 根 HUB 中断传输通过定时器轮询 (rh_timer) 或硬件事件驱动
 *
 * 3. 总线生命周期管理 (usb_add_hcd / usb_remove_hcd)
 *    - usb_bus_idr: 全局总线编号分配器, 最多 USB_MAXBUS (64) 个总线
 *    - 根 HUB 注册、DMA 掩码设置、PHY 初始化
 *
 * 4. 带宽管理 (usb_hcd_alloc_bandwidth / usb_calc_bus_time)
 *    - 计算和管理 USB 1.x/2.0 等时/中断传输的帧时间占用
 *    - 配置更改时检查带宽是否足够
 *
 * 5. DMA 映射管理 (map_urb_for_dma / unmap_urb_for_dma)
 *    - 为 URB 的传输缓冲区和设置包建立 DMA 映射
 *    - 支持 localmem (SRAM) bounce buffer、SG 列表、分散/聚集 I/O
 *
 * --------------------------------------------------------------------------
 * 全局状态:
 *
 * - usb_bus_idr (IDR): 全局总线编号池, 用于为每个注册的 HCD 分配唯一总线号
 * - usb_bus_idr_lock (mutex): 保护 usb_bus_idr 并发访问
 * - hcd_root_hub_lock (spinlock): 保护根 HUB 相关的状态 (status_urb、rh_registered)
 * - hcd_urb_list_lock (spinlock): 保护端点的 URB 链表操作
 * - hcd_urb_unlink_lock (spinlock): 保护 URB 取消链接的并发安全
 * - usb_kill_urb_queue (wait_queue): 同步等待 URB 被彻底终止的等待队列
 *
 * --------------------------------------------------------------------------
 * 架构分层:
 *
 *   USB 设备驱动 (usb_driver)
 *        |
 *   USB Core (usbcore)
 *        |
 *   HCD 框架 (本文件) ← 提供 usb_bus、URB 管理、根 HUB 模拟
 *        |
 *   hc_driver 操作集 (struct hc_driver)
 *        |
 *   主机控制器硬件 (xHCI / EHCI / OHCI / UHCI)
 *
 * --------------------------------------------------------------------------
 * 根 HUB 特殊说明:
 *
 * 根 HUB 不是真正的物理 USB 设备, 而是 HCD 软件模拟的虚拟设备。
 * 它的设备描述符 (usb*_rh_dev_descriptor) 和配置描述符 (fs/hs/ss_rh_config_descriptor)
 * 在此文件中硬编码。对根 HUB 的 URB 请求直接由 rh_urb_enqueue() 截获处理,
 * 无需经过真实的 USB 总线传输.
 *
 * 根 HUB 端口操作的典型调用链:
 *   hub_events() [hub_wq] → hub_port_xxx_change() →
 *   usb_hcd_submit_urb() → rh_urb_enqueue() → rh_call_control() →
 *   hcd->driver->hub_control() [进入具体 HC 驱动]
 *
 * --------------------------------------------------------------------------
 * URB 提交流程详解:
 *
 *   usb_submit_urb()                         [USB Core / usb_urb.c]
 *     └→ usb_hcd_submit_urb()               [本文件]
 *          ├→ usb_get_urb() / atomic_inc(&urb->use_count)   [引用计数保护]
 *          ├→ 根 HUB? → rh_urb_enqueue()    [软件模拟路径]
 *          │    ├→ 控制传输 → rh_call_control()  [同步执行]
 *          │    └→ 中断传输 → rh_queue_status() [定时器轮询]
 *          └→ 硬件设备 → map_urb_for_dma()  [DMA 映射]
 *                          └→ hcd->driver->urb_enqueue()    [进入 HC 驱动]
 *                                └→ (硬件完成) → usb_hcd_giveback_urb()
 *                                     └→ tasklet (BH) → urb->complete()
 *
 * --------------------------------------------------------------------------
 */

#include <linux/bcd.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/completion.h>
#include <linux/utsname.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <asm/irq.h>
#include <asm/byteorder.h>
#include <linux/unaligned.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/kcov.h>

#include <linux/phy/phy.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/otg.h>

#include "usb.h"
#include "phy.h"


/*-------------------------------------------------------------------------*/

/*
 * USB Host Controller Driver framework
 *
 * Plugs into usbcore (usb_bus) and lets HCDs share code, minimizing
 * HCD-specific behaviors/bugs.
 *
 * This does error checks, tracks devices and urbs, and delegates to a
 * "hc_driver" only for code (and data) that really needs to know about
 * hardware differences.  That includes root hub registers, i/o queues,
 * and so on ... but as little else as possible.
 *
 * Shared code includes most of the "root hub" code (these are emulated,
 * though each HC's hardware works differently) and PCI glue, plus request
 * tracking overhead.  The HCD code should only block on spinlocks or on
 * hardware handshaking; blocking on software events (such as other kernel
 * threads releasing resources, or completing actions) is all generic.
 *
 * Happens the USB 2.0 spec says this would be invisible inside the "USBD",
 * and includes mostly a "HCDI" (HCD Interface) along with some APIs used
 * only by the hub driver ... and that neither should be seen or used by
 * usb client device drivers.
 *
 * Contributors of ideas or unattributed patches include: David Brownell,
 * Roman Weissgaerber, Rory Bolt, Greg Kroah-Hartman, ...
 *
 * HISTORY:
 * 2002-02-21	Pull in most of the usb_bus support from usb.c; some
 *		associated cleanup.  "usb_hcd" still != "usb_bus".
 * 2001-12-12	Initial patch version for Linux 2.5.1 kernel.
 */

/*-------------------------------------------------------------------------*/

/* host controllers we manage */
/*
 * usb_bus_idr -- 全局 USB 总线编号分配器 (IDR 映射表)
 *
 * 为每个注册的 HCD 分配一个唯一的总线编号 (1 ~ USB_MAXBUS-1)。
 * 通过 idr_alloc() / idr_remove() 管理。
 * 保护锁: usb_bus_idr_lock (mutex)
 * 导出给 usbfs 使用.
 */
DEFINE_IDR (usb_bus_idr);
EXPORT_SYMBOL_GPL (usb_bus_idr);

/* used when allocating bus numbers */
#define USB_MAXBUS		64

/*
 * usb_bus_idr_lock -- 保护 usb_bus_idr 并发访问的互斥锁
 * 导出给 usbfs 用于枚举系统中的 USB 总线.
 */
/* used when updating list of hcds */
DEFINE_MUTEX(usb_bus_idr_lock);	/* exported only for usbfs */
EXPORT_SYMBOL_GPL (usb_bus_idr_lock);

/*
 * hcd_root_hub_lock -- 保护根 HUB 相关状态的 spinlock
 *
 * 保护的内容包括:
 *   - hcd->status_urb (根 HUB 中断传输的 URB)
 *   - hcd->rh_registered (根 HUB 是否已注册)
 *   - HCD_FLAG_POLL_PENDING (轮询挂起标志)
 *   - HCD_FLAG_WAKEUP_PENDING (唤醒挂起标志)
 * 注意: 此锁在中断上下文中使用, 必须 irqsave.
 */
/* used for controlling access to virtual root hubs */
static DEFINE_SPINLOCK(hcd_root_hub_lock);

/*
 * hcd_urb_list_lock -- 保护端点 URB 链表的 spinlock
 *
 * 保护 usb_host_endpoint->urb_list 链表的插入和删除操作。
 * 在 usb_hcd_link_urb_to_ep() 和 usb_hcd_unlink_urb_from_ep() 中使用。
 * 注意: 此锁与 HC 驱动的私有锁存在锁顺序要求 (先取此锁, 再取 HC 锁).
 */
/* used when updating an endpoint's URB list */
static DEFINE_SPINLOCK(hcd_urb_list_lock);

/*
 * hcd_urb_unlink_lock -- 保护 URB 取消链接的并发安全
 *
 * 防止在设备已断开后仍有驱动程序尝试取消 URB。
 * usb_hcd_unlink_urb() 在此锁的保护下检查 urb->use_count,
 * 确保设备引用计数正确后再执行取消操作。
 * usb_hcd_synchronize_unlinks() 通过获取再释放此锁实现同步等待.
 */
/* used to protect against unlinking URBs after the device is gone */
static DEFINE_SPINLOCK(hcd_urb_unlink_lock);

/*
 * usb_kill_urb_queue -- 同步等待 URB 终止的等待队列
 *
 * 当 usb_kill_urb() / usb_poison_urb() 将 urb->reject 置为 1 后,
 * 需要等待正在执行的 URB 完成。URB 的 complete 回调在递减 use_count
 * 后, 会检查 reject 标志并唤醒此队列.
 * 通过 smp_mb__after_atomic() 内存屏障保证 use_count 的写入
 * 在读取 reject 之前全局可见.
 */
/* wait queue for synchronous unlinks */
DECLARE_WAIT_QUEUE_HEAD(usb_kill_urb_queue);

/*-------------------------------------------------------------------------*/

/*
 * Sharable chunks of root hub code.
 */

/*-------------------------------------------------------------------------*/
#define KERNEL_REL	bin2bcd(LINUX_VERSION_MAJOR)
#define KERNEL_VER	bin2bcd(LINUX_VERSION_PATCHLEVEL)

/*
 * 根 HUB 设备描述符 -- 硬编码的虚拟 USB 设备描述符
 *
 * 每个主机控制器在软件中模拟一个根 HUB, 其设备描述符根据 USB 协议版本
 * 分为四种: USB 1.1 (usb11), USB 2.0 (usb2), USB 3.0 (usb3), USB 3.1 (usb31).
 * 根 HUB 的类代码固定为 HUB_CLASSCODE (0x09), 设备协议字段标识 HUB 类型.
 *
 * Linux 根 HUB 的 idVendor 固定为 0x1d6b (Linux Foundation),
 * idProduct 表示 HUB 版本: 0x0001 (USB 1.1), 0x0002 (USB 2.0), 0x0003 (USB 3.x).
 *
 * 注意: bMaxPacketSize0 字段:
 *   - USB 1.x/2.0 根 HUB: 64 字节 (USB 规范要求的端点 0 最大包大小)
 *   - USB 3.x 根 HUB: 512 字节 (2^9, SuperSpeed 强制要求)
 */

/* usb 3.1 root hub device descriptor */
static const u8 usb31_rh_dev_descriptor[18] = {
	0x12,       /*  __u8  bLength; */
	USB_DT_DEVICE, /* __u8 bDescriptorType; Device */
	0x10, 0x03, /*  __le16 bcdUSB; v3.1 */

	0x09,	    /*  __u8  bDeviceClass; HUB_CLASSCODE */
	0x00,	    /*  __u8  bDeviceSubClass; */
	0x03,       /*  __u8  bDeviceProtocol; USB 3 hub */
	0x09,       /*  __u8  bMaxPacketSize0; 2^9 = 512 Bytes */

	0x6b, 0x1d, /*  __le16 idVendor; Linux Foundation 0x1d6b */
	0x03, 0x00, /*  __le16 idProduct; device 0x0003 */
	KERNEL_VER, KERNEL_REL, /*  __le16 bcdDevice */

	0x03,       /*  __u8  iManufacturer; */
	0x02,       /*  __u8  iProduct; */
	0x01,       /*  __u8  iSerialNumber; */
	0x01        /*  __u8  bNumConfigurations; */
};

/* usb 3.0 root hub device descriptor */
static const u8 usb3_rh_dev_descriptor[18] = {
	0x12,       /*  __u8  bLength; */
	USB_DT_DEVICE, /* __u8 bDescriptorType; Device */
	0x00, 0x03, /*  __le16 bcdUSB; v3.0 */

	0x09,	    /*  __u8  bDeviceClass; HUB_CLASSCODE */
	0x00,	    /*  __u8  bDeviceSubClass; */
	0x03,       /*  __u8  bDeviceProtocol; USB 3.0 hub */
	0x09,       /*  __u8  bMaxPacketSize0; 2^9 = 512 Bytes */

	0x6b, 0x1d, /*  __le16 idVendor; Linux Foundation 0x1d6b */
	0x03, 0x00, /*  __le16 idProduct; device 0x0003 */
	KERNEL_VER, KERNEL_REL, /*  __le16 bcdDevice */

	0x03,       /*  __u8  iManufacturer; */
	0x02,       /*  __u8  iProduct; */
	0x01,       /*  __u8  iSerialNumber; */
	0x01        /*  __u8  bNumConfigurations; */
};

/* usb 2.0 root hub device descriptor */
static const u8 usb2_rh_dev_descriptor[18] = {
	0x12,       /*  __u8  bLength; */
	USB_DT_DEVICE, /* __u8 bDescriptorType; Device */
	0x00, 0x02, /*  __le16 bcdUSB; v2.0 */

	0x09,	    /*  __u8  bDeviceClass; HUB_CLASSCODE */
	0x00,	    /*  __u8  bDeviceSubClass; */
	0x00,       /*  __u8  bDeviceProtocol; [ usb 2.0 no TT ] */
	0x40,       /*  __u8  bMaxPacketSize0; 64 Bytes */

	0x6b, 0x1d, /*  __le16 idVendor; Linux Foundation 0x1d6b */
	0x02, 0x00, /*  __le16 idProduct; device 0x0002 */
	KERNEL_VER, KERNEL_REL, /*  __le16 bcdDevice */

	0x03,       /*  __u8  iManufacturer; */
	0x02,       /*  __u8  iProduct; */
	0x01,       /*  __u8  iSerialNumber; */
	0x01        /*  __u8  bNumConfigurations; */
};

/* no usb 2.0 root hub "device qualifier" descriptor: one speed only */

/* usb 1.1 root hub device descriptor */
static const u8 usb11_rh_dev_descriptor[18] = {
	0x12,       /*  __u8  bLength; */
	USB_DT_DEVICE, /* __u8 bDescriptorType; Device */
	0x10, 0x01, /*  __le16 bcdUSB; v1.1 */

	0x09,	    /*  __u8  bDeviceClass; HUB_CLASSCODE */
	0x00,	    /*  __u8  bDeviceSubClass; */
	0x00,       /*  __u8  bDeviceProtocol; [ low/full speeds only ] */
	0x40,       /*  __u8  bMaxPacketSize0; 64 Bytes */

	0x6b, 0x1d, /*  __le16 idVendor; Linux Foundation 0x1d6b */
	0x01, 0x00, /*  __le16 idProduct; device 0x0001 */
	KERNEL_VER, KERNEL_REL, /*  __le16 bcdDevice */

	0x03,       /*  __u8  iManufacturer; */
	0x02,       /*  __u8  iProduct; */
	0x01,       /*  __u8  iSerialNumber; */
	0x01        /*  __u8  bNumConfigurations; */
};


/*-------------------------------------------------------------------------*/

/*
 * 根 HUB 配置描述符 -- 硬编码的虚拟 HUB 配置
 *
 * 每种速率 (FS/HS/SS) 的根 HUB 都有一个对应的配置描述符,
 * 包含一个接口 (HUB 类) 和一个中断输入端点 (状态变化通知).
 *
 * 配置差异:
 *   - FS (Full Speed, USB 1.1): 中断端点间隔 255ms, wMaxPacketSize = 2 字节
 *   - HS (High Speed, USB 2.0): 中断端点间隔 256ms (0x0c = 12, 2^(12-1) ms)
 *   - SS (SuperSpeed, USB 3.x): 包含 SuperSpeed Endpoint Companion 描述符,
 *     支持更大的突发传输
 *
 * 中断端点的 wMaxPacketSize 由 hub_configure() 根据 USB_MAXCHILDREN 动态确定:
 *   size = 1 + (USB_MAXCHILDREN + 7) / 8 (字节)
 * 用于报告端口状态变化位图.
 */

/* Configuration descriptors for our root hubs */

static const u8 fs_rh_config_descriptor[] = {

	/* one configuration */
	0x09,       /*  __u8  bLength; */
	USB_DT_CONFIG, /* __u8 bDescriptorType; Configuration */
	0x19, 0x00, /*  __le16 wTotalLength; */
	0x01,       /*  __u8  bNumInterfaces; (1) */
	0x01,       /*  __u8  bConfigurationValue; */
	0x00,       /*  __u8  iConfiguration; */
	0xc0,       /*  __u8  bmAttributes;
				 Bit 7: must be set,
				     6: Self-powered,
				     5: Remote wakeup,
				     4..0: resvd */
	0x00,       /*  __u8  MaxPower; */

	/* USB 1.1:
	 * USB 2.0, single TT organization (mandatory):
	 *	one interface, protocol 0
	 *
	 * USB 2.0, multiple TT organization (optional):
	 *	two interfaces, protocols 1 (like single TT)
	 *	and 2 (multiple TT mode) ... config is
	 *	sometimes settable
	 *	NOT IMPLEMENTED
	 */

	/* one interface */
	0x09,       /*  __u8  if_bLength; */
	USB_DT_INTERFACE,  /* __u8 if_bDescriptorType; Interface */
	0x00,       /*  __u8  if_bInterfaceNumber; */
	0x00,       /*  __u8  if_bAlternateSetting; */
	0x01,       /*  __u8  if_bNumEndpoints; */
	0x09,       /*  __u8  if_bInterfaceClass; HUB_CLASSCODE */
	0x00,       /*  __u8  if_bInterfaceSubClass; */
	0x00,       /*  __u8  if_bInterfaceProtocol; [usb1.1 or single tt] */
	0x00,       /*  __u8  if_iInterface; */

	/* one endpoint (status change endpoint) */
	0x07,       /*  __u8  ep_bLength; */
	USB_DT_ENDPOINT, /* __u8 ep_bDescriptorType; Endpoint */
	0x81,       /*  __u8  ep_bEndpointAddress; IN Endpoint 1 */
	0x03,       /*  __u8  ep_bmAttributes; Interrupt */
	0x02, 0x00, /*  __le16 ep_wMaxPacketSize; 1 + (MAX_ROOT_PORTS / 8) */
	0xff        /*  __u8  ep_bInterval; (255ms -- usb 2.0 spec) */
};

static const u8 hs_rh_config_descriptor[] = {

	/* one configuration */
	0x09,       /*  __u8  bLength; */
	USB_DT_CONFIG, /* __u8 bDescriptorType; Configuration */
	0x19, 0x00, /*  __le16 wTotalLength; */
	0x01,       /*  __u8  bNumInterfaces; (1) */
	0x01,       /*  __u8  bConfigurationValue; */
	0x00,       /*  __u8  iConfiguration; */
	0xc0,       /*  __u8  bmAttributes;
				 Bit 7: must be set,
				     6: Self-powered,
				     5: Remote wakeup,
				     4..0: resvd */
	0x00,       /*  __u8  MaxPower; */

	/* USB 1.1:
	 * USB 2.0, single TT organization (mandatory):
	 *	one interface, protocol 0
	 *
	 * USB 2.0, multiple TT organization (optional):
	 *	two interfaces, protocols 1 (like single TT)
	 *	and 2 (multiple TT mode) ... config is
	 *	sometimes settable
	 *	NOT IMPLEMENTED
	 */

	/* one interface */
	0x09,       /*  __u8  if_bLength; */
	USB_DT_INTERFACE, /* __u8 if_bDescriptorType; Interface */
	0x00,       /*  __u8  if_bInterfaceNumber; */
	0x00,       /*  __u8  if_bAlternateSetting; */
	0x01,       /*  __u8  if_bNumEndpoints; */
	0x09,       /*  __u8  if_bInterfaceClass; HUB_CLASSCODE */
	0x00,       /*  __u8  if_bInterfaceSubClass; */
	0x00,       /*  __u8  if_bInterfaceProtocol; [usb1.1 or single tt] */
	0x00,       /*  __u8  if_iInterface; */

	/* one endpoint (status change endpoint) */
	0x07,       /*  __u8  ep_bLength; */
	USB_DT_ENDPOINT, /* __u8 ep_bDescriptorType; Endpoint */
	0x81,       /*  __u8  ep_bEndpointAddress; IN Endpoint 1 */
	0x03,       /*  __u8  ep_bmAttributes; Interrupt */
		    /* __le16 ep_wMaxPacketSize; 1 + (MAX_ROOT_PORTS / 8)
		     * see hub.c:hub_configure() for details. */
	(USB_MAXCHILDREN + 1 + 7) / 8, 0x00,
	0x0c        /*  __u8  ep_bInterval; (256ms -- usb 2.0 spec) */
};

static const u8 ss_rh_config_descriptor[] = {
	/* one configuration */
	0x09,       /*  __u8  bLength; */
	USB_DT_CONFIG, /* __u8 bDescriptorType; Configuration */
	0x1f, 0x00, /*  __le16 wTotalLength; */
	0x01,       /*  __u8  bNumInterfaces; (1) */
	0x01,       /*  __u8  bConfigurationValue; */
	0x00,       /*  __u8  iConfiguration; */
	0xc0,       /*  __u8  bmAttributes;
				 Bit 7: must be set,
				     6: Self-powered,
				     5: Remote wakeup,
				     4..0: resvd */
	0x00,       /*  __u8  MaxPower; */

	/* one interface */
	0x09,       /*  __u8  if_bLength; */
	USB_DT_INTERFACE, /* __u8 if_bDescriptorType; Interface */
	0x00,       /*  __u8  if_bInterfaceNumber; */
	0x00,       /*  __u8  if_bAlternateSetting; */
	0x01,       /*  __u8  if_bNumEndpoints; */
	0x09,       /*  __u8  if_bInterfaceClass; HUB_CLASSCODE */
	0x00,       /*  __u8  if_bInterfaceSubClass; */
	0x00,       /*  __u8  if_bInterfaceProtocol; */
	0x00,       /*  __u8  if_iInterface; */

	/* one endpoint (status change endpoint) */
	0x07,       /*  __u8  ep_bLength; */
	USB_DT_ENDPOINT, /* __u8 ep_bDescriptorType; Endpoint */
	0x81,       /*  __u8  ep_bEndpointAddress; IN Endpoint 1 */
	0x03,       /*  __u8  ep_bmAttributes; Interrupt */
		    /* __le16 ep_wMaxPacketSize; 1 + (MAX_ROOT_PORTS / 8)
		     * see hub.c:hub_configure() for details. */
	(USB_MAXCHILDREN + 1 + 7) / 8, 0x00,
	0x0c,       /*  __u8  ep_bInterval; (256ms -- usb 2.0 spec) */

	/* one SuperSpeed endpoint companion descriptor */
	0x06,        /* __u8 ss_bLength */
	USB_DT_SS_ENDPOINT_COMP, /* __u8 ss_bDescriptorType; SuperSpeed EP */
		     /* Companion */
	0x00,        /* __u8 ss_bMaxBurst; allows 1 TX between ACKs */
	0x00,        /* __u8 ss_bmAttributes; 1 packet per service interval */
	0x02, 0x00   /* __le16 ss_wBytesPerInterval; 15 bits for max 15 ports */
};

/* authorized_default behaviour:
 * -1 is authorized for all devices (leftover from wireless USB)
 * 0 is unauthorized for all devices
 * 1 is authorized for all devices
 * 2 is authorized for internal devices
 */
#define USB_AUTHORIZE_WIRED	-1
#define USB_AUTHORIZE_NONE	0
#define USB_AUTHORIZE_ALL	1
#define USB_AUTHORIZE_INTERNAL	2

static int authorized_default = CONFIG_USB_DEFAULT_AUTHORIZATION_MODE;
module_param(authorized_default, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(authorized_default,
		"Default USB device authorization: 0 is not authorized, 1 is authorized (default), 2 is authorized for internal devices, -1 is authorized (same as 1)");
/*-------------------------------------------------------------------------*/

/*
 * ascii2desc -- 将 ASCII 字符串转换为 USB UTF-16LE 字符串描述符
 *
 * USB 字符串描述符的格式: 1 字节长度 + 1 字节描述符类型 (USB_DT_STRING) + UTF-16LE 编码的字符串数据。
 * 此函数将 ISO-8859-1 (扩展 ASCII) 字符串转换为这种格式.
 *
 * 注意: USB 规范规定字符串描述符最多 126 个字符 (总共 254 字节).
 */
/**
 * ascii2desc() - Helper routine for producing UTF-16LE string descriptors
 * @s: Null-terminated ASCII (actually ISO-8859-1) string
 * @buf: Buffer for USB string descriptor (header + UTF-16LE)
 * @len: Length (in bytes; may be odd) of descriptor buffer.
 *
 * Return: The number of bytes filled in: 2 + 2*strlen(s) or @len,
 * whichever is less.
 *
 * Note:
 * USB String descriptors can contain at most 126 characters; input
 * strings longer than that are truncated.
 */
static unsigned
ascii2desc(char const *s, u8 *buf, unsigned len)
{
	unsigned n, t = 2 + 2*strlen(s);

	if (t > 254)
		t = 254;	/* Longest possible UTF string descriptor */
	if (len > t)
		len = t;

	t += USB_DT_STRING << 8;	/* Now t is first 16 bits to store */

	n = len;
	while (n--) {
		*buf++ = t;
		if (!n--)
			break;
		*buf++ = t >> 8;
		t = (unsigned char)*s++;
	}
	return len;
}

/*
 * rh_string -- 为根 HUB 提供字符串描述符
 *
 * 支持三种字符串描述符:
 *   id=0: 支持的语言 ID 列表 (固定为英语-美国 0x0409)
 *   id=1: 序列号 (使用 hcd->self.bus_name, 即总线名称)
 *   id=2: 产品名称 (使用 hcd->product_desc)
 *   id=3: 制造商 (格式: "sysname release driver_description")
 *
 * 这些字符串描述符由 rh_call_control() 在收到 USB_REQ_GET_DESCRIPTOR
 * 类型为 USB_DT_STRING 时调用。
 */
/**
 * rh_string() - provides string descriptors for root hub
 * @id: the string ID number (0: langids, 1: serial #, 2: product, 3: vendor)
 * @hcd: the host controller for this root hub
 * @data: buffer for output packet
 * @len: length of the provided buffer
 *
 * Produces either a manufacturer, product or serial number string for the
 * virtual root hub device.
 *
 * Return: The number of bytes filled in: the length of the descriptor or
 * of the provided buffer, whichever is less.
 */
static unsigned
rh_string(int id, struct usb_hcd const *hcd, u8 *data, unsigned len)
{
	char buf[160];
	char const *s;
	static char const langids[4] = {4, USB_DT_STRING, 0x09, 0x04};

	/* language ids */
	switch (id) {
	case 0:
		/* Array of LANGID codes (0x0409 is MSFT-speak for "en-us") */
		/* See http://www.usb.org/developers/docs/USB_LANGIDs.pdf */
		if (len > 4)
			len = 4;
		memcpy(data, langids, len);
		return len;
	case 1:
		/* Serial number */
		s = hcd->self.bus_name;
		break;
	case 2:
		/* Product name */
		s = hcd->product_desc;
		break;
	case 3:
		/* Manufacturer */
		snprintf (buf, sizeof buf, "%s %s %s", init_utsname()->sysname,
			init_utsname()->release, hcd->driver->description);
		s = buf;
		break;
	default:
		/* Can't happen; caller guarantees it */
		return 0;
	}

	return ascii2desc(s, data, len);
}


/*
 * rh_call_control -- 处理根 HUB 的控制传输 (同步执行)
 *
 * 根 HUB 的控制传输不需要经过真实的 USB 总线, 而是完全在软件中模拟完成。
 * 此函数直接解析 URB 中的 USB 控制请求 (struct usb_ctrlrequest), 模拟
 * 根 HUB 设备对这些请求的响应.
 *
 * 处理的标准设备请求 (DEVICE REQUESTS):
 *   - GET_STATUS:        返回根 HUB 的远程唤醒和自供电状态
 *   - CLEAR/SET_FEATURE: 管理根 HUB 的远程唤醒功能 (通过 device_may_wakeup)
 *   - GET/SET_CONFIG:    root hub 固定为配置 1
 *   - GET_DESCRIPTOR:    返回设备描述符、配置描述符或字符串描述符
 *   - GET/SET_INTERFACE: root hub 固定为接口 0 (无实际意义)
 *   - SET_ADDRESS:       设置根 HUB 的设备地址 (通常为 1)
 *
 * 处理的类请求 (CLASS REQUESTS, 转发给 hc_driver->hub_control):
 *   - GetHubStatus / GetPortStatus / GetHubDescriptor
 *   - SetPortFeature / ClearPortFeature (通过 hub_control 的 typeReq)
 *
 * 关键设计:
 *   1. 同步执行: 所有操作在当前上下文中完成, 不涉及硬件 DMA
 *   2. 调用 usb_hcd_giveback_urb() 同步返回 (非 BH 路径)
 *   3. 使用 hcd_root_hub_lock 保护状态一致性
 *   4. 对硬件相关的请求 (如端口操作) 委托给 hc_driver->hub_control()
 *   5. 注意 patch_wakeup 和 patch_protocol 标志: 在返回的描述符中
 *      动态修正远程唤醒能力和 Transaction Translator (TT) 支持位
 *
 * 参数:
 *   @hcd:  主机控制器
 *   @urb:  根 HUB 控制传输的 URB (urb->setup_packet 包含控制请求)
 *
 * 返回: 0 表示成功 (URB 已通过 giveback 返回)
 *       注意: 实际错误通过 urb->status 传递
 */
/* Root hub control transfers execute synchronously */
static int rh_call_control (struct usb_hcd *hcd, struct urb *urb)
{
	struct usb_ctrlrequest *cmd;
	u16		typeReq, wValue, wIndex, wLength;
	u8		*ubuf = urb->transfer_buffer;
	unsigned	len = 0;
	int		status;
	u8		patch_wakeup = 0;
	u8		patch_protocol = 0;
	u16		tbuf_size;
	u8		*tbuf = NULL;
	const u8	*bufp;

	might_sleep();

	spin_lock_irq(&hcd_root_hub_lock);
	status = usb_hcd_link_urb_to_ep(hcd, urb);
	spin_unlock_irq(&hcd_root_hub_lock);
	if (status)
		return status;
	urb->hcpriv = hcd;	/* Indicate it's queued */

	cmd = (struct usb_ctrlrequest *) urb->setup_packet;
	typeReq  = (cmd->bRequestType << 8) | cmd->bRequest;
	wValue   = le16_to_cpu (cmd->wValue);
	wIndex   = le16_to_cpu (cmd->wIndex);
	wLength  = le16_to_cpu (cmd->wLength);

	if (wLength > urb->transfer_buffer_length)
		goto error;

	/*
	 * tbuf should be at least as big as the
	 * USB hub descriptor.
	 */
	tbuf_size =  max_t(u16, sizeof(struct usb_hub_descriptor), wLength);
	tbuf = kzalloc(tbuf_size, GFP_KERNEL);
	if (!tbuf) {
		status = -ENOMEM;
		goto err_alloc;
	}

	bufp = tbuf;


	urb->actual_length = 0;
	switch (typeReq) {

	/* DEVICE REQUESTS */

	/* The root hub's remote wakeup enable bit is implemented using
	 * driver model wakeup flags.  If this system supports wakeup
	 * through USB, userspace may change the default "allow wakeup"
	 * policy through sysfs or these calls.
	 *
	 * Most root hubs support wakeup from downstream devices, for
	 * runtime power management (disabling USB clocks and reducing
	 * VBUS power usage).  However, not all of them do so; silicon,
	 * board, and BIOS bugs here are not uncommon, so these can't
	 * be treated quite like external hubs.
	 *
	 * Likewise, not all root hubs will pass wakeup events upstream,
	 * to wake up the whole system.  So don't assume root hub and
	 * controller capabilities are identical.
	 */

	case DeviceRequest | USB_REQ_GET_STATUS:
		tbuf[0] = (device_may_wakeup(&hcd->self.root_hub->dev)
					<< USB_DEVICE_REMOTE_WAKEUP)
				| (1 << USB_DEVICE_SELF_POWERED);
		tbuf[1] = 0;
		len = 2;
		break;
	case DeviceOutRequest | USB_REQ_CLEAR_FEATURE:
		if (wValue == USB_DEVICE_REMOTE_WAKEUP)
			device_set_wakeup_enable(&hcd->self.root_hub->dev, 0);
		else
			goto error;
		break;
	case DeviceOutRequest | USB_REQ_SET_FEATURE:
		if (device_can_wakeup(&hcd->self.root_hub->dev)
				&& wValue == USB_DEVICE_REMOTE_WAKEUP)
			device_set_wakeup_enable(&hcd->self.root_hub->dev, 1);
		else
			goto error;
		break;
	case DeviceRequest | USB_REQ_GET_CONFIGURATION:
		tbuf[0] = 1;
		len = 1;
		fallthrough;
	case DeviceOutRequest | USB_REQ_SET_CONFIGURATION:
		break;
	case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
		switch (wValue & 0xff00) {
		case USB_DT_DEVICE << 8:
			switch (hcd->speed) {
			case HCD_USB32:
			case HCD_USB31:
				bufp = usb31_rh_dev_descriptor;
				break;
			case HCD_USB3:
				bufp = usb3_rh_dev_descriptor;
				break;
			case HCD_USB2:
				bufp = usb2_rh_dev_descriptor;
				break;
			case HCD_USB11:
				bufp = usb11_rh_dev_descriptor;
				break;
			default:
				goto error;
			}
			len = 18;
			if (hcd->has_tt)
				patch_protocol = 1;
			break;
		case USB_DT_CONFIG << 8:
			switch (hcd->speed) {
			case HCD_USB32:
			case HCD_USB31:
			case HCD_USB3:
				bufp = ss_rh_config_descriptor;
				len = sizeof ss_rh_config_descriptor;
				break;
			case HCD_USB2:
				bufp = hs_rh_config_descriptor;
				len = sizeof hs_rh_config_descriptor;
				break;
			case HCD_USB11:
				bufp = fs_rh_config_descriptor;
				len = sizeof fs_rh_config_descriptor;
				break;
			default:
				goto error;
			}
			if (device_can_wakeup(&hcd->self.root_hub->dev))
				patch_wakeup = 1;
			break;
		case USB_DT_STRING << 8:
			if ((wValue & 0xff) < 4)
				urb->actual_length = rh_string(wValue & 0xff,
						hcd, ubuf, wLength);
			else /* unsupported IDs --> "protocol stall" */
				goto error;
			break;
		case USB_DT_BOS << 8:
			goto nongeneric;
		default:
			goto error;
		}
		break;
	case DeviceRequest | USB_REQ_GET_INTERFACE:
		tbuf[0] = 0;
		len = 1;
		fallthrough;
	case DeviceOutRequest | USB_REQ_SET_INTERFACE:
		break;
	case DeviceOutRequest | USB_REQ_SET_ADDRESS:
		/* wValue == urb->dev->devaddr */
		dev_dbg (hcd->self.controller, "root hub device address %d\n",
			wValue);
		break;

	/* INTERFACE REQUESTS (no defined feature/status flags) */

	/* ENDPOINT REQUESTS */

	case EndpointRequest | USB_REQ_GET_STATUS:
		/* ENDPOINT_HALT flag */
		tbuf[0] = 0;
		tbuf[1] = 0;
		len = 2;
		fallthrough;
	case EndpointOutRequest | USB_REQ_CLEAR_FEATURE:
	case EndpointOutRequest | USB_REQ_SET_FEATURE:
		dev_dbg (hcd->self.controller, "no endpoint features yet\n");
		break;

	/* CLASS REQUESTS (and errors) */

	default:
nongeneric:
		/* non-generic request */
		switch (typeReq) {
		case GetHubStatus:
			len = 4;
			break;
		case GetPortStatus:
			if (wValue == HUB_PORT_STATUS)
				len = 4;
			else
				/* other port status types return 8 bytes */
				len = 8;
			break;
		case GetHubDescriptor:
			len = sizeof (struct usb_hub_descriptor);
			break;
		case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
			/* len is returned by hub_control */
			break;
		}
		status = hcd->driver->hub_control (hcd,
			typeReq, wValue, wIndex,
			tbuf, wLength);

		if (typeReq == GetHubDescriptor)
			usb_hub_adjust_deviceremovable(hcd->self.root_hub,
				(struct usb_hub_descriptor *)tbuf);
		break;
error:
		/* "protocol stall" on error */
		status = -EPIPE;
	}

	if (status < 0) {
		len = 0;
		if (status != -EPIPE) {
			dev_dbg (hcd->self.controller,
				"CTRL: TypeReq=0x%x val=0x%x "
				"idx=0x%x len=%d ==> %d\n",
				typeReq, wValue, wIndex,
				wLength, status);
		}
	} else if (status > 0) {
		/* hub_control may return the length of data copied. */
		len = status;
		status = 0;
	}
	if (len) {
		if (urb->transfer_buffer_length < len)
			len = urb->transfer_buffer_length;
		urb->actual_length = len;
		/* always USB_DIR_IN, toward host */
		memcpy (ubuf, bufp, len);

		/* report whether RH hardware supports remote wakeup */
		if (patch_wakeup &&
				len > offsetof (struct usb_config_descriptor,
						bmAttributes))
			((struct usb_config_descriptor *)ubuf)->bmAttributes
				|= USB_CONFIG_ATT_WAKEUP;

		/* report whether RH hardware has an integrated TT */
		if (patch_protocol &&
				len > offsetof(struct usb_device_descriptor,
						bDeviceProtocol))
			((struct usb_device_descriptor *) ubuf)->
				bDeviceProtocol = USB_HUB_PR_HS_SINGLE_TT;
	}

	kfree(tbuf);
 err_alloc:

	/* any errors get returned through the urb completion */
	spin_lock_irq(&hcd_root_hub_lock);
	usb_hcd_unlink_urb_from_ep(hcd, urb);
	usb_hcd_giveback_urb(hcd, urb, status);
	spin_unlock_irq(&hcd_root_hub_lock);
	return 0;
}

/*-------------------------------------------------------------------------*/

/*
 * usb_hcd_poll_rh_status -- 轮询根 HUB 状态变化并通知上层
 *
 * 根 HUB 的状态变化 (如端口连接/断开、过流等) 通过两种方式检测:
 *   1. 定时器轮询: 由 rh_timer 定时触发此函数 (适用于不支持中断通知的 HC)
 *   2. 硬件事件驱动: HC 驱动在检测到状态变化时主动调用此函数
 *
 * 工作流程:
 *   1. 调用 hc_driver->hub_status_data() 从硬件获取端口状态变化位图
 *   2. 如果有状态变化 (length > 0):
 *      a. 如果存在 status_urb (上层已提交中断传输 URB),
 *         将状态数据填充到 URB 的 transfer_buffer 中, 然后通过
 *         usb_hcd_giveback_urb() 返回给上层 (hub_wq)
 *      b. 如果不存在 status_urb, 设置 POLL_PENDING 标志,
 *         表示有未处理的状态变化 (待上层提交 status URB 后再处理)
 *   3. 如果没有状态变化且 status_urb 仍存在 (旧轮询模式),
 *      重新调度定时器在 250ms 后再次轮询
 *
 * buffer[6] 数组的设计: 每位表示一个端口, 最多支持 31 个端口 (4 字节).
 * 使用 6 字节以兼容未来扩展.
 *
 * 注意: 此函数可能在中断上下文调用 (当 HC 驱动主动调用时),
 * 也可能在软中断或进程上下文调用 (定时器回调).
 */
/*
 * Root Hub interrupt transfers are polled using a timer if the
 * driver requests it; otherwise the driver is responsible for
 * calling usb_hcd_poll_rh_status() when an event occurs.
 *
 * Completion handler may not sleep. See usb_hcd_giveback_urb() for details.
 */
void usb_hcd_poll_rh_status(struct usb_hcd *hcd)
{
	struct urb	*urb;
	int		length;
	int		status;
	unsigned long	flags;
	char		buffer[6];	/* Any root hubs with > 31 ports? */

	if (unlikely(!hcd->rh_pollable))
		return;
	if (!hcd->uses_new_polling && !hcd->status_urb)
		return;

	length = hcd->driver->hub_status_data(hcd, buffer);
	if (length > 0) {

		/* try to complete the status urb */
		spin_lock_irqsave(&hcd_root_hub_lock, flags);
		urb = hcd->status_urb;
		if (urb) {
			clear_bit(HCD_FLAG_POLL_PENDING, &hcd->flags);
			hcd->status_urb = NULL;
			if (urb->transfer_buffer_length >= length) {
				status = 0;
			} else {
				status = -EOVERFLOW;
				length = urb->transfer_buffer_length;
			}
			urb->actual_length = length;
			memcpy(urb->transfer_buffer, buffer, length);

			usb_hcd_unlink_urb_from_ep(hcd, urb);
			usb_hcd_giveback_urb(hcd, urb, status);
		} else {
			length = 0;
			set_bit(HCD_FLAG_POLL_PENDING, &hcd->flags);
		}
		spin_unlock_irqrestore(&hcd_root_hub_lock, flags);
	}

	/* The USB 2.0 spec says 256 ms.  This is close enough and won't
	 * exceed that limit if HZ is 100. The math is more clunky than
	 * maybe expected, this is to make sure that all timers for USB devices
	 * fire at the same time to give the CPU a break in between */
	if (hcd->uses_new_polling ? HCD_POLL_RH(hcd) :
			(length == 0 && hcd->status_urb != NULL))
		mod_timer (&hcd->rh_timer, (jiffies/(HZ/4) + 1) * (HZ/4));
}
EXPORT_SYMBOL_GPL(usb_hcd_poll_rh_status);

/* timer callback */
static void rh_timer_func (struct timer_list *t)
{
	struct usb_hcd *_hcd = timer_container_of(_hcd, t, rh_timer);

	usb_hcd_poll_rh_status(_hcd);
}

/*-------------------------------------------------------------------------*/

/*
 * rh_queue_status -- 将根 HUB 状态中断传输 URB 加入等待队列
 *
 * 上层 (hub_wq 线程) 通过提交中断传输 URB 来接收端口状态变化通知。
 * 此函数将该 URB 注册为 hcd->status_urb, 后续当 usb_hcd_poll_rh_status()
 * 检测到端口状态变化时, 会用状态数据填充此 URB 并完成它.
 *
 * 关键设计:
 *   1. 每个 HCD 同一时刻只能有一个未完成的状态 URB (hcd->status_urb)
 *   2. URB 的 transfer_buffer 大小必须 >= 1 + (maxchild / 8) 字节
 *   3. 旧轮询模式 (uses_new_polling=0): 立即启动 rh_timer 开始轮询
 *   4. 新轮询模式: 如果已经有 PENDING 标志, 立即触发轮询以快速响应
 *
 * 注意: status_urb 和 rh_timer 均在 hcd_root_hub_lock 保护下操作.
 */
static int rh_queue_status (struct usb_hcd *hcd, struct urb *urb)
{
	int		retval;
	unsigned long	flags;
	unsigned	len = 1 + (urb->dev->maxchild / 8);

	spin_lock_irqsave (&hcd_root_hub_lock, flags);
	if (hcd->status_urb || urb->transfer_buffer_length < len) {
		dev_dbg (hcd->self.controller, "not queuing rh status urb\n");
		retval = -EINVAL;
		goto done;
	}

	retval = usb_hcd_link_urb_to_ep(hcd, urb);
	if (retval)
		goto done;

	hcd->status_urb = urb;
	urb->hcpriv = hcd;	/* indicate it's queued */
	if (!hcd->uses_new_polling)
		mod_timer(&hcd->rh_timer, (jiffies/(HZ/4) + 1) * (HZ/4));

	/* If a status change has already occurred, report it ASAP */
	else if (HCD_POLL_PENDING(hcd))
		mod_timer(&hcd->rh_timer, jiffies);
	retval = 0;
 done:
	spin_unlock_irqrestore (&hcd_root_hub_lock, flags);
	return retval;
}

/*
 * rh_urb_enqueue -- 根 HUB 的 URB 入队函数 (软件模拟的"硬件"入队)
 *
 * 这是根 HUB 路径的入口点, 代替真实 HC 驱动的 urb_enqueue().
 * 根据端点类型分发到不同的处理路径:
 *   - 控制端点 (端点 0): 调用 rh_call_control() 同步处理
 *   - 中断端点 (端点 1, 状态变化端点): 调用 rh_queue_status() 注册状态通知
 *   - 其他端点类型: 不支持, 返回 -EINVAL
 *
 * 注意: 根 HUB 不支持批量传输和等时传输.
 */
static int rh_urb_enqueue (struct usb_hcd *hcd, struct urb *urb)
{
	if (usb_endpoint_xfer_int(&urb->ep->desc))
		return rh_queue_status (hcd, urb);
	if (usb_endpoint_xfer_control(&urb->ep->desc))
		return rh_call_control (hcd, urb);
	return -EINVAL;
}

/*-------------------------------------------------------------------------*/

/*
 * usb_rh_urb_dequeue -- 取消根 HUB 的 URB
 *
 * 根 HUB 的控制 URB 是同步执行的, 因此取消操作实际上什么也不做
 * (URB 在执行 rh_call_control 时已经完成).
 *
 * 对于中断状态 URB:
 *   1. 停止 rh_timer 定时器 (旧轮询模式)
 *   2. 如果该 URB 就是当前挂起的 status_urb,
 *      将其从链表中移除并通过 usb_hcd_giveback_urb() 返回
 *   3. 将 hcd->status_urb 置为 NULL, 允许后续提交新的状态 URB
 *
 * 注意: 此操作在 hcd_root_hub_lock 保护下进行, 支持中断上下文调用.
 */
/* Unlinks of root-hub control URBs are legal, but they don't do anything
 * since these URBs always execute synchronously.
 */
static int usb_rh_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
	unsigned long	flags;
	int		rc;

	spin_lock_irqsave(&hcd_root_hub_lock, flags);
	rc = usb_hcd_check_unlink_urb(hcd, urb, status);
	if (rc)
		goto done;

	if (usb_endpoint_num(&urb->ep->desc) == 0) {	/* Control URB */
		;	/* Do nothing */

	} else {				/* Status URB */
		if (!hcd->uses_new_polling)
			timer_delete(&hcd->rh_timer);
		if (urb == hcd->status_urb) {
			hcd->status_urb = NULL;
			usb_hcd_unlink_urb_from_ep(hcd, urb);
			usb_hcd_giveback_urb(hcd, urb, status);
		}
	}
 done:
	spin_unlock_irqrestore(&hcd_root_hub_lock, flags);
	return rc;
}


/*-------------------------------------------------------------------------*/

/*
 * usb_bus_init -- 初始化 USB 总线结构体
 *
 * 对 struct usb_bus 进行基本的初始化:
 *   - devmap: 清零设备地址位图 (地址 0~127)
 *   - devnum_next: 下一个可分配的设备号从 1 开始
 *   - root_hub: 根 HUB 指针置空 (后续由 usb_add_hcd 分配)
 *   - busnum: 总线编号初始化为 -1 (未分配)
 *   - bandwidth_*: 带宽统计清零
 *   - 初始化 devnum_next_mutex 互斥锁
 *
 * 注意: 此函数仅初始化通用字段, 不分配任何资源。
 * 总线编号的分配由 usb_register_bus() 通过 idr_alloc 完成.
 */
/**
 * usb_bus_init - shared initialization code
 * @bus: the bus structure being initialized
 *
 * This code is used to initialize a usb_bus structure, memory for which is
 * separately managed.
 */
static void usb_bus_init (struct usb_bus *bus)
{
	memset(&bus->devmap, 0, sizeof(bus->devmap));

	bus->devnum_next = 1;

	bus->root_hub = NULL;
	bus->busnum = -1;
	bus->bandwidth_allocated = 0;
	bus->bandwidth_int_reqs  = 0;
	bus->bandwidth_isoc_reqs = 0;
	mutex_init(&bus->devnum_next_mutex);
}

/*-------------------------------------------------------------------------*/

/**
 * usb_register_bus - registers the USB host controller with the usb core
 * @bus: pointer to the bus to register
 *
 * Context: task context, might sleep.
 *
 * Assigns a bus number, and links the controller into usbcore data
 * structures so that it can be seen by scanning the bus list.
 *
 * Return: 0 if successful. A negative error code otherwise.
 */
/*
 * usb_register_bus -- 注册 USB 总线到 USB 核心
 *
 * 通过 IDR (Integer ID Allocation) 分配器从 1~USB_MAXBUS-1 范围内
 * 分配一个唯一的总线编号。随后通过 usb_notify_add_bus() 通知
 * 系统其他部分 (如 usbfs) 有新总线加入.
 *
 * 注意: usb_bus_idr_lock 是 mutex, 因此此函数可以睡眠。
 * 总线编号 0 是保留的, idr_alloc 从 1 开始.
 */
static int usb_register_bus(struct usb_bus *bus)
{
	int result = -E2BIG;
	int busnum;

	mutex_lock(&usb_bus_idr_lock);
	busnum = idr_alloc(&usb_bus_idr, bus, 1, USB_MAXBUS, GFP_KERNEL);
	if (busnum < 0) {
		pr_err("%s: failed to get bus number\n", usbcore_name);
		goto error_find_busnum;
	}
	bus->busnum = busnum;
	mutex_unlock(&usb_bus_idr_lock);

	usb_notify_add_bus(bus);

	dev_info (bus->controller, "new USB bus registered, assigned bus "
		  "number %d\n", bus->busnum);
	return 0;

error_find_busnum:
	mutex_unlock(&usb_bus_idr_lock);
	return result;
}

/**
 * usb_deregister_bus - deregisters the USB host controller
 * @bus: pointer to the bus to deregister
 *
 * Context: task context, might sleep.
 *
 * Recycles the bus number, and unlinks the controller from usbcore data
 * structures so that it won't be seen by scanning the bus list.
 */
/*
 * usb_deregister_bus -- 注销 USB 总线
 *
 * 将总线编号归还给 IDR 池, 并通知系统其他部分 (通过 usb_notify_remove_bus).
 * 注意: 在调用此函数之前, 调用者必须确保该总线上所有设备已被移除.
 */
static void usb_deregister_bus (struct usb_bus *bus)
{
	dev_info (bus->controller, "USB bus %d deregistered\n", bus->busnum);

	/*
	 * NOTE: make sure that all the devices are removed by the
	 * controller code, as well as having it call this when cleaning
	 * itself up
	 */
	mutex_lock(&usb_bus_idr_lock);
	idr_remove(&usb_bus_idr, bus->busnum);
	mutex_unlock(&usb_bus_idr_lock);

	usb_notify_remove_bus(bus);
}

/*
 * register_root_hub -- 注册根 HUB 到 USB 子系统
 *
 * 在 usb_add_hcd() 的最后阶段调用, 完成根 HUB 的注册:
 *
 * 1. 分配根 HUB 的 USB 设备地址为 1 (固定地址)
 * 2. 在 devmap 中标记地址 1 已使用
 * 3. 设置设备状态为 USB_STATE_ADDRESS
 * 4. 读取设备描述符 (从硬编码的描述符中获取)
 * 5. 如果 bcdUSB >= 0x0201, 读取 BOS 描述符以获取 LPM 能力
 * 6. 调用 usb_new_device() 将根 HUB 注册为新 USB 设备
 * 7. 如果注册后 HC 已死亡 (HCD_DEAD), 调用 usb_hc_died() 清理
 *
 * 注意: 根 HUB 的设备地址始终为 1 (由 USB 规范隐含要求,
 * 因为根 HUB 在枚举过程中不需要地址分配).
 *
 * 锁: 需要 usb_bus_idr_lock (mutex) 保护设备枚举过程.
 */
/**
 * register_root_hub - called by usb_add_hcd() to register a root hub
 * @hcd: host controller for this root hub
 *
 * This function registers the root hub with the USB subsystem.  It sets up
 * the device properly in the device tree and then calls usb_new_device()
 * to register the usb device.  It also assigns the root hub's USB address
 * (always 1).
 *
 * Return: 0 if successful. A negative error code otherwise.
 */
static int register_root_hub(struct usb_hcd *hcd)
{
	struct device *parent_dev = hcd->self.controller;
	struct usb_device *usb_dev = hcd->self.root_hub;
	struct usb_device_descriptor *descr;
	const int devnum = 1;
	int retval;

	usb_dev->devnum = devnum;
	usb_dev->bus->devnum_next = devnum + 1;
	set_bit(devnum, usb_dev->bus->devmap);
	usb_set_device_state(usb_dev, USB_STATE_ADDRESS);

	mutex_lock(&usb_bus_idr_lock);

	usb_dev->ep0.desc.wMaxPacketSize = cpu_to_le16(64);
	descr = usb_get_device_descriptor(usb_dev);
	if (IS_ERR(descr)) {
		retval = PTR_ERR(descr);
		mutex_unlock(&usb_bus_idr_lock);
		dev_dbg (parent_dev, "can't read %s device descriptor %d\n",
				dev_name(&usb_dev->dev), retval);
		return retval;
	}
	usb_dev->descriptor = *descr;
	kfree(descr);

	if (le16_to_cpu(usb_dev->descriptor.bcdUSB) >= 0x0201) {
		retval = usb_get_bos_descriptor(usb_dev);
		if (!retval) {
			usb_dev->lpm_capable = usb_device_supports_lpm(usb_dev);
		} else if (usb_dev->speed >= USB_SPEED_SUPER) {
			mutex_unlock(&usb_bus_idr_lock);
			dev_dbg(parent_dev, "can't read %s bos descriptor %d\n",
					dev_name(&usb_dev->dev), retval);
			return retval;
		}
	}

	retval = usb_new_device (usb_dev);
	if (retval) {
		dev_err (parent_dev, "can't register root hub for %s, %d\n",
				dev_name(&usb_dev->dev), retval);
	} else {
		spin_lock_irq (&hcd_root_hub_lock);
		hcd->rh_registered = 1;
		spin_unlock_irq (&hcd_root_hub_lock);

		/* Did the HC die before the root hub was registered? */
		if (HCD_DEAD(hcd))
			usb_hc_died (hcd);	/* This time clean up */
	}
	mutex_unlock(&usb_bus_idr_lock);

	return retval;
}

/*
 * usb_hcd_start_port_resume - a root-hub port is sending a resume signal
 * @bus: the bus which the root hub belongs to
 * @portnum: the port which is being resumed
 *
 * HCDs should call this function when they know that a resume signal is
 * being sent to a root-hub port.  The root hub will be prevented from
 * going into autosuspend until usb_hcd_end_port_resume() is called.
 *
 * The bus's private lock must be held by the caller.
 */
void usb_hcd_start_port_resume(struct usb_bus *bus, int portnum)
{
	unsigned bit = 1 << portnum;

	if (!(bus->resuming_ports & bit)) {
		bus->resuming_ports |= bit;
		pm_runtime_get_noresume(&bus->root_hub->dev);
	}
}
EXPORT_SYMBOL_GPL(usb_hcd_start_port_resume);

/*
 * usb_hcd_end_port_resume - a root-hub port has stopped sending a resume signal
 * @bus: the bus which the root hub belongs to
 * @portnum: the port which is being resumed
 *
 * HCDs should call this function when they know that a resume signal has
 * stopped being sent to a root-hub port.  The root hub will be allowed to
 * autosuspend again.
 *
 * The bus's private lock must be held by the caller.
 */
void usb_hcd_end_port_resume(struct usb_bus *bus, int portnum)
{
	unsigned bit = 1 << portnum;

	if (bus->resuming_ports & bit) {
		bus->resuming_ports &= ~bit;
		pm_runtime_put_noidle(&bus->root_hub->dev);
	}
}
EXPORT_SYMBOL_GPL(usb_hcd_end_port_resume);

/*-------------------------------------------------------------------------*/

/*
 * usb_calc_bus_time -- 计算周期传输的近似总线时间 (纳秒)
 *
 * 此函数估算在 USB 总线上完成一个周期传输 (等时或中断) 所需的时间。
 * 结果用于带宽管理: 在配置设备时检查总线上的总带宽是否超出限制
 * (USB 1.x/2.0 的一个帧/微帧时间分别为 1ms/125us).
 *
 * 计算依据 USB 2.0 规范第 5.11.3 节, 包含以下开销:
 *   - SYNC 和 EOP (包结束符)
 *   - 包标识符 (PID)
 *   - 数据负载 (每字节的位时间)
 *   - CRC 校验
 *   - HUB 延迟 (针对低速传输的 BW_HUB_LS_SETUP)
 *   - 主机延迟 (BW_HOST_DELAY)
 *
 * 对于不同的速度:
 *   - LOW 速度: 仅用于中断传输, 包含 2 个 BW_HUB_LS_SETUP
 *   - FULL 速度: 等时和中断使用不同的时间基数
 *   - HIGH 速度: 使用 HS_NSECS() / HS_NSECS_ISO() 宏计算
 *
 * 注意: SuperSpeed (USB 3.x) 不需要此函数, 因为其带宽调度
 * 由硬件管理.
 *
 * 参数:
 *   @speed: USB_SPEED_{LOW|FULL|HIGH}
 *   @is_input: true=设备到主机 (IN), false=主机到设备 (OUT)
 *   @isoc: true=等时传输, false=中断传输
 *   @bytecount: 传输的字节数
 *
 * 返回: 纳秒数, 或 -1 (无效速度).
 */
/**
 * usb_calc_bus_time - approximate periodic transaction time in nanoseconds
 * @speed: from dev->speed; USB_SPEED_{LOW,FULL,HIGH}
 * @is_input: true iff the transaction sends data to the host
 * @isoc: true for isochronous transactions, false for interrupt ones
 * @bytecount: how many bytes in the transaction.
 *
 * Return: Approximate bus time in nanoseconds for a periodic transaction.
 *
 * Note:
 * See USB 2.0 spec section 5.11.3; only periodic transfers need to be
 * scheduled in software, this function is only used for such scheduling.
 */
long usb_calc_bus_time (int speed, int is_input, int isoc, int bytecount)
{
	unsigned long	tmp;

	switch (speed) {
	case USB_SPEED_LOW: 	/* INTR only */
		if (is_input) {
			tmp = (67667L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return 64060L + (2 * BW_HUB_LS_SETUP) + BW_HOST_DELAY + tmp;
		} else {
			tmp = (66700L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return 64107L + (2 * BW_HUB_LS_SETUP) + BW_HOST_DELAY + tmp;
		}
	case USB_SPEED_FULL:	/* ISOC or INTR */
		if (isoc) {
			tmp = (8354L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return ((is_input) ? 7268L : 6265L) + BW_HOST_DELAY + tmp;
		} else {
			tmp = (8354L * (31L + 10L * BitTime (bytecount))) / 1000L;
			return 9107L + BW_HOST_DELAY + tmp;
		}
	case USB_SPEED_HIGH:	/* ISOC or INTR */
		/* FIXME adjust for input vs output */
		if (isoc)
			tmp = HS_NSECS_ISO (bytecount);
		else
			tmp = HS_NSECS (bytecount);
		return tmp;
	default:
		pr_debug ("%s: bogus device speed!\n", usbcore_name);
		return -1;
	}
}
EXPORT_SYMBOL_GPL(usb_calc_bus_time);


/*-------------------------------------------------------------------------*/

/*
 * Generic HC operations.
 */

/*-------------------------------------------------------------------------*/

/*
 * usb_hcd_link_urb_to_ep -- 将 URB 添加到端点队列
 *
 * 这是 URB 提交过程中的关键步骤, 在 HC 驱动的 urb_enqueue() 中调用。
 * 将 URB 加入端点的 urb_list 链表尾部, 同时进行四项安全检查:
 *
 *   1. urb->reject: 如果设置了 reject 标志 (由 usb_kill_urb / usb_poison_urb 设置),
 *      说明此 URB 正在被终止, 拒绝新提交 → -EPERM
 *   2. ep->enabled: 如果端点已禁用 (如配置更改时), 拒绝 → -ENOENT
 *   3. dev->can_submit: 如果设备不允许提交 (已断开), 拒绝 → -EHOSTUNREACH
 *   4. HCD_RH_RUNNING: 如果 HC 未运行 (已挂起或死亡), 拒绝 → -ESHUTDOWN
 *
 * 只有通过所有检查后, URB 才被加入端点队列, urb->unlinked 被清零表示正常运行.
 *
 * 锁: 调用者必须持有 HC 驱动的私有 spinlock, 且中断已禁用。
 * 此函数内部还会获取 hcd_urb_list_lock.
 *
 * 注意: 如果 HC 驱动的 urb_enqueue() 在调用此函数后失败,
 * 必须调用 usb_hcd_unlink_urb_from_ep() 回滚.
 */
/**
 * usb_hcd_link_urb_to_ep - add an URB to its endpoint queue
 * @hcd: host controller to which @urb was submitted
 * @urb: URB being submitted
 *
 * Host controller drivers should call this routine in their enqueue()
 * method.  The HCD's private spinlock must be held and interrupts must
 * be disabled.  The actions carried out here are required for URB
 * submission, as well as for endpoint shutdown and for usb_kill_urb.
 *
 * Return: 0 for no error, otherwise a negative error code (in which case
 * the enqueue() method must fail).  If no error occurs but enqueue() fails
 * anyway, it must call usb_hcd_unlink_urb_from_ep() before releasing
 * the private spinlock and returning.
 */
int usb_hcd_link_urb_to_ep(struct usb_hcd *hcd, struct urb *urb)
{
	int		rc = 0;

	spin_lock(&hcd_urb_list_lock);

	/* Check that the URB isn't being killed */
	if (unlikely(atomic_read(&urb->reject))) {
		rc = -EPERM;
		goto done;
	}

	if (unlikely(!urb->ep->enabled)) {
		rc = -ENOENT;
		goto done;
	}

	if (unlikely(!urb->dev->can_submit)) {
		rc = -EHOSTUNREACH;
		goto done;
	}

	/*
	 * Check the host controller's state and add the URB to the
	 * endpoint's queue.
	 */
	if (HCD_RH_RUNNING(hcd)) {
		urb->unlinked = 0;
		list_add_tail(&urb->urb_list, &urb->ep->urb_list);
	} else {
		rc = -ESHUTDOWN;
		goto done;
	}
 done:
	spin_unlock(&hcd_urb_list_lock);
	return rc;
}
EXPORT_SYMBOL_GPL(usb_hcd_link_urb_to_ep);

/*
 * usb_hcd_check_unlink_urb -- 检查 URB 是否可以被取消链接
 *
 * 在 HC 驱动的 dequeue() 方法中调用, 验证取消操作是否合法:
 *   1. URB 仍然在端点队列中 (未被完成或之前已移除) → 否则返回 -EIDRM
 *   2. URB 尚未被取消 (urb->unlinked == 0) → 否则返回 -EBUSY
 *
 * 通过检查后, 将 urb->unlinked 设置为取消原因 (status).
 * 实际的硬件取消操作由 HC 驱动完成.
 *
 * 锁: 调用者必须持有 HC 驱动的私有 spinlock, 且中断已禁用.
 */
/**
 * usb_hcd_check_unlink_urb - check whether an URB may be unlinked
 * @hcd: host controller to which @urb was submitted
 * @urb: URB being checked for unlinkability
 * @status: error code to store in @urb if the unlink succeeds
 *
 * Host controller drivers should call this routine in their dequeue()
 * method.  The HCD's private spinlock must be held and interrupts must
 * be disabled.  The actions carried out here are required for making
 * sure than an unlink is valid.
 *
 * Return: 0 for no error, otherwise a negative error code (in which case
 * the dequeue() method must fail).  The possible error codes are:
 *
 *	-EIDRM: @urb was not submitted or has already completed.
 *		The completion function may not have been called yet.
 *
 *	-EBUSY: @urb has already been unlinked.
 */
int usb_hcd_check_unlink_urb(struct usb_hcd *hcd, struct urb *urb,
		int status)
{
	struct list_head	*tmp;

	/* insist the urb is still queued */
	list_for_each(tmp, &urb->ep->urb_list) {
		if (tmp == &urb->urb_list)
			break;
	}
	if (tmp != &urb->urb_list)
		return -EIDRM;

	/* Any status except -EINPROGRESS means something already started to
	 * unlink this URB from the hardware.  So there's no more work to do.
	 */
	if (urb->unlinked)
		return -EBUSY;
	urb->unlinked = status;
	return 0;
}
EXPORT_SYMBOL_GPL(usb_hcd_check_unlink_urb);

/*
 * usb_hcd_unlink_urb_from_ep -- 从端点队列中移除 URB
 *
 * 在 URB 完成或取消时调用, 将 URB 从它所属的端点队列中移除。
 * 在调用 usb_hcd_giveback_urb() 之前必须执行此操作,
 * 以确保 URB 不再被 HC 驱动引用.
 *
 * 注意: 此操作在 hcd_urb_list_lock 保护下进行, 与
 * usb_hcd_link_urb_to_ep() 使用相同的锁进行序列化.
 */
/**
 * usb_hcd_unlink_urb_from_ep - remove an URB from its endpoint queue
 * @hcd: host controller to which @urb was submitted
 * @urb: URB being unlinked
 *
 * Host controller drivers should call this routine before calling
 * usb_hcd_giveback_urb().  The HCD's private spinlock must be held and
 * interrupts must be disabled.  The actions carried out here are required
 * for URB completion.
 */
void usb_hcd_unlink_urb_from_ep(struct usb_hcd *hcd, struct urb *urb)
{
	/* clear all state linking urb to this dev (and hcd) */
	spin_lock(&hcd_urb_list_lock);
	list_del_init(&urb->urb_list);
	spin_unlock(&hcd_urb_list_lock);
}
EXPORT_SYMBOL_GPL(usb_hcd_unlink_urb_from_ep);

/*
 * DMA bounce buffer 支持 -- 为 DMA 能力受限的主机控制器提供支持
 *
 * 某些 USB 主机控制器 (如嵌入式平台的片上控制器) 只能访问有限的
 * SRAM 区域, 或对可寻址的 DRAM 有特殊限制, 无法像 PCI 设备那样
 * 直接对系统内存进行 DMA 操作.
 *
 * 解决方案: DMA bounce buffer
 *   1. 通过 usb_hcd_setup_local_mem() 初始化 hcd->localmem_pool
 *   2. localmem_pool 基于 genalloc API 从预分配的本地内存池中分配缓冲区
 *   3. 在 map_urb_for_dma() 中, 如果检测到 hcd->localmem_pool,
 *      使用 hcd_alloc_coherent() 从本地内存池分配 DMA 缓冲区
 *   4. 数据方向为 TO_DEVICE 时, 将原始数据拷贝到本地缓冲区;
 *      数据方向为 FROM_DEVICE 时, 在 unmap 时将本地缓冲区数据拷贝回原始缓冲区
 *
 * hcd_alloc_coherent() 的存储布局:
 *   [本地 DMA 缓冲区: size 字节] [原始虚拟地址指针: sizeof(ulong) 字节]
 *   原始虚拟地址存储在 DMA 缓冲区的末尾, 用于在释放时恢复.
 *
 * 注意: 本地内存的主要需求是"可被设备寻址 (local)"而非"一致性 (coherent)".
 *
 */

static int hcd_alloc_coherent(struct usb_bus *bus,
			      gfp_t mem_flags, dma_addr_t *dma_handle,
			      void **vaddr_handle, size_t size,
			      enum dma_data_direction dir)
{
	unsigned char *vaddr;

	if (*vaddr_handle == NULL) {
		WARN_ON_ONCE(1);
		return -EFAULT;
	}

	vaddr = hcd_buffer_alloc(bus, size + sizeof(unsigned long),
				 mem_flags, dma_handle);
	if (!vaddr)
		return -ENOMEM;

	/*
	 * Store the virtual address of the buffer at the end
	 * of the allocated dma buffer. The size of the buffer
	 * may be uneven so use unaligned functions instead
	 * of just rounding up. It makes sense to optimize for
	 * memory footprint over access speed since the amount
	 * of memory available for dma may be limited.
	 */
	put_unaligned((unsigned long)*vaddr_handle,
		      (unsigned long *)(vaddr + size));

	if (dir == DMA_TO_DEVICE)
		memcpy(vaddr, *vaddr_handle, size);

	*vaddr_handle = vaddr;
	return 0;
}

static void hcd_free_coherent(struct usb_bus *bus, dma_addr_t *dma_handle,
			      void **vaddr_handle, size_t size,
			      enum dma_data_direction dir)
{
	unsigned char *vaddr = *vaddr_handle;

	vaddr = (void *)get_unaligned((unsigned long *)(vaddr + size));

	if (dir == DMA_FROM_DEVICE)
		memcpy(vaddr, *vaddr_handle, size);

	hcd_buffer_free(bus, size + sizeof(vaddr), *vaddr_handle, *dma_handle);

	*vaddr_handle = vaddr;
	*dma_handle = 0;
}

void usb_hcd_unmap_urb_setup_for_dma(struct usb_hcd *hcd, struct urb *urb)
{
	if (IS_ENABLED(CONFIG_HAS_DMA) &&
	    (urb->transfer_flags & URB_SETUP_MAP_SINGLE))
		dma_unmap_single(hcd->self.sysdev,
				urb->setup_dma,
				sizeof(struct usb_ctrlrequest),
				DMA_TO_DEVICE);
	else if (urb->transfer_flags & URB_SETUP_MAP_LOCAL)
		hcd_free_coherent(urb->dev->bus,
				&urb->setup_dma,
				(void **) &urb->setup_packet,
				sizeof(struct usb_ctrlrequest),
				DMA_TO_DEVICE);

	/* Make it safe to call this routine more than once */
	urb->transfer_flags &= ~(URB_SETUP_MAP_SINGLE | URB_SETUP_MAP_LOCAL);
}
EXPORT_SYMBOL_GPL(usb_hcd_unmap_urb_setup_for_dma);

/*
 * unmap_urb_for_dma -- URB DMA 解映射 (包装函数)
 *
 * 首选调用 HC 驱动自定义的 unmap_urb_for_dma (如果存在),
 * 否则使用通用的 usb_hcd_unmap_urb_for_dma().
 *
 * 在 URB 完成后调用, 撤销 map_urb_for_dma 所做的所有 DMA 映射.
 */
static void unmap_urb_for_dma(struct usb_hcd *hcd, struct urb *urb)
{
	if (hcd->driver->unmap_urb_for_dma)
		hcd->driver->unmap_urb_for_dma(hcd, urb);
	else
		usb_hcd_unmap_urb_for_dma(hcd, urb);
}

/*
 * usb_hcd_unmap_urb_for_dma -- URB DMA 解映射 (通用实现)
 *
 * 根据 URB 建立映射时设置的 flags, 选择对应的解映射方式:
 *
 *   1. URB_SETUP_MAP_SINGLE / URB_SETUP_MAP_LOCAL: 先解映射 setup_packet
 *   2. URB_DMA_MAP_SG:    解映射散聚列表 (dma_unmap_sg)
 *   3. URB_DMA_MAP_PAGE:  解映射单页 (dma_unmap_page)
 *   4. URB_DMA_MAP_SINGLE:解映射单缓冲区 (dma_unmap_single)
 *   5. URB_MAP_LOCAL:     释放本地 bounce buffer (hcd_free_coherent)
 *   6. URB_NO_TRANSFER_DMA_MAP + sgt: DMA 同步操作
 *
 * 最后清除所有的 DMA 映射标志, 确保重复调用安全.
 */
void usb_hcd_unmap_urb_for_dma(struct usb_hcd *hcd, struct urb *urb)
{
	enum dma_data_direction dir;

	usb_hcd_unmap_urb_setup_for_dma(hcd, urb);

	dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	if (IS_ENABLED(CONFIG_HAS_DMA) &&
	    (urb->transfer_flags & URB_DMA_MAP_SG)) {
		dma_unmap_sg(hcd->self.sysdev,
				urb->sg,
				urb->num_sgs,
				dir);
	} else if (IS_ENABLED(CONFIG_HAS_DMA) &&
		 (urb->transfer_flags & URB_DMA_MAP_PAGE)) {
		dma_unmap_page(hcd->self.sysdev,
				urb->transfer_dma,
				urb->transfer_buffer_length,
				dir);
	} else if (IS_ENABLED(CONFIG_HAS_DMA) &&
		 (urb->transfer_flags & URB_DMA_MAP_SINGLE)) {
		dma_unmap_single(hcd->self.sysdev,
				urb->transfer_dma,
				urb->transfer_buffer_length,
				dir);
	} else if (urb->transfer_flags & URB_MAP_LOCAL) {
		hcd_free_coherent(urb->dev->bus,
				&urb->transfer_dma,
				&urb->transfer_buffer,
				urb->transfer_buffer_length,
				dir);
	} else if ((urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP) && urb->sgt) {
		dma_sync_sgtable_for_cpu(hcd->self.sysdev, urb->sgt, dir);
		if (dir == DMA_FROM_DEVICE)
			invalidate_kernel_vmap_range(urb->transfer_buffer,
						     urb->transfer_buffer_length);
	}

	/* Make it safe to call this routine more than once */
	urb->transfer_flags &= ~(URB_DMA_MAP_SG | URB_DMA_MAP_PAGE |
			URB_DMA_MAP_SINGLE | URB_MAP_LOCAL);
}
EXPORT_SYMBOL_GPL(usb_hcd_unmap_urb_for_dma);

/*
 * map_urb_for_dma -- URB DMA 映射 (包装函数)
 *
 * 为 URB 的传输缓冲区建立 DMA 映射, 以便主机控制器硬件可以直接访问。
 * 首选调用 HC 驱动自定义的 map_urb_for_dma (如果存在),
 * 否则使用通用的 usb_hcd_map_urb_for_dma().
 *
 * 映射的内容:
 *   1. setup_packet (控制传输的设置包) → DMA_TO_DEVICE
 *   2. transfer_buffer (实际数据传输缓冲区) → 方向取决于 URB 类型
 *
 * 根据硬件能力, 使用不同的映射策略:
 *   - 普通 DMA: dma_map_single / dma_map_page / dma_map_sg
 *   - 本地内存池 (localmem): hcd_alloc_coherent (bounce buffer)
 *   - PIO 模式: HC 驱动自行处理, 不设置 DMA 映射
 */
static int map_urb_for_dma(struct usb_hcd *hcd, struct urb *urb,
			   gfp_t mem_flags)
{
	if (hcd->driver->map_urb_for_dma)
		return hcd->driver->map_urb_for_dma(hcd, urb, mem_flags);
	else
		return usb_hcd_map_urb_for_dma(hcd, urb, mem_flags);
}

/*
 * usb_hcd_map_urb_for_dma -- URB DMA 映射 (通用实现)
 *
 * 为 URB 建立两种 DMA 映射:
 *
 * 1. setup_packet 映射 (仅控制传输):
 *    - 如果 hcd->self.uses_pio_for_control, 跳过 (PIO 模式)
 *    - 如果 hcd->localmem_pool 存在, 从本地内存池分配 bounce buffer
 *    - 否则使用 dma_map_single 映射 (如果 HC 使用 DMA)
 *
 * 2. transfer_buffer 映射:
 *    根据 URB 的标志选择映射方式, 优先级从高到低:
 *    a) URB_NO_TRANSFER_DMA_MAP:  不映射, 仅同步 sgt (如果存在)
 *    b) 本地内存池 (localmem):     hcd_alloc_coherent
 *    c) scatter-gather 列表 (num_sgs > 0): dma_map_sg
 *    d) 单页 (sg 非空):            dma_map_page
 *    e) 单缓冲区:                   dma_map_single
 *
 * 映射失败时: 清除已建立的所有映射, 返回 -EAGAIN.
 *
 * 注意: 不支持等时端点的 SG 列表 (WARN_ON).
 */
int usb_hcd_map_urb_for_dma(struct usb_hcd *hcd, struct urb *urb,
			    gfp_t mem_flags)
{
	enum dma_data_direction dir;
	int ret = 0;

	/* Map the URB's buffers for DMA access.
	 * Lower level HCD code should use *_dma exclusively,
	 * unless it uses pio or talks to another transport,
	 * or uses the provided scatter gather list for bulk.
	 */

	if (usb_endpoint_xfer_control(&urb->ep->desc)) {
		if (hcd->self.uses_pio_for_control)
			return ret;
		if (hcd->localmem_pool) {
			ret = hcd_alloc_coherent(
					urb->dev->bus, mem_flags,
					&urb->setup_dma,
					(void **)&urb->setup_packet,
					sizeof(struct usb_ctrlrequest),
					DMA_TO_DEVICE);
			if (ret)
				return ret;
			urb->transfer_flags |= URB_SETUP_MAP_LOCAL;
		} else if (hcd_uses_dma(hcd)) {
			if (object_is_on_stack(urb->setup_packet)) {
				WARN_ONCE(1, "setup packet is on stack\n");
				return -EAGAIN;
			}

			urb->setup_dma = dma_map_single(
					hcd->self.sysdev,
					urb->setup_packet,
					sizeof(struct usb_ctrlrequest),
					DMA_TO_DEVICE);
			if (dma_mapping_error(hcd->self.sysdev,
						urb->setup_dma))
				return -EAGAIN;
			urb->transfer_flags |= URB_SETUP_MAP_SINGLE;
		}
	}

	dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	if (urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP) {
		if (!urb->sgt)
			return 0;

		if (dir == DMA_TO_DEVICE)
			flush_kernel_vmap_range(urb->transfer_buffer,
						urb->transfer_buffer_length);
		dma_sync_sgtable_for_device(hcd->self.sysdev, urb->sgt, dir);
	} else if (urb->transfer_buffer_length != 0) {
		if (hcd->localmem_pool) {
			ret = hcd_alloc_coherent(
					urb->dev->bus, mem_flags,
					&urb->transfer_dma,
					&urb->transfer_buffer,
					urb->transfer_buffer_length,
					dir);
			if (ret == 0)
				urb->transfer_flags |= URB_MAP_LOCAL;
		} else if (hcd_uses_dma(hcd)) {
			if (urb->num_sgs) {
				int n;

				/* We don't support sg for isoc transfers ! */
				if (usb_endpoint_xfer_isoc(&urb->ep->desc)) {
					WARN_ON(1);
					return -EINVAL;
				}

				n = dma_map_sg(
						hcd->self.sysdev,
						urb->sg,
						urb->num_sgs,
						dir);
				if (!n)
					ret = -EAGAIN;
				else
					urb->transfer_flags |= URB_DMA_MAP_SG;
				urb->num_mapped_sgs = n;
				if (n != urb->num_sgs)
					urb->transfer_flags |=
							URB_DMA_SG_COMBINED;
			} else if (urb->sg) {
				struct scatterlist *sg = urb->sg;
				urb->transfer_dma = dma_map_page(
						hcd->self.sysdev,
						sg_page(sg),
						sg->offset,
						urb->transfer_buffer_length,
						dir);
				if (dma_mapping_error(hcd->self.sysdev,
						urb->transfer_dma))
					ret = -EAGAIN;
				else
					urb->transfer_flags |= URB_DMA_MAP_PAGE;
			} else if (object_is_on_stack(urb->transfer_buffer)) {
				WARN_ONCE(1, "transfer buffer is on stack\n");
				ret = -EAGAIN;
			} else {
				urb->transfer_dma = dma_map_single(
						hcd->self.sysdev,
						urb->transfer_buffer,
						urb->transfer_buffer_length,
						dir);
				if (dma_mapping_error(hcd->self.sysdev,
						urb->transfer_dma))
					ret = -EAGAIN;
				else
					urb->transfer_flags |= URB_DMA_MAP_SINGLE;
			}
		}
		if (ret && (urb->transfer_flags & (URB_SETUP_MAP_SINGLE |
				URB_SETUP_MAP_LOCAL)))
			usb_hcd_unmap_urb_for_dma(hcd, urb);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(usb_hcd_map_urb_for_dma);

/*-------------------------------------------------------------------------*/

/*
 * usb_hcd_submit_urb -- 将 URB 提交给主机控制器 (核心提交路径)
 *
 * 这是 URB 从 USB 核心进入 HCD 层的关键入口函数, 由 usb_submit_urb() 调用。
 * 调用者已通过 usb_submit_urb() 完成了所有合法性检查并填充了 URB 字段.
 *
 * ============================== 执行流程 ==============================
 *
 * 1. 引用计数保护:
 *    usb_get_urb(urb)         -- 增加 URB 的引用计数 (防止 URB 被释放)
 *    atomic_inc(&urb->use_count) -- use_count 表示 URB 正在被硬件使用
 *    atomic_inc(&urb->dev->urbnum) -- 统计设备上的活跃 URB 数量
 *    usbmon_urb_submit()      -- 通知 usbmon 监控系统 (用于调试/抓包)
 *
 * 2. 路径分叉: 根 HUB vs 硬件设备
 *
 *    ┌─ 是根 HUB? → rh_urb_enqueue()
 *    │   ├─ 控制端点 → rh_call_control()      [纯软件同步处理]
 *    │   └─ 中断端点 → rh_queue_status()       [注册状态轮询 URB]
 *    │
 *    └─ 硬件设备 → map_urb_for_dma()           [建立 DMA 映射]
 *                    └→ hcd->driver->urb_enqueue() [提交到真实硬件]
 *
 *    根 HUB 的特殊处理: 根 HUB 的 URB 不需要 DMA 映射和真实总线传输,
 *    而是完全在软件中模拟 (见 rh_call_control / rh_queue_status 的注释).
 *
 * 3. 错误路径 (status != 0):
 *    a) usbmon_urb_submit_error()  -- 通知监控系统提交失败
 *    b) urb->hcpriv = NULL         -- 清除 HC 驱动私有指针
 *    c) INIT_LIST_HEAD(&urb->urb_list) -- 确保链表头初始化
 *    d) atomic_dec(&urb->use_count) -- 回退 use_count 增量
 *    e) smp_mb__after_atomic()     -- 内存屏障: 保证 use_count 的写入
 *                                    在读取 reject 前全局可见
 *    f) atomic_dec(&urb->dev->urbnum)
 *    g) 如果 urb->reject 被设置 (kill/poison 正在等待), 唤醒
 *       usb_kill_urb_queue 以通知等待者
 *    h) usb_put_urb(urb)          -- 释放引用计数
 *
 * ============================== 内存屏障说明 ==============================
 *
 *    smp_mb__after_atomic() 后紧接 atomic_read(&urb->reject):
 *    - usb_kill_urb()/usb_poison_urb() 在设置 reject 后使用 smp_mb__after_atomic()
 *      并检查 use_count 是否为 0
 *    - 这里在递减 use_count 后使用 smp_mb__after_atomic() 再检查 reject
 *    - 两者通过内存屏障保证: 要么 kill 看到 use_count 为 0, 要么这里看到
 *      reject 被设置, 不会出现双方都错过的情况
 *
 * ============================== 调用约束 ==============================
 *
 *    此函数可以在任何上下文调用 (进程上下文 / 软中断 / 硬中断),
 *    但调用者必须保证 urb->dev 的引用计数有效.
 *    调用者交出 URB 的"所有权": HCD 保证要么返回错误,
 *    要么最终调用 usb_hcd_giveback_urb() (两者不会同时发生).
 *
 * 参数:
 *   @urb:      需要提交的 URB
 *   @mem_flags: 内存分配标志 (GFP_*)
 *
 * 返回: 0 表示成功, 负值表示错误码.
 *       成功不代表传输已完成, 只是表示 URB 已入队等待硬件处理.
 */
/* may be called in any context with a valid urb->dev usecount
 * caller surrenders "ownership" of urb
 * expects usb_submit_urb() to have sanity checked and conditioned all
 * inputs in the urb
 */
int usb_hcd_submit_urb (struct urb *urb, gfp_t mem_flags)
{
	int			status;
	struct usb_hcd		*hcd = bus_to_hcd(urb->dev->bus);

	/* increment urb's reference count as part of giving it to the HCD
	 * (which will control it).  HCD guarantees that it either returns
	 * an error or calls giveback(), but not both.
	 */
	usb_get_urb(urb);
	atomic_inc(&urb->use_count);
	atomic_inc(&urb->dev->urbnum);
	usbmon_urb_submit(&hcd->self, urb);

	/* NOTE requirements on root-hub callers (usbfs and the hub
	 * driver, for now):  URBs' urb->transfer_buffer must be
	 * valid and usb_buffer_{sync,unmap}() not be needed, since
	 * they could clobber root hub response data.  Also, control
	 * URBs must be submitted in process context with interrupts
	 * enabled.
	 */

	if (is_root_hub(urb->dev)) {
		status = rh_urb_enqueue(hcd, urb);
	} else {
		status = map_urb_for_dma(hcd, urb, mem_flags);
		if (likely(status == 0)) {
			status = hcd->driver->urb_enqueue(hcd, urb, mem_flags);
			if (unlikely(status))
				unmap_urb_for_dma(hcd, urb);
		}
	}

	if (unlikely(status)) {
		usbmon_urb_submit_error(&hcd->self, urb, status);
		urb->hcpriv = NULL;
		INIT_LIST_HEAD(&urb->urb_list);
		atomic_dec(&urb->use_count);
		/*
		 * Order the write of urb->use_count above before the read
		 * of urb->reject below.  Pairs with the memory barriers in
		 * usb_kill_urb() and usb_poison_urb().
		 */
		smp_mb__after_atomic();

		atomic_dec(&urb->dev->urbnum);
		if (atomic_read(&urb->reject))
			wake_up(&usb_kill_urb_queue);
		usb_put_urb(urb);
	}
	return status;
}

/*-------------------------------------------------------------------------*/

/*
 * unlink1 -- 从 HC 队列中取消一个 URB
 *
 * 根据目标是否为根 HUB 分叉:
 *   - 根 HUB: 调用 usb_rh_urb_dequeue() (纯软件处理)
 *   - 硬件设备: 调用 hc_driver->urb_dequeue() (需要 HC 硬件操作)
 *
 * 注意: HC 驱动的 urb_dequeue() 可能在 URB 尚未完全入队时失败,
 * 这种失败是无害的, 因为 URB 还没有被硬件持有.
 *
 * 参数:
 *   @hcd:    主机控制器
 *   @urb:    要取消的 URB
 *   @status: 取消原因 (将传递给 urb->complete 回调)
 */
/* this makes the hcd giveback() the urb more quickly, by kicking it
 * off hardware queues (which may take a while) and returning it as
 * soon as practical.  we've already set up the urb's return status,
 * but we can't know if the callback completed already.
 */
static int unlink1(struct usb_hcd *hcd, struct urb *urb, int status)
{
	int		value;

	if (is_root_hub(urb->dev))
		value = usb_rh_urb_dequeue(hcd, urb, status);
	else {

		/* The only reason an HCD might fail this call is if
		 * it has not yet fully queued the urb to begin with.
		 * Such failures should be harmless. */
		value = hcd->driver->urb_dequeue(hcd, urb, status);
	}
	return value;
}

/*
 * usb_hcd_unlink_urb -- 从外部取消 URB (核心取消路径)
 *
 * 由 usb_unlink_urb() 和 usb_kill_urb() 调用。
 * 在 hcd_urb_unlink_lock 的保护下, 检查 URB 是否仍然活跃
 * (use_count > 0), 如果是则增加设备引用计数后执行取消操作.
 *
 * 返回值:
 *   0: URB 取消操作已启动 (但尚未完成, 返回 -EINPROGRESS 给调用者)
 *   -EINPROGRESS: 实际返回给调用者的"成功"值, 表示取消正在进行
 *   -EIDRM: URB 不在队列中 (可能已完成或从未提交)
 *   -EBUSY: URB 已经被取消
 *
 * 注意: 调用者必须保证在取消和完成回调都返回之前 URB 不会被回收.
 * 此函数可以在任何上下文中调用.
 */
/*
 * called in any context
 *
 * caller guarantees urb won't be recycled till both unlink()
 * and the urb's completion function return
 */
int usb_hcd_unlink_urb (struct urb *urb, int status)
{
	struct usb_hcd		*hcd;
	struct usb_device	*udev = urb->dev;
	int			retval = -EIDRM;
	unsigned long		flags;

	/* Prevent the device and bus from going away while
	 * the unlink is carried out.  If they are already gone
	 * then urb->use_count must be 0, since disconnected
	 * devices can't have any active URBs.
	 */
	spin_lock_irqsave(&hcd_urb_unlink_lock, flags);
	if (atomic_read(&urb->use_count) > 0) {
		retval = 0;
		usb_get_dev(udev);
	}
	spin_unlock_irqrestore(&hcd_urb_unlink_lock, flags);
	if (retval == 0) {
		hcd = bus_to_hcd(urb->dev->bus);
		retval = unlink1(hcd, urb, status);
		if (retval == 0)
			retval = -EINPROGRESS;
		else if (retval != -EIDRM && retval != -EBUSY)
			dev_dbg(&udev->dev, "hcd_unlink_urb %p fail %d\n",
					urb, retval);
		usb_put_dev(udev);
	}
	return retval;
}

/*-------------------------------------------------------------------------*/

/*
 * __usb_hcd_giveback_urb -- URB 完成的内部处理函数
 *
 * 当主机控制器完成 URB 的数据传输后, 调用此函数将 URB 返回到 USB 核心.
 * 这是 URB 生命周期的终点 (完成回调之前).
 *
 * ============================== 处理步骤 ==============================
 *
 * 1. 检查短传输:
 *    如果 URB_SHORT_NOT_OK 标志被设置且实际传输长度小于请求长度,
 *    将状态设置为 -EREMOTEIO (即使 HC 报告了成功).
 *
 * 2. 清理资源:
 *    unmap_urb_for_dma()  -- 撤销 DMA 映射
 *    usbmon_urb_complete() -- 通知 usbmon 监控系统
 *    usb_unanchor_urb()    -- 从 anchor 中移除 URB
 *
 * 3. 调用完成回调:
 *    urb->status = status  -- 设置最终状态
 *    urb->complete(urb)    -- 调用上层驱动注册的回调函数
 *    注意: kcov 仅在 softirq 上下文中收集覆盖率 (避免递归问题)
 *
 * 4. 清理引用计数:
 *    atomic_dec(&urb->use_count)  -- 标记 URB 不再被硬件使用
 *    smp_mb__after_atomic()       -- 内存屏障 (与 usb_kill_urb 配对)
 *    如果 urb->reject 被设置, 唤醒 usb_kill_urb_queue
 *    usb_put_urb(urb)             -- 释放引用计数
 *
 * ============================== 调用约束 ==============================
 *
 * 此函数可能在下述任一上下文调用:
 *   - 直接在 usb_hcd_giveback_urb() 中被调用 (非 BH 模式)
 *   - 通过 usb_giveback_urb_bh() 在 BH (软中断) 中被调用
 *
 * 注意: 调用者的 complete() 回调中可能会释放设备、重新提交 URB 等,
 * 因此调用此函数前必须释放所有 HC 驱动的锁.
 */
static void __usb_hcd_giveback_urb(struct urb *urb)
{
	struct usb_hcd *hcd = bus_to_hcd(urb->dev->bus);
	struct usb_anchor *anchor = urb->anchor;
	int status = urb->unlinked;

	urb->hcpriv = NULL;
	if (unlikely((urb->transfer_flags & URB_SHORT_NOT_OK) &&
	    urb->actual_length < urb->transfer_buffer_length &&
	    !status))
		status = -EREMOTEIO;

	unmap_urb_for_dma(hcd, urb);
	usbmon_urb_complete(&hcd->self, urb, status);
	usb_anchor_suspend_wakeups(anchor);
	usb_unanchor_urb(urb);
	if (likely(status == 0))
		usb_led_activity(USB_LED_EVENT_HOST);

	/* pass ownership to the completion handler */
	urb->status = status;
	/*
	 * This function can be called in task context inside another remote
	 * coverage collection section, but kcov doesn't support that kind of
	 * recursion yet. Only collect coverage in softirq context for now.
	 */
	kcov_remote_start_usb_softirq((u64)urb->dev->bus->busnum);
	urb->complete(urb);
	kcov_remote_stop_softirq();

	usb_anchor_resume_wakeups(anchor);
	atomic_dec(&urb->use_count);
	/*
	 * Order the write of urb->use_count above before the read
	 * of urb->reject below.  Pairs with the memory barriers in
	 * usb_kill_urb() and usb_poison_urb().
	 */
	smp_mb__after_atomic();

	if (unlikely(atomic_read(&urb->reject)))
		wake_up(&usb_kill_urb_queue);
	usb_put_urb(urb);
}

/*
 * usb_giveback_urb_bh -- URB 完成回调的 BH (bottom half) 处理函数
 *
 * 作为 workqueue 的处理函数运行, 将 URB 的 complete 回调从硬件中断
 * 上下文延迟到软中断上下文执行.
 *
 * 设计要点:
 *   1. 使用 local_list 将待处理的 URB 列表从 bh->head 中批量移出,
 *      减少锁竞争
 *   2. 设置 bh->running = true 表示 BH 正在运行中
 *   3. 记录 bh->completing_ep 用于调试 (跟踪当前完成的端点)
 *   4. 处理完一批后, 如果还有新的 URB 加入 (队列非空), 重新调度自己,
 *      避免单次处理时间过长 (类似于 tasklet 的防饿死机制)
 *
 * 注意: high_prio_bh 使用 system_bh_highpri_wq (高优先级工作队列),
 * 用于等时和中断传输 (需要较低的完成延迟);
 * low_prio_bh 使用 system_bh_wq (普通工作队列), 用于控制和批量传输.
 */
static void usb_giveback_urb_bh(struct work_struct *work)
{
	struct giveback_urb_bh *bh =
		container_of(work, struct giveback_urb_bh, bh);
	struct list_head local_list;

	spin_lock_irq(&bh->lock);
	bh->running = true;
	list_replace_init(&bh->head, &local_list);
	spin_unlock_irq(&bh->lock);

	while (!list_empty(&local_list)) {
		struct urb *urb;

		urb = list_entry(local_list.next, struct urb, urb_list);
		list_del_init(&urb->urb_list);
		bh->completing_ep = urb->ep;
		__usb_hcd_giveback_urb(urb);
		bh->completing_ep = NULL;
	}

	/*
	 * giveback new URBs next time to prevent this function
	 * from not exiting for a long time.
	 */
	spin_lock_irq(&bh->lock);
	if (!list_empty(&bh->head)) {
		if (bh->high_prio)
			queue_work(system_bh_highpri_wq, &bh->bh);
		else
			queue_work(system_bh_wq, &bh->bh);
	}
	bh->running = false;
	spin_unlock_irq(&bh->lock);
}

/*
 * usb_hcd_giveback_urb -- 将 URB 从 HCD 返回到 USB 设备驱动
 *
 * 这是 URB 完成路径的核心分发函数, 由 HC 驱动的中断处理程序或
 * 轮询机制调用。它决定 URB 的 complete 回调在哪种上下文中执行:
 *
 * ============================== 上下文选择 ==============================
 *
 * 1. 非 BH 模式 + 非根 HUB: 直接同步调用
 *    适用于那些可以在中断上下文安全执行回调的驱动.
 *    判断条件: !hcd_giveback_urb_in_bh(hcd) && !is_root_hub(urb->dev)
 *
 * 2. BH 模式 (默认): 通过 workqueue 延迟执行
 *    a) 将 URB 加入 bh->head 链表
 *    b) 如果 BH 尚未运行, 调度 workqueue:
 *       - 等时/中断传输: high_prio_bh → system_bh_highpri_wq
 *       - 控制/批量传输: low_prio_bh  → system_bh_wq
 *    c) 如果 BH 已在运行中, 新 URB 会在下一轮被处理
 *
 * 3. 根 HUB URB: 总是通过 BH 路径
 *    因为根 HUB 的控制传输在 rh_call_control() 中同步执行,
 *    需要将完成回调延迟到非中断上下文.
 *
 * ============================== 状态传递 ==============================
 *
 * 状态通过 urb->unlinked 传递:
 *   - 如果 urb->unlinked 已被设置 (取消路径), 优先使用该值
 *   - 否则使用 status 参数
 * 这确保了取消操作的优先级高于正常完成.
 *
 * 锁要求: 调用者必须已经:
 *   1. 释放了 HC 驱动的所有私有锁
 *   2. 调用了 usb_hcd_unlink_urb_from_ep()
 *   3. urb->hcpriv 中的资源已释放
 */
/**
 * usb_hcd_giveback_urb - return URB from HCD to device driver
 * @hcd: host controller returning the URB
 * @urb: urb being returned to the USB device driver.
 * @status: completion status code for the URB.
 *
 * Context: atomic. The completion callback is invoked either in a work queue
 * (BH) context or in the caller's context, depending on whether the HCD_BH
 * flag is set in the @hcd structure, except that URBs submitted to the
 * root hub always complete in BH context.
 *
 * This hands the URB from HCD to its USB device driver, using its
 * completion function.  The HCD has freed all per-urb resources
 * (and is done using urb->hcpriv).  It also released all HCD locks;
 * the device driver won't cause problems if it frees, modifies,
 * or resubmits this URB.
 *
 * If @urb was unlinked, the value of @status will be overridden by
 * @urb->unlinked.  Erroneous short transfers are detected in case
 * the HCD hasn't checked for them.
 */
void usb_hcd_giveback_urb(struct usb_hcd *hcd, struct urb *urb, int status)
{
	struct giveback_urb_bh *bh;
	bool running;

	/* pass status to BH via unlinked */
	if (likely(!urb->unlinked))
		urb->unlinked = status;

	if (!hcd_giveback_urb_in_bh(hcd) && !is_root_hub(urb->dev)) {
		__usb_hcd_giveback_urb(urb);
		return;
	}

	if (usb_pipeisoc(urb->pipe) || usb_pipeint(urb->pipe))
		bh = &hcd->high_prio_bh;
	else
		bh = &hcd->low_prio_bh;

	spin_lock(&bh->lock);
	list_add_tail(&urb->urb_list, &bh->head);
	running = bh->running;
	spin_unlock(&bh->lock);

	if (running)
		;
	else if (bh->high_prio)
		queue_work(system_bh_highpri_wq, &bh->bh);
	else
		queue_work(system_bh_wq, &bh->bh);
}
EXPORT_SYMBOL_GPL(usb_hcd_giveback_urb);

/*-------------------------------------------------------------------------*/

/*
 * usb_hcd_flush_endpoint -- 清空端点上的所有待处理 URB
 *
 * 当设备断开、配置更改或驱动卸载时, 需要取消端点上所有未完成的 URB。
 * 此函数执行以下操作:
 *   1. 遍历端点的 urb_list, 对每个未取消的 URB 调用 unlink1()
 *   2. 使用 goto rescan 处理链表在取消过程中的变化
 *   3. 遍历完成后, 等待端点队列完全变空 (使用 usb_kill_urb 同步等待)
 *
 * 调用者必须确保没有新的 URB 可以提交到此端点 (通常已禁用端点或断开设备).
 */
/* Cancel all URBs pending on this endpoint and wait for the endpoint's
 * queue to drain completely.  The caller must first insure that no more
 * URBs can be submitted for this endpoint.
 */
void usb_hcd_flush_endpoint(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	struct usb_hcd		*hcd;
	struct urb		*urb;

	if (!ep)
		return;
	might_sleep();
	hcd = bus_to_hcd(udev->bus);

	/* No more submits can occur */
	spin_lock_irq(&hcd_urb_list_lock);
rescan:
	list_for_each_entry_reverse(urb, &ep->urb_list, urb_list) {
		int	is_in;

		if (urb->unlinked)
			continue;
		usb_get_urb (urb);
		is_in = usb_urb_dir_in(urb);
		spin_unlock(&hcd_urb_list_lock);

		/* kick hcd */
		unlink1(hcd, urb, -ESHUTDOWN);
		dev_dbg (hcd->self.controller,
			"shutdown urb %p ep%d%s-%s\n",
			urb, usb_endpoint_num(&ep->desc),
			is_in ? "in" : "out",
			usb_ep_type_string(usb_endpoint_type(&ep->desc)));
		usb_put_urb (urb);

		/* list contents may have changed */
		spin_lock(&hcd_urb_list_lock);
		goto rescan;
	}
	spin_unlock_irq(&hcd_urb_list_lock);

	/* Wait until the endpoint queue is completely empty */
	while (!list_empty (&ep->urb_list)) {
		spin_lock_irq(&hcd_urb_list_lock);

		/* The list may have changed while we acquired the spinlock */
		urb = NULL;
		if (!list_empty (&ep->urb_list)) {
			urb = list_entry (ep->urb_list.prev, struct urb,
					urb_list);
			usb_get_urb (urb);
		}
		spin_unlock_irq(&hcd_urb_list_lock);

		if (urb) {
			usb_kill_urb (urb);
			usb_put_urb (urb);
		}
	}
}

/*
 * usb_hcd_alloc_bandwidth -- 检查和分配 USB 总线带宽
 *
 * 当设备的配置或替代接口设置发生变化时, 需要重新计算和分配总线带宽。
 * 此函数协调 HCD 驱动的 add_endpoint / drop_endpoint / check_bandwidth
 * 回调, 确保新配置的带宽需求不超过总线容量.
 *
 * 三种操作模式:
 *   1. 设置新配置 (new_config != NULL):
 *      - 删除旧配置的所有端点 (除端点 0 外)
 *      - 为新配置的每个接口的 alt setting 0 添加端点
 *      - 注意: 先删除旧端点再添加新端点, 避免重复计费
 *
 *   2. 重置设备到 ADDRESSED 状态 (new_config == NULL && cur_alt == NULL):
 *      - 删除所有非端点 0 的端点
 *      - 调用 check_bandwidth 确认更改
 *
 *   3. 切换替代接口 (cur_alt != NULL && new_alt != NULL):
 *      - 删除当前 alt setting 的所有端点
 *      - 添加新 alt setting 的所有端点
 *
 * 错误恢复:
 *   如果 add_endpoint 或 check_bandwidth 返回错误,
 *   调用 hcd->driver->reset_bandwidth() 回滚所有更改.
 *
 * 注意: 此函数需要 hcd->driver->check_bandwidth 被实现才能工作.
 *
 * 参数:
 *   @udev:      目标 USB 设备
 *   @new_config: 要安装的新配置 (或 NULL)
 *   @cur_alt:   当前的替代接口设置 (切换 alt 时使用)
 *   @new_alt:   要安装的新替代接口设置 (切换 alt 时使用)
 *
 * 返回: 0 表示成功, 负值表示带宽不足或资源受限.
 */
/**
 * usb_hcd_alloc_bandwidth - check whether a new bandwidth setting exceeds
 *				the bus bandwidth
 * @udev: target &usb_device
 * @new_config: new configuration to install
 * @cur_alt: the current alternate interface setting
 * @new_alt: alternate interface setting that is being installed
 *
 * To change configurations, pass in the new configuration in new_config,
 * and pass NULL for cur_alt and new_alt.
 *
 * To reset a device's configuration (put the device in the ADDRESSED state),
 * pass in NULL for new_config, cur_alt, and new_alt.
 *
 * To change alternate interface settings, pass in NULL for new_config,
 * pass in the current alternate interface setting in cur_alt,
 * and pass in the new alternate interface setting in new_alt.
 *
 * Return: An error if the requested bandwidth change exceeds the
 * bus bandwidth or host controller internal resources.
 */
int usb_hcd_alloc_bandwidth(struct usb_device *udev,
		struct usb_host_config *new_config,
		struct usb_host_interface *cur_alt,
		struct usb_host_interface *new_alt)
{
	int num_intfs, i, j;
	struct usb_host_interface *alt = NULL;
	int ret = 0;
	struct usb_hcd *hcd;
	struct usb_host_endpoint *ep;

	hcd = bus_to_hcd(udev->bus);
	if (!hcd->driver->check_bandwidth)
		return 0;

	/* Configuration is being removed - set configuration 0 */
	if (!new_config && !cur_alt) {
		for (i = 1; i < 16; ++i) {
			ep = udev->ep_out[i];
			if (ep)
				hcd->driver->drop_endpoint(hcd, udev, ep);
			ep = udev->ep_in[i];
			if (ep)
				hcd->driver->drop_endpoint(hcd, udev, ep);
		}
		hcd->driver->check_bandwidth(hcd, udev);
		return 0;
	}
	/* Check if the HCD says there's enough bandwidth.  Enable all endpoints
	 * each interface's alt setting 0 and ask the HCD to check the bandwidth
	 * of the bus.  There will always be bandwidth for endpoint 0, so it's
	 * ok to exclude it.
	 */
	if (new_config) {
		num_intfs = new_config->desc.bNumInterfaces;
		/* Remove endpoints (except endpoint 0, which is always on the
		 * schedule) from the old config from the schedule
		 */
		for (i = 1; i < 16; ++i) {
			ep = udev->ep_out[i];
			if (ep) {
				ret = hcd->driver->drop_endpoint(hcd, udev, ep);
				if (ret < 0)
					goto reset;
			}
			ep = udev->ep_in[i];
			if (ep) {
				ret = hcd->driver->drop_endpoint(hcd, udev, ep);
				if (ret < 0)
					goto reset;
			}
		}
		for (i = 0; i < num_intfs; ++i) {
			struct usb_host_interface *first_alt;
			int iface_num;

			first_alt = &new_config->intf_cache[i]->altsetting[0];
			iface_num = first_alt->desc.bInterfaceNumber;
			/* Set up endpoints for alternate interface setting 0 */
			alt = usb_find_alt_setting(new_config, iface_num, 0);
			if (!alt)
				/* No alt setting 0? Pick the first setting. */
				alt = first_alt;

			for (j = 0; j < alt->desc.bNumEndpoints; j++) {
				ret = hcd->driver->add_endpoint(hcd, udev, &alt->endpoint[j]);
				if (ret < 0)
					goto reset;
			}
		}
	}
	if (cur_alt && new_alt) {
		struct usb_interface *iface = usb_ifnum_to_if(udev,
				cur_alt->desc.bInterfaceNumber);

		if (!iface)
			return -EINVAL;
		if (iface->resetting_device) {
			/*
			 * The USB core just reset the device, so the xHCI host
			 * and the device will think alt setting 0 is installed.
			 * However, the USB core will pass in the alternate
			 * setting installed before the reset as cur_alt.  Dig
			 * out the alternate setting 0 structure, or the first
			 * alternate setting if a broken device doesn't have alt
			 * setting 0.
			 */
			cur_alt = usb_altnum_to_altsetting(iface, 0);
			if (!cur_alt)
				cur_alt = &iface->altsetting[0];
		}

		/* Drop all the endpoints in the current alt setting */
		for (i = 0; i < cur_alt->desc.bNumEndpoints; i++) {
			ret = hcd->driver->drop_endpoint(hcd, udev,
					&cur_alt->endpoint[i]);
			if (ret < 0)
				goto reset;
		}
		/* Add all the endpoints in the new alt setting */
		for (i = 0; i < new_alt->desc.bNumEndpoints; i++) {
			ret = hcd->driver->add_endpoint(hcd, udev,
					&new_alt->endpoint[i]);
			if (ret < 0)
				goto reset;
		}
	}
	ret = hcd->driver->check_bandwidth(hcd, udev);
reset:
	if (ret < 0)
		hcd->driver->reset_bandwidth(hcd, udev);
	return ret;
}

/*
 * usb_hcd_disable_endpoint -- 禁用端点并清理 HCD 端状态
 *
 * 在配置更改、接口切换、驱动卸载或物理断开时调用。
 * 调用 usb_hcd_flush_endpoint() 之后执行, 确保所有 URB 已移除.
 *
 * 委托给 hc_driver->endpoint_disable() 释放 HCD 驱动维护的端点私有数据
 * (如 xHCI 的 endpoint ring 上下文、EHCI 的 qh 结构等).
 *
 * 注意: 此函数可能睡眠 (might_sleep).
 */
/* Disables the endpoint: synchronizes with the hcd to make sure all
 * endpoint state is gone from hardware.  usb_hcd_flush_endpoint() must
 * have been called previously.  Use for set_configuration, set_interface,
 * driver removal, physical disconnect.
 *
 * example:  a qh stored in ep->hcpriv, holding state related to endpoint
 * type, maxpacket size, toggle, halt status, and scheduling.
 */
void usb_hcd_disable_endpoint(struct usb_device *udev,
		struct usb_host_endpoint *ep)
{
	struct usb_hcd		*hcd;

	might_sleep();
	hcd = bus_to_hcd(udev->bus);
	if (hcd->driver->endpoint_disable)
		hcd->driver->endpoint_disable(hcd, ep);
}

/*
 * usb_hcd_reset_endpoint -- 复位主机端点的状态
 *
 * 复位端点的数据切换位 (data toggle) 和其他主机端状态。
 * 在清除端点的 HALT 特征后调用 (如 CLEAR_FEATURE(ENDPOINT_HALT)).
 *
 * 首选调用 hc_driver->endpoint_reset() 让 HCD 驱动处理,
 * 否则使用通用回退: 通过 usb_settoggle() 复位数据切换位.
 *
 * 对于控制端点, 同时复位 IN 和 OUT 两个方向的数据切换位.
 */
/**
 * usb_hcd_reset_endpoint - reset host endpoint state
 * @udev: USB device.
 * @ep:   the endpoint to reset.
 *
 * Resets any host endpoint state such as the toggle bit, sequence
 * number and current window.
 */
void usb_hcd_reset_endpoint(struct usb_device *udev,
			    struct usb_host_endpoint *ep)
{
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	if (hcd->driver->endpoint_reset)
		hcd->driver->endpoint_reset(hcd, ep);
	else {
		int epnum = usb_endpoint_num(&ep->desc);
		int is_out = usb_endpoint_dir_out(&ep->desc);
		int is_control = usb_endpoint_xfer_control(&ep->desc);

		usb_settoggle(udev, epnum, is_out, 0);
		if (is_control)
			usb_settoggle(udev, epnum, !is_out, 0);
	}
}

/*
 * usb_alloc_streams -- 为批量端点分配流 ID (SuperSpeed 特性)
 *
 * USB 3.0 (SuperSpeed) 引入了流 (Stream) 概念, 允许在一个批量端点
 * 上同时进行多个独立的传输, 每个流有独立的 ID. 流可以乱序完成,
 * 提高了批量传输的并行性和吞吐量.
 *
 * 调用条件:
 *   - 设备速度必须 >= USB_SPEED_SUPER
 *   - 设备状态必须 >= USB_STATE_CONFIGURED
 *   - 目标端点必须是批量端点
 *   - 端点不能已分配流 (不支持重新分配)
 *   - HCD 驱动必须实现 alloc_streams / free_streams
 *
 * 分配成功后, 端点结构中的 streams 字段保存分配到的流数量.
 *
 * 参数:
 *   @interface:  包含所有端点的替代接口
 *   @eps:        需要分配流的端点数组
 *   @num_eps:    端点数量
 *   @num_streams: 请求分配的流数量
 *   @mem_flags:  内存分配标志
 *
 * 返回: 成功时返回分配的流数量, 失败时返回负错误码.
 */
/**
 * usb_alloc_streams - allocate bulk endpoint stream IDs.
 * @interface:		alternate setting that includes all endpoints.
 * @eps:		array of endpoints that need streams.
 * @num_eps:		number of endpoints in the array.
 * @num_streams:	number of streams to allocate.
 * @mem_flags:		flags hcd should use to allocate memory.
 *
 * Sets up a group of bulk endpoints to have @num_streams stream IDs available.
 * Drivers may queue multiple transfers to different stream IDs, which may
 * complete in a different order than they were queued.
 *
 * Return: On success, the number of allocated streams. On failure, a negative
 * error code.
 */
int usb_alloc_streams(struct usb_interface *interface,
		struct usb_host_endpoint **eps, unsigned int num_eps,
		unsigned int num_streams, gfp_t mem_flags)
{
	struct usb_hcd *hcd;
	struct usb_device *dev;
	int i, ret;

	dev = interface_to_usbdev(interface);
	hcd = bus_to_hcd(dev->bus);
	if (!hcd->driver->alloc_streams || !hcd->driver->free_streams)
		return -EINVAL;
	if (dev->speed < USB_SPEED_SUPER)
		return -EINVAL;
	if (dev->state < USB_STATE_CONFIGURED)
		return -ENODEV;

	for (i = 0; i < num_eps; i++) {
		/* Streams only apply to bulk endpoints. */
		if (!usb_endpoint_xfer_bulk(&eps[i]->desc))
			return -EINVAL;
		/* Re-alloc is not allowed */
		if (eps[i]->streams)
			return -EINVAL;
	}

	ret = hcd->driver->alloc_streams(hcd, dev, eps, num_eps,
			num_streams, mem_flags);
	if (ret < 0)
		return ret;

	for (i = 0; i < num_eps; i++)
		eps[i]->streams = ret;

	return ret;
}
EXPORT_SYMBOL_GPL(usb_alloc_streams);

/*
 * usb_free_streams -- 释放批量端点的流 ID
 *
 * 撤销 usb_alloc_streams() 的效果, 将端点恢复为不使用流 ID 的模式。
 * 释放后, endpoints[i]->streams 被清零.
 *
 * 注意: 不支持双重释放 (free 已释放的流).
 */
/**
 * usb_free_streams - free bulk endpoint stream IDs.
 * @interface:	alternate setting that includes all endpoints.
 * @eps:	array of endpoints to remove streams from.
 * @num_eps:	number of endpoints in the array.
 * @mem_flags:	flags hcd should use to allocate memory.
 *
 * Reverts a group of bulk endpoints back to not using stream IDs.
 * Can fail if we are given bad arguments, or HCD is broken.
 *
 * Return: 0 on success. On failure, a negative error code.
 */
int usb_free_streams(struct usb_interface *interface,
		struct usb_host_endpoint **eps, unsigned int num_eps,
		gfp_t mem_flags)
{
	struct usb_hcd *hcd;
	struct usb_device *dev;
	int i, ret;

	dev = interface_to_usbdev(interface);
	hcd = bus_to_hcd(dev->bus);
	if (dev->speed < USB_SPEED_SUPER)
		return -EINVAL;

	/* Double-free is not allowed */
	for (i = 0; i < num_eps; i++)
		if (!eps[i] || !eps[i]->streams)
			return -EINVAL;

	ret = hcd->driver->free_streams(hcd, dev, eps, num_eps, mem_flags);
	if (ret < 0)
		return ret;

	for (i = 0; i < num_eps; i++)
		eps[i]->streams = 0;

	return ret;
}
EXPORT_SYMBOL_GPL(usb_free_streams);

/*
 * usb_hcd_synchronize_unlinks -- 等待所有 URB 取消完成
 *
 * 保护机制: 防止驱动程序在设备已断开后仍试图取消 URB.
 * 由于内核不按设备跟踪 URB, 此函数通过简单地获取再释放
 * hcd_urb_unlink_lock 来确保所有正在进行的取消操作已完成.
 *
 * 当 hcd_urb_unlink_lock 被持有时, usb_hcd_unlink_urb() 会等待,
 * 因此获取再释放此锁就足以同步所有正在进行的取消操作.
 *
 * 此函数在 usb_disable_device() 中断开设备时调用.
 */
/* Protect against drivers that try to unlink URBs after the device
 * is gone, by waiting until all unlinks for @udev are finished.
 * Since we don't currently track URBs by device, simply wait until
 * nothing is running in the locked region of usb_hcd_unlink_urb().
 */
void usb_hcd_synchronize_unlinks(struct usb_device *udev)
{
	spin_lock_irq(&hcd_urb_unlink_lock);
	spin_unlock_irq(&hcd_urb_unlink_lock);
}

/*-------------------------------------------------------------------------*/

/*
 * usb_hcd_get_frame_number -- 获取当前 USB 帧/微帧编号
 *
 * 委托给 hc_driver->get_frame_number() 获取硬件当前正在处理的
 * 帧/微帧编号。USB 1.x/2.0 使用帧 (1ms) 编号, USB 2.0 高速模式
 * 使用微帧 (125us) 编号.
 *
 * 主要用于需要精确时间同步的等时传输和调试.
 *
 * 注意: 如果 HC 未运行 (挂起或死亡), 返回 -ESHUTDOWN.
 */
/* called in any context */
int usb_hcd_get_frame_number (struct usb_device *udev)
{
	struct usb_hcd	*hcd = bus_to_hcd(udev->bus);

	if (!HCD_RH_RUNNING(hcd))
		return -ESHUTDOWN;
	return hcd->driver->get_frame_number (hcd);
}

/*-------------------------------------------------------------------------*/
#ifdef CONFIG_USB_HCD_TEST_MODE

static void usb_ehset_completion(struct urb *urb)
{
	struct completion  *done = urb->context;

	complete(done);
}
/*
 * Allocate and initialize a control URB. This request will be used by the
 * EHSET SINGLE_STEP_SET_FEATURE test in which the DATA and STATUS stages
 * of the GetDescriptor request are sent 15 seconds after the SETUP stage.
 * Return NULL if failed.
 */
static struct urb *request_single_step_set_feature_urb(
	struct usb_device	*udev,
	void			*dr,
	void			*buf,
	struct completion	*done)
{
	struct urb *urb;
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return NULL;

	urb->pipe = usb_rcvctrlpipe(udev, 0);

	urb->ep = &udev->ep0;
	urb->dev = udev;
	urb->setup_packet = (void *)dr;
	urb->transfer_buffer = buf;
	urb->transfer_buffer_length = USB_DT_DEVICE_SIZE;
	urb->complete = usb_ehset_completion;
	urb->status = -EINPROGRESS;
	urb->actual_length = 0;
	urb->transfer_flags = URB_DIR_IN | URB_NO_TRANSFER_DMA_MAP;
	usb_get_urb(urb);
	atomic_inc(&urb->use_count);
	atomic_inc(&urb->dev->urbnum);
	if (map_urb_for_dma(hcd, urb, GFP_KERNEL)) {
		usb_put_urb(urb);
		usb_free_urb(urb);
		return NULL;
	}

	urb->context = done;
	return urb;
}

int ehset_single_step_set_feature(struct usb_hcd *hcd, int port)
{
	int retval = -ENOMEM;
	struct usb_ctrlrequest *dr;
	struct urb *urb;
	struct usb_device *udev;
	struct usb_device_descriptor *buf;
	DECLARE_COMPLETION_ONSTACK(done);

	/* Obtain udev of the rhub's child port */
	udev = usb_hub_find_child(hcd->self.root_hub, port);
	if (!udev) {
		dev_err(hcd->self.controller, "No device attached to the RootHub\n");
		return -ENODEV;
	}
	buf = kmalloc(USB_DT_DEVICE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	dr = kmalloc_obj(struct usb_ctrlrequest);
	if (!dr) {
		kfree(buf);
		return -ENOMEM;
	}

	/* Fill Setup packet for GetDescriptor */
	dr->bRequestType = USB_DIR_IN;
	dr->bRequest = USB_REQ_GET_DESCRIPTOR;
	dr->wValue = cpu_to_le16(USB_DT_DEVICE << 8);
	dr->wIndex = 0;
	dr->wLength = cpu_to_le16(USB_DT_DEVICE_SIZE);
	urb = request_single_step_set_feature_urb(udev, dr, buf, &done);
	if (!urb)
		goto cleanup;

	/* Submit just the SETUP stage */
	retval = hcd->driver->submit_single_step_set_feature(hcd, urb, 1);
	if (retval)
		goto out1;
	if (!wait_for_completion_timeout(&done, msecs_to_jiffies(2000))) {
		usb_kill_urb(urb);
		retval = -ETIMEDOUT;
		dev_err(hcd->self.controller,
			"%s SETUP stage timed out on ep0\n", __func__);
		goto out1;
	}
	msleep(15 * 1000);

	/* Complete remaining DATA and STATUS stages using the same URB */
	urb->status = -EINPROGRESS;
	urb->transfer_flags &= ~URB_NO_TRANSFER_DMA_MAP;
	usb_get_urb(urb);
	atomic_inc(&urb->use_count);
	atomic_inc(&urb->dev->urbnum);
	if (map_urb_for_dma(hcd, urb, GFP_KERNEL)) {
		usb_put_urb(urb);
		goto out1;
	}

	retval = hcd->driver->submit_single_step_set_feature(hcd, urb, 0);
	if (!retval && !wait_for_completion_timeout(&done,
						msecs_to_jiffies(2000))) {
		usb_kill_urb(urb);
		retval = -ETIMEDOUT;
		dev_err(hcd->self.controller,
			"%s IN stage timed out on ep0\n", __func__);
	}
out1:
	usb_free_urb(urb);
cleanup:
	kfree(dr);
	kfree(buf);
	return retval;
}
EXPORT_SYMBOL_GPL(ehset_single_step_set_feature);
#endif /* CONFIG_USB_HCD_TEST_MODE */

/*-------------------------------------------------------------------------*/

#ifdef	CONFIG_PM

/*
 * hcd_bus_suspend -- 挂起整个 USB 总线
 *
 * 由 USB 核心 (hub_wq) 在根 HUB 进入挂起状态时调用。
 * 流程:
 *   1. 清除 RH_RUNNING 标志, 阻止新的 URB 提交
 *   2. 将 HC 状态设置为 QUIESCING (静止中)
 *   3. 调用 hc_driver->bus_suspend() 让 HC 驱动挂起硬件
 *   4. 挂起成功后:
 *      - 设置设备状态为 USB_STATE_SUSPENDED
 *      - 如果不是自动挂起, 关闭 PHY
 *      - 检查是否有唤醒事件竞争 (hub_status_data)
 *   5. 挂起失败时: 恢复 RH_RUNNING 标志和 HC 状态
 *
 * 注意: 支持远程唤醒的根 HUB 在挂起后如果检测到端口状态变化,
 * 需要立即恢复以处理唤醒事件.
 */
int hcd_bus_suspend(struct usb_device *rhdev, pm_message_t msg)
{
	struct usb_hcd	*hcd = bus_to_hcd(rhdev->bus);
	int		status;
	int		old_state = hcd->state;

	dev_dbg(&rhdev->dev, "bus %ssuspend, wakeup %d\n",
			(PMSG_IS_AUTO(msg) ? "auto-" : ""),
			rhdev->do_remote_wakeup);
	if (HCD_DEAD(hcd)) {
		dev_dbg(&rhdev->dev, "skipped %s of dead bus\n", "suspend");
		return 0;
	}

	if (!hcd->driver->bus_suspend) {
		status = -ENOENT;
	} else {
		clear_bit(HCD_FLAG_RH_RUNNING, &hcd->flags);
		hcd->state = HC_STATE_QUIESCING;
		status = hcd->driver->bus_suspend(hcd);
	}
	if (status == 0) {
		usb_set_device_state(rhdev, USB_STATE_SUSPENDED);
		hcd->state = HC_STATE_SUSPENDED;

		if (!PMSG_IS_AUTO(msg))
			usb_phy_roothub_suspend(hcd->self.sysdev,
						hcd->phy_roothub);

		/* Did we race with a root-hub wakeup event? */
		if (rhdev->do_remote_wakeup) {
			char	buffer[6];

			status = hcd->driver->hub_status_data(hcd, buffer);
			if (status != 0) {
				dev_dbg(&rhdev->dev, "suspend raced with wakeup event\n");
				hcd_bus_resume(rhdev, PMSG_AUTO_RESUME);
				status = -EBUSY;
			}
		}
	} else {
		spin_lock_irq(&hcd_root_hub_lock);
		if (!HCD_DEAD(hcd)) {
			set_bit(HCD_FLAG_RH_RUNNING, &hcd->flags);
			hcd->state = old_state;
		}
		spin_unlock_irq(&hcd_root_hub_lock);
		dev_dbg(&rhdev->dev, "bus %s fail, err %d\n",
				"suspend", status);
	}
	return status;
}

/*
 * hcd_bus_resume -- 恢复整个 USB 总线
 *
 * 流程:
 *   1. 如果不是自动恢复, 先恢复 PHY
 *   2. 检查 HC 驱动是否实现了 bus_resume
 *   3. 将 HC 状态设置为 RESUMING
 *   4. 调用 hc_driver->bus_resume() 恢复硬件
 *   5. 清除 WAKEUP_PENDING 标志
 *   6. 校准 PHY
 *   7. 恢复成功:
 *      - 恢复设备状态
 *      - 设置 RH_RUNNING 标志 (允许提交 URB)
 *      - 如果存在未挂起的端口, 执行 TRSMRCY 延迟 (USB 2.0 全局恢复)
 *   8. 恢复失败:
 *      - 恢复 HC 到之前的状态
 *      - 重新挂起 PHY
 *      - 如果错误不是 -ESHUTDOWN, 调用 usb_hc_died() (HC 可能已死亡)
 */
int hcd_bus_resume(struct usb_device *rhdev, pm_message_t msg)
{
	struct usb_hcd	*hcd = bus_to_hcd(rhdev->bus);
	int		status;
	int		old_state = hcd->state;

	dev_dbg(&rhdev->dev, "usb %sresume\n",
			(PMSG_IS_AUTO(msg) ? "auto-" : ""));
	if (HCD_DEAD(hcd)) {
		dev_dbg(&rhdev->dev, "skipped %s of dead bus\n", "resume");
		return 0;
	}

	if (!PMSG_IS_AUTO(msg)) {
		status = usb_phy_roothub_resume(hcd->self.sysdev,
						hcd->phy_roothub);
		if (status)
			return status;
	}

	if (!hcd->driver->bus_resume)
		return -ENOENT;
	if (HCD_RH_RUNNING(hcd))
		return 0;

	hcd->state = HC_STATE_RESUMING;
	status = hcd->driver->bus_resume(hcd);
	clear_bit(HCD_FLAG_WAKEUP_PENDING, &hcd->flags);
	if (status == 0)
		status = usb_phy_roothub_calibrate(hcd->phy_roothub);

	if (status == 0) {
		struct usb_device *udev;
		int port1;

		spin_lock_irq(&hcd_root_hub_lock);
		if (!HCD_DEAD(hcd)) {
			usb_set_device_state(rhdev, rhdev->actconfig
					? USB_STATE_CONFIGURED
					: USB_STATE_ADDRESS);
			set_bit(HCD_FLAG_RH_RUNNING, &hcd->flags);
			hcd->state = HC_STATE_RUNNING;
		}
		spin_unlock_irq(&hcd_root_hub_lock);

		/*
		 * Check whether any of the enabled ports on the root hub are
		 * unsuspended.  If they are then a TRSMRCY delay is needed
		 * (this is what the USB-2 spec calls a "global resume").
		 * Otherwise we can skip the delay.
		 */
		usb_hub_for_each_child(rhdev, port1, udev) {
			if (udev->state != USB_STATE_NOTATTACHED &&
					!udev->port_is_suspended) {
				usleep_range(10000, 11000);	/* TRSMRCY */
				break;
			}
		}
	} else {
		hcd->state = old_state;
		usb_phy_roothub_suspend(hcd->self.sysdev, hcd->phy_roothub);
		dev_dbg(&rhdev->dev, "bus %s fail, err %d\n",
				"resume", status);
		if (status != -ESHUTDOWN)
			usb_hc_died(hcd);
	}
	return status;
}

/* Workqueue routine for root-hub remote wakeup */
static void hcd_resume_work(struct work_struct *work)
{
	struct usb_hcd *hcd = container_of(work, struct usb_hcd, wakeup_work);
	struct usb_device *udev = hcd->self.root_hub;

	usb_remote_wakeup(udev);
}

/*
 * usb_hcd_resume_root_hub -- HC 驱动请求恢复根 HUB
 *
 * 当主机控制器硬件检测到端口上的远程唤醒信号时, 调用此函数。
 * 例如: 设备在挂起状态下检测到 USB 总线活动而发起远程唤醒.
 *
 * 作用:
 *   1. 记录唤醒事件 (pm_wakeup_event)
 *   2. 设置 WAKEUP_PENDING 标志
 *   3. 通过系统工作队列调度 hcd_resume_work() 来异步恢复根 HUB
 *
 * 使用 workqueue 而非直接恢复的原因: 需要在进程上下文中执行,
 * 因为恢复过程可能涉及锁获取和 PHY 操作等可能睡眠的操作.
 *
 * 注意: 在 CONFIG_PM 未配置时此函数为空.
 */
/**
 * usb_hcd_resume_root_hub - called by HCD to resume its root hub
 * @hcd: host controller for this root hub
 *
 * The USB host controller calls this function when its root hub is
 * suspended (with the remote wakeup feature enabled) and a remote
 * wakeup request is received.  The routine submits a workqueue request
 * to resume the root hub (that is, manage its downstream ports again).
 */
void usb_hcd_resume_root_hub (struct usb_hcd *hcd)
{
	unsigned long flags;

	spin_lock_irqsave (&hcd_root_hub_lock, flags);
	if (hcd->rh_registered) {
		pm_wakeup_event(&hcd->self.root_hub->dev, 0);
		set_bit(HCD_FLAG_WAKEUP_PENDING, &hcd->flags);
		queue_work(system_freezable_wq, &hcd->wakeup_work);
	}
	spin_unlock_irqrestore (&hcd_root_hub_lock, flags);
}
EXPORT_SYMBOL_GPL(usb_hcd_resume_root_hub);

#endif	/* CONFIG_PM */

/*-------------------------------------------------------------------------*/

#ifdef	CONFIG_USB_OTG

/**
 * usb_bus_start_enum - start immediate enumeration (for OTG)
 * @bus: the bus (must use hcd framework)
 * @port_num: 1-based number of port; usually bus->otg_port
 * Context: atomic
 *
 * Starts enumeration, with an immediate reset followed later by
 * hub_wq identifying and possibly configuring the device.
 * This is needed by OTG controller drivers, where it helps meet
 * HNP protocol timing requirements for starting a port reset.
 *
 * Return: 0 if successful.
 */
int usb_bus_start_enum(struct usb_bus *bus, unsigned port_num)
{
	struct usb_hcd		*hcd;
	int			status = -EOPNOTSUPP;

	/* NOTE: since HNP can't start by grabbing the bus's address0_sem,
	 * boards with root hubs hooked up to internal devices (instead of
	 * just the OTG port) may need more attention to resetting...
	 */
	hcd = bus_to_hcd(bus);
	if (port_num && hcd->driver->start_port_reset)
		status = hcd->driver->start_port_reset(hcd, port_num);

	/* allocate hub_wq shortly after (first) root port reset finishes;
	 * it may issue others, until at least 50 msecs have passed.
	 */
	if (status == 0)
		mod_timer(&hcd->rh_timer, jiffies + msecs_to_jiffies(10));
	return status;
}
EXPORT_SYMBOL_GPL(usb_bus_start_enum);

#endif

/*-------------------------------------------------------------------------*/

/*
 * usb_hcd_irq -- HCD 框架的中断处理入口
 *
 * 注册为所有 USB 主机控制器的中断处理函数 (由 usb_add_hcd 通过 request_irq 注册).
 *
 * 处理逻辑:
 *   1. 如果 HC 已死亡或硬件不可访问, 返回 IRQ_NONE
 *   2. 否则委托给 hc_driver->irq() 进行实际的硬件中断处理
 *
 * 注意: 实际的 URB 完成处理可能在 HC 驱动的 irq() 中通过
 * usb_hcd_giveback_urb() 触发, 后者将 complete 回调延迟到 BH 中执行.
 */
/**
 * usb_hcd_irq - hook IRQs to HCD framework (bus glue)
 * @irq: the IRQ being raised
 * @__hcd: pointer to the HCD whose IRQ is being signaled
 *
 * If the controller isn't HALTed, calls the driver's irq handler.
 * Checks whether the controller is now dead.
 *
 * Return: %IRQ_HANDLED if the IRQ was handled. %IRQ_NONE otherwise.
 */
irqreturn_t usb_hcd_irq (int irq, void *__hcd)
{
	struct usb_hcd		*hcd = __hcd;
	irqreturn_t		rc;

	if (unlikely(HCD_DEAD(hcd) || !HCD_HW_ACCESSIBLE(hcd)))
		rc = IRQ_NONE;
	else if (hcd->driver->irq(hcd) == IRQ_NONE)
		rc = IRQ_NONE;
	else
		rc = IRQ_HANDLED;

	return rc;
}
EXPORT_SYMBOL_GPL(usb_hcd_irq);

/*-------------------------------------------------------------------------*/

/* Workqueue routine for when the root-hub has died. */
static void hcd_died_work(struct work_struct *work)
{
	struct usb_hcd *hcd = container_of(work, struct usb_hcd, died_work);
	static char *env[] = {
		"ERROR=DEAD",
		NULL
	};

	/* Notify user space that the host controller has died */
	kobject_uevent_env(&hcd->self.root_hub->dev.kobj, KOBJ_OFFLINE, env);
}

/*
 * usb_hc_died -- 报告主机控制器异常死亡
 *
 * 当主机控制器硬件发生致命错误 (如 PCI SERR、DMA 错误、超时等)
 * 时调用此函数。由总线粘合层 (PCI glue 等) 自动检测并调用.
 *
 * 处理步骤:
 *   1. 清除 RH_RUNNING 标志 (拒绝新 URB 提交)
 *   2. 设置 DEAD 标志
 *   3. 如果根 HUB 已注册:
 *      a. 清除 POLL_RH 标志 (停止根 HUB 轮询)
 *      b. 将根 HUB 设备状态设置为 NOTATTACHED
 *      c. 唤醒 hub_wq 工作队列, 让其清理旧的 URB 和设备
 *   4. 如果存在 shared HCD (如 xHCI 的 USB 2.0 和 USB 3.0 共享),
 *      也对共享 HCD 执行相同的清理
 *   5. 调度 hcd_died_work 工作队列, 通过 kobject_uevent_env 向
 *      用户空间发送 ERROR=DEAD 事件 (KOBJ_OFFLINE)
 *
 * 注意: 此函数只应该使用 primary HCD 调用。
 * 对于 shared HCD 情况, 内部会处理两个根 HUB.
 */
/**
 * usb_hc_died - report abnormal shutdown of a host controller (bus glue)
 * @hcd: pointer to the HCD representing the controller
 *
 * This is called by bus glue to report a USB host controller that died
 * while operations may still have been pending.  It's called automatically
 * by the PCI glue, so only glue for non-PCI busses should need to call it.
 *
 * Only call this function with the primary HCD.
 */
void usb_hc_died (struct usb_hcd *hcd)
{
	unsigned long flags;

	dev_err (hcd->self.controller, "HC died; cleaning up\n");

	spin_lock_irqsave (&hcd_root_hub_lock, flags);
	clear_bit(HCD_FLAG_RH_RUNNING, &hcd->flags);
	set_bit(HCD_FLAG_DEAD, &hcd->flags);
	if (hcd->rh_registered) {
		clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);

		/* make hub_wq clean up old urbs and devices */
		usb_set_device_state (hcd->self.root_hub,
				USB_STATE_NOTATTACHED);
		usb_kick_hub_wq(hcd->self.root_hub);
	}
	if (usb_hcd_is_primary_hcd(hcd) && hcd->shared_hcd) {
		hcd = hcd->shared_hcd;
		clear_bit(HCD_FLAG_RH_RUNNING, &hcd->flags);
		set_bit(HCD_FLAG_DEAD, &hcd->flags);
		if (hcd->rh_registered) {
			clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);

			/* make hub_wq clean up old urbs and devices */
			usb_set_device_state(hcd->self.root_hub,
					USB_STATE_NOTATTACHED);
			usb_kick_hub_wq(hcd->self.root_hub);
		}
	}

	/* Handle the case where this function gets called with a shared HCD */
	if (usb_hcd_is_primary_hcd(hcd))
		schedule_work(&hcd->died_work);
	else
		schedule_work(&hcd->primary_hcd->died_work);

	spin_unlock_irqrestore (&hcd_root_hub_lock, flags);
	/* Make sure that the other roothub is also deallocated. */
}
EXPORT_SYMBOL_GPL (usb_hc_died);

/*-------------------------------------------------------------------------*/

static void init_giveback_urb_bh(struct giveback_urb_bh *bh)
{

	spin_lock_init(&bh->lock);
	INIT_LIST_HEAD(&bh->head);
	INIT_WORK(&bh->bh, usb_giveback_urb_bh);
}

/*
 * __usb_create_hcd -- 创建并初始化 HCD 结构体 (底层实现)
 *
 * 分配 struct usb_hcd, 末尾附带 hc_driver->hcd_priv_size 字节的
 * HC 驱动私有数据空间。这是 usb_create_hcd() 和
 * usb_create_shared_hcd() 的共同底层实现.
 *
 * 初始化内容:
 *   1. 为 primary HCD 分配 address0_mutex 和 bandwidth_mutex (互斥锁)
 *   2. 对于 shared HCD (如 xHCI 的 USB 2.0/3.0 共享控制器):
 *      复用 primary HCD 的互斥锁, 设置 primary_hcd 和 shared_hcd 指针,
 *      形成双向链接
 *   3. kref 引用计数初始化
 *   4. usb_bus_init: 初始化 usb_bus 的通用字段
 *   5. 保存 controller/sysdev/bus_name 设备指针
 *   6. 初始化 rh_timer 定时器 (根 HUB 状态轮询)
 *   7. 初始化 wakeup_work / died_work 工作队列
 *   8. 保存 hc_driver 指针, 解析 speed (从 flags 中提取 HCD_MASK)
 *
 * 参数:
 *   @driver:    HC 驱动操作集
 *   @sysdev:   DMA 操作的系统设备 (PCI 设备等)
 *   @dev:      控制器的通用设备
 *   @bus_name:  总线名称
 *   @primary_hcd: 共享 HCD 的 primary (或 NULL 表示独立的 HCD)
 *
 * 返回: 创建的 usb_hcd 指针, 或 NULL (内存不足).
 */
struct usb_hcd *__usb_create_hcd(const struct hc_driver *driver,
		struct device *sysdev, struct device *dev, const char *bus_name,
		struct usb_hcd *primary_hcd)
{
	struct usb_hcd *hcd;

	hcd = kzalloc(sizeof(*hcd) + driver->hcd_priv_size, GFP_KERNEL);
	if (!hcd)
		return NULL;
	if (primary_hcd == NULL) {
		hcd->address0_mutex = kmalloc_obj(*hcd->address0_mutex);
		if (!hcd->address0_mutex) {
			kfree(hcd);
			dev_dbg(dev, "hcd address0 mutex alloc failed\n");
			return NULL;
		}
		mutex_init(hcd->address0_mutex);
		hcd->bandwidth_mutex = kmalloc_obj(*hcd->bandwidth_mutex);
		if (!hcd->bandwidth_mutex) {
			kfree(hcd->address0_mutex);
			kfree(hcd);
			dev_dbg(dev, "hcd bandwidth mutex alloc failed\n");
			return NULL;
		}
		mutex_init(hcd->bandwidth_mutex);
		dev_set_drvdata(dev, hcd);
	} else {
		mutex_lock(&usb_port_peer_mutex);
		hcd->address0_mutex = primary_hcd->address0_mutex;
		hcd->bandwidth_mutex = primary_hcd->bandwidth_mutex;
		hcd->primary_hcd = primary_hcd;
		primary_hcd->primary_hcd = primary_hcd;
		hcd->shared_hcd = primary_hcd;
		primary_hcd->shared_hcd = hcd;
		mutex_unlock(&usb_port_peer_mutex);
	}

	kref_init(&hcd->kref);

	usb_bus_init(&hcd->self);
	hcd->self.controller = dev;
	hcd->self.sysdev = sysdev;
	hcd->self.bus_name = bus_name;

	timer_setup(&hcd->rh_timer, rh_timer_func, 0);
#ifdef CONFIG_PM
	INIT_WORK(&hcd->wakeup_work, hcd_resume_work);
#endif

	INIT_WORK(&hcd->died_work, hcd_died_work);

	hcd->driver = driver;
	hcd->speed = driver->flags & HCD_MASK;
	hcd->product_desc = (driver->product_desc) ? driver->product_desc :
			"USB Host Controller";
	return hcd;
}
EXPORT_SYMBOL_GPL(__usb_create_hcd);

/**
 * usb_create_shared_hcd - create and initialize an HCD structure
 * @driver: HC driver that will use this hcd
 * @dev: device for this HC, stored in hcd->self.controller
 * @bus_name: value to store in hcd->self.bus_name
 * @primary_hcd: a pointer to the usb_hcd structure that is sharing the
 *              PCI device.  Only allocate certain resources for the primary HCD
 *
 * Context: task context, might sleep.
 *
 * Allocate a struct usb_hcd, with extra space at the end for the
 * HC driver's private data.  Initialize the generic members of the
 * hcd structure.
 *
 * Return: On success, a pointer to the created and initialized HCD structure.
 * On failure (e.g. if memory is unavailable), %NULL.
 */
struct usb_hcd *usb_create_shared_hcd(const struct hc_driver *driver,
		struct device *dev, const char *bus_name,
		struct usb_hcd *primary_hcd)
{
	return __usb_create_hcd(driver, dev, dev, bus_name, primary_hcd);
}
EXPORT_SYMBOL_GPL(usb_create_shared_hcd);

/**
 * usb_create_hcd - create and initialize an HCD structure
 * @driver: HC driver that will use this hcd
 * @dev: device for this HC, stored in hcd->self.controller
 * @bus_name: value to store in hcd->self.bus_name
 *
 * Context: task context, might sleep.
 *
 * Allocate a struct usb_hcd, with extra space at the end for the
 * HC driver's private data.  Initialize the generic members of the
 * hcd structure.
 *
 * Return: On success, a pointer to the created and initialized HCD
 * structure. On failure (e.g. if memory is unavailable), %NULL.
 */
struct usb_hcd *usb_create_hcd(const struct hc_driver *driver,
		struct device *dev, const char *bus_name)
{
	return __usb_create_hcd(driver, dev, dev, bus_name, NULL);
}
EXPORT_SYMBOL_GPL(usb_create_hcd);

/*
 * hcd_release -- 释放 HCD 结构体的回调
 *
 * 当最后一次 usb_put_hcd() 递减 kref 引用计数到 0 时调用。
 *
 * 关键处理:
 *   - 如果此 HCD 有 shared peer, 清除 peer 的 shared_hcd 和
 *     primary_hcd 指针 (防止悬空指针)
 *   - 否则 (这是最后的 primary HCD), 释放 address0_mutex 和
 *     bandwidth_mutex
 *
 * 注意: peer 关系在 usb_port_peer_mutex 保护下更新.
 */
/*
 * Roothubs that share one PCI device must also share the bandwidth mutex.
 * Don't deallocate the bandwidth_mutex until the last shared usb_hcd is
 * deallocated.
 *
 * Make sure to deallocate the bandwidth_mutex only when the last HCD is
 * freed.  When hcd_release() is called for either hcd in a peer set,
 * invalidate the peer's ->shared_hcd and ->primary_hcd pointers.
 */
static void hcd_release(struct kref *kref)
{
	struct usb_hcd *hcd = container_of (kref, struct usb_hcd, kref);

	mutex_lock(&usb_port_peer_mutex);
	if (hcd->shared_hcd) {
		struct usb_hcd *peer = hcd->shared_hcd;

		peer->shared_hcd = NULL;
		peer->primary_hcd = NULL;
	} else {
		kfree(hcd->address0_mutex);
		kfree(hcd->bandwidth_mutex);
	}
	mutex_unlock(&usb_port_peer_mutex);
	kfree(hcd);
}

/*
 * usb_get_hcd / usb_put_hcd -- HCD 引用计数管理
 *
 * 通过 kref 机制管理 HCD 的生命周期。
 * usb_get_hcd: 增加引用计数
 * usb_put_hcd: 减少引用计数, 到 0 时调用 hcd_release() 释放
 */
struct usb_hcd *usb_get_hcd(struct usb_hcd *hcd)
{
	if (hcd)
		kref_get(&hcd->kref);
	return hcd;
}
EXPORT_SYMBOL_GPL(usb_get_hcd);

void usb_put_hcd(struct usb_hcd *hcd)
{
	if (hcd)
		kref_put(&hcd->kref, hcd_release);
}
EXPORT_SYMBOL_GPL(usb_put_hcd);

int usb_hcd_is_primary_hcd(struct usb_hcd *hcd)
{
	if (!hcd->primary_hcd)
		return 1;
	return hcd == hcd->primary_hcd;
}
EXPORT_SYMBOL_GPL(usb_hcd_is_primary_hcd);

int usb_hcd_find_raw_port_number(struct usb_hcd *hcd, int port1)
{
	if (!hcd->driver->find_raw_port_number)
		return port1;

	return hcd->driver->find_raw_port_number(hcd, port1);
}

/*
 * usb_hcd_request_irqs -- 注册 HCD 的中断处理函数
 *
 * 使用 request_irq() 注册 usb_hcd_irq 作为中断处理函数。
 * 中断描述符的格式: "driver_description:usb{busnum}".
 *
 * 如果 hc_driver 不提供 irq 处理函数 (如某些 SoC 平台通过
 * 轮询而非中断驱动), 则不注册中断, 仅显示资源地址信息.
 *
 * 注意: 只有 primary HCD 才注册中断 (shared HCD 复用同一个中断).
 */
static int usb_hcd_request_irqs(struct usb_hcd *hcd,
		unsigned int irqnum, unsigned long irqflags)
{
	int retval;

	if (hcd->driver->irq) {

		snprintf(hcd->irq_descr, sizeof(hcd->irq_descr), "%s:usb%d",
				hcd->driver->description, hcd->self.busnum);
		retval = request_irq(irqnum, &usb_hcd_irq, irqflags,
				hcd->irq_descr, hcd);
		if (retval != 0) {
			dev_err(hcd->self.controller,
					"request interrupt %d failed\n",
					irqnum);
			return retval;
		}
		hcd->irq = irqnum;
		dev_info(hcd->self.controller, "irq %d, %s 0x%08llx\n", irqnum,
				(hcd->driver->flags & HCD_MEMORY) ?
					"io mem" : "io port",
				(unsigned long long)hcd->rsrc_start);
	} else {
		hcd->irq = 0;
		if (hcd->rsrc_start)
			dev_info(hcd->self.controller, "%s 0x%08llx\n",
					(hcd->driver->flags & HCD_MEMORY) ?
						"io mem" : "io port",
					(unsigned long long)hcd->rsrc_start);
	}
	return 0;
}

/*
 * Before we free this root hub, flush in-flight peering attempts
 * and disable peer lookups
 */
static void usb_put_invalidate_rhdev(struct usb_hcd *hcd)
{
	struct usb_device *rhdev;

	mutex_lock(&usb_port_peer_mutex);
	rhdev = hcd->self.root_hub;
	hcd->self.root_hub = NULL;
	mutex_unlock(&usb_port_peer_mutex);
	usb_put_dev(rhdev);
}

/*
 * usb_stop_hcd -- 停止 HCD 硬件并停止根 HUB 轮询
 *
 * 在 usb_remove_hcd() 和 usb_add_hcd() 的错误路径中调用。
 *
 * 执行的操作:
 *   1. 设置 rh_pollable = 0 (阻止新轮询)
 *   2. 清除 POLL_RH 标志
 *   3. 同步删除 rh_timer 定时器 (timer_delete_sync 等待定时器完成)
 *   4. 调用 hc_driver->stop() 停止硬件
 *   5. 设置 HC 状态为 HALT
 *   6. 再次清除 POLL_RH 标志和删除定时器 (防止 stop() 中重新启动定时器)
 *
 * 注意: 双重 stop 定时器是为了防止 hc_driver->stop() 回调中
 * 重新启动了根 HUB 轮询定时器.
 */
/**
 * usb_stop_hcd - Halt the HCD
 * @hcd: the usb_hcd that has to be halted
 *
 * Stop the root-hub polling timer and invoke the HCD's ->stop callback.
 */
static void usb_stop_hcd(struct usb_hcd *hcd)
{
	hcd->rh_pollable = 0;
	clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	timer_delete_sync(&hcd->rh_timer);

	hcd->driver->stop(hcd);
	hcd->state = HC_STATE_HALT;

	/* In case the HCD restarted the timer, stop it again. */
	clear_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	timer_delete_sync(&hcd->rh_timer);
}

/*
 * usb_add_hcd -- 完成 HCD 初始化和注册 (核心初始化函数)
 *
 * 这是 HCD 初始化的第二阶段, 在 __usb_create_hcd() 之后调用。
 * 完成以下关键步骤:
 *
 * ============================== 初始化流程 ==============================
 *
 * 1. PHY 初始化:
 *    - 为 primary HCD 分配并初始化 USB PHY (物理层收发器)
 *    - 设置 PHY 模式 (USB_HOST_SS 或 USB_HOST)
 *    - 为 PHY 上电
 *
 * 2. 设备授权策略设置:
 *    根据 authorized_default 参数设置 hcd->dev_policy:
 *    - USB_AUTHORIZE_NONE:     所有设备默认未授权
 *    - USB_AUTHORIZE_INTERNAL: 仅内部设备授权
 *    - USB_AUTHORIZE_ALL:      所有设备授权 (默认)
 *
 * 3. DMA 缓冲区创建:
 *    hcd_buffer_create() 创建 DMA 一致性内存池
 *
 * 4. 总线注册:
 *    usb_register_bus() 通过 IDR 分配总线编号
 *
 * 5. 根 HUB 分配:
 *    usb_alloc_dev() 创建根 HUB 的 usb_device 结构
 *
 * 6. 设置根 HUB 速度:
 *    根据 hcd->speed 设置根 HUB 的速度和 SSP 等级:
 *    HCD_USB11 → USB_SPEED_FULL
 *    HCD_USB2  → USB_SPEED_HIGH
 *    HCD_USB3  → USB_SPEED_SUPER
 *    HCD_USB32 → USB_SPEED_SUPER_PLUS (2x2 通道)
 *    HCD_USB31 → USB_SPEED_SUPER_PLUS (2x1 通道)
 *
 * 7. 唤醒能力设置:
 *    默认所有根 HUB 都支持远程唤醒 (驱动可在 reset 中覆盖)
 *
 * 8. HC 硬件初始化:
 *    hc_driver->reset(): 复位并初始化硬件
 *    usb_phy_roothub_calibrate(): 校准 PHY
 *
 * 9. BH (bottom half) 初始化:
 *    init_giveback_urb_bh() 初始化高/低优先级 giveback 工作队列
 *
 * 10. 中断注册:
 *     usb_hcd_request_irqs() 注册中断处理函数
 *
 * 11. HC 启动:
 *     hc_driver->start(): 启动控制器硬件
 *
 * 12. 根 HUB 注册:
 *     register_root_hub(): 将根 HUB 注册为 USB 设备
 *     (如果 HCD_DEFER_RH_REGISTER 被设置, 根 HUB 注册延迟)
 *
 * ============================== 错误路径 ==============================
 *
 * 任何步骤失败都会跳转到对应的 err_ 标签, 执行逆向清理:
 *   err_register_root_hub  → usb_stop_hcd + free_irq
 *   err_hcd_driver_start   → free_irq
 *   ... (逐级回退)
 *
 * 参数:
 *   @hcd:      已通过 __usb_create_hcd 创建的 HCD 结构
 *   @irqnum:  中断号
 *   @irqflags:中断标志 (如 IRQF_SHARED)
 *
 * 返回: 0 表示成功, 负值表示错误码.
 */
/**
 * usb_add_hcd - finish generic HCD structure initialization and register
 * @hcd: the usb_hcd structure to initialize
 * @irqnum: Interrupt line to allocate
 * @irqflags: Interrupt type flags
 *
 * Finish the remaining parts of generic HCD initialization: allocate the
 * buffers of consistent memory, register the bus, request the IRQ line,
 * and call the driver's reset() and start() routines.
 */
int usb_add_hcd(struct usb_hcd *hcd,
		unsigned int irqnum, unsigned long irqflags)
{
	int retval;
	struct usb_device *rhdev;
	struct usb_hcd *shared_hcd;
	int skip_phy_initialization;

	if (usb_hcd_is_primary_hcd(hcd))
		skip_phy_initialization = hcd->skip_phy_initialization;
	else
		skip_phy_initialization = hcd->primary_hcd->skip_phy_initialization;

	if (!skip_phy_initialization) {
		if (usb_hcd_is_primary_hcd(hcd)) {
			hcd->phy_roothub = usb_phy_roothub_alloc(hcd->self.sysdev);
			if (IS_ERR(hcd->phy_roothub))
				return PTR_ERR(hcd->phy_roothub);
		} else {
			hcd->phy_roothub = usb_phy_roothub_alloc_usb3_phy(hcd->self.sysdev);
			if (IS_ERR(hcd->phy_roothub))
				return PTR_ERR(hcd->phy_roothub);
		}

		retval = usb_phy_roothub_init(hcd->phy_roothub);
		if (retval)
			return retval;

		retval = usb_phy_roothub_set_mode(hcd->phy_roothub,
						  PHY_MODE_USB_HOST_SS);
		if (retval)
			retval = usb_phy_roothub_set_mode(hcd->phy_roothub,
							  PHY_MODE_USB_HOST);
		if (retval)
			goto err_usb_phy_roothub_power_on;

		retval = usb_phy_roothub_power_on(hcd->phy_roothub);
		if (retval)
			goto err_usb_phy_roothub_power_on;
	}

	dev_info(hcd->self.controller, "%s\n", hcd->product_desc);

	switch (authorized_default) {
	case USB_AUTHORIZE_NONE:
		hcd->dev_policy = USB_DEVICE_AUTHORIZE_NONE;
		break;

	case USB_AUTHORIZE_INTERNAL:
		hcd->dev_policy = USB_DEVICE_AUTHORIZE_INTERNAL;
		break;

	case USB_AUTHORIZE_ALL:
	case USB_AUTHORIZE_WIRED:
	default:
		hcd->dev_policy = USB_DEVICE_AUTHORIZE_ALL;
		break;
	}

	set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);

	/* per default all interfaces are authorized */
	set_bit(HCD_FLAG_INTF_AUTHORIZED, &hcd->flags);

	/* HC is in reset state, but accessible.  Now do the one-time init,
	 * bottom up so that hcds can customize the root hubs before hub_wq
	 * starts talking to them.  (Note, bus id is assigned early too.)
	 */
	retval = hcd_buffer_create(hcd);
	if (retval != 0) {
		dev_dbg(hcd->self.sysdev, "pool alloc failed\n");
		goto err_create_buf;
	}

	retval = usb_register_bus(&hcd->self);
	if (retval < 0)
		goto err_register_bus;

	rhdev = usb_alloc_dev(NULL, &hcd->self, 0);
	if (rhdev == NULL) {
		dev_err(hcd->self.sysdev, "unable to allocate root hub\n");
		retval = -ENOMEM;
		goto err_allocate_root_hub;
	}
	mutex_lock(&usb_port_peer_mutex);
	hcd->self.root_hub = rhdev;
	mutex_unlock(&usb_port_peer_mutex);

	rhdev->rx_lanes = 1;
	rhdev->tx_lanes = 1;
	rhdev->ssp_rate = USB_SSP_GEN_UNKNOWN;

	switch (hcd->speed) {
	case HCD_USB11:
		rhdev->speed = USB_SPEED_FULL;
		break;
	case HCD_USB2:
		rhdev->speed = USB_SPEED_HIGH;
		break;
	case HCD_USB3:
		rhdev->speed = USB_SPEED_SUPER;
		break;
	case HCD_USB32:
		rhdev->rx_lanes = 2;
		rhdev->tx_lanes = 2;
		rhdev->ssp_rate = USB_SSP_GEN_2x2;
		rhdev->speed = USB_SPEED_SUPER_PLUS;
		break;
	case HCD_USB31:
		rhdev->ssp_rate = USB_SSP_GEN_2x1;
		rhdev->speed = USB_SPEED_SUPER_PLUS;
		break;
	default:
		retval = -EINVAL;
		goto err_set_rh_speed;
	}

	/* wakeup flag init defaults to "everything works" for root hubs,
	 * but drivers can override it in reset() if needed, along with
	 * recording the overall controller's system wakeup capability.
	 */
	device_set_wakeup_capable(&rhdev->dev, 1);

	/* HCD_FLAG_RH_RUNNING doesn't matter until the root hub is
	 * registered.  But since the controller can die at any time,
	 * let's initialize the flag before touching the hardware.
	 */
	set_bit(HCD_FLAG_RH_RUNNING, &hcd->flags);

	/* "reset" is misnamed; its role is now one-time init. the controller
	 * should already have been reset (and boot firmware kicked off etc).
	 */
	if (hcd->driver->reset) {
		retval = hcd->driver->reset(hcd);
		if (retval < 0) {
			dev_err(hcd->self.controller, "can't setup: %d\n",
					retval);
			goto err_hcd_driver_setup;
		}
	}
	hcd->rh_pollable = 1;

	retval = usb_phy_roothub_calibrate(hcd->phy_roothub);
	if (retval)
		goto err_hcd_driver_setup;

	/* NOTE: root hub and controller capabilities may not be the same */
	if (device_can_wakeup(hcd->self.controller)
			&& device_can_wakeup(&hcd->self.root_hub->dev))
		dev_dbg(hcd->self.controller, "supports USB remote wakeup\n");

	/* initialize BHs */
	init_giveback_urb_bh(&hcd->high_prio_bh);
	hcd->high_prio_bh.high_prio = true;
	init_giveback_urb_bh(&hcd->low_prio_bh);

	/* enable irqs just before we start the controller,
	 * if the BIOS provides legacy PCI irqs.
	 */
	if (usb_hcd_is_primary_hcd(hcd) && irqnum) {
		retval = usb_hcd_request_irqs(hcd, irqnum, irqflags);
		if (retval)
			goto err_request_irq;
	}

	hcd->state = HC_STATE_RUNNING;
	retval = hcd->driver->start(hcd);
	if (retval < 0) {
		dev_err(hcd->self.controller, "startup error %d\n", retval);
		goto err_hcd_driver_start;
	}

	/* starting here, usbcore will pay attention to the shared HCD roothub */
	shared_hcd = hcd->shared_hcd;
	if (!usb_hcd_is_primary_hcd(hcd) && shared_hcd && HCD_DEFER_RH_REGISTER(shared_hcd)) {
		retval = register_root_hub(shared_hcd);
		if (retval != 0)
			goto err_register_root_hub;

		if (shared_hcd->uses_new_polling && HCD_POLL_RH(shared_hcd))
			usb_hcd_poll_rh_status(shared_hcd);
	}

	/* starting here, usbcore will pay attention to this root hub */
	if (!HCD_DEFER_RH_REGISTER(hcd)) {
		retval = register_root_hub(hcd);
		if (retval != 0)
			goto err_register_root_hub;

		if (hcd->uses_new_polling && HCD_POLL_RH(hcd))
			usb_hcd_poll_rh_status(hcd);
	}

	return retval;

err_register_root_hub:
	usb_stop_hcd(hcd);
err_hcd_driver_start:
	if (usb_hcd_is_primary_hcd(hcd) && hcd->irq > 0)
		free_irq(irqnum, hcd);
err_request_irq:
err_hcd_driver_setup:
err_set_rh_speed:
	usb_put_invalidate_rhdev(hcd);
err_allocate_root_hub:
	usb_deregister_bus(&hcd->self);
err_register_bus:
	hcd_buffer_destroy(hcd);
err_create_buf:
	usb_phy_roothub_power_off(hcd->phy_roothub);
err_usb_phy_roothub_power_on:
	usb_phy_roothub_exit(hcd->phy_roothub);

	return retval;
}
EXPORT_SYMBOL_GPL(usb_add_hcd);

/*
 * usb_remove_hcd -- 移除并注销 USB 主机控制器
 *
 * 这是 usb_add_hcd() 的逆操作, 在 HC 驱动卸载或系统关闭时调用。
 *
 * ============================== 注销流程 ==============================
 *
 * 1. 停止新的 URB 提交:
 *    清除 RH_RUNNING 标志, 设置状态为 QUIESCING
 *
 * 2. 断开根 HUB:
 *    在 hcd_root_hub_lock 保护下检查 rh_registered 并清零,
 *    然后通过 usb_disconnect() 断开根 HUB (这会触发 hub_wq 清理所有子设备)
 *
 * 3. 取消待处理的工作:
 *    cancel_work_sync(wakeup_work) 和 cancel_work_sync(died_work)
 *
 * 4. 停止 HC 硬件:
 *    usb_stop_hcd() 停止定时器并调用 hc_driver->stop()
 *
 * 5. 释放中断:
 *    仅 primary HCD 释放 IRQ
 *
 * 6. 注销总线:
 *    usb_deregister_bus() 回收总线编号
 *
 * 7. 销毁 DMA 缓冲区:
 *    hcd_buffer_destroy()
 *
 * 8. 关闭并退出 PHY:
 *    usb_phy_roothub_power_off() + usb_phy_roothub_exit()
 *
 * 9. 释放根 HUB:
 *    usb_put_invalidate_rhdev() 释放根 HUB 引用并清除指针
 *
 * 注意: 此函数可能睡眠, 必须在进程上下文中调用。
 * flush_work() 不需要在此处调用, 因为 usb_disconnect() 中的
 * disconnect() 回调应确保所有 URB 已完成.
 */
/**
 * usb_remove_hcd - shutdown processing for generic HCDs
 * @hcd: the usb_hcd structure to remove
 *
 * Context: task context, might sleep.
 *
 * Disconnects the root hub, then reverses the effects of usb_add_hcd(),
 * invoking the HCD's stop() method.
 */
void usb_remove_hcd(struct usb_hcd *hcd)
{
	struct usb_device *rhdev;
	bool rh_registered;

	if (!hcd) {
		pr_debug("%s: hcd is NULL\n", __func__);
		return;
	}
	rhdev = hcd->self.root_hub;

	dev_info(hcd->self.controller, "remove, state %x\n", hcd->state);

	usb_get_dev(rhdev);
	clear_bit(HCD_FLAG_RH_RUNNING, &hcd->flags);
	if (HC_IS_RUNNING (hcd->state))
		hcd->state = HC_STATE_QUIESCING;

	dev_dbg(hcd->self.controller, "roothub graceful disconnect\n");
	spin_lock_irq (&hcd_root_hub_lock);
	rh_registered = hcd->rh_registered;
	hcd->rh_registered = 0;
	spin_unlock_irq (&hcd_root_hub_lock);

#ifdef CONFIG_PM
	cancel_work_sync(&hcd->wakeup_work);
#endif
	cancel_work_sync(&hcd->died_work);

	mutex_lock(&usb_bus_idr_lock);
	if (rh_registered)
		usb_disconnect(&rhdev);		/* Sets rhdev to NULL */
	mutex_unlock(&usb_bus_idr_lock);

	/*
	 * flush_work() isn't needed here because:
	 * - driver's disconnect() called from usb_disconnect() should
	 *   make sure its URBs are completed during the disconnect()
	 *   callback
	 *
	 * - it is too late to run complete() here since driver may have
	 *   been removed already now
	 */

	/* Prevent any more root-hub status calls from the timer.
	 * The HCD might still restart the timer (if a port status change
	 * interrupt occurs), but usb_hcd_poll_rh_status() won't invoke
	 * the hub_status_data() callback.
	 */
	usb_stop_hcd(hcd);

	if (usb_hcd_is_primary_hcd(hcd)) {
		if (hcd->irq > 0)
			free_irq(hcd->irq, hcd);
	}

	usb_deregister_bus(&hcd->self);
	hcd_buffer_destroy(hcd);

	usb_phy_roothub_power_off(hcd->phy_roothub);
	usb_phy_roothub_exit(hcd->phy_roothub);

	usb_put_invalidate_rhdev(hcd);
	hcd->flags = 0;
}
EXPORT_SYMBOL_GPL(usb_remove_hcd);

void
usb_hcd_platform_shutdown(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);

	/* No need for pm_runtime_put(), we're shutting down */
	pm_runtime_get_sync(&dev->dev);

	if (hcd->driver->shutdown)
		hcd->driver->shutdown(hcd);
}
EXPORT_SYMBOL_GPL(usb_hcd_platform_shutdown);

/*
 * usb_hcd_setup_local_mem -- 为 DMA 能力受限的 HCD 设置本地内存池
 *
 * 为那些只能访问有限 SRAM 或特殊地址空间的主机控制器创建本地内存池。
 * 使用 genalloc (通用内存分配器) 管理.
 *
 * 内存来源:
 *   1. 物理 SRAM 地址 (phys_addr != 0):
 *      通过 devm_memremap() 将 SRAM 映射到内核虚拟地址空间
 *   2. 系统内存 (phys_addr == 0):
 *      通过 dmam_alloc_attrs() 分配 DMA 一致性内存
 *
 * 注意: 对于 SRAM 情况, gen_pool_add_virt 的 phys_addr 参数实际上
 * 接收 dma_addr_t (DMA 地址), 而不是真正的物理地址.
 * 这是因为 SRAM 不是系统内存, 没有内核映射.
 *
 * 参数:
 *   @hcd:       主机控制器
 *   @phys_addr: 物理 SRAM 基址 (0 表示分配系统内存)
 *   @dma:       DMA 地址 (用于系统内存分配时输出)
 *   @size:      内存池大小
 *
 * 返回: 0 表示成功, 负值表示错误.
 */
int usb_hcd_setup_local_mem(struct usb_hcd *hcd, phys_addr_t phys_addr,
			    dma_addr_t dma, size_t size)
{
	int err;
	void *local_mem;

	hcd->localmem_pool = devm_gen_pool_create(hcd->self.sysdev, 4,
						  dev_to_node(hcd->self.sysdev),
						  dev_name(hcd->self.sysdev));
	if (IS_ERR(hcd->localmem_pool))
		return PTR_ERR(hcd->localmem_pool);

	/*
	 * if a physical SRAM address was passed, map it, otherwise
	 * allocate system memory as a buffer.
	 */
	if (phys_addr)
		local_mem = devm_memremap(hcd->self.sysdev, phys_addr,
					  size, MEMREMAP_WC);
	else
		local_mem = dmam_alloc_attrs(hcd->self.sysdev, size, &dma,
					     GFP_KERNEL,
					     DMA_ATTR_WRITE_COMBINE);

	if (IS_ERR_OR_NULL(local_mem)) {
		if (!local_mem)
			return -ENOMEM;

		return PTR_ERR(local_mem);
	}

	/*
	 * Here we pass a dma_addr_t but the arg type is a phys_addr_t.
	 * It's not backed by system memory and thus there's no kernel mapping
	 * for it.
	 */
	err = gen_pool_add_virt(hcd->localmem_pool, (unsigned long)local_mem,
				dma, size, dev_to_node(hcd->self.sysdev));
	if (err < 0) {
		dev_err(hcd->self.sysdev, "gen_pool_add_virt failed with %d\n",
			err);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_hcd_setup_local_mem);

/*-------------------------------------------------------------------------*/

#if IS_ENABLED(CONFIG_USB_MON)

/*
 * usbmon (USB Monitor) 支持 -- USB 流量监控/抓包接口
 *
 * usbmon 允许用户空间通过 debugfs 监控 USB 总线上的所有 URB 流量,
 * 类似于网络抓包工具 tcpdump 对 USB 的作用.
 *
 * mon_ops 是一个全局函数指针表, 指向 usbmon 模块注册的操作集。
 * 注册时使用 mb() 内存屏障确保 mon_ops 的写入对其他 CPU 可见.
 *
 * 使用无锁注册的原因是避免在热路径 (URB 提交/完成) 中增加锁开销。
 * 潜在的风险 (mon_ops 可能在初始化/反初始化过程中被读取) 是
 * 可接受的, 因为 usbmon 模块引用了 usbcore 的符号, 所以 usbcore
 * 不能在 usbmon 之前被卸载.
 *
 * 这两个函数由 usbmon 模块在 init/exit 时调用.
 */

const struct usb_mon_operations *mon_ops;

/*
 * The registration is unlocked.
 * We do it this way because we do not want to lock in hot paths.
 *
 * Notice that the code is minimally error-proof. Because usbmon needs
 * symbols from usbcore, usbcore gets referenced and cannot be unloaded first.
 */

int usb_mon_register(const struct usb_mon_operations *ops)
{

	if (mon_ops)
		return -EBUSY;

	mon_ops = ops;
	mb();
	return 0;
}
EXPORT_SYMBOL_GPL (usb_mon_register);

void usb_mon_deregister (void)
{

	if (mon_ops == NULL) {
		printk(KERN_ERR "USB: monitor was not registered\n");
		return;
	}
	mon_ops = NULL;
	mb();
}
EXPORT_SYMBOL_GPL (usb_mon_deregister);

#endif /* CONFIG_USB_MON || CONFIG_USB_MON_MODULE */
