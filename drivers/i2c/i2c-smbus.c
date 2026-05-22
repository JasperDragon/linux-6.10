// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * i2c-smbus.c - I2C 协议的 SMBus 扩展
 *
 * 这一层补充了 SMBALERT#、Host Notify、SPD 自动实例化等能力，
 * 用来处理那些“不是纯 I2C 寄存器访问”的兼容接口。
 *
 * Copyright (C) 2008 David Brownell
 * Copyright (C) 2010-2019 Jean Delvare <jdelvare@suse.de>
 */

#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct i2c_smbus_alert {
	struct work_struct	alert;
	struct i2c_client	*ara;		/* Alert response address */
};

struct alert_data {
	unsigned short		addr;
	enum i2c_alert_protocol	type;
	unsigned int		data;
};

/* 如果当前设备就是发出告警的那个客户端，就通知它的驱动。 */
static int smbus_do_alert(struct device *dev, void *addrp)
{
	struct i2c_client *client = i2c_verify_client(dev);
	struct alert_data *data = addrp;
	struct i2c_driver *driver;
	int ret;

	if (!client || client->addr != data->addr)
		return 0;
	if (client->flags & I2C_CLIENT_TEN)
		return 0;

	/*
	 * Drivers should either disable alerts, or provide at least
	 * a minimal handler.  Lock so the driver won't change.
	 */
	device_lock(dev);
	if (client->dev.driver) {
		driver = to_i2c_driver(client->dev.driver);
		if (driver->alert) {
			/* Stop iterating after we find the device */
			driver->alert(client, data->type, data->data);
			ret = -EBUSY;
		} else {
			dev_warn(&client->dev, "no driver alert()!\n");
			ret = -EOPNOTSUPP;
		}
	} else {
		dev_dbg(&client->dev, "alert with no driver\n");
		ret = -ENODEV;
	}
	device_unlock(dev);

	return ret;
}

/* 和上面类似，但会把所有带 alert() 的驱动都回调一遍。 */
static int smbus_do_alert_force(struct device *dev, void *addrp)
{
	struct i2c_client *client = i2c_verify_client(dev);
	struct alert_data *data = addrp;
	struct i2c_driver *driver;

	if (!client || (client->flags & I2C_CLIENT_TEN))
		return 0;

	/*
	 * Drivers should either disable alerts, or provide at least
	 * a minimal handler. Lock so the driver won't change.
	 */
	device_lock(dev);
	if (client->dev.driver) {
		driver = to_i2c_driver(client->dev.driver);
		if (driver->alert)
			driver->alert(client, data->type, data->data);
	}
	device_unlock(dev);

	return 0;
}

/*
 * alert 中断处理函数不能直接做 SMBus 传输，因为这类调用可能睡眠。
 * 所以它只负责把工作交给 workqueue。
 */
static irqreturn_t smbus_alert(int irq, void *d)
{
	struct i2c_smbus_alert *alert = d;
	struct i2c_client *ara;
	unsigned short prev_addr = I2C_CLIENT_END; /* Not a valid address */

	ara = alert->ara;

	for (;;) {
		s32 status;
		struct alert_data data;

		/*
		 * Devices with pending alerts reply in address order, low
		 * to high, because of slave transmit arbitration.  After
		 * responding, an SMBus device stops asserting SMBALERT#.
		 *
		 * Note that SMBus 2.0 reserves 10-bit addresses for future
		 * use.  We neither handle them, nor try to use PEC here.
		 */
		status = i2c_smbus_read_byte(ara);
		if (status < 0)
			break;

		data.data = status & 1;
		data.addr = status >> 1;
		data.type = I2C_PROTOCOL_SMBUS_ALERT;

		dev_dbg(&ara->dev, "SMBALERT# from dev 0x%02x, flag %d\n",
			data.addr, data.data);

		/* Notify driver for the device which issued the alert */
		status = device_for_each_child(&ara->adapter->dev, &data,
					       smbus_do_alert);
		/*
		 * If we read the same address more than once, and the alert
		 * was not handled by a driver, it won't do any good to repeat
		 * the loop because it will never terminate. Try again, this
		 * time calling the alert handlers of all devices connected to
		 * the bus, and abort the loop afterwards. If this helps, we
		 * are all set. If it doesn't, there is nothing else we can do,
		 * so we might as well abort the loop.
		 * Note: This assumes that a driver with alert handler handles
		 * the alert properly and clears it if necessary.
		 */
		if (data.addr == prev_addr && status != -EBUSY) {
			device_for_each_child(&ara->adapter->dev, &data,
					      smbus_do_alert_force);
			break;
		}
		prev_addr = data.addr;
	}

	return IRQ_HANDLED;
}

