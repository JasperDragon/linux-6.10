/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_REGMAP_H
#define __LINUX_REGMAP_H

/*
 * Register map 访问 API
 *
 * Copyright 2011 Wolfson Microelectronics plc
 *
 * Author: Mark Brown <broonie@opensource.wolfsonmicro.com>
 */

#include <linux/bug.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fwnode.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/rbtree.h>

struct module;
struct clk;
struct device;
struct device_node;
struct fsi_device;
struct i2c_client;
struct i3c_device;
struct irq_domain;
struct mdio_device;
struct slim_device;
struct spi_device;
struct spmi_device;
struct regmap;
struct regmap_range_cfg;
struct regmap_field;
struct snd_ac97;
struct sdw_slave;

/*
 * regmap_mdio 地址编码。
 * IEEE 802.3ae clause 45 地址由“设备地址 + 寄存器地址”拼成一个逻辑地址。
 */
#define REGMAP_MDIO_C45_DEVAD_SHIFT	16
#define REGMAP_MDIO_C45_DEVAD_MASK	GENMASK(20, 16)
#define REGMAP_MDIO_C45_REGNUM_MASK	GENMASK(15, 0)

/*
 * regmap.reg_shift 表示在发起任何访问前，需要对寄存器地址做多少位移。
 * 这是带符号值：
 * 正数表示把原始寄存器地址右移（downshift），
 * 负数表示把原始寄存器地址左移（upshift）。
 */
#define REGMAP_UPSHIFT(s)	(-(s))
#define REGMAP_DOWNSHIFT(s)	(s)

/*
 * 支持的缓存类型，默认是不使用缓存。
 * 新增用户通常优先选 maple tree cache；只有在“运行期绝不允许动态分配”
 * 这类很强的约束下，才更适合 sparse flat cache。
 * rbtree cache 可能在非常低端、且频繁做 cache sync 的系统上略有优势，
 * 但整体上已经更偏历史兼容。
 *
 * 这些 sparse cache 在没有提供默认值时，会按需从硬件回读初始化。
 * 非 sparse 的 flat cache 主要为兼容旧用户保留；若缺失默认值，
 * 它会把对应缓存项初始化成 0。新用户应优先使用 sparse flat cache。
 */
enum regcache_type {
	REGCACHE_NONE,
	REGCACHE_RBTREE,
	REGCACHE_FLAT,
	REGCACHE_MAPLE,
	REGCACHE_FLAT_S,
};

/**
 * struct reg_default - 单个寄存器的默认值描述
 *
 * @reg: Register address.
 * @def: Register default value.
 *
 * 之所以使用结构体数组而不是简单线性数组，是因为很多现代设备的寄存器图
 * 都很稀疏，不适合按连续地址逐项展开。
 */
struct reg_default {
	unsigned int reg;
	unsigned int def;
};

/**
 * struct reg_sequence - 一组顺序写操作中的单条写入项
 *
 * @reg: Register address.
 * @def: Register value.
 * @delay_us: Delay to be applied after the register write in microseconds
 *
 * 用于描述一串“寄存器/值”写入项，每次写入后还可附带一个可选延迟，
 * 延迟单位为微秒。
 */
struct reg_sequence {
	unsigned int reg;
	unsigned int def;
	unsigned int delay_us;
};

#define REG_SEQ(_reg, _def, _delay_us) {		\
				.reg = _reg,		\
				.def = _def,		\
				.delay_us = _delay_us,	\
				}
#define REG_SEQ0(_reg, _def)	REG_SEQ(_reg, _def, 0)

/**
 * regmap_read_poll_timeout - 轮询读取寄存器，直到条件满足或超时
 *
 * @map: Regmap to read from
 * @addr: Address to poll
 * @val: Unsigned integer variable to read the value into
 * @cond: Break condition (usually involving @val)
 * @sleep_us: Maximum time to sleep between reads in us (0 tight-loops). Please
 *            read usleep_range() function description for details and
 *            limitations.
 * @timeout_us: Timeout in us, 0 means never timeout
 *
 * 该宏的风格与 linux/iopoll.h 里的 readx_poll_timeout 宏一致。
 *
 * 返回值：成功返回 0；超时返回 -ETIMEDOUT；若 regmap_read 本身失败，
 * 则返回其错误码。在成功或超时这两种情况下，@val 中都会保留最后一次读取值。
 * 若使用了 sleep_us 或 timeout_us，则不能在原子上下文中调用。
 */
#define regmap_read_poll_timeout(map, addr, val, cond, sleep_us, timeout_us) \
({ \
	int __ret, __tmp; \
	__tmp = read_poll_timeout(regmap_read, __ret, __ret || (cond), \
			sleep_us, timeout_us, false, (map), (addr), &(val)); \
	__ret ?: __tmp; \
})

/**
 * regmap_read_poll_timeout_atomic - 原子上下文可用的轮询读取宏
 *
 * @map: Regmap to read from
 * @addr: Address to poll
 * @val: Unsigned integer variable to read the value into
 * @cond: Break condition (usually involving @val)
 * @delay_us: Time to udelay between reads in us (0 tight-loops). Please
 *            read udelay() function description for details and
 *            limitations.
 * @timeout_us: Timeout in us, 0 means never timeout
 *
 * 该宏的风格与 linux/iopoll.h 里的 readx_poll_timeout_atomic 宏一致。
 *
 * 注意：通常 regmap 并不适合原子上下文。
 * 如果确实要在原子上下文中使用这个宏，必须先把 regmap 配成可原子访问模式，
 * 典型组合是 MMIO regmap，并关闭缓存或使用 flat/no cache。
 *
 * 返回值：成功返回 0；超时返回 -ETIMEDOUT；若 regmap_read 失败，则返回其错误码。
 * 在成功或超时这两种情况下，@val 中都会保留最后一次读取值。
 */
#define regmap_read_poll_timeout_atomic(map, addr, val, cond, delay_us, timeout_us) \
({ \
	u64 __timeout_us = (timeout_us); \
	unsigned long __delay_us = (delay_us); \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us); \
	int __ret; \
	for (;;) { \
		__ret = regmap_read((map), (addr), &(val)); \
		if (__ret) \
			break; \
		if (cond) \
			break; \
		if ((__timeout_us) && \
		    ktime_compare(ktime_get(), __timeout) > 0) { \
			__ret = regmap_read((map), (addr), &(val)); \
			break; \
		} \
		if (__delay_us) \
			udelay(__delay_us); \
	} \
	__ret ?: ((cond) ? 0 : -ETIMEDOUT); \
})

/**
 * regmap_field_read_poll_timeout - 轮询读取 bitfield，直到条件满足或超时
 *
 * @field: Regmap field to read from
 * @val: Unsigned integer variable to read the value into
 * @cond: Break condition (usually involving @val)
 * @sleep_us: Maximum time to sleep between reads in us (0 tight-loops). Please
 *            read usleep_range() function description for details and
 *            limitations.
 * @timeout_us: Timeout in us, 0 means never timeout
 *
 * 该宏的风格与 linux/iopoll.h 里的 readx_poll_timeout 宏一致。
 *
 * 返回值：成功返回 0；超时返回 -ETIMEDOUT；若 regmap_field_read 失败，
 * 则返回其错误码。在成功或超时这两种情况下，@val 中都会保留最后一次读取值。
 * 若使用了 sleep_us 或 timeout_us，则不能在原子上下文中调用。
 */
#define regmap_field_read_poll_timeout(field, val, cond, sleep_us, timeout_us) \
({ \
	int __ret, __tmp; \
	__tmp = read_poll_timeout(regmap_field_read, __ret, __ret || (cond), \
			sleep_us, timeout_us, false, (field), &(val)); \
	__ret ?: __tmp; \
})

#ifdef CONFIG_REGMAP

enum regmap_endian {
	/* Unspecified -> 0 -> Backwards compatible default */
	REGMAP_ENDIAN_DEFAULT = 0,
	REGMAP_ENDIAN_BIG,
	REGMAP_ENDIAN_LITTLE,
	REGMAP_ENDIAN_NATIVE,
};

