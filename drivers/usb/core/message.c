// SPDX-License-Identifier: GPL-2.0
/*
 * message.c - synchronous message handling
 *
 * Released under the GPLv2 only.
 */

/*
 * 中文注释集 - USB 同步消息处理与标准请求
 *
 * 本文件实现了 Linux USB 核心中的同步（synchronous）传输 API 和标准 USB 设备请求。
 * 所谓"同步"，是指调用者发起传输后阻塞等待，直到传输完成或超时才会返回。
 * 所有导出的函数都可以在进程上下文中安全调用（可能会睡眠），但不能在中断上下文中使用。
 *
 * === 核心架构 ===
 * 所有同步传输最终都归结为以下流程：
 *   usb_alloc_urb()          -- 分配 URB（USB Request Block）
 *   usb_fill_*_urb()         -- 填充 URB 各字段（控制/批量/中断）
 *   usb_submit_urb()         -- 将 URB 提交给主机控制器驱动程序
 *   wait_for_completion()    -- 阻塞等待传输完成
 *   usb_free_urb()           -- 释放 URB
 *
 * 完成回调 usb_api_blocking_completion() 是连接异步 URB 机制与同步等待的桥梁，
 * 它在 URB 完成时调用 complete() 唤醒等待的任务。
 *
 * === 导出的主要 API ===
 *
 * [1] usb_control_msg()        -- 通用控制传输。调用者需手动构造 SETUP 包的
 *                                  request/requesttype/value/index 字段。
 *                                 返回实际传输的字节数（正数）或错误码（负数）。
 *
 * [2] usb_control_msg_send()   -- usb_control_msg() 的"发送"简化版。
 *                                 自动处理数据拷贝，成功时返回 0。
 *                                 适合只需要发送数据、不需要接收响应的场景。
 *
 * [3] usb_control_msg_recv()   -- usb_control_msg() 的"接收"简化版。
 *                                 自动分配接收缓冲区并要求数据量严格匹配。
 *                                 若设备返回的数据少于预期则返回 -EREMOTEIO。
 *
 * [4] usb_bulk_msg()           -- 同步批量传输。若目标端点为中断类型，
 *                                 会自动创建中断 URB 而非批量 URB。
 *
 * [5] usb_interrupt_msg()      -- 同步中断传输。当前实现直接调用 usb_bulk_msg()。
 *
 * [6] usb_bulk_msg_killable()  -- 可被 kill 的同步批量传输版本，
 *                                 使用 wait_for_completion_killable_timeout() 等待。
 *
 * [7] usb_sg_init() / usb_sg_wait() / usb_sg_cancel()
 *                              -- 散列-聚集（scatter/gather）批量/中断传输。
 *                                 将多个缓冲区组织成一条传输链，提高高带宽传输效率。
 *
 * === 标准 USB 设备请求（Chapter 9）===
 *
 * [8] usb_get_descriptor()     -- 读取任意类型的描述符（GET_DESCRIPTOR）。
 *                                 内部重试 3 次以应对不稳定设备。
 *
 * [9] usb_get_string()         -- 读取字符串描述符（内部函数）。
 *                                 使用 UTF-16LE 编码。
 *
 * [10] usb_string()             -- 读取并将 UTF-16LE 字符串描述符转换为 UTF-8。
 *                                  自动获取设备的语言 ID（第一个支持的语言）。
 *
 * [11] usb_get_status()         -- 获取设备/接口/端点的状态（GET_STATUS）。
 *                                  支持标准状态（2 字节）和 PTM 状态（4 字节）。
 *
 * [12] usb_set_interface()      -- 选择接口的备用设置（SET_INTERFACE）。
 *                                  用于切换同一接口的不同带宽模式。
 *
 * [13] usb_set_configuration()  -- 选择设备的配置（SET_CONFIGURATION）。
 *                                  改变配置会销毁旧接口、创建新接口并触发驱动绑定。
 *
 * [14] usb_clear_halt()         -- 清除端点的停止（STALL）条件。
 *
 * [15] usb_reset_configuration()-- 轻量级设备复位：重新设置当前配置，
 *                                  将所有接口的备用设置重置为 0。
 *
 * === 内部辅助函数 ===
 *
 * usb_start_wait_urb()        -- 核心等待函数：提交 URB 并等待完成。
 *                                支持可中断等待和不可中断等待两种模式。
 *                                超时值受 USB_MAX_SYNCHRONOUS_TIMEOUT（5 秒）限制。
 *
 * usb_internal_control_msg()  -- 内部控制传输函数，是 usb_control_msg() 的底层实现。
 *                                完成 URB 分配、填充、提交、等待和释放的完整生命周期。
 *
 * sg_complete() / sg_clean()  -- 散列-聚集传输的完成回调和资源清理函数。
 *
 * === 注意事项 ===
 * - 所有同步 API 都不能在中断上下文中使用，必须使用 usb_submit_urb() 的异步方式。
 * - 驱动程序的 disconnect() 方法必须能够等待正在进行的同步调用完成。
 * - 对于控制传输的 data 缓冲区，usb_control_msg() 要求使用可 DMA 的内存
 *   （即不能是栈上的局部变量），而 usb_control_msg_send/recv() 没有此限制。
 */

#include <linux/acpi.h>
#include <linux/pci.h>	/* for scatterlist macros */
#include <linux/usb.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/ctype.h>
#include <linux/nls.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/usb/cdc.h>
#include <linux/usb/quirks.h>
#include <linux/usb/hcd.h>	/* for usbcore internals */
#include <linux/usb/of.h>
#include <asm/byteorder.h>

#include "usb.h"

static void cancel_async_set_config(struct usb_device *udev);

struct api_context {
	struct completion	done;
	int			status;
};

static void usb_api_blocking_completion(struct urb *urb)
{
	struct api_context *ctx = urb->context;

	ctx->status = urb->status;
	complete(&ctx->done);
}


/*
 * Starts urb and waits for completion or timeout.
 * Whether or not the wait is killable depends on the flag passed in.
 * For example, compare usb_bulk_msg() and usb_bulk_msg_killable().
 *
 * For non-killable waits, we enforce a maximum limit on the timeout value.
 */
static int usb_start_wait_urb(struct urb *urb, int timeout, int *actual_length,
		bool killable)
{
	struct api_context ctx;
	unsigned long expire;
	int retval;
	long rc;

	init_completion(&ctx.done);
	urb->context = &ctx;
	urb->actual_length = 0;
	retval = usb_submit_urb(urb, GFP_NOIO);
	if (unlikely(retval))
		goto out;

	if (!killable && (timeout <= 0 || timeout > USB_MAX_SYNCHRONOUS_TIMEOUT))
		timeout = USB_MAX_SYNCHRONOUS_TIMEOUT;
	expire = (timeout > 0) ? msecs_to_jiffies(timeout) : MAX_SCHEDULE_TIMEOUT;
	if (killable)
		rc = wait_for_completion_killable_timeout(&ctx.done, expire);
	else
		rc = wait_for_completion_timeout(&ctx.done, expire);
	if (rc <= 0) {
		usb_kill_urb(urb);
		if (ctx.status != -ENOENT)
			retval = ctx.status;
		else if (rc == 0)
			retval = -ETIMEDOUT;
		else
			retval = rc;

		dev_dbg(&urb->dev->dev,
			"%s timed out or killed on ep%d%s len=%u/%u\n",
			current->comm,
			usb_endpoint_num(&urb->ep->desc),
			usb_urb_dir_in(urb) ? "in" : "out",
			urb->actual_length,
			urb->transfer_buffer_length);
	} else
		retval = ctx.status;
out:
	if (actual_length)
		*actual_length = urb->actual_length;

	usb_free_urb(urb);
	return retval;
}

/*-------------------------------------------------------------------*/
/*
 * 中文注释: usb_internal_control_msg() -- 同步控制传输的核心实现
 *
 * 这是整个同步控制传输机制的"发动机"。它将一个构造好的 SETUP 包
 * （struct usb_ctrlrequest）通过 URB 提交给主机控制器，并同步等待完成。
 *
 === 执行流程 ===
 *   (1) usb_alloc_urb(0, GFP_NOIO)
 *       -- 分配 URB 结构体。参数 '0' 表示不分配额外的 ISO 包描述符，
 *          因为控制传输最多只需要一个 URB 即可完成（SETUP+DATA+STATUS 阶段
 *          都在同一个 URB 内由 HCD 管理）。
 *
 *   (2) usb_fill_control_urb(urb, usb_dev, pipe, cmd, data, len,
 *                             usb_api_blocking_completion, NULL)
 *       -- 填充 URB 的控制传输相关字段:
 *         - pipe:      目标端点管道（含方向、端点号、设备地址）
 *         - cmd:       SETUP 数据包 (8 字节，包含 bmRequestType/bRequest/wValue/wIndex/wLength)
 *         - data:      DATA 阶段的数据缓冲区指针
 *         - len:       DATA 阶段的数据长度
 *         - complete:  完成回调函数 usb_api_blocking_completion
 *         - context:   回调上下文（此处传 NULL，因为 usb_start_wait_urb 会覆盖它）
 *       usb_fill_control_urb 还会自动设置 URB 的 transfer_flags，确保
 *       SETUP 包被正确 DMA 映射。
 *
 *   (3) usb_start_wait_urb(urb, timeout, &length, false)
 *       -- 提交 URB 并等待完成。内部调用链:
 *         usb_submit_urb()              将 URB 放入 HCD 的调度队列
 *         wait_for_completion_timeout() 阻塞当前进程，等待 URB 完成
 *         usb_kill_urb()                若超时则强制终止 URB
 *         usb_free_urb()                释放 URB
 *       -- 第四个参数 false 表示使用不可杀死的等待模式，
 *          超时值被限制在 USB_MAX_SYNCHRONOUS_TIMEOUT (5 秒) 内。
 *
 === 完成回调机制 ===
 * usb_api_blocking_completion() 是连接异步 URB 完成事件与同步等待的关键:
 *   static void usb_api_blocking_completion(struct urb *urb)
 *   {
 *       struct api_context *ctx = urb->context;
 *       ctx->status = urb->status;     // 保存 URB 的完成状态
 *       complete(&ctx->done);          // 唤醒在 wait_for_completion 上睡眠的进程
 *   }
 * -- 注意: 当 URB 被 usb_kill_urb() 强制终止时，urb->status 被设置为 -ENOENT，
 *     usb_start_wait_urb 会据此判断是超时还是被杀死。
 *
 === pending_flag 机制 ===
 * 本函数没有显式的 pending_flag，因为这里通过 usb_start_wait_urb 内部的
 * 完成量（completion）来同步。每次调用都会分配新的 URB，因此不存在 URB 复用问题。
 * 而在其他更复杂的场景（如连续的控制传输）中，pending_flag 用于防止在
 * 前一个 URB 尚未完成时重用同一个 URB 结构体。
 *
 * 返回值: 正数表示实际传输的字节数，负数表示错误码。
 */
/* returns status (negative) or length (positive) */
static int usb_internal_control_msg(struct usb_device *usb_dev,
				    unsigned int pipe,
				    struct usb_ctrlrequest *cmd,
				    void *data, int len, int timeout)
{
	struct urb *urb;
	int retv;
	int length;

	urb = usb_alloc_urb(0, GFP_NOIO);
	if (!urb)
		return -ENOMEM;

	usb_fill_control_urb(urb, usb_dev, pipe, (unsigned char *)cmd, data,
			     len, usb_api_blocking_completion, NULL);

	retv = usb_start_wait_urb(urb, timeout, &length, false);
	if (retv < 0)
		return retv;
	else
		return length;
}

/*
 * 中文注释: usb_control_msg() -- 经典的控制传输封装
 *
 * 这是 Linux USB 子系统最常用的同步控制传输接口。调用者传入 USB 规范
 * 定义的 SETUP 包各字段，函数构造完整的 SETUP 包并通过内部控制函数发送。
 *
 === SETUP 包的构造 ===
 * USB 控制传输的第一个阶段是 SETUP 阶段，主机向设备发送一个 8 字节的
 * 建立包（struct usb_ctrlrequest），包含以下字段:
 *
 *   bmRequestType (1 byte) -- 请求类型:
 *     bit 7:     方向 (0=Host-to-Device, 1=Device-to-Host)
 *     bit 6-5:   类型 (0=标准, 1=类, 2=厂商)
 *     bit 4-0:   接收者 (0=设备, 1=接口, 2=端点, 3=其他)
 *   bRequest      (1 byte) -- 具体请求号，如 USB_REQ_GET_DESCRIPTOR
 *   wValue        (2 bytes)-- 请求参数，对 GET_DESCRIPTOR 是高字节存描述符类型、
 *                             低字节存描述符索引
 *   wIndex        (2 bytes)-- 请求参数，对 GET_DESCRIPTOR 存语言 ID（字符串描述符），
 *                             对接口/端点请求存接口号/端点号
 *   wLength       (2 bytes)-- DATA 阶段的数据长度
 *
 * 函数将这些参数填入 struct usb_ctrlrequest，并通过 cpu_to_le16() 将
 * 多字节字段转换为 USB 要求的小端字节序。
 *
 === 内部调用链 ===
 * usb_control_msg()
 *   -> kmalloc(struct usb_ctrlrequest)  分配 SETUP 包内存
 *   -> 填充 bRequestType/bRequest/wValue/wIndex/wLength
 *   -> usb_internal_control_msg()       执行核心传输
 *      -> usb_alloc_urb()              分配 URB
 *      -> usb_fill_control_urb()       填充 URB
 *      -> usb_start_wait_urb()         提交并等待
 *         -> usb_submit_urb()
 *         -> wait_for_completion_timeout()
 *   -> kfree(dr)                       释放 SETUP 包内存
 *
 === 注意事项 ===
 * - data 缓冲区必须位于可 DMA 的内存区域（不能是栈变量）！
 *   如果无法保证，请使用 usb_control_msg_send/recv() 替代。
 * - 若设备有 USB_QUIRK_DELAY_CTRL_MSG 标志，传输完成后会额外延迟 200ms，
 *   这是为了兼容某些需要控制命令间间隔的 USB 设备。
 * - 返回值为正数表示实际传输的字节数，为负数表示错误码。
 */
