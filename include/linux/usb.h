/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_USB_H
#define __LINUX_USB_H

/*
 * ============================================================================
 * usb.h — Linux USB 子系统核心头文件（Host 端）
 * ============================================================================
 *
 * 【本文件在 USB 架构中的位置】
 *   这是 USB 子系统最重要的单个头文件。每个 USB 设备驱动都会 #include <linux/usb.h>。
 *   它定义了 USB 驱动开发所需的全部核心数据结构、API 声明和宏。
 *
 * 【USB 设备层次结构（描述符树）】
 *
 *   一个 USB 设备的描述符按层次组织：
 *
 *   设备 (usb_device)
 *   │
 *   ├── 设备描述符 (usb_device_descriptor)
 *   │   bLength=18, bDescriptorType=DEVICE(1)
 *   │   字段: idVendor, idProduct, bcdUSB, bMaxPacketSize0,
 *   │         bDeviceClass, bNumConfigurations, ...
 *   │
 *   ├── 配置 1..N (usb_host_config)
 *   │   ├── 配置描述符 (usb_config_descriptor)
 *   │   │   bLength=9, bDescriptorType=CONFIG(2)
 *   │   │   bConfigurationValue, bmAttributes, bMaxPower
 *   │   │
 *   │   ├── 接口 0..N (usb_host_interface)
 *   │   │   ├── 接口描述符 (usb_interface_descriptor)
 *   │   │   │   bLength=9, bDescriptorType=INTERFACE(4)
 *   │   │   │   bInterfaceNumber, bAlternateSetting, bNumEndpoints,
 *   │   │   │   bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol
 *   │   │   │   (这三个是驱动匹配的关键!)
 *   │   │   │
 *   │   │   └── 端点 1..N (usb_host_endpoint)
 *   │   │       ├── 端点描述符 (usb_endpoint_descriptor)
 *   │   │       │   bLength=7, bDescriptorType=ENDPOINT(5)
 *   │   │       │   bEndpointAddress(bit7=方向:0=OUT/1=IN, bits0-3=端点号),
 *   │   │       │   bmAttributes(bits0-1=传输类型:控制0/同步1/批量2/中断3),
 *   │   │       │   wMaxPacketSize, bInterval
 *   │   │       └── USB3: ep_companion 描述符
 *   │   │
 *   │   └── (可选)HID/音频/视频/厂商特定描述符
 *   │
 *   └── 字符串描述符 (通过 usb_get_string() 获取)
 *       iManufacturer, iProduct, iSerialNumber
 *
 * 【API 类别速查】
 *
 *   ┌────────────────┬──────────────────────────────────────────┐
 *   │ 类别            │ 函数                                     │
 *   ├────────────────┼──────────────────────────────────────────┤
 *   │ 控制传输        │ usb_control_msg(), usb_control_msg_send()│
 *   │ 批量传输        │ usb_bulk_msg()                          │
 *   │ URB 提交        │ usb_submit_urb(), usb_fill_*_urb()      │
 *   │ 设备发现        │ usb_register_driver(), usb_driver.probe │
 *   │ 描述符解析      │ usb_get_descriptor(), usb_get_string()  │
 *   │ 端点查找        │ usb_find_bulk_in_endpoint() 等          │
 *   │ 电源管理        │ usb_autopm_*, usb_autosuspend()        │
 *   │ 复位/配置       │ usb_reset_device(), usb_set_configuration│
 *   └────────────────┴──────────────────────────────────────────┘
 *
 * 【与 Gadget 端的关系】
 *   本文件定义 HOST 端 API。Gadget 端（设备模式）使用
 *   include/linux/usb/gadget.h（不同的 struct 和 API）。
 */

#include <linux/mod_devicetable.h>
#include <linux/usb/ch9.h>

#define USB_MAJOR			180
#define USB_DEVICE_MAJOR		189


#ifdef __KERNEL__

#include <linux/errno.h>        /* for -ENODEV */
#include <linux/delay.h>	/* for mdelay() */
#include <linux/interrupt.h>	/* for in_interrupt() */
#include <linux/list.h>		/* for struct list_head */
#include <linux/kref.h>		/* for struct kref */
#include <linux/device.h>	/* for struct device */
#include <linux/fs.h>		/* for struct file_operations */
#include <linux/completion.h>	/* for struct completion */
#include <linux/sched.h>	/* for current && schedule_timeout */
#include <linux/mutex.h>	/* for struct mutex */
#include <linux/spinlock.h>	/* for spinlock_t */
#include <linux/pm_runtime.h>	/* for runtime PM */

struct usb_device;
struct usb_driver;

/*-------------------------------------------------------------------------*/

/*
 * Host-side wrappers for standard USB descriptors ... these are parsed
 * from the data provided by devices.  Parsing turns them from a flat
 * sequence of descriptors into a hierarchy:
 *
 *  - devices have one (usually) or more configs;
 *  - configs have one (often) or more interfaces;
 *  - interfaces have one (usually) or more settings;
 *  - each interface setting has zero or (usually) more endpoints.
 *  - a SuperSpeed endpoint has a companion descriptor
 *
 * And there might be other descriptors mixed in with those.
 *
 * Devices may also have class-specific or vendor-specific descriptors.
 */

struct ep_device;

/**
 * struct usb_host_endpoint - host-side endpoint descriptor and queue
 * @desc: descriptor for this endpoint, wMaxPacketSize in native byteorder
 * @ss_ep_comp: SuperSpeed companion descriptor for this endpoint
 * @ssp_isoc_ep_comp: SuperSpeedPlus isoc companion descriptor for this endpoint
 * @eusb2_isoc_ep_comp: eUSB2 isoc companion descriptor for this endpoint
 * @urb_list: urbs queued to this endpoint; maintained by usbcore
 * @hcpriv: for use by HCD; typically holds hardware dma queue head (QH)
 *	with one or more transfer descriptors (TDs) per urb; must be preserved
 *	by core while BW is allocated for the endpoint
 * @ep_dev: ep_device for sysfs info
 * @extra: descriptors following this endpoint in the configuration
 * @extralen: how many bytes of "extra" are valid
 * @enabled: URBs may be submitted to this endpoint
 * @streams: number of USB-3 streams allocated on the endpoint
 *
 * USB requests are always queued to a given endpoint, identified by a
 * descriptor within an active interface in a given USB configuration.
 */
struct usb_host_endpoint {
	struct usb_endpoint_descriptor			desc;
	struct usb_ss_ep_comp_descriptor		ss_ep_comp;
	struct usb_ssp_isoc_ep_comp_descriptor		ssp_isoc_ep_comp;
	struct usb_eusb2_isoc_ep_comp_descriptor	eusb2_isoc_ep_comp;
	struct list_head		urb_list;
	void				*hcpriv;
	struct ep_device		*ep_dev;	/* For sysfs info */

	unsigned char *extra;   /* Extra descriptors */
	int extralen;
	int enabled;
	int streams;
};

/* host-side wrapper for one interface setting's parsed descriptors */
struct usb_host_interface {
	struct usb_interface_descriptor	desc;

	int extralen;
	unsigned char *extra;   /* Extra descriptors */

	/* array of desc.bNumEndpoints endpoints associated with this
	 * interface setting.  these will be in no particular order.
	 */
	struct usb_host_endpoint *endpoint;

	char *string;		/* iInterface string, if present */
};

enum usb_interface_condition {
	USB_INTERFACE_UNBOUND = 0,
	USB_INTERFACE_BINDING,
	USB_INTERFACE_BOUND,
	USB_INTERFACE_UNBINDING,
};

int __must_check
usb_find_common_endpoints(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **bulk_in,
		struct usb_endpoint_descriptor **bulk_out,
		struct usb_endpoint_descriptor **int_in,
		struct usb_endpoint_descriptor **int_out);

int __must_check
usb_find_common_endpoints_reverse(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **bulk_in,
		struct usb_endpoint_descriptor **bulk_out,
		struct usb_endpoint_descriptor **int_in,
		struct usb_endpoint_descriptor **int_out);

static inline int __must_check
usb_find_bulk_in_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **bulk_in)
{
	return usb_find_common_endpoints(alt, bulk_in, NULL, NULL, NULL);
}

static inline int __must_check
usb_find_bulk_out_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **bulk_out)
{
	return usb_find_common_endpoints(alt, NULL, bulk_out, NULL, NULL);
}

static inline int __must_check
usb_find_int_in_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **int_in)
{
	return usb_find_common_endpoints(alt, NULL, NULL, int_in, NULL);
}

static inline int __must_check
usb_find_int_out_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **int_out)
{
	return usb_find_common_endpoints(alt, NULL, NULL, NULL, int_out);
}

static inline int __must_check
usb_find_last_bulk_in_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **bulk_in)
{
	return usb_find_common_endpoints_reverse(alt, bulk_in, NULL, NULL, NULL);
}

static inline int __must_check
usb_find_last_bulk_out_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **bulk_out)
{
	return usb_find_common_endpoints_reverse(alt, NULL, bulk_out, NULL, NULL);
}

static inline int __must_check
usb_find_last_int_in_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **int_in)
{
	return usb_find_common_endpoints_reverse(alt, NULL, NULL, int_in, NULL);
}

static inline int __must_check
usb_find_last_int_out_endpoint(struct usb_host_interface *alt,
		struct usb_endpoint_descriptor **int_out)
{
	return usb_find_common_endpoints_reverse(alt, NULL, NULL, NULL, int_out);
}

enum usb_wireless_status {
	USB_WIRELESS_STATUS_NA = 0,
	USB_WIRELESS_STATUS_DISCONNECTED,
	USB_WIRELESS_STATUS_CONNECTED,
};

/**
 * struct usb_interface - what usb device drivers talk to
 * @altsetting: array of interface structures, one for each alternate
 *	setting that may be selected.  Each one includes a set of
 *	endpoint configurations.  They will be in no particular order.
 * @cur_altsetting: the current altsetting.
 * @num_altsetting: number of altsettings defined.
 * @intf_assoc: interface association descriptor
 * @minor: the minor number assigned to this interface, if this
 *	interface is bound to a driver that uses the USB major number.
 *	If this interface does not use the USB major, this field should
 *	be unused.  The driver should set this value in the probe()
 *	function of the driver, after it has been assigned a minor
 *	number from the USB core by calling usb_register_dev().
 * @condition: binding state of the interface: not bound, binding
 *	(in probe()), bound to a driver, or unbinding (in disconnect())
 * @sysfs_files_created: sysfs attributes exist
 * @ep_devs_created: endpoint child pseudo-devices exist
 * @unregistering: flag set when the interface is being unregistered
 * @needs_remote_wakeup: flag set when the driver requires remote-wakeup
 *	capability during autosuspend.
 * @needs_altsetting0: flag set when a set-interface request for altsetting 0
 *	has been deferred.
 * @needs_binding: flag set when the driver should be re-probed or unbound
 *	following a reset or suspend operation it doesn't support.
 * @authorized: This allows to (de)authorize individual interfaces instead
 *	a whole device in contrast to the device authorization.
 * @wireless_status: if the USB device uses a receiver/emitter combo, whether
 *	the emitter is connected.
 * @wireless_status_work: Used for scheduling wireless status changes
 *	from atomic context.
 * @dev: driver model's view of this device
 * @usb_dev: if an interface is bound to the USB major, this will point
 *	to the sysfs representation for that device.
 * @reset_ws: Used for scheduling resets from atomic context.
 * @resetting_device: USB core reset the device, so use alt setting 0 as
 *	current; needs bandwidth alloc after reset.
 *
 * USB device drivers attach to interfaces on a physical device.  Each
 * interface encapsulates a single high level function, such as feeding
 * an audio stream to a speaker or reporting a change in a volume control.
 * Many USB devices only have one interface.  The protocol used to talk to
 * an interface's endpoints can be defined in a usb "class" specification,
 * or by a product's vendor.  The (default) control endpoint is part of
 * every interface, but is never listed among the interface's descriptors.
 *
 * The driver that is bound to the interface can use standard driver model
 * calls such as dev_get_drvdata() on the dev member of this structure.
 *
 * Each interface may have alternate settings.  The initial configuration
 * of a device sets altsetting 0, but the device driver can change
 * that setting using usb_set_interface().  Alternate settings are often
 * used to control the use of periodic endpoints, such as by having
 * different endpoints use different amounts of reserved USB bandwidth.
 * All standards-conformant USB devices that use isochronous endpoints
 * will use them in non-default settings.
 *
 * The USB specification says that alternate setting numbers must run from
 * 0 to one less than the total number of alternate settings.  But some
 * devices manage to mess this up, and the structures aren't necessarily
 * stored in numerical order anyhow.  Use usb_altnum_to_altsetting() to
 * look up an alternate setting in the altsetting array based on its number.
 */
struct usb_interface {
	/* array of alternate settings for this interface,
	 * stored in no particular order */
	struct usb_host_interface *altsetting;

	struct usb_host_interface *cur_altsetting;	/* the currently
					 * active alternate setting */
	unsigned num_altsetting;	/* number of alternate settings */

	/* If there is an interface association descriptor then it will list
	 * the associated interfaces */
	struct usb_interface_assoc_descriptor *intf_assoc;

	int minor;			/* minor number this interface is
					 * bound to */
	enum usb_interface_condition condition;		/* state of binding */
	unsigned sysfs_files_created:1;	/* the sysfs attributes exist */
	unsigned ep_devs_created:1;	/* endpoint "devices" exist */
	unsigned unregistering:1;	/* unregistration is in progress */
	unsigned needs_remote_wakeup:1;	/* driver requires remote wakeup */
	unsigned needs_altsetting0:1;	/* switch to altsetting 0 is pending */
	unsigned needs_binding:1;	/* needs delayed unbind/rebind */
	unsigned resetting_device:1;	/* true: bandwidth alloc after reset */
	unsigned authorized:1;		/* used for interface authorization */
	enum usb_wireless_status wireless_status;
	struct work_struct wireless_status_work;

