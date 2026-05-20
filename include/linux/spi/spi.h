/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2005 David Brownell
 */

#ifndef __LINUX_SPI_H
#define __LINUX_SPI_H

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>
#include <linux/mod_devicetable.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/u64_stats_sync.h>

#include <uapi/linux/spi/spi.h>

/* 单个 spi_device 最多支持的片选数量。 */
#define SPI_DEVICE_CS_CNT_MAX 4

/* 单个 spi_device 最多支持的数据 lane 数量。 */
#define SPI_DEVICE_DATA_LANE_CNT_MAX 8

struct dma_chan;
struct software_node;
struct ptp_system_timestamp;
struct spi_controller;
struct spi_transfer;
struct spi_controller_mem_ops;
struct spi_controller_mem_caps;
struct spi_message;
struct spi_offload;
struct spi_offload_config;

/*
 * SPI 控制器驱动、目标协议驱动，以及 SPI 基础设施之间的公共接口。
 */
extern const struct bus_type spi_bus_type;

/**
 * struct spi_statistics - SPI 传输统计信息
 * @syncp:         用于保护本结构体 per-cpu 成员的 seqcount
 *                 在 32 位系统上尤其需要它来保证统计读取一致性
 *
 * @messages:      已处理的 spi_message 数量
 * @transfers:     已处理的 spi_transfer 数量
 * @errors:        spi_transfer 期间发生的错误次数
 * @timedout:      spi_transfer 超时次数
 *
 * @spi_sync:      spi_sync 被调用的次数
 * @spi_sync_immediate:
 *                 spi_sync 在当前上下文中直接执行、没有排队调度的次数
 * @spi_async:     spi_async 被调用的次数
 *
 * @bytes:         传输到/来自设备的总字节数
 * @bytes_tx:      发送到设备的字节数
 * @bytes_rx:      从设备接收的字节数
 *
 * @transfer_bytes_histo:
 *                 按传输长度统计的直方图
 *
 * @transfers_split_maxsize:
 *                 因超过最大长度限制而被拆分的传输次数
 */
struct spi_statistics {
	struct u64_stats_sync	syncp;

	u64_stats_t		messages;
	u64_stats_t		transfers;
	u64_stats_t		errors;
	u64_stats_t		timedout;

	u64_stats_t		spi_sync;
	u64_stats_t		spi_sync_immediate;
	u64_stats_t		spi_async;

	u64_stats_t		bytes;
	u64_stats_t		bytes_rx;
	u64_stats_t		bytes_tx;

#define SPI_STATISTICS_HISTO_SIZE 17
	u64_stats_t	transfer_bytes_histo[SPI_STATISTICS_HISTO_SIZE];

	u64_stats_t	transfers_split_maxsize;
};

#define SPI_STATISTICS_ADD_TO_FIELD(pcpu_stats, field, count)		\
	do {								\
		struct spi_statistics *__lstats;			\
		get_cpu();						\
		__lstats = this_cpu_ptr(pcpu_stats);			\
		u64_stats_update_begin(&__lstats->syncp);		\
		u64_stats_add(&__lstats->field, count);			\
		u64_stats_update_end(&__lstats->syncp);			\
		put_cpu();						\
	} while (0)

#define SPI_STATISTICS_INCREMENT_FIELD(pcpu_stats, field)		\
	do {								\
		struct spi_statistics *__lstats;			\
		get_cpu();						\
		__lstats = this_cpu_ptr(pcpu_stats);			\
		u64_stats_update_begin(&__lstats->syncp);		\
		u64_stats_inc(&__lstats->field);			\
		u64_stats_update_end(&__lstats->syncp);			\
		put_cpu();						\
	} while (0)

/**
 * struct spi_delay - SPI 延迟信息
 * @value: 延迟数值
 * @unit: 延迟单位
 */
struct spi_delay {
#define SPI_DELAY_UNIT_USECS	0
#define SPI_DELAY_UNIT_NSECS	1
#define SPI_DELAY_UNIT_SCK	2
	u16	value;
	u8	unit;
};

extern int spi_delay_to_ns(struct spi_delay *_delay, struct spi_transfer *xfer);
extern int spi_delay_exec(struct spi_delay *_delay, struct spi_transfer *xfer);
extern void spi_transfer_cs_change_delay_exec(struct spi_message *msg,
						  struct spi_transfer *xfer);

/**
 * struct spi_device - SPI 目标设备在控制器侧的代理对象
 * @dev: 设备模型中的表示
 * @controller: 与该设备配合使用的 SPI 控制器
 * @max_speed_hz: 这块芯片允许使用的最高时钟速率
 *	（针对这块板子）；可由设备驱动修改。
 *	每个 spi_transfer 也可以通过 spi_transfer.speed_hz 覆盖它。
 * @bits_per_word: 数据传输以字为单位；常见字长有 8 位、12 位等。
 *	内存里的字长通常按 2 的幂对齐（例如 20 位样本会用 32 位承载）。
 *	可以由设备驱动修改，或者保持默认值 0，表示协议字长是 8 位。
 *	每个 spi_transfer 也可以通过 spi_transfer.bits_per_word 覆盖它。
 * @rt: 将 pump 线程设置为实时优先级。
 * @mode: SPI 模式，定义数据如何输出和采样。
 *	这个值可以由设备驱动修改。
 *	默认的片选低有效可以通过 SPI_CS_HIGH 覆盖，
 *	每个字节 MSB 先传也可以通过 SPI_LSB_FIRST 覆盖。
 * @irq: 小于 0 的值，或者传给 request_irq() 的中断号
 * @controller_state: 控制器的运行时状态
 * @controller_data: 控制器相关的板级私有数据，例如 FIFO 初始化参数；
 *	来源于 board_info.controller_data
 * @modalias: 该设备对应的驱动名或别名。
 *	会出现在 sysfs 的 "modalias" 属性中，用于 coldplug，
 *	也会出现在 hotplug 的 uevent 中。
 * @pcpu_statistics: 该 spi_device 的统计信息
 * @word_delay: 相邻两个 word 之间插入的延迟
 * @cs_setup: 控制器在 CS 拉低后需要等待的时间
 * @cs_hold: 控制器在 CS 拉高前需要等待的时间
 * @cs_inactive: 控制器在 CS 拉高后需要等待的时间。
 *	如果 @spi_transfer 里使用了 @cs_change_delay，那么两者会累加。
 * @chip_select: 物理 chip select 数组，spi->chipselect[i] 表示逻辑 CS i
 *	对应的物理 CS
 * @num_chipselect: 实际使用的物理 chip select 数量
 * @cs_index_mask: chipselect 数组中被驱动实际使用的位掩码
 * @cs_gpiod: 对应 chipselect 线的 GPIO 描述符数组
 *	（可选，未使用 GPIO 时为 NULL）
 * @tx_lane_map: 外设 lane（索引）到控制器 lane（值）的映射
 * @num_tx_lanes: 已连接的发送 lane 数量
 * @rx_lane_map: 外设 lane（索引）到控制器 lane（值）的映射
 * @num_rx_lanes: 已连接的接收 lane 数量
 *
 * @spi_device 用于在 SPI 目标设备（通常是一颗独立芯片）和 CPU 内存之间
 * 交换数据。
 *
 * 在 @dev 中，platform_data 用来保存与设备协议驱动相关、但控制器不关心的
 * 信息。例如芯片变体标识，或者该板子如何接线等。
 */
struct spi_device {
	struct device		dev;
	struct spi_controller	*controller;
	u32			max_speed_hz;
	u8			bits_per_word;
	bool			rt;
#define SPI_NO_TX		BIT(31)		/* No transmit wire */
#define SPI_NO_RX		BIT(30)		/* No receive wire */
	/*
	 * TPM 规范定义了通过 SPI 的流控。
	 * 当控制器在 MOSI 上发送地址时，client 设备可以在 MISO 上
	 * 插入一个 wait state。软件只能在全双工控制器上检测这种状态；
	 * 对于只支持半双工的控制器，wait state 的检测必须由硬件实现。
	 * 当 TPM 设备期望 SPI 控制器提供硬件流控时，会设置这个标志。
	 */
#define SPI_TPM_HW_FLOW		BIT(29)		/* TPM HW flow control */
	/*
	 * 上面定义的所有 bit 都应该被 SPI_MODE_KERNEL_MASK 覆盖。
	 * SPI_MODE_KERNEL_MASK 在用户态有对应的 SPI_MODE_USER_MASK，
	 * 后者定义在 'include/uapi/linux/spi/spi.h'。
	 * 这里定义的 bit 是从 bit 31 向下排列的，而 SPI_MODE_USER_MASK
	 * 则是从 0 往上排列。
	 * 这些 bit 之间不能重叠，static assert 会负责检查这一点。
	 * 如果要添加新的 bit，也要同步向下调整 bit 位编号。
	 */
#define SPI_MODE_KERNEL_MASK	(~(BIT(29) - 1))
	u32			mode;
	int			irq;
	void			*controller_state;
	void			*controller_data;
	char			modalias[SPI_NAME_SIZE];

