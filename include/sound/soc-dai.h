/* SPDX-License-Identifier: GPL-2.0
 *
 * linux/sound/soc-dai.h -- ALSA SoC 层
 *
 * Copyright:	2005-2008 Wolfson Microelectronics. PLC.
 *
 * 数字音频接口（DAI）API。
 */

#ifndef __LINUX_SND_SOC_DAI_H
#define __LINUX_SND_SOC_DAI_H


#include <linux/list.h>
#include <sound/asoc.h>

struct snd_pcm_substream;
struct snd_soc_dapm_widget;
struct snd_compr_stream;

/*
 * DAI 物理格式定义。
 * 这里描述的是总线层面的音频传输规则：I2S、Left/Right Justified、
 * DSP A/B、AC97、PDM 等。它决定了 BCLK/LRCLK 的关系和数据采样边沿。
 */
#define SND_SOC_DAIFMT_I2S		SND_SOC_DAI_FORMAT_I2S
#define SND_SOC_DAIFMT_RIGHT_J		SND_SOC_DAI_FORMAT_RIGHT_J
#define SND_SOC_DAIFMT_LEFT_J		SND_SOC_DAI_FORMAT_LEFT_J
#define SND_SOC_DAIFMT_DSP_A		SND_SOC_DAI_FORMAT_DSP_A
#define SND_SOC_DAIFMT_DSP_B		SND_SOC_DAI_FORMAT_DSP_B
#define SND_SOC_DAIFMT_AC97		SND_SOC_DAI_FORMAT_AC97
#define SND_SOC_DAIFMT_PDM		SND_SOC_DAI_FORMAT_PDM

/* left and right justified also known as MSB and LSB respectively */
#define SND_SOC_DAIFMT_MSB		SND_SOC_DAIFMT_LEFT_J
#define SND_SOC_DAIFMT_LSB		SND_SOC_DAIFMT_RIGHT_J

/*
 * 以 bitmask 形式描述“这个 DAI 能支持哪些格式”。
 * 这是 ASoC core 做格式自动选择时会参考的能力集。
 */
/*
 * 这里按 SND_SOC_DAI_FORMAT_xx 作为位移值来组织能力掩码。
 * 具体的格式协商过程见 snd_soc_runtime_get_dai_fmt()。
 */
#define SND_SOC_POSSIBLE_DAIFMT_FORMAT_SHIFT	0
#define SND_SOC_POSSIBLE_DAIFMT_FORMAT_MASK	(0xFFFF << SND_SOC_POSSIBLE_DAIFMT_FORMAT_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_I2S		(1 << SND_SOC_DAI_FORMAT_I2S)
#define SND_SOC_POSSIBLE_DAIFMT_RIGHT_J		(1 << SND_SOC_DAI_FORMAT_RIGHT_J)
#define SND_SOC_POSSIBLE_DAIFMT_LEFT_J		(1 << SND_SOC_DAI_FORMAT_LEFT_J)
#define SND_SOC_POSSIBLE_DAIFMT_DSP_A		(1 << SND_SOC_DAI_FORMAT_DSP_A)
#define SND_SOC_POSSIBLE_DAIFMT_DSP_B		(1 << SND_SOC_DAI_FORMAT_DSP_B)
#define SND_SOC_POSSIBLE_DAIFMT_AC97		(1 << SND_SOC_DAI_FORMAT_AC97)
#define SND_SOC_POSSIBLE_DAIFMT_PDM		(1 << SND_SOC_DAI_FORMAT_PDM)

/*
 * TDM slot 空闲策略。
 * 当某个 slot 没有真正承载音频时，硬件希望把该 slot 保持为 0、
 * 高阻、下拉、上拉还是驱动高电平。
 */
#define SND_SOC_DAI_TDM_IDLE_NONE	0
#define SND_SOC_DAI_TDM_IDLE_OFF	1
#define SND_SOC_DAI_TDM_IDLE_ZERO	2
#define SND_SOC_DAI_TDM_IDLE_PULLDOWN	3
#define SND_SOC_DAI_TDM_IDLE_HIZ	4
#define SND_SOC_DAI_TDM_IDLE_PULLUP	5
#define SND_SOC_DAI_TDM_IDLE_DRIVE_HIGH	6

