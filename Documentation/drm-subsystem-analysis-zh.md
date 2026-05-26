# Linux Kernel DRM 显示子系统框架分析

## 概述

DRM (Direct Rendering Manager) 是 Linux 内核的现代图形/显示基础设施，同时管理 GPU 渲染和显示输出。它取代了旧的 fbdev 框架，提供了：

1. **KMS (Kernel Mode Setting)**：显示管线的配置与管理
2. **Atomic Modesetting**：基于事务的显示状态更新（all-or-nothing 语义）
3. **GEM (Graphics Execution Manager)**：GPU 内存和缓冲区管理
4. **PRIME**：跨设备的缓冲区共享（GPU-显示控制器 DMA-BUF）

DRM 核心约 83,000 行代码，加上各 GPU 驱动（i915/amdgpu/nouveau/...）总计数百万行。

---

## 1. 整体架构

```
用户态 (User Space)
    │
    ├─ /dev/dri/card0    ← 主节点 (primary): OpenGL/Vulkan, modesetting
    ├─ /dev/dri/renderD128 ← 渲染节点 (render): 无显示输出, 仅 GPU 计算
    └─ /dev/accel/accel0 ← 加速器节点 (accel): 独立 AI/计算加速器 (非 GPU)
         │
    ┌─────┼─────┐
    │  DRM Core  │
    │  (ioctl 分发)  │
    ├─────────────┤
    │  KMS 子系统  │  ← 显示管线管理
    │  Atomic     │  ← 事务性状态更新
    │  GEM 子系统  │  ← GPU 内存管理
    └─────────────┘
         │
    ┌─────┴─────┐
    │ GPU 驱动   │
    │ (i915/     │
    │  amdgpu/   │
    │  nouveau/  │
    │  panfrost/ │
    │  ...)     │
    └─────────────┘
```

---

## 2. DRM 核心数据结构

### 2.1 drm_device — 顶层设备

```
drm_device
├─ driver: *drm_driver         ← 驱动回调 (open, ioctl, ...)
├─ primary/render/accel        ← drm_minor 节点
├─ mode_config                 ← KMS 全局配置
│   ├─ crtc_list               ← 所有 CRTC
│   ├─ connector_list          ← 所有 Connector
│   ├─ encoder_list            ← 所有 Encoder
│   ├─ plane_list              ← 所有 Plane
│   ├─ fb_list                 ← 所有 Framebuffer
│   └─ property_list           ← 所有 Property
├─ object_name_idr             ← DRM 对象 ID 分配器
├─ vblank[]                    ← 每 CRTC 的 VBlank 数据结构
└─ buf_alloc / vma_offset_manager ← GEM 全局管理
```

### 2.2 KMS 显示管线对象

显示管线的物理拓扑与 KMS 对象之间的映射：

```
[Framebuffer]                 ← GPU 渲染目标 (/ 显存中的像素数据)
    │
    ▼
[Plane]                       ← 图层 (主图层 + 叠加/光标图层)
    │  每个 CRTC 可以有多个 Plane
    ▼
[CRTC]                        ← 显示控制器 (扫描输出引擎)
    │  负责从 Framebuffer 读取像素, 按 timing 输出到 Encoder
    ▼
[Encoder]                     ← 编码器 (将像素流转换为物理信号协议)
    │
    ▼
[Bridge]                      ← 桥接芯片 (可选的中间转换, 如 LVDS→eDP)
    │  可通过链式串联
    ▼
[Connector]                   ← 连接器 (物理接口: HDMI / DP / eDP / VGA / DSI)
    │
    ▼
[Panel / Monitor]             ← 实际显示设备
```

### 2.3 drm_mode_config — KMS 全局配置

```c
struct drm_mode_config {
    // 能力约束
    int min_width, max_width;    // 最小/最大显示宽度
    int min_height, max_height;  // 最小/最大显示高度
    uint64_t cursor_width, cursor_height; // 光标图层大小限制

    // 对象列表 (被 driver init 时创建的对象填充)
    struct list_head crtc_list;      // 所有 CRTC
    struct list_head encoder_list;   // 所有 Encoder
    struct list_head connector_list; // 所有 Connector
    struct list_head plane_list;     // 所有 Plane

    // Atomic 函数指针 (驱动提供)
    funcs: *drm_mode_config_funcs
      ├─ atomic_check()   ← 校验状态转换是否合法
      └─ atomic_commit()  ← 提交状态到硬件
};
```

