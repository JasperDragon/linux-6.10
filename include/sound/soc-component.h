/* SPDX-License-Identifier: GPL-2.0
 *
 * soc-component.h
 *
 * Copyright (C) 2019 Renesas Electronics Corp.
 * Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 */
#ifndef __SOC_COMPONENT_H
#define __SOC_COMPONENT_H

#include <sound/soc.h>

/*
 * component 的 probe/remove 顺序控制。
 * ASoC 里一个 card 往往同时绑定多个 component，例如 codec、platform、
 * DSP、aux amp 等。这里的 order 用于解决它们之间的运行时依赖。
 */
#define SND_SOC_COMP_ORDER_FIRST	-2
#define SND_SOC_COMP_ORDER_EARLY	-1
#define SND_SOC_COMP_ORDER_NORMAL	 0
#define SND_SOC_COMP_ORDER_LATE		 1
#define SND_SOC_COMP_ORDER_LAST		 2

/* 遍历所有 probe/remove 顺序级别。 */
#define for_each_comp_order(order)		\
	for (order  = SND_SOC_COMP_ORDER_FIRST;	\
	     order <= SND_SOC_COMP_ORDER_LAST;	\
	     order++)

/*
 * 压缩流接口。
 * 当 component 需要提供 compressed offload 支持时，会通过这组回调和
 * 用户态的 compress 接口对接。
 */
struct snd_compress_ops {
	int (*open)(struct snd_soc_component *component,
		    struct snd_compr_stream *stream);
	int (*free)(struct snd_soc_component *component,
		    struct snd_compr_stream *stream);
	int (*set_params)(struct snd_soc_component *component,
			  struct snd_compr_stream *stream,
			  struct snd_compr_params *params);
	int (*get_params)(struct snd_soc_component *component,
			  struct snd_compr_stream *stream,
			  struct snd_codec *params);
	int (*set_metadata)(struct snd_soc_component *component,
			    struct snd_compr_stream *stream,
			    struct snd_compr_metadata *metadata);
	int (*get_metadata)(struct snd_soc_component *component,
			    struct snd_compr_stream *stream,
			    struct snd_compr_metadata *metadata);
	int (*trigger)(struct snd_soc_component *component,
		       struct snd_compr_stream *stream, int cmd);
	int (*pointer)(struct snd_soc_component *component,
		       struct snd_compr_stream *stream,
		       struct snd_compr_tstamp64 *tstamp);
	int (*copy)(struct snd_soc_component *component,
		    struct snd_compr_stream *stream, char __user *buf,
		    size_t count);
	int (*mmap)(struct snd_soc_component *component,
		    struct snd_compr_stream *stream,
		    struct vm_area_struct *vma);
	int (*ack)(struct snd_soc_component *component,
		   struct snd_compr_stream *stream, size_t bytes);
	int (*get_caps)(struct snd_soc_component *component,
			struct snd_compr_stream *stream,
			struct snd_compr_caps *caps);
	int (*get_codec_caps)(struct snd_soc_component *component,
			      struct snd_compr_stream *stream,
			      struct snd_compr_codec_caps *codec);
};

/*
 * component driver 是 ASoC 中“硬件功能块”的抽象。
 * 它描述一个独立硬件单元的：
 * - 控件和 DAPM 图
 * - 寄存器读写
 * - PCM/compress 生命周期
 * - 时钟、PLL、jack、DT 映射等能力
 */
struct snd_soc_component_driver {
	/* component 的逻辑名字，主要用于调试和匹配。 */
	const char *name;

	/* probe 后自动挂上的默认控件和 DAPM 资源。 */
	const struct snd_kcontrol_new *controls;
	unsigned int num_controls;
	const struct snd_soc_dapm_widget *dapm_widgets;
	unsigned int num_dapm_widgets;
	const struct snd_soc_dapm_route *dapm_routes;
	unsigned int num_dapm_routes;

	/* component 生命周期回调。 */
	int (*probe)(struct snd_soc_component *component);
	void (*remove)(struct snd_soc_component *component);
	int (*suspend)(struct snd_soc_component *component);
	int (*resume)(struct snd_soc_component *component);

