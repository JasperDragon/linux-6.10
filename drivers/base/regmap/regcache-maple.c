// SPDX-License-Identifier: GPL-2.0
//
// Register cache 访问 API - 基于 maple tree 的缓存
//
// Copyright 2023 Arm, Ltd
//
// Author: Mark Brown <broonie@kernel.org>

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/maple_tree.h>
#include <linux/slab.h>

#include "internal.h"

static int regcache_maple_read(struct regmap *map,
			       unsigned int reg, unsigned int *value)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, reg, reg);
	unsigned long *entry;

	rcu_read_lock();

	/* Maple tree 里每个 entry 保存一段连续寄存器值数组。 */
	entry = mas_walk(&mas);
	if (!entry) {
		rcu_read_unlock();
		return -ENOENT;
	}

	*value = entry[reg - mas.index];

	rcu_read_unlock();

	return 0;
}

static int regcache_maple_write(struct regmap *map, unsigned int reg,
				unsigned int val)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, reg, reg);
	unsigned long *entry, *upper, *lower;
	unsigned long index, last;
	size_t lower_sz, upper_sz;
	int ret;

	rcu_read_lock();

	entry = mas_walk(&mas);
	if (entry) {
		entry[reg - mas.index] = val;
		rcu_read_unlock();
		return 0;
	}

	/* 看看左右是否已有相邻块，尽量把新值并入或桥接成更大的连续块。 */
	mas_set_range(&mas, reg - 1, reg + 1);
	index = reg;
	last = reg;

	lower = mas_find(&mas, reg - 1);
	if (lower) {
		index = mas.index;
		lower_sz = (mas.last - mas.index + 1) * sizeof(unsigned long);
	}

	upper = mas_find(&mas, reg + 1);
	if (upper) {
		last = mas.last;
		upper_sz = (mas.last - mas.index + 1) * sizeof(unsigned long);
	}

	rcu_read_unlock();

	entry = kmalloc_array(last - index + 1, sizeof(*entry), map->alloc_flags);
	if (!entry)
		return -ENOMEM;

	if (lower)
		memcpy(entry, lower, lower_sz);
	entry[reg - index] = val;
	if (upper)
		memcpy(&entry[reg - index + 1], upper, upper_sz);

	/*
	 * 逻辑上 regmap 自身的锁已经足够保护这里的修改，
	 * 但 maple tree 内部有 lockdep 断言，因此仍需显式拿它的锁。
	 */
	mas_lock(&mas);

	mas_set_range(&mas, index, last);
	ret = mas_store_gfp(&mas, entry, map->alloc_flags);

	mas_unlock(&mas);

	if (ret) {
		kfree(entry);
		return ret;
	}
	kfree(lower);
	kfree(upper);
	return 0;
}

static int regcache_maple_drop(struct regmap *map, unsigned int min,
			       unsigned int max)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, min, max);
	unsigned long *entry, *lower, *upper;
	/* 初始化只是为了规避编译器对未初始化路径的误报。 */
	unsigned long lower_index = 0, lower_last = 0;
	unsigned long upper_index, upper_last;
	int ret = 0;

	lower = NULL;
	upper = NULL;

	mas_lock(&mas);

	mas_for_each(&mas, entry, max) {
		/*
		 * 与 write 路径相同：功能上 regmap 锁已经串行化，
		 * 这里拿 maple 锁主要是满足其内部 lockdep 断言。
		 */
		mas_unlock(&mas);

		/* 如果被删区间只覆盖 entry 的中间一段，就要把上下残留部分先备份。 */
		if (mas.index < min) {
			lower_index = mas.index;
			lower_last = min -1;

			lower = kmemdup_array(entry,
					      min - mas.index, sizeof(*lower),
					      map->alloc_flags);
			if (!lower) {
				ret = -ENOMEM;
				goto out_unlocked;
			}
		}

		if (mas.last > max) {
			upper_index = max + 1;
			upper_last = mas.last;

			upper = kmemdup_array(&entry[max - mas.index + 1],
					      mas.last - max, sizeof(*upper),
					      map->alloc_flags);
			if (!upper) {
				ret = -ENOMEM;
				goto out_unlocked;
			}
		}

		kfree(entry);
		mas_lock(&mas);
		mas_erase(&mas);

		/* 再把保留下来的上下半段作为新节点重新插回去。 */
		if (lower) {
			mas_set_range(&mas, lower_index, lower_last);
			ret = mas_store_gfp(&mas, lower, map->alloc_flags);
			if (ret != 0)
				goto out;
			lower = NULL;
		}

		if (upper) {
			mas_set_range(&mas, upper_index, upper_last);
			ret = mas_store_gfp(&mas, upper, map->alloc_flags);
			if (ret != 0)
				goto out;
			upper = NULL;
		}
	}

out:
	mas_unlock(&mas);
out_unlocked:
	kfree(lower);
	kfree(upper);

	return ret;
}