### 2.4 drm_gem_object — GEM 缓冲区

```c
struct drm_gem_object {
    size_t size;                          // 缓冲区大小
    struct drm_device *dev;               // 所属设备
    struct file *filp;                    // 底层 shmem file (CPU 访问路径)
    const struct drm_gem_object_funcs *funcs;
      ├─ get_sg_table()    ← 获取 scatter/gather 表 (用于 DMA)
      ├─ vmap() / vunmap() ← 映射/解映射到内核虚拟地址空间
      ├─ mmap()            ← 映射到用户空间
      ├─ export()          ← PRIME: 导出为 dma-buf fd
      └─ vm_ops            ← 缺页异常处理 (按需分配物理页)
    struct dma_buf *dma_buf;             // PRIME 导出时的 dma-buf
    struct drm_gpuvm *gpuvm;            // GPU 虚拟地址空间关联
};
```

**GEM 变体**：
- `drm_gem_shmem_object`：基于 shmem 的软件 GEM（适用于无 IOMMU 的简单 GPU）
- `drm_gem_dma_object`：基于 DMA API 的 GEM（保证设备可寻址）
- `drm_gem_ttm_object`：基于 TTM (Translation Table Manager) 的 GEM（支持显存管理）

---

## 3. Atomic Modesetting（原子模式设置）

Atomic 是 DRM KMS 的核心 API 设计范式，取代了旧的逐个属性设置的 ioctl。

### 3.1 核心概念

```
旧的 Legacy API:
  set_crtc() → 立即生效, 可能与正在运行的 vblank 不同步
  set_plane() → 可能导致中间状态闪烁
  set_connector() → 无法保证多个属性同时更新

新的 Atomic API:
  drmModeAtomicCommit()
    ├─ 用户态构建 drm_mode_atomic 结构 (包含所有要修改的属性)
    ├─ DRM_IOCTL_MODE_ATOMIC
    │   ├─ 收集所有对象的状态 (old state)
    │   ├─ 应用用户请求的属性更改 (new state)
    │   ├─ atomic_check() → 验证整个配置的合法性
    │   └─ atomic_commit() → 提交到硬件
    │       ├─ 异步: non-blocking commit (在 vblank 回调中完成)
    │       └─ 同步: blocking commit
    └─ 要么全部生效, 要么全部回滚 (all-or-nothing)
```

### 3.2 状态对象模型

```
每个 KMS 对象都有私有状态:
  drm_plane_state     — 图层的位置、尺寸、framebuffer、旋转、缩放...
  drm_crtc_state      — CRTC 的 mode、active、vblank 配置...
  drm_connector_state — Connector 的 CRTC 绑定、状态 (插拔)...
  drm_bridge_state    — Bridge 的输入/输出总线配置...

全局状态:
  drm_atomic_state
    ├─ plane_states[]      ← 每 plane 的 old/new state
    ├─ crtc_states[]       ← 每 CRTC 的 old/new state
    ├─ connector_states[]  ← 每 connector 的 old/new state
    └─ private_objs[]      ← 驱动的私有全局状态
```

### 3.3 atomic_check & atomic_commit 流程

```
drm_atomic_commit()
  │
  ├─ 第一阶段: atomic_check()
  │   ├─ drm_atomic_helper_check_modeset()
  │   │   ├─ 检测 CRTC mode 是否改变
  │   │   ├─ 计算需要的 Encoder/Bridge 链路
  │   │   └─ 检查 Connector 的 CRTC 分配是否合法
  │   │
  │   ├─ drm_atomic_helper_check_planes()
  │   │   ├─ 检查每 layer 的 framebuffer 格式 + 尺寸
  │   │   ├─ 验证 layer z-order (叠加顺序)
  │   │   └─ 硬件能力检查 (缩放、旋转、像素格式支持)
  │   │
  │   └─ 驱动私有 check (如 i915 的 watermark 计算)
  │
  └─ 第二阶段: atomic_commit()
       ├─ 非阻塞路径 (async):
       │   ├─ drm_atomic_helper_prepare_planes() — pin framebuffer
       │   ├─ drm_atomic_helper_swap_state() — old_state ↔ new_state 交换
       │   ├─ 调度 vblank 事件回调 (在下一个 vblank 时执行)
       │   └─ 返回用户态 (不等待硬件完成)
       │
       └─ 阻塞路径 (sync):
           ├─ 等待 vblank
           ├─ 编程硬件寄存器
           └─ 通知用户态 (page flip event)
```

