// SPDX-License-Identifier: GPL-2.0+
//
// soc-dapm.c  --  ALSA SoC Dynamic Audio Power Management
//
// Copyright 2005 Wolfson Microelectronics PLC.
// Author: Liam Girdwood <lrg@slimlogic.co.uk>
//
//  Features:
//    o Changes power status of internal codec blocks depending on the
//      dynamic configuration of codec internal audio paths and active
//      DACs/ADCs.
//    o Platform power domain - can support external components i.e. amps and
//      mic/headphone insertion events.
//    o Automatic Mic Bias support
//    o Jack insertion power event initiation - e.g. hp insertion will enable
//      sinks, dacs, etc
//    o Delayed power down of audio subsystem to reduce pops between a quick
//      device reopen.

#include <linux/module.h>
#include <linux/init.h>
#include <linux/async.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/bitops.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/debugfs.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>

/*
 * soc-dapm.c 负责 DAPM 的运行时状态机：
 * 路径遍历、widget 上下电、事件回调、bias 级别推进以及调试接口都在这里。
 */
#include <trace/events/asoc.h>

/* DAPM context */
struct snd_soc_dapm_context {
	/* 当前 bias 状态：OFF / STANDBY / PREPARE / ON。 */
	enum snd_soc_bias_level bias_level;

	/* 是否在空闲时保持偏置，不直接掉到 OFF。 */
	bool idle_bias;				/* Use BIAS_OFF instead of STANDBY when false */

	/* 这个 DAPM context 挂在哪个 component / card 下面。 */
	struct snd_soc_component *component;	/* parent component */
	struct snd_soc_card *card;		/* parent card */

	/* DAPM 更新过程中，用来保存目标 bias 和临时遍历状态。 */
	enum snd_soc_bias_level target_bias_level;
	struct list_head list;

	/* 路径搜索缓存，减少重复遍历。 */
	struct snd_soc_dapm_widget *wcache_sink;
	struct snd_soc_dapm_widget *wcache_source;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_dapm;
#endif
};

#define DAPM_UPDATE_STAT(widget, val) widget->dapm->card->dapm_stats.val++;

#define DAPM_DIR_REVERSE(x) ((x == SND_SOC_DAPM_DIR_IN) ? \
	SND_SOC_DAPM_DIR_OUT : SND_SOC_DAPM_DIR_IN)

#define dapm_for_each_direction(dir) \
	for ((dir) = SND_SOC_DAPM_DIR_IN; (dir) <= SND_SOC_DAPM_DIR_OUT; \
		(dir)++)

/* DAPM 上下电顺序列表，未来可以按 codec 维度拆分。 */
static int dapm_up_seq[] = {
	/* 上电顺序：越靠前越早开。 */
	[snd_soc_dapm_pre] = 1,
	[snd_soc_dapm_regulator_supply] = 2,
	[snd_soc_dapm_pinctrl] = 2,
	[snd_soc_dapm_clock_supply] = 2,
	[snd_soc_dapm_supply] = 3,
	[snd_soc_dapm_dai_link] = 3,
	[snd_soc_dapm_micbias] = 4,
	[snd_soc_dapm_vmid] = 4,
	[snd_soc_dapm_dai_in] = 5,
	[snd_soc_dapm_dai_out] = 5,
	[snd_soc_dapm_aif_in] = 5,
	[snd_soc_dapm_aif_out] = 5,
	[snd_soc_dapm_mic] = 6,
	[snd_soc_dapm_siggen] = 6,
	[snd_soc_dapm_input] = 6,
	[snd_soc_dapm_output] = 6,
	[snd_soc_dapm_mux] = 7,
	[snd_soc_dapm_mux_named_ctl] = 7,
	[snd_soc_dapm_demux] = 7,
	[snd_soc_dapm_dac] = 8,
	[snd_soc_dapm_switch] = 9,
	[snd_soc_dapm_mixer] = 9,
	[snd_soc_dapm_mixer_named_ctl] = 9,
	[snd_soc_dapm_pga] = 10,
	[snd_soc_dapm_buffer] = 10,
	[snd_soc_dapm_scheduler] = 10,
	[snd_soc_dapm_effect] = 10,
	[snd_soc_dapm_src] = 10,
	[snd_soc_dapm_asrc] = 10,
	[snd_soc_dapm_encoder] = 10,
	[snd_soc_dapm_decoder] = 10,
	[snd_soc_dapm_adc] = 11,
	[snd_soc_dapm_out_drv] = 12,
	[snd_soc_dapm_hp] = 12,
	[snd_soc_dapm_line] = 12,
	[snd_soc_dapm_sink] = 12,
	[snd_soc_dapm_spk] = 13,
	[snd_soc_dapm_kcontrol] = 14,
	[snd_soc_dapm_post] = 15,
};

static int dapm_down_seq[] = {
	/* 下电顺序：越靠前越晚关，整体与上电大体相反。 */
	[snd_soc_dapm_pre] = 1,
	[snd_soc_dapm_kcontrol] = 2,
	[snd_soc_dapm_adc] = 3,
	[snd_soc_dapm_spk] = 4,
	[snd_soc_dapm_hp] = 5,
	[snd_soc_dapm_line] = 5,
	[snd_soc_dapm_out_drv] = 5,
	[snd_soc_dapm_sink] = 6,
	[snd_soc_dapm_pga] = 6,
	[snd_soc_dapm_buffer] = 6,
	[snd_soc_dapm_scheduler] = 6,
	[snd_soc_dapm_effect] = 6,
	[snd_soc_dapm_src] = 6,
	[snd_soc_dapm_asrc] = 6,
	[snd_soc_dapm_encoder] = 6,
	[snd_soc_dapm_decoder] = 6,
	[snd_soc_dapm_switch] = 7,
	[snd_soc_dapm_mixer_named_ctl] = 7,
	[snd_soc_dapm_mixer] = 7,
	[snd_soc_dapm_dac] = 8,
	[snd_soc_dapm_mic] = 9,
	[snd_soc_dapm_siggen] = 9,
	[snd_soc_dapm_input] = 9,
	[snd_soc_dapm_output] = 9,
	[snd_soc_dapm_micbias] = 10,
	[snd_soc_dapm_vmid] = 10,
	[snd_soc_dapm_mux] = 11,
	[snd_soc_dapm_mux_named_ctl] = 11,
	[snd_soc_dapm_demux] = 11,
	[snd_soc_dapm_aif_in] = 12,
	[snd_soc_dapm_aif_out] = 12,
	[snd_soc_dapm_dai_in] = 12,
	[snd_soc_dapm_dai_out] = 12,
	[snd_soc_dapm_dai_link] = 13,
	[snd_soc_dapm_supply] = 14,
	[snd_soc_dapm_clock_supply] = 15,
	[snd_soc_dapm_pinctrl] = 15,
	[snd_soc_dapm_regulator_supply] = 15,
	[snd_soc_dapm_post] = 16,
};

static void dapm_assert_locked(struct snd_soc_dapm_context *dapm)
{
	/* card 已经 instantiated 后，DAPM 操作必须持有专用锁。 */
	if (snd_soc_card_is_instantiated(dapm->card))
		snd_soc_dapm_mutex_assert_held(dapm);
}

static void dapm_pop_wait(u32 pop_time)
{
	/* pop/click 抑制通过延迟一小段时间给硬件缓冲。 */
	if (pop_time)
		schedule_timeout_uninterruptible(msecs_to_jiffies(pop_time));
}

__printf(3, 4)
static void dapm_pop_dbg(struct device *dev, u32 pop_time, const char *fmt, ...)
{
	va_list args;
	char *buf;

	/* 只有启用 pop 延迟时才值得拼接调试信息。 */
	if (!pop_time)
		return;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (buf == NULL)
		return;

	va_start(args, fmt);
	vsnprintf(buf, PAGE_SIZE, fmt, args);
	dev_info(dev, "%s", buf);
	va_end(args);

	kfree(buf);
}

struct snd_soc_dapm_context *snd_soc_dapm_alloc(struct device *dev)
{
	/* DAPM context 统一用 devres 分配，跟随 device 生命周期释放。 */
	return devm_kzalloc(dev, sizeof(struct snd_soc_dapm_context), GFP_KERNEL);
}

struct device *snd_soc_dapm_to_dev(struct snd_soc_dapm_context *dapm)
{
	if (dapm->component)
		return dapm->component->dev;

	return dapm->card->dev;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_to_dev);

struct snd_soc_card *snd_soc_dapm_to_card(struct snd_soc_dapm_context *dapm)
{
	return dapm->card;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_to_card);

struct snd_soc_component *snd_soc_dapm_to_component(struct snd_soc_dapm_context *dapm)
{
	return dapm->component;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_to_component);

static bool dapm_dirty_widget(struct snd_soc_dapm_widget *w)
{
	/* dirty 列表非空表示该 widget 需要重新计算连接关系或状态。 */
	return !list_empty(&w->dirty);
}

static void dapm_mark_dirty(struct snd_soc_dapm_widget *w, const char *reason)
{
	struct device *dev = snd_soc_dapm_to_dev(w->dapm);

	/* dirty widget 会在后续 DAPM 遍历里被重新收敛。 */
	dapm_assert_locked(w->dapm);

	if (!dapm_dirty_widget(w)) {
		dev_vdbg(dev, "Marking %s dirty due to %s\n",
			 w->name, reason);
		list_add_tail(&w->dirty, &w->dapm->card->dapm_dirty);
	}
}

/*
 * dapm_widget_invalidate_input_paths() 和
 * dapm_widget_invalidate_output_paths() 的公共实现。
 * 之所以做成内联，是因为两个特化函数合起来的体积只比通用版本略大，
 * 但特化版本的快路径明显更短，整体更划算。
 */
static __always_inline void dapm_widget_invalidate_paths(
	struct snd_soc_dapm_widget *w, enum snd_soc_dapm_direction dir)
{
	enum snd_soc_dapm_direction rdir = DAPM_DIR_REVERSE(dir);
	struct snd_soc_dapm_widget *node;
	struct snd_soc_dapm_path *p;
	LIST_HEAD(list);

	/* 递归失效缓存路径计数，直到所有可达节点都被重标记。 */
	dapm_assert_locked(w->dapm);

	if (w->endpoints[dir] == -1)
		return;

	list_add_tail(&w->work_list, &list);
	w->endpoints[dir] = -1;

	list_for_each_entry(w, &list, work_list) {
		snd_soc_dapm_widget_for_each_path(w, dir, p) {
			if (p->is_supply || !p->connect)
				continue;
			node = p->node[rdir];
			if (node->endpoints[dir] != -1) {
				node->endpoints[dir] = -1;
				list_add_tail(&node->work_list, &list);
			}
		}
	}
}

/*
 * dapm_widget_invalidate_input_paths() - 失效缓存的输入路径数量
 * @w: 需要失效缓存输入路径数量的 widget
 *
 * 该函数会把指定 widget 以及从它沿输出路径可达的所有 widget 的输入
 * 缓存重新置空。
 *
 * 当某个 widget 的输出路径数量可能变化时必须调用它，例如 widget 的
 * source 状态变化，或者新增/激活了一条以它为 sink 的路径。
 */
static void dapm_widget_invalidate_input_paths(struct snd_soc_dapm_widget *w)
{
	/* 某个 widget 的输出发生变化时，需要失效它的输入缓存。 */
	dapm_widget_invalidate_paths(w, SND_SOC_DAPM_DIR_IN);
}

/*
 * dapm_widget_invalidate_output_paths() - 失效缓存的输出路径数量
 * @w: 需要失效缓存输出路径数量的 widget
 *
 * 该函数会把指定 widget 以及从它沿输入路径可达的所有 widget 的输出
 * 缓存重新置空。
 *
 * 当某个 widget 的输出路径数量可能变化时必须调用它，例如 widget 的
 * sink 状态变化，或者新增/激活了一条以它为 source 的路径。
 */
static void dapm_widget_invalidate_output_paths(struct snd_soc_dapm_widget *w)
{
	/* 某个 widget 的输入发生变化时，需要失效它的输出缓存。 */
	dapm_widget_invalidate_paths(w, SND_SOC_DAPM_DIR_OUT);
}

/*
 * dapm_path_invalidate() - 失效路径两端 widget 的输入/输出缓存
 * @p: 需要失效的路径
 *
 * 该函数会重置路径 sink 侧的输入缓存，以及路径 source 侧的输出缓存。
 *
 * 当一条路径被新增、删除，或者 connect 状态变化时，必须调用它。
 */
static void dapm_path_invalidate(struct snd_soc_dapm_path *p)
{
	/*
	 * 弱路径或供电路径不会影响邻居节点的输入/输出路径数量。
	 */
	/* path 变化时，只影响真正与之相连的上下游节点。 */
	if (p->is_supply)
		return;

	/*
	 * 已连接 endpoint 的数量等于所有邻居节点已连接 endpoint 数量之和。
	 * 如果某个节点本来就没有已连接 endpoint，那么它连上或断开都不会
	 * 改变这个总和，因此不需要重新检查这条路径。
	 */
	if (p->source->endpoints[SND_SOC_DAPM_DIR_IN] != 0)
		dapm_widget_invalidate_input_paths(p->sink);
	if (p->sink->endpoints[SND_SOC_DAPM_DIR_OUT] != 0)
		dapm_widget_invalidate_output_paths(p->source);
}

void snd_soc_dapm_mark_endpoints_dirty(struct snd_soc_card *card)
{
	struct snd_soc_dapm_widget *w;

	/* 卡片重新路由后，把所有 endpoint 标记为 dirty 重新计算。 */
	snd_soc_dapm_mutex_lock_root(card);

	for_each_card_widgets(card, w) {
		if (w->is_ep) {
			dapm_mark_dirty(w, "Rechecking endpoints");
			if (w->is_ep & SND_SOC_DAPM_EP_SINK)
				dapm_widget_invalidate_output_paths(w);
			if (w->is_ep & SND_SOC_DAPM_EP_SOURCE)
				dapm_widget_invalidate_input_paths(w);
		}
	}

	snd_soc_dapm_mutex_unlock(card);
}

/* 创建一个新的 DAPM widget */
static inline struct snd_soc_dapm_widget *dapm_cnew_widget(
	const struct snd_soc_dapm_widget *_widget,
	const char *prefix)
{
	/* 创建 widget 副本时也会复制名称，并按 prefix 重新命名。 */
	struct snd_soc_dapm_widget *w __free(kfree) = kmemdup(_widget,
							      sizeof(*_widget),
							      GFP_KERNEL);
	if (!w)
		return NULL;

	if (prefix)
		w->name = kasprintf(GFP_KERNEL, "%s %s", prefix, _widget->name);
	else
		w->name = kstrdup_const(_widget->name, GFP_KERNEL);
	if (!w->name)
		return NULL;

	if (_widget->sname) {
		w->sname = kstrdup_const(_widget->sname, GFP_KERNEL);
		if (!w->sname) {
			kfree_const(w->name);
			return NULL;
		}
	}

	return_ptr(w);
}

struct dapm_kcontrol_data {
	unsigned int value;
	struct snd_soc_dapm_widget *widget;
	struct list_head paths;
	struct snd_soc_dapm_widget_list *wlist;
};

static unsigned int dapm_read(struct snd_soc_dapm_context *dapm, int reg)
{
	/* DAPM 需要在当前 component 上读取寄存器状态。 */
	if (!dapm->component)
		return -EIO;
	return  snd_soc_component_read(dapm->component, reg);
}

/* 初始化 codec path 状态 */
static void dapm_set_mixer_path_status(struct snd_soc_dapm_path *p, int i,
				       int nth_path)
{
	/* 根据当前寄存器值判断 mixer path 是否应当默认连通。 */
	struct soc_mixer_control *mc = (struct soc_mixer_control *)
		p->sink->kcontrol_news[i].private_value;
	unsigned int reg = mc->reg;
	unsigned int invert = mc->invert;

	if (reg != SND_SOC_NOPM) {
		unsigned int shift = mc->shift;
		unsigned int max = mc->max;
		unsigned int mask = (1 << fls(max)) - 1;
		unsigned int val = dapm_read(p->sink->dapm, reg);

		/*
		 * nth_path 参数用来告诉这个函数：当前是在为 kcontrol 的哪一条
		 * path 计算初始状态。理想情况下它应该支持任意数量的 path 和
		 * channel，但实际 kcontrol 只分单声道和立体声两类，因此这里只能
		 * 处理 2 个 channel。
		 *
		 * 下面的代码假定：对于立体声 control，第一条 path 对应左声道，
		 * 其余 path 都对应右声道。
		 */
		if (snd_soc_volsw_is_stereo(mc) && nth_path > 0) {
			if (reg != mc->rreg)
				val = dapm_read(p->sink->dapm, mc->rreg);
			val = (val >> mc->rshift) & mask;
		} else {
			val = (val >> shift) & mask;
		}
		if (invert)
			val = max - val;
		p->connect = !!val;
	} else {
		/*
		 * 虚拟 mixer 没有底层寄存器可用来判断该连哪条 path，因此这里只能
		 * 直接沿用初始状态。这样可以保证初始化时默认选中的 mixer 分支会
		 * 被正确上电。
		 */
		p->connect = invert;
	}
}

/* 将 mux widget 连接到它所对应的音频路径 */
static int dapm_connect_mux(struct snd_soc_dapm_context *dapm,
			    struct snd_soc_dapm_path *path, const char *control_name,
			    struct snd_soc_dapm_widget *w)
{
	/* mux/demux 通过枚举文本匹配当前选择项。 */
	const struct snd_kcontrol_new *kcontrol = &w->kcontrol_news[0];
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int item;
	int i;

	if (e->reg != SND_SOC_NOPM) {
		unsigned int val;

		val = dapm_read(dapm, e->reg);
		val = (val >> e->shift_l) & e->mask;
		item = snd_soc_enum_val_to_item(e, val);
	} else {
		/*
		 * 虚拟 mux 同样没有底层寄存器来决定应该连哪条 path，所以这里
		 * 直接匹配第一个枚举项。这样初始化时默认 mux 选项（第一个）
		 * 能正确上电。
		 */
		item = 0;
	}

	/* 根据 control_name 找到枚举项，匹配的项才会被视为连通。 */
	i = match_string(e->texts, e->items, control_name);
	if (i < 0)
		return -ENODEV;

	path->name = e->texts[i];
	path->connect = (i == item);
	return 0;

}

/* 将 mixer widget 连接到它所对应的音频路径 */
static int dapm_connect_mixer(struct snd_soc_dapm_context *dapm,
			      struct snd_soc_dapm_path *path, const char *control_name)
{
	/* mixer 通过同名 kcontrol 找到路径并判断默认是否连通。 */
	int i, nth_path = 0;

	/* 查找对应的 mixer kcontrol。 */
	for (i = 0; i < path->sink->num_kcontrols; i++) {
		if (!strcmp(control_name, path->sink->kcontrol_news[i].name)) {
			path->name = path->sink->kcontrol_news[i].name;
			dapm_set_mixer_path_status(path, i, nth_path++);
			return 0;
		}
	}
	return -ENODEV;
}

/*
 * dapm_update_widget_flags() - 重新计算 widget 的 sink/source 标志
 * @w: 需要更新标志的 widget
 *
 * 某些 widget 的类别是动态的，会随着相邻节点的连接关系变化而变化。
 * 这个函数就是用来更新这类 widget 的分类。
 *
 * 当 widget 相关路径被新增或删除时，必须调用它。
 */
static void dapm_update_widget_flags(struct snd_soc_dapm_widget *w)
{
	/* widget 的 sink/source 属性会随着相邻路径变化而动态修正。 */
	enum snd_soc_dapm_direction dir;
	struct snd_soc_dapm_path *p;
	unsigned int ep;

	switch (w->id) {
	case snd_soc_dapm_input:
		/* 在 fully_routed 的卡上，input 不会再被视为 source。 */
		if (w->dapm->card->fully_routed)
			return;
		ep = SND_SOC_DAPM_EP_SOURCE;
		snd_soc_dapm_widget_for_each_source_path(w, p) {
			if (p->source->id == snd_soc_dapm_micbias ||
			    p->source->id == snd_soc_dapm_mic ||
			    p->source->id == snd_soc_dapm_line ||
			    p->source->id == snd_soc_dapm_output) {
				ep = 0;
				break;
			}
		}
		break;
	case snd_soc_dapm_output:
		/* 在 fully_routed 的卡上，output 不会再被视为 sink。 */
		if (w->dapm->card->fully_routed)
			return;
		ep = SND_SOC_DAPM_EP_SINK;
		snd_soc_dapm_widget_for_each_sink_path(w, p) {
			if (p->sink->id == snd_soc_dapm_spk ||
			    p->sink->id == snd_soc_dapm_hp ||
			    p->sink->id == snd_soc_dapm_line ||
			    p->sink->id == snd_soc_dapm_input) {
				ep = 0;
				break;
			}
		}
		break;
	case snd_soc_dapm_line:
		ep = 0;
		dapm_for_each_direction(dir) {
			if (!list_empty(&w->edges[dir]))
				ep |= SND_SOC_DAPM_DIR_TO_EP(dir);
		}
		break;
	default:
		return;
	}

	w->is_ep = ep;
}