	/* 统计信息。 */
	struct spi_statistics __percpu	*pcpu_statistics;

	struct spi_delay	word_delay; /* word 之间的延迟 */

	/* CS 时序延迟。 */
	struct spi_delay	cs_setup;
	struct spi_delay	cs_hold;
	struct spi_delay	cs_inactive;

	u8			chip_select[SPI_DEVICE_CS_CNT_MAX];
	u8			num_chipselect;

	/*
	 * 驱动需要从 chipselect 数组中使用的 chipselect 位掩码。
	 * 当控制器可以同时处理多个 chip select，且多个 memory 并联时，
	 * cs_index_mask 中可能需要设置多个 bit。
	 */
	u32			cs_index_mask : SPI_DEVICE_CS_CNT_MAX;

	struct gpio_desc	*cs_gpiod[SPI_DEVICE_CS_CNT_MAX];	/* chip select 的 GPIO 描述符 */

	/* 多 lane SPI 控制器支持。 */
	u8			tx_lane_map[SPI_DEVICE_DATA_LANE_CNT_MAX];
	u8			num_tx_lanes;
	u8			rx_lane_map[SPI_DEVICE_DATA_LANE_CNT_MAX];
	u8			num_rx_lanes;

	/*
	 * 未来很可能还需要更多 hook，来覆盖那些会影响控制器与每颗芯片
	 * 通信方式的协议选项，例如：
	 *  - memory packing（把 12 位样本打包到低位，其它位清零）
	 *  - priority
	 *  - chipselect 延迟
	 *  - ...
	 */
};

/* Make sure that SPI_MODE_KERNEL_MASK & SPI_MODE_USER_MASK don't overlap */
static_assert((SPI_MODE_KERNEL_MASK & SPI_MODE_USER_MASK) == 0,
	      "SPI_MODE_USER_MASK & SPI_MODE_KERNEL_MASK must not overlap");

#define to_spi_device(__dev)	container_of_const(__dev, struct spi_device, dev)

	/* 大多数驱动不需要关心设备引用计数。 */
static inline struct spi_device *spi_dev_get(struct spi_device *spi)
{
	return (spi && get_device(&spi->dev)) ? spi : NULL;
}

static inline void spi_dev_put(struct spi_device *spi)
{
	if (spi)
		put_device(&spi->dev);
}

	/* ctldata 供 bus_controller 驱动保存运行时状态。 */
static inline void *spi_get_ctldata(const struct spi_device *spi)
{
	return spi->controller_state;
}

static inline void spi_set_ctldata(struct spi_device *spi, void *state)
{
	spi->controller_state = state;
}

	/* 设备驱动数据。 */

static inline void spi_set_drvdata(struct spi_device *spi, void *data)
{
	dev_set_drvdata(&spi->dev, data);
}

static inline void *spi_get_drvdata(const struct spi_device *spi)
{
	return dev_get_drvdata(&spi->dev);
}

static inline u8 spi_get_chipselect(const struct spi_device *spi, u8 idx)
{
	return spi->chip_select[idx];
}

static inline void spi_set_chipselect(struct spi_device *spi, u8 idx, u8 chipselect)
{
	spi->chip_select[idx] = chipselect;
}

static inline struct gpio_desc *spi_get_csgpiod(const struct spi_device *spi, u8 idx)
{
	return spi->cs_gpiod[idx];
}

static inline void spi_set_csgpiod(struct spi_device *spi, u8 idx, struct gpio_desc *csgpiod)
{
	spi->cs_gpiod[idx] = csgpiod;
}

static inline bool spi_is_csgpiod(struct spi_device *spi)
{
	u8 idx;

	for (idx = 0; idx < spi->num_chipselect; idx++) {
		if (spi_get_csgpiod(spi, idx))
			return true;
	}
	return false;
}

/**
 * struct spi_driver - SPI 侧的“协议”驱动
 * @id_table: 该驱动支持的 SPI 设备列表
 * @probe: 将该驱动绑定到 SPI 设备。驱动可以在这里确认设备确实存在，
 *	并且可能需要配置系统初始化阶段未配置过的特性（例如 bits_per_word）。
 * @remove: 将该驱动与 SPI 设备解绑
 * @shutdown: 在系统状态切换时使用的标准 shutdown 回调，例如 powerdown、
 *	halt 和 kexec
 * @driver: SPI 设备驱动应初始化这个结构体中的 name 和 owner 字段。
 *
 * 这里表示的是一种使用 SPI 消息与 SPI 链路另一端硬件交互的设备驱动。
 * 它被称为“协议驱动”，是因为它通过消息工作，而不是直接操作 SPI 硬件；
 * 真正把消息送到硬件上的，是底层的 SPI 控制器驱动。这里所谓的协议，
 * 指的是驱动所支持设备规格里定义的传输格式和语义。
 *
 * 一般来说，这类设备协议代表驱动支持的最低层接口，而驱动通常还会
 * 提供更高层的接口。例如 MTD、网络、MMC、RTC、字符设备节点以及
 * 硬件监控框架等。
 */
struct spi_driver {
	const struct spi_device_id *id_table;
	int			(*probe)(struct spi_device *spi);
	void			(*remove)(struct spi_device *spi);
	void			(*shutdown)(struct spi_device *spi);
	struct device_driver	driver;
};

#define to_spi_driver(__drv)   \
	( __drv ? container_of_const(__drv, struct spi_driver, driver) : NULL )

extern int __spi_register_driver(struct module *owner, struct spi_driver *sdrv);

/**
 * spi_unregister_driver - spi_register_driver 的反向操作
 * @sdrv: 需要注销的驱动
 * Context: 可以睡眠
 */
static inline void spi_unregister_driver(struct spi_driver *sdrv)
{
	if (sdrv)
		driver_unregister(&sdrv->driver);
}

extern struct spi_device *spi_new_ancillary_device(struct spi_device *spi, u8 chip_select);
extern struct spi_device *devm_spi_new_ancillary_device(struct spi_device *spi, u8 chip_select);

/* 使用宏定义来避免为了 THIS_MODULE 引入额外的头文件。 */
#define spi_register_driver(driver) \
	__spi_register_driver(THIS_MODULE, driver)

/**
 * module_spi_driver() - 注册 SPI 驱动的辅助宏
 * @__spi_driver: spi_driver 结构体
 *
 * 这是给那些在 module init/exit 中不需要做特殊处理的 SPI 驱动准备的
 * 辅助宏，可以去掉大量模板代码。每个模块只能使用一次这个宏，
 * 调用它会替代 module_init() 和 module_exit()。
 */
#define module_spi_driver(__spi_driver) \
	module_driver(__spi_driver, spi_register_driver, \
			spi_unregister_driver)