static void smbalert_work(struct work_struct *work)
{
	struct i2c_smbus_alert *alert;

	alert = container_of(work, struct i2c_smbus_alert, alert);

	smbus_alert(0, alert);

}

/* 初始化 SMBALERT# 基础设施。 */
static int smbalert_probe(struct i2c_client *ara)
{
	struct i2c_smbus_alert_setup *setup = dev_get_platdata(&ara->dev);
	struct i2c_smbus_alert *alert;
	struct i2c_adapter *adapter = ara->adapter;
	unsigned long irqflags = IRQF_SHARED | IRQF_ONESHOT;
	struct gpio_desc *gpiod;
	int res, irq;

	alert = devm_kzalloc(&ara->dev, sizeof(struct i2c_smbus_alert),
			     GFP_KERNEL);
	if (!alert)
		return -ENOMEM;

	if (setup) {
		irq = setup->irq;
	} else {
		irq = fwnode_irq_get_byname(dev_fwnode(adapter->dev.parent),
					    "smbus_alert");
		if (irq <= 0) {
			gpiod = devm_gpiod_get(adapter->dev.parent, "smbalert", GPIOD_IN);
			if (IS_ERR(gpiod))
				return PTR_ERR(gpiod);

			irq = gpiod_to_irq(gpiod);
			if (irq <= 0)
				return irq;

			irqflags |= IRQF_TRIGGER_FALLING;
		}
	}

	INIT_WORK(&alert->alert, smbalert_work);
	alert->ara = ara;

	if (irq > 0) {
		res = devm_request_threaded_irq(&ara->dev, irq, NULL, smbus_alert,
						irqflags, "smbus_alert", alert);
		if (res)
			return res;
	}

	i2c_set_clientdata(ara, alert);
	dev_info(&adapter->dev, "supports SMBALERT#\n");

	return 0;
}

/* IRQ 和内存资源都交给 devm 管理，卸载时会自动释放。 */
static void smbalert_remove(struct i2c_client *ara)
{
	struct i2c_smbus_alert *alert = i2c_get_clientdata(ara);

	cancel_work_sync(&alert->alert);
}

static const struct i2c_device_id smbalert_ids[] = {
	{ "smbus_alert" },
	{ /* LIST END */ }
};
MODULE_DEVICE_TABLE(i2c, smbalert_ids);

static struct i2c_driver smbalert_driver = {
	.driver = {
		.name	= "smbus_alert",
	},
	.probe		= smbalert_probe,
	.remove		= smbalert_remove,
	.id_table	= smbalert_ids,
};

/**
 * i2c_handle_smbus_alert - 处理一次 SMBus 告警
 * @ara: the ARA client on the relevant adapter
 * Context: can't sleep
 *
 * 供 I2C 控制器驱动的中断处理函数调用。
 * 这里只负责调度告警 work，随后由 work 去回调对应 I2C 设备驱动的 alert()。
 *
 * 这里假定 ara 是之前由 i2c_new_smbus_alert_device() 返回的有效 client。
 */
