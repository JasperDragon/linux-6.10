/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * i2c.h - Linux I2C 总线接口定义
 *
 * 这个头文件定义了 I2C 核心对外暴露的公共对象和访问接口：
 * - I2C 客户端、适配器、驱动的基础数据结构
 * - I2C/SMBus 主机读写快捷接口
 * - 设备实例化、驱动匹配、总线锁和时序信息
 * Copyright (C) 1995-2000 Simon G. Vogl
 * Copyright (C) 2013-2019 Wolfram Sang <wsa@kernel.org>
 *
 * 其中也吸收了 Kyösti Mälkki <kmalkki@cc.hut.fi> 和
 * Frodo Looijaard <frodol@dds.nl> 的部分修改
 */
#ifndef _LINUX_I2C_H
#define _LINUX_I2C_H

#include <linux/acpi.h>		/* acpi_handle */
#include <linux/bits.h>
#include <linux/mod_devicetable.h>
#include <linux/device.h>	/* struct device */
#include <linux/sched.h>	/* completion */
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/rtmutex.h>
#include <linux/irqdomain.h>	/* Host Notify IRQ */
#include <linux/of.h>		/* struct device_node */
#include <linux/swab.h>		/* swab16 */
#include <uapi/linux/i2c.h>

extern const struct bus_type i2c_bus_type;
extern const struct device_type i2c_adapter_type;
extern const struct device_type i2c_client_type;

/* ---------------------------------------------------------------------
 * 通用声明与基础类型
 * --------------------------------------------------------------------- */

struct i2c_msg;
struct i2c_adapter;
struct i2c_client;
struct i2c_driver;
struct i2c_device_identity;
union i2c_smbus_data;
struct i2c_board_info;
enum i2c_slave_event;
typedef int (*i2c_slave_cb_t)(struct i2c_client *client,
			      enum i2c_slave_event event, u8 *val);

/* I2C 标准频率档位。这里用于把总线速率映射成可读字符串。 */
#define I2C_MAX_STANDARD_MODE_FREQ	100000
#define I2C_MAX_FAST_MODE_FREQ		400000
#define I2C_MAX_FAST_MODE_PLUS_FREQ	1000000
#define I2C_MAX_TURBO_MODE_FREQ		1400000
#define I2C_MAX_HIGH_SPEED_MODE_FREQ	3400000
#define I2C_MAX_ULTRA_FAST_MODE_FREQ	5000000

struct module;
struct property_entry;

#if IS_ENABLED(CONFIG_I2C)
/* 根据总线频率返回对应的 I2C 标准模式字符串。 */
const char *i2c_freq_mode_string(u32 bus_freq_hz);

/*
 * 主机收发基础接口。
 *
 * 这组接口是驱动里最常用的 I2C 访问路径：
 * - 发送/接收单个消息
 * - 通过一个缓冲区完成一次收或发
 * - 支持普通缓冲区和 DMA 安全缓冲区两种使用方式
 *
 * @count 不能超过 64KiB，因为底层 msg.len 是 u16。
 */
int i2c_transfer_buffer_flags(const struct i2c_client *client,
			      char *buf, int count, u16 flags);

/**
 * i2c_master_recv - 以主机接收方式执行一次 I2C 传输
 * @client: 从设备句柄
 * @buf:    用于接收数据的缓冲区
 * @count:  要读取的字节数；因为 msg.len 是 u16，所以必须小于 64 KiB
 *
 * 这是对 i2c_transfer_buffer_flags(..., I2C_M_RD) 的便捷封装，适合
 * 驱动直接做一次简单读操作。
 *
 * Return: 负 errno，或成功读取到的字节数
 */
static inline int i2c_master_recv(const struct i2c_client *client,
				  char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, buf, count, I2C_M_RD);
};

/**
 * i2c_master_recv_dmasafe - 使用 DMA 安全缓冲区接收一次 I2C 数据
 * @client: 从设备句柄
 * @buf:    用于接收数据的 DMA 安全缓冲区
 * @count:  要读取的字节数；因为 msg.len 是 u16，所以必须小于 64 KiB
 *
 * 和 i2c_master_recv() 的区别仅在于这里会带上 I2C_M_DMA_SAFE 标志，
 * 让底层控制器驱动可以直接把这个缓冲区交给 DMA。
 *
 * Return: 负 errno，或成功读取到的字节数
 */
static inline int i2c_master_recv_dmasafe(const struct i2c_client *client,
					  char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, buf, count,
					 I2C_M_RD | I2C_M_DMA_SAFE);
};

/**
 * i2c_master_send - 以主机发送方式执行一次 I2C 传输
 * @client: 从设备句柄
 * @buf:    要写给从设备的数据缓冲区
 * @count:  要写入的字节数；因为 msg.len 是 u16，所以必须小于 64 KiB
 *
 * Return: 负 errno，或成功写入的字节数
 */
static inline int i2c_master_send(const struct i2c_client *client,
				  const char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, (char *)buf, count, 0);
};

/**
 * i2c_master_send_dmasafe - 使用 DMA 安全缓冲区发送一次 I2C 数据
 * @client: 从设备句柄
 * @buf:    要写给从设备的 DMA 安全缓冲区
 * @count:  要写入的字节数；因为 msg.len 是 u16，所以必须小于 64 KiB
 *
 * Return: 负 errno，或成功写入的字节数
 */
static inline int i2c_master_send_dmasafe(const struct i2c_client *client,
					  const char *buf, int count)
{
	return i2c_transfer_buffer_flags(client, (char *)buf, count,
					 I2C_M_DMA_SAFE);
};

/* 执行由 num 个消息组成的 I2C 组合传输。 */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
/* 不加总线外层锁的版本，供内核内部在已持锁时使用。 */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);

