// SPDX-License-Identifier: GPL-2.0
/*
 * USB Skeleton driver - 2.2
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c
 * but has been rewritten to be easier to read and use.
 */

/*
 * ============================================================================
 * USB 驱动完整教学模板 (USB Skeleton Driver)
 * ============================================================================
 *
 * 本文件是 Linux 内核 USB 驱动开发的完整模板/教学文件，由 Greg Kroah-Hartman
 * 维护，旨在为开发者提供一个可直接参考和修改的 USB 驱动框架。
 * 它展示了 USB 驱动开发中几乎所有关键模式和核心机制：
 *
 * 一、驱动注册与注销
 *   - module_usb_driver() 宏：自动生成 module_init 和 module_exit
 *   - usb_register() / usb_deregister()：向 USB 核心层注册/注销驱动
 *   - struct usb_driver：驱动的主要结构体，包含所有回调函数指针
 *
 * 二、设备热插拔管理 (probe / disconnect)
 *   - skel_probe(): 当 USB 设备插入时被调用，完成设备初始化
 *   - skel_disconnect(): 当 USB 设备拔出时被调用，完成资源清理
 *   - 这是 USB 驱动最核心的两个回调，处理设备的整个生命周期
 *
 * 三、批量传输 (Bulk Transfer)
 *   - 批量端点适用于大数据量传输，如 U 盘、串口等
 *   - 批量 IN (设备->主机): usb_submit_urb() 提交 URB，异步接收数据
 *   - 批量 OUT (主机->设备): usb_submit_urb() 提交 URB，同步/异步发送
 *   - 批量传输不保证实时性，但保证数据完整性（通过 CRC 校验和重传）
 *
 * 四、URB (USB Request Block) 机制
 *   - URB 是 USB 数据传输的核心抽象，类似于网络协议中的 sk_buff
 *   - usb_fill_bulk_urb(): 初始化 URB，设置端点、缓冲区、回调函数
 *   - usb_submit_urb(): 提交 URB 到 USB 核心层，开始传输
 *   - 回调函数 (skel_read_bulk_callback / skel_write_bulk_callback):
 *     在 URB 完成（成功/失败/取消）时被调用，运行在中断上下文
 *   - usb_alloc_urb() / usb_free_urb(): URB 的分配与释放
 *   - usb_anchor_urb() / usb_unanchor_urb(): URB 锚点管理
 *   - usb_kill_urb(): 同步杀死 URB，等待其完全停止
 *
 * 五、控制传输 (Control Transfer)
 *   - 控制端点 (端点 0) 是所有 USB 设备都必须支持的默认端点
 *   - 用于设备枚举、配置查询、标准请求等
 *   - 虽然本模板主要展示批量传输，但控制传输是 USB 协议的基础
 *
 * 六、中断传输 (Interrupt Transfer)
 *   - 中断端点适用于小数据量的周期性传输，如键盘、鼠标
 *   - 中断传输具有固定的轮询间隔 (bInterval)，保证一定的实时性
 *   - 与批量传输的主要区别在于：中断传输有固定的时间保证
 *
 * 七、用户空间接口 (file_operations)
 *   - skel_fops: 定义 open/read/write/release/flush 回调
 *   - usb_register_dev(): 在 /dev/ 下创建设备节点 (如 /dev/skel0)
 *   - usb_deregister_dev(): 移除设备节点
 *   - usb_find_interface(): 通过次设备号找到对应的 usb_interface
 *   - copy_to_user() / copy_from_user(): 内核与用户空间的数据交换
 *
 * 八、电源管理 (suspend / resume)
 *   - skel_suspend(): 系统挂起时调用，停止所有正在进行的 URB
 *   - skel_resume(): 系统恢复时调用，重新初始化设备
 *   - usb_autopm_get_interface() / usb_autopm_put_interface():
 *     自动挂起管理的引用计数，防止设备在操作过程中进入低功耗模式
 *   - supports_autosuspend = 1: 声明驱动支持自动挂起
 *
 * 九、并发与同步机制
 *   - io_mutex: 互斥锁，保护 I/O 操作与 disconnect 的同步
 *   - err_lock: 自旋锁，保护中断上下文中访问的错误状态
 *   - limit_sem: 信号量，限制并发写操作的数量，防止内存耗尽
 *   - kref: 引用计数器，跟踪结构体的使用情况，防止过早释放
 *   - wait_queue_head_t: 等待队列，实现阻塞 I/O 的唤醒机制
 *
 * 十、struct usb_device_id 与 MODULE_DEVICE_TABLE
 *   - skel_table[]: 声明此驱动支持的 USB 设备列表
 *   - USB_DEVICE(vendor, product): 宏，构造 vendor:product 匹配项
 *   - MODULE_DEVICE_TABLE(usb, skel_table): 导出设备表，
 *     hotplug 系统 (udev) 通过此表在设备插入时自动加载对应驱动
 *
 * 十一、批量端点 (BULK) 与中断端点 (INTERRUPT) 的区别
 *   - BULK: 大数据量传输，无实时性保证，但有错误重传机制
 *           适用于批量传输而不在乎延迟的场景 (U盘、打印机)
 *   - INTERRUPT: 小数据量，有固定轮询间隔 (bInterval)
 *               适用于需要周期性轮询且数据量小的场景 (键盘、鼠标)
 *   - 由端点描述符中的 bmAttributes 字段区分：
 *     0x02 = BULK, 0x03 = INTERRUPT
 *
 * 十二、驱动的完整生命周期
 *   module_init
 *     -> usb_register() 注册驱动到 USB 核心
 *       [设备插入]
 *       -> probe() -> skel_probe()
 *         -> kzalloc 分配私有结构体
 *         -> 查找端点、分配缓冲区、分配 URB
 *         -> usb_set_intfdata() -> usb_register_dev()
 *       [用户打开设备]
 *       -> skel_open() -> kref_get()
 *       [应用程序读写]
 *       -> skel_read() / skel_write()
 *       [用户关闭设备]
 *       -> skel_release() -> kref_put()
 *       [设备拔出]
 *       -> disconnect() -> skel_disconnect()
 *         -> usb_deregister_dev() -> 停止所有 URB -> kref_put()
 *   module_exit
 *     -> usb_deregister() 从 USB 核心注销驱动
 *
 * 参考资料:
 *   - Documentation/usb/usb-skeleton.txt (内核源码文档)
 *   - Linux 内核源码 drivers/usb/core/ 下的 USB 核心实现
 *   - USB 规范 (USB 2.0 / USB 3.0 规范文档)
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>


/*
 * ==============================================================
 * USB 设备标识配置 — VID/PID 与 usb_device_id 表
 * ==============================================================
 *
 * 每个 USB 设备都有唯一的厂商 ID (Vendor ID, VID) 和产品 ID
 * (Product ID, PID)，用于在设备插入时匹配对应的驱动程序。
 *
 * USB_DEVICE(vendor, product) 宏:
 *   构造一个 usb_device_id 表项，要求 VID 和 PID 完全匹配。
 *   还有其它匹配方式:
 *   - USB_DEVICE_VER(v, p, lo, hi): 匹配特定版本范围的设备
 *   - USB_DEVICE_INFO(class, subclass, protocol): 匹配设备类
 *   - USB_DEVICE_INTERFACE_CLASS(class): 匹配接口类
 *   详见 include/linux/usb.h 中的定义。
 *
 * MODULE_DEVICE_TABLE(usb, skel_table):
 *   将设备 ID 表导出到模块的 .modinfo 段。
 *   depmod/udev/mdev 等工具读取此信息，构建驱动-设备映射数据库。
 *   当新 USB 设备插入时，udev 根据 VID:PID 搜索此数据库，
 *   自动加载匹配的驱动模块。
 *
 * 注意:
 *   VID 0xfff0 是示例用的占位值，不是真实的厂商 ID。
 *   真实驱动的 VID 需要向 USB-IF (USB Implementers Forum)
 *   申请或使用厂商分配的 ID。
 *   skel_table[] 必须以空项 { } 终止，USB 核心层通过空项
 *   判断表结束。
 */