	struct device dev;		/* interface specific device info */
	struct device *usb_dev;
	struct work_struct reset_ws;	/* for resets in atomic context */
};

#define to_usb_interface(__dev)	container_of_const(__dev, struct usb_interface, dev)

/*
 * usb_get_intfdata / usb_set_intfdata — 获取/设置接口关联的驱动私有数据
 *
 * 这是 USB 驱动的标准模式:
 *   probe() 中分配私有 struct → usb_set_intfdata(intf, priv)
 *   后续通过 usb_get_intfdata(intf) 取回
 *
 * 底层使用 dev_get_drvdata/dev_set_drvdata，数据存储于 intf->dev.driver_data。
 */
static inline void *usb_get_intfdata(struct usb_interface *intf)
{
	return dev_get_drvdata(&intf->dev);
}

/**
 * usb_set_intfdata() - associate driver-specific data with an interface
 * @intf: USB interface
 * @data: driver data
 *
 * Drivers can use this function in their probe() callbacks to associate
 * driver-specific data with an interface.
 *
 * Note that there is generally no need to clear the driver-data pointer even
 * if some drivers do so for historical or implementation-specific reasons.
 */
static inline void usb_set_intfdata(struct usb_interface *intf, void *data)
{
	dev_set_drvdata(&intf->dev, data);
}

// usb_get_intf / usb_put_intf — 接口引用计数管理
// 获取引用防止接口在操作期间被释放。必须配对调用。
struct usb_interface *usb_get_intf(struct usb_interface *intf);
void usb_put_intf(struct usb_interface *intf);

// USB 规范硬限制
#define USB_MAXENDPOINTS	30     // 每个接口最多 30 个端点 (含 ep0)
/* this maximum is arbitrary */
#define USB_MAXINTERFACES	32     // 每个配置最多 32 个接口
#define USB_MAXIADS		(USB_MAXINTERFACES/2) // 最多 16 个 IAD (Interface Association Descriptor)

// usb_check_bulk_endpoints / usb_check_int_endpoints
// 验证接口是否包含指定的批量/中断端点集合, 常用于驱动 probe() 的完整性检查
bool usb_check_bulk_endpoints(
		const struct usb_interface *intf, const u8 *ep_addrs);
bool usb_check_int_endpoints(
		const struct usb_interface *intf, const u8 *ep_addrs);

/*
 * USB Resume Timer: Every Host controller driver should drive the resume
 * signalling on the bus for the amount of time defined by this macro.
 *
 * That way we will have a 'stable' behavior among all HCDs supported by Linux.
 *
 * Note that the USB Specification states we should drive resume for *at least*
 * 20 ms, but it doesn't give an upper bound. This creates two possible
 * situations which we want to avoid:
 *
 * (a) sometimes an msleep(20) might expire slightly before 20 ms, which causes
 * us to fail USB Electrical Tests, thus failing Certification
 *
 * (b) Some (many) devices actually need more than 20 ms of resume signalling,
 * and while we can argue that's against the USB Specification, we don't have
 * control over which devices a certification laboratory will be using for
 * certification. If CertLab uses a device which was tested against Windows and
 * that happens to have relaxed resume signalling rules, we might fall into
 * situations where we fail interoperability and electrical tests.
 *
 * In order to avoid both conditions, we're using a 40 ms resume timeout, which
 * should cope with both LPJ calibration errors and devices not following every
 * detail of the USB Specification.
 */
#define USB_RESUME_TIMEOUT	40 /* ms */

/**
 * struct usb_interface_cache - long-term representation of a device interface
 * @num_altsetting: number of altsettings defined.
 * @ref: reference counter.
 * @altsetting: variable-length array of interface structures, one for
 *	each alternate setting that may be selected.  Each one includes a
 *	set of endpoint configurations.  They will be in no particular order.
 *
 * These structures persist for the lifetime of a usb_device, unlike
 * struct usb_interface (which persists only as long as its configuration
 * is installed).  The altsetting arrays can be accessed through these
 * structures at any time, permitting comparison of configurations and
 * providing support for the /sys/kernel/debug/usb/devices pseudo-file.
 */
struct usb_interface_cache {
	unsigned num_altsetting;	/* number of alternate settings */
	struct kref ref;		/* reference counter */

	/* variable-length array of alternate settings for this interface,
	 * stored in no particular order */
	struct usb_host_interface altsetting[];
};
#define	ref_to_usb_interface_cache(r) \
		container_of(r, struct usb_interface_cache, ref)
#define	altsetting_to_usb_interface_cache(a) \
		container_of(a, struct usb_interface_cache, altsetting[0])

/**
 * struct usb_host_config - representation of a device's configuration
 * @desc: the device's configuration descriptor.
 * @string: pointer to the cached version of the iConfiguration string, if
 *	present for this configuration.
 * @intf_assoc: list of any interface association descriptors in this config
 * @interface: array of pointers to usb_interface structures, one for each
 *	interface in the configuration.  The number of interfaces is stored
 *	in desc.bNumInterfaces.  These pointers are valid only while the
 *	configuration is active.
 * @intf_cache: array of pointers to usb_interface_cache structures, one
 *	for each interface in the configuration.  These structures exist
 *	for the entire life of the device.
 * @extra: pointer to buffer containing all extra descriptors associated
 *	with this configuration (those preceding the first interface
 *	descriptor).
 * @extralen: length of the extra descriptors buffer.
 *
 * USB devices may have multiple configurations, but only one can be active
 * at any time.  Each encapsulates a different operational environment;
 * for example, a dual-speed device would have separate configurations for
 * full-speed and high-speed operation.  The number of configurations
 * available is stored in the device descriptor as bNumConfigurations.
 *
 * A configuration can contain multiple interfaces.  Each corresponds to
 * a different function of the USB device, and all are available whenever
 * the configuration is active.  The USB standard says that interfaces
 * are supposed to be numbered from 0 to desc.bNumInterfaces-1, but a lot
 * of devices get this wrong.  In addition, the interface array is not
 * guaranteed to be sorted in numerical order.  Use usb_ifnum_to_if() to
 * look up an interface entry based on its number.
 *
 * Device drivers should not attempt to activate configurations.  The choice
 * of which configuration to install is a policy decision based on such
 * considerations as available power, functionality provided, and the user's
 * desires (expressed through userspace tools).  However, drivers can call
 * usb_reset_configuration() to reinitialize the current configuration and
 * all its interfaces.
 */
struct usb_host_config {
	struct usb_config_descriptor	desc;

	char *string;		/* iConfiguration string, if present */

	/* List of any Interface Association Descriptors in this
	 * configuration. */
	struct usb_interface_assoc_descriptor *intf_assoc[USB_MAXIADS];

	/* the interfaces associated with this configuration,
	 * stored in no particular order */
	struct usb_interface *interface[USB_MAXINTERFACES];

	/* Interface information available even when this is not the
	 * active configuration */
	struct usb_interface_cache *intf_cache[USB_MAXINTERFACES];

	unsigned char *extra;   /* Extra descriptors */
	int extralen;
};

/* USB2.0 and USB3.0 device BOS descriptor set */
struct usb_host_bos {
	struct usb_bos_descriptor	*desc;

	struct usb_ext_cap_descriptor	*ext_cap;
	struct usb_ss_cap_descriptor	*ss_cap;
	struct usb_ssp_cap_descriptor	*ssp_cap;
	struct usb_ss_container_id_descriptor	*ss_id;
	struct usb_ptm_cap_descriptor	*ptm_cap;
};

int __usb_get_extra_descriptor(char *buffer, unsigned size,
	unsigned char type, void **ptr, size_t min);
#define usb_get_extra_descriptor(ifpoint, type, ptr) \
				__usb_get_extra_descriptor((ifpoint)->extra, \
				(ifpoint)->extralen, \
				type, (void **)ptr, sizeof(**(ptr)))

/* ----------------------------------------------------------------------- */

/*
 * Allocated per bus (tree of devices) we have:
 */
struct usb_bus {
	struct device *controller;	/* host side hardware */
	struct device *sysdev;		/* as seen from firmware or bus */
	int busnum;			/* Bus number (in order of reg) */
	const char *bus_name;		/* stable id (PCI slot_name etc) */
	u8 uses_pio_for_control;	/*
					 * Does the host controller use PIO
					 * for control transfers?
					 */
	u8 otg_port;			/* 0, or number of OTG/HNP port */
	unsigned is_b_host:1;		/* true during some HNP roleswitches */
	unsigned b_hnp_enable:1;	/* OTG: did A-Host enable HNP? */
	unsigned no_stop_on_short:1;    /*
					 * Quirk: some controllers don't stop
					 * the ep queue on a short transfer
					 * with the URB_SHORT_NOT_OK flag set.
					 */
	unsigned no_sg_constraint:1;	/* no sg constraint */
	unsigned sg_tablesize;		/* 0 or largest number of sg list entries */

	int devnum_next;		/* Next open device number in
					 * round-robin allocation */
	struct mutex devnum_next_mutex; /* devnum_next mutex */

	DECLARE_BITMAP(devmap, 128);	/* USB device number allocation bitmap */
	struct usb_device *root_hub;	/* Root hub */
	struct usb_bus *hs_companion;	/* Companion EHCI bus, if any */

	int bandwidth_allocated;	/* on this bus: how much of the time
					 * reserved for periodic (intr/iso)
					 * requests is used, on average?
					 * Units: microseconds/frame.
					 * Limits: Full/low speed reserve 90%,
					 * while high speed reserves 80%.
					 */
	int bandwidth_int_reqs;		/* number of Interrupt requests */
	int bandwidth_isoc_reqs;	/* number of Isoc. requests */

	unsigned resuming_ports;	/* bit array: resuming root-hub ports */

#if defined(CONFIG_USB_MON) || defined(CONFIG_USB_MON_MODULE)
	struct mon_bus *mon_bus;	/* non-null when associated */
	int monitored;			/* non-zero when monitored */
#endif
};

struct usb_dev_state;

/* ----------------------------------------------------------------------- */

struct usb_tt;

enum usb_link_tunnel_mode {
	USB_LINK_UNKNOWN = 0,
	USB_LINK_NATIVE,
	USB_LINK_TUNNELED,
};

enum usb_port_connect_type {
	USB_PORT_CONNECT_TYPE_UNKNOWN = 0,
	USB_PORT_CONNECT_TYPE_HOT_PLUG,
	USB_PORT_CONNECT_TYPE_HARD_WIRED,
	USB_PORT_NOT_USED,
};

/*
 * USB port quirks.
 */

/* For the given port, prefer the old (faster) enumeration scheme. */
#define USB_PORT_QUIRK_OLD_SCHEME	BIT(0)

/* Decrease TRSTRCY to 10ms during device enumeration. */
#define USB_PORT_QUIRK_FAST_ENUM	BIT(1)

/*
 * USB 2.0 Link Power Management (LPM) parameters.
 */
struct usb2_lpm_parameters {
	/* Best effort service latency indicate how long the host will drive
	 * resume on an exit from L1.
	 */
	unsigned int besl;

	/* Timeout value in microseconds for the L1 inactivity (LPM) timer.
	 * When the timer counts to zero, the parent hub will initiate a LPM
	 * transition to L1.
	 */
	int timeout;
};

/*
 * USB 3.0 Link Power Management (LPM) parameters.
 *
 * PEL and SEL are USB 3.0 Link PM latencies for device-initiated LPM exit.
 * MEL is the USB 3.0 Link PM latency for host-initiated LPM exit.
 * All three are stored in nanoseconds.
 */
struct usb3_lpm_parameters {
	/*
	 * Maximum exit latency (MEL) for the host to send a packet to the
	 * device (either a Ping for isoc endpoints, or a data packet for
	 * interrupt endpoints), the hubs to decode the packet, and for all hubs
	 * in the path to transition the links to U0.
	 */
	unsigned int mel;
	/*
	 * Maximum exit latency for a device-initiated LPM transition to bring
	 * all links into U0.  Abbreviated as "PEL" in section 9.4.12 of the USB
	 * 3.0 spec, with no explanation of what "P" stands for.  "Path"?
	 */
	unsigned int pel;

	/*
	 * The System Exit Latency (SEL) includes PEL, and three other
	 * latencies.  After a device initiates a U0 transition, it will take
	 * some time from when the device sends the ERDY to when it will finally
	 * receive the data packet.  Basically, SEL should be the worse-case
	 * latency from when a device starts initiating a U0 transition to when
	 * it will get data.
	 */
	unsigned int sel;
	/*
	 * The idle timeout value that is currently programmed into the parent
	 * hub for this device.  When the timer counts to zero, the parent hub
	 * will initiate an LPM transition to either U1 or U2.
	 */
	int timeout;
};

