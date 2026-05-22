# Linux Kernel 启动与初始化过程分析

## 概述

Linux kernel 的启动和初始化，可以按“从硬件上电，到用户态 `init` 进程启动”为主线理解。

内核启动本质上做 6 件事：

1. 建立最小运行环境
2. 解压或搬运内核镜像
3. 初始化内存、页表、中断、时钟、调度器
4. 初始化设备模型和驱动框架
5. 挂载根文件系统
6. 启动第一个用户态进程

这条链路的核心目标，是把一个“刚从引导器跳进来的内核镜像”，变成一个“具备完整调度、内存管理、设备管理和用户态入口的操作系统”。

---

## 1. 架构入口：从汇编到 C

kernel 启动并不是从 `start_kernel()` 直接开始的，前面总有一段架构相关的 early boot 代码。

以 ARM64 为例，典型入口包括：

- `arch/arm64/kernel/head.S`
- 然后转入通用 C 入口 `start_kernel()`

这段代码主要负责：

- 设置异常级和 CPU 初始状态
- 建立最小页表
- 准备 MMU 打开前后的地址切换
- 完成镜像重定位或解压前置准备
- 切换到 C 运行环境

这一阶段的特点是：

- 还没有完整内存管理
- 还没有调度器
- 还没有驱动模型
- 只能做极少量、严格受限的初始化

不同架构这部分实现不同，例如：

- x86 使用自己的 `head_64.S`
- ARM32、RISC-V 也有各自 early boot 路径

---

## 2. 进入 `start_kernel()`：通用启动主入口

通用初始化主入口定义在：

- `init/main.c`

关键函数：

- `start_kernel()`

它是整个内核初始化的中枢，负责把系统从“早期引导态”推进到“完整内核运行态”。

这里大致会完成：

- 解析早期命令行参数
- 初始化异常和中断基础设施
- 初始化早期内存管理
- 初始化 slab/slub 分配器
- 初始化调度器
- 初始化 timekeeping、tick、timer
- 初始化 workqueue、RCU、softirq
- 初始化 printk 和控制台基础设施
- 最终进入 `rest_init()`

可以把这一步理解为：

- 把“只能执行少量早期代码的系统”，升级成“能够稳定运行完整内核代码的系统”

---

## 3. 内存初始化：最早、最关键的基础设施之一

内核启动最早必须解决的问题之一，就是“如何安全地分配和管理内存”。

关键组成包括：

- memblock 早期内存分配
- 页表建立和完善
- buddy allocator 初始化
- vmalloc 区初始化
- slab/slub 初始化

相关代码主要分布在：

- `mm/`
- `mm/page_alloc.c`
- `mm/slab_common.c`
- `mm/slub.c`

以及架构层的：

- `setup_arch()`

### 为什么 memblock 很重要

在 buddy 和 slab 还没有准备好之前，内核仍然需要分配少量内存。

这时依赖的是：

- `memblock`

它是内核早期的临时内存分配机制。

所以很多早期初始化路径的模式都是：

1. 先用 memblock 分配早期对象
2. 等正式页分配器和 slab 可用后
3. 再切换到常规内存管理机制

---

## 4. `setup_arch()`：架构层接管平台信息

在 `start_kernel()` 的早期阶段，会调用：

- `setup_arch(&command_line)`

这是架构层最重要的启动钩子之一。

它通常负责：

- 解析设备树或 ACPI
- 初始化内存布局
- 处理保留内存
- 准备 bootargs
- 配置 CPU 拓扑
- 建立 early fixmap / early ioremap
- 初始化部分中断控制器相关前置状态

在嵌入式平台上，这一步尤其关键，因为很多板级关键信息都在这里进入通用内核：

- 设备树
- reserved-memory
- CMA
- initrd 地址
- 外设地址空间
- 平台启动参数

---

## 5. 中断、时间、调度器、RCU：建立运行时骨架

当内存基础设施初步建立后，内核还需要建立完整的运行时机制。

主要包括：

- `trap_init()`
- `init_IRQ()`
- `tick_init()`
- `timekeeping_init()`
- `sched_init()`
- `rcu_init()`
- `softirq_init()`
- `workqueue_init_early()`

这些子系统的意义是：

- 让 CPU 可以处理中断和异常
- 让任务调度器可运行
- 让时间和定时机制可用
- 让软中断、延迟工作、RCU 等基础机制成立

只有这些基础设施具备后，后续更复杂的驱动初始化和设备枚举才能可靠执行。

---

## 6. `initcall` 机制：按阶段拉起子系统和驱动