/* Define these values to match your devices */
#define USB_SKEL_VENDOR_ID	0xfff0
#define USB_SKEL_PRODUCT_ID	0xfff0

/* table of devices that work with this driver */
static const struct usb_device_id skel_table[] = {
	{ USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, skel_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_SKEL_MINOR_BASE	192

/* our private defines. if this grows any larger, use your own .h file */

/*
 * MAX_TRANSFER — 单次传输的最大数据量
 *
 * 定义为 (PAGE_SIZE - 512)，约 3.5KB (PAGE_SIZE 通常为 4096)。
 * 选择此值的原因:
 * 1. 避免分配超过 PAGE_SIZE 的缓冲区，减少对虚拟内存管理 (VM) 的压力
 * 2. EHCI (USB 2.0) 控制器最大数据包为 512 字节，
 *    PAGE_SIZE - 512 保证传输的数据包数量为整数
 * 3. 较大的传输可以分成多个 URB 提交，而不是使用巨大的单次传输
 */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/*
 * MAX_TRANSFER is chosen so that the VM is not stressed by
 * allocations > PAGE_SIZE and the number of packets in a page
 * is an integer 512 is the largest possible packet on EHCI
 */

/*
 * WRITES_IN_FLIGHT — 并发写操作的最大数量
 *
 * 限制同时进行的写 URB 数量，防止用户空间程序通过
 * 大量并发 write() 调用耗尽系统内存 (每个写 URB 都
 * 需要分配 DMA 缓冲区)。
 *
 * 当 WRITES_IN_FLIGHT 个写 URB 都在飞行中时，下一个
 * write() 调用会被阻塞 (或 O_NONBLOCK 模式下返回 -EAGAIN)，
 * 直到某个写 URB 完成并释放槽位。
 *
 * limit_sem 信号量实现此限制:
 * - 初始值 = WRITES_IN_FLIGHT (8)
 * - 每个写操作: down_interruptible() 减少计数
 * - 每个写完成: up() 增加计数 (在 skel_write_bulk_callback 中)
 */
#define WRITES_IN_FLIGHT	8
/* arbitrarily chosen */

/* Structure to hold all of our device specific stuff */
struct usb_skel {
	struct usb_device	*udev;			/* the usb device for this device */
	/* 指向与此驱动关联的 USB 设备结构体，通过 interface_to_usbdev() 获得。
	 * usb_get_dev() 增加引用计数，确保设备在驱动使用期间不会被释放。 */
	struct usb_interface	*interface;		/* the interface for this device */
	/* 指向此设备所属的 USB 接口描述符。驱动通过 interface 与 USB 核心层交互，
	 * 如提交 URB、注册/注销设备节点等。usb_get_intf() 增加接口引用计数。 */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	/* 计数信号量，限制并发写操作的数量 (最大 WRITES_IN_FLIGHT = 8)，
	 * 防止用户空间程序通过大量并发写操作耗尽系统内存。
	 * down_interruptible() 获取 (P 操作)，up() 释放 (V 操作)。 */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	/* USB 请求锚点，跟踪所有已提交但尚未完成的写 URB。
	 * 当设备断开连接时，通过 usb_kill_anchored_urbs() 或
	 * usb_unlink_anchored_urbs() 统一取消所有正在进行的 URB。 */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	/* 批量 IN 端点使用的 URB 指针，用于从 USB 设备读取数据。
	 * 在 probe() 中通过 usb_alloc_urb() 分配，在 skel_delete() 中释放。
	 * 每次读操作前通过 usb_fill_bulk_urb() 重新初始化。 */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
	/* 批量 IN 数据的接收缓冲区，通过 kmalloc() 分配。
	 * 缓冲区大小等于批量 IN 端点的 wMaxPacketSize (最大数据包长度)。 */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	/* 接收缓冲区的大小，等于批量 IN 端点的最大数据包长度
	 * (从端点描述符的 wMaxPacketSize 字段获取)。
	 * 决定了单次 URB 传输能接收的最大数据量。 */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	/* 上一次 URB 完成时实际接收到的字节数。
	 * 在 skel_read_bulk_callback() 中由 urb->actual_length 填充，
	 * 在 skel_read() 中用于判断是否有数据可读。 */
	size_t			bulk_in_copied;		/* already copied to user space */
	/* 已经从 bulk_in_buffer 复制到用户空间的字节数。
	 * 用于处理部分读取的情况：当一次 URB 接收的数据量大于
	 * 用户请求的读取量时，剩余数据可下次读取。 */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	/* 批量 IN 端点的地址 (高位为 1 表示 USB_DIR_IN 方向)。
	 * 用于 usb_rcvbulkpipe() 宏构造 USB 管道 (pipe)，
	 * 该管道用于 usb_fill_bulk_urb() 的批量读操作。 */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	/* 批量 OUT 端点的地址 (高位为 0 表示 USB_DIR_OUT 方向)。
	 * 用于 usb_sndbulkpipe() 宏构造 USB 管道，
	 * 该管道用于 usb_fill_bulk_urb() 的批量写操作。 */
	int			errors;			/* the last request tanked */
	/* 最近一次 URB 传输的错误码。由 URB 回调函数在 urb->status 非零时设置。
	 * 在每次读写操作开始时检查 (报告一次后即清零)，
	 * 将底层 USB 错误传递到用户空间。 */
	bool			ongoing_read;		/* a read is going on */
	/* 布尔标志，标记当前是否有正在进行的批量读 URB。
	 * 由 err_lock 自旋锁保护。用于避免多个读操作并发提交 URB，
	 * 以及在等待读完成时判断是否需要阻塞。 */
	spinlock_t		err_lock;		/* lock for errors */
	/* 自旋锁，保护 errors 和 ongoing_read 两个字段的并发访问。
	 * 之所以使用自旋锁而非互斥锁，是因为 URB 回调函数在
	 * 中断上下文中执行，在中断上下文中只能使用自旋锁。 */
	struct kref		kref;
	/* 内核引用计数器 (reference counter)，跟踪 struct usb_skel 的
	 * 使用者数量。每次 skel_open() 调用 kref_get() 增加计数，
	 * 每次 skel_release() 调用 kref_put() 减少计数。
	 * 当计数降为 0 时，自动调用 skel_delete() 释放结构体。
	 * 这是防止 use-after-free 的关键机制：即使设备断开连接，
	 * 只要仍有打开的文件描述符引用此结构体，它就不会被释放。 */
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	/* 互斥锁，用于同步 I/O 操作 (read/write) 与设备断开操作 (disconnect)。
	 * 读/写操作前加锁，完成后解锁。disconnect() 中加锁后设置
	 * disconnected 标志，确保不会有新的 I/O 操作在断线后启动。
	 * 这是防止 use-after-free 的核心同步机制。 */
	unsigned long		disconnected:1;
	/* 位域标志 (仅用 1 位)，标记设备是否已经断开连接。
	 * 由 io_mutex 保护。在 probe() 中初始化为 0，
	 * 在 disconnect() 中设置为 1。
	 * 读/写操作在获取 io_mutex 后检查此标志，
	 * 若设备已断开则立即返回 -ENODEV。 */
	wait_queue_head_t	bulk_in_wait;		/* to wait for an ongoing read */
	/* 等待队列头，用于读操作的阻塞等待机制。
	 * 当 skel_read() 发现已有正在进行的读操作时，调用
	 * wait_event_interruptible() 在此队列上睡眠等待。
	 * 当 URB 完成时，skel_read_bulk_callback() 调用
	 * wake_up_interruptible() 唤醒等待的读进程。 */
};
#define to_skel_dev(d) container_of(d, struct usb_skel, kref)

static struct usb_driver skel_driver;
static void skel_draw_down(struct usb_skel *dev);

/*
 * ==============================================================
 * skel_delete() — 私有结构体析构函数 (kref 回调)
 * ==============================================================
 *
 * 当 struct usb_skel 的引用计数降为 0 时，kref_put() 自动调用
 * 此函数释放所有已分配的资源。
 *
 * 这是 USB 驱动中引用计数管理的典型模式:
 *   - skel_delete 注册为 kref 的 release 回调
 *   - 任何需要引用计数保护的结构体都使用此模式
 *   - 资源释放集中在此函数中，确保不会遗漏
 *
 * 释放顺序 (与分配顺序相反):
 * 1. usb_free_urb(dev->bulk_in_urb): 释放批量读 URB
 * 2. usb_put_intf(dev->interface): 释放 USB 接口引用
 * 3. usb_put_dev(dev->udev): 释放 USB 设备引用
 * 4. kfree(dev->bulk_in_buffer): 释放批量 IN 缓冲区
 * 5. kfree(dev): 释放结构体本身
 *
 * 注意: 写 URB 不在 skel_delete 中释放，因为写 URB
 * 是在 skel_write() 中动态分配、在 skel_write_bulk_callback()
 * 中释放的。写 URB 的生命周期在提交后由 USB 核心层管理。
 */
static void skel_delete(struct kref *kref)
{
	struct usb_skel *dev = to_skel_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}

/*
 * ==============================================================
 * skel_open() — 设备文件打开操作 (kref 引用计数 + 自动挂起)
 * ==============================================================
 *
 * 当用户空间程序调用 open("/dev/skel0", ...) 时，VFS 层通过
 * skel_fops 跳转到此函数。执行流程如下：
 *
 * 1. usb_find_interface(skel_driver, subminor):
 *    通过次设备号 (subminor) 反向查找对应的 usb_interface。
 *    这是从字符设备号映射回 USB 设备的关键步骤。
 *
 * 2. usb_get_intfdata(interface):
 *    从 interface 中取出之前在 probe() 中通过 usb_set_intfdata()
 *    设置的驱动私有数据 (struct usb_skel 指针)。
 *
 * 3. usb_autopm_get_interface(interface):
 *    增加 USB 自动挂管理的引用计数，防止在设备打开期间
 *    系统将 USB 设备挂起进入低功耗模式。
 *    对应 skel_release() 中的 usb_autopm_put_interface()。
 *
 * 4. kref_get(&dev->kref):
 *    增加 struct usb_skel 的引用计数。只要设备被打开，
 *    引用计数保持 >= 1，即使设备断开连接，skel_delete()
 *    也不会被调用，结构体不会被释放。
 *    对应 skel_release() 中的 kref_put(&dev->kref, skel_delete)。
 *
 * 引用计数 (kref) 的核心作用:
 *   - kref_get(): 每次 open 时增加计数，表示多了一个使用者
 *   - kref_put(): 每次 release 时减少计数，并检查是否降为 0
 *   - 当计数降为 0 时自动调用 skel_delete() 释放内存
 *   - 这是 use-after-free 的第一道防线:
 *     即使 disconnect() 被调用，只要仍有 open 的文件描述符，
 *     struct usb_skel 就不会被释放。
 */
static int skel_open(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&skel_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;

	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data = dev;

exit:
	return retval;
}

/*
 * ==============================================================
 * skel_release() — 设备文件关闭操作 (kref 引用计数释放)
 * ==============================================================
 *
 * 当用户空间程序调用 close() 或进程退出时，VFS 层调用此函数。
 * 这是 skel_open() 的逆操作：
 *
 * 1. usb_autopm_put_interface(dev->interface):
 *    释放自动挂起的引用计数。如果所有使用者都已释放，
 *    系统可以安全地将 USB 设备挂起以节省功耗。
 *
 * 2. kref_put(&dev->kref, skel_delete):
 *    减少 struct usb_skel 的引用计数。当计数降为 0 时，
 *    自动调用 skel_delete() 释放所有已分配的资源。
 *    注意：kref_put() 返回 1 表示结构体已被释放，返回 0 表示
 *    还有其它持有者。调用者无需关心释放时机——这是 kref 机制
 *    自动管理的。
 */
static int skel_release(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* allow the device to be autosuspended */
	usb_autopm_put_interface(dev->interface);

	/* decrement the count on our device */
	kref_put(&dev->kref, skel_delete);
	return 0;
}

/*
 * ==============================================================
 * skel_flush() — 刷新文件描述符 (fsync / close 时调用)
 * ==============================================================
 *
 * 当用户空间程序调用 fsync() 或 close() 时，VFS 层调用此函数。
 * 它的主要职责是确保所有之前提交的写 URB 都已经完成，
 * 数据已发送到 USB 设备。
 *
 * 执行流程:
 * 1. skel_draw_down(dev): 等待所有飞行中的写 URB 完成
 *    (参见 skel_draw_down 的详细注释)
 * 2. 检查 dev->errors: 返回上一次传输的错误码给用户空间
 * 3. 如果错误是 -EPIPE (端点停止)，返回 -EPIPE
 *    否则返回 -EIO (通用的 I/O 错误)
 * 4. 清空 dev->errors，为后续操作准备干净的初始状态
 *
 * 注意: skel_flush() 在 close() 时也会被调用，即使应用
 * 没有显式调用 fsync()。这意味着 close() 会阻塞等待所有
 * 写 URB 完成 —— 这是确保数据完整性的重要设计。
 */
static int skel_flush(struct file *file, fl_owner_t id)
{
	struct usb_skel *dev;
	int res;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* wait for io to stop */
	mutex_lock(&dev->io_mutex);
	skel_draw_down(dev);

	/* read out errors, leave subsequent opens a clean slate */
	spin_lock_irq(&dev->err_lock);
	res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
	dev->errors = 0;
	spin_unlock_irq(&dev->err_lock);

	mutex_unlock(&dev->io_mutex);

	return res;
}

/*
 * ==============================================================
 * skel_read_bulk_callback() — 批量读 URB 完成回调函数
 * ==============================================================
 *
 * 此函数是批量 IN 传输的 URB 完成回调，在 USB 核心层处理完
 * URB 后自动调用。注意：此函数运行在中断上下文 (softirq) 中，
 * 因此不能调用可能睡眠的函数 (如 copy_to_user、kmalloc with GFP_KERNEL)。
 *
 * URB 完成回调是 USB 驱动中最关键的异步模式之一：
 *
 * 回调的可能状态 (urb->status):
 *
 *   0            — 传输成功。urb->actual_length 包含实际接收到的字节数
 *   -ENOENT      — URB 被 usb_kill_urb() 同步取消 (正常取消，非错误)
 *   -ECONNRESET  — URB 被 usb_unlink_urb() 异步取消 (正常取消，非错误)
 *   -ESHUTDOWN   — 设备已被移除或 USB 控制器已停机 (非错误)
 *   其他负值     — 真正的传输错误 (如 -EPIPE 表示端点被停止)
 *
 * 处理流程：
 *
 * 1. 检查 urb->status:
 *    - 如果是正常的取消/设备移除状态 (-ENOENT, -ECONNRESET, -ESHUTDOWN)，
 *      仅记录日志，不视为错误
 *    - 如果是真正的错误，保存到 dev->errors 供 read() 检查
 *    - 如果传输成功 (status == 0)，将 actual_length 保存到
 *      dev->bulk_in_filled，供 skel_read() 使用
 *
 * 2. 设置 dev->ongoing_read = 0:
 *    标记读操作已完成，允许后续读操作提交新的 URB
 *
 * 3. wake_up_interruptible(&dev->bulk_in_wait):
 *    唤醒在 skel_read() 中通过 wait_event_interruptible() 等待
 *    读操作完成的进程。这是典型的"等待队列"同步模式：
 *    - 读进程在 skel_read() 中睡眠等待
 *    - URB 完成后在此回调中唤醒读进程
 *    - 读进程继续执行并将数据复制到用户空间
 */
static void skel_read_bulk_callback(struct urb *urb)
{
	struct usb_skel *dev;
	unsigned long flags;

	dev = urb->context;

	spin_lock_irqsave(&dev->err_lock, flags);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = 0;
	spin_unlock_irqrestore(&dev->err_lock, flags);

	wake_up_interruptible(&dev->bulk_in_wait);
}

/*
 * ==============================================================
 * skel_do_read_io() — 发起批量读 URB 传输
 * ==============================================================
 *
 * 此函数执行实际的 USB 批量读操作，被 skel_read() 调用。
 *
 * 执行步骤：
 *
 * 1. usb_fill_bulk_urb():
 *    填充/初始化批量读 URB 的各字段：
 *    - dev->bulk_in_urb: 在 probe() 中预分配的 URB
 *    - usb_rcvbulkpipe(): 构造批量 IN 管道 (包含设备地址 + 端点号 + 方向)
 *    - dev->bulk_in_buffer: 接收数据的 DMA 安全缓冲区
 *    - count: 请求读取的字节数
 *    - skel_read_bulk_callback: 完成回调函数
 *    - dev: 回调上下文参数 (在回调中通过 urb->context 获取)
 *
 * 2. 设置 dev->ongoing_read = 1:
 *    在 err_lock 保护下设置"正在读"标志，
 *    阻止其他读操作同时提交 URB。
 *
 * 3. usb_submit_urb():
 *    将 URB 提交给 USB 核心层。这是将读请求真正发送到
 *    USB 控制器硬件的关键步骤。
 *    提交后立即返回 (异步)，不会等待传输完成。
 *    实际数据传输由 USB 控制器在后台进行。
 *
 * 4. 错误处理:
 *    如果 usb_submit_urb() 失败 (如 -ENOMEM 内存不足)，
 *    清除 ongoing_read 标志并返回错误码。
 */
static int skel_do_read_io(struct usb_skel *dev, size_t count)
{
	int rv;

	/* prepare a read */
	usb_fill_bulk_urb(dev->bulk_in_urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			min(dev->bulk_in_size, count),
			skel_read_bulk_callback,
			dev);
	/* tell everybody to leave the URB alone */
	spin_lock_irq(&dev->err_lock);
	dev->ongoing_read = 1;
	spin_unlock_irq(&dev->err_lock);

	/* submit bulk in urb, which means no data to deliver */
	dev->bulk_in_filled = 0;
	dev->bulk_in_copied = 0;

	/* do it */
	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, rv);
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&dev->err_lock);
		dev->ongoing_read = 0;
		spin_unlock_irq(&dev->err_lock);
	}

	return rv;
}

