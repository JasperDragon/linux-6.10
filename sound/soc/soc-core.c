// SPDX-License-Identifier: GPL-2.0+
//
// soc-core.c  --  ALSA SoC Audio Layer
//
// Copyright 2005 Wolfson Microelectronics PLC.
// Copyright 2005 Openedhand Ltd.
// Copyright (C) 2010 Slimlogic Ltd.
// Copyright (C) 2010 Texas Instruments Inc.
//
// Author: Liam Girdwood <lrg@slimlogic.co.uk>
//         with code, comments and ideas from :-
//         Richard Purdie <richard@openedhand.com>
//
//  TODO:
//   o Add hw rules to enforce rates, etc.
//   o More testing with other codecs/machines.
//   o Add more codecs and platforms to ensure good API coverage.
//   o Support TDM on PCM and I2S

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/dmi.h>
#include <linux/acpi.h>
#include <linux/string_choices.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dpcm.h>
#include <sound/soc-topology.h>
#include <sound/soc-link.h>
#include <sound/initval.h>

/*
 * soc-core.c 是 ASoC 的核心调度层：
 * 负责 card/component/dai 的注册绑定、runtime 构建、PCM 流控制、
 * DAPM 路径推进、debugfs/sysfs 以及模块生命周期管理。
 */
#define CREATE_TRACE_POINTS
#include <trace/events/asoc.h>

/* 保护全局 component/card 绑定关系的总锁。 */
static DEFINE_MUTEX(client_mutex);
/* 已注册的 component 全局链表。 */
static LIST_HEAD(component_list);
/* 解绑过程中临时挂起的 card 链表。 */
static LIST_HEAD(unbind_card_list);

#define for_each_component(component)			\
	list_for_each_entry(component, &component_list, list)

/*
 * 当驱动不需要显式定义 CPU/Codec/Platform dai_link 时使用。
 * 具体约定见 soc.h。
 */
/* 某些 DAI link 不需要显式 platform/cpu/codec 端点时使用的空数组。 */
struct snd_soc_dai_link_component null_dailink_component[0];
EXPORT_SYMBOL_GPL(null_dailink_component);

/*
 * 这是一个在流关闭后执行 DAPM 下电的超时时间。
 * 它可用于减少不同播放流之间的 pop 声，例如两段音轨之间切换时。
 */
/* 关闭流后延迟下电的默认时间，单位毫秒，用于减少重开时的 pop 声。 */
static int pmdown_time = 5000;
module_param(pmdown_time, int, 0);
MODULE_PARM_DESC(pmdown_time, "DAPM stream powerdown time (msecs)");

/* 将当前 runtime 的 pmdown_time 暴露到 sysfs。 */
static ssize_t pmdown_time_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct snd_soc_pcm_runtime *rtd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%ld\n", rtd->pmdown_time);
}

/* 允许用户态通过 sysfs 调整当前 runtime 的下电延迟。 */
static ssize_t pmdown_time_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct snd_soc_pcm_runtime *rtd = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &rtd->pmdown_time);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(pmdown_time);

/* card/runtime 设备上的 ASoC 专用属性集。 */
static struct attribute *soc_dev_attrs[] = {
	&dev_attr_pmdown_time.attr,
	NULL
};

/*
 * 控制某些属性是否对当前 runtime 可见。
 * 例如只有有 codec 的 runtime 才显示某些控制项。
 */
static umode_t soc_dev_attr_is_visible(struct kobject *kobj,
				       struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct snd_soc_pcm_runtime *rtd = dev_get_drvdata(dev);

	if (!rtd)
		return 0;

	if (attr == &dev_attr_pmdown_time.attr)
		return attr->mode; /* always visible */
	return rtd->dai_link->num_codecs ? attr->mode : 0; /* enabled only with codec */
}

/* DAPM 相关的 sysfs attribute 组。 */
static const struct attribute_group soc_dapm_dev_group = {
	.attrs = snd_soc_dapm_dev_attrs,
	.is_visible = soc_dev_attr_is_visible,
};

/* 非 DAPM 的 card/runtime 属性组。 */
static const struct attribute_group soc_dev_group = {
	.attrs = soc_dev_attrs,
	.is_visible = soc_dev_attr_is_visible,
};

/* 供 device core 注册的属性组列表。 */
static const struct attribute_group *soc_dev_attr_groups[] = {
	&soc_dapm_dev_group,
	&soc_dev_group,
	NULL
};

#ifdef CONFIG_DEBUG_FS
struct dentry *snd_soc_debugfs_root;
EXPORT_SYMBOL_GPL(snd_soc_debugfs_root);

static void soc_init_component_debugfs(struct snd_soc_component *component)
{
	if (!component->card->debugfs_card_root)
		return;

	if (component->debugfs_prefix) {
		char *name;

		name = kasprintf(GFP_KERNEL, "%s:%s",
			component->debugfs_prefix, component->name);
		if (name) {
			component->debugfs_root = debugfs_create_dir(name,
				component->card->debugfs_card_root);
			kfree(name);
		}
	} else {
		component->debugfs_root = debugfs_create_dir(component->name,
				component->card->debugfs_card_root);
	}

	snd_soc_dapm_debugfs_init(snd_soc_component_to_dapm(component),
		component->debugfs_root);
}

static void soc_cleanup_component_debugfs(struct snd_soc_component *component)
{
	if (!component->debugfs_root)
		return;
	debugfs_remove_recursive(component->debugfs_root);
	component->debugfs_root = NULL;
}

static int dai_list_show(struct seq_file *m, void *v)
{
	struct snd_soc_component *component;
	struct snd_soc_dai *dai;
	guard(mutex)(&client_mutex);

	for_each_component(component)
		for_each_component_dais(component, dai)
			seq_printf(m, "%s\n", dai->name);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dai_list);

static int component_list_show(struct seq_file *m, void *v)
{
	struct snd_soc_component *component;
	guard(mutex)(&client_mutex);

	for_each_component(component)
		seq_printf(m, "%s\n", component->name);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(component_list);

static void soc_init_card_debugfs(struct snd_soc_card *card)
{
	card->debugfs_card_root = debugfs_create_dir(card->name,
						     snd_soc_debugfs_root);

	debugfs_create_u32("dapm_pop_time", 0644, card->debugfs_card_root,
			   &card->pop_time);

	snd_soc_dapm_debugfs_init(snd_soc_card_to_dapm(card), card->debugfs_card_root);
}

static void soc_cleanup_card_debugfs(struct snd_soc_card *card)
{
	debugfs_remove_recursive(card->debugfs_card_root);
	card->debugfs_card_root = NULL;
}

static void snd_soc_debugfs_init(void)
{
	snd_soc_debugfs_root = debugfs_create_dir("asoc", NULL);

	debugfs_create_file("dais", 0444, snd_soc_debugfs_root, NULL,
			    &dai_list_fops);

	debugfs_create_file("components", 0444, snd_soc_debugfs_root, NULL,
			    &component_list_fops);
}

static void snd_soc_debugfs_exit(void)
{
	debugfs_remove_recursive(snd_soc_debugfs_root);
}

#else

static inline void soc_init_component_debugfs(struct snd_soc_component *component) { }
static inline void soc_cleanup_component_debugfs(struct snd_soc_component *component) { }
static inline void soc_init_card_debugfs(struct snd_soc_card *card) { }
static inline void soc_cleanup_card_debugfs(struct snd_soc_card *card) { }
static inline void snd_soc_debugfs_init(void) { }
static inline void snd_soc_debugfs_exit(void) { }

#endif

static int snd_soc_is_match_dai_args(const struct of_phandle_args *args1,
				     const struct of_phandle_args *args2)
{
	/* DAI args 匹配要求 node 和参数数组都完全一致。 */
	if (!args1 || !args2)
		return 0;

	if (args1->np != args2->np)
		return 0;

	for (int i = 0; i < args1->args_count; i++)
		if (args1->args[i] != args2->args[i])
			return 0;

	return 1;
}

static inline int snd_soc_dlc_component_is_empty(struct snd_soc_dai_link_component *dlc)
{
	return !(dlc->dai_args || dlc->name || dlc->of_node);
}

static inline int snd_soc_dlc_component_is_invalid(struct snd_soc_dai_link_component *dlc)
{
	return (dlc->name && dlc->of_node);
}

static inline int snd_soc_dlc_dai_is_empty(struct snd_soc_dai_link_component *dlc)
{
	return !(dlc->dai_args || dlc->dai_name);
}

static int snd_soc_is_matching_dai(const struct snd_soc_dai_link_component *dlc,
				   struct snd_soc_dai *dai)
{
	/* 端点匹配优先看 phandle 参数，其次看名字。 */
	if (!dlc)
		return 0;

	if (dlc->dai_args)
		return snd_soc_is_match_dai_args(dai->driver->dai_args, dlc->dai_args);

	if (!dlc->dai_name)
		return 1;

	/* 参见 snd_soc_dai_name_get()。 */

	if (dai->driver->name &&
	    strcmp(dlc->dai_name, dai->driver->name) == 0)
		return 1;

	if (strcmp(dlc->dai_name, dai->name) == 0)
		return 1;

	if (dai->component->name &&
	    strcmp(dlc->dai_name, dai->component->name) == 0)
		return 1;

	return 0;
}

const char *snd_soc_dai_name_get(const struct snd_soc_dai *dai)
{
	/* 取一个最适合对外显示的名字，优先级为 driver->name / dai->name / component->name。 */
	/* 参见 snd_soc_is_matching_dai()。 */
	if (dai->driver->name)
		return dai->driver->name;

	if (dai->name)
		return dai->name;

	if (dai->component->name)
		return dai->component->name;

	return NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_dai_name_get);

static int snd_soc_rtd_add_component(struct snd_soc_pcm_runtime *rtd,
				     struct snd_soc_component *component)
{
	/* runtime 里不能重复挂同一个 component。 */
	struct snd_soc_component *comp;
	int i;

	for_each_rtd_components(rtd, i, comp) {
		/* 已经连接过了。 */
		if (comp == component)
			return 0;
	}

	/* 供 for_each_rtd_components() 遍历使用。 */
	rtd->num_components++; // increment flex array count at first
	rtd->components[rtd->num_components - 1] = component;

	return 0;
}

struct snd_soc_component *snd_soc_rtdcom_lookup(struct snd_soc_pcm_runtime *rtd,
						const char *driver_name)
{
	/* 在 runtime 中按 component driver name 取回已绑定 component。 */
	struct snd_soc_component *component;
	int i;

	if (!driver_name)
		return NULL;

	/*
	 * 注意：
	 *
	 * snd_soc_rtdcom_lookup() 会按指定的 driver name 从 rtd 中查找
	 * component。
	 * 但如果多个 component 使用了相同的 driver name 并连接到同一个
	 * rtd，这个函数只会返回第一个找到的 component。
	 */
	for_each_rtd_components(rtd, i, component) {
		const char *component_name = component->driver->name;

		if (!component_name)
			continue;

		if ((component_name == driver_name) ||
		    strcmp(component_name, driver_name) == 0)
			return component;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_rtdcom_lookup);

struct snd_soc_component
*snd_soc_lookup_component_nolocked(struct device *dev, const char *driver_name)
{
	/* 不加 client_mutex 的 component 查找，仅供内部持锁路径使用。 */
	struct snd_soc_component *component;

	for_each_component(component) {
		if (dev != component->dev)
			continue;

		if (!driver_name)
			return component;

		if (!component->driver->name)
			continue;

		if (component->driver->name == driver_name)
			return component;

		if (strcmp(component->driver->name, driver_name) == 0)
			return component;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_lookup_component_nolocked);

struct snd_soc_component *snd_soc_lookup_component(struct device *dev,
						   const char *driver_name)
{
	/* 对外暴露的 component 查找入口。 */
	guard(mutex)(&client_mutex);

	return snd_soc_lookup_component_nolocked(dev, driver_name);
}
EXPORT_SYMBOL_GPL(snd_soc_lookup_component);

struct snd_soc_component *snd_soc_lookup_component_by_name(const char *component_name)
{
	/* 按名称模糊查找 component，主要用于调试和 topology 路径。 */
	struct snd_soc_component *component;

	guard(mutex)(&client_mutex);
	for_each_component(component)
		if (strstr(component->name, component_name))
			return component;

	return NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_lookup_component_by_name);

struct snd_soc_pcm_runtime
*snd_soc_get_pcm_runtime(struct snd_soc_card *card,
			 struct snd_soc_dai_link *dai_link)
{
	/* 按 dai_link 指针精确定位 runtime。 */
	struct snd_soc_pcm_runtime *rtd;

	for_each_card_rtds(card, rtd) {
		if (rtd->dai_link == dai_link)
			return rtd;
	}
	dev_dbg(card->dev, "ASoC: failed to find rtd %s\n", dai_link->name);
	return NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_get_pcm_runtime);

/*
 * 在流 close 之后延迟 pmdown_time 毫秒再关闭音频子系统。
 * 这样可以避免由于 DAPM 反复上/下电而在音乐轨道切换时产生 pop/click。
 */
void snd_soc_close_delayed_work(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	int playback = SNDRV_PCM_STREAM_PLAYBACK;

	/* 延迟关流发生时要持有 DPCM 锁，避免和 trigger 竞态。 */
	snd_soc_dpcm_mutex_lock(rtd);

	dev_dbg(rtd->dev,
		"ASoC: pop wq checking: %s status: %s waiting: %s\n",
		codec_dai->driver->playback.stream_name,
		snd_soc_dai_stream_active(codec_dai, playback) ?
		"active" : "inactive",
		str_yes_no(rtd->pop_wait));

	/* 这个 codec DAI stream 还在等待吗？ */
	if (rtd->pop_wait == 1) {
		rtd->pop_wait = 0;
		snd_soc_dapm_stream_event(rtd, playback,
					  SND_SOC_DAPM_STREAM_STOP);
	}

	snd_soc_dpcm_mutex_unlock(rtd);
}
EXPORT_SYMBOL_GPL(snd_soc_close_delayed_work);

static void soc_release_rtd_dev(struct device *dev)
{
	/* 这里的 "dev" 指的是 "rtd->dev"。 */
	kfree(dev);
}

static void soc_free_pcm_runtime(struct snd_soc_pcm_runtime *rtd)
{
	if (!rtd)
		return;

	/* rtd 释放前要先退出链表、停止 delayed work，再释放嵌套资源。 */
	list_del(&rtd->list);

	flush_delayed_work(&rtd->delayed_work);
	snd_soc_pcm_component_free(rtd);

	/*
	 * 不需要对 rtd->dev 单独调用 kfree()。
	 * 也不需要检查 rtd->dev 是否为 NULL，因为它是在 rtd 之前分配的。
	 * 同样也不用单独处理 rtd 的释放，因为它本身就是从 dev
	 * （也就是 rtd->dev）创建出来的。
	 */
	device_unregister(rtd->dev);
}

static void close_delayed_work(struct work_struct *work) {
	struct snd_soc_pcm_runtime *rtd =
			container_of(work, struct snd_soc_pcm_runtime,
				     delayed_work.work);

	if (rtd->close_delayed_work_func)
		rtd->close_delayed_work_func(rtd);
}

static struct snd_soc_pcm_runtime *soc_new_pcm_runtime(
	struct snd_soc_card *card, struct snd_soc_dai_link *dai_link)
{
	struct snd_soc_pcm_runtime *rtd;
	struct device *dev;
	int ret;
	int stream;

	/* 为 runtime 单独创建一个 device 节点。 */
	dev = kzalloc_obj(struct device);
	if (!dev)
		return NULL;

	dev->parent	= card->dev;
	dev->release	= soc_release_rtd_dev;

	dev_set_name(dev, "%s", dai_link->name);

	ret = device_register(dev);
	if (ret < 0) {
		put_device(dev); /* soc_release_rtd_dev */
		return NULL;
	}

	/* 运行时对象本体跟随这个 device 分配。 */
	rtd = devm_kzalloc(dev,
			   struct_size(rtd, components,
				       dai_link->num_cpus +
				       dai_link->num_codecs +
				       dai_link->num_platforms),
			   GFP_KERNEL);
	if (!rtd) {
		device_unregister(dev);
		return NULL;
	}

	rtd->dev = dev;
	INIT_LIST_HEAD(&rtd->list);
	for_each_pcm_streams(stream) {
		INIT_LIST_HEAD(&rtd->dpcm[stream].be_clients);
		INIT_LIST_HEAD(&rtd->dpcm[stream].fe_clients);
	}
	dev_set_drvdata(dev, rtd);
	INIT_DELAYED_WORK(&rtd->delayed_work, close_delayed_work);

	if ((dai_link->num_cpus + dai_link->num_codecs) == 0) {
		dev_err(dev, "ASoC: it has no CPU or codec DAIs\n");
		goto free_rtd;
	}

	/* 为 runtime 预留 CPU/Codec DAI 指针数组。 */
	rtd->dais = devm_kcalloc(dev, dai_link->num_cpus + dai_link->num_codecs,
					sizeof(struct snd_soc_dai *),
					GFP_KERNEL);
	if (!rtd->dais)
		goto free_rtd;

	/*
	 * dais = [][][][][][][][][][][][][][][][][][]
	 *	  ^cpu_dais         ^codec_dais
	 *	  |--- num_cpus ---|--- num_codecs --|
	 * see
	 *	snd_soc_rtd_to_cpu()
	 *	snd_soc_rtd_to_codec()
	 */
	rtd->card	= card;
	rtd->dai_link	= dai_link;
	rtd->id		= card->num_rtd++;
	rtd->pmdown_time = pmdown_time;			/* default power off timeout */

	/* 供 for_each_card_rtds() 遍历使用。 */
	list_add_tail(&rtd->list, &card->rtd_list);

	ret = device_add_groups(dev, soc_dev_attr_groups);
	if (ret < 0)
		goto free_rtd;

	return rtd;

free_rtd:
	soc_free_pcm_runtime(rtd);
	return NULL;
}

static void snd_soc_fill_dummy_dai(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *dai_link;
	int i;

	/*
	 * COMP_DUMMY() 会生成 0 长度端点数组，这里把 CPU/CODEC 补成 dummy。
	 * Platform 端点不在这个 helper 里处理。
	 */
	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->num_cpus == 0 && dai_link->cpus) {
			dai_link->num_cpus	= 1;
			dai_link->cpus		= &snd_soc_dummy_dlc;
		}
		if (dai_link->num_codecs == 0 && dai_link->codecs) {
			dai_link->num_codecs	= 1;
			dai_link->codecs	= &snd_soc_dummy_dlc;
		}
	}
}

static void snd_soc_flush_all_delayed_work(struct snd_soc_card *card)
{
	/* 卡片级 flush：把所有 runtime 的延迟 work 一次性跑完。 */
	struct snd_soc_pcm_runtime *rtd;

	for_each_card_rtds(card, rtd)
		flush_delayed_work(&rtd->delayed_work);
}

#ifdef CONFIG_PM_SLEEP
static void soc_playback_digital_mute(struct snd_soc_card *card, int mute)
{
	/* 仅对真正活跃的 playback DAI 执行 mute/unmute。 */
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *dai;
	int playback = SNDRV_PCM_STREAM_PLAYBACK;
	int i;

	for_each_card_rtds(card, rtd) {

		if (rtd->dai_link->ignore_suspend)
			continue;

		for_each_rtd_dais(rtd, i, dai) {
			if (snd_soc_dai_stream_active(dai, playback))
				snd_soc_dai_digital_mute(dai, mute, playback);
		}
	}
}

static void soc_dapm_suspend_resume(struct snd_soc_card *card, int event)
{
	/* suspend/resume 时让每条 runtime 的 DAPM 图同步接收事件。 */
	struct snd_soc_pcm_runtime *rtd;
	int stream;

	for_each_card_rtds(card, rtd) {

		if (rtd->dai_link->ignore_suspend)
			continue;

		for_each_pcm_streams(stream)
			snd_soc_dapm_stream_event(rtd, stream, event);
	}
}

/* suspend 时关闭音频子系统。 */
int snd_soc_suspend(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct snd_soc_component *component;
	struct snd_soc_pcm_runtime *rtd;
	int i;

	/* card 还没完成 instantiate 就没有可挂起的运行时对象。 */
	if (!snd_soc_card_is_instantiated(card))
		return 0;

	/*
	 * 由于 resume 是通过 workqueue 调度的，我们可能会在它完成前就进入
	 * suspend，因此这里要等它先跑完。
	 */
	snd_power_wait(card->snd_card);

	/* 在 resume 完成之前，先阻止用户态继续访问。 */
	snd_power_change_state(card->snd_card, SNDRV_CTL_POWER_D3hot);

	/* 先静音所有活动中的 DAC。 */
	soc_playback_digital_mute(card, 1);

	/* suspend 所有 PCM。 */
	for_each_card_rtds(card, rtd) {
		if (rtd->dai_link->ignore_suspend)
			continue;

		snd_pcm_suspend_all(rtd->pcm);
	}

	snd_soc_card_suspend_pre(card);

	/* 关闭所有还在等待的流。 */
	snd_soc_flush_all_delayed_work(card);

	soc_dapm_suspend_resume(card, SND_SOC_DAPM_STREAM_SUSPEND);

	/* 重新检查所有 endpoint，因为 suspend 会影响它们的状态。 */
	snd_soc_dapm_mark_endpoints_dirty(card);
	snd_soc_dapm_sync(snd_soc_card_to_dapm(card));

	/* suspend 所有 COMPONENT。 */
	for_each_card_rtds(card, rtd) {

		if (rtd->dai_link->ignore_suspend)
			continue;

		for_each_rtd_components(rtd, i, component) {
			struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);

			/*
			 * 如果 component 之前已经 suspend，就直接忽略。
			 */
			if (snd_soc_component_is_suspended(component))
				continue;

			/*
			 * 如果还有路径处于活动状态，COMPONENT 就会保持在
			 * bias _ON，这时不应该被 suspend。
			 */
			switch (snd_soc_dapm_get_bias_level(dapm)) {
			case SND_SOC_BIAS_STANDBY:
				/*
				 * 如果 COMPONENT 支持 idle bias off，
				 * 那么处于 STANDBY 说明它仍在工作；
				 * 否则就继续向下处理。
				 */
				if (!snd_soc_dapm_get_idle_bias(dapm)) {
					dev_dbg(component->dev,
						"ASoC: idle_bias_off CODEC on over suspend\n");
					break;
				}
				fallthrough;

			case SND_SOC_BIAS_OFF:
				snd_soc_component_suspend(component);
				if (component->regmap)
					regcache_mark_dirty(component->regmap);
				/* 将 pin 切换到睡眠态。 */
				pinctrl_pm_select_sleep_state(component->dev);
				break;
			default:
				dev_dbg(component->dev,
					"ASoC: COMPONENT is on over suspend\n");
				break;
			}
		}
	}

	snd_soc_card_suspend_post(card);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_suspend);