/**
 * struct spi_controller - SPI host 或 target 控制器接口
 * @dev: 该驱动的设备接口
 * @list: 与全局 spi_controller 链表相连
 * @bus_num: 某个 SPI 控制器的板级（通常也是 SoC 级）编号
 * @num_chipselect: 用于区分不同 SPI target 的 chip select 数量
 *	每个 target 都有 chip select 信号，但常见情况是并不是每个
 *	物理 CS 都接到了 target。
 * @num_data_lanes: 该控制器支持的数据 lane 数量，默认是 1
 * @dma_alignment: SPI 控制器对 DMA 缓冲区对齐的约束
 * @mode_bits: 该控制器驱动能理解的 mode 标志
 * @buswidth_override_bits: 该控制器驱动需要覆盖的 buswidth 标志
 * @bits_per_word_mask: 一个掩码，用来指示哪些 bits_per_word 值被支持。
 *	第 n 位表示支持 bits_per_word = n+1。若设置了它，SPI core 会
 *	拒绝任何不支持 bits_per_word 的 transfer；若未设置，则是否校验
 *	由具体驱动自己决定。
 * @min_speed_hz: 最低支持的传输速度
 * @max_speed_hz: 最高支持的传输速度
 * @flags: 该驱动相关的其它约束
 * @slave: 表示这是一个 SPI slave 控制器
 * @target: 表示这是一个 SPI target 控制器
 * @devm_allocated: 该结构体是否由 devres 管理
 * @max_transfer_size: 返回某个 &spi_device; 的最大 transfer 大小；
 *	可能为 %NULL，这时默认使用 %SIZE_MAX。
 * @max_message_size: 返回某个 &spi_device; 的最大 message 大小；
 *	可能为 %NULL，这时默认使用 %SIZE_MAX。
 * @io_mutex: 物理总线访问的互斥锁
 * @add_lock: 避免向同一个 chipselect 重复添加设备的互斥锁
 * @bus_lock_spinlock: SPI 总线锁定用的自旋锁
 * @bus_lock_mutex: 保护多个调用者互斥的 mutex
 * @bus_lock_flag: 表示 SPI 总线已被独占锁定
 * @setup: 更新设备模式和时钟配置的记录；协议代码可调用。
 *	如果请求了不认识或不支持的模式，这个函数必须返回失败。
 *	只要该设备上没有正在进行的 transfer，调用它总是安全的。
 * @set_cs_timing: 可选钩子，允许 SPI 设备请求控制器配置特定的
 *	CS setup time、hold time 和 inactive delay
 * @transfer: 把一个 message 加入控制器的传输队列
 * @cleanup: 释放控制器私有状态
 * @can_dma: 判断该控制器是否支持 DMA
 * @dma_map_dev: 可用于 DMA 映射的设备
 * @cur_rx_dma_dev: 当前用于 RX DMA 映射的设备
 * @cur_tx_dma_dev: 当前用于 TX DMA 映射的设备
 * @queued: 该控制器是否提供内部 message queue
 * @kworker: message pump 所在线程的指针
 * @pump_messages: 用于调度 message pump 的 work 结构
 * @queue_lock: 保护 message queue 访问的自旋锁
 * @queue: message 队列
 * @cur_msg: 当前正在传输的 message
 * @cur_msg_completion: 当前正在传输的 message 的 completion
 * @cur_msg_incomplete: 内部标志，用于机会性跳过 @cur_msg_completion；
 *	该标志用于检查驱动是否已经调用 spi_finalize_current_message()。
 * @cur_msg_need_completion: 内部标志，用于机会性跳过 @cur_msg_completion；
 *	该标志用于通知正在执行 spi_finalize_current_message() 的上下文，
 *	它需要调用 complete()。
 * @fallback: DMA 传输返回 SPI_TRANS_FAIL_NO_START 时回退到 PIO
 * @last_cs_mode_high: 上一次调用 set_cs 时，(mode & SPI_CS_HIGH) 是否为真
 * @last_cs: set_cs 记录的最后一个 chip_select，未选中时为 -1
 * @last_cs_index_mask: 最近一次使用的 chip select 位掩码
 * @xfer_completion: core transfer_one_message() 使用的 completion
 * @busy: message pump 正忙
 * @running: message pump 正在运行
 * @rt: 该队列是否设置为实时任务运行
 * @auto_runtime_pm: core 是否应在硬件准备期间持有 runtime PM 引用，
 *	这里使用的是 parent device
 * @max_dma_len: 设备允许的最大 DMA 传输长度
 * @prepare_transfer_hardware: 队列里即将到来一个 message 时调用，
 *	通知驱动提前准备传输硬件
 * @transfer_one_message: subsystem 调用驱动传输单个 message；
 *	在此期间到来的其它 transfers 会排队。驱动完成后必须调用
 *	spi_finalize_current_message()，以便 subsystem 发送下一个 message。
 * @unprepare_transfer_hardware: 当队列里已经没有更多 message 时调用，
 *	通知驱动可以放松硬件准备
 *
 * @set_cs: 设置 chip select 线的逻辑电平。可在中断上下文中调用。
 * @optimize_message: 为复用而优化 message
 * @unoptimize_message: 释放 optimize_message 分配的资源
 * @prepare_message: 为传输单个 message 做准备，例如进行 DMA 映射；
 *	在 threaded context 中调用
 * @transfer_one: 传输单个 spi_transfer
 *
 *                  - 如果 transfer 已完成，返回 0
 *                  - 如果 transfer 仍在进行，返回 1。驱动在完成该
 *                    transfer 后必须调用 spi_finalize_current_transfer()，
 *                    以便 subsystem 发送下一个 transfer。
 *                    如果 transfer 失败，驱动必须先把
 *                    SPI_TRANS_FAIL_IO 标记写入 spi_transfer->error，
 *                    再调用 spi_finalize_current_transfer()。
 *                    注意：transfer_one 和 transfer_one_message 是互斥的；
 *                    两者都设置时，generic subsystem 不会调用 transfer_one
 * @handle_err: 在 transfer_one_message() 的通用实现发生错误时，
 *	由 subsystem 调用驱动处理错误
 * @mem_ops: 针对 SPI memory 操作的优化/专用操作
 *	     只有控制器原生支持 memory-like 操作时才应该实现该字段
 * @get_offload: 支持 offload 的控制器获取匹配 offload 实例的回调；
 *	如果没有找到匹配项，返回 -ENODEV
 * @put_offload: 释放通过 @get_offload 获取到的 offload 实例
 * @mem_caps: 内存操作相关的控制器能力
 * @dtr_caps: 如果控制器支持 DTR(single/dual transfer rate)，则为 true；
 *	QSPI 控制器应根据自身能力填写
 * @unprepare_message: 撤销 prepare_message() 做的工作
 * @target_abort: 中止 SPI target 控制器上的正在进行的传输请求
 * @cs_gpiods: 作为 chip select 线使用的 GPIO 描述符数组；每个 CS 一个。
 *	某个条目可以为 NULL，表示那条 CS 线不是 GPIO，而是由控制器自身驱动。
 * @use_gpio_descriptors: 打开 SPI core 解析和获取 GPIO 描述符的逻辑。
 *	它会填充 @cs_gpiods；如果某个 chipselect 对应的是 GPIO 线，
 *	SPI 设备也会得到 cs_gpiod 赋值。
 * @unused_native_cs: 使用 cs_gpiods 时，spi_register_controller() 会在这里
 *	填入第一个未使用的原生 CS，供需要在使用 GPIO CS 时仍驱动原生 CS 的
 *	SPI 控制器驱动使用。
 * @max_native_cs: 使用 cs_gpiods 时，如果这个字段被填充，
 *	spi_register_controller() 会用它来校验所有原生 CS（包括未使用的那个）。
 * @pcpu_statistics: spi_controller 的统计信息
 * @dma_tx: DMA 发送通道
 * @dma_rx: DMA 接收通道
 * @dummy_rx: 全双工设备使用的 dummy 接收缓冲区
 * @dummy_tx: 全双工设备使用的 dummy 发送缓冲区
 * @fw_translate_cs: 如果启动固件使用的编号方案和 Linux 不同，
 *	这个可选钩子可用于在两者之间做转换
 * @ptp_sts_supported: 如果驱动把它设为 true，就必须在
 *	@spi_transfer->ptp_sts 中尽可能接近 @spi_transfer->ptp_sts_word_pre 和
 *	@spi_transfer->ptp_sts_word_post 实际发送时刻给出时间戳。
 *	如果不设置它，SPI core 会尽量在驱动交接时刻附近完成采样。
 * @irq_flags: PTP system timestamping 期间的中断使能状态
 * @queue_empty: 给 spi_sync transfers 提供机会性跳过队列的“绿灯”
 * @must_async: 禁用 core 中所有快速路径
 * @defer_optimize_message: 若控制器不能预优化 message，
 *	而需要把优化推迟到 message 真正开始传输时，则设为 true
 *
 * 每个 SPI 控制器都可以与一个或多个 @spi_device 子设备通信。
 * 它们构成一条很小的总线，共享 MOSI、MISO 和 SCK，但不共享 chip select。
 * 每个设备都可以配置不同的时钟速率，因为除非 chip 被选中，
 * 这些共享信号都不会起作用。
 *
 * SPI 控制器驱动通过 spi_message 事务队列管理这些设备的访问，
 * 在 CPU 内存和 SPI target 设备之间复制数据。它为自己排队的每条
 * message 在传输完成后都会调用该 message 的完成回调。
 */
struct spi_controller {
	struct device	dev;

	struct list_head list;

	/*
	 * 除了负值（表示动态分配）以外，bus_num 完全由板级决定。
	 * 通常这会进一步体现为 SoC 级别的编号。
	 * 例如，某个 SoC 有 3 个 SPI 控制器，编号 0..2，
	 * 某块板子的原理图可能把它标成 SPI-2；软件通常会对
	 * 这个控制器使用 bus_num=2。
	 */
	s16			bus_num;

	/*
	 * chipselect 对很多控制器来说是内建能力；另外一些控制器
	 * 则可能使用板级 GPIO 来实现。
	 */
	u16			num_chipselect;

	/*
	 * 某些专用 SPI 控制器在每个控制器上可能有多个物理数据 lane
	 * 接口（每个 lane 都有自己的 serializer）。
	 * 这里用于指定这种情况下的数据 lane 数量。
	 * 其它控制器不需要设置它（默认值是 1）。
	 */
	u16			num_data_lanes;

	/*
	 * 某些 SPI 控制器对可 DMA 缓冲区有对齐要求；
	 * 这里把这些要求告知协议驱动。
	 */
	u16			dma_alignment;