int i2c_handle_smbus_alert(struct i2c_client *ara)
{
	struct i2c_smbus_alert *alert = i2c_get_clientdata(ara);

	return schedule_work(&alert->alert);
}
EXPORT_SYMBOL_GPL(i2c_handle_smbus_alert);

module_i2c_driver(smbalert_driver);

#if IS_ENABLED(CONFIG_I2C_SLAVE)
#define SMBUS_HOST_NOTIFY_LEN	3
struct i2c_slave_host_notify_status {
	u8 index;
	u8 addr;
};

static int i2c_slave_host_notify_cb(struct i2c_client *client,
				    enum i2c_slave_event event, u8 *val)
{
	struct i2c_slave_host_notify_status *status = client->dev.platform_data;

	switch (event) {
	case I2C_SLAVE_WRITE_RECEIVED:
		/* We only retrieve the first byte received (addr)
		 * since there is currently no support to retrieve the data
		 * parameter from the client.
		 */
		if (status->index == 0)
			status->addr = *val;
		if (status->index < U8_MAX)
			status->index++;
		break;
	case I2C_SLAVE_STOP:
		if (status->index == SMBUS_HOST_NOTIFY_LEN)
			i2c_handle_smbus_host_notify(client->adapter,
						     status->addr);
		fallthrough;
	case I2C_SLAVE_WRITE_REQUESTED:
		status->index = 0;
		break;
	case I2C_SLAVE_READ_REQUESTED:
	case I2C_SLAVE_READ_PROCESSED:
		*val = 0xff;
		break;
	}

	return 0;
}

/**
 * i2c_new_slave_host_notify_device - 创建用于 SMBus Host Notify 的 client
 * @adapter: the target adapter
 * Context: can sleep
 *
 * 在指定 I2C 总线上建立 SMBus host-notify 支持。
 *
 * 这里的实现方式是创建一个 device 和对应回调，用来处理通过
 * SMBus host-notify 地址（0x8）收到的数据。
 *
 * 返回创建好的 client，后续应调用 i2c_free_slave_host_notify_device()
 * 释放；失败时返回 ERR_PTR。
 */
struct i2c_client *i2c_new_slave_host_notify_device(struct i2c_adapter *adapter)
{
	struct i2c_board_info host_notify_board_info = {
		I2C_BOARD_INFO("smbus_host_notify", 0x08),
		.flags  = I2C_CLIENT_SLAVE,
	};
	struct i2c_slave_host_notify_status *status;
	struct i2c_client *client;
	int ret;

	status = kzalloc_obj(struct i2c_slave_host_notify_status);
	if (!status)
		return ERR_PTR(-ENOMEM);

	host_notify_board_info.platform_data = status;

	client = i2c_new_client_device(adapter, &host_notify_board_info);
	if (IS_ERR(client)) {
		kfree(status);
		return client;
	}

	ret = i2c_slave_register(client, i2c_slave_host_notify_cb);
	if (ret) {
		i2c_unregister_device(client);
		kfree(status);
		return ERR_PTR(ret);
	}

	return client;
}
EXPORT_SYMBOL_GPL(i2c_new_slave_host_notify_device);

/**
 * i2c_free_slave_host_notify_device - 释放 SMBus Host Notify client
 * @client: the client to free
 * Context: can sleep
 *
 * 释放通过 i2c_new_slave_host_notify_device() 分配的 i2c_client。
 */
void i2c_free_slave_host_notify_device(struct i2c_client *client)
{
	if (IS_ERR_OR_NULL(client))
		return;

	i2c_slave_unregister(client);
	kfree(client->dev.platform_data);
	i2c_unregister_device(client);
}
EXPORT_SYMBOL_GPL(i2c_free_slave_host_notify_device);
#endif

/*
 * SPD 不是 SMBus 的一部分，但因为目标系统相同，这里一起处理。
 *
 * 自动实例化 SPD 的限制：
 * - 所有已填插槽必须是同一种内存类型
 * - 只支持到 DDR5 的 (LP)DDR 类型
 * - 只适用于 1 到 8 个内存槽位的系统
 */
