// SPDX-License-Identifier: GPL-2.0
//
// Register cache 访问 API
//
// Copyright 2011 Wolfson Microelectronics plc
//
// Author: Dimitris Papastamos <dp@opensource.wolfsonmicro.com>

#include <linux/bsearch.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include "trace.h"
#include "internal.h"

static const struct regcache_ops *cache_types[] = {
	&regcache_flat_sparse_ops,
	&regcache_rbtree_ops,
	&regcache_maple_ops,
	&regcache_flat_ops,
};

/* 供排序/二分查找使用的默认值比较函数，按寄存器地址升序排列。 */
static int regcache_defaults_cmp(const void *a, const void *b)
{
	const struct reg_default *x = a;
	const struct reg_default *y = b;

	if (x->reg > y->reg)
		return 1;
	else if (x->reg < y->reg)
		return -1;
	else
		return 0;
}

void regcache_sort_defaults(struct reg_default *defaults, unsigned int ndefaults)
{
	sort(defaults, ndefaults, sizeof(*defaults),
	     regcache_defaults_cmp, NULL);
}
EXPORT_SYMBOL_GPL(regcache_sort_defaults);

static int regcache_count_cacheable_registers(struct regmap *map)
{
	unsigned int count;

	/* 统计真正可放入缓存的寄存器数。
	 * 只有“可读且非 volatile”的寄存器才有缓存意义。
	 */
	count = 0;
	for (unsigned int i = 0; i < map->num_reg_defaults_raw; i++)
		if (regmap_readable(map, i * map->reg_stride) &&
		    !regmap_volatile(map, i * map->reg_stride))
			count++;

	return count;
}

static int regcache_hw_init(struct regmap *map)
{
	int ret;
	unsigned int reg, val;
	void *tmp_buf;

	if (!map->reg_defaults_raw) {
		bool cache_bypass = map->cache_bypass;
		dev_dbg(map->dev, "No cache defaults, reading back from HW\n");

		/* 在构造默认值阶段临时绕过缓存，直接从硬件回读整块默认镜像。 */
		map->cache_bypass = true;
		tmp_buf = kmalloc(map->cache_size_raw, GFP_KERNEL);
		if (!tmp_buf)
			return -ENOMEM;
		ret = regmap_raw_read(map, 0, tmp_buf,
				      map->cache_size_raw);
		map->cache_bypass = cache_bypass;
		if (ret == 0) {
			map->reg_defaults_raw = tmp_buf;
			map->cache_free = true;
		} else {
			kfree(tmp_buf);
		}
	}

	/* 把“原始默认值镜像”转换成 reg_default 数组形式，供 cache 后端使用。 */
	for (unsigned int i = 0, j = 0; i < map->num_reg_defaults_raw; i++) {
		reg = i * map->reg_stride;

		if (!regmap_readable(map, reg))
			continue;

		if (regmap_volatile(map, reg))
			continue;

		if (map->reg_defaults_raw) {
			val = regcache_get_val(map, map->reg_defaults_raw, i);
		} else {
			bool cache_bypass = map->cache_bypass;

			map->cache_bypass = true;
			ret = regmap_read(map, reg, &val);
			map->cache_bypass = cache_bypass;
			if (ret != 0) {
				dev_err(map->dev, "Failed to read %x: %d\n",
					reg, ret);
				return ret;
			}
		}

		map->reg_defaults[j].reg = reg;
		map->reg_defaults[j].def = val;
		j++;
	}

	return 0;
}

static void regcache_hw_exit(struct regmap *map)
{
	if (map->cache_free)
		kfree(map->reg_defaults_raw);
}

