// SPDX-License-Identifier: GPL-2.0
//
// soc-component.c
//
// Copyright 2009-2011 Wolfson Microelectronics PLC.
// Copyright (C) 2019 Renesas Electronics Corp.
//
// Mark Brown <broonie@opensource.wolfsonmicro.com>
// Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
//
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <sound/soc.h>
#include <linux/bitops.h>

/* component 级辅助逻辑：时钟、PLL、jack、control、PCM/compress 代理调用。 */
#define soc_component_ret(dai, ret) _soc_component_ret(dai, __func__, ret)
static inline int _soc_component_ret(struct snd_soc_component *component, const char *func, int ret)
{
	return snd_soc_ret(component->dev, ret,
			   "at %s() on %s\n", func, component->name);
}

#define soc_component_ret_reg_rw(dai, ret, reg) _soc_component_ret_reg_rw(dai, __func__, ret, reg)
static inline int _soc_component_ret_reg_rw(struct snd_soc_component *component,
					    const char *func, int ret, int reg)
{
	return snd_soc_ret(component->dev, ret,
			   "at %s() on %s for register: [0x%08x]\n",
			   func, component->name, reg);
}

static inline int soc_component_field_shift(struct snd_soc_component *component,
					    unsigned int mask)
{
	/* 计算寄存器字段最低位的偏移，供 update_bits/read_field 使用。 */
	if (!mask) {
		dev_err(component->dev,	"ASoC: error field mask is zero for %s\n",
			component->name);
		return 0;
	}

	return (ffs(mask) - 1);
}

/*
 * 以后如果需要改成通过链表检查 substream，这里这组标记宏可以一起调整。
 */
#define soc_component_mark_push(component, substream, tgt)	((component)->mark_##tgt = substream)
#define soc_component_mark_pop(component, tgt)	((component)->mark_##tgt = NULL)
#define soc_component_mark_match(component, substream, tgt)	((component)->mark_##tgt == substream)

void snd_soc_component_set_aux(struct snd_soc_component *component,
			       struct snd_soc_aux_dev *aux)
{
	/* aux 设备把自己的 init 回调挂到 component 上。 */
	component->init = (aux) ? aux->init : NULL;
}

int snd_soc_component_init(struct snd_soc_component *component)
{
	int ret = 0;

	/* component 初始化时，如果有板级/辅助 init 先执行它。 */
	if (component->init)
		ret = component->init(component);

	return soc_component_ret(component, ret);
}

/**
 * snd_soc_component_set_sysclk - 配置 component 的系统/主时钟
 * @component: component
 * @clk_id: DAI 相关的时钟 ID
 * @source: 时钟来源
 * @freq: 新的时钟频率，单位 Hz
 * @dir: 时钟方向，输入或输出
 *
 * 用于配置 CODEC 的主时钟（MCLK）或系统时钟（SYSCLK）。
 */
int snd_soc_component_set_sysclk(struct snd_soc_component *component,
				 int clk_id, int source, unsigned int freq,
				 int dir)
{
	int ret = -ENOTSUPP;

	/* 统一入口：优先调用 driver 提供的 set_sysclk。 */
	if (component->driver->set_sysclk)
		ret = component->driver->set_sysclk(component, clk_id, source,
						     freq, dir);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_set_sysclk);

/**
 * snd_soc_component_set_pll - 配置 component 的 PLL
 * @component: component
 * @pll_id: DAI 相关的 PLL ID
 * @source: PLL 的输入来源
 * @freq_in: PLL 输入时钟频率，单位 Hz
 * @freq_out: 请求的 PLL 输出时钟频率，单位 Hz
 *
 * 根据输入时钟配置并使能 PLL 以生成输出时钟。
 */
int snd_soc_component_set_pll(struct snd_soc_component *component, int pll_id,
			      int source, unsigned int freq_in,
			      unsigned int freq_out)
{
	int ret = -EINVAL;

	/* 统一入口：优先调用 driver 提供的 set_pll。 */
	if (component->driver->set_pll)
		ret = component->driver->set_pll(component, pll_id, source,
						  freq_in, freq_out);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_set_pll);