内核不会在一个函数里手工写死所有初始化顺序，而是依赖 `initcall` 机制按阶段推进。

常见阶段包括：

- `pure_initcall`
- `core_initcall`
- `postcore_initcall`
- `arch_initcall`
- `subsys_initcall`
- `fs_initcall`
- `device_initcall`
- `late_initcall`

这套机制的核心作用是：

- 让不同子系统按依赖关系分阶段起来
- 让编译进内核的驱动自动参与初始化
- 让总线、设备模型、文件系统、驱动按层推进

一个常见依赖顺序是：

1. 先初始化总线和核心框架
2. 再初始化设备模型
3. 再做设备发现和枚举
4. 最后 probe 具体驱动

理解 `initcall`，是理解 kernel 初始化过程的关键。

---

## 7. `rest_init()`：从单一路径进入正常内核运行态

`start_kernel()` 末尾会进入：

- `rest_init()`

这里是内核启动流程中的一个关键转折点。

它会：

- 创建 `kernel_init` 内核线程
- 创建 `kthreadd`
- 当前上下文转入 idle 线程

这意味着系统从“单一启动执行流”切换到“正常多任务内核运行模式”。

可以理解成：

- 启动主线从“引导脚本阶段”切入“正式运行阶段”

---

## 8. `kernel_init`：准备根文件系统和用户态

`kernel_init` 最终会执行到：

- `kernel_init_freeable()`

这部分主要负责：

- 推进后续初始化
- 完成剩余 initcall
- 准备 rootfs
- 打开 `/dev/console`
- 查找并执行第一个用户态程序

常见尝试顺序包括：

- `ramdisk_execute_command`
- `execute_command`
- `/sbin/init`
- `/etc/init`
- `/bin/init`
- `/bin/sh`

如果这些入口都失败，内核会 panic。

---

## 9. 根文件系统挂载

根文件系统的准备方式依赖具体系统设计，常见有三类：

### 9.1 initramfs

- kernel 自带 cpio
- 启动较早阶段即可解包使用

### 9.2 块设备根文件系统

- 例如 `ext4`
- 依赖 MMC、NVMe、SATA 等块设备驱动先初始化完成

### 9.3 网络根文件系统

- 例如 NFS root

相关代码主要在：

- `init/do_mounts*.c`

嵌入式系统中常见组合有：

- `initramfs`
- `ext4 rootfs`
- `squashfs + overlay`
- `ubifs rootfs`

---

## 10. 用户态 `init` 启动：启动主线完成

当 `/sbin/init` 或其他 init 进程成功 `exec` 后：

- kernel 启动和初始化主线就基本完成
- 系统进入常规运行状态
- 后续的服务启动、设备管理、守护进程等逻辑由用户态接管

这一步标志着：

- Linux kernel 已从“引导内核”变成“运行中的完整系统核心”

---

## 11. 推荐阅读顺序

如果要从源码角度真正读懂启动过程，建议按下面顺序看：

1. 架构入口
   - `arch/<arch>/kernel/head*.S`
2. 通用入口
   - `init/main.c`
3. 架构初始化
   - `setup_arch()`
4. 内存初始化
   - `mm/`
5. 中断、调度、时间
   - `kernel/`
6. `initcall` 执行路径
7. rootfs 挂载
   - `init/do_mounts*.c`
8. `kernel_init` 和用户态入口

---

## 12. 总结

Linux kernel 启动过程的本质，是一条按层推进的初始化链路：

1. 架构早期引导
2. 通用核心子系统初始化
3. 通过 `initcall` 分阶段拉起各类框架和驱动
4. 挂载根文件系统
5. 启动第一个用户态进程

从工程视角看，真正重要的不是死记顺序，而是理解每一步在解决什么问题：

- 谁提供最早内存
- 谁接收平台信息
- 谁建立运行时骨架
- 谁负责设备初始化排序
- 谁最终把控制权交给用户态

只有把这些职责边界看清楚，后续分析：

- early boot 卡死
- 驱动 probe 时序问题
- rootfs 挂载失败
- init 进程启动失败

这些问题时，才不会陷入“只知道函数名，不知道它在整个启动链路中的位置”的困境。

---

## 13. `start_kernel()` 逐阶段详解

如果要真正跟源码主线，最值得深读的函数就是：

- `init/main.c:start_kernel()`

它不是简单地“调用很多初始化函数”，而是按依赖关系一层层把系统搭起来。

下面按源码中的执行顺序，把它拆成几个阶段。

### 13.1 最早期防护与 CPU 身份建立

开头最早几步包括：