int regcache_init(struct regmap *map, const struct regmap_config *config)
{
	int count = 0;
	int ret;
	int i;
	void *tmp_buf;

	if (map->cache_type == REGCACHE_NONE) {
		/* 用户明确不要缓存时，任何默认值描述都不会真正生效。 */
		if (config->reg_defaults || config->num_reg_defaults_raw)
			dev_warn(map->dev,
				 "No cache used with register defaults set!\n");

		map->cache_bypass = true;
		return 0;
	}

	if (config->reg_defaults && !config->num_reg_defaults) {
		dev_err(map->dev,
			 "Register defaults are set without the number!\n");
		return -EINVAL;
	}

	if (config->num_reg_defaults && !config->reg_defaults) {
		dev_err(map->dev,
			"Register defaults number are set without the reg!\n");
		return -EINVAL;
	}

	for (i = 0; i < config->num_reg_defaults; i++)
		if (config->reg_defaults[i].reg % map->reg_stride)
			return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(cache_types); i++)
		if (cache_types[i]->type == map->cache_type)
			break;

	if (i == ARRAY_SIZE(cache_types)) {
		dev_err(map->dev, "Could not match cache type: %d\n",
			map->cache_type);
		return -EINVAL;
	}

	map->num_reg_defaults = config->num_reg_defaults;
	map->num_reg_defaults_raw = config->num_reg_defaults_raw;
	map->reg_defaults_raw = config->reg_defaults_raw;
	map->cache_word_size = BITS_TO_BYTES(config->val_bits);
	map->cache_size_raw = map->cache_word_size * config->num_reg_defaults_raw;

	map->cache = NULL;
	map->cache_ops = cache_types[i];

	if (!map->cache_ops->read ||
	    !map->cache_ops->write ||
	    !map->cache_ops->name)
		return -EINVAL;

	/* reg_defaults 可能来自调用者的静态/临时内存，这里复制一份，
	 * 确保 regmap 生命周期内都可安全访问。
	 */
	if (config->reg_defaults) {
		tmp_buf = kmemdup_array(config->reg_defaults, map->num_reg_defaults,
					sizeof(*map->reg_defaults), GFP_KERNEL);
		if (!tmp_buf)
			return -ENOMEM;
		map->reg_defaults = tmp_buf;
	} else if (map->num_reg_defaults_raw) {
		count = regcache_count_cacheable_registers(map);
		if (!count)
			map->cache_bypass = true;

		/* 如果所有寄存器都不可缓存，就退化成纯直通访问模式。 */
		if (map->cache_bypass)
			return 0;

		map->num_reg_defaults = count;
		map->reg_defaults = kmalloc_objs(struct reg_default, count);
		if (!map->reg_defaults)
			return -ENOMEM;
	}

	if (!map->max_register_is_set && map->num_reg_defaults_raw) {
		map->max_register = (map->num_reg_defaults_raw  - 1) * map->reg_stride;
		map->max_register_is_set = true;
	}

	if (map->cache_ops->init) {
		dev_dbg(map->dev, "Initializing %s cache\n",
			map->cache_ops->name);
		map->lock(map->lock_arg);
		ret = map->cache_ops->init(map);
		map->unlock(map->lock_arg);
		if (ret)
			goto err_free_reg_defaults;
	}

	/*
	 * 有些设备，例如部分 PMIC，不提供寄存器默认值表。
	 * 对这类设备，只能在初始化阶段直接回读硬件，
	 * 再由 regmap core 手工拼出 cache 默认值。
	 */
	if (count) {
		ret = regcache_hw_init(map);
		if (ret)
			goto err_exit;
	}

	if (map->cache_ops->populate &&
	    (map->num_reg_defaults || map->reg_default_cb)) {
		dev_dbg(map->dev, "Populating %s cache\n", map->cache_ops->name);
		map->lock(map->lock_arg);
		ret = map->cache_ops->populate(map);
		map->unlock(map->lock_arg);
		if (ret)
			goto err_free;
	}
	return 0;

err_free:
	regcache_hw_exit(map);
err_exit:
	if (map->cache_ops->exit) {
		dev_dbg(map->dev, "Destroying %s cache\n", map->cache_ops->name);
		map->lock(map->lock_arg);
		ret = map->cache_ops->exit(map);
		map->unlock(map->lock_arg);
	}
err_free_reg_defaults:
	kfree(map->reg_defaults);

	return ret;
}

void regcache_exit(struct regmap *map)
{
	if (map->cache_type == REGCACHE_NONE)
		return;

	BUG_ON(!map->cache_ops);

	regcache_hw_exit(map);

	if (map->cache_ops->exit) {
		dev_dbg(map->dev, "Destroying %s cache\n",
			map->cache_ops->name);
		map->lock(map->lock_arg);
		map->cache_ops->exit(map);
		map->unlock(map->lock_arg);
	}

	kfree(map->reg_defaults);
}

