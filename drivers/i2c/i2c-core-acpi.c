// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core 的 ACPI 支持代码
 *
 * 这一层负责把 ACPI 描述的 I2C 资源转换成 i2c_client，
 * 同时解析 IRQ、总线速度和 adapter 绑定关系。
 *
 * Copyright (C) 2014 Intel Corp, Author: Lan Tianyu <tianyu.lan@intel.com>
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "i2c-core.h"

struct i2c_acpi_handler_data {
	struct acpi_connection_info info;
	struct i2c_adapter *adapter;
};

struct gsb_buffer {
	u8	status;
	u8	len;
	union {
		u16	wdata;
		u8	bdata;
		DECLARE_FLEX_ARRAY(u8, data);
	};
} __packed;

struct i2c_acpi_lookup {
	struct i2c_board_info *info;
	acpi_handle adapter_handle;
	acpi_handle device_handle;
	acpi_handle search_handle;
	int n;
	int index;
	u32 speed;
	u32 min_speed;
	u32 force_speed;
};

/**
 * i2c_acpi_get_i2c_resource - 从 ACPI 资源中提取 I2cSerialBus 项
 * @ares: ACPI 资源项
 * @i2c:  若匹配成功，则在这里返回 I2cSerialBus 资源指针
 *
 * 判断给定 ACPI 资源是否为 I2cSerialBus。
 * 如果是，就把该资源返回给调用者。
 *
 * 返回 true 表示资源类型匹配，否则返回 false。
 */
bool i2c_acpi_get_i2c_resource(struct acpi_resource *ares,
			       struct acpi_resource_i2c_serialbus **i2c)
{
	struct acpi_resource_i2c_serialbus *sb;

	if (ares->type != ACPI_RESOURCE_TYPE_SERIAL_BUS)
		return false;

	sb = &ares->data.i2c_serial_bus;
	if (sb->type != ACPI_RESOURCE_SERIAL_TYPE_I2C)
		return false;

	*i2c = sb;
	return true;
}
EXPORT_SYMBOL_GPL(i2c_acpi_get_i2c_resource);

static int i2c_acpi_resource_count(struct acpi_resource *ares, void *data)
{
	struct acpi_resource_i2c_serialbus *sb;
	int *count = data;

	if (i2c_acpi_get_i2c_resource(ares, &sb))
		*count = *count + 1;

	return 1;
}

/**
 * i2c_acpi_client_count - 统计 ACPI 设备里有多少个 I2cSerialBus 资源
 * @adev: ACPI 设备
 *
 * 一个 ACPI device 可能声明多个 I2cSerialBus() 资源，分别指向不同地址
 * 甚至不同 adapter。这个 helper 只负责计数，供上层决定是否要为第 N 个
 * 资源额外创建 i2c_client。
 *
 * Return: I2cSerialBus 资源个数，或负 errno
 */
int i2c_acpi_client_count(struct acpi_device *adev)
{
	int ret, count = 0;
	LIST_HEAD(r);

	ret = acpi_dev_get_resources(adev, &r, i2c_acpi_resource_count, &count);
	if (ret < 0)
		return ret;

	acpi_dev_free_resource_list(&r);
	return count;
}
EXPORT_SYMBOL_GPL(i2c_acpi_client_count);

static int i2c_acpi_fill_info(struct acpi_resource *ares, void *data)
{
	struct i2c_acpi_lookup *lookup = data;
	struct i2c_board_info *info = lookup->info;
	struct acpi_resource_i2c_serialbus *sb;
	acpi_status status;

	if (info->addr || !i2c_acpi_get_i2c_resource(ares, &sb))
		return 1;

	if (lookup->index != -1 && lookup->n++ != lookup->index)
		return 1;

	status = acpi_get_handle(lookup->device_handle,
				 sb->resource_source.string_ptr,
				 &lookup->adapter_handle);
	if (ACPI_FAILURE(status))
		return 1;

	info->addr = sb->slave_address;
	lookup->speed = sb->connection_speed;
	if (sb->access_mode == ACPI_I2C_10BIT_MODE)
		info->flags |= I2C_CLIENT_TEN;

	return 1;
}

