/*
 * 多路复用 I2C 总线驱动。
 *
 * 这个文件通过把每个 mux 通道都包装成一个额外的 I2C 适配器，
 * 来简化复杂的多路复用 I2C 拓扑访问。它支持多级 mux，也就是
 * mux 后面再挂 mux 的场景。
 *
 * 代码思路来自：
 *	- Kumar Gala 的 i2c-virt.c
 *	- Ken Harrenstien 的 i2c-virtual.c
 *	- Brian Kuschak 的 i2c-virtual.c
 *
 * 本文件遵循 GNU GPL v2 许可，按“原样”提供，不附带任何担保。
 */

#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

/* 每个通道对应的 mux 私有数据。 */
struct i2c_mux_priv {
	struct i2c_adapter adap;
	struct i2c_algorithm algo;
	struct i2c_mux_core *muxc;
	u32 chan_id;
};

/*
 * 无外层锁版本的 I2C 传输转发。
 *
 * 适用于 mux 核心已经拿好了父总线锁的场景。这里做三件事：
 * - 选中目标通道
 * - 在父 adapter 上调用 __i2c_transfer()
 * - 传输后视情况撤销通道选择
 */
static int __i2c_mux_master_xfer(struct i2c_adapter *adap,
				 struct i2c_msg msgs[], int num)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* 先切到正确的 mux 通道，再执行传输。 */

	ret = muxc->select(muxc, priv->chan_id);
	if (ret >= 0)
		ret = __i2c_transfer(parent, msgs, num);
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

/*
 * 常规 I2C 传输转发入口。
 *
 * 和 __i2c_mux_master_xfer() 的唯一区别，是这里让父总线走
 * i2c_transfer()，因此会由父 adapter 自己执行标准的加锁流程。
 */
static int i2c_mux_master_xfer(struct i2c_adapter *adap,
			       struct i2c_msg msgs[], int num)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* 先切到正确的 mux 通道，再执行传输。 */

	ret = muxc->select(muxc, priv->chan_id);
	if (ret >= 0)
		ret = i2c_transfer(parent, msgs, num);
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

/*
 * 无外层锁版本的 SMBus 转发入口。
 *
 * 逻辑与 I2C message 转发完全一致，只是底层改成 __i2c_smbus_xfer()。
 */
static int __i2c_mux_smbus_xfer(struct i2c_adapter *adap,
				u16 addr, unsigned short flags,
				char read_write, u8 command,
				int size, union i2c_smbus_data *data)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* 先选择正确的 mux 通道，再执行传输。 */

	ret = muxc->select(muxc, priv->chan_id);
	if (ret >= 0)
		ret = __i2c_smbus_xfer(parent, addr, flags,
				       read_write, command, size, data);
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

/*
 * 常规 SMBus 转发入口。
 *
 * 这里会在选中通道后调用父 adapter 的 i2c_smbus_xfer()，从而把
 * 锁和协议细节继续委托给父总线。
 */
static int i2c_mux_smbus_xfer(struct i2c_adapter *adap,
			      u16 addr, unsigned short flags,
			      char read_write, u8 command,
			      int size, union i2c_smbus_data *data)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_mux_core *muxc = priv->muxc;
	struct i2c_adapter *parent = muxc->parent;
	int ret;

	/* 先选择正确的 mux 通道，再执行传输。 */

	ret = muxc->select(muxc, priv->chan_id);
	if (ret >= 0)
		ret = i2c_smbus_xfer(parent, addr, flags,
				     read_write, command, size, data);
	if (muxc->deselect)
		muxc->deselect(muxc, priv->chan_id);

	return ret;
}

/* 直接返回父适配器支持的功能集。 */
static u32 i2c_mux_functionality(struct i2c_adapter *adap)
{
	struct i2c_mux_priv *priv = adap->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	return parent->algo->functionality(parent);
}

/*
 * mux-locked 模式下的锁语义。
 *
 * 这种模式要求“选通道 + 访问父总线”作为一个不可拆分的临界区，因此
 * 必须先拿 parent->mux_lock；若调用方还要求锁根 adapter，再继续
 * 向上获取父总线锁。
 */
static void i2c_mux_lock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	rt_mutex_lock_nested(&parent->mux_lock, i2c_adapter_depth(adapter));
	if (!(flags & I2C_LOCK_ROOT_ADAPTER))
		return;
	i2c_lock_bus(parent, flags);
}