/**
 * regcache_read - Fetch the value of a given register from the cache.
 *
 * @map: map to configure.
 * @reg: The register index.
 * @value: The value to be returned.
 *
 * Return a negative value on failure, 0 on success.
 */
int regcache_read(struct regmap *map,
		  unsigned int reg, unsigned int *value)
{
	int ret;

	if (map->cache_type == REGCACHE_NONE)
		return -EINVAL;

	BUG_ON(!map->cache_ops);

	if (!regmap_volatile(map, reg)) {
		ret = map->cache_ops->read(map, reg, value);

		if (ret == 0)
			trace_regmap_reg_read_cache(map, reg, *value);

		return ret;
	}

	return -EINVAL;
}

/**
 * regcache_write - 在缓存中写入一个寄存器值
 *
 * @map: map to configure.
 * @reg: The register index.
 * @value: The new register value.
 *
 * 返回值：成功返回 0，失败返回负 errno。
 */
int regcache_write(struct regmap *map,
		   unsigned int reg, unsigned int value)
{
	if (map->cache_type == REGCACHE_NONE)
		return 0;

	BUG_ON(!map->cache_ops);

	if (!regmap_volatile(map, reg))
		return map->cache_ops->write(map, reg, value);

	return 0;
}

bool regcache_reg_needs_sync(struct regmap *map, unsigned int reg,
			     unsigned int val)
{
	int ret;

	if (!regmap_writeable(map, reg))
		return false;

	/* 只要我们无法确认硬件仍保持默认态，就宁可把缓存值全部回写一遍。 */
	if (!map->no_sync_defaults)
		return true;

	/* 已知硬件仍在默认态时，若缓存值恰好等于默认值，就没必要再写一次。 */
	ret = regcache_lookup_reg(map, reg);
	if (ret >= 0 && val == map->reg_defaults[ret].def)
		return false;
	return true;
}

static int regcache_default_sync(struct regmap *map, unsigned int min,
				 unsigned int max)
{
	unsigned int reg;

	/* 通用同步路径：逐寄存器读取缓存，再按需写回硬件。 */
	for (reg = min; reg <= max; reg += map->reg_stride) {
		unsigned int val;
		int ret;

		if (regmap_volatile(map, reg) ||
		    !regmap_writeable(map, reg))
			continue;

		ret = regcache_read(map, reg, &val);
		if (ret == -ENOENT)
			continue;
		if (ret)
			return ret;

		if (!regcache_reg_needs_sync(map, reg, val))
			continue;

		map->cache_bypass = true;
		ret = _regmap_write(map, reg, val);
		map->cache_bypass = false;
		if (ret) {
			dev_err(map->dev, "Unable to sync register %#x. %d\n",
				reg, ret);
			return ret;
		}
		dev_dbg(map->dev, "Synced register %#x, value %#x\n", reg, val);
	}

	return 0;
}

static int rbtree_all(const void *key, const struct rb_node *node)
{
	return 0;
}

/**
 * regcache_sync - Sync the register cache with the hardware.
 *
 * @map: map to configure.
 *
 * Any registers that should not be synced should be marked as
 * volatile.  In general drivers can choose not to use the provided
 * syncing functionality if they so require.
 *
 * Return a negative value on failure, 0 on success.
 */