void snd_soc_component_seq_notifier(struct snd_soc_component *component,
				    enum snd_soc_dapm_type type, int subseq)
{
	/* DAPM/Sequencer 事件通知直接转给驱动。 */
	if (component->driver->seq_notifier)
		component->driver->seq_notifier(component, type, subseq);
}

int snd_soc_component_stream_event(struct snd_soc_component *component,
				   int event)
{
	int ret = 0;

	/* 流事件（start/stop/resume/suspend）由 component driver 处理。 */
	if (component->driver->stream_event)
		ret = component->driver->stream_event(component, event);

	return soc_component_ret(component, ret);
}

int snd_soc_component_set_bias_level(struct snd_soc_component *component,
				     enum snd_soc_bias_level level)
{
	int ret = 0;

	/* component 级 bias 变化回调。 */
	if (component->driver->set_bias_level)
		ret = component->driver->set_bias_level(component, level);

	return soc_component_ret(component, ret);
}

static void soc_get_kcontrol_name(struct snd_soc_component *component,
				  char *buf, int size, const char * const ctl)
{
	/* 控件名前缀和 DAPM widget 名比较逻辑需要保持一致。 */
	if (component->name_prefix)
		snprintf(buf, size, "%s %s", component->name_prefix, ctl);
	else
		snprintf(buf, size, "%s", ctl);
}

struct snd_kcontrol *snd_soc_component_get_kcontrol(struct snd_soc_component *component,
						    const char * const ctl)
{
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

	soc_get_kcontrol_name(component, name, ARRAY_SIZE(name), ctl);

	return snd_soc_card_get_kcontrol(component->card, name);
}
EXPORT_SYMBOL_GPL(snd_soc_component_get_kcontrol);

int snd_soc_component_notify_control(struct snd_soc_component *component,
				     const char * const ctl)
{
	struct snd_kcontrol *kctl;

	/* 找到控件后，向 ALSA core 发送 value change 通知。 */
	kctl = snd_soc_component_get_kcontrol(component, ctl);
	if (!kctl)
		return soc_component_ret(component, -EINVAL);

	snd_ctl_notify(component->card->snd_card,
		       SNDRV_CTL_EVENT_MASK_VALUE, &kctl->id);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_notify_control);

/**
 * snd_soc_component_set_jack - 配置 component 的 jack 检测
 * @component: component
 * @jack: jack 结构体
 * @data: codec driver 可能需要的额外配置数据
 *
 * 配置并启用 jack 检测功能。
 */
int snd_soc_component_set_jack(struct snd_soc_component *component,
			       struct snd_soc_jack *jack, void *data)
{
	int ret = -ENOTSUPP;

	/* jack 检测能力由 component driver 决定是否实现。 */
	if (component->driver->set_jack)
		ret = component->driver->set_jack(component, jack, data);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_set_jack);

/**
 * snd_soc_component_get_jack_type - 获取 component 的 jack 类型
 * @component: component
 *
 * 返回 component 支持的 jack 类型。
 * 该类型可以来自驱动支持值，也可以来自 DT 的 jack-type 属性。
 */
int snd_soc_component_get_jack_type(
	struct snd_soc_component *component)
{
	int ret = -ENOTSUPP;

	/* 获取该 component 支持的 jack 类型。 */
	if (component->driver->get_jack_type)
		ret = component->driver->get_jack_type(component);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_get_jack_type);

int snd_soc_component_module_get(struct snd_soc_component *component,
				 void *mark, int upon_open)
{
	int ret = 0;

	if (component->driver->module_get_upon_open == !!upon_open &&
	    !try_module_get(component->dev->driver->owner))
		ret = -ENODEV;

	/* mark module if succeeded */
	if (ret == 0)
		soc_component_mark_push(component, mark, module);

	return soc_component_ret(component, ret);
}