static const struct acpi_device_id i2c_acpi_ignored_device_ids[] = {
	/*
	 * ACPI video 设备有时会携带 I2C 资源，但它们由 acpi-video 驱动接管，
	 * 不应该被 I2C core 再次实例化。
	 */
	{ ACPI_VIDEO_HID, 0 },
	{}
};

struct i2c_acpi_irq_context {
	int irq;
	bool wake_capable;
};

static int i2c_acpi_do_lookup(struct acpi_device *adev,
			      struct i2c_acpi_lookup *lookup)
{
	struct i2c_board_info *info = lookup->info;
	struct list_head resource_list;
	int ret;

	if (acpi_bus_get_status(adev))
		return -EINVAL;

	if (!acpi_dev_ready_for_enumeration(adev))
		return -ENODEV;

	if (acpi_match_device_ids(adev, i2c_acpi_ignored_device_ids) == 0)
		return -ENODEV;

	memset(info, 0, sizeof(*info));
	lookup->device_handle = acpi_device_handle(adev);

	/*
	 * 这是 ACPI -> i2c_board_info 的第一步。
	 *
	 * 它会从资源表里找出 I2cSerialBus()，提取 slave_address、
	 * 10-bit 标志、connection_speed，以及资源里引用的 adapter handle。
	 */
	INIT_LIST_HEAD(&resource_list);
	ret = acpi_dev_get_resources(adev, &resource_list,
				     i2c_acpi_fill_info, lookup);
	acpi_dev_free_resource_list(&resource_list);

	if (ret < 0 || !info->addr)
		return -EINVAL;

	return 0;
}

static int i2c_acpi_add_irq_resource(struct acpi_resource *ares, void *data)
{
	struct i2c_acpi_irq_context *irq_ctx = data;
	struct resource r;

	if (irq_ctx->irq > 0)
		return 1;

	if (!acpi_dev_resource_interrupt(ares, 0, &r))
		return 1;

	irq_ctx->irq = i2c_dev_irq_from_resources(&r, 1);
	irq_ctx->wake_capable = r.flags & IORESOURCE_IRQ_WAKECAPABLE;

	return 1; /* 不需要把这个资源再挂进资源链表。 */
}

/**
 * i2c_acpi_get_irq - 从 ACPI 中获取设备 IRQ
 * @client:        目标 I2C client
 * @wake_capable:  若 IRQ 具备唤醒能力，则在这里返回 true
 *
 * 查找指定 client 使用的 IRQ 编号。
 *
 * Return: IRQ 号或错误码。
 */
int i2c_acpi_get_irq(struct i2c_client *client, bool *wake_capable)
{
	struct acpi_device *adev = ACPI_COMPANION(&client->dev);
	struct list_head resource_list;
	struct i2c_acpi_irq_context irq_ctx = {
		.irq = -ENOENT,
	};
	int ret;

	INIT_LIST_HEAD(&resource_list);

	ret = acpi_dev_get_resources(adev, &resource_list,
				     i2c_acpi_add_irq_resource, &irq_ctx);
	if (ret < 0)
		return ret;

	acpi_dev_free_resource_list(&resource_list);

	if (irq_ctx.irq == -ENOENT)
		irq_ctx.irq = acpi_dev_gpio_irq_wake_get(adev, 0, &irq_ctx.wake_capable);

	if (irq_ctx.irq < 0)
		return irq_ctx.irq;

	if (wake_capable)
		*wake_capable = irq_ctx.wake_capable;

	return irq_ctx.irq;
}

