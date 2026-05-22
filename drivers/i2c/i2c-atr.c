// SPDX-License-Identifier: GPL-2.0
/*
 * I2C 地址翻译器（Address Translator）
 *
 * Copyright (c) 2019,2022 Luca Ceresoli <luca@lucaceresoli.net>
 * Copyright (c) 2022,2023 Tomi Valkeinen <tomi.valkeinen@ideasonboard.com>
 *
 * 最初基于 i2c-mux.c
 */

#include <linux/i2c-atr.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/lockdep.h>

#define ATR_MAX_ADAPTERS 100	/* 仅作为一个合理上限。 */
#define ATR_MAX_SYMLINK_LEN 11	/* 最长名字是 10 个字符："channel-99" */

/**
 * struct i2c_atr_alias_pair - 保存分配给某个 client 地址的别名
 * @node:   链表节点
 * @addr:   子总线上 client 的真实地址
 * @alias:  驱动分配的 I2C 别名地址
 *          该地址会在父总线（物理总线）上用于发起 I2C 事务。
 * @fixed:  在动态地址附着期间，这个别名对不能被替换。
 *          当一次 I2C 事务中包含的不同目标地址数超过 ATR 通道
 *          可容纳的数量时，就需要这个标志。
 *          它用来标记已经绑定到别名上的地址，避免同一事务后续的
 *          新地址把它挤掉。
 */
struct i2c_atr_alias_pair {
	struct list_head node;
	bool fixed;
	u16 addr;
	u16 alias;
};

/**
 * struct i2c_atr_alias_pool - ATR 可用的 client 别名池
 * @size:     别名总数
 * @shared:   该别名池是否被多个通道共享
 *
 * @lock:     保护 @aliases 和 @use_mask 的锁
 * @use_mask: 已使用别名的位图
 * @aliases:  别名数组，必须刚好容纳 @size 个元素
 */
struct i2c_atr_alias_pool {
	size_t size;
	bool shared;

	/* 保护 aliases 和 use_mask。 */
	spinlock_t lock;
	unsigned long *use_mask;
	u16 aliases[] __counted_by(size);
};

/**
 * struct i2c_atr_chan - 某个通道的数据
 * @adap:            该通道对应的 &struct i2c_adapter
 * @atr:             父 I2C ATR
 * @chan_id:         该通道的 ID
 * @alias_pairs_lock: 保护 @alias_pairs 的互斥锁
 * @alias_pairs_lock_key: @alias_pairs_lock 的 lock key
 * @alias_pairs:     保存已分配别名的 @struct i2c_atr_alias_pair 列表
 * @alias_pool:      可用 client 别名池
 *
 * @orig_addrs_lock: 保护 @orig_addrs 的互斥锁
 * @orig_addrs_lock_key: @orig_addrs_lock 的 lock key
 * @orig_addrs:      传输时保存原始地址的缓冲区
 * @orig_addrs_size: @orig_addrs 的大小
 */
struct i2c_atr_chan {
	struct i2c_adapter adap;
	struct i2c_atr *atr;
	u32 chan_id;

	/* attach/detach 期间保护 alias_pairs。 */
	struct mutex alias_pairs_lock;
	struct lock_class_key alias_pairs_lock_key;
	struct list_head alias_pairs;
	struct i2c_atr_alias_pool *alias_pool;

	/* 传输期间保护 orig_addrs。 */
	struct mutex orig_addrs_lock;
	struct lock_class_key orig_addrs_lock_key;
	u16 *orig_addrs;
	unsigned int orig_addrs_size;
};

/**
 * struct i2c_atr - 一个 I2C ATR 实例
 * @parent:    父 &struct i2c_adapter
 * @dev:       持有该 I2C ATR 实例的设备
 * @ops:       &struct i2c_atr_ops
 * @priv:      私有驱动数据，通过 i2c_atr_set_driver_data() 设置
 * @algo:      适配器使用的 &struct i2c_algorithm
 * @lock:      I2C 总线段的锁（见 &struct i2c_lock_operations）
 * @lock_key:  @lock 的 lock key
 * @max_adapters: 该 I2C ATR 能拥有的最大适配器数量
 * @flags:     ATR 标志位
 * @alias_pool: 可选的公共 client 别名池
 * @i2c_nb:    远端 client 添加/删除事件的 notifier
 * @adapter:   适配器数组
 */
struct i2c_atr {
	struct i2c_adapter *parent;
	struct device *dev;
	const struct i2c_atr_ops *ops;

	void *priv;

	struct i2c_algorithm algo;
	/* I2C 总线段的锁（见 struct i2c_lock_operations）。 */
	struct mutex lock;
	struct lock_class_key lock_key;
	int max_adapters;
	u32 flags;

	struct i2c_atr_alias_pool *alias_pool;

	struct notifier_block i2c_nb;

	struct i2c_adapter *adapter[] __counted_by(max_adapters);
};

static struct i2c_atr_alias_pool *i2c_atr_alloc_alias_pool(size_t num_aliases, bool shared)
{
	struct i2c_atr_alias_pool *alias_pool;
	int ret;

