// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core
 *
 * Copyright (C) 1995-99 Simon G. Vogl
 *   With some changes from Kyösti Mälkki <kmalkki@cc.hut.fi>
 *   Mux support by Rodolfo Giometti <giometti@enneenne.com> and
 *   Michael Lawnick <michael.lawnick.ext@nsn.com>
 *
 * Copyright (C) 2013-2017 Wolfram Sang <wsa@kernel.org>
 */

#define pr_fmt(fmt) "i2c-core: " fmt

#include <dt-bindings/i2c/i2c.h>
#include <linux/acpi.h>
#include <linux/clk/clk-conf.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/string_choices.h>

#include "i2c-core.h"

#define CREATE_TRACE_POINTS
#include <trace/events/i2c.h>

#define I2C_ADDR_OFFSET_TEN_BIT	0xa000
#define I2C_ADDR_OFFSET_SLAVE	0x1000

#define I2C_ADDR_7BITS_MAX	0x77
#define I2C_ADDR_7BITS_COUNT	(I2C_ADDR_7BITS_MAX + 1)

#define I2C_ADDR_DEVICE_ID	0x7c

/*
 * core_lock 保护 i2c_adapter_idr，并保证以下操作串行化：
 * - 适配器编号分配与回收
 * - 设备探测
 * - 通过 detect 创建的设备删除
 */
static DEFINE_MUTEX(core_lock);
static DEFINE_IDR(i2c_adapter_idr);

static int i2c_detect(struct i2c_adapter *adapter, struct i2c_driver *driver);

static DEFINE_STATIC_KEY_FALSE(i2c_trace_msg_key);
static bool is_registered;

static struct dentry *i2c_debugfs_root;

int i2c_transfer_trace_reg(void)
{
	static_branch_inc(&i2c_trace_msg_key);
	return 0;
}

void i2c_transfer_trace_unreg(void)
{
	static_branch_dec(&i2c_trace_msg_key);
}

const char *i2c_freq_mode_string(u32 bus_freq_hz)
{
	switch (bus_freq_hz) {
	case I2C_MAX_STANDARD_MODE_FREQ:
		return "Standard Mode (100 kHz)";
	case I2C_MAX_FAST_MODE_FREQ:
		return "Fast Mode (400 kHz)";
	case I2C_MAX_FAST_MODE_PLUS_FREQ:
		return "Fast Mode Plus (1.0 MHz)";
	case I2C_MAX_TURBO_MODE_FREQ:
		return "Turbo Mode (1.4 MHz)";
	case I2C_MAX_HIGH_SPEED_MODE_FREQ:
		return "High Speed Mode (3.4 MHz)";
	case I2C_MAX_ULTRA_FAST_MODE_FREQ:
		return "Ultra Fast Mode (5.0 MHz)";
	default:
		return "Unknown Mode";
	}
}
EXPORT_SYMBOL_GPL(i2c_freq_mode_string);