int regcache_sync(struct regmap *map)
{
	int ret = 0;
	unsigned int i;
	const char *name;
	bool bypass;
	struct rb_node *node;

	if (WARN_ON(map->cache_type == REGCACHE_NONE))
		return -EINVAL;

	BUG_ON(!map->cache_ops);

	map->lock(map->lock_arg);
	/* 记住调用前的 bypass 状态，函数退出前必须恢复。 */
	bypass = map->cache_bypass;
	dev_dbg(map->dev, "Syncing %s cache\n",
		map->cache_ops->name);
	name = map->cache_ops->name;
	trace_regcache_sync(map, name, "start");

	if (!map->cache_dirty)
		goto out;

	/* patch 始终优先于普通缓存内容，先写它，确保后续同步基于补丁后的硬件状态。 */
	map->cache_bypass = true;
	for (i = 0; i < map->patch_regs; i++) {
		ret = _regmap_write(map, map->patch[i].reg, map->patch[i].def);
		if (ret != 0) {
			dev_err(map->dev, "Failed to write %x = %x: %d\n",
				map->patch[i].reg, map->patch[i].def, ret);
			goto out;
		}
	}
	map->cache_bypass = false;

	if (map->cache_ops->sync)
		ret = map->cache_ops->sync(map, 0, map->max_register);
	else
		ret = regcache_default_sync(map, 0, map->max_register);

	if (ret == 0)
		map->cache_dirty = false;

out:
	/* 恢复调用前的 bypass 状态。 */
	map->cache_bypass = bypass;
	map->no_sync_defaults = false;

	/*
	 * 如果前面在 cache_bypass 模式下做过分页切换，而 selector/page 寄存器
	 * 本身又是缓存寄存器，那么“硬件当前页”和“缓存记住的页”可能已经偏离。
	 * 这里强制把所有 paging selector 再按缓存值写回一遍，重新对齐状态。
	 */
	rb_for_each(node, NULL, &map->range_tree, rbtree_all) {
		struct regmap_range_node *this =
			rb_entry(node, struct regmap_range_node, node);

		/* selector 没有缓存值就跳过，不额外制造硬件写。 */
		if (regcache_read(map, this->selector_reg, &i) != 0)
			continue;

		ret = _regmap_write(map, this->selector_reg, i);
		if (ret != 0) {
			dev_err(map->dev, "Failed to write %x = %x: %d\n",
				this->selector_reg, i, ret);
			break;
		}
	}

	map->unlock(map->lock_arg);

	regmap_async_complete(map);

	trace_regcache_sync(map, name, "stop");

	return ret;
}
EXPORT_SYMBOL_GPL(regcache_sync);

/**
 * regcache_sync_region - 仅同步缓存中的一段寄存器区间
 *
 * @map: map to sync.
 * @min: first register to sync
 * @max: last register to sync
 *
 * 将指定区间内的非默认缓存值回写到硬件。
 *
 * Return a negative value on failure, 0 on success.
 */
int regcache_sync_region(struct regmap *map, unsigned int min,
			 unsigned int max)
{
	int ret = 0;
	const char *name;
	bool bypass;

	if (WARN_ON(map->cache_type == REGCACHE_NONE))
		return -EINVAL;

	BUG_ON(!map->cache_ops);

	map->lock(map->lock_arg);

	/* 记住调用前的 bypass 状态。 */
	bypass = map->cache_bypass;

	name = map->cache_ops->name;
	dev_dbg(map->dev, "Syncing %s cache from %#x-%#x\n", name, min, max);

	trace_regcache_sync(map, name, "start region");

	if (!map->cache_dirty)
		goto out;

	map->async = true;

	if (map->cache_ops->sync)
		ret = map->cache_ops->sync(map, min, max);
	else
		ret = regcache_default_sync(map, min, max);

out:
	/* 恢复调用前的 bypass 状态。 */
	map->cache_bypass = bypass;
	map->async = false;
	map->no_sync_defaults = false;
	map->unlock(map->lock_arg);

	regmap_async_complete(map);

	trace_regcache_sync(map, name, "stop region");

	return ret;
}
EXPORT_SYMBOL_GPL(regcache_sync_region);

/**
 * regcache_drop_region - 丢弃缓存中的一段寄存器区间
 *
 * @map: map to operate on
 * @min: first register to discard
 * @max: last register to discard
 *
 * Discard part of the register cache.
 *
 * Return a negative value on failure, 0 on success.
 */
int regcache_drop_region(struct regmap *map, unsigned int min,
			 unsigned int max)
{
	int ret = 0;

	if (!map->cache_ops || !map->cache_ops->drop)
		return -EINVAL;

	map->lock(map->lock_arg);

	trace_regcache_drop_region(map, min, max);

	ret = map->cache_ops->drop(map, min, max);

	map->unlock(map->lock_arg);

	return ret;
}
EXPORT_SYMBOL_GPL(regcache_drop_region);

/**
 * regcache_cache_only - 把 regmap 切换到“只改缓存”模式
 *
 * @map: 要配置的 regmap
 * @enable: 为 true 时启用 cache-only 模式
 *
 * 进入 cache-only 后，所有通过 regmap API 发起的写操作都只更新缓存，
 * 不会真正触碰硬件。
 * 这适合设备因省电而暂时断电/不可访问时，驱动仍希望照常修改“逻辑寄存器状态”
 * 的场景。
 */