/*
 * 时钟门控策略。
 * gated 表示在没有数据时关闭 bit clock，以降低功耗；
 * cont 表示时钟持续输出。
 */
#define SND_SOC_DAIFMT_CONT		(1 << 4) /* continuous clock */
#define SND_SOC_DAIFMT_GATED		(0 << 4) /* clock is gated */

/* 以 bitmask 形式描述“时钟门控能力集”。 */
/*
 * 定义能力选择时遵循 GATED -> CONT 的优先级；如果两者都可选，
 * 最终会优先选择 GATED。
 * 详细选择逻辑见 snd_soc_runtime_get_dai_fmt()。
 */
#define SND_SOC_POSSIBLE_DAIFMT_CLOCK_SHIFT	16
#define SND_SOC_POSSIBLE_DAIFMT_CLOCK_MASK	(0xFFFF	<< SND_SOC_POSSIBLE_DAIFMT_CLOCK_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_GATED		(0x1ULL	<< SND_SOC_POSSIBLE_DAIFMT_CLOCK_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_CONT		(0x2ULL	<< SND_SOC_POSSIBLE_DAIFMT_CLOCK_SHIFT)

/*
 * 时钟极性定义。
 * 这里定义 BCLK 和 LRCLK/FSYNC 的极性组合，决定数据在哪个边沿被
 * 采样或输出。
 */
#define SND_SOC_DAIFMT_NB_NF		(0 << 8) /* normal bit clock + frame */
#define SND_SOC_DAIFMT_NB_IF		(2 << 8) /* normal BCLK + inv FRM */
#define SND_SOC_DAIFMT_IB_NF		(3 << 8) /* invert BCLK + nor FRM */
#define SND_SOC_DAIFMT_IB_IF		(4 << 8) /* invert BCLK + FRM */

/* 以 bitmask 形式描述“极性组合能力集”。 */
#define SND_SOC_POSSIBLE_DAIFMT_INV_SHIFT	32
#define SND_SOC_POSSIBLE_DAIFMT_INV_MASK	(0xFFFFULL << SND_SOC_POSSIBLE_DAIFMT_INV_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_NB_NF		(0x1ULL    << SND_SOC_POSSIBLE_DAIFMT_INV_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_NB_IF		(0x2ULL    << SND_SOC_POSSIBLE_DAIFMT_INV_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_IB_NF		(0x4ULL    << SND_SOC_POSSIBLE_DAIFMT_INV_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_IB_IF		(0x8ULL    << SND_SOC_POSSIBLE_DAIFMT_INV_SHIFT)

/*
 * 时钟主从关系。
 * 这里的定义是“针对 codec 的视角”：
 * codec 负责输出时钟还是接收时钟，frame sync 也同理。
 */
#define SND_SOC_DAIFMT_CBP_CFP		(1 << 12) /* codec clk provider & frame provider */
#define SND_SOC_DAIFMT_CBC_CFP		(2 << 12) /* codec clk consumer & frame provider */
#define SND_SOC_DAIFMT_CBP_CFC		(3 << 12) /* codec clk provider & frame consumer */
#define SND_SOC_DAIFMT_CBC_CFC		(4 << 12) /* codec clk consumer & frame consumer */

/* 直接传给 set_fmt() 时，用于标识当前设备是主设备还是从设备。 */
#define SND_SOC_DAIFMT_BP_FP		SND_SOC_DAIFMT_CBP_CFP
#define SND_SOC_DAIFMT_BC_FP		SND_SOC_DAIFMT_CBC_CFP
#define SND_SOC_DAIFMT_BP_FC		SND_SOC_DAIFMT_CBP_CFC
#define SND_SOC_DAIFMT_BC_FC		SND_SOC_DAIFMT_CBC_CFC

/* 以 bitmask 形式描述“主从关系能力集”。 */
#define SND_SOC_POSSIBLE_DAIFMT_CLOCK_PROVIDER_SHIFT	48
#define SND_SOC_POSSIBLE_DAIFMT_CLOCK_PROVIDER_MASK	(0xFFFFULL << SND_SOC_POSSIBLE_DAIFMT_CLOCK_PROVIDER_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_CBP_CFP			(0x1ULL    << SND_SOC_POSSIBLE_DAIFMT_CLOCK_PROVIDER_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_CBC_CFP			(0x2ULL    << SND_SOC_POSSIBLE_DAIFMT_CLOCK_PROVIDER_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_CBP_CFC			(0x4ULL    << SND_SOC_POSSIBLE_DAIFMT_CLOCK_PROVIDER_SHIFT)
#define SND_SOC_POSSIBLE_DAIFMT_CBC_CFC			(0x8ULL    << SND_SOC_POSSIBLE_DAIFMT_CLOCK_PROVIDER_SHIFT)