const struct i2c_device_id *i2c_match_id(const struct i2c_device_id *id,
						const struct i2c_client *client)
{
	if (!(id && client))
		return NULL;

	while (id->name[0]) {
		if (strcmp(client->name, id->name) == 0)
			return id;
		id++;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(i2c_match_id);

const void *i2c_get_match_data(const struct i2c_client *client)
{
	struct i2c_driver *driver = to_i2c_driver(client->dev.driver);
	const struct i2c_device_id *match;
	const void *data;

	data = device_get_match_data(&client->dev);
	if (!data) {
		match = i2c_match_id(driver->id_table, client);
		if (!match)
			return NULL;

		data = (const void *)match->driver_data;
	}

	return data;
}
EXPORT_SYMBOL(i2c_get_match_data);

static int i2c_device_match(struct device *dev, const struct device_driver *drv)
{
	struct i2c_client	*client = i2c_verify_client(dev);
	const struct i2c_driver	*driver;

	/*
	 * 匹配顺序：
	 * 1. 设备树
	 * 2. ACPI
	 * 3. I2C id_table
	 */
	if (i2c_of_match_device(drv->of_match_table, client))
		return 1;

	if (acpi_driver_match_device(dev, drv))
		return 1;

	driver = to_i2c_driver(drv);

	if (i2c_match_id(driver->id_table, client))
		return 1;

	return 0;
}

static int i2c_device_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct i2c_client *client = to_i2c_client(dev);
	int rc;

	/* 先输出 OF/ACPI modalias；都没有时再回退到 I2C 字符串。 */
	rc = of_device_uevent_modalias(dev, env);
	if (rc != -ENODEV)
		return rc;

	rc = acpi_device_uevent_modalias(dev, env);
	if (rc != -ENODEV)
		return rc;

	return add_uevent_var(env, "MODALIAS=%s%s", I2C_MODULE_PREFIX, client->name);
}

/*
 * I2C 总线恢复逻辑。
 *
 * 这里负责处理 SDA/SCL 卡死、时钟丢失等场景，尽量通过 GPIO 或 pinctrl
 * 把总线恢复到可用状态。
 */
static int get_scl_gpio_value(struct i2c_adapter *adap)
{
	return gpiod_get_value_cansleep(adap->bus_recovery_info->scl_gpiod);
}

static void set_scl_gpio_value(struct i2c_adapter *adap, int val)
{
	gpiod_set_value_cansleep(adap->bus_recovery_info->scl_gpiod, val);
}

static int get_sda_gpio_value(struct i2c_adapter *adap)
{
	return gpiod_get_value_cansleep(adap->bus_recovery_info->sda_gpiod);
}

static void set_sda_gpio_value(struct i2c_adapter *adap, int val)
{
	gpiod_set_value_cansleep(adap->bus_recovery_info->sda_gpiod, val);
}

static int i2c_generic_bus_free(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	int ret = -EOPNOTSUPP;

	if (bri->get_bus_free)
		ret = bri->get_bus_free(adap);
	else if (bri->get_sda)
		ret = bri->get_sda(adap);

	if (ret < 0)
		return ret;

	return ret ? 0 : -EBUSY;
}

/*
 * 这里是在手动生成时钟脉冲，ndelay() 决定每个时钟电平的持续时间。
 * 我们按 100 KHz 目标来生成时钟，因此两个电平的持续时间都约为 5 us。
 */
#define RECOVERY_NDELAY		5000
#define RECOVERY_CLK_CNT	9

int i2c_generic_scl_recovery(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	int i = 0, scl = 1, ret = 0;

	if (bri->prepare_recovery)
		bri->prepare_recovery(adap);
	if (bri->pinctrl)
		pinctrl_select_state(bri->pinctrl, bri->pins_gpio);

	/*
	 * 如果可以控制 SDA，就主动构造 STOP 条件，避免后续恢复脉冲
	 * 继续干扰从设备状态。
	 */
	bri->set_scl(adap, scl);
	ndelay(RECOVERY_NDELAY);
	if (bri->set_sda)
		bri->set_sda(adap, scl);
	ndelay(RECOVERY_NDELAY / 2);

	/*
	 * By this time SCL is high, as we need to give 9 falling-rising edges
	 */
	while (i++ < RECOVERY_CLK_CNT * 2) {
		if (scl) {
				/* 这里 SCL 不应该还是低电平。 */
			if (!bri->get_scl(adap)) {
				dev_err(&adap->dev,
					"SCL is stuck low, exit recovery\n");
				ret = -EBUSY;
				break;
			}
		}

		scl = !scl;
		bri->set_scl(adap, scl);
			/* 再次构造 STOP，避免恢复过程破坏设备状态。 */
		if (scl)  {
			/* Honour minimum tsu:sto */
			ndelay(RECOVERY_NDELAY);
		} else {
			/* Honour minimum tf and thd:dat */
			ndelay(RECOVERY_NDELAY / 2);
		}
		if (bri->set_sda)
			bri->set_sda(adap, scl);
		ndelay(RECOVERY_NDELAY / 2);

		if (scl) {
			ret = i2c_generic_bus_free(adap);
			if (ret == 0)
				break;
		}
	}

	/* 如果无法检查总线状态，就默认恢复成功。 */
	if (ret == -EOPNOTSUPP)
		ret = 0;

	if (bri->unprepare_recovery)
		bri->unprepare_recovery(adap);
	if (bri->pinctrl)
		pinctrl_select_state(bri->pinctrl, bri->pins_default);

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_generic_scl_recovery);

int i2c_recover_bus(struct i2c_adapter *adap)
{
	if (!adap->bus_recovery_info)
		return -EBUSY;

	dev_dbg(&adap->dev, "Trying i2c bus recovery\n");
	return adap->bus_recovery_info->recover_bus(adap);
}
EXPORT_SYMBOL_GPL(i2c_recover_bus);

static void i2c_gpio_init_pinctrl_recovery(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	struct device *dev = &adap->dev;
	struct pinctrl *p = bri->pinctrl ?: dev_pinctrl(dev->parent);

	bri->pinctrl = p;

		/* 没有 pinctrl 就无法切换状态，直接清空恢复配置。 */
	if (!p) {
		bri->pins_default = NULL;
		bri->pins_gpio = NULL;
		return;
	}

	if (!bri->pins_default) {
		bri->pins_default = pinctrl_lookup_state(p,
							 PINCTRL_STATE_DEFAULT);
		if (IS_ERR(bri->pins_default)) {
			dev_dbg(dev, PINCTRL_STATE_DEFAULT " state not found for GPIO recovery\n");
			bri->pins_default = NULL;
		}
	}
	if (!bri->pins_gpio) {
		bri->pins_gpio = pinctrl_lookup_state(p, "gpio");
		if (IS_ERR(bri->pins_gpio))
			bri->pins_gpio = pinctrl_lookup_state(p, "recovery");

		if (IS_ERR(bri->pins_gpio)) {
			dev_dbg(dev, "no gpio or recovery state found for GPIO recovery\n");
			bri->pins_gpio = NULL;
		}
	}

	/* 只有 default/gpio 两个状态都存在，才算完整支持 pinctrl 恢复。 */
	if (bri->pins_default && bri->pins_gpio) {
		dev_info(dev, "using pinctrl states for GPIO recovery");
	} else {
		bri->pinctrl = NULL;
		bri->pins_default = NULL;
		bri->pins_gpio = NULL;
	}
}

static int i2c_gpio_init_generic_recovery(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	struct device *dev = &adap->dev;
	struct gpio_desc *gpiod;
	int ret = 0;

		/* 不是通用 SCL 恢复路径时，不要修改已有恢复信息。 */
	if (bri->recover_bus && bri->recover_bus != i2c_generic_scl_recovery)
		return 0;

		/* 引脚要改作 GPIO 使用时，先切到 gpio 状态。 */
	if (bri->pinctrl)
		pinctrl_select_state(bri->pinctrl, bri->pins_gpio);

		/* 恢复信息不完整时，尝试自动获取通用 GPIO 恢复引脚。 */
	if (!bri->scl_gpiod) {
		gpiod = devm_gpiod_get(dev, "scl", GPIOD_OUT_HIGH_OPEN_DRAIN);
		if (PTR_ERR(gpiod) == -EPROBE_DEFER) {
			ret  = -EPROBE_DEFER;
			goto cleanup_pinctrl_state;
		}
		if (!IS_ERR(gpiod)) {
			bri->scl_gpiod = gpiod;
			bri->recover_bus = i2c_generic_scl_recovery;
			dev_info(dev, "using generic GPIOs for recovery\n");
		}
	}

	/* SDA GPIOD line is optional, so we care about DEFER only */
	if (!bri->sda_gpiod) {
		/*
		 * 已经拿到 SCL 后先把它拉低，再等一小会儿，避免 SDA 毛刺
		 * 对后续恢复动作造成影响。
		 */
		gpiod_direction_output(bri->scl_gpiod, 0);
		udelay(10);
		gpiod = devm_gpiod_get(dev, "sda", GPIOD_IN);

		/* 再等一会儿防止 SDA 突发毛刺，然后释放 SCL。 */
		udelay(10);
		gpiod_direction_output(bri->scl_gpiod, 1);

		if (PTR_ERR(gpiod) == -EPROBE_DEFER) {
			ret = -EPROBE_DEFER;
			goto cleanup_pinctrl_state;
		}
		if (!IS_ERR(gpiod))
			bri->sda_gpiod = gpiod;
	}

cleanup_pinctrl_state:
	/* 最后把引脚状态切回默认状态。 */
	if (bri->pinctrl)
		pinctrl_select_state(bri->pinctrl, bri->pins_default);

	return ret;
}

static int i2c_gpio_init_recovery(struct i2c_adapter *adap)
{
	i2c_gpio_init_pinctrl_recovery(adap);
	return i2c_gpio_init_generic_recovery(adap);
}

static int i2c_init_recovery(struct i2c_adapter *adap)
{
	struct i2c_bus_recovery_info *bri = adap->bus_recovery_info;
	bool is_error_level = true;
	char *err_str;

	if (!bri)
		return 0;

	if (i2c_gpio_init_recovery(adap) == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (!bri->recover_bus) {
		err_str = "no suitable method provided";
		is_error_level = false;
		goto err;
	}

	if (bri->scl_gpiod && bri->recover_bus == i2c_generic_scl_recovery) {
		bri->get_scl = get_scl_gpio_value;
		bri->set_scl = set_scl_gpio_value;
		if (bri->sda_gpiod) {
			bri->get_sda = get_sda_gpio_value;
			if (gpiod_get_direction(bri->sda_gpiod) == GPIO_LINE_DIRECTION_OUT)
				bri->set_sda = set_sda_gpio_value;
		}
	} else if (bri->recover_bus == i2c_generic_scl_recovery) {
		/* 通用 SCL 恢复。 */
		if (!bri->set_scl || !bri->get_scl) {
			err_str = "no {get|set}_scl() found";
			goto err;
		}
		if (!bri->set_sda && !bri->get_sda) {
			err_str = "either get_sda() or set_sda() needed";
			goto err;
		}
	}

	return 0;
 err:
	if (is_error_level)
		dev_err(&adap->dev, "Not using recovery: %s\n", err_str);
	else
		dev_dbg(&adap->dev, "Not using recovery: %s\n", err_str);
	adap->bus_recovery_info = NULL;

	return -EINVAL;
}

static int i2c_smbus_host_notify_to_irq(const struct i2c_client *client)
{
	struct i2c_adapter *adap = client->adapter;
	unsigned int irq;

	if (!adap->host_notify_domain)
		return -ENXIO;

	if (client->flags & I2C_CLIENT_TEN)
		return -EINVAL;

	irq = irq_create_mapping(adap->host_notify_domain, client->addr);

	return irq > 0 ? irq : -ENXIO;
}

static int i2c_device_probe(struct device *dev)
{
	struct fwnode_handle	*fwnode = dev_fwnode(dev);
	struct i2c_client	*client = i2c_verify_client(dev);
	struct i2c_driver	*driver;
	bool do_power_on;
	int status;

	if (!client)
		return 0;

	client->irq = client->init_irq;

	if (!client->irq) {
		int irq = -ENOENT;

		if (client->flags & I2C_CLIENT_HOST_NOTIFY) {
			dev_dbg(dev, "Using Host Notify IRQ\n");
			/* Host Notify 需要适配器保持激活，否则中断域会失效。 */
			pm_runtime_get_sync(&client->adapter->dev);
			irq = i2c_smbus_host_notify_to_irq(client);
		} else if (is_of_node(fwnode)) {
			irq = fwnode_irq_get_byname(fwnode, "irq");
			if (irq == -EINVAL || irq == -ENODATA)
				irq = fwnode_irq_get(fwnode, 0);
		} else if (is_acpi_device_node(fwnode)) {
			bool wake_capable;

			irq = i2c_acpi_get_irq(client, &wake_capable);
			if (irq > 0 && wake_capable)
				client->flags |= I2C_CLIENT_WAKE;
		}
		if (irq == -EPROBE_DEFER) {
			status = dev_err_probe(dev, irq, "can't get irq\n");
			goto put_sync_adapter;
		}

		if (irq < 0)
			irq = 0;

		client->irq = irq;
	}

	driver = to_i2c_driver(dev->driver);

	/* 设备树或 ACPI 已提供匹配信息时，id_table 不是强制要求。 */
	if (!driver->id_table &&
	    !acpi_driver_match_device(dev, dev->driver) &&
	    !i2c_of_match_device(dev->driver->of_match_table, client)) {
		status = -ENODEV;
		goto put_sync_adapter;
	}

	if (client->flags & I2C_CLIENT_WAKE) {
		int wakeirq;

		wakeirq = fwnode_irq_get_byname(fwnode, "wakeup");
		if (wakeirq == -EPROBE_DEFER) {
			status = dev_err_probe(dev, wakeirq, "can't get wakeirq\n");
			goto put_sync_adapter;
		}

		device_init_wakeup(&client->dev, true);

		if (wakeirq > 0 && wakeirq != client->irq)
			status = dev_pm_set_dedicated_wake_irq(dev, wakeirq);
		else if (client->irq > 0)
			status = dev_pm_set_wake_irq(dev, client->irq);
		else
			status = 0;

		if (status)
			dev_warn(&client->dev, "failed to set up wakeup irq\n");
	}

	dev_dbg(dev, "probe\n");

	status = of_clk_set_defaults(to_of_node(fwnode), false);
	if (status < 0)
		goto err_clear_wakeup_irq;

	do_power_on = !i2c_acpi_waive_d0_probe(dev);
	status = dev_pm_domain_attach(&client->dev, PD_FLAG_DETACH_POWER_OFF |
				      (do_power_on ? PD_FLAG_ATTACH_POWER_ON : 0));
	if (status)
		goto err_clear_wakeup_irq;

	client->devres_group_id = devres_open_group(&client->dev, NULL,
						    GFP_KERNEL);
	if (!client->devres_group_id) {
		status = -ENOMEM;
		goto err_clear_wakeup_irq;
	}

	client->debugfs = debugfs_create_dir(dev_name(&client->dev),
					     client->adapter->debugfs);

	if (driver->probe)
		status = driver->probe(client);
	else
		status = -EINVAL;

	/*
	 * 这里故意不关闭 devres group。
	 * 这样 probe 期间和 probe 之后追加的资源都会在 remove 时统一释放，
	 * 适合固件更新等“先 probe，后补资源”的驱动。
	 */

	if (status)
		goto err_release_driver_resources;

	return 0;

err_release_driver_resources:
	debugfs_remove_recursive(client->debugfs);
	devres_release_group(&client->dev, client->devres_group_id);
err_clear_wakeup_irq:
	dev_pm_clear_wake_irq(&client->dev);
	device_init_wakeup(&client->dev, false);
put_sync_adapter:
	if (client->flags & I2C_CLIENT_HOST_NOTIFY)
		pm_runtime_put_sync(&client->adapter->dev);

	return status;
}

static void i2c_device_remove(struct device *dev)
{
	struct i2c_client	*client = to_i2c_client(dev);
	struct i2c_driver	*driver;

	driver = to_i2c_driver(dev->driver);
	if (driver->remove) {
		dev_dbg(dev, "remove\n");

		driver->remove(client);
	}

	debugfs_remove_recursive(client->debugfs);

	devres_release_group(&client->dev, client->devres_group_id);

	dev_pm_clear_wake_irq(&client->dev);
	device_init_wakeup(&client->dev, false);

		/* 设备卸载后，IRQ 和 Host Notify 相关状态都要清理。 */
		client->irq = 0;
	if (client->flags & I2C_CLIENT_HOST_NOTIFY)
		pm_runtime_put(&client->adapter->dev);
}

static void i2c_device_shutdown(struct device *dev)
{
	struct i2c_client *client = i2c_verify_client(dev);
	struct i2c_driver *driver;

	if (!client || !dev->driver)
		return;
	driver = to_i2c_driver(dev->driver);
	if (driver->shutdown)
		driver->shutdown(client);
	else if (client->irq > 0)
		disable_irq(client->irq);
}

static void i2c_client_dev_release(struct device *dev)
{
	kfree(to_i2c_client(dev));
}

static ssize_t
name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", dev->type == &i2c_client_type ?
		       to_i2c_client(dev)->name : to_i2c_adapter(dev)->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t
modalias_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	int len;

	len = of_device_modalias(dev, buf, PAGE_SIZE);
	if (len != -ENODEV)
		return len;

	len = acpi_device_modalias(dev, buf, PAGE_SIZE - 1);
	if (len != -ENODEV)
		return len;

	return sprintf(buf, "%s%s\n", I2C_MODULE_PREFIX, client->name);
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *i2c_dev_attrs[] = {
	&dev_attr_name.attr,
	/* modalias 方便冷插拔：用户空间可以直接据此 modprobe 对应驱动。 */
	&dev_attr_modalias.attr,
	NULL
};
ATTRIBUTE_GROUPS(i2c_dev);

const struct bus_type i2c_bus_type = {
	.name		= "i2c",
	.match		= i2c_device_match,
	.probe		= i2c_device_probe,
	.remove		= i2c_device_remove,
	.shutdown	= i2c_device_shutdown,
};
EXPORT_SYMBOL_GPL(i2c_bus_type);

const struct device_type i2c_client_type = {
	.groups		= i2c_dev_groups,
	.uevent		= i2c_device_uevent,
	.release	= i2c_client_dev_release,
};
EXPORT_SYMBOL_GPL(i2c_client_type);


/**
 * i2c_verify_client - 将参数验证为 i2c_client，否则返回 NULL
 * @dev: device, probably from some driver model iterator
 *
 * 在遍历 driver model 树时，不能假设遇到的节点一定是 I2C 设备。
 * 这个 helper 用来避免把非 I2C 设备误当成 i2c_client。
 */
struct i2c_client *i2c_verify_client(struct device *dev)
{
	return (dev->type == &i2c_client_type)
			? to_i2c_client(dev)
			: NULL;
}
EXPORT_SYMBOL(i2c_verify_client);


/* 生成一个唯一地址，并把 client 的 flags 也编码进去，避免冲突。 */
static unsigned short i2c_encode_flags_to_addr(struct i2c_client *client)
{
	unsigned short addr = client->addr;

	/* 某些 client flags 需要额外偏移，以避免地址碰撞。 */
	if (client->flags & I2C_CLIENT_TEN)
		addr |= I2C_ADDR_OFFSET_TEN_BIT;

	if (client->flags & I2C_CLIENT_SLAVE)
		addr |= I2C_ADDR_OFFSET_SLAVE;

	return addr;
}

/*
 * 这是一个宽松的地址合法性检查。
 * 除了通用调用地址外，不强制执行完整的 I2C 地址映射约束。
 */
static int i2c_check_addr_validity(unsigned int addr, unsigned short flags)
{
	if (flags & I2C_CLIENT_TEN) {
		/* 10 位地址，所有值都视为合法范围内。 */
		if (addr > 0x3ff)
			return -EINVAL;
	} else {
		/* 7 位地址，拒绝通用调用地址。 */
		if (addr == 0x00 || addr > 0x7f)
			return -EINVAL;
	}
	return 0;
}

/*
 * 这是一个严格的地址合法性检查，主要用于 probe。
 * 如果设备使用保留地址，就不应该被扫描。
 * 这里默认按 7 位地址处理，10 位地址设备应当显式枚举。
 */
int i2c_check_7bit_addr_validity_strict(unsigned short addr)
{
	/*
	 * I2C 规范里的保留地址：
	 *  0x00       通用调用地址 / START byte
	 *  0x01       CBUS 地址
	 *  0x02       为不同总线格式保留
	 *  0x03       预留给未来用途
	 *  0x04-0x07  高速模式 master code
	 *  0x78-0x7b  10 位从设备地址
	 *  0x7c-0x7f  预留给未来用途
	 */
	if (addr < 0x08 || addr > 0x77)
		return -EINVAL;
	return 0;
}

static int __i2c_check_addr_busy(struct device *dev, void *addrp)
{
	struct i2c_client	*client = i2c_verify_client(dev);
	int			addr = *(int *)addrp;

	if (client && i2c_encode_flags_to_addr(client) == addr)
		return -EBUSY;
	return 0;
}

/* 向上遍历 mux 树。 */
static int i2c_check_mux_parents(struct i2c_adapter *adapter, int addr)
{
	struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(adapter);
	int result;

	result = device_for_each_child(&adapter->dev, &addr,
					__i2c_check_addr_busy);

	if (!result && parent)
		result = i2c_check_mux_parents(parent, addr);

	return result;
}

/* 向下递归遍历 mux 树。 */
static int i2c_check_mux_children(struct device *dev, void *addrp)
{
	int result;

	if (dev->type == &i2c_adapter_type)
		result = device_for_each_child(dev, addrp,
						i2c_check_mux_children);
	else
		result = __i2c_check_addr_busy(dev, addrp);

	return result;
}

static int i2c_check_addr_busy(struct i2c_adapter *adapter, int addr)
{
	struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(adapter);
	int result = 0;

	if (parent)
		result = i2c_check_mux_parents(parent, addr);

	if (!result)
		result = device_for_each_child(&adapter->dev, &addr,
						i2c_check_mux_children);

	return result;
}

/**
 * i2c_adapter_lock_bus - 获取某个 I2C 总线段的独占访问权
 * @adapter: 目标 I2C 总线段
 * @flags:   I2C_LOCK_ROOT_ADAPTER 表示锁住根适配器，
 *	     I2C_LOCK_SEGMENT 表示只锁当前拓扑分支
 */
static void i2c_adapter_lock_bus(struct i2c_adapter *adapter,
				 unsigned int flags)
{
	rt_mutex_lock_nested(&adapter->bus_lock, i2c_adapter_depth(adapter));
}

/**
 * i2c_adapter_trylock_bus - 尝试获取某个 I2C 总线段的独占访问权
 * @adapter: 目标 I2C 总线段
 * @flags:   I2C_LOCK_ROOT_ADAPTER 表示尝试锁住根适配器，
 *	     I2C_LOCK_SEGMENT 表示只尝试锁当前拓扑分支
 */
static int i2c_adapter_trylock_bus(struct i2c_adapter *adapter,
				   unsigned int flags)
{
	return rt_mutex_trylock(&adapter->bus_lock);
}

/**
 * i2c_adapter_unlock_bus - 释放某个 I2C 总线段的独占访问权
 * @adapter: 目标 I2C 总线段
 * @flags:   I2C_LOCK_ROOT_ADAPTER 表示解锁根适配器，
 *	     I2C_LOCK_SEGMENT 表示只解锁当前拓扑分支
 */
static void i2c_adapter_unlock_bus(struct i2c_adapter *adapter,
				   unsigned int flags)
{
	rt_mutex_unlock(&adapter->bus_lock);
}

static void i2c_dev_set_name(struct i2c_adapter *adap,
			     struct i2c_client *client,
			     struct i2c_board_info const *info)
{
	struct acpi_device *adev = ACPI_COMPANION(&client->dev);

	if (info && info->dev_name) {
		dev_set_name(&client->dev, "i2c-%s", info->dev_name);
		return;
	}

	if (adev) {
		dev_set_name(&client->dev, "i2c-%s", acpi_dev_name(adev));
		return;
	}

	dev_set_name(&client->dev, "%d-%04x", i2c_adapter_id(adap),
		     i2c_encode_flags_to_addr(client));
}

int i2c_dev_irq_from_resources(const struct resource *resources,
			       unsigned int num_resources)
{
	struct irq_data *irqd;
	int i;

	for (i = 0; i < num_resources; i++) {
		const struct resource *r = &resources[i];

		if (resource_type(r) != IORESOURCE_IRQ)
			continue;

		if (r->flags & IORESOURCE_BITS) {
			irqd = irq_get_irq_data(r->start);
			if (!irqd)
				break;

			irqd_set_trigger_type(irqd, r->flags & IORESOURCE_BITS);
		}

		return r->start;
	}

	return 0;
}

/*
 * 设备实例化需要串行化，
 * 因为同一个地址可能既能被显式创建，也能被自动探测创建。
 */
static int i2c_lock_addr(struct i2c_adapter *adap, unsigned short addr,
			 unsigned short flags)
{
	if (!(flags & I2C_CLIENT_TEN) &&
	    test_and_set_bit(addr, adap->addrs_in_instantiation))
		return -EBUSY;

	return 0;
}

static void i2c_unlock_addr(struct i2c_adapter *adap, unsigned short addr,
			    unsigned short flags)
{
	if (!(flags & I2C_CLIENT_TEN))
		clear_bit(addr, adap->addrs_in_instantiation);
}

/**
 * i2c_new_client_device - 实例化一个 I2C 设备
 * @adap: 管理该设备的适配器
 * @info: 描述一个 I2C 设备；其中 bus_num 会被忽略
 * Context: can sleep
 *
 * 这是 I2C core 里最基础的“显式创建设备实例”入口。它负责把
 * `struct i2c_board_info` 转成真正的 `struct i2c_client` 和
 * `struct device`，再交给 driver core 去做匹配与 probe。
 *
 * 设备绑定由 driver model 接管：匹配成功后会进入 probe/remove 生命周期。
 * 在这个函数返回时，驱动可能已经绑定到设备，也可能稍后才绑定
 * （例如热插拔会在之后加载驱动模块）。
 *
 * 这个接口不适合板级初始化代码使用，因为板级初始化通常发生在
 * arch_initcall() 阶段，那个时候 I2C 适配器还可能根本不存在。
 *
 * 内部关键步骤包括：
 * - 校验地址是否合法
 * - 用 addrs_in_instantiation 串行化同地址实例化
 * - 检查总线上该地址是否已被占用
 * - 挂接 fwnode / software node / IRQ 等设备描述
 * - 注册 struct device，让后续匹配进入 driver core
 *
 * 返回新的 i2c client，后续可交给 i2c_unregister_device() 释放；
 * 如果出错，则返回 ERR_PTR。
 */
struct i2c_client *
i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info)
{
	struct fwnode_handle *fwnode = info->fwnode;
	struct i2c_client *client;
	bool need_put = false;
	int status;

	client = kzalloc_obj(*client);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->adapter = adap;

	client->dev.platform_data = info->platform_data;
	client->flags = info->flags;
	client->addr = info->addr;

	client->init_irq = info->irq;
	if (!client->init_irq)
		client->init_irq = i2c_dev_irq_from_resources(info->resources,
							 info->num_resources);

	strscpy(client->name, info->type, sizeof(client->name));

	status = i2c_check_addr_validity(client->addr, client->flags);
	if (status) {
		dev_err(&adap->dev, "Invalid %d-bit I2C address 0x%02hx\n",
			client->flags & I2C_CLIENT_TEN ? 10 : 7, client->addr);
		goto out_err_silent;
	}

	status = i2c_lock_addr(adap, client->addr, client->flags);
	if (status)
		goto out_err_silent;

	/* 再检查这条总线上是否已经有别的设备占用了同一地址。 */
	status = i2c_check_addr_busy(adap, i2c_encode_flags_to_addr(client));
	if (status)
		goto out_err;

	client->dev.parent = &client->adapter->dev;
	client->dev.bus = &i2c_bus_type;
	client->dev.type = &i2c_client_type;

	device_enable_async_suspend(&client->dev);

	device_set_node(&client->dev, fwnode_handle_get(fwnode));

	if (info->swnode) {
		status = device_add_software_node(&client->dev, info->swnode);
		if (status) {
			dev_err(&adap->dev,
				"Failed to add software node to client %s: %d\n",
				client->name, status);
			goto out_err_put_fwnode;
		}
	}

	i2c_dev_set_name(adap, client, info);
	status = device_register(&client->dev);
	if (status)
		goto out_remove_swnode;

	dev_dbg(&adap->dev, "client [%s] registered with bus id %s\n",
		client->name, dev_name(&client->dev));

	i2c_unlock_addr(adap, client->addr, client->flags);

	return client;

out_remove_swnode:
	device_remove_software_node(&client->dev);
	need_put = true;
out_err_put_fwnode:
	fwnode_handle_put(fwnode);
out_err:
	dev_err(&adap->dev,
		"Failed to register i2c client %s at 0x%02x (%d)\n",
		client->name, client->addr, status);
	i2c_unlock_addr(adap, client->addr, client->flags);
out_err_silent:
	if (need_put)
		put_device(&client->dev);
	else
		kfree(client);
	return ERR_PTR(status);
}
EXPORT_SYMBOL_GPL(i2c_new_client_device);

/**
 * i2c_unregister_device - 撤销 i2c_new_*_device() 创建的设备
 * @client: 由 i2c_new_*_device() 返回的 client
 * Context: can sleep
 *
 * 这个函数负责把一个显式实例化出来的 I2C 设备从 driver model 中移除。
 * 除了普通的 device_unregister()，它还要顺便清理 OF/ACPI/software node
 * 侧的“已枚举”状态，否则同一个固件节点后续可能无法再次被实例化。
 */
void i2c_unregister_device(struct i2c_client *client)
{
	struct fwnode_handle *fwnode;

	if (IS_ERR_OR_NULL(client))
		return;

	fwnode = dev_fwnode(&client->dev);
	if (is_of_node(fwnode))
		of_node_clear_flag(to_of_node(fwnode), OF_POPULATED);
	else if (is_acpi_device_node(fwnode))
		acpi_device_clear_enumerated(to_acpi_device_node(fwnode));

	/*
	 * 如果主 fwnode 本身就是 software node，那么后面的
	 * device_remove_software_node() 会负责释放它，这里不能再 put 一次。
	 */
	if (!is_software_node(fwnode))
		fwnode_handle_put(fwnode);

	device_remove_software_node(&client->dev);
	device_unregister(&client->dev);
}
EXPORT_SYMBOL_GPL(i2c_unregister_device);

/**
 * i2c_find_device_by_fwnode() - 根据 fwnode 查找对应的 i2c_client
 * @fwnode: 与目标 &struct i2c_client 对应的 &struct fwnode_handle
 *
 * 查找并返回与 @fwnode 对应的 &struct i2c_client。
 * 如果找不到 client，或者 @fwnode 为空，则返回 NULL。
 *
 * 使用完后，调用者必须执行 put_device(&client->dev)。
 */
struct i2c_client *i2c_find_device_by_fwnode(struct fwnode_handle *fwnode)
{
	struct i2c_client *client;
	struct device *dev;

	if (IS_ERR_OR_NULL(fwnode))
		return NULL;

	dev = bus_find_device_by_fwnode(&i2c_bus_type, fwnode);
	if (!dev)
		return NULL;

	client = i2c_verify_client(dev);
	if (!client)
		put_device(dev);

	return client;
}
EXPORT_SYMBOL(i2c_find_device_by_fwnode);


static const struct i2c_device_id dummy_id[] = {
	{ "dummy", },
	{ "smbus_host_notify", },
	{ }
};

static int dummy_probe(struct i2c_client *client)
{
	return 0;
}

static struct i2c_driver dummy_driver = {
	.driver.name	= "dummy",
	.probe		= dummy_probe,
	.id_table	= dummy_id,
};

/**
 * i2c_new_dummy_device - 创建一个绑定到 dummy driver 的 I2C 设备
 * @adapter: 管理该设备的适配器
 * @address: 要使用的 7 位地址
 * Context: can sleep
 *
 * 这个 dummy client 常用于多地址芯片的“附属地址”访问：
 *
 * - 给 SMBus/I2C API 提供一个可用的 client 句柄
 * - 阻止其他驱动抢占这个地址
 *
 * 它适合 EEPROM、codec 等会占用多个地址的芯片。
 *
 * 返回一个绑定到 "dummy" 驱动的 I2C client，后续可交给
 * i2c_unregister_device() 释放；如果出错，则返回 ERR_PTR。
 */
struct i2c_client *i2c_new_dummy_device(struct i2c_adapter *adapter, u16 address)
{
	struct i2c_board_info info = {
		I2C_BOARD_INFO("dummy", address),
	};

	return i2c_new_client_device(adapter, &info);
}
EXPORT_SYMBOL_GPL(i2c_new_dummy_device);

static void devm_i2c_release_dummy(void *client)
{
	i2c_unregister_device(client);
}

/**
 * devm_i2c_new_dummy_device - i2c_new_dummy_device() 的受管理版本
 * @dev: 该受管理资源绑定到的设备
 * @adapter: 管理该设备的适配器
 * @address: 要使用的 7 位地址
 * Context: can sleep
 *
 * 这是 @i2c_new_dummy_device 的设备管理版本。成功时返回新的
 * i2c client，失败时返回 ERR_PTR。
 */
struct i2c_client *devm_i2c_new_dummy_device(struct device *dev,
					     struct i2c_adapter *adapter,
					     u16 address)
{
	struct i2c_client *client;
	int ret;

	client = i2c_new_dummy_device(adapter, address);
	if (IS_ERR(client))
		return client;

	ret = devm_add_action_or_reset(dev, devm_i2c_release_dummy, client);
	if (ret)
		return ERR_PTR(ret);

	return client;
}
EXPORT_SYMBOL_GPL(devm_i2c_new_dummy_device);

/**
 * i2c_new_ancillary_device - 根据从属名称创建附属 I2C 设备
 *
 * 这个 helper 用来处理“一个物理芯片占用多个 I2C 地址”的场景。
 * 主驱动通常先绑定主地址，然后再通过名字查找固件里声明的次级地址，
 * 为这些附属地址创建 dummy client，后续即可借助标准 SMBus/I2C API
 * 去访问它们。
 *
 * 这个 helper 会从固件描述里查找 secondary address，再创建对应的 dummy
 * client。常见于一个芯片内部拆成多个 I2C slave 的场景。
 * @client: 主 client
 * @name: 用来指定要获取哪个 secondary address 的名称
 * @default_addr: 如果固件里没有指定 secondary address，就使用这个值
 * Context: can sleep
 *
 * 一个 I2C client 可能由多个 I2C slave 共同组成一个单元。此时
 * I2C client 驱动先绑定到主 I2C slave，再创建 I2C dummy client，
 * 用来和其它 slave 通信。
 *
 * 这个函数会创建并返回一个 I2C dummy client，它的 I2C 地址来源于
 * 平台固件中与给定 slave 名称匹配的条目；如果固件没有提供地址，
 * 就使用 default_addr。
 *
 * 在基于 DT 的平台上，地址会从 "reg" 属性中读取，而对应的
 * "reg-names" 值必须和 slave 名称匹配。
 *
 * 返回新的 i2c client，后续可交给 i2c_unregister_device() 释放；
 * 如果出错，则返回 ERR_PTR。
 */
struct i2c_client *i2c_new_ancillary_device(struct i2c_client *client,
						const char *name,
						u16 default_addr)
{
	struct device_node *np = client->dev.of_node;
	u32 addr = default_addr;
	int i;

	i = of_property_match_string(np, "reg-names", name);
	if (i >= 0)
		of_property_read_u32_index(np, "reg", i, &addr);

	dev_dbg(&client->adapter->dev, "Address for %s : 0x%x\n", name, addr);
	return i2c_new_dummy_device(client->adapter, addr);
}
EXPORT_SYMBOL_GPL(i2c_new_ancillary_device);

/* ------------------------------------------------------------------------- */

/* I2C 适配器管理：每个 adapter 对应一段独立的 I2C/SMBus 总线。 */

static void i2c_adapter_dev_release(struct device *dev)
{
	struct i2c_adapter *adap = to_i2c_adapter(dev);
	complete(&adap->dev_released);
}

unsigned int i2c_adapter_depth(struct i2c_adapter *adapter)
{
	unsigned int depth = 0;
	struct device *parent;

	for (parent = adapter->dev.parent; parent; parent = parent->parent)
		if (parent->type == &i2c_adapter_type)
			depth++;

	WARN_ONCE(depth >= MAX_LOCKDEP_SUBCLASSES,
		  "adapter depth exceeds lockdep subclass limit\n");

	return depth;
}
EXPORT_SYMBOL_GPL(i2c_adapter_depth);

/*
 * 允许用户通过 sysfs 手工实例化 I2C 设备。
 *
 * 这种方式适合平台初始化代码没有提供正确设备信息的场景，
 * 也适合那些依赖探测逻辑、但自动探测失败的驱动。失败原因
 * 可能是设备地址与预期不一致，也可能是兼容器件使用了不同的
 * ID 寄存器值。
 *
 * 这里的参数检查看起来比较严格，但这是有意为之，避免用户
 * 传入错误参数导致创建出错误的设备实例。
 */
static ssize_t
new_device_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	struct i2c_adapter *adap = to_i2c_adapter(dev);
	struct i2c_board_info info;
	struct i2c_client *client;
	char *blank, end;
	int res;

	memset(&info, 0, sizeof(struct i2c_board_info));

	blank = strchr(buf, ' ');
	if (!blank) {
		dev_err(dev, "%s: Missing parameters\n", "new_device");
		return -EINVAL;
	}
	if (blank - buf > I2C_NAME_SIZE - 1) {
		dev_err(dev, "%s: Invalid device name\n", "new_device");
		return -EINVAL;
	}
	memcpy(info.type, buf, blank - buf);

	/* 解析剩余参数，并拒绝多余字段。 */
	res = sscanf(++blank, "%hi%c", &info.addr, &end);
	if (res < 1) {
		dev_err(dev, "%s: Can't parse I2C address\n", "new_device");
		return -EINVAL;
	}
	if (res > 1  && end != '\n') {
		dev_err(dev, "%s: Extra parameters\n", "new_device");
		return -EINVAL;
	}

	if ((info.addr & I2C_ADDR_OFFSET_TEN_BIT) == I2C_ADDR_OFFSET_TEN_BIT) {
		info.addr &= ~I2C_ADDR_OFFSET_TEN_BIT;
		info.flags |= I2C_CLIENT_TEN;
	}

	if (info.addr & I2C_ADDR_OFFSET_SLAVE) {
		info.addr &= ~I2C_ADDR_OFFSET_SLAVE;
		info.flags |= I2C_CLIENT_SLAVE;
	}

	client = i2c_new_client_device(adap, &info);
	if (IS_ERR(client))
		return PTR_ERR(client);

	/* 记录这个通过 sysfs 添加的设备，便于后续删除。 */
	mutex_lock(&adap->userspace_clients_lock);
	list_add_tail(&client->detected, &adap->userspace_clients);
	mutex_unlock(&adap->userspace_clients_lock);
	dev_info(dev, "%s: Instantiated device %s at 0x%02hx\n", "new_device",
		 info.type, info.addr);

	return count;
}
static DEVICE_ATTR_WO(new_device);

