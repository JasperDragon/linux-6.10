/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usb hub driver head file
 *
 * Copyright (C) 1999 Linus Torvalds
 * Copyright (C) 1999 Johannes Erdfelt
 * Copyright (C) 1999 Gregory P. Smith
 * Copyright (C) 2001 Brad Hards (bhards@bigpond.net.au)
 * Copyright (C) 2012 Intel Corp (tianyu.lan@intel.com)
 *
 *  move struct usb_hub to this file.
 */

#include <linux/usb.h>
#include <linux/usb/ch11.h>
#include <linux/usb/hcd.h>
#include <linux/usb/typec.h>
#include "usb.h"

/*
 * struct usb_hub - USB Hub 实例的运行时数据结构
 *
 * 每个 USB Hub 对应一个 usb_hub 实例, 由 hub 接口驱动在 probe 时创建.
 * 该结构管理 hub 的所有运行时状态, 包括:
 *   - 端口事件位图 (event/change/removed/wakeup/power)
 *   - 中断轮询 URB 及其状态缓冲区
 *   - 事务转换器 (TT, 仅 USB2 HUB)
 *   - 工作队列与电源管理状态
 */
struct usb_hub {
	struct device		*intfdev;	/* HUB 接口设备 (struct usb_interface) */
	struct usb_device	*hdev;		/* 指向本 HUB 的 USB 设备结构 */
	struct kref		kref;		/* 引用计数, 用于安全释放 */
	struct urb		*urb;		/* 中断轮询 URB: 获取端口状态变化 */

	/* 中断 URB 数据缓冲区, 预留额外空间处理 babble 条件 */
	u8			(*buffer)[8];
	union {
		struct usb_hub_status	hub;
		struct usb_port_status	port;
	}			*status;	/* 状态报告缓冲区, 被 status_mutex 保护 */
	struct mutex		status_mutex;	/* 保护 status 缓冲区的互斥锁 */

	int			error;		/* 上一次中断传输的错误码 */
	int			nerrors;	/* 连续错误次数, 累计 10 次触发 HUB 复位 */

	unsigned long		event_bits[1];	/* 中断 URB 上报的事件位图, bit0=hub自身, bitN=端口N */
	unsigned long		change_bits[1];	/* 端口逻辑连接状态变化位图, 由 hub_port_connect_change() 清除 */
	unsigned long		removed_bits[1]; /* 端口设备被逻辑移除的位图 (如复位失败时设置) */
	unsigned long		wakeup_bits[1];	/* 端口收到远程唤醒信号的位图 */
	unsigned long		power_bits[1]; /* 端口供电状态位图 (哪些端口应该保持供电) */
	unsigned long		child_usage_bits[1]; /* 子设备正在使用的端口, 阻止父设备 port PM 关闭 */
	unsigned long		warm_reset_bits[1]; /* 需要执行 warm reset (BH reset) 恢复的端口 */
#if USB_MAXCHILDREN > 31 /* 8*sizeof(unsigned long) - 1 */
#error event_bits[] is too short!
#endif

	struct usb_hub_descriptor *descriptor;	/* HUB 类描述符: 端口特性, 电源参数等 */
	struct usb_tt		tt;		/* 事务转换器 (仅 USB2 HUB): 支持低速/全速通过高速链路通信 */

	unsigned		mA_per_port;	/* 每个端口可用的最大电流 (mA), 由电源预算决定 */
#ifdef	CONFIG_PM
	unsigned		wakeup_enabled_descendants; /* 使能唤醒的后代设备数量 */
#endif

	unsigned		limited_power:1;	/* HUB 是否处于供电受限状态 (过流/电源不足) */
	unsigned		quiescing:1;		/* HUB 正在暂停/停止中, 拒绝新事件 */
	unsigned		disconnected:1;		/* HUB 已被物理断开 */
	unsigned		in_reset:1;		/* HUB 正在执行复位中 */
	unsigned		quirk_disable_autosuspend:1; /* 由于 quirk 禁止运行时自动挂起 */

	unsigned		quirk_check_port_auto_suspend:1; /* 自动挂起前需检查端口状态变化 */

	unsigned		has_indicators:1;	/* HUB 是否有端口 LED 指示灯 */
	u8			indicator[USB_MAXCHILDREN]; /* 每个端口 LED 的状态 */
	struct delayed_work	leds;		/* LED 闪烁定时器工作 */
	struct delayed_work	init_work;	/* HUB 初始化第二阶段 (HUB_INIT2) */
	struct delayed_work	post_resume_work; /* 恢复后的延迟工作 (HUB_RESUME) */
	struct work_struct      events;		/* hub_event() 工作队列项, 由 kick_hub_wq() 调度 */
	spinlock_t		irq_urb_lock;	/* 保护中断 URB 重提交的自旋锁 */
	struct timer_list	irq_urb_retry;	/* 中断 URB 提交失败后的重试定时器 */
	struct usb_port		**ports;	/* 端口数组, 索引 0 对应 port1 */
	struct list_head        onboard_devs;	/* 板载设备链表 */
};