#define SND_SOC_DAIFMT_FORMAT_MASK		0x000f
#define SND_SOC_DAIFMT_CLOCK_MASK		0x00f0
#define SND_SOC_DAIFMT_INV_MASK			0x0f00
#define SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK	0xf000

#define SND_SOC_DAIFMT_MASTER_MASK	SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK

/* 主时钟方向：输入或输出。 */
#define SND_SOC_CLOCK_IN		0
#define SND_SOC_CLOCK_OUT		1

#define SND_SOC_STD_AC97_FMTS (SNDRV_PCM_FMTBIT_S8 |\
			       SNDRV_PCM_FMTBIT_S16_LE |\
			       SNDRV_PCM_FMTBIT_S16_BE |\
			       SNDRV_PCM_FMTBIT_S20_3LE |\
			       SNDRV_PCM_FMTBIT_S20_3BE |\
			       SNDRV_PCM_FMTBIT_S20_LE |\
			       SNDRV_PCM_FMTBIT_S20_BE |\
			       SNDRV_PCM_FMTBIT_S24_3LE |\
			       SNDRV_PCM_FMTBIT_S24_3BE |\
                               SNDRV_PCM_FMTBIT_S32_LE |\
                               SNDRV_PCM_FMTBIT_S32_BE)

struct snd_soc_dai_driver;
struct snd_soc_dai;
struct snd_ac97_bus_ops;

/* DAI 时钟相关 API。 */
int snd_soc_dai_set_sysclk(struct snd_soc_dai *dai, int clk_id,
	unsigned int freq, int dir);

int snd_soc_dai_set_clkdiv(struct snd_soc_dai *dai,
	int div_id, int div);

int snd_soc_dai_set_pll(struct snd_soc_dai *dai,
	int pll_id, int source, unsigned int freq_in, unsigned int freq_out);

int snd_soc_dai_set_bclk_ratio(struct snd_soc_dai *dai, unsigned int ratio);

/* DAI 传输格式相关 API。 */
int snd_soc_dai_get_fmt_max_priority(const struct snd_soc_pcm_runtime *rtd);
u64 snd_soc_dai_get_fmt(const struct snd_soc_dai *dai, int priority);
int snd_soc_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt);

int snd_soc_dai_set_tdm_slot(struct snd_soc_dai *dai,
	unsigned int tx_mask, unsigned int rx_mask, int slots, int slot_width);

int snd_soc_dai_set_tdm_idle(struct snd_soc_dai *dai,
			     unsigned int tx_mask, unsigned int rx_mask,
			     int tx_mode, int rx_mode);

int snd_soc_dai_set_channel_map(struct snd_soc_dai *dai,
	unsigned int tx_num, const unsigned int *tx_slot,
	unsigned int rx_num, const unsigned int *rx_slot);

int snd_soc_dai_set_tristate(struct snd_soc_dai *dai, int tristate);

int snd_soc_dai_prepare(struct snd_soc_dai *dai,
			struct snd_pcm_substream *substream);

/* DAI 数字静音相关 API。 */
int snd_soc_dai_digital_mute(struct snd_soc_dai *dai, int mute,
			     int direction);
int snd_soc_dai_mute_is_ctrled_at_trigger(struct snd_soc_dai *dai);

int snd_soc_dai_get_channel_map(const struct snd_soc_dai *dai,
		unsigned int *tx_num, unsigned int *tx_slot,
		unsigned int *rx_num, unsigned int *rx_slot);

int snd_soc_dai_is_dummy(const struct snd_soc_dai *dai);

int snd_soc_dai_hw_params(struct snd_soc_dai *dai,
			  struct snd_pcm_substream *substream,
			  struct snd_pcm_hw_params *params);
void snd_soc_dai_hw_free(struct snd_soc_dai *dai,
			 struct snd_pcm_substream *substream,
			 int rollback);
