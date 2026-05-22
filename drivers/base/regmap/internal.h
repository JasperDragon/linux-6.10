/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register map 访问 API 内部头文件
 *
 * Copyright 2011 Wolfson Microelectronics plc
 *
 * Author: Mark Brown <broonie@opensource.wolfsonmicro.com>
 */

#ifndef _REGMAP_INTERNAL_H
#define _REGMAP_INTERNAL_H

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/wait.h>

struct regmap;
struct regcache_ops;

struct regmap_debugfs_off_cache {
	struct list_head list;
	off_t min;
	off_t max;
	unsigned int base_reg;
	unsigned int max_reg;
};

/* 描述“逻辑寄存器/值”如何编码到实际总线缓冲区。
 * regmap core 先根据 reg_bits/val_bits/pad_bits/endianness 选好格式器，
 * 再借助这里的回调把一个抽象的读写操作落成具体字节序列。
 */
struct regmap_format {
	size_t buf_size;
	size_t reg_bytes;
	size_t pad_bytes;
	size_t val_bytes;
	s8 reg_shift;
	void (*format_write)(struct regmap *map,
			     unsigned int reg, unsigned int val);
	void (*format_reg)(void *buf, unsigned int reg, unsigned int shift);
	void (*format_val)(void *buf, unsigned int val, unsigned int shift);
	unsigned int (*parse_val)(const void *buf);
	void (*parse_inplace)(void *buf);
};

struct regmap_async {
	struct list_head list;
	struct regmap *map;
	void *work_buf;		/* 异步提交时独占的格式化缓冲区 */
};

/* regmap 运行时核心对象。
 * 它既保存访问后端（bus/context/format），也保存访问约束、缓存策略、
 * debugfs 状态和异步队列，是整个框架的中心状态容器。
 */
struct regmap {
	union {
		struct mutex mutex;
		struct {
			spinlock_t spinlock;
			unsigned long spinlock_flags;
		};
		struct {
			raw_spinlock_t raw_spinlock;
			unsigned long raw_spinlock_flags;
		};
	};
	struct lock_class_key *lock_key;
	regmap_lock lock;
	regmap_unlock unlock;
	void *lock_arg; /* 原样透传给 lock/unlock 回调的私有参数 */
	gfp_t alloc_flags;
	unsigned int reg_base;	/* 对外逻辑寄存器到硬件地址的统一基址偏移 */

	struct device *dev; /* 实际发生寄存器访问的设备 */
	void *work_buf;     /* 访问前拼包/解包时使用的临时缓冲区 */
	struct regmap_format format;  /* 当前 reg/val 编码格式 */
	const struct regmap_bus *bus;
	void *bus_context;
	const char *name;

	spinlock_t async_lock;
	wait_queue_head_t async_waitq;
	struct list_head async_list;
	struct list_head async_free;
	int async_ret;
	bool async;

#ifdef CONFIG_DEBUG_FS
	bool debugfs_disable;
	struct dentry *debugfs;
	const char *debugfs_name;
	int debugfs_dummy_id;

	unsigned int debugfs_reg_len;
	unsigned int debugfs_val_len;
	unsigned int debugfs_tot_len;

	struct list_head debugfs_off_cache;
	struct mutex cache_lock;
#endif

	unsigned int max_register;
	bool max_register_is_set;
	bool (*writeable_reg)(struct device *dev, unsigned int reg);
	bool (*readable_reg)(struct device *dev, unsigned int reg);
	bool (*volatile_reg)(struct device *dev, unsigned int reg);
	bool (*precious_reg)(struct device *dev, unsigned int reg);
	bool (*writeable_noinc_reg)(struct device *dev, unsigned int reg);
	bool (*readable_noinc_reg)(struct device *dev, unsigned int reg);
	const struct regmap_access_table *wr_table;
	const struct regmap_access_table *rd_table;
	const struct regmap_access_table *volatile_table;
	const struct regmap_access_table *precious_table;
	const struct regmap_access_table *wr_noinc_table;
	const struct regmap_access_table *rd_noinc_table;