	alias_pool = kzalloc_flex(*alias_pool, aliases, num_aliases);
	if (!alias_pool)
		return ERR_PTR(-ENOMEM);

	alias_pool->size = num_aliases;

	alias_pool->use_mask = bitmap_zalloc(num_aliases, GFP_KERNEL);
	if (!alias_pool->use_mask) {
		ret = -ENOMEM;
		goto err_free_alias_pool;
	}

	alias_pool->shared = shared;

	spin_lock_init(&alias_pool->lock);

	return alias_pool;

err_free_alias_pool:
	kfree(alias_pool);
	return ERR_PTR(ret);
}

/*
 * 申请一个别名池对象。
 *
 * ATR 可以使用一个全局共享别名池，也可以让每个通道维护独立池。
 * 这里不仅分配保存别名值的弹性数组，还会同时准备位图，用来跟踪
 * 哪些别名已经被映射占用。
 */
static void i2c_atr_free_alias_pool(struct i2c_atr_alias_pool *alias_pool)
{
	bitmap_free(alias_pool->use_mask);
	kfree(alias_pool);
}

/* 必须在持有 alias_pairs_lock 时调用。 */
static struct i2c_atr_alias_pair *i2c_atr_create_c2a(struct i2c_atr_chan *chan,
						     u16 alias, u16 addr)
{
	struct i2c_atr_alias_pair *c2a;

	lockdep_assert_held(&chan->alias_pairs_lock);

	c2a = kzalloc_obj(*c2a);
	if (!c2a)
		return NULL;

	c2a->addr = addr;
	c2a->alias = alias;

	list_add(&c2a->node, &chan->alias_pairs);

	return c2a;
}

/* 必须在持有 alias_pairs_lock 时调用。 */
static void i2c_atr_destroy_c2a(struct i2c_atr_alias_pair **pc2a)
{
	list_del(&(*pc2a)->node);
	kfree(*pc2a);
	*pc2a = NULL;
}

static int i2c_atr_reserve_alias(struct i2c_atr_alias_pool *alias_pool)
{
	unsigned long idx;
	u16 alias;

	/* 在池中挑选第一个空闲别名并保留。 */
	spin_lock(&alias_pool->lock);

	idx = find_first_zero_bit(alias_pool->use_mask, alias_pool->size);
	if (idx >= alias_pool->size) {
		spin_unlock(&alias_pool->lock);
		return -EBUSY;
	}

	set_bit(idx, alias_pool->use_mask);

	alias = alias_pool->aliases[idx];

	spin_unlock(&alias_pool->lock);
	return alias;
}

/*
 * 释放先前保留的别名。
 *
 * 别名池按位图管理使用状态，因此这里只需要根据别名值反查到数组槽位，
 * 再清掉对应位即可。别名值本身保持不变，后续分配可以直接复用。
 */
static void i2c_atr_release_alias(struct i2c_atr_alias_pool *alias_pool, u16 alias)
{
	unsigned int idx;

	/* 按别名值反查索引，再清掉使用位。 */
	spin_lock(&alias_pool->lock);

	for (idx = 0; idx < alias_pool->size; ++idx) {
		if (alias_pool->aliases[idx] == alias) {
			clear_bit(idx, alias_pool->use_mask);
			spin_unlock(&alias_pool->lock);
			return;
		}
	}

	spin_unlock(&alias_pool->lock);
}

static struct i2c_atr_alias_pair *
i2c_atr_find_mapping_by_addr(struct i2c_atr_chan *chan, u16 addr)
{
	struct i2c_atr_alias_pair *c2a;

	lockdep_assert_held(&chan->alias_pairs_lock);

	/* 在线性列表里查找“真实地址 -> 别名”的映射。 */
	list_for_each_entry(c2a, &chan->alias_pairs, node) {
		if (c2a->addr == addr)
			return c2a;
	}

	return NULL;
}

/*
 * 为一个真实 client 地址创建新的地址映射。
 *
 * 调用顺序是：
 * 1. 从别名池拿一个尚未使用的 alias；
 * 2. 在软件链表里插入 addr -> alias 映射；
 * 3. 通过底层驱动回调把这个映射真正写入硬件 ATR。
 *
 * 任一步失败都必须完整回滚，否则软件状态和硬件状态会失配。
 */
static struct i2c_atr_alias_pair *
i2c_atr_create_mapping_by_addr(struct i2c_atr_chan *chan, u16 addr)
{
	struct i2c_atr *atr = chan->atr;
	struct i2c_atr_alias_pair *c2a;
	u16 alias;
	int ret;

	lockdep_assert_held(&chan->alias_pairs_lock);

	/* 先从别名池里保留一个空闲别名，再通知硬件建立映射。 */
	ret = i2c_atr_reserve_alias(chan->alias_pool);
	if (ret < 0)
		return NULL;

	alias = ret;

	c2a = i2c_atr_create_c2a(chan, alias, addr);
	if (!c2a)
		goto err_release_alias;

	ret = atr->ops->attach_addr(atr, chan->chan_id, c2a->addr, c2a->alias);
	if (ret) {
		dev_err(atr->dev, "failed to attach 0x%02x on channel %d: err %d\n",
			addr, chan->chan_id, ret);
		goto err_del_c2a;
	}

	return c2a;

err_del_c2a:
	i2c_atr_destroy_c2a(&c2a);
err_release_alias:
	i2c_atr_release_alias(chan->alias_pool, alias);
	return NULL;
}