static int dapm_check_dynamic_path(
	struct snd_soc_dapm_context *dapm,
	struct snd_soc_dapm_widget *source, struct snd_soc_dapm_widget *sink,
	const char *control)
{
	/* 只允许 mux/switch/mixer 类动态节点参与受控路径。 */
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	bool dynamic_source = false;
	bool dynamic_sink = false;

	if (!control)
		return 0;

	switch (source->id) {
	case snd_soc_dapm_demux:
		dynamic_source = true;
		break;
	default:
		break;
	}

	switch (sink->id) {
	case snd_soc_dapm_mux:
	case snd_soc_dapm_mux_named_ctl:
	case snd_soc_dapm_switch:
	case snd_soc_dapm_mixer:
	case snd_soc_dapm_mixer_named_ctl:
		dynamic_sink = true;
		break;
	default:
		break;
	}

	if (dynamic_source && dynamic_sink) {
		dev_err(dev,
			"Direct connection between demux and mixer/mux not supported for path %s -> [%s] -> %s\n",
			source->name, control, sink->name);
		return -EINVAL;
	} else if (!dynamic_source && !dynamic_sink) {
		dev_err(dev,
			"Control not supported for path %s -> [%s] -> %s\n",
			source->name, control, sink->name);
		return -EINVAL;
	}

	return 0;
}

static int dapm_add_path(
	struct snd_soc_dapm_context *dapm,
	struct snd_soc_dapm_widget *wsource, struct snd_soc_dapm_widget *wsink,
	const char *control,
	int (*connected)(struct snd_soc_dapm_widget *source,
			 struct snd_soc_dapm_widget *sink))
{
	/* 新路径加入时，需要校验供电边界、动态控制类型和默认连通状态。 */
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	enum snd_soc_dapm_direction dir;
	struct snd_soc_dapm_path *path;
	int ret;

	if (wsink->is_supply && !wsource->is_supply) {
		dev_err(dev,
			"Connecting non-supply widget to supply widget is not supported (%s -> %s)\n",
			wsource->name, wsink->name);
		return -EINVAL;
	}

	if (connected && !wsource->is_supply) {
		dev_err(dev,
			"connected() callback only supported for supply widgets (%s -> %s)\n",
			wsource->name, wsink->name);
		return -EINVAL;
	}

	if (wsource->is_supply && control) {
		dev_err(dev,
			"Conditional paths are not supported for supply widgets (%s -> [%s] -> %s)\n",
			wsource->name, control, wsink->name);
		return -EINVAL;
	}

	ret = dapm_check_dynamic_path(dapm, wsource, wsink, control);
	if (ret)
		return ret;

	path = kzalloc_obj(struct snd_soc_dapm_path);
	if (!path)
		return -ENOMEM;

	path->node[SND_SOC_DAPM_DIR_IN] = wsource;
	path->node[SND_SOC_DAPM_DIR_OUT] = wsink;

	path->connected = connected;
	INIT_LIST_HEAD(&path->list);
	INIT_LIST_HEAD(&path->list_kcontrol);

	if (wsource->is_supply || wsink->is_supply)
		path->is_supply = 1;

/* 连接静态路径 */
	if (control == NULL) {
		path->connect = 1;
	} else {
		switch (wsource->id) {
		case snd_soc_dapm_demux:
			ret = dapm_connect_mux(dapm, path, control, wsource);
			if (ret)
				goto err;
			break;
		default:
			break;
		}

		switch (wsink->id) {
		case snd_soc_dapm_mux:
		case snd_soc_dapm_mux_named_ctl:
			ret = dapm_connect_mux(dapm, path, control, wsink);
			if (ret != 0)
				goto err;
			break;
		case snd_soc_dapm_switch:
		case snd_soc_dapm_mixer:
		case snd_soc_dapm_mixer_named_ctl:
			ret = dapm_connect_mixer(dapm, path, control);
			if (ret != 0)
				goto err;
			break;
		default:
			break;
		}
	}

	list_add(&path->list, &dapm->card->paths);

	/* 双向挂到 source/sink 的边表中，后续遍历依赖这个结构。 */
	dapm_for_each_direction(dir)
		list_add(&path->list_node[dir], &path->node[dir]->edges[dir]);

	/* 新增路径会让两端 widget 重新进入 dirty 状态。 */
	dapm_for_each_direction(dir) {
		dapm_update_widget_flags(path->node[dir]);
		dapm_mark_dirty(path->node[dir], "Route added");
	}

	if (snd_soc_card_is_instantiated(dapm->card) && path->connect)
		dapm_path_invalidate(path);

	return 0;
err:
	kfree(path);
	return ret;
}

static int dapm_kcontrol_data_alloc(struct snd_soc_dapm_widget *widget,
	struct snd_kcontrol *kcontrol, const char *ctrl_name)
{
	/* kcontrol 绑定到 DAPM widget 后，会生成一份专用的路径数据。 */
	struct device *dev = snd_soc_dapm_to_dev(widget->dapm);
	struct dapm_kcontrol_data *data;
	struct soc_mixer_control *mc;
	struct soc_enum *e;
	const char *name;
	int ret;

	data = kzalloc_obj(*data);
	if (!data)
		return -ENOMEM;

	INIT_LIST_HEAD(&data->paths);

	switch (widget->id) {
	case snd_soc_dapm_switch:
	case snd_soc_dapm_mixer:
	case snd_soc_dapm_mixer_named_ctl:
		mc = (struct soc_mixer_control *)kcontrol->private_value;

		if (mc->autodisable) {
			/*
			 * autodisable 的意思是：当这个 control 被关闭时，
			 * 不只是关掉寄存器位，还要把对应的 DAPM widget 一并
			 * 映射成一个“自动关断”节点，方便 DAPM 图计算电源。
			 */
			struct snd_soc_dapm_widget template;

			if (snd_soc_volsw_is_stereo(mc))
				dev_warn(dev,
					 "ASoC: Unsupported stereo autodisable control '%s'\n",
					 ctrl_name);

			name = kasprintf(GFP_KERNEL, "%s %s", ctrl_name,
					 "Autodisable");
			if (!name) {
				ret = -ENOMEM;
				goto err_data;
			}

			memset(&template, 0, sizeof(template));
			template.reg = mc->reg;
			template.mask = (1 << fls(mc->max)) - 1;
			template.shift = mc->shift;
			if (mc->invert)
				template.off_val = mc->max;
			else
				template.off_val = 0;
			template.on_val = template.off_val;
			template.id = snd_soc_dapm_kcontrol;
			template.name = name;

			data->value = template.on_val;

			data->widget =
				snd_soc_dapm_new_control_unlocked(widget->dapm,
				&template);
			kfree(name);
			if (IS_ERR(data->widget)) {
				ret = PTR_ERR(data->widget);
				goto err_data;
			}
		}
		break;
	case snd_soc_dapm_demux:
	case snd_soc_dapm_mux:
	case snd_soc_dapm_mux_named_ctl:
		e = (struct soc_enum *)kcontrol->private_value;

		if (e->autodisable) {
			/*
			 * mux/demux 场景下，autodisable 也会生成一个独立的
			 * DAPM kcontrol 节点，用来把枚举值和供电状态绑定起来。
			 */
			struct snd_soc_dapm_widget template;

			name = kasprintf(GFP_KERNEL, "%s %s", ctrl_name,
					 "Autodisable");
			if (!name) {
				ret = -ENOMEM;
				goto err_data;
			}

			memset(&template, 0, sizeof(template));
			template.reg = e->reg;
			template.mask = e->mask;
			template.shift = e->shift_l;
			template.off_val = snd_soc_enum_item_to_val(e, 0);
			template.on_val = template.off_val;
			template.id = snd_soc_dapm_kcontrol;
			template.name = name;

			data->value = template.on_val;

			data->widget = snd_soc_dapm_new_control_unlocked(
						widget->dapm, &template);
			kfree(name);
			if (IS_ERR(data->widget)) {
				ret = PTR_ERR(data->widget);
				goto err_data;
			}

			dapm_add_path(widget->dapm, data->widget,
				      widget, NULL, NULL);
		} else if (e->reg != SND_SOC_NOPM) {
			data->value = dapm_read(widget->dapm, e->reg) &
				      (e->mask << e->shift_l);
		}
		break;
	default:
		break;
	}

	kcontrol->private_data = data;

	return 0;

err_data:
	kfree(data);
	return ret;
}

static void dapm_kcontrol_free(struct snd_kcontrol *kctl)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kctl);

	/* 释放 kcontrol 时，要先把它挂着的 path 链表摘掉，再回收辅助 list。 */
	list_del(&data->paths);
	kfree(data->wlist);
	kfree(data);
}

static struct snd_soc_dapm_widget_list *dapm_kcontrol_get_wlist(
	const struct snd_kcontrol *kcontrol)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kcontrol);

	return data->wlist;
}

static int dapm_kcontrol_add_widget(struct snd_kcontrol *kcontrol,
	struct snd_soc_dapm_widget *widget)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_widget_list *new_wlist;
	unsigned int n;

	/*
	 * 一个 DAPM kcontrol 可能由多个 widget 共享，所以这里维护的是
	 * 一个可扩展的 widget 列表，而不是单点指针。
	 */
	if (data->wlist)
		n = data->wlist->num_widgets + 1;
	else
		n = 1;

	new_wlist = krealloc(data->wlist,
			     struct_size(new_wlist, widgets, n),
			     GFP_KERNEL);
	if (!new_wlist)
		return -ENOMEM;

	new_wlist->num_widgets = n;
	new_wlist->widgets[n - 1] = widget;

	data->wlist = new_wlist;

	return 0;
}

static void dapm_kcontrol_add_path(const struct snd_kcontrol *kcontrol,
	struct snd_soc_dapm_path *path)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kcontrol);

	/* 该 kcontrol 直接控制到的所有 DAPM path 统一挂到路径链表里。 */
	list_add_tail(&path->list_kcontrol, &data->paths);
}

static bool dapm_kcontrol_is_powered(const struct snd_kcontrol *kcontrol)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kcontrol);

	/*
	 * 某些 kcontrol 本身会被映射到一个辅助 widget；如果还没有
	 * 创建出这个 widget，就默认认为它是“可访问”的。
	 */
	if (!data->widget)
		return true;

	return data->widget->power;
}

static struct list_head *dapm_kcontrol_get_path_list(
	const struct snd_kcontrol *kcontrol)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kcontrol);

	return &data->paths;
}

#define dapm_kcontrol_for_each_path(path, kcontrol) \
	list_for_each_entry(path, dapm_kcontrol_get_path_list(kcontrol), \
		list_kcontrol)

unsigned int snd_soc_dapm_kcontrol_get_value(const struct snd_kcontrol *kcontrol)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kcontrol);

	return data->value;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_kcontrol_get_value);

static bool dapm_kcontrol_set_value(const struct snd_kcontrol *kcontrol,
	unsigned int value)
{
	struct dapm_kcontrol_data *data = snd_kcontrol_chip(kcontrol);

	/* 值没变就不需要继续做状态传播，避免重复触发 DAPM 更新。 */
	if (data->value == value)
		return false;

	if (data->widget) {
		switch (dapm_kcontrol_get_wlist(kcontrol)->widgets[0]->id) {
		case snd_soc_dapm_switch:
		case snd_soc_dapm_mixer:
		case snd_soc_dapm_mixer_named_ctl:
			data->widget->on_val = value & data->widget->mask;
			break;
		case snd_soc_dapm_demux:
		case snd_soc_dapm_mux:
		case snd_soc_dapm_mux_named_ctl:
			data->widget->on_val = value >> data->widget->shift;
			break;
		default:
			data->widget->on_val = value;
			break;
		}
	}

	data->value = value;

	return true;
}

/**
 * snd_soc_dapm_kcontrol_to_widget() - 取得与 kcontrol 关联的 widget
 * @kcontrol: kcontrol
 */
struct snd_soc_dapm_widget *snd_soc_dapm_kcontrol_to_widget(struct snd_kcontrol *kcontrol)
{
	/* 取出这个 DAPM kcontrol 绑定的代表性 widget。 */
	return dapm_kcontrol_get_wlist(kcontrol)->widgets[0];
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_kcontrol_to_widget);

/**
 * snd_soc_dapm_kcontrol_to_dapm() - 取得与 kcontrol 关联的 DAPM 上下文
 * @kcontrol: kcontrol
 *
 * 注意：该函数只能用于已经确认是为 CODEC 注册的 kcontrol，
 * 否则行为未定义。
 */
struct snd_soc_dapm_context *snd_soc_dapm_kcontrol_to_dapm(struct snd_kcontrol *kcontrol)
{
	/* 通过 kcontrol 反查它属于哪个 DAPM 上下文。 */
	return dapm_kcontrol_get_wlist(kcontrol)->widgets[0]->dapm;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_kcontrol_to_dapm);

/**
 * snd_soc_dapm_kcontrol_to_component() - 取得与 kcontrol 关联的 component
 * @kcontrol: kcontrol
 *
 * 该函数只能用于已经确认属于某个 component 的 DAPM 上下文，
 * 否则行为未定义。
 */
struct snd_soc_component *snd_soc_dapm_kcontrol_to_component(struct snd_kcontrol *kcontrol)
{
	/* DAPM context 再往上就是 component，本函数提供反向桥接。 */
	return snd_soc_dapm_to_component(snd_soc_dapm_kcontrol_to_dapm(kcontrol));
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_kcontrol_to_component);

static void dapm_reset(struct snd_soc_card *card)
{
	struct snd_soc_dapm_widget *w;

	snd_soc_dapm_mutex_assert_held(card);

	/* 先清空统计和中间态，再从当前 power 值重新初始化遍历缓存。 */
	memset(&card->dapm_stats, 0, sizeof(card->dapm_stats));

	for_each_card_widgets(card, w) {
		w->new_power = w->power;
		w->power_checked = false;
	}
}

static const char *dapm_prefix(struct snd_soc_dapm_context *dapm)
{
	/* DAPM widget / kcontrol 的前缀通常来自 component 的 name_prefix。 */
	if (!dapm->component)
		return NULL;
	return dapm->component->name_prefix;
}

static int dapm_update_bits(struct snd_soc_dapm_context *dapm,
	int reg, unsigned int mask, unsigned int value)
{
	/* DAPM 层的寄存器写回最终还是代理到 component 的 regmap 接口。 */
	if (!dapm->component)
		return -EIO;
	return snd_soc_component_update_bits(dapm->component, reg,
					     mask, value);
}

static int dapm_test_bits(struct snd_soc_dapm_context *dapm,
	int reg, unsigned int mask, unsigned int value)
{
	/* 仅做寄存器值比较，不触发实际写入。用于判断状态是否会改变。 */
	if (!dapm->component)
		return -EIO;
	return snd_soc_component_test_bits(dapm->component, reg, mask, value);
}

static void dapm_async_complete(struct snd_soc_dapm_context *dapm)
{
	/* 让 component 把延迟的异步访问补完，避免 DAPM 与底层写寄存器打架。 */
	if (dapm->component)
		snd_soc_component_async_complete(dapm->component);
}

static struct snd_soc_dapm_widget *
dapm_wcache_lookup(struct snd_soc_dapm_widget *w, const char *name)
{
	if (w) {
		struct list_head *wlist = &w->dapm->card->widgets;
		const int depth = 2;
		int i = 0;

		/* 从当前 widget 附近做一次短距离查找，避免全卡遍历。 */
		list_for_each_entry_from(w, wlist, list) {
			if (!strcmp(name, w->name))
				return w;

			if (++i == depth)
				break;
		}
	}

	return NULL;
}

/**
 * snd_soc_dapm_force_bias_level() - 强制设置 DAPM bias level
 * @dapm: 需要设置的 DAPM 上下文
 * @level: 目标 level
 *
 * 强制把 DAPM bias level 设为指定状态。即使当前已经处于同一状态，
 * 也会调用 bias level 回调。该过程不会经过正常的 bias level 序列，
 * 因此不会进入当前状态与目标状态之间的中间状态。
 *
 * 注意，这种修改只是临时的。下次调用 snd_soc_dapm_sync() 时，状态会
 * 再次回到 DAPM core 计算出的 level。这个函数通常用于 probe 或
 * suspend/resume 期间先把设备拉起来，方便完成初始化。
 */
int snd_soc_dapm_force_bias_level(struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	int ret = 0;

	/*
	 * 强制切到目标 bias level，不走完整的中间态序列。
	 * 这是 probe / resume 这类场景下用来快速把设备唤醒或对齐状态的。
	 */
	if (dapm->component)
		ret = snd_soc_component_set_bias_level(dapm->component, level);

	if (ret == 0)
		dapm->bias_level = level;

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_force_bias_level);

/**
 * snd_soc_dapm_init_bias_level() - 初始化 DAPM bias level
 * @dapm: 需要初始化的 DAPM 上下文
 * @level: 目标 DAPM level
 *
 * 该函数只会设置驱动内部记录的 DAPM level，不会修改设备状态。
 * 因此它不应在正常运行期间使用，只适合把内部状态同步到设备状态。
 * 例如在 driver probe 时，把 DAPM level 设为与设备上电复位状态一致。
 *
 * 若要真正改变设备的 DAPM 状态，请使用 snd_soc_dapm_set_bias_level()。
 */
void snd_soc_dapm_init_bias_level(struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	/* 只改内核侧记录，不直接碰硬件寄存器。 */
	dapm->bias_level = level;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_init_bias_level);

/**
 * snd_soc_dapm_set_bias_level - 设置系统的 bias level
 * @dapm: DAPM 上下文
 * @level: 需要配置的 level
 *
 * 配置 SoC 音频设备的 bias（电源）级别。
 *
 * 返回：成功时返回 0，否则返回错误码。
 */
static int snd_soc_dapm_set_bias_level(struct snd_soc_dapm_context *dapm,
				       enum snd_soc_bias_level level)
{
	struct snd_soc_card *card = dapm->card;
	int ret = 0;

	/* 卡级回调先做前置动作，DAPM 自身再强制同步，最后执行后置收尾。 */
	trace_snd_soc_bias_level_start(dapm, level);

	ret = snd_soc_card_set_bias_level(card, dapm, level);
	if (ret != 0)
		goto out;

	if (dapm != card->dapm)
		ret = snd_soc_dapm_force_bias_level(dapm, level);

	if (ret != 0)
		goto out;

	ret = snd_soc_card_set_bias_level_post(card, dapm, level);
out:
	trace_snd_soc_bias_level_done(dapm, level);

	/* success */
	if (ret == 0)
		snd_soc_dapm_init_bias_level(dapm, level);

	return ret;
}

/**
 * snd_soc_dapm_get_bias_level() - 获取当前 DAPM bias level
 * @dapm: 需要获取 bias level 的上下文
 *
 * 返回：该 DAPM 上下文当前的 bias level。
 */
enum snd_soc_bias_level snd_soc_dapm_get_bias_level(struct snd_soc_dapm_context *dapm)
{
	return dapm->bias_level;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_bias_level);

static int dapm_is_shared_kcontrol(struct snd_soc_dapm_context *dapm,
	struct snd_soc_dapm_widget *kcontrolw,
	const struct snd_kcontrol_new *kcontrol_new,
	struct snd_kcontrol **kcontrol)
{
	struct snd_soc_dapm_widget *w;
	int i;

	/*
	 * 共享 kcontrol 的场景：不同 widget 可能引用同一个控制定义。
	 * 这里先在同一张 card 的同类 widget 里找现成实例，避免重复创建。
	 */
	*kcontrol = NULL;

