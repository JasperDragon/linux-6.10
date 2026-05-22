/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IRQ subsystem internal functions and variables.
 *
 * 此文件仅供 kernel/irq/ 内部使用, 外部代码请勿引用。
 *
 * ============================================================================
 * 中断子系统核心概念
 * ============================================================================
 *
 * 两条 IRQ 状态线:
 *
 * 1) irq_data.state_use_accessors (IRQD_* flags)
 *    - 描述 irq_chip 级别的硬件状态
 *    - 如: IRQD_IRQ_MASKED (硬件屏蔽), IRQD_IRQ_INPROGRESS (正在处理中),
 *      IRQD_TRIGGER_MASK (触发类型: 边沿/电平/...)
 *
 * 2) irq_desc.status_use_accessors (_IRQ_* flags, 通过 settings.h 访问)
 *    - 描述 Linux IRQ 子系统的软件状态
 *    - 如: _IRQ_PER_CPU (per-CPU 中断), _IRQ_LEVEL (电平触发),
 *      _IRQ_NOPROBE (禁止自动探测), _IRQ_NOTHREAD (禁止线程化)
 *
 * 3) irq_desc.istate (IRQS_* flags, 内部核心状态)
 *    - 中断处理流程的瞬时状态机
 *    - 如: IRQS_PENDING (待重发), IRQS_REPLAY (已重发), IRQS_ONESHOT,
 *      IRQS_SUSPENDED, IRQS_SPURIOUS_DISABLED
 *
 * 4) irqaction 线程标志 (IRQTF_*)
 *    - 中断线程的状态控制
 *    - 如: IRQTF_RUNTHREAD (唤醒线程), IRQTF_FORCED_THREAD (强制线程化)
 */
#include <linux/irqdesc.h>
#include <linux/kernel_stat.h>
#include <linux/pm_runtime.h>
#include <linux/sched/clock.h>

#ifdef CONFIG_SPARSE_IRQ
# define MAX_SPARSE_IRQS	INT_MAX
#else
# define MAX_SPARSE_IRQS	NR_IRQS
#endif

#define istate core_internal_state__do_not_mess_with_it

extern bool noirqdebug;
extern int irq_poll_cpu;

extern struct irqaction chained_action;

/*
 * 中断线程标志 (IRQTF_*) — 控制 threaded interrupt handler 的行为。
 *
 * IRQTF_RUNTHREAD      — 通知 irq 线程: 有工作要处理
 * IRQTF_WARNED         — 已打印 "IRQ_WAKE_THREAD w/o thread_fn" 警告 (防重复)
 * IRQTF_AFFINITY       — irq 线程被请求调整 CPU 亲和性
 * IRQTF_FORCED_THREAD  — 该 irqaction 被强制线程化 (irq_thread 内核参数)
 * IRQTF_READY          — irq 线程已准备好 (已完成初始化, 可以处理中断)
 */
enum {
	IRQTF_RUNTHREAD,
	IRQTF_WARNED,
	IRQTF_AFFINITY,
	IRQTF_FORCED_THREAD,
	IRQTF_READY,
};

/*
 * desc->istate 位掩码 (IRQS_*) — 中断处理流程的核心状态机。
 *
 * IRQS_AUTODETECT        — 自动探测正在进行 (探测哪个 IRQ 触发了)
 * IRQS_SPURIOUS_DISABLED — 因伪中断检测被禁用 (spurious.c)
 * IRQS_POLL_INPROGRESS   — 轮询模式正在处理此中断
 * IRQS_ONESHOT           — ONESHOT 模式: 主 handler 返回后不自动 unmask,
 *                          由线程 handler 负责重新使能中断
 * IRQS_REPLAY            — 中断已重发, 在 handler 运行并清除此标志前
 *                          不会再次重发
 * IRQS_WAITING           — 中断正在等待重发 (边沿中断丢失检测)
 * IRQS_PENDING           — 中断需要重发, 在下一个可用时机重发
 * IRQS_SUSPENDED         — 中断因系统挂起而被暂停
 * IRQS_NMI               — 此中断线用于递送 NMI (不可屏蔽中断)
 * IRQS_SYSFS             — 描述符已添加到 sysfs
 */
