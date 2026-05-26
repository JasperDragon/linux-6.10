// SPDX-License-Identifier: GPL-2.0
/*
 * handle.c — 核心中断处理入口 (irq_handle / irq_handler dispatch).
 *
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * ============================================================================
 * 中断处理流程
 * ============================================================================
 *
 * 当 CPU 收到一个 IRQ 时, 经过以下路径:
 *
 *   架构入口 (arch entry, e.g. vectors.S)
 *     → 读取中断控制器寄存器获取 hwirq
 *     → irq_domain 将 hwirq 翻译为 Linux IRQ 号
 *     → generic_handle_irq(irq)
 *       └─ generic_handle_irq_desc(desc)
 *           └─ desc->handle_irq(desc)    ← 高层 flow handler
 *                ├─ handle_level_irq()   — 电平触发
 *                ├─ handle_edge_irq()    — 边沿触发
 *                ├─ handle_fasteoi_irq() — EOI 型控制器 (ARM GIC)
 *                └─ handle_percpu_irq()  — per-CPU 中断 (PPI/SGI)
 *                     │
 *                     └─ handle_irq_event(desc)
 *                          └─ handle_irq_event_percpu(desc)
 *                               ├─ 遍历 desc->action 链表
 *                               │   ├─ 调用 action->handler (主 handler)
 *                               │   └─ 若返回 IRQ_WAKE_THREAD, 唤醒 action->thread_fn
 *                               └─ 统计计数 + 随机熵采集
 *
 * 关键概念:
 *   - flow handler: 负责处理中断控制器的硬件协议 (mask/ack/eoi),
 *     确保相同中断线不会嵌套重入
 *   - handler / thread_fn: 驱动注册的业务处理逻辑
 *   - ONESHOT: 主 handler 处理完后不自动 unmask, 线程 handler 完成后才 unmask
 */

#include <linux/irq.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>

#include <asm/irq_regs.h>

#include <trace/events/irq.h>

#include "internals.h"

#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init;
#endif

/**
 * handle_bad_irq - handle spurious and unhandled irqs
 * @desc:      description of the interrupt
 *
 * Handles spurious and unhandled IRQ's. It also prints a debugmessage.
 */
void handle_bad_irq(struct irq_desc *desc)
{
	unsigned int irq = irq_desc_get_irq(desc);

	print_irq_desc(irq, desc);
	kstat_incr_irqs_this_cpu(desc);
	ack_bad_irq(irq);
}
EXPORT_SYMBOL_GPL(handle_bad_irq);

/*
 * Special, empty irq handler:
 */
irqreturn_t no_action(int cpl, void *dev_id)
{
	return IRQ_NONE;
}
EXPORT_SYMBOL_GPL(no_action);

static void warn_no_thread(unsigned int irq, struct irqaction *action)
{
	if (test_and_set_bit(IRQTF_WARNED, &action->thread_flags))
		return;

	printk(KERN_WARNING "IRQ %d device %s returned IRQ_WAKE_THREAD "
	       "but no thread function available.", irq, action->name);
}

void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action)
{
	/*
	 * In case the thread crashed and was killed we just pretend that
	 * we handled the interrupt. The hardirq handler has disabled the
	 * device interrupt, so no irq storm is lurking.
	 */
	if (action->thread->flags & PF_EXITING)
		return;

	/*
	 * Wake up the handler thread for this action. If the
	 * RUNTHREAD bit is already set, nothing to do.
	 */
	if (test_and_set_bit(IRQTF_RUNTHREAD, &action->thread_flags))
		return;

	/*
	 * It's safe to OR the mask lockless here. We have only two
	 * places which write to threads_oneshot: This code and the
	 * irq thread.
	 *
	 * This code is the hard irq context and can never run on two
	 * cpus in parallel. If it ever does we have more serious
	 * problems than this bitmask.
	 *
	 * The irq threads of this irq which clear their "running" bit
	 * in threads_oneshot are serialized via desc->lock against
	 * each other and they are serialized against this code by
	 * IRQS_INPROGRESS.
	 *
	 * Hard irq handler:
	 *
	 *	spin_lock(desc->lock);
	 *	desc->state |= IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *	set_bit(IRQTF_RUNTHREAD, &action->thread_flags);
	 *	desc->threads_oneshot |= mask;
	 *	spin_lock(desc->lock);
	 *	desc->state &= ~IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *
	 * irq thread:
	 *
	 * again:
	 *	spin_lock(desc->lock);
	 *	if (desc->state & IRQS_INPROGRESS) {
	 *		spin_unlock(desc->lock);
	 *		while(desc->state & IRQS_INPROGRESS)
	 *			cpu_relax();
	 *		goto again;
	 *	}
	 *	if (!test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
	 *		desc->threads_oneshot &= ~mask;
	 *	spin_unlock(desc->lock);
	 *
	 * So either the thread waits for us to clear IRQS_INPROGRESS
	 * or we are waiting in the flow handler for desc->lock to be
	 * released before we reach this point. The thread also checks
	 * IRQTF_RUNTHREAD under desc->lock. If set it leaves
	 * threads_oneshot untouched and runs the thread another time.
	 */
	desc->threads_oneshot |= action->thread_mask;

	/*
	 * We increment the threads_active counter in case we wake up
	 * the irq thread. The irq thread decrements the counter when
	 * it returns from the handler or in the exit path and wakes
	 * up waiters which are stuck in synchronize_irq() when the
	 * active count becomes zero. synchronize_irq() is serialized
	 * against this code (hard irq handler) via IRQS_INPROGRESS
	 * like the finalize_oneshot() code. See comment above.
	 */
	atomic_inc(&desc->threads_active);

	/*
	 * This might be a premature wakeup before the thread reached the
	 * thread function and set the IRQTF_READY bit. It's waiting in
	 * kthread code with state UNINTERRUPTIBLE. Once it reaches the
	 * thread function it waits with INTERRUPTIBLE. The wakeup is not
	 * lost in that case because the thread is guaranteed to observe
	 * the RUN flag before it goes to sleep in wait_for_interrupt().
	 */
	wake_up_state(action->thread, TASK_INTERRUPTIBLE);
}