	for_each_card_widgets(dapm->card, w) {
		if (w == kcontrolw || w->dapm != kcontrolw->dapm)
			continue;
		for (i = 0; i < w->num_kcontrols; i++) {
			if (&w->kcontrol_news[i] == kcontrol_new) {
				if (w->kcontrols)
					*kcontrol = w->kcontrols[i];
				return 1;
			}
		}
	}

	return 0;
}

/*
 * 判断某个 kcontrol 是否被共享。如果是就复用现有实例；如果不是就创建。
 * 无论哪种情况，都要把当前 widget 加入该 control 的 widget 列表。
 */
static int dapm_create_or_share_kcontrol(struct snd_soc_dapm_widget *w,
	int kci)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	struct snd_card *card = dapm->card->snd_card;
	const char *prefix;
	size_t prefix_len;
	int shared;
	struct snd_kcontrol *kcontrol;
	bool wname_in_long_name, kcname_in_long_name;
	char *long_name = NULL;
	const char *name;
	int ret = 0;

	prefix = dapm_prefix(dapm);
	if (prefix)
		prefix_len = strlen(prefix) + 1;
	else
		prefix_len = 0;

	shared = dapm_is_shared_kcontrol(dapm, w, &w->kcontrol_news[kci],
					 &kcontrol);

	if (!kcontrol) {
		/*
		 * 这个控制定义如果没有被别的 widget 共享，就按 widget 类型
		 * 决定名字拼装策略：有些要把 widget 名塞进长名里，有些只
		 * 需要 control 名即可。
		 */
		if (shared) {
			wname_in_long_name = false;
			kcname_in_long_name = true;
		} else {
			switch (w->id) {
			case snd_soc_dapm_switch:
			case snd_soc_dapm_mixer:
			case snd_soc_dapm_pga:
			case snd_soc_dapm_effect:
			case snd_soc_dapm_out_drv:
				wname_in_long_name = true;
				kcname_in_long_name = true;
				break;
			case snd_soc_dapm_mux_named_ctl:
			case snd_soc_dapm_mixer_named_ctl:
				wname_in_long_name = false;
				kcname_in_long_name = true;
				break;
			case snd_soc_dapm_demux:
			case snd_soc_dapm_mux:
				wname_in_long_name = true;
				kcname_in_long_name = false;
				break;
			default:
				return -EINVAL;
			}
		}
		if (w->no_wname_in_kcontrol_name)
			wname_in_long_name = false;

		if (wname_in_long_name && kcname_in_long_name) {
			/*
			 * control 在创建过程中会自动带上前缀，但 widget 也在使用
			 * 同一前缀，所以这里要把 widget 名称前面的前缀裁掉。
			 */
			long_name = kasprintf(GFP_KERNEL, "%s %s",
				 w->name + prefix_len,
				 w->kcontrol_news[kci].name);
			if (long_name == NULL)
				return -ENOMEM;

			name = long_name;
		} else if (wname_in_long_name) {
			long_name = NULL;
			name = w->name + prefix_len;
		} else {
			long_name = NULL;
			name = w->kcontrol_news[kci].name;
		}

		kcontrol = snd_soc_cnew(&w->kcontrol_news[kci], NULL, name,
					prefix);
		if (!kcontrol) {
			ret = -ENOMEM;
			goto exit_free;
		}

		kcontrol->private_free = dapm_kcontrol_free;

		ret = dapm_kcontrol_data_alloc(w, kcontrol, name);
		if (ret) {
			snd_ctl_free_one(kcontrol);
			goto exit_free;
		}

		ret = snd_ctl_add(card, kcontrol);
		if (ret < 0) {
			dev_err(dev,
				"ASoC: failed to add widget %s dapm kcontrol %s: %d\n",
				w->name, name, ret);
			goto exit_free;
		}
	}

	ret = dapm_kcontrol_add_widget(kcontrol, w);
	if (ret == 0)
		w->kcontrols[kci] = kcontrol;

exit_free:
	kfree(long_name);

	return ret;
}

/* 创建新的 DAPM mixer control。 */
static int dapm_new_mixer(struct snd_soc_dapm_widget *w)
{
	int i, ret;
	struct snd_soc_dapm_path *path;
	struct dapm_kcontrol_data *data;

	/* 添加 kcontrol。 */
	for (i = 0; i < w->num_kcontrols; i++) {
		/* 按名字匹配。 */
		snd_soc_dapm_widget_for_each_source_path(w, path) {
			/* mixer/mux 路径名必须和 control 名称匹配。 */
			if (path->name != (char *)w->kcontrol_news[i].name)
				continue;

			if (!w->kcontrols[i]) {
				ret = dapm_create_or_share_kcontrol(w, i);
				if (ret < 0)
					return ret;
			}

			dapm_kcontrol_add_path(w->kcontrols[i], path);

			data = snd_kcontrol_chip(w->kcontrols[i]);
			if (data->widget)
				dapm_add_path(data->widget->dapm,
					      data->widget,
					      path->source,
					      NULL, NULL);
		}
	}

	return 0;
}

/* 创建新的 DAPM mux control。 */
static int dapm_new_mux(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	enum snd_soc_dapm_direction dir;
	struct snd_soc_dapm_path *path;
	const char *type;
	int ret;

	switch (w->id) {
	case snd_soc_dapm_mux:
	case snd_soc_dapm_mux_named_ctl:
		dir = SND_SOC_DAPM_DIR_OUT;
		type = "mux";
		break;
	case snd_soc_dapm_demux:
		dir = SND_SOC_DAPM_DIR_IN;
		type = "demux";
		break;
	default:
		return -EINVAL;
	}

	if (w->num_kcontrols != 1) {
		dev_err(dev,
			"ASoC: %s %s has incorrect number of controls\n", type,
			w->name);
		return -EINVAL;
	}

	if (list_empty(&w->edges[dir])) {
		dev_err(dev, "ASoC: %s %s has no paths\n", type, w->name);
		return -EINVAL;
	}

	ret = dapm_create_or_share_kcontrol(w, 0);
	if (ret < 0)
		return ret;

	snd_soc_dapm_widget_for_each_path(w, dir, path) {
		if (path->name)
			dapm_kcontrol_add_path(w->kcontrols[0], path);
	}

	return 0;
}

/* 创建新的 DAPM volume control。 */
static int dapm_new_pga(struct snd_soc_dapm_widget *w)
{
	int i;

	/* PGA 只是为每一路 kcontrol 建立对应控制，没有额外路径匹配逻辑。 */
	for (i = 0; i < w->num_kcontrols; i++) {
		int ret = dapm_create_or_share_kcontrol(w, i);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* 创建新的 DAPM DAI link control。 */
static int dapm_new_dai_link(struct snd_soc_dapm_widget *w)
{
	int i;
	struct snd_soc_pcm_runtime *rtd = w->priv;

	/* 只为拥有多个配置的 link 创建控制。 */
	if (rtd->dai_link->num_c2c_params <= 1)
		return 0;

	/* DAI link 类 widget 的控制是直接挂到 runtime 上，而不是共享路径。 */
	/* 添加 kcontrol。 */
	for (i = 0; i < w->num_kcontrols; i++) {
		struct snd_soc_dapm_context *dapm = w->dapm;
		struct device *dev = snd_soc_dapm_to_dev(dapm);
		struct snd_card *card = dapm->card->snd_card;
		struct snd_kcontrol *kcontrol = snd_soc_cnew(&w->kcontrol_news[i],
							     w, w->name, NULL);
		int ret = snd_ctl_add(card, kcontrol);

		if (ret < 0) {
			dev_err(dev,
				"ASoC: failed to add widget %s dapm kcontrol %s: %d\n",
				w->name, w->kcontrol_news[i].name, ret);
			return ret;
		}
		kcontrol->private_data = w;
		w->kcontrols[i] = kcontrol;
	}

	return 0;
}

/*
 * 这里通过检查 ALSA card 的 power state 来实现 suspend 时下电；
 * 当系统挂起时，card 的 ALSA 状态会被设置为 D3。
 */
static int dapm_suspend_check(struct snd_soc_dapm_widget *widget)
{
	struct device *dev = snd_soc_dapm_to_dev(widget->dapm);
	int level = snd_power_get_state(widget->dapm->card->snd_card);

	/* 挂起时是否保留这个 widget，取决于卡当前的 power state 和 ignore_suspend。 */
	switch (level) {
	case SNDRV_CTL_POWER_D3hot:
	case SNDRV_CTL_POWER_D3cold:
		if (widget->ignore_suspend)
			dev_dbg(dev, "ASoC: %s ignoring suspend\n",
				widget->name);
		return widget->ignore_suspend;
	default:
		return 1;
	}
}

static void dapm_widget_list_free(struct snd_soc_dapm_widget_list **list)
{
	/* widget list 是一次性分配的聚合容器，直接整体释放即可。 */
	kfree(*list);
}

static int dapm_widget_list_create(struct snd_soc_dapm_widget_list **list,
	struct list_head *widgets)
{
	struct snd_soc_dapm_widget *w;
	struct list_head *it;
	unsigned int size = 0;
	unsigned int i = 0;

	/* 把临时链表压缩成连续数组，方便导出给 DAI 连接查询接口。 */
	list_for_each(it, widgets)
		size++;

	*list = kzalloc_flex(**list, widgets, size);
	if (*list == NULL)
		return -ENOMEM;

	(*list)->num_widgets = size;

	list_for_each_entry(w, widgets, work_list)
		(*list)->widgets[i++] = w;

	(*list)->num_widgets = i;

	return 0;
}

/*
 * 递归重置指定 widget 以及所有可达 widget 的输入/输出路径缓存计数。
 */
static void dapm_invalidate_paths_ep(struct snd_soc_dapm_widget *widget,
	enum snd_soc_dapm_direction dir)
{
	enum snd_soc_dapm_direction rdir = DAPM_DIR_REVERSE(dir);
	struct snd_soc_dapm_path *path;

	widget->endpoints[dir] = -1;

	/* 反向遍历所有路径，把沿途缓存的 endpoint 计数全部打脏。 */
	snd_soc_dapm_widget_for_each_path(widget, rdir, path) {
		if (path->is_supply)
			continue;

		if (path->walking)
			return;

		if (path->connect) {
			path->walking = 1;
			dapm_invalidate_paths_ep(path->node[dir], dir);
			path->walking = 0;
		}
	}
}

/*
 * is_connected_output_ep() 和 is_connected_input_ep() 的公共实现。
 * 之所以内联，是因为两个特化函数的总大小只比通用版本略大，
 * 但特化版本的快路径会明显更小。
 */
static __always_inline int dapm_is_connected_ep(struct snd_soc_dapm_widget *widget,
	struct list_head *list, enum snd_soc_dapm_direction dir,
	int (*fn)(struct snd_soc_dapm_widget *, struct list_head *,
		  bool (*custom_stop_condition)(struct snd_soc_dapm_widget *,
						enum snd_soc_dapm_direction)),
	bool (*custom_stop_condition)(struct snd_soc_dapm_widget *,
				      enum snd_soc_dapm_direction))
{
	enum snd_soc_dapm_direction rdir = DAPM_DIR_REVERSE(dir);
	struct snd_soc_dapm_path *path;
	int con = 0;

	if (widget->endpoints[dir] >= 0)
		return widget->endpoints[dir];

	/* 这个递归会把可达的 widget 顺手串进 list，供后续统一同步。 */
	DAPM_UPDATE_STAT(widget, path_checks);

	/* 这个 widget 需要加入列表吗？ */
	if (list)
		list_add_tail(&widget->work_list, list);

	if (custom_stop_condition && custom_stop_condition(widget, dir)) {
		list = NULL;
		custom_stop_condition = NULL;
	}

	if ((widget->is_ep & SND_SOC_DAPM_DIR_TO_EP(dir)) && widget->connected) {
		widget->endpoints[dir] = dapm_suspend_check(widget);
		return widget->endpoints[dir];
	}

	snd_soc_dapm_widget_for_each_path(widget, rdir, path) {
		DAPM_UPDATE_STAT(widget, neighbour_checks);

		if (path->is_supply)
			continue;

		if (path->walking)
			return 1;

		trace_snd_soc_dapm_path(widget, dir, path);

		if (path->connect) {
			path->walking = 1;
			con += fn(path->node[dir], list, custom_stop_condition);
			path->walking = 0;
		}
	}

	widget->endpoints[dir] = con;

	return con;
}

/*
 * 递归检查到一个已激活或物理连接的输出 widget 是否存在完整路径。
 * 返回完整路径数量。
 *
 * 也可以提供一个停止条件函数。该函数接收当前检查的 dapm widget
 * 和遍历方向；如果从该点开始不应再把后续 widget 加入列表，则返回 true。
 */
static int dapm_is_connected_output_ep(struct snd_soc_dapm_widget *widget,
	struct list_head *list,
	bool (*custom_stop_condition)(struct snd_soc_dapm_widget *i,
				      enum snd_soc_dapm_direction))
{
	return dapm_is_connected_ep(widget, list, SND_SOC_DAPM_DIR_OUT,
			dapm_is_connected_output_ep, custom_stop_condition);
}

/*
 * 递归检查到一个已激活或物理连接的输入 widget 是否存在完整路径。
 * 返回完整路径数量。
 *
 * 也可以提供一个停止条件函数。该函数接收当前检查的 dapm widget
 * 和遍历方向；如果应该停止遍历，则返回 true，否则返回 false。
 */
static int dapm_is_connected_input_ep(struct snd_soc_dapm_widget *widget,
	struct list_head *list,
	bool (*custom_stop_condition)(struct snd_soc_dapm_widget *i,
				      enum snd_soc_dapm_direction))
{
	return dapm_is_connected_ep(widget, list, SND_SOC_DAPM_DIR_IN,
			dapm_is_connected_input_ep, custom_stop_condition);
}

/**
 * snd_soc_dapm_dai_get_connected_widgets - 查询音频路径及其 widget
 * @dai: SoC DAI
 * @stream: 流方向
 * @list: 当前流对应的活跃 widget 列表
 * @custom_stop_condition: 可选的停止条件回调，用于按自定义逻辑中止遍历
 *
 * 查询 DAPM 图中指定流方向是否存在有效音频路径。
 * 这个过程会考虑当前 mixer 和 mux kcontrol 的设置，并创建
 * 有效 widget 的列表。
 *
 * 也可以传入一个停止条件函数。该函数接收当前正在检查的 dapm widget
 * 以及遍历方向，如果应该停止继续向图中遍历，则返回 true。
 *
 * 返回：有效路径数量，或负错误码。
 */
int snd_soc_dapm_dai_get_connected_widgets(struct snd_soc_dai *dai, int stream,
	struct snd_soc_dapm_widget_list **list,
	bool (*custom_stop_condition)(struct snd_soc_dapm_widget *,
				      enum snd_soc_dapm_direction))
{
	struct snd_soc_card *card = dai->component->card;
	struct snd_soc_dapm_widget *w = snd_soc_dai_get_widget(dai, stream);
	LIST_HEAD(widgets);
	int paths;
	int ret;

	snd_soc_dapm_mutex_lock(card);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		dapm_invalidate_paths_ep(w, SND_SOC_DAPM_DIR_OUT);
		paths = dapm_is_connected_output_ep(w, &widgets,
				custom_stop_condition);
	} else {
		dapm_invalidate_paths_ep(w, SND_SOC_DAPM_DIR_IN);
		paths = dapm_is_connected_input_ep(w, &widgets,
				custom_stop_condition);
	}

	/* Drop starting point */
	list_del(widgets.next);

	ret = dapm_widget_list_create(list, &widgets);
	if (ret)
		paths = ret;

	trace_snd_soc_dapm_connected(paths, stream);
	snd_soc_dapm_mutex_unlock(card);

	return paths;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_dai_get_connected_widgets);

void snd_soc_dapm_dai_free_widgets(struct snd_soc_dapm_widget_list **list)
{
	dapm_widget_list_free(list);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_dai_free_widgets);

/* regulator supply widget 的处理函数。 */
int snd_soc_dapm_regulator_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	struct device *dev = snd_soc_dapm_to_dev(w->dapm);
	int ret;

	/*
	 * regulator widget 只负责把供电器件开/关，真正的电源时序仍由
	 * DAPM 统一调度。这里先补完异步访问，再切 regulator 状态。
	 */
	dapm_async_complete(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if (w->on_val & SND_SOC_DAPM_REGULATOR_BYPASS) {
			ret = regulator_allow_bypass(w->regulator, false);
			if (ret != 0)
				dev_warn(dev,
					 "ASoC: Failed to unbypass %s: %d\n",
					 w->name, ret);
		}

		return regulator_enable(w->regulator);
	} else {
		if (w->on_val & SND_SOC_DAPM_REGULATOR_BYPASS) {
			ret = regulator_allow_bypass(w->regulator, true);
			if (ret != 0)
				dev_warn(dev,
					 "ASoC: Failed to bypass %s: %d\n",
					 w->name, ret);
		}

		return regulator_disable_deferred(w->regulator, w->shift);
	}
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_regulator_event);

/* pinctrl widget 的处理函数。 */
int snd_soc_dapm_pinctrl_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_dapm_pinctrl_priv *priv = w->priv;
	struct pinctrl *p = w->pinctrl;
	struct pinctrl_state *s;

	/* pinctrl widget 的职责是把 DAPM 状态映射到 pinctrl state 切换。 */
	if (!p || !priv)
		return -EIO;

	if (SND_SOC_DAPM_EVENT_ON(event))
		s = pinctrl_lookup_state(p, priv->active_state);
	else
		s = pinctrl_lookup_state(p, priv->sleep_state);

	if (IS_ERR(s))
		return PTR_ERR(s);

	return pinctrl_select_state(p, s);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_pinctrl_event);

/* clock supply widget 的处理函数。 */
int snd_soc_dapm_clock_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	if (!w->clk)
		return -EIO;

	/* clock widget 只是对时钟使能位做包装，依然要先等待异步事务完成。 */
	dapm_async_complete(w->dapm);

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		return clk_prepare_enable(w->clk);
	} else {
		clk_disable_unprepare(w->clk);
		return 0;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_clock_event);

static int dapm_widget_power_check(struct snd_soc_dapm_widget *w)
{
	if (w->power_checked)
		return w->new_power;

	if (w->force)
		w->new_power = 1;
	else
		w->new_power = w->power_check(w);

	w->power_checked = true;

	return w->new_power;
}

/* 通用的 widget 上电判定。 */
static int dapm_generic_check_power(struct snd_soc_dapm_widget *w)
{
	int in, out;

	DAPM_UPDATE_STAT(w, power_checks);

	in  = dapm_is_connected_input_ep(w, NULL, NULL);
	out = dapm_is_connected_output_ep(w, NULL, NULL);
	return out != 0 && in != 0;
}

/* 判断是否需要电源供给。 */
static int dapm_supply_check_power(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_path *path;

	DAPM_UPDATE_STAT(w, power_checks);

	/* Check if one of our outputs is connected */
	snd_soc_dapm_widget_for_each_sink_path(w, path) {
		DAPM_UPDATE_STAT(w, neighbour_checks);

		if (path->connected &&
		    !path->connected(path->source, path->sink))
			continue;

		if (dapm_widget_power_check(path->sink))
			return 1;
	}

	return 0;
}

static int dapm_always_on_check_power(struct snd_soc_dapm_widget *w)
{
	return w->connected;
}

static int dapm_seq_compare(struct snd_soc_dapm_widget *a,
			    struct snd_soc_dapm_widget *b,
			    bool power_up)
{
	int *sort;

	BUILD_BUG_ON(ARRAY_SIZE(dapm_up_seq) != SND_SOC_DAPM_TYPE_COUNT);
	BUILD_BUG_ON(ARRAY_SIZE(dapm_down_seq) != SND_SOC_DAPM_TYPE_COUNT);

	if (power_up)
		sort = dapm_up_seq;
	else
		sort = dapm_down_seq;

	WARN_ONCE(sort[a->id] == 0, "offset a->id %d not initialized\n", a->id);
	WARN_ONCE(sort[b->id] == 0, "offset b->id %d not initialized\n", b->id);

	if (sort[a->id] != sort[b->id])
		return sort[a->id] - sort[b->id];
	if (a->subseq != b->subseq) {
		if (power_up)
			return a->subseq - b->subseq;
		else
			return b->subseq - a->subseq;
	}
	if (a->reg != b->reg)
		return a->reg - b->reg;
	if (a->dapm != b->dapm)
		return (unsigned long)a->dapm - (unsigned long)b->dapm;

	return 0;
}