/*
 * mux-locked 模式下的 trylock 版本。
 *
 * 成功的条件是：
 * - 至少拿到 mux_lock
 * - 如果要求锁根 adapter，还必须同时拿到父总线锁
 */
static int i2c_mux_trylock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	if (!rt_mutex_trylock(&parent->mux_lock))
		return 0;	/* mux_lock not locked, failure */
	if (!(flags & I2C_LOCK_ROOT_ADAPTER))
		return 1;	/* we only want mux_lock, success */
	if (i2c_trylock_bus(parent, flags))
		return 1;	/* parent locked too, success */
	rt_mutex_unlock(&parent->mux_lock);
	return 0;		/* parent not locked, failure */
}

/* 与 i2c_mux_lock_bus() 成对的解锁路径。 */
static void i2c_mux_unlock_bus(struct i2c_adapter *adapter, unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	if (flags & I2C_LOCK_ROOT_ADAPTER)
		i2c_unlock_bus(parent, flags);
	rt_mutex_unlock(&parent->mux_lock);
}

/*
 * parent-lock 模式下的加锁语义。
 *
 * 这种模式下，mux 层自己不把“通道选择”视为单独的原子窗口，而是
 * 始终把父 adapter 锁一起拿住，因此所有并发都串行到父总线一级。
 */
static void i2c_parent_lock_bus(struct i2c_adapter *adapter,
				unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	rt_mutex_lock_nested(&parent->mux_lock, i2c_adapter_depth(adapter));
	i2c_lock_bus(parent, flags);
}

/* parent-lock 模式下的 trylock 版本。 */
static int i2c_parent_trylock_bus(struct i2c_adapter *adapter,
				  unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	if (!rt_mutex_trylock(&parent->mux_lock))
		return 0;	/* mux_lock not locked, failure */
	if (i2c_trylock_bus(parent, flags))
		return 1;	/* parent locked too, success */
	rt_mutex_unlock(&parent->mux_lock);
	return 0;		/* parent not locked, failure */
}

/*
 * parent-lock 模式对应的解锁路径。
 *
 * 顺序与加锁相反：先释放父 adapter 锁，再释放 mux_lock。
 */
static void i2c_parent_unlock_bus(struct i2c_adapter *adapter,
				  unsigned int flags)
{
	struct i2c_mux_priv *priv = adapter->algo_data;
	struct i2c_adapter *parent = priv->muxc->parent;

	i2c_unlock_bus(parent, flags);
	rt_mutex_unlock(&parent->mux_lock);
}

struct i2c_adapter *i2c_root_adapter(struct device *dev)
{
	struct device *i2c;
	struct i2c_adapter *i2c_root;

	/*
	 * 向上遍历设备树，寻找一个 I2C 适配器，说明当前设备属于
	 * 某个 I2C client。这里要检查所有祖先节点，以兼容 MFD 这类
	 * 复合设备。
	 */
	for (i2c = dev; i2c; i2c = i2c->parent) {
		if (i2c->type == &i2c_adapter_type)
			break;
	}
	if (!i2c)
		return NULL;

	/* 继续向上寻找最顶层的 root I2C 适配器。 */
	i2c_root = to_i2c_adapter(i2c);
	while (i2c_parent_is_i2c_adapter(i2c_root))
		i2c_root = i2c_parent_is_i2c_adapter(i2c_root);

	return i2c_root;
}
EXPORT_SYMBOL_GPL(i2c_root_adapter);

/**
 * i2c_mux_alloc - 申请并初始化一个 I2C mux 核心对象
 * @parent:       上游父适配器
 * @dev:          mux 设备本身
 * @max_adapters: 最多允许导出的子适配器数量
 * @sizeof_priv:  需要额外挂在 muxc 尾部的私有数据大小
 * @flags:        mux 工作模式标志
 * @select:       选中某个通道的底层回调
 * @deselect:     取消选中某个通道的底层回调，可选
 *
 * 该函数只建立 mux 核心对象，不会立即向 I2C core 注册任何子总线。
 * 真正的 channel adapter 需要后续通过 i2c_mux_add_adapter() 逐个创建。
 *
 * Return: 成功返回 mux 核心对象，失败返回 NULL
 */