	/* 寄存器访问入口，通常由 regmap 驱动层转发。 */
	unsigned int (*read)(struct snd_soc_component *component,
			     unsigned int reg);
	int (*write)(struct snd_soc_component *component,
		     unsigned int reg, unsigned int val);

	/* PCM 生命周期钩子。 */
	int (*pcm_new)(struct snd_soc_component *component,
		       struct snd_soc_pcm_runtime *rtd);
	void (*pcm_free)(struct snd_soc_component *component,
			 struct snd_pcm *pcm);

	/* component 级统一控制面：时钟、PLL、jack、DT 映射。 */
	int (*set_sysclk)(struct snd_soc_component *component,
			  int clk_id, int source, unsigned int freq, int dir);
	int (*set_pll)(struct snd_soc_component *component, int pll_id,
		       int source, unsigned int freq_in, unsigned int freq_out);
	int (*set_jack)(struct snd_soc_component *component,
			struct snd_soc_jack *jack,  void *data);
	int (*get_jack_type)(struct snd_soc_component *component);

	/* Device Tree / firmware endpoint 到 DAI 的翻译入口。 */
	int (*of_xlate_dai_name)(struct snd_soc_component *component,
				 const struct of_phandle_args *args,
				 const char **dai_name);
	int (*of_xlate_dai_id)(struct snd_soc_component *comment,
			       struct device_node *endpoint);
	void (*seq_notifier)(struct snd_soc_component *component,
			     enum snd_soc_dapm_type type, int subseq);
	int (*stream_event)(struct snd_soc_component *component, int event);
	int (*set_bias_level)(struct snd_soc_component *component,
			      enum snd_soc_bias_level level);

	int (*open)(struct snd_soc_component *component,
		    struct snd_pcm_substream *substream);
	int (*close)(struct snd_soc_component *component,
		     struct snd_pcm_substream *substream);
	int (*ioctl)(struct snd_soc_component *component,
		     struct snd_pcm_substream *substream,
		     unsigned int cmd, void *arg);
	int (*hw_params)(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *params);
	int (*hw_free)(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream);
	int (*prepare)(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream);
	int (*trigger)(struct snd_soc_component *component,
		       struct snd_pcm_substream *substream, int cmd);
	int (*sync_stop)(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream);
	snd_pcm_uframes_t (*pointer)(struct snd_soc_component *component,
				     struct snd_pcm_substream *substream);
	int (*get_time_info)(struct snd_soc_component *component,
		struct snd_pcm_substream *substream, struct timespec64 *system_ts,
		struct timespec64 *audio_ts,
		struct snd_pcm_audio_tstamp_config *audio_tstamp_config,
		struct snd_pcm_audio_tstamp_report *audio_tstamp_report);
	int (*copy)(struct snd_soc_component *component,
		    struct snd_pcm_substream *substream, int channel,
		    unsigned long pos, struct iov_iter *iter,
		    unsigned long bytes);
	struct page *(*page)(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream,
			     unsigned long offset);
	int (*mmap)(struct snd_soc_component *component,
		    struct snd_pcm_substream *substream,
		    struct vm_area_struct *vma);
	int (*ack)(struct snd_soc_component *component,
		   struct snd_pcm_substream *substream);
	snd_pcm_sframes_t (*delay)(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream);

	const struct snd_compress_ops *compress_ops;

	/* probe/remove 顺序，用于存在依赖关系的多个 component。 */
	int probe_order;
	int remove_order;

	/*
	 * soc_pcm_trigger() 的启动/停止顺序控制。
	 * 相关联的优先级和链路规则也会受 snd_soc_dai_link
	 * 以及 soc_pcm_trigger() 的实现影响。
	 */
	enum snd_soc_trigger_order trigger_start;
	enum snd_soc_trigger_order trigger_stop;

	/*
	 * 打开 PCM 时才拿 module 引用，probe 阶段不持有。
	 * 适合希望 module 在空闲时可卸载的驱动。
	 */
	unsigned int module_get_upon_open:1;