/* 将 widget 按顺序插入 DAPM 上下电序列。 */
static void dapm_seq_insert(struct snd_soc_dapm_widget *new_widget,
			    struct list_head *list,
			    bool power_up)
{
	struct snd_soc_dapm_widget *w;

	list_for_each_entry(w, list, power_list)
		if (dapm_seq_compare(new_widget, w, power_up) < 0) {
			list_add_tail(&new_widget->power_list, &w->power_list);
			return;
		}

	list_add_tail(&new_widget->power_list, list);
}

static void dapm_seq_check_event(struct snd_soc_card *card,
				 struct snd_soc_dapm_widget *w, int event)
{
	struct device *dev = card->dev;
	const char *ev_name;
	int power;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ev_name = "PRE_PMU";
		power = 1;
		break;
	case SND_SOC_DAPM_POST_PMU:
		ev_name = "POST_PMU";
		power = 1;
		break;
	case SND_SOC_DAPM_PRE_PMD:
		ev_name = "PRE_PMD";
		power = 0;
		break;
	case SND_SOC_DAPM_POST_PMD:
		ev_name = "POST_PMD";
		power = 0;
		break;
	case SND_SOC_DAPM_WILL_PMU:
		ev_name = "WILL_PMU";
		power = 1;
		break;
	case SND_SOC_DAPM_WILL_PMD:
		ev_name = "WILL_PMD";
		power = 0;
		break;
	default:
		WARN(1, "Unknown event %d\n", event);
		return;
	}

	if (w->new_power != power)
		return;

	if (w->event && (w->event_flags & event)) {
		int ret;

		dapm_pop_dbg(dev, card->pop_time, "pop test : %s %s\n",
			w->name, ev_name);
		dapm_async_complete(w->dapm);
		trace_snd_soc_dapm_widget_event_start(w, event);
		ret = w->event(w, NULL, event);
		trace_snd_soc_dapm_widget_event_done(w, event);
		if (ret < 0)
			dev_err(dev, "ASoC: %s: %s event failed: %d\n",
			       ev_name, w->name, ret);
	}
}

/* 应用 DAPM 序列里合并后的更改。 */
static void dapm_seq_run_coalesced(struct snd_soc_card *card,
				   struct list_head *pending)
{
	struct device *dev = card->dev;
	struct snd_soc_dapm_context *dapm;
	struct snd_soc_dapm_widget *w;
	int reg;
	unsigned int value = 0;
	unsigned int mask = 0;

	w = list_first_entry(pending, struct snd_soc_dapm_widget, power_list);
	reg = w->reg;
	dapm = w->dapm;

	list_for_each_entry(w, pending, power_list) {
		WARN_ON(reg != w->reg || dapm != w->dapm);
		w->power = w->new_power;

		mask |= w->mask << w->shift;
		if (w->power)
			value |= w->on_val << w->shift;
		else
			value |= w->off_val << w->shift;

		dapm_pop_dbg(dev, card->pop_time,
			"pop test : Queue %s: reg=0x%x, 0x%x/0x%x\n",
			w->name, reg, value, mask);

		/* Check for events */
		dapm_seq_check_event(card, w, SND_SOC_DAPM_PRE_PMU);
		dapm_seq_check_event(card, w, SND_SOC_DAPM_PRE_PMD);
	}

	if (reg >= 0) {
		/* Any widget will do, they should all be updating the
		 * same register.
		 */

		dapm_pop_dbg(dev, card->pop_time,
			"pop test : Applying 0x%x/0x%x to %x in %dms\n",
			value, mask, reg, card->pop_time);
		dapm_pop_wait(card->pop_time);
		dapm_update_bits(dapm, reg, mask, value);
	}

	list_for_each_entry(w, pending, power_list) {
		dapm_seq_check_event(card, w, SND_SOC_DAPM_POST_PMU);
		dapm_seq_check_event(card, w, SND_SOC_DAPM_POST_PMD);
	}
}

/*
 * 应用一段 DAPM 上/下电序列。
 *
 * 这里会遍历已经排好序的 widget 列表并执行上电/下电。
 * 为了尽量减少对设备的写次数，能合并写入的 widget 会尽量
 * 合并成一次写操作。当前暂不处理需要多次写入才能完成的情况。
 */
static void dapm_seq_run(struct snd_soc_card *card,
	struct list_head *list, int event, bool power_up)
{
	struct device *dev = card->dev;
	struct snd_soc_dapm_widget *w, *n;
	struct snd_soc_dapm_context *d;
	LIST_HEAD(pending);
	int cur_sort = -1;
	int cur_subseq = -1;
	int cur_reg = SND_SOC_NOPM;
	struct snd_soc_dapm_context *cur_dapm = NULL;
	int i;
	int *sort;

	if (power_up)
		sort = dapm_up_seq;
	else
		sort = dapm_down_seq;

	list_for_each_entry_safe(w, n, list, power_list) {
		int ret = 0;

		/* Do we need to apply any queued changes? */
		if (sort[w->id] != cur_sort || w->reg != cur_reg ||
		    w->dapm != cur_dapm || w->subseq != cur_subseq) {
			if (!list_empty(&pending))
				dapm_seq_run_coalesced(card, &pending);

			if (cur_dapm && cur_dapm->component) {
				for (i = 0; i < ARRAY_SIZE(dapm_up_seq); i++)
					if (sort[i] == cur_sort)
						snd_soc_component_seq_notifier(
							cur_dapm->component,
							i, cur_subseq);
			}

			if (cur_dapm && w->dapm != cur_dapm)
				dapm_async_complete(cur_dapm);

			INIT_LIST_HEAD(&pending);
			cur_sort = -1;
			cur_subseq = INT_MIN;
			cur_reg = SND_SOC_NOPM;
			cur_dapm = NULL;
		}

		switch (w->id) {
		case snd_soc_dapm_pre:
			if (!w->event)
				continue;

			if (event == SND_SOC_DAPM_STREAM_START)
				ret = w->event(w,
					       NULL, SND_SOC_DAPM_PRE_PMU);
			else if (event == SND_SOC_DAPM_STREAM_STOP)
				ret = w->event(w,
					       NULL, SND_SOC_DAPM_PRE_PMD);
			break;

		case snd_soc_dapm_post:
			if (!w->event)
				continue;

			if (event == SND_SOC_DAPM_STREAM_START)
				ret = w->event(w,
					       NULL, SND_SOC_DAPM_POST_PMU);
			else if (event == SND_SOC_DAPM_STREAM_STOP)
				ret = w->event(w,
					       NULL, SND_SOC_DAPM_POST_PMD);
			break;

		default:
			/* Queue it up for application */
			cur_sort = sort[w->id];
			cur_subseq = w->subseq;
			cur_reg = w->reg;
			cur_dapm = w->dapm;
			list_move(&w->power_list, &pending);
			break;
		}

		if (ret < 0)
			dev_err(dev,
				"ASoC: Failed to apply widget power: %d\n", ret);
	}

	if (!list_empty(&pending))
		dapm_seq_run_coalesced(card, &pending);

	if (cur_dapm && cur_dapm->component) {
		for (i = 0; i < ARRAY_SIZE(dapm_up_seq); i++)
			if (sort[i] == cur_sort)
				snd_soc_component_seq_notifier(
					cur_dapm->component,
					i, cur_subseq);
	}

	for_each_card_dapms(card, d)
		dapm_async_complete(d);
}

static void dapm_widget_update(struct snd_soc_card *card, struct snd_soc_dapm_update *update)
{
	struct device *dev = card->dev;
	struct snd_soc_dapm_widget_list *wlist;
	struct snd_soc_dapm_widget *w = NULL;
	unsigned int wi;
	int ret;

	if (!update || !dapm_kcontrol_is_powered(update->kcontrol))
		return;

	wlist = dapm_kcontrol_get_wlist(update->kcontrol);

	for_each_dapm_widgets(wlist, wi, w) {
		if (w->event && (w->event_flags & SND_SOC_DAPM_PRE_REG)) {
			ret = w->event(w, update->kcontrol, SND_SOC_DAPM_PRE_REG);
			if (ret != 0)
				dev_err(dev, "ASoC: %s DAPM pre-event failed: %d\n",
					   w->name, ret);
		}
	}

	if (!w)
		return;

	ret = dapm_update_bits(w->dapm, update->reg, update->mask,
		update->val);
	if (ret < 0)
		dev_err(dev, "ASoC: %s DAPM update failed: %d\n",
			w->name, ret);

	if (update->has_second_set) {
		ret = dapm_update_bits(w->dapm, update->reg2,
					   update->mask2, update->val2);
		if (ret < 0)
			dev_err(dev,
				"ASoC: %s DAPM update failed: %d\n",
				w->name, ret);
	}

	for_each_dapm_widgets(wlist, wi, w) {
		if (w->event && (w->event_flags & SND_SOC_DAPM_POST_REG)) {
			ret = w->event(w, update->kcontrol, SND_SOC_DAPM_POST_REG);
			if (ret != 0)
				dev_err(dev, "ASoC: %s DAPM post-event failed: %d\n",
					   w->name, ret);
		}
	}
}

/*
 * 在 DAPM 序列执行前运行的异步回调。
 * 如果状态发生变化，会先把它们切到 _PREPARE。
 */
static void dapm_pre_sequence_async(void *data, async_cookie_t cookie)
{
	struct snd_soc_dapm_context *dapm = data;
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	int ret;

	/* If we're off and we're not supposed to go into STANDBY */
	if (dapm->bias_level == SND_SOC_BIAS_OFF &&
	    dapm->target_bias_level != SND_SOC_BIAS_OFF) {
		if (dev && cookie)
			pm_runtime_get_sync(dev);

		ret = snd_soc_dapm_set_bias_level(dapm, SND_SOC_BIAS_STANDBY);
		if (ret != 0)
			dev_err(dev,
				"ASoC: Failed to turn on bias: %d\n", ret);
	}

	/* Prepare for a transition to ON or away from ON */
	if ((dapm->target_bias_level == SND_SOC_BIAS_ON &&
	     dapm->bias_level != SND_SOC_BIAS_ON) ||
	    (dapm->target_bias_level != SND_SOC_BIAS_ON &&
	     dapm->bias_level == SND_SOC_BIAS_ON)) {
		ret = snd_soc_dapm_set_bias_level(dapm, SND_SOC_BIAS_PREPARE);
		if (ret != 0)
			dev_err(dev,
				"ASoC: Failed to prepare bias: %d\n", ret);
	}
}

/*
 * 在 DAPM 序列执行前运行的异步回调。
 * 用于把对象推进到最终状态。
 */
static void dapm_post_sequence_async(void *data, async_cookie_t cookie)
{
	struct snd_soc_dapm_context *dapm = data;
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	int ret;

	/* If we just powered the last thing off drop to standby bias */
	if (dapm->bias_level == SND_SOC_BIAS_PREPARE &&
	    (dapm->target_bias_level == SND_SOC_BIAS_STANDBY ||
	     dapm->target_bias_level == SND_SOC_BIAS_OFF)) {
		ret = snd_soc_dapm_set_bias_level(dapm, SND_SOC_BIAS_STANDBY);
		if (ret != 0)
			dev_err(dev, "ASoC: Failed to apply standby bias: %d\n", ret);
	}

	/* If we're in standby and can support bias off then do that */
	if (dapm->bias_level == SND_SOC_BIAS_STANDBY &&
	    dapm->target_bias_level == SND_SOC_BIAS_OFF) {
		ret = snd_soc_dapm_set_bias_level(dapm, SND_SOC_BIAS_OFF);
		if (ret != 0)
			dev_err(dev, "ASoC: Failed to turn off bias: %d\n", ret);

		if (dev && cookie)
			pm_runtime_put(dev);
	}

	/* If we just powered up then move to active bias */
	if (dapm->bias_level == SND_SOC_BIAS_PREPARE &&
	    dapm->target_bias_level == SND_SOC_BIAS_ON) {
		ret = snd_soc_dapm_set_bias_level(dapm, SND_SOC_BIAS_ON);
		if (ret != 0)
			dev_err(dev, "ASoC: Failed to apply active bias: %d\n", ret);
	}
}

static void dapm_widget_set_peer_power(struct snd_soc_dapm_widget *peer,
				       bool power, bool connect)
{
	/*
	 * 只有在路径真的建立/断开时，peer 才可能受影响。
	 * 这里不直接改对端电源状态，只在必要时把它打脏，让后续
	 * DAPM 统一重新计算。
	 */
	if (!connect)
		return;

	/* 如果对端已经处在目标状态，就没有必要再次标脏。 */
	if (power != peer->power)
		dapm_mark_dirty(peer, "peer state change");
}

static void dapm_power_one_widget(struct snd_soc_dapm_widget *w,
				  struct list_head *up_list,
				  struct list_head *down_list)
{
	struct snd_soc_dapm_path *path;
	int power;

	switch (w->id) {
	case snd_soc_dapm_pre:
		power = 0;
		goto end;
	case snd_soc_dapm_post:
		power = 1;
		goto end;
	default:
		break;
	}

	power = dapm_widget_power_check(w);

	if (w->power == power)
		return;

	trace_snd_soc_dapm_widget_power(w, power);

	/*
	 * 当前 widget 的 power 变化可能会连带影响前后级路径。
	 * 这里先把源端的 peer 标脏，再由下一轮 DAPM 递归重新计算。
	 */
	snd_soc_dapm_widget_for_each_source_path(w, path)
		dapm_widget_set_peer_power(path->source, power, path->connect);

	/*
	 * 供电类 widget 只会影响下游输入，不会反向改变输出端。
	 */
	if (!w->is_supply)
		snd_soc_dapm_widget_for_each_sink_path(w, path)
			dapm_widget_set_peer_power(path->sink, power, path->connect);

end:
	if (power)
		dapm_seq_insert(w, up_list, true);
	else
		dapm_seq_insert(w, down_list, false);
}

bool snd_soc_dapm_get_idle_bias(struct snd_soc_dapm_context *dapm)
{
	if (dapm->idle_bias) {
		struct snd_soc_component *component = snd_soc_dapm_to_component(dapm);
		unsigned int state = snd_power_get_state(dapm->card->snd_card);

		if ((state == SNDRV_CTL_POWER_D3hot || (state == SNDRV_CTL_POWER_D3cold)) &&
		    component)
			return !component->driver->suspend_bias_off;
	}

	return dapm->idle_bias;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_idle_bias);

void snd_soc_dapm_set_idle_bias(struct snd_soc_dapm_context *dapm, bool on)
{
	dapm->idle_bias = on;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_set_idle_bias);

/*
 * 扫描每个 DAPM widget，寻找完整的音频路径。
 * 所谓完整路径，是指端点有效的路径，例如：
 *
 *  o DAC 到输出 pin
 *  o 输入 pin 到 ADC
 *  o 输入 pin 到输出 pin（bypass、sidetone）
 *  o DAC 到 ADC（loopback）
 */
static int dapm_power_widgets(struct snd_soc_card *card, int event,
			      struct snd_soc_dapm_update *update)
{
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	struct snd_soc_dapm_widget *w;
	struct snd_soc_dapm_context *d;
	LIST_HEAD(up_list);
	LIST_HEAD(down_list);
	ASYNC_DOMAIN_EXCLUSIVE(async_domain);
	enum snd_soc_bias_level bias;
	int ret;

	snd_soc_dapm_mutex_assert_held(card);

	trace_snd_soc_dapm_start(card, event);

	/*
	 * 每轮 power walk 先给所有 DAPM context 计算目标 bias。
	 * 这个目标值是“图上有多少活跃路径”与“idle bias 策略”的共同结果。
	 */
	for_each_card_dapms(card, d) {
		if (snd_soc_dapm_get_idle_bias(d))
			d->target_bias_level = SND_SOC_BIAS_STANDBY;
		else
			d->target_bias_level = SND_SOC_BIAS_OFF;
	}

	dapm_reset(card);

	/*
	 * 先把 dirty widget 分拣成 up/down 两个序列，再按序列做
	 * 寄存器更新和事件回调。这样可以尽量减少 pop/click。
	 */
	list_for_each_entry(w, &card->dapm_dirty, dirty) {
		dapm_power_one_widget(w, &up_list, &down_list);
	}

	for_each_card_widgets(card, w) {
		switch (w->id) {
		case snd_soc_dapm_pre:
		case snd_soc_dapm_post:
			/* These widgets always need to be powered */
			break;
		default:
			list_del_init(&w->dirty);
			break;
		}

		if (w->new_power) {
			d = w->dapm;

			/*
			 * 供电、micbias 这类 widget 只需要把上下文抬到
			 * STANDBY；信号源 / 虚拟源自身不直接决定整条链路
			 * 要不要进入 ON。
			 */
			switch (w->id) {
			case snd_soc_dapm_siggen:
			case snd_soc_dapm_vmid:
				break;
			case snd_soc_dapm_supply:
			case snd_soc_dapm_regulator_supply:
			case snd_soc_dapm_pinctrl:
			case snd_soc_dapm_clock_supply:
			case snd_soc_dapm_micbias:
				if (d->target_bias_level < SND_SOC_BIAS_STANDBY)
					d->target_bias_level = SND_SOC_BIAS_STANDBY;
				break;
			default:
				d->target_bias_level = SND_SOC_BIAS_ON;
				break;
			}
		}

	}

	/*
	 * 对于非 ground-referenced 的上下文，把 card 内所有 target bias
	 * 收敛到同一个最高值，避免同一张卡上不同子图处于不一致状态。
	 */
	bias = SND_SOC_BIAS_OFF;
	for_each_card_dapms(card, d)
		if (d->target_bias_level > bias)
			bias = d->target_bias_level;
	for_each_card_dapms(card, d)
		if (snd_soc_dapm_get_idle_bias(d))
			d->target_bias_level = bias;

	trace_snd_soc_dapm_walk_done(card);

	/* 先执行 card 自身的 bias 过渡，再并行处理其他 context。 */
	dapm_pre_sequence_async(dapm, 0);
	/* 其他 context 的 bias 变化可以并行。 */
	for_each_card_dapms(card, d) {
		if (d != dapm && d->bias_level != d->target_bias_level)
			async_schedule_domain(dapm_pre_sequence_async, d,
						&async_domain);
	}
	async_synchronize_full_domain(&async_domain);

	list_for_each_entry(w, &down_list, power_list) {
		dapm_seq_check_event(card, w, SND_SOC_DAPM_WILL_PMD);
	}

	list_for_each_entry(w, &up_list, power_list) {
		dapm_seq_check_event(card, w, SND_SOC_DAPM_WILL_PMU);
	}

	/* 先关下游路径，尽量把 pop/click 控制在关断阶段。 */
	dapm_seq_run(card, &down_list, event, false);

	dapm_widget_update(card, update);

	/* 再开上游路径。 */
	dapm_seq_run(card, &up_list, event, true);

	/* 关断/开启后的 bias 收尾也尽量并行。 */
	for_each_card_dapms(card, d) {
		if (d != dapm && d->bias_level != d->target_bias_level)
			async_schedule_domain(dapm_post_sequence_async, d,
						&async_domain);
	}
	async_synchronize_full_domain(&async_domain);
	/* card 自己的 bias 收尾放在最后。 */
	dapm_post_sequence_async(dapm, 0);

	/* do we need to notify any clients that DAPM event is complete */
	for_each_card_dapms(card, d) {
		if (!d->component)
			continue;

		ret = snd_soc_component_stream_event(d->component, event);
		if (ret < 0)
			return ret;
	}

	dapm_pop_dbg(card->dev, card->pop_time,
		"DAPM sequencing finished, waiting %dms\n", card->pop_time);
	dapm_pop_wait(card->pop_time);