/*
 * resume 通过 workqueue 延迟执行，因此它可能在 codec 重新配置完成前就
 * 已经返回。I2C 上这一步可能很慢，所以需要特别处理。
 */
static void soc_resume_deferred(struct work_struct *work)
{
	struct snd_soc_card *card =
			container_of(work, struct snd_soc_card,
				     deferred_resume_work);
	struct snd_soc_component *component;

	/*
	 * 这里的 power state 仍是 D3hot，用户态还不能碰硬件。
	 * 先把 component 恢复，再把 DAPM 和用户态状态切回来。
	 */

	dev_dbg(card->dev, "ASoC: starting resume work\n");

	/* 先把电源状态提升到 D2，让 DAPM 开始启用各个节点。 */
	snd_power_change_state(card->snd_card, SNDRV_CTL_POWER_D2);

	snd_soc_card_resume_pre(card);

	for_each_card_components(card, component) {
		if (snd_soc_component_is_suspended(component))
			snd_soc_component_resume(component);
	}

	soc_dapm_suspend_resume(card, SND_SOC_DAPM_STREAM_RESUME);

	/* 取消所有活动 DAC 的静音。 */
	soc_playback_digital_mute(card, 0);

	snd_soc_card_resume_post(card);

	dev_dbg(card->dev, "ASoC: resume work completed\n");

	/* 同样重新检查所有 endpoint，因为 suspend 会影响它们的状态。 */
	snd_soc_dapm_mark_endpoints_dirty(card);
	snd_soc_dapm_sync(snd_soc_card_to_dapm(card));

	/* 现在状态已经恢复，用户态可以重新访问了。 */
	snd_power_change_state(card->snd_card, SNDRV_CTL_POWER_D0);
}

/* suspend 后重新开启音频子系统。 */
int snd_soc_resume(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct snd_soc_component *component;

	/* card 还没完成 instantiate 就没有可恢复的运行时对象。 */
	if (!snd_soc_card_is_instantiated(card))
		return 0;

	/* 将 pin 从睡眠态激活回来。 */
	for_each_card_components(card, component)
		if (snd_soc_component_active(component))
			pinctrl_pm_select_default_state(component->dev);

	dev_dbg(dev, "ASoC: Scheduling resume work\n");
	if (!schedule_work(&card->deferred_resume_work))
		dev_err(dev, "ASoC: resume work item may be lost\n");

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_resume);

static void soc_resume_init(struct snd_soc_card *card)
{
	/* resume 通过 workqueue 延迟执行，避免同步恢复过慢。 */
	INIT_WORK(&card->deferred_resume_work, soc_resume_deferred);
}
#else
#define snd_soc_suspend NULL
#define snd_soc_resume NULL
static inline void soc_resume_init(struct snd_soc_card *card) { }
#endif

static struct device_node
*soc_component_to_node(struct snd_soc_component *component)
{
	/* 先取 component 自己的 of_node，取不到再回退到父设备。 */
	struct device_node *of_node;

	of_node = component->dev->of_node;
	if (!of_node && component->dev->parent)
		of_node = component->dev->parent->of_node;

	return of_node;
}

struct of_phandle_args *snd_soc_copy_dai_args(struct device *dev,
					      const struct of_phandle_args *args)
{
	/* 复制一份 phandle args 供 runtime 保存。 */
	struct of_phandle_args *ret = devm_kzalloc(dev, sizeof(*ret), GFP_KERNEL);

	if (!ret)
		return NULL;

	*ret = *args;

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_copy_dai_args);

static int snd_soc_is_matching_component(
	const struct snd_soc_dai_link_component *dlc,
	struct snd_soc_component *component)
{
	/* component 匹配时，dai_args 优先，随后才比对 of_node/name。 */
	struct device_node *component_of_node;

	if (!dlc)
		return 0;

	if (dlc->dai_args) {
		struct snd_soc_dai *dai;

		for_each_component_dais(component, dai)
			if (snd_soc_is_matching_dai(dlc, dai))
				return 1;
		return 0;
	}

	component_of_node = soc_component_to_node(component);

	if (dlc->of_node && component_of_node != dlc->of_node)
		return 0;
	if (dlc->name && strcmp(component->name, dlc->name))
		return 0;

	return 1;
}

static struct snd_soc_component *soc_find_component(
	const struct snd_soc_dai_link_component *dlc)
{
	/* 全局 component 列表中找第一个匹配项。 */
	struct snd_soc_component *component;

	lockdep_assert_held(&client_mutex);

	/*
	 * 注意：
	 *
	 * 它只会返回找到的第一个 component，但某些 driver 会让多个
	 * component 共享同一个 of_node/name，例如 CPU component 和通用
	 * DMAEngine component。
	 */
	for_each_component(component)
		if (snd_soc_is_matching_component(dlc, component))
			return component;

	return NULL;
}

/**
 * snd_soc_find_dai - 查找已注册的 DAI
 *
 * @dlc: 需要匹配的 DAI 信息，可能包含 DAI 名称、DAI driver 名称
 *       以及可选的 component 信息
 *
 * 该函数会遍历所有已注册的 component 及其下属 DAI，查找名称匹配的
 * 目标 DAI。如果指定了 component 的 of_node 或 name，也必须同时匹配。
 *
 * 返回：找到时返回 DAI 指针，未找到则返回 NULL。
 */
struct snd_soc_dai *snd_soc_find_dai(
	const struct snd_soc_dai_link_component *dlc)
{
	/* 先找匹配的 component，再在它下面找 DAI。 */
	struct snd_soc_component *component;
	struct snd_soc_dai *dai;

	lockdep_assert_held(&client_mutex);

	/* 从已注册的 DAI 中查找 CPU DAI。 */
	for_each_component(component)
		if (snd_soc_is_matching_component(dlc, component))
			for_each_component_dais(component, dai)
				if (snd_soc_is_matching_dai(dlc, dai))
					return dai;

	return NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_find_dai);

struct snd_soc_dai *snd_soc_find_dai_with_mutex(
	const struct snd_soc_dai_link_component *dlc)
{
	guard(mutex)(&client_mutex);

	return snd_soc_find_dai(dlc);
}
EXPORT_SYMBOL_GPL(snd_soc_find_dai_with_mutex);

static int soc_dai_link_sanity_check(struct snd_soc_card *card,
				     struct snd_soc_dai_link *link)
{
	int i;
	struct snd_soc_dai_link_component *dlc;

	/*
	 * 在真正创建 runtime 之前，先把这条 dai_link 的三类端点都校验一遍：
	 * - CODEC 必须能定位到明确的 component + DAI
	 * - PLATFORM 可以为空，但一旦写了就必须能找到对应 component
	 * - CPU 允许只写 DAI 名，但如果写了 component 也必须可解析
	 *
	 * 这里的目标不是检查“硬件对不对”，而是尽早把板级描述里的
	 * 语义错误、歧义描述、以及 probe 顺序尚未满足的情况分离出来。
	 */
	/* 检查 Codec。 */
	for_each_link_codecs(link, i, dlc) {
		/*
		 * Codec 必须通过 name 或 OF node 二选一指定，不能同时指定，
		 * 也不能完全不指定。
		 */
		if (snd_soc_dlc_component_is_invalid(dlc))
			goto component_invalid;

		if (snd_soc_dlc_component_is_empty(dlc))
			goto component_empty;

		/* 必须指定 Codec DAI 名称。 */
		if (snd_soc_dlc_dai_is_empty(dlc))
			goto dai_empty;

		/*
		 * 如果 codec component 还没加入 component 列表，就延后
		 * card 注册。
		 */
		if (!soc_find_component(dlc))
			goto component_not_found;
	}

	/* 检查 Platform。 */
	for_each_link_platforms(link, i, dlc) {
		/*
		 * Platform 可以通过 name 或 OF node 指定，也可以留空；
		 * 如果留空，就不会把任何 component 插入 rtdcom 列表。
		 */
		if (snd_soc_dlc_component_is_invalid(dlc))
			goto component_invalid;

		if (snd_soc_dlc_component_is_empty(dlc))
			goto component_empty;

		/*
		 * 如果 platform component 还没加入 component 列表，就延后
		 * card 注册。
		 */
		if (!soc_find_component(dlc))
			goto component_not_found;
	}