/*
 * ==============================================================
 * skel_read() — 从 USB 设备读取数据的阻塞读操作
 * ==============================================================
 *
 * 这是 USB 驱动的核心读路径，展示了典型的"提交 URB → 等待完成
 * → 复制到用户空间"异步读模式。理解此函数是理解 USB 驱动 I/O
 * 的关键。
 *
 * 整体流程:
 *
 * 1. 获取 io_mutex 互斥锁 (mutex_lock_interruptible):
 *    确保与 disconnect() 互斥。如果设备正在断开连接，此锁
 *    可以防止读操作与资源释放并发执行。
 *
 * 2. 检查 dev->disconnected 标志:
 *    如果设备已断开，直接返回 -ENODEV。
 *    这是 use-after-free 的第二道防线 (第一道是 kref)。
 *
 * 3. 检查是否有正在进行的读操作 (ongoing_read):
 *    - 如果有正在进行的读:
 *      a. 非阻塞模式 (O_NONBLOCK): 返回 -EAGAIN
 *      b. 阻塞模式: wait_event_interruptible() 睡眠等待
 *         在 bulk_in_wait 队列上，直到 ongoing_read 变为 0
 *         (由 skel_read_bulk_callback() 在 URB 完成时唤醒)
 *
 * 4. 检查错误状态 (dev->errors):
 *    - 如果存在未报告的错误，清空并返回错误码给用户空间
 *    - 错误报告一次后即清零 ("报告一次"机制)
 *
 * 5. 检查缓冲区是否有数据 (bulk_in_filled):
 *    - 如果缓冲区有数据:
 *      a. 计算可用的字节数: available = bulk_in_filled - bulk_in_copied
 *      b. copy_to_user() 将数据从内核空间复制到用户空间
 *      c. 更新 bulk_in_copied 跟踪已复制的字节数
 *      d. 如果还有更多数据要读且缓冲区已空，发起新的 URB
 *    - 如果缓冲区为空:
 *      a. 调用 skel_do_read_io() 提交新的读 URB
 *      b. 跳转到 retry 重新等待 URB 完成
 *
 * 关键设计点:
 *   - 批量传输是异步的: usb_submit_urb() 立即返回，
 *     实际数据传输在后台进行，完成时通过回调通知
 *   - 等待队列提供同步点: 将异步的 URB 完成转换为
 *     同步的 read() 系统调用返回
 *   - bulk_in_copied 支持部分读取: 如果用户请求的字节数
 *     小于 URB 接收的字节数，剩余数据可以下次读取
 *   - O_NONBLOCK 支持: 非阻塞模式下不会睡眠等待
 */
