// SPDX-License-Identifier: GPL-2.0
/*
 * Released under the GPLv2 only.
 */

/*
 * urb.c -- URB (USB Request Block) 生命周期管理
 *
 * URB 是 USB 数据传输的基本单元，类似于网络协议栈中的 sk_buff。
 * 本文件实现了 URB 从创建到销毁的完整生命周期管理，包括：
 *
 *   [初始化]   usb_init_urb()  /  usb_alloc_urb()
 *   [提交]     usb_submit_urb()
 *   [取消]     usb_unlink_urb() / usb_kill_urb() / usb_poison_urb()
 *   [回收]     usb_free_urb()
 *
 * 关键函数一览:
 *   usb_init_urb()   -- 初始化栈上分配的 struct urb
 *   usb_alloc_urb()  -- 分配并初始化 URB（支持等时传输的 ISO 帧数组）
 *   usb_submit_urb() -- 将 URB 提交给主机控制器驱动（HCD）执行
 *   usb_free_urb()   -- 释放 URB，基于 kref 引用计数
 *   usb_kill_urb()   -- 同步取消 URB 并等待完成
 *   usb_unlink_urb() -- 异步取消 URB
 *   usb_poison_urb() -- 杀死 URB 并阻止其重新提交
 *
 * 并发模型:
 *   URB 使用 kref 引用计数机制管理生命周期。usb_get_urb() 增加引用计数，
 *   usb_free_urb() 减少引用计数，当计数归零时触发 urb_destroy() 回收内存。
 *   usb_submit_urb() 通过检查 hcpriv 确保 URB 不会重复提交。
 *   usb_kill_urb() / usb_poison_urb() 通过 reject 标志阻止新的提交，
 *   并使用 wait_event 等待 use_count 归零。
 *
 * usb_submit_urb() 的校验流程:
 *   1. 参数检查: urb 和 urb->complete 非空
 *   2. 活跃性检查: urb->hcpriv 为空（未处于活跃状态）
 *   3. 设备状态检查: dev->state >= USB_STATE_UNAUTHENTICATED
 *   4. 端点解析: 从 pipe 获取端点描述符
 *   5. 状态初始化: status = -EINPROGRESS, actual_length = 0
 *   6. 传输类型校验: 根据 control/bulk/interrupt/isochronous 执行不同检查
 *   7. 传输大小检查: transfer_buffer_length 合法
 *   8. 传输标志检查: 校验 transfer_flags 合法性
 *   9. 周期传输间隔检查: interval 值合法且为 2 的幂
 *   10. 提交: 调用 usb_hcd_submit_urb() 委托给 HCD 层
 *
 * kill vs unlink vs poison 的区别:
 *   usb_unlink_urb() -- 异步取消，立即返回 -EINPROGRESS，
 *                       完成后 complete() 看到 status = -ECONNRESET
 *   usb_kill_urb()   -- 同步取消，等待 URB 完全停止，
 *                       完成后 status = -ENOENT，
 *                       不会阻止后续提交
 *   usb_poison_urb() -- 同步取消 + 永久拒绝，
 *                       设置 reject 标志阻止后续所有提交，
 *                       即使 complete() 中尝试 resubmit 也会失败
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/kmsan.h>
#include <linux/usb.h>
#include <linux/wait.h>
#include <linux/usb/hcd.h>
#include <linux/scatterlist.h>

#define to_urb(d) container_of(d, struct urb, kref)


/*
 * urb_destroy -- kref 回调，引用计数归零时释放 URB 内存
 *
 * 此函数通过 container_of 从 kref 获取对应的 URB 指针。
 * 当 kref_put() 检测到引用计数归零时自动调用此回调。
 *
 * 内存释放策略:
 *   - 如果设置了 URB_FREE_BUFFER 标志，说明 URB 拥有 transfer_buffer
 *     的所有权（通常由 usb_buffer_alloc() 或类似 API 分配），
 *     需要在此一并释放 transfer_buffer。
 *   - 最终通过 kfree(urb) 释放 URB 结构体本身。
 *
 * 注意: 如果 URB_FREE_BUFFER 未设置，transfer_buffer 由提交者
 * （USB 设备驱动程序）自行管理生命周期，此函数不会释放它。
 */
static void urb_destroy(struct kref *kref)
{
	struct urb *urb = to_urb(kref);

	if (urb->transfer_flags & URB_FREE_BUFFER)
		kfree(urb->transfer_buffer);

	kfree(urb);
}

/**
 * usb_init_urb - initializes a urb so that it can be used by a USB driver
 * @urb: pointer to the urb to initialize
 *
 * Initializes a urb so that the USB subsystem can use it properly.
 *
 * If a urb is created with a call to usb_alloc_urb() it is not
 * necessary to call this function.  Only use this if you allocate the
 * space for a struct urb on your own.  If you call this function, be
 * careful when freeing the memory for your urb that it is no longer in
 * use by the USB core.
 *
 * Only use this function if you _really_ understand what you are doing.
 */

/*
 * usb_init_urb -- 初始化 USB 请求块（适用于栈上分配的 URB）
 *
 * 此函数用于初始化调用者自行分配（例如栈上分配或嵌入到其他结构体）
 * 的 struct urb。如果通过 usb_alloc_urb() 动态分配，则无需调用此函数，
 * 因为 usb_alloc_urb() 内部已调用 usb_init_urb()。
 *
 * 初始化内容包括:
 *   - memset 将整个结构体清零
 *   - kref_init 初始化引用计数为 1
 *   - INIT_LIST_HEAD 初始化 urb_list（用于 HCD 内部队列管理）
 *   - INIT_LIST_HEAD 初始化 anchor_list（用于锚点批量管理）
 *
 * 使用场景:
 *   当驱动需要在栈上或嵌入到其他数据结构中使用 URB 时调用此函数。
 *   绝大多数驱动应使用 usb_alloc_urb() 而非此函数。
 */
void usb_init_urb(struct urb *urb)
{
	if (urb) {
		memset(urb, 0, sizeof(*urb));
		kref_init(&urb->kref);
		INIT_LIST_HEAD(&urb->urb_list);
		INIT_LIST_HEAD(&urb->anchor_list);
	}
}
EXPORT_SYMBOL_GPL(usb_init_urb);

/**
 * usb_alloc_urb - creates a new urb for a USB driver to use
 * @iso_packets: number of iso packets for this urb
 * @mem_flags: the type of memory to allocate, see kmalloc() for a list of
 *	valid options for this.
 *
 * Creates an urb for the USB driver to use, initializes a few internal
 * structures, increments the usage counter, and returns a pointer to it.
 *
 * If the driver want to use this urb for interrupt, control, or bulk
 * endpoints, pass '0' as the number of iso packets.
 *
 * The driver must call usb_free_urb() when it is finished with the urb.
 *
 * Return: A pointer to the new urb, or %NULL if no memory is available.
 */

