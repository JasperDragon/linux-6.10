/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __LINUX_SND_SOC_USB_H
#define __LINUX_SND_SOC_USB_H

#include <sound/soc.h>

enum snd_soc_usb_kctl {
	SND_SOC_USB_KCTL_CARD_ROUTE,
	SND_SOC_USB_KCTL_PCM_ROUTE,
};

/**
 * struct snd_soc_usb_device - USB 声卡在 SoC USB 框架中的实例描述
 * @card_idx: 关联的 sound card 索引
 * @chip_idx: USB 声芯片在阵列里的索引
 * @cpcm_idx: capture PCM 索引数组
 * @ppcm_idx: playback PCM 索引数组
 * @num_capture: capture 流数量
 * @num_playback: playback 流数量
 * @list: 挂到全局 USB SoC 列表里的节点
 **/
struct snd_soc_usb_device {
	int card_idx;
	int chip_idx;

	/* PCM index arrays */
	unsigned int *cpcm_idx; /* TODO: capture path is not tested yet */
	unsigned int *ppcm_idx;
	int num_capture; /* TODO: capture path is not tested yet */
	int num_playback;

	struct list_head list;
};

/**
 * struct snd_soc_usb - USB offload / route 管理上下文
 * @list: 全局链表节点
 * @component: 关联的 ASoC component
 * @connection_status_cb: 连接状态变化回调
 * @update_offload_route_info: 更新 offload route 信息的回调
 * @priv_data: 驱动私有数据
 **/
struct snd_soc_usb {
	struct list_head list;
	struct snd_soc_component *component;
	int (*connection_status_cb)(struct snd_soc_usb *usb,
				    struct snd_soc_usb_device *sdev,
				    bool connected);
	int (*update_offload_route_info)(struct snd_soc_component *component,
					 int card, int pcm, int direction,
					 enum snd_soc_usb_kctl path,
					 long *route);
	void *priv_data;
};

#if IS_ENABLED(CONFIG_SND_SOC_USB)
/* 根据 card_idx 和 hw_params 判断一个 USB 设备是否支持当前格式。 */
int snd_soc_usb_find_supported_format(int card_idx,
				      struct snd_pcm_hw_params *params,
				      int direction);

/* USB 设备连接/断开通知。 */
int snd_soc_usb_connect(struct device *usbdev, struct snd_soc_usb_device *sdev);
int snd_soc_usb_disconnect(struct device *usbdev, struct snd_soc_usb_device *sdev);
/* 取回挂在 USB 设备上的私有数据。 */
void *snd_soc_usb_find_priv_data(struct device *usbdev);

/* 给 USB offload 相关场景创建 jack。 */
int snd_soc_usb_setup_offload_jack(struct snd_soc_component *component,
				   struct snd_soc_jack *jack);
/* 更新 offload route 的 card/pcm 映射。 */
int snd_soc_usb_update_offload_route(struct device *dev, int card, int pcm,
				     int direction, enum snd_soc_usb_kctl path,
				     long *route);

/* 申请 / 释放 / 注册 / 注销一个 USB SoC 端口。 */
struct snd_soc_usb *snd_soc_usb_allocate_port(struct snd_soc_component *component,
					      void *data);
void snd_soc_usb_free_port(struct snd_soc_usb *usb);
void snd_soc_usb_add_port(struct snd_soc_usb *usb);
void snd_soc_usb_remove_port(struct snd_soc_usb *usb);
#else
static inline int
snd_soc_usb_find_supported_format(int card_idx, struct snd_pcm_hw_params *params,
				  int direction)
{
	return -EINVAL;
}

static inline int snd_soc_usb_connect(struct device *usbdev,
				      struct snd_soc_usb_device *sdev)
{
	return -ENODEV;
}

static inline int snd_soc_usb_disconnect(struct device *usbdev,
					 struct snd_soc_usb_device *sdev)
{
	return -EINVAL;
}

static inline void *snd_soc_usb_find_priv_data(struct device *usbdev)
{
	return NULL;
}

static inline int snd_soc_usb_setup_offload_jack(struct snd_soc_component *component,
						 struct snd_soc_jack *jack)
{
	return 0;
}

static int snd_soc_usb_update_offload_route(struct device *dev, int card, int pcm,
					    int direction, enum snd_soc_usb_kctl path,
					    long *route)
{
	return -ENODEV;
}

static inline struct snd_soc_usb *
snd_soc_usb_allocate_port(struct snd_soc_component *component, void *data)
{
	return ERR_PTR(-ENOMEM);
}

static inline void snd_soc_usb_free_port(struct snd_soc_usb *usb)
{ }

static inline void snd_soc_usb_add_port(struct snd_soc_usb *usb)
{ }

static inline void snd_soc_usb_remove_port(struct snd_soc_usb *usb)
{ }
#endif /* IS_ENABLED(CONFIG_SND_SOC_USB) */
#endif /*__LINUX_SND_SOC_USB_H */