void regcache_cache_only(struct regmap *map, bool enable)
{
	map->lock(map->lock_arg);
	WARN_ON(map->cache_type != REGCACHE_NONE &&
		map->cache_bypass && enable);
	map->cache_only = enable;
	trace_regmap_cache_only(map, enable);
	map->unlock(map->lock_arg);
}
EXPORT_SYMBOL_GPL(regcache_cache_only);

/**
 * regcache_mark_dirty - 告知 regcache：硬件寄存器已回到默认态
 *
 * @map: 要标记的 regmap
 *
 * 用于通知 regcache：设备刚刚掉电、复位，或硬件寄存器已经丢失到默认值。
 * 这样在后续 regcache_sync() 时，框架就知道要把缓存里的所有非默认值重新写回。
 *
 * 如果不调用这个接口，regcache_sync() 会默认认为硬件状态仍与缓存一致，
 * 只考虑 cache_only 期间积累下来的那些写入差异。
 */
void regcache_mark_dirty(struct regmap *map)
{
	map->lock(map->lock_arg);
	map->cache_dirty = true;
	map->no_sync_defaults = true;
	map->unlock(map->lock_arg);
}
EXPORT_SYMBOL_GPL(regcache_mark_dirty);

/**
 * regcache_cache_bypass - 把 regmap 切换到“绕过缓存”模式
 *
 * @map: 要配置的 regmap
 * @enable: 为 true 时启用 bypass 模式
 *
 * 进入 cache-bypass 后，regmap API 的写操作只更新硬件，不会同步写入缓存。
 * 这通常用于 cache sync、特殊恢复路径或需要避免缓存自污染的场景。
 */
void regcache_cache_bypass(struct regmap *map, bool enable)
{
	map->lock(map->lock_arg);
	WARN_ON(map->cache_only && enable);
	map->cache_bypass = enable;
	trace_regmap_cache_bypass(map, enable);
	map->unlock(map->lock_arg);
}
EXPORT_SYMBOL_GPL(regcache_cache_bypass);

/**
 * regcache_reg_cached - 检查某个寄存器当前是否存在缓存项
 *
 * @map: map to check
 * @reg: register to check
 *
 * 返回该寄存器是否能从当前缓存后端中直接取到值。
 */
bool regcache_reg_cached(struct regmap *map, unsigned int reg)
{
	unsigned int val;
	int ret;

	map->lock(map->lock_arg);

	ret = regcache_read(map, reg, &val);

	map->unlock(map->lock_arg);

	return ret == 0;
}
EXPORT_SYMBOL_GPL(regcache_reg_cached);

void regcache_set_val(struct regmap *map, void *base, unsigned int idx,
		      unsigned int val)
{
	/* 能用设备原生格式编码时，优先走设备原生格式。 */
	if (map->format.format_val) {
		map->format.format_val(base + (map->cache_word_size * idx),
				       val, 0);
		return;
	}

	switch (map->cache_word_size) {
	case 1: {
		u8 *cache = base;

		cache[idx] = val;
		break;
	}
	case 2: {
		u16 *cache = base;

		cache[idx] = val;
		break;
	}
	case 4: {
		u32 *cache = base;

		cache[idx] = val;
		break;
	}
	default:
		BUG();
	}
}

unsigned int regcache_get_val(struct regmap *map, const void *base,
			      unsigned int idx)
{
	if (!base)
		return -EINVAL;

	/* 能按设备原生格式解析时，优先直接解析。 */
	if (map->format.parse_val)
		return map->format.parse_val(regcache_get_val_addr(map, base,
								   idx));

	switch (map->cache_word_size) {
	case 1: {
		const u8 *cache = base;

		return cache[idx];
	}
	case 2: {
		const u16 *cache = base;

		return cache[idx];
	}
	case 4: {
		const u32 *cache = base;

		return cache[idx];
	}
	default:
		BUG();
	}
	/* 理论不可达，保留给编译器和静态分析器。 */
	return -1;
}

