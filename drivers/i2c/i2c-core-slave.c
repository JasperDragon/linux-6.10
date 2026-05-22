// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core 的从设备（slave）支持代码
 *
 * Copyright (C) 2014 by Wolfram Sang <wsa@sang-engineering.com>
 */

#include <dt-bindings/i2c/i2c.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/property.h>

#include "i2c-core.h"

#define CREATE_TRACE_POINTS
#include <trace/events/i2c_slave.h>

int i2c_slave_register(struct i2c_client *client, i2c_slave_cb_t slave_cb)
{
	int ret;

	if (WARN(IS_ERR_OR_NULL(client) || !slave_cb, "insufficient data\n"))
		return -EINVAL;

	if (!(client->flags & I2C_CLIENT_SLAVE))
		dev_warn(&client->dev, "%s: client slave 标志未设置，可能会出现地址冲突\n",
			 __func__);

	if (!(client->flags & I2C_CLIENT_TEN)) {
		/* 对 7 位地址执行更严格的检查。 */
		ret = i2c_check_7bit_addr_validity_strict(client->addr);
		if (ret) {
			dev_err(&client->dev, "%s: 地址无效\n", __func__);
			return ret;
		}
	}

	if (!client->adapter->algo->reg_slave) {
		dev_err(&client->dev, "%s: 该适配器不支持此功能\n", __func__);
		return -EOPNOTSUPP;
	}

	client->slave_cb = slave_cb;

	i2c_lock_bus(client->adapter, I2C_LOCK_ROOT_ADAPTER);
	ret = client->adapter->algo->reg_slave(client);
	i2c_unlock_bus(client->adapter, I2C_LOCK_ROOT_ADAPTER);

	if (ret) {
		client->slave_cb = NULL;
		dev_err(&client->dev, "%s: 适配器返回错误 %d\n", __func__, ret);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_slave_register);

int i2c_slave_unregister(struct i2c_client *client)
{
	int ret;

	if (IS_ERR_OR_NULL(client))
		return -EINVAL;

	if (!client->adapter->algo->unreg_slave) {
		dev_err(&client->dev, "%s: 该适配器不支持此功能\n", __func__);
		return -EOPNOTSUPP;
	}

	i2c_lock_bus(client->adapter, I2C_LOCK_ROOT_ADAPTER);
	ret = client->adapter->algo->unreg_slave(client);
	i2c_unlock_bus(client->adapter, I2C_LOCK_ROOT_ADAPTER);

	if (ret == 0)
		client->slave_cb = NULL;
	else
		dev_err(&client->dev, "%s: 适配器返回错误 %d\n", __func__, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_slave_unregister);

int i2c_slave_event(struct i2c_client *client,
		    enum i2c_slave_event event, u8 *val)
{
	int ret = client->slave_cb(client, event, val);

	if (trace_i2c_slave_enabled())
		trace_call__i2c_slave(client, event, val, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_slave_event);

/**
 * i2c_detect_slave_mode - 检测设备是否以从设备模式工作
 * @dev: 持有该总线的设备
 *
 * 通过检查设备树中 reg 属性所使用的地址，判断是否存在 I2C 从设备。
 * 如果地址匹配 I2C_OWN_SLAVE_ADDRESS 标志，就说明该设备被配置为
 * I2C 从设备，并会在该地址上监听。
 *
 * 如果检测到 I2C own slave address，则返回 true，否则返回 false。
 */
bool i2c_detect_slave_mode(struct device *dev)
{
	struct fwnode_handle *fwnode = dev_fwnode(dev);

	if (is_of_node(fwnode)) {
		u32 reg;

		fwnode_for_each_child_node_scoped(fwnode, child) {
			fwnode_property_read_u32(child, "reg", &reg);
			if (reg & I2C_OWN_SLAVE_ADDRESS)
				return true;
		}
	} else if (is_acpi_device_node(fwnode)) {
		dev_dbg(dev, "ACPI 从设备模式暂不支持\n");
	}
	return false;
}
EXPORT_SYMBOL_GPL(i2c_detect_slave_mode);