static int i2c_acpi_get_info(struct acpi_device *adev,
			     struct i2c_board_info *info,
			     struct i2c_adapter *adapter,
			     acpi_handle *adapter_handle)
{
	struct i2c_acpi_lookup lookup;
	int ret;

	memset(&lookup, 0, sizeof(lookup));
	lookup.info = info;
	lookup.index = -1;

	if (acpi_device_enumerated(adev))
		return -EINVAL;

	ret = i2c_acpi_do_lookup(adev, &lookup);
	if (ret)
		return ret;

	if (adapter) {
		/* 传进来的 adapter 必须和 I2cSerialBus() 里声明的上游控制器一致。 */
		if (!device_match_acpi_handle(&adapter->dev, lookup.adapter_handle))
			return -ENODEV;
	} else {
		struct acpi_device *adapter_adev;

		/* 如果调用方没给 adapter，至少要保证资源里引用的 adapter 真实存在。 */
		adapter_adev = acpi_fetch_acpi_dev(lookup.adapter_handle);
		if (!adapter_adev)
			return -ENODEV;
		if (acpi_bus_get_status(adapter_adev) ||
		    !adapter_adev->status.present)
			return -ENODEV;
	}

	info->fwnode = acpi_fwnode_handle(adev);
	if (adapter_handle)
		*adapter_handle = lookup.adapter_handle;

	acpi_set_modalias(adev, dev_name(&adev->dev), info->type,
			  sizeof(info->type));

	return 0;
}

/*
 * 把一个 ACPI 设备真正注册成 i2c_client。
 *
 * 这一步会把 ACPI companion 标记为“已枚举”，避免后续重复实例化。
 * 如果 i2c_new_client_device() 失败，还要把这个状态回滚掉。
 */
static void i2c_acpi_register_device(struct i2c_adapter *adapter,
				     struct acpi_device *adev,
				     struct i2c_board_info *info)
{
	/*
	 * Skip registration on boards where the ACPI tables are
	 * known to contain bogus I2C devices.
	 */
	if (acpi_quirk_skip_i2c_client_enumeration(adev))
		return;

	adev->power.flags.ignore_parent = true;
	acpi_device_set_enumerated(adev);

	if (IS_ERR(i2c_new_client_device(adapter, info)))
		adev->power.flags.ignore_parent = false;
}

static acpi_status i2c_acpi_add_device(acpi_handle handle, u32 level,
				       void *data, void **return_value)
{
	struct i2c_adapter *adapter = data;
	struct acpi_device *adev = acpi_fetch_acpi_dev(handle);
	struct i2c_board_info info;

	if (!adev || i2c_acpi_get_info(adev, &info, adapter, NULL))
		return AE_OK;

	i2c_acpi_register_device(adapter, adev, &info);

	return AE_OK;
}

#define I2C_ACPI_MAX_SCAN_DEPTH 32

/**
 * i2c_acpi_register_devices - 枚举挂在某个 adapter 后面的 ACPI I2C 设备
 * @adap: 目标 adapter
 *
 * 这里会遍历 ACPI 命名空间，找出资源里引用当前 adapter 的所有
 * I2cSerialBus() 设备，并把它们注册到 Linux device model。
 *
 * Return: 无
 */
void i2c_acpi_register_devices(struct i2c_adapter *adap)
{
	struct acpi_device *adev;
	acpi_status status;

	if (!has_acpi_companion(&adap->dev))
		return;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				     I2C_ACPI_MAX_SCAN_DEPTH,
				     i2c_acpi_add_device, NULL,
				     adap, NULL);
	if (ACPI_FAILURE(status))
		dev_warn(&adap->dev, "failed to enumerate I2C slaves\n");

	if (!adap->dev.parent)
		return;

	adev = ACPI_COMPANION(adap->dev.parent);
	if (!adev)
		return;

	acpi_dev_clear_dependencies(adev);
}

static const struct acpi_device_id i2c_acpi_force_400khz_device_ids[] = {
	/*
	 * These Silead touchscreen controllers only work at 400KHz, for
	 * some reason they do not work at 100KHz. On some devices the ACPI
	 * tables list another device at their bus as only being capable
	 * of 100KHz, testing has shown that these other devices work fine
	 * at 400KHz (as can be expected of any recent i2c hw) so we force
	 * the speed of the bus to 400 KHz if a Silead device is present.
	 */
	{ "MSSL1680", 0 },
	{}
};