	/* 该控制器驱动能够理解的 spi_device.mode 标志。 */
	u32			mode_bits;

	/* 该控制器驱动的 spi_device.mode 覆盖标志。 */
	u32			buswidth_override_bits;

	/* transfer 支持的 bits_per_word 位掩码。 */
	u32			bits_per_word_mask;
#define SPI_BPW_MASK(bits) BIT((bits) - 1)
#define SPI_BPW_RANGE_MASK(min, max) GENMASK((max) - 1, (min) - 1)

	/* 传输速度限制。 */
	u32			min_speed_hz;
	u32			max_speed_hz;

	/* 该驱动相关的其它约束。 */
	u16			flags;
#define SPI_CONTROLLER_HALF_DUPLEX	BIT(0)	/* 不能做全双工。 */
#define SPI_CONTROLLER_NO_RX		BIT(1)	/* 不能读缓冲区。 */
#define SPI_CONTROLLER_NO_TX		BIT(2)	/* 不能写缓冲区。 */
#define SPI_CONTROLLER_MUST_RX		BIT(3)	/* 必须有 RX。 */
#define SPI_CONTROLLER_MUST_TX		BIT(4)	/* 必须有 TX。 */
#define SPI_CONTROLLER_GPIO_SS		BIT(5)	/* GPIO CS 必须选中目标设备。 */
#define SPI_CONTROLLER_SUSPENDED	BIT(6)	/* 当前处于挂起状态。 */
	/*
	 * 该 spi-controller 具备多 chip select 能力，
	 * 可以同时 assert / de-assert 多个 chip select。
	 */
#define SPI_CONTROLLER_MULTI_CS		BIT(7)

	/* 指示该结构体的分配是否由 devres 管理。 */
	bool			devm_allocated;

	union {
		/* 指示这是一个 SPI slave 控制器。 */
		bool			slave;
		/* 指示这是一个 SPI target 控制器。 */
		bool			target;
	};

	/*
	 * 在某些硬件上，transfer / message 大小可能会受限制，
	 * 而且这个限制还可能依赖设备的传输配置。
	 */
	size_t (*max_transfer_size)(struct spi_device *spi);
	size_t (*max_message_size)(struct spi_device *spi);

	/* I/O mutex。 */
	struct mutex		io_mutex;

	/* 用于避免同一个 CS 被重复添加。 */
	struct mutex		add_lock;

	/* SPI 总线锁用的自旋锁和 mutex。 */
	spinlock_t		bus_lock_spinlock;
	struct mutex		bus_lock_mutex;

	/* 表示 SPI 总线已被独占锁定。 */
	bool			bus_lock_flag;

	/*
	 * 设置模式、时钟等参数（SPI 驱动可能会多次调用）。
	 *
	 * 重要：在其它设备的传输仍在进行时，也可能调用这里。
	 * 不要以可能破坏这些传输的方式去更新共享寄存器。
	 */
	int			(*setup)(struct spi_device *spi);

	/*
	 * set_cs_timing() 方法用于支持配置 CS 时序的 SPI 控制器。
	 *
	 * 这个钩子允许 SPI client 驱动在 spi_setup() 之后，
	 * 通过 spi_set_cs_timing() 请求控制器配置特定的 CS 时序。
	 */
	int (*set_cs_timing)(struct spi_device *spi);

	/*
	 * 双向批量传输。
	 *
	 * + transfer() 方法不能睡眠；它的主要职责只是把 message
	 *   加入队列。
	 * + 目前没有从队列中移除请求的操作，也没有其它请求管理机制。
	 * + 对某个 spi_device 而言，message 排队完全遵循 FIFO。
	 *
	 * + 控制器的主要工作是处理 message 队列，先选中芯片（对于
	 *   controller 而言），然后再传输数据。
	 * + 如果有多个 spi_device 子设备，I/O 队列仲裁算法并未规定
	 *   （round robin、FIFO、priority、reservations、preemption 等都可能）。
	 *
	 * + 除非 spi_transfer.cs_change != 0，否则 chipselect 会在整个
	 *   message 期间保持有效。
	 * + message 中的传输会使用之前由 setup() 为该设备建立的时钟
	 *   和 SPI mode 参数。
	 */
	int			(*transfer)(struct spi_device *spi,
						struct spi_message *mesg);

	/* 在注销时调用，用于释放 spi_controller 提供的内存。 */
	void			(*cleanup)(struct spi_device *spi);

	/*
	 * 用于开启 core 对 DMA 处理的支持。
	 * 如果 can_dma() 存在并返回 true，那么在调用 transfer_one()
	 * 之前，transfer 会先被映射。驱动不应修改或保存 xfer，
	 * 并且在设备准备期间必须设置 dma_tx 和 dma_rx。
	 */
	bool			(*can_dma)(struct spi_controller *ctlr,
					   struct spi_device *spi,
					   struct spi_transfer *xfer);
	struct device *dma_map_dev;
	struct device *cur_rx_dma_dev;
	struct device *cur_tx_dma_dev;

	/*
	 * 这些钩子供想使用 generic controller transfer queueing 机制
	 * 的驱动使用。如果用了这些钩子，上面的 transfer() 函数就不能再由
	 * 驱动提供。随着时间推移，我们希望 SPI 驱动逐步迁移到这个 API。
	 */
	bool				queued;
	struct kthread_worker		*kworker;
	struct kthread_work		pump_messages;
	spinlock_t			queue_lock;
	struct list_head		queue;
	struct spi_message		*cur_msg;
	struct completion               cur_msg_completion;
	bool				cur_msg_incomplete;
	bool				cur_msg_need_completion;
	bool				busy;
	bool				running;
	bool				rt;
	bool				auto_runtime_pm;
	bool                            fallback;
	bool				last_cs_mode_high;
	s8				last_cs[SPI_DEVICE_CS_CNT_MAX];
	u32				last_cs_index_mask : SPI_DEVICE_CS_CNT_MAX;
	struct completion               xfer_completion;
	size_t				max_dma_len;

	int (*optimize_message)(struct spi_message *msg);
	int (*unoptimize_message)(struct spi_message *msg);
	int (*prepare_transfer_hardware)(struct spi_controller *ctlr);
	int (*transfer_one_message)(struct spi_controller *ctlr,
				    struct spi_message *mesg);
	int (*unprepare_transfer_hardware)(struct spi_controller *ctlr);
	int (*prepare_message)(struct spi_controller *ctlr,
			       struct spi_message *message);
	int (*unprepare_message)(struct spi_controller *ctlr,
				 struct spi_message *message);
	int (*target_abort)(struct spi_controller *ctlr);

	/*
	 * 这些钩子供使用 core 提供的 transfer_one_message() 通用实现的驱动使用。
	 */
	void (*set_cs)(struct spi_device *spi, bool enable);
	int (*transfer_one)(struct spi_controller *ctlr, struct spi_device *spi,
			    struct spi_transfer *transfer);
	void (*handle_err)(struct spi_controller *ctlr,
			   struct spi_message *message);

	/* 针对 SPI memory-like 操作的优化处理函数。 */
	const struct spi_controller_mem_ops *mem_ops;
	const struct spi_controller_mem_caps *mem_caps;

	/* 如果 SPI 或 QSPI 控制器支持 SDR/DDR 传输速率，可将其设为 true。 */
	bool			dtr_caps;

	struct spi_offload *(*get_offload)(struct spi_device *spi,
					   const struct spi_offload_config *config);
	void (*put_offload)(struct spi_offload *offload);

	/* GPIO chip select。 */
	struct gpio_desc	**cs_gpiods;
	bool			use_gpio_descriptors;
	s8			unused_native_cs;
	s8			max_native_cs;

	/* 统计信息。 */
	struct spi_statistics __percpu	*pcpu_statistics;

	/* 供 core dmaengine helper 使用的 DMA 通道。 */
	struct dma_chan		*dma_tx;
	struct dma_chan		*dma_rx;

	/* 全双工设备使用的 dummy 数据。 */
	void			*dummy_rx;
	void			*dummy_tx;

	int (*fw_translate_cs)(struct spi_controller *ctlr, unsigned cs);

	/*
	 * 驱动通过这个字段表明自己能够对 SPI 传输做时间快照，
	 * 例如读取 POSIX clocks 的时间时就需要它。
	 */
	bool			ptp_sts_supported;

	/* PTP system timestamping 期间的中断使能状态。 */
	unsigned long		irq_flags;

	/* 允许 spi_sync 在某些情况下机会性跳过队列的标志。 */
	bool			queue_empty;
	bool			must_async;
	bool			defer_optimize_message;
};

static inline void *spi_controller_get_devdata(struct spi_controller *ctlr)
{
	return dev_get_drvdata(&ctlr->dev);
}