void snd_soc_component_module_put(struct snd_soc_component *component,
				  void *mark, int upon_open, int rollback)
{
	/* rollback 场景只回收本次确实拿过的 module 引用。 */
	if (rollback && !soc_component_mark_match(component, mark, module))
		return;

	if (component->driver->module_get_upon_open == !!upon_open)
		module_put(component->dev->driver->owner);

	/* remove the mark from module */
	soc_component_mark_pop(component, module);
}

int snd_soc_component_open(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	int ret = 0;

	/* component 级 open 负责为当前 substream 准备底层资源。 */
	if (component->driver->open)
		ret = component->driver->open(component, substream);

	/* mark substream if succeeded */
	if (ret == 0)
		soc_component_mark_push(component, substream, open);

	return soc_component_ret(component, ret);
}

int snd_soc_component_close(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream,
			    int rollback)
{
	int ret = 0;

	/* close 与 open 成对出现，rollback 时避免重复回收。 */
	if (rollback && !soc_component_mark_match(component, substream, open))
		return 0;

	if (component->driver->close)
		ret = component->driver->close(component, substream);

	/* remove marked substream */
	soc_component_mark_pop(component, open);

	return soc_component_ret(component, ret);
}

void snd_soc_component_suspend(struct snd_soc_component *component)
{
	/* suspend/resume 是 component 的电源管理边界。 */
	if (component->driver->suspend)
		component->driver->suspend(component);
	component->suspended = 1;
}

void snd_soc_component_resume(struct snd_soc_component *component)
{
	/* 恢复时先让驱动恢复硬件状态，再清除 suspended 标记。 */
	if (component->driver->resume)
		component->driver->resume(component);
	component->suspended = 0;
}

int snd_soc_component_is_suspended(struct snd_soc_component *component)
{
	return component->suspended;
}

int snd_soc_component_probe(struct snd_soc_component *component)
{
	int ret = 0;

	/* probe 是 component 进入可用状态之前的最后一次驱动钩子。 */
	if (component->driver->probe)
		ret = component->driver->probe(component);

	return soc_component_ret(component, ret);
}

void snd_soc_component_remove(struct snd_soc_component *component)
{
	/* remove 只做驱动自定义清理，不改状态机逻辑。 */
	if (component->driver->remove)
		component->driver->remove(component);
}

int snd_soc_component_of_xlate_dai_id(struct snd_soc_component *component,
				      struct device_node *ep)
{
	int ret = -ENOTSUPP;

	/* OF endpoint 到 DAI ID 的翻译由驱动自己决定。 */
	if (component->driver->of_xlate_dai_id)
		ret = component->driver->of_xlate_dai_id(component, ep);

	return soc_component_ret(component, ret);
}

int snd_soc_component_of_xlate_dai_name(struct snd_soc_component *component,
					const struct of_phandle_args *args,
					const char **dai_name)
{
	/* 优先尝试让驱动从 phandle 参数里解析出 DAI 名字。 */
	if (component->driver->of_xlate_dai_name)
		return component->driver->of_xlate_dai_name(component,
							    args, dai_name);
	/*
	 * 这里不要直接用 soc_component_ret，因为此时未必需要立刻上报错误。
	 * 如果一个设备上挂了多个 component，前一个可能不匹配，
	 * 我们不希望因为这种情况反复刷日志。
	 */
	return -ENOTSUPP;
}

int snd_soc_component_regmap_val_bytes(struct snd_soc_component *component)
{
	int val_bytes;

	/* Errors are legitimate for non-integer byte multiples */

	if (!component->regmap)
		return 0;

	val_bytes = regmap_get_val_bytes(component->regmap);
	if (val_bytes < 0)
		return 0;

	return val_bytes;
}
EXPORT_SYMBOL_GPL(snd_soc_component_regmap_val_bytes);

#ifdef CONFIG_REGMAP

