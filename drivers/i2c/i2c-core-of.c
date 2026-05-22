// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core 的设备树支持代码
 *
 * 这一层负责把设备树里的 I2C 节点转换成 i2c_client，
 * 并处理运行时设备树变更带来的增删。
 *
 * Copyright (C) 2008 Jochen Friedrich <jochen@scram.de>
 * based on a previous patch from Jon Smirl <jonsmirl@gmail.com>
 *
 * Copyright (C) 2013, 2018 Wolfram Sang <wsa@kernel.org>
 */

#include <dt-bindings/i2c/i2c.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/sysfs.h>

#include "i2c-core.h"

int of_i2c_get_board_info(struct device *dev, struct device_node *node,
			  struct i2c_board_info *info)
{
	u32 addr;
	int ret;

	memset(info, 0, sizeof(*info));

	/* 先把 compatible 转成驱动匹配用的设备名。 */
	if (of_alias_from_compatible(node, info->type, sizeof(info->type)) < 0) {
		dev_err(dev, "of_i2c: modalias failure on %pOF\n", node);
		return -EINVAL;
	}

	/* reg 属性就是 I2C 地址；没有它就无法实例化设备。 */
	ret = of_property_read_u32(node, "reg", &addr);
	if (ret) {
		dev_err(dev, "of_i2c: invalid reg on %pOF\n", node);
		return ret;
	}

	if (addr & I2C_TEN_BIT_ADDRESS) {
		addr &= ~I2C_TEN_BIT_ADDRESS;
		info->flags |= I2C_CLIENT_TEN;
	}

	if (addr & I2C_OWN_SLAVE_ADDRESS) {
		addr &= ~I2C_OWN_SLAVE_ADDRESS;
		info->flags |= I2C_CLIENT_SLAVE;
	}

	info->addr = addr;
	info->fwnode = of_fwnode_handle(node);

	if (of_property_read_bool(node, "host-notify"))
		info->flags |= I2C_CLIENT_HOST_NOTIFY;

	if (of_property_read_bool(node, "wakeup-source"))
		info->flags |= I2C_CLIENT_WAKE;

	return 0;
}
EXPORT_SYMBOL_GPL(of_i2c_get_board_info);

/*
 * 把一个设备树子节点实例化成 i2c_client。
 *
 * 这一步本质上是“固件描述 -> i2c_board_info -> i2c_client”的桥接层，
 * 其中真正的设备注册仍然由 i2c_new_client_device() 完成。
 */
static struct i2c_client *of_i2c_register_device(struct i2c_adapter *adap,
						 struct device_node *node)
{
	struct i2c_client *client;
	struct i2c_board_info info;
	int ret;

	/* 把一个 OF 子节点转换成真正的 i2c_client。 */
	dev_dbg(&adap->dev, "of_i2c: register %pOF\n", node);

	ret = of_i2c_get_board_info(&adap->dev, node, &info);
	if (ret)
		return ERR_PTR(ret);

	client = i2c_new_client_device(adap, &info);
	if (IS_ERR(client))
		dev_err(&adap->dev, "of_i2c: Failure registering %pOF\n", node);

	return client;
}

void of_i2c_register_devices(struct i2c_adapter *adap)
{
	struct device_node *bus, *node;
	struct i2c_client *client;

	/* 只有适配器本身挂在设备树上，才会去扫描它的子节点。 */
	if (!adap->dev.of_node)
		return;

	/*
	 * 遍历 `i2c-bus` 子节点，或者直接遍历适配器节点本身的可用子节点。
	 *
	 * 很多 DT 写法会显式再包一层 `i2c-bus`；也有一些平台直接把外设
	 * 挂在 adapter 节点下面。这里同时兼容这两种描述风格。
	 */
	dev_dbg(&adap->dev, "of_i2c: walking child nodes\n");

	bus = of_get_child_by_name(adap->dev.of_node, "i2c-bus");
	if (!bus)
		bus = of_node_get(adap->dev.of_node);

	for_each_available_child_of_node(bus, node) {
		if (of_node_test_and_set_flag(node, OF_POPULATED))
			continue;

		client = of_i2c_register_device(adap, node);
		if (IS_ERR(client)) {
			dev_err(&adap->dev,
				 "Failed to create I2C device for %pOF\n",
				 node);
			of_node_clear_flag(node, OF_POPULATED);
		}
	}

	of_node_put(bus);
}