static inline void spi_controller_set_devdata(struct spi_controller *ctlr,
					      void *data)
{
	dev_set_drvdata(&ctlr->dev, data);
}

static inline struct spi_controller *spi_controller_get(struct spi_controller *ctlr)
{
	if (!ctlr || !get_device(&ctlr->dev))
		return NULL;
	return ctlr;
}

static inline void spi_controller_put(struct spi_controller *ctlr)
{
	if (ctlr)
		put_device(&ctlr->dev);
}

static inline bool spi_controller_is_target(struct spi_controller *ctlr)
{
	return IS_ENABLED(CONFIG_SPI_SLAVE) && ctlr->target;
}

/* 由驱动直接发起的 PM 调用。 */
extern int spi_controller_suspend(struct spi_controller *ctlr);
extern int spi_controller_resume(struct spi_controller *ctlr);

/* 驱动与消息队列交互时使用的调用。 */
extern struct spi_message *spi_get_next_queued_message(struct spi_controller *ctlr);
extern void spi_finalize_current_message(struct spi_controller *ctlr);
extern void spi_finalize_current_transfer(struct spi_controller *ctlr);

/* 驱动用于为传输打时间戳的辅助调用。 */
void spi_take_timestamp_pre(struct spi_controller *ctlr,
			    struct spi_transfer *xfer,
			    size_t progress, bool irqs_off);
void spi_take_timestamp_post(struct spi_controller *ctlr,
			     struct spi_transfer *xfer,
			     size_t progress, bool irqs_off);

/* SPI driver core 负责管理 spi_controller 类设备的内存。 */
extern struct spi_controller *__spi_alloc_controller(struct device *host,
						unsigned int size, bool target);

static inline struct spi_controller *spi_alloc_host(struct device *dev,
						    unsigned int size)
{
	return __spi_alloc_controller(dev, size, false);
}

static inline struct spi_controller *spi_alloc_target(struct device *dev,
						      unsigned int size)
{
	if (!IS_ENABLED(CONFIG_SPI_SLAVE))
		return NULL;

	return __spi_alloc_controller(dev, size, true);
}

struct spi_controller *__devm_spi_alloc_controller(struct device *dev,
						   unsigned int size,
						   bool target);

static inline struct spi_controller *devm_spi_alloc_host(struct device *dev,
							 unsigned int size)
{
	return __devm_spi_alloc_controller(dev, size, false);
}

static inline struct spi_controller *devm_spi_alloc_target(struct device *dev,
							   unsigned int size)
{
	if (!IS_ENABLED(CONFIG_SPI_SLAVE))
		return NULL;

	return __devm_spi_alloc_controller(dev, size, true);
}

extern int spi_register_controller(struct spi_controller *ctlr);
extern int devm_spi_register_controller(struct device *dev,
					struct spi_controller *ctlr);
extern void spi_unregister_controller(struct spi_controller *ctlr);

#if IS_ENABLED(CONFIG_OF)
extern struct spi_controller *of_find_spi_controller_by_node(struct device_node *node);
#else
static inline struct spi_controller *of_find_spi_controller_by_node(struct device_node *node)
{
	return NULL;
}
#endif

#if IS_ENABLED(CONFIG_ACPI) && IS_ENABLED(CONFIG_SPI_MASTER)
extern struct spi_controller *acpi_spi_find_controller_by_adev(struct acpi_device *adev);
extern struct spi_device *acpi_spi_device_alloc(struct spi_controller *ctlr,
						struct acpi_device *adev,
						int index);
int acpi_spi_count_resources(struct acpi_device *adev);
#else
static inline struct spi_controller *acpi_spi_find_controller_by_adev(struct acpi_device *adev)
{
	return NULL;
}

static inline struct spi_device *acpi_spi_device_alloc(struct spi_controller *ctlr,
						       struct acpi_device *adev,
						       int index)
{
	return ERR_PTR(-ENODEV);
}

static inline int acpi_spi_count_resources(struct acpi_device *adev)
{
	return 0;
}
#endif

/*
 * SPI resource management while processing a SPI message
 */

typedef void (*spi_res_release_t)(struct spi_controller *ctlr,
				  struct spi_message *msg,
				  void *res);

/**
 * struct spi_res - SPI 资源管理结构体
 * @entry:   链表节点
 * @release: 在释放该资源之前调用的回调
 * @data:    为特定用途额外分配的数据
 *
 * 这个设计借鉴了 devres，但重点放在 spi_message 处理过程中的
 * 生命周期管理。
 */
struct spi_res {
	struct list_head        entry;
	spi_res_release_t       release;
	unsigned long long      data[]; /* Guarantee ull alignment */
};

/*---------------------------------------------------------------------------*/

/*
 * SPI 控制器与协议驱动之间的 I/O 接口
 *
 * 协议驱动会使用一队 spi_message，每个 message 负责在控制器与
 * 内存缓冲区之间传输数据。
 *
 * spi_message 本身由一系列读/写传输段组成。每个段写入和读出的
 * bit 数通常相同，但只要把其中一侧的缓冲区指针设为 NULL，就能
 * 很容易地忽略写或读。这里和大多数 I/O API 不同，因为 SPI 硬件
 * 本身就是全双工的。
 *
 * 注意：spi_transfer 和 spi_message 的内存分配完全由协议驱动负责；
 * 只要消息还在队列中，这个驱动就必须保证它们以及相关数据缓冲区的
 * 完整性。
 */