/**
 * snd_soc_component_init_regmap() - 初始化 component 的 regmap 实例
 * @component: 要初始化 regmap 的 component
 * @regmap: component 应使用的 regmap 实例
 *
 * 允许在 component 注册时延迟绑定 regmap。
 * 只有当 regmap 还没准备好、但后面会补上时才使用这个接口。
 * 并且必须在 component 第一次 IO 之前调用。
 */
void snd_soc_component_init_regmap(struct snd_soc_component *component,
				   struct regmap *regmap)
{
	component->regmap = regmap;
}
EXPORT_SYMBOL_GPL(snd_soc_component_init_regmap);

/**
 * snd_soc_component_exit_regmap() - 反初始化 component 的 regmap
 * @component: 要反初始化 regmap 的 component
 *
 * 对 component 关联的 regmap 调用 regmap_exit()，并清空 component 上的引用。
 * 仅当之前使用过 snd_soc_component_init_regmap() 时才应调用。
 */
void snd_soc_component_exit_regmap(struct snd_soc_component *component)
{
	regmap_exit(component->regmap);
	component->regmap = NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_component_exit_regmap);

#endif

int snd_soc_component_compr_open(struct snd_soc_component *component,
				 struct snd_compr_stream *cstream)
{
	int ret = 0;

	/* compressed offload 也遵循和 PCM 类似的 open/mark 模式。 */
	if (component->driver->compress_ops &&
	    component->driver->compress_ops->open)
		ret = component->driver->compress_ops->open(component, cstream);

	/* mark substream if succeeded */
	if (ret == 0)
		soc_component_mark_push(component, cstream, compr_open);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_open);

void snd_soc_component_compr_free(struct snd_soc_component *component,
				  struct snd_compr_stream *cstream,
				  int rollback)
{
	/* rollback 下只清理当前这次真正打开过的 compress stream。 */
	if (rollback && !soc_component_mark_match(component, cstream, compr_open))
		return;

	if (component->driver->compress_ops &&
	    component->driver->compress_ops->free)
		component->driver->compress_ops->free(component, cstream);

	/* remove marked substream */
	soc_component_mark_pop(component, compr_open);
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_free);

int snd_soc_component_compr_trigger(struct snd_compr_stream *cstream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret;

	/* 一个 runtime 里可能挂多个 component，这里逐个下发 trigger。 */
	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->trigger) {
			ret = component->driver->compress_ops->trigger(
				component, cstream, cmd);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_trigger);

int snd_soc_component_compr_set_params(struct snd_compr_stream *cstream,
				       struct snd_compr_params *params)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret;

	/* set_params 把压缩流协商结果同步到所有参与 component。 */
	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->set_params) {
			ret = component->driver->compress_ops->set_params(
				component, cstream, params);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_set_params);

int snd_soc_component_compr_get_params(struct snd_compr_stream *cstream,
				       struct snd_codec *params)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret;

	/* 读取参数时，只要有一个 component 能给出结果就直接返回。 */
	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->get_params) {
			ret = component->driver->compress_ops->get_params(
				component, cstream, params);
			return soc_component_ret(component, ret);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_get_params);

int snd_soc_component_compr_get_caps(struct snd_compr_stream *cstream,
				     struct snd_compr_caps *caps)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret = 0;

	/* 压缩能力通常由后端组件决定，所以这里也要先拿 card 级锁。 */
	snd_soc_dpcm_mutex_lock(rtd);

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->get_caps) {
			ret = component->driver->compress_ops->get_caps(
				component, cstream, caps);
			break;
		}
	}

	snd_soc_dpcm_mutex_unlock(rtd);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_get_caps);

int snd_soc_component_compr_get_codec_caps(struct snd_compr_stream *cstream,
					   struct snd_compr_codec_caps *codec)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret = 0;

	snd_soc_dpcm_mutex_lock(rtd);

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->get_codec_caps) {
			ret = component->driver->compress_ops->get_codec_caps(
				component, cstream, codec);
			break;
		}
	}

	snd_soc_dpcm_mutex_unlock(rtd);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_get_codec_caps);

