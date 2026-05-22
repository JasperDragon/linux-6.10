/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Linux I2C OF 组件探测器的公共定义
 *
 * Copyright (C) 2024 Google LLC
 */

#ifndef _LINUX_I2C_OF_PROBER_H
#define _LINUX_I2C_OF_PROBER_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct device_node;

/**
 * struct i2c_of_probe_ops - I2C OF 组件探测器回调集合
 *
 * 这组回调用于 i2c_of_probe_component()。
 *
 * 所有回调都是可选的。每轮探测中，每个回调最多调用一次，并严格按
 * 结构体里定义的顺序执行。
 *
 * 有返回值的回调在成功时应返回 %0，失败时返回负 errno。
 *
 * 传给回调的 @dev 与传给 i2c_of_probe_component() 的 @dev 相同。
 * 它只应该用于 dev_printk() 这类日志用途，不应拿去做别的事情，
 * 尤其不要调用受管理资源（devres）接口。
 */
struct i2c_of_probe_ops {
	/**
	 * @enable: 获取并使能资源，让组件能对 probe 有响应
	 *
	 * 这个回调允许返回 -EPROBE_DEFER，因为典型用途就包括“先取资源，
	 * 再把组件上电/复位释放，使其能够被探测”。如果中途失败，返回前必须
	 * 把资源恢复到初始状态并释放干净。
	 */
	int (*enable)(struct device *dev, struct device_node *bus_node, void *data);

	/**
	 * @cleanup_early: 在对已发现组件调用 probe() 前，提前释放独占资源
	 *
	 * 只有真正找到了匹配组件时才会调用。
	 * 如果最终没有找到任何组件，那么本应在这里释放的资源，应改由
	 * @cleanup 收尾。
	 */
	void (*cleanup_early)(struct device *dev, void *data);

	/**
	 * @cleanup: 与 @enable 相对的收尾操作，用于平衡引用计数并释放资源
	 *
	 * 实现时应自行判断资源是否已经在 @cleanup_early 中释放过。
	 */
	void (*cleanup)(struct device *dev, void *data);
};

/**
 * struct i2c_of_probe_cfg - I2C OF 组件探测器配置
 * @ops:  探测器要使用的回调集合
 * @type: 用于匹配设备节点名前缀的字符串
 */
struct i2c_of_probe_cfg {
	const struct i2c_of_probe_ops *ops;
	const char *type;
};

#if IS_ENABLED(CONFIG_OF_DYNAMIC)

int i2c_of_probe_component(struct device *dev, const struct i2c_of_probe_cfg *cfg, void *ctx);

/**
 * DOC: I2C OF 组件探测器的简化辅助函数
 *
 * 触摸板这类组件通常通过 6 针排线接到主板上。除了 I2C 总线、IRQ 和地，
 * 往往只剩下一路电源和一路 GPIO（常见是 enable 或 reset）。
 * 集成在显示面板上的触摸屏，连接关系通常也与此类似。
 *
 * 因此这里提供了一组简单 helper，专门服务这类“最多一路 regulator +
 * 一路 GPIO”的 I2C 组件探测场景。
 *
 * 目前提供的 helper 有：
 * * i2c_of_probe_simple_enable()
 * * i2c_of_probe_simple_cleanup_early()
 * * i2c_of_probe_simple_cleanup()
 */

/**
 * struct i2c_of_probe_simple_opts - 简化 I2C 组件探测回调的配置项
 * @res_node_compatible: 用于寻找资源节点的 compatible 字符串
 * @supply_name:         regulator 供电名
 * @gpio_name:           GPIO 名称。若不用 GPIO 则为 NULL；若 GPIO 无名字，
 *			 则传空字符串 ("")
 * @post_power_on_delay_ms:   供电打开后的延迟，传给 msleep()
 * @post_gpio_config_delay_ms: GPIO 配置完成后的延迟，传给 msleep()
 * @gpio_assert_to_enable: 若为 %true，则 GPIO 置为逻辑高表示使能组件
 *
 * 这描述的是一类常见上电时序：
 * * 先按 @gpio_assert_to_enable 的反向状态配置 @gpio_name
 * * 再使能 @supply_name 对应的 regulator
 * * 等待 @post_power_on_delay_ms
 * * 再按 @gpio_assert_to_enable 的有效状态配置 @gpio_name
 * * 最后等待 @post_gpio_config_delay_ms
 */
struct i2c_of_probe_simple_opts {
	const char *res_node_compatible;
	const char *supply_name;
	const char *gpio_name;
	unsigned int post_power_on_delay_ms;
	unsigned int post_gpio_config_delay_ms;
	bool gpio_assert_to_enable;
};

struct gpio_desc;
struct regulator;

struct i2c_of_probe_simple_ctx {
	/* 公共输入：由调用者在 helper 使用前填好。 */
	const struct i2c_of_probe_simple_opts *opts;
	/* 私有状态：供 helper 内部维护。 */
	struct regulator *supply;
	struct gpio_desc *gpiod;
};

int i2c_of_probe_simple_enable(struct device *dev, struct device_node *bus_node, void *data);
void i2c_of_probe_simple_cleanup_early(struct device *dev, void *data);
void i2c_of_probe_simple_cleanup(struct device *dev, void *data);

extern struct i2c_of_probe_ops i2c_of_probe_simple_ops;

#endif /* IS_ENABLED(CONFIG_OF_DYNAMIC) */

#endif /* _LINUX_I2C_OF_PROBER_H */