int snd_soc_dai_startup(struct snd_soc_dai *dai,
			struct snd_pcm_substream *substream);
void snd_soc_dai_shutdown(struct snd_soc_dai *dai,
			  struct snd_pcm_substream *substream, int rollback);
void snd_soc_dai_suspend(struct snd_soc_dai *dai);
void snd_soc_dai_resume(struct snd_soc_dai *dai);
int snd_soc_dai_compress_new(struct snd_soc_dai *dai, struct snd_soc_pcm_runtime *rtd);
bool snd_soc_dai_stream_valid(const struct snd_soc_dai *dai, int stream);
void snd_soc_dai_action(struct snd_soc_dai *dai,
			int stream, int action);
static inline void snd_soc_dai_activate(struct snd_soc_dai *dai,
					int stream)
{
	snd_soc_dai_action(dai, stream,  1);
}
static inline void snd_soc_dai_deactivate(struct snd_soc_dai *dai,
					  int stream)
{
	snd_soc_dai_action(dai, stream, -1);
}
int snd_soc_dai_active(const struct snd_soc_dai *dai);

int snd_soc_pcm_dai_probe(struct snd_soc_pcm_runtime *rtd, int order);
int snd_soc_pcm_dai_remove(struct snd_soc_pcm_runtime *rtd, int order);
int snd_soc_pcm_dai_new(struct snd_soc_pcm_runtime *rtd);
int snd_soc_pcm_dai_prepare(struct snd_pcm_substream *substream);
int snd_soc_pcm_dai_trigger(struct snd_pcm_substream *substream, int cmd,
			    int rollback);
void snd_soc_pcm_dai_delay(struct snd_pcm_substream *substream,
			   snd_pcm_sframes_t *cpu_delay, snd_pcm_sframes_t *codec_delay);

int snd_soc_dai_compr_startup(struct snd_soc_dai *dai,
			      struct snd_compr_stream *cstream);
void snd_soc_dai_compr_shutdown(struct snd_soc_dai *dai,
				struct snd_compr_stream *cstream,
				int rollback);
int snd_soc_dai_compr_trigger(struct snd_soc_dai *dai,
			      struct snd_compr_stream *cstream, int cmd);
int snd_soc_dai_compr_set_params(struct snd_soc_dai *dai,
				 struct snd_compr_stream *cstream,
				 struct snd_compr_params *params);
int snd_soc_dai_compr_get_params(struct snd_soc_dai *dai,
				 struct snd_compr_stream *cstream,
				 struct snd_codec *params);
int snd_soc_dai_compr_ack(struct snd_soc_dai *dai,
			  struct snd_compr_stream *cstream,
			  size_t bytes);
int snd_soc_dai_compr_pointer(struct snd_soc_dai *dai,
			      struct snd_compr_stream *cstream,
			      struct snd_compr_tstamp64 *tstamp);
int snd_soc_dai_compr_set_metadata(struct snd_soc_dai *dai,
				   struct snd_compr_stream *cstream,
				   struct snd_compr_metadata *metadata);
int snd_soc_dai_compr_get_metadata(struct snd_soc_dai *dai,
				   struct snd_compr_stream *cstream,
				   struct snd_compr_metadata *metadata);

const char *snd_soc_dai_name_get(const struct snd_soc_dai *dai);

/*
 * DAI driver 的操作集。
 * DAI 负责“总线和流”的关系，通常会由 codec driver 或 CPU DAI driver
 * 提供。它包含时钟、格式、TDM、PCM 生命周期以及压缩流相关回调。
 */
struct snd_soc_dai_ops {
	/* DAI 驱动生命周期回调 */
	int (*probe)(struct snd_soc_dai *dai);
	int (*remove)(struct snd_soc_dai *dai);
	/* compress dai */
	int (*compress_new)(struct snd_soc_pcm_runtime *rtd);
	/* Optional Callback used at pcm creation*/
	int (*pcm_new)(struct snd_soc_pcm_runtime *rtd,
		       struct snd_soc_dai *dai);

	/*
	 * DAI 时钟配置。
	 * 通常在 hw_params() 阶段由 card driver 设置。
	 */
	int (*set_sysclk)(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir);
	int (*set_pll)(struct snd_soc_dai *dai, int pll_id, int source,
		unsigned int freq_in, unsigned int freq_out);
	int (*set_clkdiv)(struct snd_soc_dai *dai, int div_id, int div);
	int (*set_bclk_ratio)(struct snd_soc_dai *dai, unsigned int ratio);

