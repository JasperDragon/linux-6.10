/*
 * Copyright (C) 2013 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * 中文注释: DRM 页面翻转工作队列 (Page Flip Work Queue)
 *
 * 本文件实现了页面翻转工作队列 (flip-work) 机制, 用于在 DRM 驱动中
 * 延迟执行页面翻转相关操作。典型应用场景是在 vblank 中断上下文中
 * 提交工作, 然后在工作队列中实际执行。
 *
 * 工作流程:
 *   1. drm_flip_work_queue(): 在任何上下文 (包括 vblank 中断) 中
 *      将工作项加入队列。如果无法分配内存, 则立即执行回调函数。
 *   2. drm_flip_work_commit(): 在 vblank 中断中提交所有排队的工作,
 *      将任务转移到工作队列执行。
 *   3. flip_worker: 工作线程, 遍历 commited 列表并调用回调函数。
 *
 * 设计特点:
 *   - 使用两个列表 (queued 和 commited) 实现生产-消费模式
 *   - 生产者 (任意上下文) 添加到 queued 列表
 *   - 消费者 (工作线程) 从 commited 列表取出执行
 *   - commit 操作用 list_splice_tail 原子性地转移整个列表
 */

#include <linux/export.h>
#include <linux/slab.h>

#include <drm/drm_flip_work.h>
#include <drm/drm_print.h>
#include <drm/drm_util.h>

struct drm_flip_task {
	struct list_head node;
	void *data;
};

static struct drm_flip_task *drm_flip_work_allocate_task(void *data, gfp_t flags)
{
	struct drm_flip_task *task;

	task = kzalloc_obj(*task, flags);
	if (task)
		task->data = data;

	return task;
}

static void drm_flip_work_queue_task(struct drm_flip_work *work, struct drm_flip_task *task)
{
	unsigned long flags;

	spin_lock_irqsave(&work->lock, flags);
	list_add_tail(&task->node, &work->queued);
	spin_unlock_irqrestore(&work->lock, flags);
}

/**
 * 中文注释: 将工作项加入队列
 * 将 @val 加入翻转工作队列。如果内存分配失败 (无法创建 task),
 * 则立即在当前上下文中同步执行回调函数 (fallback)。
 * 可在任何上下文中调用, 包括 vblank 中断处理函数。
 * 在可睡眠的上下文中使用 GFP_KERNEL, 否则使用 GFP_ATOMIC。
 *
 * drm_flip_work_queue - queue work
 * @work: the flip-work
 * @val: the value to queue
 *
 * Queues work, that will later be run (passed back to drm_flip_func_t
 * func) on a work queue after drm_flip_work_commit() is called.
 */
void drm_flip_work_queue(struct drm_flip_work *work, void *val)
{
	struct drm_flip_task *task;

	task = drm_flip_work_allocate_task(val,
				drm_can_sleep() ? GFP_KERNEL : GFP_ATOMIC);
	if (task) {
		drm_flip_work_queue_task(work, task);
	} else {
		DRM_ERROR("%s could not allocate task!\n", work->name);
		work->func(work, val);
	}
}
EXPORT_SYMBOL(drm_flip_work_queue);

/**
 * 中文注释: 提交已排队的工作
 * 将 queued 列表中的所有工作项原子性地转移到 commited 列表,
 * 并通过 queue_work() 调度工作线程执行。典型用法:
 *   1. 在 vblank 中断或之前任意时刻通过 drm_flip_work_queue() 排队
 *   2. 在 vblank 中断中调用此函数提交所有排队的工作
 *   3. 工作线程在进程上下文中执行实际的回调函数
 *
 * drm_flip_work_commit - commit queued work
 * @work: the flip-work
 * @wq: the work-queue to run the queued work on
 *
 * Trigger work previously queued by drm_flip_work_queue() to run
 * on a workqueue.  The typical usage would be to queue work (via
 * drm_flip_work_queue()) at any point (from vblank irq and/or
 * prior), and then from vblank irq commit the queued work.
 */
void drm_flip_work_commit(struct drm_flip_work *work,
		struct workqueue_struct *wq)
{
	unsigned long flags;

	spin_lock_irqsave(&work->lock, flags);
	list_splice_tail(&work->queued, &work->commited);
	INIT_LIST_HEAD(&work->queued);
	spin_unlock_irqrestore(&work->lock, flags);
	queue_work(wq, &work->worker);
}
EXPORT_SYMBOL(drm_flip_work_commit);

static void flip_worker(struct work_struct *w)
{
	struct drm_flip_work *work = container_of(w, struct drm_flip_work, worker);
	struct list_head tasks;
	unsigned long flags;

	while (1) {
		struct drm_flip_task *task, *tmp;

		INIT_LIST_HEAD(&tasks);
		spin_lock_irqsave(&work->lock, flags);
		list_splice_tail(&work->commited, &tasks);
		INIT_LIST_HEAD(&work->commited);
		spin_unlock_irqrestore(&work->lock, flags);

		if (list_empty(&tasks))
			break;

		list_for_each_entry_safe(task, tmp, &tasks, node) {
			work->func(work, task->data);
			kfree(task);
		}
	}
}

/**
 * 中文注释: 初始化翻转工作队列
 * 初始化 drm_flip_work 结构, 包括 queued/commited 链表、自旋锁
 * 和工作线程。@name 用于调试标识, @func 是处理工作项的回调函数。
 *
 * drm_flip_work_init - initialize flip-work
 * @work: the flip-work to initialize
 * @name: debug name
 * @func: the callback work function
 *
 * Initializes/allocates resources for the flip-work
 */
void drm_flip_work_init(struct drm_flip_work *work,
		const char *name, drm_flip_func_t func)
{
	work->name = name;
	INIT_LIST_HEAD(&work->queued);
	INIT_LIST_HEAD(&work->commited);
	spin_lock_init(&work->lock);
	work->func = func;

	INIT_WORK(&work->worker, flip_worker);
}
EXPORT_SYMBOL(drm_flip_work_init);

/**
 * 中文注释: 清理翻转工作队列
 * 销毁翻转工作队列的资源。如果在清理时仍有未处理的工作项
 * (queued 或 commited 列表非空), 发出警告。
 *
 * drm_flip_work_cleanup - cleans up flip-work
 * @work: the flip-work to cleanup
 *
 * Destroy resources allocated for the flip-work
 */
void drm_flip_work_cleanup(struct drm_flip_work *work)
{
	WARN_ON(!list_empty(&work->queued) || !list_empty(&work->commited));
}
EXPORT_SYMBOL(drm_flip_work_cleanup);