struct i2c_mux_core *i2c_mux_alloc(struct i2c_adapter *parent,
				   struct device *dev, int max_adapters,
				   int sizeof_priv, u32 flags,
				   int (*select)(struct i2c_mux_core *, u32),
				   int (*deselect)(struct i2c_mux_core *, u32))
{
	struct i2c_mux_core *muxc;
	size_t mux_size;

	mux_size = struct_size(muxc, adapter, max_adapters);
	muxc = devm_kzalloc(dev, size_add(mux_size, sizeof_priv), GFP_KERNEL);
	if (!muxc)
		return NULL;
	if (sizeof_priv)
		muxc->priv = &muxc->adapter[max_adapters];

	muxc->parent = parent;
	muxc->dev = dev;
	muxc->mux_locked = !!(flags & I2C_MUX_LOCKED);
	muxc->arbitrator = !!(flags & I2C_MUX_ARBITRATOR);
	muxc->gate = !!(flags & I2C_MUX_GATE);
	muxc->select = select;
	muxc->deselect = deselect;
	muxc->max_adapters = max_adapters;

	return muxc;
}
EXPORT_SYMBOL_GPL(i2c_mux_alloc);

static const struct i2c_lock_operations i2c_mux_lock_ops = {
	.lock_bus =    i2c_mux_lock_bus,
	.trylock_bus = i2c_mux_trylock_bus,
	.unlock_bus =  i2c_mux_unlock_bus,
};

static const struct i2c_lock_operations i2c_parent_lock_ops = {
	.lock_bus =    i2c_parent_lock_bus,
	.trylock_bus = i2c_parent_trylock_bus,
	.unlock_bus =  i2c_parent_unlock_bus,
};

/**
 * i2c_mux_add_adapter - 为一个 mux 通道创建子适配器
 * @muxc:     mux 核心对象
 * @force_nr: 若非 0，则强制使用这个总线号
 * @chan_id:  要导出的逻辑通道号
 *
 * 每个 mux 通道在 Linux 里都会表现成一条独立的 i2c_adapter。
 * 这样上层驱动不需要感知“先切通道再访问设备”的细节，只会看到
 * 一条普通 I2C 总线。
 *
 * Return: 0 表示成功，负值表示失败原因
 */
