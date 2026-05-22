// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core 的 OF 组件探测器代码
 *
 * Copyright (C) 2024 Google LLC
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/i2c-of-prober.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/stddef.h>

/*
 * 某些设备，例如 Google Hana Chromebook，会由多个厂商分别提供
 * 各自偏好的器件，而这些器件都被写进了设备树。与其把所有器件
 * 都启用后再让各自驱动去抢共享资源，不如把它们标记为
 * "fail-needs-probe"，再由一个探测器预先判断到底实际用了哪一颗。
 *
 * 这个探测器假定这些可替换器件挂在同一条 I2C 总线上，地址互不
 * 冲突，并且可以通过探测哪个地址有响应来直接确认。
 *
 * TODO:
 * - 支持 I2C mux
 */

static struct device_node *i2c_of_probe_get_i2c_node(struct device *dev, const char *type)
{
	struct device_node *node __free(device_node) = of_find_node_by_name(NULL, type);
	if (!node) {
		dev_err(dev, "找不到 %s 设备节点\n", type);
		return NULL;
	}

	struct device_node *i2c_node __free(device_node) = of_get_parent(node);
	if (!of_node_name_eq(i2c_node, "i2c")) {
		dev_err(dev, "%s 设备不在 I2C 总线上\n", type);
		return NULL;
	}

	if (!of_device_is_available(i2c_node)) {
		dev_err(dev, "I2C 控制器不可用\n");
		return NULL;
	}

	return no_free_ptr(i2c_node);
}

static int i2c_of_probe_enable_node(struct device *dev, struct device_node *node)
{
	int ret;

	dev_dbg(dev, "启用 %pOF\n", node);

	struct of_changeset *ocs __free(kfree) = kzalloc_obj(*ocs);
	if (!ocs)
		return -ENOMEM;

	of_changeset_init(ocs);
	ret = of_changeset_update_prop_string(ocs, node, "status", "okay");
	if (ret)
		return ret;

	ret = of_changeset_apply(ocs);
	if (ret) {
		/* 释放前必须显式清理 ocs。 */
		of_changeset_destroy(ocs);
	} else {
		/*
		 * 这里刻意保留 ocs，因为只要变更还在生效，它就必须继续存在。
		 */
		void *ptr __always_unused = no_free_ptr(ocs);
	}

	return ret;
}

static const struct i2c_of_probe_ops i2c_of_probe_dummy_ops;

/**
 * i2c_of_probe_component() - 在同一条 I2C 总线上探测某种 type 的设备
 * @dev: 调用者的 &struct device 指针，仅用于 dev_printk() 日志
 * @cfg: 指向包含回调和其他探测选项的 &struct i2c_of_probe_cfg
 * @ctx: 回调使用的上下文数据
 *
 * 探测同一条 I2C 总线上、并且 status 标记为 "fail-needs-probe" 的
 * 同类型 I2C 组件（类型名由 &i2c_of_probe_cfg->type 指定）。
 *
 * 假设整个设备树里，只有那些以 "type" 为前缀的节点名称（不包含
 * 地址部分）才是需要处理的第二来源组件。换句话说，如果 type 是
 * "touchscreen"，那么所有名为 "touchscreen*" 的节点都是待探测
 * 对象，不应该再有其它已经启用的 "touchscreen*" 节点。
 *
 * 还假设每一种 type 的组件最终只会有一个实际存在，也就是只会启用
 * 一个匹配且存在的设备。
 *
 * Context: 只能在进程上下文中调用，会进行非原子的 I2C 传输。
 *          最好只在驱动的 probe 函数里调用，因为当 I2C 适配器或
 *          其它资源不可用时，它可能返回 -EPROBE_DEFER。
 * Return: 成功或无操作返回 0，其它情况返回错误码。
 *         当看起来设备树里已经启用了待探测类型的组件时，可能会
 *         什么都不做。这可能是因为设备树还没有更新为
 *         "fail-needs-probe"，也可能是这个函数已经用同样参数运行
 *         过并成功启用了一个组件。后者常见于：用户有多个组件要探测，
 *         而列表中较靠后的某个组件触发了 deferred probe。这是预期行为。
 */
