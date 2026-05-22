// SPDX-License-Identifier: MIT

/*
 * 文件名: drm_vblank_work.c
 *
 * 中文描述: VBlank 工作队列管理
 *
 * 本文件实现了基于 VBlank（垂直消隐期）的工作队列调度机制。在 DRM 子系统中，
 * 许多驱动程序需要在特定的扫描输出区域内完成硬件编程操作，这些操作对时间极为敏感。
 * 传统的 IRQ 处理程序可能因执行时间过长而无法完成这些任务，而普通的工作队列又可能
 * 被调度器抢占导致错过硬件截止时间。
 *
 * drm_vblank_work 提供了一种通用的延迟工作执行方案：将工作项延迟到特定的 VBlank
 * 计数到达后执行，并以实时优先级运行。这最大程度地降低了被抢占的概率，为时间敏感型
 * 硬件编程提供了最佳的执行时机保障。
 *
 * 核心机制包括：
 *   1. 工作项调度 (drm_vblank_work_schedule) - 指定目标 VBlank 计数进行调度
 *   2. 工作项取消 (drm_vblank_work_cancel_sync) - 同步取消已调度的工作
 *   3. 工作项刷新 (drm_vblank_work_flush) - 等待工作项执行完成
 *   4. 工作项初始化 (drm_vblank_work_init) - 初始化 VBlank 工作项
 *
 * 工作项基于 kthread_work 实现，通过 kthread_worker 在专用内核线程中执行。
 */

#include <uapi/linux/sched/types.h>

#include <linux/export.h>

#include <drm/drm_print.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_work.h>
#include <drm/drm_crtc.h>

#include "drm_internal.h"

/**
 * DOC: vblank works
 *
 * Many DRM drivers need to program hardware in a time-sensitive manner, many
 * times with a deadline of starting and finishing within a certain region of
 * the scanout. Most of the time the safest way to accomplish this is to
 * simply do said time-sensitive programming in the driver's IRQ handler,
 * which allows drivers to avoid being preempted during these critical
 * regions. Or even better, the hardware may even handle applying such
 * time-critical programming independently of the CPU.
 *
 * While there's a decent amount of hardware that's designed so that the CPU
 * doesn't need to be concerned with extremely time-sensitive programming,
 * there's a few situations where it can't be helped. Some unforgiving
 * hardware may require that certain time-sensitive programming be handled
 * completely by the CPU, and said programming may even take too long to
 * handle in an IRQ handler. Another such situation would be where the driver
 * needs to perform a task that needs to complete within a specific scanout
 * period, but might possibly block and thus cannot be handled in an IRQ
 * context. Both of these situations can't be solved perfectly in Linux since
 * we're not a realtime kernel, and thus the scheduler may cause us to miss
 * our deadline if it decides to preempt us. But for some drivers, it's good
 * enough if we can lower our chance of being preempted to an absolute
 * minimum.
 *
 * This is where &drm_vblank_work comes in. &drm_vblank_work provides a simple
 * generic delayed work implementation which delays work execution until a
 * particular vblank has passed, and then executes the work at realtime
 * priority. This provides the best possible chance at performing
 * time-sensitive hardware programming on time, even when the system is under
 * heavy load. &drm_vblank_work also supports rescheduling, so that self
 * re-arming work items can be easily implemented.
 */

/*
 * drm_handle_vblank_works - 处理所有待处理的 VBlank 工作项
 * @vblank: VBlank CRTC 对象
 *
 * 在 VBlank 中断处理程序中被调用。遍历所有待处理的工作项列表，
 * 将那些已经达到目标 VBlank 计数的工作项从等待队列中移除，
 * 释放 VBlank 引用计数，并将其提交到 kthread 工作队列中执行。
 * 如果有任何工作项被提交，则唤醒等待队列中可能正在睡眠的任务。
 *
 * 调用者必须持有 dev->event_lock 自旋锁。
 */
void drm_handle_vblank_works(struct drm_vblank_crtc *vblank)
{
	struct drm_vblank_work *work, *next;
	u64 count = atomic64_read(&vblank->count);
	bool wake = false;

	assert_spin_locked(&vblank->dev->event_lock);

	list_for_each_entry_safe(work, next, &vblank->pending_work, node) {
		if (!drm_vblank_passed(count, work->count))
			continue;

		list_del_init(&work->node);
		drm_vblank_put(vblank->dev, vblank->pipe);
		kthread_queue_work(vblank->worker, &work->base);
		wake = true;
	}
	if (wake)
		wake_up_all(&vblank->work_wait_queue);
}

/*
 * drm_vblank_cancel_pending_works - 取消所有待处理的 VBlank 工作项
 * @vblank: VBlank CRTC 对象
 *
 * 当 VBlank 中断被禁用时调用。取消所有仍在等待 VBlank 计数的
 * 工作项，将其从待处理列表中移除，并释放相应的 VBlank 引用计数。
 *
 * 这通常在 CRTC 关闭或模式设置时发生，此时 VBlank 中断不再产生，
 * 因此等待的工作项永远不会被触发，需要手动清理。
 *
 * 调用者必须持有 dev->event_lock 自旋锁。
 */