	/* 运行时策略位。 */
	unsigned int idle_bias_on:1;
	unsigned int suspend_bias_off:1;
	unsigned int use_pmdown_time:1; /* care pmdown_time at stop */
	/*
	 * 表示该 component 不关心 PCM 音频数据的字节序。
	 * core 会确保每个被使用的格式同时具备 LE 和 BE 版本。
	 * 这通常是因为 component 后面挂着一个会屏蔽原始字节序的总线，
	 * 例如传输字节序已经定义好的 I2S/SLIMbus/SoundWire；或者像 PDM、
	 * 模拟链路这样根本不存在字节序的场景。
	 */
	unsigned int endianness:1;
	unsigned int legacy_dai_naming:1;

	/* topology 场景下可能忽略 machine driver 的前端 link。 */
	const char *ignore_machine;
	const char *topology_name_prefix;
	int (*be_hw_params_fixup)(struct snd_soc_pcm_runtime *rtd,
				  struct snd_pcm_hw_params *params);
	bool use_dai_pcm_id;	/* use DAI link PCM ID as PCM device number */
	int be_pcm_base;	/* base device ID for all BE PCMs */

#ifdef CONFIG_DEBUG_FS
	const char *debugfs_prefix;
#endif
};

/*
 * 运行时 component 对象。
 * driver 被注册后，内核会为它创建对应的 component；card 绑定时通过
 * 它来完成控制、DAI、PCM 和 DAPM 的实际操作。
 */
struct snd_soc_component {
	/* component 的显示名和前缀。 */
	const char *name;
	const char *name_prefix;
	/* 关联的设备、card 和活跃状态。 */
	struct device *dev;
	struct snd_soc_card *card;

	unsigned int active;

	/* runtime PM 状态。 */
	unsigned int suspended:1; /* is in suspend PM state */

	/* 组件挂入全局列表和 card 列表时使用的节点。 */
	struct list_head list;
	struct list_head card_aux_list; /* for auxiliary bound components */
	struct list_head card_list;

	/* 对应的驱动描述。 */
	const struct snd_soc_component_driver *driver;

	/* 该 component 下注册的 DAI 列表。 */
	struct list_head dai_list;
	int num_dai;

	/* 寄存器缓存。 */
	struct regmap *regmap;

	/* component 级 IO 串行化。 */
	struct mutex io_mutex;

	/* 由 topology / runtime 绑定上的动态对象。 */
	struct list_head dobj_list;

		/*
		 * 驱动代码不要使用下面这些字段，它们只是过渡性的，
		 * 很快会再次被移除。若在驱动里依赖这些字段，等字段移除后
		 * 该驱动会被标记为 BROKEN。
		 */

	struct snd_soc_dapm_context *dapm;

	/* machine specific init */
	int (*init)(struct snd_soc_component *component);

	/* 函数级回滚标记，避免重复释放。 */
	void *mark_module;
	struct snd_pcm_substream *mark_open;
	struct snd_pcm_substream *mark_hw_params;
	struct snd_pcm_substream *mark_trigger;
	struct snd_compr_stream  *mark_compr_open;
	void *mark_pm;

	struct dentry *debugfs_root;
	const char *debugfs_prefix;
};

#define for_each_component_dais(component, dai)\
	list_for_each_entry(dai, &(component)->dai_list, list)
#define for_each_component_dais_safe(component, dai, _dai)\
	list_for_each_entry_safe(dai, _dai, &(component)->dai_list, list)

/**
 * snd_soc_component_to_dapm() - 取得 component 关联的 DAPM 上下文
 * @component: 需要获取 DAPM 上下文的 component
 *
 * 返回：该 component 绑定的 DAPM context。
 */
static inline struct snd_soc_dapm_context *snd_soc_component_to_dapm(
	struct snd_soc_component *component)
{
	return component->dapm;
}