	int (*reg_read)(void *context, unsigned int reg, unsigned int *val);
	int (*reg_write)(void *context, unsigned int reg, unsigned int val);
	int (*reg_update_bits)(void *context, unsigned int reg,
			       unsigned int mask, unsigned int val);
	/* bulk read/write 层面的底层访问回调 */
	int (*read)(void *context, const void *reg_buf, size_t reg_size,
		    void *val_buf, size_t val_size);
	int (*write)(void *context, const void *data, size_t count);

	int (*reg_default_cb)(struct device *dev, unsigned int reg,
			      unsigned int *val);

	unsigned long read_flag_mask;
	unsigned long write_flag_mask;

	/* 格式化寄存器地址时，要额外左移的位数 */
	int reg_shift;
	int reg_stride;
	int reg_stride_order;

	bool defer_caching;

	/* 若置位，field 写操作即使值未变化也强制落到硬件。 */
	bool force_write_field;

	/* regcache 专用状态 */
	const struct regcache_ops *cache_ops;
	enum regcache_type cache_type;

	/* reg_defaults_raw 中的总字节数 */
	unsigned int cache_size_raw;
	/* reg_defaults_raw 中每个寄存器值占用的字节数 */
	unsigned int cache_word_size;
	/* reg_defaults 数组中的条目数 */
	unsigned int num_reg_defaults;
	/* reg_defaults_raw 对应的寄存器项数 */
	unsigned int num_reg_defaults_raw;

	/* 置位后只改缓存，不落硬件 */
	bool cache_only;
	/* 置位后只改硬件，不碰缓存 */
	bool cache_bypass;
	/* 置位后表示 reg_defaults_raw 由 regmap 动态分配并负责释放 */
	bool cache_free;

	struct reg_default *reg_defaults;
	const void *reg_defaults_raw;
	void *cache;
	/* 置位后表示缓存比硬件更新，需要后续 sync 回写 */
	bool cache_dirty;
	/* 置位后表示硬件当前状态已知与 reg_defaults 一致 */
	bool no_sync_defaults;

	struct reg_sequence *patch;
	unsigned int patch_regs;

	/* 置位后表示该 regmap 的底层访问允许睡眠 */
	bool can_sleep;

	/* 置位后把 bulk read 拆成单寄存器读 */
	bool use_single_read;
	/* 置位后把 bulk write 拆成单寄存器写 */
	bool use_single_write;
	/* 置位后表示后端支持 multi write */
	bool can_multi_write;

	/* 非 0 时限制 raw read/write 的最大传输大小 */
	size_t max_raw_read;
	size_t max_raw_write;

	struct rb_root range_tree;
	void *selector_work_buf;	/* range selector 切换时使用的临时缓冲区 */

	struct hwspinlock *hwlock;
};

/* 各类寄存器缓存后端对 regmap core 暴露的统一操作表。 */
struct regcache_ops {
	const char *name;
	enum regcache_type type;
	int (*init)(struct regmap *map);
	int (*exit)(struct regmap *map);
	int (*populate)(struct regmap *map);
#ifdef CONFIG_DEBUG_FS
	void (*debugfs_init)(struct regmap *map);
#endif
	int (*read)(struct regmap *map, unsigned int reg, unsigned int *value);
	int (*write)(struct regmap *map, unsigned int reg, unsigned int value);
	int (*sync)(struct regmap *map, unsigned int min, unsigned int max);
	int (*drop)(struct regmap *map, unsigned int min, unsigned int max);
};

bool regmap_cached(struct regmap *map, unsigned int reg);
bool regmap_writeable(struct regmap *map, unsigned int reg);
bool regmap_readable(struct regmap *map, unsigned int reg);
bool regmap_volatile(struct regmap *map, unsigned int reg);
bool regmap_precious(struct regmap *map, unsigned int reg);
bool regmap_writeable_noinc(struct regmap *map, unsigned int reg);
bool regmap_readable_noinc(struct regmap *map, unsigned int reg);

int _regmap_write(struct regmap *map, unsigned int reg,
		  unsigned int val);

struct regmap_range_node {
	struct rb_node node;
	const char *name;
	struct regmap *map;

	unsigned int range_min;
	unsigned int range_max;

	unsigned int selector_reg;
	unsigned int selector_mask;
	int selector_shift;