	/*
	 * DAI 传输格式配置。
	 * 包括 I2S/DSP、主从关系、TDM slot、通道映射等。
	 */
	int (*set_fmt)(struct snd_soc_dai *dai, unsigned int fmt);
	int (*xlate_tdm_slot_mask)(unsigned int slots,
		unsigned int *tx_mask, unsigned int *rx_mask);
	int (*set_tdm_slot)(struct snd_soc_dai *dai,
		unsigned int tx_mask, unsigned int rx_mask,
		int slots, int slot_width);
	int (*set_tdm_idle)(struct snd_soc_dai *dai,
			    unsigned int tx_mask, unsigned int rx_mask,
			    int tx_mode, int rx_mode);
	int (*set_channel_map)(struct snd_soc_dai *dai,
		unsigned int tx_num, const unsigned int *tx_slot,
		unsigned int rx_num, const unsigned int *rx_slot);
	int (*get_channel_map)(const struct snd_soc_dai *dai,
			unsigned int *tx_num, unsigned int *tx_slot,
			unsigned int *rx_num, unsigned int *rx_slot);
	int (*set_tristate)(struct snd_soc_dai *dai, int tristate);

	int (*set_stream)(struct snd_soc_dai *dai,
			  void *stream, int direction);
	void *(*get_stream)(struct snd_soc_dai *dai, int direction);

	/*
	 * 数字静音。
	 * soc-core 在启动/停止流时会尽量调用它，用于减少爆音。
	 */
	int (*mute_stream)(struct snd_soc_dai *dai, int mute, int stream);

	/*
	 * ALSA PCM 回调。
	 * 这些函数在 open/hw_params/prepare/trigger 等生命周期中被调用。
	 */
	int (*startup)(struct snd_pcm_substream *,
		struct snd_soc_dai *);
	void (*shutdown)(struct snd_pcm_substream *,
		struct snd_soc_dai *);
	int (*hw_params)(struct snd_pcm_substream *,
		struct snd_pcm_hw_params *, struct snd_soc_dai *);
	int (*hw_free)(struct snd_pcm_substream *,
		struct snd_soc_dai *);
	int (*prepare)(struct snd_pcm_substream *,
		struct snd_soc_dai *);
	/*
	 * 注意：传给 trigger 的命令不一定和 DAI 的当前状态完全一致。
	 * 例如可能出现 START STOP STOP 这样的序列。
	 * 因此不要在 trigger 里无条件使用引用计数式的开关操作，
	 * 比如 clk_enable()/clk_disable()。
	 */
	int (*trigger)(struct snd_pcm_substream *, int,
		struct snd_soc_dai *);

	/* FIFO/硬件管线引入的延迟上报。 */
	snd_pcm_sframes_t (*delay)(struct snd_pcm_substream *,
		struct snd_soc_dai *);

	/*
	 * 自动选择格式列表。
	 * core 会按优先级逐步尝试这些格式，直到找到双方都支持的组合。
	 */
	const u64 *auto_selectable_formats;
	int num_auto_selectable_formats;

	/* probe/remove 顺序，与 component driver 的 order 语义一致。 */
	int probe_order;
	int remove_order;

	/* bit field */
	unsigned int no_capture_mute:1;
	unsigned int mute_unmute_on_trigger:1;
};

struct snd_soc_cdai_ops {
	/* compressed 流操作集。 */
	int (*startup)(struct snd_compr_stream *,
			struct snd_soc_dai *);
	int (*shutdown)(struct snd_compr_stream *,
			struct snd_soc_dai *);
	int (*set_params)(struct snd_compr_stream *,
			struct snd_compr_params *, struct snd_soc_dai *);
	int (*get_params)(struct snd_compr_stream *,
			struct snd_codec *, struct snd_soc_dai *);
	int (*set_metadata)(struct snd_compr_stream *,
			struct snd_compr_metadata *, struct snd_soc_dai *);
	int (*get_metadata)(struct snd_compr_stream *,
			struct snd_compr_metadata *, struct snd_soc_dai *);
	int (*trigger)(struct snd_compr_stream *, int,
			struct snd_soc_dai *);
	int (*pointer)(struct snd_compr_stream *stream,
		       struct snd_compr_tstamp64 *tstamp,
		       struct snd_soc_dai *dai);
	int (*ack)(struct snd_compr_stream *, size_t,
			struct snd_soc_dai *);
};

