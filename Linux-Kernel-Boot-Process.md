# Linux Kernel 启动过程分析（ARM/ARM64）

## 整体流程概览

```
汇编入口 → setup_arch() → start_kernel() → rest_init() → kernel_init() → 用户态 init
```

---

## 阶段 1：架构特定汇编入口

### ARM (32-bit) — `arch/arm/kernel/head.S:93` 入口 `stext`

- 安装 hypervisor stub（如需要）
- 安全模式设置，关闭中断
- `__lookup_processor_type` 查找处理器类型
- `__vet_atags` 验证 FDT/ATAGS
- `__create_page_tables` 创建初始页表（恒等映射 + 内核映射）
- `__enable_mmu` → `__turn_mmu_on` 开启 MMU
- `__mmap_switched` → 清 BSS、跳转 `start_kernel()`

### ARM64 (64-bit) — `arch/arm64/kernel/head.S:85` 入口 `primary_entry`

- `preserve_boot_args` 保存 FDT 指针
- `__pi_create_init_idmap` 创建初始恒等映射
- `init_kernel_el` 确定 EL 级别并初始化
- `__cpu_setup` 处理器初始化（TCR/MAIR 等）
- `__primary_switch` → `__pi_early_map_kernel` 映射内核 + FDT
- `__primary_switched` → 设置 SP、异常向量表 → 跳转 `start_kernel()`

---

## 阶段 2：`start_kernel()` — `init/main.c:1017`

架构无关的 C 入口，严格按序初始化：

| 步骤 | 函数 | 作用 |
|------|------|------|
| 1 | `setup_arch(&command_line)` | 架构设置 |
| 2 | `mm_core_init_early()` | 早期内存（HugeTLB CMA、free_area_init） |
| 3 | `setup_per_cpu_areas()` | per-CPU 内存分配 |
| 4 | `parse_args()` | 解析内核命令行 |
| 5 | `trap_init()` | 异常/陷阱处理初始化 |
| 6 | `mm_core_init()` | **核心内存分配器**（memblock_free_all → buddy，SLUB） |
| 7 | `sched_init()` | 调度器初始化 |
| 8 | `rcu_init()` | RCU 初始化 |
| 9 | `init_IRQ()` | 中断子系统 |
| 10 | `console_init()` | **控制台可用（首个可见输出）** |
| 11 | `rest_init()` | 创建 init/kthreadd，进入 idle |

---

## 阶段 3：`setup_arch()` 架构初始化

### ARM — `arch/arm/kernel/setup.c:1096`

- `setup_processor()` — 读 CPU ID，查 `proc_info` 表，初始化处理器函数向量
- `setup_machine_fdt()` — 解析 FDT 匹配 `machine_desc`
- `arm_memblock_init()` — 保留内核/initrd/FDT 内存区域
- `paging_init()` → `map_lowmem()` + `map_kernel()` + `devicemaps_init()`
- `unflatten_device_tree()` — 将 FDT 展开为设备树结构
- `psci_dt_init()` + `smp_init_cpus()`

### ARM64 — `arch/arm64/kernel/setup.c:281`

- `setup_machine_fdt()` — 解析 FDT
- `arm64_memblock_init()` — 保留内核/initrd/EFI/FDT 内存
- `paging_init()` → `map_mem()` + `create_idmap()`
- `unflatten_device_tree()` / ACPI 初始化
- `bootmem_init()` — memblock 最终化
- `psci_dt_init()` + `smp_init_cpus()`

---

## 阶段 4：`mm_core_init()` 和 `sched_init()`

### 内存 — `mm/mm_init.c:2696`

```
build_all_zonelists() → memblock_free_all() → buddy 分配器接管
→ kmem_cache_init() → vmalloc_init()
```

### 调度器 — `kernel/sched/core.c:8874`

```
每个 CPU 初始化 rq 运行队列（CFS/RT/DL 子队列）
→ 初始化 root_task_group
```

---

## 阶段 5：`rest_init()` → `init/main.c:716`

```
rcu_scheduler_starting()
→ kernel_thread(kernel_init)  → PID 1（init 进程）
→ kernel_thread(kthreadd)     → PID 2（内核线程守护者）
→ cpu_startup_entry()         → boot CPU 成为 idle 线程（PID 0）
```

---

## 阶段 6：`kernel_init()` → 用户态过渡 — `init/main.c:1584`

1. `kernel_init_freeable()`：
   - `smp_init()` **启动从核**
   - `do_basic_setup()` → `do_initcalls()` **按顺序执行所有 initcall**（8 个级别：pure→core→postcore→arch→subsys→fs→device→late）
   - `prepare_namespace()` **挂载根文件系统**
2. `free_initmem()` — **释放 `__init` 段内存**
3. 按序尝试启动 init 进程：
   ```
   ramdisk 的 /init → init= 参数 → CONFIG_DEFAULT_INIT
   → /sbin/init → /etc/init → /bin/init → /bin/sh
   ```
   通过 `kernel_execve()` 将内核线程替换为用户态进程
4. 全部失败 → panic: "No working init found"

---

## 关键文件索引

| 组件 | 路径 |
|------|------|
| ARM 入口 | `arch/arm/kernel/head.S` |
| ARM64 入口 | `arch/arm64/kernel/head.S` |
| ARM setup_arch | `arch/arm/kernel/setup.c:1096` |
| ARM64 setup_arch | `arch/arm64/kernel/setup.c:281` |
| ARM64 early_map_kernel | `arch/arm64/kernel/pi/map_kernel.c:241` |
| ARM paging_init | `arch/arm/mm/mmu.c:1748` |
| ARM64 paging_init | `arch/arm64/mm/mmu.c:1420` |
| start_kernel | `init/main.c:1017` |
| rest_init | `init/main.c:716` |
| kernel_init | `init/main.c:1584` |
| mm_core_init | `mm/mm_init.c:2696` |
| sched_init | `kernel/sched/core.c:8874` |
