// SPDX-License-Identifier: GPL-2.0
//
// soc-card.c
//
// Copyright (C) 2019 Renesas Electronics Corp.
// Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
//

#include <linux/lockdep.h>
#include <linux/rwsem.h>
#include <sound/soc.h>
#include <sound/jack.h>

/*
 * card 级辅助逻辑：jack 创建、probe/remove/suspend/resume 封装等。
 *
 * 这一层不直接做复杂音频处理，而是把 machine driver 的 card
 * 生命周期、jack 资源和错误码包装成统一的 helper。
 */
#define soc_card_ret(dai, ret) _soc_card_ret(dai, __func__, ret)
static inline int _soc_card_ret(struct snd_soc_card *card,
				const char *func, int ret)
{
	return snd_soc_ret(card->dev, ret,
			   "at %s() on %s\n", func, card->name);
}

struct snd_kcontrol *snd_soc_card_get_kcontrol(struct snd_soc_card *soc_card,
					       const char *name)
{
	/* card 级控件查找通常只需要按 mixer 名字去 ALSA core 里找。 */
	if (unlikely(!name))
		return NULL;

	return snd_ctl_find_id_mixer(soc_card->snd_card, name);
}
EXPORT_SYMBOL_GPL(snd_soc_card_get_kcontrol);

static int jack_new(struct snd_soc_card *card, const char *id, int type,
		    struct snd_soc_jack *jack, bool initial_kctl)
{
	/* jack 对象既要有 ALSA jack 实体，也要有 ASoC 封装的 pins/zones。 */
	mutex_init(&jack->mutex);
	jack->card = card;
	INIT_LIST_HEAD(&jack->pins);
	INIT_LIST_HEAD(&jack->jack_zones);
	BLOCKING_INIT_NOTIFIER_HEAD(&jack->notifier);

	return snd_jack_new(card->snd_card, id, type, &jack->jack, initial_kctl, false);
}

/**
 * snd_soc_card_jack_new - Create a new jack without pins
 * @card:  ASoC card
 * @id:    an identifying string for this jack
 * @type:  a bitmask of enum snd_jack_type values that can be detected by
 *         this jack
 * @jack:  structure to use for the jack
 *
 * Creates a new jack object without pins. If adding pins later,
 * snd_soc_card_jack_new_pins() should be used instead with 0 as num_pins
 * argument.
 *
 * Returns zero if successful, or a negative error code on failure.
 * On success jack will be initialised.
 */
int snd_soc_card_jack_new(struct snd_soc_card *card, const char *id, int type,
			  struct snd_soc_jack *jack)
{
	/* 不带 pins 的 jack，通常用于板级代码后续再补 pin 绑定。 */
	return soc_card_ret(card, jack_new(card, id, type, jack, true));
}
EXPORT_SYMBOL_GPL(snd_soc_card_jack_new);

/**
 * snd_soc_card_jack_new_pins - Create a new jack with pins
 * @card:  ASoC card
 * @id:    an identifying string for this jack
 * @type:  a bitmask of enum snd_jack_type values that can be detected by
 *         this jack
 * @jack:  structure to use for the jack
 * @pins:  Array of jack pins to be added to the jack or NULL
 * @num_pins: Number of elements in the @pins array
 *
 * Creates a new jack object with pins. If not adding pins,
 * snd_soc_card_jack_new() should be used instead.
 *
 * Returns zero if successful, or a negative error code on failure.
 * On success jack will be initialised.
 */