---

## 4. GEM 缓冲区管理

### 4.1 对象生命周期

```
创建:
  drm_gem_object_init()          ← 基于 shmem 的 GEM 对象
  drm_gem_private_object_init()  ← 驱动自定义的 GEM 对象
  drm_gem_dma_create()           ← 基于 DMA 的 GEM 对象
  drm_gem_prime_import()         ← 从 dma-buf fd 导入

使用:
  drm_gem_object_get() / _put()  ← 引用计数
  drm_gem_mmap_obj()             ← 映射到用户空间
  drm_gem_dmabuf_export()        ← 导出为 dma-buf fd (PRIME)
  drm_gem_vmap() / _vunmap()     ← 内核态临时映射

销毁:
  当 refcount 归零时, 通过 funcs->free() 释放
```

### 4.2 PRIME — 跨设备缓冲区共享

```
[GPU A]                    [显示控制器 B]
   │                          │
   └─ PRIME export ────→ dma-buf fd ────→ PRIME import
      (drm_gem_prime_export)    │          (drm_gem_prime_import)
                             内核 dma-buf 层
                             负责物理页/IOMMU/SG table 管理
```

**关键流程**：

1. GPU 驱动创建 GEM 对象并渲染
2. `drm_gem_prime_export()` → 创建 `dma_buf`，分配文件描述符
3. 用户态将 fd 传给显示驱动（通过 socket/DBus）
4. 显示驱动 `drm_gem_prime_import()` → 映射为本地 GEM 对象
5. 显示控制器直接扫描导出该缓冲区（零拷贝）

### 4.3 GPUVM — GPU 虚拟地址空间

```
drm_gpuvm
├─ 管理 GPU 侧的虚拟地址空间
├─ 支持多 VM (per-process / per-context)
├─ 通过 drm_gpuva / drm_gpuvma 表示映射区域
└─ 与 GEM 对象绑定:
    GPU VM 的 va → 映射到某个 GEM 对象 + 偏移
```

---

## 5. VBlank 和页面翻转

VBlank (垂直消隐期) 是显示控制器在帧与帧之间的短暂间隔。
在此期间更新显示配置可避免画面撕裂。

```
帧 N 显示           VBlank          帧 N+1 显示
┌──────────────┐    │\    ┌──────────────┐
│              │ ...│ \   │              │
│ 扫描线输出   │    │  \  │ 扫描线输出   │
│              │    │   \ │              │
└──────────────┘    │    \└──────────────┘
        ↑           │     ↑      ↑
        │           │     │      │
   vblank 中断       │   新的 framebuffer
                     │   在此时生效
                vsync 信号
```

### 关键函数

```c
drm_vblank_init()          ← 初始化 VBlank 子系统
drm_crtc_vblank_get()      ← 开启 VBlank 中断
drm_crtc_vblank_put()      ← 关闭 VBlank 中断 (引用计数)
drm_crtc_wait_one_vblank() ← 等待一个 vblank
drm_crtc_send_vblank_event() ← 向用户态发送 page flip 完成事件
```

---

## 6. 设备生命周期

### 6.1 驱动初始化和注册

```
驱动 probe():
  │
  ├─ drm_dev_init(&drm, &driver, &pdev->dev)
  │   ├─ 分配 drm_device
  │   ├─ 设置 driver 回调
  │   └─ 初始化 mode_config, GEM, VBlank 等子系统
  │
  ├─ 创建 KMS 对象:
  │   ├─ drm_crtc_init_with_planes()  — CRTC
  │   ├─ drm_encoder_init()           — Encoder
  │   ├─ drm_connector_init()         — Connector
  │   ├─ drm_plane_init()             — Plane (Primary + Overlay + Cursor)
  │   └─ drm_bridge_add()            — Bridge
  │
  ├─ 连接 KMS 管线:
  │   ├─ drm_connector_attach_encoder()
  │   └─ drm_bridge_attach()  (encoder → bridge → connector)
  │
  ├─ 初始化 GEM/VRAM:
  │   ├─ drm_vma_offset_manager_init()
  │   └─ 创建 per-GPU 内存管理器 (TTM / shmem / DMA)
  │
  └─ drm_dev_register(drm)
      ├─ 注册 /dev/dri/cardN 字符设备
      ├─ 注册 sysfs 接口
      └─ 创建 /dev/dri/renderDN 渲染节点
```