- `set_task_stack_end_magic(&init_task)`
- `smp_setup_processor_id()`
- `debug_objects_early_init()`
- `init_vmlinux_build_id()`
- `cgroup_init_early()`

这几步主要做：

- 给 `init_task` 的内核栈布置越界检测魔数
- 确认当前 boot CPU 的逻辑身份
- 建立 debug objects 的最早期支持
- 记录内核镜像 build-id
- 初始化 cgroup 的超早期骨架

这时系统还没有正式进入“完整内核态”，但已经开始为后续调试和一致性检查打基础。

### 13.2 关中断并进入架构初始化前置阶段

接着会执行：

- `local_irq_disable()`
- `early_boot_irqs_disabled = true`
- `boot_cpu_init()`
- `page_address_init()`
- `pr_notice("%s", linux_banner)`
- `setup_arch(&command_line)`

这一段的关键点是：

- 在真正建立完异常/中断体系前，必须先关闭本地中断
- `boot_cpu_init()` 完成 boot CPU 的最初登记
- `page_address_init()` 准备页地址辅助结构
- `setup_arch()` 把架构和平台信息正式引入通用内核

`setup_arch()` 是启动路径里最重要的架构钩子之一，通常会完成：

- 设备树或 ACPI 解析
- 内存布局建立
- 保留内存处理
- 命令行获取
- 早期页表和平台资源准备

### 13.3 命令行、CPU 编号和 per-cpu 区域准备

紧接着会进入一段“平台信息转运行时参数”的处理：

- `mm_core_init_early()`
- `jump_label_init()`
- `static_call_init()`
- `early_security_init()`
- `setup_boot_config()`
- `setup_command_line(command_line)`
- `setup_nr_cpu_ids()`
- `setup_per_cpu_areas()`
- `smp_prepare_boot_cpu()`
- `early_numa_node_init()`
- `boot_cpu_hotplug_init()`

这里的作用是：

- 把早期内存管理再往前推进一步
- 让 static key / static call 可用
- 初始化最早期安全框架
- 接管 bootconfig 和内核命令行
- 确定系统支持的 CPU 数量
- 初始化 per-cpu 内存区
- 建立 boot CPU 在 SMP 环境里的初始状态
- 初步处理 NUMA 和 CPU hotplug 基础状态

这一步之后，内核已经不再只是“单 CPU 的引导上下文”，而开始为多 CPU、per-cpu 数据和 NUMA 运行模式做准备。

### 13.4 解析启动参数

然后内核会正式打印并解析命令行：

- `print_kernel_cmdline(saved_command_line)`
- `parse_early_param()`
- `parse_args("Booting kernel", ...)`
- `print_unknown_bootoptions()`
- `parse_args("Setting init args", ...)`
- `parse_args("Setting extra init args", ...)`

这里需要区分三类参数：

1. **early param**
   - 影响极早期行为，必须在很多子系统起来之前解析
2. **普通 kernel 参数**
   - 交给内核子系统消费
3. **传给用户态 init 的参数**
   - 最终进入 `argv_init[]` / `envp_init[]`

源码里还有一个容易忽略但很实用的机制：

- 未被内核识别的参数，不一定直接报错
- 某些未知参数会被保留并传给用户态

这对调试 `init=`、`rdinit=` 或早期用户态参数传递很重要。

### 13.5 随机数、日志缓冲区、异常表、最早 VFS 缓存

在正式内存管理和异常系统完全建立前，还会做一批“必须尽早完成”的基础设施初始化：

- `random_init_early(command_line)`
- `setup_log_buf(0)`
- `vfs_caches_init_early()`
- `sort_main_extable()`
- `trap_init()`

它们分别解决：

- 早期随机数初始化
- `printk` 环形日志缓冲区建立
- VFS 早期缓存初始化
- 异常表排序
- 陷阱和异常处理框架建立

其中 `sort_main_extable()` 的意义在于：

- 后续异常修复路径通常依赖异常表快速查找
- 启动时排序好，运行时查找才高效

### 13.6 正式内存管理、追踪基础设施和调度器上线

再往后，会进入真正“内核运行骨架”建立阶段：

- `mm_core_init()`
- `maple_tree_init()`
- `poking_init()`
- `ftrace_init()`
- `early_trace_init()`
- `sched_init()`

这一段的意义非常大：

- 正式内存管理继续完善
- Maple Tree 基础设施可用
- 文本 patching / poking 机制初始化
- ftrace 初始化
- 调度器初始化

`sched_init()` 是一个关键分界点：