/*
 * 最通用的 SMBus 访问入口。
 *
 * 一般驱动更适合直接调用下面那些封装好的读写函数，而不是自己拼装
 * protocol 参数。不过这个入口保留了最完整的 SMBus 访问能力。
 *
 * 这里使用 i2c_adapter 作为参数，是因为不需要“专门的 SMBus 适配器”。
 */
s32 i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		   unsigned short flags, char read_write, u8 command,
		   int protocol, union i2c_smbus_data *data);

/* 不加总线锁的 SMBus 版本。 */
s32 __i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		     unsigned short flags, char read_write, u8 command,
		     int protocol, union i2c_smbus_data *data);

/*
 * 下面是一组更“顺手”的 SMBus 访问封装。
 *
 * 它们既简化调用，也顺带表达了 i2c_smbus_xfer 的调用约定。
 */

u8 i2c_smbus_pec(u8 crc, u8 *p, size_t count);
s32 i2c_smbus_read_byte(const struct i2c_client *client);
s32 i2c_smbus_write_byte(const struct i2c_client *client, u8 value);
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_byte_data(const struct i2c_client *client,
			      u8 command, u8 value);
s32 i2c_smbus_read_word_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_word_data(const struct i2c_client *client,
			      u8 command, u16 value);

static inline s32
i2c_smbus_read_word_swapped(const struct i2c_client *client, u8 command)
{
	s32 value = i2c_smbus_read_word_data(client, command);

	return (value < 0) ? value : swab16(value);
}

static inline s32
i2c_smbus_write_word_swapped(const struct i2c_client *client,
			     u8 command, u16 value)
{
	return i2c_smbus_write_word_data(client, command, swab16(value));
}

/* 返回读取到的字节数。 */
s32 i2c_smbus_read_block_data(const struct i2c_client *client,
			      u8 command, u8 *values);
s32 i2c_smbus_write_block_data(const struct i2c_client *client,
			       u8 command, u8 length, const u8 *values);
/* 返回读取到的字节数。 */
s32 i2c_smbus_read_i2c_block_data(const struct i2c_client *client,
				  u8 command, u8 length, u8 *values);
s32 i2c_smbus_write_i2c_block_data(const struct i2c_client *client,
				   u8 command, u8 length, const u8 *values);
s32 i2c_smbus_read_i2c_block_data_or_emulated(const struct i2c_client *client,
					      u8 command, u8 length,
					      u8 *values);
int i2c_get_device_id(const struct i2c_client *client,
		      struct i2c_device_identity *id);
const struct i2c_device_id *i2c_client_get_device_id(const struct i2c_client *client);
#endif /* I2C */

/**
 * struct i2c_device_identity - I2C Device ID 规范返回的设备身份信息
 * @manufacturer_id: 厂商 ID，范围 0..4095，由 NXP 维护分配
 * @part_id:         厂商内部的器件型号 ID，范围 0..511
 * @die_revision:    芯片修订号，范围 0..7
 */
struct i2c_device_identity {
	u16 manufacturer_id;
#define I2C_DEVICE_ID_NXP_SEMICONDUCTORS                0
#define I2C_DEVICE_ID_NXP_SEMICONDUCTORS_1              1
#define I2C_DEVICE_ID_NXP_SEMICONDUCTORS_2              2
#define I2C_DEVICE_ID_NXP_SEMICONDUCTORS_3              3
#define I2C_DEVICE_ID_RAMTRON_INTERNATIONAL             4
#define I2C_DEVICE_ID_ANALOG_DEVICES                    5
#define I2C_DEVICE_ID_STMICROELECTRONICS                6
#define I2C_DEVICE_ID_ON_SEMICONDUCTOR                  7
#define I2C_DEVICE_ID_SPRINTEK_CORPORATION              8
#define I2C_DEVICE_ID_ESPROS_PHOTONICS_AG               9
#define I2C_DEVICE_ID_FUJITSU_SEMICONDUCTOR            10
#define I2C_DEVICE_ID_FLIR                             11
#define I2C_DEVICE_ID_O2MICRO                          12
#define I2C_DEVICE_ID_ATMEL                            13
#define I2C_DEVICE_ID_NONE                         0xffff
	u16 part_id;
	u8 die_revision;
};

enum i2c_alert_protocol {
	I2C_PROTOCOL_SMBUS_ALERT,
	I2C_PROTOCOL_SMBUS_HOST_NOTIFY,
};

/**
 * enum i2c_driver_flags - I2C 设备驱动标志位
 *
 * @I2C_DRV_ACPI_WAIVE_D0_PROBE:
 *	在 ACPI 场景下，probe 前不要强制把设备切到 D0 电源状态。
 */
enum i2c_driver_flags {
	I2C_DRV_ACPI_WAIVE_D0_PROBE = BIT(0),
};

/**
 * struct i2c_driver - 表示一个 I2C 设备驱动
 *
 * 这就是 I2C 设备驱动的核心对象。它负责：
 * - 识别设备是否匹配
 * - 在 probe/remove 中完成资源申请与释放
 * - 处理 alert、command、detect 这类总线扩展能力
 * @class:        该驱动参与哪些 adapter class 的 detect 自动探测
 * @probe:        设备绑定时的回调
 * @remove:       设备解绑时的回调
 * @shutdown:     关机/重启路径回调
 * @alert:        SMBus Alert / Host Notify 这类总线侧告警回调
 * @command:      总线级广播命令回调，可选
 * @driver:       嵌入到 driver model 里的通用 device_driver
 * @id_table:     本驱动支持的 I2C 设备 ID 表
 * @detect:       自动探测回调
 * @address_list: detect() 允许扫描的地址列表
 * @clients:      通过 detect() 自动创建出来的 client 链表，仅 core 自用
 * @flags:        &enum i2c_driver_flags 定义的标志位集合
 *
 * `driver.owner` 通常应设为模块自身，`driver.name` 应设为驱动名。
 *
 * 如果要支持 detect() 自动探测，@detect 和 @address_list 必须同时提供，
 * 一般还要设置 @class；否则只有通过模块参数强制创建的设备才会走这条路。
 * detect() 成功时，至少要填充传入的 i2c_board_info.name，必要时也可
 * 一并填写 flags。
 *
 * 很多现代平台都通过 DT/ACPI/boardinfo 显式枚举设备，因此即使没有
 * @detect，驱动照样能正常工作；只是不会支持“扫地址猜设备”的旧式流程。
 *
 * 传给 detect() 的 i2c_client 不是一个完整、已注册的真实 client。
 * 它只被初始化到“足够做 SMBus 读写探测”的程度，不能对它做其它操作，
 * 尤其不能拿它去打 dev_dbg() 或访问完整设备模型状态。
 */