	trace_snd_soc_dapm_done(card, event);

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static const char * const dapm_type_name[] = {
	[snd_soc_dapm_input]            = "input",
	[snd_soc_dapm_output]           = "output",
	[snd_soc_dapm_mux]              = "mux",
	[snd_soc_dapm_mux_named_ctl]    = "mux_named_ctl",
	[snd_soc_dapm_demux]            = "demux",
	[snd_soc_dapm_mixer]            = "mixer",
	[snd_soc_dapm_mixer_named_ctl]  = "mixer_named_ctl",
	[snd_soc_dapm_pga]              = "pga",
	[snd_soc_dapm_out_drv]          = "out_drv",
	[snd_soc_dapm_adc]              = "adc",
	[snd_soc_dapm_dac]              = "dac",
	[snd_soc_dapm_micbias]          = "micbias",
	[snd_soc_dapm_mic]              = "mic",
	[snd_soc_dapm_hp]               = "hp",
	[snd_soc_dapm_spk]              = "spk",
	[snd_soc_dapm_line]             = "line",
	[snd_soc_dapm_switch]           = "switch",
	[snd_soc_dapm_vmid]             = "vmid",
	[snd_soc_dapm_pre]              = "pre",
	[snd_soc_dapm_post]             = "post",
	[snd_soc_dapm_supply]           = "supply",
	[snd_soc_dapm_pinctrl]          = "pinctrl",
	[snd_soc_dapm_regulator_supply] = "regulator_supply",
	[snd_soc_dapm_clock_supply]     = "clock_supply",
	[snd_soc_dapm_aif_in]           = "aif_in",
	[snd_soc_dapm_aif_out]          = "aif_out",
	[snd_soc_dapm_siggen]           = "siggen",
	[snd_soc_dapm_sink]             = "sink",
	[snd_soc_dapm_dai_in]           = "dai_in",
	[snd_soc_dapm_dai_out]          = "dai_out",
	[snd_soc_dapm_dai_link]         = "dai_link",
	[snd_soc_dapm_kcontrol]         = "kcontrol",
	[snd_soc_dapm_buffer]           = "buffer",
	[snd_soc_dapm_scheduler]        = "scheduler",
	[snd_soc_dapm_effect]           = "effect",
	[snd_soc_dapm_src]              = "src",
	[snd_soc_dapm_asrc]             = "asrc",
	[snd_soc_dapm_encoder]          = "encoder",
	[snd_soc_dapm_decoder]          = "decoder",
};

static ssize_t dapm_widget_power_read_file(struct file *file,
					   char __user *user_buf,
					   size_t count, loff_t *ppos)
{
	struct snd_soc_dapm_widget *w = file->private_data;
	enum snd_soc_dapm_direction dir, rdir;
	char *buf;
	int in, out;
	ssize_t ret;
	struct snd_soc_dapm_path *p = NULL;
	const char *c_name;

	BUILD_BUG_ON(ARRAY_SIZE(dapm_type_name) != SND_SOC_DAPM_TYPE_COUNT);

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	snd_soc_dapm_mutex_lock_root(w->dapm);

	/* Supply widgets are not handled by dapm_is_connected_{input,output}_ep() */
	if (w->is_supply) {
		in = 0;
		out = 0;
	} else {
		in  = dapm_is_connected_input_ep(w, NULL, NULL);
		out = dapm_is_connected_output_ep(w, NULL, NULL);
	}

	ret = scnprintf(buf, PAGE_SIZE, "%s: %s%s  in %d out %d",
		       w->name, w->power ? "On" : "Off",
		       w->force ? " (forced)" : "", in, out);

	if (w->reg >= 0)
		ret += scnprintf(buf + ret, PAGE_SIZE - ret,
				" - R%d(0x%x) mask 0x%x",
				w->reg, w->reg, w->mask << w->shift);

	ret += scnprintf(buf + ret, PAGE_SIZE - ret, "\n");

	if (w->sname)
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, " stream %s %s\n",
				w->sname,
				w->active ? "active" : "inactive");

	ret += scnprintf(buf + ret, PAGE_SIZE - ret, " widget-type %s\n",
			 dapm_type_name[w->id]);

	dapm_for_each_direction(dir) {
		rdir = DAPM_DIR_REVERSE(dir);
		snd_soc_dapm_widget_for_each_path(w, dir, p) {
			if (p->connected && !p->connected(p->source, p->sink))
				continue;

			if (!p->connect)
				continue;

			c_name = p->node[rdir]->dapm->component ?
				p->node[rdir]->dapm->component->name : NULL;
			ret += scnprintf(buf + ret, PAGE_SIZE - ret,
					" %s  \"%s\" \"%s\" \"%s\"\n",
					(rdir == SND_SOC_DAPM_DIR_IN) ? "in" : "out",
					p->name ? p->name : "static",
					p->node[rdir]->name, c_name);
		}
	}

	snd_soc_dapm_mutex_unlock(w->dapm);

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, ret);

	kfree(buf);
	return ret;
}

static const struct file_operations dapm_widget_power_fops = {
	.open = simple_open,
	.read = dapm_widget_power_read_file,
	.llseek = default_llseek,
};

static ssize_t dapm_bias_read_file(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct snd_soc_dapm_context *dapm = file->private_data;
	char *level;

	switch (dapm->bias_level) {
	case SND_SOC_BIAS_ON:
		level = "On\n";
		break;
	case SND_SOC_BIAS_PREPARE:
		level = "Prepare\n";
		break;
	case SND_SOC_BIAS_STANDBY:
		level = "Standby\n";
		break;
	case SND_SOC_BIAS_OFF:
		level = "Off\n";
		break;
	default:
		WARN(1, "Unknown bias_level %d\n", dapm->bias_level);
		level = "Unknown\n";
		break;
	}

	return simple_read_from_buffer(user_buf, count, ppos, level,
				       strlen(level));
}

static const struct file_operations dapm_bias_fops = {
	.open = simple_open,
	.read = dapm_bias_read_file,
	.llseek = default_llseek,
};

void snd_soc_dapm_debugfs_init(struct snd_soc_dapm_context *dapm,
	struct dentry *parent)
{
	if (IS_ERR_OR_NULL(parent))
		return;

	dapm->debugfs_dapm = debugfs_create_dir("dapm", parent);

	debugfs_create_file("bias_level", 0444, dapm->debugfs_dapm, dapm,
			    &dapm_bias_fops);
}

static void dapm_debugfs_add_widget(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_context *dapm = w->dapm;

	if (!dapm->debugfs_dapm || !w->name)
		return;

	debugfs_create_file(w->name, 0444, dapm->debugfs_dapm, w,
			    &dapm_widget_power_fops);
}

static void dapm_debugfs_free_widget(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_context *dapm = w->dapm;

	if (!dapm->debugfs_dapm || !w->name)
		return;

	debugfs_lookup_and_remove(w->name, dapm->debugfs_dapm);
}

static void dapm_debugfs_cleanup(struct snd_soc_dapm_context *dapm)
{
	debugfs_remove_recursive(dapm->debugfs_dapm);
	dapm->debugfs_dapm = NULL;
}

#else
void snd_soc_dapm_debugfs_init(struct snd_soc_dapm_context *dapm,
	struct dentry *parent)
{
}

static inline void dapm_debugfs_add_widget(struct snd_soc_dapm_widget *w)
{
}

static inline void dapm_debugfs_free_widget(struct snd_soc_dapm_widget *w)
{
}

static inline void dapm_debugfs_cleanup(struct snd_soc_dapm_context *dapm)
{
}

#endif

/*
 * dapm_connect_path() - Connects or disconnects a path
 * @path: The path to update
 * @connect: The new connect state of the path. True if the path is connected,
 *  false if it is disconnected.
 * @reason: The reason why the path changed (for debugging only)
 */
static void dapm_connect_path(struct snd_soc_dapm_path *path,
	bool connect, const char *reason)
{
	if (path->connect == connect)
		return;

	/* 改路径状态时，源端和汇端都要重新参与下一轮图计算。 */
	path->connect = connect;
	dapm_mark_dirty(path->source, reason);
	dapm_mark_dirty(path->sink, reason);
	dapm_path_invalidate(path);
}

/* 测试并更新 mux widget 的电源状态。 */
static int dapm_mux_update_power(struct snd_soc_card *card,
				 struct snd_kcontrol *kcontrol,
				 struct snd_soc_dapm_update *update,
				 int mux, struct soc_enum *e)
{
	struct snd_soc_dapm_path *path;
	int found = 0;
	bool connect;

	snd_soc_dapm_mutex_assert_held(card);

	/* 找出这个 mux control 对应的全部 path，再按枚举值重建连通状态。 */
	dapm_kcontrol_for_each_path(path, kcontrol) {
		found = 1;
		/* 现在需要把枚举字符串和路径名匹配起来。 */
		if (e && !(strcmp(path->name, e->texts[mux])))
			connect = true;
		else
			connect = false;

		dapm_connect_path(path, connect, "mux update");
	}

	if (found)
		dapm_power_widgets(card, SND_SOC_DAPM_STREAM_NOP, update);

	return found;
}

int snd_soc_dapm_mux_update_power(struct snd_soc_dapm_context *dapm,
	struct snd_kcontrol *kcontrol, int mux, struct soc_enum *e,
	struct snd_soc_dapm_update *update)
{
	struct snd_soc_card *card = dapm->card;
	int ret;

	snd_soc_dapm_mutex_lock(card);
	ret = dapm_mux_update_power(card, kcontrol, update, mux, e);
	snd_soc_dapm_mutex_unlock(card);
	if (ret > 0)
		snd_soc_dpcm_runtime_update(card);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_mux_update_power);

/* 测试并更新 mixer 或 switch widget 的电源状态。 */
static int dapm_mixer_update_power(struct snd_soc_card *card,
				   struct snd_kcontrol *kcontrol,
				   struct snd_soc_dapm_update *update,
				   int connect, int rconnect)
{
	struct snd_soc_dapm_path *path;
	int found = 0;

	snd_soc_dapm_mutex_assert_held(card);

	/* mixer/switch 的 kcontrol 可能对应多条路径，逐条更新连通状态。 */
	dapm_kcontrol_for_each_path(path, kcontrol) {
		/*
		 * 理想情况下，这个函数应该支持任意数量的路径和通道。
		 * 但由于 kcontrol 只有 mono 和 stereo 两种形式，实际只限定为
		 * 2 个通道。
		 *
		 * 下面的代码假设 stereo control 中，第一条路径
		 *（found == 0）是左声道，其余路径（found == 1）是右声道。
		 *
		 * stereo control 通过有效的 rconnect 值来表示：0 代表未连接，
		 * 大于等于 0 代表已连接。
		 * 这里不使用 snd_soc_volsw_is_stereo，是为了即使传入的是 stereo
		 * kcontrol，也不改变 snd_soc_dapm_mixer_update_power 的行为。
		 *
		 * connect 作为左声道的路径连接状态，rconnect 作为右声道状态。
		 */
		if (found && rconnect >= 0)
			dapm_connect_path(path, rconnect, "mixer update");
		else
			dapm_connect_path(path, connect, "mixer update");
		found = 1;
	}

	if (found)
		dapm_power_widgets(card, SND_SOC_DAPM_STREAM_NOP, update);

	return found;
}

int snd_soc_dapm_mixer_update_power(struct snd_soc_dapm_context *dapm,
	struct snd_kcontrol *kcontrol, int connect,
	struct snd_soc_dapm_update *update)
{
	struct snd_soc_card *card = dapm->card;
	int ret;

	snd_soc_dapm_mutex_lock(card);
	ret = dapm_mixer_update_power(card, kcontrol, update, connect, -1);
	snd_soc_dapm_mutex_unlock(card);
	if (ret > 0)
		snd_soc_dpcm_runtime_update(card);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_mixer_update_power);

static ssize_t dapm_widget_show_component(struct snd_soc_component *component,
					  char *buf, int count)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	struct snd_soc_dapm_widget *w;
	char *state = "not set";

	/*
	 * dummy component 可能没有正常的 card 关联，sysfs 展示时要先兜底。
	 * 这里是把 component 下面的“耗电 widget”按当前 power 状态打印出来。
	 */
	if (!component->card)
		return 0;

	for_each_card_widgets(component->card, w) {
		if (w->dapm != dapm)
			continue;

		/* sysfs 只展示那些真的会影响功耗的 widget。 */
		switch (w->id) {
		case snd_soc_dapm_hp:
		case snd_soc_dapm_mic:
		case snd_soc_dapm_spk:
		case snd_soc_dapm_line:
		case snd_soc_dapm_micbias:
		case snd_soc_dapm_dac:
		case snd_soc_dapm_adc:
		case snd_soc_dapm_pga:
		case snd_soc_dapm_effect:
		case snd_soc_dapm_out_drv:
		case snd_soc_dapm_mixer:
		case snd_soc_dapm_mixer_named_ctl:
		case snd_soc_dapm_supply:
		case snd_soc_dapm_regulator_supply:
		case snd_soc_dapm_pinctrl:
		case snd_soc_dapm_clock_supply:
			if (w->name)
				count += sysfs_emit_at(buf, count, "%s: %s\n",
					w->name, w->power ? "On":"Off");
		break;
		default:
		break;
		}
	}

	switch (snd_soc_dapm_get_bias_level(dapm)) {
	case SND_SOC_BIAS_ON:
		state = "On";
		break;
	case SND_SOC_BIAS_PREPARE:
		state = "Prepare";
		break;
	case SND_SOC_BIAS_STANDBY:
		state = "Standby";
		break;
	case SND_SOC_BIAS_OFF:
		state = "Off";
		break;
	}
	count += sysfs_emit_at(buf, count, "PM State: %s\n", state);

	return count;
}

/* 在 sysfs 中显示 DAPM widget 状态。 */
static ssize_t dapm_widget_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct snd_soc_pcm_runtime *rtd = dev_get_drvdata(dev);
	struct snd_soc_dai *codec_dai;
	int i, count = 0;

	/* sysfs 入口：遍历 runtime 上的 codec DAI，把每个 component 的状态拼到缓冲区。 */
	snd_soc_dapm_mutex_lock_root(rtd->card);

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		struct snd_soc_component *component = codec_dai->component;

		count = dapm_widget_show_component(component, buf, count);
	}

	snd_soc_dapm_mutex_unlock(rtd->card);

	return count;
}

static DEVICE_ATTR_RO(dapm_widget);

struct attribute *snd_soc_dapm_dev_attrs[] = {
	&dev_attr_dapm_widget.attr,
	NULL
};

static void dapm_free_path(struct snd_soc_dapm_path *path)
{
	/* 一个 path 同时挂在输入、输出和 kcontrol 三个链上，释放时都要摘掉。 */
	list_del(&path->list_node[SND_SOC_DAPM_DIR_IN]);
	list_del(&path->list_node[SND_SOC_DAPM_DIR_OUT]);
	list_del(&path->list_kcontrol);
	list_del(&path->list);
	kfree(path);
}

/**
 * snd_soc_dapm_free_widget - 释放指定 widget
 * @w: 需要释放的 widget
 *
 * 从所有路径中移除该 widget，并释放其占用的内存。
 */
void snd_soc_dapm_free_widget(struct snd_soc_dapm_widget *w)
{
	struct snd_soc_dapm_path *p, *next_p;
	enum snd_soc_dapm_direction dir;

	if (!w)
		return;

	list_del(&w->list);
	list_del(&w->dirty);
	/*
	 * 释放 widget 前，必须把它挂住的所有 path 先拆掉。
	 * 这里同时从 source/sink 两侧解除引用，保证每条 path 只回收一次。
	 */
	dapm_for_each_direction(dir) {
		snd_soc_dapm_widget_for_each_path_safe(w, dir, p, next_p)
			dapm_free_path(p);
	}

	dapm_debugfs_free_widget(w);

	kfree(w->kcontrols);
	kfree_const(w->name);
	kfree_const(w->sname);
	kfree(w);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_free_widget);

/* 释放所有 DAPM widget 和资源。 */
static void dapm_free_widgets(struct snd_soc_dapm_context *dapm)
{
	struct snd_soc_dapm_widget *w, *next_w;

	/* 只清理当前 DAPM context 下的 widget，避免误伤其他 context。 */
	for_each_card_widgets_safe(dapm->card, w, next_w) {
		if (w->dapm != dapm)
			continue;
		snd_soc_dapm_free_widget(w);
	}

	dapm->wcache_sink	= NULL;
	dapm->wcache_source	= NULL;
}

static struct snd_soc_dapm_widget *dapm_find_widget(
			struct snd_soc_dapm_context *dapm, const char *pin,
			bool search_other_contexts)
{
	struct snd_soc_dapm_widget *w;
	struct snd_soc_dapm_widget *fallback = NULL;
	char prefixed_pin[80];
	const char *pin_name;
	const char *prefix = dapm_prefix(dapm);

	if (prefix) {
		snprintf(prefixed_pin, sizeof(prefixed_pin), "%s %s",
			 prefix, pin);
		pin_name = prefixed_pin;
	} else {
		pin_name = pin;
	}

	/* 先找本 context，找不到时才按需要去 card 里找同名 fallback。 */
	for_each_card_widgets(dapm->card, w) {
		if (!strcmp(w->name, pin_name)) {
			if (w->dapm == dapm)
				return w;
			else
				fallback = w;
		}
	}

	if (search_other_contexts)
		return fallback;

	return NULL;
}

/*
 * 设置 DAPM pin 状态：
 * 值发生变化时返回 1，未变化时返回 0，出错时返回负错误码；
 * 由 kcontrol 的 put 回调调用。
 */
static int __dapm_set_pin(struct snd_soc_dapm_context *dapm,
			  const char *pin, int status)
{
	struct snd_soc_dapm_widget *w = dapm_find_widget(dapm, pin, true);
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	int ret = 0;

	dapm_assert_locked(dapm);

	if (!w) {
		dev_err(dev, "ASoC: DAPM unknown pin %s\n", pin);
		return -EINVAL;
	}

	if (w->connected != status) {
		/* pin 状态变化会影响整条路由图，所以先把相关缓存全部打脏。 */
		dapm_mark_dirty(w, "pin configuration");
		dapm_widget_invalidate_input_paths(w);
		dapm_widget_invalidate_output_paths(w);
		ret = 1;
	}

	w->connected = status;
	if (status == 0)
		w->force = 0;

	return ret;
}

/*
 * 与 __dapm_set_pin() 类似，但成功时返回 0；
 * 下方多个 API 函数都会调用它。
 */
static int dapm_set_pin(struct snd_soc_dapm_context *dapm,
				const char *pin, int status)
{
	int ret = __dapm_set_pin(dapm, pin, status);

	/* 对外 API 只关心成功与否，0/1 的差异在内部 already handled. */
	return ret < 0 ? ret : 0;
}

/**
 * snd_soc_dapm_sync_unlocked - 扫描并驱动 DAPM 路径上下电
 * @dapm: DAPM 上下文
 *
 * 遍历所有 DAPM 音频路径，并根据它们的 stream 或 path 使用情况
 * 对 widget 上下电。
 *
 * 需要外部锁保护。
 *
 * 成功时返回 0。
 */
int snd_soc_dapm_sync_unlocked(struct snd_soc_dapm_context *dapm)
{
	/*
	 * card 还没正式 instantiated 前，不要提前跑 DAPM。
	 * 这能避免 jack / pin / debugfs 这些早期状态变化在 probe 阶段
	 * 触发无意义的图计算。
	 */
	if (!snd_soc_card_is_instantiated(dapm->card))
		return 0;

	return dapm_power_widgets(dapm->card, SND_SOC_DAPM_STREAM_NOP, NULL);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_sync_unlocked);

/**
 * snd_soc_dapm_sync - 扫描并驱动 DAPM 路径上下电
 * @dapm: DAPM 上下文
 *
 * 遍历所有 DAPM 音频路径，并根据它们的 stream 或 path 使用情况
 * 对 widget 上下电。
 *
 * 成功时返回 0。
 */
int snd_soc_dapm_sync(struct snd_soc_dapm_context *dapm)
{
	int ret;

	snd_soc_dapm_mutex_lock(dapm);
	ret = snd_soc_dapm_sync_unlocked(dapm);
	snd_soc_dapm_mutex_unlock(dapm);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_sync);

static int dapm_update_dai_chan(struct snd_soc_dapm_path *p,
				struct snd_soc_dapm_widget *w,
				int channels)
{
	struct device *dev = snd_soc_dapm_to_dev(w->dapm);

	switch (w->id) {
	case snd_soc_dapm_aif_out:
	case snd_soc_dapm_aif_in:
		break;
	default:
		return 0;
	}

	dev_dbg(dev, "%s DAI route %s -> %s\n",
		w->channel < channels ? "Connecting" : "Disconnecting",
		p->source->name, p->sink->name);

	if (w->channel < channels)
		dapm_connect_path(p, true, "dai update");
	else
		dapm_connect_path(p, false, "dai update");

	return 0;
}