/**
 * struct usb_device - kernel's representation of a USB device
 * @devnum: device number; address on a USB bus
 * @devpath: device ID string for use in messages (e.g., /port/...)
 * @route: tree topology hex string for use with xHCI
 * @state: device state: configured, not attached, etc.
 * @speed: device speed: high/full/low (or error)
 * @rx_lanes: number of rx lanes in use, USB 3.2 adds dual-lane support
 * @tx_lanes: number of tx lanes in use, USB 3.2 adds dual-lane support
 * @ssp_rate: SuperSpeed Plus phy signaling rate and lane count
 * @tt: Transaction Translator info; used with low/full speed dev, highspeed hub
 * @ttport: device port on that tt hub
 * @toggle: one bit for each endpoint, with ([0] = IN, [1] = OUT) endpoints
 * @parent: our hub, unless we're the root
 * @bus: bus we're part of
 * @ep0: endpoint 0 data (default control pipe)
 * @dev: generic device interface
 * @descriptor: USB device descriptor
 * @bos: USB device BOS descriptor set
 * @config: all of the device's configs
 * @actconfig: the active configuration
 * @ep_in: array of IN endpoints
 * @ep_out: array of OUT endpoints
 * @rawdescriptors: raw descriptors for each config
 * @bus_mA: Current available from the bus
 * @portnum: parent port number (origin 1)
 * @level: number of USB hub ancestors
 * @devaddr: device address, XHCI: assigned by HW, others: same as devnum
 * @can_submit: URBs may be submitted
 * @persist_enabled:  USB_PERSIST enabled for this device
 * @reset_in_progress: the device is being reset
 * @have_langid: whether string_langid is valid
 * @authorized: policy has said we can use it;
 *	(user space) policy determines if we authorize this device to be
 *	used or not. By default, wired USB devices are authorized.
 *	WUSB devices are not, until we authorize them from user space.
 *	FIXME -- complete doc
 * @authenticated: Crypto authentication passed
 * @tunnel_mode: Connection native or tunneled over USB4
 * @usb4_link: device link to the USB4 host interface
 * @lpm_capable: device supports LPM
 * @lpm_devinit_allow: Allow USB3 device initiated LPM, exit latency is in range
 * @usb2_hw_lpm_capable: device can perform USB2 hardware LPM
 * @usb2_hw_lpm_besl_capable: device can perform USB2 hardware BESL LPM
 * @usb2_hw_lpm_enabled: USB2 hardware LPM is enabled
 * @usb2_hw_lpm_allowed: Userspace allows USB 2.0 LPM to be enabled
 * @usb3_lpm_u1_enabled: USB3 hardware U1 LPM enabled
 * @usb3_lpm_u2_enabled: USB3 hardware U2 LPM enabled
 * @string_langid: language ID for strings
 * @product: iProduct string, if present (static)
 * @manufacturer: iManufacturer string, if present (static)
 * @serial: iSerialNumber string, if present (static)
 * @filelist: usbfs files that are open to this device
 * @maxchild: number of ports if hub
 * @quirks: quirks of the whole device
 * @urbnum: number of URBs submitted for the whole device
 * @active_duration: total time device is not suspended
 * @connect_time: time device was first connected
 * @do_remote_wakeup:  remote wakeup should be enabled
 * @reset_resume: needs reset instead of resume
 * @port_is_suspended: the upstream port is suspended (L2 or U3)
 * @offload_pm_locked: prevents offload_usage changes during PM transitions.
 * @offload_usage: number of offload activities happening on this usb device.
 * @offload_lock: protects offload_usage and offload_pm_locked
 * @slot_id: Slot ID assigned by xHCI
 * @l1_params: best effor service latency for USB2 L1 LPM state, and L1 timeout.
 * @u1_params: exit latencies for USB3 U1 LPM state, and hub-initiated timeout.
 * @u2_params: exit latencies for USB3 U2 LPM state, and hub-initiated timeout.
 * @lpm_disable_count: Ref count used by usb_disable_lpm() and usb_enable_lpm()
 *	to keep track of the number of functions that require USB 3.0 Link Power
 *	Management to be disabled for this usb_device.  This count should only
 *	be manipulated by those functions, with the bandwidth_mutex is held.
 * @hub_delay: cached value consisting of:
 *	parent->hub_delay + wHubDelay + tTPTransmissionDelay (40ns)
 *	Will be used as wValue for SetIsochDelay requests.
 * @use_generic_driver: ask driver core to reprobe using the generic driver.
 *
 * Notes:
 * Usbcore drivers should not set usbdev->state directly.  Instead use
 * usb_set_device_state().
 */
struct usb_device {
	// === 设备标识与路由 ===
	int		devnum;       // USB 设备地址 (1-127), 由 SET_ADDRESS 分配
	char	devpath[16];   // 设备拓扑路径 (如 "1-2.1" = bus1, port2, subport1)
	u32		route;         // USB3 路由字符串 (4位×5层=20位, 用于 SS HUB)

	// === 状态与速度 ===
	enum usb_device_state	state; // 设备状态: ATTACHED→POWERED→DEFAULT→ADDRESS→CONFIGURED
	enum usb_device_speed	speed; // 速度: USB_SPEED_LOW/FULL/HIGH/SUPER/SUPER_PLUS
	unsigned int		rx_lanes; // USB3.2/4 接收通道数 (1 或 2)
	unsigned int		tx_lanes; // USB3.2/4 发送通道数
	enum usb_ssp_rate	ssp_rate; // SuperSpeedPlus 速率: 5G/10G/20G

	// === Transaction Translator (USB2 HUB 接 LS/FS 设备时使用) ===
	struct usb_tt	*tt;       	// 指向 HUB 的 TT (如果此设备接在 HS HUB 下游的 LS/FS 端口)
	int				ttport;     // TT 端口号

	// === 数据触发位 (Toggle Bit) ===
	unsigned int toggle[2];     // [0]=IN, [1]=OUT 端点的 data toggle 位
	                            // 批量/中断/控制传输使用, 同步传输不使用

	// === 拓扑与总线关联 ===
	struct usb_device *parent;   // 父设备 (若为根 HUB 则为 NULL)
	struct usb_bus *bus;         // 所属 USB 总线 (struct usb_bus 关联到 struct usb_hcd)
	struct usb_host_endpoint ep0; // 端点 0 (控制端点, 唯一保证存在的双向端点)

	// === Linux 设备模型集成 ===
	struct device dev;           // 内嵌 struct device (用于 sysfs, 电源管理, 驱动绑定)
	                             // 通过 to_usb_device() 可从 dev 反查 usb_device

	// === 描述符缓存 ===
	struct usb_device_descriptor descriptor; // 设备描述符 (18 字节, 含 VID/PID/bcdUSB)
	struct usb_host_bos *bos;                // BOS 描述符 (USB 2.1+, 含 LPM/SS capability)
	struct usb_host_config *config;          // 所有已解析配置的数组

	// === 当前活动配置 ===
	struct usb_host_config *actconfig;       // 当前激活的配置 (由 SET_CONFIGURATION 选定)
	struct usb_host_endpoint *ep_in[16];     // IN 端点快速查找表 (按端点号索引)
	struct usb_host_endpoint *ep_out[16];    // OUT 端点快速查找表 (按端点号索引)
	                                         // 注意: ep0 不在这些数组中!

	// === 原始描述符 ===
	char **rawdescriptors;                   // 原始二进制描述符 (每个配置一个 char*)

	// === HUB 相关信息 ===
	unsigned short bus_mA;                   // 总线可用电流 (mA), 用于电源预算
	u8 portnum;                              // 在父 HUB 上的端口号 (1-based)
	u8 level;                                // USB 树层级 (根 HUB=0, 逐级+1)
	u8 devaddr;                              // 设备地址 (与 devnum 相同, 用于 XHCI slot)

	// === 设备状态标志位 ===
	unsigned can_submit:1;           // 允许提交 URB (设备至少已初始化)
	unsigned persist_enabled:1;      // USB persist 启用 (设备关闭期间保持 /dev 节点)
	unsigned reset_in_progress:1;    // 正在复位中
	unsigned have_langid:1;          // 已知语言 ID (用于获取字符串描述符)
	unsigned authorized:1;           // 已授权 (用户空间通过 usbfs 控制)
	unsigned authenticated:1;        // 已认证 (无线 USB)
	// === Link Power Management (LPM) 相关 ===
	unsigned lpm_capable:1;          // 设备支持 LPM (Link Power Management)
	unsigned lpm_devinit_allow:1;    // 设备启动期间允许 LPM
	unsigned usb2_hw_lpm_capable:1;  // 硬件支持 USB2 LPM
	unsigned usb2_hw_lpm_besl_capable:1; // 支持 BESL (Best Effort Service Latency)
	unsigned usb2_hw_lpm_enabled:1;  // USB2 HW LPM 已启用
	unsigned usb2_hw_lpm_allowed:1;  // USB2 HW LPM 允许
	unsigned usb3_lpm_u1_enabled:1;  // USB3 U1 状态已启用
	unsigned usb3_lpm_u2_enabled:1;  // USB3 U2 状态已启用
	int string_langid;               // 字符串描述符的语言 ID (0x0409 = 英语)

	// === 设备字符串 (来自描述符) ===
	char *product;                   // iProduct 字符串
	char *manufacturer;              // iManufacturer 字符串
	char *serial;                    // iSerialNumber 字符串

	// === usbfs (用户空间驱动) 相关 ===
	struct list_head filelist;       // usbfs 打开的 file 链表 (用于设备移除时通知)

	// === 如果此设备是 HUB ===
	int maxchild;                    // 下游端口数量 (0 = 非 HUB)

	// === 不常见/变通 ===
	u32 quirks;                      // USB_QUIRK_* 标志 (设备特定变通方案)
	atomic_t urbnum;                 // 活跃 URB 计数 (用于调试和 driver unbind)

	// === 计时与电源管理 ===
	unsigned long active_duration;   // 累计活跃时间 (ms, 用于 autosuspend)
	unsigned long connect_time;      // 连接时间戳

	unsigned do_remote_wakeup:1;     // 启用远程唤醒
	unsigned reset_resume:1;         // 使用复位-恢复 (而非正常恢复)
	unsigned port_is_suspended:1;    // 端口已挂起
	unsigned offload_pm_locked:1;    // 卸载电源管理已锁定
	int offload_usage;               // 卸载 PM 使用计数
	spinlock_t offload_lock;         // 卸载 PM 自旋锁
	enum usb_link_tunnel_mode tunnel_mode; // USB4 隧道模式 (USB3/DP/PCIe)
	struct device_link *usb4_link;   // USB4 设备链接

	// === XHCI 特定 ===
	int slot_id;                     // XHCI 控制器分配的设备槽 (Device Slot)
	struct usb2_lpm_parameters l1_params; // USB2 L1 LPM 参数
	struct usb3_lpm_parameters u1_params; // USB3 U1 LPM 参数
	struct usb3_lpm_parameters u2_params; // USB3 U2 LPM 参数
	unsigned lpm_disable_count;      // LPM 禁用引用计数 (>0 = LPM 禁用)

	u16 hub_delay;                   // HUB 延迟值 (从 USB3 HUB 描述符获取)
	unsigned use_generic_driver:1;   // 使用通用驱动 (usb_generic_driver)
};

#define to_usb_device(__dev)	container_of_const(__dev, struct usb_device, dev)

static inline struct usb_device *__intf_to_usbdev(struct usb_interface *intf)
{
	return to_usb_device(intf->dev.parent);
}
static inline const struct usb_device *__intf_to_usbdev_const(const struct usb_interface *intf)
{
	return to_usb_device((const struct device *)intf->dev.parent);
}

#define interface_to_usbdev(intf)					\
	_Generic((intf),						\
		 const struct usb_interface *: __intf_to_usbdev_const,	\
		 struct usb_interface *: __intf_to_usbdev)(intf)

/*
 * usb_get_dev / usb_put_dev — USB 设备引用计数管理
 *
 * 每获取一个 usb_device 指针，调用 usb_get_dev() 增加引用计数；
 * 使用完毕后调用 usb_put_dev() 减少引用。当引用归零时释放 usb_device。
 * 这对函数防止在驱动仍持有设备指针时设备被热拔出释放。
 *
 * 典型用法:
 *   struct usb_device *udev = usb_get_dev(intf_to_usbdev(intf));
 *   ... 使用 udev ...
 *   usb_put_dev(udev);
 */
extern struct usb_device *usb_get_dev(struct usb_device *dev);
extern void usb_put_dev(struct usb_device *dev);

// usb_hub_find_child — 在 HUB 的指定端口查找子设备
extern struct usb_device *usb_hub_find_child(struct usb_device *hdev,
	int port1);

/**
 * usb_hub_for_each_child - iterate over all child devices on the hub
 * @hdev:  USB device belonging to the usb hub
 * @port1: portnum associated with child device
 * @child: child device pointer
 *
 * 遍历 HUB 所有下游端口连接的子设备。
 * continue 会自动跳过空端口。
 */
#define usb_hub_for_each_child(hdev, port1, child) \
	for (port1 = 1,	child =	usb_hub_find_child(hdev, port1); \
			port1 <= hdev->maxchild; \
			child = usb_hub_find_child(hdev, ++port1)) \
		if (!child) continue; else

/*
 * USB 设备锁 — 保护 usb_device 的并发访问
 *
 * usb_lock_device/unlock_device:
 *   对 usb_device 内嵌的 struct device->mutex 加锁。
 *   任何需要原子化执行的操作（如配置选择、接口操作）都应持有此锁。
 *
 * usb_lock_device_interruptible:
 *   可中断版本的锁（允许信号唤醒）。
 *
 * usb_trylock_device:
 *   非阻塞尝试加锁，失败立即返回。
 *
 * usb_lock_device_for_reset:
 *   为设备复位获取锁（比普通锁更复杂，涉及 pre_reset/post_reset 回调）。
 */
#define usb_lock_device(udev)			device_lock(&(udev)->dev)
#define usb_unlock_device(udev)			device_unlock(&(udev)->dev)
#define usb_lock_device_interruptible(udev)	device_lock_interruptible(&(udev)->dev)
#define usb_trylock_device(udev)		device_trylock(&(udev)->dev)
extern int usb_lock_device_for_reset(struct usb_device *udev,
				     const struct usb_interface *iface);

