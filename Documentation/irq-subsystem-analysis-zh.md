# Linux Kernel IRQ 中断子系统框架分析

## 概述

Linux IRQ 子系统负责管理系统中所有的硬件中断——从硬件中断控制器触发信号的那一刻，到驱动程序的 handler 函数被执行，再到中断完成后的清理和统计。它是内核中最核心的基础设施之一，与调度器、时间子系统、RCU 共同构成操作系统的运行时骨架。

理解 IRQ 子系统的关键不是死记函数名，而是把握三条主线：

1. **数据流**：硬件 IRQ 如何一步步传递到驱动 handler
2. **控制流**：中断控制器如何被配置、使能、屏蔽
3. **并发模型**：在 SMP 环境下，中断处理如何保证正确性和性能

---

## 1. 整体架构：从硬件到驱动

```
硬件中断信号
    │
    ▼
中断控制器 (GIC / GPIO expander / PCI MSI...)
    │  读取硬件寄存器获取 hwirq
    ▼
架构入口 (arch entry, e.g. vectors.S / entry.S)
    │  irq_enter() 进入中断上下文
    ▼
irq_domain 翻译
    │  hwirq → Linux IRQ 号 (通过 linear / tree / direct 映射)
    ▼
generic_handle_irq(irq)
    │
    ▼
desc->handle_irq(desc)    ← 高层 flow handler (handle_fasteoi_irq 等)
    │
    ├─ mask / ack / eoi    ← 与硬件控制器交互
    │
    ▼
handle_irq_event(desc)
    │
    ▼
handle_irq_event_percpu(desc)
    │
    ├─ 遍历 desc->action 链表
    │   ├─ action->handler(irq, dev_id)     ← 驱动的主 handler (hardirq 上下文)
    │   └─ 若返回 IRQ_WAKE_THREAD:
    │       └─ 唤醒 action->thread_fn       ← 驱动线程 handler (进程上下文)
    │
    ▼
irq_exit() 退出中断上下文, 触发 softirq 等
```

这一整条路径的核心目标：**将硬件中断安全地转换为驱动可处理的软件事件，并在 SMP 环境下保证正确性和性能。**

---

## 2. 核心数据结构及其关系

### 2.1 irq_desc —— 一切的核心

每个 Linux IRQ 号对应一个 `irq_desc`，它是中断子系统的中心枢纽：

```
irq_desc
├─ irq_data                    ← 硬件层抽象
│   ├─ irq: Linux IRQ 号
│   ├─ hwirq: 硬件中断号 (控制器内部编号)
│   ├─ chip: *irq_chip         ← 中断控制器操作
│   ├─ domain: *irq_domain      ← 所属 IRQ 域
│   ├─ parent_data: *irq_data   ← 父级控制器数据 (层级 domain)
│   └─ state_use_accessors      ← IRQD_* 硬件状态标志
├─ irq_common_data
│   ├─ affinity: cpumask        ← CPU 亲和性
│   └─ node: NUMA 节点
├─ action: *irqaction           ← 已注册的处理程序 (共享 IRQ 时为链表)
│   ├─ handler: 主处理函数 (hardirq 上下文)
│   ├─ thread_fn: 线程处理函数 (进程上下文)
│   ├─ thread: 内核线程 task_struct
│   ├─ flags: IRQF_* (IRQF_SHARED | IRQF_ONESHOT | ...)
│   └─ next: 链表下一节点
├─ handle_irq: 高层 flow handler 函数指针
│   (handle_level_irq / handle_edge_irq / handle_fasteoi_irq / handle_percpu_irq)
├─ kstat_irqs: per-CPU 中断计数
├─ istate: IRQS_* 内部状态机
└─ status_use_accessors: _IRQ_* 软件配置
```

### 2.2 irq_chip —— 中断控制器驱动抽象

```c
struct irq_chip {
    irq_startup     // 首次启用
    irq_shutdown    // 关闭
    irq_enable      // 使能
    irq_disable     // 禁用
    irq_ack         // 确认 (告知控制器已收到)
    irq_mask        // 屏蔽 (控制器不再转发)
    irq_unmask      // 取消屏蔽
    irq_eoi         // 中断结束 (GIC 使用, 替代 ack)
    irq_set_type    // 设置触发类型
    irq_set_affinity // 设置 CPU 亲和性
    flags           // IRQCHIP_* 能力标志
};
```

