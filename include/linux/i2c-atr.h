/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I2C 地址翻译器（Address Translator）公共接口
 *
 * Copyright (c) 2019,2022 Luca Ceresoli <luca@lucaceresoli.net>
 * Copyright (c) 2022,2023 Tomi Valkeinen <tomi.valkeinen@ideasonboard.com>
 *
 * 最初基于 i2c-mux.h
 */

#ifndef _LINUX_I2C_ATR_H
#define _LINUX_I2C_ATR_H

#include <linux/i2c.h>
#include <linux/types.h>

struct device;
struct fwnode_handle;
struct i2c_atr;

/**
 * enum i2c_atr_flags - ATR 驱动的工作模式标志
 *
 * @I2C_ATR_F_STATIC:
 *	ATR 不支持运行时动态映射，只能使用静态映射。
 *	映射项只会随着子总线上设备的创建/删除而增减，不会在一次普通
 *	传输中因为地址未命中而临时建立新映射。
 *	这要求 alias 池足够大，能够容纳所有预期会出现在子总线上的设备。
 * @I2C_ATR_F_PASSTHROUGH:
 *	允许未映射地址直接透传到父总线。
 *	这通常用于“不是每个地址都必须经过 ATR 改写”的硬件实现。
 */
enum i2c_atr_flags {
	I2C_ATR_F_STATIC = BIT(0),
	I2C_ATR_F_PASSTHROUGH = BIT(1),
};

/**
 * struct i2c_atr_ops - ATR 核心回调到底层硬件驱动的接口
 * @attach_addr:
 *	当某个子总线地址需要建立映射时调用。
 *	核心层已经为这个地址选好了 alias，底层驱动要把
 *	`(chan_id, addr) -> alias` 的关系编程到硬件 ATR 中。
 * @detach_addr:
 *	当某个地址不再需要映射时调用。
 *	底层驱动要撤掉该地址对应的硬件映射，避免旧 alias 继续生效。
 *
 * attach_addr() 成功返回 0，失败返回负的 errno。detach_addr() 没有返回值，
 * 调用方默认底层驱动能同步完成清理，因此实现时要保证它是可完成的。
 */
struct i2c_atr_ops {
	int (*attach_addr)(struct i2c_atr *atr, u32 chan_id,
			   u16 addr, u16 alias);
	void (*detach_addr)(struct i2c_atr *atr, u32 chan_id,
			    u16 addr);
};

/**
 * struct i2c_atr_adap_desc - ATR 下游子总线描述符
 * @chan_id:
 *	要创建的子适配器编号，范围是 `0 .. max_adapters - 1`。
 *	这个值会原样传给 struct i2c_atr_ops 回调，底层驱动通常用它来
 *	选择具体的硬件通道或端口。
 * @parent:
 *	新 i2c_adapter 在设备模型中的父设备。
 *	若为 NULL，则默认使用 i2c-atr 自身的设备对象作为父节点。
 * @bus_handle:
 *	指向这个子总线下游外设集合的固件节点。
 *	若为 NULL，核心层会回退到 `i2c-atr` 子节点中，按 `reg = chan_id`
 *	去自动匹配一个固件子节点。
 * @num_aliases:
 *	该子总线私有 alias 池中的地址数量。
 *	若为 0，则表示这个子适配器复用 ATR 的全局共享 alias 池。
 * @aliases:
 *	可选的私有 alias 数组。
 *	仅当 @num_aliases 大于 0 时才会使用，且必须准确提供
 *	`num_aliases` 个地址。
 */
struct i2c_atr_adap_desc {
	u32 chan_id;
	struct device *parent;
	struct fwnode_handle *bus_handle;
	size_t num_aliases;
	u16 *aliases;
};

/**
 * i2c_atr_new() - 创建并初始化一个 ATR 帮助对象
 * @parent: 上游父适配器
 * @dev:    实现 ATR 功能的设备对象
 * @ops:    底层驱动回调
 * @max_adapters: 最多允许创建多少个子适配器
 * @flags:  ATR 工作模式标志
 *
 * 这个函数只建立 ATR 框架本身：记录父总线、准备公共锁、解析共享
 * alias 池、注册总线 notifier。新建的 ATR 此时还没有任何子适配器，
 * 后续必须通过 i2c_atr_add_adapter() 逐个添加。
 *
 * 与之对应的销毁接口是 i2c_atr_delete()。
 *
 * Return: 成功时返回 ATR 对象指针，失败时返回 ERR_PTR()
 */
struct i2c_atr *i2c_atr_new(struct i2c_adapter *parent, struct device *dev,
			    const struct i2c_atr_ops *ops, int max_adapters,
			    u32 flags);

/**
 * i2c_atr_delete - 销毁一个 ATR 帮助对象
 * @atr: 要销毁的 ATR 对象
 *
 * 前提条件：所有通过 i2c_atr_add_adapter() 创建的子适配器都必须先
 * 用 i2c_atr_del_adapter() 删除干净。
 */
void i2c_atr_delete(struct i2c_atr *atr);

/**
 * i2c_atr_add_adapter - 创建一个下游子 I2C 总线
 * @atr:  ATR 对象
 * @desc: 子适配器描述符
 *
 * 调用后，系统里会出现一个新的 i2c_adapter。该子总线上设备的创建与
 * 删除，会经由 I2C bus notifier 转换成对底层驱动 attach/detach
 * 回调的调用，从而在硬件 ATR 中建立或撤销 alias 映射。
 *
 * 适配器的 fwnode 取自 @bus_handle；若 @bus_handle 为 NULL，则会在
 * `i2c-atr` 设备的 `i2c-atr` 子节点下，寻找 `reg == chan_id` 的节点。
 *
 * 对应的删除接口是 i2c_atr_del_adapter()。
 *
 * Return: 成功返回 0，失败返回负的 errno
 */
int i2c_atr_add_adapter(struct i2c_atr *atr, struct i2c_atr_adap_desc *desc);

/**
 * i2c_atr_del_adapter - 删除一个先前创建的下游子总线
 * @atr:     ATR 对象
 * @chan_id: 要删除的子适配器编号，范围是 `0 .. max_adapters - 1`
 *
 * 如果对应通道并不存在，这个函数什么也不做。
 */
void i2c_atr_del_adapter(struct i2c_atr *atr, u32 chan_id);

/**
 * i2c_atr_set_driver_data - 给 ATR 对象保存底层驱动私有数据
 * @atr:  ATR 对象
 * @data: 要保存的私有指针
 */
void i2c_atr_set_driver_data(struct i2c_atr *atr, void *data);

/**
 * i2c_atr_get_driver_data - 取回先前保存的驱动私有数据
 * @atr: ATR 对象
 *
 * Return: 之前通过 i2c_atr_set_driver_data() 保存的指针
 */
void *i2c_atr_get_driver_data(struct i2c_atr *atr);

#endif /* _LINUX_I2C_ATR_H */