	/* 检查 CPU。 */
	for_each_link_cpus(link, i, dlc) {
		/*
		 * CPU device 可以通过 name 或 OF node 指定，也可以留空，
		 * 最终只按 DAI name 进行匹配。
		 */
		if (snd_soc_dlc_component_is_invalid(dlc))
			goto component_invalid;


		if (snd_soc_dlc_component_is_empty(dlc)) {
			/*
			 * At least one of CPU DAI name or CPU device name/node must be specified
			 */
			if (snd_soc_dlc_dai_is_empty(dlc))
				goto component_dai_empty;
		} else {
			/*
			 * Defer card registration if Component is not added
			 */
			if (!soc_find_component(dlc))
				goto component_not_found;
		}
	}

	return 0;

component_invalid:
	dev_err(card->dev, "ASoC: Both Component name/of_node are set for %s\n", link->name);
	return -EINVAL;

component_empty:
	dev_err(card->dev, "ASoC: Neither Component name/of_node are set for %s\n", link->name);
	return -EINVAL;

component_not_found:
	dev_dbg(card->dev, "ASoC: Component %s not found for link %s\n", dlc->name, link->name);
	return -EPROBE_DEFER;

dai_empty:
	dev_err(card->dev, "ASoC: DAI name is not set for %s\n", link->name);
	return -EINVAL;

component_dai_empty:
	dev_err(card->dev, "ASoC: Neither DAI/Component name/of_node are set for %s\n", link->name);
	return -EINVAL;
}

#define MAX_DEFAULT_CH_MAP_SIZE 8
static struct snd_soc_dai_link_ch_map default_ch_map_sync[MAX_DEFAULT_CH_MAP_SIZE] = {
	{ .cpu = 0, .codec = 0 },
	{ .cpu = 1, .codec = 1 },
	{ .cpu = 2, .codec = 2 },
	{ .cpu = 3, .codec = 3 },
	{ .cpu = 4, .codec = 4 },
	{ .cpu = 5, .codec = 5 },
	{ .cpu = 6, .codec = 6 },
	{ .cpu = 7, .codec = 7 },
};
static struct snd_soc_dai_link_ch_map default_ch_map_1cpu[MAX_DEFAULT_CH_MAP_SIZE] = {
	{ .cpu = 0, .codec = 0 },
	{ .cpu = 0, .codec = 1 },
	{ .cpu = 0, .codec = 2 },
	{ .cpu = 0, .codec = 3 },
	{ .cpu = 0, .codec = 4 },
	{ .cpu = 0, .codec = 5 },
	{ .cpu = 0, .codec = 6 },
	{ .cpu = 0, .codec = 7 },
};
static struct snd_soc_dai_link_ch_map default_ch_map_1codec[MAX_DEFAULT_CH_MAP_SIZE] = {
	{ .cpu = 0, .codec = 0 },
	{ .cpu = 1, .codec = 0 },
	{ .cpu = 2, .codec = 0 },
	{ .cpu = 3, .codec = 0 },
	{ .cpu = 4, .codec = 0 },
	{ .cpu = 5, .codec = 0 },
	{ .cpu = 6, .codec = 0 },
	{ .cpu = 7, .codec = 0 },
};
static int snd_soc_compensate_channel_connection_map(struct snd_soc_card *card,
						     struct snd_soc_dai_link *dai_link)
{
	struct snd_soc_dai_link_ch_map *ch_maps;
	int i;

	/*
	 * dai_link->ch_maps 用来描述 CPU/Codec 的通道连接方式。
	 * 它表示的是从较大编号 DAI 视角看到的映射。
	 * 见 soc.h 里的 dai_link->ch_maps 示例。
	 */

	/* it should have ch_maps if connection was N:M */
	if (dai_link->num_cpus > 1 && dai_link->num_codecs > 1 &&
	    dai_link->num_cpus != dai_link->num_codecs && !dai_link->ch_maps) {
		dev_err(card->dev, "need to have ch_maps when N:M connection (%s)",
			dai_link->name);
		return -EINVAL;
	}

	/* do nothing if it has own maps */
	if (dai_link->ch_maps)
		goto sanity_check;

	/* check default map size */
	if (dai_link->num_cpus   > MAX_DEFAULT_CH_MAP_SIZE ||
	    dai_link->num_codecs > MAX_DEFAULT_CH_MAP_SIZE) {
		dev_err(card->dev, "soc-core.c needs update default_connection_maps");
		return -EINVAL;
	}

	/* Compensate missing map for ... */
	if (dai_link->num_cpus == dai_link->num_codecs)
		dai_link->ch_maps = default_ch_map_sync;	/* for 1:1 or N:N */
	else if (dai_link->num_cpus <  dai_link->num_codecs)
		dai_link->ch_maps = default_ch_map_1cpu;	/* for 1:N */
	else
		dai_link->ch_maps = default_ch_map_1codec;	/* for N:1 */

sanity_check:
	dev_dbg(card->dev, "dai_link %s\n", dai_link->stream_name);
	for_each_link_ch_maps(dai_link, i, ch_maps) {
		if ((ch_maps->cpu   >= dai_link->num_cpus) ||
		    (ch_maps->codec >= dai_link->num_codecs)) {
			dev_err(card->dev,
				"unexpected dai_link->ch_maps[%d] index (cpu(%d/%d) codec(%d/%d))",
				i,
				ch_maps->cpu,	dai_link->num_cpus,
				ch_maps->codec,	dai_link->num_codecs);
			return -EINVAL;
		}

		dev_dbg(card->dev, "  [%d] cpu%d <-> codec%d\n",
			i, ch_maps->cpu, ch_maps->codec);
	}

	return 0;
}

/**
 * snd_soc_remove_pcm_runtime - 从 card 中移除 pcm_runtime
 * @card: 持有该 pcm_runtime 的 ASoC card
 * @rtd: 需要移除的 pcm_runtime
 *
 * 该函数会把 pcm_runtime 从 ASoC card 的运行时列表中摘除。
 */
void snd_soc_remove_pcm_runtime(struct snd_soc_card *card,
				struct snd_soc_pcm_runtime *rtd)
{
	if (!rtd)
		return;

	/* card 上的 runtime 先通知 machine driver，再释放 runtime 对象。 */
	lockdep_assert_held(&client_mutex);

	/*
	 * 通知 machine driver 做额外的销毁处理。
	 */
	snd_soc_card_remove_dai_link(card, rtd->dai_link);

	soc_free_pcm_runtime(rtd);
}
EXPORT_SYMBOL_GPL(snd_soc_remove_pcm_runtime);

/**
 * snd_soc_add_pcm_runtime - 通过 dai_link 动态添加 pcm_runtime
 * @card: 需要添加 pcm_runtime 的 ASoC card
 * @dai_link: 用于创建 pcm_runtime 的 DAI link
 *
 * 该函数根据 dai_link 动态把一条链路实例化为 pcm_runtime。
 *
 * 注意：topology 可以在 probing topology component 时调用该 API 添加
 * pcm_runtime；machine driver 仍然可以在 dai_link 数组里定义静态链路。
 */
static int snd_soc_add_pcm_runtime(struct snd_soc_card *card,
				   struct snd_soc_dai_link *dai_link)
{
	/* 动态把一条 dai_link 变成真正的 pcm_runtime。 */
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai_link_component *codec, *platform, *cpu;
	struct snd_soc_component *component;
	int i, id, ret;

	lockdep_assert_held(&client_mutex);

	/*
	 * 通知 machine driver 做额外的初始化处理。
	 */
	ret = snd_soc_card_add_dai_link(card, dai_link);
	if (ret < 0)
		return ret;

	if (dai_link->ignore)
		return 0;

	dev_dbg(card->dev, "ASoC: binding %s\n", dai_link->name);

	ret = soc_dai_link_sanity_check(card, dai_link);
	if (ret < 0)
		return ret;

	rtd = soc_new_pcm_runtime(card, dai_link);
	if (!rtd)
		return -ENOMEM;

	for_each_link_cpus(dai_link, i, cpu) {
		snd_soc_rtd_to_cpu(rtd, i) = snd_soc_find_dai(cpu);
		if (!snd_soc_rtd_to_cpu(rtd, i)) {
			dev_info(card->dev, "ASoC: CPU DAI %s not registered\n",
				 cpu->dai_name);
			goto _err_defer;
		}
		snd_soc_rtd_add_component(rtd, snd_soc_rtd_to_cpu(rtd, i)->component);
	}

	/* Find CODEC from registered CODECs */
	for_each_link_codecs(dai_link, i, codec) {
		snd_soc_rtd_to_codec(rtd, i) = snd_soc_find_dai(codec);
		if (!snd_soc_rtd_to_codec(rtd, i)) {
			dev_info(card->dev, "ASoC: CODEC DAI %s not registered\n",
				 codec->dai_name);
			goto _err_defer;
		}

		snd_soc_rtd_add_component(rtd, snd_soc_rtd_to_codec(rtd, i)->component);
	}

	/* Find PLATFORM from registered PLATFORMs */
	for_each_link_platforms(dai_link, i, platform) {
		for_each_component(component) {
			if (!snd_soc_is_matching_component(platform, component))
				continue;

			if (snd_soc_component_is_dummy(component) && component->num_dai)
				continue;

			snd_soc_rtd_add_component(rtd, component);
		}
	}

	/*
	 * 大多数驱动会按 DAI link 顺序注册 PCM；
	 * 基于 topology 的驱动则可以利用 DAI link 的 id 字段设置 PCM
	 * 设备号，再用 rtd 加上 BE 的基址偏移来编号。
	 *
	 * FIXME
	 *
	 * 这一点本应通过 "dai_link" 特性实现，而不是依赖 "component" 特性。
	 */
	id = rtd->id;
	for_each_rtd_components(rtd, i, component) {
		if (!component->driver->use_dai_pcm_id)
			continue;

		if (rtd->dai_link->no_pcm)
			id += component->driver->be_pcm_base;
		else
			id = rtd->dai_link->id;
	}
	rtd->id = id;

	return 0;

_err_defer:
	snd_soc_remove_pcm_runtime(card, rtd);
	return -EPROBE_DEFER;
}

int snd_soc_add_pcm_runtimes(struct snd_soc_card *card,
			     struct snd_soc_dai_link *dai_link,
			     int num_dai_link)
{
	/* 批量添加 runtime 时，逐个先修正通道映射再创建。 */
	for (int i = 0; i < num_dai_link; i++) {
		int ret;

		ret = snd_soc_compensate_channel_connection_map(card, dai_link + i);
		if (ret < 0)
			return ret;

		ret = snd_soc_add_pcm_runtime(card, dai_link + i);
		if (ret < 0)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_add_pcm_runtimes);

static void snd_soc_runtime_get_dai_fmt(struct snd_soc_pcm_runtime *rtd)
{
	/*
	 * 从每个 DAI 的可选格式集合里协商出最终的 runtime dai_fmt。
	 *
	 * 这一层做的是“运行时格式求交集”而不是简单地读取 card 里
	 * 配好的 dai_fmt。原因是多 DAI 链路里，CPU 和 CODEC 可能各自
	 * 只支持一部分格式，最终只能从双方都支持的交集中选一个。
	 */
	struct snd_soc_dai_link *dai_link = rtd->dai_link;
	struct snd_soc_dai *dai, *not_used;
	u64 pos, possible_fmt;
	unsigned int mask = 0, dai_fmt = 0;
	int i, j, priority, pri, until;

	/*
	 * 从每个 DAI 的可选格式集合里取出可协商的格式。
	 *
	 ****************************
	 * 注意
	 * 使用 .auto_selectable_formats 不是强制要求；
	 * 也可以直接在 Sound Card 里手动指定格式。
	 * 但如果启用它，驱动应当只列出已经充分验证过的格式。
	 ****************************
	 *
	 * 例如：
	 *	auto_selectable_formats (= SND_SOC_POSSIBLE_xxx)
	 *		 (A)	 (B)	 (C)
	 *	DAI0_: { 0x000F, 0x00F0, 0x0F00 };
	 *	DAI1 : { 0xF000, 0x0F00 };
	 *		 (X)	 (Y)
	 *
	 * 这里的 until 为 3（取决于 DAI0/DAI1 数组的最大长度）。
	 * 下面的 dev_dbg() 示例用于说明协商过程。
	 *
	 * priority = 1
	 * ...
	 * found auto selected format: 0000000000000F00
	 */
	until = snd_soc_dai_get_fmt_max_priority(rtd);
	for (priority = 1; priority <= until; priority++) {
		for_each_rtd_dais(rtd, j, not_used) {

			possible_fmt = ULLONG_MAX;
			for_each_rtd_dais(rtd, i, dai) {
				u64 fmt = 0;

				pri = (j >= i) ? priority : priority - 1;
				fmt = snd_soc_dai_get_fmt(dai, pri);
				possible_fmt &= fmt;
			}
			if (possible_fmt)
				goto found;
		}
	}
	/* Not Found */
	return;
found:
	/*
	 * 把 POSSIBLE_DAIFMT 转换成最终的 DAIFMT。
	 *
	 * 某些基础/默认设置的编码值是 0，例如：
	 *	SND_SOC_DAIFMT_NB_NF
	 *	SND_SOC_DAIFMT_GATED
	 *
	 * 如果 Sound Card 显式指定了这些值，SND_SOC_DAIFMT_xxx_MASK
	 * 不容易单独识别出来，最后会被自动选择的值覆盖。
	 *
	 * 为了避免这个问题，这里从 63 递减到 0 遍历。
	 * 数值越小的 SND_SOC_POSSIBLE_xxx 优先级越高，
	 * 基础/默认设置也会被视为高优先级。
	 */
	for (i = 63; i >= 0; i--) {
		pos = 1ULL << i;
		switch (possible_fmt & pos) {
		/* 格式部分。 */
		case SND_SOC_POSSIBLE_DAIFMT_I2S:
		case SND_SOC_POSSIBLE_DAIFMT_RIGHT_J:
		case SND_SOC_POSSIBLE_DAIFMT_LEFT_J:
		case SND_SOC_POSSIBLE_DAIFMT_DSP_A:
		case SND_SOC_POSSIBLE_DAIFMT_DSP_B:
		case SND_SOC_POSSIBLE_DAIFMT_AC97:
		case SND_SOC_POSSIBLE_DAIFMT_PDM:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_FORMAT_MASK) | i;
			break;
		/* 时钟门控部分。 */
		case SND_SOC_POSSIBLE_DAIFMT_CONT:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_CLOCK_MASK) | SND_SOC_DAIFMT_CONT;
			break;
		case SND_SOC_POSSIBLE_DAIFMT_GATED:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_CLOCK_MASK) | SND_SOC_DAIFMT_GATED;
			break;
		/* 时钟极性部分。 */
		case SND_SOC_POSSIBLE_DAIFMT_NB_NF:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_INV_MASK) | SND_SOC_DAIFMT_NB_NF;
			break;
		case SND_SOC_POSSIBLE_DAIFMT_NB_IF:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_INV_MASK) | SND_SOC_DAIFMT_NB_IF;
			break;
		case SND_SOC_POSSIBLE_DAIFMT_IB_NF:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_INV_MASK) | SND_SOC_DAIFMT_IB_NF;
			break;
		case SND_SOC_POSSIBLE_DAIFMT_IB_IF:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_INV_MASK) | SND_SOC_DAIFMT_IB_IF;
			break;
		/* 主从时钟关系部分。 */
		case SND_SOC_POSSIBLE_DAIFMT_CBP_CFP:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) | SND_SOC_DAIFMT_CBP_CFP;
			break;
		case SND_SOC_POSSIBLE_DAIFMT_CBC_CFP:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) | SND_SOC_DAIFMT_CBC_CFP;
			break;
		case SND_SOC_POSSIBLE_DAIFMT_CBP_CFC:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) | SND_SOC_DAIFMT_CBP_CFC;
			break;
		case SND_SOC_POSSIBLE_DAIFMT_CBC_CFC:
			dai_fmt = (dai_fmt & ~SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) | SND_SOC_DAIFMT_CBC_CFC;
			break;
		}
	}

	/*
	 * 有些驱动会有非常复杂的限制条件。
	 * 这时用户通常希望自动选择不受限制的部分，同时手动指定复杂部分。
	 *
	 * 例如 CPU 和 Codec 都能作为时钟提供者时，用户可能希望因为器件
	 * 质量或板级约束而手动指定其中一方。
	 *
	 * 如果 Sound Card 已经手动指定了某些设置，这里就保留手动配置。
	 */
	if (!(dai_link->dai_fmt & SND_SOC_DAIFMT_FORMAT_MASK))
		mask |= SND_SOC_DAIFMT_FORMAT_MASK;
	if (!(dai_link->dai_fmt & SND_SOC_DAIFMT_CLOCK_MASK))
		mask |= SND_SOC_DAIFMT_CLOCK_MASK;
	if (!(dai_link->dai_fmt & SND_SOC_DAIFMT_INV_MASK))
		mask |= SND_SOC_DAIFMT_INV_MASK;
	if (!(dai_link->dai_fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK))
		mask |= SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK;

	dai_link->dai_fmt |= (dai_fmt & mask);
}