	unsigned int window_start;
	unsigned int window_len;
};

struct regmap_field {
	struct regmap *regmap;
	unsigned int mask;
	/* 字段最低有效位位置 */
	unsigned int shift;
	unsigned int reg;

	unsigned int id_size;
	unsigned int id_offset;
};

#ifdef CONFIG_DEBUG_FS
extern void regmap_debugfs_initcall(void);
extern void regmap_debugfs_init(struct regmap *map);
extern void regmap_debugfs_exit(struct regmap *map);

static inline void regmap_debugfs_disable(struct regmap *map)
{
	map->debugfs_disable = true;
}

#else
static inline void regmap_debugfs_initcall(void) { }
static inline void regmap_debugfs_init(struct regmap *map) { }
static inline void regmap_debugfs_exit(struct regmap *map) { }
static inline void regmap_debugfs_disable(struct regmap *map) { }
#endif

/* regcache core declarations */
int regcache_init(struct regmap *map, const struct regmap_config *config);
void regcache_exit(struct regmap *map);
int regcache_read(struct regmap *map,
		       unsigned int reg, unsigned int *value);
int regcache_write(struct regmap *map,
			unsigned int reg, unsigned int value);
int regcache_sync(struct regmap *map);
int regcache_sync_block(struct regmap *map, void *block,
			unsigned long *cache_present,
			unsigned int block_base, unsigned int start,
			unsigned int end);
bool regcache_reg_needs_sync(struct regmap *map, unsigned int reg,
			     unsigned int val);

static inline const void *regcache_get_val_addr(struct regmap *map,
						const void *base,
						unsigned int idx)
{
	return base + (map->cache_word_size * idx);
}

unsigned int regcache_get_val(struct regmap *map, const void *base,
			      unsigned int idx);
void regcache_set_val(struct regmap *map, void *base, unsigned int idx,
		      unsigned int val);
int regcache_lookup_reg(struct regmap *map, unsigned int reg);
int regcache_sync_val(struct regmap *map, unsigned int reg, unsigned int val);

int _regmap_raw_write(struct regmap *map, unsigned int reg,
		      const void *val, size_t val_len, bool noinc);

void regmap_async_complete_cb(struct regmap_async *async, int ret);

enum regmap_endian regmap_get_val_endian(struct device *dev,
					 const struct regmap_bus *bus,
					 const struct regmap_config *config);

extern struct regcache_ops regcache_flat_sparse_ops;
extern struct regcache_ops regcache_rbtree_ops;
extern struct regcache_ops regcache_maple_ops;
extern struct regcache_ops regcache_flat_ops;

static inline const char *regmap_name(const struct regmap *map)
{
	if (map->dev)
		return dev_name(map->dev);

	return map->name;
}

static inline unsigned int regmap_get_offset(const struct regmap *map,
					     unsigned int index)
{
	if (map->reg_stride_order >= 0)
		return index << map->reg_stride_order;
	else
		return index * map->reg_stride;
}

static inline unsigned int regcache_get_index_by_order(const struct regmap *map,
						       unsigned int reg)
{
	return reg >> map->reg_stride_order;
}

struct regmap_ram_data {
	unsigned int *vals;  /* Allocatd by caller */
	bool *read;
	bool *written;
	enum regmap_endian reg_endian;
	bool (*noinc_reg)(struct regmap_ram_data *data, unsigned int reg);
};

/*
 * Create a test register map with data stored in RAM, not intended
 * for practical use.
 */
struct regmap *__regmap_init_ram(struct device *dev,
				 const struct regmap_config *config,
				 struct regmap_ram_data *data,
				 struct lock_class_key *lock_key,
				 const char *lock_name);

#define regmap_init_ram(dev, config, data)					\
	__regmap_lockdep_wrapper(__regmap_init_ram, #dev, dev, config, data)

struct regmap *__regmap_init_raw_ram(struct device *dev,
				     const struct regmap_config *config,
				     struct regmap_ram_data *data,
				     struct lock_class_key *lock_key,
				     const char *lock_name);

#define regmap_init_raw_ram(dev, config, data)				\
	__regmap_lockdep_wrapper(__regmap_init_raw_ram, #dev, dev, config, data)

#endif