- 到这里，系统已经具备“可运行任务调度”的核心框架
- 虽然 SMP 和完整拓扑还没完全起来
- 但单核或早期上下文已经能使用功能完整的调度器

### 13.7 workqueue、RCU、trace、IRQ 基础设施

接着会继续把运行时核心框架补齐：

- `radix_tree_init()`
- `housekeeping_init()`
- `workqueue_init_early()`
- `rcu_init()`
- `kvfree_rcu_init()`
- `trace_init()`
- `context_tracking_init()`
- `early_irq_init()`
- `init_IRQ()`

这一层的职责是：

- 初始化老的 radix tree 相关基础设施
- 确定 housekeeping CPU 语义
- 提前允许 workqueue 建立和排队
- 初始化 RCU
- 初始化 tracepoints
- 建立上下文跟踪
- 建立 IRQ 域和中断控制器框架

此时虽然中断总体还没放开，但中断子系统已经开始具备完整运行条件。

### 13.8 定时器、软中断、时钟和随机数完整初始化

然后是时间和中断运行时基础：

- `tick_init()`
- `rcu_init_nohz()`
- `timers_init()`
- `srcu_init()`
- `hrtimers_init()`
- `softirq_init()`
- `vdso_setup_data_pages()`
- `timekeeping_init()`
- `time_init()`
- `random_init()`

这一阶段完成后：

- tick/timer/hrtimer 全部具备运行能力
- softirq 可用
- timekeeping 正式上线
- vDSO 时间页准备好
- 完整随机数初始化完成

这一步是很多驱动和调试设施真正可依赖“时间”的开始。

### 13.9 允许开中断前的最后准备

在真正开中断之前，还有一些重要初始化：

- `kfence_init()`
- `boot_init_stack_canary()`
- `perf_event_init()`
- `profile_init()`
- `call_function_init()`

主要目的：

- 准备内存错误检测和栈保护
- 建立性能事件基础设施
- 初始化跨 CPU 调用框架

之后内核会显式检查中断状态：

- 如果发现中断被异常提前打开，会发警告并重新关闭

这说明 `start_kernel()` 对“何时允许中断真正上线”控制得非常严格。

### 13.10 开中断与控制台初始化

再往后就是一个非常关键的分界：

- `early_boot_irqs_disabled = false`
- `local_irq_enable()`

这标志着：

- 早期“全局关中断”阶段结束
- 内核正式进入可以处理中断的常规运行阶段

紧接着又会做：

- `kmem_cache_init_late()`
- `console_init()`
- `lockdep_init()`
- `locking_selftest()`

这里的作用是：

- 完成 slab 相关晚期初始化
- 把控制台真正拉起来
- 初始化 lockdep
- 做锁依赖自检

`console_init()` 很重要，因为从这一步起：

- 大量早期失败将更容易在控制台上被直接看到

### 13.11 后半段核心子系统初始化

在中断和控制台可用后，`start_kernel()` 继续推进一大批常规核心子系统：

- `setup_per_cpu_pageset()`
- `numa_policy_init()`
- `acpi_early_init()`
- `late_time_init()`
- `sched_clock_init()`
- `calibrate_delay()`
- `arch_cpu_finalize_init()`
- `pid_idr_init()`
- `anon_vma_init()`
- `thread_stack_cache_init()`
- `cred_init()`
- `fork_init()`
- `proc_caches_init()`
- `uts_ns_init()`
- `time_ns_init()`
- `key_init()`
- `security_init()`
- `dbg_late_init()`
- `net_ns_init()`
- `vfs_caches_init()`
- `pagecache_init()`
- `signals_init()`
- `seq_file_init()`
- `proc_root_init()`
- `nsfs_init()`
- `pidfs_init()`
- `cpuset_init()`
- `mem_cgroup_init()`
- `cgroup_init()`
- `taskstats_init_early()`
- `delayacct_init()`
- `acpi_subsystem_init()`
- `arch_post_acpi_subsys_init()`
- `kcsan_init()`

这一段可以理解成：

- 把“操作系统骨架”补成“能支撑驱动、进程、文件系统、命名空间和安全框架”的完整核心

此时虽然用户态还没起来，但内核已经非常接近正常运行状态。

### 13.12 `rest_init()`：`start_kernel()` 的终点

`start_kernel()` 的最后一步是：

- `rest_init()`

它意味着：

- 早期单线程初始化主线结束
- 系统开始切换到常规内核线程世界
- 后续由 `kernel_init`、`kthreadd` 和 idle 线程接管

所以从阅读源码角度看：

- `start_kernel()` 是“建立世界”
- `rest_init()` 是“把世界交给正常调度”

---