static int dapm_update_dai_unlocked(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	int dir = substream->stream;
	int channels = params_channels(params);
	struct snd_soc_dapm_path *p;
	struct snd_soc_dapm_widget *w;
	int ret;

	w = snd_soc_dai_get_widget(dai, dir);

	if (!w)
		return 0;

	/*
	 * DAI route update 是“按当前 hw_params 重新裁剪路径”，不是普通 pin
	 * 设定。这里会根据声道数决定哪些 AIF 节点需要保持连通。
	 */
	dev_dbg(dai->dev, "Update DAI routes for %s %s\n", dai->name, snd_pcm_direction_name(dir));

	snd_soc_dapm_widget_for_each_sink_path(w, p) {
		ret = dapm_update_dai_chan(p, p->sink, channels);
		if (ret < 0)
			return ret;
	}

	snd_soc_dapm_widget_for_each_source_path(w, p) {
		ret = dapm_update_dai_chan(p, p->source, channels);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int snd_soc_dapm_update_dai(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	int ret;

	snd_soc_dapm_mutex_lock(rtd->card);
	ret = dapm_update_dai_unlocked(substream, params, dai);
	snd_soc_dapm_mutex_unlock(rtd->card);

	return ret;
}

int snd_soc_dapm_widget_name_cmp(struct snd_soc_dapm_widget *widget, const char *s)
{
	struct snd_soc_component *component = widget->dapm->component;
	const char *wname = widget->name;

	/* 比较时忽略 component name_prefix，方便按裸 widget 名做查找。 */
	if (component && component->name_prefix)
		wname += strlen(component->name_prefix) + 1; /* plus space */

	return strcmp(wname, s);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_widget_name_cmp);

static int snd_soc_dapm_add_route(struct snd_soc_dapm_context *dapm,
				  const struct snd_soc_dapm_route *route)
{
	struct snd_soc_dapm_widget *wsource = NULL, *wsink = NULL, *w;
	struct snd_soc_dapm_widget *wtsource = NULL, *wtsink = NULL;
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	const char *sink;
	const char *source;
	char prefixed_sink[80];
	char prefixed_source[80];
	const char *prefix;
	unsigned int sink_ref = 0;
	unsigned int source_ref = 0;
	int ret;

	prefix = dapm_prefix(dapm);
	if (prefix) {
		snprintf(prefixed_sink, sizeof(prefixed_sink), "%s %s",
			 prefix, route->sink);
		sink = prefixed_sink;
		snprintf(prefixed_source, sizeof(prefixed_source), "%s %s",
			 prefix, route->source);
		source = prefixed_source;
	} else {
		sink = route->sink;
		source = route->source;
	}

	wsource	= dapm_wcache_lookup(dapm->wcache_source, source);
	wsink	= dapm_wcache_lookup(dapm->wcache_sink,   sink);

	/*
	 * 先走短缓存，再全卡扫描。DAPM 的 route 常常成批插入，
	 * 这种“最近一次命中的 source/sink”缓存能显著减少重复查找。
	 */
	if (wsink && wsource)
		goto skip_search;

	/*
	 * find src and dest widgets over all widgets but favor a widget from
	 * current DAPM context
	 */
	for_each_card_widgets(dapm->card, w) {
		if (!wsink && !(strcmp(w->name, sink))) {
			wtsink = w;
			if (w->dapm == dapm) {
				wsink = w;
				if (wsource)
					break;
			}
			sink_ref++;
			if (sink_ref > 1)
				dev_warn(dev,
					"ASoC: sink widget %s overwritten\n",
					w->name);
			continue;
		}
		if (!wsource && !(strcmp(w->name, source))) {
			wtsource = w;
			if (w->dapm == dapm) {
				wsource = w;
				if (wsink)
					break;
			}
			source_ref++;
			if (source_ref > 1)
				dev_warn(dev,
					"ASoC: source widget %s overwritten\n",
					w->name);
		}
	}
	/* 如果本 context 中找不到，就复用另一个 DAPM context 的 widget。 */
	if (!wsink)
		wsink = wtsink;
	if (!wsource)
		wsource = wtsource;

	ret = -ENODEV;
	if (!wsource)
		goto err;
	if (!wsink)
		goto err;

skip_search:
	/* 更新缓存。 */
	dapm->wcache_sink	= wsink;
	dapm->wcache_source	= wsource;

	/* route 解析到 widget 后，真正把边插入图里的是 dapm_add_path(). */
	ret = dapm_add_path(dapm, wsource, wsink, route->control,
		route->connected);
err:
	if (ret)
		dev_err(dev, "ASoC: Failed to add route %s%s -%s%s%s> %s%s\n",
			source, !wsource ? "(*)" : "",
			!route->control ? "" : "> [",
			!route->control ? "" : route->control,
			!route->control ? "" : "] -",
			sink,  !wsink ? "(*)" : "");
	return ret;
}

static int snd_soc_dapm_del_route(struct snd_soc_dapm_context *dapm,
				  const struct snd_soc_dapm_route *route)
{
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	struct snd_soc_dapm_path *path, *p;
	const char *sink;
	const char *source;
	char prefixed_sink[80];
	char prefixed_source[80];
	const char *prefix;

	if (route->control) {
		dev_err(dev,
			"ASoC: Removal of routes with controls not supported\n");
		return -EINVAL;
	}

	prefix = dapm_prefix(dapm);
	if (prefix) {
		snprintf(prefixed_sink, sizeof(prefixed_sink), "%s %s",
			 prefix, route->sink);
		sink = prefixed_sink;
		snprintf(prefixed_source, sizeof(prefixed_source), "%s %s",
			 prefix, route->source);
		source = prefixed_source;
	} else {
		sink = route->sink;
		source = route->source;
	}

	path = NULL;
	list_for_each_entry(p, &dapm->card->paths, list) {
		if (strcmp(p->source->name, source) != 0)
			continue;
		if (strcmp(p->sink->name, sink) != 0)
			continue;
		path = p;
		break;
	}

	if (path) {
		struct snd_soc_dapm_widget *wsource = path->source;
		struct snd_soc_dapm_widget *wsink = path->sink;

		/* 删除 route 时，先把两端节点和路径本身都打脏，再做回收。 */
		dapm_mark_dirty(wsource, "Route removed");
		dapm_mark_dirty(wsink, "Route removed");
		if (path->connect)
			dapm_path_invalidate(path);

		dapm_free_path(path);

		/* Update any path related flags */
		dapm_update_widget_flags(wsource);
		dapm_update_widget_flags(wsink);
	} else {
		dev_warn(dev, "ASoC: Route %s->%s does not exist\n",
			 source, sink);
	}

	return 0;
}

/**
 * snd_soc_dapm_add_routes - 添加 DAPM widget 之间的路径
 * @dapm: DAPM 上下文
 * @route: 音频路径数组
 * @num: 路径数量
 *
 * 通过命名的音频路径把两个 DAPM widget 连接起来。sink 是接收音频信号的
 * widget，source 是发送音频信号的 widget。
 *
 * 返回：成功时返回 0，失败时返回错误码。出错后资源可通过
 * snd_soc_card_free() 统一释放。
 */
int snd_soc_dapm_add_routes(struct snd_soc_dapm_context *dapm,
			    const struct snd_soc_dapm_route *route, int num)
{
	int i, ret = 0;

	/* route 通常批量加载，逐条处理但保留第一个错误返回。 */
	snd_soc_dapm_mutex_lock(dapm);
	for (i = 0; i < num; i++) {
		int r = snd_soc_dapm_add_route(dapm, route);
		if (r < 0)
			ret = r;
		route++;
	}
	snd_soc_dapm_mutex_unlock(dapm);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_add_routes);

/**
 * snd_soc_dapm_del_routes - 删除 DAPM widget 之间的路径
 * @dapm: DAPM 上下文
 * @route: 音频路径数组
 * @num: 路径数量
 *
 * 从 DAPM 上下文中移除路径。
 */
int snd_soc_dapm_del_routes(struct snd_soc_dapm_context *dapm,
			    const struct snd_soc_dapm_route *route, int num)
{
	int i;

	snd_soc_dapm_mutex_lock(dapm);
	for (i = 0; i < num; i++) {
		snd_soc_dapm_del_route(dapm, route);
		route++;
	}
	snd_soc_dapm_mutex_unlock(dapm);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_del_routes);

/**
 * snd_soc_dapm_new_widgets - 添加新的 DAPM widget
 * @card: 需要检查新增 DAPM widget 的 card
 *
 * 检查 card 里是否存在新的 DAPM widget，如果有则创建。
 *
 * 返回：成功时返回 0。
 */
int snd_soc_dapm_new_widgets(struct snd_soc_card *card)
{
	struct snd_soc_dapm_widget *w;
	unsigned int val;

	snd_soc_dapm_mutex_lock_root(card);

	/*
	 * 新 widget 创建完后并不立刻重算整张图，而是先统一建对象、
	 * 标脏、挂 debugfs，最后只跑一次 DAPM power walk。
	 */
	for_each_card_widgets(card, w)
	{
		if (w->new)
			continue;

		if (w->num_kcontrols) {
			w->kcontrols = kzalloc_objs(struct snd_kcontrol *,
						    w->num_kcontrols);
			if (!w->kcontrols) {
				snd_soc_dapm_mutex_unlock(card);
				return -ENOMEM;
			}
		}

		switch(w->id) {
		case snd_soc_dapm_switch:
		case snd_soc_dapm_mixer:
		case snd_soc_dapm_mixer_named_ctl:
			dapm_new_mixer(w);
			break;
		case snd_soc_dapm_mux:
		case snd_soc_dapm_mux_named_ctl:
		case snd_soc_dapm_demux:
			dapm_new_mux(w);
			break;
		case snd_soc_dapm_pga:
		case snd_soc_dapm_effect:
		case snd_soc_dapm_out_drv:
			dapm_new_pga(w);
			break;
		case snd_soc_dapm_dai_link:
			dapm_new_dai_link(w);
			break;
		default:
			break;
		}

		/* 从设备读取初始电源状态。 */
		if (w->reg >= 0) {
			val = dapm_read(w->dapm, w->reg);
			val = val >> w->shift;
			val &= w->mask;
			if (val == w->on_val)
				w->power = 1;
		}

		w->new = 1;

		dapm_mark_dirty(w, "new widget");
		dapm_debugfs_add_widget(w);
	}

	dapm_power_widgets(card, SND_SOC_DAPM_STREAM_NOP, NULL);
	snd_soc_dapm_mutex_unlock(card);
	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_new_widgets);

/**
 * snd_soc_dapm_get_volsw - DAPM mixer 的 get 回调
 * @kcontrol: mixer control
 * @ucontrol: control 元素信息
 *
 * 读取 DAPM mixer control 的当前值。
 *
 * 返回：成功时返回 0。
 */
int snd_soc_dapm_get_volsw(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int reg = mc->reg;
	unsigned int shift = mc->shift;
	int max = mc->max;
	unsigned int width = fls(max);
	unsigned int mask = (1 << fls(max)) - 1;
	unsigned int invert = mc->invert;
	unsigned int reg_val, val, rval = 0;

	/*
	 * 读取 volume/switch 时有两条路径：
	 * - widget 已供电：直接读硬件寄存器，得到真实状态
	 * - widget 未供电：读 DAPM 缓存值，避免把关机态器件强行唤醒
	 */
	snd_soc_dapm_mutex_lock(dapm);
	if (dapm_kcontrol_is_powered(kcontrol) && reg != SND_SOC_NOPM) {
		reg_val = dapm_read(dapm, reg);
		val = (reg_val >> shift) & mask;

		if (reg != mc->rreg)
			reg_val = dapm_read(dapm, mc->rreg);

		if (snd_soc_volsw_is_stereo(mc))
			rval = (reg_val >> mc->rshift) & mask;
	} else {
		reg_val = snd_soc_dapm_kcontrol_get_value(kcontrol);
		val = reg_val & mask;

		if (snd_soc_volsw_is_stereo(mc))
			rval = (reg_val >> width) & mask;
	}
	snd_soc_dapm_mutex_unlock(dapm);

	if (invert)
		ucontrol->value.integer.value[0] = max - val;
	else
		ucontrol->value.integer.value[0] = val;

	if (snd_soc_volsw_is_stereo(mc)) {
		if (invert)
			ucontrol->value.integer.value[1] = max - rval;
		else
			ucontrol->value.integer.value[1] = rval;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_volsw);

/**
 * snd_soc_dapm_put_volsw - DAPM mixer 的 set 回调
 * @kcontrol: mixer control
 * @ucontrol: control 元素信息
 *
 * 设置 DAPM mixer control 的值。
 *
 * 返回：成功时返回 0。
 */
int snd_soc_dapm_put_volsw(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	struct snd_soc_card *card = dapm->card;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int reg = mc->reg;
	unsigned int shift = mc->shift;
	int max = mc->max;
	unsigned int width = fls(max);
	unsigned int mask = (1 << width) - 1;
	unsigned int invert = mc->invert;
	unsigned int val, rval = 0;
	int connect, rconnect = -1, change, reg_change = 0;
	struct snd_soc_dapm_update update = {};
	struct snd_soc_dapm_update *pupdate = NULL;
	int ret = 0;

	/*
	 * set 回调要同时处理三件事：
	 * 1) 更新 DAPM 侧缓存值
	 * 2) 判断硬件寄存器是否真的需要写
	 * 3) 如果连通性变化了，要重新跑 DAPM power walk
	 */
	val = (ucontrol->value.integer.value[0] & mask);
	connect = !!val;

	if (invert)
		val = max - val;

	if (snd_soc_volsw_is_stereo(mc)) {
		rval = (ucontrol->value.integer.value[1] & mask);
		rconnect = !!rval;
		if (invert)
			rval = max - rval;
	}

	snd_soc_dapm_mutex_lock(card);

	/* This assumes field width < (bits in unsigned int / 2) */
	if (width > sizeof(unsigned int) * 8 / 2)
		dev_warn(dev,
			 "ASoC: control %s field width limit exceeded\n",
			 kcontrol->id.name);
	change = dapm_kcontrol_set_value(kcontrol, val | (rval << width));

	if (reg != SND_SOC_NOPM) {
		val = val << shift;
		rval = rval << mc->rshift;

		reg_change = dapm_test_bits(dapm, reg, mask << shift, val);

		if (snd_soc_volsw_is_stereo(mc))
			reg_change |= dapm_test_bits(dapm, mc->rreg,
						     mask << mc->rshift,
						     rval);
	}

	if (change || reg_change) {
		if (reg_change) {
			if (snd_soc_volsw_is_stereo(mc)) {
				update.has_second_set = true;
				update.reg2 = mc->rreg;
				update.mask2 = mask << mc->rshift;
				update.val2 = rval;
			}
			update.kcontrol = kcontrol;
			update.reg = reg;
			update.mask = mask << shift;
			update.val = val;
			pupdate = &update;
		}
		ret = dapm_mixer_update_power(card, kcontrol, pupdate, connect, rconnect);
	}

	snd_soc_dapm_mutex_unlock(card);

	if (ret > 0)
		snd_soc_dpcm_runtime_update(card);

	return change;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_put_volsw);

/**
 * snd_soc_dapm_get_enum_double - DAPM 枚举型双通道 mixer 的 get 回调
 * @kcontrol: mixer control
 * @ucontrol: control 元素信息
 *
 * 读取 DAPM 枚举型双通道 mixer control 的当前值。
 *
 * 返回：成功时返回 0。
 */
int snd_soc_dapm_get_enum_double(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int reg_val, val;

	/* 枚举控件同样分“读硬件”与“读缓存”两条路。 */
	snd_soc_dapm_mutex_lock(dapm);
	if (e->reg != SND_SOC_NOPM && dapm_kcontrol_is_powered(kcontrol)) {
		reg_val = dapm_read(dapm, e->reg);
	} else {
		reg_val = snd_soc_dapm_kcontrol_get_value(kcontrol);
	}
	snd_soc_dapm_mutex_unlock(dapm);

	val = (reg_val >> e->shift_l) & e->mask;
	ucontrol->value.enumerated.item[0] = snd_soc_enum_val_to_item(e, val);
	if (e->shift_l != e->shift_r) {
		val = (reg_val >> e->shift_r) & e->mask;
		val = snd_soc_enum_val_to_item(e, val);
		ucontrol->value.enumerated.item[1] = val;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_enum_double);

/**
 * snd_soc_dapm_put_enum_double - DAPM 枚举型双通道 mixer 的 set 回调
 * @kcontrol: mixer control
 * @ucontrol: control 元素信息
 *
 * 设置 DAPM 枚举型双通道 mixer control 的值。
 *
 * 返回：成功时返回 0。
 */
int snd_soc_dapm_put_enum_double(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_card *card = dapm->card;
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	unsigned int *item = ucontrol->value.enumerated.item;
	unsigned int val, change, reg_change = 0;
	unsigned int mask;
	struct snd_soc_dapm_update update = {};
	struct snd_soc_dapm_update *pupdate = NULL;
	int ret = 0;

	/*
	 * 枚举类型的 put 先把 item 转成寄存器值，再按结果决定：
	 * - 只改图上连通性
	 * - 或者同时更新寄存器和 DAPM 图
	 */
	if (item[0] >= e->items)
		return -EINVAL;

	val = snd_soc_enum_item_to_val(e, item[0]) << e->shift_l;
	mask = e->mask << e->shift_l;
	if (e->shift_l != e->shift_r) {
		if (item[1] > e->items)
			return -EINVAL;
		val |= snd_soc_enum_item_to_val(e, item[1]) << e->shift_r;
		mask |= e->mask << e->shift_r;
	}

	snd_soc_dapm_mutex_lock(card);

	change = dapm_kcontrol_set_value(kcontrol, val);

	if (e->reg != SND_SOC_NOPM)
		reg_change = dapm_test_bits(dapm, e->reg, mask, val);

	if (change || reg_change) {
		if (reg_change) {
			update.kcontrol = kcontrol;
			update.reg = e->reg;
			update.mask = mask;
			update.val = val;
			pupdate = &update;
		}
		ret = dapm_mux_update_power(card, kcontrol, pupdate, item[0], e);
	}

	snd_soc_dapm_mutex_unlock(card);

	if (ret > 0)
		snd_soc_dpcm_runtime_update(card);

	return change;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_put_enum_double);

/**
 * snd_soc_dapm_info_pin_switch - pin switch 的信息回调
 *
 * @kcontrol: mixer control
 * @uinfo: control 元素信息
 *
 * 提供 pin switch control 的类型和取值范围信息。
 */
int snd_soc_dapm_info_pin_switch(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_info *uinfo)
{
	/* pin switch 在用户态看来就是一个标准 boolean 开关。 */
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_info_pin_switch);

static int __snd_soc_dapm_get_pin_switch(struct snd_soc_dapm_context *dapm,
					 const char *pin,
					 struct snd_ctl_elem_value *ucontrol)
{
	/* 这个 helper 统一做锁保护下的 pin 状态读取。 */
	snd_soc_dapm_mutex_lock(dapm);
	ucontrol->value.integer.value[0] = snd_soc_dapm_get_pin_status(dapm, pin);
	snd_soc_dapm_mutex_unlock(dapm);

	return 0;
}

/**
 * snd_soc_dapm_get_pin_switch - 获取 card 级 pin switch 的状态
 *
 * @kcontrol: mixer control
 * @ucontrol: Value
 *
 * 读取 card 级 pin switch 的当前状态。
 */
int snd_soc_dapm_get_pin_switch(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	const char *pin = (const char *)kcontrol->private_value;

	/* card 级 pin switch：先从 card 找到 top-level DAPM context。 */
	return __snd_soc_dapm_get_pin_switch(dapm, pin, ucontrol);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_pin_switch);

/**
 * snd_soc_dapm_get_component_pin_switch - 获取 component 级 pin switch 的状态
 *
 * @kcontrol: mixer control
 * @ucontrol: Value
 *
 * 读取 component 级 pin switch 的当前状态。
 */
int snd_soc_dapm_get_component_pin_switch(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	const char *pin = (const char *)kcontrol->private_value;

	/* component 级 pin switch：直接在 component 自己的 DAPM 上下文里查。 */
	return __snd_soc_dapm_get_pin_switch(dapm, pin, ucontrol);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_component_pin_switch);

static int __dapm_put_pin_switch(struct snd_soc_dapm_context *dapm,
				 const char *pin,
				 struct snd_ctl_elem_value *ucontrol)
{
	int ret;

	/*
	 * pin 状态切换后必须立即做一次 sync，让图上所有 endpoint
	 * 重新计算，不能只停留在 connected 标志位上。
	 */
	snd_soc_dapm_mutex_lock(dapm);
	ret = __dapm_set_pin(dapm, pin, !!ucontrol->value.integer.value[0]);
	snd_soc_dapm_mutex_unlock(dapm);

	snd_soc_dapm_sync(dapm);

	return ret;
}

/**
 * snd_soc_dapm_put_pin_switch - 设置 card 级 pin switch
 *
 * @kcontrol: mixer control
 * @ucontrol: Value
 *
 * 写入 card 级 pin switch 的状态。
 */
int snd_soc_dapm_put_pin_switch(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	const char *pin = (const char *)kcontrol->private_value;

	/* card 级 pin switch 的写入路径。 */
	return __dapm_put_pin_switch(dapm, pin, ucontrol);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_put_pin_switch);

/**
 * snd_soc_dapm_put_component_pin_switch - 设置 component 级 pin switch
 *
 * @kcontrol: mixer control
 * @ucontrol: Value
 *
 * 写入 component 级 pin switch 的状态。
 */
int snd_soc_dapm_put_component_pin_switch(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	const char *pin = (const char *)kcontrol->private_value;

	/* component 级 pin switch 的写入路径。 */
	return __dapm_put_pin_switch(dapm, pin, ucontrol);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_put_component_pin_switch);

struct snd_soc_dapm_widget *
snd_soc_dapm_new_control_unlocked(struct snd_soc_dapm_context *dapm,
			 const struct snd_soc_dapm_widget *widget)
{
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	enum snd_soc_dapm_direction dir;
	struct snd_soc_dapm_widget *w;
	int ret = -ENOMEM;

	/*
	 * 这里把模板 widget 复制成真正的运行时 widget，并按 widget 类型
	 * 绑定 regulator/pinctrl/clock/endpoint 语义。
	 */
	w = dapm_cnew_widget(widget, dapm_prefix(dapm));
	if (!w)
		goto cnew_failed;

	switch (w->id) {
	case snd_soc_dapm_regulator_supply:
		w->regulator = devm_regulator_get(dev, widget->name);
		if (IS_ERR(w->regulator)) {
			ret = PTR_ERR(w->regulator);
			goto request_failed;
		}

		if (w->on_val & SND_SOC_DAPM_REGULATOR_BYPASS) {
			ret = regulator_allow_bypass(w->regulator, true);
			if (ret != 0)
				dev_warn(dev,
					 "ASoC: Failed to bypass %s: %d\n",
					 w->name, ret);
		}
		break;
	case snd_soc_dapm_pinctrl:
		w->pinctrl = devm_pinctrl_get(dev);
		if (IS_ERR(w->pinctrl)) {
			ret = PTR_ERR(w->pinctrl);
			goto request_failed;
		}

		/* set to sleep_state when initializing */
		snd_soc_dapm_pinctrl_event(w, NULL, SND_SOC_DAPM_POST_PMD);
		break;
	case snd_soc_dapm_clock_supply:
		w->clk = devm_clk_get(dev, widget->name);
		if (IS_ERR(w->clk)) {
			ret = PTR_ERR(w->clk);
			goto request_failed;
		}
		break;
	default:
		break;
	}

	switch (w->id) {
	case snd_soc_dapm_mic:
		w->is_ep = SND_SOC_DAPM_EP_SOURCE;
		w->power_check = dapm_generic_check_power;
		break;
	case snd_soc_dapm_input:
		if (!dapm->card->fully_routed)
			w->is_ep = SND_SOC_DAPM_EP_SOURCE;
		w->power_check = dapm_generic_check_power;
		break;
	case snd_soc_dapm_spk:
	case snd_soc_dapm_hp:
		w->is_ep = SND_SOC_DAPM_EP_SINK;
		w->power_check = dapm_generic_check_power;
		break;
	case snd_soc_dapm_output:
		if (!dapm->card->fully_routed)
			w->is_ep = SND_SOC_DAPM_EP_SINK;
		w->power_check = dapm_generic_check_power;
		break;
	case snd_soc_dapm_vmid:
	case snd_soc_dapm_siggen:
		w->is_ep = SND_SOC_DAPM_EP_SOURCE;
		w->power_check = dapm_always_on_check_power;
		break;
	case snd_soc_dapm_sink:
		w->is_ep = SND_SOC_DAPM_EP_SINK;
		w->power_check = dapm_always_on_check_power;
		break;

	case snd_soc_dapm_mux:
	case snd_soc_dapm_mux_named_ctl:
	case snd_soc_dapm_demux:
	case snd_soc_dapm_switch:
	case snd_soc_dapm_mixer:
	case snd_soc_dapm_mixer_named_ctl:
	case snd_soc_dapm_adc:
	case snd_soc_dapm_aif_out:
	case snd_soc_dapm_dac:
	case snd_soc_dapm_aif_in:
	case snd_soc_dapm_pga:
	case snd_soc_dapm_buffer:
	case snd_soc_dapm_scheduler:
	case snd_soc_dapm_effect:
	case snd_soc_dapm_src:
	case snd_soc_dapm_asrc:
	case snd_soc_dapm_encoder:
	case snd_soc_dapm_decoder:
	case snd_soc_dapm_out_drv:
	case snd_soc_dapm_micbias:
	case snd_soc_dapm_line:
	case snd_soc_dapm_dai_link:
	case snd_soc_dapm_dai_out:
	case snd_soc_dapm_dai_in:
		w->power_check = dapm_generic_check_power;
		break;
	case snd_soc_dapm_supply:
	case snd_soc_dapm_regulator_supply:
	case snd_soc_dapm_pinctrl:
	case snd_soc_dapm_clock_supply:
	case snd_soc_dapm_kcontrol:
		w->is_supply = 1;
		w->power_check = dapm_supply_check_power;
		break;
	default:
		w->power_check = dapm_always_on_check_power;
		break;
	}

	w->dapm = dapm;
	INIT_LIST_HEAD(&w->list);
	INIT_LIST_HEAD(&w->dirty);
	/* 供 for_each_card_widgets() 遍历使用。 */
	list_add_tail(&w->list, &dapm->card->widgets);

	dapm_for_each_direction(dir) {
		INIT_LIST_HEAD(&w->edges[dir]);
		w->endpoints[dir] = -1;
	}

	/* machine layer sets up unconnected pins and insertions */
	w->connected = 1;
	return w;

request_failed:
	dev_err_probe(dev, ret, "ASoC: Failed to request %s\n",
		      w->name);
	kfree_const(w->name);
	kfree_const(w->sname);
	kfree(w);
cnew_failed:
	return ERR_PTR(ret);
}

/**
 * snd_soc_dapm_new_control - 创建新的 DAPM control
 * @dapm: DAPM 上下文
 * @widget: widget 模板
 *
 * 根据模板创建一个新的 DAPM control。
 *
 * 返回：成功时返回 widget 指针，失败时返回错误指针。
 */
struct snd_soc_dapm_widget *
snd_soc_dapm_new_control(struct snd_soc_dapm_context *dapm,
			 const struct snd_soc_dapm_widget *widget)
{
	struct snd_soc_dapm_widget *w;

	snd_soc_dapm_mutex_lock(dapm);
	w = snd_soc_dapm_new_control_unlocked(dapm, widget);
	snd_soc_dapm_mutex_unlock(dapm);

	return w;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_new_control);

/**
 * snd_soc_dapm_new_controls - 创建一组新的 DAPM control
 * @dapm: DAPM 上下文
 * @widget: widget 数组
 * @num: widget 数量
 *
 * 根据模板批量创建 DAPM control。
 *
 * 返回：成功时返回 0，否则返回错误码。
 */
int snd_soc_dapm_new_controls(struct snd_soc_dapm_context *dapm,
	const struct snd_soc_dapm_widget *widget,
	unsigned int num)
{
	int i;
	int ret = 0;

	snd_soc_dapm_mutex_lock_root(dapm);
	for (i = 0; i < num; i++) {
		struct snd_soc_dapm_widget *w = snd_soc_dapm_new_control_unlocked(dapm, widget);
		if (IS_ERR(w)) {
			ret = PTR_ERR(w);
			break;
		}
		widget++;
	}
	snd_soc_dapm_mutex_unlock(dapm);
	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_new_controls);

static int dapm_dai_link_event_pre_pmu(struct snd_soc_dapm_widget *w,
				       struct snd_pcm_substream *substream)
{
	struct device *dev = snd_soc_dapm_to_dev(w->dapm);
	struct snd_soc_dapm_path *path;
	struct snd_soc_dai *source, *sink;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	const struct snd_soc_pcm_stream *config = NULL;
	struct snd_pcm_runtime *runtime = NULL;
	unsigned int fmt;
	int ret;

	/*
	 * 注意：
	 *
	 * snd_pcm_hw_params 在 arm64 上已经很大（608 字节），如果再叠加
	 * KASAN 一类会增加栈占用的配置，把它放在栈上分配已经有点过量了。
	 * 所以这个函数里用 kzalloc()/kfree() 来保存 params。
	 */
	struct snd_pcm_hw_params *params __free(kfree) = kzalloc_obj(*params);
	if (!params)
		return -ENOMEM;

	runtime = kzalloc_obj(*runtime);
	if (!runtime)
		return -ENOMEM;

	substream->runtime = runtime;

	substream->stream = SNDRV_PCM_STREAM_CAPTURE;
	snd_soc_dapm_widget_for_each_source_path(w, path) {
		source = path->source->priv;

		ret = snd_soc_dai_startup(source, substream);
		if (ret < 0)
			return ret;

		snd_soc_dai_activate(source, substream->stream);
	}

	substream->stream = SNDRV_PCM_STREAM_PLAYBACK;
	snd_soc_dapm_widget_for_each_sink_path(w, path) {
		sink = path->sink->priv;

		ret = snd_soc_dai_startup(sink, substream);
		if (ret < 0)
			return ret;

		snd_soc_dai_activate(sink, substream->stream);
	}

	substream->hw_opened = 1;

	/*
	 * Note: getting the config after .startup() gives a chance to
	 * either party on the link to alter the configuration if
	 * necessary
	 */
	config = rtd->dai_link->c2c_params + rtd->c2c_params_select;
	if (!config) {
		dev_err(dev, "ASoC: link config missing\n");
		return -EINVAL;
	}

	/* 这里要稍微小心，避免 mask 数组溢出。 */
	if (!config->formats) {
		dev_warn(dev, "ASoC: Invalid format was specified\n");

		return -EINVAL;
	}

	fmt = ffs(config->formats) - 1;

	snd_mask_set(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT), fmt);
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE)->min =
		config->rate_min;
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE)->max =
		config->rate_max;
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS)->min
		= config->channels_min;
	hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS)->max
		= config->channels_max;

	substream->stream = SNDRV_PCM_STREAM_CAPTURE;
	snd_soc_dapm_widget_for_each_source_path(w, path) {
		source = path->source->priv;

		ret = snd_soc_dai_hw_params(source, substream, params);
		if (ret < 0)
			return ret;

		dapm_update_dai_unlocked(substream, params, source);
	}

	substream->stream = SNDRV_PCM_STREAM_PLAYBACK;
	snd_soc_dapm_widget_for_each_sink_path(w, path) {
		sink = path->sink->priv;

		ret = snd_soc_dai_hw_params(sink, substream, params);
		if (ret < 0)
			return ret;

		dapm_update_dai_unlocked(substream, params, sink);
	}

	runtime->format = params_format(params);
	runtime->subformat = params_subformat(params);
	runtime->channels = params_channels(params);
	runtime->rate = params_rate(params);

	return 0;
}