/*
 * usb_reset_device — 复位 USB 设备
 *
 * 执行 USB 端口复位，恢复设备到默认状态。成功后设备的所有配置/接口
 * 驱动会被重新 probe。失败则设备被移除。这是设备错误恢复的最后手段。
 *
 * usb_queue_reset_device — 从原子上下文调度设备复位
 *   将复位请求入队到 workqueue，避免在中断处理程序中直接复位。
 */
extern int usb_reset_device(struct usb_device *dev);
extern void usb_queue_reset_device(struct usb_interface *dev);

// usb_intf_get_dma_device — 获取接口的 DMA 设备 (通常 = udev->dev, USB4 可能不同)
extern struct device *usb_intf_get_dma_device(struct usb_interface *intf);

#ifdef CONFIG_ACPI
extern int usb_acpi_set_power_state(struct usb_device *hdev, int index,
	bool enable);
extern bool usb_acpi_power_manageable(struct usb_device *hdev, int index);
extern int usb_acpi_port_lpm_incapable(struct usb_device *hdev, int index);
#else
static inline int usb_acpi_set_power_state(struct usb_device *hdev, int index,
	bool enable) { return 0; }
static inline bool usb_acpi_power_manageable(struct usb_device *hdev, int index)
	{ return true; }
static inline int usb_acpi_port_lpm_incapable(struct usb_device *hdev, int index)
	{ return 0; }
#endif

/*
 * USB autosuspend / autoresume — 自动电源管理 API
 *
 * 启用 autosuspend 后，USB Core 会在设备空闲一段时间后自动将其挂起
 * (节省电源)。当有 URB 提交或驱动调用 autopm_get 时自动恢复。
 *
 * usb_enable_autosuspend / usb_disable_autosuspend:
 *   启用/禁用设备的自动挂起。
 *   注意: 如果驱动的 supports_autosuspend=0, USB Core 也不会允许 autosuspend。
 *
 * usb_autopm_get_interface / usb_autopm_put_interface (同步):
 *   获取/释放接口的 runtime PM 引用。get 确保设备处于活跃状态 (可能触发 resume)；
 *   put 允许设备在空闲后重新 autosuspend。必须配对调用！
 *
 *   probe() 期间设备自动处于活跃状态, 不需要调用 get。
 *
 * _async 变体 (中断上下文安全):
 *   不等待 resume 完成, 而是启动异步 resume 后立即返回。
 *   适用于中断上下文或持有自旋锁的场景。
 *
 * _no_resume 变体:
 *   get_no_resume: 仅增加计数, 不触发 resume (假设设备已经是活跃的)。
 *   put_no_suspend: 仅减少计数, 不触发 suspend。
 *   用于精确控制电源转换时机的特殊场景。
 *
 * usb_mark_last_busy:
 *   重置空闲计时器。在每次完成 I/O 操作后调用, 延迟 autosuspend。
 */
#ifdef CONFIG_PM
extern void usb_enable_autosuspend(struct usb_device *udev);
extern void usb_disable_autosuspend(struct usb_device *udev);

extern int usb_autopm_get_interface(struct usb_interface *intf);    // 获取 PM 引用 (同步恢复)
extern void usb_autopm_put_interface(struct usb_interface *intf);   // 释放 PM 引用 (允许挂起)
extern int usb_autopm_get_interface_async(struct usb_interface *intf); // 异步恢复 (IRQ 安全)
extern void usb_autopm_put_interface_async(struct usb_interface *intf);// 异步释放
extern void usb_autopm_get_interface_no_resume(struct usb_interface *intf); // 仅增计数
extern void usb_autopm_put_interface_no_suspend(struct usb_interface *intf); // 仅减计数

static inline void usb_mark_last_busy(struct usb_device *udev)
{
	pm_runtime_mark_last_busy(&udev->dev);  // 重置空闲计时器
}

#else

static inline void usb_enable_autosuspend(struct usb_device *udev)
{ }
static inline void usb_disable_autosuspend(struct usb_device *udev)
{ }

static inline int usb_autopm_get_interface(struct usb_interface *intf)
{ return 0; }
static inline int usb_autopm_get_interface_async(struct usb_interface *intf)
{ return 0; }

static inline void usb_autopm_put_interface(struct usb_interface *intf)
{ }
static inline void usb_autopm_put_interface_async(struct usb_interface *intf)
{ }
static inline void usb_autopm_get_interface_no_resume(
		struct usb_interface *intf)
{ }
static inline void usb_autopm_put_interface_no_suspend(
		struct usb_interface *intf)
{ }
static inline void usb_mark_last_busy(struct usb_device *udev)
{ }
#endif

#if IS_ENABLED(CONFIG_USB_XHCI_SIDEBAND)
int usb_offload_get(struct usb_device *udev);
int usb_offload_put(struct usb_device *udev);
bool usb_offload_check(struct usb_device *udev);
void usb_offload_set_pm_locked(struct usb_device *udev, bool locked);
#else

static inline int usb_offload_get(struct usb_device *udev)
{ return 0; }
static inline int usb_offload_put(struct usb_device *udev)
{ return 0; }
static inline bool usb_offload_check(struct usb_device *udev)
{ return false; }
static inline void usb_offload_set_pm_locked(struct usb_device *udev, bool locked)
{ }
#endif

extern int usb_disable_lpm(struct usb_device *udev);
extern void usb_enable_lpm(struct usb_device *udev);
/* Same as above, but these functions lock/unlock the bandwidth_mutex. */
extern int usb_unlocked_disable_lpm(struct usb_device *udev);
extern void usb_unlocked_enable_lpm(struct usb_device *udev);

extern int usb_disable_ltm(struct usb_device *udev);
extern void usb_enable_ltm(struct usb_device *udev);

static inline bool usb_device_supports_ltm(struct usb_device *udev)
{
	if (udev->speed < USB_SPEED_SUPER || !udev->bos || !udev->bos->ss_cap)
		return false;
	return udev->bos->ss_cap->bmAttributes & USB_LTM_SUPPORT;
}

static inline bool usb_device_no_sg_constraint(struct usb_device *udev)
{
	return udev && udev->bus && udev->bus->no_sg_constraint;
}


/*-------------------------------------------------------------------------*/

/* for drivers using iso endpoints */
extern int usb_get_current_frame_number(struct usb_device *usb_dev);

/* Sets up a group of bulk endpoints to support multiple stream IDs. */
extern int usb_alloc_streams(struct usb_interface *interface,
		struct usb_host_endpoint **eps, unsigned int num_eps,
		unsigned int num_streams, gfp_t mem_flags);

/* Reverts a group of bulk endpoints back to not using stream IDs. */
extern int usb_free_streams(struct usb_interface *interface,
		struct usb_host_endpoint **eps, unsigned int num_eps,
		gfp_t mem_flags);

/* used these for multi-interface device registration */
extern int usb_driver_claim_interface(struct usb_driver *driver,
			struct usb_interface *iface, void *data);

/**
 * usb_interface_claimed - returns true iff an interface is claimed
 * @iface: the interface being checked
 *
 * Return: %true (nonzero) iff the interface is claimed, else %false
 * (zero).
 *
 * Note:
 * Callers must own the driver model's usb bus readlock.  So driver
 * probe() entries don't need extra locking, but other call contexts
 * may need to explicitly claim that lock.
 *
 */
static inline int usb_interface_claimed(struct usb_interface *iface)
{
	return (iface->dev.driver != NULL);
}

extern void usb_driver_release_interface(struct usb_driver *driver,
			struct usb_interface *iface);

int usb_set_wireless_status(struct usb_interface *iface,
			enum usb_wireless_status status);

const struct usb_device_id *usb_match_id(struct usb_interface *interface,
					 const struct usb_device_id *id);
extern int usb_match_one_id(struct usb_interface *interface,
			    const struct usb_device_id *id);

extern int usb_for_each_dev(void *data, int (*fn)(struct usb_device *, void *));
extern struct usb_interface *usb_find_interface(struct usb_driver *drv,
		int minor);
extern struct usb_interface *usb_ifnum_to_if(const struct usb_device *dev,
		unsigned ifnum);
extern struct usb_host_interface *usb_altnum_to_altsetting(
		const struct usb_interface *intf, unsigned int altnum);
extern struct usb_host_interface *usb_find_alt_setting(
		struct usb_host_config *config,
		unsigned int iface_num,
		unsigned int alt_num);

/* port claiming functions */
int usb_hub_claim_port(struct usb_device *hdev, unsigned port1,
		struct usb_dev_state *owner);
int usb_hub_release_port(struct usb_device *hdev, unsigned port1,
		struct usb_dev_state *owner);

/**
 * usb_make_path - returns stable device path in the usb tree
 * @dev: the device whose path is being constructed
 * @buf: where to put the string
 * @size: how big is "buf"?
 *
 * Return: Length of the string (> 0) or negative if size was too small.
 *
 * Note:
 * This identifier is intended to be "stable", reflecting physical paths in
 * hardware such as physical bus addresses for host controllers or ports on
 * USB hubs.  That makes it stay the same until systems are physically
 * reconfigured, by re-cabling a tree of USB devices or by moving USB host
 * controllers.  Adding and removing devices, including virtual root hubs
 * in host controller driver modules, does not change these path identifiers;
 * neither does rebooting or re-enumerating.  These are more useful identifiers
 * than changeable ("unstable") ones like bus numbers or device addresses.
 *
 * With a partial exception for devices connected to USB 2.0 root hubs, these
 * identifiers are also predictable.  So long as the device tree isn't changed,
 * plugging any USB device into a given hub port always gives it the same path.
 * Because of the use of "companion" controllers, devices connected to ports on
 * USB 2.0 root hubs (EHCI host controllers) will get one path ID if they are
 * high speed, and a different one if they are full or low speed.
 */
static inline int usb_make_path(struct usb_device *dev, char *buf, size_t size)
{
	int actual;
	actual = snprintf(buf, size, "usb-%s-%s", dev->bus->bus_name,
			  dev->devpath);
	return (actual >= (int)size) ? -1 : actual;
}

/*
 * ==========================================================================
 * USB 设备 ID 匹配宏 — struct usb_device_id 构建器
 * ==========================================================================
 *
 * 每个 USB 驱动必须定义 usb_device_id 数组。USB Core 在设备插入时
 * 遍历所有已注册驱动的 id_table, 找到匹配项后调用 probe()。
 *
 * 常用匹配宏:
 *   USB_DEVICE(vid, pid)              — 精确 VID+PID
 *   USB_DEVICE_VER(vid, pid, lo, hi)  — VID+PID + 设备版本范围
 *   USB_DEVICE_INTERFACE_CLASS(...)   — VID+PID + 接口 class
 *   USB_INTERFACE_INFO(cl,sc,pr)      — 仅接口 class/subclass/protocol
 *                                      (用于标准 USB 类驱动: HID/audio/storage)
 *
 * 必须配合 MODULE_DEVICE_TABLE(usb, table) 导出, 使 udev 能自动加载模块。
 * 省略 MODULE_DEVICE_TABLE 将导致驱动仅在手动加载时工作 (无热插拔)。
 */

/*-------------------------------------------------------------------------*/

#define USB_DEVICE_ID_MATCH_DEVICE \
		(USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT)
#define USB_DEVICE_ID_MATCH_DEV_RANGE \
		(USB_DEVICE_ID_MATCH_DEV_LO | USB_DEVICE_ID_MATCH_DEV_HI)
#define USB_DEVICE_ID_MATCH_DEVICE_AND_VERSION \
		(USB_DEVICE_ID_MATCH_DEVICE | USB_DEVICE_ID_MATCH_DEV_RANGE)
#define USB_DEVICE_ID_MATCH_DEV_INFO \
		(USB_DEVICE_ID_MATCH_DEV_CLASS | \
		USB_DEVICE_ID_MATCH_DEV_SUBCLASS | \
		USB_DEVICE_ID_MATCH_DEV_PROTOCOL)
#define USB_DEVICE_ID_MATCH_INT_INFO \
		(USB_DEVICE_ID_MATCH_INT_CLASS | \
		USB_DEVICE_ID_MATCH_INT_SUBCLASS | \
		USB_DEVICE_ID_MATCH_INT_PROTOCOL)

/**
 * USB_DEVICE - macro used to describe a specific usb device
 * @vend: the 16 bit USB Vendor ID
 * @prod: the 16 bit USB Product ID
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific device.
 */
#define USB_DEVICE(vend, prod) \
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE, \
	.idVendor = (vend), \
	.idProduct = (prod)
/**
 * USB_DEVICE_VER - describe a specific usb device with a version range
 * @vend: the 16 bit USB Vendor ID
 * @prod: the 16 bit USB Product ID
 * @lo: the bcdDevice_lo value
 * @hi: the bcdDevice_hi value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific device, with a version range.
 */
#define USB_DEVICE_VER(vend, prod, lo, hi) \
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE_AND_VERSION, \
	.idVendor = (vend), \
	.idProduct = (prod), \
	.bcdDevice_lo = (lo), \
	.bcdDevice_hi = (hi)

/**
 * USB_DEVICE_INTERFACE_CLASS - describe a usb device with a specific interface class
 * @vend: the 16 bit USB Vendor ID
 * @prod: the 16 bit USB Product ID
 * @cl: bInterfaceClass value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific interface class of devices.
 */
#define USB_DEVICE_INTERFACE_CLASS(vend, prod, cl) \
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE | \
		       USB_DEVICE_ID_MATCH_INT_CLASS, \
	.idVendor = (vend), \
	.idProduct = (prod), \
	.bInterfaceClass = (cl)