static ssize_t skel_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	struct usb_skel *dev;
	int rv;
	bool ongoing_io;

	dev = file->private_data;

	if (!count)
		return 0;

	/* no concurrent readers */
	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0)
		return rv;

	if (dev->disconnected) {		/* disconnect() was called */
		rv = -ENODEV;
		goto exit;
	}

	/* if IO is under way, we must not touch things */
retry:
	spin_lock_irq(&dev->err_lock);
	ongoing_io = dev->ongoing_read;
	spin_unlock_irq(&dev->err_lock);

	if (ongoing_io) {
		/* nonblocking IO shall not wait */
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto exit;
		}
		/*
		 * IO may take forever
		 * hence wait in an interruptible state
		 */
		rv = wait_event_interruptible(dev->bulk_in_wait, (!dev->ongoing_read));
		if (rv < 0)
			goto exit;
	}

	/* errors must be reported */
	rv = dev->errors;
	if (rv < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		rv = (rv == -EPIPE) ? rv : -EIO;
		/* report it */
		goto exit;
	}

	/*
	 * if the buffer is filled we may satisfy the read
	 * else we need to start IO
	 */

	if (dev->bulk_in_filled) {
		/* we had read data */
		size_t available = dev->bulk_in_filled - dev->bulk_in_copied;
		size_t chunk = min(available, count);

		if (!available) {
			/*
			 * all data has been used
			 * actual IO needs to be done
			 */
			rv = skel_do_read_io(dev, count);
			if (rv < 0)
				goto exit;
			else
				goto retry;
		}
		/*
		 * data is available
		 * chunk tells us how much shall be copied
		 */

		if (copy_to_user(buffer,
				 dev->bulk_in_buffer + dev->bulk_in_copied,
				 chunk))
			rv = -EFAULT;
		else
			rv = chunk;

		dev->bulk_in_copied += chunk;

		/*
		 * if we are asked for more than we have,
		 * we start IO but don't wait
		 */
		if (available < count)
			skel_do_read_io(dev, count - chunk);
	} else {
		/* no data in the buffer */
		rv = skel_do_read_io(dev, count);
		if (rv < 0)
			goto exit;
		else
			goto retry;
	}