关键 flags：
- `IRQCHIP_EOI_THREADED`：EOI 在线程 handler 完成后才发送（实时性优化）
- `IRQCHIP_EOI_IF_HANDLED`：只有确实处理了中断才发 EOI
- `IRQCHIP_MOVE_DEFERRED`：亲和性迁移延迟到目标 CPU 执行

### 2.3 irq_domain —— 硬件中断号到 Linux 中断号的翻译层

```
[设备] → hwirq=3  →  irq_domain  →  Linux IRQ=42  →  irq_desc[42]
[设备] → hwirq=0  →  irq_domain  →  Linux IRQ=100 →  irq_desc[100]
                        (另一个控制器)
```

三种映射方式：

| 类型 | 查找复杂度 | 适用场景 | 实现 |
|------|-----------|---------|------|
| linear | O(1) | 连续编号 (GIC) | `irq = hwirq + revmap_base` |
| tree | O(log n) | 稀疏编号 (MSI) | radix tree / maple tree |
| direct | O(1) | 已知范围 | 全范围数组 |

**层级 domain**（现代 ARM64 SoC 的标准架构）：

```
[设备] → [GPIO 控制器 domain] → [二级中断控制器 domain] → [GIC domain] → CPU
          parent ↑                        parent ↑              root domain
```

分配流程从根 domain 开始逐层向下 `alloc()` + `activate()`。

---

## 3. 中断处理流程：五种 Flow Handler 详解

flow handler 是 `desc->handle_irq` 指向的函数，负责处理中断控制器的硬件协议。不同的硬件需要不同的处理序列。

### 3.1 handle_simple_irq —— 最简单

```
锁 desc->lock → 计数+1 → handle_irq_event → 解锁返回
```

适用于：硬件已经完成了 ack/mask/eoi，不需要软件干预的场景（如 chained interrupt 的内部分发）。

### 3.2 handle_level_irq —— 电平触发

```
锁 desc->lock
  ├─ mask + ack         ← 先屏蔽，防止电平一直保持导致中断风暴
  ├─ 计数+1
  ├─ handle_irq_event   ← 调用驱动 handler
  └─ cond_unmask         ← 条件 unmask (非 ONESHOT 或线程未唤醒时)
解锁返回
```

核心思路：**电平触发的中断只要线上电平保持就会持续触发，所以必须先 mask，等 handler 处理完设备（清除了中断源）后再 unmask。**

### 3.3 handle_edge_irq —— 边沿触发

```
锁 desc->lock
  ├─ 检查 IRQS_PENDING    ← 如果在处理期间收到了新的边沿
  │   ├─ 有 PENDING: ack, 重放处理
  │   └─ 无 PENDING: ack, mask (防止抖动)
  ├─ 计数+1
  ├─ handle_irq_event
  ├─ 检查 IRQS_PENDING (处理期间的新中断)
  └─ unmask (如果 IRQS_PENDING 则继续保持 unmask 等下一个)
解锁返回
```

核心思路：**边沿触发的中断可能丢失——如果在 handler 处理期间又来了一次边沿，控制器可能不产生新中断。因此需要在 handler 返回后检查是否有遗漏，并通过 IRQS_PENDING 机制重发。**

### 3.4 handle_fasteoi_irq —— EOI 型（ARM GIC 标准）

```
锁 desc->lock
  ├─ IRQS_ONESHOT? → mask      ← ONESHOT 模式下先屏蔽，等线程完成再开
  ├─ 计数+1
  ├─ handle_irq_event
  └─ cond_unmask_eoi:
       ├─ 非 ONESHOT: eoi (通知控制器)
       ├─ ONESHOT + 未唤醒线程 + 未禁用: eoi + unmask
       └─ ONESHOT + 线程已唤醒: 只发 eoi, 保持 mask (等线程完)
解锁返回
```

核心思路：**GIC 等现代控制器使用 EOI 而非 ack/mask。EOI 告诉控制器"此中断处理完毕，可以发送下一个"。ONESHOT 模式下由线程 handler 负责最终的 unmask + eoi。**

### 3.5 handle_percpu_irq —— Per-CPU 中断