int snd_soc_component_compr_ack(struct snd_compr_stream *cstream, size_t bytes)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->ack) {
			ret = component->driver->compress_ops->ack(
				component, cstream, bytes);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_ack);

int snd_soc_component_compr_pointer(struct snd_compr_stream *cstream,
				    struct snd_compr_tstamp64 *tstamp)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->pointer) {
			ret = component->driver->compress_ops->pointer(
				component, cstream, tstamp);
			return soc_component_ret(component, ret);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_pointer);

int snd_soc_component_compr_copy(struct snd_compr_stream *cstream,
				 char __user *buf, size_t count)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret = 0;

	snd_soc_dpcm_mutex_lock(rtd);

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->copy) {
			ret = component->driver->compress_ops->copy(
				component, cstream, buf, count);
			break;
		}
	}

	snd_soc_dpcm_mutex_unlock(rtd);

	return soc_component_ret(component, ret);
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_copy);

int snd_soc_component_compr_set_metadata(struct snd_compr_stream *cstream,
					 struct snd_compr_metadata *metadata)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->set_metadata) {
			ret = component->driver->compress_ops->set_metadata(
				component, cstream, metadata);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_set_metadata);

int snd_soc_component_compr_get_metadata(struct snd_compr_stream *cstream,
					 struct snd_compr_metadata *metadata)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->compress_ops &&
		    component->driver->compress_ops->get_metadata) {
			ret = component->driver->compress_ops->get_metadata(
				component, cstream, metadata);
			return soc_component_ret(component, ret);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_compr_get_metadata);

static unsigned int soc_component_read_no_lock(
	struct snd_soc_component *component,
	unsigned int reg)
{
	int ret;
	unsigned int val = 0;

	if (component->regmap)
		ret = regmap_read(component->regmap, reg, &val);
	else if (component->driver->read) {
		ret = 0;
		val = component->driver->read(component, reg);
	}
	else
		ret = -EIO;

	if (ret < 0)
		return soc_component_ret_reg_rw(component, ret, reg);

	return val;
}

/**
 * snd_soc_component_read() - 读取寄存器值
 * @component: 要读取的 component
 * @reg: 要读取的寄存器
 *
 * 返回：寄存器当前值。
 */
unsigned int snd_soc_component_read(struct snd_soc_component *component,
				    unsigned int reg)
{
	unsigned int val;

	mutex_lock(&component->io_mutex);
	val = soc_component_read_no_lock(component, reg);
	mutex_unlock(&component->io_mutex);

	return val;
}
EXPORT_SYMBOL_GPL(snd_soc_component_read);

static int soc_component_write_no_lock(
	struct snd_soc_component *component,
	unsigned int reg, unsigned int val)
{
	int ret = -EIO;

	if (component->regmap)
		ret = regmap_write(component->regmap, reg, val);
	else if (component->driver->write)
		ret = component->driver->write(component, reg, val);

	return soc_component_ret_reg_rw(component, ret, reg);
}

/**
 * snd_soc_component_write() - 写入寄存器值
 * @component: 要写入的 component
 * @reg: 要写入的寄存器
 * @val: 要写入寄存器的值
 *
 * 返回：成功时返回 0，失败时返回负错误码。
 */