int i2c_of_probe_component(struct device *dev, const struct i2c_of_probe_cfg *cfg, void *ctx)
{
	const struct i2c_of_probe_ops *ops;
	const char *type;
	struct i2c_adapter *i2c;
	int ret;

	ops = cfg->ops ?: &i2c_of_probe_dummy_ops;
	type = cfg->type;

	struct device_node *i2c_node __free(device_node) = i2c_of_probe_get_i2c_node(dev, type);
	if (!i2c_node)
		return -ENODEV;

	/*
	 * 如果给定 type 的设备已经有任何一个处于启用状态，那么这个
	 * 函数就直接无操作返回。
	 * 这通常表示设备树还没有按这个探测器的要求改造，或者这个
	 * 函数之前已经运行过并成功启用了某个组件。
	 */
	for_each_child_of_node_with_prefix(i2c_node, node, type)
		if (of_device_is_available(node))
			return 0;

	i2c = of_get_i2c_adapter_by_node(i2c_node);
	if (!i2c)
		return dev_err_probe(dev, -EPROBE_DEFER, "Couldn't get I2C adapter\n");

	/* 申请并启用资源。 */
	ret = 0;
	if (ops->enable)
		ret = ops->enable(dev, i2c_node, ctx);
	if (ret)
		goto out_put_i2c_adapter;

	for_each_child_of_node_with_prefix(i2c_node, node, type) {
		union i2c_smbus_data data;
		u32 addr;

		if (of_property_read_u32(node, "reg", &addr))
			continue;
		if (i2c_smbus_xfer(i2c, addr, 0, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data) < 0)
			continue;

		/* 找到了一个会响应的设备。 */
		if (ops->cleanup_early)
			ops->cleanup_early(dev, ctx);
		ret = i2c_of_probe_enable_node(dev, node);
		break;
	}

	if (ops->cleanup)
		ops->cleanup(dev, ctx);
out_put_i2c_adapter:
	i2c_put_adapter(i2c);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(i2c_of_probe_component, "I2C_OF_PROBER");

static int i2c_of_probe_simple_get_supply(struct device *dev, struct device_node *node,
					  struct i2c_of_probe_simple_ctx *ctx)
{
	const char *supply_name;
	struct regulator *supply;

	/*
	 * 组件的设备节点完全可能没有 regulator 供电描述。
	 * 从硬件角度看这未必合理，但供电也可能一直开启，或者根本
	 * 没有在设备树里建模；即便如此，设备仍然可能正常工作。
	 */
	supply_name = ctx->opts->supply_name;
	if (!supply_name)
		return 0;

	supply = of_regulator_get_optional(dev, node, supply_name);
	if (IS_ERR(supply)) {
		return dev_err_probe(dev, PTR_ERR(supply),
				     "Failed to get regulator supply \"%s\" from %pOF\n",
				     supply_name, node);
	}

	ctx->supply = supply;

	return 0;
}

static void i2c_of_probe_simple_put_supply(struct i2c_of_probe_simple_ctx *ctx)
{
	regulator_put(ctx->supply);
	ctx->supply = NULL;
}

static int i2c_of_probe_simple_enable_regulator(struct device *dev, struct i2c_of_probe_simple_ctx *ctx)
{
	int ret;

	if (!ctx->supply)
		return 0;

	dev_dbg(dev, "Enabling regulator supply \"%s\"\n", ctx->opts->supply_name);

	ret = regulator_enable(ctx->supply);
	if (ret)
		return ret;

	if (ctx->opts->post_power_on_delay_ms)
		msleep(ctx->opts->post_power_on_delay_ms);

	return 0;
}

static void i2c_of_probe_simple_disable_regulator(struct device *dev, struct i2c_of_probe_simple_ctx *ctx)
{
	if (!ctx->supply)
		return;

	dev_dbg(dev, "Disabling regulator supply \"%s\"\n", ctx->opts->supply_name);

	regulator_disable(ctx->supply);
}

static int i2c_of_probe_simple_get_gpiod(struct device *dev, struct device_node *node,
					 struct i2c_of_probe_simple_ctx *ctx)
{
	struct fwnode_handle *fwnode = of_fwnode_handle(node);
	struct gpio_desc *gpiod;
	const char *con_id;

	/* NULL 表示不需要 GPIO。 */
	if (!ctx->opts->gpio_name)
		return 0;

	/* 空字符串表示使用未命名 GPIO。 */
	if (!ctx->opts->gpio_name[0])
		con_id = NULL;
	else
		con_id = ctx->opts->gpio_name;

	gpiod = fwnode_gpiod_get_index(fwnode, con_id, 0, GPIOD_ASIS, "i2c-of-prober");
	if (IS_ERR(gpiod))
		return PTR_ERR(gpiod);

	ctx->gpiod = gpiod;

	return 0;
}

static void i2c_of_probe_simple_put_gpiod(struct i2c_of_probe_simple_ctx *ctx)
{
	gpiod_put(ctx->gpiod);
	ctx->gpiod = NULL;
}

static int i2c_of_probe_simple_set_gpio(struct device *dev, struct i2c_of_probe_simple_ctx *ctx)
{
	int ret;

	if (!ctx->gpiod)
		return 0;

	dev_dbg(dev, "Configuring GPIO\n");

	ret = gpiod_direction_output(ctx->gpiod, ctx->opts->gpio_assert_to_enable);
	if (ret)
		return ret;

	if (ctx->opts->post_gpio_config_delay_ms)
		msleep(ctx->opts->post_gpio_config_delay_ms);

	return 0;
}

static void i2c_of_probe_simple_disable_gpio(struct device *dev, struct i2c_of_probe_simple_ctx *ctx)
{
	gpiod_set_value(ctx->gpiod, !ctx->opts->gpio_assert_to_enable);
}

/**
 * i2c_of_probe_simple_enable - I2C OF prober 的简化资源申请/启用助手
 * @dev: 调用者的 &struct device 指针，仅用于 dev_printk() 日志
 * @bus_node: I2C 适配器对应的 &struct device_node
 * @data: 指向 &struct i2c_of_probe_simple_ctx 的上下文
 *
 * 如果设置了 &i2c_of_probe_simple_opts->supply_name，就申请对应的
 * regulator 供电。
 * 如果设置了 &i2c_of_probe_simple_opts->gpio_name，就申请对应的 GPIO；
 * 如果它是空字符串，则申请未命名 GPIO。
 * 如果找到了 regulator，就把它打开。
 * 如果找到了 GPIO，就把这条 GPIO 配置成输出，并按照选项设置电平。
 *
 * Return: 成功或无操作返回 0，失败返回负错误码。
 */
int i2c_of_probe_simple_enable(struct device *dev, struct device_node *bus_node, void *data)
{
	struct i2c_of_probe_simple_ctx *ctx = data;
	struct device_node *node;
	const char *compat;
	int ret;

	dev_dbg(dev, "Requesting resources for components under I2C bus %pOF\n", bus_node);

	if (!ctx || !ctx->opts)
		return -EINVAL;

	compat = ctx->opts->res_node_compatible;
	if (!compat)
		return -EINVAL;

	node = of_get_compatible_child(bus_node, compat);
	if (!node)
		return dev_err_probe(dev, -ENODEV, "No device compatible with \"%s\" found\n",
				     compat);

	ret = i2c_of_probe_simple_get_supply(dev, node, ctx);
	if (ret)
		goto out_put_node;

	ret = i2c_of_probe_simple_get_gpiod(dev, node, ctx);
	if (ret)
		goto out_put_supply;

	ret = i2c_of_probe_simple_enable_regulator(dev, ctx);
	if (ret)
		goto out_put_gpiod;

	ret = i2c_of_probe_simple_set_gpio(dev, ctx);
	if (ret)
		goto out_disable_regulator;

	return 0;

out_disable_regulator:
	i2c_of_probe_simple_disable_regulator(dev, ctx);
out_put_gpiod:
	i2c_of_probe_simple_put_gpiod(ctx);
out_put_supply:
	i2c_of_probe_simple_put_supply(ctx);
out_put_node:
	of_node_put(node);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(i2c_of_probe_simple_enable, "I2C_OF_PROBER");

/**
 * i2c_of_probe_simple_cleanup_early - 在组件启用前提前释放 GPIO
 * @dev: 调用者的 &struct device 指针；未使用
 * @data: 指向 &struct i2c_of_probe_simple_ctx 的上下文
 *
 * GPIO 描述符是独占资源，必须在真实驱动 probe 之前释放，
 * 这样后者才能重新申请它们。
 */
void i2c_of_probe_simple_cleanup_early(struct device *dev, void *data)
{
	struct i2c_of_probe_simple_ctx *ctx = data;

	i2c_of_probe_simple_put_gpiod(ctx);
}
EXPORT_SYMBOL_NS_GPL(i2c_of_probe_simple_cleanup_early, "I2C_OF_PROBER");

/**
 * i2c_of_probe_simple_cleanup - 清理并释放 I2C OF prober 简化助手的资源
 * @dev: 调用者的 &struct device 指针，仅用于 dev_printk() 日志
 * @data: 指向 &struct i2c_of_probe_simple_ctx 的上下文
 *
 * * 如果找到了 GPIO 且还没有释放，就把它的值设为
 *   i2c_of_probe_simple_enable() 中设置值的反相，然后释放它。
 * * 如果找到了 regulator，就关闭该 regulator 并释放它。
 */
void i2c_of_probe_simple_cleanup(struct device *dev, void *data)
{
	struct i2c_of_probe_simple_ctx *ctx = data;

	/* 如果已经调用过 i2c_of_probe_simple_cleanup_early，这里的 GPIO 操作就会变成空操作。 */
	i2c_of_probe_simple_disable_gpio(dev, ctx);
	i2c_of_probe_simple_put_gpiod(ctx);

	i2c_of_probe_simple_disable_regulator(dev, ctx);
	i2c_of_probe_simple_put_supply(ctx);
}
EXPORT_SYMBOL_NS_GPL(i2c_of_probe_simple_cleanup, "I2C_OF_PROBER");

struct i2c_of_probe_ops i2c_of_probe_simple_ops = {
	.enable = i2c_of_probe_simple_enable,
	.cleanup_early = i2c_of_probe_simple_cleanup_early,
	.cleanup = i2c_of_probe_simple_cleanup,
};
EXPORT_SYMBOL_NS_GPL(i2c_of_probe_simple_ops, "I2C_OF_PROBER");