/**
 * USB_DEVICE_INTERFACE_PROTOCOL - describe a usb device with a specific interface protocol
 * @vend: the 16 bit USB Vendor ID
 * @prod: the 16 bit USB Product ID
 * @pr: bInterfaceProtocol value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific interface protocol of devices.
 */
#define USB_DEVICE_INTERFACE_PROTOCOL(vend, prod, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE | \
		       USB_DEVICE_ID_MATCH_INT_PROTOCOL, \
	.idVendor = (vend), \
	.idProduct = (prod), \
	.bInterfaceProtocol = (pr)

/**
 * USB_DEVICE_INTERFACE_NUMBER - describe a usb device with a specific interface number
 * @vend: the 16 bit USB Vendor ID
 * @prod: the 16 bit USB Product ID
 * @num: bInterfaceNumber value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific interface number of devices.
 */
#define USB_DEVICE_INTERFACE_NUMBER(vend, prod, num) \
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE | \
		       USB_DEVICE_ID_MATCH_INT_NUMBER, \
	.idVendor = (vend), \
	.idProduct = (prod), \
	.bInterfaceNumber = (num)

/**
 * USB_DEVICE_INFO - macro used to describe a class of usb devices
 * @cl: bDeviceClass value
 * @sc: bDeviceSubClass value
 * @pr: bDeviceProtocol value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific class of devices.
 */
#define USB_DEVICE_INFO(cl, sc, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_DEV_INFO, \
	.bDeviceClass = (cl), \
	.bDeviceSubClass = (sc), \
	.bDeviceProtocol = (pr)

/**
 * USB_INTERFACE_INFO - macro used to describe a class of usb interfaces
 * @cl: bInterfaceClass value
 * @sc: bInterfaceSubClass value
 * @pr: bInterfaceProtocol value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific class of interfaces.
 */
#define USB_INTERFACE_INFO(cl, sc, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_INT_INFO, \
	.bInterfaceClass = (cl), \
	.bInterfaceSubClass = (sc), \
	.bInterfaceProtocol = (pr)

/**
 * USB_DEVICE_AND_INTERFACE_INFO - describe a specific usb device with a class of usb interfaces
 * @vend: the 16 bit USB Vendor ID
 * @prod: the 16 bit USB Product ID
 * @cl: bInterfaceClass value
 * @sc: bInterfaceSubClass value
 * @pr: bInterfaceProtocol value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific device with a specific class of interfaces.
 *
 * This is especially useful when explicitly matching devices that have
 * vendor specific bDeviceClass values, but standards-compliant interfaces.
 */
#define USB_DEVICE_AND_INTERFACE_INFO(vend, prod, cl, sc, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_INT_INFO \
		| USB_DEVICE_ID_MATCH_DEVICE, \
	.idVendor = (vend), \
	.idProduct = (prod), \
	.bInterfaceClass = (cl), \
	.bInterfaceSubClass = (sc), \
	.bInterfaceProtocol = (pr)

/**
 * USB_VENDOR_AND_INTERFACE_INFO - describe a specific usb vendor with a class of usb interfaces
 * @vend: the 16 bit USB Vendor ID
 * @cl: bInterfaceClass value
 * @sc: bInterfaceSubClass value
 * @pr: bInterfaceProtocol value
 *
 * This macro is used to create a struct usb_device_id that matches a
 * specific vendor with a specific class of interfaces.
 *
 * This is especially useful when explicitly matching devices that have
 * vendor specific bDeviceClass values, but standards-compliant interfaces.
 */
#define USB_VENDOR_AND_INTERFACE_INFO(vend, cl, sc, pr) \
	.match_flags = USB_DEVICE_ID_MATCH_INT_INFO \
		| USB_DEVICE_ID_MATCH_VENDOR, \
	.idVendor = (vend), \
	.bInterfaceClass = (cl), \
	.bInterfaceSubClass = (sc), \
	.bInterfaceProtocol = (pr)

/* ----------------------------------------------------------------------- */

/* Stuff for dynamic usb ids */
extern struct mutex usb_dynids_lock;
struct usb_dynids {
	struct list_head list;
};

struct usb_dynid {
	struct list_head node;
	struct usb_device_id id;
};

extern ssize_t usb_store_new_id(struct usb_dynids *dynids,
				const struct usb_device_id *id_table,
				struct device_driver *driver,
				const char *buf, size_t count);

extern ssize_t usb_show_dynids(struct usb_dynids *dynids, char *buf);

/**
 * struct usb_driver - identifies USB interface driver to usbcore
 * @name: The driver name should be unique among USB drivers,
 *	and should normally be the same as the module name.
 * @probe: Called to see if the driver is willing to manage a particular
 *	interface on a device.  If it is, probe returns zero and uses
 *	usb_set_intfdata() to associate driver-specific data with the
 *	interface.  It may also use usb_set_interface() to specify the
 *	appropriate altsetting.  If unwilling to manage the interface,
 *	return -ENODEV, if genuine IO errors occurred, an appropriate
 *	negative errno value.
 * @disconnect: Called when the interface is no longer accessible, usually
 *	because its device has been (or is being) disconnected or the
 *	driver module is being unloaded.
 * @unlocked_ioctl: Used for drivers that want to talk to userspace through
 *	the "usbfs" filesystem.  This lets devices provide ways to
 *	expose information to user space regardless of where they
 *	do (or don't) show up otherwise in the filesystem.
 * @suspend: Called when the device is going to be suspended by the
 *	system either from system sleep or runtime suspend context. The
 *	return value will be ignored in system sleep context, so do NOT
 *	try to continue using the device if suspend fails in this case.
 *	Instead, let the resume or reset-resume routine recover from
 *	the failure.
 * @resume: Called when the device is being resumed by the system.
 * @reset_resume: Called when the suspended device has been reset instead
 *	of being resumed.
 * @pre_reset: Called by usb_reset_device() when the device is about to be
 *	reset.  This routine must not return until the driver has no active
 *	URBs for the device, and no more URBs may be submitted until the
 *	post_reset method is called.
 * @post_reset: Called by usb_reset_device() after the device
 *	has been reset
 * @shutdown: Called at shut-down time to quiesce the device.
 * @id_table: USB drivers use ID table to support hotplugging.
 *	Export this with MODULE_DEVICE_TABLE(usb,...).  This must be set
 *	or your driver's probe function will never get called.
 * @dev_groups: Attributes attached to the device that will be created once it
 *	is bound to the driver.
 * @dynids: used internally to hold the list of dynamically added device
 *	ids for this driver.
 * @driver: The driver-model core driver structure.
 * @no_dynamic_id: if set to 1, the USB core will not allow dynamic ids to be
 *	added to this driver by preventing the sysfs file from being created.
 * @supports_autosuspend: if set to 0, the USB core will not allow autosuspend
 *	for interfaces bound to this driver.
 * @soft_unbind: if set to 1, the USB core will not kill URBs and disable
 *	endpoints before calling the driver's disconnect method.
 * @disable_hub_initiated_lpm: if set to 1, the USB core will not allow hubs
 *	to initiate lower power link state transitions when an idle timeout
 *	occurs.  Device-initiated USB 3.0 link PM will still be allowed.
 *
 * USB interface drivers must provide a name, probe() and disconnect()
 * methods, and an id_table.  Other driver fields are optional.
 *
 * The id_table is used in hotplugging.  It holds a set of descriptors,
 * and specialized data may be associated with each entry.  That table
 * is used by both user and kernel mode hotplugging support.
 *
 * The probe() and disconnect() methods are called in a context where
 * they can sleep, but they should avoid abusing the privilege.  Most
 * work to connect to a device should be done when the device is opened,
 * and undone at the last close.  The disconnect code needs to address
 * concurrency issues with respect to open() and close() methods, as
 * well as forcing all pending I/O requests to complete (by unlinking
 * them as necessary, and blocking until the unlinks complete).
 */
/*
 * struct usb_driver — 接口级 USB 驱动 (最常用的驱动类型)
 *
 * 这是 USB 驱动开发中最常用的注册结构。每个 USB 接口驱动 (如 usb-storage,
 * usbhid, usbserial) 都通过此结构与 USB Core 交互。
 *
 * probe() 被调用时机: 当 USB 设备的某个接口的 class/subclass/protocol
 * 与 id_table 中的条目匹配时被调用。此时设备已配置好，端点可用。
 *
 * 生命周期:
 *   probe(intf, id) → 端点操作 → disconnect(intf)
 *   suspend(intf) / resume(intf) / reset_resume(intf) (可选, 电源管理)
 */
struct usb_driver {
	const char *name;               // 驱动名称 (须唯一, 通常与模块名相同)

	// === 热插拔回调 (必须实现) ===
	int (*probe) (struct usb_interface *intf,
		      const struct usb_device_id *id); // 设备插入时调用
	void (*disconnect) (struct usb_interface *intf); // 设备拔出时调用
	                                                 // 必须等待所有 URB 完成!

	// === 可选回调 ===
	int (*unlocked_ioctl) (struct usb_interface *intf, unsigned int code,
			void *buf);      // 驱动私有 ioctl (通过 usbfs)

	int (*suspend) (struct usb_interface *intf, pm_message_t message);
	int (*resume) (struct usb_interface *intf);
	int (*reset_resume)(struct usb_interface *intf);

	int (*pre_reset)(struct usb_interface *intf);
	int (*post_reset)(struct usb_interface *intf);

	void (*shutdown)(struct usb_interface *intf);

	const struct usb_device_id *id_table;
	const struct attribute_group **dev_groups;

	struct usb_dynids dynids;
	struct device_driver driver;
	unsigned int no_dynamic_id:1;
	unsigned int supports_autosuspend:1;
	unsigned int disable_hub_initiated_lpm:1;
	unsigned int soft_unbind:1;
};
#define	to_usb_driver(d) container_of_const(d, struct usb_driver, driver)

/**
 * struct usb_device_driver - identifies USB device driver to usbcore
 * @name: The driver name should be unique among USB drivers,
 *	and should normally be the same as the module name.
 * @match: If set, used for better device/driver matching.
 * @probe: Called to see if the driver is willing to manage a particular
 *	device.  If it is, probe returns zero and uses dev_set_drvdata()
 *	to associate driver-specific data with the device.  If unwilling
 *	to manage the device, return a negative errno value.
 * @disconnect: Called when the device is no longer accessible, usually
 *	because it has been (or is being) disconnected or the driver's
 *	module is being unloaded.
 * @suspend: Called when the device is going to be suspended by the system.
 * @resume: Called when the device is being resumed by the system.
 * @choose_configuration: If non-NULL, called instead of the default
 *	usb_choose_configuration(). If this returns an error then we'll go
 *	on to call the normal usb_choose_configuration().
 * @dev_groups: Attributes attached to the device that will be created once it
 *	is bound to the driver.
 * @driver: The driver-model core driver structure.
 * @id_table: used with @match() to select better matching driver at
 * 	probe() time.
 * @supports_autosuspend: if set to 0, the USB core will not allow autosuspend
 *	for devices bound to this driver.
 * @generic_subclass: if set to 1, the generic USB driver's probe, disconnect,
 *	resume and suspend functions will be called in addition to the driver's
 *	own, so this part of the setup does not need to be replicated.
 *
 * USB device drivers must provide a name, other driver fields are optional.
 */
struct usb_device_driver {
	const char *name;

	bool (*match) (struct usb_device *udev);
	int (*probe) (struct usb_device *udev);
	void (*disconnect) (struct usb_device *udev);

	int (*suspend) (struct usb_device *udev, pm_message_t message);
	int (*resume) (struct usb_device *udev, pm_message_t message);

	int (*choose_configuration) (struct usb_device *udev);

	const struct attribute_group **dev_groups;
	struct device_driver driver;
	const struct usb_device_id *id_table;
	unsigned int supports_autosuspend:1;
	unsigned int generic_subclass:1;
};
#define	to_usb_device_driver(d) container_of_const(d, struct usb_device_driver, driver)

/**
 * struct usb_class_driver - identifies a USB driver that wants to use the USB major number
 * @name: the usb class device name for this driver.  Will show up in sysfs.
 * @devnode: Callback to provide a naming hint for a possible
 *	device node to create.
 * @fops: pointer to the struct file_operations of this driver.
 * @minor_base: the start of the minor range for this driver.
 *
 * This structure is used for the usb_register_dev() and
 * usb_deregister_dev() functions, to consolidate a number of the
 * parameters used for them.
 */
struct usb_class_driver {
	char *name;
	char *(*devnode)(const struct device *dev, umode_t *mode);
	const struct file_operations *fops;
	int minor_base;
};

/*
 * use these in module_init()/module_exit()
 * and don't forget MODULE_DEVICE_TABLE(usb, ...)
 */
extern int usb_register_driver(struct usb_driver *, struct module *,
			       const char *);

/* use a define to avoid include chaining to get THIS_MODULE & friends */
#define usb_register(driver) \
	usb_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)

extern void usb_deregister(struct usb_driver *);

/**
 * module_usb_driver() - Helper macro for registering a USB driver
 * @__usb_driver: usb_driver struct
 *
 * Helper macro for USB drivers which do not do anything special in module
 * init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
#define module_usb_driver(__usb_driver) \
	module_driver(__usb_driver, usb_register, \
		       usb_deregister)

extern int usb_register_device_driver(struct usb_device_driver *,
			struct module *);
extern void usb_deregister_device_driver(struct usb_device_driver *);

extern int usb_register_dev(struct usb_interface *intf,
			    struct usb_class_driver *class_driver);
extern void usb_deregister_dev(struct usb_interface *intf,
			       struct usb_class_driver *class_driver);

extern int usb_disabled(void);

/* ----------------------------------------------------------------------- */