static int dapm_dai_link_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_dapm_path *path;
	struct snd_soc_dai *source, *sink;
	struct snd_pcm_substream *substream = w->priv;
	int ret = 0, saved_stream = substream->stream;

	if (WARN_ON(list_empty(&w->edges[SND_SOC_DAPM_DIR_OUT]) ||
		    list_empty(&w->edges[SND_SOC_DAPM_DIR_IN])))
		return -EINVAL;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = dapm_dai_link_event_pre_pmu(w, substream);
		if (ret < 0)
			goto out;

		break;

	case SND_SOC_DAPM_POST_PMU:
		snd_soc_dapm_widget_for_each_source_path(w, path) {
			source = path->source->priv;

			snd_soc_dai_prepare(source, substream);
		}

		snd_soc_dapm_widget_for_each_sink_path(w, path) {
			sink = path->sink->priv;

			snd_soc_dai_prepare(sink, substream);
		}

		snd_soc_dapm_widget_for_each_sink_path(w, path) {
			sink = path->sink->priv;

			snd_soc_dai_digital_mute(sink, 0, SNDRV_PCM_STREAM_PLAYBACK);
			ret = 0;
		}
		break;

	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_dapm_widget_for_each_sink_path(w, path) {
			sink = path->sink->priv;

			snd_soc_dai_digital_mute(sink, 1, SNDRV_PCM_STREAM_PLAYBACK);
			ret = 0;
		}

		substream->stream = SNDRV_PCM_STREAM_CAPTURE;
		snd_soc_dapm_widget_for_each_source_path(w, path) {
			source = path->source->priv;
			snd_soc_dai_hw_free(source, substream, 0);
		}

		substream->stream = SNDRV_PCM_STREAM_PLAYBACK;
		snd_soc_dapm_widget_for_each_sink_path(w, path) {
			sink = path->sink->priv;
			snd_soc_dai_hw_free(sink, substream, 0);
		}

		substream->stream = SNDRV_PCM_STREAM_CAPTURE;
		snd_soc_dapm_widget_for_each_source_path(w, path) {
			source = path->source->priv;
			snd_soc_dai_deactivate(source, substream->stream);
			snd_soc_dai_shutdown(source, substream, 0);
		}

		substream->stream = SNDRV_PCM_STREAM_PLAYBACK;
		snd_soc_dapm_widget_for_each_sink_path(w, path) {
			sink = path->sink->priv;
			snd_soc_dai_deactivate(sink, substream->stream);
			snd_soc_dai_shutdown(sink, substream, 0);
		}
		break;

	case SND_SOC_DAPM_POST_PMD:
		kfree(substream->runtime);
		substream->runtime = NULL;
		break;

	default:
		WARN(1, "Unknown event %d\n", event);
		ret = -EINVAL;
	}

out:
	/* 恢复 substream 的方向。 */
	substream->stream = saved_stream;
	return ret;
}

static int dapm_dai_link_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *w = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd = w->priv;

	ucontrol->value.enumerated.item[0] = rtd->c2c_params_select;

	return 0;
}

static int dapm_dai_link_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *w = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd = w->priv;

	/* widget 已经上电时不能再修改配置。 */
	if (w->power)
		return -EBUSY;

	if (ucontrol->value.enumerated.item[0] == rtd->c2c_params_select)
		return 0;

	if (ucontrol->value.enumerated.item[0] >= rtd->dai_link->num_c2c_params)
		return -EINVAL;

	rtd->c2c_params_select = ucontrol->value.enumerated.item[0];

	return 1;
}

static void dapm_free_kcontrol(struct snd_soc_card *card,
			       unsigned long *private_value,
			       int num_c2c_params,
			       const char **w_param_text)
{
	int count;

	devm_kfree(card->dev, (void *)*private_value);

	if (!w_param_text)
		return;

	for (count = 0 ; count < num_c2c_params; count++)
		devm_kfree(card->dev, (void *)w_param_text[count]);
	devm_kfree(card->dev, w_param_text);
}

static struct snd_kcontrol_new *
dapm_alloc_kcontrol(struct snd_soc_card *card,
			char *link_name,
			const struct snd_soc_pcm_stream *c2c_params,
			int num_c2c_params, const char **w_param_text,
			unsigned long *private_value)
{
	struct soc_enum w_param_enum[] = {
		SOC_ENUM_SINGLE(0, 0, 0, NULL),
	};
	struct snd_kcontrol_new kcontrol_dai_link[] = {
		SOC_ENUM_EXT(NULL, w_param_enum[0],
			     dapm_dai_link_get,
			     dapm_dai_link_put),
	};
	struct snd_kcontrol_new *kcontrol_news;
	const struct snd_soc_pcm_stream *config = c2c_params;
	int count;

	for (count = 0 ; count < num_c2c_params; count++) {
		if (!config->stream_name) {
			dev_warn(card->dev,
				"ASoC: anonymous config %d for dai link %s\n",
				count, link_name);
			w_param_text[count] =
				devm_kasprintf(card->dev, GFP_KERNEL,
					       "Anonymous Configuration %d",
					       count);
		} else {
			w_param_text[count] = devm_kmemdup(card->dev,
						config->stream_name,
						strlen(config->stream_name) + 1,
						GFP_KERNEL);
		}
		if (!w_param_text[count])
			goto outfree_w_param;
		config++;
	}

	w_param_enum[0].items = num_c2c_params;
	w_param_enum[0].texts = w_param_text;

	*private_value =
		(unsigned long) devm_kmemdup(card->dev,
			(void *)(kcontrol_dai_link[0].private_value),
			sizeof(struct soc_enum), GFP_KERNEL);
	if (!*private_value) {
		dev_err(card->dev, "ASoC: Failed to create control for %s widget\n",
			link_name);
		goto outfree_w_param;
	}
	kcontrol_dai_link[0].private_value = *private_value;
	/* 在堆上复制一份 kcontrol_dai_link，确保内存能够持续保留。 */
	kcontrol_news = devm_kmemdup(card->dev, &kcontrol_dai_link[0],
					sizeof(struct snd_kcontrol_new),
					GFP_KERNEL);
	if (!kcontrol_news) {
		dev_err(card->dev, "ASoC: Failed to create control for %s widget\n",
			link_name);
		goto outfree_w_param;
	}
	return kcontrol_news;

outfree_w_param:
	dapm_free_kcontrol(card, private_value, num_c2c_params, w_param_text);

	return NULL;
}

static struct snd_soc_dapm_widget *dapm_new_dai(struct snd_soc_card *card,
						struct snd_pcm_substream *substream,
						char *id)
{
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dapm_widget template;
	struct snd_soc_dapm_widget *w;
	const struct snd_kcontrol_new *kcontrol_news;
	int num_kcontrols;
	const char **w_param_text;
	unsigned long private_value = 0;
	char *link_name;
	int ret = -ENOMEM;

	link_name = devm_kasprintf(card->dev, GFP_KERNEL, "%s-%s",
				   rtd->dai_link->name, id);
	if (!link_name)
		goto name_fail;

	/*
	 * 只有在 codec2codec 存在多个配置时，才需要额外分配一个枚举控制。
	 * 普通单配置链路只需要一个普通 DAI link widget。
	 */
	w_param_text	= NULL;
	kcontrol_news	= NULL;
	num_kcontrols	= 0;
	if (rtd->dai_link->num_c2c_params > 1) {
		w_param_text = devm_kcalloc(card->dev,
					    rtd->dai_link->num_c2c_params,
					    sizeof(char *), GFP_KERNEL);
		if (!w_param_text)
			goto param_fail;

		num_kcontrols = 1;
		kcontrol_news = dapm_alloc_kcontrol(card, link_name,
						    rtd->dai_link->c2c_params,
						    rtd->dai_link->num_c2c_params,
						    w_param_text, &private_value);
		if (!kcontrol_news)
			goto param_fail;
	}

	memset(&template, 0, sizeof(template));
	template.reg		= SND_SOC_NOPM;
	template.id		= snd_soc_dapm_dai_link;
	template.name		= link_name;
	template.event		= dapm_dai_link_event;
	template.event_flags	= SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
				  SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD;
	template.kcontrol_news	= kcontrol_news;
	template.num_kcontrols	= num_kcontrols;

	dev_dbg(card->dev, "ASoC: adding %s widget\n", link_name);

	/* 这个 widget 是 codec2codec 链路的中间锚点，供 DAPM 事件和控制挂接。 */
	w = snd_soc_dapm_new_control_unlocked(dapm, &template);
	if (IS_ERR(w)) {
		ret = PTR_ERR(w);
		goto outfree_kcontrol_news;
	}

	w->priv = substream;

	return w;

outfree_kcontrol_news:
	devm_kfree(card->dev, (void *)template.kcontrol_news);
	dapm_free_kcontrol(card, &private_value,
				   rtd->dai_link->num_c2c_params, w_param_text);
param_fail:
	devm_kfree(card->dev, link_name);
name_fail:
	dev_err(rtd->dev, "ASoC: Failed to create %s-%s widget: %d\n",
		rtd->dai_link->name, id, ret);
	return ERR_PTR(ret);
}