/**
 * usb_control_msg - Builds a control urb, sends it off and waits for completion
 * @dev: pointer to the usb device to send the message to
 * @pipe: endpoint "pipe" to send the message to
 * @request: USB message request value
 * @requesttype: USB message request type value
 * @value: USB message value
 * @index: USB message index value
 * @data: pointer to the data to send
 * @size: length in bytes of the data to send
 * @timeout: time in msecs to wait for the message to complete before timing out
 *
 * Context: task context, might sleep.
 *
 * This function sends a simple control message to a specified endpoint and
 * waits for the message to complete, or timeout.
 *
 * Don't use this function from within an interrupt context. If you need
 * an asynchronous message, or need to send a message from within interrupt
 * context, use usb_submit_urb(). If a thread in your driver uses this call,
 * make sure your disconnect() method can wait for it to complete. Since you
 * don't have a handle on the URB used, you can't cancel the request.
 *
 * Return: If successful, the number of bytes transferred. Otherwise, a negative
 * error number.
 */
int usb_control_msg(struct usb_device *dev, unsigned int pipe, __u8 request,
		    __u8 requesttype, __u16 value, __u16 index, void *data,
		    __u16 size, int timeout)
{
	struct usb_ctrlrequest *dr;
	int ret;

	dr = kmalloc_obj(struct usb_ctrlrequest, GFP_NOIO);
	if (!dr)
		return -ENOMEM;

	dr->bRequestType = requesttype;
	dr->bRequest = request;
	dr->wValue = cpu_to_le16(value);
	dr->wIndex = cpu_to_le16(index);
	dr->wLength = cpu_to_le16(size);

	ret = usb_internal_control_msg(dev, pipe, dr, data, size, timeout);

	/* Linger a bit, prior to the next control message. */
	if (dev->quirks & USB_QUIRK_DELAY_CTRL_MSG)
		msleep(200);

	kfree(dr);

	return ret;
}
EXPORT_SYMBOL_GPL(usb_control_msg);

/*
 * 中文注释: usb_control_msg_send() -- 现代控制传输接口（发送方向）
 *
 * 这是 usb_control_msg() 的简化封装，专门用于"只发送不接收"的控制传输。
 * 与老版本相比有以下改进:
 *
 *   (1) 自动处理数据拷贝
 *       -- 内部使用 kmemdup() 将 driver_data 拷贝到可 DMA 的缓冲区，
 *          调用者可以传递栈上的局部变量或 const 数据，无需担心 DMA 限制。
 *
 *   (2) 简化的返回值
 *       -- 成功时返回 0，失败时返回负错误码。
 *          usb_control_msg() 返回正数表示字节数容易引起混淆。
 *
 *   (3) 显式的 memflags 参数
 *       -- 调用者可通过 gfp_t 参数控制内存分配行为，
 *          例如在原子上下文中传递 GFP_ATOMIC。
 *
 * 适用场景: 只需要向设备发送数据、不需要读取响应的控制命令，
 * 如 SET_FEATURE、SET_CONFIGURATION、SET_INTERFACE 等。
 */
/**
 * usb_control_msg_send - Builds a control "send" message, sends it off and waits for completion
 * @dev: pointer to the usb device to send the message to
 * @endpoint: endpoint to send the message to
 * @request: USB message request value
 * @requesttype: USB message request type value
 * @value: USB message value
 * @index: USB message index value
 * @driver_data: pointer to the data to send
 * @size: length in bytes of the data to send
 * @timeout: time in msecs to wait for the message to complete before timing out
 * @memflags: the flags for memory allocation for buffers
 *
 * Context: !in_interrupt ()
 *
 * This function sends a control message to a specified endpoint that is not
 * expected to fill in a response (i.e. a "send message") and waits for the
 * message to complete, or timeout.
 *
 * Do not use this function from within an interrupt context. If you need
 * an asynchronous message, or need to send a message from within interrupt
 * context, use usb_submit_urb(). If a thread in your driver uses this call,
 * make sure your disconnect() method can wait for it to complete. Since you
 * don't have a handle on the URB used, you can't cancel the request.
 *
 * The data pointer can be made to a reference on the stack, or anywhere else,
 * as it will not be modified at all.  This does not have the restriction that
 * usb_control_msg() has where the data pointer must be to dynamically allocated
 * memory (i.e. memory that can be successfully DMAed to a device).
 *
 * Return: If successful, 0 is returned, Otherwise, a negative error number.
 */