int snd_soc_component_write(struct snd_soc_component *component,
			    unsigned int reg, unsigned int val)
{
	int ret;

	mutex_lock(&component->io_mutex);
	ret = soc_component_write_no_lock(component, reg, val);
	mutex_unlock(&component->io_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_component_write);

static int snd_soc_component_update_bits_legacy(
	struct snd_soc_component *component, unsigned int reg,
	unsigned int mask, unsigned int val, bool *change)
{
	unsigned int old, new;
	int ret = 0;

	mutex_lock(&component->io_mutex);

	old = soc_component_read_no_lock(component, reg);

	new = (old & ~mask) | (val & mask);
	*change = old != new;
	if (*change)
		ret = soc_component_write_no_lock(component, reg, new);

	mutex_unlock(&component->io_mutex);

	return soc_component_ret_reg_rw(component, ret, reg);
}

/**
 * snd_soc_component_update_bits() - 执行读-改-写更新
 * @component: 要更新的 component
 * @reg: 要更新的寄存器
 * @mask: 指定需要更新的位掩码
 * @val: mask 指定位的新值
 *
 * 当寄存器值发生变化时返回 1，操作成功但值未变化时返回 0，
 * 其他情况返回负错误码。
 */
int snd_soc_component_update_bits(struct snd_soc_component *component,
				  unsigned int reg, unsigned int mask, unsigned int val)
{
	bool change;
	int ret;

	if (component->regmap)
		ret = regmap_update_bits_check(component->regmap, reg, mask,
					       val, &change);
	else
		ret = snd_soc_component_update_bits_legacy(component, reg,
							   mask, val, &change);

	if (ret < 0)
		return soc_component_ret_reg_rw(component, ret, reg);
	return change;
}
EXPORT_SYMBOL_GPL(snd_soc_component_update_bits);

/**
 * snd_soc_component_update_bits_async() - 异步执行读-改-写更新
 * @component: 要更新的 component
 * @reg: 要更新的寄存器
 * @mask: 指定需要更新的位掩码
 * @val: mask 指定位的新值
 *
 * 该函数与 snd_soc_component_update_bits() 类似，但更新会异步排队，
 * 因此函数返回时更新可能尚未完成。若需要确保所有异步更新都已提交，
 * 必须调用 snd_soc_component_async_complete()。
 *
 * 返回：寄存器值变化时返回 1，操作成功但值未变化时返回 0，
 * 其他情况返回负错误码。
 */
int snd_soc_component_update_bits_async(struct snd_soc_component *component,
					unsigned int reg, unsigned int mask, unsigned int val)
{
	bool change;
	int ret;

	if (component->regmap)
		ret = regmap_update_bits_check_async(component->regmap, reg,
						     mask, val, &change);
	else
		ret = snd_soc_component_update_bits_legacy(component, reg,
							   mask, val, &change);

	if (ret < 0)
		return soc_component_ret_reg_rw(component, ret, reg);
	return change;
}
EXPORT_SYMBOL_GPL(snd_soc_component_update_bits_async);

/**
 * snd_soc_component_read_field() - 读取寄存器字段值
 * @component: 要读取的 component
 * @reg: 要读取的寄存器
 * @mask: 字段对应的掩码
 *
 * 返回：字段解码后的值。
 */
unsigned int snd_soc_component_read_field(struct snd_soc_component *component,
					  unsigned int reg, unsigned int mask)
{
	unsigned int val;

	val = snd_soc_component_read(component, reg);

	val = (val & mask) >> soc_component_field_shift(component, mask);

	return val;
}
EXPORT_SYMBOL_GPL(snd_soc_component_read_field);

/**
 * snd_soc_component_write_field() - 写入寄存器字段
 * @component: 要写入的 component
 * @reg: 要写入的寄存器
 * @mask: 需要更新的字段掩码
 * @val: 要写入字段的值
 *
 * 返回：字段内容发生变化时返回 1，否则返回 0。
 */
int snd_soc_component_write_field(struct snd_soc_component *component,
				  unsigned int reg, unsigned int mask,
				  unsigned int val)
{

	val = (val << soc_component_field_shift(component, mask)) & mask;

	return snd_soc_component_update_bits(component, reg, mask, val);
}
EXPORT_SYMBOL_GPL(snd_soc_component_write_field);

/**
 * snd_soc_component_async_complete() - 等待异步 I/O 完成
 * @component: 需要等待的 component
 *
 * 该函数会阻塞，直到此前通过 snd_soc_component_update_bits_async()
 * 提交的异步操作全部完成。
 */
void snd_soc_component_async_complete(struct snd_soc_component *component)
{
	if (component->regmap)
		regmap_async_complete(component->regmap);
}
EXPORT_SYMBOL_GPL(snd_soc_component_async_complete);

/**
 * snd_soc_component_test_bits() - 测试写入后寄存器是否会变化
 * @component: 要测试的 component
 * @reg: 要测试的寄存器
 * @mask: 需要比较的位掩码
 * @value: 用于比较的新值
 *
 * 将寄存器与新值进行合成后判断是否会产生变化。
 *
 * 返回：发生变化时返回 1，否则返回 0。
 */
int snd_soc_component_test_bits(struct snd_soc_component *component,
				unsigned int reg, unsigned int mask, unsigned int value)
{
	unsigned int old, new;

	old = snd_soc_component_read(component, reg);
	new = (old & ~mask) | value;
	return old != new;
}
EXPORT_SYMBOL_GPL(snd_soc_component_test_bits);

int snd_soc_pcm_component_pointer(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i;

	/* FIXME: use 1st pointer */
	for_each_rtd_components(rtd, i, component)
		if (component->driver->pointer)
			return component->driver->pointer(component, substream);

	return 0;
}

static bool snd_soc_component_is_codec_on_rtd(struct snd_soc_pcm_runtime *rtd,
					      struct snd_soc_component *component)
{
	struct snd_soc_dai *dai;
	int i;

	for_each_rtd_codec_dais(rtd, i, dai) {
		if (dai->component == component)
			return true;
	}

	return false;
}

void snd_soc_pcm_component_delay(struct snd_pcm_substream *substream,
				 snd_pcm_sframes_t *cpu_delay,
				 snd_pcm_sframes_t *codec_delay)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	snd_pcm_sframes_t delay;
	int i;

	/*
	 * 这里要统计的是整条音频路径上的总延迟，因此应当取所有发送端
	 * component 的最大值，再取所有接收端 component 的最大值，
	 * 而不是简单地对所有 component 取一个全局最大值。
	 */
	for_each_rtd_components(rtd, i, component) {
		if (!component->driver->delay)
			continue;

		delay = component->driver->delay(component, substream);

		if (snd_soc_component_is_codec_on_rtd(rtd, component))
			*codec_delay = max(*codec_delay, delay);
		else
			*cpu_delay = max(*cpu_delay, delay);
	}
}