/**
 * snd_soc_runtime_set_dai_fmt() - 修改 ASoC runtime 的 DAI link 格式
 * @rtd: 需要修改格式的 runtime
 * @dai_fmt: 新的 DAI link 格式
 *
 * 该函数会更新指定 runtime 所连接的所有 DAI 的 link 格式。
 *
 * 注意：如果系统使用静态格式，应当直接在对应 snd_dai_link 结构体中
 * 设置 dai_fmt 字段，而不是调用这个函数。
 *
 * 返回：成功时返回 0，否则返回负错误码。
 */
int snd_soc_runtime_set_dai_fmt(struct snd_soc_pcm_runtime *rtd,
				unsigned int dai_fmt)
{
	struct snd_soc_dai *cpu_dai;
	struct snd_soc_dai *codec_dai;
	unsigned int ext_fmt;
	unsigned int i;
	int ret;

	if (!dai_fmt)
		return 0;

	/*
	 * dai_fmt has 4 types
	 *	1. SND_SOC_DAIFMT_FORMAT_MASK
	 *	2. SND_SOC_DAIFMT_CLOCK
	 *	3. SND_SOC_DAIFMT_INV
	 *	4. SND_SOC_DAIFMT_CLOCK_PROVIDER
	 *
	 * 4. CLOCK_PROVIDER is set from Codec perspective in dai_fmt. So it will be flipped
	 * when this function calls set_fmt() for CPU (CBx_CFx -> Bx_Cx). see below.
	 * This mean, we can't set CPU/Codec both are clock consumer for example.
	 * New idea handles 4. in each dai->ext_fmt. It can keep compatibility.
	 *
	 * Legacy
	 *	dai_fmt  includes 1, 2, 3, 4
	 *
	 * New idea
	 *	dai_fmt  includes 1, 2, 3
	 *	ext_fmt  includes 4
	 */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ext_fmt = rtd->dai_link->codecs[i].ext_fmt;
		ret = snd_soc_dai_set_fmt(codec_dai, dai_fmt | ext_fmt);
		if (ret != 0 && ret != -ENOTSUPP)
			return ret;
	}

	/* Flip the polarity for the "CPU" end of link */
	/* Will effect only for 4. SND_SOC_DAIFMT_CLOCK_PROVIDER */
	dai_fmt = snd_soc_daifmt_clock_provider_flipped(dai_fmt);

	for_each_rtd_cpu_dais(rtd, i, cpu_dai) {
		ext_fmt = rtd->dai_link->cpus[i].ext_fmt;
		ret = snd_soc_dai_set_fmt(cpu_dai, dai_fmt | ext_fmt);
		if (ret != 0 && ret != -ENOTSUPP)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_runtime_set_dai_fmt);

static int soc_init_pcm_runtime(struct snd_soc_card *card,
				struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai_link *dai_link = rtd->dai_link;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret;

	/* do machine specific initialization */
	ret = snd_soc_link_init(rtd);
	if (ret < 0)
		return ret;

	snd_soc_runtime_get_dai_fmt(rtd);
	ret = snd_soc_runtime_set_dai_fmt(rtd, dai_link->dai_fmt);
	if (ret)
		goto err;

	/* add DPCM sysfs entries */
	soc_dpcm_debugfs_add(rtd);

	/* create compress_device if possible */
	ret = snd_soc_dai_compress_new(cpu_dai, rtd);
	if (ret != -ENOTSUPP)
		goto err;

	/* create the pcm */
	ret = soc_new_pcm(rtd);
	if (ret < 0) {
		dev_err(card->dev, "ASoC: can't create pcm %s :%d\n",
			dai_link->stream_name, ret);
		goto err;
	}

	ret = snd_soc_pcm_dai_new(rtd);
	if (ret < 0)
		goto err;

	rtd->initialized = true;

	return 0;
err:
	snd_soc_link_exit(rtd);
	return ret;
}

static void soc_set_name_prefix(struct snd_soc_card *card,
				struct snd_soc_component *component)
{
	struct device_node *of_node = soc_component_to_node(component);
	const char *str;
	int ret, i;

	for (i = 0; i < card->num_configs; i++) {
		struct snd_soc_codec_conf *map = &card->codec_conf[i];

		if (snd_soc_is_matching_component(&map->dlc, component) &&
		    map->name_prefix) {
			component->name_prefix = map->name_prefix;
			return;
		}
	}

	/*
	 * If there is no configuration table or no match in the table,
	 * check if a prefix is provided in the node
	 */
	ret = of_property_read_string(of_node, "sound-name-prefix", &str);
	if (ret < 0)
		return;

	component->name_prefix = str;
}

static void soc_remove_component(struct snd_soc_component *component,
				 int probed)
{

	if (!component->card)
		return;

	/*
	 * component 已经成功绑到 card 上时，卸载顺序要和 probe 对称：
	 * 先让 component 自身做收尾，再从 card 的挂接链表里摘掉，最后
	 * 释放它对应的 DAPM 上下文和 debugfs 资源。
	 */
	if (probed)
		snd_soc_component_remove(component);

	list_del_init(&component->card_list);
	snd_soc_dapm_free(snd_soc_component_to_dapm(component));
	soc_cleanup_component_debugfs(component);
	component->card = NULL;
	snd_soc_component_module_put_when_remove(component);
}

static int soc_probe_component(struct snd_soc_card *card,
			       struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	struct snd_soc_dai *dai;
	int probed = 0;
	int ret;

	if (snd_soc_component_is_dummy(component))
		return 0;

	/*
	 * 同一个 component 只能绑定到一张 card 上。
	 * 如果它已经被另一张卡占用，说明板级拓扑或者 probe 顺序有问题。
	 */
	if (component->card) {
		if (component->card != card) {
			dev_err(component->dev,
				"Trying to bind component \"%s\" to card \"%s\" but is already bound to card \"%s\"\n",
				component->name, card->name, component->card->name);
			return -ENODEV;
		}
		return 0;
	}

	ret = snd_soc_component_module_get_when_probe(component);
	if (ret < 0)
		return ret;

	component->card = card;
	soc_set_name_prefix(card, component);

	/*
	 * 先初始化 debugfs / dapm 基础对象，再创建 widget、route 和
	 * 各类 control。这样后续创建失败时，收尾路径才能统一回收。
	 */
	soc_init_component_debugfs(component);

	snd_soc_dapm_init(dapm, card, component);

	ret = snd_soc_dapm_new_controls(dapm,
					component->driver->dapm_widgets,
					component->driver->num_dapm_widgets);

	if (ret != 0) {
		dev_err(component->dev,
			"Failed to create new controls %d\n", ret);
		goto err_probe;
	}

	for_each_component_dais(component, dai) {
		ret = snd_soc_dapm_new_dai_widgets(dapm, dai);
		if (ret != 0) {
			dev_err(component->dev,
				"Failed to create DAI widgets %d\n", ret);
			goto err_probe;
		}
	}

	ret = snd_soc_component_probe(component);
	if (ret < 0)
		goto err_probe;

	WARN(!snd_soc_dapm_get_idle_bias(dapm) &&
	     snd_soc_dapm_get_bias_level(dapm) != SND_SOC_BIAS_OFF,
	     "codec %s can not start from non-off bias with idle_bias_off==1\n",
	     component->name);
	probed = 1;

	/*
	 * machine specific init
	 * see
	 *	snd_soc_component_set_aux()
	 */
	ret = snd_soc_component_init(component);
	if (ret < 0)
		goto err_probe;

	ret = snd_soc_add_component_controls(component,
					     component->driver->controls,
					     component->driver->num_controls);
	if (ret < 0)
		goto err_probe;

	ret = snd_soc_dapm_add_routes(dapm,
				      component->driver->dapm_routes,
				      component->driver->num_dapm_routes);
	if (ret < 0)
		goto err_probe;

	/* 供 for_each_card_components() 遍历使用。 */
	list_add(&component->card_list, &card->component_dev_list);

err_probe:
	if (ret < 0)
		soc_remove_component(component, probed);

	return ret;
}

static void soc_remove_link_dais(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	int order;

	/*
	 * 反向卸载 DAI 时，必须按 remove_order 分轮次执行。
	 * 这样可以保证依赖较深的 DAI 先停，再拆较上层的桥接对象。
	 */
	for_each_comp_order(order) {
		for_each_card_rtds(card, rtd) {
			/* remove all rtd connected DAIs in good order */
			snd_soc_pcm_dai_remove(rtd, order);
		}
	}
}