exit:
	mutex_unlock(&dev->io_mutex);
	return rv;
}

/*
 * ==============================================================
 * skel_write_bulk_callback() — 批量写 URB 完成回调函数
 * ==============================================================
 *
 * 当批量 OUT 传输完成 (成功/失败/取消) 时，USB 核心层调用此回调。
 * 与 skel_read_bulk_callback() 类似，此函数也在中断上下文中执行。
 *
 * 回调的职责:
 * 1. 检查 urb->status，记录错误到 dev->errors
 * 2. usb_free_coherent() 释放之前分配的 DMA 一致性缓冲区
 * 3. up(&dev->limit_sem): 释放一个写并发槽位
 *    (对应 skel_write() 中的 down_interruptible)
 *
 * 注意：与 skel_read_bulk_callback() 不同，此回调不会通过
 * 等待队列唤醒写进程。这是因为 skel_write() 使用同步写模式：
 * 它不等待 URB 完成，而是直接提交 URB 后返回。
 * 写操作的同步通过限流信号量 (limit_sem) 间接实现。
 */
static void skel_write_bulk_callback(struct urb *urb)
{
	struct usb_skel *dev;
	unsigned long flags;

	dev = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		spin_lock_irqsave(&dev->err_lock, flags);
		dev->errors = urb->status;
		spin_unlock_irqrestore(&dev->err_lock, flags);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	up(&dev->limit_sem);
}

/*
 * ==============================================================
 * skel_write() — 向 USB 设备写入数据的同步写操作
 * ==============================================================
 *
 * 写操作的执行流程展示了 USB 批量 OUT 传输的完整模式：
 *
 * 1. 并发限制 (limit_sem 信号量):
 *    - 阻塞模式: down_interruptible() 获取信号量
 *      如果 WRITES_IN_FLIGHT (8) 个写 URB 都在飞行中，
 *      则在此等待，直到有某个写 URB 完成并释放槽位
 *    - 非阻塞模式 (O_NONBLOCK): down_trylock() 尝试获取
 *      如果获取不到立即返回 -EAGAIN
 *    - 这是防止内存耗尽的重要保护: 限制飞行中的 URB 数量
 *
 * 2. 错误检查:
 *    检查 dev->errors 中是否有之前未报告的错误。
 *    如果有，报告一次后清空。
 *
 * 3. 创建 URB 和 DMA 缓冲区:
 *    - usb_alloc_urb(0, GFP_KERNEL): 分配 URB 结构体
 *      参数 0 表示使用默认的 URB 私有大小 (无私有数据)
 *    - usb_alloc_coherent(): 分配 DMA 一致性缓冲区
 *      这是 USB 控制器可直接访问 (无需 cache 同步) 的内存
 *      注意使用 usb_alloc_coherent() 而非 kmalloc()，
 *      因为 USB 控制器通过 DMA 直接访问此缓冲区
 *
 * 4. copy_from_user():
 *    将用户空间的写数据复制到 DMA 缓冲区。
 *    注意：如果在第 3 步之前 copy，可能导致用户空间数据
 *    被其他线程修改 (TOCTOU 问题)。
 *
 * 5. 断开连接检查 (io_mutex 保护):
 *    获取 io_mutex 后检查 dev->disconnected 标志。
 *    这是关键的安全检查：如果设备在分配 URB 和复制数据期间
 *    断开了连接，在此处检测到并返回 -ENODEV。
 *
 * 6. usb_fill_bulk_urb() + usb_submit_urb():
 *    - usb_sndbulkpipe(): 构造批量 OUT 管道
 *    - URB_NO_TRANSFER_DMA_MAP: 使用 DMA 缓冲区而非默认缓冲区
 *    - usb_anchor_urb(): 将 URB 添加到 submitted 锚点列表，
 *      便于 disconnect() 统一取消所有飞行中的写 URB
 *    - usb_submit_urb(): 将 URB 提交给 USB 核心层进行传输
 *
 * 7. 资源管理:
 *    - 提交成功后: usb_free_urb(urb) (但 urb->transfer_buffer 不由 URB 释放，
 *      因为在 DMA 模式下由回调中 usb_free_coherent() 释放)
 *    - 提交失败: 在 error_unanchor / error 标签处清理所有已分配资源
 *    - 无论成功失败，最终都会调用 up(&dev->limit_sem) 释放信号量槽位
 *
 * 注意: 这是"提交即返回"的写模式——UWB 提交后函数立即返回，
 * 不等待传输完成。真正的传输在后台异步进行。
 * 如果应用需要等待写完成，可以在用户空间使用 fsync()，
 * 对应 skel_flush() 中的 skel_draw_down() 等待所有 URB 完成。
 */
static ssize_t skel_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos)
{
	struct usb_skel *dev;
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	size_t writesize = min_t(size_t, count, MAX_TRANSFER);

	dev = file->private_data;

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/*
	 * limit the number of URBs in flight to stop a user from using up all
	 * RAM
	 */
	if (!(file->f_flags & O_NONBLOCK)) {
		if (down_interruptible(&dev->limit_sem)) {
			retval = -ERESTARTSYS;
			goto exit;
		}
	} else {
		if (down_trylock(&dev->limit_sem)) {
			retval = -EAGAIN;
			goto exit;
		}
	}

	spin_lock_irq(&dev->err_lock);
	retval = dev->errors;
	if (retval < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		retval = (retval == -EPIPE) ? retval : -EIO;
	}
	spin_unlock_irq(&dev->err_lock);
	if (retval < 0)
		goto error;

	/* create a urb, and a buffer for it, and copy the data to the urb */
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, writesize, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}

	if (copy_from_user(buf, user_buffer, writesize)) {
		retval = -EFAULT;
		goto error;
	}

	/* this lock makes sure we don't submit URBs to gone devices */
	mutex_lock(&dev->io_mutex);
	if (dev->disconnected) {		/* disconnect() was called */
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			  buf, writesize, skel_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &dev->submitted);

	/* send the data out the bulk port */
	retval = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (retval) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting write urb, error %d\n",
			__func__, retval);
		goto error_unanchor;
	}

	/*
	 * release our reference to this urb, the USB core will eventually free
	 * it entirely
	 */
	usb_free_urb(urb);


	return writesize;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	if (urb) {
		usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	up(&dev->limit_sem);

exit:
	return retval;
}

/*
 * ==============================================================
 * skel_fops — 文件操作结构体 (用户空间接口)
 * ==============================================================
 *
 * file_operations 结构体定义了用户空间程序通过 /dev/skel%d
 * 设备文件与驱动交互的接口。这是 VFS (虚拟文件系统) 层的
 * 标准接口，与普通文件驱动 (如 ext4) 使用的结构体相同。
 *
 * 各回调的作用:
 *   .owner   = THIS_MODULE:
 *     防止模块在使用中被卸载。当有文件打开时，模块引用
 *     计数会通过此字段维护。
 *
 *   .open    = skel_open:
 *     用户调用 open("/dev/skel0") 时调用。
 *     初始化文件描述符，增加 kref 引用计数。
 *
 *   .release = skel_release:
 *     用户调用 close(fd) 时调用。
 *     释放 kref 引用计数，释放自动挂起。
 *
 *   .read    = skel_read:
 *     用户调用 read(fd, buf, count) 时调用。
 *     提交批量读 URB，等待完成，复制数据到用户空间。
 *
 *   .write   = skel_write:
 *     用户调用 write(fd, buf, count) 时调用。
 *     分配 DMA 缓冲区，复制用户数据，提交批量写 URB。
 *
 *   .flush   = skel_flush:
 *     用户调用 close(fd) 或 fsync(fd) 时调用。
 *     等待所有飞行中的写 URB 完成，确保数据完整性。
 *
 *   .llseek  = noop_llseek:
 *     不支持文件定位操作 (seek)。USB 设备是顺序访问的
 *     流式设备，不支持随机访问。
 *     noop_llseek 表示 llseek 操作总是返回 -ESPIPE。
 */