static const struct acpi_device_id i2c_acpi_force_100khz_device_ids[] = {
	/*
	 * When a 400KHz freq is used on this model of ELAN touchpad in Linux,
	 * excessive smoothing (similar to when the touchpad's firmware detects
	 * a noisy signal) is sometimes applied. As some devices' (e.g, Lenovo
	 * V15 G4) ACPI tables specify a 400KHz frequency for this device and
	 * some I2C busses (e.g, Designware I2C) default to a 400KHz freq,
	 * force the speed to 100KHz as a workaround.
	 *
	 * For future investigation: This problem may be related to the default
	 * HCNT/LCNT values given by some busses' drivers, because they are not
	 * specified in the aforementioned devices' ACPI tables, and because
	 * the device works without issues on Windows at what is expected to be
	 * a 400KHz frequency. The root cause of the issue is not known.
	 */
	{ "DLL0945", 0 },
	{ "ELAN0678", 0 },
	{ "ELAN06FA", 0 },
	{}
};

static acpi_status i2c_acpi_lookup_speed(acpi_handle handle, u32 level,
					   void *data, void **return_value)
{
	struct i2c_acpi_lookup *lookup = data;
	struct acpi_device *adev = acpi_fetch_acpi_dev(handle);

	if (!adev || i2c_acpi_do_lookup(adev, lookup))
		return AE_OK;

	if (lookup->search_handle != lookup->adapter_handle)
		return AE_OK;

	if (lookup->speed <= lookup->min_speed)
		lookup->min_speed = lookup->speed;

	if (acpi_match_device_ids(adev, i2c_acpi_force_400khz_device_ids) == 0)
		lookup->force_speed = I2C_MAX_FAST_MODE_FREQ;

	if (acpi_match_device_ids(adev, i2c_acpi_force_100khz_device_ids) == 0)
		lookup->force_speed = I2C_MAX_STANDARD_MODE_FREQ;

	return AE_OK;
}

/**
 * i2c_acpi_find_bus_speed - 从 ACPI 中推导 I2C 总线速度
 * @dev: 拥有这条总线的设备
 *
 * 遍历 ACPI 命名空间里挂在这条总线上的所有 I2C 从设备，
 * 取其中最慢的速度作为总线速度。
 *
 * 某些已知有问题的设备还会触发强制修正逻辑，例如强制 100 kHz 或
 * 强制 400 kHz，这些都属于基于 DMI/ACPI 经验积累的兼容性兜底。
 *
 * Return: 总线速度（Hz），若无法推导则返回 0
 */
u32 i2c_acpi_find_bus_speed(struct device *dev)
{
	struct i2c_acpi_lookup lookup;
	struct i2c_board_info dummy;
	acpi_status status;

	if (!has_acpi_companion(dev))
		return 0;

	memset(&lookup, 0, sizeof(lookup));
	lookup.search_handle = ACPI_HANDLE(dev);
	lookup.min_speed = UINT_MAX;
	lookup.info = &dummy;
	lookup.index = -1;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				     I2C_ACPI_MAX_SCAN_DEPTH,
				     i2c_acpi_lookup_speed, NULL,
				     &lookup, NULL);

	if (ACPI_FAILURE(status)) {
		dev_warn(dev, "unable to find I2C bus speed from ACPI\n");
		return 0;
	}

	if (lookup.force_speed) {
		if (lookup.force_speed != lookup.min_speed)
			dev_warn(dev, FW_BUG "DSDT uses known not-working I2C bus speed %d, forcing it to %d\n",
				 lookup.min_speed, lookup.force_speed);
		return lookup.force_speed;
	} else if (lookup.min_speed != UINT_MAX) {
		return lookup.min_speed;
	} else {
		return 0;
	}
}
EXPORT_SYMBOL_GPL(i2c_acpi_find_bus_speed);

/*
 * 通过 ACPI handle 反查 i2c_adapter。
 *
 * 这和 OF 场景按 node 查 adapter 的思路类似，只不过 ACPI 这边用的是
 * companion handle 作为关联键。
 */
