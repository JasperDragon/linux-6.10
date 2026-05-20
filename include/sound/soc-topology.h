/* SPDX-License-Identifier: GPL-2.0
 *
 * linux/sound/soc-topology.h -- ALSA SoC Firmware Controls and DAPM
 *
 * Copyright (C) 2012 Texas Instruments Inc.
 * Copyright (C) 2015 Intel Corporation.
 *
 * Simple file API to load FW that includes mixers, coefficients, DAPM graphs,
 * algorithms, equalisers, DAIs, widgets, FE caps, BE caps, codec link caps etc.
 */

#ifndef __LINUX_SND_SOC_TPLG_H
#define __LINUX_SND_SOC_TPLG_H

#include <sound/asoc.h>
#include <linux/list.h>

struct firmware;
struct snd_kcontrol;
struct snd_soc_tplg_pcm_be;
struct snd_ctl_elem_value;
struct snd_ctl_elem_info;
struct snd_soc_dapm_widget;
struct snd_soc_component;
struct snd_soc_tplg_pcm_fe;
struct snd_soc_dapm_context;
struct snd_soc_card;
struct snd_kcontrol_new;
struct snd_soc_dai_link;
struct snd_soc_dai_driver;
struct snd_soc_dai;
struct snd_soc_dapm_route;

/*
 * topology 动态对象类型。
 * 固件加载后，mixer、enum、widget、DAI link、PCM 等对象都会被包装成
 * 可动态装卸的对象。
 */
enum snd_soc_dobj_type {
	SND_SOC_DOBJ_NONE		= 0,	/* 非动态对象 */
	SND_SOC_DOBJ_MIXER,
	SND_SOC_DOBJ_BYTES,
	SND_SOC_DOBJ_ENUM,
	SND_SOC_DOBJ_GRAPH,
	SND_SOC_DOBJ_WIDGET,
	SND_SOC_DOBJ_DAI_LINK,
	SND_SOC_DOBJ_PCM,
	SND_SOC_DOBJ_CODEC_LINK,
	SND_SOC_DOBJ_BACKEND_LINK,
};

/* 动态 control 对象：绑定 kcontrol 以及动态文本/值。 */
struct snd_soc_dobj_control {
	struct snd_kcontrol *kcontrol;
	char **dtexts;
	unsigned long *dvalues;
};

/* 动态 widget 对象：保存 kcontrol 类型等附加信息。 */
struct snd_soc_dobj_widget {
	unsigned int *kcontrol_type;	/* kcontrol type: mixer, enum, bytes */
};

/*
 * 通用动态对象。
 * 所有 topology 动态对象都挂在这个统一结构下，便于 core 统一管理。
 */
struct snd_soc_dobj {
	enum snd_soc_dobj_type type;
	unsigned int index;	/* objects can belong in different groups */
	struct list_head list;
	int (*unload)(struct snd_soc_component *comp, struct snd_soc_dobj *dobj);
	union {
		struct snd_soc_dobj_control control;
		struct snd_soc_dobj_widget widget;
	};
	void *private; /* core does not touch this */
};

/*
 * topology 中的 kcontrol 操作映射。
 * 当 firmware / 拓扑文件定义了控件，但真正的 get/put/info 逻辑仍由
 * 驱动提供时，就通过这组 ops 做绑定。
 */
struct snd_soc_tplg_kcontrol_ops {
	u32 id;
	int (*get)(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol);
	int (*put)(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol);
	int (*info)(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_info *uinfo);
};

/* bytes ext 操作，主要用于 TLV / 字节型控制。 */
struct snd_soc_tplg_bytes_ext_ops {
	u32 id;
	int (*get)(struct snd_kcontrol *kcontrol, unsigned int __user *bytes,
							unsigned int size);
	int (*put)(struct snd_kcontrol *kcontrol,
			const unsigned int __user *bytes, unsigned int size);
};

/* widget 事件处理映射。 */
struct snd_soc_tplg_widget_events {
	u16 type;
	int (*event_handler)(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event);
};

/*
 * topology 公共加载接口。
 * component driver 通过这些 ops 让 core 加载 / 卸载来自 firmware 的动态对象。
 */
struct snd_soc_tplg_ops {

	/* external kcontrol init - used for any driver specific init */
	int (*control_load)(struct snd_soc_component *, int index,
		struct snd_kcontrol_new *, struct snd_soc_tplg_ctl_hdr *);
	int (*control_unload)(struct snd_soc_component *,
		struct snd_soc_dobj *);

	/* DAPM graph route element loading and unloading */
	int (*dapm_route_load)(struct snd_soc_component *, int index,
		struct snd_soc_dapm_route *route);
	int (*dapm_route_unload)(struct snd_soc_component *,
		struct snd_soc_dobj *);

	/* external widget init - used for any driver specific init */
	int (*widget_load)(struct snd_soc_component *, int index,
		struct snd_soc_dapm_widget *,
		struct snd_soc_tplg_dapm_widget *);
	int (*widget_ready)(struct snd_soc_component *, int index,
		struct snd_soc_dapm_widget *,
		struct snd_soc_tplg_dapm_widget *);
	int (*widget_unload)(struct snd_soc_component *,
		struct snd_soc_dobj *);

	/* FE DAI - used for any driver specific init */
	int (*dai_load)(struct snd_soc_component *, int index,
		struct snd_soc_dai_driver *dai_drv,
		struct snd_soc_tplg_pcm *pcm, struct snd_soc_dai *dai);

	int (*dai_unload)(struct snd_soc_component *,
		struct snd_soc_dobj *);

	/* DAI link - used for any driver specific init */
	int (*link_load)(struct snd_soc_component *, int index,
		struct snd_soc_dai_link *link,
		struct snd_soc_tplg_link_config *cfg);
	int (*link_unload)(struct snd_soc_component *,
		struct snd_soc_dobj *);

	/* callback to handle vendor bespoke data */
	int (*vendor_load)(struct snd_soc_component *, int index,
		struct snd_soc_tplg_hdr *);
	int (*vendor_unload)(struct snd_soc_component *,
		struct snd_soc_tplg_hdr *);

	/* completion - called at completion of firmware loading */
	int (*complete)(struct snd_soc_component *comp);

	/* manifest - optional to inform component of manifest */
	int (*manifest)(struct snd_soc_component *, int index,
		struct snd_soc_tplg_manifest *);

	/* vendor specific kcontrol handlers available for binding */
	const struct snd_soc_tplg_kcontrol_ops *io_ops;
	int io_ops_count;

	/* vendor specific bytes ext handlers available for binding */
	const struct snd_soc_tplg_bytes_ext_ops *bytes_ext_ops;
	int bytes_ext_ops_count;
};

#ifdef CONFIG_SND_SOC_TOPOLOGY

/* 从 firmware block header 后面取出数据区指针。 */
static inline const void *snd_soc_tplg_get_data(struct snd_soc_tplg_hdr *hdr)
{
	const void *ptr = hdr;

	return ptr + sizeof(*hdr);
}

/* component driver 的动态对象加载 / 移除。 */
int snd_soc_tplg_component_load(struct snd_soc_component *comp,
	const struct snd_soc_tplg_ops *ops, const struct firmware *fw);
int snd_soc_tplg_component_remove(struct snd_soc_component *comp);

/* 给动态 widget 绑定事件处理函数。 */
int snd_soc_tplg_widget_bind_event(struct snd_soc_dapm_widget *w,
	const struct snd_soc_tplg_widget_events *events, int num_events,
	u16 event_type);

#else

static inline int snd_soc_tplg_component_remove(struct snd_soc_component *comp)
{
	return 0;
}

#endif

#endif