int i2c_mux_add_adapter(struct i2c_mux_core *muxc,
			u32 force_nr, u32 chan_id)
{
	struct i2c_adapter *parent = muxc->parent;
	struct i2c_mux_priv *priv;
	char symlink_name[20];
	int ret;

	if (muxc->num_adapters >= muxc->max_adapters) {
		dev_err(muxc->dev, "No room for more i2c-mux adapters\n");
		return -EINVAL;
	}

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;

	/* 建立通道私有数据，并让子 adapter 知道自己对应哪个 chan_id。 */
	priv->muxc = muxc;
	priv->chan_id = chan_id;

	/*
	 * 运行时拼装 algorithm。
	 *
	 * 父 adapter 可能只支持 I2C、只支持 SMBus，或者两者都支持，
	 * 因此这里要按 parent->algo 的能力动态补齐子 adapter 的入口。
	 * 同时根据 mux_locked 标志，决定转发到“有锁”还是“无锁”版本。
	 */
	if (parent->algo->master_xfer) {
		if (muxc->mux_locked)
			priv->algo.xfer = i2c_mux_master_xfer;
		else
			priv->algo.xfer = __i2c_mux_master_xfer;
	}
	if (parent->algo->master_xfer_atomic)
		priv->algo.xfer_atomic = priv->algo.master_xfer;

	if (parent->algo->smbus_xfer) {
		if (muxc->mux_locked)
			priv->algo.smbus_xfer = i2c_mux_smbus_xfer;
		else
			priv->algo.smbus_xfer = __i2c_mux_smbus_xfer;
	}
	if (parent->algo->smbus_xfer_atomic)
		priv->algo.smbus_xfer_atomic = priv->algo.smbus_xfer;

	priv->algo.functionality = i2c_mux_functionality;

	/* 再把它当作一条普通 I2C 总线去填充公共字段。 */
	snprintf(priv->adap.name, sizeof(priv->adap.name),
		 "i2c-%d-mux (chan_id %d)", i2c_adapter_id(parent), chan_id);
	priv->adap.owner = THIS_MODULE;
	priv->adap.algo = &priv->algo;
	priv->adap.algo_data = priv;
	priv->adap.dev.parent = &parent->dev;
	priv->adap.retries = parent->retries;
	priv->adap.timeout = parent->timeout;
	priv->adap.quirks = parent->quirks;
	if (muxc->mux_locked)
		priv->adap.lock_ops = &i2c_mux_lock_ops;
	else
		priv->adap.lock_ops = &i2c_parent_lock_ops;

	/*
	 * 尝试给子 adapter 关联 OF 节点。
	 *
	 * 这里兼容 i2c-mux / i2c-arb / i2c-gate 三种常见子节点容器；
	 * 若找不到显式容器，则回退到 mux 设备本身节点，再按 reg 匹配通道。
	 */
	if (muxc->dev->of_node) {
		struct device_node *dev_node = muxc->dev->of_node;
		struct device_node *mux_node, *child = NULL;
		u32 reg;

		if (muxc->arbitrator)
			mux_node = of_get_child_by_name(dev_node, "i2c-arb");
		else if (muxc->gate)
			mux_node = of_get_child_by_name(dev_node, "i2c-gate");
		else
			mux_node = of_get_child_by_name(dev_node, "i2c-mux");

		if (mux_node) {
			/* A "reg" property indicates an old-style DT entry */
			if (!of_property_read_u32(mux_node, "reg", &reg)) {
				of_node_put(mux_node);
				mux_node = NULL;
			}
		}

		if (!mux_node)
			mux_node = of_node_get(dev_node);
		else if (muxc->arbitrator || muxc->gate)
			child = of_node_get(mux_node);

		if (!child) {
			for_each_child_of_node(mux_node, child) {
				ret = of_property_read_u32(child, "reg", &reg);
				if (ret)
					continue;
				if (chan_id == reg)
					break;
			}
		}

		priv->adap.dev.of_node = child;
		of_node_put(mux_node);
	}

	/* ACPI 场景下，也让子 adapter 继承到对应的 companion 关系。 */
	if (has_acpi_companion(muxc->dev))
		acpi_preset_companion(&priv->adap.dev,
				      ACPI_COMPANION(muxc->dev),
				      chan_id);

	if (force_nr) {
		priv->adap.nr = force_nr;
		ret = i2c_add_numbered_adapter(&priv->adap);
		if (ret < 0) {
			dev_err(&parent->dev,
				"failed to add mux-adapter %u as bus %u (error=%d)\n",
				chan_id, force_nr, ret);
			goto err_free_priv;
		}
	} else {
		ret = i2c_add_adapter(&priv->adap);
		if (ret < 0) {
			dev_err(&parent->dev,
				"failed to add mux-adapter %u (error=%d)\n",
				chan_id, ret);
			goto err_free_priv;
		}
	}

	WARN(sysfs_create_link(&priv->adap.dev.kobj, &muxc->dev->kobj,
			       "mux_device"),
	     "can't create symlink to mux device\n");

	snprintf(symlink_name, sizeof(symlink_name), "channel-%u", chan_id);
	WARN(sysfs_create_link(&muxc->dev->kobj, &priv->adap.dev.kobj,
			       symlink_name),
	     "can't create symlink to channel %u\n", chan_id);
	dev_info(&parent->dev, "Added multiplexed i2c bus %d\n",
		 i2c_adapter_id(&priv->adap));

	muxc->adapter[muxc->num_adapters++] = &priv->adap;
	return 0;

err_free_priv:
	kfree(priv);
	return ret;
}
EXPORT_SYMBOL_GPL(i2c_mux_add_adapter);

/**
 * i2c_mux_del_adapters - 删除某个 mux 创建出来的全部子适配器
 * @muxc: mux 核心对象
 *
 * 逆序拆掉所有 channel adapter，并清理 sysfs 链接、OF 引用和
 * 每通道私有数据。
 */
void i2c_mux_del_adapters(struct i2c_mux_core *muxc)
{
	char symlink_name[20];

	while (muxc->num_adapters) {
		struct i2c_adapter *adap = muxc->adapter[--muxc->num_adapters];
		struct i2c_mux_priv *priv = adap->algo_data;
		struct device_node *np = adap->dev.of_node;

		muxc->adapter[muxc->num_adapters] = NULL;

		snprintf(symlink_name, sizeof(symlink_name),
			 "channel-%u", priv->chan_id);
		sysfs_remove_link(&muxc->dev->kobj, symlink_name);

		sysfs_remove_link(&priv->adap.dev.kobj, "mux_device");
		i2c_del_adapter(adap);
		of_node_put(np);
		kfree(priv);
	}
}
EXPORT_SYMBOL_GPL(i2c_mux_del_adapters);

MODULE_AUTHOR("Rodolfo Giometti <giometti@linux.it>");
MODULE_DESCRIPTION("I2C driver for multiplexed I2C busses");
MODULE_LICENSE("GPL v2");