int snd_soc_card_jack_new_pins(struct snd_soc_card *card, const char *id,
			       int type, struct snd_soc_jack *jack,
			       struct snd_soc_jack_pin *pins,
			       unsigned int num_pins)
{
	int ret;

	/* 先创建 jack，再把一组 pin 一次性挂上去。 */
	ret = jack_new(card, id, type, jack, false);
	if (ret)
		goto end;

	if (num_pins)
		ret = snd_soc_jack_add_pins(jack, num_pins, pins);
end:
	return soc_card_ret(card, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_card_jack_new_pins);

int snd_soc_card_suspend_pre(struct snd_soc_card *card)
{
	int ret = 0;

	/* card 级 suspend_pre 是 machine driver 关机前的最早一批钩子。 */
	if (card->suspend_pre)
		ret = card->suspend_pre(card);

	return soc_card_ret(card, ret);
}

int snd_soc_card_suspend_post(struct snd_soc_card *card)
{
	int ret = 0;

	/* suspend_post 通常用于在子系统关闭后再做收尾。 */
	if (card->suspend_post)
		ret = card->suspend_post(card);

	return soc_card_ret(card, ret);
}

int snd_soc_card_resume_pre(struct snd_soc_card *card)
{
	int ret = 0;

	/* resume_pre 是恢复序列里更靠前的板级回调。 */
	if (card->resume_pre)
		ret = card->resume_pre(card);

	return soc_card_ret(card, ret);
}

int snd_soc_card_resume_post(struct snd_soc_card *card)
{
	int ret = 0;

	/* resume_post 在硬件恢复后执行，用于补齐板级状态。 */
	if (card->resume_post)
		ret = card->resume_post(card);

	return soc_card_ret(card, ret);
}

int snd_soc_card_probe(struct snd_soc_card *card)
{
	if (card->probe) {
		int ret = card->probe(card);

		if (ret < 0)
			return soc_card_ret(card, ret);

		/*
		 * probe 成功后先把标志置位，给 late_probe 保留统一的
		 * 生命周期判断点。
		 */
		card->probed = 1;
	}

	return 0;
}

int snd_soc_card_late_probe(struct snd_soc_card *card)
{
	if (card->late_probe) {
		int ret = card->late_probe(card);

		if (ret < 0)
			return soc_card_ret(card, ret);
	}

	/*
	 * late_probe 跑完之后，card 才算完整可用；这里统一补置 probed。
	 */
	card->probed = 1;

	return 0;
}

void snd_soc_card_fixup_controls(struct snd_soc_card *card)
{
	/* machine driver 可以在这里给 card 级控件做最后修正。 */
	if (card->fixup_controls)
		card->fixup_controls(card);
}

int snd_soc_card_remove(struct snd_soc_card *card)
{
	int ret = 0;

	/* 只有已经 probe 过的 card 才需要真正执行 remove。 */
	if (card->probed &&
	    card->remove)
		ret = card->remove(card);

	card->probed = 0;

	return soc_card_ret(card, ret);
}

int snd_soc_card_set_bias_level(struct snd_soc_card *card,
				struct snd_soc_dapm_context *dapm,
				enum snd_soc_bias_level level)
{
	int ret = 0;

	/* card 级 bias 回调用于协调整张声卡的电源级别变化。 */
	if (card->set_bias_level)
		ret = card->set_bias_level(card, dapm, level);

	return soc_card_ret(card, ret);
}

int snd_soc_card_set_bias_level_post(struct snd_soc_card *card,
				     struct snd_soc_dapm_context *dapm,
				     enum snd_soc_bias_level level)
{
	int ret = 0;

	/* post 回调用于在 DAPM 核心完成切换后做收尾。 */
	if (card->set_bias_level_post)
		ret = card->set_bias_level_post(card, dapm, level);

	return soc_card_ret(card, ret);
}

int snd_soc_card_add_dai_link(struct snd_soc_card *card,
			      struct snd_soc_dai_link *dai_link)
{
	int ret = 0;

	/* 动态 card 可以在运行时新增一条 DAI link。 */
	if (card->add_dai_link)
		ret = card->add_dai_link(card, dai_link);

	return soc_card_ret(card, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_card_add_dai_link);

void snd_soc_card_remove_dai_link(struct snd_soc_card *card,
				  struct snd_soc_dai_link *dai_link)
{
	/* remove_dai_link 与 add_dai_link 成对，负责清理动态 link。 */
	if (card->remove_dai_link)
		card->remove_dai_link(card, dai_link);
}
EXPORT_SYMBOL_GPL(snd_soc_card_remove_dai_link);