/*
 * 当然也允许用户删除自己通过 sysfs 创建的设备，防止手工创建
 * 错误后无法回收。
 *
 * 这个接口只能删除上面通过 i2c_sysfs_new_device 创建的设备，
 * 这样可以保证不会误删仍被内核代码引用的设备。
 *
 * 这里的参数检查同样比较严格，因为我们不希望用户删错设备。
 */
static ssize_t
delete_device_store(struct device *dev, struct device_attribute *attr,
		    const char *buf, size_t count)
{
	struct i2c_adapter *adap = to_i2c_adapter(dev);
	struct i2c_client *client, *next;
	unsigned short addr;
	char end;
	int res;

	/* 解析参数，并拒绝多余字段。 */
	res = sscanf(buf, "%hi%c", &addr, &end);
	if (res < 1) {
		dev_err(dev, "%s: Can't parse I2C address\n", "delete_device");
		return -EINVAL;
	}
	if (res > 1  && end != '\n') {
		dev_err(dev, "%s: Extra parameters\n", "delete_device");
		return -EINVAL;
	}

	/* 确认目标设备确实是通过 sysfs 创建出来的。 */
	res = -ENOENT;
	mutex_lock_nested(&adap->userspace_clients_lock,
			  i2c_adapter_depth(adap));
	list_for_each_entry_safe(client, next, &adap->userspace_clients,
				 detected) {
		if (i2c_encode_flags_to_addr(client) == addr) {
			dev_info(dev, "%s: Deleting device %s at 0x%02hx\n",
				 "delete_device", client->name, client->addr);

			list_del(&client->detected);
			i2c_unregister_device(client);
			res = count;
			break;
		}
	}
	mutex_unlock(&adap->userspace_clients_lock);

	if (res < 0)
		dev_err(dev, "%s: Can't find device in list\n",
			"delete_device");
	return res;
}
static DEVICE_ATTR_IGNORE_LOCKDEP(delete_device, S_IWUSR, NULL,
				  delete_device_store);