## 14. `kernel_init_freeable()`：从“内核基本可运行”到“rootfs 就绪”

如果 `start_kernel()` 是内核早期初始化主线，那么：

- `kernel_init_freeable()`

就是“从内核内部世界，迈向用户态世界”的关键桥梁。

源码里它大致做这些事：

- 允许正常阻塞分配：
  - `gfp_allowed_mask = __GFP_BITS_MASK`
- 允许 `init` 在任意内存节点分配页：
  - `set_mems_allowed(node_states[N_MEMORY])`
- 准备 CPU 和 SMP 相关后续初始化：
  - `smp_prepare_cpus()`
  - `smp_init()`
  - `sched_init_smp()`
- 初始化完整 workqueue：
  - `workqueue_init()`
  - `workqueue_init_topology()`
- 推进晚于 `start_kernel()` 的异步和并行框架：
  - `async_init()`
  - `padata_init()`
- 完成页分配晚期初始化：
  - `page_alloc_init_late()`
- 进入真正的设备/驱动初始化阶段：
  - `do_basic_setup()`
- 执行 KUnit：
  - `kunit_run_all_tests()`
- 等待 initramfs：
  - `wait_for_initramfs()`
- 打开 `/dev/console`：
  - `console_on_rootfs()`
- 如果 `rdinit=` 不可用，则进入：
  - `prepare_namespace()`

### 14.1 `do_basic_setup()` 的真实意义

`do_basic_setup()` 内部会做：

- `cpuset_init_smp()`
- `ksysfs_init()`
- `driver_init()`
- `init_irq_proc()`
- `do_ctors()`
- `do_initcalls()`

这里真正关键的是：

- `driver_init()`：设备模型和驱动核心彻底上线
- `do_initcalls()`：按级别执行 built-in 子系统和驱动初始化

这意味着：

- 前面 `start_kernel()` 建的是“运行框架”
- 到了 `do_basic_setup()`，系统才真正开始“把驱动和设备拉起来”

### 14.2 `prepare_namespace()`：根文件系统接入点

如果没有可用的 `rdinit=` 提前用户态，内核就会进入：

- `prepare_namespace()`

它负责：

- 挂载根文件系统
- 准备真实 rootfs
- 为最终执行 `/sbin/init` 铺路

所以这一步是：

- “内核初始化完成”
  到
- “用户态入口准备好”

之间最关键的桥。

---

## 15. `kernel_init()`：释放 init 内存并启动用户态

`kernel_init()` 做的是最后收尾：

- 等待 `kthreadd` 就绪
- 调 `kernel_init_freeable()`
- 等待所有异步 init 工作完成
- 释放 `__init` 段内存
- 把 rodata 改成只读
- 完成 PTI、NUMA 默认策略、boot sysctl 参数等收尾
- 最终尝试执行：
  - `ramdisk_execute_command`
  - `execute_command`
  - `CONFIG_DEFAULT_INIT`
  - `/sbin/init`
  - `/etc/init`
  - `/bin/init`
  - `/bin/sh`

这一阶段可以概括成：

- 清理启动期专用资源
- 固化内核内存权限
- 把控制权交给第一个用户态进程

如果所有 init 入口都失败，内核会 panic：

- `No working init found`

这也是分析“系统启动到了哪一步”的一个重要边界：

- 如果 panic 出现在这里，说明内核本身已经基本初始化完成
- 问题通常落在 rootfs、init 程序或启动参数

---

## 16. 用 `start_kernel()` 主线分析启动问题

从调试角度，可以把启动问题粗分成以下层次：

### 16.1 到不了 `start_kernel()`

通常说明问题在：

- 引导器
- 镜像加载
- 架构 early boot 汇编
- 页表/MMU 切换

### 16.2 卡在 `start_kernel()` 前半段

通常优先怀疑：

- `setup_arch()`
- 内存布局
- 设备树/ACPI
- early param
- 页分配或异常表初始化

### 16.3 卡在中断或时钟初始化附近

优先看：

- `init_IRQ()`
- `timekeeping_init()`
- `tick_init()`
- 时钟源/中断控制器驱动

### 16.4 卡在 `do_initcalls()`

优先看：

- built-in 驱动初始化
- `initcall_debug`
- 某个总线/驱动 probe
- 死锁或中断依赖

### 16.5 卡在 `prepare_namespace()` 或 `run_init_process()`

优先看：

- rootfs 挂载
- `initramfs`
- `rdinit=` / `init=` 参数
- `/sbin/init` 是否存在且可执行

这套分层方法，比死记“第几个函数”更实用。