```
无锁 (per-CPU 数据天然隔离)
  ├─ 计数+1
  └─ handle_irq_event_percpu
```

适用于：PPI (Private Peripheral Interrupt) 和 SGI (Software Generated Interrupt)，每个 CPU 有独立的控制器寄存器。

---

## 4. 关键算法深度解析

### 4.1 延迟 disable（Lazy Disable）

```
disable_irq(irq)
  ├─ 设置 IRQD_IRQ_DISABLED 标志 (不立即操作硬件!)
  └─ 等待可能正在运行的 handler 完成 (synchronize_irq)

中断到达时:
  irq_disable()
    ├─ 如果 IRQD_IRQ_DISABLED 已置位
    │   └─ mask_irq() → 真正屏蔽硬件
    └─ 否则 → 正常处理
```

**为什么不立即 mask？**
- 对慢速总线（I2C/SPI）上的 GPIO 扩展器，一次寄存器写可能耗时数百微秒
- 在热路径上延迟 mask 可显著减少总线事务
- 只有在中断真正到来时（用户确实不想处理）才付出硬件访问开销

### 4.2 中断重发（IRQ Resend）

边沿中断丢失是经典问题。内核的处理：

```
handle_edge_irq 返回时:
  check_irq_resend(desc)
    ├─ desc->istate & IRQS_PENDING? → 是，需要重发
    ├─ 尝试立即重发 (如果是同 CPU)
    └─ 否则通过 tasklet 延迟重发 (irq_resend_softirq)
```

`IRQS_PENDING` 的设置时机：
- 在 handler 处理期间，如果再次收到同一个 IRQ
- arch code 设置 `desc->istate |= IRQS_PENDING`

### 4.3 伪中断检测（Spurious Detection）

```
note_interrupt(irq, desc, action_ret)
  ├─ action_ret == IRQ_NONE (没人认领)
  │   ├─ irqs_unhandled++
  │   ├─ time_before(jiffies, last_unhandled + HZ/100)?
  │   │   ├─ 是: 未处理中断来得太快
  │   │   │   └─ irqs_unhandled > 99900? → __report_bad_irq()
  │   │   │       └─ 设置 IRQS_SPURIOUS_DISABLED + 禁用中断线
  │   │   └─ 否: 重置计数器
  │   └─ last_unhandled = jiffies
  └─ action_ret == IRQ_HANDLED
      └─ 重置计数器 (中断正常)
```

检测阈值：在 10ms 内收到 100,000 次无人认领的中断。

### 4.4 中断线程化（Threaded IRQ）

```
request_threaded_irq(irq, handler, thread_fn, flags, ...)
  │
  ▼
__setup_irq()
  ├─ 创建内核线程 (若需要 thread_fn 或 force_irqthreads)
  ├─ 设置 action->thread_fn
  └─ 注册到 desc->action 链表

中断到达:
  handler(irq, dev_id)              ← hardirq 上下文, 关中断
    ├─ 做最小化的硬件操作 (ack/eoi)
    └─ 返回 IRQ_WAKE_THREAD
        └─ __irq_wake_thread()
            ├─ 设置 IRQTF_RUNTHREAD
            └─ wake_up_process(action->thread)
                │
                ▼
            thread_fn(irq, dev_id)  ← 进程上下文, 可睡眠
                ├─ 处理设备数据
                └─ 完成后 unmask_irq + eoi (ONESHOT 模式)
```

**为什么需要线程化？**
- 减少关中断时间（hardirq handler 只做最基本的 ack）
- thread_fn 可以睡眠（可以获取 mutex、做 I/O 等）
- 实时性：`force_irqthreads` 内核参数将所有中断强制线程化

---

## 5. 与内核启动过程的关系

在 `start_kernel()` 中，IRQ 子系统的初始化分为两步：