enum {
	IRQS_AUTODETECT		= 0x00000001,
	IRQS_SPURIOUS_DISABLED	= 0x00000002,
	IRQS_POLL_INPROGRESS	= 0x00000008,
	IRQS_ONESHOT		= 0x00000020,
	IRQS_REPLAY		= 0x00000040,
	IRQS_WAITING		= 0x00000080,
	IRQS_PENDING		= 0x00000200,
	IRQS_SUSPENDED		= 0x00000800,
	IRQS_TIMINGS		= 0x00001000,
	IRQS_NMI		= 0x00002000,
	IRQS_SYSFS		= 0x00004000,
};

#include "debug.h"
#include "settings.h"

extern int __irq_set_trigger(struct irq_desc *desc, unsigned long flags);
extern void __disable_irq(struct irq_desc *desc);
extern void __enable_irq(struct irq_desc *desc);

#define IRQ_RESEND	true
#define IRQ_NORESEND	false

#define IRQ_START_FORCE	true
#define IRQ_START_COND	false

extern int irq_activate(struct irq_desc *desc);
extern int irq_activate_and_startup(struct irq_desc *desc, bool resend);
extern int irq_startup(struct irq_desc *desc, bool resend, bool force);
extern void irq_startup_managed(struct irq_desc *desc);

extern void irq_shutdown(struct irq_desc *desc);
extern void irq_shutdown_and_deactivate(struct irq_desc *desc);
extern void irq_disable(struct irq_desc *desc);
extern void irq_percpu_enable(struct irq_desc *desc, unsigned int cpu);
extern void irq_percpu_disable(struct irq_desc *desc, unsigned int cpu);
extern void mask_irq(struct irq_desc *desc);
extern void unmask_irq(struct irq_desc *desc);
extern void unmask_threaded_irq(struct irq_desc *desc);

#ifdef CONFIG_SPARSE_IRQ
static inline void irq_mark_irq(unsigned int irq) { }
#else
extern void irq_mark_irq(unsigned int irq);
#endif

irqreturn_t __handle_irq_event_percpu(struct irq_desc *desc);
irqreturn_t handle_irq_event_percpu(struct irq_desc *desc);
irqreturn_t handle_irq_event(struct irq_desc *desc);

/* Resending of interrupts :*/
int check_irq_resend(struct irq_desc *desc, bool inject);
void clear_irq_resend(struct irq_desc *desc);
void irq_resend_init(struct irq_desc *desc);
void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action);

void wake_threads_waitq(struct irq_desc *desc);

#ifdef CONFIG_PROC_FS
extern void register_irq_proc(unsigned int irq, struct irq_desc *desc);
extern void unregister_irq_proc(unsigned int irq, struct irq_desc *desc);
extern void register_handler_proc(unsigned int irq, struct irqaction *action);
extern void unregister_handler_proc(unsigned int irq, struct irqaction *action);
#else
static inline void register_irq_proc(unsigned int irq, struct irq_desc *desc) { }
static inline void unregister_irq_proc(unsigned int irq, struct irq_desc *desc) { }
static inline void register_handler_proc(unsigned int irq,
					 struct irqaction *action) { }
static inline void unregister_handler_proc(unsigned int irq,
					   struct irqaction *action) { }
#endif

extern bool irq_can_set_affinity_usr(unsigned int irq);

extern int irq_do_set_affinity(struct irq_data *data,
			       const struct cpumask *dest, bool force);
extern void irq_affinity_schedule_notify_work(struct irq_desc *desc);

#ifdef CONFIG_SMP
extern int irq_setup_affinity(struct irq_desc *desc);
#else
static inline int irq_setup_affinity(struct irq_desc *desc) { return 0; }
#endif

#define for_each_action_of_desc(desc, act)			\
	for (act = desc->action; act; act = act->next)

/* Inline functions for support of irq chips on slow busses */
static inline void chip_bus_lock(struct irq_desc *desc)
{
	if (unlikely(desc->irq_data.chip->irq_bus_lock))
		desc->irq_data.chip->irq_bus_lock(&desc->irq_data);
}