/**
 * snd_soc_component_cache_sync() - 将寄存器缓存同步到硬件
 * @component: 需要同步的 component
 *
 * 注意：该函数会调用 regcache_sync()。
 */
static inline int snd_soc_component_cache_sync(
	struct snd_soc_component *component)
{
	return regcache_sync(component->regmap);
}

void snd_soc_component_set_aux(struct snd_soc_component *component,
			       struct snd_soc_aux_dev *aux);
int snd_soc_component_init(struct snd_soc_component *component);
int snd_soc_component_is_dummy(struct snd_soc_component *component);

/* component 读写 I/O 接口。 */
unsigned int snd_soc_component_read(struct snd_soc_component *component,
				      unsigned int reg);
int snd_soc_component_write(struct snd_soc_component *component,
			    unsigned int reg, unsigned int val);
int snd_soc_component_update_bits(struct snd_soc_component *component,
				  unsigned int reg, unsigned int mask,
				  unsigned int val);
int snd_soc_component_update_bits_async(struct snd_soc_component *component,
					unsigned int reg, unsigned int mask,
					unsigned int val);
void snd_soc_component_async_complete(struct snd_soc_component *component);
int snd_soc_component_test_bits(struct snd_soc_component *component,
				unsigned int reg, unsigned int mask,
				unsigned int value);

unsigned int snd_soc_component_read_field(struct snd_soc_component *component,
					  unsigned int reg, unsigned int mask);
int snd_soc_component_write_field(struct snd_soc_component *component,
				  unsigned int reg, unsigned int mask,
				  unsigned int val);

/* component 级统一控制接口。 */
int snd_soc_component_set_sysclk(struct snd_soc_component *component,
				 int clk_id, int source,
				 unsigned int freq, int dir);
int snd_soc_component_set_pll(struct snd_soc_component *component, int pll_id,
			      int source, unsigned int freq_in,
			      unsigned int freq_out);
int snd_soc_component_set_jack(struct snd_soc_component *component,
			       struct snd_soc_jack *jack, void *data);
int snd_soc_component_get_jack_type(struct snd_soc_component *component);

void snd_soc_component_seq_notifier(struct snd_soc_component *component,
				    enum snd_soc_dapm_type type, int subseq);
int snd_soc_component_stream_event(struct snd_soc_component *component,
				   int event);
int snd_soc_component_set_bias_level(struct snd_soc_component *component,
				     enum snd_soc_bias_level level);

int snd_soc_component_regmap_val_bytes(struct snd_soc_component *component);
#ifdef CONFIG_REGMAP
void snd_soc_component_init_regmap(struct snd_soc_component *component,
				   struct regmap *regmap);
void snd_soc_component_exit_regmap(struct snd_soc_component *component);
#endif

#define snd_soc_component_module_get_when_probe(component)\
	snd_soc_component_module_get(component, NULL, 0)
#define snd_soc_component_module_get_when_open(component, substream)	\
	snd_soc_component_module_get(component, substream, 1)
int snd_soc_component_module_get(struct snd_soc_component *component,
				 void *mark, int upon_open);
#define snd_soc_component_module_put_when_remove(component)	\
	snd_soc_component_module_put(component, NULL, 0, 0)
#define snd_soc_component_module_put_when_close(component, substream, rollback) \
	snd_soc_component_module_put(component, substream, 1, rollback)
void snd_soc_component_module_put(struct snd_soc_component *component,
				  void *mark, int upon_open, int rollback);

static inline void snd_soc_component_set_drvdata(struct snd_soc_component *c,
						 void *data)
{
	dev_set_drvdata(c->dev, data);
}

static inline void *snd_soc_component_get_drvdata(struct snd_soc_component *c)
{
	return dev_get_drvdata(c->dev);
}

static inline unsigned int
snd_soc_component_active(struct snd_soc_component *component)
{
	return component->active;
}

/* component 控制项查询与通知。 */
struct snd_kcontrol *snd_soc_component_get_kcontrol(struct snd_soc_component *component,
						    const char * const ctl);
int snd_soc_component_notify_control(struct snd_soc_component *component,
				     const char * const ctl);