struct i2c_adapter *i2c_acpi_find_adapter_by_handle(acpi_handle handle)
{
	struct i2c_adapter *adapter;
	struct device *dev;

	dev = bus_find_device(&i2c_bus_type, NULL, handle, device_match_acpi_handle);
	if (!dev)
		return NULL;

	adapter = i2c_verify_adapter(dev);
	if (!adapter)
		put_device(dev);

	return adapter;
}
EXPORT_SYMBOL_GPL(i2c_acpi_find_adapter_by_handle);

static struct i2c_client *i2c_acpi_find_client_by_adev(struct acpi_device *adev)
{
	return i2c_find_device_by_fwnode(acpi_fwnode_handle(adev));
}

static struct i2c_adapter *i2c_acpi_find_adapter_by_adev(struct acpi_device *adev)
{
	return i2c_find_adapter_by_fwnode(acpi_fwnode_handle(adev));
}

static int i2c_acpi_notify(struct notifier_block *nb, unsigned long value,
			   void *arg)
{
	struct acpi_device *adev = arg;
	struct i2c_board_info info;
	acpi_handle adapter_handle;
	struct i2c_adapter *adapter;
	struct i2c_client *client;

	switch (value) {
	case ACPI_RECONFIG_DEVICE_ADD:
		/* 设备热插入或运行时补枚举：重新走一遍 ACPI -> client 实例化。 */
		if (i2c_acpi_get_info(adev, &info, NULL, &adapter_handle))
			break;

		adapter = i2c_acpi_find_adapter_by_handle(adapter_handle);
		if (!adapter)
			break;

		i2c_acpi_register_device(adapter, adev, &info);
		put_device(&adapter->dev);
		break;
	case ACPI_RECONFIG_DEVICE_REMOVE:
		/* 热移除时，先删 client，再清理可能绑定到 companion 的 adapter。 */
		if (!acpi_device_enumerated(adev))
			break;

		client = i2c_acpi_find_client_by_adev(adev);
		if (client) {
			i2c_unregister_device(client);
			put_device(&client->dev);
		}

		adapter = i2c_acpi_find_adapter_by_adev(adev);
		if (adapter) {
			acpi_unbind_one(&adapter->dev);
			put_device(&adapter->dev);
		}

		break;
	}

	return NOTIFY_OK;
}

struct notifier_block i2c_acpi_notifier = {
	.notifier_call = i2c_acpi_notify,
};

/**
 * i2c_acpi_new_device_by_fwnode - 为第 N 个 I2cSerialBus 资源创建 i2c_client
 * @fwnode:  含有目标 ACPI 资源的 fwnode
 * @index:   要提取的 ACPI 资源索引
 * @info:    设备描述模板；函数内部会回填地址等字段
 * Context: can sleep
 *
 * 默认情况下，I2C 子系统只会为 acpi_device 的第一个 I2cSerialBus
 * 资源创建 i2c-client；但有些 acpi_device 有多个 I2cSerialBus 资源，
 * 此时就可以用这个函数来为当前资源表（Current Resource Settings）中的其他资源
 * 创建对应的 i2c-client。
 *
 * 该函数内部会调用 i2c_new_client_device。
 *
 * Return: 成功返回新的 i2c-client；失败时返回 ERR_PTR()。
 * 如果暂时还找不到对应 adapter，则返回 -EPROBE_DEFER。
 */
struct i2c_client *i2c_acpi_new_device_by_fwnode(struct fwnode_handle *fwnode,
						 int index,
						 struct i2c_board_info *info)
{
	struct i2c_acpi_lookup lookup;
	struct i2c_adapter *adapter;
	struct acpi_device *adev;
	LIST_HEAD(resource_list);
	int ret;

	adev = to_acpi_device_node(fwnode);
	if (!adev)
		return ERR_PTR(-ENODEV);

	memset(&lookup, 0, sizeof(lookup));
	lookup.info = info;
	lookup.device_handle = acpi_device_handle(adev);
	lookup.index = index;

	ret = acpi_dev_get_resources(adev, &resource_list,
				     i2c_acpi_fill_info, &lookup);
	if (ret < 0)
		return ERR_PTR(ret);