static inline void chip_bus_sync_unlock(struct irq_desc *desc)
{
	if (unlikely(desc->irq_data.chip->irq_bus_sync_unlock))
		desc->irq_data.chip->irq_bus_sync_unlock(&desc->irq_data);
}

#define _IRQ_DESC_CHECK		(1 << 0)
#define _IRQ_DESC_PERCPU	(1 << 1)

#define IRQ_GET_DESC_CHECK_GLOBAL	(_IRQ_DESC_CHECK)
#define IRQ_GET_DESC_CHECK_PERCPU	(_IRQ_DESC_CHECK | _IRQ_DESC_PERCPU)

struct irq_desc *__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,
				     unsigned int check);
void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus);

__DEFINE_CLASS_IS_CONDITIONAL(irqdesc_lock, true);
__DEFINE_UNLOCK_GUARD(irqdesc_lock, struct irq_desc,
		      __irq_put_desc_unlock(_T->lock, _T->flags, _T->bus),
		      unsigned long flags; bool bus);

static inline class_irqdesc_lock_t class_irqdesc_lock_constructor(unsigned int irq, bool bus,
								  unsigned int check)
{
	class_irqdesc_lock_t _t = { .bus = bus, };

	_t.lock = __irq_get_desc_lock(irq, &_t.flags, bus, check);

	return _t;
}

#define scoped_irqdesc_get_and_lock(_irq, _check)		\
	scoped_guard(irqdesc_lock, _irq, false, _check)

#define scoped_irqdesc_get_and_buslock(_irq, _check)		\
	scoped_guard(irqdesc_lock, _irq, true, _check)

#define scoped_irqdesc		((struct irq_desc *)(__guard_ptr(irqdesc_lock)(&scope)))

#define __irqd_to_state(d) ACCESS_PRIVATE((d)->common, state_use_accessors)

static inline unsigned int irqd_get(struct irq_data *d)
{
	return __irqd_to_state(d);
}

/*
 * Manipulation functions for irq_data.state
 */
static inline void irqd_set_move_pending(struct irq_data *d)
{
	__irqd_to_state(d) |= IRQD_SETAFFINITY_PENDING;
}

static inline void irqd_clr_move_pending(struct irq_data *d)
{
	__irqd_to_state(d) &= ~IRQD_SETAFFINITY_PENDING;
}

static inline void irqd_set_managed_shutdown(struct irq_data *d)
{
	__irqd_to_state(d) |= IRQD_MANAGED_SHUTDOWN;
}

static inline void irqd_clr_managed_shutdown(struct irq_data *d)
{
	__irqd_to_state(d) &= ~IRQD_MANAGED_SHUTDOWN;
}

static inline void irqd_clear(struct irq_data *d, unsigned int mask)
{
	__irqd_to_state(d) &= ~mask;
}

static inline void irqd_set(struct irq_data *d, unsigned int mask)
{
	__irqd_to_state(d) |= mask;
}

static inline bool irqd_has_set(struct irq_data *d, unsigned int mask)
{
	return __irqd_to_state(d) & mask;
}

static inline void irq_state_set_disabled(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_DISABLED);
}

static inline void irq_state_set_masked(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_MASKED);
}

#undef __irqd_to_state

static inline void __kstat_incr_irqs_this_cpu(struct irq_desc *desc)
{
	__this_cpu_inc(desc->kstat_irqs->cnt);
	__this_cpu_inc(kstat.irqs_sum);
}

static inline void kstat_incr_irqs_this_cpu(struct irq_desc *desc)
{
	__kstat_incr_irqs_this_cpu(desc);
	desc->tot_count++;
}

static inline int irq_desc_get_node(struct irq_desc *desc)
{
	return irq_common_data_get_node(&desc->irq_common_data);
}

static inline int irq_desc_is_chained(struct irq_desc *desc)
{
	return (desc->action && desc->action == &chained_action);
}

static inline bool irq_is_nmi(struct irq_desc *desc)
{
	return desc->istate & IRQS_NMI;
}