/**
 * struct spi_transfer - 一对读/写缓冲区
 * @tx_buf: 待写出的数据（DMA 安全内存），或者为 NULL
 * @rx_buf: 待读入的数据（DMA 安全内存），或者为 NULL
 * @tx_dma: tx_buf 的 DMA 地址，目前不供客户端使用
 * @rx_dma: rx_buf 的 DMA 地址，目前不供客户端使用
 * @tx_nbits: 写方向使用的 bit 数。如果为 0，则使用默认值
 *      （SPI_NBITS_SINGLE）。
 * @rx_nbits: 读方向使用的 bit 数。如果为 0，则使用默认值
 *      （SPI_NBITS_SINGLE）。
 * @multi_lane_mode: 多 lane 数据如何串行化。取值为
 *      SPI_MULTI_LANE_MODE_* 之一。
 * @len: 读写缓冲区大小（以字节计）
 * @speed_hz: 选择不同于设备默认值的传输速度。如果为 0，
 *      则使用默认值（来自 @spi_device）。
 * @bits_per_word: 为这次传输选择不同于设备默认值的每字位数。
 *      如果为 0，则使用默认值（来自 @spi_device）。
 * @dummy_data: 表示这是 dummy 字节传输。
 * @cs_off: 传输时关闭 chip select。
 * @cs_change: 在本次传输完成后影响 chip select 状态。
 * @cs_change_delay: 当设置了 @cs_change 且 @spi_transfer 不是
 *      @spi_message 中最后一个传输时，在 cs 取消和重新拉起之间
 *      需要等待的延迟。
 * @delay: 本次传输结束后、在可选地改变 chip select 状态并开始
 *      下一次传输或结束该 @spi_message 之前引入的延迟。
 * @word_delay: 每个按 bits_per_word 定义的字传输之间需要引入的
 *      间隔延迟。
 * @effective_speed_hz: 本次实际使用的有效 SCK 速率。如果 SPI
 *      总线驱动不支持，则设为 0。
 * @transfer_list: 通过 @spi_message.transfers 串联的传输链表节点
 * @tx_sg_mapped: 如果为 true，表示 @tx_sg 已映射用于 DMA
 * @rx_sg_mapped: 如果为 true，表示 @rx_sg 已映射用于 DMA
 * @tx_sg: 发送方向的 scatterlist，目前不供客户端使用
 * @rx_sg: 接收方向的 scatterlist，目前不供客户端使用
 * @offload_flags: 仅适用于特化 SPI offload 传输的标志位。
 *	参见 spi-offload.h 中的 %SPI_OFFLOAD_XFER_*。
 * @ptp_sts_word_pre: 该字段表示 SPI 设备请求在 @tx_buf 中哪个字
 *	开始采样时间戳。这里的偏移按 bits_per_word 语义计算。
 *	在 SPI 传输完成后，这个值可能会因为可用的采样精度而发生变化
 *	（例如 DMA 传输、@ptp_sts_supported 为 false 等）。
 * @ptp_sts_word_post: 参见 @ptp_sts_word_pre。两者可以相等，
 *	表示只需要对单个字节采样。
 *	如果由 core 来负责时间戳（即该控制器的 @ptp_sts_supported
 *	为 false），那么它会把 @ptp_sts_word_pre 设为 0，并把
 *	@ptp_sts_word_post 设为传输长度。这样做是有意为之，
 *	而不是设成 spi_transfer->len - 1，这样可以表达“驱动内部采样”
 *	仍可能有更高质量。
 * @ptp_sts: SPI 目标设备持有的一块内存，其中可以保存 PTP 系统
 *	timestamp 结构。若驱动使用 PIO，或者硬件本身提供了某种
 *	辅助机制来获取精确的传输时序，它们可以（也应该）将
 *	@ptp_sts_supported 设为 true，并通过 ptp_read_system_*ts
 *	辅助函数填充这个结构。
 *	时间戳必须表示 SPI 目标设备处理完该 word 的时刻；也就是说，
 *	“pre” 时间戳应在发送 “pre” word 之前采样，而 “post” 时间戳
 *	应在收到控制器对 “post” word 的发送确认之后采样。
 * @dtr_mode: 如果支持双倍传输速率，则为 true。
 * @timestamped: 如果这次传输已经被采样时间戳，则为 true
 * @error: SPI 控制器驱动记录的错误状态。
 *
 * SPI 传输写出的字节数总是和读入的字节数相同。
 * 协议驱动应始终提供 @rx_buf 和/或 @tx_buf。
 * 在某些情况下，它们也可以为正在传输的数据提供 DMA 地址；
 * 如果底层驱动使用 DMA，这样做可以减少开销。
 *
 * 如果发送缓冲区为 NULL，就会在填充 @rx_buf 的同时移出全零。
 * 如果接收缓冲区为 NULL，读入的数据会被丢弃。只有 "len" 字节会
 * 被移出（或移入）。试图移出一个半字是错误的。例如，如果字长是
 * 16 位或 20 位，却只移出 3 个字节，就属于错误；前者每字占 2 字节，
 * 后者每字占 4 字节。
 *
 * 内存中的数据值始终采用本机 CPU 字节序，它们由线缆上的字节序
 * 转换而来（SPI_LSB_FIRST 以外通常为大端序）。例如当 bits_per_word
 * 等于 16 时，缓冲区长度是 2N 字节
 *
 * 当 SPI 传输的每字位数不是 8 的幂次倍时，这些内存中的字会包含
 * 额外的 bit。协议驱动看到的内存字始终是右对齐的，因此未定义的
 * （rx）或未使用的（tx）bit 总是放在最高位。
 *
 * 所有 SPI 传输都从对应 chip select 处于有效状态开始。通常它会
 * 一直保持选中，直到消息中的最后一次传输完成。驱动可以通过
 * cs_change 来影响 chip select 信号。
 *
 * （i）如果这次传输不是消息中的最后一次，设置该标志会让 chip select
 * 在消息中间短暂失效。这样切换 chip select 有时是终止芯片命令所必需的，
 * 从而让一个 spi_message 一次完成一组相关的芯片事务。
 *
 * （ii）当这次传输是消息中的最后一次时，芯片可能会一直保持选中状态，
 * 直到下一次传输开始。对于没有阻塞消息去往其他设备的多设备 SPI 总线，
 * 这只是一个性能提示；一旦启动发往另一设备的消息，这个设备就会被取消选择。
 * 但在其它场景下，这也可以用来保证正确性。有些设备需要把协议事务拆成一串
 * spi_message 提交，每条消息的内容由前一条消息的结果决定，而整个事务会在
 * chip select 失效时结束。
 *
 * 当 SPI 支持 1x、2x 或 4x 传输时，可通过 @tx_nbits 和 @rx_nbits
 * 从设备传入这些传输信息。双向传输时，这两个字段都应该设置。
 * 用户可以用 SPI_NBITS_SINGLE(1x)、SPI_NBITS_DUAL(2x) 和
 * SPI_NBITS_QUAD(4x) 来支持这三种传输模式。
 *
 * 用户还可以把 dtr_mode 设为 true 来启用双倍数据率传输模式。
 * 如果不设置，默认将使用单倍数据率模式。
 *
 * 提交 spi_message（以及它的 spi_transfer）到下层代码的驱动，
 * 负责管理这些对象的内存。除非显式设置了每个字段，否则请先将其
 * 全部清零，以便未来 API 扩展时仍能保持兼容。提交消息和传输后，
 * 在完成回调触发前不要再修改它们。
 */
struct spi_transfer {
	/*
 * tx_buf == rx_buf 也是允许的。
 * 对于 MicroWire，一侧缓冲区必须为 NULL。
 * 缓冲区必须能够配合 dma_*map_single() 调用正常工作。
	 */
	const void	*tx_buf;
	void		*rx_buf;
	unsigned	len;

#define SPI_TRANS_FAIL_NO_START	BIT(0)
#define SPI_TRANS_FAIL_IO	BIT(1)
	u16		error;

	bool		tx_sg_mapped;
	bool		rx_sg_mapped;

	struct sg_table tx_sg;
	struct sg_table rx_sg;
	dma_addr_t	tx_dma;
	dma_addr_t	rx_dma;

	unsigned	dummy_data:1;
	unsigned	cs_off:1;
	unsigned	cs_change:1;
	unsigned	tx_nbits:4;
	unsigned	rx_nbits:4;

#define SPI_MULTI_LANE_MODE_SINGLE	0 /* only use single lane */
#define SPI_MULTI_LANE_MODE_STRIPE	1 /* one data word per lane */
#define SPI_MULTI_LANE_MODE_MIRROR	2 /* same word sent on all lanes */
	unsigned	multi_lane_mode: 2;

	unsigned	timestamped:1;
	bool		dtr_mode;
#define	SPI_NBITS_SINGLE	0x01 /* 1-bit transfer */
#define	SPI_NBITS_DUAL		0x02 /* 2-bit transfer */
#define	SPI_NBITS_QUAD		0x04 /* 4-bit transfer */
#define	SPI_NBITS_OCTAL	0x08 /* 8-bit transfer */
	u8		bits_per_word;
	struct spi_delay	delay;
	struct spi_delay	cs_change_delay;
	struct spi_delay	word_delay;
	u32		speed_hz;

	u32		effective_speed_hz;

	/* Use %SPI_OFFLOAD_XFER_* from spi-offload.h */
	unsigned int	offload_flags;

	unsigned int	ptp_sts_word_pre;
	unsigned int	ptp_sts_word_post;

	struct ptp_system_timestamp *ptp_sts;

	struct list_head transfer_list;
};

/**
 * struct spi_message - 一条多段 SPI 事务
 * @transfers: 这条事务中的传输段链表
 * @spi: 该事务被排队到哪个 SPI 设备
 * @pre_optimized: 协议驱动已经预先优化了这条消息
 * @optimized: 这条消息当前处于优化状态
 * @prepared: 已经为这条消息调用过 spi_prepare_message()
 * @status: 成功为 0，否则为负 errno
 * @complete: 用于报告事务完成的回调
 * @context: 调用 complete() 时传入的参数
 * @frame_length: 这条消息中的总字节数
 * @actual_length: 所有成功传输段实际传输的总字节数
 * @queue: 由当前拥有该消息的驱动使用
 * @state: 由当前拥有该消息的驱动使用
 * @opt_state: 由当前拥有该消息的驱动使用
 * @resources: SPI 消息处理期间使用的资源管理链表
 * @offload: （可选）本消息使用的 offload 实例
 *
 * @spi_message 用于执行一串原子性的传输操作，每个操作由一个
 * struct spi_transfer 表示。这里说“原子性”，是指在这串事务完成前，
 * 其他 spi_message 不能占用同一条 SPI 总线。在某些系统上，许多这样的
 * 序列可以合并成一次程序化 DMA 传输。在所有系统上，这些消息都会排队，
 * 并且可能在其他设备事务之后才完成。发送到同一个 spi_device 的消息，
 * 总是按 FIFO 顺序执行。
 *
 * 提交 spi_message（以及它的 spi_transfers）到下层的代码，需要负责
 * 管理这些对象的内存。没有显式设置的字段应先清零，以便兼容未来的
 * API 扩展。提交消息及其传输后，在完成回调触发前不要再去修改它们。
 */
struct spi_message {
	struct list_head	transfers;

	struct spi_device	*spi;

	/* 这个消息调用过 spi_optimize_message()。 */
	bool			pre_optimized;
	/* 这个消息已经执行过 __spi_optimize_message()。 */
	bool			optimized;

	/* 这个消息已经调用过 spi_prepare_message()。 */
	bool			prepared;

	/*
	 * REVISIT：我们也许会想引入一个标志，用来影响最后一次传输的
	 * 行为……例如“先读 16 位长度 L”，然后立刻“读 L 个字节”。
	 * 本质上是在施加一种特定的消息调度算法。
	 *
	 * 某些控制器驱动（一次只处理一条消息的队列）可以把它作为默认
	 * 调度策略，但另外一些（具有多消息流水线的）驱动可能需要一个
	 * 标志来告诉它们这种特殊情况。
	 */