static int regcache_default_cmp(const void *a, const void *b)
{
	const struct reg_default *_a = a;
	const struct reg_default *_b = b;

	return _a->reg - _b->reg;
}

int regcache_lookup_reg(struct regmap *map, unsigned int reg)
{
	struct reg_default key;
	struct reg_default *r;

	key.reg = reg;
	key.def = 0;

	r = bsearch(&key, map->reg_defaults, map->num_reg_defaults,
		    sizeof(struct reg_default), regcache_default_cmp);

	if (r)
		return r - map->reg_defaults;
	else
		return -ENOENT;
}

static bool regcache_reg_present(unsigned long *cache_present, unsigned int idx)
{
	if (!cache_present)
		return true;

	return test_bit(idx, cache_present);
}

int regcache_sync_val(struct regmap *map, unsigned int reg, unsigned int val)
{
	int ret;

	if (!regcache_reg_needs_sync(map, reg, val))
		return 0;

	map->cache_bypass = true;

	ret = _regmap_write(map, reg, val);

	map->cache_bypass = false;

	if (ret != 0) {
		dev_err(map->dev, "Unable to sync register %#x. %d\n",
			reg, ret);
		return ret;
	}
	dev_dbg(map->dev, "Synced register %#x, value %#x\n",
		reg, val);

	return 0;
}

static int regcache_sync_block_single(struct regmap *map, void *block,
				      unsigned long *cache_present,
				      unsigned int block_base,
				      unsigned int start, unsigned int end)
{
	unsigned int i, regtmp, val;
	int ret;

	for (i = start; i < end; i++) {
		regtmp = block_base + (i * map->reg_stride);

		if (!regcache_reg_present(cache_present, i) ||
		    !regmap_writeable(map, regtmp))
			continue;

		val = regcache_get_val(map, block, i);
		ret = regcache_sync_val(map, regtmp, val);
		if (ret != 0)
			return ret;
	}

	return 0;
}

static int regcache_sync_block_raw_flush(struct regmap *map, const void **data,
					 unsigned int base, unsigned int cur)
{
	size_t val_bytes = map->format.val_bytes;
	int ret, count;

	if (*data == NULL)
		return 0;

	count = (cur - base) / map->reg_stride;

	dev_dbg(map->dev, "Writing %zu bytes for %d registers from 0x%x-0x%x\n",
		count * val_bytes, count, base, cur - map->reg_stride);

	map->cache_bypass = true;

	ret = _regmap_raw_write(map, base, *data, count * val_bytes, false);
	if (ret)
		dev_err(map->dev, "Unable to sync registers %#x-%#x. %d\n",
			base, cur - map->reg_stride, ret);

	map->cache_bypass = false;

	*data = NULL;

	return ret;
}

static int regcache_sync_block_raw(struct regmap *map, void *block,
			    unsigned long *cache_present,
			    unsigned int block_base, unsigned int start,
			    unsigned int end)
{
	unsigned int regtmp = 0;
	unsigned int base = 0;
	const void *data = NULL;
	unsigned int val;
	int ret;

	for (unsigned int i = start; i < end; i++) {
		regtmp = block_base + (i * map->reg_stride);

		if (!regcache_reg_present(cache_present, i) ||
		    !regmap_writeable(map, regtmp)) {
			ret = regcache_sync_block_raw_flush(map, &data,
							    base, regtmp);
			if (ret != 0)
				return ret;
			continue;
		}

		val = regcache_get_val(map, block, i);
		if (!regcache_reg_needs_sync(map, regtmp, val)) {
			ret = regcache_sync_block_raw_flush(map, &data,
							    base, regtmp);
			if (ret != 0)
				return ret;
			continue;
		}

		if (!data) {
			data = regcache_get_val_addr(map, block, i);
			base = regtmp;
		}
	}

	return regcache_sync_block_raw_flush(map, &data, base, regtmp +
			map->reg_stride);
}

int regcache_sync_block(struct regmap *map, void *block,
			unsigned long *cache_present,
			unsigned int block_base, unsigned int start,
			unsigned int end)
{
	if (regmap_can_raw_write(map) && !map->use_single_write)
		return regcache_sync_block_raw(map, block, cache_present,
					       block_base, start, end);
	else
		return regcache_sync_block_single(map, block, cache_present,
						  block_base, start, end);
}