static const struct of_device_id*
i2c_of_match_device_sysfs(const struct of_device_id *matches,
				  struct i2c_client *client)
{
	const char *name;

	for (; matches->compatible[0]; matches++) {
			/*
			 * 通过 sysfs 新增设备时只有字符串，没有真正的 of_node，
			 * 所以不能依赖标准 of_match_device()。
			 */
		if (sysfs_streq(client->name, matches->compatible))
			return matches;

		name = strchr(matches->compatible, ',');
		if (!name)
			name = matches->compatible;
		else
			name++;

		if (sysfs_streq(client->name, name))
			return matches;
	}

	return NULL;
}

/*
 * 兼顾真实 OF 节点和 sysfs 动态创建设备的匹配 helper。
 *
 * 正常情况下优先走标准 of_match_device()；如果 client 是通过 sysfs
 * 手工创建出来的，它没有真正的 of_node，就只能退化成字符串匹配。
 */
const struct of_device_id
*i2c_of_match_device(const struct of_device_id *matches,
		     struct i2c_client *client)
{
	const struct of_device_id *match;

	if (!(client && matches))
		return NULL;

	match = of_match_device(matches, &client->dev);
	if (match)
		return match;

	return i2c_of_match_device_sysfs(matches, client);
}

#if IS_ENABLED(CONFIG_OF_DYNAMIC)
static int of_i2c_notify(struct notifier_block *nb, unsigned long action,
			 void *arg)
{
	struct of_reconfig_data *rd = arg;
	struct i2c_adapter *adap;
	struct i2c_client *client;

	switch (of_reconfig_get_state_change(action, rd)) {
	case OF_RECONFIG_CHANGE_ADD:
		/*
		 * 动态加节点时，先根据父节点反查 adapter，再把新节点实例化。
		 * OF_POPULATED 用来防止同一节点被重复注册。
		 */
		adap = of_find_i2c_adapter_by_node(rd->dn->parent);
		if (adap == NULL)
			return NOTIFY_OK;	/* not for us */

		if (of_node_test_and_set_flag(rd->dn, OF_POPULATED)) {
			put_device(&adap->dev);
			return NOTIFY_OK;
		}

			/*
			 * 在真正创建设备前先清掉标志，避免 fw_devlink
			 * 因为误判而跳过消费者关系。
			 */
		fwnode_clear_flag(&rd->dn->fwnode, FWNODE_FLAG_NOT_DEVICE);
		client = of_i2c_register_device(adap, rd->dn);
		if (IS_ERR(client)) {
			dev_err(&adap->dev, "failed to create client for '%pOF'\n",
				 rd->dn);
			put_device(&adap->dev);
			of_node_clear_flag(rd->dn, OF_POPULATED);
			return notifier_from_errno(PTR_ERR(client));
		}
		put_device(&adap->dev);
		break;
	case OF_RECONFIG_CHANGE_REMOVE:
		/* 已经反注册过就直接跳过。 */
		if (!of_node_check_flag(rd->dn, OF_POPULATED))
			return NOTIFY_OK;

		/* 通过节点反查对应的 i2c_client。 */
		client = of_find_i2c_device_by_node(rd->dn);
		if (client == NULL)
			return NOTIFY_OK;	/* no? not meant for us */

		/* 注销会先释放掉设备模型的一层引用。 */
		i2c_unregister_device(client);

		/* 把 find_device 得到的引用再放回去。 */
		put_device(&client->dev);
		break;
	}

	return NOTIFY_OK;
}

struct notifier_block i2c_of_notifier = {
	.notifier_call = of_i2c_notify,
};
#endif /* CONFIG_OF_DYNAMIC */