static struct attribute *i2c_adapter_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_new_device.attr,
	&dev_attr_delete_device.attr,
	NULL
};
ATTRIBUTE_GROUPS(i2c_adapter);

const struct device_type i2c_adapter_type = {
	.groups		= i2c_adapter_groups,
	.release	= i2c_adapter_dev_release,
};
EXPORT_SYMBOL_GPL(i2c_adapter_type);

/**
 * i2c_verify_adapter - 将参数验证为 i2c_adapter，否则返回 NULL
 * @dev: 设备对象，通常来自驱动模型迭代器
 *
 * 在遍历驱动模型树时，尤其是使用 @device_for_each_child() 这类
 * 迭代器时，不能对遇到的节点做太多假设。使用这个辅助函数可以
 * 避免把非 I2C 设备误当成 i2c_adapter，从而引发崩溃。
 */
struct i2c_adapter *i2c_verify_adapter(struct device *dev)
{
	return (dev->type == &i2c_adapter_type)
			? to_i2c_adapter(dev)
			: NULL;
}
EXPORT_SYMBOL(i2c_verify_adapter);

static void i2c_scan_static_board_info(struct i2c_adapter *adapter)
{
	struct i2c_devinfo	*devinfo;

	down_read(&__i2c_board_lock);
	list_for_each_entry(devinfo, &__i2c_board_list, list) {
		if (devinfo->busnum == adapter->nr &&
		    IS_ERR(i2c_new_client_device(adapter, &devinfo->board_info)))
			dev_err(&adapter->dev,
				"Can't create device at 0x%02x\n",
				devinfo->board_info.addr);
	}
	up_read(&__i2c_board_lock);
}

static int i2c_do_add_adapter(struct i2c_driver *driver,
			      struct i2c_adapter *adap)
{
	/* 扫描当前总线上可识别的设备，并实例化对应的 client。 */
	i2c_detect(adap, driver);