/**
 * struct regmap_range - 一段连续寄存器区间
 *
 * 主要用于访问属性判断，例如 readable/writeable/volatile/precious。
 *
 * @range_min: 区间首寄存器地址
 * @range_max: 区间尾寄存器地址
 */
struct regmap_range {
	unsigned int range_min;
	unsigned int range_max;
};

#define regmap_reg_range(low, high) { .range_min = low, .range_max = high, }

/**
 * struct regmap_access_table - 用于访问属性检查的寄存器区间表
 *
 * @yes_ranges : “允许访问”区间数组
 * @n_yes_ranges: 上面数组的元素个数
 * @no_ranges: “禁止访问”区间数组
 * @n_no_ranges: 上面数组的元素个数
 *
 * 这张表同时支持 yes_ranges 和 no_ranges。
 * 如果寄存器命中 no_ranges，对应检查函数直接返回 false。
 * 如果命中 yes_ranges，则返回 true。
 * 查找顺序永远是先看 no_ranges，再看 yes_ranges。
 */
struct regmap_access_table {
	const struct regmap_range *yes_ranges;
	unsigned int n_yes_ranges;
	const struct regmap_range *no_ranges;
	unsigned int n_no_ranges;
};

typedef void (*regmap_lock)(void *);
typedef void (*regmap_unlock)(void *);

/**
 * struct regmap_config - 描述一个设备寄存器图的配置模板
 *
 * @name: regmap 的可选名字。一个设备有多片寄存器区时很有用。
 *
 * @reg_bits: 寄存器地址宽度，单位 bit，必填。
 * @reg_stride: 寄存器地址步长。合法地址必须是它的整数倍；若填 0，则按 1 处理。
 * @reg_shift: 访问前对寄存器地址额外做的位移。正数表示右移，负数表示左移。
 * @reg_base: 每次访问前统一加到寄存器地址上的基址偏移。
 * @pad_bits: 寄存器地址和寄存器值之间的填充位数。
 * @val_bits: 寄存器值宽度，单位 bit，必填。
 *
 * @writeable_reg: 可选回调，返回寄存器是否可写。若为空但 wr_table 有效，
 *		   则改为查表判断。
 * @readable_reg: 可选回调，返回寄存器是否可读。若为空但 rd_table 有效，
 *		  则改为查表判断。
 * @volatile_reg: 可选回调，返回寄存器是否为 volatile，即值不应缓存。
 *		  若为空但 volatile_table 有效，则改为查表判断。
 * @precious_reg: 可选回调，返回寄存器是否为 precious。
 *		  precious 寄存器通常不能被框架随意读取，例如“读后清除”的状态寄存器。
 *		  若为空但 precious_table 有效，则改为查表判断。
 * @writeable_noinc_reg: 可选回调，标记支持“地址不自增的多次写”寄存器。
 *			若为空但 wr_noinc_table 有效，则改为查表判断。
 * @readable_noinc_reg: 可选回调，标记支持“地址不自增的多次读”寄存器。
 *			若为空但 rd_noinc_table 有效，则改为查表判断。
 * @reg_read:	  可选的单寄存器读回调。只有在设备读操作无法抽象成普通总线读时
 *		  才需要自己实现；大多数设备不需要。
 * @reg_write:	  单寄存器写回调，语义同上。
 * @reg_update_bits: 可选的 update_bits/rmw 回调。仅当该操作需要特殊锁语义
 *		     或特殊寄存器处理，无法由通用 regmap 流程表达时才需要。
 * @read: 可选的 bulk read 回调。
 * @write: 可选的 bulk write 回调。
 * @max_raw_read: 设备支持的 raw read 最大长度。
 * @max_raw_write: 设备支持的 raw write 最大长度。
 * @can_sleep:	  可选，声明 regmap 访问路径是否允许睡眠。
 * @fast_io:	  声明寄存器 I/O 很快，框架可优先用 spinlock 而不是 mutex。
 *		  若自定义了 lock/unlock，则该字段被忽略。
 *		  这个字段和 struct regmap_bus 里的同名语义一致，主要用于“无总线”场景。
 * @io_port:	  支持 IO port 访问器。只有在 MMIO 和 IO port 可区分时才有意义。
 * @disable_locking: 该 regmap 已由外部机制保护，或保证不会被多线程并发访问；
 *		     因此完全关闭框架内部锁。
 * @lock:	  可选自定义加锁回调，覆盖 regmap 默认锁实现。
 * @unlock:	  可选自定义解锁回调。
 * @lock_arg:	  作为唯一参数传给 lock/unlock 的私有指针。
 * @max_register: 可选，最大合法寄存器地址。
 * @max_register_is_0: 可选，表示 @max_register 即使为 0 也应视为有效。
 *                     主要用于“整个 regmap 只有一个寄存器”的特殊场景。
 * @wr_table:     可选，写访问区间表。
 * @rd_table:     可选，读访问区间表。
 * @volatile_table: 可选，volatile 区间表。
 * @precious_table: 可选，precious 区间表。
 * @wr_noinc_table: 可选，不自增写区间表。
 * @rd_noinc_table: 可选，不自增读区间表。
 * @reg_defaults: 上电复位默认值表，供 regcache 初始化使用。
 * @num_reg_defaults: reg_defaults 中的元素个数。
 * @reg_default_cb: 可选回调，用于补齐 reg_defaults 没列出的默认值。
 *                  当前主要用于 REGCACHE_FLAT 的填充；驱动必须自己保证
 *                  readable_reg/writeable_reg 能正确处理这些空洞地址。
 *
 * @read_flag_mask: 读操作时要 OR 到寄存器地址高位的标志位。
 * @write_flag_mask: 写操作时要 OR 到寄存器地址高位的标志位。
 *                   如果 read_flag_mask/write_flag_mask 都为空，
 *                   且 zero_flag_mask 未置位，就使用 regmap_bus 默认值。
 * @zero_flag_mask: 若置位，则即使读写 flag mask 都为 0，也视为“显式指定 0”。
 * @use_relaxed_mmio: 若置位，MMIO 读写不带内存屏障。
 *                    这能降低某些设备的访存开销，但驱动必须自行补足必要屏障。
 * @use_single_read: 若置位，把 bulk read 拆成一串单寄存器读。
 *                   适合不支持 bulk read 的设备。
 * @use_single_write: 若置位，把 bulk write 拆成一串单寄存器写。
 *                    适合不支持 bulk write 的设备。
 * @can_multi_write: 若置位，表示设备支持 bulk write 的 multi write 模式；
 *                   若未置位，则 multi write 请求会被拆成单条写操作。
 *
 * @cache_type: 选择实际使用的缓存类型。
 * @reg_defaults_raw: 以原始线性格式给出的上电默认值，供 regcache 初始化使用。
 * @num_reg_defaults_raw: reg_defaults_raw 里的寄存器项个数。
 * @use_hwlock: 若置位，表示需要使用硬件 spinlock。
 * @use_raw_spinlock: 若置位，表示软件锁层使用 raw spinlock。
 * @hwlock_id: 指定硬件 spinlock 的编号。
 * @hwlock_mode: 硬件 spinlock 的模式，应为 HWLOCK_IRQSTATE、HWLOCK_IRQ 或 0。
 * @reg_format_endian: 格式化寄存器地址时使用的字节序。
 *		       若为 DEFAULT，则回退到 regmap bus 给出的默认值。
 * @val_format_endian: 格式化寄存器值时使用的字节序。
 *		       若为 DEFAULT，则回退到 regmap bus 给出的默认值。
 *
 * @ranges: 虚拟地址范围配置项数组。
 * @num_ranges: ranges 数组中的配置项个数。
 */
struct regmap_config {
	const char *name;

	int reg_bits;
	int reg_stride;
	int reg_shift;
	unsigned int reg_base;
	int pad_bits;
	int val_bits;

	bool (*writeable_reg)(struct device *dev, unsigned int reg);
	bool (*readable_reg)(struct device *dev, unsigned int reg);
	bool (*volatile_reg)(struct device *dev, unsigned int reg);
	bool (*precious_reg)(struct device *dev, unsigned int reg);
	bool (*writeable_noinc_reg)(struct device *dev, unsigned int reg);
	bool (*readable_noinc_reg)(struct device *dev, unsigned int reg);