struct i2c_driver {
	unsigned int class;

	/* 标准 driver model 接口。 */
	int (*probe)(struct i2c_client *client);
	void (*remove)(struct i2c_client *client);


	/* 与枚举无直接关系的 driver model 接口。 */
	void (*shutdown)(struct i2c_client *client);

	/* Alert 回调，例如 SMBus Alert 协议。
	 * data 的含义取决于具体协议：
	 * - SMBus Alert: 仅使用 alert response 最低位的事件标志
	 * - SMBus Host Notify: data 对应从设备上报的 16 位负载数据
	 */
	void (*alert)(struct i2c_client *client, enum i2c_alert_protocol protocol,
		      unsigned int data);

	/* 类似 ioctl 的总线广播命令接口，可选。 */
	int (*command)(struct i2c_client *client, unsigned int cmd, void *arg);

	struct device_driver driver;
	const struct i2c_device_id *id_table;

	/* 自动创建设备时使用的 detect 回调。 */
	int (*detect)(struct i2c_client *client, struct i2c_board_info *info);
	const unsigned short *address_list;
	struct list_head clients;

	u32 flags;
};
#define to_i2c_driver(d) container_of_const(d, struct i2c_driver, driver)

/**
 * struct i2c_client - 表示一个 I2C 从设备实例
 *
 * 这不是“驱动”，而是总线上一个真实存在的设备节点。
 * 驱动 probe 到它以后，会围绕这个对象完成寄存器访问、IRQ、PM 等工作。
 * @flags:           I2C_CLIENT_* 标志位
 * @addr:            挂在父适配器这条总线上的从地址
 * @name:            设备类型名，通常取一个能覆盖兼容修订版本的通用芯片名
 * @adapter:         承载这个设备的总线适配器
 * @dev:             设备模型里的 struct device
 * @init_irq:        初始化时得到的 IRQ 号
 * @irq:             设备当前对外暴露的 IRQ 号
 * @detected:        挂在 i2c_driver.clients 或 core userspace 列表中的节点
 * @slave_cb:        适配器工作在 target/slave 模式时的事件回调
 * @devres_group_id: probe 期间为该设备创建的 devres 组 ID
 * @debugfs:         I2C core 为该 client 创建的 debugfs 目录
 *
 * 一个 i2c_client 就代表总线上的一个真实芯片实例。Linux 侧真正暴露出
 * 什么行为，取决于后续绑定到它的驱动。
 */
struct i2c_client {
	unsigned short flags;		/* 各类 I2C_CLIENT_* 标志位，见下。 */
#define I2C_CLIENT_PEC		0x04	/* 启用 PEC（Packet Error Checking） */
#define I2C_CLIENT_TEN		0x10	/* 该设备使用 10 位地址 */
					/* 必须与下面的 I2C_M_TEN 保持一致 */
#define I2C_CLIENT_SLAVE	0x20	/* 当前 client 工作在 target/slave 模式 */
#define I2C_CLIENT_HOST_NOTIFY	0x40	/* 启用 I2C Host Notify */
#define I2C_CLIENT_WAKE		0x80	/* board_info 标记：该设备具备唤醒能力 */
#define I2C_CLIENT_SCCB		0x9000	/* 使用 Omnivision SCCB 协议 */
					/* 必须匹配 I2C_M_STOP | I2C_M_IGNORE_NAK */

	unsigned short addr;		/* 芯片地址；7 位地址保存在低 7 位 */
	char name[I2C_NAME_SIZE];
	struct i2c_adapter *adapter;	/* 当前 client 所挂接的适配器 */
	struct device dev;		/* 设备模型里的 struct device */
	int init_irq;			/* 初始化阶段得到的 IRQ */
	int irq;			/* 设备当前使用的 IRQ */
	struct list_head detected;
#if IS_ENABLED(CONFIG_I2C_SLAVE)
	i2c_slave_cb_t slave_cb;	/* target/slave 模式回调 */
#endif
	void *devres_group_id;		/* probe 期间创建的 devres 组 ID */
	struct dentry *debugfs;		/* 该 client 对应的 debugfs 目录 */
};
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)

struct i2c_adapter *i2c_verify_adapter(struct device *dev);
const struct i2c_device_id *i2c_match_id(const struct i2c_device_id *id,
					 const struct i2c_client *client);

const void *i2c_get_match_data(const struct i2c_client *client);

static inline struct i2c_client *kobj_to_i2c_client(struct kobject *kobj)
{
	struct device * const dev = kobj_to_dev(kobj);
	return to_i2c_client(dev);
}

static inline void *i2c_get_clientdata(const struct i2c_client *client)
{
	return dev_get_drvdata(&client->dev);
}

static inline void i2c_set_clientdata(struct i2c_client *client, void *data)
{
	dev_set_drvdata(&client->dev, data);
}

/* I2C target/slave 模式支持。 */

enum i2c_slave_event {
	I2C_SLAVE_READ_REQUESTED,
	I2C_SLAVE_WRITE_REQUESTED,
	I2C_SLAVE_READ_PROCESSED,
	I2C_SLAVE_WRITE_RECEIVED,
	I2C_SLAVE_STOP,
};