int usb_control_msg_send(struct usb_device *dev, __u8 endpoint, __u8 request,
			 __u8 requesttype, __u16 value, __u16 index,
			 const void *driver_data, __u16 size, int timeout,
			 gfp_t memflags)
{
	unsigned int pipe = usb_sndctrlpipe(dev, endpoint);
	int ret;
	u8 *data = NULL;

	if (size) {
		data = kmemdup(driver_data, size, memflags);
		if (!data)
			return -ENOMEM;
	}

	ret = usb_control_msg(dev, pipe, request, requesttype, value, index,
			      data, size, timeout);
	kfree(data);

	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(usb_control_msg_send);

/*
 * 中文注释: usb_control_msg_recv() -- 现代控制传输接口（接收方向）
 *
 * 这是 usb_control_msg() 的简化封装，专门用于"接收数据"的控制传输。
 * 与 usb_control_msg_send() 对称设计，但有额外的保护措施:
 *
 *   (1) 自动分配接收缓冲区
 *       -- 内部使用 kmalloc() 分配可 DMA 的缓冲区，调用者的 driver_data
 *          可以是任意可写内存（包括栈变量）。
 *
 *   (2) 严格的数据量检查
 *       -- 如果设备返回的数据不等于请求的 size，返回 -EREMOTEIO。
 *          这可以防止驱动程序在未收到完整数据时错误地处理部分数据。
 *          因此，本函数不适合用于返回可变长度数据的请求。
 *
 *   (3) 零长度保护
 *       -- 明确拒绝 size == 0 或 driver_data == NULL 的情况，返回 -EINVAL。
 *
 * 适用场景: 需要从设备读取确定长度数据的控制命令，
 * 如 GET_DESCRIPTOR（但需注意描述符长度可能小于请求值）、GET_STATUS 等。
 */

/**
 * usb_control_msg_recv - Builds a control "receive" message, sends it off and waits for completion
 * @dev: pointer to the usb device to send the message to
 * @endpoint: endpoint to send the message to
 * @request: USB message request value
 * @requesttype: USB message request type value
 * @value: USB message value
 * @index: USB message index value
 * @driver_data: pointer to the data to be filled in by the message
 * @size: length in bytes of the data to be received
 * @timeout: time in msecs to wait for the message to complete before timing out
 * @memflags: the flags for memory allocation for buffers
 *
 * Context: !in_interrupt ()
 *
 * This function sends a control message to a specified endpoint that is
 * expected to fill in a response (i.e. a "receive message") and waits for the
 * message to complete, or timeout.
 *
 * Do not use this function from within an interrupt context. If you need
 * an asynchronous message, or need to send a message from within interrupt
 * context, use usb_submit_urb(). If a thread in your driver uses this call,
 * make sure your disconnect() method can wait for it to complete. Since you
 * don't have a handle on the URB used, you can't cancel the request.
 *
 * The data pointer can be made to a reference on the stack, or anywhere else
 * that can be successfully written to.  This function does not have the
 * restriction that usb_control_msg() has where the data pointer must be to
 * dynamically allocated memory (i.e. memory that can be successfully DMAed to a
 * device).
 *
 * The "whole" message must be properly received from the device in order for
 * this function to be successful.  If a device returns less than the expected
 * amount of data, then the function will fail.  Do not use this for messages
 * where a variable amount of data might be returned.
 *
 * Return: If successful, 0 is returned, Otherwise, a negative error number.
 */
int usb_control_msg_recv(struct usb_device *dev, __u8 endpoint, __u8 request,
			 __u8 requesttype, __u16 value, __u16 index,
			 void *driver_data, __u16 size, int timeout,
			 gfp_t memflags)
{
	unsigned int pipe = usb_rcvctrlpipe(dev, endpoint);
	int ret;
	u8 *data;

	if (!size || !driver_data)
		return -EINVAL;

	data = kmalloc(size, memflags);
	if (!data)
		return -ENOMEM;

	ret = usb_control_msg(dev, pipe, request, requesttype, value, index,
			      data, size, timeout);

	if (ret < 0)
		goto exit;

	if (ret == size) {
		memcpy(driver_data, data, size);
		ret = 0;
	} else {
		ret = -EREMOTEIO;
	}

exit:
	kfree(data);
	return ret;
}
EXPORT_SYMBOL_GPL(usb_control_msg_recv);

/*
 * 中文注释: usb_interrupt_msg() -- 同步中断传输
 *
 * 注意: 本函数的当前实现仅仅是 usb_bulk_msg() 的别名:
 *   return usb_bulk_msg(usb_dev, pipe, data, len, actual_length, timeout);
 *
 * 这意味着它实际上是通过 usb_bulk_msg() 内的自动检测机制来创建中断 URB。
 * usb_bulk_msg() 内部会检查目标端点的描述符:
 *   如果 bmAttributes 指示为中断端点 (USB_ENDPOINT_XFER_INT)，
 *   则使用 usb_fill_int_urb() 创建中断 URB；
 *   否则使用 usb_fill_bulk_urb() 创建批量 URB。
 *
 * 使用场景: 中断传输适用于小数据量、周期性轮询的传输场景。
 * USB 规范保证中断传输的最大延迟（轮询间隔由端点描述符的 bInterval 字段指定）。
 * 典型应用: HID 设备（键盘、鼠标）、USB 音频设备的同步反馈端点等。
 *
 * 与批量传输的区别:
 * - 中断传输有固定的轮询间隔，保证及时性但吞吐量较低。
 * - 批量传输没有实时性保证，但可以充分利用带宽。
 * - 中断传输在低速设备上最大包长为 8 字节，全速为 64 字节，
 *   高速为 1024 字节（每次微帧最多 3 笔事务）。
 */
/**
 * usb_interrupt_msg - Builds an interrupt urb, sends it off and waits for completion
 * @usb_dev: pointer to the usb device to send the message to
 * @pipe: endpoint "pipe" to send the message to
 * @data: pointer to the data to send
 * @len: length in bytes of the data to send
 * @actual_length: pointer to a location to put the actual length transferred
 *	in bytes
 * @timeout: time in msecs to wait for the message to complete before timing out
 *
 * Context: task context, might sleep.
 *
 * This function sends a simple interrupt message to a specified endpoint and
 * waits for the message to complete, or timeout.
 *
 * Don't use this function from within an interrupt context. If you need
 * an asynchronous message, or need to send a message from within interrupt
 * context, use usb_submit_urb() If a thread in your driver uses this call,
 * make sure your disconnect() method can wait for it to complete. Since you
 * don't have a handle on the URB used, you can't cancel the request.
 *
 * Return:
 * If successful, 0. Otherwise a negative error number. The number of actual
 * bytes transferred will be stored in the @actual_length parameter.
 */
int usb_interrupt_msg(struct usb_device *usb_dev, unsigned int pipe,
		      void *data, int len, int *actual_length, int timeout)
{
	return usb_bulk_msg(usb_dev, pipe, data, len, actual_length, timeout);
}
EXPORT_SYMBOL_GPL(usb_interrupt_msg);

/*
 * 中文注释: usb_bulk_msg() -- 同步批量传输的完整实现
 *
 * 这是同步批量/中断传输的"主力"函数。usb_interrupt_msg() 和
 * usb_bulk_msg_killable() 都基于此实现。
 *
 === 自动端点类型检测 ===
 * 函数首先通过 usb_pipe_endpoint() 获取目标端点的描述符，然后检查
 * bmAttributes 字段:
 *
 *   if ((ep->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
 *           USB_ENDPOINT_XFER_INT) {
 *       // 中断端点: 创建中断 URB
 *       pipe = (pipe & ~(3 << 30)) | (PIPE_INTERRUPT << 30);
 *       usb_fill_int_urb(urb, usb_dev, pipe, data, len,
 *                        usb_api_blocking_completion, NULL,
 *                        ep->desc.bInterval);
 *   } else {
 *       // 批量端点: 创建批量 URB
 *       usb_fill_bulk_urb(urb, usb_dev, pipe, data, len,
 *                         usb_api_blocking_completion, NULL);
 *   }
 *
 * 注意 pipe 的转换: pipe 的高 2 位编码了传输类型（PIPE_CONTROL/PIPE_BULK/
 * PIPE_INTERRUPT/PIPE_ISOCHRONOUS），这里将批量管道强制转换为中断管道，
 * 以确保 HCD 按照中断传输的调度规则处理。
 *
 === 完成回调 ===
 * 与控制传输使用相同的完成回调 usb_api_blocking_completion()。
 * 回调在 URB 完成（成功/失败/取消）时被 HCD 调用，保存状态并唤醒等待进程。
 *
 === 适用场景 ===
 * 批量传输适用于大块数据的可靠传输，具有以下特点:
 * - 无固定带宽保证，利用空闲总线带宽传输
 * - 硬件自动重传错误的数据包（通过 ACK/NAK 握手协议）
 * - 典型应用: USB 存储设备（U 盘、移动硬盘）、USB 网卡、打印机等
 */
/**
 * usb_bulk_msg - Builds a bulk urb, sends it off and waits for completion
 * @usb_dev: pointer to the usb device to send the message to
 * @pipe: endpoint "pipe" to send the message to
 * @data: pointer to the data to send
 * @len: length in bytes of the data to send
 * @actual_length: pointer to a location to put the actual length transferred
 *	in bytes
 * @timeout: time in msecs to wait for the message to complete before timing out
 *
 * Context: task context, might sleep.
 *
 * This function sends a simple bulk message to a specified endpoint
 * and waits for the message to complete, or timeout.
 *
 * Don't use this function from within an interrupt context. If you need
 * an asynchronous message, or need to send a message from within interrupt
 * context, use usb_submit_urb() If a thread in your driver uses this call,
 * make sure your disconnect() method can wait for it to complete. Since you
 * don't have a handle on the URB used, you can't cancel the request.
 *
 * Because there is no usb_interrupt_msg() and no USBDEVFS_INTERRUPT ioctl,
 * users are forced to abuse this routine by using it to submit URBs for
 * interrupt endpoints.  We will take the liberty of creating an interrupt URB
 * (with the default interval) if the target is an interrupt endpoint.
 *
 * Return:
 * If successful, 0. Otherwise a negative error number. The number of actual
 * bytes transferred will be stored in the @actual_length parameter.
 *
 */
int usb_bulk_msg(struct usb_device *usb_dev, unsigned int pipe,
		 void *data, int len, int *actual_length, int timeout)
{
	struct urb *urb;
	struct usb_host_endpoint *ep;

	ep = usb_pipe_endpoint(usb_dev, pipe);
	if (!ep || len < 0)
		return -EINVAL;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	if ((ep->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
			USB_ENDPOINT_XFER_INT) {
		pipe = (pipe & ~(3 << 30)) | (PIPE_INTERRUPT << 30);
		usb_fill_int_urb(urb, usb_dev, pipe, data, len,
				usb_api_blocking_completion, NULL,
				ep->desc.bInterval);
	} else
		usb_fill_bulk_urb(urb, usb_dev, pipe, data, len,
				usb_api_blocking_completion, NULL);

	return usb_start_wait_urb(urb, timeout, actual_length, false);
}
EXPORT_SYMBOL_GPL(usb_bulk_msg);

/**
 * usb_bulk_msg_killable - Builds a bulk urb, sends it off and waits for completion in a killable state
 * @usb_dev: pointer to the usb device to send the message to
 * @pipe: endpoint "pipe" to send the message to
 * @data: pointer to the data to send
 * @len: length in bytes of the data to send
 * @actual_length: pointer to a location to put the actual length transferred
 *	in bytes
 * @timeout: time in msecs to wait for the message to complete before
 *	timing out (if <= 0, the wait is as long as possible)
 *
 * Context: task context, might sleep.
 *
 * This function is just like usb_blk_msg(), except that it waits in a
 * killable state and there is no limit on the timeout length.
 *
 * Return:
 * If successful, 0. Otherwise a negative error number. The number of actual
 * bytes transferred will be stored in the @actual_length parameter.
 *
 */
int usb_bulk_msg_killable(struct usb_device *usb_dev, unsigned int pipe,
		 void *data, int len, int *actual_length, int timeout)
{
	struct urb *urb;
	struct usb_host_endpoint *ep;

	ep = usb_pipe_endpoint(usb_dev, pipe);
	if (!ep || len < 0)
		return -EINVAL;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	if ((ep->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
			USB_ENDPOINT_XFER_INT) {
		pipe = (pipe & ~(3 << 30)) | (PIPE_INTERRUPT << 30);
		usb_fill_int_urb(urb, usb_dev, pipe, data, len,
				usb_api_blocking_completion, NULL,
				ep->desc.bInterval);
	} else
		usb_fill_bulk_urb(urb, usb_dev, pipe, data, len,
				usb_api_blocking_completion, NULL);

	return usb_start_wait_urb(urb, timeout, actual_length, true);
}
EXPORT_SYMBOL_GPL(usb_bulk_msg_killable);

/*-------------------------------------------------------------------*/

static void sg_clean(struct usb_sg_request *io)
{
	if (io->urbs) {
		while (io->entries--)
			usb_free_urb(io->urbs[io->entries]);
		kfree(io->urbs);
		io->urbs = NULL;
	}
	io->dev = NULL;
}

static void sg_complete(struct urb *urb)
{
	unsigned long flags;
	struct usb_sg_request *io = urb->context;
	int status = urb->status;

	spin_lock_irqsave(&io->lock, flags);

	/* In 2.5 we require hcds' endpoint queues not to progress after fault
	 * reports, until the completion callback (this!) returns.  That lets
	 * device driver code (like this routine) unlink queued urbs first,
	 * if it needs to, since the HC won't work on them at all.  So it's
	 * not possible for page N+1 to overwrite page N, and so on.
	 *
	 * That's only for "hard" faults; "soft" faults (unlinks) sometimes
	 * complete before the HCD can get requests away from hardware,
	 * though never during cleanup after a hard fault.
	 */
	if (io->status
			&& (io->status != -ECONNRESET
				|| status != -ECONNRESET)
			&& urb->actual_length) {
		dev_err(io->dev->bus->controller,
			"dev %s ep%d%s scatterlist error %d/%d\n",
			io->dev->devpath,
			usb_endpoint_num(&urb->ep->desc),
			usb_urb_dir_in(urb) ? "in" : "out",
			status, io->status);
		/* BUG (); */
	}

	if (io->status == 0 && status && status != -ECONNRESET) {
		int i, found, retval;

		io->status = status;

		/* the previous urbs, and this one, completed already.
		 * unlink pending urbs so they won't rx/tx bad data.
		 * careful: unlink can sometimes be synchronous...
		 */
		spin_unlock_irqrestore(&io->lock, flags);
		for (i = 0, found = 0; i < io->entries; i++) {
			if (!io->urbs[i])
				continue;
			if (found) {
				usb_block_urb(io->urbs[i]);
				retval = usb_unlink_urb(io->urbs[i]);
				if (retval != -EINPROGRESS &&
				    retval != -ENODEV &&
				    retval != -EBUSY &&
				    retval != -EIDRM)
					dev_err(&io->dev->dev,
						"%s, unlink --> %d\n",
						__func__, retval);
			} else if (urb == io->urbs[i])
				found = 1;
		}
		spin_lock_irqsave(&io->lock, flags);
	}

	/* on the last completion, signal usb_sg_wait() */
	io->bytes += urb->actual_length;
	io->count--;
	if (!io->count)
		complete(&io->complete);

	spin_unlock_irqrestore(&io->lock, flags);
}


/**
 * usb_sg_init - initializes scatterlist-based bulk/interrupt I/O request
 * @io: request block being initialized.  until usb_sg_wait() returns,
 *	treat this as a pointer to an opaque block of memory,
 * @dev: the usb device that will send or receive the data
 * @pipe: endpoint "pipe" used to transfer the data
 * @period: polling rate for interrupt endpoints, in frames or
 * 	(for high speed endpoints) microframes; ignored for bulk
 * @sg: scatterlist entries
 * @nents: how many entries in the scatterlist
 * @length: how many bytes to send from the scatterlist, or zero to
 * 	send every byte identified in the list.
 * @mem_flags: SLAB_* flags affecting memory allocations in this call
 *
 * This initializes a scatter/gather request, allocating resources such as
 * I/O mappings and urb memory (except maybe memory used by USB controller
 * drivers).
 *
 * The request must be issued using usb_sg_wait(), which waits for the I/O to
 * complete (or to be canceled) and then cleans up all resources allocated by
 * usb_sg_init().
 *
 * The request may be canceled with usb_sg_cancel(), either before or after
 * usb_sg_wait() is called.
 *
 * Return: Zero for success, else a negative errno value.
 */
int usb_sg_init(struct usb_sg_request *io, struct usb_device *dev,
		unsigned pipe, unsigned	period, struct scatterlist *sg,
		int nents, size_t length, gfp_t mem_flags)
{
	int i;
	int urb_flags;
	int use_sg;

	if (!io || !dev || !sg
			|| usb_pipecontrol(pipe)
			|| usb_pipeisoc(pipe)
			|| nents <= 0)
		return -EINVAL;

	spin_lock_init(&io->lock);
	io->dev = dev;
	io->pipe = pipe;

	if (dev->bus->sg_tablesize > 0) {
		use_sg = true;
		io->entries = 1;
	} else {
		use_sg = false;
		io->entries = nents;
	}

	/* initialize all the urbs we'll use */
	io->urbs = kmalloc_objs(*io->urbs, io->entries, mem_flags);
	if (!io->urbs)
		goto nomem;

	urb_flags = URB_NO_INTERRUPT;
	if (usb_pipein(pipe))
		urb_flags |= URB_SHORT_NOT_OK;

	for_each_sg(sg, sg, io->entries, i) {
		struct urb *urb;
		unsigned len;

		urb = usb_alloc_urb(0, mem_flags);
		if (!urb) {
			io->entries = i;
			goto nomem;
		}
		io->urbs[i] = urb;

		urb->dev = NULL;
		urb->pipe = pipe;
		urb->interval = period;
		urb->transfer_flags = urb_flags;
		urb->complete = sg_complete;
		urb->context = io;
		urb->sg = sg;

		if (use_sg) {
			/* There is no single transfer buffer */
			urb->transfer_buffer = NULL;
			urb->num_sgs = nents;

			/* A length of zero means transfer the whole sg list */
			len = length;
			if (len == 0) {
				struct scatterlist	*sg2;
				int			j;

				for_each_sg(sg, sg2, nents, j)
					len += sg2->length;
			}
		} else {
			/*
			 * Some systems can't use DMA; they use PIO instead.
			 * For their sakes, transfer_buffer is set whenever
			 * possible.
			 */
			if (!PageHighMem(sg_page(sg)))
				urb->transfer_buffer = sg_virt(sg);
			else
				urb->transfer_buffer = NULL;

			len = sg->length;
			if (length) {
				len = min_t(size_t, len, length);
				length -= len;
				if (length == 0)
					io->entries = i + 1;
			}
		}
		urb->transfer_buffer_length = len;
	}
	io->urbs[--i]->transfer_flags &= ~URB_NO_INTERRUPT;

	/* transaction state */
	io->count = io->entries;
	io->status = 0;
	io->bytes = 0;
	init_completion(&io->complete);
	return 0;

nomem:
	sg_clean(io);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(usb_sg_init);

/**
 * usb_sg_wait - synchronously execute scatter/gather request
 * @io: request block handle, as initialized with usb_sg_init().
 * 	some fields become accessible when this call returns.
 *
 * Context: task context, might sleep.
 *
 * This function blocks until the specified I/O operation completes.  It
 * leverages the grouping of the related I/O requests to get good transfer
 * rates, by queueing the requests.  At higher speeds, such queuing can
 * significantly improve USB throughput.
 *
 * There are three kinds of completion for this function.
 *
 * (1) success, where io->status is zero.  The number of io->bytes
 *     transferred is as requested.
 * (2) error, where io->status is a negative errno value.  The number
 *     of io->bytes transferred before the error is usually less
 *     than requested, and can be nonzero.
 * (3) cancellation, a type of error with status -ECONNRESET that
 *     is initiated by usb_sg_cancel().
 *
 * When this function returns, all memory allocated through usb_sg_init() or
 * this call will have been freed.  The request block parameter may still be
 * passed to usb_sg_cancel(), or it may be freed.  It could also be
 * reinitialized and then reused.
 *
 * Data Transfer Rates:
 *
 * Bulk transfers are valid for full or high speed endpoints.
 * The best full speed data rate is 19 packets of 64 bytes each
 * per frame, or 1216 bytes per millisecond.
 * The best high speed data rate is 13 packets of 512 bytes each
 * per microframe, or 52 KBytes per millisecond.
 *
 * The reason to use interrupt transfers through this API would most likely
 * be to reserve high speed bandwidth, where up to 24 KBytes per millisecond
 * could be transferred.  That capability is less useful for low or full
 * speed interrupt endpoints, which allow at most one packet per millisecond,
 * of at most 8 or 64 bytes (respectively).
 *
 * It is not necessary to call this function to reserve bandwidth for devices
 * under an xHCI host controller, as the bandwidth is reserved when the
 * configuration or interface alt setting is selected.
 */
void usb_sg_wait(struct usb_sg_request *io)
{
	int i;
	int entries = io->entries;

	/* queue the urbs.  */
	spin_lock_irq(&io->lock);
	i = 0;
	while (i < entries && !io->status) {
		int retval;

		io->urbs[i]->dev = io->dev;
		spin_unlock_irq(&io->lock);

		retval = usb_submit_urb(io->urbs[i], GFP_NOIO);

		switch (retval) {
			/* maybe we retrying will recover */
		case -ENXIO:	/* hc didn't queue this one */
		case -EAGAIN:
		case -ENOMEM:
			retval = 0;
			yield();
			break;

			/* no error? continue immediately.
			 *
			 * NOTE: to work better with UHCI (4K I/O buffer may
			 * need 3K of TDs) it may be good to limit how many
			 * URBs are queued at once; N milliseconds?
			 */
		case 0:
			++i;
			cpu_relax();
			break;

			/* fail any uncompleted urbs */
		default:
			io->urbs[i]->status = retval;
			dev_dbg(&io->dev->dev, "%s, submit --> %d\n",
				__func__, retval);
			usb_sg_cancel(io);
		}
		spin_lock_irq(&io->lock);
		if (retval && (io->status == 0 || io->status == -ECONNRESET))
			io->status = retval;
	}
	io->count -= entries - i;
	if (io->count == 0)
		complete(&io->complete);
	spin_unlock_irq(&io->lock);

	/* OK, yes, this could be packaged as non-blocking.
	 * So could the submit loop above ... but it's easier to
	 * solve neither problem than to solve both!
	 */
	wait_for_completion(&io->complete);

	sg_clean(io);
}
EXPORT_SYMBOL_GPL(usb_sg_wait);

/**
 * usb_sg_cancel - stop scatter/gather i/o issued by usb_sg_wait()
 * @io: request block, initialized with usb_sg_init()
 *
 * This stops a request after it has been started by usb_sg_wait().
 * It can also prevents one initialized by usb_sg_init() from starting,
 * so that call just frees resources allocated to the request.
 */
void usb_sg_cancel(struct usb_sg_request *io)
{
	unsigned long flags;
	int i, retval;

	spin_lock_irqsave(&io->lock, flags);
	if (io->status || io->count == 0) {
		spin_unlock_irqrestore(&io->lock, flags);
		return;
	}
	/* shut everything down */
	io->status = -ECONNRESET;
	io->count++;		/* Keep the request alive until we're done */
	spin_unlock_irqrestore(&io->lock, flags);

	for (i = io->entries - 1; i >= 0; --i) {
		usb_block_urb(io->urbs[i]);

		retval = usb_unlink_urb(io->urbs[i]);
		if (retval != -EINPROGRESS
		    && retval != -ENODEV
		    && retval != -EBUSY
		    && retval != -EIDRM)
			dev_warn(&io->dev->dev, "%s, unlink --> %d\n",
				 __func__, retval);
	}

	spin_lock_irqsave(&io->lock, flags);
	io->count--;
	if (!io->count)
		complete(&io->complete);
	spin_unlock_irqrestore(&io->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_sg_cancel);

/*-------------------------------------------------------------------*/

/*
 * 中文注释: usb_get_descriptor() -- 读取 USB 描述符的通用接口
 *
 * 这是用户空间和内核空间读取 USB 描述符最常用的底层函数之一。
 * USB 描述符是 USB 设备的"身份证"，包含了设备的所有关键信息。
 *
 === 描述符类型 ===
 * USB 规范定义了多种标准描述符类型（通过 USB_DT_* 常量区分）:
 *   USB_DT_DEVICE       (1)  -- 设备描述符: 包含 VID/PID、USB 版本、设备类等
 *   USB_DT_CONFIG       (2)  -- 配置描述符: 包含供电方式、接口数量、最大电流等
 *   USB_DT_STRING       (3)  -- 字符串描述符: 可读的厂商/产品/序列号信息
 *   USB_DT_INTERFACE    (4)  -- 接口描述符: 描述一个功能接口
 *   USB_DT_ENDPOINT     (5)  -- 端点描述符: 描述一个端点的类型和最大包长
 *   USB_DT_DEVICE_QUAL  (6)  -- 设备限定符（USB 2.0 高速设备）
 *   USB_DT_OTHER_SPEED  (7)  -- 其他速度配置
 *   USB_DT_INTERFACE_POWER (8) -- 接口电源
 *   此外还有类特定（class-specific）和厂商特定（vendor-specific）描述符。
 *
 === 重试机制 ===
 * 函数内部最多重试 3 次（for 循环 i < 3）:
 *   - 若 result <= 0 且非 -ETIMEDOUT（超时），继续重试
 *   - 若接收到的描述符类型（buf[1]）与请求的类型不匹配，返回 -ENODATA
 *   - 否则跳出循环
 * 这种重试是因为一些 USB 设备在首次上电时可能无法立即返回描述符，
 * 需要额外的延迟或重试。这是处理 USB 规范中"设备就绪延迟"的经典方式。
 *
 === SETUP 包的构造 ===
 * 发送的控制请求:
 *   bmRequestType = USB_DIR_IN (Device-to-Host)
 *   bRequest      = USB_REQ_GET_DESCRIPTOR (0x06)
 *   wValue        = (type << 8) + index  -- 高字节: 描述符类型, 低字节: 索引
 *   wIndex        = 0                    -- 对非字符串描述符为 0
 *   wLength       = size                 -- 请求读取的字节数
 *
 === 调用链 ===
 * usb_get_descriptor()
 *   -> usb_control_msg()
 *      -> usb_internal_control_msg()
 *         -> usb_start_wait_urb()
 *
 * 返回值: 成功时返回接收到的字节数，失败时返回负错误码。
 */
/**
 * usb_get_descriptor - issues a generic GET_DESCRIPTOR request
 * @dev: the device whose descriptor is being retrieved
 * @type: the descriptor type (USB_DT_*)
 * @index: the number of the descriptor
 * @buf: where to put the descriptor
 * @size: how big is "buf"?
 *
 * Context: task context, might sleep.
 *
 * Gets a USB descriptor.  Convenience functions exist to simplify
 * getting some types of descriptors.  Use
 * usb_get_string() or usb_string() for USB_DT_STRING.
 * Device (USB_DT_DEVICE) and configuration descriptors (USB_DT_CONFIG)
 * are part of the device structure.
 * In addition to a number of USB-standard descriptors, some
 * devices also use class-specific or vendor-specific descriptors.
 *
 * This call is synchronous, and may not be used in an interrupt context.
 *
 * Return: The number of bytes received on success, or else the status code
 * returned by the underlying usb_control_msg() call.
 */
int usb_get_descriptor(struct usb_device *dev, unsigned char type,
		       unsigned char index, void *buf, int size)
{
	int i;
	int result;

	if (size <= 0)		/* No point in asking for no data */
		return -EINVAL;

	memset(buf, 0, size);	/* Make sure we parse really received data */

	for (i = 0; i < 3; ++i) {
		/* retry on length 0 or error; some devices are flakey */
		result = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
				USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
				(type << 8) + index, 0, buf, size,
				USB_CTRL_GET_TIMEOUT);
		if (result <= 0 && result != -ETIMEDOUT)
			continue;
		if (result > 1 && ((u8 *)buf)[1] != type) {
			result = -ENODATA;
			continue;
		}
		break;
	}
	return result;
}
EXPORT_SYMBOL_GPL(usb_get_descriptor);

/*
 * 中文注释: usb_get_string() -- 读取 USB 字符串描述符
 *
 * 字符串描述符是 USB 设备以 UTF-16LE（Unicode 小端序）编码的人类可读信息，
 * 通常包含厂商名称、产品名称和序列号。
 *
 === 语言 ID 机制 ===
 * USB 设备可以支持多种语言的字符串描述符。语言 ID 的获取流程:
 *
 *   (1) 首先读取字符串描述符 0（索引为 0），它不是一个真正的字符串，
 *       而是一个语言 ID 数组（每个语言 ID 占 2 字节）。
 *       例如: 0x0409 表示英语（美国），0x0804 表示中文（简体）。
 *
 *   (2) 使用第一个语言 ID 作为后续字符串读取的 langid 参数。
 *       设备通常将首选语言列在第一位。
 *
 *   (3) 然后使用选定的语言 ID 和字符串索引读取实际的字符串描述符。
 *
 === SETUP 包 ===
 * 与 usb_get_descriptor() 类似，但 wIndex 被设置为语言 ID:
 *   bmRequestType = USB_DIR_IN
 *   bRequest      = USB_REQ_GET_DESCRIPTOR (0x06)
 *   wValue        = (USB_DT_STRING << 8) + index
 *   wIndex        = langid     -- 语言 ID
 *   wLength       = size
 *
 === 重试机制 ===
 * 与 usb_get_descriptor() 一样最多重试 3 次:
 *   - 若 result == 0 或 result == -EPIPE（端点停止），继续重试
 *   - 若接收到的描述符类型不是 USB_DT_STRING，返回 -ENODATA
 *
 * 返回值: 成功时返回原始 UTF-16LE 字节数，失败时返回负错误码。
 *
 * 注意: 外部驱动的入口点通常使用 usb_string()，它会自动处理语言 ID 获取
 * 和 UTF-16LE 到 UTF-8 的转换。usb_get_string() 是内部函数，返回原始编码。
 */
/**
 * usb_get_string - gets a string descriptor
 * @dev: the device whose string descriptor is being retrieved
 * @langid: code for language chosen (from string descriptor zero)
 * @index: the number of the descriptor
 * @buf: where to put the string
 * @size: how big is "buf"?
 *
 * Context: task context, might sleep.
 *
 * Retrieves a string, encoded using UTF-16LE (Unicode, 16 bits per character,
 * in little-endian byte order).
 * The usb_string() function will often be a convenient way to turn
 * these strings into kernel-printable form.
 *
 * Strings may be referenced in device, configuration, interface, or other
 * descriptors, and could also be used in vendor-specific ways.
 *
 * This call is synchronous, and may not be used in an interrupt context.
 *
 * Return: The number of bytes received on success, or else the status code
 * returned by the underlying usb_control_msg() call.
 */
static int usb_get_string(struct usb_device *dev, unsigned short langid,
			  unsigned char index, void *buf, int size)
{
	int i;
	int result;

	if (size <= 0)		/* No point in asking for no data */
		return -EINVAL;

	for (i = 0; i < 3; ++i) {
		/* retry on length 0 or stall; some devices are flakey */
		result = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
			USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
			(USB_DT_STRING << 8) + index, langid, buf, size,
			USB_CTRL_GET_TIMEOUT);
		if (result == 0 || result == -EPIPE)
			continue;
		if (result > 1 && ((u8 *) buf)[1] != USB_DT_STRING) {
			result = -ENODATA;
			continue;
		}
		break;
	}
	return result;
}

static void usb_try_string_workarounds(unsigned char *buf, int *length)
{
	int newlength, oldlength = *length;

	for (newlength = 2; newlength + 1 < oldlength; newlength += 2)
		if (!isprint(buf[newlength]) || buf[newlength + 1])
			break;

	if (newlength > 2) {
		buf[0] = newlength;
		*length = newlength;
	}
}

static int usb_string_sub(struct usb_device *dev, unsigned int langid,
			  unsigned int index, unsigned char *buf)
{
	int rc;

	/* Try to read the string descriptor by asking for the maximum
	 * possible number of bytes */
	if (dev->quirks & USB_QUIRK_STRING_FETCH_255)
		rc = -EIO;
	else
		rc = usb_get_string(dev, langid, index, buf, 255);

	/* If that failed try to read the descriptor length, then
	 * ask for just that many bytes */
	if (rc < 2) {
		rc = usb_get_string(dev, langid, index, buf, 2);
		if (rc == 2)
			rc = usb_get_string(dev, langid, index, buf, buf[0]);
	}

	if (rc >= 2) {
		if (!buf[0] && !buf[1])
			usb_try_string_workarounds(buf, &rc);

		/* There might be extra junk at the end of the descriptor */
		if (buf[0] < rc)
			rc = buf[0];

		rc = rc - (rc & 1); /* force a multiple of two */
	}

	if (rc < 2)
		rc = (rc < 0 ? rc : -EINVAL);

	return rc;
}

static int usb_get_langid(struct usb_device *dev, unsigned char *tbuf)
{
	int err;

	if (dev->have_langid)
		return 0;

	if (dev->string_langid < 0)
		return -EPIPE;

	err = usb_string_sub(dev, 0, 0, tbuf);

	/* If the string was reported but is malformed, default to english
	 * (0x0409) */
	if (err == -ENODATA || (err > 0 && err < 4)) {
		dev->string_langid = 0x0409;
		dev->have_langid = 1;
		dev_err(&dev->dev,
			"language id specifier not provided by device, defaulting to English\n");
		return 0;
	}

	/* In case of all other errors, we assume the device is not able to
	 * deal with strings at all. Set string_langid to -1 in order to
	 * prevent any string to be retrieved from the device */
	if (err < 0) {
		dev_info(&dev->dev, "string descriptor 0 read error: %d\n",
					err);
		dev->string_langid = -1;
		return -EPIPE;
	}

	/* always use the first langid listed */
	dev->string_langid = tbuf[2] | (tbuf[3] << 8);
	dev->have_langid = 1;
	dev_dbg(&dev->dev, "default language 0x%04x\n",
				dev->string_langid);
	return 0;
}

/**
 * usb_string - returns UTF-8 version of a string descriptor
 * @dev: the device whose string descriptor is being retrieved
 * @index: the number of the descriptor
 * @buf: where to put the string
 * @size: how big is "buf"?
 *
 * Context: task context, might sleep.
 *
 * This converts the UTF-16LE encoded strings returned by devices, from
 * usb_get_string_descriptor(), to null-terminated UTF-8 encoded ones
 * that are more usable in most kernel contexts.  Note that this function
 * chooses strings in the first language supported by the device.
 *
 * This call is synchronous, and may not be used in an interrupt context.
 *
 * Return: length of the string (>= 0) or usb_control_msg status (< 0).
 */
int usb_string(struct usb_device *dev, int index, char *buf, size_t size)
{
	unsigned char *tbuf;
	int err;

	if (dev->state == USB_STATE_SUSPENDED)
		return -EHOSTUNREACH;
	if (size <= 0 || !buf)
		return -EINVAL;
	buf[0] = 0;
	if (index <= 0 || index >= 256)
		return -EINVAL;
	tbuf = kmalloc(256, GFP_NOIO);
	if (!tbuf)
		return -ENOMEM;

	err = usb_get_langid(dev, tbuf);
	if (err < 0)
		goto errout;

	err = usb_string_sub(dev, dev->string_langid, index, tbuf);
	if (err < 0)
		goto errout;

	size--;		/* leave room for trailing NULL char in output buffer */
	err = utf16s_to_utf8s((wchar_t *) &tbuf[2], (err - 2) / 2,
			UTF16_LITTLE_ENDIAN, buf, size);
	buf[err] = 0;

	if (tbuf[1] != USB_DT_STRING)
		dev_dbg(&dev->dev,
			"wrong descriptor type %02x for string %d (\"%s\")\n",
			tbuf[1], index, buf);

 errout:
	kfree(tbuf);
	return err;
}
EXPORT_SYMBOL_GPL(usb_string);

/* one 16-bit character, when UTF-8-encoded, has at most three bytes */
#define MAX_USB_STRING_SIZE (127 * 3 + 1)

/**
 * usb_cache_string - read a string descriptor and cache it for later use
 * @udev: the device whose string descriptor is being read
 * @index: the descriptor index
 *
 * Return: A pointer to a kmalloc'ed buffer containing the descriptor string,
 * or %NULL if the index is 0 or the string could not be read.
 */
char *usb_cache_string(struct usb_device *udev, int index)
{
	char *buf;
	char *smallbuf = NULL;
	int len;

	if (index <= 0)
		return NULL;

	buf = kmalloc(MAX_USB_STRING_SIZE, GFP_NOIO);
	if (!buf)
		return NULL;

	len = usb_string(udev, index, buf, MAX_USB_STRING_SIZE);
	if (len <= 0) {
		kfree(buf);
		return NULL;
	}

	smallbuf = krealloc(buf, len + 1, GFP_NOIO);
	if (unlikely(!smallbuf))
		return buf;
	return smallbuf;
}
EXPORT_SYMBOL_GPL(usb_cache_string);

/*
 * usb_get_device_descriptor - read the device descriptor
 * @udev: the device whose device descriptor should be read
 *
 * Context: task context, might sleep.
 *
 * Not exported, only for use by the core.  If drivers really want to read
 * the device descriptor directly, they can call usb_get_descriptor() with
 * type = USB_DT_DEVICE and index = 0.
 *
 * Returns: a pointer to a dynamically allocated usb_device_descriptor
 * structure (which the caller must deallocate), or an ERR_PTR value.
 */
struct usb_device_descriptor *usb_get_device_descriptor(struct usb_device *udev)
{
	struct usb_device_descriptor *desc;
	int ret;

	desc = kmalloc_obj(*desc, GFP_NOIO);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	ret = usb_get_descriptor(udev, USB_DT_DEVICE, 0, desc, sizeof(*desc));
	if (ret == sizeof(*desc))
		return desc;

	if (ret >= 0)
		ret = -EMSGSIZE;
	kfree(desc);
	return ERR_PTR(ret);
}

/*
 * usb_set_isoch_delay - informs the device of the packet transmit delay
 * @dev: the device whose delay is to be informed
 * Context: task context, might sleep
 *
 * Since this is an optional request, we don't bother if it fails.
 */
int usb_set_isoch_delay(struct usb_device *dev)
{
	/* skip hub devices */
	if (dev->descriptor.bDeviceClass == USB_CLASS_HUB)
		return 0;

	/* skip non-SS/non-SSP devices */
	if (dev->speed < USB_SPEED_SUPER)
		return 0;

	return usb_control_msg_send(dev, 0,
			USB_REQ_SET_ISOCH_DELAY,
			USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
			dev->hub_delay, 0, NULL, 0,
			USB_CTRL_SET_TIMEOUT,
			GFP_NOIO);
}

/*
 * 中文注释: usb_get_status() -- 获取 USB 设备/接口/端点状态
 *
 * 对应 USB 规范第 9.4.5 节的 GET_STATUS 请求。这是一个标准 USB 请求，
 * 用于查询设备、接口或端点的当前状态。
 *
 === 接收者和状态类型 ===
 * 根据接收者（recip）的不同，返回的状态含义也不同:
 *
 *   USB_RECIP_DEVICE (0x00) -- 设备状态（2 字节）:
 *     bit 0: Self Powered（自供电）
 *     bit 1: Remote Wakeup（远程唤醒已使能）
 *     bit 2-15: 保留
 *
 *   USB_RECIP_INTERFACE (0x01) -- 接口状态（2 字节）:
 *     目前所有位保留，始终返回 0
 *
 *   USB_RECIP_ENDPOINT (0x02) -- 端点状态（2 字节）:
 *     bit 0: Halt（端点已停止/STALL）
 *     bit 1-15: 保留
 *
 * 状态类型（type）参数支持:
 *   USB_STATUS_TYPE_STANDARD (0) -- 标准状态，2 字节
 *   USB_STATUS_TYPE_PTM (1)      -- PTM（精确时间测量）状态，4 字节
 *                                  仅用于 USB_RECIP_DEVICE
 *
 === 使用场景 ===
 * - 检查设备是否自供电（用于电源管理决策）
 * - 检查远程唤醒是否使能
 * - 检查端点是否处于 HALT 状态（用于从 STALL 中恢复）
 * - 支持 PTM 的 USB 设备获取精确时间测量状态
 *
 * 状态位通过 SET_FEATURE 请求设置，通过 CLEAR_FEATURE 请求清除。
 * 对于端点 HALT 状态的清除，推荐使用 usb_clear_halt() 函数，
 * 它会在发送 CLEAR_FEATURE 后自动重置端点的数据切换位（DATA0/DATA1）。
 */
/**
 * usb_get_status - issues a GET_STATUS call
 * @dev: the device whose status is being checked
 * @recip: USB_RECIP_*; for device, interface, or endpoint
 * @type: USB_STATUS_TYPE_*; for standard or PTM status types
 * @target: zero (for device), else interface or endpoint number
 * @data: pointer to two bytes of bitmap data
 *
 * Context: task context, might sleep.
 *
 * Returns device, interface, or endpoint status.  Normally only of
 * interest to see if the device is self powered, or has enabled the
 * remote wakeup facility; or whether a bulk or interrupt endpoint
 * is halted ("stalled").
 *
 * Bits in these status bitmaps are set using the SET_FEATURE request,
 * and cleared using the CLEAR_FEATURE request.  The usb_clear_halt()
 * function should be used to clear halt ("stall") status.
 *
 * This call is synchronous, and may not be used in an interrupt context.
 *
 * Returns 0 and the status value in *@data (in host byte order) on success,
 * or else the status code from the underlying usb_control_msg() call.
 */
int usb_get_status(struct usb_device *dev, int recip, int type, int target,
		void *data)
{
	int ret;
	void *status;
	int length;

	switch (type) {
	case USB_STATUS_TYPE_STANDARD:
		length = 2;
		break;
	case USB_STATUS_TYPE_PTM:
		if (recip != USB_RECIP_DEVICE)
			return -EINVAL;

		length = 4;
		break;
	default:
		return -EINVAL;
	}

	status =  kmalloc(length, GFP_KERNEL);
	if (!status)
		return -ENOMEM;

	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
		USB_REQ_GET_STATUS, USB_DIR_IN | recip, USB_STATUS_TYPE_STANDARD,
		target, status, length, USB_CTRL_GET_TIMEOUT);

	switch (ret) {
	case 4:
		if (type != USB_STATUS_TYPE_PTM) {
			ret = -EIO;
			break;
		}

		*(u32 *) data = le32_to_cpu(*(__le32 *) status);
		ret = 0;
		break;
	case 2:
		if (type != USB_STATUS_TYPE_STANDARD) {
			ret = -EIO;
			break;
		}

		*(u16 *) data = le16_to_cpu(*(__le16 *) status);
		ret = 0;
		break;
	default:
		ret = -EIO;
	}

	kfree(status);
	return ret;
}
EXPORT_SYMBOL_GPL(usb_get_status);

/**
 * usb_clear_halt - tells device to clear endpoint halt/stall condition
 * @dev: device whose endpoint is halted
 * @pipe: endpoint "pipe" being cleared
 *
 * Context: task context, might sleep.
 *
 * This is used to clear halt conditions for bulk and interrupt endpoints,
 * as reported by URB completion status.  Endpoints that are halted are
 * sometimes referred to as being "stalled".  Such endpoints are unable
 * to transmit or receive data until the halt status is cleared.  Any URBs
 * queued for such an endpoint should normally be unlinked by the driver
 * before clearing the halt condition, as described in sections 5.7.5
 * and 5.8.5 of the USB 2.0 spec.
 *
 * Note that control and isochronous endpoints don't halt, although control
 * endpoints report "protocol stall" (for unsupported requests) using the
 * same status code used to report a true stall.
 *
 * This call is synchronous, and may not be used in an interrupt context.
 * If a thread in your driver uses this call, make sure your disconnect()
 * method can wait for it to complete.
 *
 * Return: Zero on success, or else the status code returned by the
 * underlying usb_control_msg() call.
 */
int usb_clear_halt(struct usb_device *dev, int pipe)
{
	int result;
	int endp = usb_pipeendpoint(pipe);

	if (usb_pipein(pipe))
		endp |= USB_DIR_IN;

	/* we don't care if it wasn't halted first. in fact some devices
	 * (like some ibmcam model 1 units) seem to expect hosts to make
	 * this request for iso endpoints, which can't halt!
	 */
	result = usb_control_msg_send(dev, 0,
				      USB_REQ_CLEAR_FEATURE, USB_RECIP_ENDPOINT,
				      USB_ENDPOINT_HALT, endp, NULL, 0,
				      USB_CTRL_SET_TIMEOUT, GFP_NOIO);

	/* don't un-halt or force to DATA0 except on success */
	if (result)
		return result;

	/* NOTE:  seems like Microsoft and Apple don't bother verifying
	 * the clear "took", so some devices could lock up if you check...
	 * such as the Hagiwara FlashGate DUAL.  So we won't bother.
	 *
	 * NOTE:  make sure the logic here doesn't diverge much from
	 * the copy in usb-storage, for as long as we need two copies.
	 */

	usb_reset_endpoint(dev, endp);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_clear_halt);

static int create_intf_ep_devs(struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt = intf->cur_altsetting;
	int i;

	if (intf->ep_devs_created || intf->unregistering)
		return 0;

	for (i = 0; i < alt->desc.bNumEndpoints; ++i)
		(void) usb_create_ep_devs(&intf->dev, &alt->endpoint[i], udev);
	intf->ep_devs_created = 1;
	return 0;
}

static void remove_intf_ep_devs(struct usb_interface *intf)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	int i;

	if (!intf->ep_devs_created)
		return;

	for (i = 0; i < alt->desc.bNumEndpoints; ++i)
		usb_remove_ep_devs(&alt->endpoint[i]);
	intf->ep_devs_created = 0;
}

/**
 * usb_disable_endpoint -- Disable an endpoint by address
 * @dev: the device whose endpoint is being disabled
 * @epaddr: the endpoint's address.  Endpoint number for output,
 *	endpoint number + USB_DIR_IN for input
 * @reset_hardware: flag to erase any endpoint state stored in the
 *	controller hardware
 *
 * Disables the endpoint for URB submission and nukes all pending URBs.
 * If @reset_hardware is set then also deallocates hcd/hardware state
 * for the endpoint.
 */
void usb_disable_endpoint(struct usb_device *dev, unsigned int epaddr,
		bool reset_hardware)
{
	unsigned int epnum = epaddr & USB_ENDPOINT_NUMBER_MASK;
	struct usb_host_endpoint *ep;

	if (!dev)
		return;

	if (usb_endpoint_out(epaddr)) {
		ep = dev->ep_out[epnum];
		if (reset_hardware && epnum != 0)
			dev->ep_out[epnum] = NULL;
	} else {
		ep = dev->ep_in[epnum];
		if (reset_hardware && epnum != 0)
			dev->ep_in[epnum] = NULL;
	}
	if (ep) {
		ep->enabled = 0;
		usb_hcd_flush_endpoint(dev, ep);
		if (reset_hardware)
			usb_hcd_disable_endpoint(dev, ep);
	}
}

/**
 * usb_reset_endpoint - Reset an endpoint's state.
 * @dev: the device whose endpoint is to be reset
 * @epaddr: the endpoint's address.  Endpoint number for output,
 *	endpoint number + USB_DIR_IN for input
 *
 * Resets any host-side endpoint state such as the toggle bit,
 * sequence number or current window.
 */
void usb_reset_endpoint(struct usb_device *dev, unsigned int epaddr)
{
	unsigned int epnum = epaddr & USB_ENDPOINT_NUMBER_MASK;
	struct usb_host_endpoint *ep;

	if (usb_endpoint_out(epaddr))
		ep = dev->ep_out[epnum];
	else
		ep = dev->ep_in[epnum];
	if (ep)
		usb_hcd_reset_endpoint(dev, ep);
}
EXPORT_SYMBOL_GPL(usb_reset_endpoint);


/**
 * usb_disable_interface -- Disable all endpoints for an interface
 * @dev: the device whose interface is being disabled
 * @intf: pointer to the interface descriptor
 * @reset_hardware: flag to erase any endpoint state stored in the
 *	controller hardware
 *
 * Disables all the endpoints for the interface's current altsetting.
 */
void usb_disable_interface(struct usb_device *dev, struct usb_interface *intf,
		bool reset_hardware)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	int i;

	for (i = 0; i < alt->desc.bNumEndpoints; ++i) {
		usb_disable_endpoint(dev,
				alt->endpoint[i].desc.bEndpointAddress,
				reset_hardware);
	}
}