static int regcache_maple_sync_block(struct regmap *map, unsigned long *entry,
				     struct ma_state *mas,
				     unsigned int min, unsigned int max)
{
	void *buf;
	unsigned long r;
	size_t val_bytes = map->format.val_bytes;
	int ret = 0;

	mas_pause(mas);
	rcu_read_unlock();

	/*
	 * 如果是一段连续寄存器，而且后端支持 raw write，
	 * 就尽量合并成一次大事务，以减少总线开销。
	 */
	if (max - min > 1 && regmap_can_raw_write(map)) {
		buf = kmalloc_array(max - min, val_bytes, map->alloc_flags);
		if (!buf) {
			ret = -ENOMEM;
			goto out;
		}

		/* 先把 maple 缓存里的 machine word 重新编码成设备原生值格式。 */
		for (r = min; r < max; r++) {
			regcache_set_val(map, buf, r - min,
					 entry[r - mas->index]);
		}

		ret = _regmap_raw_write(map, min, buf, (max - min) * val_bytes,
					false);

		kfree(buf);
	} else {
		for (r = min; r < max; r++) {
			ret = _regmap_write(map, r,
					    entry[r - mas->index]);
			if (ret != 0)
				goto out;
		}
	}

out:
	rcu_read_lock();

	return ret;
}

static int regcache_maple_sync(struct regmap *map, unsigned int min,
			       unsigned int max)
{
	struct maple_tree *mt = map->cache;
	unsigned long *entry;
	MA_STATE(mas, mt, min, max);
	unsigned long lmin = min;
	unsigned long lmax = max;
	unsigned int r, v, sync_start;
	int ret = 0;
	bool sync_needed = false;

	/* sync 阶段必须直接命中硬件，避免“从缓存再写回缓存”的自循环。 */
	map->cache_bypass = true;

	rcu_read_lock();

	mas_for_each(&mas, entry, max) {
		for (r = max(mas.index, lmin); r <= min(mas.last, lmax); r++) {
			v = entry[r - mas.index];

			if (regcache_reg_needs_sync(map, r, v)) {
				if (!sync_needed) {
					sync_start = r;
					sync_needed = true;
				}
				continue;
			}

			if (!sync_needed)
				continue;

			ret = regcache_maple_sync_block(map, entry, &mas,
							sync_start, r);
			if (ret != 0)
				goto out;
			sync_needed = false;
		}

		if (sync_needed) {
			ret = regcache_maple_sync_block(map, entry, &mas,
							sync_start, r);
			if (ret != 0)
				goto out;
			sync_needed = false;
		}
	}

out:
	rcu_read_unlock();

	map->cache_bypass = false;

	return ret;
}

static int regcache_maple_init(struct regmap *map)
{
	struct maple_tree *mt;

	mt = kmalloc_obj(*mt, map->alloc_flags);
	if (!mt)
		return -ENOMEM;
	map->cache = mt;

	mt_init(mt);

	if (!mt_external_lock(mt) && map->lock_key)
		lockdep_set_class_and_subclass(&mt->ma_lock, map->lock_key, 1);

	return 0;
}

static int regcache_maple_exit(struct regmap *map)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, 0, UINT_MAX);
	unsigned int *entry;

	/* if we've already been called then just return */
	if (!mt)
		return 0;

	mas_lock(&mas);
	mas_for_each(&mas, entry, UINT_MAX)
		kfree(entry);
	__mt_destroy(mt);
	mas_unlock(&mas);

	kfree(mt);
	map->cache = NULL;

	return 0;
}

static int regcache_maple_insert_block(struct regmap *map, int first,
					int last)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, first, last);
	unsigned long *entry;
	int i, ret;

	entry = kmalloc_array(last - first + 1, sizeof(*entry), map->alloc_flags);
	if (!entry)
		return -ENOMEM;

	for (i = 0; i < last - first + 1; i++)
		entry[i] = map->reg_defaults[first + i].def;

	mas_lock(&mas);

	mas_set_range(&mas, map->reg_defaults[first].reg,
		      map->reg_defaults[last].reg);
	ret = mas_store_gfp(&mas, entry, map->alloc_flags);

	mas_unlock(&mas);

	if (ret)
		kfree(entry);

	return ret;
}

static int regcache_maple_populate(struct regmap *map)
{
	int i;
	int ret;
	int range_start;

	range_start = 0;

	/* Scan for ranges of contiguous registers */
	for (i = 1; i < map->num_reg_defaults; i++) {
		if (map->reg_defaults[i].reg !=
		    map->reg_defaults[i - 1].reg + 1) {
			ret = regcache_maple_insert_block(map, range_start,
							  i - 1);
			if (ret != 0)
				return ret;

			range_start = i;
		}
	}

	/* Add the last block */
	return regcache_maple_insert_block(map, range_start, map->num_reg_defaults - 1);
}

struct regcache_ops regcache_maple_ops = {
	.type = REGCACHE_MAPLE,
	.name = "maple",
	.init = regcache_maple_init,
	.exit = regcache_maple_exit,
	.populate = regcache_maple_populate,
	.read = regcache_maple_read,
	.write = regcache_maple_write,
	.drop = regcache_maple_drop,
	.sync = regcache_maple_sync,
};