int i2c_slave_register(struct i2c_client *client, i2c_slave_cb_t slave_cb);
int i2c_slave_unregister(struct i2c_client *client);
int i2c_slave_event(struct i2c_client *client,
		    enum i2c_slave_event event, u8 *val);
#if IS_ENABLED(CONFIG_I2C_SLAVE)
bool i2c_detect_slave_mode(struct device *dev);
#else
static inline bool i2c_detect_slave_mode(struct device *dev) { return false; }
#endif

/**
 * struct i2c_board_info - 设备创建模板
 *
 * 这个结构体用于描述一个“即将被实例化”的 I2C 设备。
 * 它既可来自板级静态描述，也可来自设备树、ACPI 或运行时创建。
 * @type:          初始化 i2c_client.name 使用的芯片类型名
 * @flags:         初始化 i2c_client.flags 使用的标志位
 * @addr:          初始化 i2c_client.addr 使用的地址
 * @dev_name:      如果设置，则覆盖默认的 `<busnr>-<addr>` 设备名
 * @platform_data: 写入 i2c_client.dev.platform_data 的平台私有数据
 * @fwnode:        平台固件提供的设备节点
 * @swnode:        设备的 software node
 * @resources:     与设备关联的资源数组
 * @num_resources: @resources 数组中的资源个数
 * @irq:           初始化 i2c_client.irq 使用的 IRQ
 *
 * I2C 本身不具备可靠的硬件枚举能力。即便某些控制器/设备能用
 * I2C_SMBUS_QUICK 粗略判断“这个地址上似乎有响应”，驱动通常仍然需要
 * 更多信息，例如芯片型号、板级配置、IRQ 连接方式等。
 *
 * i2c_board_info 就是用来描述这些“已知存在的设备模板”的。主板固件或
 * 板级代码可以先建立这样一份描述，之后由 I2C core 把它扩展成真正的
 * driver model 设备节点。
 */
struct i2c_board_info {
	char		type[I2C_NAME_SIZE];
	unsigned short	flags;
	unsigned short	addr;
	const char	*dev_name;
	void		*platform_data;
	struct fwnode_handle *fwnode;
	const struct software_node *swnode;
	const struct resource *resources;
	unsigned int	num_resources;
	int		irq;
};

/**
 * I2C_BOARD_INFO - 用于快速填充 i2c_board_info 的宏
 * @dev_type: 设备类型名
 * @dev_addr: 设备在总线上的地址
 *
 * 这个宏只初始化 i2c_board_info 的最小必需字段。IRQ、platform_data 等
 * 可选字段仍然由调用者继续按普通结构体初始化语法补充。
 */
#define I2C_BOARD_INFO(dev_type, dev_addr) \
	.type = dev_type, .addr = (dev_addr)


#if IS_ENABLED(CONFIG_I2C)
/*
 * 扩展板或热插拔场景应使用这组动态实例化/反实例化接口。
 * 典型例子包括带独立 EEPROM、传感器、codec 的附加板卡。
 */
struct i2c_client *
i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info);

/* 如果不知道设备的精确地址，可改用这个接口在一组候选地址中探测。
 * probe 回调是可选的；若提供，成功时必须返回 1，失败返回 0。
 * 如果不提供，则使用 I2C core 的默认探测方式。
 */
struct i2c_client *
i2c_new_scanned_device(struct i2c_adapter *adap,
		       struct i2c_board_info *info,
		       unsigned short const *addr_list,
		       int (*probe)(struct i2c_adapter *adap, unsigned short addr));

/* 常用的自定义 probe helper。 */
int i2c_probe_func_quick_read(struct i2c_adapter *adap, unsigned short addr);

struct i2c_client *
i2c_new_dummy_device(struct i2c_adapter *adapter, u16 address);

struct i2c_client *
devm_i2c_new_dummy_device(struct device *dev, struct i2c_adapter *adap, u16 address);

struct i2c_client *
i2c_new_ancillary_device(struct i2c_client *client,
			 const char *name,
			 u16 default_addr);

void i2c_unregister_device(struct i2c_client *client);

struct i2c_client *i2c_verify_client(struct device *dev);
#else
static inline struct i2c_client *i2c_verify_client(struct device *dev)
{
	return NULL;
}
#endif /* I2C */

/* 主板级 arch_initcall 代码通常应在适配器注册前预声明板载 I2C 设备。
 * 扩展板或热插拔场景则不适合这套静态路径，应改用其它动态接口。
 */
#ifdef CONFIG_I2C_BOARDINFO
int
i2c_register_board_info(int busnum, struct i2c_board_info const *info,
			unsigned n);
#else
static inline int
i2c_register_board_info(int busnum, struct i2c_board_info const *info,
			unsigned n)
{
	return 0;
}
#endif /* I2C_BOARDINFO */

/**
 * struct i2c_algorithm - 表示 I2C 控制器的传输算法
 *
 * 适配器驱动通过它向 I2C core 提供：
 * - 普通 I2C 消息传输
 * - SMBus 传输
 * - 功能位查询
 * - 可选的 target/slave 模式注册
 * @xfer:              通过指定适配器执行 msgs 数组定义的一组 I2C 消息
 * @xfer_atomic:       @xfer 的原子上下文版本，可选
 * @smbus_xfer:        执行原生 SMBus 事务；若为空，core 会尝试用 I2C 消息仿真
 * @smbus_xfer_atomic: @smbus_xfer 的原子上下文版本，可选
 * @functionality:     返回该适配器/算法支持的 I2C_FUNC_* 能力位
 * @reg_target:        把给定 client 注册到本适配器的 target/slave 模式
 * @unreg_target:      从本适配器的 target/slave 模式中注销给定 client
 *
 * @master_xfer:        已废弃，改用 @xfer
 * @master_xfer_atomic: 已废弃，改用 @xfer_atomic
 * @reg_slave:          已废弃，改用 @reg_target
 * @unreg_slave:        已废弃，改用 @unreg_target
 *
 * i2c_algorithm 抽象的是“一类能用同一套总线时序访问的硬件控制器实现”，
 * 例如 bit-banging 控制器，或者某类固定寄存器接口的 I2C IP。
 *
 * `xfer{_atomic}` 的返回值应尽量使用 I2C fault-codes 文档约定的错误码；
 * 如果没有错误，则返回实际执行完成的消息条数。
 */