/*
 * usb_disable_device_endpoints -- Disable all endpoints for a device
 * @dev: the device whose endpoints are being disabled
 * @skip_ep0: 0 to disable endpoint 0, 1 to skip it.
 */
static void usb_disable_device_endpoints(struct usb_device *dev, int skip_ep0)
{
	struct usb_hcd *hcd = bus_to_hcd(dev->bus);
	int i;

	if (hcd->driver->check_bandwidth) {
		/* First pass: Cancel URBs, leave endpoint pointers intact. */
		for (i = skip_ep0; i < 16; ++i) {
			usb_disable_endpoint(dev, i, false);
			usb_disable_endpoint(dev, i + USB_DIR_IN, false);
		}
		/* Remove endpoints from the host controller internal state */
		mutex_lock(hcd->bandwidth_mutex);
		usb_hcd_alloc_bandwidth(dev, NULL, NULL, NULL);
		mutex_unlock(hcd->bandwidth_mutex);
	}
	/* Second pass: remove endpoint pointers */
	for (i = skip_ep0; i < 16; ++i) {
		usb_disable_endpoint(dev, i, true);
		usb_disable_endpoint(dev, i + USB_DIR_IN, true);
	}
}

/**
 * usb_disable_device - Disable all the endpoints for a USB device
 * @dev: the device whose endpoints are being disabled
 * @skip_ep0: 0 to disable endpoint 0, 1 to skip it.
 *
 * Disables all the device's endpoints, potentially including endpoint 0.
 * Deallocates hcd/hardware state for the endpoints (nuking all or most
 * pending urbs) and usbcore state for the interfaces, so that usbcore
 * must usb_set_configuration() before any interfaces could be used.
 */
