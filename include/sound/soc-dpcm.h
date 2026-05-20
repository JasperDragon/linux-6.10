/* SPDX-License-Identifier: GPL-2.0
 *
 * linux/sound/soc-dpcm.h -- ALSA SoC Dynamic PCM Support
 *
 * Author:		Liam Girdwood <lrg@ti.com>
 */

#ifndef __LINUX_SND_SOC_DPCM_H
#define __LINUX_SND_SOC_DPCM_H

#include <linux/slab.h>
#include <linux/list.h>
#include <sound/pcm.h>

struct snd_soc_pcm_runtime;

/*
 * runtime_update 的类型。
 * DPCM 中 FE 的 PCM 操作、mux/mixer 触发的路由变化，最终都会落到
 * 这里统一决定要不要同步 BE/FE 的状态。
 */
enum snd_soc_dpcm_update {
	SND_SOC_DPCM_UPDATE_NO	= 0,
	SND_SOC_DPCM_UPDATE_BE,
	SND_SOC_DPCM_UPDATE_FE,
};

/* FE -> BE 链接管理状态。 */
enum snd_soc_dpcm_link_state {
	SND_SOC_DPCM_LINK_STATE_NEW	= 0,	/* newly created link */
	SND_SOC_DPCM_LINK_STATE_FREE,		/* link to be dismantled */
};

/* FE -> BE 链路在 PCM 生命周期中的状态机。 */
enum snd_soc_dpcm_state {
	SND_SOC_DPCM_STATE_NEW	= 0,
	SND_SOC_DPCM_STATE_OPEN,
	SND_SOC_DPCM_STATE_HW_PARAMS,
	SND_SOC_DPCM_STATE_PREPARE,
	SND_SOC_DPCM_STATE_START,
	SND_SOC_DPCM_STATE_STOP,
	SND_SOC_DPCM_STATE_PAUSED,
	SND_SOC_DPCM_STATE_SUSPEND,
	SND_SOC_DPCM_STATE_HW_FREE,
	SND_SOC_DPCM_STATE_CLOSE,
};

/*
 * DPCM trigger 的前后顺序。
 * 某些 DSP 或平台要求在 CPU/platform DAI 之前或之后触发，所以 core
 * 需要给出 PRE / POST 两种顺序策略。
 */
enum snd_soc_dpcm_trigger {
	SND_SOC_DPCM_TRIGGER_PRE		= 0,
	SND_SOC_DPCM_TRIGGER_POST,
};

/*
 * DPCM link。
 * 运行时把 FE 和 BE 绑定起来，并保存当前的 link state 以及
 * hw_params 配置。
 */
struct snd_soc_dpcm {
	/* 对应的 FE / BE runtime。 */
	struct snd_soc_pcm_runtime *be;
	struct snd_soc_pcm_runtime *fe;

	/* 链路状态。 */
	enum snd_soc_dpcm_link_state state;

	/* 在 FE / BE 侧挂接的链表节点。 */
	struct list_head list_be;
	struct list_head list_fe;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_state;
#endif
};

/* 每个 stream 方向对应一份 DPCM runtime 数据。 */
struct snd_soc_dpcm_runtime {
	struct list_head be_clients;
	struct list_head fe_clients;

	int users;
	struct snd_pcm_hw_params hw_params;

	/* 运行状态和最近一次更新类型。 */
	enum snd_soc_dpcm_update runtime_update;
	enum snd_soc_dpcm_state state;

	/* 有 pending trigger 时保存命令 + 1，0 表示没有待处理触发。 */
	int trigger_pending;

	/* 由 BE stream 的 PCM lock 保护的引用计数。 */
	int be_start;
	int be_pause;
	/* 记录 FE 是否处于 PAUSE 后的 STOP 路径。 */
	bool fe_pause;
};

/* 遍历一个 BE 对应的所有 FE 客户端。 */
#define for_each_dpcm_fe(be, stream, _dpcm)				\
	list_for_each_entry(_dpcm, &(be)->dpcm[stream].fe_clients, list_fe)

/* 遍历一个 FE 对应的所有 BE 客户端。 */
#define for_each_dpcm_be(fe, stream, _dpcm)				\
	list_for_each_entry(_dpcm, &(fe)->dpcm[stream].be_clients, list_be)
/* 安全遍历版本。 */
#define for_each_dpcm_be_safe(fe, stream, _dpcm, __dpcm)			\
	list_for_each_entry_safe(_dpcm, __dpcm, &(fe)->dpcm[stream].be_clients, list_be)
/* 回滚时反向遍历已建立的 BE 客户端。 */
#define for_each_dpcm_be_rollback(fe, stream, _dpcm)			\
	list_for_each_entry_continue_reverse(_dpcm, &(fe)->dpcm[stream].be_clients, list_be)


/* 获取某个 BE 对应的 substream。 */
struct snd_pcm_substream *
	snd_soc_dpcm_get_substream(struct snd_soc_pcm_runtime *be, int stream);

/* 触发 PCM 与 DAI link 之间的路由更新。 */
int snd_soc_dpcm_runtime_update(struct snd_soc_card *card);

#ifdef CONFIG_DEBUG_FS
void soc_dpcm_debugfs_add(struct snd_soc_pcm_runtime *rtd);
#else
static inline void soc_dpcm_debugfs_add(struct snd_soc_pcm_runtime *rtd)
{
}
#endif

int dpcm_path_get(struct snd_soc_pcm_runtime *fe,
	int stream, struct snd_soc_dapm_widget_list **list_);
void dpcm_path_put(struct snd_soc_dapm_widget_list **list);
int dpcm_add_paths(struct snd_soc_pcm_runtime *fe, int stream,
		   struct snd_soc_dapm_widget_list **list_);
int dpcm_be_dai_startup(struct snd_soc_pcm_runtime *fe, int stream);
void dpcm_be_dai_stop(struct snd_soc_pcm_runtime *fe, int stream,
		      int do_hw_free, struct snd_soc_dpcm *last);
void dpcm_be_disconnect(struct snd_soc_pcm_runtime *fe, int stream);
void dpcm_clear_pending_state(struct snd_soc_pcm_runtime *fe, int stream);
void dpcm_be_dai_hw_free(struct snd_soc_pcm_runtime *fe, int stream);
int dpcm_be_dai_hw_params(struct snd_soc_pcm_runtime *fe, int tream);
int dpcm_be_dai_trigger(struct snd_soc_pcm_runtime *fe, int stream, int cmd);
int dpcm_be_dai_prepare(struct snd_soc_pcm_runtime *fe, int stream);
void dpcm_dapm_stream_event(struct snd_soc_pcm_runtime *fe, int dir, int event);

bool dpcm_end_walk_at_be(struct snd_soc_dapm_widget *widget, enum snd_soc_dapm_direction dir);
int widget_in_list(struct snd_soc_dapm_widget_list *list,
		   struct snd_soc_dapm_widget *widget);

#define dpcm_be_dai_startup_rollback(fe, stream, last)	\
						dpcm_be_dai_stop(fe, stream, 0, last)
#define dpcm_be_dai_startup_unwind(fe, stream)	dpcm_be_dai_stop(fe, stream, 0, NULL)
#define dpcm_be_dai_shutdown(fe, stream)	dpcm_be_dai_stop(fe, stream, 1, NULL)

#endif