struct i2c_algorithm {
	/*
	 * 如果某个算法无法提供 I2C 级别访问，就把 xfer 设为 NULL。
	 * 如果它能提供原生 SMBus 访问，就实现 smbus_xfer；
	 * 否则 I2C core 会尝试用普通 I2C 消息去仿真 SMBus 协议。
	 */
	union {
		int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
			    int num);
		int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
				   int num);
	};
	union {
		int (*xfer_atomic)(struct i2c_adapter *adap,
				   struct i2c_msg *msgs, int num);
		int (*master_xfer_atomic)(struct i2c_adapter *adap,
					   struct i2c_msg *msgs, int num);
	};
	int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr,
			  unsigned short flags, char read_write,
			  u8 command, int size, union i2c_smbus_data *data);
	int (*smbus_xfer_atomic)(struct i2c_adapter *adap, u16 addr,
				 unsigned short flags, char read_write,
				 u8 command, int size, union i2c_smbus_data *data);

	/* 用来声明这个适配器到底支持哪些事务类型。 */
	u32 (*functionality)(struct i2c_adapter *adap);

#if IS_ENABLED(CONFIG_I2C_SLAVE)
	union {
		int (*reg_target)(struct i2c_client *client);
		int (*reg_slave)(struct i2c_client *client);
	};
	union {
		int (*unreg_target)(struct i2c_client *client);
		int (*unreg_slave)(struct i2c_client *client);
	};
#endif
};

/**
 * struct i2c_lock_operations - I2C 总线锁操作集合
 *
 * 用于屏蔽 adapter 内部的锁实现差异。core 只关心“加锁/尝试加锁/解锁”
 * 这三个动作，不直接依赖具体锁类型。
 * @lock_bus:    获取某段 I2C 总线的独占访问权
 * @trylock_bus: 尝试获取某段 I2C 总线的独占访问权
 * @unlock_bus:  释放某段 I2C 总线的独占访问权
 *
 * 常规调用方通常不直接碰这组回调，而是通过 i2c_lock_bus() /
 * i2c_unlock_bus() 这层包装来使用。
 */
struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *adapter, unsigned int flags);
};

/**
 * struct i2c_timings - I2C 时序信息
 *
 * 这组字段描述总线频率、SCL/SDA 的上升下降时间、滤波器宽度等参数，
 * 供控制器驱动根据硬件能力和设备树/固件描述计算最终寄存器值。
 * @bus_freq_hz:                总线频率，单位 Hz
 * @scl_rise_ns:                SCL 上升时间，I2C 规范中的 t(r)
 * @scl_fall_ns:                SCL 下降时间，I2C 规范中的 t(f)
 * @scl_int_delay_ns:           控制器内部额外需要的 SCL 建立时间
 * @sda_fall_ns:                SDA 下降时间，I2C 规范中的 t(f)
 * @sda_hold_ns:                控制器内部额外需要维持的 SDA 保持时间
 * @digital_filter_width_ns:    数字滤波器可滤除毛刺的最大宽度
 * @analog_filter_cutoff_freq_hz: 模拟低通滤波器的截止频率
 */
struct i2c_timings {
	u32 bus_freq_hz;
	u32 scl_rise_ns;
	u32 scl_fall_ns;
	u32 scl_int_delay_ns;
	u32 sda_fall_ns;
	u32 sda_hold_ns;
	u32 digital_filter_width_ns;
	u32 analog_filter_cutoff_freq_hz;
};

/**
 * struct i2c_bus_recovery_info - I2C 总线恢复所需的回调和资源
 * @recover_bus:         恢复总线的主入口，可用驱动自定义实现或
 *			 i2c_generic_scl_recovery()
 * @get_scl:             读取 SCL 当前电平；通用 SCL 恢复必须提供
 * @set_scl:             拉高/拉低 SCL；通用 SCL 恢复必须提供
 * @get_sda:             读取 SDA 当前电平；和 @set_sda 至少提供一个
 * @set_sda:             拉高/拉低 SDA；和 @get_sda 至少提供一个
 * @get_bus_free:        用控制器视角判断总线是否空闲，可选
 * @prepare_recovery:    开始恢复前的预处理，例如切 padmux
 * @unprepare_recovery:  恢复完成后的收尾，例如恢复 padmux
 * @scl_gpiod:           SCL 的 GPIO 描述符，仅 GPIO 恢复需要
 * @sda_gpiod:           SDA 的 GPIO 描述符，仅 GPIO 恢复需要
 * @pinctrl:             GPIO 恢复时用于切换引脚状态的 pinctrl，可选
 * @pins_default:        I2C 正常工作时的 pinctrl 状态，可选
 * @pins_gpio:           用 GPIO 方式恢复时的 pinctrl 状态，可选
 */
struct i2c_bus_recovery_info {
	int (*recover_bus)(struct i2c_adapter *adap);

	int (*get_scl)(struct i2c_adapter *adap);
	void (*set_scl)(struct i2c_adapter *adap, int val);
	int (*get_sda)(struct i2c_adapter *adap);
	void (*set_sda)(struct i2c_adapter *adap, int val);
	int (*get_bus_free)(struct i2c_adapter *adap);

	void (*prepare_recovery)(struct i2c_adapter *adap);
	void (*unprepare_recovery)(struct i2c_adapter *adap);