void usb_disable_device(struct usb_device *dev, int skip_ep0)
{
	int i;

	/* getting rid of interfaces will disconnect
	 * any drivers bound to them (a key side effect)
	 */
	if (dev->actconfig) {
		/*
		 * FIXME: In order to avoid self-deadlock involving the
		 * bandwidth_mutex, we have to mark all the interfaces
		 * before unregistering any of them.
		 */
		for (i = 0; i < dev->actconfig->desc.bNumInterfaces; i++)
			dev->actconfig->interface[i]->unregistering = 1;

		for (i = 0; i < dev->actconfig->desc.bNumInterfaces; i++) {
			struct usb_interface	*interface;

			/* remove this interface if it has been registered */
			interface = dev->actconfig->interface[i];
			if (!device_is_registered(&interface->dev))
				continue;
			dev_dbg(&dev->dev, "unregistering interface %s\n",
				dev_name(&interface->dev));
			remove_intf_ep_devs(interface);
			device_del(&interface->dev);
		}

		/* Now that the interfaces are unbound, nobody should
		 * try to access them.
		 */
		for (i = 0; i < dev->actconfig->desc.bNumInterfaces; i++) {
			put_device(&dev->actconfig->interface[i]->dev);
			dev->actconfig->interface[i] = NULL;
		}

		usb_disable_usb2_hardware_lpm(dev);
		usb_unlocked_disable_lpm(dev);
		usb_disable_ltm(dev);

		dev->actconfig = NULL;
		if (dev->state == USB_STATE_CONFIGURED)
			usb_set_device_state(dev, USB_STATE_ADDRESS);
	}

	dev_dbg(&dev->dev, "%s nuking %s URBs\n", __func__,
		skip_ep0 ? "non-ep0" : "all");

	usb_disable_device_endpoints(dev, skip_ep0);
}