/*
 * URB support, for asynchronous request completions
 */

/*
 * urb->transfer_flags — 传输行为控制标志
 *
 * 驱动可设置的标志:
 *   URB_SHORT_NOT_OK — 若实际传输 < 预期长度, 报告为错误 (非零 status)
 *   URB_ZERO_PACKET   — 批量 OUT: 若传输长度是端点 maxpacket 的倍数,
 *                       追加一个 zero-length packet 表示数据结束
 *   URB_NO_INTERRUPT  — 仅当出错时才产生中断 (优化: 减少中断开销)
 *   URB_FREE_BUFFER   — URB 释放时自动 kfree(transfer_buffer)
 *   URB_NO_TRANSFER_DMA_MAP — 驱动已自行 DMA 映射 transfer_dma, 无需 HCD 映射
 *   URB_ISO_ASAP      — 同步传输: 使用第一个未过期的调度槽
 *
 * 内部标志 (驱动不应设置):
 *   URB_DIR_IN/OUT    — 传输方向, 由 usb_submit_urb() 自动设置
 *   URB_DMA_MAP_*     — DMA 映射方式, HCD 内部使用
 *   URB_SETUP_MAP_*   — setup packet 的 DMA 映射状态
 *
 * Note: URB_DIR_IN/OUT is automatically set in usb_submit_urb().
 */
#define URB_SHORT_NOT_OK	0x0001	// 短包当作错误 (不是 -EREMOTEIO)
#define URB_ISO_ASAP		0x0002	// 同步: 尽快调度
#define URB_NO_TRANSFER_DMA_MAP	0x0004	// 驱动已自行 DMA 映射
#define URB_ZERO_PACKET		0x0040	// 批量OUT: 追加 ZLP 结束传输
#define URB_NO_INTERRUPT	0x0080	// 仅错误时产生中断
#define URB_FREE_BUFFER		0x0100	// 释放 URB 时自动 kfree transfer_buffer

/* The following flags are used internally by usbcore and HCDs */
#define URB_DIR_IN		0x0200	// 设备→主机 (读)
#define URB_DIR_OUT		0       // 主机→设备 (写)
#define URB_DIR_MASK		URB_DIR_IN

#define URB_DMA_MAP_SINGLE	0x00010000	// 无 scatter-gather 的 DMA 映射
#define URB_DMA_MAP_PAGE	0x00020000	// 逐页映射 (HCD 不做 SG)
#define URB_DMA_MAP_SG		0x00040000	// HCD 做了 SG 映射
#define URB_MAP_LOCAL		0x00080000	// HCD 本地内存
#define URB_SETUP_MAP_SINGLE	0x00100000	// setup_packet DMA 已映射
#define URB_SETUP_MAP_LOCAL	0x00200000	// setup_packet 在 HCD 本地内存
#define URB_DMA_SG_COMBINED	0x00400000	// SG 条目已合并
#define URB_ALIGNED_TEMP_BUFFER	0x00800000	// 临时对齐缓冲区已分配

struct usb_iso_packet_descriptor {
	unsigned int offset;
	unsigned int length;		/* expected length */
	unsigned int actual_length;
	int status;
};

struct urb;

struct usb_anchor {
	struct list_head urb_list;
	wait_queue_head_t wait;
	spinlock_t lock;
	atomic_t suspend_wakeups;
	unsigned int poisoned:1;
};

static inline void init_usb_anchor(struct usb_anchor *anchor)
{
	memset(anchor, 0, sizeof(*anchor));
	INIT_LIST_HEAD(&anchor->urb_list);
	init_waitqueue_head(&anchor->wait);
	spin_lock_init(&anchor->lock);
}

typedef void (*usb_complete_t)(struct urb *);

/**
 * struct urb - USB Request Block
 * @urb_list: For use by current owner of the URB.
 * @anchor_list: membership in the list of an anchor
 * @anchor: to anchor URBs to a common mooring
 * @ep: Points to the endpoint's data structure.  Will eventually
 *	replace @pipe.
 * @pipe: Holds endpoint number, direction, type, and more.
 *	Create these values with the eight macros available;
 *	usb_{snd,rcv}TYPEpipe(dev,endpoint), where the TYPE is "ctrl"
 *	(control), "bulk", "int" (interrupt), or "iso" (isochronous).
 *	For example usb_sndbulkpipe() or usb_rcvintpipe().  Endpoint
 *	numbers range from zero to fifteen.  Note that "in" endpoint two
 *	is a different endpoint (and pipe) from "out" endpoint two.
 *	The current configuration controls the existence, type, and
 *	maximum packet size of any given endpoint.
 * @stream_id: the endpoint's stream ID for bulk streams
 * @dev: Identifies the USB device to perform the request.
 * @status: This is read in non-iso completion functions to get the
 *	status of the particular request.  ISO requests only use it
 *	to tell whether the URB was unlinked; detailed status for
 *	each frame is in the fields of the iso_frame-desc.
 * @transfer_flags: A variety of flags may be used to affect how URB
 *	submission, unlinking, or operation are handled.  Different
 *	kinds of URB can use different flags.
 * @transfer_buffer:  This identifies the buffer to (or from) which the I/O
 *	request will be performed unless URB_NO_TRANSFER_DMA_MAP is set
 *	(however, do not leave garbage in transfer_buffer even then).
 *	This buffer must be suitable for DMA; allocate it with
 *	kmalloc() or equivalent.  For transfers to "in" endpoints, contents
 *	of this buffer will be modified.  This buffer is used for the data
 *	stage of control transfers.
 * @transfer_dma: When transfer_flags includes URB_NO_TRANSFER_DMA_MAP,
 *	the device driver is saying that it provided this DMA address,
 *	which the host controller driver should use in preference to the
 *	transfer_buffer.
 * @sg: scatter gather buffer list, the buffer size of each element in
 * 	the list (except the last) must be divisible by the endpoint's
 * 	max packet size if no_sg_constraint isn't set in 'struct usb_bus'
 * @sgt: used to hold a scatter gather table returned by usb_alloc_noncoherent(),
 *      which describes the allocated non-coherent and possibly non-contiguous
 *      memory and is guaranteed to have 1 single DMA mapped segment. The
 *      allocated memory needs to be freed by usb_free_noncoherent().
 * @num_mapped_sgs: (internal) number of mapped sg entries
 * @num_sgs: number of entries in the sg list
 * @transfer_buffer_length: How big is transfer_buffer.  The transfer may
 *	be broken up into chunks according to the current maximum packet
 *	size for the endpoint, which is a function of the configuration
 *	and is encoded in the pipe.  When the length is zero, neither
 *	transfer_buffer nor transfer_dma is used.
 * @actual_length: This is read in non-iso completion functions, and
 *	it tells how many bytes (out of transfer_buffer_length) were
 *	transferred.  It will normally be the same as requested, unless
 *	either an error was reported or a short read was performed.
 *	The URB_SHORT_NOT_OK transfer flag may be used to make such
 *	short reads be reported as errors.
 * @setup_packet: Only used for control transfers, this points to eight bytes
 *	of setup data.  Control transfers always start by sending this data
 *	to the device.  Then transfer_buffer is read or written, if needed.
 * @setup_dma: DMA pointer for the setup packet.  The caller must not use
 *	this field; setup_packet must point to a valid buffer.
 * @start_frame: Returns the initial frame for isochronous transfers.
 * @number_of_packets: Lists the number of ISO transfer buffers.
 * @interval: Specifies the polling interval for interrupt or isochronous
 *	transfers.  The units are frames (milliseconds) for full and low
 *	speed devices, and microframes (1/8 millisecond) for highspeed
 *	and SuperSpeed devices.
 * @error_count: Returns the number of ISO transfers that reported errors.
 * @context: For use in completion functions.  This normally points to
 *	request-specific driver context.
 * @complete: Completion handler. This URB is passed as the parameter to the
 *	completion function.  The completion function may then do what
 *	it likes with the URB, including resubmitting or freeing it.
 * @iso_frame_desc: Used to provide arrays of ISO transfer buffers and to
 *	collect the transfer status for each buffer.
 *
 * This structure identifies USB transfer requests.  URBs must be allocated by
 * calling usb_alloc_urb() and freed with a call to usb_free_urb().
 * Initialization may be done using various usb_fill_*_urb() functions.  URBs
 * are submitted using usb_submit_urb(), and pending requests may be canceled
 * using usb_unlink_urb() or usb_kill_urb().
 *
 * Data Transfer Buffers:
 *
 * Normally drivers provide I/O buffers allocated with kmalloc() or otherwise
 * taken from the general page pool.  That is provided by transfer_buffer
 * (control requests also use setup_packet), and host controller drivers
 * perform a dma mapping (and unmapping) for each buffer transferred.  Those
 * mapping operations can be expensive on some platforms (perhaps using a dma
 * bounce buffer or talking to an IOMMU),
 * although they're cheap on commodity x86 and ppc hardware.
 *
 * Alternatively, drivers may pass the URB_NO_TRANSFER_DMA_MAP transfer flag,
 * which tells the host controller driver that no such mapping is needed for
 * the transfer_buffer since
 * the device driver is DMA-aware.  For example, a device driver might
 * allocate a DMA buffer with usb_alloc_coherent() or call usb_buffer_map().
 * When this transfer flag is provided, host controller drivers will
 * attempt to use the dma address found in the transfer_dma
 * field rather than determining a dma address themselves.
 *
 * Note that transfer_buffer must still be set if the controller
 * does not support DMA (as indicated by hcd_uses_dma()) and when talking
 * to root hub. If you have to transfer between highmem zone and the device
 * on such controller, create a bounce buffer or bail out with an error.
 * If transfer_buffer cannot be set (is in highmem) and the controller is DMA
 * capable, assign NULL to it, so that usbmon knows not to use the value.
 * The setup_packet must always be set, so it cannot be located in highmem.
 *
 * Initialization:
 *
 * All URBs submitted must initialize the dev, pipe, transfer_flags (may be
 * zero), and complete fields.  All URBs must also initialize
 * transfer_buffer and transfer_buffer_length.  They may provide the
 * URB_SHORT_NOT_OK transfer flag, indicating that short reads are
 * to be treated as errors; that flag is invalid for write requests.
 *
 * Bulk URBs may
 * use the URB_ZERO_PACKET transfer flag, indicating that bulk OUT transfers
 * should always terminate with a short packet, even if it means adding an
 * extra zero length packet.
 *
 * Control URBs must provide a valid pointer in the setup_packet field.
 * Unlike the transfer_buffer, the setup_packet may not be mapped for DMA
 * beforehand.
 *
 * Interrupt URBs must provide an interval, saying how often (in milliseconds
 * or, for highspeed devices, 125 microsecond units)
 * to poll for transfers.  After the URB has been submitted, the interval
 * field reflects how the transfer was actually scheduled.
 * The polling interval may be more frequent than requested.
 * For example, some controllers have a maximum interval of 32 milliseconds,
 * while others support intervals of up to 1024 milliseconds.
 * Isochronous URBs also have transfer intervals.  (Note that for isochronous
 * endpoints, as well as high speed interrupt endpoints, the encoding of
 * the transfer interval in the endpoint descriptor is logarithmic.
 * Device drivers must convert that value to linear units themselves.)
 *
 * If an isochronous endpoint queue isn't already running, the host
 * controller will schedule a new URB to start as soon as bandwidth
 * utilization allows.  If the queue is running then a new URB will be
 * scheduled to start in the first transfer slot following the end of the
 * preceding URB, if that slot has not already expired.  If the slot has
 * expired (which can happen when IRQ delivery is delayed for a long time),
 * the scheduling behavior depends on the URB_ISO_ASAP flag.  If the flag
 * is clear then the URB will be scheduled to start in the expired slot,
 * implying that some of its packets will not be transferred; if the flag
 * is set then the URB will be scheduled in the first unexpired slot,
 * breaking the queue's synchronization.  Upon URB completion, the
 * start_frame field will be set to the (micro)frame number in which the
 * transfer was scheduled.  Ranges for frame counter values are HC-specific
 * and can go from as low as 256 to as high as 65536 frames.
 *
 * Isochronous URBs have a different data transfer model, in part because
 * the quality of service is only "best effort".  Callers provide specially
 * allocated URBs, with number_of_packets worth of iso_frame_desc structures
 * at the end.  Each such packet is an individual ISO transfer.  Isochronous
 * URBs are normally queued, submitted by drivers to arrange that
 * transfers are at least double buffered, and then explicitly resubmitted
 * in completion handlers, so
 * that data (such as audio or video) streams at as constant a rate as the
 * host controller scheduler can support.
 *
 * Completion Callbacks:
 *
 * The completion callback is made in_interrupt(), and one of the first
 * things that a completion handler should do is check the status field.
 * The status field is provided for all URBs.  It is used to report
 * unlinked URBs, and status for all non-ISO transfers.  It should not
 * be examined before the URB is returned to the completion handler.
 *
 * The context field is normally used to link URBs back to the relevant
 * driver or request state.
 *
 * When the completion callback is invoked for non-isochronous URBs, the
 * actual_length field tells how many bytes were transferred.  This field
 * is updated even when the URB terminated with an error or was unlinked.
 *
 * ISO transfer status is reported in the status and actual_length fields
 * of the iso_frame_desc array, and the number of errors is reported in
 * error_count.  Completion callbacks for ISO transfers will normally
 * (re)submit URBs to ensure a constant transfer rate.
 *
 * Note that even fields marked "public" should not be touched by the driver
 * when the urb is owned by the hcd, that is, since the call to
 * usb_submit_urb() till the entry into the completion routine.
 */
