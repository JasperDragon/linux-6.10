// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/of.h>
#include <linux/usb.h>

#include <sound/jack.h>
#include <sound/soc-usb.h>

#include "../usb/card.h"

static DEFINE_MUTEX(ctx_mutex);
static LIST_HEAD(usb_ctx_list);

static struct device_node *snd_soc_find_phandle(struct device *dev)
{
	struct device_node *node;

	node = of_parse_phandle(dev->of_node, "usb-soc-be", 0);
	if (!node)
		return ERR_PTR(-ENODEV);

	return node;
}

static struct snd_soc_usb *snd_soc_usb_ctx_lookup(struct device_node *node)
{
	struct snd_soc_usb *ctx;

	if (!node)
		return NULL;

	list_for_each_entry(ctx, &usb_ctx_list, list) {
		if (ctx->component->dev->of_node == node)
			return ctx;
	}

	return NULL;
}

static struct snd_soc_usb *snd_soc_find_usb_ctx(struct device *dev)
{
	struct snd_soc_usb *ctx;
	struct device_node *node;

	node = snd_soc_find_phandle(dev);
	if (!IS_ERR(node)) {
		ctx = snd_soc_usb_ctx_lookup(node);
		of_node_put(node);
	} else {
		ctx = snd_soc_usb_ctx_lookup(dev->of_node);
	}

	return ctx ? ctx : NULL;
}

/* SoC USB 声卡控制相关逻辑。 */
/**
 * snd_soc_usb_setup_offload_jack() - 创建 USB offload jack
 * @component: USB DPCM 后端 DAI component
 * @jack: 要创建的 jack 结构体
 *
 * 创建一个 jack 设备，用于通知用户空间当前设备是否支持 offload。
 *
 * 返回 0 表示成功，负值表示错误。
 */
int snd_soc_usb_setup_offload_jack(struct snd_soc_component *component,
				   struct snd_soc_jack *jack)
{
	int ret;

	ret = snd_soc_card_jack_new(component->card, "USB Offload Jack",
				    SND_JACK_USB, jack);
	if (ret < 0) {
		dev_err(component->card->dev, "Unable to add USB offload jack: %d\n",
			ret);
		return ret;
	}