/* component driver 的运行时回调入口。 */
int snd_soc_component_open(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream);
int snd_soc_component_close(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream,
			    int rollback);
void snd_soc_component_suspend(struct snd_soc_component *component);
void snd_soc_component_resume(struct snd_soc_component *component);
int snd_soc_component_is_suspended(struct snd_soc_component *component);
int snd_soc_component_probe(struct snd_soc_component *component);
void snd_soc_component_remove(struct snd_soc_component *component);
int snd_soc_component_of_xlate_dai_id(struct snd_soc_component *component,
				      struct device_node *ep);
int snd_soc_component_of_xlate_dai_name(struct snd_soc_component *component,
					const struct of_phandle_args *args,
					const char **dai_name);
int snd_soc_component_compr_open(struct snd_soc_component *component,
				 struct snd_compr_stream *cstream);
void snd_soc_component_compr_free(struct snd_soc_component *component,
				  struct snd_compr_stream *cstream,
				  int rollback);
int snd_soc_component_compr_trigger(struct snd_compr_stream *cstream, int cmd);
int snd_soc_component_compr_set_params(struct snd_compr_stream *cstream,
				       struct snd_compr_params *params);
int snd_soc_component_compr_get_params(struct snd_compr_stream *cstream,
				       struct snd_codec *params);
int snd_soc_component_compr_get_caps(struct snd_compr_stream *cstream,
				     struct snd_compr_caps *caps);
int snd_soc_component_compr_get_codec_caps(struct snd_compr_stream *cstream,
					   struct snd_compr_codec_caps *codec);
int snd_soc_component_compr_ack(struct snd_compr_stream *cstream, size_t bytes);
int snd_soc_component_compr_pointer(struct snd_compr_stream *cstream,
				    struct snd_compr_tstamp64 *tstamp);
int snd_soc_component_compr_copy(struct snd_compr_stream *cstream,
				 char __user *buf, size_t count);
int snd_soc_component_compr_set_metadata(struct snd_compr_stream *cstream,
					 struct snd_compr_metadata *metadata);
int snd_soc_component_compr_get_metadata(struct snd_compr_stream *cstream,
					 struct snd_compr_metadata *metadata);

int snd_soc_pcm_component_pointer(struct snd_pcm_substream *substream);
int snd_soc_pcm_component_ioctl(struct snd_pcm_substream *substream,
				unsigned int cmd, void *arg);
int snd_soc_pcm_component_sync_stop(struct snd_pcm_substream *substream);
int snd_soc_pcm_component_copy(struct snd_pcm_substream *substream,
			       int channel, unsigned long pos,
			       struct iov_iter *iter, unsigned long bytes);
struct page *snd_soc_pcm_component_page(struct snd_pcm_substream *substream,
					unsigned long offset);
int snd_soc_pcm_component_mmap(struct snd_pcm_substream *substream,
			       struct vm_area_struct *vma);
int snd_soc_pcm_component_new(struct snd_soc_pcm_runtime *rtd);
void snd_soc_pcm_component_free(struct snd_soc_pcm_runtime *rtd);
int snd_soc_pcm_component_prepare(struct snd_pcm_substream *substream);
int snd_soc_pcm_component_hw_params(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params);
void snd_soc_pcm_component_hw_free(struct snd_pcm_substream *substream,
				   int rollback);
int snd_soc_pcm_component_trigger(struct snd_pcm_substream *substream,
				  int cmd, int rollback);
int snd_soc_pcm_component_pm_runtime_get(struct snd_soc_pcm_runtime *rtd,
					 void *stream);
void snd_soc_pcm_component_pm_runtime_put(struct snd_soc_pcm_runtime *rtd,
					  void *stream, int rollback);
int snd_soc_pcm_component_ack(struct snd_pcm_substream *substream);
void snd_soc_pcm_component_delay(struct snd_pcm_substream *substream,
				 snd_pcm_sframes_t *cpu_delay, snd_pcm_sframes_t *codec_delay);

#endif /* __SOC_COMPONENT_H */
