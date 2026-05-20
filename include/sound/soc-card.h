/* SPDX-License-Identifier: GPL-2.0
 *
 * soc-card.h
 *
 * Copyright (C) 2019 Renesas Electronics Corp.
 * Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 */
#ifndef __SOC_CARD_H
#define __SOC_CARD_H

/*
 * 这里的接口围绕 snd_soc_card 这一层展开。
 * card 是 ASoC 的板级抽象，负责把 codec、CPU DAI、platform、
 * jack、controls 和 PM 回调串起来。
 */
enum snd_soc_card_subclass {
	SND_SOC_CARD_CLASS_ROOT		= 0,
	SND_SOC_CARD_CLASS_RUNTIME	= 1,
};

/*
 * card->mutex 的嵌套锁顺序标记。
 *
 * card 级操作和 runtime 级操作可能在不同回调里交叉出现，使用 nested
 * lock subclass 可以让 lockdep 识别“同一把锁的不同语义入口”。
 */
static inline void snd_soc_card_mutex_lock_root(struct snd_soc_card *card)
{
	/* root 路径优先级更高，通常用于 card 绑定/解绑等全局操作。 */
	mutex_lock_nested(&card->mutex, SND_SOC_CARD_CLASS_ROOT);
}

/* runtime 场景下的 card 锁。 */
static inline void snd_soc_card_mutex_lock(struct snd_soc_card *card)
{
	/* runtime 路径用于 PCM / DAPM 相关的日常操作。 */
	mutex_lock_nested(&card->mutex, SND_SOC_CARD_CLASS_RUNTIME);
}

/* 对称解锁。 */
static inline void snd_soc_card_mutex_unlock(struct snd_soc_card *card)
{
	mutex_unlock(&card->mutex);
}

/* 根据 mixer 控件名，从整个声卡里查找对应的 kcontrol。 */
struct snd_kcontrol *snd_soc_card_get_kcontrol(struct snd_soc_card *soc_card,
					       const char *name);

/* 创建一个 jack，但不预先绑定 pin。 */
int snd_soc_card_jack_new(struct snd_soc_card *card, const char *id, int type,
			  struct snd_soc_jack *jack);

/* 创建一个 jack，并一次性绑定 pin 列表。 */
int snd_soc_card_jack_new_pins(struct snd_soc_card *card, const char *id,
			       int type, struct snd_soc_jack *jack,
			       struct snd_soc_jack_pin *pins,
			       unsigned int num_pins);

/* 板级 suspend/resume 前后回调。 */
int snd_soc_card_suspend_pre(struct snd_soc_card *card);
int snd_soc_card_suspend_post(struct snd_soc_card *card);
int snd_soc_card_resume_pre(struct snd_soc_card *card);
int snd_soc_card_resume_post(struct snd_soc_card *card);

/* card 生命周期：probe / late_probe / remove。 */
int snd_soc_card_probe(struct snd_soc_card *card);
int snd_soc_card_late_probe(struct snd_soc_card *card);
void snd_soc_card_fixup_controls(struct snd_soc_card *card);
int snd_soc_card_remove(struct snd_soc_card *card);

/* DAPM bias level 变化时，板级可插入额外处理。 */
int snd_soc_card_set_bias_level(struct snd_soc_card *card,
				struct snd_soc_dapm_context *dapm,
				enum snd_soc_bias_level level);
int snd_soc_card_set_bias_level_post(struct snd_soc_card *card,
				     struct snd_soc_dapm_context *dapm,
				     enum snd_soc_bias_level level);

/* 动态增删 DAI link 的板级钩子。 */
int snd_soc_card_add_dai_link(struct snd_soc_card *card,
			      struct snd_soc_dai_link *dai_link);
void snd_soc_card_remove_dai_link(struct snd_soc_card *card,
				  struct snd_soc_dai_link *dai_link);

#ifdef CONFIG_PCI
static inline void snd_soc_card_set_pci_ssid(struct snd_soc_card *card,
					     unsigned short vendor,
					     unsigned short device)
{
	/* PCI 场景下给 card 记录子系统 vendor/device。 */
	card->pci_subsystem_vendor = vendor;
	card->pci_subsystem_device = device;
	card->pci_subsystem_set = true;
}

static inline int snd_soc_card_get_pci_ssid(struct snd_soc_card *card,
					    unsigned short *vendor,
					    unsigned short *device)
{
	/* 只有显式设置过的 SSID 才会被返回。 */
	if (!card->pci_subsystem_set)
		return -ENOENT;

	*vendor = card->pci_subsystem_vendor;
	*device = card->pci_subsystem_device;

	return 0;
}
#else /* !CONFIG_PCI */
static inline void snd_soc_card_set_pci_ssid(struct snd_soc_card *card,
					     unsigned short vendor,
					     unsigned short device)
{
}

static inline int snd_soc_card_get_pci_ssid(struct snd_soc_card *card,
					    unsigned short *vendor,
					    unsigned short *device)
{
	return -ENOENT;
}
#endif /* CONFIG_PCI */

/* device driver data */
static inline void snd_soc_card_set_drvdata(struct snd_soc_card *card,
					    void *data)
{
	/* card 级私有数据直接挂到 dev 上，便于通用 driver data 访问。 */
	card->drvdata = data;
}

static inline void *snd_soc_card_get_drvdata(struct snd_soc_card *card)
{
	/* 取回 card 私有数据。 */
	return card->drvdata;
}

static inline
struct snd_soc_dai *snd_soc_card_get_codec_dai(struct snd_soc_card *card,
					       const char *dai_name)
{
	struct snd_soc_pcm_runtime *rtd;

	/* 从 card 所有 runtime 中遍历查找名字匹配的 codec DAI。 */
	for_each_card_rtds(card, rtd) {
		if (!strcmp(snd_soc_rtd_to_codec(rtd, 0)->name, dai_name))
			return snd_soc_rtd_to_codec(rtd, 0);
	}

	return NULL;
}

#endif /* __SOC_CARD_H */