	/* 完成通过回调报告。 */
	int			status;
	void			(*complete)(void *context);
	void			*context;
	unsigned		frame_length;
	unsigned		actual_length;

	/*
	 * 可选地由当前拥有 spi_message 的驱动使用……
	 * 在 spi_async 和后续 complete() 之间，这通常是
	 * spi_controller 控制器驱动。
	 */
	struct list_head	queue;
	void			*state;
	/*
	 * 控制器驱动可选使用的状态，位于
	 * __spi_optimize_message() 和 __spi_unoptimize_message()
	 * 之间。
	 */
	void			*opt_state;

	/*
	 * 本消息使用的可选 offload 实例。必须由外设驱动在调用
	 * spi_optimize_message() 之前设置好。
	 */
	struct spi_offload	*offload;

	/* SPI 消息处理期间使用的 spi_res 资源链表。 */
	struct list_head        resources;
};

static inline void spi_message_init_no_memset(struct spi_message *m)
{
	INIT_LIST_HEAD(&m->transfers);
	INIT_LIST_HEAD(&m->resources);
}

static inline void spi_message_init(struct spi_message *m)
{
	memset(m, 0, sizeof *m);
	spi_message_init_no_memset(m);
}

static inline void
spi_message_add_tail(struct spi_transfer *t, struct spi_message *m)
{
	list_add_tail(&t->transfer_list, &m->transfers);
}

static inline void
spi_transfer_del(struct spi_transfer *t)
{
	list_del(&t->transfer_list);
}

static inline int
spi_transfer_delay_exec(struct spi_transfer *t)
{
	return spi_delay_exec(&t->delay, t);
}

/**
 * spi_message_init_with_transfers - 初始化 spi_message 并追加传输段
 * @m: 待初始化的 spi_message
 * @xfers: SPI 传输数组
 * @num_xfers: 传输数组中的元素个数
 *
 * 先初始化给定的 spi_message，然后把数组中的每个 spi_transfer
 * 依次加入消息链表。
 */
static inline void
spi_message_init_with_transfers(struct spi_message *m,
struct spi_transfer *xfers, unsigned int num_xfers)
{
	unsigned int i;

	spi_message_init(m);
	for (i = 0; i < num_xfers; ++i)
		spi_message_add_tail(&xfers[i], m);
}

/*
 * 只要在它们仍被使用时不释放，把 message 和 transaction 结构体
 * 嵌入到其它数据结构里是完全可以的。
 */
static inline struct spi_message *spi_message_alloc(unsigned ntrans, gfp_t flags)
{
	struct spi_message_with_transfers {
		struct spi_message m;
		struct spi_transfer t[];
	} *mwt;
	unsigned i;

	mwt = kzalloc_flex(*mwt, t, ntrans, flags);
	if (!mwt)
		return NULL;

	spi_message_init_no_memset(&mwt->m);
	for (i = 0; i < ntrans; i++)
		spi_message_add_tail(&mwt->t[i], &mwt->m);

	return &mwt->m;
}

static inline void spi_message_free(struct spi_message *m)
{
	kfree(m);
}

extern int spi_optimize_message(struct spi_device *spi, struct spi_message *msg);
extern void spi_unoptimize_message(struct spi_message *msg);
extern int devm_spi_optimize_message(struct device *dev, struct spi_device *spi,
				     struct spi_message *msg);

extern int spi_setup(struct spi_device *spi);
extern int spi_async(struct spi_device *spi, struct spi_message *message);
extern int spi_target_abort(struct spi_device *spi);

static inline size_t
spi_max_message_size(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;

	if (!ctlr->max_message_size)
		return SIZE_MAX;
	return ctlr->max_message_size(spi);
}

static inline size_t
spi_max_transfer_size(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;
	size_t tr_max = SIZE_MAX;
	size_t msg_max = spi_max_message_size(spi);

	if (ctlr->max_transfer_size)
		tr_max = ctlr->max_transfer_size(spi);

	/* 传输长度上限不能大于消息长度上限。 */
	return min(tr_max, msg_max);
}

/**
 * spi_is_bpw_supported - 检查是否支持指定的每字位数
 * @spi: SPI 设备
 * @bpw: 每字位数
 *
 * 判断当前 SPI 控制器是否支持 @bpw。
 *
 * 返回：
 * 支持则返回 true，否则返回 false。
 */
static inline bool spi_is_bpw_supported(struct spi_device *spi, u32 bpw)
{
	u32 bpw_mask = spi->controller->bits_per_word_mask;

	if (bpw == 8 || (bpw <= 32 && bpw_mask & SPI_BPW_MASK(bpw)))
		return true;

	return false;
}

/**
 * spi_bpw_to_bytes - 将每字位数转换为字节数
 * @bpw: 每字位数
 *
 * 把给定的 @bpw 转换成字节数。结果总是 2 的幂，例如：
 *
 *  ===============    =================
 *  输入（bit）         输出（byte）
 *  ===============    =================
 *          5                   1
 *          9                   2
 *          21                  4
 *          37                  8
 *  ===============    =================
 *
 * 输入为 0 时返回 0。
 *
 * 返回：
 * 给定 @bpw 对应的字节数。
 */
static inline u32 spi_bpw_to_bytes(u32 bpw)
{
	return roundup_pow_of_two(BITS_TO_BYTES(bpw));
}

/**
 * spi_controller_xfer_timeout - 计算合适的传输超时时间
 * @ctlr: SPI 控制器
 * @xfer: 传输描述符
 *
 * 根据当前传输的长度和速率估算一个超时值。这里先按单条数据线
 * 的耗时来估算，再乘以 2，并设置最小值 500ms，以避免系统负载
 * 较高时产生误判。
 *
 * 返回：
 * 以毫秒为单位的传输超时时间。
 */
static inline unsigned int spi_controller_xfer_timeout(struct spi_controller *ctlr,
						       struct spi_transfer *xfer)
{
	return max(xfer->len * 8 * 2 / (xfer->speed_hz / 1000), 500U);
}

/*---------------------------------------------------------------------------*/

/* 使用 spi_res 的 SPI 传输替换方法。 */

struct spi_replaced_transfers;
typedef void (*spi_replaced_release_t)(struct spi_controller *ctlr,
				       struct spi_message *msg,
				       struct spi_replaced_transfers *res);
/**
 * struct spi_replaced_transfers - 描述被替换的 spi_transfer 及其回滚信息
 * @release: 释放该结构体前需要额外执行的回调
 * @extradata: 额外数据指针；如果没有申请则为 NULL
 * @replaced_transfers: 已被替换、需要恢复的传输项
 * @replaced_after: 需要把 @replaced_transfers 重新插回去的位置
 * @inserted: 新插入的传输个数
 * @inserted_transfers: 大小为 @inserted 的 spi_transfer 数组，用于替代
 *                      被替换的传输
 *
 * 注意：如果额外申请了空间，@extradata 会指向
 * @inserted_transfers[@inserted]，因此其对齐方式与 spi_transfer 相同。
 */
struct spi_replaced_transfers {
	spi_replaced_release_t release;
	void *extradata;
	struct list_head replaced_transfers;
	struct list_head *replaced_after;
	size_t inserted;
	struct spi_transfer inserted_transfers[];
};

/*---------------------------------------------------------------------------*/

/* SPI 传输转换方法。 */

extern int spi_split_transfers_maxsize(struct spi_controller *ctlr,
				       struct spi_message *msg,
				       size_t maxsize);
extern int spi_split_transfers_maxwords(struct spi_controller *ctlr,
					struct spi_message *msg,
					size_t maxwords);

/*---------------------------------------------------------------------------*/

/*
 * 下面这些同步 SPI 传输接口，都是构建在核心异步传输原语之上的
 * 便利封装。这里的“同步”表示它们会一直睡眠等待，直到异步传输完成。
 */

extern int spi_sync(struct spi_device *spi, struct spi_message *message);
extern int spi_sync_locked(struct spi_device *spi, struct spi_message *message);
extern int spi_bus_lock(struct spi_controller *ctlr);
extern int spi_bus_unlock(struct spi_controller *ctlr);