	acpi_dev_free_resource_list(&resource_list);

	if (!info->addr)
		return ERR_PTR(-EADDRNOTAVAIL);

	adapter = i2c_acpi_find_adapter_by_handle(lookup.adapter_handle);
	if (!adapter)
		return ERR_PTR(-EPROBE_DEFER);

	return i2c_new_client_device(adapter, info);
}
EXPORT_SYMBOL_GPL(i2c_acpi_new_device_by_fwnode);

/*
 * 判断这个 ACPI I2C 设备在 probe 前是否可以跳过强制 D0 上电。
 *
 * 某些平台的 ACPI 电源时序比较脆弱，驱动会显式声明“不要为了 probe
 * 把设备切到 D0”。这里就是 I2C core 侧的统一判断入口。
 */
bool i2c_acpi_waive_d0_probe(struct device *dev)
{
	struct i2c_driver *driver = to_i2c_driver(dev->driver);
	struct acpi_device *adev = ACPI_COMPANION(dev);

	return driver->flags & I2C_DRV_ACPI_WAIVE_D0_PROBE &&
		adev && adev->power.state_for_enumeration >= adev->power.state;
}
EXPORT_SYMBOL_GPL(i2c_acpi_waive_d0_probe);

#ifdef CONFIG_ACPI_I2C_OPREGION
static int acpi_gsb_i2c_read_bytes(struct i2c_client *client,
		u8 cmd, u8 *data, u8 data_len)
{

	struct i2c_msg msgs[2];
	int ret;
	u8 *buffer;

	buffer = kzalloc(data_len, GFP_KERNEL);
	if (!buffer)
		return AE_NO_MEMORY;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &cmd;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].len = data_len;
	msgs[1].buf = buffer;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0) {
		/* Getting a NACK is unfortunately normal with some DSTDs */
		if (ret == -EREMOTEIO)
			dev_dbg(&client->adapter->dev, "i2c read %d bytes from client@%#x starting at reg %#x failed, error: %d\n",
				data_len, client->addr, cmd, ret);
		else
			dev_err(&client->adapter->dev, "i2c read %d bytes from client@%#x starting at reg %#x failed, error: %d\n",
				data_len, client->addr, cmd, ret);
	/* 2 transfers must have completed successfully */
	} else if (ret == 2) {
		memcpy(data, buffer, data_len);
		ret = 0;
	} else {
		ret = -EIO;
	}

	kfree(buffer);
	return ret;
}

static int acpi_gsb_i2c_write_bytes(struct i2c_client *client,
		u8 cmd, u8 *data, u8 data_len)
{

	struct i2c_msg msgs[1];
	u8 *buffer;
	int ret = AE_OK;

	buffer = kzalloc(data_len + 1, GFP_KERNEL);
	if (!buffer)
		return AE_NO_MEMORY;

	buffer[0] = cmd;
	memcpy(buffer + 1, data, data_len);

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = data_len + 1;
	msgs[0].buf = buffer;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));

	kfree(buffer);

	if (ret < 0) {
		dev_err(&client->adapter->dev, "i2c write failed: %d\n", ret);
		return ret;
	}

	/* 1 transfer must have completed successfully */
	return (ret == 1) ? 0 : -EIO;
}