```
start_kernel()
  ├─ early_irq_init()         ← 第一步：初始化静态预分配的 irq_desc
  │   ├─ 初始化 irq_desc 的 spinlock、affinity mask
  │   ├─ 设置默认 flow handler (handle_bad_irq)
  │   └─ 分配 per-CPU 中断统计 (kstat_irqs)
  │
  ├─ init_IRQ()               ← 第二步：架构中断控制器初始化 (ARM64)
  │   └─ irqchip_init()
  │       └─ of_irq_init()    ← 从设备树匹配中断控制器驱动
  │           ├─ 为每个 irqchip 调用 init 回调
  │           ├─ 分配 irq_domain (linear / tree)
  │           ├─ 为每个硬件中断分配 Linux IRQ 号
  │           └─ 设置 desc->handle_irq (flow handler)
  │
  └─ local_irq_enable()       ← 第三步：开中断！
      (此后 request_irq() 才可正常工作)
```

**关键点**：
- `early_irq_init()` 只初始化了静态描述符的数据结构
- 真正的中断控制器配置在 `init_IRQ()` → `irqchip_init()` 中完成
- 在 `local_irq_enable()` 之前，任何到达的中断都会被硬件屏蔽（DAIF.I 置位）
- 开中断后，驱动通过 `request_irq()` 注册 handler，此后的中断就能被正常递送

---

## 6. 并发模型分析

IRQ 子系统的并发控制是逐层细化的：

### 第一层：IRQ 关闭（DAIF）

```
local_irq_disable()  →  设置 PSTATE.I 位, CPU 不再响应 IRQ
local_irq_enable()   →  清除 PSTATE.I 位
```

保证在同一个 CPU 上，中断处理不会被其他中断打断（ARM64 默认 IRQ 不嵌套）。

### 第二层：desc->lock（raw_spinlock）

每个 irq_desc 有独立的 spinlock，保证：
- 同一个 IRQ 不会被多个 CPU 同时处理
- flow handler 的 mask/ack/eoi 操作原子化

### 第三层：RCU + refcount

```
rcu_read_lock()
  irq_desc = irq_to_desc(irq)      ← 无锁查找 (RCU 保护)
  irq_lock_object(irq_desc)        ← 获取 spinlock
  ... 处理 ...
  irq_unlock_object(irq_desc)
rcu_read_unlock()
```

`irq_free_descs()` 通过 `call_rcu()` 延迟释放，保证所有 RCU 读者安全。

### 第四层：IRQS_INPROGRESS 状态机

```
handle_irq_event 入口:
  desc->istate |= IRQS_INPROGRESS

handle_irq_event 出口:
  desc->istate &= ~IRQS_INPROGRESS
```

用于同步：
- `synchronize_irq()` 等待 `IRQS_INPROGRESS` 被清除
- ONESHOT 模式的线程完成检测

---

## 7. ARM64 中断控制器的层级结构

典型的 ARM64 SoC 中断系统：

```
Peripherals (UART, MMC, Ethernet...)
    │
    ▼
[GPIO expander] ── parent ──→ [SOC-internal GIC extension]
    │ irq_domain                    │ irq_domain
    │ (gpiochip)                    │ (二级)
    ▼                               ▼
[GIC Distributor] ← 分发中断到各 CPU
    │
    ├─ PPI (Private): 每 CPU 私有 (timer, PMU)
    ├─ SPI (Shared):  所有 CPU 共享
    └─ SGI (Software): IPI
    │
    ▼
[GIC CPU Interface] ← 每 CPU 一个
    │
    ▼
CPU core (IRQ / FIQ exception)
```

对应的 Linux IRQ 域层次：
```
gic_ipi_domain (SGI)
gic_irq_domain (PPI + SPI root domain)
  └─ its_domain (MSI for PCI)
  └─ gpio_domain (GPIO expander)
  └─ other domains...
```

---

## 8. 关键 API 使用场景

### 驱动侧

```c
// 注册中断 (最常用)
request_irq(irq, handler, IRQF_SHARED, "mydev", dev);
request_threaded_irq(irq, hard_handler, thread_fn, IRQF_ONESHOT, "mydev", dev);

// 注销 (等待可能正在运行的 handler 完成)
free_irq(irq, dev);

// 禁用/使能
disable_irq(irq);        // 等待 handler 完成
disable_irq_nosync(irq); // 不等待, 立即返回
enable_irq(irq);

// 亲和性
irq_set_affinity(irq, cpumask_of(cpu));
```

### 中断控制器驱动侧