/*
 * usb_alloc_urb -- 分配并初始化一个新的 URB
 *
 * @iso_packets: 等时传输（Isochronous）的帧数量。
 *               非等时传输（控制/批量/中断）传 0 即可。
 *
 * 分配细节:
 *   使用 kmalloc_flex() 分配 struct urb，末尾带柔性数组
 *   iso_frame_desc[iso_packets]。这个柔性数组用于等时传输中
 *   描述每一帧的偏移、长度和状态。
 *   iso_packets 为 0 时只分配 struct urb 本身，
 *   不占用 iso_frame_desc 数组的额外空间。
 *
 * 调用链:
 *   usb_alloc_urb() -> kmalloc_flex() -> usb_init_urb() -> kref_init()
 *
 * 调用者必须在使用完毕后调用 usb_free_urb() 释放。
 *
 * 返回: 新分配的 URB 指针，内存不足时返回 NULL。
 */
struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
	struct urb *urb;

	urb = kmalloc_flex(*urb, iso_frame_desc, iso_packets, mem_flags);
	if (!urb)
		return NULL;
	usb_init_urb(urb);
	return urb;
}
EXPORT_SYMBOL_GPL(usb_alloc_urb);

/**
 * usb_free_urb - frees the memory used by a urb when all users of it are finished
 * @urb: pointer to the urb to free, may be NULL
 *
 * Must be called when a user of a urb is finished with it.  When the last user
 * of the urb calls this function, the memory of the urb is freed.
 *
 * Note: The transfer buffer associated with the urb is not freed unless the
 * URB_FREE_BUFFER transfer flag is set.
 */

/*
 * usb_free_urb -- 释放 URB（基于 kref 引用计数）
 *
 * 这是 URB 的"析构"入口。每次调用都会减少 kref 引用计数。
 * 当引用计数从 1 降为 0 时，kref_put() 会自动调用 urb_destroy()
 * 执行真正的内存释放。
 *
 * 可以被 NULL 安全调用（urb == NULL 时直接返回）。
 *
 * 引用计数规则:
 *   usb_alloc_urb() 将计数置为 1
 *   usb_get_urb()   将计数 +1
 *   usb_free_urb()  将计数 -1
 *   提交 URB 时 HCD 会通过 usb_get_urb() 增加引用
 *   完成后 HCD 通过 usb_put_urb()（间接调用 usb_free_urb()）减少引用
 */
void usb_free_urb(struct urb *urb)
{
	if (urb)
		kref_put(&urb->kref, urb_destroy);
}
EXPORT_SYMBOL_GPL(usb_free_urb);

/**
 * usb_get_urb - increments the reference count of the urb
 * @urb: pointer to the urb to modify, may be NULL
 *
 * This must be  called whenever a urb is transferred from a device driver to a
 * host controller driver.  This allows proper reference counting to happen
 * for urbs.
 *
 * Return: A pointer to the urb with the incremented reference counter.
 */

/*
 * usb_get_urb -- 增加 URB 的引用计数（kref 模式）
 *
 * 任何需要延长 URB 生命周期的代码路径都应调用此函数。
 * 典型场景: HCD 在接收 URB 提交时调用，确保在处理期间 URB 不会被释放。
 *
 * 每调用一次 usb_get_urb()，必须对应一次 usb_free_urb()（或等价的
 * kref_put），否则 URB 内存将泄漏。
 *
 * 可以被 NULL 安全调用。
 */
struct urb *usb_get_urb(struct urb *urb)
{
	if (urb)
		kref_get(&urb->kref);
	return urb;
}
EXPORT_SYMBOL_GPL(usb_get_urb);

/**
 * usb_anchor_urb - anchors an URB while it is processed
 * @urb: pointer to the urb to anchor
 * @anchor: pointer to the anchor
 *
 * This can be called to have access to URBs which are to be executed
 * without bothering to track them
 */

/*
 * usb_anchor_urb -- 将 URB 挂接到锚点进行批量管理
 *
 * 锚点（anchor）提供了一种批量管理多个 URB 的机制。驱动可以将多个
 * URB 挂接到同一个锚点，然后通过 usb_kill_anchored_urbs() 或
 * usb_poison_anchored_urbs() 一次性取消所有 URB。
 *
 * 操作流程:
 *   1. 获取锚点的自旋锁
 *   2. 调用 usb_get_urb() 增加 URB 引用计数（锚点持有引用）
 *   3. 将 URB 的 anchor_list 节点加入锚点的 urb_list 链表
 *   4. 设置 URB 的 anchor 指针指向该锚点
 *   5. 如果锚点已被 poisoned，同步增加 URB 的 reject 计数
 *   6. 释放锁
 *
 * 锚点被 poison 后，新挂接的 URB 也会被自动标记 reject，
 * 从而阻止它们被提交。
 */