/**
 * usb_enable_endpoint - Enable an endpoint for USB communications
 * @dev: the device whose interface is being enabled
 * @ep: the endpoint
 * @reset_ep: flag to reset the endpoint state
 *
 * Resets the endpoint state if asked, and sets dev->ep_{in,out} pointers.
 * For control endpoints, both the input and output sides are handled.
 */
void usb_enable_endpoint(struct usb_device *dev, struct usb_host_endpoint *ep,
		bool reset_ep)
{
	int epnum = usb_endpoint_num(&ep->desc);
	int is_out = usb_endpoint_dir_out(&ep->desc);
	int is_control = usb_endpoint_xfer_control(&ep->desc);

	if (reset_ep)
		usb_hcd_reset_endpoint(dev, ep);
	if (is_out || is_control)
		dev->ep_out[epnum] = ep;
	if (!is_out || is_control)
		dev->ep_in[epnum] = ep;
	ep->enabled = 1;
}

/**
 * usb_enable_interface - Enable all the endpoints for an interface
 * @dev: the device whose interface is being enabled
 * @intf: pointer to the interface descriptor
 * @reset_eps: flag to reset the endpoints' state
 *
 * Enables all the endpoints for the interface's current altsetting.
 */
void usb_enable_interface(struct usb_device *dev,
		struct usb_interface *intf, bool reset_eps)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	int i;

	for (i = 0; i < alt->desc.bNumEndpoints; ++i)
		usb_enable_endpoint(dev, &alt->endpoint[i], reset_eps);
}

/*
 * 中文注释: usb_set_interface() -- 选择接口的备用设置（Alternate Setting）
 *
 * USB 规范允许一个接口（Interface）拥有多个备用设置（Alternate Setting）。
 * 每个备用设置可以有不同的端点配置和带宽需求。这是 USB 设备实现
 * 动态带宽管理的关键机制。
 *
 === 备用设置的作用 ===
 * 典型场景: USB 视频设备（UVC）
 *   - 备用设置 0: 无等时端点，消耗 0 带宽（设备的"空闲"模式）
 *   - 备用设置 1: 一个等时端点，最大包长 256 字节
 *   - 备用设置 2: 一个等时端点，最大包长 512 字节
 *   - 备用设置 3: 一个等时端点，最大包长 1024 字节
 * 当应用程序开始视频流时，驱动选择更高的备用设置以获得更多带宽；
 * 当视频流停止时，切换回备用设置 0 以释放 USB 总线带宽。
 *
 === 执行流程 ===
 *   (1) 参数验证
 *       -- 检查设备是否挂起，接口号和备用设置号是否有效
 *
 *   (2) 刷新旧端点
 *       -- 调用 usb_disable_interface() 禁用当前接口的所有端点，
 *          清除所有待处理的 URB。这对 xHCI 主机控制器尤其重要，
 *          因为需要在释放带宽前刷新端点。
 *
 *   (3) 带宽分配
 *       -- 在 hcd->bandwidth_mutex 保护下:
 *         a) 调用 usb_disable_lpm() 禁用 LPM（链路电源管理）
 *         b) 清除旧备用设置的 streams 计数
 *         c) 调用 usb_hcd_alloc_bandwidth() 移除旧端点并添加新端点
 *            如果带宽不足，此调用会失败
 *         d) 发送 USB_REQ_SET_INTERFACE 控制请求给设备
 *
 *   (4) 特殊情况: 设备不支持 SET_INTERFACE
 *       -- 如果接口只有一个备用设置，USB 规范允许设备 STALL
 *          SET_INTERFACE 请求。此时函数进入"manual"模式:
 *          不切换备用设置，但手动清除新设置中所有端点的 HALT 状态。
 *
 *   (5) 状态切换
 *       -- 更新 iface->cur_altsetting 指向新设置
 *       -- 调用 usb_enable_interface() 启用新端点到 HCD
 *       -- 创建新的 sysfs 文件和端点设备节点
 *
 === 内部操作详见 ===
 * 函数大量依赖 usb_hcd_alloc_bandwidth()，它由 HCD（主机控制器驱动）
 * 实现。对于 xHCI，该函数负责:
 *   - 配置端点上下文（Endpoint Context）
 *   - 分配或释放传输环（Transfer Ring）
 *   - 计算和预留 U1/U2 超时值
 */
/**
 * usb_set_interface - Makes a particular alternate setting be current
 * @dev: the device whose interface is being updated
 * @interface: the interface being updated
 * @alternate: the setting being chosen.
 *
 * Context: task context, might sleep.
 *
 * This is used to enable data transfers on interfaces that may not
 * be enabled by default.  Not all devices support such configurability.
 * Only the driver bound to an interface may change its setting.
 *
 * Within any given configuration, each interface may have several
 * alternative settings.  These are often used to control levels of
 * bandwidth consumption.  For example, the default setting for a high
 * speed interrupt endpoint may not send more than 64 bytes per microframe,
 * while interrupt transfers of up to 3KBytes per microframe are legal.
 * Also, isochronous endpoints may never be part of an
 * interface's default setting.  To access such bandwidth, alternate
 * interface settings must be made current.
 *
 * Note that in the Linux USB subsystem, bandwidth associated with
 * an endpoint in a given alternate setting is not reserved until an URB
 * is submitted that needs that bandwidth.  Some other operating systems
 * allocate bandwidth early, when a configuration is chosen.
 *
 * xHCI reserves bandwidth and configures the alternate setting in
 * usb_hcd_alloc_bandwidth(). If it fails the original interface altsetting
 * may be disabled. Drivers cannot rely on any particular alternate
 * setting being in effect after a failure.
 *
 * This call is synchronous, and may not be used in an interrupt context.
 * Also, drivers must not change altsettings while urbs are scheduled for
 * endpoints in that interface; all such urbs must first be completed
 * (perhaps forced by unlinking). If a thread in your driver uses this call,
 * make sure your disconnect() method can wait for it to complete.
 *
 * Return: Zero on success, or else the status code returned by the
 * underlying usb_control_msg() call.
 */
int usb_set_interface(struct usb_device *dev, int interface, int alternate)
{
	struct usb_interface *iface;
	struct usb_host_interface *alt;
	struct usb_hcd *hcd = bus_to_hcd(dev->bus);
	int i, ret, manual = 0;
	unsigned int epaddr;
	unsigned int pipe;

	if (dev->state == USB_STATE_SUSPENDED)
		return -EHOSTUNREACH;

	iface = usb_ifnum_to_if(dev, interface);
	if (!iface) {
		dev_dbg(&dev->dev, "selecting invalid interface %d\n",
			interface);
		return -EINVAL;
	}
	if (iface->unregistering)
		return -ENODEV;

	alt = usb_altnum_to_altsetting(iface, alternate);
	if (!alt) {
		dev_warn(&dev->dev, "selecting invalid altsetting %d\n",
			 alternate);
		return -EINVAL;
	}
	/*
	 * usb3 hosts configure the interface in usb_hcd_alloc_bandwidth,
	 * including freeing dropped endpoint ring buffers.
	 * Make sure the interface endpoints are flushed before that
	 */
	usb_disable_interface(dev, iface, false);

	/* Make sure we have enough bandwidth for this alternate interface.
	 * Remove the current alt setting and add the new alt setting.
	 */
	mutex_lock(hcd->bandwidth_mutex);
	/* Disable LPM, and re-enable it once the new alt setting is installed,
	 * so that the xHCI driver can recalculate the U1/U2 timeouts.
	 */
	if (usb_disable_lpm(dev)) {
		dev_err(&iface->dev, "%s Failed to disable LPM\n", __func__);
		mutex_unlock(hcd->bandwidth_mutex);
		return -ENOMEM;
	}
	/* Changing alt-setting also frees any allocated streams */
	for (i = 0; i < iface->cur_altsetting->desc.bNumEndpoints; i++)
		iface->cur_altsetting->endpoint[i].streams = 0;

	ret = usb_hcd_alloc_bandwidth(dev, NULL, iface->cur_altsetting, alt);
	if (ret < 0) {
		dev_info(&dev->dev, "Not enough bandwidth for altsetting %d\n",
				alternate);
		usb_enable_lpm(dev);
		mutex_unlock(hcd->bandwidth_mutex);
		return ret;
	}

	if (dev->quirks & USB_QUIRK_NO_SET_INTF)
		ret = -EPIPE;
	else
		ret = usb_control_msg_send(dev, 0,
					   USB_REQ_SET_INTERFACE,
					   USB_RECIP_INTERFACE, alternate,
					   interface, NULL, 0, 5000,
					   GFP_NOIO);

	/* 9.4.10 says devices don't need this and are free to STALL the
	 * request if the interface only has one alternate setting.
	 */
	if (ret == -EPIPE && iface->num_altsetting == 1) {
		dev_dbg(&dev->dev,
			"manual set_interface for iface %d, alt %d\n",
			interface, alternate);
		manual = 1;
	} else if (ret) {
		/* Re-instate the old alt setting */
		usb_hcd_alloc_bandwidth(dev, NULL, alt, iface->cur_altsetting);
		usb_enable_lpm(dev);
		mutex_unlock(hcd->bandwidth_mutex);
		return ret;
	}
	mutex_unlock(hcd->bandwidth_mutex);

	/* FIXME drivers shouldn't need to replicate/bugfix the logic here
	 * when they implement async or easily-killable versions of this or
	 * other "should-be-internal" functions (like clear_halt).
	 * should hcd+usbcore postprocess control requests?
	 */

	/* prevent submissions using previous endpoint settings */
	if (iface->cur_altsetting != alt) {
		remove_intf_ep_devs(iface);
		usb_remove_sysfs_intf_files(iface);
	}
	usb_disable_interface(dev, iface, true);

	iface->cur_altsetting = alt;

	/* Now that the interface is installed, re-enable LPM. */
	usb_unlocked_enable_lpm(dev);

	/* If the interface only has one altsetting and the device didn't
	 * accept the request, we attempt to carry out the equivalent action
	 * by manually clearing the HALT feature for each endpoint in the
	 * new altsetting.
	 */
	if (manual) {
		for (i = 0; i < alt->desc.bNumEndpoints; i++) {
			epaddr = alt->endpoint[i].desc.bEndpointAddress;
			pipe = __create_pipe(dev,
					USB_ENDPOINT_NUMBER_MASK & epaddr) |
					(usb_endpoint_out(epaddr) ?
					USB_DIR_OUT : USB_DIR_IN);

			usb_clear_halt(dev, pipe);
		}
	}

	/* 9.1.1.5: reset toggles for all endpoints in the new altsetting
	 *
	 * Note:
	 * Despite EP0 is always present in all interfaces/AS, the list of
	 * endpoints from the descriptor does not contain EP0. Due to its
	 * omnipresence one might expect EP0 being considered "affected" by
	 * any SetInterface request and hence assume toggles need to be reset.
	 * However, EP0 toggles are re-synced for every individual transfer
	 * during the SETUP stage - hence EP0 toggles are "don't care" here.
	 * (Likewise, EP0 never "halts" on well designed devices.)
	 */
	usb_enable_interface(dev, iface, true);
	if (device_is_registered(&iface->dev)) {
		usb_create_sysfs_intf_files(iface);
		create_intf_ep_devs(iface);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(usb_set_interface);

/**
 * usb_reset_configuration - lightweight device reset
 * @dev: the device whose configuration is being reset
 *
 * This issues a standard SET_CONFIGURATION request to the device using
 * the current configuration.  The effect is to reset most USB-related
 * state in the device, including interface altsettings (reset to zero),
 * endpoint halts (cleared), and endpoint state (only for bulk and interrupt
 * endpoints).  Other usbcore state is unchanged, including bindings of
 * usb device drivers to interfaces.
 *
 * Because this affects multiple interfaces, avoid using this with composite
 * (multi-interface) devices.  Instead, the driver for each interface may
 * use usb_set_interface() on the interfaces it claims.  Be careful though;
 * some devices don't support the SET_INTERFACE request, and others won't
 * reset all the interface state (notably endpoint state).  Resetting the whole
 * configuration would affect other drivers' interfaces.
 *
 * The caller must own the device lock.
 *
 * Return: Zero on success, else a negative error code.
 *
 * If this routine fails the device will probably be in an unusable state
 * with endpoints disabled, and interfaces only partially enabled.
 */
int usb_reset_configuration(struct usb_device *dev)
{
	int			i, retval;
	struct usb_host_config	*config;
	struct usb_hcd *hcd = bus_to_hcd(dev->bus);

	if (dev->state == USB_STATE_SUSPENDED)
		return -EHOSTUNREACH;

	/* caller must have locked the device and must own
	 * the usb bus readlock (so driver bindings are stable);
	 * calls during probe() are fine
	 */

	usb_disable_device_endpoints(dev, 1); /* skip ep0*/

	config = dev->actconfig;
	retval = 0;
	mutex_lock(hcd->bandwidth_mutex);
	/* Disable LPM, and re-enable it once the configuration is reset, so
	 * that the xHCI driver can recalculate the U1/U2 timeouts.
	 */
	if (usb_disable_lpm(dev)) {
		dev_err(&dev->dev, "%s Failed to disable LPM\n", __func__);
		mutex_unlock(hcd->bandwidth_mutex);
		return -ENOMEM;
	}

	/* xHCI adds all endpoints in usb_hcd_alloc_bandwidth */
	retval = usb_hcd_alloc_bandwidth(dev, config, NULL, NULL);
	if (retval < 0) {
		usb_enable_lpm(dev);
		mutex_unlock(hcd->bandwidth_mutex);
		return retval;
	}
	retval = usb_control_msg_send(dev, 0, USB_REQ_SET_CONFIGURATION, 0,
				      config->desc.bConfigurationValue, 0,
				      NULL, 0, USB_CTRL_SET_TIMEOUT,
				      GFP_NOIO);
	if (retval) {
		usb_hcd_alloc_bandwidth(dev, NULL, NULL, NULL);
		usb_enable_lpm(dev);
		mutex_unlock(hcd->bandwidth_mutex);
		return retval;
	}
	mutex_unlock(hcd->bandwidth_mutex);

	/* re-init hc/hcd interface/endpoint state */
	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		struct usb_interface *intf = config->interface[i];
		struct usb_host_interface *alt;

		alt = usb_altnum_to_altsetting(intf, 0);

		/* No altsetting 0?  We'll assume the first altsetting.
		 * We could use a GetInterface call, but if a device is
		 * so non-compliant that it doesn't have altsetting 0
		 * then I wouldn't trust its reply anyway.
		 */
		if (!alt)
			alt = &intf->altsetting[0];

		if (alt != intf->cur_altsetting) {
			remove_intf_ep_devs(intf);
			usb_remove_sysfs_intf_files(intf);
		}
		intf->cur_altsetting = alt;
		usb_enable_interface(dev, intf, true);
		if (device_is_registered(&intf->dev)) {
			usb_create_sysfs_intf_files(intf);
			create_intf_ep_devs(intf);
		}
	}
	/* Now that the interfaces are installed, re-enable LPM. */
	usb_unlocked_enable_lpm(dev);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_reset_configuration);

static void usb_release_interface(struct device *dev)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_interface_cache *intfc =
			altsetting_to_usb_interface_cache(intf->altsetting);

	kref_put(&intfc->ref, usb_release_interface_cache);
	usb_put_dev(interface_to_usbdev(intf));
	of_node_put(dev->of_node);
	kfree(intf);
}