	return 0;
}

static int __process_new_adapter(struct device_driver *d, void *data)
{
	return i2c_do_add_adapter(to_i2c_driver(d), data);
}

static const struct i2c_lock_operations i2c_adapter_lock_ops = {
	.lock_bus =    i2c_adapter_lock_bus,
	.trylock_bus = i2c_adapter_trylock_bus,
	.unlock_bus =  i2c_adapter_unlock_bus,
};

static void i2c_host_notify_irq_teardown(struct i2c_adapter *adap)
{
	struct irq_domain *domain = adap->host_notify_domain;
	irq_hw_number_t hwirq;

	if (!domain)
		return;

	for (hwirq = 0 ; hwirq < I2C_ADDR_7BITS_COUNT ; hwirq++)
		irq_dispose_mapping(irq_find_mapping(domain, hwirq));

	irq_domain_remove(domain);
	adap->host_notify_domain = NULL;
}

static int i2c_host_notify_irq_map(struct irq_domain *h,
					  unsigned int virq,
					  irq_hw_number_t hw_irq_num)
{
	irq_set_chip_and_handler(virq, &dummy_irq_chip, handle_simple_irq);

	return 0;
}

static const struct irq_domain_ops i2c_host_notify_irq_ops = {
	.map = i2c_host_notify_irq_map,
};

static int i2c_setup_host_notify_irq_domain(struct i2c_adapter *adap)
{
	struct irq_domain *domain;

	if (!i2c_check_functionality(adap, I2C_FUNC_SMBUS_HOST_NOTIFY))
		return 0;

	domain = irq_domain_create_linear(dev_fwnode(adap->dev.parent),
					  I2C_ADDR_7BITS_COUNT,
					  &i2c_host_notify_irq_ops, adap);
	if (!domain)
		return -ENOMEM;

	adap->host_notify_domain = domain;

	return 0;
}

/**
 * i2c_handle_smbus_host_notify - 将 Host Notify 事件转发给正确的 I2C client
 * @adap: 目标适配器
 * @addr: 发出通知的设备地址
 * Context: can't sleep
 *
 * 这是给 I2C 总线驱动中断处理函数调用的辅助函数，用来安排
 * 对应的 Host Notify IRQ。
 */
int i2c_handle_smbus_host_notify(struct i2c_adapter *adap, unsigned short addr)
{
	int irq;

	if (!adap)
		return -EINVAL;

	dev_dbg(&adap->dev, "Detected HostNotify from address 0x%02x", addr);

	irq = irq_find_mapping(adap->host_notify_domain, addr);
	if (irq <= 0)
		return -ENXIO;

	generic_handle_irq_safe(irq);

	return 0;
}
EXPORT_SYMBOL_GPL(i2c_handle_smbus_host_notify);

static int i2c_register_adapter(struct i2c_adapter *adap)
{
	int res = -EINVAL;

	/* 必须等驱动模型初始化完成后才能注册。 */
	if (WARN_ON(!is_registered)) {
		res = -EAGAIN;
		goto out_list;
	}

	/* 基本有效性检查。 */
	if (WARN(!adap->name[0], "i2c adapter has no name"))
		goto out_list;

	if (!adap->algo) {
		pr_err("adapter '%s': no algo supplied!\n", adap->name);
		goto out_list;
	}

	if (!adap->lock_ops)
		adap->lock_ops = &i2c_adapter_lock_ops;

	adap->locked_flags = 0;
	rt_mutex_init(&adap->bus_lock);
	rt_mutex_init(&adap->mux_lock);
	mutex_init(&adap->userspace_clients_lock);
	INIT_LIST_HEAD(&adap->userspace_clients);

	/* 如果还没有设置默认超时，就把它设为 1 秒。 */
	if (adap->timeout == 0)
		adap->timeout = HZ;

	/* 为 Host Notify 注册软中断映射。 */
	res = i2c_setup_host_notify_irq_domain(adap);
	if (res) {
		pr_err("adapter '%s': can't create Host Notify IRQs (%d)\n",
		       adap->name, res);
		goto out_list;
	}

	dev_set_name(&adap->dev, "i2c-%d", adap->nr);
	adap->dev.bus = &i2c_bus_type;
	adap->dev.type = &i2c_adapter_type;
	device_initialize(&adap->dev);

	/*
	 * device_add() 之后这个适配器就可能被当作父设备使用，
	 * 所以必须提前准备好 runtime PM，尤其是 ignore-children。
	 */
	device_enable_async_suspend(&adap->dev);
	pm_runtime_no_callbacks(&adap->dev);
	pm_suspend_ignore_children(&adap->dev, true);
	pm_runtime_enable(&adap->dev);

	res = device_add(&adap->dev);
	if (res) {
		pr_err("adapter '%s': can't register device (%d)\n", adap->name, res);
		put_device(&adap->dev);
		goto out_list;
	}

	adap->debugfs = debugfs_create_dir(dev_name(&adap->dev), i2c_debugfs_root);

	res = i2c_setup_smbus_alert(adap);
	if (res)
		goto out_reg;

	res = i2c_init_recovery(adap);
	if (res == -EPROBE_DEFER)
		goto out_reg;

	dev_dbg(&adap->dev, "adapter [%s] registered\n", adap->name);

	/* 创建预先声明好的设备节点。 */
	of_i2c_register_devices(adap);
	i2c_acpi_install_space_handler(adap);
	i2c_acpi_register_devices(adap);

	if (adap->nr < __i2c_first_dynamic_bus_num)
		i2c_scan_static_board_info(adap);

	/* 通知已经存在的驱动来尝试绑定这个新适配器。 */
	mutex_lock(&core_lock);
	bus_for_each_drv(&i2c_bus_type, NULL, adap, __process_new_adapter);
	mutex_unlock(&core_lock);

	return 0;

out_reg:
	debugfs_remove_recursive(adap->debugfs);
	init_completion(&adap->dev_released);
	device_unregister(&adap->dev);
	wait_for_completion(&adap->dev_released);
out_list:
	mutex_lock(&core_lock);
	idr_remove(&i2c_adapter_idr, adap->nr);
	mutex_unlock(&core_lock);
	return res;
}

/**
 * __i2c_add_numbered_adapter - 适用于 bus number 已经固定的注册路径
 * @adap: 要注册的适配器，调用前已经初始化 adap->nr
 * Context: can sleep
 *
 * 这是 i2c_add_numbered_adapter() 的内部实现，适配器号不会是 -1。
 */
static int __i2c_add_numbered_adapter(struct i2c_adapter *adap)
{
	int id;

	mutex_lock(&core_lock);
	id = idr_alloc(&i2c_adapter_idr, adap, adap->nr, adap->nr + 1, GFP_KERNEL);
	mutex_unlock(&core_lock);
	if (WARN(id < 0, "couldn't get idr"))
		return id == -ENOSPC ? -EBUSY : id;

	return i2c_register_adapter(adap);
}

/**
 * i2c_add_adapter - 注册 I2C 适配器，使用动态总线号
 * @adapter: 要添加的适配器
 * Context: can sleep
 *
 * 这个接口用于总线号不重要，或者总线号由 DT alias 指定的场景。
 * 例如 USB 转接出来的 I2C 适配器，或者 PCI 插件卡上的 I2C 控制器。
 *
 * 核心流程是：
 * - 若固件给了 `i2cX` alias，就优先复用这个编号
 * - 否则从动态总线号区间分配一个空闲 nr
 * - 再调用 i2c_register_adapter() 完成剩余注册动作
 *
 * i2c_register_adapter() 内部还会继续做：
 * - 注册 adapter 的 struct device
 * - 建立 sysfs/debugfs 节点
 * - 枚举 OF/ACPI/boardinfo 里预声明的 client
 * - 让已经注册的 i2c_driver 补做一轮探测
 *
 * 返回 0 表示成功，此时会为适配器分配一个新的总线号并写入
 * adap->nr；否则返回负的 errno。
 */
int i2c_add_adapter(struct i2c_adapter *adapter)
{
	struct device *dev = &adapter->dev;
	int id;

	id = of_alias_get_id(dev->of_node, "i2c");
	if (id >= 0) {
		adapter->nr = id;
		return __i2c_add_numbered_adapter(adapter);
	}

	mutex_lock(&core_lock);
	id = idr_alloc(&i2c_adapter_idr, adapter,
		       __i2c_first_dynamic_bus_num, 0, GFP_KERNEL);
	mutex_unlock(&core_lock);
	if (WARN(id < 0, "couldn't get idr"))
		return id;

	adapter->nr = id;

	return i2c_register_adapter(adapter);
}
EXPORT_SYMBOL(i2c_add_adapter);

/**
 * i2c_add_numbered_adapter - 注册 I2C 适配器，使用静态总线号
 * @adap: 要注册的适配器，调用前已经初始化 adap->nr
 * Context: can sleep
 *
 * 这个接口用于总线号很重要的场景。例如 SoC 内部固定的 I2C 控制器，
 * 或者板级设计中已经明确指定总线号、并通过 i2c_board_info 配置
 * 设备的场景。
 *
 * 如果请求的总线号是 -1，那么这个函数会退化成 i2c_add_adapter()，
 * 转而动态分配一个总线号。
 *
 * 如果这条总线上没有提前声明过设备，务必先注册适配器，再注册动态
 * 创建的设备，否则需要的总线号可能会被占用。
 *
 * 返回 0 表示成功，此时 adap->nr 对应的总线可以被客户端使用，
 * 同时会扫描 i2c_register_board_info() 预先登记的设备并创建
 * 相应的驱动模型节点；否则返回负的 errno。
 */
int i2c_add_numbered_adapter(struct i2c_adapter *adap)
{
	if (adap->nr == -1) /* -1 means dynamically assign bus id */
		return i2c_add_adapter(adap);

	return __i2c_add_numbered_adapter(adap);
}
EXPORT_SYMBOL_GPL(i2c_add_numbered_adapter);

static void i2c_do_del_adapter(struct i2c_driver *driver,
			      struct i2c_adapter *adapter)
{
	struct i2c_client *client, *_n;

	/* 删除之前通过 detect 自动创建的设备。 */
	list_for_each_entry_safe(client, _n, &driver->clients, detected) {
		if (client->adapter == adapter) {
			dev_dbg(&adapter->dev, "Removing %s at 0x%x\n",
				client->name, client->addr);
			list_del(&client->detected);
			i2c_unregister_device(client);
		}
	}
}

static int __unregister_client(struct device *dev, void *dummy)
{
	struct i2c_client *client = i2c_verify_client(dev);
	if (client && strcmp(client->name, "dummy"))
		i2c_unregister_device(client);
	return 0;
}

static int __unregister_dummy(struct device *dev, void *dummy)
{
	struct i2c_client *client = i2c_verify_client(dev);
	i2c_unregister_device(client);
	return 0;
}

static int __process_removed_adapter(struct device_driver *d, void *data)
{
	i2c_do_del_adapter(to_i2c_driver(d), data);
	return 0;
}

/**
 * i2c_del_adapter - 注销 I2C 适配器
 * @adap: 要注销的适配器
 * Context: can sleep
 *
 * 注销一个之前通过 @i2c_add_adapter 或 @i2c_add_numbered_adapter
 * 注册成功的 I2C 适配器。
 *
 * 删除顺序不能乱：
 * - 先让所有使用 detect() 自动生成的设备撤掉
 * - 再清理 userspace 通过 sysfs 创建的 client
 * - 再分两轮注销真实 client 和 dummy client
 * - 最后注销 adapter 自身并归还总线号
 *
 * 之所以把 dummy client 放到第二轮，是因为一些真实驱动会在 remove()
 * 期间依赖这些 dummy 句柄去完成寄存器收尾或撤销附属地址状态。
 */