void drm_vblank_cancel_pending_works(struct drm_vblank_crtc *vblank)
{
	struct drm_vblank_work *work, *next;

	assert_spin_locked(&vblank->dev->event_lock);

	drm_WARN_ONCE(vblank->dev, !list_empty(&vblank->pending_work),
		      "Cancelling pending vblank works!\n");

	list_for_each_entry_safe(work, next, &vblank->pending_work, node) {
		list_del_init(&work->node);
		drm_vblank_put(vblank->dev, vblank->pipe);
	}

	wake_up_all(&vblank->work_wait_queue);
}

/**
 * drm_vblank_work_schedule - 调度一个 VBlank 工作项
 *
 * 此函数将一个 VBlank 工作项调度到指定 VBlank 计数的目标。当 CRTC 的
 * VBlank 计数达到 @count 时，工作项将在实时优先级的 kthread 上执行。
 *
 * 如果目标计数已经过去：
 *   - nextonmiss=false: 工作项立即执行
 *   - nextonmiss=true:  延迟到下一个 VBlank
 *
 * 如果工作项已经被调度，将使用新的 @count 重新调度（适用于自重新武装的工作项）。
 */
 * @work: vblank work to schedule
 * @count: target vblank count
 * @nextonmiss: defer until the next vblank if target vblank was missed
 *
 * Schedule @work for execution once the crtc vblank count reaches @count.
 *
 * If the crtc vblank count has already reached @count and @nextonmiss is
 * %false the work starts to execute immediately.
 *
 * If the crtc vblank count has already reached @count and @nextonmiss is
 * %true the work is deferred until the next vblank (as if @count has been
 * specified as crtc vblank count + 1).
 *
 * If @work is already scheduled, this function will reschedule said work
 * using the new @count. This can be used for self-rearming work items.
 *
 * Returns:
 * %1 if @work was successfully (re)scheduled, %0 if it was either already
 * scheduled or cancelled, or a negative error code on failure.
 */
int drm_vblank_work_schedule(struct drm_vblank_work *work,
			     u64 count, bool nextonmiss)
{
	struct drm_vblank_crtc *vblank = work->vblank;
	struct drm_device *dev = vblank->dev;
	u64 cur_vbl;
	unsigned long irqflags;
	bool passed, inmodeset, rescheduling = false, wake = false;
	int ret = 0;

	spin_lock_irqsave(&dev->event_lock, irqflags);
	if (work->cancelling)
		goto out;

	spin_lock(&dev->vbl_lock);
	inmodeset = vblank->inmodeset;
	spin_unlock(&dev->vbl_lock);
	if (inmodeset)
		goto out;

	if (list_empty(&work->node)) {
		ret = drm_vblank_get(dev, vblank->pipe);
		if (ret < 0)
			goto out;
	} else if (work->count == count) {
		/* Already scheduled w/ same vbl count */
		goto out;
	} else {
		rescheduling = true;
	}

	work->count = count;
	cur_vbl = drm_vblank_count(dev, vblank->pipe);
	passed = drm_vblank_passed(cur_vbl, count);
	if (passed)
		drm_dbg_core(dev,
			     "crtc %d vblank %llu already passed (current %llu)\n",
			     vblank->pipe, count, cur_vbl);

	if (!nextonmiss && passed) {
		drm_vblank_put(dev, vblank->pipe);
		ret = kthread_queue_work(vblank->worker, &work->base);

		if (rescheduling) {
			list_del_init(&work->node);
			wake = true;
		}
	} else {
		if (!rescheduling)
			list_add_tail(&work->node, &vblank->pending_work);
		ret = true;
	}

out:
	spin_unlock_irqrestore(&dev->event_lock, irqflags);
	if (wake)
		wake_up_all(&vblank->work_wait_queue);
	return ret;
}
EXPORT_SYMBOL(drm_vblank_work_schedule);

/**
 * drm_vblank_work_cancel_sync - 取消 VBlank 工作项并等待其执行完毕
 *
 * 同步取消一个已调度的 VBlank 工作项。如果工作项已经在执行，
 * 将等待其完成。返回后确保工作项不再被调度或运行中，
 * 即使它是自重新武装的（self-arming）。
 *
 * 取消过程分为两步：
 *   1. 如果工作项仍在 pending 列表中，从列表中移除并释放 VBlank 引用
 *   2. 通过 kthread_cancel_work_sync() 等待内核线程中的工作项完成
 *
 * 在等待期间设置 cancelling 标志，防止工作项被重新调度。
 */
 * @work: vblank work to cancel
 *
 * Cancel an already scheduled vblank work and wait for its
 * execution to finish.
 *
 * On return, @work is guaranteed to no longer be scheduled or running, even
 * if it's self-arming.
 *
 * Returns:
 * %True if the work was cancelled before it started to execute, %false
 * otherwise.
 */