	/* GPIO 方式恢复总线时需要的资源。 */
	struct gpio_desc *scl_gpiod;
	struct gpio_desc *sda_gpiod;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_gpio;
};

int i2c_recover_bus(struct i2c_adapter *adap);

/* 通用的总线恢复 helper。 */
int i2c_generic_scl_recovery(struct i2c_adapter *adap);

/**
 * struct i2c_adapter_quirks - 描述某个 I2C 控制器的能力缺陷或限制
 * @flags:               I2C_AQ_* 定义的限制标志
 * @max_num_msgs:        一次传输允许的最大消息数
 * @max_write_len:       单条写消息允许的最大长度
 * @max_read_len:        单条读消息允许的最大长度
 * @max_comb_1st_msg_len: 组合消息中第一条消息允许的最大长度
 * @max_comb_2nd_msg_len: 组合消息中第二条消息允许的最大长度
 *
 * 关于组合消息：有些控制器一次只支持单消息，外加一种非常受限的
 * “组合模式”或“write-then-read”。它通常只够访问 EEPROM 这类寄存器型
 * 设备。为支持这类硬件，quirks 把“是否支持组合”“第一条必须写”
 * “第二条必须读”“两条消息地址必须相同”等限制拆成了可组合的标志位。
 */

struct i2c_adapter_quirks {
	u64 flags;
	int max_num_msgs;
	u16 max_write_len;
	u16 max_read_len;
	u16 max_comb_1st_msg_len;
	u16 max_comb_2nd_msg_len;
};

/* 强制最大消息数为 2，并使用 max_comb_*_len 做长度检查。 */
#define I2C_AQ_COMB			BIT(0)
/* 组合消息的第一条必须是写。 */
#define I2C_AQ_COMB_WRITE_FIRST		BIT(1)
/* 组合消息的第二条必须是读。 */
#define I2C_AQ_COMB_READ_SECOND		BIT(2)
/* 组合消息的两条都必须访问同一个目标地址。 */
#define I2C_AQ_COMB_SAME_ADDR		BIT(3)
/* 典型 write-then-read 场景的便捷组合宏。 */
#define I2C_AQ_COMB_WRITE_THEN_READ	(I2C_AQ_COMB | I2C_AQ_COMB_WRITE_FIRST | \
					 I2C_AQ_COMB_READ_SECOND | I2C_AQ_COMB_SAME_ADDR)
/* 控制器不支持 clock stretching。 */
#define I2C_AQ_NO_CLK_STRETCH		BIT(4)
/* 消息长度不能为 0。 */
#define I2C_AQ_NO_ZERO_LEN_READ		BIT(5)
#define I2C_AQ_NO_ZERO_LEN_WRITE	BIT(6)
#define I2C_AQ_NO_ZERO_LEN		(I2C_AQ_NO_ZERO_LEN_READ | I2C_AQ_NO_ZERO_LEN_WRITE)
/* 控制器不支持 repeated START。 */
#define I2C_AQ_NO_REP_START		BIT(7)

/*
 * i2c_adapter 用来标识一条物理 I2C 总线，以及访问它所需的算法与状态。
 *
 * 它是 I2C core 的“总线对象”：client 挂在它下面，driver 通过它发起
 * 事务，mux/ATR 也都围绕它来组织总线拓扑。
 */
struct i2c_adapter {
	struct module *owner;
	unsigned int class;		  /* 允许 detect() 探测的 adapter class */
	const struct i2c_algorithm *algo; /* 访问这条总线所使用的算法实现 */
	void *algo_data;

	/* 对所有适配器都通用的运行时字段。 */
	const struct i2c_lock_operations *lock_ops;
	struct rt_mutex bus_lock;
	struct rt_mutex mux_lock;

	int timeout;			/* 传输超时，单位 jiffies */
	int retries;
	struct device dev;		/* 适配器对应的 struct device */
	unsigned long locked_flags;	/* 由 I2C core 管理的锁定状态位 */
#define I2C_ALF_IS_SUSPENDED		0
#define I2C_ALF_SUSPEND_REPORTED	1

	int nr;
	char name[48];
	struct completion dev_released;

	struct mutex userspace_clients_lock;
	struct list_head userspace_clients;

	struct i2c_bus_recovery_info *bus_recovery_info;
	const struct i2c_adapter_quirks *quirks;

	struct irq_domain *host_notify_domain;
	struct regulator *bus_regulator;

	struct dentry *debugfs;

	/* 7 位地址空间的实例化占用位图。 */
	DECLARE_BITMAP(addrs_in_instantiation, 1 << 7);
};
#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)

static inline void *i2c_get_adapdata(const struct i2c_adapter *adap)
{
	return dev_get_drvdata(&adap->dev);
}

static inline void i2c_set_adapdata(struct i2c_adapter *adap, void *data)
{
	dev_set_drvdata(&adap->dev, data);
}

static inline struct i2c_adapter *
i2c_parent_is_i2c_adapter(const struct i2c_adapter *adapter)
{
#if IS_ENABLED(CONFIG_I2C_MUX)
	struct device *parent = adapter->dev.parent;

	if (parent != NULL && parent->type == &i2c_adapter_type)
		return to_i2c_adapter(parent);
	else
#endif
		return NULL;
}

int i2c_for_each_dev(void *data, int (*fn)(struct device *dev, void *data));

/* 导出给共享引脚/总线拓扑场景使用的 adapter 加锁标志。 */
#define I2C_LOCK_ROOT_ADAPTER BIT(0)
#define I2C_LOCK_SEGMENT      BIT(1)

/**
 * i2c_lock_bus - 获取一段 I2C 总线的独占访问权
 * @adapter: 目标总线段
 * @flags:   I2C_LOCK_ROOT_ADAPTER 表示锁住根适配器，
 *	     I2C_LOCK_SEGMENT 表示只锁当前拓扑分支
 */