void i2c_del_adapter(struct i2c_adapter *adap)
{
	struct i2c_adapter *found;
	struct i2c_client *client, *next;

	/* 先确认这个适配器确实注册过。 */
	mutex_lock(&core_lock);
	found = idr_find(&i2c_adapter_idr, adap->nr);
	mutex_unlock(&core_lock);
	if (found != adap) {
		pr_debug("attempting to delete unregistered adapter [%s]\n", adap->name);
		return;
	}

	i2c_acpi_remove_space_handler(adap);
	/* 通知驱动这个适配器正在被移除。 */
	mutex_lock(&core_lock);
	bus_for_each_drv(&i2c_bus_type, NULL, adap,
			       __process_removed_adapter);
	mutex_unlock(&core_lock);

	/* 移除之前通过 sysfs 实例化出来的设备。 */
	mutex_lock_nested(&adap->userspace_clients_lock,
			  i2c_adapter_depth(adap));
	list_for_each_entry_safe(client, next, &adap->userspace_clients,
				 detected) {
		dev_dbg(&adap->dev, "Removing %s at 0x%x\n", client->name,
			client->addr);
		list_del(&client->detected);
		i2c_unregister_device(client);
	}
	mutex_unlock(&adap->userspace_clients_lock);

	/*
	 * 分两轮拆掉当前绑定的客户端。这个过程不会失败，所以不检查
	 * 返回值。之所以要两轮，是因为 dummy 设备不能在第一轮就删掉：
	 * 它们可能是由真实设备创建出来、用于后续清理的，所以要先给
	 * 真实设备一个自行清理 dummy 的机会。
	 */
	device_for_each_child(&adap->dev, NULL, __unregister_client);
	device_for_each_child(&adap->dev, NULL, __unregister_dummy);

	/* device_unregister 之后设备名就不再可靠。 */
	dev_dbg(&adap->dev, "adapter [%s] unregistered\n", adap->name);

	pm_runtime_disable(&adap->dev);

	i2c_host_notify_irq_teardown(adap);

	debugfs_remove_recursive(adap->debugfs);

	/*
	 * 等待所有引用都释放完毕。
	 *
	 * FIXME: 这段老代码理想情况下应该改掉，最好像 SPI 或 netdev
	 * 那样，把 struct device 的生命周期和 i2c_adapter 解耦。任何
	 * 替代方案都应该在打开 DEBUG_KOBJECT_RELEASE 后充分测试。
	 */
	init_completion(&adap->dev_released);
	device_unregister(&adap->dev);
	wait_for_completion(&adap->dev_released);

	/* 释放总线号。 */
	mutex_lock(&core_lock);
	idr_remove(&i2c_adapter_idr, adap->nr);
	mutex_unlock(&core_lock);

	/* 清空 device 结构，避免这个适配器将来再次注册时残留旧状态。 */
	memset(&adap->dev, 0, sizeof(adap->dev));
}
EXPORT_SYMBOL(i2c_del_adapter);

static void devm_i2c_del_adapter(void *adapter)
{
	i2c_del_adapter(adapter);
}

/**
 * devm_i2c_add_adapter - i2c_add_adapter() 的受管理版本
 * @dev: 负责管理这个 I2C 适配器生命周期的设备
 * @adapter: 要添加的适配器
 * Context: can sleep
 *
 * 行为与 i2c_add_adapter() 相同，只是驱动卸载时会自动删除该适配器。
 */
int devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adapter)
{
	int ret;

	ret = i2c_add_adapter(adapter);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, devm_i2c_del_adapter, adapter);
}
EXPORT_SYMBOL_GPL(devm_i2c_add_adapter);

static int i2c_dev_or_parent_fwnode_match(struct device *dev, const void *data)
{
	if (device_match_fwnode(dev, data))
		return 1;

	if (dev->parent && device_match_fwnode(dev->parent, data))
		return 1;

	return 0;
}

/**
 * i2c_find_adapter_by_fwnode() - 根据 fwnode 查找对应的 i2c_adapter
 * @fwnode: 与目标 &struct i2c_adapter 对应的 &struct fwnode_handle
 *
 * 查找并返回与 @fwnode 对应的 &struct i2c_adapter。
 * 如果找不到适配器，或者 @fwnode 为空，则返回 NULL。
 *
 * 使用完后，调用者必须执行 put_device(&adapter->dev)。
 */
struct i2c_adapter *i2c_find_adapter_by_fwnode(struct fwnode_handle *fwnode)
{
	struct i2c_adapter *adapter;
	struct device *dev;

	if (IS_ERR_OR_NULL(fwnode))
		return NULL;

	dev = bus_find_device(&i2c_bus_type, NULL, fwnode,
			      i2c_dev_or_parent_fwnode_match);
	if (!dev)
		return NULL;

	adapter = i2c_verify_adapter(dev);
	if (!adapter)
		put_device(dev);

	return adapter;
}
EXPORT_SYMBOL(i2c_find_adapter_by_fwnode);

/**
 * i2c_get_adapter_by_fwnode() - 根据 fwnode 查找并持有对应的 i2c_adapter
 * @fwnode: 与目标 &struct i2c_adapter 对应的 &struct fwnode_handle
 *
 * 查找并返回与 @fwnode 对应的 &struct i2c_adapter，同时增加适配器
 * 所属模块的引用计数。如果找不到适配器，或者 @fwnode 为空，则
 * 返回 NULL。
 *
 * 使用完后，调用者必须执行 i2c_put_adapter(adapter)。
 * 注意这与 i2c_find_adapter_by_node() 不同。
 */
struct i2c_adapter *i2c_get_adapter_by_fwnode(struct fwnode_handle *fwnode)
{
	struct i2c_adapter *adapter;

	adapter = i2c_find_adapter_by_fwnode(fwnode);
	if (!adapter)
		return NULL;

	if (!try_module_get(adapter->owner)) {
		put_device(&adapter->dev);
		adapter = NULL;
	}

	return adapter;
}
EXPORT_SYMBOL(i2c_get_adapter_by_fwnode);

static void i2c_parse_timing(struct device *dev, char *prop_name, u32 *cur_val_p,
			    u32 def_val, bool use_def)
{
	int ret;

	ret = device_property_read_u32(dev, prop_name, cur_val_p);
	if (ret && use_def)
		*cur_val_p = def_val;

	dev_dbg(dev, "%s: %u\n", prop_name, *cur_val_p);
}

/**
 * i2c_parse_fw_timings - 从固件中读取 I2C 相关时序参数
 * @dev: 要扫描 I2C 时序属性的设备
 * @t: 用于填充结果的 i2c_timings 结构体
 * @use_defaults: 当属性缺失时，是否使用 I2C 规范推导出的默认值
 *
 * 从固件属性中读取时序相关参数；如果没有提供，还可以按需要填入
 * 标准默认值。
 *
 * 这些字段并不会直接驱动硬件，它们只是由 I2C 控制器驱动读取后，
 * 再换算成具体寄存器值。也就是说，这个 helper 负责的是“统一解析
 * 固件语义”，而不是“统一配置硬件时序”。
 */
void i2c_parse_fw_timings(struct device *dev, struct i2c_timings *t, bool use_defaults)
{
	bool u = use_defaults;
	u32 d;

	i2c_parse_timing(dev, "clock-frequency", &t->bus_freq_hz,
			 I2C_MAX_STANDARD_MODE_FREQ, u);

	d = t->bus_freq_hz <= I2C_MAX_STANDARD_MODE_FREQ ? 1000 :
	    t->bus_freq_hz <= I2C_MAX_FAST_MODE_FREQ ? 300 : 120;
	i2c_parse_timing(dev, "i2c-scl-rising-time-ns", &t->scl_rise_ns, d, u);

	d = t->bus_freq_hz <= I2C_MAX_FAST_MODE_FREQ ? 300 : 120;
	i2c_parse_timing(dev, "i2c-scl-falling-time-ns", &t->scl_fall_ns, d, u);

	i2c_parse_timing(dev, "i2c-scl-internal-delay-ns",
			 &t->scl_int_delay_ns, 0, u);
	i2c_parse_timing(dev, "i2c-sda-falling-time-ns", &t->sda_fall_ns,
			 t->scl_fall_ns, u);
	i2c_parse_timing(dev, "i2c-sda-hold-time-ns", &t->sda_hold_ns, 0, u);
	i2c_parse_timing(dev, "i2c-digital-filter-width-ns",
			 &t->digital_filter_width_ns, 0, u);
	i2c_parse_timing(dev, "i2c-analog-filter-cutoff-frequency",
			 &t->analog_filter_cutoff_freq_hz, 0, u);
}
EXPORT_SYMBOL_GPL(i2c_parse_fw_timings);

/* ------------------------------------------------------------------------- */

int i2c_for_each_dev(void *data, int (*fn)(struct device *dev, void *data))
{
	int res;

	mutex_lock(&core_lock);
	res = bus_for_each_dev(&i2c_bus_type, NULL, data, fn);
	mutex_unlock(&core_lock);

	return res;
}
EXPORT_SYMBOL_GPL(i2c_for_each_dev);

static int __process_new_driver(struct device *dev, void *data)
{
	if (dev->type != &i2c_adapter_type)
		return 0;
	return i2c_do_add_adapter(data, to_i2c_adapter(dev));
}

/*
 * I2C 驱动注册与解绑。
 *
 * 一个 i2c_driver 可以同时匹配多个 i2c_client，真正的绑定/解绑动作
 * 仍由 driver core 负责。I2C core 在这里补充的工作主要有两类：
 * - 维护 detect() 自动探测出来的 client 链表
 * - 在驱动或适配器“后注册”的情况下，补做一次遍历以完成迟到匹配
 */

int i2c_register_driver(struct module *owner, struct i2c_driver *driver)
{
	int res;

	/* 必须等驱动模型初始化完成后才能注册。 */
	if (WARN_ON(!is_registered))
		return -EAGAIN;

	/* 把驱动加入 driver core 里的 I2C 驱动列表。 */
	driver->driver.owner = owner;
	driver->driver.bus = &i2c_bus_type;
	INIT_LIST_HEAD(&driver->clients);

	/*
	 * driver_register() 返回时，driver core 已经对所有“匹配但尚未
	 * 绑定”的设备调用过 probe()。
	 */
	res = driver_register(&driver->driver);
	if (res)
		return res;

	pr_debug("driver [%s] registered\n", driver->driver.name);

	/*
	 * 对已经存在的 adapter 再补做一轮处理。
	 *
	 * driver_register() 会让 driver core 去匹配“已存在的 client”，
	 * 但 detect() 风格驱动还需要在每条 adapter 上主动跑一遍地址扫描，
	 * 这一步由 i2c_do_add_adapter() 完成。
	 */
	i2c_for_each_dev(driver, __process_new_driver);

	return 0;
}
EXPORT_SYMBOL(i2c_register_driver);

static int __process_removed_driver(struct device *dev, void *data)
{
	if (dev->type == &i2c_adapter_type)
		i2c_do_del_adapter(data, to_i2c_adapter(dev));
	return 0;
}

/**
 * i2c_del_driver - 注销 I2C 驱动
 * @driver: 要注销的驱动
 * Context: can sleep
 */
void i2c_del_driver(struct i2c_driver *driver)
{
	i2c_for_each_dev(driver, __process_removed_driver);

	driver_unregister(&driver->driver);
	pr_debug("driver [%s] unregistered\n", driver->driver.name);
}
EXPORT_SYMBOL(i2c_del_driver);

/* ------------------------------------------------------------------------- */