bool drm_vblank_work_cancel_sync(struct drm_vblank_work *work)
{
	struct drm_vblank_crtc *vblank = work->vblank;
	struct drm_device *dev = vblank->dev;
	bool ret = false;

	spin_lock_irq(&dev->event_lock);
	if (!list_empty(&work->node)) {
		list_del_init(&work->node);
		drm_vblank_put(vblank->dev, vblank->pipe);
		ret = true;
	}

	work->cancelling++;
	spin_unlock_irq(&dev->event_lock);

	wake_up_all(&vblank->work_wait_queue);

	if (kthread_cancel_work_sync(&work->base))
		ret = true;

	spin_lock_irq(&dev->event_lock);
	work->cancelling--;
	spin_unlock_irq(&dev->event_lock);

	return ret;
}
EXPORT_SYMBOL(drm_vblank_work_cancel_sync);

/**
 * drm_vblank_work_flush - 等待已调度的 VBlank 工作项执行完成
 *
 * 等待指定的 VBlank 工作项至少执行一次完成。如果工作项正在等待
 * VBlank 计数到达，会先等待它从 pending 列表中移除（即 VBlank 条件满足），
 * 然后再等待内核线程工作执行完毕。
 *
 * 与 cancel_sync 不同，flush 不会取消工作项，只是等待其完成。
 */
 * @work: vblank work to flush
 *
 * Wait until @work has finished executing once.
 */
void drm_vblank_work_flush(struct drm_vblank_work *work)
{
	struct drm_vblank_crtc *vblank = work->vblank;
	struct drm_device *dev = vblank->dev;

	spin_lock_irq(&dev->event_lock);
	wait_event_lock_irq(vblank->work_wait_queue, list_empty(&work->node),
			    dev->event_lock);
	spin_unlock_irq(&dev->event_lock);

	kthread_flush_work(&work->base);
}
EXPORT_SYMBOL(drm_vblank_work_flush);

/**
 * drm_vblank_work_flush_all - 刷新 CRTC 上所有待处理的 VBlank 工作项
 *
 * 等待指定 CRTC 上当前所有排队的 VBlank 工作项都执行完成。
 * 首先等待 pending_work 列表为空（所有工作项的 VBlank 条件都已满足），
 * 然后等待 kthread_worker 中的所有工作执行完毕。
 *
 * 这在 CRTC 关闭或模式设置时特别有用，确保在继续之前所有
 * 待处理的 VBlank 工作都已完成。
 */
 * @crtc: crtc for which vblank work to flush
 *
 * Wait until all currently queued vblank work on @crtc
 * has finished executing once.
 */
void drm_vblank_work_flush_all(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);

	spin_lock_irq(&dev->event_lock);
	wait_event_lock_irq(vblank->work_wait_queue,
			    list_empty(&vblank->pending_work),
			    dev->event_lock);
	spin_unlock_irq(&dev->event_lock);

	kthread_flush_worker(vblank->worker);
}
EXPORT_SYMBOL(drm_vblank_work_flush_all);

/**
 * drm_vblank_work_init - 初始化一个 VBlank 工作项
 *
 * 初始化一个 VBlank 工作项，将其绑定到指定的 CRTC。工作项初始化后
 * 尚未被调度，需要通过 drm_vblank_work_schedule() 来调度执行。
 *
 * 内核线程工作函数 @func 将在 VBlank 条件满足后以实时优先级被调用。
 * 工作函数接口为 void (*func)(struct kthread_work *work)，可以通过
 * container_of 获取对应的 drm_vblank_work 结构体。
 */
 * @work: vblank work item
 * @crtc: CRTC whose vblank will trigger the work execution
 * @func: work function to be executed
 *
 * Initialize a vblank work item for a specific crtc.
 */
void drm_vblank_work_init(struct drm_vblank_work *work, struct drm_crtc *crtc,
			  void (*func)(struct kthread_work *work))
{
	kthread_init_work(&work->base, func);
	INIT_LIST_HEAD(&work->node);
	work->vblank = drm_crtc_vblank_crtc(crtc);
}
EXPORT_SYMBOL(drm_vblank_work_init);

/*
 * drm_vblank_worker_init - 初始化 VBlank kthread 工作者
 * @vblank: VBlank CRTC 对象
 *
 * 为指定的 VBlank CRTC 创建一个专用的 kthread_worker 内核线程。
 * 该线程将负责执行所有与此 VBlank CRTC 关联的工作项。
 *
 * 初始化的内容包括：
 *   1. 初始化 pending_work 链表头
 *   2. 初始化工作等待队列
 *   3. 创建名为 "card%d-crtc%d" 的内核线程
 *   4. 将线程调度策略设置为 FIFO 实时优先级
 *
 * 返回：0 成功，负错误码失败
 */
int drm_vblank_worker_init(struct drm_vblank_crtc *vblank)
{
	struct kthread_worker *worker;

	INIT_LIST_HEAD(&vblank->pending_work);
	init_waitqueue_head(&vblank->work_wait_queue);
	worker = kthread_run_worker(0, "card%d-crtc%d",
				       vblank->dev->primary->index,
				       vblank->pipe);
	if (IS_ERR(worker))
		return PTR_ERR(worker);

	vblank->worker = worker;

	sched_set_fifo(worker->task);
	return 0;
}