#if IS_ENABLED(CONFIG_DMI)
static void i2c_register_spd(struct i2c_adapter *adap, bool write_disabled)
{
	int n, slot_count = 0, dimm_count = 0;
	u16 handle;
	u8 common_mem_type = 0x0, mem_type;
	u64 mem_size;
	bool instantiate = true;
	const char *name;

	while ((handle = dmi_memdev_handle(slot_count)) != 0xffff) {
		slot_count++;

		/* Skip empty slots */
		mem_size = dmi_memdev_size(handle);
		if (!mem_size)
			continue;

		/* Skip undefined memory type */
		mem_type = dmi_memdev_type(handle);
		if (mem_type <= 0x02)		/* Invalid, Other, Unknown */
			continue;

		if (!common_mem_type) {
			/* First filled slot */
			common_mem_type = mem_type;
		} else {
			/* Check that all filled slots have the same type */
			if (mem_type != common_mem_type) {
				dev_warn(&adap->dev,
					 "Different memory types mixed, not instantiating SPD\n");
				return;
			}
		}
		dimm_count++;
	}

	/* No useful DMI data, bail out */
	if (!dimm_count)
		return;

	/*
	 * The max number of SPD EEPROMs that can be addressed per bus is 8.
	 * If more slots are present either muxed or multiple busses are
	 * necessary or the additional slots are ignored.
	 */
	slot_count = min(slot_count, 8);

	/*
	 * Memory types could be found at section 7.18.2 (Memory Device — Type), table 78
	 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0134_3.6.0.pdf
	 */
	switch (common_mem_type) {
	case 0x12:	/* DDR */
	case 0x13:	/* DDR2 */
	case 0x18:	/* DDR3 */
	case 0x1B:	/* LPDDR */
	case 0x1C:	/* LPDDR2 */
	case 0x1D:	/* LPDDR3 */
		name = "spd";
		break;
	case 0x1A:	/* DDR4 */
	case 0x1E:	/* LPDDR4 */
		name = "ee1004";
		break;
	case 0x22:	/* DDR5 */
	case 0x23:	/* LPDDR5 */
		name = "spd5118";
		instantiate = !write_disabled;
		break;
	default:
		dev_info(&adap->dev,
			 "Memory type 0x%02x not supported yet, not instantiating SPD\n",
			 common_mem_type);
		return;
	}

	/*
	 * We don't know in which slots the memory modules are. We could
	 * try to guess from the slot names, but that would be rather complex
	 * and unreliable, so better probe all possible addresses until we
	 * have found all memory modules.
	 */
	for (n = 0; n < slot_count && dimm_count; n++) {
		struct i2c_board_info info;
		unsigned short addr_list[2];

		memset(&info, 0, sizeof(struct i2c_board_info));
		strscpy(info.type, name, I2C_NAME_SIZE);
		addr_list[0] = 0x50 + n;
		addr_list[1] = I2C_CLIENT_END;

		if (!instantiate)
			continue;

		if (!IS_ERR(i2c_new_scanned_device(adap, &info, addr_list, NULL))) {
			dev_info(&adap->dev,
				 "Successfully instantiated SPD at 0x%hx\n",
				 addr_list[0]);
			dimm_count--;
		}
	}
}

void i2c_register_spd_write_disable(struct i2c_adapter *adap)
{
	i2c_register_spd(adap, true);
}
EXPORT_SYMBOL_GPL(i2c_register_spd_write_disable);

void i2c_register_spd_write_enable(struct i2c_adapter *adap)
{
	i2c_register_spd(adap, false);
}
EXPORT_SYMBOL_GPL(i2c_register_spd_write_enable);

#endif

MODULE_AUTHOR("Jean Delvare <jdelvare@suse.de>");
MODULE_DESCRIPTION("SMBus protocol extensions support");
MODULE_LICENSE("GPL");