/*
 * DAI driver 静态描述。
 * 这不是运行时对象，而是“某个硬件 DAI 支持什么能力”的模板。
 * codec/platform driver 通常为每个 DAI 提供一个这样的描述。
 */
struct snd_soc_dai_driver {
	/* DAI 描述信息。 */
	const char *name;
	unsigned int id;
	unsigned int base;
	struct snd_soc_dobj dobj;
	const struct of_phandle_args *dai_args;

	/* DAI 和 compressed 流回调。 */
	const struct snd_soc_dai_ops *ops;
	const struct snd_soc_cdai_ops *cops;

	/* 播放/录音能力描述，供 runtime 推导硬件约束。 */
	struct snd_soc_pcm_stream capture;
	struct snd_soc_pcm_stream playback;
	/* driver 是否要求各 runtime 参数对称。 */
	unsigned int symmetric_rate:1;
	unsigned int symmetric_channels:1;
	unsigned int symmetric_sample_bits:1;
};

/* 每个 stream 对应的运行时状态。 */
struct snd_soc_dai_stream {
	/* 绑定到该方向的 DAPM widget。 */
	struct snd_soc_dapm_widget *widget;

	/* 活跃计数。 */
	unsigned int active;
	/* TDM slot mask，供参数修正和 debug 使用。 */
	unsigned int tdm_mask;

	/* 平台/codec 私有 DMA 数据。 */
	void *dma_data;
};

/*
 * DAI 运行时对象。
 * 这是注册后的 DAI 实例，不是静态 driver 描述。它负责保存当前 DAI
 * 所属的 component、运行时 widget、活动计数和调试标记。
 */
struct snd_soc_dai {
	/* 实例名字和编号。 */
	const char *name;
	int id;
	struct device *dev;

	/* 关联的静态 driver 描述。 */
	struct snd_soc_dai_driver *driver;

	/* 每个 stream 的运行时状态。 */
	struct snd_soc_dai_stream stream[SNDRV_PCM_STREAM_LAST + 1];

	/* 对称性缓存，仅在需要强制对称时有效。 */
	unsigned int symmetric_rate;
	unsigned int symmetric_channels;
	unsigned int symmetric_sample_bits;

	/* 所属 component。 */
	struct snd_soc_component *component;

	/* 挂入 component->dai_list 的链表节点。 */
	struct list_head list;

	/* 函数级回滚标记。 */
	struct snd_pcm_substream *mark_startup;
	struct snd_pcm_substream *mark_hw_params;
	struct snd_pcm_substream *mark_trigger;
	struct snd_compr_stream  *mark_compr_startup;

	/* 是否已经 probe。 */
	unsigned int probed:1;

	/* DAI 私有数据。 */
	void *priv;
};

static inline const struct snd_soc_pcm_stream *
snd_soc_dai_get_pcm_stream(const struct snd_soc_dai *dai, int stream)
{
	return (stream == SNDRV_PCM_STREAM_PLAYBACK) ?
		&dai->driver->playback : &dai->driver->capture;
}

#define snd_soc_dai_get_widget_playback(dai)	snd_soc_dai_get_widget(dai, SNDRV_PCM_STREAM_PLAYBACK)
#define snd_soc_dai_get_widget_capture(dai)	snd_soc_dai_get_widget(dai, SNDRV_PCM_STREAM_CAPTURE)
static inline
struct snd_soc_dapm_widget *snd_soc_dai_get_widget(struct snd_soc_dai *dai, int stream)
{
	return dai->stream[stream].widget;
}

#define snd_soc_dai_set_widget_playback(dai, widget)	snd_soc_dai_set_widget(dai, SNDRV_PCM_STREAM_PLAYBACK, widget)
#define snd_soc_dai_set_widget_capture(dai,  widget)	snd_soc_dai_set_widget(dai, SNDRV_PCM_STREAM_CAPTURE,  widget)
static inline
void snd_soc_dai_set_widget(struct snd_soc_dai *dai, int stream, struct snd_soc_dapm_widget *widget)
{
	dai->stream[stream].widget = widget;
}