static const struct file_operations skel_fops = {
	.owner =	THIS_MODULE,
	.read =		skel_read,
	.write =	skel_write,
	.open =		skel_open,
	.release =	skel_release,
	.flush =	skel_flush,
	.llseek =	noop_llseek,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 *
 * usb_class_driver 结构体:
 *   USB 驱动通过此结构体向 USB 核心层注册字符设备接口。
 *   usb_register_dev(interface, &skel_class) 使用此结构体
 *   在 /dev/ 下创建设备文件节点。
 *
 *   .name = "skel%d":
 *     设备文件名称模板。%d 被替换为 USB 核心层分配的次设备号。
 *     例如第一次注册可能创建 /dev/skel0，第二次创建 /dev/skel1。
 *     通常 USB 核心从 minor_base 开始顺序分配次设备号。
 *
 *   .fops = &skel_fops:
 *     指向 file_operations 结构体，定义设备文件的操作函数。
 *     VFS 层通过此结构体将用户空间的系统调用分派到驱动函数。
 *
 *   .minor_base = USB_SKEL_MINOR_BASE (192):
 *     次设备号的起始值。USB 核心层从 minor_base 开始，
 *     为此设备分配一个可用的次设备号。
 *     注意: USB 次设备号范围需要在 Linux USB 子系统维护者
 *     处申请并登记，以避免与其它 USB 驱动冲突。
 */
static struct usb_class_driver skel_class = {
	.name =		"skel%d",
	.fops =		&skel_fops,
	.minor_base =	USB_SKEL_MINOR_BASE,
};

/*
 * ==============================================================
 * skel_probe() — USB 设备探测/初始化函数 (设备插入时调用)
 * ==============================================================
 *
 * 当 USB 核心层检测到与 skel_table[] 匹配的设备插入时，
 * 调用此函数。这是 USB 驱动中最重要的函数之一 ——
 * 它负责初始化驱动与设备之间的所有连接。
 *
 * 参数:
 *   @interface: 匹配的 USB 接口描述符。USB 设备可以有多个接口
 *                (如音频设备有控制接口和音频流接口)，
 *                此参数指向匹配到的那个接口。
 *   @id:        匹配到的 usb_device_id 条目，包含 vendor/product ID 等信息。
 *
 * 以下是 probe() 的完整执行步骤，每一步都是 USB 驱动开发的标准模式：
 *
 * ---- 第 1 步: 分配驱动私有结构体 ----
 * dev = kzalloc(sizeof(*dev), GFP_KERNEL);
 *   - kzalloc 分配内存并清零，避免未初始化的字段导致问题
 *   - 如果分配失败，直接返回 -ENOMEM
 *   - 所有 USB 驱动都应该有这样一个私有结构体来管理设备状态
 *
 * ---- 第 2 步: 初始化同步原语 ----
 * kref_init(&dev->kref):       初始化引用计数器为 1
 * sema_init(&dev->limit_sem, 8):初始化信号量，允许最多 8 个并发写
 * mutex_init(&dev->io_mutex):   初始化 I/O 互斥锁
 * spin_lock_init(&dev->err_lock):初始化错误状态自旋锁
 * init_usb_anchor(&dev->submitted):初始化 URB 锚点列表
 * init_waitqueue_head(&dev->bulk_in_wait):初始化读等待队列
 *
 * ---- 第 3 步: 获取 USB 设备引用 ----
 * dev->udev = usb_get_dev(interface_to_usbdev(interface));
 *   - interface_to_usbdev(): 从 interface 反向获取 usb_device
 *   - usb_get_dev(): 增加 USB 设备引用计数，确保设备不会被意外释放
 *
 * ---- 第 4 步: 获取接口引用 ----
 * dev->interface = usb_get_intf(interface);
 *   - usb_get_intf(): 增加接口引用计数
 *
 * ---- 第 5 步: 查找批量端点 ----
 * usb_find_common_endpoints(interface->cur_altsetting,
 *                            &bulk_in, &bulk_out, NULL, NULL);
 *   - 从当前接口的端点描述符中搜索批量 IN 和批量 OUT 端点
 *   - cur_altsetting 指向当前激活的接口描述符的端点数组
 *   - 最后两个 NULL 表示不查找中断端点
 *   - 如果找不到批量端点，跳转到 error 清理并返回错误
 *
 * ---- 第 6 步: 保存端点地址和信息 ----
 * dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
 *   - 获取批量 IN 端点的最大数据包长度 (wMaxPacketSize)
 * dev->bulk_in_endpointAddr = bulk_in->bEndpointAddress;
 *   - 保存端点地址 (含方向位)，用于构造管道
 * dev->bulk_in_buffer = kmalloc(dev->bulk_in_size, GFP_KERNEL);
 *   - 为批量 IN 分配接收缓冲区
 * dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
 *   - 为批量 IN 预分配一个 URB (读操作复用此 URB)
 * dev->bulk_out_endpointAddr = bulk_out->bEndpointAddress;
 *   - 保存批量 OUT 端点地址
 *
 * ---- 第 7 步: 关联私有数据到 interface ----
 * usb_set_intfdata(interface, dev);
 *   - 将私有结构体指针保存到 interface 中
 *   - 之后在 open/disconnect/release 中通过 usb_get_intfdata() 取回
 *
 * ---- 第 8 步: 注册字符设备节点 ----
 * usb_register_dev(interface, &skel_class);
 *   - 在 /dev/ 下创建 skel%d 设备文件 (如 /dev/skel0)
 *   - skel_class.name = "skel%d", %d 被替换为分配的次设备号
 *   - 用户空间通过此设备文件与驱动交互
 *   - 如果注册失败，清理已设置的 intfdata 并跳转到 error
 *
 * ---- 第 9 步: 通知用户 ----
 * dev_info("USB Skeleton device now attached to USBSkel-%d", interface->minor);
 *   - 在内核日志中打印设备已就绪的消息
 *   - interface->minor 是 USB 核心分配的次设备号
 *
 * ---- 错误处理 (error 标签) ----
 * 如果以上任何一步失败，跳转到 error:
 * kref_put(&dev->kref, skel_delete);
 *   - 减少引用计数，触发 skel_delete() 释放所有已分配的资源
 *   - 注意 kref_init() 将引用计数初始化为 1，
 *     所以 kref_put() 在这里会释放结构体
 */
static int skel_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_skel *dev;
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	int retval;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc_obj(*dev);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->err_lock);
	init_usb_anchor(&dev->submitted);
	init_waitqueue_head(&dev->bulk_in_wait);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);

	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */
	retval = usb_find_common_endpoints(interface->cur_altsetting,
			&bulk_in, &bulk_out, NULL, NULL);
	if (retval) {
		dev_err(&interface->dev,
			"Could not find both bulk-in and bulk-out endpoints\n");
		goto error;
	}

	dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
	dev->bulk_in_endpointAddr = bulk_in->bEndpointAddress;
	dev->bulk_in_buffer = kmalloc(dev->bulk_in_size, GFP_KERNEL);
	if (!dev->bulk_in_buffer) {
		retval = -ENOMEM;
		goto error;
	}
	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		retval = -ENOMEM;
		goto error;
	}

	dev->bulk_out_endpointAddr = bulk_out->bEndpointAddress;

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	/* we can register the device now, as it is ready */
	retval = usb_register_dev(interface, &skel_class);
	if (retval) {
		/* something prevented us from registering this driver */
		dev_err(&interface->dev,
			"Not able to get a minor for this device.\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "USB Skeleton device now attached to USBSkel-%d",
		 interface->minor);
	return 0;

error:
	/* this frees allocated memory */
	kref_put(&dev->kref, skel_delete);

	return retval;
}