/**
 * struct usb_port - kernel's representation of a usb port
 * @child: usb device attached to the port
 * @dev: generic device interface
 * @port_owner: port's owner
 * @peer: related usb2 and usb3 ports (share the same connector)
 * @connector: USB Type-C connector
 * @req: default pm qos request for hubs without port power control
 * @connect_type: port's connect type
 * @state: device state of the usb device attached to the port
 * @state_kn: kernfs_node of the sysfs attribute that accesses @state
 * @location: opaque representation of platform connector location
 * @status_lock: synchronize port_event() vs usb_port_{suspend|resume}
 * @portnum: port index num based one
 * @is_superspeed cache super-speed status
 * @usb3_lpm_u1_permit: whether USB3 U1 LPM is permitted.
 * @usb3_lpm_u2_permit: whether USB3 U2 LPM is permitted.
 * @early_stop: whether port initialization will be stopped earlier.
 * @ignore_event: whether events of the port are ignored.
 */
struct usb_port {
	struct usb_device *child;	/* 连接到该端口的 USB 子设备 */
	struct device dev;		/* 端口设备模型接口, 用于 sysfs 与电源管理 */
	struct usb_dev_state *port_owner; /* 端口所有者 (由 usbfs 管理) */
	struct usb_port *peer;		/* 配对的 USB2/USB3 端口 (共享同一物理连接器) */
	struct typec_connector *connector; /* USB Type-C 连接器 */
	struct dev_pm_qos_request *req; /* 默认 PM QoS 请求 (端口无电源控制时使用) */
	enum usb_port_connect_type connect_type; /* 端口连接类型: 热插拔/硬连线等 */
	enum usb_device_state state;	/* 端口上设备的连接状态 */
	struct kernfs_node *state_kn;	/* sysfs state 属性的 kernfs 节点 */
	usb_port_location_t location;	/* 平台连接器位置信息 (ACPI 等) */
	struct mutex status_lock;	/* 同步 port_event() 与 usb_port_suspend/resume */
	u32 over_current_count;		/* 过流事件计数, 用于通知用户空间 */
	u8 portnum;			/* 端口号 (基于 1) */
	u32 quirks;			/* 端口级 quirk 标志 */
	unsigned int early_stop:1;	/* 是否提前停止端口枚举 */
	unsigned int ignore_event:1;	/* 是否忽略端口事件 */
	unsigned int is_superspeed:1;	/* 标识 SuperSpeed 端口 */
	unsigned int usb3_lpm_u1_permit:1; /* 是否允许 USB3 U1 LPM */
	unsigned int usb3_lpm_u2_permit:1; /* 是否允许 USB3 U2 LPM */
};

#define to_usb_port(_dev) \
	container_of(_dev, struct usb_port, dev)

extern int usb_hub_create_port_device(struct usb_hub *hub,
		int port1);
extern void usb_hub_remove_port_device(struct usb_hub *hub,
		int port1);
extern int usb_hub_set_port_power(struct usb_device *hdev, struct usb_hub *hub,
		int port1, bool set);
extern struct usb_hub *usb_hub_to_struct_hub(struct usb_device *hdev);
extern void hub_get(struct usb_hub *hub);
extern void hub_put(struct usb_hub *hub);
extern int hub_port_debounce(struct usb_hub *hub, int port1,
		bool must_be_connected);
extern int usb_clear_port_feature(struct usb_device *hdev,
		int port1, int feature);
extern int usb_hub_port_status(struct usb_hub *hub, int port1,
		u16 *status, u16 *change);
extern int usb_port_is_power_on(struct usb_hub *hub, unsigned int portstatus);

static inline bool hub_is_port_power_switchable(struct usb_hub *hub)
{
	__le16 hcs;

	if (!hub)
		return false;
	hcs = hub->descriptor->wHubCharacteristics;
	return (le16_to_cpu(hcs) & HUB_CHAR_LPSM) < HUB_CHAR_NO_LPSM;
}

static inline int hub_is_superspeed(struct usb_device *hdev)
{
	return hdev->descriptor.bDeviceProtocol == USB_HUB_PR_SS;
}

static inline int hub_is_superspeedplus(struct usb_device *hdev)
{
	return (hdev->descriptor.bDeviceProtocol == USB_HUB_PR_SS &&
		le16_to_cpu(hdev->descriptor.bcdUSB) >= 0x0310 &&
		hdev->bos && hdev->bos->ssp_cap);
}

static inline unsigned hub_power_on_good_delay(struct usb_hub *hub)
{
	unsigned delay = hub->descriptor->bPwrOn2PwrGood * 2;

	if (!hub->hdev->parent)	/* root hub */
		return delay;
	else /* Wait at least 100 msec for power to become stable */
		return max(delay, 100U);
}

static inline int hub_port_debounce_be_connected(struct usb_hub *hub,
		int port1)
{
	return hub_port_debounce(hub, port1, true);
}

static inline int hub_port_debounce_be_stable(struct usb_hub *hub,
		int port1)
{
	return hub_port_debounce(hub, port1, false);
}