int snd_soc_pcm_component_ioctl(struct snd_pcm_substream *substream,
				unsigned int cmd, void *arg)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i;

	/* FIXME: use 1st ioctl */
	for_each_rtd_components(rtd, i, component)
		if (component->driver->ioctl)
			return soc_component_ret(
				component,
				component->driver->ioctl(component,
							 substream, cmd, arg));

	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

int snd_soc_pcm_component_sync_stop(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->sync_stop) {
			ret = component->driver->sync_stop(component,
							   substream);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
	}

	return 0;
}

int snd_soc_pcm_component_copy(struct snd_pcm_substream *substream,
			       int channel, unsigned long pos,
			       struct iov_iter *iter, unsigned long bytes)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i;

	/* FIXME. it returns 1st copy now */
	for_each_rtd_components(rtd, i, component)
		if (component->driver->copy)
			return soc_component_ret(component,
				component->driver->copy(component, substream,
					channel, pos, iter, bytes));

	return -EINVAL;
}

struct page *snd_soc_pcm_component_page(struct snd_pcm_substream *substream,
					unsigned long offset)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	struct page *page;
	int i;

	/* FIXME. it returns 1st page now */
	for_each_rtd_components(rtd, i, component) {
		if (component->driver->page) {
			page = component->driver->page(component,
						       substream, offset);
			if (page)
				return page;
		}
	}

	return NULL;
}