#define snd_soc_dai_dma_data_get_playback(dai)	snd_soc_dai_dma_data_get(dai, SNDRV_PCM_STREAM_PLAYBACK)
#define snd_soc_dai_dma_data_get_capture(dai)	snd_soc_dai_dma_data_get(dai, SNDRV_PCM_STREAM_CAPTURE)
#define snd_soc_dai_get_dma_data(dai, ss)	snd_soc_dai_dma_data_get(dai, ss->stream)
static inline void *snd_soc_dai_dma_data_get(const struct snd_soc_dai *dai, int stream)
{
	return dai->stream[stream].dma_data;
}

#define snd_soc_dai_dma_data_set_playback(dai, data)	snd_soc_dai_dma_data_set(dai, SNDRV_PCM_STREAM_PLAYBACK, data)
#define snd_soc_dai_dma_data_set_capture(dai,  data)	snd_soc_dai_dma_data_set(dai, SNDRV_PCM_STREAM_CAPTURE,  data)
#define snd_soc_dai_set_dma_data(dai, ss, data)		snd_soc_dai_dma_data_set(dai, ss->stream, data)
static inline void snd_soc_dai_dma_data_set(struct snd_soc_dai *dai, int stream, void *data)
{
	dai->stream[stream].dma_data = data;
}

static inline void snd_soc_dai_init_dma_data(struct snd_soc_dai *dai, void *playback, void *capture)
{
	snd_soc_dai_dma_data_set_playback(dai, playback);
	snd_soc_dai_dma_data_set_capture(dai,  capture);
}

static inline unsigned int snd_soc_dai_tdm_mask_get(const struct snd_soc_dai *dai,
						    int stream)
{
	return dai->stream[stream].tdm_mask;
}

static inline void snd_soc_dai_tdm_mask_set(struct snd_soc_dai *dai, int stream,
					    unsigned int tdm_mask)
{
	dai->stream[stream].tdm_mask = tdm_mask;
}

static inline unsigned int snd_soc_dai_stream_active(const struct snd_soc_dai *dai,
						     int stream)
{
	/* see snd_soc_dai_action() for setup */
	return dai->stream[stream].active;
}

static inline void snd_soc_dai_set_drvdata(struct snd_soc_dai *dai,
		void *data)
{
	dev_set_drvdata(dai->dev, data);
}

static inline void *snd_soc_dai_get_drvdata(struct snd_soc_dai *dai)
{
	return dev_get_drvdata(dai->dev);
}

/**
 * snd_soc_dai_set_stream() - 为 DAI 配置流对象
 * @dai: DAI
 * @stream: 流对象（opaque 结构，具体内容依 DAI 类型而定）
 * @direction: 流方向（播放/录音）
 *
 * 某些子系统，比如 SoundWire，并没有“方向”这个概念，这里就复用
 * ASoC 的流方向来配置 sink/source 端口。播放映射到 source 端口，
 * 录音映射到 sink 端口。
 *
 * 如果传入 NULL，则会清除之前设置的流对象。
 *
 * 返回：成功时返回 0，失败时返回负错误码。
 */
static inline int snd_soc_dai_set_stream(struct snd_soc_dai *dai,
					 void *stream, int direction)
{
	/* 某些总线类型用一个 opaque stream 对象描述 sink/source 端口。 */
	if (dai->driver->ops->set_stream)
		return dai->driver->ops->set_stream(dai, stream, direction);
	else
		return -ENOTSUPP;
}

/**
 * snd_soc_dai_get_stream() - 从 DAI 取回此前配置的流对象
 * @dai: DAI
 * @direction: 流方向（播放/录音）
 *
 * 这个函数只会返回此前通过 snd_soc_dai_set_stream() 配置过的对象。
 *
 * 返回：流对象指针，或者 ERR_PTR 值，例如回调不支持时返回
 * ERR_PTR(-ENOTSUPP)。
 */
static inline void *snd_soc_dai_get_stream(struct snd_soc_dai *dai,
					   int direction)
{
	/* 读回之前配置的 stream 对象。 */
	if (dai->driver->ops->get_stream)
		return dai->driver->ops->get_stream(dai, direction);
	else
		return ERR_PTR(-ENOTSUPP);
}

#endif