/**
 * snd_soc_dapm_new_dai_widgets - 创建新的 DAPM DAI widget
 * @dapm: DAPM 上下文
 * @dai: 父 DAI
 *
 * 返回：成功时返回 0，否则返回错误码。
 */
int snd_soc_dapm_new_dai_widgets(struct snd_soc_dapm_context *dapm,
				 struct snd_soc_dai *dai)
{
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	struct snd_soc_dapm_widget template;
	struct snd_soc_dapm_widget *w;

	WARN_ON(dev != dai->dev);

	memset(&template, 0, sizeof(template));
	template.reg = SND_SOC_NOPM;

	/*
	 * 每个 DAI 的 playback/capture stream 都会变成一个专用 widget，
	 * 后续的 DAI link 连接、stream event 都依赖这两个锚点。
	 */
	if (dai->driver->playback.stream_name) {
		template.id = snd_soc_dapm_dai_in;
		template.name = dai->driver->playback.stream_name;
		template.sname = dai->driver->playback.stream_name;

		dev_dbg(dai->dev, "ASoC: adding %s widget\n",
			template.name);

		w = snd_soc_dapm_new_control_unlocked(dapm, &template);
		if (IS_ERR(w))
			return PTR_ERR(w);

		w->priv = dai;
		snd_soc_dai_set_widget_playback(dai, w);
	}

	if (dai->driver->capture.stream_name) {
		template.id = snd_soc_dapm_dai_out;
		template.name = dai->driver->capture.stream_name;
		template.sname = dai->driver->capture.stream_name;

		dev_dbg(dai->dev, "ASoC: adding %s widget\n",
			template.name);

		w = snd_soc_dapm_new_control_unlocked(dapm, &template);
		if (IS_ERR(w))
			return PTR_ERR(w);

		w->priv = dai;
		snd_soc_dai_set_widget_capture(dai, w);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_new_dai_widgets);

int snd_soc_dapm_link_dai_widgets(struct snd_soc_card *card)
{
	struct snd_soc_dapm_widget *dai_w, *w;
	struct snd_soc_dapm_widget *src, *sink;
	struct snd_soc_dai *dai;

	/* 对每个 DAI widget 分别处理。 */
	for_each_card_widgets(card, dai_w) {
		switch (dai_w->id) {
		case snd_soc_dapm_dai_in:
		case snd_soc_dapm_dai_out:
			break;
		default:
			continue;
		}

		/* 让用户知道这里没有可连接的 DAI。 */
		if (!dai_w->priv) {
			dev_dbg(card->dev, "dai widget %s has no DAI\n",
				dai_w->name);
			continue;
		}

		dai = dai_w->priv;

		/* 在同一个 DAPM context 内找同 stream 的普通 widget，然后建立静态连边。 */
		for_each_card_widgets(card, w) {
			if (w->dapm != dai_w->dapm)
				continue;

			switch (w->id) {
			case snd_soc_dapm_dai_in:
			case snd_soc_dapm_dai_out:
				continue;
			default:
				break;
			}

			if (!w->sname || !strstr(w->sname, dai_w->sname))
				continue;

			if (dai_w->id == snd_soc_dapm_dai_in) {
				src = dai_w;
				sink = w;
			} else {
				src = w;
				sink = dai_w;
			}
			dev_dbg(dai->dev, "%s -> %s\n", src->name, sink->name);
			/* 这里插入的是“DAI widget 到普通 widget”的静态连边。 */
			dapm_add_path(w->dapm, src, sink, NULL, NULL);
		}
	}

	return 0;
}

static void dapm_connect_dai_routes(struct snd_soc_dapm_context *dapm,
				    struct snd_soc_dai *src_dai,
				    struct snd_soc_dapm_widget *src,
				    struct snd_soc_dapm_widget *dai,
				    struct snd_soc_dai *sink_dai,
				    struct snd_soc_dapm_widget *sink)
{
	struct device *dev = snd_soc_dapm_to_dev(dapm);

	dev_dbg(dev, "connected DAI link %s:%s -> %s:%s\n",
		src_dai->component->name, src->name,
		sink_dai->component->name, sink->name);

	/*
	 * 对于 codec2codec 链路，中间可能要先挂一个专用 DAI widget；
	 * 普通链路则直接把 src 和 sink 连接起来。
	 */
	if (dai) {
		dapm_add_path(dapm, src, dai, NULL, NULL);
		src = dai;
	}

	dapm_add_path(dapm, src, sink, NULL, NULL);
}

static void dapm_connect_dai_pair(struct snd_soc_card *card,
				  struct snd_soc_pcm_runtime *rtd,
				  struct snd_soc_dai *codec_dai,
				  struct snd_soc_dai *cpu_dai)
{
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	struct snd_soc_dai_link *dai_link = rtd->dai_link;
	struct snd_soc_dapm_widget *codec, *cpu;
	struct snd_soc_dai *src_dai[]		= { cpu_dai,	codec_dai };
	struct snd_soc_dai *sink_dai[]		= { codec_dai,	cpu_dai };
	struct snd_soc_dapm_widget **src[]	= { &cpu,	&codec };
	struct snd_soc_dapm_widget **sink[]	= { &codec,	&cpu };
	char *widget_name[]			= { "playback",	"capture" };
	int stream;

	for_each_pcm_streams(stream) {
		int stream_cpu, stream_codec;

		stream_cpu	= snd_soc_get_stream_cpu(dai_link, stream);
		stream_codec	= stream;

		/* 只有 CPU/Codec 两端对应的 DAI widget 都存在，才能建立这条 route。 */
		cpu	= snd_soc_dai_get_widget(cpu_dai,	stream_cpu);
		codec	= snd_soc_dai_get_widget(codec_dai,	stream_codec);

		if (!cpu || !codec)
			continue;

		/* codec2codec 场景下，每个 stream 都可能需要单独的中间 widget。 */
		if (dai_link->c2c_params && !rtd->c2c_widget[stream]) {
			struct snd_pcm_substream *substream = rtd->pcm->streams[stream].substream;
			struct snd_soc_dapm_widget *dai = dapm_new_dai(card, substream,
								       widget_name[stream]);

			if (IS_ERR(dai))
				continue;

			rtd->c2c_widget[stream] = dai;
		}

		/* 每个流方向都单独建立一条 DAPM route。 */
		dapm_connect_dai_routes(dapm, src_dai[stream], *src[stream],
					rtd->c2c_widget[stream],
					sink_dai[stream], *sink[stream]);
	}
}

static void dapm_dai_stream_event(struct snd_soc_dai *dai, int stream, int event)
{
	struct snd_soc_dapm_widget *w;

	w = snd_soc_dai_get_widget(dai, stream);

	if (w) {
		unsigned int ep;

		/* stream start/stop 会改变 widget 的活跃状态，必须标脏后重算。 */
		dapm_mark_dirty(w, "stream event");

		if (w->id == snd_soc_dapm_dai_in) {
			ep = SND_SOC_DAPM_EP_SOURCE;
			dapm_widget_invalidate_input_paths(w);
		} else {
			ep = SND_SOC_DAPM_EP_SINK;
			dapm_widget_invalidate_output_paths(w);
		}

		switch (event) {
		case SND_SOC_DAPM_STREAM_START:
			w->active = 1;
			w->is_ep = ep;
			break;
		case SND_SOC_DAPM_STREAM_STOP:
			w->active = 0;
			w->is_ep = 0;
			break;
		case SND_SOC_DAPM_STREAM_SUSPEND:
		case SND_SOC_DAPM_STREAM_RESUME:
		case SND_SOC_DAPM_STREAM_PAUSE_PUSH:
		case SND_SOC_DAPM_STREAM_PAUSE_RELEASE:
			break;
		}
	}
}

void snd_soc_dapm_connect_dai_link_widgets(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *cpu_dai;
	struct snd_soc_dai *codec_dai;

	/* for each BE DAI link... */
	for_each_card_rtds(card, rtd)  {
		struct snd_soc_dai_link_ch_map *ch_maps;
		int i;

		/*
		 * dynamic FE links have no fixed DAI mapping.
		 * CODEC<->CODEC links have no direct connection.
		 */
		if (rtd->dai_link->dynamic)
			continue;

		/*
		 * see
		 *	soc.h :: [dai_link->ch_maps Image sample]
		 */
		for_each_rtd_ch_maps(rtd, i, ch_maps) {
			cpu_dai   = snd_soc_rtd_to_cpu(rtd,   ch_maps->cpu);
			codec_dai = snd_soc_rtd_to_codec(rtd, ch_maps->codec);

			dapm_connect_dai_pair(card, rtd, codec_dai, cpu_dai);
		}
	}
}

static void dapm_stream_event(struct snd_soc_pcm_runtime *rtd, int stream, int event)
{
	struct snd_soc_dai *dai;
	int i;

	/* runtime 级 stream event 先分发给所有 DAI，再统一跑一次 power walk。 */
	for_each_rtd_dais(rtd, i, dai)
		dapm_dai_stream_event(dai, stream, event);

	dapm_power_widgets(rtd->card, event, NULL);
}

/**
 * snd_soc_dapm_stream_event - 向 DAPM core 发送流事件
 * @rtd: PCM runtime 数据
 * @stream: 流方向
 * @event: 流事件
 *
 * 向 DAPM core 发送流事件，core 会据此做必要的 widget 上下电切换。
 *
 * 返回：成功时返回 0，否则返回错误码。
 */
void snd_soc_dapm_stream_event(struct snd_soc_pcm_runtime *rtd, int stream,
			      int event)
{
	struct snd_soc_card *card = rtd->card;

	/* 对外接口先加卡级锁，再进入内部的 DAPM 事件分发。 */
	snd_soc_dapm_mutex_lock(card);
	dapm_stream_event(rtd, stream, event);
	snd_soc_dapm_mutex_unlock(card);
}

void snd_soc_dapm_stream_stop(struct snd_soc_pcm_runtime *rtd, int stream)
{
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (snd_soc_runtime_ignore_pmdown_time(rtd)) {
			/* powered down playback stream now */
			snd_soc_dapm_stream_event(rtd,
						  SNDRV_PCM_STREAM_PLAYBACK,
						  SND_SOC_DAPM_STREAM_STOP);
		} else {
			/* start delayed pop wq here for playback streams */
			rtd->pop_wait = 1;
			queue_delayed_work(system_power_efficient_wq,
					   &rtd->delayed_work,
					   msecs_to_jiffies(rtd->pmdown_time));
		}
	} else {
		/* capture streams can be powered down now */
		snd_soc_dapm_stream_event(rtd, SNDRV_PCM_STREAM_CAPTURE,
					  SND_SOC_DAPM_STREAM_STOP);
	}
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_stream_stop);

/**
 * snd_soc_dapm_enable_pin_unlocked - 使能 pin
 * @dapm: DAPM 上下文
 * @pin: pin 名称
 *
 * 如果存在有效音频路径且有活跃音频流，则使能输入/输出 pin 及其父/子 widget。
 *
 * 需要外部锁保护。
 *
 * 注意：之后需要调用 snd_soc_dapm_sync()，DAPM 才会真正执行
 * widget 上下电切换。
 */
int snd_soc_dapm_enable_pin_unlocked(struct snd_soc_dapm_context *dapm,
				   const char *pin)
{
	return dapm_set_pin(dapm, pin, 1);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_enable_pin_unlocked);

/**
 * snd_soc_dapm_enable_pin - 使能 pin
 * @dapm: DAPM 上下文
 * @pin: pin 名称
 *
 * 如果存在有效音频路径且有活跃音频流，则使能输入/输出 pin 及其父/子 widget。
 *
 * 注意：之后需要调用 snd_soc_dapm_sync()，DAPM 才会真正执行
 * widget 上下电切换。
 */
int snd_soc_dapm_enable_pin(struct snd_soc_dapm_context *dapm, const char *pin)
{
	int ret;

	snd_soc_dapm_mutex_lock(dapm);

	ret = dapm_set_pin(dapm, pin, 1);

	snd_soc_dapm_mutex_unlock(dapm);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_enable_pin);

/**
 * snd_soc_dapm_force_enable_pin_unlocked - 强制使能 pin
 * @dapm: DAPM 上下文
 * @pin: pin 名称
 *
 * 不考虑其他状态，强制使能输入/输出 pin。通常用于麦克风 bias 供电，
 * 以及麦克风 jack 检测场景。
 *
 * 需要外部锁保护。
 *
 * 注意：之后需要调用 snd_soc_dapm_sync()，DAPM 才会真正执行
 * widget 上下电切换。
 */
int snd_soc_dapm_force_enable_pin_unlocked(struct snd_soc_dapm_context *dapm,
					 const char *pin)
{
	struct device *dev;
	struct snd_soc_dapm_widget *w = dapm_find_widget(dapm, pin, true);

	if (!w) {
		dev = snd_soc_dapm_to_dev(dapm);

		dev_err(dev, "ASoC: unknown pin %s\n", pin);
		return -EINVAL;
	}

	dev = snd_soc_dapm_to_dev(w->dapm);

	dev_dbg(dev, "ASoC: force enable pin %s\n", pin);
	if (!w->connected) {
		/*
		 * w->force does not affect the number of input or output paths,
		 * so we only have to recheck if w->connected is changed
		 */
		dapm_widget_invalidate_input_paths(w);
		dapm_widget_invalidate_output_paths(w);
		w->connected = 1;
	}
	w->force = 1;
	dapm_mark_dirty(w, "force enable");

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_force_enable_pin_unlocked);

/**
 * snd_soc_dapm_force_enable_pin - 强制使能 pin
 * @dapm: DAPM 上下文
 * @pin: pin 名称
 *
 * 不考虑其他状态，强制使能输入/输出 pin。通常用于麦克风 bias 供电，
 * 以及麦克风 jack 检测场景。
 *
 * 注意：之后需要调用 snd_soc_dapm_sync()，DAPM 才会真正执行
 * widget 上下电切换。
 */
int snd_soc_dapm_force_enable_pin(struct snd_soc_dapm_context *dapm,
				  const char *pin)
{
	int ret;

	snd_soc_dapm_mutex_lock(dapm);

	ret = snd_soc_dapm_force_enable_pin_unlocked(dapm, pin);

	snd_soc_dapm_mutex_unlock(dapm);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_force_enable_pin);

/**
 * snd_soc_dapm_disable_pin_unlocked - 禁用 pin
 * @dapm: DAPM 上下文
 * @pin: pin 名称
 *
 * 禁用输入/输出 pin 及其父/子 widget。
 *
 * 需要外部锁保护。
 *
 * 注意：之后需要调用 snd_soc_dapm_sync()，DAPM 才会真正执行
 * widget 上下电切换。
 */
int snd_soc_dapm_disable_pin_unlocked(struct snd_soc_dapm_context *dapm,
				    const char *pin)
{
	return dapm_set_pin(dapm, pin, 0);
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_disable_pin_unlocked);

/**
 * snd_soc_dapm_disable_pin - 禁用 pin
 * @dapm: DAPM 上下文
 * @pin: pin 名称
 *
 * 禁用输入/输出 pin 及其父/子 widget。
 *
 * 注意：之后需要调用 snd_soc_dapm_sync()，DAPM 才会真正执行
 * widget 上下电切换。
 */
int snd_soc_dapm_disable_pin(struct snd_soc_dapm_context *dapm,
			     const char *pin)
{
	int ret;

	snd_soc_dapm_mutex_lock(dapm);

	ret = dapm_set_pin(dapm, pin, 0);

	snd_soc_dapm_mutex_unlock(dapm);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_disable_pin);

/**
 * snd_soc_dapm_get_pin_status - 获取音频 pin 状态
 * @dapm: DAPM 上下文
 * @pin: 音频信号 pin 的端点（或起点）
 *
 * 获取音频 pin 的连接状态：已连接或未连接。
 *
 * 返回：已连接返回 1，否则返回 0。
 */
int snd_soc_dapm_get_pin_status(struct snd_soc_dapm_context *dapm,
				const char *pin)
{
	struct snd_soc_dapm_widget *w = dapm_find_widget(dapm, pin, true);

	/* 只是查询逻辑状态，不触发重算。 */
	if (w)
		return w->connected;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_get_pin_status);

/**
 * snd_soc_dapm_ignore_suspend - 让 DAPM 端点忽略 suspend
 * @dapm: DAPM 上下文
 * @pin: 音频信号 pin 的端点（或起点）
 *
 * 把给定端点或 pin 标记为忽略 suspend。系统进入 suspend 时，
 * 连接两个被标记为忽略 suspend 的端点之间的路径不会被关闭。
 * 但该路径必须在 suspend 前就已经通过正常方式处于启用状态，
 * 如果本来没启用，不会自动帮你打开。
 */
int snd_soc_dapm_ignore_suspend(struct snd_soc_dapm_context *dapm,
				const char *pin)
{
	struct device *dev = snd_soc_dapm_to_dev(dapm);
	struct snd_soc_dapm_widget *w = dapm_find_widget(dapm, pin, false);

	/*
	 * 这个标志只影响 suspend/resume 期间的图裁剪，
	 * 不会主动把路径打开，也不会替用户补连通性。
	 */
	if (!w) {
		dev_err(dev, "ASoC: unknown pin %s\n", pin);
		return -EINVAL;
	}

	w->ignore_suspend = 1;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_dapm_ignore_suspend);

/**
 * snd_soc_dapm_free - 释放 DAPM 资源
 * @dapm: DAPM 上下文
 *
 * 释放所有 DAPM widget 和相关资源。
 */
void snd_soc_dapm_free(struct snd_soc_dapm_context *dapm)
{
	/* 先清 debugfs，再拆 widget 和路径，最后摘掉 context 链表。 */
	dapm_debugfs_cleanup(dapm);
	dapm_free_widgets(dapm);
	list_del(&dapm->list);
}

void snd_soc_dapm_init(struct snd_soc_dapm_context *dapm,
		       struct snd_soc_card *card,
		       struct snd_soc_component *component)
{
	dapm->card		= card;
	dapm->component		= component;
	dapm->bias_level	= SND_SOC_BIAS_OFF;

	if (component)
		dapm->idle_bias		= component->driver->idle_bias_on;

	INIT_LIST_HEAD(&dapm->list);
	/* 供 for_each_card_dapms() 遍历使用。 */
	list_add(&dapm->list, &card->dapm_list);
}

static void dapm_shutdown(struct snd_soc_dapm_context *dapm)
{
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_dapm_widget *w;
	LIST_HEAD(down_list);
	int powerdown = 0;

	snd_soc_dapm_mutex_lock_root(card);

	for_each_card_widgets(dapm->card, w) {
		if (w->dapm != dapm)
			continue;
		if (w->power) {
			dapm_seq_insert(w, &down_list, false);
			w->new_power = 0;
			powerdown = 1;
		}
	}

	/* 如果没有需要下电的 widget，系统其实已经处于 standby。 */
	if (powerdown) {
		if (dapm->bias_level == SND_SOC_BIAS_ON)
			snd_soc_dapm_set_bias_level(dapm,
						    SND_SOC_BIAS_PREPARE);
		dapm_seq_run(card, &down_list, 0, false);
		if (dapm->bias_level == SND_SOC_BIAS_PREPARE)
			snd_soc_dapm_set_bias_level(dapm,
						    SND_SOC_BIAS_STANDBY);
	}

	snd_soc_dapm_mutex_unlock(card);
}

/*
 * snd_soc_dapm_shutdown - callback for system shutdown
 */
void snd_soc_dapm_shutdown(struct snd_soc_card *card)
{
	struct snd_soc_dapm_context *card_dapm = snd_soc_card_to_dapm(card);
	struct snd_soc_dapm_context *dapm;

	for_each_card_dapms(card, dapm) {
		if (dapm != card_dapm) {
			dapm_shutdown(dapm);
			if (dapm->bias_level == SND_SOC_BIAS_STANDBY)
				snd_soc_dapm_set_bias_level(dapm, SND_SOC_BIAS_OFF);
		}
	}

	dapm_shutdown(card_dapm);
	if (card_dapm->bias_level == SND_SOC_BIAS_STANDBY)
		snd_soc_dapm_set_bias_level(card_dapm, SND_SOC_BIAS_OFF);
}

/* 模块信息。 */
MODULE_AUTHOR("Liam Girdwood, lrg@slimlogic.co.uk");
MODULE_DESCRIPTION("Dynamic Audio Power Management core for ALSA SoC");
MODULE_LICENSE("GPL");