/*
 * ==============================================================
 * skel_disconnect() — USB 设备断开连接处理函数
 * ==============================================================
 *
 * 当 USB 设备被拔出 (物理移除) 或系统决定卸载驱动时，
 * USB 核心层调用此函数。这是 probe() 的逆操作。
 *
 * disconnect() 的核心目标：安全地停止所有正在进行的 I/O 操作，
 * 释放所有已分配的资源，同时确保不会影响仍在使用的
 * 文件描述符 (由 kref 机制保护)。
 *
 * 设备移除的完整执行序列:
 *
 * == 第 1 步: 获取私有数据 ==
 * dev = usb_get_intfdata(interface);
 *   从 interface 中取出之前在 probe() 中设置的私有数据。
 *
 * == 第 2 步: 注销设备节点 ==
 * usb_deregister_dev(interface, &skel_class);
 *   移除 /dev/skel%d 设备节点。在此之后，新的 open() 调用将
 *   无法找到此设备 (usb_find_interface() 将返回 NULL)。
 *   但已打开的 fd 仍然可以继续操作。
 *
 * == 第 3 步: 设置断开标志 ==
 * mutex_lock(&dev->io_mutex);
 * dev->disconnected = 1;
 * mutex_unlock(&dev->io_mutex);
 *   在 io_mutex 保护下设置 disconnected 标志。
 *   此标志在 skel_read() 和 skel_write() 中检查：
 *   如果设备已断开，read/write 立即返回 -ENODEV。
 *   io_mutex 确保与正在执行的 read/write 操作的互斥：
 *   - 如果 read/write 正在执行中，mutex_lock 会等待其完成
 *   - 获得锁后设置标志，确保后续 read/write 不会开始
 *
 * == 第 4 步: 停止所有正在进行的 URB ==
 * usb_kill_urb(dev->bulk_in_urb);
 *   同步杀死批量读 URB。usb_kill_urb() 会等待 URB 完全停止。
 *   如果 URB 正在传输中，等待其完成；如果已排队但未开始，取消它。
 *
 * usb_kill_anchored_urbs(&dev->submitted);
 *   遍历 submitted 锚点列表，杀死所有正在进行的写 URB。
 *   锚点机制 (usb_anchor_urb / usb_kill_anchored_urbs) 使得
 *   管理多个飞行中的 URB 变得更加方便。
 *
 * == 第 5 步: 释放引用计数 ==
 * kref_put(&dev->kref, skel_delete);
 *   释放 probe() 中 kref_init() 时的初始引用计数。
 *   如果此时没有打开的文件描述符 (即 skel_open 未调用)，
 *   引用计数降为 0，自动调用 skel_delete() 释放所有资源。
 *   如果有打开的文件描述符，kref_put 仅减少计数，
 *   结构体会在最后一个 skel_release() 调用时释放。
 *
 * == 第 6 步: 日志输出 ==
 * dev_info("USB Skeleton #%d now disconnected", minor);
 *   在内核日志中记录设备已断开的消息。
 *
 * ---- 关于 use-after-free 保护 ----
 * disconnect() 中的资源释放是通过 kref 引用计数来控制的，
 * 而不是直接释放。这意味着：
 * - 如果仍有用户空间的程序打开了设备，struct usb_skel 不会被释放
 * - 但 disconnected 标志阻止了任何新的 I/O 操作
 * - 当所有 fd 都关闭后，skel_release() 中的 kref_put()
 *   最终将引用计数降为 0，触发 skel_delete() 释放结构体
 */
static void skel_disconnect(struct usb_interface *interface)
{
	struct usb_skel *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);

	/* give back our minor */
	usb_deregister_dev(interface, &skel_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->disconnected = 1;
	mutex_unlock(&dev->io_mutex);

	usb_kill_urb(dev->bulk_in_urb);
	usb_kill_anchored_urbs(&dev->submitted);

	/* decrement our usage count */
	kref_put(&dev->kref, skel_delete);

	dev_info(&interface->dev, "USB Skeleton #%d now disconnected", minor);
}

/*
 * ==============================================================
 * skel_draw_down() — 等待并停止所有正在进行的 URB
 * ==============================================================
 *
 * 此函数被 skel_flush()、skel_suspend() 和 skel_pre_reset() 调用，
 * 用于确保在关键操作前所有飞行中的 URB 都已完成或停止。
 *
 * 为什么需要等待所有 URB 完成?
 *
 * 1. flush (文件描述符关闭前):
 *    当用户调用 close() 前调用 fsync() 时，
 *    需要确保所有写 URB 已完成，数据已发送到设备，
 *    避免数据丢失。
 *
 * 2. suspend (系统挂起前):
 *    系统进入休眠前必须停止所有 URB。
 *    如果系统挂起时仍有 URB 在传输中，
 *    醒来时 USB 控制器和设备的状态可能不一致。
 *    这是 USB 电源管理的基本要求。
 *
 * 3. pre_reset (USB 设备复位前):
 *    设备复位会断开所有端点，所以必须首先停止所有 URB。
 *    复位完成后，post_reset 重新初始化设备状态。
 *
 * 执行策略 (两阶段等待):
 *
 * 阶段 1: 等待所有锚定的写 URB 完成
 *   usb_wait_anchor_empty_timeout(&dev->submitted, 1000):
 *   - 等待 submitted 锚点列表中的所有写 URB 完成
 *   - 超时时间 1000ms (1 秒)
 *   - 返回剩余时间 (毫秒)，如果超时返回 0
 *
 * 阶段 2: 如果超时，强制杀死写 URB
 *   if (!time) usb_kill_anchored_urbs(&dev->submitted):
 *   - 如果第一阶段超时 (某些写 URB 卡住了)，
 *     强制杀死所有写 URB
 *
 * 阶段 3: 杀死批量读 URB
 *   usb_kill_urb(dev->bulk_in_urb):
 *   - 同步杀死读 URB
 *   - usb_kill_urb() 会等待直到 URB 完全停止
 *   - 这是阻塞操作，但一般很快完成
 *
 * 注意: skel_draw_down() 内部可能会睡眠等待，
 * 因此不能在中断上下文中调用。
 */
static void skel_draw_down(struct usb_skel *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

/*
 * ==============================================================
 * skel_suspend() — 系统挂起回调 (电源管理)
 * ==============================================================
 *
 * 当系统进入休眠状态 (STR/Suspend-to-RAM) 时，USB 核心层
 * 调用此函数。驱动必须停止所有正在进行的 URB，确保设备
 * 在系统挂起期间处于一致的状态。
 *
 * 处理逻辑:
 * 1. skel_draw_down(dev): 等待并停止所有正在进行的 URB
 *    (读 URB 和所有飞行中的写 URB)
 * 2. 返回 0 表示挂起成功
 *
 * 注意: skel_draw_down() 会等待 URB 完成，这意味着
 * 如果设备正在传输大量数据，挂起操作可能会有延迟。
 * 但这是必要的 —— 系统无法在 URB 还在进行中时安全地挂起。
 */
static int skel_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;
	skel_draw_down(dev);
	return 0;
}