/**
 * spi_sync_transfer - 同步 SPI 数据传输
 * @spi: 要交换数据的设备
 * @xfers: spi_transfer 数组
 * @num_xfers: 传输数组中的元素个数
 * Context: can sleep
 *
 * 对给定的 spi_transfer 数组执行一次同步 SPI 数据传输。
 *
 * 更具体的语义请参考 spi_sync()。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static inline int
spi_sync_transfer(struct spi_device *spi, struct spi_transfer *xfers,
	unsigned int num_xfers)
{
	struct spi_message msg;

	spi_message_init_with_transfers(&msg, xfers, num_xfers);

	return spi_sync(spi, &msg);
}

/**
 * spi_write - SPI 同步写
 * @spi: 目标设备
 * @buf: 数据缓冲区
 * @len: 数据缓冲区长度
 * Context: can sleep
 *
 * 将缓冲区 @buf 中的数据写到设备。
 * 只能在可睡眠上下文中调用。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static inline int
spi_write(struct spi_device *spi, const void *buf, size_t len)
{
	struct spi_transfer	t = {
			.tx_buf		= buf,
			.len		= len,
		};

	return spi_sync_transfer(spi, &t, 1);
}

/**
 * spi_read - SPI 同步读
 * @spi: 数据来源设备
 * @buf: 数据缓冲区
 * @len: 数据缓冲区长度
 * Context: can sleep
 *
 * 从设备读取数据到缓冲区 @buf。
 * 只能在可睡眠上下文中调用。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static inline int
spi_read(struct spi_device *spi, void *buf, size_t len)
{
	struct spi_transfer	t = {
			.rx_buf		= buf,
			.len		= len,
		};

	return spi_sync_transfer(spi, &t, 1);
}

/* 这里只会复制 txbuf 和 rxbuf，适合小数据量传输。 */
extern int spi_write_then_read(struct spi_device *spi,
		const void *txbuf, unsigned n_tx,
		void *rxbuf, unsigned n_rx);

/**
 * spi_w8r8 - SPI 同步 8 位写后接 8 位读
 * @spi: 要交换数据的设备
 * @cmd: 读回数据前要先写入的命令字
 * Context: can sleep
 *
 * 只能在可睡眠上下文中调用。
 *
 * 返回：
 * 设备返回的 8 位无符号值；失败时返回负错误码。
 */
static inline ssize_t spi_w8r8(struct spi_device *spi, u8 cmd)
{
	ssize_t			status;
	u8			result;

	status = spi_write_then_read(spi, &cmd, 1, &result, 1);

	/* 返回负 errno，或者无符号结果值。 */
	return (status < 0) ? status : result;
}

/**
 * spi_w8r16 - SPI 同步 8 位写后接 16 位读
 * @spi: 要交换数据的设备
 * @cmd: 读回数据前要先写入的命令字
 * Context: can sleep
 *
 * 返回值按线缆上的字节序解释，很多设备实际使用的是大端序。
 *
 * 只能在可睡眠上下文中调用。
 *
 * 返回：
 * 设备返回的 16 位无符号值；失败时返回负错误码。
 */
static inline ssize_t spi_w8r16(struct spi_device *spi, u8 cmd)
{
	ssize_t			status;
	u16			result;

	status = spi_write_then_read(spi, &cmd, 1, &result, 2);

	/* 返回负 errno，或者无符号结果值。 */
	return (status < 0) ? status : result;
}

/**
 * spi_w8r16be - SPI 同步 8 位写后接 16 位大端读
 * @spi: 要交换数据的设备
 * @cmd: 读回数据前要先写入的命令字
 * Context: can sleep
 *
 * 这个函数与 spi_w8r16 类似，不同之处在于它会把读回的 16 位数据
 * 从大端序转换为本机 CPU 字节序。
 *
 * 只能在可睡眠上下文中调用。
 *
 * 返回：
 * 以 CPU 字节序表示的 16 位无符号值；失败时返回负错误码。
 */
static inline ssize_t spi_w8r16be(struct spi_device *spi, u8 cmd)

{
	ssize_t status;
	__be16 result;

	status = spi_write_then_read(spi, &cmd, 1, &result, 2);
	if (status < 0)
		return status;

	return be16_to_cpu(result);
}

/*---------------------------------------------------------------------------*/

/*
 * 板级初始化代码与 SPI 基础设施之间的接口。
 *
 * 这些 SPI 设备表片段不会被任何普通 SPI 驱动直接看到，
 * 但它们是 SPI core（或者后续热插拔上来的适配器）扩展
 * 驱动模型树的方式。
 *
 * 一般来说，SPI 设备不会像平台设备那样自动探测；相反，
 * 板级初始化代码会提供一张设备表，列出当前存在的芯片，
 * 并给出足够的信息让驱动完成绑定和初始化。
 * 对非静态配置也提供了基础支持，例如并口适配器或
 * 充当 USB-to-SPI 桥的微控制器。
 */

/**
 * struct spi_board_info - SPI 设备的板级模板
 * @modalias: 初始化 spi_device.modalias，用于识别驱动。
 * @platform_data: 初始化 spi_device.platform_data；其中保存的数据
 *	由具体驱动决定。
 * @swnode: 设备的软件节点。
 * @controller_data: 初始化 spi_device.controller_data；有些控制器
 *	会需要硬件配置提示，例如 DMA。
 * @irq: 初始化 spi_device.irq；取决于板级连线方式。
 * @max_speed_hz: 初始化 spi_device.max_speed_hz；由芯片手册限制和
 *	板级信号质量共同决定。
 * @bus_num: 指明哪个 spi_controller 作为该 spi_device 的父控制器；
 *	spi_new_device() 不使用它，其他情况下则取决于板级布线。
 * @chip_select: 初始化 spi_device.chip_select；取决于板级连线。
 * @mode: 初始化 spi_device.mode；由芯片手册、板级布线（例如某些
 *	设备同时支持 3WIRE 和标准模式），以及片选路径上是否存在
 *	反相器共同决定。
 *
 * 当向设备树添加新的 SPI 设备时，这些结构体相当于一个部分模板。
 * 它们保存的是驱动无法总是自行推导出来的信息。那些 probe() 阶段
 * 能确定的信息（例如默认传输字长）不放在这里。
 *
 * 这些结构体有两个用途。主要用途是存放在板级设备描述表中，
 * 这类表通常在板级初始化早期定义，之后在控制器驱动初始化完成后
 * 再用来填充控制器的设备树。另一个次要且不常见的用途，是作为
 * spi_new_device() 的参数，用于某些动态板级配置模型。
 */
struct spi_board_info {
	/*
	 * 设备名和模块名是绑定的，就像 platform_bus 一样；
	 * "modalias" 通常就是驱动名。
	 *
	 * platform_data 会写入 spi_device.dev.platform_data，
	 * controller_data 会写入 spi_device.controller_data，
	 * IRQ 也会一并复制。
	 */
	char		modalias[SPI_NAME_SIZE];
	const void	*platform_data;
	const struct software_node *swnode;
	void		*controller_data;
	int		irq;

	/* 在噪声较大或低电压板上使用较慢的信号速率。 */
	u32		max_speed_hz;


	/*
	 * bus_num 是板级相关的，会匹配某个稍后可能注册的
	 * spi_controller 的 bus_num。
	 *
	 * chip_select 反映该芯片如何连到控制器上；它必须小于
	 * num_chipselect。
	 */
	u16		bus_num;
	u16		chip_select;

	/*
	 * mode 会成为 spi_device.mode；对那些默认 SPI_CS_HIGH=0
	 * 不正确的芯片来说，这个字段非常关键。
	 */
	u32		mode;

	/*
	 * 这里以后可能还会加入更多 spi_device 级别的芯片配置数据。
	 * 尽量避免放那些协议驱动自己就能设置的内容；但应包含那些
	 * 在未绑定驱动时仍然需要生效的配置，例如：
	 *  - 未被选中时，时钟频率仍然会影响芯片行为的特殊约束
	 */
};

#ifdef	CONFIG_SPI
extern int
spi_register_board_info(struct spi_board_info const *info, unsigned n);
#else
/* 板级初始化代码可以不关心 SPI 是否已经启用。 */
static inline int
spi_register_board_info(struct spi_board_info const *info, unsigned n)
	{ return 0; }
#endif

/*
 * 如果你在热插拔适配器上挂载设备（例如并口、USB 等），
 * 应使用 spi_new_device() 来描述每一个设备。也可以调用
 * spi_unregister_device() 让该设备开始消失，不过通常这类工作
 * 会由 spi_unregister_controller() 统一处理。
 *
 * 也可以用 spi_alloc_device() 和 spi_add_device() 走两阶段注册流程。
 * 这样调用者在注册前能对 spi_device 进行更多定制，不过也意味着
 * 调用者必须自己填好那些原本会由 board info 指定的字段。
 */
extern struct spi_device *
spi_alloc_device(struct spi_controller *ctlr);

extern int
spi_add_device(struct spi_device *spi);

extern struct spi_device *
spi_new_device(struct spi_controller *, struct spi_board_info *);

extern void spi_unregister_device(struct spi_device *spi);

extern const struct spi_device_id *
spi_get_device_id(const struct spi_device *sdev);

extern const void *
spi_get_device_match_data(const struct spi_device *sdev);

static inline bool
spi_transfer_is_last(struct spi_controller *ctlr, struct spi_transfer *xfer)
{
	return list_is_last(&xfer->transfer_list, &ctlr->cur_msg->transfers);
}

#endif /* __LINUX_SPI_H */