#ifdef CONFIG_PM_SLEEP
void irq_pm_handle_wakeup(struct irq_desc *desc);
void irq_pm_install_action(struct irq_desc *desc, struct irqaction *action);
void irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action);
#else
static inline void irq_pm_handle_wakeup(struct irq_desc *desc) { }
static inline void
irq_pm_install_action(struct irq_desc *desc, struct irqaction *action) { }
static inline void
irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action) { }
#endif

#ifdef CONFIG_GENERIC_IRQ_CHIP
void irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,
			   int num_ct, unsigned int irq_base,
			   void __iomem *reg_base, irq_flow_handler_t handler);
#else
static inline void
irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,
		      int num_ct, unsigned int irq_base,
		      void __iomem *reg_base, irq_flow_handler_t handler) { }
#endif /* CONFIG_GENERIC_IRQ_CHIP */

#ifdef CONFIG_GENERIC_PENDING_IRQ
static inline bool irq_can_move_pcntxt(struct irq_data *data)
{
	return !(data->chip->flags & IRQCHIP_MOVE_DEFERRED);
}
static inline bool irq_move_pending(struct irq_data *data)
{
	return irqd_is_setaffinity_pending(data);
}
static inline void
irq_copy_pending(struct irq_desc *desc, const struct cpumask *mask)
{
	cpumask_copy(desc->pending_mask, mask);
}
static inline void
irq_get_pending(struct cpumask *mask, struct irq_desc *desc)
{
	cpumask_copy(mask, desc->pending_mask);
}
static inline struct cpumask *irq_desc_get_pending_mask(struct irq_desc *desc)
{
	return desc->pending_mask;
}
bool irq_fixup_move_pending(struct irq_desc *desc, bool force_clear);
void irq_force_complete_move(struct irq_desc *desc);
#else /* CONFIG_GENERIC_PENDING_IRQ */
static inline bool irq_can_move_pcntxt(struct irq_data *data)
{
	return true;
}
static inline bool irq_move_pending(struct irq_data *data)
{
	return false;
}
static inline void
irq_copy_pending(struct irq_desc *desc, const struct cpumask *mask)
{
}
static inline void
irq_get_pending(struct cpumask *mask, struct irq_desc *desc)
{
}
static inline struct cpumask *irq_desc_get_pending_mask(struct irq_desc *desc)
{
	return NULL;
}
static inline bool irq_fixup_move_pending(struct irq_desc *desc, bool fclear)
{
	return false;
}
static inline void irq_force_complete_move(struct irq_desc *desc) { }
#endif /* !CONFIG_GENERIC_PENDING_IRQ */

#if !defined(CONFIG_IRQ_DOMAIN) || !defined(CONFIG_IRQ_DOMAIN_HIERARCHY)
static inline int irq_domain_activate_irq(struct irq_data *data, bool reserve)
{
	irqd_set_activated(data);
	return 0;
}
static inline void irq_domain_deactivate_irq(struct irq_data *data)
{
	irqd_clr_activated(data);
}
#endif

static inline struct irq_data *irqd_get_parent_data(struct irq_data *irqd)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	return irqd->parent_data;
#else
	return NULL;
#endif
}

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
#include <linux/debugfs.h>

struct irq_bit_descr {
	unsigned int	mask;
	char		*name;
};

#define BIT_MASK_DESCR(m)	{ .mask = m, .name = #m }

void irq_debug_show_bits(struct seq_file *m, int ind, unsigned int state,
			 const struct irq_bit_descr *sd, int size);

void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *desc);
static inline void irq_remove_debugfs_entry(struct irq_desc *desc)
{
	debugfs_remove(desc->debugfs_file);
	kfree(desc->dev_name);
}
void irq_debugfs_copy_devname(int irq, struct device *dev);
# ifdef CONFIG_IRQ_DOMAIN
void irq_domain_debugfs_init(struct dentry *root);
# else
static inline void irq_domain_debugfs_init(struct dentry *root)
{
}
# endif
#else /* CONFIG_GENERIC_IRQ_DEBUGFS */
static inline void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *d)
{
}
static inline void irq_remove_debugfs_entry(struct irq_desc *d)
{
}
static inline void irq_debugfs_copy_devname(int irq, struct device *dev)
{
}
#endif /* CONFIG_GENERIC_IRQ_DEBUGFS */