	ret = snd_soc_component_set_jack(component, jack, NULL);
	if (ret) {
		dev_err(component->card->dev, "Failed to set jack: %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_setup_offload_jack);

/**
 * snd_soc_usb_update_offload_route - 查询当前 USB offload 路径
 * @dev: 用于查询 offload 状态的 USB 设备
 * @card: USB card 索引
 * @pcm: USB PCM 设备索引
 * @direction: 播放或录音方向
 * @path: 查询 card 还是 pcm 索引
 * @route: 路由输出数组指针
 *
 * 读取指定 USB 声卡和 PCM 设备索引的当前状态。
 * route 参数应该是一个用于 kcontrol 输出的整数数组；
 * 第一个元素保存选择的 card 索引，第二个元素保存选择的 PCM 索引。
 */
int snd_soc_usb_update_offload_route(struct device *dev, int card, int pcm,
				     int direction, enum snd_soc_usb_kctl path,
				     long *route)
{
	struct snd_soc_usb *ctx;
	int ret = -ENODEV;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(dev);
	if (!ctx)
		goto exit;

	if (ctx->update_offload_route_info)
		ret = ctx->update_offload_route_info(ctx->component, card, pcm,
						     direction, path, route);
exit:
	mutex_unlock(&ctx_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_update_offload_route);

/**
 * snd_soc_usb_find_priv_data() - 取回保存的私有数据
 * @usbdev: 设备引用
 *
 * 取回 USB SoC 结构体中保存的私有数据。
 */
void *snd_soc_usb_find_priv_data(struct device *usbdev)
{
	struct snd_soc_usb *ctx;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(usbdev);
	mutex_unlock(&ctx_mutex);

	return ctx ? ctx->priv_data : NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_find_priv_data);

/**
 * snd_soc_usb_find_supported_format() - 检查音频格式是否受支持
 * @card_idx: USB 声芯片数组索引
 * @params: PCM 参数
 * @direction: 播放或录音方向
 *
 * 确认 ASoC 侧请求的音频配置能够被 USB 设备支持。
 *
 * 返回 0 表示成功，负值表示错误。
 */
int snd_soc_usb_find_supported_format(int card_idx,
				      struct snd_pcm_hw_params *params,
				      int direction)
{
	struct snd_usb_stream *as;

	as = snd_usb_find_suppported_substream(card_idx, params, direction);
	if (!as)
		return -EOPNOTSUPP;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_find_supported_format);

/**
 * snd_soc_usb_allocate_port() - 为 offload 支持分配 SoC USB 端口
 * @component: USB DPCM 后端 DAI component
 * @data: 私有数据
 *
 * 分配并初始化一个 SoC USB 端口。
 * 该端口用于和不同的 USB 音频设备通信，以便由 ASoC 实体接管 offload。
 * USB 设备的插入/拔出会通过通知上报，但不会直接影响这个端口的内存分配。
 */
struct snd_soc_usb *snd_soc_usb_allocate_port(struct snd_soc_component *component,
					      void *data)
{
	struct snd_soc_usb *usb;

	usb = kzalloc_obj(*usb);
	if (!usb)
		return ERR_PTR(-ENOMEM);

	usb->component = component;
	usb->priv_data = data;

	return usb;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_allocate_port);

/**
 * snd_soc_usb_free_port() - 释放用于 offload 支持的 SoC USB 端口
 * @usb: 已分配的 SoC USB 端口
 *
 * 从可用端口列表中移除并释放该端口，确保 USB SND 和 ASoC 的通信停止。
 */
void snd_soc_usb_free_port(struct snd_soc_usb *usb)
{
	snd_soc_usb_remove_port(usb);
	kfree(usb);
}
EXPORT_SYMBOL_GPL(snd_soc_usb_free_port);

/**
 * snd_soc_usb_add_port() - 添加一个 USB 后端端口
 * @usb: 要添加的 SoC USB 端口
 *
 * 将一个 USB 后端 DAI link 注册到 USB SoC 框架。
 * 端口相关内存由后端 DAI link 负责管理。
 */
void snd_soc_usb_add_port(struct snd_soc_usb *usb)
{
	mutex_lock(&ctx_mutex);
	list_add_tail(&usb->list, &usb_ctx_list);
	mutex_unlock(&ctx_mutex);

	snd_usb_rediscover_devices();
}
EXPORT_SYMBOL_GPL(snd_soc_usb_add_port);

/**
 * snd_soc_usb_remove_port() - 移除一个 USB 后端端口
 * @usb: 要移除的 SoC USB 端口
 *
 * 从 USB SoC 框架中移除 USB 后端 DAI link。
 * 相关内存在后端 DAI 删除或调用 snd_soc_usb_free_port() 时释放。
 */
void snd_soc_usb_remove_port(struct snd_soc_usb *usb)
{
	struct snd_soc_usb *ctx, *tmp;

	mutex_lock(&ctx_mutex);
	list_for_each_entry_safe(ctx, tmp, &usb_ctx_list, list) {
		if (ctx == usb) {
			list_del(&ctx->list);
			break;
		}
	}
	mutex_unlock(&ctx_mutex);
}
EXPORT_SYMBOL_GPL(snd_soc_usb_remove_port);

/**
 * snd_soc_usb_connect() - USB 设备连接通知
 * @usbdev: USB 总线设备
 * @sdev: 要添加的 USB SND 设备
 *
 * 通知一个新的 USB SND 设备已连接。
 * sdev->card_idx 可用于控制 DPCM 后端选择哪个设备来启用 USB offload。
 */
int snd_soc_usb_connect(struct device *usbdev, struct snd_soc_usb_device *sdev)
{
	struct snd_soc_usb *ctx;

	if (!usbdev)
		return -ENODEV;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(usbdev);
	if (!ctx)
		goto exit;

	if (ctx->connection_status_cb)
		ctx->connection_status_cb(ctx, sdev, true);

exit:
	mutex_unlock(&ctx_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_connect);

/**
 * snd_soc_usb_disconnect() - USB 设备断开通知
 * @usbdev: USB 总线设备
 * @sdev: 要移除的 USB SND 设备
 *
 * 通知 USB 后端有新的 USB SND 设备断开。
 */
int snd_soc_usb_disconnect(struct device *usbdev, struct snd_soc_usb_device *sdev)
{
	struct snd_soc_usb *ctx;

	if (!usbdev)
		return -ENODEV;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(usbdev);
	if (!ctx)
		goto exit;

	if (ctx->connection_status_cb)
		ctx->connection_status_cb(ctx, sdev, false);

exit:
	mutex_unlock(&ctx_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_disconnect);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SoC USB driver for offloading");