```c
// 创建 domain
domain = irq_domain_add_linear(np, nr_irqs, &ops, NULL);

// 分配 IRQ
virq = irq_create_mapping(domain, hwirq);

// 设置 flow handler
irq_set_handler(virq, handle_fasteoi_irq);

// 设置 irq_chip
irq_set_chip(virq, &my_chip);
```

### 层级 domain

```c
// 创建子 domain (挂到父 domain)
child = irq_domain_add_hierarchy(parent, 0, nr_irqs, np, &ops, NULL);

// 分配时从根 domain 逐层 alloc
virq = __irq_domain_alloc_irqs(parent, -1, 1, NUMA_NO_NODE, NULL, false, NULL);
```

---

## 9. 调试与诊断

### 关键文件

| 文件 | 内容 |
|------|------|
| `/proc/interrupts` | 每 CPU 的 IRQ 计数 + 触发类型 + 关联设备名 |
| `/proc/irq/N/smp_affinity` | IRQ N 的 CPU 亲和性位图 |
| `/proc/irq/N/spurious` | IRQ N 的伪中断计数 |
| `/proc/softirqs` | 软中断统计 |

### 启动参数

| 参数 | 效果 |
|------|------|
| `noirqdebug` | 禁用伪中断检测 |
| `irqaffinity=...` | 设置默认 IRQ 亲和性 |
| `irqhandler.duration_warn_us=N` | 超过 N 微秒的 handler 打印警告 |
| `threadirqs` / `irqthreads` | 强制线程化所有中断 |

### 常见问题定位

**中断风暴**：`/proc/interrupts` 中某个 IRQ 的计数异常高。
- 检查触发类型设置是否正确（边沿 vs 电平配错）
- 检查是否缺少 ack/eoi

**中断丢失**：设备事件不触发 handler。
- 检查 IRQ 是否被 `disable_irq()` 或 lazy disable 屏蔽
- 检查边沿中断的 pending 重发机制

**高延迟**：`irqhandler.duration_warn_us=100` 可追踪耗时过长的 handler。

---

## 10. 源码阅读顺序建议

1. **数据结构**：
   - `include/linux/irqdesc.h` — `struct irq_desc`
   - `include/linux/irq.h` — `struct irq_chip`, `struct irq_data`
   - `include/linux/irqdomain.h` — `struct irq_domain`
   - `kernel/irq/internals.h` — 状态标志 (IRQD_* / IRQS_* / IRQTF_*)

2. **启动路径**：
   - `kernel/irq/irqdesc.c` — `early_irq_init()`
   - `drivers/irqchip/irq-gic.c` — GIC irqchip 驱动（ARM64 标准）

3. **中断处理热路径**：
   - `kernel/irq/handle.c` — `generic_handle_irq()` → `__handle_irq_event_percpu()`
   - `kernel/irq/chip.c` — `handle_fasteoi_irq()` 等 flow handler

4. **中断管理**：
   - `kernel/irq/manage.c` — `request_irq()` / `free_irq()`
   - `kernel/irq/spurious.c` — 伪中断检测
   - `kernel/irq/resend.c` — 中断重发

5. **进阶**：
   - `kernel/irq/irqdomain.c` — domain 映射与层级分配
   - `kernel/irq/msi.c` — MSI 中断
   - `kernel/irq/affinity.c` — 多队列亲和性计算

---

## 11. 总结

Linux IRQ 中断子系统的核心设计思想可以归纳为几个关键权衡：

**抽象与效率**：通过 `irq_chip` + `irq_domain` + `irq_desc` 三层抽象，既隔离了硬件差异，又在热路径上保持了极低的查找开销（RCU 无锁 + per-IRQ spinlock）。

**延迟与吞吐**：线程化中断和 ONESHOT 机制让长时间处理不阻塞其他中断；lazy disable 优化了慢速总线控制器的操作开销。

**正确性与并发**：通过逐层细化的锁机制（DAIF → desc->lock → IRQS_INPROGRESS），在 SMP 环境下保证中断处理既不丢失也不重复。

**可靠性**：伪中断检测、中断重发、轮询恢复等机制确保了中断子系统的健壮性，即使面对有噪音的硬件也不会导致系统崩溃。

理解这套框架后，无论是调试中断风暴、分析驱动 probe 时序问题，还是为新的中断控制器编写驱动，都能快速定位到正确的层次。