### 6.2 文件操作

```
用户态 open("/dev/dri/card0")
  │
  ├─ drm_open()
  │   ├─ 创建 struct drm_file (per-fd 的客户端上下文)
  │   ├─ 调用 driver->open() 回调 (如创建 per-fd GPU context)
  │   └─ 返回 fd
  │
  ├─ ioctl(fd, DRM_IOCTL_*, args)
  │   ├─ drm_ioctl()
  │   │   ├─ 权限检查
  │   │   ├─ 查找 ioctl 命令表
  │   │   └─ 调用对应的 handler
  │   │
  │   └─ 返回
  │
  └─ close(fd)
      └─ drm_release()
          ├─ 清理该客户端持有的所有 GEM 句柄
          ├─ 释放 per-fd 资源
          └─ 调用 driver->postclose()
```

---

## 7. Framebuffer Console (fbdev emulation)

DRM 提供了对传统 fbdev 接口的兼容层（drm_fb_helper）：

```
DRM Native 显示
    │
    └─ drm_fbdev_*_setup()
        ├─ 创建 pseudo fbdev 设备
        ├─ 配置初始模式 (启动时从固件/bootloader 继承)
        └─ 提供 /dev/fb0 兼容接口

当 KMS 客户端 (compositor) 启动时:
    ├─ 打开 /dev/dri/card0
    ├─ 通过 DRM_IOCTL_MODE_GETRESOURCES 枚举显示资源
    ├─ 通过 DRM_IOCTL_MODE_ATOMIC 配置显示
    └─ 此时 fbdev 仿真自动退让 (输出被 compositor 接管)
```

---

## 8. Display Pipeline 典型示例

### ARM SoC 嵌入式显示管线

```
[GPU 2D/3D] ─→ Framebuffer (GEM 对象)
                    │
                    ▼
[Hardware Plane] ─→ [Hardware Scaler] ─→ [CRTC] ─→ [Encoder] ─→ [DSI Bridge] ─→ [DSI Connector] ─→ [LCD Panel]

Overlay/Cursor:
                    │
[Overlay Plane] ────┤
                    │
[Cursor Plane] ─────┘
```

### x86 独立显卡管线 (i915/amdgpu)

```
[GPU Render Engine] ─→ GEM Framebuffer
  │
  ├─ [Primary Plane] ─→ [CRTC 0]  ─→ [DisplayPort Encoder] ─→ [DP Connector] → Monitor 0
  ├─ [Primary Plane] ─→ [CRTC 1]  ─→ [HDMI Encoder]       ─→ [HDMI Connector] → Monitor 1
  └─ [Primary Plane] ─→ [CRTC 2]  ─→ [eDP Encoder]        ─→ [eDP Connector] → Internal Laptop Screen
```

---

## 9. 核心文件分类

### KMS 框架
| 文件 | 功能 |
|------|------|
| `drm_mode_config.c` | mode_config 初始化, DRM_IOCTL_MODE_GETRESOURCES |
| `drm_crtc.c` | CRTC 对象管理, 模式设置 |
| `drm_connector.c` | Connector 管理, 热插拔, 探测 |
| `drm_encoder.c` | Encoder 管理 |
| `drm_plane.c` | Plane 管理 (Primary + Overlay + Cursor) |
| `drm_bridge.c` | Bridge 链管理 |
| `drm_panel.c` | Panel 抽象 (LCD 面板驱动) |
| `drm_modes.c` | 显示模式数据库 (VESA DMT/GTF/CVT/...) |
| `drm_edid.c` | EDID 解析 (从显示器读取支持的模式) |

### Atomic 事务
| 文件 | 功能 |
|------|------|
| `drm_atomic.c` | Atomic state 管理, commit 流程 |
| `drm_atomic_helper.c` | 辅助函数 (check/commit/prepare/swap_state/...) |
| `drm_atomic_state_helper.c` | state 的 duplicate/destroy/reset |
| `drm_atomic_uapi.c` | 用户态 Atomic ioctl 接口 |
| `drm_modeset_lock.c` | 模式设置锁 (ww_mutex + 死锁检测回退) |
| `drm_crtc_helper.c` | CRTC 辅助函数 (mode_fixup/atomic_check) |
| `drm_probe_helper.c` | Connector 探测辅助 |

