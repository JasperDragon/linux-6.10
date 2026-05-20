/* SPDX-License-Identifier: GPL-2.0
 *
 * soc-jack.h
 *
 * Copyright (C) 2019 Renesas Electronics Corp.
 * Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 */
#ifndef __SOC_JACK_H
#define __SOC_JACK_H

/*
 * jack 相关的公共定义。
 * 这一层把插拔检测结果映射到 pin、zone 和 GPIO 事件，供 card / codec
 * driver 统一消费。
 */

/**
 * struct snd_soc_jack_pin - 根据 jack 检测结果需要联动更新的 pin
 *
 * 这个结构体描述“jack 检测结果会影响哪些 pin”。
 * 常见场景是耳机插入后打开某个输出 pin，或插入麦克风后切换输入路径。
 */
struct snd_soc_jack_pin {
	struct list_head list;
	const char *pin;
	int mask;
	bool invert;
};

/**
 * struct snd_soc_jack_zone - jack 电压分区
 *
 * 这个结构体用于“电压分区识别”。
 * 很多 codec 会通过 micbias 电压或 ADC 采样值区分耳机、麦克风、
 * 按键耳机等不同器件。
 */
struct snd_soc_jack_zone {
	unsigned int min_mv;
	unsigned int max_mv;
	unsigned int jack_type;
	unsigned int debounce_time;
	struct list_head list;
};

/**
 * struct snd_soc_jack_gpio - 用于 jack 检测的 GPIO 描述
 *
 * 这个结构体把 GPIO 级插拔检测接到 jack 框架里。
 * 适合没有专用 jack detect 引脚、但板级 GPIO 能提供插拔状态的场景。
 */
struct snd_soc_jack_gpio {
	unsigned int idx;
	struct device *gpiod_dev;
	const char *name;
	int report;
	int invert;
	int debounce_time;
	bool wake;

	/* private: */
	struct snd_soc_jack *jack;
	struct delayed_work work;
	struct notifier_block pm_notifier;
	struct gpio_desc *desc;

	void *data;
	/* public: */
	int (*jack_status_check)(void *data);
};

struct snd_soc_jack {
	/* 保护 jack 状态、pin 列表和通知链的互斥锁。 */
	struct mutex mutex;
	struct snd_jack *jack;
	struct snd_soc_card *card;
	/* 与 jack 绑定的 pin 列表和电压区间列表。 */
	struct list_head pins;
	int status;
	/* jack 状态变更的 blocking notifier。 */
	struct blocking_notifier_head notifier;
	struct list_head jack_zones;
};

/* jack 的状态上报、pin 绑定、通知注册、GPIO 绑定等 API。 */
void snd_soc_jack_report(struct snd_soc_jack *jack, int status, int mask);
int snd_soc_jack_add_pins(struct snd_soc_jack *jack, int count,
			  struct snd_soc_jack_pin *pins);
void snd_soc_jack_notifier_register(struct snd_soc_jack *jack,
				    struct notifier_block *nb);
void snd_soc_jack_notifier_unregister(struct snd_soc_jack *jack,
				      struct notifier_block *nb);
int snd_soc_jack_add_zones(struct snd_soc_jack *jack, int count,
			   struct snd_soc_jack_zone *zones);
int snd_soc_jack_get_type(struct snd_soc_jack *jack, int micbias_voltage);
#ifdef CONFIG_GPIOLIB
int snd_soc_jack_add_gpios(struct snd_soc_jack *jack, int count,
			   struct snd_soc_jack_gpio *gpios);
int snd_soc_jack_add_gpiods(struct device *gpiod_dev,
			    struct snd_soc_jack *jack,
			    int count, struct snd_soc_jack_gpio *gpios);
void snd_soc_jack_free_gpios(struct snd_soc_jack *jack, int count,
			     struct snd_soc_jack_gpio *gpios);
#else
static inline int snd_soc_jack_add_gpios(struct snd_soc_jack *jack, int count,
					 struct snd_soc_jack_gpio *gpios)
{
	return 0;
}

static inline int snd_soc_jack_add_gpiods(struct device *gpiod_dev,
					  struct snd_soc_jack *jack,
					  int count,
					  struct snd_soc_jack_gpio *gpios)
{
	return 0;
}

static inline void snd_soc_jack_free_gpios(struct snd_soc_jack *jack, int count,
					   struct snd_soc_jack_gpio *gpios)
{
}
#endif

#endif /* __SOC_JACK_H */