/*
 * usb_deauthorize_interface - deauthorize an USB interface
 *
 * @intf: USB interface structure
 */
void usb_deauthorize_interface(struct usb_interface *intf)
{
	struct device *dev = &intf->dev;

	device_lock(dev->parent);

	if (intf->authorized) {
		device_lock(dev);
		intf->authorized = 0;
		device_unlock(dev);

		usb_forced_unbind_intf(intf);
	}

	device_unlock(dev->parent);
}

/*
 * usb_authorize_interface - authorize an USB interface
 *
 * @intf: USB interface structure
 */
void usb_authorize_interface(struct usb_interface *intf)
{
	struct device *dev = &intf->dev;

	if (!intf->authorized) {
		device_lock(dev);
		intf->authorized = 1; /* authorize interface */
		device_unlock(dev);
	}
}

static int usb_if_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct usb_device *usb_dev;
	const struct usb_interface *intf;
	const struct usb_host_interface *alt;

	intf = to_usb_interface(dev);
	usb_dev = interface_to_usbdev(intf);
	alt = intf->cur_altsetting;

	if (add_uevent_var(env, "INTERFACE=%d/%d/%d",
		   alt->desc.bInterfaceClass,
		   alt->desc.bInterfaceSubClass,
		   alt->desc.bInterfaceProtocol))
		return -ENOMEM;

	if (add_uevent_var(env,
		   "MODALIAS=usb:"
		   "v%04Xp%04Xd%04Xdc%02Xdsc%02Xdp%02Xic%02Xisc%02Xip%02Xin%02X",
		   le16_to_cpu(usb_dev->descriptor.idVendor),
		   le16_to_cpu(usb_dev->descriptor.idProduct),
		   le16_to_cpu(usb_dev->descriptor.bcdDevice),
		   usb_dev->descriptor.bDeviceClass,
		   usb_dev->descriptor.bDeviceSubClass,
		   usb_dev->descriptor.bDeviceProtocol,
		   alt->desc.bInterfaceClass,
		   alt->desc.bInterfaceSubClass,
		   alt->desc.bInterfaceProtocol,
		   alt->desc.bInterfaceNumber))
		return -ENOMEM;

	return 0;
}

const struct device_type usb_if_device_type = {
	.name =		"usb_interface",
	.release =	usb_release_interface,
	.uevent =	usb_if_uevent,
};

static struct usb_interface_assoc_descriptor *find_iad(struct usb_device *dev,
						struct usb_host_config *config,
						u8 inum)
{
	struct usb_interface_assoc_descriptor *retval = NULL;
	struct usb_interface_assoc_descriptor *intf_assoc;
	int first_intf;
	int last_intf;
	int i;

	for (i = 0; (i < USB_MAXIADS && config->intf_assoc[i]); i++) {
		intf_assoc = config->intf_assoc[i];
		if (intf_assoc->bInterfaceCount == 0)
			continue;

		first_intf = intf_assoc->bFirstInterface;
		last_intf = first_intf + (intf_assoc->bInterfaceCount - 1);
		if (inum >= first_intf && inum <= last_intf) {
			if (!retval)
				retval = intf_assoc;
			else
				dev_err(&dev->dev, "Interface #%d referenced"
					" by multiple IADs\n", inum);
		}
	}

	return retval;
}


/*
 * Internal function to queue a device reset
 * See usb_queue_reset_device() for more details
 */
static void __usb_queue_reset_device(struct work_struct *ws)
{
	int rc;
	struct usb_interface *iface =
		container_of(ws, struct usb_interface, reset_ws);
	struct usb_device *udev = interface_to_usbdev(iface);

	rc = usb_lock_device_for_reset(udev, iface);
	if (rc >= 0) {
		usb_reset_device(udev);
		usb_unlock_device(udev);
	}
	usb_put_intf(iface);	/* Undo _get_ in usb_queue_reset_device() */
}

/*
 * Internal function to set the wireless_status sysfs attribute
 * See usb_set_wireless_status() for more details
 */
static void __usb_wireless_status_intf(struct work_struct *ws)
{
	struct usb_interface *iface =
		container_of(ws, struct usb_interface, wireless_status_work);

	device_lock(iface->dev.parent);
	if (iface->sysfs_files_created)
		usb_update_wireless_status_attr(iface);
	device_unlock(iface->dev.parent);
	usb_put_intf(iface);	/* Undo _get_ in usb_set_wireless_status() */
}

/**
 * usb_set_wireless_status - sets the wireless_status struct member
 * @iface: the interface to modify
 * @status: the new wireless status
 *
 * Set the wireless_status struct member to the new value, and emit
 * sysfs changes as necessary.
 *
 * Returns: 0 on success, -EALREADY if already set.
 */
int usb_set_wireless_status(struct usb_interface *iface,
		enum usb_wireless_status status)
{
	if (iface->wireless_status == status)
		return -EALREADY;

	usb_get_intf(iface);
	iface->wireless_status = status;
	schedule_work(&iface->wireless_status_work);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_set_wireless_status);

/*
 * 中文注释: usb_set_configuration() -- 选择设备配置（SET_CONFIGURATION）
 *
 * 这是 USB 子系统中最关键、最复杂的操作之一。改变配置意味着销毁所有
 * 现有接口、创建新接口、重新绑定驱动程序——相当于"热插拔"多个虚拟设备。
 *
 === 配置（Configuration）vs 接口备用设置（Interface Alternate Setting）===
 * USB 设备有两个层次的配置:
 *   配置层:   usb_set_configuration() -- 切换整个设备的功能集
 *             例如: 一个复合设备可能有配置 1（CDC 以太网）和配置 2（USB 串口）
 *   接口层:   usb_set_interface()     -- 切换同一接口的带宽模式
 *             例如: UVC 摄像头在空闲和高带宽模式之间切换
 *
 === 执行流程 ===
 *   (1) 参数处理
 *       -- 若 dev->authorized == 0 或 configuration == -1，强制设为 0（未配置）
 *       -- 遍历 dev->config[] 数组查找匹配的配置描述符
 *
 *   (2) 为新接口预分配内存
 *       -- 提前分配所有 struct usb_interface 和接口数组，
 *          这样一旦后续步骤失败，旧状态不受影响
 *
 *   (3) 电源唤醒
 *       -- 调用 usb_autoresume_device() 唤醒设备（如果设备已挂起）
 *
 *   (4) 销毁旧配置
 *       -- 调用 usb_disable_device(dev, 1) 禁用所有非 ep0 端点
 *       -- 这会卸载所有已绑定的接口驱动（unbind drivers）
 *       -- 取消所有待处理的异步 Set-Config 请求
 *
 *   (5) 带宽重新分配
 *       -- 在 hcd->bandwidth_mutex 保护下
 *       -- 调用 usb_hcd_alloc_bandwidth(dev, cp, NULL, NULL)
 *         * cp 为新配置: 分配带宽并添加端点
 *         * cp 为 NULL: 释放所有带宽（设置配置 0）
 *
 *   (6) 初始化新接口
 *       -- 遍历所有接口，设置:
 *         * cur_altsetting = 备用设置 0（或第一个可用设置）
 *         * intf_assoc = 对应的 IAD（接口关联描述符）
 *         * dev 设备模型父节点、ACPI/OF 节点等
 *       -- 注意: 此时接口尚未注册到设备模型，因此不会触发 probe()
 *
 *   (7) 发送 SET_CONFIGURATION 请求
 *       -- 若失败，释放所有已分配的接口资源，设备可能处于无法使用的状态
 *
 *   (8) 注册接口
 *       -- 调用 device_add() 将每个接口注册到 Linux 设备模型
 *       -- 这会触发 USB 总线驱动匹配，调用对应驱动的 probe() 方法
 *       -- probe() 可能进一步调用 usb_set_interface() 选择备用设置
 *
 === 重要注意事项 ===
 * - 调用者必须持有设备锁（device lock），且不能持有 USB 总线互斥锁
 *   （否则会导致死锁，因为 probe() 需要获取总线锁）。
 * - 接口驱动的 probe() 方法不能直接调用此函数。
 * - 改变配置后，所有旧接口的处理程序（drivers）都已被卸载，
 *   新接口的处理程序在 device_add() 时绑定。
 * - 非授权设备（authorized == 0）只能被置于未配置状态。
 */
/*
 * usb_set_configuration - Makes a particular device setting be current
 * @dev: the device whose configuration is being updated
 * @configuration: the configuration being chosen.
 *
 * Context: task context, might sleep. Caller holds device lock.
 *
 * This is used to enable non-default device modes.  Not all devices
 * use this kind of configurability; many devices only have one
 * configuration.
 *
 * @configuration is the value of the configuration to be installed.
 * According to the USB spec (e.g. section 9.1.1.5), configuration values
 * must be non-zero; a value of zero indicates that the device in
 * unconfigured.  However some devices erroneously use 0 as one of their
 * configuration values.  To help manage such devices, this routine will
 * accept @configuration = -1 as indicating the device should be put in
 * an unconfigured state.
 *
 * USB device configurations may affect Linux interoperability,
 * power consumption and the functionality available.  For example,
 * the default configuration is limited to using 100mA of bus power,
 * so that when certain device functionality requires more power,
 * and the device is bus powered, that functionality should be in some
 * non-default device configuration.  Other device modes may also be
 * reflected as configuration options, such as whether two ISDN
 * channels are available independently; and choosing between open
 * standard device protocols (like CDC) or proprietary ones.
 *
 * Note that a non-authorized device (dev->authorized == 0) will only
 * be put in unconfigured mode.
 *
 * Note that USB has an additional level of device configurability,
 * associated with interfaces.  That configurability is accessed using
 * usb_set_interface().
 *
 * This call is synchronous. The calling context must be able to sleep,
 * must own the device lock, and must not hold the driver model's USB
 * bus mutex; usb interface driver probe() methods cannot use this routine.
 *
 * Returns zero on success, or else the status code returned by the
 * underlying call that failed.  On successful completion, each interface
 * in the original device configuration has been destroyed, and each one
 * in the new configuration has been probed by all relevant usb device
 * drivers currently known to the kernel.
 */