int snd_soc_pcm_component_mmap(struct snd_pcm_substream *substream,
			       struct vm_area_struct *vma)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i;

	/* FIXME. it returns 1st mmap now */
	for_each_rtd_components(rtd, i, component)
		if (component->driver->mmap)
			return soc_component_ret(
				component,
				component->driver->mmap(component,
							substream, vma));

	return -EINVAL;
}

int snd_soc_pcm_component_new(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component;
	int ret;
	int i;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->pcm_new) {
			ret = component->driver->pcm_new(component, rtd);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
	}

	return 0;
}

void snd_soc_pcm_component_free(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component;
	int i;

	if (!rtd->pcm)
		return;

	for_each_rtd_components(rtd, i, component)
		if (component->driver->pcm_free)
			component->driver->pcm_free(component, rtd->pcm);
}

int snd_soc_pcm_component_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->prepare) {
			ret = component->driver->prepare(component, substream);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
	}

	return 0;
}

int snd_soc_pcm_component_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (component->driver->hw_params) {
			ret = component->driver->hw_params(component,
							   substream, params);
			if (ret < 0)
				return soc_component_ret(component, ret);
		}
		/* mark substream if succeeded */
		soc_component_mark_push(component, substream, hw_params);
	}

	return 0;
}

void snd_soc_pcm_component_hw_free(struct snd_pcm_substream *substream,
				   int rollback)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i, ret;

	for_each_rtd_components(rtd, i, component) {
		if (rollback && !soc_component_mark_match(component, substream, hw_params))
			continue;

		if (component->driver->hw_free) {
			ret = component->driver->hw_free(component, substream);
			if (ret < 0)
				soc_component_ret(component, ret);
		}

		/* remove marked substream */
		soc_component_mark_pop(component, hw_params);
	}
}

static int soc_component_trigger(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream,
				 int cmd)
{
	int ret = 0;

	if (component->driver->trigger)
		ret = component->driver->trigger(component, substream, cmd);

	return soc_component_ret(component, ret);
}

int snd_soc_pcm_component_trigger(struct snd_pcm_substream *substream,
				  int cmd, int rollback)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i, r, ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		for_each_rtd_components(rtd, i, component) {
			ret = soc_component_trigger(component, substream, cmd);
			if (ret < 0)
				break;
			soc_component_mark_push(component, substream, trigger);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		for_each_rtd_components(rtd, i, component) {
			if (rollback && !soc_component_mark_match(component, substream, trigger))
				continue;

			r = soc_component_trigger(component, substream, cmd);
			if (r < 0)
				ret = r; /* use last ret */
			soc_component_mark_pop(component, trigger);
		}
	}

	return ret;
}

int snd_soc_pcm_component_pm_runtime_get(struct snd_soc_pcm_runtime *rtd,
					 void *stream)
{
	struct snd_soc_component *component;
	int i;

	for_each_rtd_components(rtd, i, component) {
		int ret = pm_runtime_get_sync(component->dev);
		if (ret < 0 && ret != -EACCES) {
			pm_runtime_put_noidle(component->dev);
			return soc_component_ret(component, ret);
		}
		/* mark stream if succeeded */
		soc_component_mark_push(component, stream, pm);
	}

	return 0;
}

void snd_soc_pcm_component_pm_runtime_put(struct snd_soc_pcm_runtime *rtd,
					  void *stream, int rollback)
{
	struct snd_soc_component *component;
	int i;

	for_each_rtd_components(rtd, i, component) {
		if (rollback && !soc_component_mark_match(component, stream, pm))
			continue;

		pm_runtime_put_autosuspend(component->dev);

		/* remove marked stream */
		soc_component_mark_pop(component, pm);
	}
}

int snd_soc_pcm_component_ack(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component;
	int i;

	/* FIXME: use 1st pointer */
	for_each_rtd_components(rtd, i, component)
		if (component->driver->ack)
			return component->driver->ack(component, substream);

	return 0;
}