/*
 * struct urb — USB Request Block (USB 请求块)
 *
 * URB 是 USB 传输的基本单元。一次 USB 传输 = 一个 URB。
 * 驱动通过 usb_submit_urb() 提交，传输完成后调用 urb->complete 回调。
 *
 * 字段标注: (in)=输入参数 (驱动填写), (return)=输出参数 (HCD 填写)
 *
 * 生命周期: alloc → fill → submit → [complete callback] → free/重用
 */
struct urb {
	// ===== 私有字段 (HCD/USB Core 内部使用) =====
	struct kref kref;		// 引用计数 (usb_get_urb/usb_free_urb)
	int unlinked;			// 已 unlink 标志 + 错误码
	void *hcpriv;			// HCD 私有数据 (XHCI: 指向 TD 等)
	atomic_t use_count;		// 并发提交计数器
	atomic_t reject;		// 标记: 此 URB 将被拒绝提交

	// ===== 公共字段 (驱动使用) =====
	struct list_head urb_list;	// URB 链表节点 (用于 enqueue/dequeue)
	struct list_head anchor_list;	// Anchor 链表 (用于批量管理 URB)
	struct usb_anchor *anchor;      // 指向所属的 URB anchor
	struct usb_device *dev;		// (in) 目标 USB 设备
	struct usb_host_endpoint *ep;	// (internal) 目标端点 (由 pipe 自动解析)
	unsigned int pipe;		// (in) 管道编码: 类型+方向+设备地址+端点号
	                                //   通过 usb_sndctrlpipe/rcvbulkpipe 等宏创建
	unsigned int stream_id;		// (in) USB3 流 ID (0=不使用流)
	int status;			// (return) 完成状态: 0=成功, 负数=错误码
	unsigned int transfer_flags;	// (in) 传输标志: URB_SHORT_NOT_OK,
	                               //   URB_NO_TRANSFER_DMA_MAP, URB_ZERO_PACKET...
	void *transfer_buffer;		// (in) 数据缓冲区 (DMA 可用)
	dma_addr_t transfer_dma;	// (in) 如果 URB_NO_TRANSFER_DMA_MAP 设置,
	                               //   驱动提供预映射的 DMA 地址
	struct scatterlist *sg;		// (in) scatter-gather 列表 (替代 transfer_buffer)
	struct sg_table *sgt;		// (in) 非一致性缓冲区的 sg_table
	int num_mapped_sgs;		// (internal) HCD 映射的 SG 条目数
	int num_sgs;			// (in) SG 列表中条目数
	u32 transfer_buffer_length;	// (in) 缓冲区总大小 (字节)
	u32 actual_length;		// (return) 实际传输的字节数
	unsigned char *setup_packet;	// (in) 控制传输的 SETUP 包 (8字节, 仅 CONTROL)
	                               //   指向 struct usb_ctrlrequest
	dma_addr_t setup_dma;		// (in) SETUP 包的 DMA 地址
	// === 同步传输特有字段 ===
	int start_frame;		// (modify) 起始帧号 (同步传输)
	int number_of_packets;		// (in) 同步传输的包数量
	// === 中断/同步传输 ===
	int interval;			// (modify) 轮询间隔 (单位: USB 微帧)
	                               //   高速: 2^(bInterval-1) × 125μs
	                               //   USB3: 2^(bInterval-1) × 125μs
	int error_count;		// (return) 同步传输错误计数
	// === 完成回调 ===
	void *context;			// (in) 传递给 complete() 的私有数据
	usb_complete_t complete;	// (in) 完成回调: void (*)(struct urb *)
	                               //   在 tasklet(软中断)上下文中调用, 可以睡眠
	// === 同步传输帧描述符 ===
	struct usb_iso_packet_descriptor iso_frame_desc[];
					// (in) 变长数组, 每个同步包一个描述符
	                               //   包含每个包的 offset, length, actual_length, status
};

/*
 * ==========================================================================
 * usb_fill_*_urb() — URB 初始化辅助宏
 * ==========================================================================
 *
 * 在提交 URB 前, 必须用这些函数填充必要的字段。
 * 每种传输类型有对应的 fill 函数:
 *   usb_fill_control_urb — 控制传输 (Setup + Data + Status 三阶段)
 *   usb_fill_bulk_urb    — 批量传输 (大数据量, 可靠)
 *   usb_fill_int_urb     — 中断传输 (小数据量, 周期性轮询)
 *
 * 注意: transfer_buffer 必须是 DMA 安全的 (kmalloc 分配, 非栈变量!)
 */

/* ----------------------------------------------------------------------- */

/**
 * usb_fill_control_urb - initializes a control urb
 * @urb: pointer to the urb to initialize.
 * @dev: pointer to the struct usb_device for this urb.
 * @pipe: the endpoint pipe
 * @setup_packet: pointer to the setup_packet buffer. The buffer must be
 *	suitable for DMA.
 * @transfer_buffer: pointer to the transfer buffer. The buffer must be
 *	suitable for DMA.
 * @buffer_length: length of the transfer buffer
 * @complete_fn: pointer to the usb_complete_t function
 * @context: what to set the urb context to.
 *
 * Initializes a control urb with the proper information needed to submit
 * it to a device.
 *
 * The transfer buffer and the setup_packet buffer will most likely be filled
 * or read via DMA. The simplest way to get a buffer that can be DMAed to is
 * allocating it via kmalloc() or equivalent, even for very small buffers.
 * If the buffers are embedded in a bigger structure, there is a risk that
 * the buffer itself, the previous fields and/or the next fields are corrupted
 * due to cache incoherencies; or slowed down if they are evicted from the
 * cache. For more information, check &struct urb.
 *
 */
static inline void usb_fill_control_urb(struct urb *urb,
					struct usb_device *dev,
					unsigned int pipe,
					unsigned char *setup_packet,
					void *transfer_buffer,
					int buffer_length,
					usb_complete_t complete_fn,
					void *context)
{
	urb->dev = dev;
	urb->pipe = pipe;
	urb->setup_packet = setup_packet;
	urb->transfer_buffer = transfer_buffer;
	urb->transfer_buffer_length = buffer_length;
	urb->complete = complete_fn;
	urb->context = context;
}

/**
 * usb_fill_bulk_urb - macro to help initialize a bulk urb
 * @urb: pointer to the urb to initialize.
 * @dev: pointer to the struct usb_device for this urb.
 * @pipe: the endpoint pipe
 * @transfer_buffer: pointer to the transfer buffer. The buffer must be
 *	suitable for DMA.
 * @buffer_length: length of the transfer buffer
 * @complete_fn: pointer to the usb_complete_t function
 * @context: what to set the urb context to.
 *
 * Initializes a bulk urb with the proper information needed to submit it
 * to a device.
 *
 * Refer to usb_fill_control_urb() for a description of the requirements for
 * transfer_buffer.
 */
static inline void usb_fill_bulk_urb(struct urb *urb,
				     struct usb_device *dev,
				     unsigned int pipe,
				     void *transfer_buffer,
				     int buffer_length,
				     usb_complete_t complete_fn,
				     void *context)
{
	urb->dev = dev;
	urb->pipe = pipe;
	urb->transfer_buffer = transfer_buffer;
	urb->transfer_buffer_length = buffer_length;
	urb->complete = complete_fn;
	urb->context = context;
}

/**
 * usb_fill_int_urb - macro to help initialize a interrupt urb
 * @urb: pointer to the urb to initialize.
 * @dev: pointer to the struct usb_device for this urb.
 * @pipe: the endpoint pipe
 * @transfer_buffer: pointer to the transfer buffer. The buffer must be
 *	suitable for DMA.
 * @buffer_length: length of the transfer buffer
 * @complete_fn: pointer to the usb_complete_t function
 * @context: what to set the urb context to.
 * @interval: what to set the urb interval to, encoded like
 *	the endpoint descriptor's bInterval value.
 *
 * Initializes a interrupt urb with the proper information needed to submit
 * it to a device.
 *
 * Refer to usb_fill_control_urb() for a description of the requirements for
 * transfer_buffer.
 *
 * Note that High Speed and SuperSpeed(+) interrupt endpoints use a logarithmic
 * encoding of the endpoint interval, and express polling intervals in
 * microframes (eight per millisecond) rather than in frames (one per
 * millisecond).
 */
static inline void usb_fill_int_urb(struct urb *urb,
				    struct usb_device *dev,
				    unsigned int pipe,
				    void *transfer_buffer,
				    int buffer_length,
				    usb_complete_t complete_fn,
				    void *context,
				    int interval)
{
	urb->dev = dev;
	urb->pipe = pipe;
	urb->transfer_buffer = transfer_buffer;
	urb->transfer_buffer_length = buffer_length;
	urb->complete = complete_fn;
	urb->context = context;

	if (dev->speed == USB_SPEED_HIGH || dev->speed >= USB_SPEED_SUPER) {
		/* make sure interval is within allowed range */
		interval = clamp(interval, 1, 16);

		urb->interval = 1 << (interval - 1);
	} else {
		urb->interval = interval;
	}

	urb->start_frame = -1;
}

/*
 * ==========================================================================
 * URB 生命周期管理 API
 * ==========================================================================
 *
 * alloc → fill → submit → [complete callback] → free/重用
 *
 * usb_alloc_urb(iso_packets, gfp):
 *   分配 URB + iso_packets 个同步帧描述符 (非同步传 0)。
 *   返回的 URB 需要用 usb_free_urb() 释放。
 *
 * usb_submit_urb(urb, gfp):
 *   提交 URB 到 USB 核心 → HCD → 硬件。异步操作，立即返回。
 *   完成后调用 urb->complete(urb) 回调。
 *
 * usb_kill_urb / usb_unlink_urb:
 *   kill: 同步取消 (阻塞等待 URB 完成并调用 complete 回调)。
 *   unlink: 异步取消 (立即返回, URB 稍后完成)。
 *   区别: kill 保证回调已执行完毕才返回; unlink 不保证。
 *
 * usb_poison_urb / usb_unpoison_urb:
 *   poison: 标记 URB 为 "有毒" → 后续提交将失败。
 *   用于 disconnect() 时阻止新的 URB 提交。
 *
 * usb_get_urb / usb_free_urb (≡ usb_put_urb):
 *   引用计数管理。get 增加计数, free/put 减少并可能释放。
 */
extern void usb_init_urb(struct urb *urb);           // 初始化已分配的 URB 内存
extern struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags); // 分配 URB
extern void usb_free_urb(struct urb *urb);            // 释放 URB (减少引用计数)
#define usb_put_urb usb_free_urb                       // usb_free_urb 的别名
extern struct urb *usb_get_urb(struct urb *urb);       // 增加 URB 引用计数
extern int usb_submit_urb(struct urb *urb, gfp_t mem_flags); // 提交 URB (异步)
extern int usb_unlink_urb(struct urb *urb);            // 异步取消
extern void usb_kill_urb(struct urb *urb);             // 同步取消 (阻塞等待)
extern void usb_poison_urb(struct urb *urb);           // 标记 URB 为 (阻止新提交)
extern void usb_unpoison_urb(struct urb *urb);         // 解除 poisoned 状态
extern void usb_block_urb(struct urb *urb);            // usb_poison_urb 的别名
extern void usb_kill_anchored_urbs(struct usb_anchor *anchor); // 取消 anchor 中所有 URB
extern void usb_poison_anchored_urbs(struct usb_anchor *anchor); // 毒化 anchor 中所有 URB
extern void usb_unpoison_anchored_urbs(struct usb_anchor *anchor); // 解除毒化
extern void usb_anchor_suspend_wakeups(struct usb_anchor *anchor); // URB 完成时暂停唤醒
extern void usb_anchor_resume_wakeups(struct usb_anchor *anchor); // 恢复唤醒
extern void usb_anchor_urb(struct urb *urb, struct usb_anchor *anchor); // 将 URB 加入 anchor
extern void usb_unanchor_urb(struct urb *urb);         // 从 anchor 移除 URB
extern int usb_wait_anchor_empty_timeout(struct usb_anchor *anchor,
					 unsigned int timeout); // 等待 anchor 清空
extern struct urb *usb_get_from_anchor(struct usb_anchor *anchor); // 取出一个完成的 URB
extern void usb_scuttle_anchored_urbs(struct usb_anchor *anchor); // 强制清空 anchor
extern int usb_anchor_empty(struct usb_anchor *anchor); // anchor 是否为空

#define usb_unblock_urb	usb_unpoison_urb

/**
 * usb_urb_dir_in - check if an URB describes an IN transfer
 * @urb: URB to be checked
 *
 * Return: 1 if @urb describes an IN transfer (device-to-host),
 * otherwise 0.
 */
static inline int usb_urb_dir_in(struct urb *urb)
{
	return (urb->transfer_flags & URB_DIR_MASK) == URB_DIR_IN;
}

/**
 * usb_urb_dir_out - check if an URB describes an OUT transfer
 * @urb: URB to be checked
 *
 * Return: 1 if @urb describes an OUT transfer (host-to-device),
 * otherwise 0.
 */
static inline int usb_urb_dir_out(struct urb *urb)
{
	return (urb->transfer_flags & URB_DIR_MASK) == URB_DIR_OUT;
}

int usb_pipe_type_check(struct usb_device *dev, unsigned int pipe);
int usb_urb_ep_type_check(const struct urb *urb);

void *usb_alloc_coherent(struct usb_device *dev, size_t size,
	gfp_t mem_flags, dma_addr_t *dma);