	int (*reg_read)(void *context, unsigned int reg, unsigned int *val);
	int (*reg_write)(void *context, unsigned int reg, unsigned int val);
	int (*reg_update_bits)(void *context, unsigned int reg,
			       unsigned int mask, unsigned int val);
	/* bulk read/write 回调 */
	int (*read)(void *context, const void *reg_buf, size_t reg_size,
		    void *val_buf, size_t val_size);
	int (*write)(void *context, const void *data, size_t count);
	size_t max_raw_read;
	size_t max_raw_write;

	bool can_sleep;

	bool fast_io;
	bool io_port;

	bool disable_locking;
	regmap_lock lock;
	regmap_unlock unlock;
	void *lock_arg;

	unsigned int max_register;
	bool max_register_is_0;
	const struct regmap_access_table *wr_table;
	const struct regmap_access_table *rd_table;
	const struct regmap_access_table *volatile_table;
	const struct regmap_access_table *precious_table;
	const struct regmap_access_table *wr_noinc_table;
	const struct regmap_access_table *rd_noinc_table;
	const struct reg_default *reg_defaults;
	unsigned int num_reg_defaults;
	int (*reg_default_cb)(struct device *dev, unsigned int reg,
			      unsigned int *def);
	enum regcache_type cache_type;
	const void *reg_defaults_raw;
	unsigned int num_reg_defaults_raw;

	unsigned long read_flag_mask;
	unsigned long write_flag_mask;
	bool zero_flag_mask;

	bool use_single_read;
	bool use_single_write;
	bool use_relaxed_mmio;
	bool can_multi_write;

	bool use_hwlock;
	bool use_raw_spinlock;
	unsigned int hwlock_id;
	unsigned int hwlock_mode;

	enum regmap_endian reg_format_endian;
	enum regmap_endian val_format_endian;

	const struct regmap_range_cfg *ranges;
	unsigned int num_ranges;
};

/**
 * struct regmap_range_cfg - 间接访问/分页寄存器的范围配置
 *
 * @name: 供诊断和调试输出使用的描述性名字
 *
 * @range_min: 虚拟范围内的最低寄存器地址
 * @range_max: 虚拟范围内的最高寄存器地址
 *
 * @selector_reg: 选择器字段所在的寄存器
 * @selector_mask: 选择器字段的位掩码
 * @selector_shift: 选择器字段的位移
 *
 * @window_start: 数据窗口中第一个寄存器的地址
 * @window_len: 数据窗口包含的寄存器个数
 *
 * 映射到这个虚拟范围的寄存器访问会分两步完成：
 * 1. 先更新 page selector；
 * 2. 再通过 data window 里的寄存器实际读写数据。
 */
struct regmap_range_cfg {
	const char *name;

	/* 虚拟地址范围 */
	unsigned int range_min;
	unsigned int range_max;

	/* 间接寻址使用的 page selector */
	unsigned int selector_reg;
	unsigned int selector_mask;
	int selector_shift;

	/* 每个 page 对应的一段数据窗口 */
	unsigned int window_start;
	unsigned int window_len;
};

/**
 * struct regmap_sdw_mbq_cfg - SoundWire Multi-Byte Quantities 配置
 *
 * @mbq_size: 回调，返回指定寄存器实际占用的字节数。
 * @deferrable: 回调，返回硬件是否允许对该寄存器做 deferred transaction。
 *              这种延迟事务通常只应由 SDCA 器件使用；哪些 control 可延迟，
 *              一般由驱动内建表或平台固件中的 DisCo table 指定。
 *
 * @timeout_us: 等待 deferred transaction 完成的超时时间，单位微秒。
 * @retry_us: 轮询 function busy 状态、等待重试机会的时间间隔，单位微秒。
 *
 * 为 SoundWire MBQ 寄存器图提供附加配置。
 */
struct regmap_sdw_mbq_cfg {
	int (*mbq_size)(struct device *dev, unsigned int reg);
	bool (*deferrable)(struct device *dev, unsigned int reg);
	unsigned long timeout_us;
	unsigned long retry_us;
};

struct regmap_async;

typedef int (*regmap_hw_write)(void *context, const void *data,
			       size_t count);
typedef int (*regmap_hw_gather_write)(void *context,
				      const void *reg, size_t reg_len,
				      const void *val, size_t val_len);
typedef int (*regmap_hw_async_write)(void *context,
				     const void *reg, size_t reg_len,
				     const void *val, size_t val_len,
				     struct regmap_async *async);
typedef int (*regmap_hw_read)(void *context,
			      const void *reg_buf, size_t reg_size,
			      void *val_buf, size_t val_size);
typedef int (*regmap_hw_reg_read)(void *context, unsigned int reg,
				  unsigned int *val);
typedef int (*regmap_hw_reg_noinc_read)(void *context, unsigned int reg,
					void *val, size_t val_count);
typedef int (*regmap_hw_reg_write)(void *context, unsigned int reg,
				   unsigned int val);
typedef int (*regmap_hw_reg_noinc_write)(void *context, unsigned int reg,
					 const void *val, size_t val_count);
typedef int (*regmap_hw_reg_update_bits)(void *context, unsigned int reg,
					 unsigned int mask, unsigned int val);
typedef struct regmap_async *(*regmap_hw_async_alloc)(void);
typedef void (*regmap_hw_free_context)(void *context);

/**
 * struct regmap_bus - regmap 框架所需的硬件总线描述
 *
 * @fast_io: 寄存器 I/O 很快，框架可优先用 spinlock 而不是 mutex。
 *	     若 regmap_config 自定义了 lock/unlock，则该字段被忽略。
 * @free_on_exit: regmap 退出时是否需要 kfree 这个 bus 描述。
 * @write: 线性写操作。
 * @gather_write: 拆分的“寄存器头 + 数据体”写操作；若不支持，应返回 -ENOTSUPP。
 * @async_write: 异步写操作，可选；必须与同步 I/O 串行化。
 * @reg_write: 单寄存器写操作，函数返回前必须完成。
 * @reg_write_noinc: 向同一寄存器做多值写入，函数返回前必须完成。
 * @reg_update_bits: 对 volatile 寄存器使用的 update_bits 操作。
 *                   适合底层硬件支持 set/clear 位，而不必先读改写回的场景。
 * @read: bulk read 操作，数据放回传入的接收缓冲区。
 * @reg_read: 单寄存器读操作。
 * @free_context: 释放底层 bus context。
 * @async_alloc: 分配 regmap_async 结构。
 * @read_flag_mask: 读操作时要 OR 到寄存器地址最高字节的标志位。
 * @reg_format_endian_default: 格式化寄存器地址时的默认字节序。
 *     当 regmap_config 指定 DEFAULT 时使用；若仍为 DEFAULT，则按 BIG 处理。
 * @val_format_endian_default: 格式化寄存器值时的默认字节序。
 *     当 regmap_config 指定 DEFAULT 时使用；若仍为 DEFAULT，则按 BIG 处理。
 * @max_raw_read: 该总线允许的 raw read 最大长度。
 * @max_raw_write: 该总线允许的 raw write 最大长度。
 */
struct regmap_bus {
	bool fast_io;
	bool free_on_exit;
	regmap_hw_write write;
	regmap_hw_gather_write gather_write;
	regmap_hw_async_write async_write;
	regmap_hw_reg_write reg_write;
	regmap_hw_reg_noinc_write reg_noinc_write;
	regmap_hw_reg_update_bits reg_update_bits;
	regmap_hw_read read;
	regmap_hw_reg_read reg_read;
	regmap_hw_reg_noinc_read reg_noinc_read;
	regmap_hw_free_context free_context;
	regmap_hw_async_alloc async_alloc;
	u8 read_flag_mask;
	enum regmap_endian reg_format_endian_default;
	enum regmap_endian val_format_endian_default;
	size_t max_raw_read;
	size_t max_raw_write;
};

/*
 * __regmap_init 系列底层入口。
 *
 * 这些函数需要显式传入 lockdep key 和 lockdep name，不建议直接调用。
 * 驱动应使用下面的 regmap_init* 宏，由宏自动为每个调用点生成独立的 key/name。
 */
struct regmap *__regmap_init(struct device *dev,
			     const struct regmap_bus *bus,
			     void *bus_context,
			     const struct regmap_config *config,
			     struct lock_class_key *lock_key,
			     const char *lock_name);
