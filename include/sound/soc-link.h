/* SPDX-License-Identifier: GPL-2.0
 *
 * soc-link.h
 *
 * Copyright (C) 2019 Renesas Electronics Corp.
 * Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 */
#ifndef __SOC_LINK_H
#define __SOC_LINK_H

/*
 * soc-link.h 负责把 card / runtime / stream 之间的调用串起来。
 * 这些接口多数在 PCM 打开、参数配置、启动停止时被 ASoC core 使用。
 *
 * 你可以把它理解成“把 card 级策略落到具体 PCM/DPCM 运行时”的桥接层。
 */
/* 初始化一条 runtime 绑定：把 link、DAI、component、PCM 组织起来。 */
int snd_soc_link_init(struct snd_soc_pcm_runtime *rtd);

/* 反向清理一条 runtime 绑定。 */
void snd_soc_link_exit(struct snd_soc_pcm_runtime *rtd);

/* BE 侧 hw_params 修正入口。 */
int snd_soc_link_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				    struct snd_pcm_hw_params *params);

/* PCM 流生命周期的桥接函数。 */
int snd_soc_link_startup(struct snd_pcm_substream *substream);

/* shutdown 会在 rollback 场景下按标记决定是否真正回收。 */
void snd_soc_link_shutdown(struct snd_pcm_substream *substream,
			   int rollback);

/* prepare 阶段负责把 runtime 的 DAI 们都拉到可触发状态。 */
int snd_soc_link_prepare(struct snd_pcm_substream *substream);

/* hw_params 阶段负责把用户态参数协商结果下发到 DAI/component。 */
int snd_soc_link_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params);

/* 释放 hw_params 阶段占用的资源。 */
void snd_soc_link_hw_free(struct snd_pcm_substream *substream,
			  int rollback);

/* trigger 阶段负责真正的 start/stop/pause 传递。 */
int snd_soc_link_trigger(struct snd_pcm_substream *substream, int cmd,
			 int rollback);

/* compressed offload 流生命周期的桥接函数。 */
int snd_soc_link_compr_startup(struct snd_compr_stream *cstream);

/* 压缩流 shutdown。 */
void snd_soc_link_compr_shutdown(struct snd_compr_stream *cstream,
				 int rollback);

/* 压缩流 set_params。 */
int snd_soc_link_compr_set_params(struct snd_compr_stream *cstream);

#endif /* __SOC_LINK_H */