static inline void
i2c_lock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	adapter->lock_ops->lock_bus(adapter, flags);
}

/**
 * i2c_trylock_bus - 尝试获取一段 I2C 总线的独占访问权
 * @adapter: 目标总线段
 * @flags:   I2C_LOCK_ROOT_ADAPTER 表示尝试锁住根适配器，
 *	     I2C_LOCK_SEGMENT 表示只尝试锁当前拓扑分支
 *
 * Return: 成功返回 true，失败返回 false
 */
static inline int
i2c_trylock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	return adapter->lock_ops->trylock_bus(adapter, flags);
}

/**
 * i2c_unlock_bus - 释放一段 I2C 总线的独占访问权
 * @adapter: 目标总线段
 * @flags:   I2C_LOCK_ROOT_ADAPTER 表示解锁根适配器，
 *	     I2C_LOCK_SEGMENT 表示只解锁当前拓扑分支
 */
static inline void
i2c_unlock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	adapter->lock_ops->unlock_bus(adapter, flags);
}

/**
 * i2c_mark_adapter_suspended - 向 I2C core 报告 adapter 已挂起
 * @adap: 要标记为挂起的 adapter
 *
 * 标记后，I2C core 会拒绝后续发往这条 adapter 的新传输。
 * 这不是强制接口，但对于同时区分 system suspend 和 runtime suspend 的
 * 控制器驱动，使用它通常更稳妥。
 */
static inline void i2c_mark_adapter_suspended(struct i2c_adapter *adap)
{
	i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	set_bit(I2C_ALF_IS_SUSPENDED, &adap->locked_flags);
	i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
}

/**
 * i2c_mark_adapter_resumed - 向 I2C core 报告 adapter 已恢复
 * @adap: 要标记为恢复的 adapter
 *
 * 标记后，I2C core 会重新允许后续传输发往这条 adapter。
 * 另见 i2c_mark_adapter_suspended()。
 */
static inline void i2c_mark_adapter_resumed(struct i2c_adapter *adap)
{
	i2c_lock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
	clear_bit(I2C_ALF_IS_SUSPENDED, &adap->locked_flags);
	i2c_unlock_bus(adap, I2C_LOCK_ROOT_ADAPTER);
}

/* adapter class 位图。 */
#define I2C_CLASS_HWMON		(1<<0)	/* 硬件监控类设备，如 lm_sensors */
/* 提示该 adapter 已不再支持基于 class 的 detect。 */
#define I2C_CLASS_DEPRECATED	(1<<8)

/* 用于结束地址列表的内部哨兵值。 */
#define I2C_CLIENT_END		0xfffeU

/* 构造一个以 I2C_CLIENT_END 结尾的 I2C 地址数组。 */
#define I2C_ADDRS(addr, addrs...) \
	((const unsigned short []){ addr, ## addrs, I2C_CLIENT_END })


/* ----- i2c.o 导出的管理类接口 ----- */
#if IS_ENABLED(CONFIG_I2C)
int i2c_add_adapter(struct i2c_adapter *adap);
int devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adapter);
void i2c_del_adapter(struct i2c_adapter *adap);
int i2c_add_numbered_adapter(struct i2c_adapter *adap);

int i2c_register_driver(struct module *owner, struct i2c_driver *driver);
void i2c_del_driver(struct i2c_driver *driver);

/* 用宏包一层，避免为了拿到 THIS_MODULE 而额外引入头文件链。 */
#define i2c_add_driver(driver) \
	i2c_register_driver(THIS_MODULE, driver)

static inline bool i2c_client_has_driver(struct i2c_client *client)
{
	return !IS_ERR_OR_NULL(client) && client->dev.driver;
}

/* 对挂在这条 adapter 上的所有 client 调用其 command() 回调。 */
void i2c_clients_command(struct i2c_adapter *adap,
			 unsigned int cmd, void *arg);

struct i2c_adapter *i2c_get_adapter(int nr);
void i2c_put_adapter(struct i2c_adapter *adap);
unsigned int i2c_adapter_depth(struct i2c_adapter *adapter);

void i2c_parse_fw_timings(struct device *dev, struct i2c_timings *t, bool use_defaults);

/* 返回该 adapter 的功能位掩码。 */
static inline u32 i2c_get_functionality(struct i2c_adapter *adap)
{
	return adap->algo->functionality(adap);
}

/* 若 adapter 支持 func 指定的全部能力则返回 1，否则返回 0。 */
static inline int i2c_check_functionality(struct i2c_adapter *adap, u32 func)
{
	return (func & i2c_get_functionality(adap)) == func;
}

/**
 * i2c_check_quirks() - 检查 adapter 是否带有指定 quirk 标志
 * @adap:   目标 adapter
 * @quirks: 要检查的 quirk 位
 *
 * Return: 若 adapter 具备所有指定 quirk 位则返回 true，否则返回 false
 */
static inline bool i2c_check_quirks(struct i2c_adapter *adap, u64 quirks)
{
	if (!adap->quirks)
		return false;
	return (adap->quirks->flags & quirks) == quirks;
}

/* 返回指定 adapter 的总线编号。 */
static inline int i2c_adapter_id(struct i2c_adapter *adap)
{
	return adap->nr;
}

/* 获取 8 位地址字节：7 位地址左移 1 位，再叠加读写方向位。 */
static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg)
{
	return (msg->addr << 1) | (msg->flags & I2C_M_RD);
}

/*
 * 10 位地址的编码方式：
 *   addr_1: 5'b11110 | addr[9:8] | (R/nW)
 *   addr_2: addr[7:0]
 */
static inline u8 i2c_10bit_addr_hi_from_msg(const struct i2c_msg *msg)
{
	return 0xf0 | ((msg->addr & GENMASK(9, 8)) >> 7) | (msg->flags & I2C_M_RD);
}