/*
 * 用新的真实地址替换一个旧映射。
 *
 * 这个路径只在“没有空闲 alias 可分配”时使用，相当于做一次受控驱逐。
 * 选择策略很保守：从链表尾部向前找最近使用、但当前事务未固定的映射，
 * 先 detach 旧地址，再把同一个 alias 重新 attach 到新地址。
 */
static struct i2c_atr_alias_pair *
i2c_atr_replace_mapping_by_addr(struct i2c_atr_chan *chan, u16 addr)
{
	struct i2c_atr *atr = chan->atr;
	struct i2c_atr_alias_pair *c2a;
	struct list_head *alias_pairs;
	bool found = false;
	u16 alias;
	int ret;

	lockdep_assert_held(&chan->alias_pairs_lock);

	alias_pairs = &chan->alias_pairs;

	if (unlikely(list_empty(alias_pairs)))
		return NULL;

	/* 优先回收最近使用但当前未被固定的映射项。 */
	list_for_each_entry_reverse(c2a, alias_pairs, node) {
		if (!c2a->fixed) {
			found = true;
			break;
		}
	}

	if (!found)
		return NULL;

	atr->ops->detach_addr(atr, chan->chan_id, c2a->addr);
	c2a->addr = addr;

	list_move(&c2a->node, alias_pairs);

	alias = c2a->alias;

	ret = atr->ops->attach_addr(atr, chan->chan_id, c2a->addr, c2a->alias);
	if (ret) {
		dev_err(atr->dev, "failed to attach 0x%02x on channel %d: err %d\n",
			addr, chan->chan_id, ret);
		i2c_atr_destroy_c2a(&c2a);
		i2c_atr_release_alias(chan->alias_pool, alias);
		return NULL;
	}

	return c2a;
}

/*
 * 取得某个真实地址对应的映射。
 *
 * 优先复用已有映射；如果是动态模式，再尝试新建映射；新建失败时，
 * 最后再尝试通过替换旧映射腾出 alias。静态模式则严格要求映射预先
 * 建好，不允许在传输过程中动态生成。
 */
static struct i2c_atr_alias_pair *
i2c_atr_get_mapping_by_addr(struct i2c_atr_chan *chan, u16 addr)
{
	struct i2c_atr *atr = chan->atr;
	struct i2c_atr_alias_pair *c2a;

	c2a = i2c_atr_find_mapping_by_addr(chan, addr);
	if (c2a)
		return c2a;

	/* 静态模式下不允许按需创建映射。 */
	if (atr->flags & I2C_ATR_F_STATIC)
		return NULL;

	c2a = i2c_atr_create_mapping_by_addr(chan, addr);
	if (c2a)
		return c2a;

	return i2c_atr_replace_mapping_by_addr(chan, addr);
}

/*
 * 把所有消息地址替换成对应的别名，并保存原始地址。
 *
 * 这个函数仅供 i2c_atr_master_xfer() 内部使用，之后必须调用
 * i2c_atr_unmap_msgs() 恢复原始地址。
 *
 * 它处理的是“一个事务里有多条 i2c_msg”这种情况。因为不同消息可能访问
 * 不同目标地址，所以必须先为每条消息找到或创建对应 alias，并把原始地址
 * 暂存到 orig_addrs 中，等父总线传输结束后再统一恢复。
 */
static int i2c_atr_map_msgs(struct i2c_atr_chan *chan, struct i2c_msg *msgs,
			    int num)
{
	struct i2c_atr *atr = chan->atr;
	static struct i2c_atr_alias_pair *c2a;
	int i, ret = 0;

	/* 确保有足够空间保存原始地址。 */
	if (unlikely(chan->orig_addrs_size < num)) {
		u16 *new_buf;

		/* 不关心旧数据，所以不需要 realloc()。 */
		new_buf = kmalloc_array(num, sizeof(*new_buf), GFP_KERNEL);
		if (!new_buf)
			return -ENOMEM;

		kfree(chan->orig_addrs);
		chan->orig_addrs = new_buf;
		chan->orig_addrs_size = num;
	}

	mutex_lock(&chan->alias_pairs_lock);

	for (i = 0; i < num; i++) {
		chan->orig_addrs[i] = msgs[i].addr;

		c2a = i2c_atr_get_mapping_by_addr(chan, msgs[i].addr);

		if (!c2a) {
			if (atr->flags & I2C_ATR_F_PASSTHROUGH)
				continue;

			dev_err(atr->dev, "client 0x%02x not mapped!\n",
				msgs[i].addr);

			while (i--)
				msgs[i].addr = chan->orig_addrs[i];

			ret = -ENXIO;
			goto out_unlock;
		}

		/* 防止同一事务里的其它 client 覆盖这个 c2a。 */
		c2a->fixed = true;

		msgs[i].addr = c2a->alias;
	}

out_unlock:
	mutex_unlock(&chan->alias_pairs_lock);
	return ret;
}