struct i2c_cmd_arg {
	unsigned	cmd;
	void		*arg;
};

static int i2c_cmd(struct device *dev, void *_arg)
{
	struct i2c_client	*client = i2c_verify_client(dev);
	struct i2c_cmd_arg	*arg = _arg;
	struct i2c_driver	*driver;

	if (!client || !client->dev.driver)
		return 0;

	driver = to_i2c_driver(client->dev.driver);
	if (driver->command)
		driver->command(client, arg->cmd, arg->arg);
	return 0;
}

void i2c_clients_command(struct i2c_adapter *adap, unsigned int cmd, void *arg)
{
	struct i2c_cmd_arg	cmd_arg;

	cmd_arg.cmd = cmd;
	cmd_arg.arg = arg;
	device_for_each_child(&adap->dev, &cmd_arg, i2c_cmd);
}
EXPORT_SYMBOL(i2c_clients_command);

static int __init i2c_init(void)
{
	int retval;

	retval = of_alias_get_highest_id("i2c");

	down_write(&__i2c_board_lock);
	if (retval >= __i2c_first_dynamic_bus_num)
		__i2c_first_dynamic_bus_num = retval + 1;
	up_write(&__i2c_board_lock);

	retval = bus_register(&i2c_bus_type);
	if (retval)
		return retval;

	is_registered = true;

	i2c_debugfs_root = debugfs_create_dir("i2c", NULL);

	retval = i2c_add_driver(&dummy_driver);
	if (retval)
		goto class_err;

	if (IS_ENABLED(CONFIG_OF_DYNAMIC))
		WARN_ON(of_reconfig_notifier_register(&i2c_of_notifier));
	if (IS_ENABLED(CONFIG_ACPI))
		WARN_ON(acpi_reconfig_notifier_register(&i2c_acpi_notifier));

	return 0;

class_err:
	is_registered = false;
	bus_unregister(&i2c_bus_type);
	return retval;
}

static void __exit i2c_exit(void)
{
	if (IS_ENABLED(CONFIG_ACPI))
		WARN_ON(acpi_reconfig_notifier_unregister(&i2c_acpi_notifier));
	if (IS_ENABLED(CONFIG_OF_DYNAMIC))
		WARN_ON(of_reconfig_notifier_unregister(&i2c_of_notifier));
	i2c_del_driver(&dummy_driver);
	debugfs_remove_recursive(i2c_debugfs_root);
	bus_unregister(&i2c_bus_type);
	tracepoint_synchronize_unregister();
}

/*
 * 必须尽早初始化，因为有些子系统会在 subsys_initcall() 里注册
 * I2C 驱动，而它们的链接和初始化顺序可能早于 i2c core。
 */
postcore_initcall(i2c_init);
module_exit(i2c_exit);

/* ----------------------------------------------------
 * I2C 总线功能接口
 * ----------------------------------------------------
 */

/* 仅在 quirk 非 0 时才检查长度是否超限。 */
#define i2c_quirk_exceeded(val, quirk) ((quirk) && ((val) > (quirk)))

static int i2c_quirk_error(struct i2c_adapter *adap, struct i2c_msg *msg, char *err_msg)
{
	dev_err_ratelimited(&adap->dev, "adapter quirk: %s (addr 0x%04x, size %u, %s)\n",
			    err_msg, msg->addr, msg->len,
			    str_read_write(msg->flags & I2C_M_RD));
	return -EOPNOTSUPP;
}

static int i2c_check_for_quirks(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	const struct i2c_adapter_quirks *q = adap->quirks;
	int max_num = q->max_num_msgs, i;
	bool do_len_check = true;

	if (q->flags & I2C_AQ_COMB) {
		max_num = 2;

		/* 组合消息需要做额外检查。 */
		if (num == 2) {
			if (q->flags & I2C_AQ_COMB_WRITE_FIRST && msgs[0].flags & I2C_M_RD)
				return i2c_quirk_error(adap, &msgs[0], "1st comb msg must be write");

			if (q->flags & I2C_AQ_COMB_READ_SECOND && !(msgs[1].flags & I2C_M_RD))
				return i2c_quirk_error(adap, &msgs[1], "2nd comb msg must be read");

			if (q->flags & I2C_AQ_COMB_SAME_ADDR && msgs[0].addr != msgs[1].addr)
				return i2c_quirk_error(adap, &msgs[0], "comb msg only to same addr");

			if (i2c_quirk_exceeded(msgs[0].len, q->max_comb_1st_msg_len))
				return i2c_quirk_error(adap, &msgs[0], "msg too long");

			if (i2c_quirk_exceeded(msgs[1].len, q->max_comb_2nd_msg_len))
				return i2c_quirk_error(adap, &msgs[1], "msg too long");

			do_len_check = false;
		}
	}

	if (i2c_quirk_exceeded(num, max_num))
		return i2c_quirk_error(adap, &msgs[0], "too many messages");

	for (i = 0; i < num; i++) {
		u16 len = msgs[i].len;

		if (msgs[i].flags & I2C_M_RD) {
			if (do_len_check && i2c_quirk_exceeded(len, q->max_read_len))
				return i2c_quirk_error(adap, &msgs[i], "msg too long");

			if (q->flags & I2C_AQ_NO_ZERO_LEN_READ && len == 0)
				return i2c_quirk_error(adap, &msgs[i], "no zero length");
		} else {
			if (do_len_check && i2c_quirk_exceeded(len, q->max_write_len))
				return i2c_quirk_error(adap, &msgs[i], "msg too long");

			if (q->flags & I2C_AQ_NO_ZERO_LEN_WRITE && len == 0)
				return i2c_quirk_error(adap, &msgs[i], "no zero length");
		}
	}

	return 0;
}

/**
 * __i2c_transfer - i2c_transfer() 的无锁版本
 * @adap: I2C 总线句柄
 * @msgs: 要执行的一条或多条消息；每条消息都从 START 开始，
 *	在发送 STOP 结束整个操作之前依次完成。
 * @num: 要执行的消息数量
 *
 * 这是 I2C 传输的真正核心入口。它假设调用者已经完成总线锁定，因此
 * 这里专注于“能不能传”和“怎么调算法层”：
 * - 检查 adapter 是否支持 master_xfer
 * - 检查 suspend/quirk 约束
 * - 在 tracepoint 打开时记录消息内容
 * - 必要时根据 adapter->retries 自动重试
 *
 * 返回负的 errno，否则返回已执行的消息数量。
 *
 * 调用此函数前必须持有适配器锁，这里也不会额外打印调试日志。
 */
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	unsigned long orig_jiffies;
	int ret, try;

	if (!adap->algo->master_xfer) {
		dev_dbg(&adap->dev, "I2C level transfers not supported\n");
		return -EOPNOTSUPP;
	}

	if (WARN_ON(!msgs || num < 1))
		return -EINVAL;

	ret = __i2c_check_suspended(adap);
	if (ret)
		return ret;

	if (adap->quirks && i2c_check_for_quirks(adap, msgs, num))
		return -EOPNOTSUPP;

	/*
	 * 当 tracepoint i2c_transfer 被启用时，i2c_trace_msg_key 也会被
	 * 打开。这样可以在不需要时避免执行这段 for 循环。
	 */
	if (static_branch_unlikely(&i2c_trace_msg_key)) {
		int i;
		for (i = 0; i < num; i++)
			if (msgs[i].flags & I2C_M_RD)
				trace_i2c_read(adap, &msgs[i], i);
			else
				trace_i2c_write(adap, &msgs[i], i);
	}

	/*
	 * 遇到 -EAGAIN 时自动重试。
	 *
	 * 对很多控制器实现来说，-EAGAIN 对应仲裁丢失或暂时无法完成传输。
	 * I2C core 用 retries + timeout 做一层统一兜底，避免每个调用方
	 * 都重复实现这一层重试逻辑。
	 */
	orig_jiffies = jiffies;
	for (ret = 0, try = 0; try <= adap->retries; try++) {
		if (i2c_in_atomic_xfer_mode() && adap->algo->master_xfer_atomic)
			ret = adap->algo->master_xfer_atomic(adap, msgs, num);
		else
			ret = adap->algo->master_xfer(adap, msgs, num);

		if (ret != -EAGAIN)
			break;
		if (time_after(jiffies, orig_jiffies + adap->timeout))
			break;
	}

	if (static_branch_unlikely(&i2c_trace_msg_key)) {
		int i;
		for (i = 0; i < ret; i++)
			if (msgs[i].flags & I2C_M_RD)
				trace_i2c_reply(adap, &msgs[i], i);
		trace_i2c_result(adap, num, ret);
	}

	return ret;
}
EXPORT_SYMBOL(__i2c_transfer);

/**
 * i2c_transfer - 执行单条或组合 I2C 消息
 * @adap: I2C 总线句柄
 * @msgs: 要执行的一条或多条消息；在发送 STOP 结束整个操作之前，
 *	每条消息都会先从 START 开始。
 * @num: 要执行的消息数量
 *
 * 这是给上层驱动使用的常规 I2C 组合传输 API。它在 __i2c_transfer()
 * 外围补上了 adapter 级总线锁，因此调用者不需要自己关心与其它传输
 * 的并发串行化。
 *
 * 返回负的 errno，否则返回已执行的消息数量。
 *
 * 注意，这里并不要求每条消息都发往同一个从设备地址，虽然这
 * 是最常见的使用方式。
 */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret;

	/*
	 * REVISIT: 这里的错误上报模型仍然比较弱：
	 *
	 *  - 如果从从设备读到 N 个字节后发生错误，当前没有办法报告
	 *    “已经收到了 N 个字节”。
	 *
	 *  - 如果向从设备发送了 N 个字节后收到 NAK，也没有办法报告
	 *    “已经发了 N 个字节”；如果这是正确的响应，也无法继续执
	 *    行组合消息后面的剩余部分。
	 *
	 *  - 比如 num=2 时，第一个消息成功、第二个消息中途出错，现
	 *    在也不清楚应该返回 1（丢弃第二条状态）还是 errno（丢
	 *    弃第一条状态）。
	 */
	ret = __i2c_lock_bus_helper(adap);
	if (ret)
		return ret;

	ret = __i2c_transfer(adap, msgs, num);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);

	return ret;
}
EXPORT_SYMBOL(i2c_transfer);

/**
 * i2c_transfer_buffer_flags - 用缓冲区发送/接收单条 I2C 消息
 * @client: 从设备句柄
 * @buf: 数据缓冲区
 * @count: 要传输的字节数，因为 msg.len 是 u16，所以必须小于 64k
 * @flags: 消息要使用的标志，例如读操作使用 I2C_M_RD
 *
 * 返回负的 errno，否则返回已传输的字节数。
 */
int i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf,
			      int count, u16 flags)
{
	int ret;
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = flags | (client->flags & I2C_M_TEN),
		.len = count,
		.buf = buf,
	};

	ret = i2c_transfer(client->adapter, &msg, 1);

	/* 如果一切正常（即只传输了 1 条消息），返回字节数，否则返回错误码。 */
	return (ret == 1) ? count : ret;
}
EXPORT_SYMBOL(i2c_transfer_buffer_flags);

/**
 * i2c_get_device_id - 获取设备的厂商 ID、器件 ID 和版本号
 * @client: 要查询的设备
 * @id: 用于返回查询结果的结构体
 *
 * 这是对 I2C Device ID 规范访问流程的封装。目标地址固定是
 * `0x7c`，真正的从设备地址则通过 SMBus command 字段编码传入。
 * 设备若支持这套机制，返回的 3 字节数据会被拆成：
 * - manufacturer_id
 * - part_id
 * - die_revision
 *
 * 失败返回负的 errno，成功返回 0。
 */