static acpi_status
i2c_acpi_space_handler(u32 function, acpi_physical_address command,
			u32 bits, u64 *value64,
			void *handler_context, void *region_context)
{
	struct gsb_buffer *gsb = (struct gsb_buffer *)value64;
	struct i2c_acpi_handler_data *data = handler_context;
	struct acpi_connection_info *info = &data->info;
	struct acpi_resource_i2c_serialbus *sb;
	struct i2c_adapter *adapter = data->adapter;
	struct i2c_client *client;
	struct acpi_resource *ares;
	u32 accessor_type = function >> 16;
	u8 action = function & ACPI_IO_MASK;
	acpi_status ret;
	int status;

	ret = acpi_buffer_to_resource(info->connection, info->length, &ares);
	if (ACPI_FAILURE(ret))
		return ret;

	client = kzalloc_obj(*client);
	if (!client) {
		ret = AE_NO_MEMORY;
		goto err;
	}

	if (!value64 || !i2c_acpi_get_i2c_resource(ares, &sb)) {
		ret = AE_BAD_PARAMETER;
		goto err;
	}

	client->adapter = adapter;
	client->addr = sb->slave_address;

	if (sb->access_mode == ACPI_I2C_10BIT_MODE)
		client->flags |= I2C_CLIENT_TEN;

	switch (accessor_type) {
	case ACPI_GSB_ACCESS_ATTRIB_SEND_RCV:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_byte(client);
			if (status >= 0) {
				gsb->bdata = status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_byte(client, gsb->bdata);
		}
		break;

	case ACPI_GSB_ACCESS_ATTRIB_BYTE:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_byte_data(client, command);
			if (status >= 0) {
				gsb->bdata = status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_byte_data(client, command,
					gsb->bdata);
		}
		break;

	case ACPI_GSB_ACCESS_ATTRIB_WORD:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_word_data(client, command);
			if (status >= 0) {
				gsb->wdata = status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_word_data(client, command,
					gsb->wdata);
		}
		break;

	case ACPI_GSB_ACCESS_ATTRIB_BLOCK:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_block_data(client, command,
					gsb->data);
			if (status >= 0) {
				gsb->len = status;
				status = 0;
			}
		} else {
			status = i2c_smbus_write_block_data(client, command,
					gsb->len, gsb->data);
		}
		break;

	case ACPI_GSB_ACCESS_ATTRIB_MULTIBYTE:
		if (action == ACPI_READ) {
			status = acpi_gsb_i2c_read_bytes(client, command,
					gsb->data, info->access_length);
		} else {
			status = acpi_gsb_i2c_write_bytes(client, command,
					gsb->data, info->access_length);
		}
		break;

	default:
		dev_warn(&adapter->dev, "protocol 0x%02x not supported for client 0x%02x\n",
			 accessor_type, client->addr);
		ret = AE_BAD_PARAMETER;
		goto err;
	}

	gsb->status = status;

 err:
	kfree(client);
	ACPI_FREE(ares);
	return ret;
}


int i2c_acpi_install_space_handler(struct i2c_adapter *adapter)
{
	acpi_handle handle;
	struct i2c_acpi_handler_data *data;
	acpi_status status;

	if (!adapter->dev.parent)
		return -ENODEV;

	handle = ACPI_HANDLE(adapter->dev.parent);

	if (!handle)
		return -ENODEV;

	data = kzalloc_obj(struct i2c_acpi_handler_data);
	if (!data)
		return -ENOMEM;

	data->adapter = adapter;
	status = acpi_bus_attach_private_data(handle, (void *)data);
	if (ACPI_FAILURE(status)) {
		kfree(data);
		return -ENOMEM;
	}

	status = acpi_install_address_space_handler(handle,
				ACPI_ADR_SPACE_GSBUS,
				&i2c_acpi_space_handler,
				NULL,
				data);
	if (ACPI_FAILURE(status)) {
		dev_err(&adapter->dev, "Error installing i2c space handler\n");
		acpi_bus_detach_private_data(handle);
		kfree(data);
		return -ENOMEM;
	}

	return 0;
}

void i2c_acpi_remove_space_handler(struct i2c_adapter *adapter)
{
	acpi_handle handle;
	struct i2c_acpi_handler_data *data;
	acpi_status status;

	if (!adapter->dev.parent)
		return;

	handle = ACPI_HANDLE(adapter->dev.parent);

	if (!handle)
		return;

	acpi_remove_address_space_handler(handle,
				ACPI_ADR_SPACE_GSBUS,
				&i2c_acpi_space_handler);

	status = acpi_bus_get_private_data(handle, (void **)&data);
	if (ACPI_SUCCESS(status))
		kfree(data);

	acpi_bus_detach_private_data(handle);
}
#endif /* CONFIG_ACPI_I2C_OPREGION */