struct regmap *__regmap_init_i2c(struct i2c_client *i2c,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__regmap_init_mdio(struct mdio_device *mdio_dev,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__regmap_init_sccb(struct i2c_client *i2c,
				  const struct regmap_config *config,
				  struct lock_class_key *lock_key,
				  const char *lock_name);
struct regmap *__regmap_init_slimbus(struct slim_device *slimbus,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__regmap_init_spi(struct spi_device *dev,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__regmap_init_spmi_base(struct spmi_device *dev,
				       const struct regmap_config *config,
				       struct lock_class_key *lock_key,
				       const char *lock_name);
struct regmap *__regmap_init_spmi_ext(struct spmi_device *dev,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);
struct regmap *__regmap_init_w1(struct device *w1_dev,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__regmap_init_mmio_clk(struct device *dev, const char *clk_id,
				      void __iomem *regs,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);
struct regmap *__regmap_init_ac97(struct snd_ac97 *ac97,
				  const struct regmap_config *config,
				  struct lock_class_key *lock_key,
				  const char *lock_name);
struct regmap *__regmap_init_sdw(struct sdw_slave *sdw,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__regmap_init_sdw_mbq(struct device *dev, struct sdw_slave *sdw,
				     const struct regmap_config *config,
				     const struct regmap_sdw_mbq_cfg *mbq_config,
				     struct lock_class_key *lock_key,
				     const char *lock_name);
struct regmap *__regmap_init_i3c(struct i3c_device *i3c,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__regmap_init_spi_avmm(struct spi_device *spi,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);
struct regmap *__regmap_init_fsi(struct fsi_device *fsi_dev,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);

struct regmap *__devm_regmap_init(struct device *dev,
				  const struct regmap_bus *bus,
				  void *bus_context,
				  const struct regmap_config *config,
				  struct lock_class_key *lock_key,
				  const char *lock_name);
struct regmap *__devm_regmap_init_i2c(struct i2c_client *i2c,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);
struct regmap *__devm_regmap_init_mdio(struct mdio_device *mdio_dev,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);
struct regmap *__devm_regmap_init_sccb(struct i2c_client *i2c,
				       const struct regmap_config *config,
				       struct lock_class_key *lock_key,
				       const char *lock_name);
struct regmap *__devm_regmap_init_spi(struct spi_device *dev,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);
struct regmap *__devm_regmap_init_spmi_base(struct spmi_device *dev,
					    const struct regmap_config *config,
					    struct lock_class_key *lock_key,
					    const char *lock_name);
struct regmap *__devm_regmap_init_spmi_ext(struct spmi_device *dev,
					   const struct regmap_config *config,
					   struct lock_class_key *lock_key,
					   const char *lock_name);
struct regmap *__devm_regmap_init_w1(struct device *w1_dev,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);
struct regmap *__devm_regmap_init_mmio_clk(struct device *dev,
					   const char *clk_id,
					   void __iomem *regs,
					   const struct regmap_config *config,
					   struct lock_class_key *lock_key,
					   const char *lock_name);
struct regmap *__devm_regmap_init_ac97(struct snd_ac97 *ac97,
				       const struct regmap_config *config,
				       struct lock_class_key *lock_key,
				       const char *lock_name);
struct regmap *__devm_regmap_init_sdw(struct sdw_slave *sdw,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__devm_regmap_init_sdw_mbq(struct device *dev, struct sdw_slave *sdw,
					  const struct regmap_config *config,
					  const struct regmap_sdw_mbq_cfg *mbq_config,
					  struct lock_class_key *lock_key,
					  const char *lock_name);
struct regmap *__devm_regmap_init_slimbus(struct slim_device *slimbus,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__devm_regmap_init_i3c(struct i3c_device *i3c,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name);
struct regmap *__devm_regmap_init_spi_avmm(struct spi_device *spi,
					   const struct regmap_config *config,
					   struct lock_class_key *lock_key,
					   const char *lock_name);
struct regmap *__devm_regmap_init_fsi(struct fsi_device *fsi_dev,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name);

/*
 * regmap_init 宏的包装层：为每个调用点自动附带独立的 lockdep key/name。
 * 若未开启 CONFIG_LOCKDEP，则退化成普通函数调用。
 *
 * @fn: 实际要调用的底层函数（形如 __[*_]regmap_init[_*]）
 * @name: 调用宏里 config 参数的名字（即 #config）
 **/
#ifdef CONFIG_LOCKDEP
#define __regmap_lockdep_wrapper(fn, name, ...)				\
(									\
	({								\
		static struct lock_class_key _key;			\
		fn(__VA_ARGS__, &_key,					\
			KBUILD_BASENAME ":"				\
			__stringify(__LINE__) ":"			\
			"(" name ")->lock");				\
	})								\
)
#else
#define __regmap_lockdep_wrapper(fn, name, ...) fn(__VA_ARGS__, NULL, NULL)
#endif

/**
 * regmap_init() - 初始化一个寄存器映射
 *
 * @dev: 要访问的设备
 * @bus: 与该设备配套的总线回调集合
 * @bus_context: 透传给总线回调的私有上下文
 * @config: regmap 配置
 *
 * 返回值：失败时返回 ERR_PTR()，成功时返回有效的 struct regmap 指针。
 * 一般不应直接调用，应由各总线专用的 regmap_init_* 包装宏调用。
 */
#define regmap_init(dev, bus, bus_context, config)			\
	__regmap_lockdep_wrapper(__regmap_init, #config,		\
				dev, bus, bus_context, config)
int regmap_attach_dev(struct device *dev, struct regmap *map,
		      const struct regmap_config *config);

/**
 * regmap_init_i2c() - 为 I2C 设备初始化 regmap
 *
 * @i2c: Device that will be interacted with
 * @config: Configuration for register map
 *
 * 返回值：失败时返回 ERR_PTR()，成功时返回有效 regmap。
 */
#define regmap_init_i2c(i2c, config)					\
	__regmap_lockdep_wrapper(__regmap_init_i2c, #config,		\
				i2c, config)

/**
 * regmap_init_mdio() - Initialise register map
 *
 * @mdio_dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_mdio(mdio_dev, config)				\
	__regmap_lockdep_wrapper(__regmap_init_mdio, #config,		\
				mdio_dev, config)

/**
 * regmap_init_sccb() - Initialise register map
 *
 * @i2c: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_sccb(i2c, config)					\
	__regmap_lockdep_wrapper(__regmap_init_sccb, #config,		\
				i2c, config)

/**
 * regmap_init_slimbus() - Initialise register map
 *
 * @slimbus: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_slimbus(slimbus, config)				\
	__regmap_lockdep_wrapper(__regmap_init_slimbus, #config,	\
				slimbus, config)

/**
 * regmap_init_spi() - 为 SPI 设备初始化 regmap
 *
 * @dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_spi(dev, config)					\
	__regmap_lockdep_wrapper(__regmap_init_spi, #config,		\
				dev, config)

/**
 * regmap_init_spmi_base() - Create regmap for the Base register space
 *
 * @dev:	SPMI device that will be interacted with
 * @config:	Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_spmi_base(dev, config)				\
	__regmap_lockdep_wrapper(__regmap_init_spmi_base, #config,	\
				dev, config)

/**
 * regmap_init_spmi_ext() - Create regmap for Ext register space
 *
 * @dev:	Device that will be interacted with
 * @config:	Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_spmi_ext(dev, config)				\
	__regmap_lockdep_wrapper(__regmap_init_spmi_ext, #config,	\
				dev, config)

/**
 * regmap_init_w1() - Initialise register map
 *
 * @w1_dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_w1(w1_dev, config)					\
	__regmap_lockdep_wrapper(__regmap_init_w1, #config,		\
				w1_dev, config)

/**
 * regmap_init_mmio_clk() - Initialise register map with register clock
 *
 * @dev: Device that will be interacted with
 * @clk_id: register clock consumer ID
 * @regs: Pointer to memory-mapped IO region
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap. Implies 'fast_io'.
 */
#define regmap_init_mmio_clk(dev, clk_id, regs, config)			\
	__regmap_lockdep_wrapper(__regmap_init_mmio_clk, #config,	\
				dev, clk_id, regs, config)

/**
 * regmap_init_mmio() - Initialise register map
 *
 * @dev: Device that will be interacted with
 * @regs: Pointer to memory-mapped IO region
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap. Implies 'fast_io'.
 */
#define regmap_init_mmio(dev, regs, config)		\
	regmap_init_mmio_clk(dev, NULL, regs, config)

/**
 * regmap_init_ac97() - Initialise AC'97 register map
 *
 * @ac97: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_ac97(ac97, config)					\
	__regmap_lockdep_wrapper(__regmap_init_ac97, #config,		\
				ac97, config)
bool regmap_ac97_default_volatile(struct device *dev, unsigned int reg);

/**
 * regmap_init_sdw() - Initialise register map
 *
 * @sdw: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_sdw(sdw, config)					\
	__regmap_lockdep_wrapper(__regmap_init_sdw, #config,		\
				sdw, config)

/**
 * regmap_init_sdw_mbq() - Initialise register map
 *
 * @sdw: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_sdw_mbq(sdw, config)					\
	__regmap_lockdep_wrapper(__regmap_init_sdw_mbq, #config,		\
				&sdw->dev, sdw, config, NULL)

/**
 * regmap_init_sdw_mbq_cfg() - Initialise MBQ SDW register map with config
 *
 * @sdw: Device that will be interacted with
 * @config: Configuration for register map
 * @mbq_config: Properties for the MBQ registers
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap. The regmap will be automatically freed by the
 * device management code.
 */
#define regmap_init_sdw_mbq_cfg(dev, sdw, config, mbq_config)		\
	__regmap_lockdep_wrapper(__regmap_init_sdw_mbq, #config,	\
				dev, sdw, config, mbq_config)

/**
 * regmap_init_i3c() - Initialise register map
 *
 * @i3c: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_i3c(i3c, config)					\
	__regmap_lockdep_wrapper(__regmap_init_i3c, #config,		\
				i3c, config)

/**
 * regmap_init_spi_avmm() - Initialize register map for Intel SPI Slave
 * to AVMM Bus Bridge
 *
 * @spi: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.
 */
#define regmap_init_spi_avmm(spi, config)					\
	__regmap_lockdep_wrapper(__regmap_init_spi_avmm, #config,		\
				 spi, config)

/**
 * regmap_init_fsi() - Initialise register map
 *
 * @fsi_dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer to
 * a struct regmap.
 */
#define regmap_init_fsi(fsi_dev, config)				\
	__regmap_lockdep_wrapper(__regmap_init_fsi, #config, fsi_dev,	\
				 config)

/**
 * devm_regmap_init() - 初始化受 devres 管理的 regmap
 *
 * @dev: Device that will be interacted with
 * @bus: Bus-specific callbacks to use with device
 * @bus_context: Data passed to bus-specific callbacks
 * @config: Configuration for register map
 *
 * 返回值：失败时返回 ERR_PTR()，成功时返回有效 regmap。
 * 一般不应直接调用，应由总线专用的 devm_regmap_init_* 宏调用。
 * 成功创建后，regmap 会在设备解绑时自动释放。
 */
#define devm_regmap_init(dev, bus, bus_context, config)			\
	__regmap_lockdep_wrapper(__devm_regmap_init, #config,		\
				dev, bus, bus_context, config)

/**
 * devm_regmap_init_i2c() - Initialise managed register map
 *
 * @i2c: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_i2c(i2c, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_i2c, #config,	\
				i2c, config)

/**
 * devm_regmap_init_mdio() - Initialise managed register map
 *
 * @mdio_dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_mdio(mdio_dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_mdio, #config,	\
				mdio_dev, config)

/**
 * devm_regmap_init_sccb() - Initialise managed register map
 *
 * @i2c: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_sccb(i2c, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_sccb, #config,	\
				i2c, config)

/**
 * devm_regmap_init_spi() - Initialise register map
 *
 * @dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The map will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_spi(dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_spi, #config,	\
				dev, config)

/**
 * devm_regmap_init_spmi_base() - Create managed regmap for Base register space
 *
 * @dev:	SPMI device that will be interacted with
 * @config:	Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_spmi_base(dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_spmi_base, #config,	\
				dev, config)

/**
 * devm_regmap_init_spmi_ext() - Create managed regmap for Ext register space
 *
 * @dev:	SPMI device that will be interacted with
 * @config:	Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_spmi_ext(dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_spmi_ext, #config,	\
				dev, config)

/**
 * devm_regmap_init_w1() - Initialise managed register map
 *
 * @w1_dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_w1(w1_dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_w1, #config,	\
				w1_dev, config)
/**
 * devm_regmap_init_mmio_clk() - Initialise managed register map with clock
 *
 * @dev: Device that will be interacted with
 * @clk_id: register clock consumer ID
 * @regs: Pointer to memory-mapped IO region
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code. Implies 'fast_io'.
 */
#define devm_regmap_init_mmio_clk(dev, clk_id, regs, config)		\
	__regmap_lockdep_wrapper(__devm_regmap_init_mmio_clk, #config,	\
				dev, clk_id, regs, config)

/**
 * devm_regmap_init_mmio() - Initialise managed register map
 *
 * @dev: Device that will be interacted with
 * @regs: Pointer to memory-mapped IO region
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code. Implies 'fast_io'.
 */
#define devm_regmap_init_mmio(dev, regs, config)		\
	devm_regmap_init_mmio_clk(dev, NULL, regs, config)

/**
 * devm_regmap_init_ac97() - Initialise AC'97 register map
 *
 * @ac97: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_ac97(ac97, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_ac97, #config,	\
				ac97, config)

/**
 * devm_regmap_init_sdw() - Initialise managed register map
 *
 * @sdw: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap. The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_sdw(sdw, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_sdw, #config,	\
				sdw, config)

/**
 * devm_regmap_init_sdw_mbq() - Initialise managed register map
 *
 * @sdw: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap. The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_sdw_mbq(sdw, config)			\
	__regmap_lockdep_wrapper(__devm_regmap_init_sdw_mbq, #config,   \
				&sdw->dev, sdw, config, NULL)

/**
 * devm_regmap_init_sdw_mbq_cfg() - Initialise managed MBQ SDW register map with config
 *
 * @dev: Device that will be interacted with
 * @sdw: SoundWire Device that will be interacted with
 * @config: Configuration for register map
 * @mbq_config: Properties for the MBQ registers
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap. The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_sdw_mbq_cfg(dev, sdw, config, mbq_config)	\
	__regmap_lockdep_wrapper(__devm_regmap_init_sdw_mbq,		\
				#config, dev, sdw, config, mbq_config)

/**
 * devm_regmap_init_slimbus() - Initialise managed register map
 *
 * @slimbus: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap. The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_slimbus(slimbus, config)			\
	__regmap_lockdep_wrapper(__devm_regmap_init_slimbus, #config,	\
				slimbus, config)

/**
 * devm_regmap_init_i3c() - Initialise managed register map
 *
 * @i3c: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_i3c(i3c, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_i3c, #config,	\
				i3c, config)

/**
 * devm_regmap_init_spi_avmm() - Initialize register map for Intel SPI Slave
 * to AVMM Bus Bridge
 *
 * @spi: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The map will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_spi_avmm(spi, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_spi_avmm, #config,	\
				 spi, config)

/**
 * devm_regmap_init_fsi() - Initialise managed register map
 *
 * @fsi_dev: Device that will be interacted with
 * @config: Configuration for register map
 *
 * The return value will be an ERR_PTR() on error or a valid pointer
 * to a struct regmap.  The regmap will be automatically freed by the
 * device management code.
 */
#define devm_regmap_init_fsi(fsi_dev, config)				\
	__regmap_lockdep_wrapper(__devm_regmap_init_fsi, #config,	\
				 fsi_dev, config)

int regmap_mmio_attach_clk(struct regmap *map, struct clk *clk);
void regmap_mmio_detach_clk(struct regmap *map);
void regmap_exit(struct regmap *map);
int regmap_reinit_cache(struct regmap *map,
			const struct regmap_config *config);
struct regmap *dev_get_regmap(struct device *dev, const char *name);
struct device *regmap_get_device(struct regmap *map);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int val);
int regmap_write_async(struct regmap *map, unsigned int reg, unsigned int val);
int regmap_raw_write(struct regmap *map, unsigned int reg,
		     const void *val, size_t val_len);
int regmap_noinc_write(struct regmap *map, unsigned int reg,
		     const void *val, size_t val_len);
int regmap_bulk_write(struct regmap *map, unsigned int reg, const void *val,
			size_t val_count);
int regmap_multi_reg_write(struct regmap *map, const struct reg_sequence *regs,
			int num_regs);
int regmap_multi_reg_write_bypassed(struct regmap *map,
				    const struct reg_sequence *regs,
				    int num_regs);
int regmap_raw_write_async(struct regmap *map, unsigned int reg,
			   const void *val, size_t val_len);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val);
int regmap_read_bypassed(struct regmap *map, unsigned int reg, unsigned int *val);
int regmap_raw_read(struct regmap *map, unsigned int reg,
		    void *val, size_t val_len);
int regmap_noinc_read(struct regmap *map, unsigned int reg,
		      void *val, size_t val_len);
int regmap_bulk_read(struct regmap *map, unsigned int reg, void *val,
		     size_t val_count);
int regmap_multi_reg_read(struct regmap *map, const unsigned int *reg, void *val,
			  size_t val_count);
int regmap_update_bits_base(struct regmap *map, unsigned int reg,
			    unsigned int mask, unsigned int val,
			    bool *change, bool async, bool force);

static inline int regmap_update_bits(struct regmap *map, unsigned int reg,
				     unsigned int mask, unsigned int val)
{
	return regmap_update_bits_base(map, reg, mask, val, NULL, false, false);
}

static inline int regmap_update_bits_async(struct regmap *map, unsigned int reg,
					   unsigned int mask, unsigned int val)
{
	return regmap_update_bits_base(map, reg, mask, val, NULL, true, false);
}

static inline int regmap_update_bits_check(struct regmap *map, unsigned int reg,
					   unsigned int mask, unsigned int val,
					   bool *change)
{
	return regmap_update_bits_base(map, reg, mask, val,
				       change, false, false);
}

static inline int
regmap_update_bits_check_async(struct regmap *map, unsigned int reg,
			       unsigned int mask, unsigned int val,
			       bool *change)
{
	return regmap_update_bits_base(map, reg, mask, val,
				       change, true, false);
}

static inline int regmap_write_bits(struct regmap *map, unsigned int reg,
				    unsigned int mask, unsigned int val)
{
	return regmap_update_bits_base(map, reg, mask, val, NULL, false, true);
}

static inline int regmap_default_zero_cb(struct device *dev,
					 unsigned int reg,
					 unsigned int *def)
{
	*def = 0;
	return 0;
}

int regmap_get_val_bytes(struct regmap *map);
int regmap_get_max_register(struct regmap *map);
int regmap_get_reg_stride(struct regmap *map);
bool regmap_might_sleep(struct regmap *map);
int regmap_async_complete(struct regmap *map);
bool regmap_can_raw_write(struct regmap *map);
size_t regmap_get_raw_read_max(struct regmap *map);
size_t regmap_get_raw_write_max(struct regmap *map);

void regcache_sort_defaults(struct reg_default *defaults, unsigned int ndefaults);
int regcache_sync(struct regmap *map);
int regcache_sync_region(struct regmap *map, unsigned int min,
			 unsigned int max);
int regcache_drop_region(struct regmap *map, unsigned int min,
			 unsigned int max);
void regcache_cache_only(struct regmap *map, bool enable);
void regcache_cache_bypass(struct regmap *map, bool enable);
void regcache_mark_dirty(struct regmap *map);
bool regcache_reg_cached(struct regmap *map, unsigned int reg);

bool regmap_check_range_table(struct regmap *map, unsigned int reg,
			      const struct regmap_access_table *table);

int regmap_register_patch(struct regmap *map, const struct reg_sequence *regs,
			  int num_regs);
int regmap_parse_val(struct regmap *map, const void *buf,
				unsigned int *val);

static inline bool regmap_reg_in_range(unsigned int reg,
				       const struct regmap_range *range)
{
	return reg >= range->range_min && reg <= range->range_max;
}

bool regmap_reg_in_ranges(unsigned int reg,
			  const struct regmap_range *ranges,
			  unsigned int nranges);

static inline int regmap_set_bits(struct regmap *map,
				  unsigned int reg, unsigned int bits)
{
	return regmap_update_bits_base(map, reg, bits, bits,
				       NULL, false, false);
}

static inline int regmap_clear_bits(struct regmap *map,
				    unsigned int reg, unsigned int bits)
{
	return regmap_update_bits_base(map, reg, bits, 0, NULL, false, false);
}

static inline int regmap_assign_bits(struct regmap *map, unsigned int reg,
				     unsigned int bits, bool value)
{
	if (value)
		return regmap_set_bits(map, reg, bits);
	else
		return regmap_clear_bits(map, reg, bits);
}

int regmap_test_bits(struct regmap *map, unsigned int reg, unsigned int bits);

/**
 * struct reg_field - Description of an register field
 *
 * @reg: Offset of the register within the regmap bank
 * @lsb: lsb of the register field.
 * @msb: msb of the register field.
 * @id_size: port size if it has some ports
 * @id_offset: address offset for each ports
 */
struct reg_field {
	unsigned int reg;
	unsigned int lsb;
	unsigned int msb;
	unsigned int id_size;
	unsigned int id_offset;
};

#define REG_FIELD(_reg, _lsb, _msb) {		\
				.reg = _reg,	\
				.lsb = _lsb,	\
				.msb = _msb,	\
				}

#define REG_FIELD_ID(_reg, _lsb, _msb, _size, _offset) {	\
				.reg = _reg,			\
				.lsb = _lsb,			\
				.msb = _msb,			\
				.id_size = _size,		\
				.id_offset = _offset,		\
				}

struct regmap_field *regmap_field_alloc(struct regmap *regmap,
		struct reg_field reg_field);
void regmap_field_free(struct regmap_field *field);

DEFINE_FREE(regmap_field, struct regmap_field *, if (_T) regmap_field_free(_T))

struct regmap_field *devm_regmap_field_alloc(struct device *dev,
		struct regmap *regmap, struct reg_field reg_field);
void devm_regmap_field_free(struct device *dev,	struct regmap_field *field);

int regmap_field_bulk_alloc(struct regmap *regmap,
			     struct regmap_field **rm_field,
			     const struct reg_field *reg_field,
			     int num_fields);
void regmap_field_bulk_free(struct regmap_field *field);
int devm_regmap_field_bulk_alloc(struct device *dev, struct regmap *regmap,
				 struct regmap_field **field,
				 const struct reg_field *reg_field,
				 int num_fields);
void devm_regmap_field_bulk_free(struct device *dev,
				 struct regmap_field *field);

int regmap_field_read(struct regmap_field *field, unsigned int *val);
int regmap_field_update_bits_base(struct regmap_field *field,
				  unsigned int mask, unsigned int val,
				  bool *change, bool async, bool force);
int regmap_fields_read(struct regmap_field *field, unsigned int id,
		       unsigned int *val);
int regmap_fields_update_bits_base(struct regmap_field *field,  unsigned int id,
				   unsigned int mask, unsigned int val,
				   bool *change, bool async, bool force);

static inline int regmap_field_write(struct regmap_field *field,
				     unsigned int val)
{
	return regmap_field_update_bits_base(field, ~0, val,
					     NULL, false, false);
}

static inline int regmap_field_force_write(struct regmap_field *field,
					   unsigned int val)
{
	return regmap_field_update_bits_base(field, ~0, val, NULL, false, true);
}

static inline int regmap_field_update_bits(struct regmap_field *field,
					   unsigned int mask, unsigned int val)
{
	return regmap_field_update_bits_base(field, mask, val,
					     NULL, false, false);
}

static inline int regmap_field_set_bits(struct regmap_field *field,
					unsigned int bits)
{
	return regmap_field_update_bits_base(field, bits, bits, NULL, false,
					     false);
}

static inline int regmap_field_clear_bits(struct regmap_field *field,
					  unsigned int bits)
{
	return regmap_field_update_bits_base(field, bits, 0, NULL, false,
					     false);
}

int regmap_field_test_bits(struct regmap_field *field, unsigned int bits);

static inline int
regmap_field_force_update_bits(struct regmap_field *field,
			       unsigned int mask, unsigned int val)
{
	return regmap_field_update_bits_base(field, mask, val,
					     NULL, false, true);
}

static inline int regmap_fields_write(struct regmap_field *field,
				      unsigned int id, unsigned int val)
{
	return regmap_fields_update_bits_base(field, id, ~0, val,
					      NULL, false, false);
}

static inline int regmap_fields_force_write(struct regmap_field *field,
					    unsigned int id, unsigned int val)
{
	return regmap_fields_update_bits_base(field, id, ~0, val,
					      NULL, false, true);
}

static inline int
regmap_fields_update_bits(struct regmap_field *field, unsigned int id,
			  unsigned int mask, unsigned int val)
{
	return regmap_fields_update_bits_base(field, id, mask, val,
					      NULL, false, false);
}

static inline int
regmap_fields_force_update_bits(struct regmap_field *field, unsigned int id,
				unsigned int mask, unsigned int val)
{
	return regmap_fields_update_bits_base(field, id, mask, val,
					      NULL, false, true);
}

/**
 * struct regmap_irq_type - IRQ type definitions.
 *
 * @type_reg_offset: Offset register for the irq type setting.
 * @type_rising_val: Register value to configure RISING type irq.
 * @type_falling_val: Register value to configure FALLING type irq.
 * @type_level_low_val: Register value to configure LEVEL_LOW type irq.
 * @type_level_high_val: Register value to configure LEVEL_HIGH type irq.
 * @types_supported: logical OR of IRQ_TYPE_* flags indicating supported types.
 */
struct regmap_irq_type {
	unsigned int type_reg_offset;
	unsigned int type_reg_mask;
	unsigned int type_rising_val;
	unsigned int type_falling_val;
	unsigned int type_level_low_val;
	unsigned int type_level_high_val;
	unsigned int types_supported;
};

/**
 * struct regmap_irq - Description of an IRQ for the generic regmap irq_chip.
 *
 * @reg_offset: Offset of the status/mask register within the bank
 * @mask:       Mask used to flag/control the register.
 * @type:	IRQ trigger type setting details if supported.
 */
struct regmap_irq {
	unsigned int reg_offset;
	unsigned int mask;
	struct regmap_irq_type type;
};

#define REGMAP_IRQ_REG(_irq, _off, _mask)		\
	[_irq] = { .reg_offset = (_off), .mask = (_mask) }

#define REGMAP_IRQ_REG_LINE(_id, _reg_bits) \
	[_id] = {				\
		.mask = BIT((_id) % (_reg_bits)),	\
		.reg_offset = (_id) / (_reg_bits),	\
	}

#define REGMAP_IRQ_MAIN_REG_OFFSET(arr)				\
	{ .num_regs = ARRAY_SIZE((arr)), .offset = &(arr)[0] }

struct regmap_irq_sub_irq_map {
	unsigned int num_regs;
	unsigned int *offset;
};

struct regmap_irq_chip_data;

/**
 * struct regmap_irq_chip - Description of a generic regmap irq_chip.
 *
 * @name:        Descriptive name for IRQ controller.
 * @domain_suffix: Name suffix to be appended to end of IRQ domain name. Needed
 *		   when multiple regmap-IRQ controllers are created from same
 *		   device.
 *
 * @main_status: Base main status register address. For chips which have
 *		 interrupts arranged in separate sub-irq blocks with own IRQ
 *		 registers and which have a main IRQ registers indicating
 *		 sub-irq blocks with unhandled interrupts. For such chips fill
 *		 sub-irq register information in status_base, mask_base and
 *		 ack_base.
 * @num_main_status_bits: Should be given to chips where number of meaningfull
 *			  main status bits differs from num_regs.
 * @sub_reg_offsets: arrays of mappings from main register bits to sub irq
 *		     registers. First item in array describes the registers
 *		     for first main status bit. Second array for second bit etc.
 *		     Offset is given as sub register status offset to
 *		     status_base. Should contain num_regs arrays.
 *		     Can be provided for chips with more complex mapping than
 *		     1.st bit to 1.st sub-reg, 2.nd bit to 2.nd sub-reg, ...
 * @num_main_regs: Number of 'main status' irq registers for chips which have
 *		   main_status set.
 *
 * @status_base: Base status register address.
 * @mask_base:   Base mask register address. Mask bits are set to 1 when an
 *               interrupt is masked, 0 when unmasked.
 * @unmask_base:  Base unmask register address. Unmask bits are set to 1 when
 *                an interrupt is unmasked and 0 when masked.
 * @ack_base:    Base ack address. If zero then the chip is clear on read.
 *               Using zero value is possible with @use_ack bit.
 * @wake_base:   Base address for wake enables.  If zero unsupported.
 * @config_base: Base address for IRQ type config regs. If null unsupported.
 * @irq_reg_stride:  Stride to use for chips where registers are not contiguous.
 * @init_ack_masked: Ack all masked interrupts once during initalization.
 * @mask_unmask_non_inverted: Controls mask bit inversion for chips that set
 *	both @mask_base and @unmask_base. If false, mask and unmask bits are
 *	inverted (which is deprecated behavior); if true, bits will not be
 *	inverted and the registers keep their normal behavior. Note that if
 *	you use only one of @mask_base or @unmask_base, this flag has no
 *	effect and is unnecessary. Any new drivers that set both @mask_base
 *	and @unmask_base should set this to true to avoid relying on the
 *	deprecated behavior.
 * @use_ack:     Use @ack register even if it is zero.
 * @ack_invert:  Inverted ack register: cleared bits for ack.
 * @clear_ack:  Use this to set 1 and 0 or vice-versa to clear interrupts.
 * @status_invert: Inverted status register: cleared bits are active interrupts.
 * @status_is_level: Status register is actuall signal level: Xor status
 *		     register with previous value to get active interrupts.
 * @wake_invert: Inverted wake register: cleared bits are wake disabled.
 * @type_in_mask: Use the mask registers for controlling irq type. Use this if
 *		  the hardware provides separate bits for rising/falling edge
 *		  or low/high level interrupts and they should be combined into
 *		  a single logical interrupt. Use &struct regmap_irq_type data
 *		  to define the mask bit for each irq type.
 * @clear_on_unmask: For chips with interrupts cleared on read: read the status
 *                   registers before unmasking interrupts to clear any bits
 *                   set when they were masked.
 * @runtime_pm:  Hold a runtime PM lock on the device when accessing it.
 * @no_status: No status register: all interrupts assumed generated by device.
 *
 * @num_regs:    Number of registers in each control bank.
 *
 * @irqs:        Descriptors for individual IRQs.  Interrupt numbers are
 *               assigned based on the index in the array of the interrupt.
 * @num_irqs:    Number of descriptors.
 * @num_config_bases:	Number of config base registers.
 * @num_config_regs:	Number of config registers for each config base register.
 *
 * @handle_pre_irq:  Driver specific callback to handle interrupt from device
 *		     before regmap_irq_handler process the interrupts.
 * @handle_post_irq: Driver specific callback to handle interrupt from device
 *		     after handling the interrupts in regmap_irq_handler().
 * @handle_mask_sync: Callback used to handle IRQ mask syncs. The index will be
 *		      in the range [0, num_regs)
 * @set_type_config: Callback used for configuring irq types.
 * @get_irq_reg: Callback for mapping (base register, index) pairs to register
 *		 addresses. The base register will be one of @status_base,
 *		 @mask_base, etc., @main_status, or any of @config_base.
 *		 The index will be in the range [0, num_main_regs[ for the
 *		 main status base, [0, num_config_regs[ for any config
 *		 register base, and [0, num_regs[ for any other base.
 *		 If unspecified then regmap_irq_get_irq_reg_linear() is used.
 * @irq_drv_data:    Driver specific IRQ data which is passed as parameter when
 *		     driver specific pre/post interrupt handler is called.
 *
 * This is not intended to handle every possible interrupt controller, but
 * it should handle a substantial proportion of those that are found in the
 * wild.
 */
struct regmap_irq_chip {
	const char *name;
	const char *domain_suffix;

	unsigned int main_status;
	unsigned int num_main_status_bits;
	const struct regmap_irq_sub_irq_map *sub_reg_offsets;
	int num_main_regs;

	unsigned int status_base;
	unsigned int mask_base;
	unsigned int unmask_base;
	unsigned int ack_base;
	unsigned int wake_base;
	const unsigned int *config_base;
	unsigned int irq_reg_stride;
	unsigned int init_ack_masked:1;
	unsigned int mask_unmask_non_inverted:1;
	unsigned int use_ack:1;
	unsigned int ack_invert:1;
	unsigned int clear_ack:1;
	unsigned int status_invert:1;
	unsigned int status_is_level:1;
	unsigned int wake_invert:1;
	unsigned int type_in_mask:1;
	unsigned int clear_on_unmask:1;
	unsigned int runtime_pm:1;
	unsigned int no_status:1;

	int num_regs;

	const struct regmap_irq *irqs;
	int num_irqs;

	int num_config_bases;
	int num_config_regs;

	int (*handle_pre_irq)(void *irq_drv_data);
	int (*handle_post_irq)(void *irq_drv_data);
	int (*handle_mask_sync)(int index, unsigned int mask_buf_def,
				unsigned int mask_buf, void *irq_drv_data);
	int (*set_type_config)(unsigned int **buf, unsigned int type,
			       const struct regmap_irq *irq_data, int idx,
			       void *irq_drv_data);
	unsigned int (*get_irq_reg)(struct regmap_irq_chip_data *data,
				    unsigned int base, int index);
	void *irq_drv_data;
};

unsigned int regmap_irq_get_irq_reg_linear(struct regmap_irq_chip_data *data,
					   unsigned int base, int index);
int regmap_irq_set_type_config_simple(unsigned int **buf, unsigned int type,
				      const struct regmap_irq *irq_data,
				      int idx, void *irq_drv_data);

int regmap_add_irq_chip(struct regmap *map, int irq, int irq_flags,
			int irq_base, const struct regmap_irq_chip *chip,
			struct regmap_irq_chip_data **data);
int regmap_add_irq_chip_fwnode(struct fwnode_handle *fwnode,
			       struct regmap *map, int irq,
			       int irq_flags, int irq_base,
			       const struct regmap_irq_chip *chip,
			       struct regmap_irq_chip_data **data);
void regmap_del_irq_chip(int irq, struct regmap_irq_chip_data *data);

int devm_regmap_add_irq_chip(struct device *dev, struct regmap *map, int irq,
			     int irq_flags, int irq_base,
			     const struct regmap_irq_chip *chip,
			     struct regmap_irq_chip_data **data);
int devm_regmap_add_irq_chip_fwnode(struct device *dev,
				    struct fwnode_handle *fwnode,
				    struct regmap *map, int irq,
				    int irq_flags, int irq_base,
				    const struct regmap_irq_chip *chip,
				    struct regmap_irq_chip_data **data);
void devm_regmap_del_irq_chip(struct device *dev, int irq,
			      struct regmap_irq_chip_data *data);

int regmap_irq_chip_get_base(struct regmap_irq_chip_data *data);
int regmap_irq_get_virq(struct regmap_irq_chip_data *data, int irq);
struct irq_domain *regmap_irq_get_domain(struct regmap_irq_chip_data *data);

#else

/*
 * These stubs should only ever be called by generic code which has
 * regmap based facilities, if they ever get called at runtime
 * something is going wrong and something probably needs to select
 * REGMAP.
 */

static inline int regmap_write(struct regmap *map, unsigned int reg,
			       unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_write_async(struct regmap *map, unsigned int reg,
				     unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_raw_write(struct regmap *map, unsigned int reg,
				   const void *val, size_t val_len)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_raw_write_async(struct regmap *map, unsigned int reg,
					 const void *val, size_t val_len)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_noinc_write(struct regmap *map, unsigned int reg,
				    const void *val, size_t val_len)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_bulk_write(struct regmap *map, unsigned int reg,
				    const void *val, size_t val_count)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_read(struct regmap *map, unsigned int reg,
			      unsigned int *val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_read_bypassed(struct regmap *map, unsigned int reg,
				       unsigned int *val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_raw_read(struct regmap *map, unsigned int reg,
				  void *val, size_t val_len)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_noinc_read(struct regmap *map, unsigned int reg,
				    void *val, size_t val_len)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_bulk_read(struct regmap *map, unsigned int reg,
				   void *val, size_t val_count)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_update_bits_base(struct regmap *map, unsigned int reg,
					  unsigned int mask, unsigned int val,
					  bool *change, bool async, bool force)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_set_bits(struct regmap *map,
				  unsigned int reg, unsigned int bits)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_clear_bits(struct regmap *map,
				    unsigned int reg, unsigned int bits)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_assign_bits(struct regmap *map, unsigned int reg,
				     unsigned int bits, bool value)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_test_bits(struct regmap *map,
				   unsigned int reg, unsigned int bits)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_field_update_bits_base(struct regmap_field *field,
					unsigned int mask, unsigned int val,
					bool *change, bool async, bool force)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_fields_update_bits_base(struct regmap_field *field,
				   unsigned int id,
				   unsigned int mask, unsigned int val,
				   bool *change, bool async, bool force)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_update_bits(struct regmap *map, unsigned int reg,
				     unsigned int mask, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_update_bits_async(struct regmap *map, unsigned int reg,
					   unsigned int mask, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_update_bits_check(struct regmap *map, unsigned int reg,
					   unsigned int mask, unsigned int val,
					   bool *change)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int
regmap_update_bits_check_async(struct regmap *map, unsigned int reg,
			       unsigned int mask, unsigned int val,
			       bool *change)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_write_bits(struct regmap *map, unsigned int reg,
				    unsigned int mask, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_field_write(struct regmap_field *field,
				     unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_field_force_write(struct regmap_field *field,
					   unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_field_update_bits(struct regmap_field *field,
					   unsigned int mask, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int
regmap_field_force_update_bits(struct regmap_field *field,
			       unsigned int mask, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_field_set_bits(struct regmap_field *field,
					unsigned int bits)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_field_clear_bits(struct regmap_field *field,
					  unsigned int bits)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_field_test_bits(struct regmap_field *field,
					 unsigned int bits)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_fields_write(struct regmap_field *field,
				      unsigned int id, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_fields_force_write(struct regmap_field *field,
					    unsigned int id, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int
regmap_fields_update_bits(struct regmap_field *field, unsigned int id,
			  unsigned int mask, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int
regmap_fields_force_update_bits(struct regmap_field *field, unsigned int id,
				unsigned int mask, unsigned int val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_get_val_bytes(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_get_max_register(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_get_reg_stride(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline bool regmap_might_sleep(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
	return true;
}

static inline void regcache_sort_defaults(struct reg_default *defaults,
					  unsigned int ndefaults)
{
	WARN_ONCE(1, "regmap API is disabled");
}

static inline int regcache_sync(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regcache_sync_region(struct regmap *map, unsigned int min,
				       unsigned int max)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regcache_drop_region(struct regmap *map, unsigned int min,
				       unsigned int max)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline void regcache_cache_only(struct regmap *map, bool enable)
{
	WARN_ONCE(1, "regmap API is disabled");
}

static inline void regcache_cache_bypass(struct regmap *map, bool enable)
{
	WARN_ONCE(1, "regmap API is disabled");
}

static inline void regcache_mark_dirty(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
}

static inline void regmap_async_complete(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
}

static inline int regmap_register_patch(struct regmap *map,
					const struct reg_sequence *regs,
					int num_regs)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline int regmap_parse_val(struct regmap *map, const void *buf,
				unsigned int *val)
{
	WARN_ONCE(1, "regmap API is disabled");
	return -EINVAL;
}

static inline struct regmap *dev_get_regmap(struct device *dev,
					    const char *name)
{
	return NULL;
}

static inline struct device *regmap_get_device(struct regmap *map)
{
	WARN_ONCE(1, "regmap API is disabled");
	return NULL;
}

#endif

#endif