static DEFINE_STATIC_KEY_FALSE(irqhandler_duration_check_enabled);
static u64 irqhandler_duration_threshold_ns __ro_after_init;

static int __init irqhandler_duration_check_setup(char *arg)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(arg, 0, &val);
	if (ret) {
		pr_err("Unable to parse irqhandler.duration_warn_us setting: ret=%d\n", ret);
		return 0;
	}

	if (!val) {
		pr_err("Invalid irqhandler.duration_warn_us setting, must be > 0\n");
		return 0;
	}

	irqhandler_duration_threshold_ns = val * 1000;
	static_branch_enable(&irqhandler_duration_check_enabled);

	return 1;
}
__setup("irqhandler.duration_warn_us=", irqhandler_duration_check_setup);

static inline void irqhandler_duration_check(u64 ts_start, unsigned int irq,
					     const struct irqaction *action)
{
	u64 delta_ns = local_clock() - ts_start;

	if (unlikely(delta_ns > irqhandler_duration_threshold_ns)) {
		pr_warn_ratelimited("[CPU%u] long duration of IRQ[%u:%ps], took: %llu us\n",
				    smp_processor_id(), irq, action->handler,
				    div_u64(delta_ns, NSEC_PER_USEC));
	}
}

/*
 * __handle_irq_event_percpu — 在当前 CPU 上执行中断处理链 (核心 hot path)。
 * @desc: 中断描述符
 *
 * 这是中断子系统最核心的执行函数, 负责:
 *   1) 遍历 desc->action 链表, 逐个调用已注册的 handler
 *   2) 根据返回值决定是否唤醒线程 handler:
 *      - IRQ_NONE:        中断不属于此 handler, 继续下一个
 *      - IRQ_HANDLED:     中断已被处理, 继续下一个 handler
 *      - IRQ_WAKE_THREAD: 主 handler 完成, 需要唤醒 thread_fn 继续处理
 *   3) 累加 retval: 只要有一个 handler 返回了 IRQ_HANDLED, 最终就是 IRQ_HANDLED
 *
 * 锁上下文: 在中断上下文中运行 (hardirq), 关中断, 持有 desc->lock
 */
irqreturn_t __handle_irq_event_percpu(struct irq_desc *desc)
{
	irqreturn_t retval = IRQ_NONE;
	unsigned int irq = desc->irq_data.irq;
	struct irqaction *action;

	for_each_action_of_desc(desc, action) {
		irqreturn_t res;

		/*
		 * If this IRQ would be threaded under force_irqthreads, mark it so.
		 */
		if (irq_settings_can_thread(desc) &&
		    !(action->flags & (IRQF_NO_THREAD | IRQF_PERCPU | IRQF_ONESHOT)))
			lockdep_hardirq_threaded();

		trace_irq_handler_entry(irq, action);

		if (static_branch_unlikely(&irqhandler_duration_check_enabled)) {
			u64 ts_start = local_clock();

			res = action->handler(irq, action->dev_id);
			irqhandler_duration_check(ts_start, irq, action);
		} else {
			res = action->handler(irq, action->dev_id);
		}

		trace_irq_handler_exit(irq, action, res);

		if (WARN_ONCE(!irqs_disabled(),"irq %u handler %pS enabled interrupts\n",
			      irq, action->handler))
			local_irq_disable();

		switch (res) {
		case IRQ_WAKE_THREAD:
			/*
			 * Catch drivers which return WAKE_THREAD but
			 * did not set up a thread function
			 */
			if (unlikely(!action->thread_fn)) {
				warn_no_thread(irq, action);
				break;
			}

			__irq_wake_thread(desc, action);
			break;

		default:
			break;
		}

		retval |= res;
	}

	return retval;
}

irqreturn_t handle_irq_event_percpu(struct irq_desc *desc)
{
	irqreturn_t retval;

	retval = __handle_irq_event_percpu(desc);

	add_interrupt_randomness(desc->irq_data.irq);

	if (!irq_settings_no_debug(desc))
		note_interrupt(desc, retval);
	return retval;
}

irqreturn_t handle_irq_event(struct irq_desc *desc)
{
	irqreturn_t ret;

	desc->istate &= ~IRQS_PENDING;
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	raw_spin_unlock(&desc->lock);

	ret = handle_irq_event_percpu(desc);

	raw_spin_lock(&desc->lock);
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	return ret;
}

#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER
int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	if (handle_arch_irq)
		return -EBUSY;

	handle_arch_irq = handle_irq;
	return 0;
}

/**
 * generic_handle_arch_irq - root irq handler for architectures which do no
 *                           entry accounting themselves
 * @regs:	Register file coming from the low-level handling code
 */
asmlinkage void noinstr generic_handle_arch_irq(struct pt_regs *regs)
{
	struct pt_regs *old_regs;

	irq_enter();
	old_regs = set_irq_regs(regs);
	handle_arch_irq(regs);
	set_irq_regs(old_regs);
	irq_exit();
}
#endif