/*
 * 把所有消息别名恢复为原始地址。
 *
 * 这个函数仅供 i2c_atr_master_xfer() 使用，因此不需要对 orig_addr
 * 再做空指针和大小检查。
 *
 * @see i2c_atr_map_msgs()
 *
 * 除了恢复地址本身，它还会把当前事务里被 fixed 的映射全部解锁，
 * 让这些 alias 在后续事务中重新参与替换与复用。
 */
static void i2c_atr_unmap_msgs(struct i2c_atr_chan *chan, struct i2c_msg *msgs,
			       int num)
{
	struct i2c_atr_alias_pair *c2a;
	int i;

	for (i = 0; i < num; i++)
		msgs[i].addr = chan->orig_addrs[i];

	mutex_lock(&chan->alias_pairs_lock);

	if (unlikely(list_empty(&chan->alias_pairs)))
		goto out_unlock;

	/* 解除 c2a 的固定状态，以便后续传输复用这些别名。 */
	list_for_each_entry(c2a, &chan->alias_pairs, node) {
		c2a->fixed = false;
	}

out_unlock:
	mutex_unlock(&chan->alias_pairs_lock);
}

/*
 * 处理普通 I2C message 传输。
 *
 * 子总线 client 发来的地址在真正下发到父总线之前，必须先被改写成
 * ATR 能识别的 alias。传输完成后再把消息地址恢复，保证上层调用者
 * 看到的仍是原始 client 地址。
 *
 * orig_addrs_lock 的作用是串行化“地址改写 + 父总线传输 + 地址恢复”
 * 这整个窗口，避免并发事务互相覆盖 orig_addrs 缓冲区。
 */
static int i2c_atr_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			       int num)
{
	struct i2c_atr_chan *chan = adap->algo_data;
	struct i2c_atr *atr = chan->atr;
	struct i2c_adapter *parent = atr->parent;
	int ret;

	/* 翻译地址。 */
	mutex_lock(&chan->orig_addrs_lock);

	ret = i2c_atr_map_msgs(chan, msgs, num);
	if (ret < 0)
		goto err_unlock;

	/* 执行传输。 */
	ret = i2c_transfer(parent, msgs, num);

	/* 恢复地址。 */
	i2c_atr_unmap_msgs(chan, msgs, num);

err_unlock:
	mutex_unlock(&chan->orig_addrs_lock);

	return ret;
}

static int i2c_atr_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			      unsigned short flags, char read_write, u8 command,
			      int size, union i2c_smbus_data *data)
{
	struct i2c_atr_chan *chan = adap->algo_data;
	struct i2c_atr *atr = chan->atr;
	struct i2c_adapter *parent = atr->parent;
	struct i2c_atr_alias_pair *c2a;
	u16 alias;

	mutex_lock(&chan->alias_pairs_lock);

	c2a = i2c_atr_get_mapping_by_addr(chan, addr);

	/* 未映射地址只有在 passthrough 模式下才允许直接透传。 */
	if (!c2a && !(atr->flags & I2C_ATR_F_PASSTHROUGH)) {
		dev_err(atr->dev, "client 0x%02x not mapped!\n", addr);
		mutex_unlock(&chan->alias_pairs_lock);
		return -ENXIO;
	}

	alias = c2a ? c2a->alias : addr;

	mutex_unlock(&chan->alias_pairs_lock);

	return i2c_smbus_xfer(parent, alias, flags, read_write, command,
			      size, data);
}

/*
 * 处理 SMBus 风格传输。
 *
 * SMBus API 只携带一个目标地址，不像 master_xfer 那样需要批量改写
 * message 数组，因此这里只做单地址查表。若驱动启用了 passthrough，
 * 则允许未映射地址直接沿用原地址访问父总线。
 */
static u32 i2c_atr_functionality(struct i2c_adapter *adap)
{
	struct i2c_atr_chan *chan = adap->algo_data;
	struct i2c_adapter *parent = chan->atr->parent;

	return parent->algo->functionality(parent);
}

/*
 * 子适配器能力直接继承父适配器。
 *
 * ATR 只改地址，不改变底层控制器是否支持 plain I2C、SMBus block、
 * PEC 等硬件能力，所以 capability 查询可以直接透传给 parent。
 */
/*
 * ATR 子总线与父总线共享同一段序列化锁。
 *
 * ATR 的关键状态并不是“当前选中的通道”，而是“地址映射表可能在传输前
 * 被动态创建或替换”。因此这里必须把同一 ATR 下所有子总线都串行起来，
 * 否则两个并发事务可能同时改写 alias 映射，导致父总线看到错误地址。
 */