int usb_set_configuration(struct usb_device *dev, int configuration)
{
	int i, ret;
	struct usb_host_config *cp = NULL;
	struct usb_interface **new_interfaces = NULL;
	struct usb_hcd *hcd = bus_to_hcd(dev->bus);
	int n, nintf;

	if (dev->authorized == 0 || configuration == -1)
		configuration = 0;
	else {
		for (i = 0; i < dev->descriptor.bNumConfigurations; i++) {
			if (dev->config[i].desc.bConfigurationValue ==
					configuration) {
				cp = &dev->config[i];
				break;
			}
		}
	}
	if ((!cp && configuration != 0))
		return -EINVAL;

	/* The USB spec says configuration 0 means unconfigured.
	 * But if a device includes a configuration numbered 0,
	 * we will accept it as a correctly configured state.
	 * Use -1 if you really want to unconfigure the device.
	 */
	if (cp && configuration == 0)
		dev_warn(&dev->dev, "config 0 descriptor??\n");

	/* Allocate memory for new interfaces before doing anything else,
	 * so that if we run out then nothing will have changed. */
	n = nintf = 0;
	if (cp) {
		nintf = cp->desc.bNumInterfaces;
		new_interfaces = kmalloc_objs(*new_interfaces, nintf, GFP_NOIO);
		if (!new_interfaces)
			return -ENOMEM;

		for (; n < nintf; ++n) {
			new_interfaces[n] = kzalloc_obj(struct usb_interface,
							GFP_NOIO);
			if (!new_interfaces[n]) {
				ret = -ENOMEM;
free_interfaces:
				while (--n >= 0)
					kfree(new_interfaces[n]);
				kfree(new_interfaces);
				return ret;
			}
		}

		i = dev->bus_mA - usb_get_max_power(dev, cp);
		if (i < 0)
			dev_warn(&dev->dev, "new config #%d exceeds power "
					"limit by %dmA\n",
					configuration, -i);
	}

	/* Wake up the device so we can send it the Set-Config request */
	ret = usb_autoresume_device(dev);
	if (ret)
		goto free_interfaces;

	/* if it's already configured, clear out old state first.
	 * getting rid of old interfaces means unbinding their drivers.
	 */
	if (dev->state != USB_STATE_ADDRESS)
		usb_disable_device(dev, 1);	/* Skip ep0 */

	/* Get rid of pending async Set-Config requests for this device */
	cancel_async_set_config(dev);

	/* Make sure we have bandwidth (and available HCD resources) for this
	 * configuration.  Remove endpoints from the schedule if we're dropping
	 * this configuration to set configuration 0.  After this point, the
	 * host controller will not allow submissions to dropped endpoints.  If
	 * this call fails, the device state is unchanged.
	 */
	mutex_lock(hcd->bandwidth_mutex);
	/* Disable LPM, and re-enable it once the new configuration is
	 * installed, so that the xHCI driver can recalculate the U1/U2
	 * timeouts.
	 */
	if (dev->actconfig && usb_disable_lpm(dev)) {
		dev_err(&dev->dev, "%s Failed to disable LPM\n", __func__);
		mutex_unlock(hcd->bandwidth_mutex);
		ret = -ENOMEM;
		goto free_interfaces;
	}
	ret = usb_hcd_alloc_bandwidth(dev, cp, NULL, NULL);
	if (ret < 0) {
		if (dev->actconfig)
			usb_enable_lpm(dev);
		mutex_unlock(hcd->bandwidth_mutex);
		usb_autosuspend_device(dev);
		goto free_interfaces;
	}

	/*
	 * Initialize the new interface structures and the
	 * hc/hcd/usbcore interface/endpoint state.
	 */
	for (i = 0; i < nintf; ++i) {
		struct usb_interface_cache *intfc;
		struct usb_interface *intf;
		struct usb_host_interface *alt;
		u8 ifnum;

		cp->interface[i] = intf = new_interfaces[i];
		intfc = cp->intf_cache[i];
		intf->altsetting = intfc->altsetting;
		intf->num_altsetting = intfc->num_altsetting;
		intf->authorized = !!HCD_INTF_AUTHORIZED(hcd);
		kref_get(&intfc->ref);

		alt = usb_altnum_to_altsetting(intf, 0);

		/* No altsetting 0?  We'll assume the first altsetting.
		 * We could use a GetInterface call, but if a device is
		 * so non-compliant that it doesn't have altsetting 0
		 * then I wouldn't trust its reply anyway.
		 */
		if (!alt)
			alt = &intf->altsetting[0];

		ifnum = alt->desc.bInterfaceNumber;
		intf->intf_assoc = find_iad(dev, cp, ifnum);
		intf->cur_altsetting = alt;
		usb_enable_interface(dev, intf, true);
		intf->dev.parent = &dev->dev;
		if (usb_of_has_combined_node(dev)) {
			device_set_of_node_from_dev(&intf->dev, &dev->dev);
		} else {
			intf->dev.of_node = usb_of_get_interface_node(dev,
					configuration, ifnum);
		}
		ACPI_COMPANION_SET(&intf->dev, ACPI_COMPANION(&dev->dev));
		intf->dev.driver = NULL;
		intf->dev.bus = &usb_bus_type;
		intf->dev.type = &usb_if_device_type;
		intf->dev.groups = usb_interface_groups;
		INIT_WORK(&intf->reset_ws, __usb_queue_reset_device);
		INIT_WORK(&intf->wireless_status_work, __usb_wireless_status_intf);
		intf->minor = -1;
		device_initialize(&intf->dev);
		pm_runtime_no_callbacks(&intf->dev);
		dev_set_name(&intf->dev, "%d-%s:%d.%d", dev->bus->busnum,
				dev->devpath, configuration, ifnum);
		usb_get_dev(dev);
	}
	kfree(new_interfaces);

	ret = usb_control_msg_send(dev, 0, USB_REQ_SET_CONFIGURATION, 0,
				   configuration, 0, NULL, 0,
				   USB_CTRL_SET_TIMEOUT, GFP_NOIO);
	if (ret && cp) {
		/*
		 * All the old state is gone, so what else can we do?
		 * The device is probably useless now anyway.
		 */
		usb_hcd_alloc_bandwidth(dev, NULL, NULL, NULL);
		for (i = 0; i < nintf; ++i) {
			usb_disable_interface(dev, cp->interface[i], true);
			put_device(&cp->interface[i]->dev);
			cp->interface[i] = NULL;
		}
		cp = NULL;
	}

	dev->actconfig = cp;
	mutex_unlock(hcd->bandwidth_mutex);

	if (!cp) {
		usb_set_device_state(dev, USB_STATE_ADDRESS);

		/* Leave LPM disabled while the device is unconfigured. */
		usb_autosuspend_device(dev);
		return ret;
	}
	usb_set_device_state(dev, USB_STATE_CONFIGURED);

	if (cp->string == NULL &&
			!(dev->quirks & USB_QUIRK_CONFIG_INTF_STRINGS))
		cp->string = usb_cache_string(dev, cp->desc.iConfiguration);

	/* Now that the interfaces are installed, re-enable LPM. */
	usb_unlocked_enable_lpm(dev);
	/* Enable LTM if it was turned off by usb_disable_device. */
	usb_enable_ltm(dev);

	/* Now that all the interfaces are set up, register them
	 * to trigger binding of drivers to interfaces.  probe()
	 * routines may install different altsettings and may
	 * claim() any interfaces not yet bound.  Many class drivers
	 * need that: CDC, audio, video, etc.
	 */
	for (i = 0; i < nintf; ++i) {
		struct usb_interface *intf = cp->interface[i];

		if (intf->dev.of_node &&
		    !of_device_is_available(intf->dev.of_node)) {
			dev_info(&dev->dev, "skipping disabled interface %d\n",
				 intf->cur_altsetting->desc.bInterfaceNumber);
			continue;
		}

		dev_dbg(&dev->dev,
			"adding %s (config #%d, interface %d)\n",
			dev_name(&intf->dev), configuration,
			intf->cur_altsetting->desc.bInterfaceNumber);
		device_enable_async_suspend(&intf->dev);
		ret = device_add(&intf->dev);
		if (ret != 0) {
			dev_err(&dev->dev, "device_add(%s) --> %d\n",
				dev_name(&intf->dev), ret);
			continue;
		}
		create_intf_ep_devs(intf);
	}

	usb_autosuspend_device(dev);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_set_configuration);

static LIST_HEAD(set_config_list);
static DEFINE_SPINLOCK(set_config_lock);

struct set_config_request {
	struct usb_device	*udev;
	int			config;
	struct work_struct	work;
	struct list_head	node;
};

/* Worker routine for usb_driver_set_configuration() */
static void driver_set_config_work(struct work_struct *work)
{
	struct set_config_request *req =
		container_of(work, struct set_config_request, work);
	struct usb_device *udev = req->udev;

	usb_lock_device(udev);
	spin_lock(&set_config_lock);
	list_del(&req->node);
	spin_unlock(&set_config_lock);

	if (req->config >= -1)		/* Is req still valid? */
		usb_set_configuration(udev, req->config);
	usb_unlock_device(udev);
	usb_put_dev(udev);
	kfree(req);
}

/* Cancel pending Set-Config requests for a device whose configuration
 * was just changed
 */
static void cancel_async_set_config(struct usb_device *udev)
{
	struct set_config_request *req;

	spin_lock(&set_config_lock);
	list_for_each_entry(req, &set_config_list, node) {
		if (req->udev == udev)
			req->config = -999;	/* Mark as cancelled */
	}
	spin_unlock(&set_config_lock);
}

/**
 * usb_driver_set_configuration - Provide a way for drivers to change device configurations
 * @udev: the device whose configuration is being updated
 * @config: the configuration being chosen.
 * Context: In process context, must be able to sleep
 *
 * Device interface drivers are not allowed to change device configurations.
 * This is because changing configurations will destroy the interface the
 * driver is bound to and create new ones; it would be like a floppy-disk
 * driver telling the computer to replace the floppy-disk drive with a
 * tape drive!
 *
 * Still, in certain specialized circumstances the need may arise.  This
 * routine gets around the normal restrictions by using a work thread to
 * submit the change-config request.
 *
 * Return: 0 if the request was successfully queued, error code otherwise.
 * The caller has no way to know whether the queued request will eventually
 * succeed.
 */
int usb_driver_set_configuration(struct usb_device *udev, int config)
{
	struct set_config_request *req;

	req = kmalloc_obj(*req);
	if (!req)
		return -ENOMEM;
	req->udev = udev;
	req->config = config;
	INIT_WORK(&req->work, driver_set_config_work);

	spin_lock(&set_config_lock);
	list_add(&req->node, &set_config_list);
	spin_unlock(&set_config_lock);

	usb_get_dev(udev);
	schedule_work(&req->work);
	return 0;
}
EXPORT_SYMBOL_GPL(usb_driver_set_configuration);

/**
 * cdc_parse_cdc_header - parse the extra headers present in CDC devices
 * @hdr: the place to put the results of the parsing
 * @intf: the interface for which parsing is requested
 * @buffer: pointer to the extra headers to be parsed
 * @buflen: length of the extra headers
 *
 * This evaluates the extra headers present in CDC devices which
 * bind the interfaces for data and control and provide details
 * about the capabilities of the device.
 *
 * Return: number of descriptors parsed or -EINVAL
 * if the header is contradictory beyond salvage
 */

int cdc_parse_cdc_header(struct usb_cdc_parsed_header *hdr,
				struct usb_interface *intf,
				u8 *buffer,
				int buflen)
{
	/* duplicates are ignored */
	struct usb_cdc_union_desc *union_header = NULL;

	/* duplicates are not tolerated */
	struct usb_cdc_header_desc *header = NULL;
	struct usb_cdc_ether_desc *ether = NULL;
	struct usb_cdc_mdlm_detail_desc *detail = NULL;
	struct usb_cdc_mdlm_desc *desc = NULL;

	unsigned int elength;
	int cnt = 0;

	memset(hdr, 0x00, sizeof(struct usb_cdc_parsed_header));
	hdr->phonet_magic_present = false;
	while (buflen > 0) {
		elength = buffer[0];
		if (!elength) {
			dev_err(&intf->dev, "skipping garbage byte\n");
			elength = 1;
			goto next_desc;
		}
		if ((buflen < elength) || (elength < 3)) {
			dev_err(&intf->dev, "invalid descriptor buffer length\n");
			break;
		}
		if (buffer[1] != USB_DT_CS_INTERFACE) {
			dev_err(&intf->dev, "skipping garbage\n");
			goto next_desc;
		}

		switch (buffer[2]) {
		case USB_CDC_UNION_TYPE: /* we've found it */
			if (elength < sizeof(struct usb_cdc_union_desc))
				goto next_desc;
			if (union_header) {
				dev_err(&intf->dev, "More than one union descriptor, skipping ...\n");
				goto next_desc;
			}
			union_header = (struct usb_cdc_union_desc *)buffer;
			break;
		case USB_CDC_COUNTRY_TYPE:
			if (elength < sizeof(struct usb_cdc_country_functional_desc))
				goto next_desc;
			hdr->usb_cdc_country_functional_desc =
				(struct usb_cdc_country_functional_desc *)buffer;
			break;
		case USB_CDC_HEADER_TYPE:
			if (elength != sizeof(struct usb_cdc_header_desc))
				goto next_desc;
			if (header)
				return -EINVAL;
			header = (struct usb_cdc_header_desc *)buffer;
			break;
		case USB_CDC_ACM_TYPE:
			if (elength < sizeof(struct usb_cdc_acm_descriptor))
				goto next_desc;
			hdr->usb_cdc_acm_descriptor =
				(struct usb_cdc_acm_descriptor *)buffer;
			break;
		case USB_CDC_ETHERNET_TYPE:
			if (elength != sizeof(struct usb_cdc_ether_desc))
				goto next_desc;
			if (ether)
				return -EINVAL;
			ether = (struct usb_cdc_ether_desc *)buffer;
			break;
		case USB_CDC_CALL_MANAGEMENT_TYPE:
			if (elength < sizeof(struct usb_cdc_call_mgmt_descriptor))
				goto next_desc;
			hdr->usb_cdc_call_mgmt_descriptor =
				(struct usb_cdc_call_mgmt_descriptor *)buffer;
			break;
		case USB_CDC_DMM_TYPE:
			if (elength < sizeof(struct usb_cdc_dmm_desc))
				goto next_desc;
			hdr->usb_cdc_dmm_desc =
				(struct usb_cdc_dmm_desc *)buffer;
			break;
		case USB_CDC_MDLM_TYPE:
			if (elength < sizeof(struct usb_cdc_mdlm_desc))
				goto next_desc;
			if (desc)
				return -EINVAL;
			desc = (struct usb_cdc_mdlm_desc *)buffer;
			break;
		case USB_CDC_MDLM_DETAIL_TYPE:
			if (elength < sizeof(struct usb_cdc_mdlm_detail_desc))
				goto next_desc;
			if (detail)
				return -EINVAL;
			detail = (struct usb_cdc_mdlm_detail_desc *)buffer;
			break;
		case USB_CDC_NCM_TYPE:
			if (elength < sizeof(struct usb_cdc_ncm_desc))
				goto next_desc;
			hdr->usb_cdc_ncm_desc = (struct usb_cdc_ncm_desc *)buffer;
			break;
		case USB_CDC_MBIM_TYPE:
			if (elength < sizeof(struct usb_cdc_mbim_desc))
				goto next_desc;

			hdr->usb_cdc_mbim_desc = (struct usb_cdc_mbim_desc *)buffer;
			break;
		case USB_CDC_MBIM_EXTENDED_TYPE:
			if (elength < sizeof(struct usb_cdc_mbim_extended_desc))
				goto next_desc;
			hdr->usb_cdc_mbim_extended_desc =
				(struct usb_cdc_mbim_extended_desc *)buffer;
			break;
		case CDC_PHONET_MAGIC_NUMBER:
			hdr->phonet_magic_present = true;
			break;
		default:
			/*
			 * there are LOTS more CDC descriptors that
			 * could legitimately be found here.
			 */
			dev_dbg(&intf->dev, "Ignoring descriptor: type %02x, length %ud\n",
					buffer[2], elength);
			goto next_desc;
		}
		cnt++;
next_desc:
		buflen -= elength;
		buffer += elength;
	}
	hdr->usb_cdc_union_desc = union_header;
	hdr->usb_cdc_header_desc = header;
	hdr->usb_cdc_mdlm_detail_desc = detail;
	hdr->usb_cdc_mdlm_desc = desc;
	hdr->usb_cdc_ether_desc = ether;
	return cnt;
}

EXPORT_SYMBOL(cdc_parse_cdc_header);