static inline u8 i2c_10bit_addr_lo_from_msg(const struct i2c_msg *msg)
{
	return msg->addr & GENMASK(7, 0);
}

u8 *i2c_get_dma_safe_msg_buf(struct i2c_msg *msg, unsigned int threshold);
void i2c_put_dma_safe_msg_buf(u8 *buf, struct i2c_msg *msg, bool xferred);

int i2c_handle_smbus_host_notify(struct i2c_adapter *adap, unsigned short addr);

/**
 * module_i2c_driver() - 用于注册模块化 I2C 驱动的辅助宏
 * @__i2c_driver: i2c_driver 结构体
 *
 * 适用于 init/exit 没有额外特殊逻辑的 I2C 驱动，可省掉大量样板代码。
 * 每个模块只能用一次，使用后会替代 module_init() / module_exit()。
 */
#define module_i2c_driver(__i2c_driver) \
	module_driver(__i2c_driver, i2c_add_driver, \
			i2c_del_driver)

/**
 * builtin_i2c_driver() - 用于注册内建 I2C 驱动的辅助宏
 * @__i2c_driver: i2c_driver 结构体
 *
 * 适用于初始化阶段没有额外特殊逻辑的内建驱动，可省掉样板代码。
 * 每个驱动只能用一次，使用后会替代 device_initcall()。
 */
#define builtin_i2c_driver(__i2c_driver) \
	builtin_driver(__i2c_driver, i2c_add_driver)

/* 返回的 i2c_client 用完后必须调用 put_device()。 */
struct i2c_client *i2c_find_device_by_fwnode(struct fwnode_handle *fwnode);

/* 返回的 i2c_adapter 用完后必须调用 put_device()。 */
struct i2c_adapter *i2c_find_adapter_by_fwnode(struct fwnode_handle *fwnode);

/* 返回的 i2c_adapter 用完后必须调用 i2c_put_adapter()。 */
struct i2c_adapter *i2c_get_adapter_by_fwnode(struct fwnode_handle *fwnode);

#else /* I2C */

static inline struct i2c_client *
i2c_find_device_by_fwnode(struct fwnode_handle *fwnode)
{
	return NULL;
}

static inline struct i2c_adapter *
i2c_find_adapter_by_fwnode(struct fwnode_handle *fwnode)
{
	return NULL;
}

static inline struct i2c_adapter *
i2c_get_adapter_by_fwnode(struct fwnode_handle *fwnode)
{
	return NULL;
}

#endif /* !I2C */

#if IS_ENABLED(CONFIG_OF)
/* 返回的 i2c_client 用完后必须调用 put_device()。 */
static inline struct i2c_client *of_find_i2c_device_by_node(struct device_node *node)
{
	return i2c_find_device_by_fwnode(of_fwnode_handle(node));
}

/* 返回的 i2c_adapter 用完后必须调用 put_device()。 */
static inline struct i2c_adapter *of_find_i2c_adapter_by_node(struct device_node *node)
{
	return i2c_find_adapter_by_fwnode(of_fwnode_handle(node));
}

/* 返回的 i2c_adapter 用完后必须调用 i2c_put_adapter()。 */
static inline struct i2c_adapter *of_get_i2c_adapter_by_node(struct device_node *node)
{
	return i2c_get_adapter_by_fwnode(of_fwnode_handle(node));
}

int of_i2c_get_board_info(struct device *dev, struct device_node *node,
			  struct i2c_board_info *info);

#else

static inline struct i2c_client *of_find_i2c_device_by_node(struct device_node *node)
{
	return NULL;
}

static inline struct i2c_adapter *of_find_i2c_adapter_by_node(struct device_node *node)
{
	return NULL;
}

static inline struct i2c_adapter *of_get_i2c_adapter_by_node(struct device_node *node)
{
	return NULL;
}

static inline int of_i2c_get_board_info(struct device *dev,
					struct device_node *node,
					struct i2c_board_info *info)
{
	return -ENOTSUPP;
}

#endif /* CONFIG_OF */

struct acpi_resource;
struct acpi_resource_i2c_serialbus;

#if IS_REACHABLE(CONFIG_ACPI) && IS_REACHABLE(CONFIG_I2C)
bool i2c_acpi_get_i2c_resource(struct acpi_resource *ares,
			       struct acpi_resource_i2c_serialbus **i2c);
int i2c_acpi_client_count(struct acpi_device *adev);
u32 i2c_acpi_find_bus_speed(struct device *dev);
struct i2c_client *i2c_acpi_new_device_by_fwnode(struct fwnode_handle *fwnode,
						 int index,
						 struct i2c_board_info *info);
struct i2c_adapter *i2c_acpi_find_adapter_by_handle(acpi_handle handle);
bool i2c_acpi_waive_d0_probe(struct device *dev);
#else
static inline bool i2c_acpi_get_i2c_resource(struct acpi_resource *ares,
					     struct acpi_resource_i2c_serialbus **i2c)
{
	return false;
}
static inline int i2c_acpi_client_count(struct acpi_device *adev)
{
	return 0;
}
static inline u32 i2c_acpi_find_bus_speed(struct device *dev)
{
	return 0;
}
static inline struct i2c_client *i2c_acpi_new_device_by_fwnode(
					struct fwnode_handle *fwnode, int index,
					struct i2c_board_info *info)
{
	return ERR_PTR(-ENODEV);
}
static inline struct i2c_adapter *i2c_acpi_find_adapter_by_handle(acpi_handle handle)
{
	return NULL;
}
static inline bool i2c_acpi_waive_d0_probe(struct device *dev)
{
	return false;
}
#endif /* CONFIG_ACPI */

static inline struct i2c_client *i2c_acpi_new_device(struct device *dev,
						     int index,
						     struct i2c_board_info *info)
{
	return i2c_acpi_new_device_by_fwnode(dev_fwnode(dev), index, info);
}

#endif /* _LINUX_I2C_H */