void usb_free_coherent(struct usb_device *dev, size_t size,
	void *addr, dma_addr_t dma);

enum dma_data_direction;

void *usb_alloc_noncoherent(struct usb_device *dev, size_t size,
			    gfp_t mem_flags, dma_addr_t *dma,
			    enum dma_data_direction dir,
			    struct sg_table **table);
void usb_free_noncoherent(struct usb_device *dev, size_t size,
			  void *addr, enum dma_data_direction dir,
			  struct sg_table *table);

/*
 * ==========================================================================
 * 同步 USB 传输 API (阻塞式, 简化接口)
 * ==========================================================================
 *
 * 这些函数内部封装了 URB 的 alloc → fill → submit → wait → free 流程,
 * 让简单的 USB 传输变得方便。但它们会阻塞调用线程, 不能用于中断上下文。
 *
 * 控制传输 (usb_control_msg):
 *   发送 USB 控制请求 (Setup → Data → Status 三阶段)。
 *   request + requesttype + value + index 组成 8 字节 SETUP 包。
 *   返回值: ≥0 = 成功传输的字节数, <0 = 错误码。
 *
 * 批量传输 (usb_bulk_msg):
 *   同步批量 IN 或 OUT 传输。阻塞等待完成或超时。
 *   actual_length 返回实际传输字节数。
 *   _killable 版本: 可被致命信号中断 (fatal_signal_pending)。
 *
 * 中断传输 (usb_interrupt_msg):
 *   同步中断 IN 或 OUT 传输。类似 bulk 但端点类型是 INTERRUPT。
 *
 * usb_control_msg_send / usb_control_msg_recv (推荐的新接口):
 *   相比 usb_control_msg() 的参数更清晰:
 *   - endpoint 是 bEndpointAddress (如 0x81 表示 IN endpoint 1)
 *   - data 有 const (send 不需要修改缓冲区)
 *   - memflags 控制内存分配 (GFP_KERNEL / GFP_ATOMIC)
 */

/* Maximum value allowed for timeout in synchronous routines below */
#define USB_MAX_SYNCHRONOUS_TIMEOUT		60000	/* ms 最大超时 = 60 秒 */

extern int usb_control_msg(struct usb_device *dev, unsigned int pipe,
	__u8 request, __u8 requesttype, __u16 value, __u16 index,
	void *data, __u16 size, int timeout);  // 阻塞式控制传输
extern int usb_interrupt_msg(struct usb_device *usb_dev, unsigned int pipe,
	void *data, int len, int *actual_length, int timeout); // 阻塞式中断传输
extern int usb_bulk_msg(struct usb_device *usb_dev, unsigned int pipe,
	void *data, int len, int *actual_length, int timeout); // 阻塞式批量传输
extern int usb_bulk_msg_killable(struct usb_device *usb_dev, unsigned int pipe,
	void *data, int len, int *actual_length, int timeout); // 可被 signal 中断

/* usb_control_msg 的现代包装 (推荐使用) */
int usb_control_msg_send(struct usb_device *dev, __u8 endpoint, __u8 request,
			 __u8 requesttype, __u16 value, __u16 index,
			 const void *data, __u16 size, int timeout,
			 gfp_t memflags);  // 发送数据 (OUT)
int usb_control_msg_recv(struct usb_device *dev, __u8 endpoint, __u8 request,
			 __u8 requesttype, __u16 value, __u16 index,
			 void *data, __u16 size, int timeout,
			 gfp_t memflags);  // 接收数据 (IN)

// usb_get_descriptor — 获取 USB 描述符 (最常用的控制传输之一)
// desctype: USB_DT_DEVICE(1)/USB_DT_CONFIG(2)/USB_DT_STRING(3)/...
extern int usb_get_descriptor(struct usb_device *dev, unsigned char desctype,
	unsigned char descindex, void *buf, int size);
// usb_get_status — 获取设备/接口/端点状态
extern int usb_get_status(struct usb_device *dev,
	int recip, int type, int target, void *data);

static inline int usb_get_std_status(struct usb_device *dev,
	int recip, int target, void *data)
{
	return usb_get_status(dev, recip, USB_STATUS_TYPE_STANDARD, target,
		data);
}

static inline int usb_get_ptm_status(struct usb_device *dev, void *data)
{
	return usb_get_status(dev, USB_RECIP_DEVICE, USB_STATUS_TYPE_PTM,
		0, data);
}

extern int usb_string(struct usb_device *dev, int index,
	char *buf, size_t size);
extern char *usb_cache_string(struct usb_device *udev, int index);

/* wrappers that also update important state inside usbcore */
extern int usb_clear_halt(struct usb_device *dev, int pipe);
extern int usb_reset_configuration(struct usb_device *dev);
extern int usb_set_interface(struct usb_device *dev, int ifnum, int alternate);
extern void usb_reset_endpoint(struct usb_device *dev, unsigned int epaddr);

/* this request isn't really synchronous, but it belongs with the others */
extern int usb_driver_set_configuration(struct usb_device *udev, int config);

/* choose and set configuration for device */
extern int usb_choose_configuration(struct usb_device *udev);
extern int usb_set_configuration(struct usb_device *dev, int configuration);

/*
 * timeouts, in milliseconds, used for sending/receiving control messages
 * they typically complete within a few frames (msec) after they're issued
 * USB identifies 5 second timeouts, maybe more in a few cases, and a few
 * slow devices (like some MGE Ellipse UPSes) actually push that limit.
 */
#define USB_CTRL_GET_TIMEOUT	5000
#define USB_CTRL_SET_TIMEOUT	5000


/**
 * struct usb_sg_request - support for scatter/gather I/O
 * @status: zero indicates success, else negative errno
 * @bytes: counts bytes transferred.
 *
 * These requests are initialized using usb_sg_init(), and then are used
 * as request handles passed to usb_sg_wait() or usb_sg_cancel().  Most
 * members of the request object aren't for driver access.
 *
 * The status and bytecount values are valid only after usb_sg_wait()
 * returns.  If the status is zero, then the bytecount matches the total
 * from the request.
 *
 * After an error completion, drivers may need to clear a halt condition
 * on the endpoint.
 */
struct usb_sg_request {
	int			status;
	size_t			bytes;

	/* private:
	 * members below are private to usbcore,
	 * and are not provided for driver access!
	 */
	spinlock_t		lock;

	struct usb_device	*dev;
	int			pipe;

	int			entries;
	struct urb		**urbs;

	int			count;
	struct completion	complete;
};

int usb_sg_init(
	struct usb_sg_request	*io,
	struct usb_device	*dev,
	unsigned		pipe,
	unsigned		period,
	struct scatterlist	*sg,
	int			nents,
	size_t			length,
	gfp_t			mem_flags
);
void usb_sg_cancel(struct usb_sg_request *io);
void usb_sg_wait(struct usb_sg_request *io);


/* ----------------------------------------------------------------------- */

/*
 * For various legacy reasons, Linux has a small cookie that's paired with
 * a struct usb_device to identify an endpoint queue.  Queue characteristics
 * are defined by the endpoint's descriptor.  This cookie is called a "pipe",
 * an unsigned int encoded as:
 *
 *  - direction:	bit 7		(0 = Host-to-Device [Out],
 *					 1 = Device-to-Host [In] ...
 *					like endpoint bEndpointAddress)
 *  - device address:	bits 8-14       ... bit positions known to uhci-hcd
 *  - endpoint:		bits 15-18      ... bit positions known to uhci-hcd
 *  - pipe type:	bits 30-31	(00 = isochronous, 01 = interrupt,
 *					 10 = control, 11 = bulk)
 *
 * Given the device address and endpoint descriptor, pipes are redundant.
 */

/*
 * PIPE (管道) — USB 端点地址编码
 *
 * URB 通过一个 32-bit 的 "pipe" 字段编码目标端点:
 *   bits 30-31: 传输类型 (PIPE_CONTROL/BULK/INTERRUPT/ISOCHRONOUS)
 *   bits 15-18: 端点号 (0-15)
 *   bits  8-14: 设备地址 (1-127)
 *   bit     7:  方向 (USB_DIR_IN=0x80 表示 IN, 0 表示 OUT)
 *
 * 注意: PIPE_* 不同于 USB 描述符中的 USB_ENDPOINT_XFER_* 值!
 *       PIPE_CONTROL=2  vs  USB_ENDPOINT_XFER_CONTROL=0
 *       这是 usbfs 遗留的编码，不要混淆。
 */
/* NOTE:  these are not the standard USB_ENDPOINT_XFER_* values!! */
/* (yet ... they're the values used by usbfs) */
#define PIPE_ISOCHRONOUS		0   // 同步传输
#define PIPE_INTERRUPT			1   // 中断传输
#define PIPE_CONTROL			2   // 控制传输
#define PIPE_BULK			3   // 批量传输

// 方向判断: bit7=1 → IN(设备→主机), bit7=0 → OUT(主机→设备)
#define usb_pipein(pipe)	((pipe) & USB_DIR_IN)
#define usb_pipeout(pipe)	(!usb_pipein(pipe))

// 从 pipe 中提取设备地址 (bits 8-14) 和端点号 (bits 15-18)
#define usb_pipedevice(pipe)	(((pipe) >> 8) & 0x7f)
#define usb_pipeendpoint(pipe)	(((pipe) >> 15) & 0xf)

// 从 pipe 中提取传输类型 (bits 30-31)
#define usb_pipetype(pipe)	(((pipe) >> 30) & 3)
#define usb_pipeisoc(pipe)	(usb_pipetype((pipe)) == PIPE_ISOCHRONOUS)
#define usb_pipeint(pipe)	(usb_pipetype((pipe)) == PIPE_INTERRUPT)
#define usb_pipecontrol(pipe)	(usb_pipetype((pipe)) == PIPE_CONTROL)
#define usb_pipebulk(pipe)	(usb_pipetype((pipe)) == PIPE_BULK)

// __create_pipe: 将设备地址和端点号编码到 pipe 的低位
static inline unsigned int __create_pipe(struct usb_device *dev,
		unsigned int endpoint)
{
	return (dev->devnum << 8) | (endpoint << 15);
}

/*
 * Pipe 创建宏 — USB 传输管道的标准构建方式
 *
 * 命名规则: usb_{snd,rcv}{ctrl,bulk,int,isoc}pipe
 *   snd = 发送 (OUT, 主机→设备)
 *   rcv = 接收 (IN, 设备→主机)
 *
 * 用法: pipe = usb_rcvbulkpipe(udev, endpoint_addr);  // 读批量数据
 *       pipe = usb_sndctrlpipe(udev, 0);              // 控制传输到端点0
 */
#define usb_sndctrlpipe(dev, endpoint)	\
	((PIPE_CONTROL << 30) | __create_pipe(dev, endpoint))
#define usb_rcvctrlpipe(dev, endpoint)	\
	((PIPE_CONTROL << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)
#define usb_sndisocpipe(dev, endpoint)	\
	((PIPE_ISOCHRONOUS << 30) | __create_pipe(dev, endpoint))
#define usb_rcvisocpipe(dev, endpoint)	\
	((PIPE_ISOCHRONOUS << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)
#define usb_sndbulkpipe(dev, endpoint)	\
	((PIPE_BULK << 30) | __create_pipe(dev, endpoint))
#define usb_rcvbulkpipe(dev, endpoint)	\
	((PIPE_BULK << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)
#define usb_sndintpipe(dev, endpoint)	\
	((PIPE_INTERRUPT << 30) | __create_pipe(dev, endpoint))
#define usb_rcvintpipe(dev, endpoint)	\
	((PIPE_INTERRUPT << 30) | __create_pipe(dev, endpoint) | USB_DIR_IN)

static inline struct usb_host_endpoint *
usb_pipe_endpoint(struct usb_device *dev, unsigned int pipe)
{
	struct usb_host_endpoint **eps;
	eps = usb_pipein(pipe) ? dev->ep_in : dev->ep_out;
	return eps[usb_pipeendpoint(pipe)];
}

static inline u16 usb_maxpacket(struct usb_device *udev, int pipe)
{
	struct usb_host_endpoint *ep = usb_pipe_endpoint(udev, pipe);

	if (!ep)
		return 0;

	/* NOTE:  only 0x07ff bits are for packet size... */
	return usb_endpoint_maxp(&ep->desc);
}

u32 usb_endpoint_max_periodic_payload(struct usb_device *udev,
				      const struct usb_host_endpoint *ep);

bool usb_endpoint_is_hs_isoc_double(struct usb_device *udev,
				    const struct usb_host_endpoint *ep);

/* translate USB error codes to codes user space understands */
static inline int usb_translate_errors(int error_code)
{
	switch (error_code) {
	case 0:
	case -ENOMEM:
	case -ENODEV:
	case -EOPNOTSUPP:
		return error_code;
	case -ENOSPC:
		return -EBUSY;
	default:
		return -EIO;
	}
}

/* Events from the usb core */
#define USB_DEVICE_ADD		0x0001
#define USB_DEVICE_REMOVE	0x0002
#define USB_BUS_ADD		0x0003
#define USB_BUS_REMOVE		0x0004
extern void usb_register_notify(struct notifier_block *nb);
extern void usb_unregister_notify(struct notifier_block *nb);

/* debugfs stuff */
extern struct dentry *usb_debug_root;

/* LED triggers */
enum usb_led_event {
	USB_LED_EVENT_HOST = 0,
	USB_LED_EVENT_GADGET = 1,
};

#ifdef CONFIG_USB_LED_TRIG
extern void usb_led_activity(enum usb_led_event ev);
#else
static inline void usb_led_activity(enum usb_led_event ev) {}
#endif

#endif  /* __KERNEL__ */

#endif
