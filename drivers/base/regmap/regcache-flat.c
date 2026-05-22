// SPDX-License-Identifier: GPL-2.0
//
// Register cache 访问 API - flat cache 支持
//
// Copyright 2012 Wolfson Microelectronics plc
//
// Author: Mark Brown <broonie@opensource.wolfsonmicro.com>

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/limits.h>
#include <linux/overflow.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "internal.h"

static inline unsigned int regcache_flat_get_index(const struct regmap *map,
						   unsigned int reg)
{
	return regcache_get_index_by_order(map, reg);
}

struct regcache_flat_data {
	unsigned long *valid;	/* 哪些缓存槽位已经被真正初始化/写入 */
	unsigned int data[];	/* 按寄存器索引直接寻址的平铺缓存数组 */
};

static int regcache_flat_init(struct regmap *map)
{
	unsigned int cache_size;
	struct regcache_flat_data *cache;

	if (!map || map->reg_stride_order < 0 || !map->max_register_is_set)
		return -EINVAL;

	/* flat cache 直接按“最大寄存器索引 + 1”分配数组空间，换取 O(1) 定位。 */
	cache_size = regcache_flat_get_index(map, map->max_register) + 1;
	cache = kzalloc_flex(*cache, data, cache_size, map->alloc_flags);
	if (!cache)
		return -ENOMEM;

	cache->valid = bitmap_zalloc(cache_size, map->alloc_flags);
	if (!cache->valid)
		goto err_free;

	map->cache = cache;

	return 0;

err_free:
	kfree(cache);
	return -ENOMEM;
}

static int regcache_flat_exit(struct regmap *map)
{
	struct regcache_flat_data *cache = map->cache;

	if (cache)
		bitmap_free(cache->valid);

	kfree(cache);
	map->cache = NULL;

	return 0;
}

static int regcache_flat_populate(struct regmap *map)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int i;

	for (i = 0; i < map->num_reg_defaults; i++) {
		unsigned int reg = map->reg_defaults[i].reg;
		unsigned int index = regcache_flat_get_index(map, reg);

		cache->data[index] = map->reg_defaults[i].def;
		__set_bit(index, cache->valid);
	}

	if (map->reg_default_cb) {
		dev_dbg(map->dev,
			"Populating regcache_flat using reg_default_cb callback\n");

		/* 对于默认值表没有覆盖到的空洞寄存器，再逐项回调查询默认值。 */
		for (i = 0; i <= map->max_register; i += map->reg_stride) {
			unsigned int index = regcache_flat_get_index(map, i);
			unsigned int value;

			if (test_bit(index, cache->valid))
				continue;

			if (map->reg_default_cb(map->dev, i, &value))
				continue;

			cache->data[index] = value;
			__set_bit(index, cache->valid);
		}
	}

	return 0;
}

static int regcache_flat_read(struct regmap *map,
			      unsigned int reg, unsigned int *value)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int index = regcache_flat_get_index(map, reg);

	/* 传统 flat cache 的兼容语义是：即使槽位未标记 valid，也直接返回数组内容。
	 * 这种行为可能把“零初始化的空洞值”误当成真实寄存器值，因此只保留兼容，
	 * 同时发一次警告提示调用者。
	 */
	if (unlikely(!test_bit(index, cache->valid)))
		dev_warn_once(map->dev,
			"using zero-initialized flat cache, this may cause unexpected behavior");

	*value = cache->data[index];

	return 0;
}

static int regcache_flat_sparse_read(struct regmap *map,
				     unsigned int reg, unsigned int *value)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int index = regcache_flat_get_index(map, reg);

	/* sparse 版本要求显式命中 valid 位，否则视为缓存中不存在。 */
	if (unlikely(!test_bit(index, cache->valid)))
		return -ENOENT;

	*value = cache->data[index];

	return 0;
}

static int regcache_flat_write(struct regmap *map, unsigned int reg,
			       unsigned int value)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int index = regcache_flat_get_index(map, reg);

	cache->data[index] = value;
	__set_bit(index, cache->valid);

	return 0;
}

static int regcache_flat_drop(struct regmap *map, unsigned int min,
			      unsigned int max)
{
	struct regcache_flat_data *cache = map->cache;
	unsigned int bitmap_min = regcache_flat_get_index(map, min);
	unsigned int bitmap_max = regcache_flat_get_index(map, max);

	bitmap_clear(cache->valid, bitmap_min, bitmap_max + 1 - bitmap_min);

	return 0;
}

struct regcache_ops regcache_flat_ops = {
	.type = REGCACHE_FLAT,
	.name = "flat",
	.init = regcache_flat_init,
	.exit = regcache_flat_exit,
	.populate = regcache_flat_populate,
	.read = regcache_flat_read,
	.write = regcache_flat_write,
};

struct regcache_ops regcache_flat_sparse_ops = {
	.type = REGCACHE_FLAT_S,
	.name = "flat-sparse",
	.init = regcache_flat_init,
	.exit = regcache_flat_exit,
	.populate = regcache_flat_populate,
	.read = regcache_flat_sparse_read,
	.write = regcache_flat_write,
	.drop = regcache_flat_drop,
};