int i2c_get_device_id(const struct i2c_client *client,
		      struct i2c_device_identity *id)
{
	struct i2c_adapter *adap = client->adapter;
	union i2c_smbus_data raw_id;
	int ret;

	if (!i2c_check_functionality(adap, I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		return -EOPNOTSUPP;

	raw_id.block[0] = 3;
	ret = i2c_smbus_xfer(adap, I2C_ADDR_DEVICE_ID, 0,
			     I2C_SMBUS_READ, client->addr << 1,
			     I2C_SMBUS_I2C_BLOCK_DATA, &raw_id);
	if (ret)
		return ret;

	id->manufacturer_id = (raw_id.block[1] << 4) | (raw_id.block[2] >> 4);
	id->part_id = ((raw_id.block[2] & 0xf) << 5) | (raw_id.block[3] >> 3);
	id->die_revision = raw_id.block[3] & 0x7;
	return 0;
}
EXPORT_SYMBOL_GPL(i2c_get_device_id);

/**
 * i2c_client_get_device_id - 获取设备当前匹配到的 id_table 项
 * @client: 要查询的设备，且该设备必须已经绑定到驱动
 *
 * 如果找到匹配项则返回对应条目的指针，否则返回 NULL。
 */
const struct i2c_device_id *i2c_client_get_device_id(const struct i2c_client *client)
{
	const struct i2c_driver *drv = to_i2c_driver(client->dev.driver);

	return i2c_match_id(drv->id_table, client);
}
EXPORT_SYMBOL_GPL(i2c_client_get_device_id);

/* ----------------------------------------------------
 * I2C 地址扫描功能
 * 注意：这里不支持 10 位地址。
 * ----------------------------------------------------
 */

/*
 * 旧式默认探测方式，主要面向 SMBus。
 *
 * 默认 probe 使用 quick write，但某些 EEPROM 会被误伤，因此对部分地址段
 * 会回退到 byte read；如果控制器本身不支持 quick write，也会回退。
 *
 * 返回 1 表示探测成功，0 表示失败。
 */
static int i2c_default_probe(struct i2c_adapter *adap, unsigned short addr)
{
	int err;
	union i2c_smbus_data dummy;

#ifdef CONFIG_X86
	if (addr == 0x73 && (adap->class & I2C_CLASS_HWMON)
	 && i2c_check_functionality(adap, I2C_FUNC_SMBUS_READ_BYTE_DATA))
		err = i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_READ, 0,
				     I2C_SMBUS_BYTE_DATA, &dummy);
	else
#endif
	if (!((addr & ~0x07) == 0x30 || (addr & ~0x0f) == 0x50)
	 && i2c_check_functionality(adap, I2C_FUNC_SMBUS_QUICK))
		err = i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_WRITE, 0,
				     I2C_SMBUS_QUICK, NULL);
	else if (i2c_check_functionality(adap, I2C_FUNC_SMBUS_READ_BYTE))
		err = i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_READ, 0,
				     I2C_SMBUS_BYTE, &dummy);
	else {
		dev_warn(&adap->dev, "No suitable probing method supported for address 0x%02X\n",
			 addr);
		err = -EOPNOTSUPP;
	}

	return err >= 0;
}

static int i2c_detect_address(struct i2c_client *temp_client,
			      struct i2c_driver *driver)
{
	struct i2c_board_info info;
	struct i2c_adapter *adapter = temp_client->adapter;
	int addr = temp_client->addr;
	int err;

	/* 先确认地址本身合法。 */
	err = i2c_check_7bit_addr_validity_strict(addr);
	if (err) {
		dev_warn(&adapter->dev, "Invalid probe address 0x%02x\n",
			 addr);
		return err;
	}

	/* 地址已被占用就跳过。这里是 7 位地址，不需要编码 flags。 */
	if (i2c_check_addr_busy(adapter, addr))
		return 0;

	/* 先用默认 probe 判断这个地址是否真的有设备。 */
	if (!i2c_default_probe(adapter, addr))
		return 0;

	/* 最后交给驱动自定义的 detect() 做精确识别。 */
	memset(&info, 0, sizeof(struct i2c_board_info));
	info.addr = addr;
	err = driver->detect(temp_client, &info);
	if (err) {
		/* detect() 返回 -ENODEV 代表“没找到”，这不算真正的错误。 */
		return err == -ENODEV ? 0 : err;
	}

		/* 一致性检查：detect() 成功后必须填入设备名。 */
	if (info.type[0] == '\0') {
		dev_err(&adapter->dev,
			"%s detection function provided no name for 0x%x\n",
			driver->driver.name, addr);
	} else {
		struct i2c_client *client;

			/* 探测成功后，实例化设备。 */
		if (adapter->class & I2C_CLASS_DEPRECATED)
			dev_warn(&adapter->dev,
				"This adapter will soon drop class based instantiation of devices. "
				"Please make sure client 0x%02x gets instantiated by other means. "
				"Check 'Documentation/i2c/instantiating-devices.rst' for details.\n",
				info.addr);

		dev_dbg(&adapter->dev, "Creating %s at 0x%02x\n",
			info.type, info.addr);
		client = i2c_new_client_device(adapter, &info);
		if (!IS_ERR(client))
			list_add_tail(&client->detected, &driver->clients);
		else
			dev_err(&adapter->dev, "Failed creating %s at 0x%02x\n",
				info.type, info.addr);
	}
	return 0;
}

static int i2c_detect(struct i2c_adapter *adapter, struct i2c_driver *driver)
{
	const unsigned short *address_list;
	struct i2c_client *temp_client;
	int i, err = 0;

	address_list = driver->address_list;
	if (!driver->detect || !address_list)
		return 0;

	/* 提示：这个适配器已经不再支持基于 class 的自动实例化。 */
	if (adapter->class == I2C_CLASS_DEPRECATED) {
		dev_dbg(&adapter->dev,
			"This adapter dropped support for I2C classes and won't auto-detect %s devices anymore. "
			"If you need it, check 'Documentation/i2c/instantiating-devices.rst' for alternatives.\n",
			driver->driver.name);
		return 0;
	}

	/* 如果 class 不匹配，就直接停止。 */
	if (!(adapter->class & driver->class))
		return 0;

	/* 创建一个临时 client，供 detect() 回调使用。 */
	temp_client = kzalloc_obj(*temp_client);
	if (!temp_client)
		return -ENOMEM;

	temp_client->adapter = adapter;

	for (i = 0; address_list[i] != I2C_CLIENT_END; i += 1) {
		dev_dbg(&adapter->dev,
			"found normal entry for adapter %d, addr 0x%02x\n",
			i2c_adapter_id(adapter), address_list[i]);
		temp_client->addr = address_list[i];
		err = i2c_detect_address(temp_client, driver);
		if (unlikely(err))
			break;
	}

	kfree(temp_client);

	return err;
}

int i2c_probe_func_quick_read(struct i2c_adapter *adap, unsigned short addr)
{
	return i2c_smbus_xfer(adap, addr, 0, I2C_SMBUS_READ, 0,
			      I2C_SMBUS_QUICK, NULL) >= 0;
}
EXPORT_SYMBOL_GPL(i2c_probe_func_quick_read);

struct i2c_client *
i2c_new_scanned_device(struct i2c_adapter *adap,
		       struct i2c_board_info *info,
		       unsigned short const *addr_list,
		       int (*probe)(struct i2c_adapter *adap, unsigned short addr))
{
	int i;

	if (!probe)
		probe = i2c_default_probe;

	for (i = 0; addr_list[i] != I2C_CLIENT_END; i++) {
		/* 检查地址是否合法。 */
		if (i2c_check_7bit_addr_validity_strict(addr_list[i]) < 0) {
			dev_warn(&adap->dev, "Invalid 7-bit address 0x%02x\n",
				 addr_list[i]);
			continue;
		}

		/* 检查地址是否可用。这里是 7 位地址，不需要编码 flags。 */
		if (i2c_check_addr_busy(adap, addr_list[i])) {
			dev_dbg(&adap->dev,
				"Address 0x%02x already in use, not probing\n",
				addr_list[i]);
			continue;
		}

		/* 测试这个地址是否有响应。 */
		if (probe(adap, addr_list[i]))
			break;
	}

	if (addr_list[i] == I2C_CLIENT_END) {
		dev_dbg(&adap->dev, "Probing failed, no device found\n");
		return ERR_PTR(-ENODEV);
	}

	info->addr = addr_list[i];
	return i2c_new_client_device(adap, info);
}
EXPORT_SYMBOL_GPL(i2c_new_scanned_device);

struct i2c_adapter *i2c_get_adapter(int nr)
{
	struct i2c_adapter *adapter;

	mutex_lock(&core_lock);
	adapter = idr_find(&i2c_adapter_idr, nr);
	if (!adapter)
		goto exit;

	if (try_module_get(adapter->owner))
		get_device(&adapter->dev);
	else
		adapter = NULL;

 exit:
	mutex_unlock(&core_lock);
	return adapter;
}
EXPORT_SYMBOL(i2c_get_adapter);

void i2c_put_adapter(struct i2c_adapter *adap)
{
	if (!adap)
		return;

	module_put(adap->owner);
	/* 这个 put 操作必须放在最后，否则可能引发对 adap 的 use-after-free。 */
	put_device(&adap->dev);
}
EXPORT_SYMBOL(i2c_put_adapter);

/**
 * i2c_get_dma_safe_msg_buf() - 为指定 i2c_msg 获取 DMA 安全缓冲区
 * @msg: 要检查的消息
 * @threshold: 启用 DMA 的最小字节数，至少应为 1
 *
 * 如果没有拿到 DMA 安全缓冲区，则返回 NULL，这时继续使用 msg->buf
 * 进行 PIO 传输即可。否则返回一个可用于 DMA 的有效指针。用完后，
 * 需要调用 i2c_put_dma_safe_msg_buf() 归还。
 *
 * 这个函数只能在进程上下文中调用！
 */
u8 *i2c_get_dma_safe_msg_buf(struct i2c_msg *msg, unsigned int threshold)
{
	/* threshold 为 0 时也顺带跳过 0 长度消息。 */
	if (!threshold)
		pr_debug("DMA buffer for addr=0x%02x with length 0 is bogus\n",
			 msg->addr);
	if (msg->len < threshold || msg->len == 0)
		return NULL;

	if (msg->flags & I2C_M_DMA_SAFE)
		return msg->buf;

	pr_debug("using bounce buffer for addr=0x%02x, len=%d\n",
		 msg->addr, msg->len);

	if (msg->flags & I2C_M_RD)
		return kzalloc(msg->len, GFP_KERNEL);
	else
		return kmemdup(msg->buf, msg->len, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(i2c_get_dma_safe_msg_buf);

/**
 * i2c_put_dma_safe_msg_buf - 释放 DMA 安全缓冲区并回写到 i2c_msg
 * @buf: 从 i2c_get_dma_safe_msg_buf() 得到的缓冲区，可能为 NULL
 * @msg: 该缓冲区对应的消息
 * @xferred: 该消息是否已经成功传输
 */
void i2c_put_dma_safe_msg_buf(u8 *buf, struct i2c_msg *msg, bool xferred)
{
	if (!buf || buf == msg->buf)
		return;

	if (xferred && msg->flags & I2C_M_RD)
		memcpy(msg->buf, buf, msg->len);

	kfree(buf);
}
EXPORT_SYMBOL_GPL(i2c_put_dma_safe_msg_buf);

MODULE_AUTHOR("Simon G. Vogl <simon@tk.uni-linz.ac.at>");
MODULE_DESCRIPTION("I2C-Bus main module");
MODULE_LICENSE("GPL");