static int soc_probe_link_dais(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	int order, ret;

	/*
	 * DAI probe 也必须按 probe_order 分轮次做。
	 * 某些 DAI 需要先初始化底层时钟、regmap 或 component 状态，
	 * 再向上暴露 runtime 能力。
	 */
	for_each_comp_order(order) {
		for_each_card_rtds(card, rtd) {
			/* probe all rtd connected DAIs in good order */
			ret = snd_soc_pcm_dai_probe(rtd, order);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static void soc_remove_link_components(struct snd_soc_card *card)
{
	struct snd_soc_component *component;
	struct snd_soc_pcm_runtime *rtd;
	int i, order;

	/*
	 * 和 probe 相反，component 的 remove 也按 remove_order 逆序拆。
	 * 这样可以避免上层 runtime 还在引用下层资源时就把底层卸掉。
	 */
	for_each_comp_order(order) {
		for_each_card_rtds(card, rtd) {
			for_each_rtd_components(rtd, i, component) {
				if (component->driver->remove_order != order)
					continue;

				soc_remove_component(component, 1);
			}
		}
	}
}

static int soc_probe_link_components(struct snd_soc_card *card)
{
	struct snd_soc_component *component;
	struct snd_soc_pcm_runtime *rtd;
	int i, ret, order;

	/*
	 * 先逐轮探测所有 link 直接引用到的 component。
	 * 这一步的意义是把“卡上会用到哪些 component”先绑好，
	 * 后面的 DAI probe / DAPM graph 创建才有稳定的宿主。
	 */
	for_each_comp_order(order) {
		for_each_card_rtds(card, rtd) {
			for_each_rtd_components(rtd, i, component) {
				if (component->driver->probe_order != order)
					continue;

				ret = soc_probe_component(card, component);
				if (ret < 0)
					return ret;
			}
		}
	}

	return 0;
}

static void soc_unbind_aux_dev(struct snd_soc_card *card)
{
	struct snd_soc_component *component, *_component;

	for_each_card_auxs_safe(card, component, _component) {
		/* 供 snd_soc_component_init() 使用。 */
		snd_soc_component_set_aux(component, NULL);
		list_del(&component->card_aux_list);
	}
}

static int soc_bind_aux_dev(struct snd_soc_card *card)
{
	struct snd_soc_component *component;
	struct snd_soc_aux_dev *aux;
	int i;

	/*
	 * AUX device 是 card 级别的附加 component，它不一定出现在
	 * 主 dai_link 里，但仍然要纳入 card 的生命周期管理。
	 */
	for_each_card_pre_auxs(card, i, aux) {
		/* codecs, usually analog devices */
		component = soc_find_component(&aux->dlc);
		if (!component)
			return -EPROBE_DEFER;

		/* for snd_soc_component_init() */
		snd_soc_component_set_aux(component, aux);
		/* 供 for_each_card_auxs() 遍历使用。 */
		list_add(&component->card_aux_list, &card->aux_comp_list);
	}
	return 0;
}

static int soc_probe_aux_devices(struct snd_soc_card *card)
{
	struct snd_soc_component *component;
	int order;
	int ret;

	for_each_comp_order(order) {
		for_each_card_auxs(card, component) {
			if (component->driver->probe_order != order)
				continue;

			ret = soc_probe_component(card,	component);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

static void soc_remove_aux_devices(struct snd_soc_card *card)
{
	struct snd_soc_component *comp, *_comp;
	int order;

	for_each_comp_order(order) {
		for_each_card_auxs_safe(card, comp, _comp) {
			if (comp->driver->remove_order == order)
				soc_remove_component(comp, 1);
		}
	}
}

#ifdef CONFIG_DMI
/*
 * 如果某个 DMI 字段包含这里列出的黑名单字符串（例如
 * "Type2 - Board Manufacturer" 或 "Type1 - TBD by OEM"），
 * 那么在根据 DMI 信息设置 card 长名时会把它判定为无效并丢弃。
 */
static const char * const dmi_blacklist[] = {
	"To be filled by OEM",
	"TBD by OEM",
	"Default String",
	"Board Manufacturer",
	"Board Vendor Name",
	"Board Product Name",
	NULL,	/* terminator */
};

/*
 * 去掉特殊字符，并把 '-' 替换成 '_'，因为 '-' 会用于分隔 card 长名中
 * 不同的 DMI 字段。这里只保留数字、字母以及少量分隔字符。
 */
static void cleanup_dmi_name(char *name)
{
	int i, j = 0;

	for (i = 0; name[i]; i++) {
		if (isalnum(name[i]) || (name[i] == '.')
		    || (name[i] == '_'))
			name[j++] = name[i];
		else if (name[i] == '-')
			name[j++] = '_';
	}

	name[j] = '\0';
}

/*
 * 检查某个 DMI 字段是否有效，也就是既不为空，也不包含黑名单字符串。
 */
static int is_dmi_valid(const char *field)
{
	int i = 0;

	if (!field[0])
		return 0;

	while (dmi_blacklist[i]) {
		if (strstr(field, dmi_blacklist[i]))
			return 0;
		i++;
	}

	return 1;
}

/*
 * 把字符串追加到 card->dmi_longname，并在过程中清理字符。
 */
static void append_dmi_string(struct snd_soc_card *card, const char *str)
{
	char *dst = card->dmi_longname;
	size_t dst_len = sizeof(card->dmi_longname);
	size_t len;

	len = strlen(dst);
	snprintf(dst + len, dst_len - len, "-%s", str);

	len++;	/* skip the separator "-" */
	if (len < dst_len)
		cleanup_dmi_name(dst + len);
}

/**
 * snd_soc_set_dmi_name() - 为 card 注册 DMI 命名
 * @card: 需要注册 DMI 名称的 card
 *
 * 某些 Intel machine driver 可能会被多个不同设备复用，但用户空间很难
 * 区分这些设备，因为 machine driver 往往把自己的名字作为 card 短名，
 * 而把 card 长名留空。为了区分这类设备，并修复因缺少设备专属配置而
 * 引发的问题，该函数允许把 DMI 信息作为 sound card 的长名，格式为
 * "vendor-product-version-board"
 * （这里用字符 '-' 分隔不同的 DMI 字段）。
 *
 * 这有助于用户空间为该 card 加载设备专属的 Use Case Manager (UCM)
 * 配置。
 *
 * 可能的 card 长名示例：
 * DellInc.-XPS139343-01-0310JH
 * ASUSTeKCOMPUTERINC.-T100TA-1.0-T100TA
 * Circuitco-MinnowboardMaxD0PLATFORM-D0-MinnowBoardMAX
 *
 * 该函数还支持为 card long name 增加 flavor，用于提供额外的区分，
 * 例如 "vendor-product-version-board-flavor"。
 *
 * 由于用户空间里的 UCM 会把 card 长名当成配置目录名，因此这里只保留
 * 数字、字母以及少量分隔字符；像空格这类特殊字符不被允许。
 *
 * 返回：成功时返回 0，否则返回负错误码。
 */
static int snd_soc_set_dmi_name(struct snd_soc_card *card)
{
	const char *vendor, *product, *board;

	if (card->long_name)
		return 0; /* 长名称已经由驱动或 DMI 设置过了。 */

	if (!dmi_available)
		return 0;

	/* 按 vendor-product-version-board 的格式拼出 DMI 长名。 */
	vendor = dmi_get_system_info(DMI_BOARD_VENDOR);
	if (!vendor || !is_dmi_valid(vendor)) {
		dev_warn(card->dev, "ASoC: no DMI vendor name!\n");
		return 0;
	}

	snprintf(card->dmi_longname, sizeof(card->dmi_longname), "%s", vendor);
	cleanup_dmi_name(card->dmi_longname);

	product = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (product && is_dmi_valid(product)) {
		const char *product_version = dmi_get_system_info(DMI_PRODUCT_VERSION);

		append_dmi_string(card, product);

		/*
		 * 某些厂商，例如 Lenovo，可能只会在 product version 字段里
		 * 放一个自解释的名字。
		 */
		if (product_version && is_dmi_valid(product_version))
			append_dmi_string(card, product_version);
	}

	board = dmi_get_system_info(DMI_BOARD_NAME);
	if (board && is_dmi_valid(board)) {
		if (!product || strcasecmp(board, product))
			append_dmi_string(card, board);
	} else if (!product) {
		/* 回退到使用 legacy 名称。 */
		dev_warn(card->dev, "ASoC: no DMI board/product name!\n");
		return 0;
	}

	/* 设置 card 的长名称。 */
	card->long_name = card->dmi_longname;

	return 0;
}
#else
static inline int snd_soc_set_dmi_name(struct snd_soc_card *card)
{
	return 0;
}
#endif /* CONFIG_DMI */

static void soc_check_tplg_fes(struct snd_soc_card *card)
{
	struct snd_soc_component *component;
	const struct snd_soc_component_driver *comp_drv;
	struct snd_soc_dai_link *dai_link;
	int i;

	for_each_component(component) {

		/* 这个 component 是否会覆盖 BE？ */
		if (!component->driver->ignore_machine)
			continue;

		/* 是针对这块 machine 吗？ */
		if (!strcmp(component->driver->ignore_machine,
			    card->dev->driver->name))
			goto match;
		if (strcmp(component->driver->ignore_machine,
			   dev_name(card->dev)))
			continue;
match:
		/* machine matches, so override the rtd data */
		for_each_card_prelinks(card, i, dai_link) {

			/* 忽略这个 FE。 */
			if (dai_link->dynamic) {
				dai_link->ignore = true;
				continue;
			}

			dev_dbg(card->dev, "info: override BE DAI link %s\n",
				card->dai_link[i].name);

			/* 覆盖 platform component。 */
			if (!dai_link->platforms) {
				dev_err(card->dev, "init platform error");
				continue;
			}

			if (component->dev->of_node)
				dai_link->platforms->of_node = component->dev->of_node;
			else
				dai_link->platforms->name = component->name;

			/* 把非 BE 链路转换成 BE。 */
			dai_link->no_pcm = 1;

			/*
			 * 覆盖所有 BE fixup。
			 * 参见
			 *	snd_soc_link_be_hw_params_fixup()
			 */
			dai_link->be_hw_params_fixup =
				component->driver->be_hw_params_fixup;

			/*
			 * 大多数 BE link 不会设置 stream name，所以如果它为空，
			 * 就把它设成 dai link name，方便绑定 widget。
			 */
			if (!dai_link->stream_name)
				dai_link->stream_name = dai_link->name;
		}

		/* 通知用户态当前使用的是替代 topology。 */
		if (component->driver->topology_name_prefix) {

			/* topology shortname 是否已经创建过？ */
			if (!card->topology_shortname_created) {
				comp_drv = component->driver;

				snprintf(card->topology_shortname, 32, "%s-%s",
					 comp_drv->topology_name_prefix,
					 card->name);
				card->topology_shortname_created = true;
			}

			/* 使用 topology shortname。 */
			card->name = card->topology_shortname;
		}
	}
}

#define soc_setup_card_name(card, name, name1, name2) \
	__soc_setup_card_name(card, name, sizeof(name), name1, name2)
static void __soc_setup_card_name(struct snd_soc_card *card,
				  char *name, int len,
				  const char *name1, const char *name2)
{
	const char *src = name1 ? name1 : name2;
	int i;

	snprintf(name, len, "%s", src);

	if (name != card->snd_card->driver)
		return;

		/*
		 * 名称规范化（driver 字段）。
		 *
		 * driver 名称比较特殊，因为它会作为用户空间搜索的 key。
		 *
		 * 例如：
		 *	"abcd??efg" -> "abcd__efg"
		 */
	for (i = 0; i < len; i++) {
		switch (name[i]) {
		case '_':
		case '-':
		case '\0':
			break;
		default:
			if (!isalnum(name[i]))
				name[i] = '_';
			break;
		}
	}

		/*
		 * driver 字段应该保持为用户可识别的合法字符串。
		 * 这里通常不太适合做自动换行，所以具体 ASoC driver 应当
		 * 直接提供更短的字符串。
		 */
	if (strlen(src) > len - 1)
		dev_err(card->dev, "ASoC: driver name too long '%s' -> '%s'\n", src, name);
}

static void soc_cleanup_card_resources(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd, *n;

	/*
	 * card 销毁时的资源回收要按“先断外部可见入口，再拆内部依赖”的
	 * 顺序执行。这里集中处理 runtime、component、DAI、AUX、DAPM 和
	 * snd_card 本体，避免残留引用导致的 use-after-free。
	 */
	if (card->snd_card)
		snd_card_disconnect_sync(card->snd_card);

	snd_soc_dapm_shutdown(card);

	/* 释放 machine 私有资源。 */
	for_each_card_rtds(card, rtd)
		if (rtd->initialized)
			snd_soc_link_exit(rtd);
	/* 在移除 DAI 和 DAPM widget 之前，先把延迟工作全部刷完。 */
	snd_soc_flush_all_delayed_work(card);

	/* 逐个移除并释放 DAI。 */
	soc_remove_link_dais(card);
	soc_remove_link_components(card);

	for_each_card_rtds_safe(card, rtd, n)
		snd_soc_remove_pcm_runtime(card, rtd);

	/* 移除辅助设备。 */
	soc_remove_aux_devices(card);
	soc_unbind_aux_dev(card);

	snd_soc_dapm_free(snd_soc_card_to_dapm(card));
	soc_cleanup_card_debugfs(card);

	/* 移除 card。 */
	snd_soc_card_remove(card);

	if (card->snd_card) {
		snd_card_free(card->snd_card);
		card->snd_card = NULL;
	}
}

static void snd_soc_unbind_card(struct snd_soc_card *card)
{
	/* instantiated 只是入口状态，真正清理在 soc_cleanup_card_resources()。 */
	if (snd_soc_card_is_instantiated(card)) {
		card->instantiated = false;
		soc_cleanup_card_resources(card);
	}
}

static int snd_soc_bind_card(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_component *component;
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	int ret;

	/* bind 是 card 进入可工作状态的总入口。 */
	snd_soc_card_mutex_lock_root(card);
	snd_soc_fill_dummy_dai(card);

	/*
	 * card 级 DAPM 先建起来，后面 runtime/component 的 widget、route 和
	 * 控制都要挂在这个上下文之下。
	 */
	snd_soc_dapm_init(dapm, card, NULL);

	/* 检查是否有平台忽略 machine FE 并改用 topology。 */
	soc_check_tplg_fes(card);

	/* 同时绑定 aux_dev。 */
	ret = soc_bind_aux_dev(card);
	if (ret < 0)
		goto probe_end;

	/* 把预定义的 DAI link 加入链表。 */
	card->num_rtd = 0;
	ret = snd_soc_add_pcm_runtimes(card, card->dai_link, card->num_links);
	if (ret < 0)
		goto probe_end;

	/* card 绑定完成后，才真正创建 sound card。 */
	ret = snd_card_new(card->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			card->owner, 0, &card->snd_card);
	if (ret < 0) {
		dev_err(card->dev,
			"ASoC: can't create sound card for card %s: %d\n",
			card->name, ret);
		goto probe_end;
	}

	soc_init_card_debugfs(card);

	soc_resume_init(card);

	ret = snd_soc_dapm_new_controls(dapm, card->dapm_widgets,
					card->num_dapm_widgets);
	if (ret < 0)
		goto probe_end;

	ret = snd_soc_dapm_new_controls(dapm, card->of_dapm_widgets,
					card->num_of_dapm_widgets);
	if (ret < 0)
		goto probe_end;

	/* sound card 只初始化一次。 */
	ret = snd_soc_card_probe(card);
	if (ret < 0)
		goto probe_end;

	/* 探测该 card 上所有被 DAI link 使用的 component。 */
	ret = soc_probe_link_components(card);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER) {
			dev_err(card->dev,
				"ASoC: failed to instantiate card %d\n", ret);
		}
		goto probe_end;
	}

	/* 探测辅助 component。 */
	ret = soc_probe_aux_devices(card);
	if (ret < 0) {
		dev_err(card->dev,
			"ASoC: failed to probe aux component %d\n", ret);
		goto probe_end;
	}

	/* 探测该 card 上所有 DAI link。 */
	ret = soc_probe_link_dais(card);
	if (ret < 0) {
		dev_err(card->dev,
			"ASoC: failed to instantiate card %d\n", ret);
		goto probe_end;
	}

	for_each_card_rtds(card, rtd) {
		ret = soc_init_pcm_runtime(card, rtd);
		if (ret < 0)
			goto probe_end;
	}

	/* runtime 就位后，把 DAI widget 真正串成 DAPM 图。 */
	snd_soc_dapm_link_dai_widgets(card);
	snd_soc_dapm_connect_dai_link_widgets(card);

	ret = snd_soc_add_card_controls(card, card->controls,
					card->num_controls);
	if (ret < 0)
		goto probe_end;

	ret = snd_soc_dapm_add_routes(dapm, card->dapm_routes,
				      card->num_dapm_routes);
	if (ret < 0)
		goto probe_end;

	ret = snd_soc_dapm_add_routes(dapm, card->of_dapm_routes,
				      card->num_of_dapm_routes);
	if (ret < 0)
		goto probe_end;

	/* 如果 DMI 可用，就尽量生成一个合理的 longname。 */
	snd_soc_set_dmi_name(card);

	soc_setup_card_name(card, card->snd_card->shortname,
			    card->name, NULL);
	soc_setup_card_name(card, card->snd_card->longname,
			    card->long_name, card->name);
	soc_setup_card_name(card, card->snd_card->driver,
			    card->driver_name, card->name);

	if (card->components) {
		/* 目前 snd_component_add() 允许在字符串里用空格分隔多个 component，
		 * 但这种情况下相同字符串的冲突检查可能不够可靠。
		 */
		ret = snd_component_add(card->snd_card, card->components);
		if (ret < 0) {
			dev_err(card->dev, "ASoC: %s snd_component_add() failed: %d\n",
				card->name, ret);
			goto probe_end;
		}
	}

	ret = snd_soc_card_late_probe(card);
	if (ret < 0)
		goto probe_end;

	snd_soc_dapm_new_widgets(card);
	snd_soc_card_fixup_controls(card);

	ret = snd_card_register(card->snd_card);
	if (ret < 0) {
		dev_err(card->dev, "ASoC: failed to register soundcard %d\n",
				ret);
		goto probe_end;
	}

	card->instantiated = 1;
	snd_soc_dapm_mark_endpoints_dirty(card);
	snd_soc_dapm_sync(dapm);

	/* 将 pin 切换到睡眠态。 */
	for_each_card_components(card, component)
		if (!snd_soc_component_active(component))
			pinctrl_pm_select_sleep_state(component->dev);

probe_end:
	if (ret < 0)
		soc_cleanup_card_resources(card);
	snd_soc_card_mutex_unlock(card);

	return ret;
}

static void devm_card_bind_release(struct device *dev, void *res)
{
	snd_soc_unregister_card(*(struct snd_soc_card **)res);
}

static int devm_snd_soc_bind_card(struct device *dev, struct snd_soc_card *card)
{
	struct snd_soc_card **ptr;
	int ret;

	/* devres 版绑定，便于设备释放时自动解绑 card。 */
	ptr = devres_alloc(devm_card_bind_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ret = snd_soc_bind_card(card);
	if (ret == 0 || ret == -EPROBE_DEFER) {
		*ptr = card;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return ret;
}

static int snd_soc_rebind_card(struct snd_soc_card *card)
{
	int ret;

	/* rebind 用于 topology / devres 触发后的重新挂载。 */
	if (card->devres_dev) {
		/* 先撤掉旧的 devres 绑定，再重新走一次绑定流程。 */
		devres_destroy(card->devres_dev, devm_card_bind_release, NULL, NULL);
		ret = devm_snd_soc_bind_card(card->devres_dev, card);
	} else {
		ret = snd_soc_bind_card(card);
	}

	/* 不是 EPROBE_DEFER 的话，说明这张卡不再需要留在等待队列里。 */
	if (ret != -EPROBE_DEFER)
		list_del_init(&card->list);

	return ret;
}

/* probes a new socdev */
static int soc_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	/* 旧式 soc-audio platform driver 入口。 */
	/*
	 * 没有 card，说明 machine driver 还没有完成注册；
	 * 这种情况下理论上不应该走到这里，直接返回错误。
	 */
	if (!card)
		return -EINVAL;

	dev_warn(&pdev->dev,
		 "ASoC: machine %s should use snd_soc_register_card()\n",
		 card->name);

	/* 在理顺实例化流程之前，暂时手动补上 card->dev。 */
	card->dev = &pdev->dev;

	return devm_snd_soc_register_card(&pdev->dev, card);
}

int snd_soc_poweroff(struct device *dev)
{
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct snd_soc_component *component;

	/*
	 * poweroff 不是普通 suspend：
	 * 系统已进入关闭阶段，不需要为“稍后恢复”保留电源状态，
	 * 所以这里会把延迟关断、DAPM 和 pinctrl 都直接收尾。
	 */
	if (!snd_soc_card_is_instantiated(card))
		return 0;

	/*
	 * 刷新 pmdown_time 的延迟工作 - 这里确实希望立即执行，
	 * 因为系统正在关机，不存在马上重启的需求。
	 */
	snd_soc_flush_all_delayed_work(card);

	snd_soc_dapm_shutdown(card);

	/* 将 pin 切换到睡眠态。 */
	for_each_card_components(card, component)
		pinctrl_pm_select_sleep_state(component->dev);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_poweroff);

const struct dev_pm_ops snd_soc_pm_ops = {
	.suspend = snd_soc_suspend,
	.resume = snd_soc_resume,
	.freeze = snd_soc_suspend,
	.thaw = snd_soc_resume,
	.poweroff = snd_soc_poweroff,
	.restore = snd_soc_resume,
};
EXPORT_SYMBOL_GPL(snd_soc_pm_ops);

/* ASoC 平台驱动。 */
static struct platform_driver soc_driver = {
	.driver		= {
		.name		= "soc-audio",
		.pm		= &snd_soc_pm_ops,
	},
	.probe		= soc_probe,
};

/**
 * snd_soc_cnew - 由模板创建新的控制项
 * @_template: 控制模板
 * @data: 控制私有数据
 * @long_name: 控制项长名称
 * @prefix: 控制名称前缀
 *
 * 通过模板控制项创建一个新的 mixer control。
 *
 * 返回：成功时返回控制项指针，失败时返回 NULL。
 */
struct snd_kcontrol *snd_soc_cnew(const struct snd_kcontrol_new *_template,
				  void *data, const char *long_name,
				  const char *prefix)
{
	struct snd_kcontrol_new template;
	struct snd_kcontrol *kcontrol;
	char *name = NULL;

	/* 复制一份模板，必要时拼接 prefix 后再交给 ALSA core 创建。 */
	memcpy(&template, _template, sizeof(template));
	template.index = 0;

	if (!long_name)
		long_name = template.name;

	if (prefix) {
		name = kasprintf(GFP_KERNEL, "%s %s", prefix, long_name);
		if (!name)
			return NULL;

		template.name = name;
	} else {
		template.name = long_name;
	}

	kcontrol = snd_ctl_new1(&template, data);

	kfree(name);

	return kcontrol;
}
EXPORT_SYMBOL_GPL(snd_soc_cnew);

static int snd_soc_add_controls(struct snd_card *card, struct device *dev,
	const struct snd_kcontrol_new *controls, int num_controls,
	const char *prefix, void *data)
{
	int i;

	/* 逐个把控制模板实例化并注册进 card。 */
	for (i = 0; i < num_controls; i++) {
		const struct snd_kcontrol_new *control = &controls[i];
		int err = snd_ctl_add(card, snd_soc_cnew(control, data,
							 control->name, prefix));
		if (err < 0) {
			dev_err(dev, "ASoC: Failed to add %s: %d\n",
				control->name, err);
			return err;
		}
	}

	return 0;
}

/**
 * snd_soc_add_component_controls - 向 component 添加一组控制项
 *
 * @component: 需要添加控制项的 component
 * @controls: 要添加的控制项数组
 * @num_controls: 数组元素个数
 *
 * 返回：成功时返回 0，否则返回错误码。
 */
int snd_soc_add_component_controls(struct snd_soc_component *component,
	const struct snd_kcontrol_new *controls, unsigned int num_controls)
{
	struct snd_card *card = component->card->snd_card;

	return snd_soc_add_controls(card, component->dev, controls,
			num_controls, component->name_prefix, component);
}
EXPORT_SYMBOL_GPL(snd_soc_add_component_controls);

/**
 * snd_soc_add_card_controls - 向 SoC card 添加一组控制项
 * 这是一个添加控制项列表的便捷函数。
 *
 * @soc_card: 需要添加控制项的 SoC card
 * @controls: 要添加的控制项数组
 * @num_controls: 数组元素个数
 *
 * 返回：成功时返回 0，否则返回错误码。
 */
int snd_soc_add_card_controls(struct snd_soc_card *soc_card,
	const struct snd_kcontrol_new *controls, int num_controls)
{
	struct snd_card *card = soc_card->snd_card;

	return snd_soc_add_controls(card, soc_card->dev, controls, num_controls,
			NULL, soc_card);
}
EXPORT_SYMBOL_GPL(snd_soc_add_card_controls);

/**
 * snd_soc_add_dai_controls - 向 DAI 添加一组控制项
 * 这是一个添加控制项列表的便捷函数。
 *
 * @dai: 需要添加控制项的 DAI
 * @controls: 要添加的控制项数组
 * @num_controls: 数组元素个数
 *
 * 返回：成功时返回 0，否则返回错误码。
 */
int snd_soc_add_dai_controls(struct snd_soc_dai *dai,
	const struct snd_kcontrol_new *controls, int num_controls)
{
	struct snd_card *card = dai->component->card->snd_card;

	return snd_soc_add_controls(card, dai->dev, controls, num_controls,
			NULL, dai);
}
EXPORT_SYMBOL_GPL(snd_soc_add_dai_controls);

/**
 * snd_soc_register_card - 向 ASoC core 注册 card
 *
 * @card: 需要注册的 card
 *
 * 注册 card 之前会先完成其基础初始化。
 */
int snd_soc_register_card(struct snd_soc_card *card)
{
	int ret;

	/* register_card 负责初始化 card/DAPM/锁，并启动 bind 流程。 */
	if (!card->name || !card->dev)
		return -EINVAL;

	card->dapm = snd_soc_dapm_alloc(card->dev);
	if (!card->dapm)
		return -ENOMEM;

	dev_set_drvdata(card->dev, card);

	INIT_LIST_HEAD(&card->widgets);
	INIT_LIST_HEAD(&card->paths);
	INIT_LIST_HEAD(&card->dapm_list);
	INIT_LIST_HEAD(&card->aux_comp_list);
	INIT_LIST_HEAD(&card->component_dev_list);
	INIT_LIST_HEAD(&card->list);
	INIT_LIST_HEAD(&card->rtd_list);
	INIT_LIST_HEAD(&card->dapm_dirty);

	card->instantiated = 0;
	mutex_init(&card->mutex);
	mutex_init(&card->dapm_mutex);
	mutex_init(&card->pcm_mutex);

	guard(mutex)(&client_mutex);

	/* devres 模式下可能因为依赖未就绪而先挂到等待重绑链表里。 */
	if (card->devres_dev) {
		ret = devm_snd_soc_bind_card(card->devres_dev, card);
		if (ret == -EPROBE_DEFER) {
			list_add(&card->list, &unbind_card_list);
			ret = 0;
		}
	} else {
		ret = snd_soc_bind_card(card);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_register_card);

/**
 * snd_soc_unregister_card - 从 ASoC core 注销 card
 *
 * @card: 需要注销的 card
 */
void snd_soc_unregister_card(struct snd_soc_card *card)
{
	/* unregister 先解绑再从全局链表删掉。 */
	guard(mutex)(&client_mutex);

	snd_soc_unbind_card(card);
	list_del(&card->list);

	dev_dbg(card->dev, "ASoC: Unregistered card '%s'\n", card->name);
}
EXPORT_SYMBOL_GPL(snd_soc_unregister_card);

/*
 * 规范化单设备名字，并去掉 ".-1" 之类的 legacy 后缀。
 * 这样生成的名字更适合用于 dai_link 匹配。
 */
static char *fmt_single_name(struct device *dev, int *id)
{
	/* 把单设备名字规范化，顺便生成一个稳定 id。 */
	const char *devname = dev_name(dev);
	char *found, *name;
	unsigned int id1, id2;
	int __id;

	if (devname == NULL)
		return NULL;

	name = devm_kstrdup(dev, devname, GFP_KERNEL);
	if (!name)
		return NULL;

	/* 检查是否是 "%s.%d" 这类名字（platform / SPI 组件）。 */
	found = strstr(name, dev->driver->name);
	if (found) {
		/* 解析 ID。 */
		if (sscanf(&found[strlen(dev->driver->name)], ".%d", &__id) == 1) {

			/* 如果 ID == -1，就把名字里的后缀去掉。 */
			if (__id == -1)
				found[strlen(dev->driver->name)] = '\0';
		}

	/* I2C component 设备通常命名为 "bus-addr"。 */
	} else if (sscanf(name, "%x-%x", &id1, &id2) == 2) {

		/* 根据 I2C 地址和 bus 号生成唯一 ID。 */
		__id = ((id1 & 0xffff) << 16) + id2;

		devm_kfree(dev, name);

		/* 对 component 名称做一次清理，方便后续创建 DAI link。 */
		name = devm_kasprintf(dev, GFP_KERNEL, "%s.%s", dev->driver->name, devname);
	} else {
		__id = 0;
	}

	if (id)
		*id = __id;

	return name;
}

/*
 * 对于一个设备上挂多个 DAI 的情况，直接用 DAI 自己的名字，
 * 避免继续沿用设备名造成歧义。
 */
static inline char *fmt_multiple_name(struct device *dev,
		struct snd_soc_dai_driver *dai_drv)
{
	/* 多 DAI 场景直接沿用 DAI driver 自身名字。 */
	if (dai_drv->name == NULL) {
		dev_err(dev,
			"ASoC: error - multiple DAI %s registered with no name\n",
			dev_name(dev));
		return NULL;
	}

	return devm_kstrdup(dev, dai_drv->name, GFP_KERNEL);
}

void snd_soc_unregister_dai(struct snd_soc_dai *dai)
{
	/* DAI 解绑只负责从 component->dai_list 移除。 */
	lockdep_assert_held(&client_mutex);

	dev_dbg(dai->dev, "ASoC: Unregistered DAI '%s'\n", dai->name);
	list_del(&dai->list);
}
EXPORT_SYMBOL_GPL(snd_soc_unregister_dai);

/**
 * snd_soc_register_dai - 动态注册 DAI 并创建其 widget
 *
 * @component: DAI 所属的 component
 * @dai_drv: DAI 使用的 driver 描述
 * @legacy_dai_naming: 为 %true 时使用旧式单名字格式；
 * 	为 %false 时使用多名字格式；
 *
 * topology 可以在 probing component 时调用该 API 注册 DAI。
 * 这些 DAI 的 widget 会在 card 清理阶段释放，而 DAI 本体会在
 * component 清理阶段释放。
 */
struct snd_soc_dai *snd_soc_register_dai(struct snd_soc_component *component,
					 struct snd_soc_dai_driver *dai_drv,
					 bool legacy_dai_naming)
{
	struct device *dev = component->dev;
	struct snd_soc_dai *dai;

	/* 以 component 为宿主创建一个运行时 DAI。 */
	lockdep_assert_held(&client_mutex);

	dai = devm_kzalloc(dev, sizeof(*dai), GFP_KERNEL);
	if (dai == NULL)
		return NULL;

	/*
	 * 在早期还存在 component-less DAI 的年代，这类 DAI 没有静态名字，
	 * 而是继承父设备的名字，因此可以注册多个同名实例。即使现在这些
	 * DAI 已经不再是 component-less 了，仍要保留同样的命名风格。
	 */
	if (legacy_dai_naming &&
	    (dai_drv->id == 0 || dai_drv->name == NULL)) {
		dai->name = fmt_single_name(dev, &dai->id);
	} else {
		dai->name = fmt_multiple_name(dev, dai_drv);
		if (dai_drv->id)
			dai->id = dai_drv->id;
		else
			dai->id = component->num_dai;
	}
	if (!dai->name)
		return NULL;

	dai->component = component;
	dai->dev = dev;
	dai->driver = dai_drv;

	/* 供 for_each_component_dais() 遍历使用。 */
	list_add_tail(&dai->list, &component->dai_list);
	component->num_dai++;

	dev_dbg(dev, "ASoC: Registered DAI '%s'\n", dai->name);
	return dai;
}
EXPORT_SYMBOL_GPL(snd_soc_register_dai);

/**
 * snd_soc_unregister_dais - 从 ASoC core 注销该 component 下的所有 DAI
 *
 * @component: 需要注销 DAI 的 component
 */
static void snd_soc_unregister_dais(struct snd_soc_component *component)
{
	/* 逐个回收 component 下的所有 DAI。 */
	struct snd_soc_dai *dai, *_dai;

	for_each_component_dais_safe(component, dai, _dai)
		snd_soc_unregister_dai(dai);
}

/**
 * snd_soc_register_dais - 向 ASoC core 批量注册 DAI
 *
 * @component: 这些 DAI 所属的 component
 * @dai_drv: DAI driver 数组
 * @count: DAI 数量
 */
static int snd_soc_register_dais(struct snd_soc_component *component,
				 struct snd_soc_dai_driver *dai_drv,
				 size_t count)
{
	/* 批量注册 DAI，任一步失败则回滚全部。 */
	struct snd_soc_dai *dai;
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		dai = snd_soc_register_dai(component, dai_drv + i, count == 1 &&
					   component->driver->legacy_dai_naming);
		if (dai == NULL) {
			ret = -ENOMEM;
			goto err;
		}
	}

	return 0;

err:
	snd_soc_unregister_dais(component);

	return ret;
}

#define ENDIANNESS_MAP(name) \
	(SNDRV_PCM_FMTBIT_##name##LE | SNDRV_PCM_FMTBIT_##name##BE)
static u64 endianness_format_map[] = {
	ENDIANNESS_MAP(S16_),
	ENDIANNESS_MAP(U16_),
	ENDIANNESS_MAP(S24_),
	ENDIANNESS_MAP(U24_),
	ENDIANNESS_MAP(S32_),
	ENDIANNESS_MAP(U32_),
	ENDIANNESS_MAP(S24_3),
	ENDIANNESS_MAP(U24_3),
	ENDIANNESS_MAP(S20_3),
	ENDIANNESS_MAP(U20_3),
	ENDIANNESS_MAP(S18_3),
	ENDIANNESS_MAP(U18_3),
	ENDIANNESS_MAP(FLOAT_),
	ENDIANNESS_MAP(FLOAT64_),
	ENDIANNESS_MAP(IEC958_SUBFRAME_),
};

/*
 * 针对字节序修正 DAI 格式：
 * codec 实际上并不感知数据字节序，但这里使用的是 CPU 侧格式定义，
 * 这些定义需要区分字节序。因此要保证 codec DAI 同时具备大端和小端
 * 版本。
 */
static void convert_endianness_formats(struct snd_soc_pcm_stream *stream)
{
	/* 让 codec DAI 同时声明 LE/BE 版本格式，避免对端匹配失败。 */
	int i;

	for (i = 0; i < ARRAY_SIZE(endianness_format_map); i++)
		if (stream->formats & endianness_format_map[i])
			stream->formats |= endianness_format_map[i];
}

static void snd_soc_del_component_unlocked(struct snd_soc_component *component)
{
	/* 解绑 component 时先清 DAI，再处理 card 关联。 */
	struct snd_soc_card *card = component->card;
	bool instantiated;

	snd_soc_unregister_dais(component);

	if (card) {
		instantiated = card->instantiated;
		snd_soc_unbind_card(card);
		if (instantiated)
			list_add(&card->list, &unbind_card_list);
	}

	list_del(&component->list);
}

int snd_soc_component_initialize(struct snd_soc_component *component,
				 const struct snd_soc_component_driver *driver,
				 struct device *dev)
{
	/* 初始化 component 的运行时对象和默认链表。 */
	component->dapm = snd_soc_dapm_alloc(dev);
	if (!component->dapm)
		return -ENOMEM;

	INIT_LIST_HEAD(&component->dai_list);
	INIT_LIST_HEAD(&component->dobj_list);
	INIT_LIST_HEAD(&component->card_list);
	INIT_LIST_HEAD(&component->list);
	INIT_LIST_HEAD(&component->card_aux_list);
	mutex_init(&component->io_mutex);

	if (!component->name) {
		component->name = fmt_single_name(dev, NULL);
		if (!component->name) {
			dev_err(dev, "ASoC: Failed to allocate name\n");
			return -ENOMEM;
		}
	}

	component->dev		= dev;
	component->driver	= driver;

#ifdef CONFIG_DEBUG_FS
	if (!component->debugfs_prefix)
		component->debugfs_prefix = driver->debugfs_prefix;
#endif

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_component_initialize);

int snd_soc_add_component(struct snd_soc_component *component,
			  struct snd_soc_dai_driver *dai_drv,
			  int num_dai)
{
	struct snd_soc_card *card, *c;
	int ret;
	int i;
	guard(mutex)(&client_mutex);

	/*
	 * component 注册的顺序很固定：
	 * 1) 先把 DAI driver 变成运行时 DAI
	 * 2) 再把 component 加入全局表
	 * 3) 然后唤醒之前因为依赖未就绪而挂起的 card
	 */
	/* 先注册 DAI，再把 component 插入全局列表，最后尝试重绑等待中的 card。 */
	if (component->driver->endianness) {
		for (i = 0; i < num_dai; i++) {
			convert_endianness_formats(&dai_drv[i].playback);
			convert_endianness_formats(&dai_drv[i].capture);
		}
	}

	ret = snd_soc_register_dais(component, dai_drv, num_dai);
	if (ret < 0) {
		dev_err(component->dev, "ASoC: Failed to register DAIs: %d\n",
			ret);
		goto err_cleanup;
	}

	if (!component->driver->write && !component->driver->read) {
		/*
		 * 如果驱动没自己实现寄存器访问，优先尝试从设备上挂接的
		 * regmap 取一个默认实现。
		 */
		if (!component->regmap)
			component->regmap = dev_get_regmap(component->dev,
							   NULL);
	}

	/* 供 for_each_component() 遍历使用。 */
	list_add(&component->list, &component_list);

	/* 如果有卡在等待这个 component，就在这里重新走一次绑定流程。 */
	list_for_each_entry_safe(card, c, &unbind_card_list, list)
		snd_soc_rebind_card(card);

err_cleanup:
	if (ret < 0)
		snd_soc_del_component_unlocked(component);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_add_component);

int snd_soc_register_component(struct device *dev,
			const struct snd_soc_component_driver *component_driver,
			struct snd_soc_dai_driver *dai_drv,
			int num_dai)
{
	struct snd_soc_component *component;
	int ret;

	/* devm 版 component 注册。 */
	component = devm_kzalloc(dev, sizeof(*component), GFP_KERNEL);
	if (!component)
		return -ENOMEM;

	ret = snd_soc_component_initialize(component, component_driver, dev);
	if (ret < 0)
		return ret;

	return snd_soc_add_component(component, dai_drv, num_dai);
}
EXPORT_SYMBOL_GPL(snd_soc_register_component);

/**
 * snd_soc_unregister_component_by_driver - 按给定 driver 从 ASoC core 注销 component
 *
 * @dev: 需要注销的设备
 * @component_driver: 需要注销的 component driver
 */
void snd_soc_unregister_component_by_driver(struct device *dev,
					    const struct snd_soc_component_driver *component_driver)
{
	const char *driver_name = NULL;

	/* 按 driver 名称逐个回收该设备下的 component。 */
	if (component_driver)
		driver_name = component_driver->name;

	guard(mutex)(&client_mutex);

	while (1) {
		struct snd_soc_component *component = snd_soc_lookup_component_nolocked(dev, driver_name);

		if (!component)
			break;

		/* 解除 component 时会同时把 DAI、card 关联和全局链表都拆掉。 */
		snd_soc_del_component_unlocked(component);
	}
}
EXPORT_SYMBOL_GPL(snd_soc_unregister_component_by_driver);

/* 从 device tree 中读取 card 名称。 */
int snd_soc_of_parse_card_name(struct snd_soc_card *card,
			       const char *propname)
{
	/* 从 DT 里取 card 名。 */
	struct device_node *np;
	int ret;

	if (!card->dev) {
		pr_err("card->dev is not set before calling %s\n", __func__);
		return -EINVAL;
	}

	np = card->dev->of_node;

	ret = of_property_read_string_index(np, propname, 0, &card->name);
	/*
	 * EINVAL 表示该属性不存在。只要 card->name 之前已经被设置过，
	 * 这就是允许的；后面在 snd_soc_register_card 里会再检查。
	 */
	if (ret < 0 && ret != -EINVAL) {
		dev_err(card->dev,
			"ASoC: Property '%s' could not be read: %d\n",
			propname, ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_of_parse_card_name);

static const struct snd_soc_dapm_widget simple_widgets[] = {
	SND_SOC_DAPM_MIC("Microphone", NULL),
	SND_SOC_DAPM_LINE("Line", NULL),
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

int snd_soc_of_parse_audio_simple_widgets(struct snd_soc_card *card,
					  const char *propname)
{
	struct device_node *np = card->dev->of_node;
	struct snd_soc_dapm_widget *widgets;
	const char *template, *wname;
	int i, j, num_widgets;

	num_widgets = of_property_count_strings(np, propname);
	if (num_widgets < 0) {
		dev_err(card->dev,
			"ASoC: Property '%s' does not exist\n",	propname);
		return -EINVAL;
	}
	if (!num_widgets) {
		dev_err(card->dev, "ASoC: Property '%s's length is zero\n",
			propname);
		return -EINVAL;
	}
	if (num_widgets & 1) {
		dev_err(card->dev,
			"ASoC: Property '%s' length is not even\n", propname);
		return -EINVAL;
	}

	num_widgets /= 2;

	widgets = devm_kcalloc(card->dev, num_widgets, sizeof(*widgets),
			       GFP_KERNEL);
	if (!widgets) {
		dev_err(card->dev,
			"ASoC: Could not allocate memory for widgets\n");
		return -ENOMEM;
	}

	for (i = 0; i < num_widgets; i++) {
		int ret = of_property_read_string_index(np, propname,
							2 * i, &template);
		if (ret) {
			dev_err(card->dev,
				"ASoC: Property '%s' index %d read error:%d\n",
				propname, 2 * i, ret);
			return -EINVAL;
		}

		for (j = 0; j < ARRAY_SIZE(simple_widgets); j++) {
			if (!strncmp(template, simple_widgets[j].name,
				     strlen(simple_widgets[j].name))) {
				widgets[i] = simple_widgets[j];
				break;
			}
		}

		if (j >= ARRAY_SIZE(simple_widgets)) {
			dev_err(card->dev,
				"ASoC: DAPM widget '%s' is not supported\n",
				template);
			return -EINVAL;
		}

		ret = of_property_read_string_index(np, propname,
						    (2 * i) + 1,
						    &wname);
		if (ret) {
			dev_err(card->dev,
				"ASoC: Property '%s' index %d read error:%d\n",
				propname, (2 * i) + 1, ret);
			return -EINVAL;
		}

		widgets[i].name = wname;
	}

	card->of_dapm_widgets = widgets;
	card->num_of_dapm_widgets = num_widgets;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_of_parse_audio_simple_widgets);

int snd_soc_of_parse_pin_switches(struct snd_soc_card *card, const char *prop)
{
	const unsigned int nb_controls_max = 16;
	const char **strings, *control_name;
	struct snd_kcontrol_new *controls;
	struct device *dev = card->dev;
	unsigned int i, nb_controls;
	int ret;

	if (!of_property_present(dev->of_node, prop))
		return 0;

	strings = devm_kcalloc(dev, nb_controls_max,
			       sizeof(*strings), GFP_KERNEL);
	if (!strings)
		return -ENOMEM;

	ret = of_property_read_string_array(dev->of_node, prop,
					    strings, nb_controls_max);
	if (ret < 0)
		return ret;

	nb_controls = (unsigned int)ret;

	controls = devm_kcalloc(dev, nb_controls,
				sizeof(*controls), GFP_KERNEL);
	if (!controls)
		return -ENOMEM;

	for (i = 0; i < nb_controls; i++) {
		control_name = devm_kasprintf(dev, GFP_KERNEL,
					      "%s Switch", strings[i]);
		if (!control_name)
			return -ENOMEM;

		controls[i].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		controls[i].name = control_name;
		controls[i].info = snd_soc_dapm_info_pin_switch;
		controls[i].get = snd_soc_dapm_get_pin_switch;
		controls[i].put = snd_soc_dapm_put_pin_switch;
		controls[i].private_value = (unsigned long)strings[i];
	}

	card->controls = controls;
	card->num_controls = nb_controls;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_of_parse_pin_switches);

int snd_soc_of_get_slot_mask(struct device_node *np,
			     const char *prop_name,
			     unsigned int *mask)
{
	u32 val;
	const __be32 *of_slot_mask = of_get_property(np, prop_name, &val);
	int i;

	if (!of_slot_mask)
		return 0;
	val /= sizeof(u32);
	for (i = 0; i < val; i++)
		if (be32_to_cpup(&of_slot_mask[i]))
			*mask |= (1 << i);

	return val;
}
EXPORT_SYMBOL_GPL(snd_soc_of_get_slot_mask);

int snd_soc_of_parse_tdm_slot(struct device_node *np,
			      unsigned int *tx_mask,
			      unsigned int *rx_mask,
			      unsigned int *slots,
			      unsigned int *slot_width)
{
	u32 val;
	int ret;

	if (tx_mask)
		snd_soc_of_get_slot_mask(np, "dai-tdm-slot-tx-mask", tx_mask);
	if (rx_mask)
		snd_soc_of_get_slot_mask(np, "dai-tdm-slot-rx-mask", rx_mask);

	ret = of_property_read_u32(np, "dai-tdm-slot-num", &val);
	if (ret && ret != -EINVAL)
		return ret;
	if (!ret && slots)
		*slots = val;

	ret = of_property_read_u32(np, "dai-tdm-slot-width", &val);
	if (ret && ret != -EINVAL)
		return ret;
	if (!ret && slot_width)
		*slot_width = val;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_of_parse_tdm_slot);

void snd_soc_dlc_use_cpu_as_platform(struct snd_soc_dai_link_component *platforms,
				     struct snd_soc_dai_link_component *cpus)
{
	platforms->of_node	= cpus->of_node;
	platforms->dai_args	= cpus->dai_args;
}
EXPORT_SYMBOL_GPL(snd_soc_dlc_use_cpu_as_platform);

void snd_soc_of_parse_node_prefix(struct device_node *np,
				  struct snd_soc_codec_conf *codec_conf,
				  struct device_node *of_node,
				  const char *propname)
{
	const char *str;
	int ret;

	ret = of_property_read_string(np, propname, &str);
	if (ret < 0) {
	/* 没有 prefix 不是错误。 */
		return;
	}

	codec_conf->dlc.of_node	= of_node;
	codec_conf->name_prefix	= str;
}
EXPORT_SYMBOL_GPL(snd_soc_of_parse_node_prefix);

int snd_soc_of_parse_audio_routing(struct snd_soc_card *card,
				   const char *propname)
{
	struct device_node *np = card->dev->of_node;
	int num_routes;
	struct snd_soc_dapm_route *routes;
	int i;

	num_routes = of_property_count_strings(np, propname);
	if (num_routes < 0 || num_routes & 1) {
		dev_err(card->dev,
			"ASoC: Property '%s' does not exist or its length is not even\n",
			propname);
		return -EINVAL;
	}
	num_routes /= 2;

	routes = devm_kcalloc(card->dev, num_routes, sizeof(*routes),
			      GFP_KERNEL);
	if (!routes) {
		dev_err(card->dev,
			"ASoC: Could not allocate DAPM route table\n");
		return -ENOMEM;
	}

	for (i = 0; i < num_routes; i++) {
		int ret = of_property_read_string_index(np, propname,
							2 * i, &routes[i].sink);
		if (ret) {
			dev_err(card->dev,
				"ASoC: Property '%s' index %d could not be read: %d\n",
				propname, 2 * i, ret);
			return -EINVAL;
		}
		ret = of_property_read_string_index(np, propname,
			(2 * i) + 1, &routes[i].source);
		if (ret) {
			dev_err(card->dev,
				"ASoC: Property '%s' index %d could not be read: %d\n",
				propname, (2 * i) + 1, ret);
			return -EINVAL;
		}
	}

	card->num_of_dapm_routes = num_routes;
	card->of_dapm_routes = routes;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_of_parse_audio_routing);

int snd_soc_of_parse_aux_devs(struct snd_soc_card *card, const char *propname)
{
	struct device_node *node = card->dev->of_node;
	struct snd_soc_aux_dev *aux;
	int num, i;

	num = of_count_phandle_with_args(node, propname, NULL);
	if (num == -ENOENT) {
		return 0;
	} else if (num < 0) {
		dev_err(card->dev, "ASOC: Property '%s' could not be read: %d\n",
			propname, num);
		return num;
	}

	aux = devm_kcalloc(card->dev, num, sizeof(*aux), GFP_KERNEL);
	if (!aux)
		return -ENOMEM;
	card->aux_dev = aux;
	card->num_aux_devs = num;

	for_each_card_pre_auxs(card, i, aux) {
		aux->dlc.of_node = of_parse_phandle(node, propname, i);
		if (!aux->dlc.of_node)
			return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_of_parse_aux_devs);

unsigned int snd_soc_daifmt_clock_provider_flipped(unsigned int dai_fmt)
{
	unsigned int inv_dai_fmt = dai_fmt & ~SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK;

	switch (dai_fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBP_CFP:
		inv_dai_fmt |= SND_SOC_DAIFMT_CBC_CFC;
		break;
	case SND_SOC_DAIFMT_CBP_CFC:
		inv_dai_fmt |= SND_SOC_DAIFMT_CBC_CFP;
		break;
	case SND_SOC_DAIFMT_CBC_CFP:
		inv_dai_fmt |= SND_SOC_DAIFMT_CBP_CFC;
		break;
	case SND_SOC_DAIFMT_CBC_CFC:
		inv_dai_fmt |= SND_SOC_DAIFMT_CBP_CFP;
		break;
	}

	return inv_dai_fmt;
}
EXPORT_SYMBOL_GPL(snd_soc_daifmt_clock_provider_flipped);

unsigned int snd_soc_daifmt_clock_provider_from_bitmap(unsigned int bit_frame)
{
	/*
	 * bit_frame 是 snd_soc_daifmt_parse_clock_provider_raw() 的返回值。
	 */

	/* 以 Codec 侧的约定为基准。 */
	switch (bit_frame) {
	case 0x11:
		return SND_SOC_DAIFMT_CBP_CFP;
	case 0x10:
		return SND_SOC_DAIFMT_CBP_CFC;
	case 0x01:
		return SND_SOC_DAIFMT_CBC_CFP;
	default:
		return SND_SOC_DAIFMT_CBC_CFC;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_daifmt_clock_provider_from_bitmap);

unsigned int snd_soc_daifmt_parse_format(struct device_node *np,
					 const char *prefix)
{
	int ret;
	char prop[128];
	unsigned int format = 0;
	int bit, frame;
	const char *str;
	struct {
		char *name;
		unsigned int val;
	} of_fmt_table[] = {
		{ "i2s",	SND_SOC_DAIFMT_I2S },
		{ "right_j",	SND_SOC_DAIFMT_RIGHT_J },
		{ "left_j",	SND_SOC_DAIFMT_LEFT_J },
		{ "dsp_a",	SND_SOC_DAIFMT_DSP_A },
		{ "dsp_b",	SND_SOC_DAIFMT_DSP_B },
		{ "ac97",	SND_SOC_DAIFMT_AC97 },
		{ "pdm",	SND_SOC_DAIFMT_PDM},
		{ "msb",	SND_SOC_DAIFMT_MSB },
		{ "lsb",	SND_SOC_DAIFMT_LSB },
	};

	if (!prefix)
		prefix = "";

	/*
	 * 检查 "dai-format = xxx" 或 "[prefix]format = xxx"。
	 * 这里对应 SND_SOC_DAIFMT_FORMAT_MASK 区域。
	 */
	ret = of_property_read_string(np, "dai-format", &str);
	if (ret < 0) {
		snprintf(prop, sizeof(prop), "%sformat", prefix);
		ret = of_property_read_string(np, prop, &str);
	}
	if (ret == 0) {
		int i;

		for (i = 0; i < ARRAY_SIZE(of_fmt_table); i++) {
			if (strcmp(str, of_fmt_table[i].name) == 0) {
				format |= of_fmt_table[i].val;
				break;
			}
		}
	}

	/*
	 * 检查 "[prefix]continuous-clock"。
	 * 这里对应 SND_SOC_DAIFMT_CLOCK_MASK 区域。
	 */
	snprintf(prop, sizeof(prop), "%scontinuous-clock", prefix);
	if (of_property_read_bool(np, prop))
		format |= SND_SOC_DAIFMT_CONT;
	else
		format |= SND_SOC_DAIFMT_GATED;

	/*
	 * 检查 "[prefix]bitclock-inversion" 和 "[prefix]frame-inversion"。
	 * 这里对应 SND_SOC_DAIFMT_INV_MASK 区域。
	 */
	snprintf(prop, sizeof(prop), "%sbitclock-inversion", prefix);
	bit = of_property_read_bool(np, prop);

	snprintf(prop, sizeof(prop), "%sframe-inversion", prefix);
	frame = of_property_read_bool(np, prop);

	switch ((bit << 4) + frame) {
	case 0x11:
		format |= SND_SOC_DAIFMT_IB_IF;
		break;
	case 0x10:
		format |= SND_SOC_DAIFMT_IB_NF;
		break;
	case 0x01:
		format |= SND_SOC_DAIFMT_NB_IF;
		break;
	default:
		/* SND_SOC_DAIFMT_NB_NF 是默认值。 */
		break;
	}

	return format;
}
EXPORT_SYMBOL_GPL(snd_soc_daifmt_parse_format);

unsigned int snd_soc_daifmt_parse_clock_provider_raw(struct device_node *np,
						     const char *prefix,
						     struct device_node **bitclkmaster,
						     struct device_node **framemaster)
{
	char prop[128];
	unsigned int bit, frame;

	if (!np)
		return 0;

	if (!prefix)
		prefix = "";

	/*
	 * 检查 "[prefix]bitclock-master" 和 "[prefix]frame-master"。
	 */
	snprintf(prop, sizeof(prop), "%sbitclock-master", prefix);
	bit = of_property_present(np, prop);
	if (bit && bitclkmaster)
		*bitclkmaster = of_parse_phandle(np, prop, 0);

	snprintf(prop, sizeof(prop), "%sframe-master", prefix);
	frame = of_property_present(np, prop);
	if (frame && framemaster)
		*framemaster = of_parse_phandle(np, prop, 0);

	/*
	 * 返回位图，供 snd_soc_daifmt_clock_provider_from_bitmap() 使用。
	 */
	return (bit << 4) + frame;
}
EXPORT_SYMBOL_GPL(snd_soc_daifmt_parse_clock_provider_raw);

int snd_soc_get_stream_cpu(const struct snd_soc_dai_link *dai_link, int stream)
{
	/*
	 * [Normal]
	 *
	 * Playback
	 *	CPU  : SNDRV_PCM_STREAM_PLAYBACK
	 *	Codec: SNDRV_PCM_STREAM_PLAYBACK
	 *
	 * Capture
	 *	CPU  : SNDRV_PCM_STREAM_CAPTURE
	 *	Codec: SNDRV_PCM_STREAM_CAPTURE
	 */
	if (!dai_link->c2c_params)
		return stream;

	/*
	 * [Codec2Codec]
	 *
	 * Playback
	 *	CPU  : SNDRV_PCM_STREAM_CAPTURE
	 *	Codec: SNDRV_PCM_STREAM_PLAYBACK
	 *
	 * Capture
	 *	CPU  : SNDRV_PCM_STREAM_PLAYBACK
	 *	Codec: SNDRV_PCM_STREAM_CAPTURE
	 */
	if (stream == SNDRV_PCM_STREAM_CAPTURE)
		return SNDRV_PCM_STREAM_PLAYBACK;

	return SNDRV_PCM_STREAM_CAPTURE;
}
EXPORT_SYMBOL_GPL(snd_soc_get_stream_cpu);

int snd_soc_get_dai_id(struct device_node *ep)
{
	struct snd_soc_dai_link_component dlc = {
		.of_node = of_graph_get_port_parent(ep),
	};
	int ret;


	/*
	 * 这里只拿 endpoint 所属的 port_parent 去找 DAI id。
	 * 像 HDMI 这类设备会同时挂视频和音频端口，ALSA 只关心音频那一侧，
	 * 所以不能简单按 graph 里 endpoint 的顺序去数，必须让驱动提供
	 * 自己的 .of_xlate_dai_id 逻辑。
	 */
	ret = -ENOTSUPP;

	scoped_guard(mutex, &client_mutex) {
		struct snd_soc_component *component = soc_find_component(&dlc);

		if (component)
			ret = snd_soc_component_of_xlate_dai_id(component, ep);
	}

	of_node_put(dlc.of_node);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_get_dai_id);

int snd_soc_get_dlc(const struct of_phandle_args *args, struct snd_soc_dai_link_component *dlc)
{
	struct snd_soc_component *pos;
	int ret = -EPROBE_DEFER;
	guard(mutex)(&client_mutex);

	/*
	 * 从 DT phandle + cells 里解析出一个可用于 dai_link 的端点描述。
	 * 解析顺序是：
	 * 1) 找到对应 component
	 * 2) 让 component 的 of_xlate 回调优先把 args 映射成 DAI name
	 * 3) 如果没有回调，再退回到按 DAI id 位置查找
	 */
	for_each_component(pos) {
		struct device_node *component_of_node = soc_component_to_node(pos);

		if (component_of_node != args->np || !pos->num_dai)
			continue;

		ret = snd_soc_component_of_xlate_dai_name(pos, args, &dlc->dai_name);
		if (ret == -ENOTSUPP) {
			struct snd_soc_dai *dai;
			int id = -1;

			switch (args->args_count) {
			case 0:
				id = 0; /* same as dai_drv[0] */
				break;
			case 1:
				id = args->args[0];
				break;
			default:
				/* 不支持。 */
				break;
			}

			if (id < 0 || id >= pos->num_dai) {
				ret = -EINVAL;
				continue;
			}

			ret = 0;

			/* 查找目标 DAI。 */
			for_each_component_dais(pos, dai) {
				if (id == 0)
					break;
				id--;
			}

			dlc->dai_name	= snd_soc_dai_name_get(dai);
		} else if (ret) {
			/*
			 * 如果返回的不是 ENOTSUPP，就继续检查同一个 node 上是否
			 * 还提供了别的 component。一个设备提供多个 component 时，
			 * 这里就可能出现这种情况。
			 */
			continue;
		}

		break;
	}

	if (ret == 0)
		dlc->of_node = args->np;

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_get_dlc);

int snd_soc_of_get_dlc(struct device_node *of_node,
		       struct of_phandle_args *args,
		       struct snd_soc_dai_link_component *dlc,
		       int index)
{
	struct of_phandle_args __args;
	int ret;

	/*
	 * 这是最底层的 DT helper：先把 sound-dai phandle 解析出来，
	 * 再交给 snd_soc_get_dlc() 转成 ALSA/ASoC 可直接使用的端点描述。
	 */
	if (!args)
		args = &__args;

	ret = of_parse_phandle_with_args(of_node, "sound-dai",
					 "#sound-dai-cells", index, args);
	if (ret)
		return ret;

	return snd_soc_get_dlc(args, dlc);
}
EXPORT_SYMBOL_GPL(snd_soc_of_get_dlc);

int snd_soc_get_dai_name(const struct of_phandle_args *args,
			 const char **dai_name)
{
	struct snd_soc_dai_link_component dlc;
	int ret = snd_soc_get_dlc(args, &dlc);

	/* 只关心名字时，就先走完整解析，再把结果里的 dai_name 取出来。 */
	if (ret == 0)
		*dai_name = dlc.dai_name;

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_get_dai_name);

int snd_soc_of_get_dai_name(struct device_node *of_node,
			    const char **dai_name, int index)
{
	struct snd_soc_dai_link_component dlc;
	int ret = snd_soc_of_get_dlc(of_node, NULL, &dlc, index);

	/* 直接复用 snd_soc_of_get_dlc() 的解析结果，只取 dai 名字。 */
	if (ret == 0)
		*dai_name = dlc.dai_name;

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_of_get_dai_name);

struct snd_soc_dai *snd_soc_get_dai_via_args(const struct of_phandle_args *dai_args)
{
	struct snd_soc_dai *dai;
	struct snd_soc_component *component;
	guard(mutex)(&client_mutex);

	/* 按 dai_args 精确匹配已注册的 DAI，适合 topology 或高级板级映射。 */
	for_each_component(component) {
		for_each_component_dais(component, dai)
			if (snd_soc_is_match_dai_args(dai->driver->dai_args, dai_args))
				return dai;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_get_dai_via_args);

static void __snd_soc_of_put_component(struct snd_soc_dai_link_component *component)
{
	/* 解析时拿到的 of_node 引用必须配对 put，避免长时间持有节点。 */
	if (component->of_node) {
		of_node_put(component->of_node);
		component->of_node = NULL;
	}
}

static int __snd_soc_of_get_dai_link_component_alloc(
	struct device *dev, struct device_node *of_node,
	struct snd_soc_dai_link_component **ret_component,
	int *ret_num)
{
	struct snd_soc_dai_link_component *component;
	int num;

	/* 统计 CPU/CODEC 的数量。 */
	num = of_count_phandle_with_args(of_node, "sound-dai", "#sound-dai-cells");
	if (num <= 0) {
		if (num == -ENOENT)
			dev_err(dev, "No 'sound-dai' property\n");
		else
			dev_err(dev, "Bad phandle in 'sound-dai'\n");
		return num;
	}
	component = devm_kcalloc(dev, num, sizeof(*component), GFP_KERNEL);
	if (!component)
		return -ENOMEM;

	*ret_component	= component;
	*ret_num	= num;

	return 0;
}

/*
 * snd_soc_of_put_dai_link_codecs - 释放 codecs 数组里的 device node 引用
 * @dai_link: DAI link
 *
 * 释放由 snd_soc_of_get_dai_link_codecs() 获取的 device node 引用。
 */
void snd_soc_of_put_dai_link_codecs(struct snd_soc_dai_link *dai_link)
{
	struct snd_soc_dai_link_component *component;
	int index;

	/* 释放 codecs 数组里每个 component 持有的 of_node 引用。 */
	for_each_link_codecs(dai_link, index, component)
		__snd_soc_of_put_component(component);
}
EXPORT_SYMBOL_GPL(snd_soc_of_put_dai_link_codecs);

/*
 * snd_soc_of_get_dai_link_codecs - 解析 devicetree 中的 CODEC 列表
 * @dev: card 设备
 * @of_node: device node
 * @dai_link: DAI link
 *
 * 从 DAI link 的 sound-dai 属性中构造 CODEC DAI component 数组。
 * 解析结果会写入 @dai_link，同时更新 DAI 数量。
 * 数组里持有的 device node 引用需要在之后调用
 * snd_soc_of_put_dai_link_codecs() 释放。
 *
 * 返回 0 表示成功。
 */
int snd_soc_of_get_dai_link_codecs(struct device *dev,
				   struct device_node *of_node,
				   struct snd_soc_dai_link *dai_link)
{
	struct snd_soc_dai_link_component *component;
	int index, ret;

	ret = __snd_soc_of_get_dai_link_component_alloc(dev, of_node,
					 &dai_link->codecs, &dai_link->num_codecs);
	if (ret < 0)
		return ret;

	/* 把 codec 列表逐个解析成可直接参与 match 的 dai_link_component。 */
	for_each_link_codecs(dai_link, index, component) {
		ret = snd_soc_of_get_dlc(of_node, NULL, component, index);
		if (ret)
			goto err;
	}
	return 0;
err:
	snd_soc_of_put_dai_link_codecs(dai_link);
	dai_link->codecs = NULL;
	dai_link->num_codecs = 0;
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_of_get_dai_link_codecs);

/*
 * snd_soc_of_put_dai_link_cpus - 释放 CPU 数组里的 device node 引用
 * @dai_link: DAI link
 *
 * 释放由 snd_soc_of_get_dai_link_cpus() 获取的 device node 引用。
 */
void snd_soc_of_put_dai_link_cpus(struct snd_soc_dai_link *dai_link)
{
	struct snd_soc_dai_link_component *component;
	int index;

	/* 释放 CPUs 数组里每个 component 持有的 of_node 引用。 */
	for_each_link_cpus(dai_link, index, component)
		__snd_soc_of_put_component(component);
}
EXPORT_SYMBOL_GPL(snd_soc_of_put_dai_link_cpus);

/*
 * snd_soc_of_get_dai_link_cpus - 解析 devicetree 中的 CPU DAI 列表
 * @dev: card 设备
 * @of_node: device node
 * @dai_link: DAI link
 *
 * 逻辑与 snd_soc_of_get_dai_link_codecs() 类似，只是解析目标换成 CPU DAI。
 *
 * 返回 0 表示成功。
 */
int snd_soc_of_get_dai_link_cpus(struct device *dev,
				 struct device_node *of_node,
				 struct snd_soc_dai_link *dai_link)
{
	struct snd_soc_dai_link_component *component;
	int index, ret;

	/* 先统计 CPU 数量，再为 CPU 数组分配存储。 */
	ret = __snd_soc_of_get_dai_link_component_alloc(dev, of_node,
					 &dai_link->cpus, &dai_link->num_cpus);
	if (ret < 0)
		return ret;

	/* CPU 列表解析逻辑与 codec 相同，只是目标数组换成 cpus。 */
	for_each_link_cpus(dai_link, index, component) {
		ret = snd_soc_of_get_dlc(of_node, NULL, component, index);
		if (ret)
			goto err;
	}
	return 0;
err:
	snd_soc_of_put_dai_link_cpus(dai_link);
	dai_link->cpus = NULL;
	dai_link->num_cpus = 0;
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_of_get_dai_link_cpus);

static int __init snd_soc_init(void)
{
	int ret;

	snd_soc_debugfs_init();
	ret = snd_soc_util_init();
	if (ret)
		goto err_util_init;

	ret = platform_driver_register(&soc_driver);
	if (ret)
		goto err_register;
	return 0;

err_register:
	snd_soc_util_exit();
err_util_init:
	snd_soc_debugfs_exit();
	return ret;
}
module_init(snd_soc_init);

static void __exit snd_soc_exit(void)
{
	snd_soc_util_exit();
	snd_soc_debugfs_exit();

	platform_driver_unregister(&soc_driver);
}
module_exit(snd_soc_exit);

/* 模块信息。 */
MODULE_AUTHOR("Liam Girdwood, lrg@slimlogic.co.uk");
MODULE_DESCRIPTION("ALSA SoC Core");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:soc-audio");