void usb_anchor_urb(struct urb *urb, struct usb_anchor *anchor)
{
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	usb_get_urb(urb);
	list_add_tail(&urb->anchor_list, &anchor->urb_list);
	urb->anchor = anchor;

	if (unlikely(anchor->poisoned))
		atomic_inc(&urb->reject);

	spin_unlock_irqrestore(&anchor->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_anchor_urb);

/*
 * usb_anchor_check_wakeup -- 检查是否可以唤醒等待锚点变空的线程
 *
 * 同时满足两个条件才返回 true:
 *   1. suspend_wakeups == 0（没有暂停唤醒通知）
 *   2. urb_list 为空（所有 URB 均已解锚）
 *
 * 防止在 complete() 回调执行过程中错误地触发唤醒。
 */
static int usb_anchor_check_wakeup(struct usb_anchor *anchor)
{
	return atomic_read(&anchor->suspend_wakeups) == 0 &&
		list_empty(&anchor->urb_list);
}

/*
 * __usb_unanchor_urb -- 内部解锚操作（调用者必须持有 anchor->lock）
 *
 * 执行实际的解锚工作:
 *   1. 清除 URB 的 anchor 指针
 *   2. 将 URB 从锚点的 urb_list 链表中移除
 *   3. 释放锚点持有的 URB 引用计数（usb_put_urb）
 *   4. 如果锚点已空且无挂起唤醒，唤醒等待者
 *
 * 此函数不获取锁，调用者必须确保 anchor->lock 已被持有。
 */
/* Callers must hold anchor->lock */
static void __usb_unanchor_urb(struct urb *urb, struct usb_anchor *anchor)
{
	urb->anchor = NULL;
	list_del(&urb->anchor_list);
	usb_put_urb(urb);
	if (usb_anchor_check_wakeup(anchor))
		wake_up(&anchor->wait);
}

/**
 * usb_unanchor_urb - unanchors an URB
 * @urb: pointer to the urb to anchor
 *
 * Call this to stop the system keeping track of this URB
 */

/*
 * usb_unanchor_urb -- 将 URB 从锚点分离
 *
 * 与 usb_anchor_urb() 相反的操作。将 URB 从锚点链表中移除，
 * 同时释放锚点持有的 URB 引用计数（usb_put_urb）。
 *
 * 并发安全:
 *   使用锚点的自旋锁保护。由于可能存在多个线程同时尝试解锚
 *   同一个 URB 的竞态条件，函数通过二次检查 anchor == urb->anchor
 *   确保只有一个线程执行解锚操作，避免 double-unanchor。
 *
 * 当锚点变为空且没有挂起的唤醒时，会唤醒等待在 anchor->wait
 * 上的线程（如 usb_wait_anchor_empty_timeout）。
 */
void usb_unanchor_urb(struct urb *urb)
{
	unsigned long flags;
	struct usb_anchor *anchor;

	if (!urb)
		return;

	anchor = urb->anchor;
	if (!anchor)
		return;

	spin_lock_irqsave(&anchor->lock, flags);
	/*
	 * At this point, we could be competing with another thread which
	 * has the same intention. To protect the urb from being unanchored
	 * twice, only the winner of the race gets the job.
	 */
	if (likely(anchor == urb->anchor))
		__usb_unanchor_urb(urb, anchor);
	spin_unlock_irqrestore(&anchor->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_unanchor_urb);

/*-------------------------------------------------------------------*/

static const int pipetypes[4] = {
	PIPE_CONTROL, PIPE_ISOCHRONOUS, PIPE_BULK, PIPE_INTERRUPT
};

/**
 * usb_pipe_type_check - sanity check of a specific pipe for a usb device
 * @dev: struct usb_device to be checked
 * @pipe: pipe to check
 *
 * This performs a light-weight sanity check for the endpoint in the
 * given usb device.  It returns 0 if the pipe is valid for the specific usb
 * device, otherwise a negative error code.
 */
int usb_pipe_type_check(struct usb_device *dev, unsigned int pipe)
{
	const struct usb_host_endpoint *ep;

	ep = usb_pipe_endpoint(dev, pipe);
	if (!ep)
		return -EINVAL;
	if (usb_pipetype(pipe) != pipetypes[usb_endpoint_type(&ep->desc)])
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL_GPL(usb_pipe_type_check);

/**
 * usb_urb_ep_type_check - sanity check of endpoint in the given urb
 * @urb: urb to be checked
 *
 * This performs a light-weight sanity check for the endpoint in the
 * given urb.  It returns 0 if the urb contains a valid endpoint, otherwise
 * a negative error code.
 */
int usb_urb_ep_type_check(const struct urb *urb)
{
	return usb_pipe_type_check(urb->dev, urb->pipe);
}
EXPORT_SYMBOL_GPL(usb_urb_ep_type_check);

/**
 * usb_submit_urb - issue an asynchronous transfer request for an endpoint
 * @urb: pointer to the urb describing the request
 * @mem_flags: the type of memory to allocate, see kmalloc() for a list
 *	of valid options for this.
 *
 * This submits a transfer request, and transfers control of the URB
 * describing that request to the USB subsystem.  Request completion will
 * be indicated later, asynchronously, by calling the completion handler.
 * The three types of completion are success, error, and unlink
 * (a software-induced fault, also called "request cancellation").
 *
 * URBs may be submitted in interrupt context.
 *
 * The caller must have correctly initialized the URB before submitting
 * it.  Functions such as usb_fill_bulk_urb() and usb_fill_control_urb() are
 * available to ensure that most fields are correctly initialized, for
 * the particular kind of transfer, although they will not initialize
 * any transfer flags.
 *
 * If the submission is successful, the complete() callback from the URB
 * will be called exactly once, when the USB core and Host Controller Driver
 * (HCD) are finished with the URB.  When the completion function is called,
 * control of the URB is returned to the device driver which issued the
 * request.  The completion handler may then immediately free or reuse that
 * URB.
 *
 * With few exceptions, USB device drivers should never access URB fields
 * provided by usbcore or the HCD until its complete() is called.
 * The exceptions relate to periodic transfer scheduling.  For both
 * interrupt and isochronous urbs, as part of successful URB submission
 * urb->interval is modified to reflect the actual transfer period used
 * (normally some power of two units).  And for isochronous urbs,
 * urb->start_frame is modified to reflect when the URB's transfers were
 * scheduled to start.
 *
 * Not all isochronous transfer scheduling policies will work, but most
 * host controller drivers should easily handle ISO queues going from now
 * until 10-200 msec into the future.  Drivers should try to keep at
 * least one or two msec of data in the queue; many controllers require
 * that new transfers start at least 1 msec in the future when they are
 * added.  If the driver is unable to keep up and the queue empties out,
 * the behavior for new submissions is governed by the URB_ISO_ASAP flag.
 * If the flag is set, or if the queue is idle, then the URB is always
 * assigned to the first available (and not yet expired) slot in the
 * endpoint's schedule.  If the flag is not set and the queue is active
 * then the URB is always assigned to the next slot in the schedule
 * following the end of the endpoint's previous URB, even if that slot is
 * in the past.  When a packet is assigned in this way to a slot that has
 * already expired, the packet is not transmitted and the corresponding
 * usb_iso_packet_descriptor's status field will return -EXDEV.  If this
 * would happen to all the packets in the URB, submission fails with a
 * -EXDEV error code.
 *
 * For control endpoints, the synchronous usb_control_msg() call is
 * often used (in non-interrupt context) instead of this call.
 * That is often used through convenience wrappers, for the requests
 * that are standardized in the USB 2.0 specification.  For bulk
 * endpoints, a synchronous usb_bulk_msg() call is available.
 *
 * Return:
 * 0 on successful submissions. A negative error number otherwise.
 *
 * Request Queuing:
 *
 * URBs may be submitted to endpoints before previous ones complete, to
 * minimize the impact of interrupt latencies and system overhead on data
 * throughput.  With that queuing policy, an endpoint's queue would never
 * be empty.  This is required for continuous isochronous data streams,
 * and may also be required for some kinds of interrupt transfers. Such
 * queuing also maximizes bandwidth utilization by letting USB controllers
 * start work on later requests before driver software has finished the
 * completion processing for earlier (successful) requests.
 *
 * As of Linux 2.6, all USB endpoint transfer queues support depths greater
 * than one.  This was previously a HCD-specific behavior, except for ISO
 * transfers.  Non-isochronous endpoint queues are inactive during cleanup
 * after faults (transfer errors or cancellation).
 *
 * Reserved Bandwidth Transfers:
 *
 * Periodic transfers (interrupt or isochronous) are performed repeatedly,
 * using the interval specified in the urb.  Submitting the first urb to
 * the endpoint reserves the bandwidth necessary to make those transfers.
 * If the USB subsystem can't allocate sufficient bandwidth to perform
 * the periodic request, submitting such a periodic request should fail.
 *
 * For devices under xHCI, the bandwidth is reserved at configuration time, or
 * when the alt setting is selected.  If there is not enough bus bandwidth, the
 * configuration/alt setting request will fail.  Therefore, submissions to
 * periodic endpoints on devices under xHCI should never fail due to bandwidth
 * constraints.
 *
 * Device drivers must explicitly request that repetition, by ensuring that
 * some URB is always on the endpoint's queue (except possibly for short
 * periods during completion callbacks).  When there is no longer an urb
 * queued, the endpoint's bandwidth reservation is canceled.  This means
 * drivers can use their completion handlers to ensure they keep bandwidth
 * they need, by reinitializing and resubmitting the just-completed urb
 * until the driver longer needs that periodic bandwidth.
 *
 * Memory Flags:
 *
 * The general rules for how to decide which mem_flags to use
 * are the same as for kmalloc.  There are four
 * different possible values; GFP_KERNEL, GFP_NOFS, GFP_NOIO and
 * GFP_ATOMIC.
 *
 * GFP_NOFS is not ever used, as it has not been implemented yet.
 *
 * GFP_ATOMIC is used when
 *   (a) you are inside a completion handler, an interrupt, bottom half,
 *       tasklet or timer, or
 *   (b) you are holding a spinlock or rwlock (does not apply to
 *       semaphores), or
 *   (c) current->state != TASK_RUNNING, this is the case only after
 *       you've changed it.
 *
 * GFP_NOIO is used in the block io path and error handling of storage
 * devices.
 *
 * All other situations use GFP_KERNEL.
 *
 * Some more specific rules for mem_flags can be inferred, such as
 *  (1) start_xmit, timeout, and receive methods of network drivers must
 *      use GFP_ATOMIC (they are called with a spinlock held);
 *  (2) queuecommand methods of scsi drivers must use GFP_ATOMIC (also
 *      called with a spinlock held);
 *  (3) If you use a kernel thread with a network driver you must use
 *      GFP_NOIO, unless (b) or (c) apply;
 *  (4) after you have done a down() you can use GFP_KERNEL, unless (b) or (c)
 *      apply or your are in a storage driver's block io path;
 *  (5) USB probe and disconnect can use GFP_KERNEL unless (b) or (c) apply; and
 *  (6) changing firmware on a running storage or net device uses
 *      GFP_NOIO, unless b) or c) apply
 *
 */

/*
 * usb_submit_urb -- 向 USB 子系统提交传输请求（核心入口）
 *
 * 这是 USB 数据传输的核心入口函数。函数对 URB 进行严格的参数校验后，
 * 将实际的数据传输委托给主机控制器驱动（HCD）执行。
 *
 * 校验流程（共 10 步，详见函数体内注释）:
 *   Step 1: 检查 urb 和 urb->complete 非空
 *   Step 2: 检查 urb->hcpriv 确保 URB 未被重复提交
 *   Step 3: 检查设备状态 >= USB_STATE_UNAUTHENTICATED
 *   Step 4: 从 pipe 解析端点描述符
 *   Step 5: 初始化 status = -EINPROGRESS, actual_length = 0
 *   Step 6: 按传输类型执行专项校验
 *   Step 7: 检查传输缓冲区长度
 *   Step 8: 校验 transfer_flags 合法性
 *   Step 9: 周期传输（ISO/INT）的 interval 值校验和规整
 *   Step 10: 调用 usb_hcd_submit_urb() 移交给 HCD 层
 *
 * 注意: 此函数可在中断上下文中调用，但需使用 GFP_ATOMIC。
 */
int usb_submit_urb(struct urb *urb, gfp_t mem_flags)
{
	int				xfertype, max;
	struct usb_device		*dev;
	struct usb_host_endpoint	*ep;
	int				is_out;
	unsigned int			allowed;
	bool				is_eusb2_isoch_double;

	/* Step 1: 检查 URB 和完成回调必须非空 */
	if (!urb || !urb->complete)
		return -EINVAL;
	/* Step 2: 确保 URB 未被重复提交（hcpriv 非空表示正在被 HCD 处理） */
	if (urb->hcpriv) {
		WARN_ONCE(1, "URB %p submitted while active\n", urb);
		return -EBUSY;
	}

	/* Step 3: 检查 USB 设备是否已连接且通过认证 */
	dev = urb->dev;
	if ((!dev) || (dev->state < USB_STATE_UNAUTHENTICATED))
		return -ENODEV;

	/* Step 4: 从 pipe 中解析端点描述符 */
	/* For now, get the endpoint from the pipe.  Eventually drivers
	 * will be required to set urb->ep directly and we will eliminate
	 * urb->pipe.
	 */
	ep = usb_pipe_endpoint(dev, urb->pipe);
	if (!ep)
		return -ENOENT;

	/* Step 5: 设置初始状态（status = -EINPROGRESS，等待 HCD 完成） */
	urb->ep = ep;
	urb->status = -EINPROGRESS;
	urb->actual_length = 0;

	/*
	 * Step 6: 按传输类型执行专项校验
	 * 对控制/批量/中断/等时传输分别执行不同的合法性检查
	 */
	/* Lots of sanity checks, so HCDs can rely on clean data
	 * and don't need to duplicate tests
	 */
	xfertype = usb_endpoint_type(&ep->desc);
	if (xfertype == USB_ENDPOINT_XFER_CONTROL) {
		struct usb_ctrlrequest *setup =
				(struct usb_ctrlrequest *) urb->setup_packet;

		if (!setup)
			return -ENOEXEC;
		is_out = !(setup->bRequestType & USB_DIR_IN) ||
				!setup->wLength;
		dev_WARN_ONCE(&dev->dev, (usb_pipeout(urb->pipe) != is_out),
				"BOGUS control dir, pipe %x doesn't match bRequestType %x\n",
				urb->pipe, setup->bRequestType);
		if (le16_to_cpu(setup->wLength) != urb->transfer_buffer_length) {
			dev_dbg(&dev->dev, "BOGUS control len %d doesn't match transfer length %d\n",
					le16_to_cpu(setup->wLength),
					urb->transfer_buffer_length);
			return -EBADR;
		}
	} else {
		is_out = usb_endpoint_dir_out(&ep->desc);
	}

	/* Step 6b: 清除内部 DMA 映射标志，缓存传输方向 */
	/* Clear the internal flags and cache the direction for later use */
	urb->transfer_flags &= ~(URB_DIR_MASK | URB_DMA_MAP_SINGLE |
			URB_DMA_MAP_PAGE | URB_DMA_MAP_SG | URB_MAP_LOCAL |
			URB_SETUP_MAP_SINGLE | URB_SETUP_MAP_LOCAL |
			URB_DMA_SG_COMBINED);
	urb->transfer_flags |= (is_out ? URB_DIR_OUT : URB_DIR_IN);
	kmsan_handle_urb(urb, is_out);

	/* Step 6c: 非控制传输要求设备状态 >= CONFIGURED */
	if (xfertype != USB_ENDPOINT_XFER_CONTROL &&
			dev->state < USB_STATE_CONFIGURED)
		return -ENODEV;

	/* Step 6d: 检查端点最大包长度（maxpacket）是否合法 */
	max = usb_endpoint_maxp(&ep->desc);
	is_eusb2_isoch_double = usb_endpoint_is_hs_isoc_double(dev, ep);
	if (!max && !is_eusb2_isoch_double) {
		dev_dbg(&dev->dev,
			"bogus endpoint ep%d%s in %s (bad maxpacket %d)\n",
			usb_endpoint_num(&ep->desc), is_out ? "out" : "in",
			__func__, max);
		return -EMSGSIZE;
	}

	/*
	 * Step 6e: 等时传输（Isochronous）专项校验
	 * 根据 USB 速度计算每帧允许的最大字节数，
	 * 并逐帧检查 iso_frame_desc 合法性。
	 */
	/* periodic transfers limit size per frame/uframe,
	 * but drivers only control those sizes for ISO.
	 * while we're checking, initialize return status.
	 */
	if (xfertype == USB_ENDPOINT_XFER_ISOC) {
		int	n, len;

		/* SuperSpeed isoc endpoints have up to 16 bursts of up to
		 * 3 packets each
		 */
		if (dev->speed >= USB_SPEED_SUPER) {
			int     burst = 1 + ep->ss_ep_comp.bMaxBurst;
			int     mult = USB_SS_MULT(ep->ss_ep_comp.bmAttributes);
			max *= burst;
			max *= mult;
		}

		if (dev->speed == USB_SPEED_SUPER_PLUS &&
		    USB_SS_SSP_ISOC_COMP(ep->ss_ep_comp.bmAttributes)) {
			struct usb_ssp_isoc_ep_comp_descriptor *isoc_ep_comp;

			isoc_ep_comp = &ep->ssp_isoc_ep_comp;
			max = le32_to_cpu(isoc_ep_comp->dwBytesPerInterval);
		}

		/* High speed, 1-3 packets/uframe, max 6 for eUSB2 double bw */
		if (dev->speed == USB_SPEED_HIGH) {
			if (is_eusb2_isoch_double)
				max = le32_to_cpu(ep->eusb2_isoc_ep_comp.dwBytesPerInterval);
			else
				max *= usb_endpoint_maxp_mult(&ep->desc);
		}

		if (urb->number_of_packets <= 0)
			return -EINVAL;
		for (n = 0; n < urb->number_of_packets; n++) {
			len = urb->iso_frame_desc[n].length;
			if (len < 0 || len > max)
				return -EMSGSIZE;
			urb->iso_frame_desc[n].status = -EXDEV;
			urb->iso_frame_desc[n].actual_length = 0;
		}
	/* Step 6f: 批量/中断传输的 SG（scatter-gather）列表检查 */
	} else if (urb->num_sgs && !urb->dev->bus->no_sg_constraint) {
		struct scatterlist *sg;
		int i;

		for_each_sg(urb->sg, sg, urb->num_sgs - 1, i)
			if (sg->length % max)
				return -EINVAL;
	}

	/* Step 7: 检查传输缓冲区长度不超过上限 */
	/* the I/O buffer must be mapped/unmapped, except when length=0 */
	if (urb->transfer_buffer_length > INT_MAX)
		return -EMSGSIZE;

	/*
	 * Step 8: 校验 transfer_flags 合法性
	 * 过滤掉驱动程序不应设置的内部标志，
	 * 检查 pipe 类型与端点类型是否匹配。
	 * 不符合策略的标志组合会触发警告但不会拒绝提交。
	 */

	/* Check that the pipe's type matches the endpoint's type */
	if (usb_pipe_type_check(urb->dev, urb->pipe))
		dev_warn_once(&dev->dev, "BOGUS urb xfer, pipe %x != type %x\n",
			usb_pipetype(urb->pipe), pipetypes[xfertype]);

	/* Check against a simple/standard policy */
	allowed = (URB_NO_TRANSFER_DMA_MAP | URB_NO_INTERRUPT | URB_DIR_MASK |
			URB_FREE_BUFFER);
	switch (xfertype) {
	case USB_ENDPOINT_XFER_BULK:
	case USB_ENDPOINT_XFER_INT:
		if (is_out)
			allowed |= URB_ZERO_PACKET;
		fallthrough;
	default:			/* all non-iso endpoints */
		if (!is_out)
			allowed |= URB_SHORT_NOT_OK;
		break;
	case USB_ENDPOINT_XFER_ISOC:
		allowed |= URB_ISO_ASAP;
		break;
	}
	allowed &= urb->transfer_flags;

	/* warn if submitter gave bogus flags */
	if (allowed != urb->transfer_flags)
		dev_WARN(&dev->dev, "BOGUS urb flags, %x --> %x\n",
			urb->transfer_flags, allowed);

	/*
	 * Step 9: 周期传输（ISO/INT）的 interval 值校验和规整
	 * 强制 interval 为合法的 2 的幂，简化 HCD 的处理。
	 * 根据不同 USB 速度（SS/SSP/HS/FS/LS）有不同的单位
	 * （微帧或帧）和上限。
	 *
	 * Force periodic transfer intervals to be legal values that are
	 * a power of two (so HCDs don't need to).
	 *
	 * FIXME want bus->{intr,iso}_sched_horizon values here.  Each HC
	 * supports different values... this uses EHCI/UHCI defaults (and
	 * EHCI can use smaller non-default values).
	 */
	switch (xfertype) {
	case USB_ENDPOINT_XFER_ISOC:
	case USB_ENDPOINT_XFER_INT:
		/* too small? */
		if (urb->interval <= 0)
			return -EINVAL;

		/* too big? */
		switch (dev->speed) {
		case USB_SPEED_SUPER_PLUS:
		case USB_SPEED_SUPER:	/* units are 125us */
			/* Handle up to 2^(16-1) microframes */
			if (urb->interval > (1 << 15))
				return -EINVAL;
			max = 1 << 15;
			break;
		case USB_SPEED_HIGH:	/* units are microframes */
			/* NOTE usb handles 2^15 */
			if (urb->interval > (1024 * 8))
				urb->interval = 1024 * 8;
			max = 1024 * 8;
			break;
		case USB_SPEED_FULL:	/* units are frames/msec */
		case USB_SPEED_LOW:
			if (xfertype == USB_ENDPOINT_XFER_INT) {
				if (urb->interval > 255)
					return -EINVAL;
				/* NOTE ohci only handles up to 32 */
				max = 128;
			} else {
				if (urb->interval > 1024)
					urb->interval = 1024;
				/* NOTE usb and ohci handle up to 2^15 */
				max = 1024;
			}
			break;
		default:
			return -EINVAL;
		}
		/* Round down to a power of 2, no more than max */
		urb->interval = min(max, 1 << ilog2(urb->interval));
	}

	/* Step 10: 所有校验通过，委托给 HCD 层执行实际的硬件传输 */
	return usb_hcd_submit_urb(urb, mem_flags);
}
EXPORT_SYMBOL_GPL(usb_submit_urb);

/*-------------------------------------------------------------------*/

/**
 * usb_unlink_urb - abort/cancel a transfer request for an endpoint
 * @urb: pointer to urb describing a previously submitted request,
 *	may be NULL
 *
 * This routine cancels an in-progress request.  URBs complete only once
 * per submission, and may be canceled only once per submission.
 * Successful cancellation means termination of @urb will be expedited
 * and the completion handler will be called with a status code
 * indicating that the request has been canceled (rather than any other
 * code).
 *
 * Drivers should not call this routine or related routines, such as
 * usb_kill_urb(), after their disconnect method has returned. The
 * disconnect function should synchronize with a driver's I/O routines
 * to insure that all URB-related activity has completed before it returns.
 *
 * This request is asynchronous, however the HCD might call the ->complete()
 * callback during unlink. Therefore when drivers call usb_unlink_urb(), they
 * must not hold any locks that may be taken by the completion function.
 * Success is indicated by returning -EINPROGRESS, at which time the URB will
 * probably not yet have been given back to the device driver. When it is
 * eventually called, the completion function will see @urb->status ==
 * -ECONNRESET.
 * Failure is indicated by usb_unlink_urb() returning any other value.
 * Unlinking will fail when @urb is not currently "linked" (i.e., it was
 * never submitted, or it was unlinked before, or the hardware is already
 * finished with it), even if the completion handler has not yet run.
 *
 * The URB must not be deallocated while this routine is running.  In
 * particular, when a driver calls this routine, it must insure that the
 * completion handler cannot deallocate the URB.
 *
 * Return: -EINPROGRESS on success. See description for other values on
 * failure.
 *
 * Unlinking and Endpoint Queues:
 *
 * [The behaviors and guarantees described below do not apply to virtual
 * root hubs but only to endpoint queues for physical USB devices.]
 *
 * Host Controller Drivers (HCDs) place all the URBs for a particular
 * endpoint in a queue.  Normally the queue advances as the controller
 * hardware processes each request.  But when an URB terminates with an
 * error its queue generally stops (see below), at least until that URB's
 * completion routine returns.  It is guaranteed that a stopped queue
 * will not restart until all its unlinked URBs have been fully retired,
 * with their completion routines run, even if that's not until some time
 * after the original completion handler returns.  The same behavior and
 * guarantee apply when an URB terminates because it was unlinked.
 *
 * Bulk and interrupt endpoint queues are guaranteed to stop whenever an
 * URB terminates with any sort of error, including -ECONNRESET, -ENOENT,
 * and -EREMOTEIO.  Control endpoint queues behave the same way except
 * that they are not guaranteed to stop for -EREMOTEIO errors.  Queues
 * for isochronous endpoints are treated differently, because they must
 * advance at fixed rates.  Such queues do not stop when an URB
 * encounters an error or is unlinked.  An unlinked isochronous URB may
 * leave a gap in the stream of packets; it is undefined whether such
 * gaps can be filled in.
 *
 * Note that early termination of an URB because a short packet was
 * received will generate a -EREMOTEIO error if and only if the
 * URB_SHORT_NOT_OK flag is set.  By setting this flag, USB device
 * drivers can build deep queues for large or complex bulk transfers
 * and clean them up reliably after any sort of aborted transfer by
 * unlinking all pending URBs at the first fault.
 *
 * When a control URB terminates with an error other than -EREMOTEIO, it
 * is quite likely that the status stage of the transfer will not take
 * place.
 */

/*
 * usb_unlink_urb -- 异步取消一个正在进行的 URB 传输
 *
 * 与 usb_kill_urb() 不同，此函数是异步操作：
 *   - 立即返回 -EINPROGRESS（成功发起取消请求）
 *   - URB 的 complete() 回调仍会在未来某个时间点被调用，
 *     此时 urb->status == -ECONNRESET
 *   - 调用者不能持有 complete() 中可能获取的任何锁
 *
 * 如果 URB 未被提交、已被取消或硬件已完成处理，取消请求将失败。
 * 取消操作对等时传输的队列调度也有特殊影响（可能留下间隔）。
 *
 * 返回: -EINPROGRESS（成功发起取消）或负错误码。
 */
int usb_unlink_urb(struct urb *urb)
{
	if (!urb)
		return -EINVAL;
	if (!urb->dev)
		return -ENODEV;
	if (!urb->ep)
		return -EIDRM;
	return usb_hcd_unlink_urb(urb, -ECONNRESET);
}
EXPORT_SYMBOL_GPL(usb_unlink_urb);

/**
 * usb_kill_urb - cancel a transfer request and wait for it to finish
 * @urb: pointer to URB describing a previously submitted request,
 *	may be NULL
 *
 * This routine cancels an in-progress request.  It is guaranteed that
 * upon return all completion handlers will have finished and the URB
 * will be totally idle and available for reuse.  These features make
 * this an ideal way to stop I/O in a disconnect() callback or close()
 * function.  If the request has not already finished or been unlinked
 * the completion handler will see urb->status == -ENOENT.
 *
 * While the routine is running, attempts to resubmit the URB will fail
 * with error -EPERM.  Thus even if the URB's completion handler always
 * tries to resubmit, it will not succeed and the URB will become idle.
 *
 * The URB must not be deallocated while this routine is running.  In
 * particular, when a driver calls this routine, it must insure that the
 * completion handler cannot deallocate the URB.
 *
 * This routine may not be used in an interrupt context (such as a bottom
 * half or a completion handler), or when holding a spinlock, or in other
 * situations where the caller can't schedule().
 *
 * This routine should not be called by a driver after its disconnect
 * method has returned.
 */

/*
 * usb_kill_urb -- 同步取消 URB 并等待其完全停止
 *
 * 这是最常用的 URB 取消函数，典型应用场景:
 *   - disconnect() 回调中停止所有 I/O
 *   - 驱动程序关闭时确保 URB 完全停止
 *
 * 工作流程:
 *   1. might_sleep() 确保不在原子上下文中调用
 *   2. atomic_inc(&urb->reject) 设置拒绝标志，
 *      阻止新的提交请求（即使 complete() 尝试 resubmit 也会失败）
 *   3. smp_mb__after_atomic() 配合内存屏障，
 *      确保 reject 写入在 use_count 读取之前可见
 *   4. usb_hcd_unlink_urb(urb, -ENOENT) 通知 HCD 取消传输
 *   5. wait_event(usb_kill_urb_queue, use_count == 0) 等待 HCD
 *      完成所有处理，此时 complete() 已经执行完毕
 *   6. atomic_dec(&urb->reject) 清除拒绝标志
 *
 * 与 usb_unlink_urb() 的区别:
 *   同步等待完成 vs 异步返回
 * 与 usb_poison_urb() 的区别:
 *   kill 在完成后允许 URB 被重新提交，poison 则永久阻止
 */
void usb_kill_urb(struct urb *urb)
{
	might_sleep();
	if (!(urb && urb->dev && urb->ep))
		return;
	atomic_inc(&urb->reject);
	/*
	 * Order the write of urb->reject above before the read
	 * of urb->use_count below.  Pairs with the barriers in
	 * __usb_hcd_giveback_urb() and usb_hcd_submit_urb().
	 */
	smp_mb__after_atomic();

	usb_hcd_unlink_urb(urb, -ENOENT);
	wait_event(usb_kill_urb_queue, atomic_read(&urb->use_count) == 0);

	atomic_dec(&urb->reject);
}
EXPORT_SYMBOL_GPL(usb_kill_urb);

/**
 * usb_poison_urb - reliably kill a transfer and prevent further use of an URB
 * @urb: pointer to URB describing a previously submitted request,
 *	may be NULL
 *
 * This routine cancels an in-progress request.  It is guaranteed that
 * upon return all completion handlers will have finished and the URB
 * will be totally idle and cannot be reused.  These features make
 * this an ideal way to stop I/O in a disconnect() callback.
 * If the request has not already finished or been unlinked
 * the completion handler will see urb->status == -ENOENT.
 *
 * After and while the routine runs, attempts to resubmit the URB will fail
 * with error -EPERM.  Thus even if the URB's completion handler always
 * tries to resubmit, it will not succeed and the URB will become idle.
 *
 * The URB must not be deallocated while this routine is running.  In
 * particular, when a driver calls this routine, it must insure that the
 * completion handler cannot deallocate the URB.
 *
 * This routine may not be used in an interrupt context (such as a bottom
 * half or a completion handler), or when holding a spinlock, or in other
 * situations where the caller can't schedule().
 *
 * This routine should not be called by a driver after its disconnect
 * method has returned.
 */

/*
 * usb_poison_urb -- 同步取消 URB 并永久阻止其重新提交
 *
 * 与 usb_kill_urb() 的关键区别: poison 在设置 reject 标志后不恢复，
 * 因此 URB 被 poison 后永久不可用（任何后续提交尝试都会返回 -EPERM）。
 * 这与 disconnect() 场景高度匹配——设备已断开，URB 不应再被使用。
 *
 * 注意: poison 不会减少 reject 计数，这与 kill 不同。
 * 要恢复 URB 的可用性，必须调用 usb_unpoison_urb()。
 */
void usb_poison_urb(struct urb *urb)
{
	might_sleep();
	if (!urb)
		return;
	atomic_inc(&urb->reject);
	/*
	 * Order the write of urb->reject above before the read
	 * of urb->use_count below.  Pairs with the barriers in
	 * __usb_hcd_giveback_urb() and usb_hcd_submit_urb().
	 */
	smp_mb__after_atomic();

	if (!urb->dev || !urb->ep)
		return;

	usb_hcd_unlink_urb(urb, -ENOENT);
	wait_event(usb_kill_urb_queue, atomic_read(&urb->use_count) == 0);
}
EXPORT_SYMBOL_GPL(usb_poison_urb);

/*
 * usb_unpoison_urb -- 取消 URB 的 poison 状态，允许其重新提交
 *
 * 与 usb_poison_urb() 配对使用。将 atomic_t reject 计数减 1，
 * 当 reject 归零后，URB 可以再次被 usb_submit_urb() 接受。
 *
 * 注意: 如果同一个 URB 被多次 poison（reject > 1），
 * 则需要相同次数的 unpoison 才能完全解除阻止状态。
 */
void usb_unpoison_urb(struct urb *urb)
{
	if (!urb)
		return;

	atomic_dec(&urb->reject);
}
EXPORT_SYMBOL_GPL(usb_unpoison_urb);

/**
 * usb_block_urb - reliably prevent further use of an URB
 * @urb: pointer to URB to be blocked, may be NULL
 *
 * After the routine has run, attempts to resubmit the URB will fail
 * with error -EPERM.  Thus even if the URB's completion handler always
 * tries to resubmit, it will not succeed and the URB will become idle.
 *
 * The URB must not be deallocated while this routine is running.  In
 * particular, when a driver calls this routine, it must insure that the
 * completion handler cannot deallocate the URB.
 */

/*
 * usb_block_urb -- 阻止 URB 被提交（设置 reject 标志）
 *
 * usb_poison_urb() 的轻量版别名。仅设置 reject 标志但不执行
 * 取消操作，适用于驱动只需要防止未来提交而不需要取消正在进行的
 * 传输的场景。
 *
 * 与 usb_poison_urb() 的区别:
 *   - block 不调用 usb_hcd_unlink_urb()（不取消正在进行的传输）
 *   - block 不等待 use_count 归零（不同步等待）
 *   - block 可在非可睡眠上下文调用（无 might_sleep）
 */
void usb_block_urb(struct urb *urb)
{
	if (!urb)
		return;

	atomic_inc(&urb->reject);
}
EXPORT_SYMBOL_GPL(usb_block_urb);

/**
 * usb_kill_anchored_urbs - kill all URBs associated with an anchor
 * @anchor: anchor the requests are bound to
 *
 * This kills all outstanding URBs starting from the back of the queue,
 * with guarantee that no completer callbacks will take place from the
 * anchor after this function returns.
 *
 * This routine should not be called by a driver after its disconnect
 * method has returned.
 */

/*
 * usb_kill_anchored_urbs -- 批量杀死锚点中的所有 URB
 *
 * 遍历锚点链表，从队尾开始逐个调用 usb_kill_urb() 同步取消
 * 所有挂接到该锚点的 URB。每次操作前通过 usb_get_urb() 增加
 * 引用计数以防止 URB 在 kill 过程中被释放。
 *
 * do { ... } while (!surely_empty) 循环确保即使新的 URB
 * 在遍历过程中被添加到锚点，最终也能处理完毕。
 * 注意: usb_kill_urb() 内部会解锚 URB（unanchor）。
 */
void usb_kill_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;
	int surely_empty;

	do {
		spin_lock_irq(&anchor->lock);
		while (!list_empty(&anchor->urb_list)) {
			victim = list_entry(anchor->urb_list.prev,
					    struct urb, anchor_list);
			/* make sure the URB isn't freed before we kill it */
			usb_get_urb(victim);
			spin_unlock_irq(&anchor->lock);
			/* this will unanchor the URB */
			usb_kill_urb(victim);
			usb_put_urb(victim);
			spin_lock_irq(&anchor->lock);
		}
		surely_empty = usb_anchor_check_wakeup(anchor);

		spin_unlock_irq(&anchor->lock);
		cpu_relax();
	} while (!surely_empty);
}
EXPORT_SYMBOL_GPL(usb_kill_anchored_urbs);


/**
 * usb_poison_anchored_urbs - cease all traffic from an anchor
 * @anchor: anchor the requests are bound to
 *
 * this allows all outstanding URBs to be poisoned starting
 * from the back of the queue. Newly added URBs will also be
 * poisoned
 *
 * This routine should not be called by a driver after its disconnect
 * method has returned.
 */

/*
 * usb_poison_anchored_urbs -- 批量毒化锚点中的所有 URB
 *
 * 与 usb_kill_anchored_urbs() 的区别:
 *   1. 设置 anchor->poisoned = 1，使得后续通过 usb_anchor_urb()
 *      新挂接的 URB 也会被自动标记 reject
 *   2. 对每个 URB 调用 usb_poison_urb()（而非 usb_kill_urb()），
 *      这样即使 URB 的 complete() 回调尝试重新提交也会失败
 *
 * 适用于设备断开时彻底停止所有通信的场景。
 */
void usb_poison_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;
	int surely_empty;

	do {
		spin_lock_irq(&anchor->lock);
		anchor->poisoned = 1;
		while (!list_empty(&anchor->urb_list)) {
			victim = list_entry(anchor->urb_list.prev,
					    struct urb, anchor_list);
			/* make sure the URB isn't freed before we kill it */
			usb_get_urb(victim);
			spin_unlock_irq(&anchor->lock);
			/* this will unanchor the URB */
			usb_poison_urb(victim);
			usb_put_urb(victim);
			spin_lock_irq(&anchor->lock);
		}
		surely_empty = usb_anchor_check_wakeup(anchor);

		spin_unlock_irq(&anchor->lock);
		cpu_relax();
	} while (!surely_empty);
}
EXPORT_SYMBOL_GPL(usb_poison_anchored_urbs);

/**
 * usb_unpoison_anchored_urbs - let an anchor be used successfully again
 * @anchor: anchor the requests are bound to
 *
 * Reverses the effect of usb_poison_anchored_urbs
 * the anchor can be used normally after it returns
 */

/*
 * usb_unpoison_anchored_urbs -- 恢复锚点及其所有 URB 的正常状态
 *
 * 遍历锚点中所有的 URB，逐一调用 usb_unpoison_urb() 清除 reject 标志，
 * 然后将 anchor->poisoned 置 0。
 *
 * 变量名 lazarue（拉撒路，圣经中死后复活的人物）很形象地反映了
 * 函数的作用——让被 poison 的 URB"复活"。
 */
void usb_unpoison_anchored_urbs(struct usb_anchor *anchor)
{
	unsigned long flags;
	struct urb *lazarus;

	spin_lock_irqsave(&anchor->lock, flags);
	list_for_each_entry(lazarus, &anchor->urb_list, anchor_list) {
		usb_unpoison_urb(lazarus);
	}
	anchor->poisoned = 0;
	spin_unlock_irqrestore(&anchor->lock, flags);
}
EXPORT_SYMBOL_GPL(usb_unpoison_anchored_urbs);

/**
 * usb_anchor_suspend_wakeups
 * @anchor: the anchor you want to suspend wakeups on
 *
 * Call this to stop the last urb being unanchored from waking up any
 * usb_wait_anchor_empty_timeout waiters. This is used in the hcd urb give-
 * back path to delay waking up until after the completion handler has run.
 */

/*
 * usb_anchor_suspend_wakeups -- 暂停锚点的空队列唤醒通知
 *
 * 在 HCD 返回 URB 的回调路径中调用，增加 suspend_wakeups 计数，
 * 阻止最后一个 URB 解锚时唤醒 usb_wait_anchor_empty_timeout() 的等待者。
 * 目的是确保在 complete() 回调执行完毕之前，不会误判锚点已空。
 */
void usb_anchor_suspend_wakeups(struct usb_anchor *anchor)
{
	if (anchor)
		atomic_inc(&anchor->suspend_wakeups);
}
EXPORT_SYMBOL_GPL(usb_anchor_suspend_wakeups);

/**
 * usb_anchor_resume_wakeups
 * @anchor: the anchor you want to resume wakeups on
 *
 * Allow usb_wait_anchor_empty_timeout waiters to be woken up again, and
 * wake up any current waiters if the anchor is empty.
 */

/*
 * usb_anchor_resume_wakeups -- 恢复锚点的空队列唤醒通知
 *
 * 与 usb_anchor_suspend_wakeups() 配对。减少 suspend_wakeups 计数，
 * 如果锚点确实为空，立即唤醒等待者。
 */
void usb_anchor_resume_wakeups(struct usb_anchor *anchor)
{
	if (!anchor)
		return;

	atomic_dec(&anchor->suspend_wakeups);
	if (usb_anchor_check_wakeup(anchor))
		wake_up(&anchor->wait);
}
EXPORT_SYMBOL_GPL(usb_anchor_resume_wakeups);

/**
 * usb_wait_anchor_empty_timeout - wait for an anchor to be unused
 * @anchor: the anchor you want to become unused
 * @timeout: how long you are willing to wait in milliseconds
 *
 * Call this is you want to be sure all an anchor's
 * URBs have finished
 *
 * Return: Non-zero if the anchor became unused. Zero on timeout.
 */

/*
 * usb_wait_anchor_empty_timeout -- 等待锚点变空（带超时）
 *
 * 通过 wait_event_timeout 等待锚点的所有 URB 被解锚。
 * 当最后一个 URB 被解锚时，__usb_unanchor_urb() 会调用
 * wake_up(&anchor->wait) 唤醒此处的等待者。
 *
 * suspend_wakeups 机制防止在 complete() 回调执行中途
 * 错误地触发唤醒——只有当 suspend_wakeups == 0 且
 * urb_list 为空时才算真正空。
 */
int usb_wait_anchor_empty_timeout(struct usb_anchor *anchor,
				  unsigned int timeout)
{
	return wait_event_timeout(anchor->wait,
				  usb_anchor_check_wakeup(anchor),
				  msecs_to_jiffies(timeout));
}
EXPORT_SYMBOL_GPL(usb_wait_anchor_empty_timeout);

/**
 * usb_get_from_anchor - get an anchor's oldest urb
 * @anchor: the anchor whose urb you want
 *
 * This will take the oldest urb from an anchor,
 * unanchor and return it
 *
 * Return: The oldest urb from @anchor, or %NULL if @anchor has no
 * urbs associated with it.
 */

/*
 * usb_get_from_anchor -- 从锚点取出最旧的 URB（FIFO）
 *
 * 从锚点链表头部取出最早的 URB，先增加引用计数（usb_get_urb）
 * 防止被释放，然后调用 __usb_unanchor_urb() 解锚。
 * 调用者获得 URB 的所有权，后续必须调用 usb_free_urb()。
 *
 * 这是锚点作为"简单 URB 队列"使用的典型模式。
 */
struct urb *usb_get_from_anchor(struct usb_anchor *anchor)
{
	struct urb *victim;
	unsigned long flags;

	spin_lock_irqsave(&anchor->lock, flags);
	if (!list_empty(&anchor->urb_list)) {
		victim = list_entry(anchor->urb_list.next, struct urb,
				    anchor_list);
		usb_get_urb(victim);
		__usb_unanchor_urb(victim, anchor);
	} else {
		victim = NULL;
	}
	spin_unlock_irqrestore(&anchor->lock, flags);

	return victim;
}

EXPORT_SYMBOL_GPL(usb_get_from_anchor);

/**
 * usb_scuttle_anchored_urbs - unanchor all an anchor's urbs
 * @anchor: the anchor whose urbs you want to unanchor
 *
 * use this to get rid of all an anchor's urbs
 */

/*
 * usb_scuttle_anchored_urbs -- 清空锚点但不取消传输
 *
 * 与 usb_kill_anchored_urbs() 的区别: 此函数只解锚 URB
 * （将其从锚点链表移除）但不调用 usb_kill_urb() 取消传输，
 * 也不设置 poison 标志。
 *
 * 适用于驱动已经通过其他方式处理了 URB 生命周期，
 * 只需要清理锚点记录的场景。
 */
void usb_scuttle_anchored_urbs(struct usb_anchor *anchor)
{
	struct urb *victim;
	unsigned long flags;
	int surely_empty;

	do {
		spin_lock_irqsave(&anchor->lock, flags);
		while (!list_empty(&anchor->urb_list)) {
			victim = list_entry(anchor->urb_list.prev,
					    struct urb, anchor_list);
			__usb_unanchor_urb(victim, anchor);
		}
		surely_empty = usb_anchor_check_wakeup(anchor);

		spin_unlock_irqrestore(&anchor->lock, flags);
		cpu_relax();
	} while (!surely_empty);
}

EXPORT_SYMBOL_GPL(usb_scuttle_anchored_urbs);

/**
 * usb_anchor_empty - is an anchor empty
 * @anchor: the anchor you want to query
 *
 * Return: 1 if the anchor has no urbs associated with it.
 */

/*
 * usb_anchor_empty -- 检查锚点是否为空
 *
 * 简单地检查锚点的 urb_list 链表是否为空。
 * 注意: 不获取锁，仅用于快速判断，调用者需确保适当的同步。
 */
int usb_anchor_empty(struct usb_anchor *anchor)
{
	return list_empty(&anchor->urb_list);
}

EXPORT_SYMBOL_GPL(usb_anchor_empty);