static void i2c_atr_lock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_atr_chan *chan = adapter->algo_data;
	struct i2c_atr *atr = chan->atr;

	mutex_lock(&atr->lock);
}

/* ATR 锁的 trylock 版本，语义与 i2c_atr_lock_bus() 相同。 */
static int i2c_atr_trylock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_atr_chan *chan = adapter->algo_data;
	struct i2c_atr *atr = chan->atr;

	return mutex_trylock(&atr->lock);
}

/* 释放 ATR 级串行化锁。 */
static void i2c_atr_unlock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_atr_chan *chan = adapter->algo_data;
	struct i2c_atr *atr = chan->atr;

	mutex_unlock(&atr->lock);
}

static const struct i2c_lock_operations i2c_atr_lock_ops = {
	.lock_bus =    i2c_atr_lock_bus,
	.trylock_bus = i2c_atr_trylock_bus,
	.unlock_bus =  i2c_atr_unlock_bus,
};

/*
 * 把一个远端 client 地址附着到 ATR 硬件上。
 *
 * 这个入口通常由 bus notifier 在“远端子总线出现新 client”时触发。
 * 对静态 ATR 来说，它相当于验证预留映射是否存在；对动态 ATR 来说，
 * 它会即时分配 alias，必要时还会驱逐一个旧映射来腾空间。
 */
static int i2c_atr_attach_addr(struct i2c_adapter *adapter,
			       u16 addr)
{
	struct i2c_atr_chan *chan = adapter->algo_data;
	struct i2c_atr *atr = chan->atr;
	struct i2c_atr_alias_pair *c2a;
	int ret = 0;

	mutex_lock(&chan->alias_pairs_lock);

	c2a = i2c_atr_create_mapping_by_addr(chan, addr);
	if (!c2a && !(atr->flags & I2C_ATR_F_STATIC))
		c2a = i2c_atr_replace_mapping_by_addr(chan, addr);

	if (!c2a) {
		dev_err(atr->dev, "failed to find a free alias\n");
		ret = -EBUSY;
		goto out_unlock;
	}

	dev_dbg(atr->dev, "chan%u: using alias 0x%02x for addr 0x%02x\n",
		chan->chan_id, c2a->alias, addr);

out_unlock:
	mutex_unlock(&chan->alias_pairs_lock);
	return ret;
}

/*
 * 解除一个远端 client 地址与 alias 的绑定。
 *
 * detach 顺序很重要：先通知硬件撤掉映射，再在软件链表里释放别名。
 * 这样可以避免在硬件仍然保留旧映射时，软件过早把 alias 重新分配给
 * 另一个地址。
 */
static void i2c_atr_detach_addr(struct i2c_adapter *adapter,
				u16 addr)
{
	struct i2c_atr_chan *chan = adapter->algo_data;
	struct i2c_atr *atr = chan->atr;
	struct i2c_atr_alias_pair *c2a;

	atr->ops->detach_addr(atr, chan->chan_id, addr);

	mutex_lock(&chan->alias_pairs_lock);

	c2a = i2c_atr_find_mapping_by_addr(chan, addr);
	if (!c2a) {
		mutex_unlock(&chan->alias_pairs_lock);
		return;
	}

	i2c_atr_release_alias(chan->alias_pool, c2a->alias);

	dev_dbg(atr->dev,
		"chan%u: detached alias 0x%02x from addr 0x%02x\n",
		chan->chan_id, c2a->alias, addr);

	i2c_atr_destroy_c2a(&c2a);

	mutex_unlock(&chan->alias_pairs_lock);
}

/*
 * 监听 I2C 总线设备增删事件。
 *
 * ATR 的“远端设备”本质上也是普通 i2c_client，I2C core 在这些 client
 * 创建/销毁时会广播 notifier。这里先确认事件是否来自 ATR 管辖的
 * 子适配器，再把 add/remove 分别转成 attach/detach。
 */