/*
 * ==============================================================
 * skel_resume() — 系统恢复回调 (电源管理)
 * ==============================================================
 *
 * 当系统从休眠状态恢复时，USB 核心层调用此函数。
 *
 * 在此模板中，resume 回调简单地返回 0，因为:
 * - 批量端点不需要在恢复后重新配置 (USB 核心层会处理)
 * - 驱动没有需要恢复的寄存器状态
 * - 应用程序会在下次 read/write 时重新提交 URB
 *
 * 对于更复杂的设备 (如需要重新配置固件的设备)，
 * resume 回调可能需要重新初始化设备状态。
 */
static int skel_resume(struct usb_interface *intf)
{
	return 0;
}

/*
 * ==============================================================
 * skel_pre_reset() — USB 设备复位前回调
 * ==============================================================
 *
 * 当 USB 核心层决定复位设备时，先调用此函数通知驱动。
 * 复位可能由以下原因触发:
 * - USB 错误恢复 (如端点停止 -EPIPE 后的复位)
 * - 驱动程序主动请求复位 (usb_reset_device())
 * - 系统从挂起状态恢复时需要复位设备
 *
 * 在复位前，驱动必须:
 * 1. 停止所有正在进行的 URB (通过 skel_draw_down())
 * 2. 获取 io_mutex，阻止新的 I/O 操作
 *
 * 锁在这里是延迟获取的: 获取 io_mutex 但不释放，
 * 在 skel_post_reset() 中释放。这意味着在复位期间
 * 所有的 read/write 操作都被阻塞。
 */
static int skel_pre_reset(struct usb_interface *intf)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	mutex_lock(&dev->io_mutex);
	skel_draw_down(dev);

	return 0;
}

/*
 * ==============================================================
 * skel_post_reset() — USB 设备复位后回调
 * ==============================================================
 *
 * USB 设备复位完成后调用。此时设备恢复正常，
 * 驱动需要重置内部状态。
 *
 * 恢复步骤:
 * 1. dev->errors = -EPIPE:
 *    设置错误码为 -EPIPE，表示设备刚刚复位。
 *    后续的 read/write 操作会检测到此错误并返回 -EPIPE，
 *    通知用户空间程序设备已复位，需要重新建立连接。
 *
 * 2. mutex_unlock(&dev->io_mutex):
 *    释放在 skel_pre_reset() 中获取的 io_mutex 锁。
 *    释放后，阻塞在 mutex_lock_interruptible() 上的
 *    read/write 操作可以继续执行。
 *
 * 注意:
 * - 复位操作是 USB 子系统的错误恢复机制之一
 * - 复位后，端点状态被重置，所有之前的 URB 都被取消
 * - 应用程序需要通过 -EPIPE 返回值感知复位事件，
 *   并在必要时重新打开设备以恢复通信
 * - 此模板在复位后没有提交新的 URB —— 驱动等待
 *   应用程序的下一次 read/write 来重新开始通信
 */
static int skel_post_reset(struct usb_interface *intf)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	/* we are sure no URBs are active - no locking needed */
	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}

/*
 * ==============================================================
 * skel_driver — USB 驱动结构体 (注册到 USB 核心层)
 * ==============================================================
 *
 * struct usb_driver 是 USB 驱动的核心结构体。它定义了驱动
 * 的所有回调函数和属性。通过 module_usb_driver() 注册到
 * USB 核心后，USB 核心层在设备插入/移除/挂起/恢复等事件
 * 发生时调用相应的回调函数。
 *
 * 各字段说明:
 *
 *   .name = "skeleton":
 *     驱动的名称。在 sysfs 中显示为:
 *     /sys/bus/usb/drivers/skeleton/
 *
 *   .probe = skel_probe:
 *     当 USB 核心检测到匹配的设备时调用。完成驱动初始化。
 *
 *   .disconnect = skel_disconnect:
 *     当设备拔出或驱动卸载时调用。完成资源清理。
 *
 *   .suspend = skel_suspend:
 *     系统进入挂起状态前调用。停止所有正在进行的 URB。
 *
 *   .resume = skel_resume:
 *     系统从挂起状态恢复后调用。恢复设备通信。
 *
 *   .pre_reset = skel_pre_reset:
 *     USB 设备复位前调用。停止所有通信，获取同步锁。
 *
 *   .post_reset = skel_post_reset:
 *     USB 设备复位后调用。重新初始化状态，释放同步锁。
 *
 *   .id_table = skel_table:
 *     指向 usb_device_id 表的指针，定义此驱动支持的设备。
 *     USB 核心层使用此表进行设备的匹配和驱动的自动加载。
 *
 *   .supports_autosuspend = 1:
 *     声明此驱动支持 USB 自动挂起功能。
 *     当设备空闲时，允许 USB 核心自动将设备挂起以节省功耗。
 *     需要在 skel_open 中使用 usb_autopm_get_interface()
 *     配合使用，防止设备在活动时被挂起。
 */
static struct usb_driver skel_driver = {
	.name =		"skeleton",
	.probe =	skel_probe,
	.disconnect =	skel_disconnect,
	.suspend =	skel_suspend,
	.resume =	skel_resume,
	.pre_reset =	skel_pre_reset,
	.post_reset =	skel_post_reset,
	.id_table =	skel_table,
	.supports_autosuspend = 1,
};

/*
 * ==============================================================
 * 模块加载/卸载宏 — module_usb_driver()
 * ==============================================================
 *
 * module_usb_driver() 是 Linux 内核提供的一个便捷宏，
 * 定义在 include/linux/usb.h 中。它自动展开为标准的
 * module_init() 和 module_exit() 函数，简化了 USB 驱动的
 * 模板代码。
 *
 * 此宏等价于以下代码:
 *
 *   static int __init skel_init(void) {
 *       return usb_register(&skel_driver);
 *   }
 *   module_init(skel_init);
 *
 *   static void __exit skel_exit(void) {
 *       usb_deregister(&skel_driver);
 *   }
 *   module_exit(skel_exit);
 *
 * usb_register() 的作用:
 *   1. 将 skel_driver 结构体注册到 USB 核心层的驱动列表
 *   2. USB 核心层将 skel_table[] 中的设备 ID 与已连接的
 *      设备进行匹配
 *   3. 当检测到匹配的设备时，USB 核心层调用 skel_probe()
 *
 * usb_deregister() 的作用:
 *   1. 从 USB 核心层的驱动列表中移除 skel_driver
 *   2. 对所有当前已连接到此驱动的设备调用 skel_disconnect()
 *   3. 等待所有设备断开完成后返回
 *
 * __init / __exit 标记:
 *   - __init: 表示此函数仅在模块初始化阶段使用，
 *     初始化完成后内核可以释放此代码占用的内存
 *   - __exit: 表示此函数仅在模块卸载时使用，
 *     如果驱动被编译进内核 (非模块)，此函数将被丢弃
 *
 * struct usb_driver (skel_driver) 的关键字段:
 *   .name          — 驱动名称，在 /sys/bus/usb/drivers/ 中显示
 *   .probe         — 设备插入回调
 *   .disconnect    — 设备移除回调
 *   .suspend       — 系统挂起回调
 *   .resume        — 系统恢复回调
 *   .pre_reset     — USB 设备复位前回调
 *   .post_reset    — USB 设备复位后回调
 *   .id_table      — 支持的 USB 设备 ID 表
 *   .supports_autosuspend — 支持自动挂起
 */
module_usb_driver(skel_driver);

MODULE_LICENSE("GPL v2");