### GEM & 内存
| 文件 | 功能 |
|------|------|
| `drm_gem.c` | GEM 核心 (对象 CRUD, handle 管理) |
| `drm_gem_shmem_helper.c` | 基于 shmem 的 GEM 实现 |
| `drm_gem_dma_helper.c` | 基于 DMA API 的 GEM 实现 |
| `drm_gem_framebuffer_helper.c` | GEM → Framebuffer 桥接 |
| `drm_framebuffer.c` | Framebuffer 对象管理 |
| `drm_prime.c` | PRIME (dma-buf 导入/导出) |
| `drm_gpuvm.c` | GPU 虚拟地址空间管理 |
| `drm_vma_manager.c` | GEM mmap 偏移管理 |
| `drm_mm.c` | 显存区间分配器 (红黑树+最坏适应) |
| `drm_buddy.c` | Buddy 分配器 (用于大页/连续分配) |

### VBlank & 同步
| 文件 | 功能 |
|------|------|
| `drm_vblank.c` | VBlank 中断管理, 序列号, 等待队列 |
| `drm_vblank_work.c` | VBlank 工作队列 (在 vblank 时执行的 kthread_work) |
| `drm_syncobj.c` | Sync Object (用户态同步原语) |
| `drm_flip_work.c` | 页面翻转工作队列 |

### 设备框架
| 文件 | 功能 |
|------|------|
| `drm_drv.c` | 驱动注册, drm_device 管理, minor 分配 |
| `drm_file.c` | 文件操作, per-fd 上下文 |
| `drm_ioctl.c` | IOCTL 分发 |
| `drm_auth.c` | 认证 (magic cookie, 主节点权限) |
| `drm_lease.c` | DRM Lease (子客户端的受限资源访问) |
| `drm_managed.c` | 设备资源管理 (drmm_*, 自动释放) |
| `drm_sysfs.c` | sysfs 接口 |

---

## 10. 与内核启动的关系

DRM 是后期初始化子系统，在内核启动完成、驱动模型建立后才通过 `device_initcall` / `module_init` 注册：

```
start_kernel()
  → rest_init() → kernel_init() → kernel_init_freeable()
    → do_basic_setup() → do_initcalls()
      → device_initcall → platform_driver_register()
        → drm_*_driver_init() (各级 initcall)
          → pci_register_driver() / platform_driver_register()
            → 驱动的 probe() 被调用
              → drm_dev_init() + drm_dev_register()
```

但是，**early console / fbcon 的过渡**是启动可见性的关键：

1. 早期启动阶段：`simplefb` / `efifb` 等简单 framebuffer 提供启动画面
2. DRM 驱动注册后：接管显示硬件
3. `drm_fb_helper` 提供兼容的 `/dev/fb0` 接口
4. 用户态 compositor 启动后：通过 DRM native API 管理显示

---

## 11. 调试与诊断

| 工具/接口 | 用途 |
|----------|------|
| `/sys/kernel/debug/dri/N/` | DRM debugfs (state, connectors, crtc, framebuffer, gem_names) |
| `drm.debug=0x1e` 启动参数 | 启用 DRM 调试打印 |
| `modetest` (libdrm) | 从命令行测试 KMS 配置 |
| `drm_info` | 显示 DRM 设备信息 |
| `AMD_DEBUG=...` / `INTEL_DEBUG=...` | 厂商特定的调试环境变量 |

---

## 12. 总结

DRM 子系统的核心设计思想：

**分层抽象**：GEM (内存) + KMS (显示) + Atomic (事务) 三套独立但协作的 API，隔离了不同 GPU 的硬件差异。

**事务性保证**：Atomic modesetting 的 all-or-nothing 语义消除了旧 API 的竞态和中间状态闪烁问题——要么完整应用新配置，要么完全保持旧状态。

**零拷贝路径**：PRIME (dma-buf) 机制让 GPU 渲染结果可以直接被显示控制器扫描，无需通过 CPU 拷贝。这对于 4K@120Hz 的视频播放至关重要。

**兼容与共存**：fbdev 仿真层和 DRM native 客户端可以在同一设备上共存，compositor 接管时 fbcon 自动退让，VT 切换时又能无缝切回。

理解这套框架后，无论是编写显示驱动、调试 GPU 挂起问题、还是分析性能瓶颈，都能快速定位到正确的抽象层次。