static int i2c_atr_bus_notifier_call(struct notifier_block *nb,
				     unsigned long event, void *device)
{
	struct i2c_atr *atr = container_of(nb, struct i2c_atr, i2c_nb);
	struct device *dev = device;
	struct i2c_client *client;
	u32 chan_id;
	int ret;

	client = i2c_verify_client(dev);
	if (!client)
		return NOTIFY_DONE;

	/* 这个 client 是否属于我们的某个适配器？ */
	for (chan_id = 0; chan_id < atr->max_adapters; ++chan_id) {
		if (client->adapter == atr->adapter[chan_id])
			break;
	}

	if (chan_id == atr->max_adapters)
		return NOTIFY_DONE;

	switch (event) {
	case BUS_NOTIFY_ADD_DEVICE:
		ret = i2c_atr_attach_addr(client->adapter, client->addr);
		if (ret)
			dev_err(atr->dev,
				"Failed to attach remote client '%s': %d\n",
				dev_name(dev), ret);
		break;

	case BUS_NOTIFY_REMOVED_DEVICE:
		i2c_atr_detach_addr(client->adapter, client->addr);
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

/*
 * 解析设备树/固件中的共享别名池配置。
 *
 * 属性 `i2c-alias-pool` 描述的是一组可供硬件 ATR 使用的从地址别名。
 * 如果属性不存在，就表示平台不预先声明共享池，后续只能依赖每通道
 * 独立的 alias 列表，或者让上层驱动自己决定是否支持这种模式。
 */
static int i2c_atr_parse_alias_pool(struct i2c_atr *atr)
{
	struct i2c_atr_alias_pool *alias_pool;
	struct device *dev = atr->dev;
	size_t num_aliases;
	unsigned int i;
	u32 *aliases32;
	int ret;

	if (!fwnode_property_present(dev_fwnode(dev), "i2c-alias-pool")) {
		num_aliases = 0;
	} else {
		/* i2c-alias-pool 直接描述可用别名集合。 */
		ret = fwnode_property_count_u32(dev_fwnode(dev), "i2c-alias-pool");
		if (ret < 0) {
			dev_err(dev, "Failed to count 'i2c-alias-pool' property: %d\n",
				ret);
			return ret;
		}

		num_aliases = ret;
	}

	alias_pool = i2c_atr_alloc_alias_pool(num_aliases, true);
	if (IS_ERR(alias_pool)) {
		ret = PTR_ERR(alias_pool);
		dev_err(dev, "Failed to allocate alias pool, err %d\n", ret);
		return ret;
	}

	atr->alias_pool = alias_pool;

	if (!alias_pool->size)
		return 0;

	/* 先按 u32 读固件属性，再压成不带 flags 的 16 位地址。 */
	aliases32 = kcalloc(num_aliases, sizeof(*aliases32), GFP_KERNEL);
	if (!aliases32) {
		ret = -ENOMEM;
		goto err_free_alias_pool;
	}

	ret = fwnode_property_read_u32_array(dev_fwnode(dev), "i2c-alias-pool",
					     aliases32, num_aliases);
	if (ret < 0) {
		dev_err(dev, "Failed to read 'i2c-alias-pool' property: %d\n",
			ret);
		goto err_free_aliases32;
	}

	for (i = 0; i < num_aliases; i++) {
		if (!(aliases32[i] & 0xffff0000)) {
			alias_pool->aliases[i] = aliases32[i];
			continue;
		}

		dev_err(dev, "Failed to parse 'i2c-alias-pool' property: I2C flags are not supported\n");
		ret = -EINVAL;
		goto err_free_aliases32;
	}

	kfree(aliases32);

	dev_dbg(dev, "i2c-alias-pool has %zu aliases\n", alias_pool->size);

	return 0;

err_free_aliases32:
	kfree(aliases32);
err_free_alias_pool:
	i2c_atr_free_alias_pool(alias_pool);
	return ret;
}

/**
 * i2c_atr_new - 创建一个 ATR 框架实例
 * @parent: 父总线适配器
 * @dev:    ATR 对应的设备对象
 * @ops:    底层硬件回调，至少要提供 attach/detach
 * @max_adapters: 最多能导出的子适配器数量
 * @flags:  ATR 工作模式标志
 *
 * 这个函数只完成框架级初始化：保存 parent/ops、准备公共锁、解析共享
 * alias pool，并向 I2C bus 注册 notifier。真正的每个 channel adapter
 * 仍然要由 i2c_atr_add_adapter() 单独创建。
 *
 * Return: 成功时返回新的 ATR 对象，失败时返回 ERR_PTR()
 */
struct i2c_atr *i2c_atr_new(struct i2c_adapter *parent, struct device *dev,
			    const struct i2c_atr_ops *ops, int max_adapters,
			    u32 flags)
{
	struct i2c_atr *atr;
	int ret;

	/* attach/detach 回调是 ATR 的最小必需能力。 */
	if (max_adapters > ATR_MAX_ADAPTERS)
		return ERR_PTR(-EINVAL);

	if (!ops || !ops->attach_addr || !ops->detach_addr)
		return ERR_PTR(-EINVAL);

	atr = kzalloc_flex(*atr, adapter, max_adapters);
	if (!atr)
		return ERR_PTR(-ENOMEM);

	lockdep_register_key(&atr->lock_key);
	mutex_init_with_key(&atr->lock, &atr->lock_key);

	atr->parent = parent;
	atr->dev = dev;
	atr->ops = ops;
	atr->max_adapters = max_adapters;
	atr->flags = flags;

	if (parent->algo->master_xfer)
		atr->algo.xfer = i2c_atr_master_xfer;
	if (parent->algo->smbus_xfer)
		atr->algo.smbus_xfer = i2c_atr_smbus_xfer;
	atr->algo.functionality = i2c_atr_functionality;

	/* 先准备别名池，再开始接收远端 client 增删通知。 */
	ret = i2c_atr_parse_alias_pool(atr);
	if (ret)
		goto err_destroy_mutex;

	atr->i2c_nb.notifier_call = i2c_atr_bus_notifier_call;
	ret = bus_register_notifier(&i2c_bus_type, &atr->i2c_nb);
	if (ret)
		goto err_free_alias_pool;

	return atr;

err_free_alias_pool:
	i2c_atr_free_alias_pool(atr->alias_pool);
err_destroy_mutex:
	mutex_destroy(&atr->lock);
	lockdep_unregister_key(&atr->lock_key);
	kfree(atr);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_NS_GPL(i2c_atr_new, "I2C_ATR");

/**
 * i2c_atr_delete - 销毁一个 ATR 框架实例
 * @atr: 要销毁的 ATR 对象
 *
 * 调用者必须先删除所有已添加的 channel adapter。这里会用 WARN_ON()
 * 检查是否还有残留子适配器，以避免 notifier、alias pool 和互斥锁在
 * 仍被使用时就被释放。
 */
void i2c_atr_delete(struct i2c_atr *atr)
{
	unsigned int i;

	for (i = 0; i < atr->max_adapters; ++i)
		WARN_ON(atr->adapter[i]);

	bus_unregister_notifier(&i2c_bus_type, &atr->i2c_nb);
	i2c_atr_free_alias_pool(atr->alias_pool);
	mutex_destroy(&atr->lock);
	lockdep_unregister_key(&atr->lock_key);
	kfree(atr);
}
EXPORT_SYMBOL_NS_GPL(i2c_atr_delete, "I2C_ATR");

/**
 * i2c_atr_add_adapter - 向 ATR 注册一个子适配器
 * @atr:  ATR 实例
 * @desc: 子适配器描述，包括通道号、固件节点、可选独立 alias 列表等
 *
 * 一个 channel 在 Linux 里会表现成一个独立的 i2c_adapter。这个函数
 * 负责搭建它的 software view：初始化通道私有锁和映射链表、决定使用
 * 共享还是私有 alias pool、继承 parent 的超时/quirks、并最终向
 * I2C core 注册 adapter。
 *
 * Return: 0 表示成功，负值表示失败原因
 */
int i2c_atr_add_adapter(struct i2c_atr *atr, struct i2c_atr_adap_desc *desc)
{
	struct fwnode_handle *bus_handle = desc->bus_handle;
	struct i2c_adapter *parent = atr->parent;
	char symlink_name[ATR_MAX_SYMLINK_LEN];
	struct device *dev = atr->dev;
	u32 chan_id = desc->chan_id;
	struct i2c_atr_chan *chan;
	int ret, idx;

	if (chan_id >= atr->max_adapters) {
		dev_err(dev, "No room for more i2c-atr adapters\n");
		return -EINVAL;
	}

	if (atr->adapter[chan_id]) {
		dev_err(dev, "Adapter %d already present\n", chan_id);
		return -EEXIST;
	}

	chan = kzalloc_obj(*chan);
	if (!chan)
		return -ENOMEM;

	if (!desc->parent)
		desc->parent = dev;

	/* 每个逻辑通道都表现成一个独立的 I2C adapter。 */
	chan->atr = atr;
	chan->chan_id = chan_id;
	INIT_LIST_HEAD(&chan->alias_pairs);
	lockdep_register_key(&chan->alias_pairs_lock_key);
	lockdep_register_key(&chan->orig_addrs_lock_key);
	mutex_init_with_key(&chan->alias_pairs_lock, &chan->alias_pairs_lock_key);
	mutex_init_with_key(&chan->orig_addrs_lock, &chan->orig_addrs_lock_key);

	snprintf(chan->adap.name, sizeof(chan->adap.name), "i2c-%d-atr-%d",
		 i2c_adapter_id(parent), chan_id);
	chan->adap.owner = THIS_MODULE;
	chan->adap.algo = &atr->algo;
	chan->adap.algo_data = chan;
	chan->adap.dev.parent = desc->parent;
	chan->adap.retries = parent->retries;
	chan->adap.timeout = parent->timeout;
	chan->adap.quirks = parent->quirks;
	chan->adap.lock_ops = &i2c_atr_lock_ops;

	if (bus_handle) {
		device_set_node(&chan->adap.dev, fwnode_handle_get(bus_handle));
	} else {
		struct fwnode_handle *atr_node;
		struct fwnode_handle *child;
		u32 reg;

		/* 没有显式 bus_handle 时，从 i2c-atr 子节点里按 reg 匹配。 */
		atr_node = device_get_named_child_node(dev, "i2c-atr");

		fwnode_for_each_child_node(atr_node, child) {
			ret = fwnode_property_read_u32(child, "reg", &reg);
			if (ret)
				continue;
			if (chan_id == reg)
				break;
		}

		device_set_node(&chan->adap.dev, child);
		fwnode_handle_put(atr_node);
	}

	if (desc->num_aliases > 0) {
		/* 通道级 alias pool 会覆盖 ATR 级共享 alias pool。 */
		chan->alias_pool = i2c_atr_alloc_alias_pool(desc->num_aliases, false);
		if (IS_ERR(chan->alias_pool)) {
			ret = PTR_ERR(chan->alias_pool);
			goto err_fwnode_put;
		}

		for (idx = 0; idx < desc->num_aliases; idx++)
			chan->alias_pool->aliases[idx] = desc->aliases[idx];
	} else {
		chan->alias_pool = atr->alias_pool;
	}

	atr->adapter[chan_id] = &chan->adap;

	ret = i2c_add_adapter(&chan->adap);
	if (ret) {
		dev_err(dev, "failed to add atr-adapter %u (error=%d)\n",
			chan_id, ret);
		goto err_free_alias_pool;
	}

	snprintf(symlink_name, sizeof(symlink_name), "channel-%u",
		 chan->chan_id);

	/* 双向 sysfs link 便于从 ATR 和子总线两个方向定位彼此。 */
	ret = sysfs_create_link(&chan->adap.dev.kobj, &dev->kobj, "atr_device");
	if (ret)
		dev_warn(dev, "can't create symlink to atr device\n");
	ret = sysfs_create_link(&dev->kobj, &chan->adap.dev.kobj, symlink_name);
	if (ret)
		dev_warn(dev, "can't create symlink for channel %u\n", chan_id);

	dev_dbg(dev, "Added ATR child bus %d\n", i2c_adapter_id(&chan->adap));

	return 0;

err_free_alias_pool:
	if (!chan->alias_pool->shared)
		i2c_atr_free_alias_pool(chan->alias_pool);
err_fwnode_put:
	fwnode_handle_put(dev_fwnode(&chan->adap.dev));
	mutex_destroy(&chan->orig_addrs_lock);
	mutex_destroy(&chan->alias_pairs_lock);
	lockdep_unregister_key(&chan->orig_addrs_lock_key);
	lockdep_unregister_key(&chan->alias_pairs_lock_key);
	kfree(chan);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(i2c_atr_add_adapter, "I2C_ATR");

/**
 * i2c_atr_del_adapter - 删除一个先前注册的子适配器
 * @atr:     ATR 实例
 * @chan_id: 要删除的逻辑通道号
 *
 * 这里负责做完整逆向清理：移除 sysfs 链接、从 I2C core 注销 adapter、
 * 释放私有 alias pool 与锁、断开 ATR 对该通道的引用。
 */
void i2c_atr_del_adapter(struct i2c_atr *atr, u32 chan_id)
{
	char symlink_name[ATR_MAX_SYMLINK_LEN];
	struct i2c_adapter *adap;
	struct i2c_atr_chan *chan;
	struct fwnode_handle *fwnode;
	struct device *dev = atr->dev;

	adap = atr->adapter[chan_id];
	if (!adap)
		return;

	chan = adap->algo_data;
	fwnode = dev_fwnode(&adap->dev);

	dev_dbg(dev, "Removing ATR child bus %d\n", i2c_adapter_id(adap));

	snprintf(symlink_name, sizeof(symlink_name), "channel-%u",
		 chan->chan_id);
	sysfs_remove_link(&dev->kobj, symlink_name);
	sysfs_remove_link(&chan->adap.dev.kobj, "atr_device");

	/* 先把 adapter 从 I2C core 注销，再回收本地资源。 */
	i2c_del_adapter(adap);

	if (!chan->alias_pool->shared)
		i2c_atr_free_alias_pool(chan->alias_pool);

	atr->adapter[chan_id] = NULL;

	fwnode_handle_put(fwnode);
	mutex_destroy(&chan->orig_addrs_lock);
	mutex_destroy(&chan->alias_pairs_lock);
	lockdep_unregister_key(&chan->orig_addrs_lock_key);
	lockdep_unregister_key(&chan->alias_pairs_lock_key);
	kfree(chan->orig_addrs);
	kfree(chan);
}
EXPORT_SYMBOL_NS_GPL(i2c_atr_del_adapter, "I2C_ATR");

/*
 * 保存底层驱动私有数据。
 *
 * ATR 核心本身只负责地址翻译框架，不理解底层硬件私有上下文，所以
 * 提供这组 accessor 让具体驱动把自己的状态挂在 ATR 对象上。
 */
void i2c_atr_set_driver_data(struct i2c_atr *atr, void *data)
{
	atr->priv = data;
}
EXPORT_SYMBOL_NS_GPL(i2c_atr_set_driver_data, "I2C_ATR");

/* 取回之前通过 i2c_atr_set_driver_data() 保存的私有数据。 */
void *i2c_atr_get_driver_data(struct i2c_atr *atr)
{
	return atr->priv;
}
EXPORT_SYMBOL_NS_GPL(i2c_atr_get_driver_data, "I2C_ATR");

MODULE_AUTHOR("Luca Ceresoli <luca.ceresoli@bootlin.com>");
MODULE_AUTHOR("Tomi Valkeinen <tomi.valkeinen@ideasonboard.com>");
MODULE_DESCRIPTION("I2C Address Translator");
MODULE_LICENSE("GPL");
