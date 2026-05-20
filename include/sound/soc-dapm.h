/* SPDX-License-Identifier: GPL-2.0
 *
 * linux/sound/soc-dapm.h -- ALSA SoC 动态音频电源管理
 *
 * Author:	Liam Girdwood
 * Created:	Aug 11th 2005
 * Copyright:	Wolfson Microelectronics. PLC.
 */

#ifndef __LINUX_SND_SOC_DAPM_H
#define __LINUX_SND_SOC_DAPM_H

#include <linux/types.h>
#include <sound/control.h>
#include <sound/soc-topology.h>
#include <sound/asoc.h>

struct device;
struct regulator;
struct soc_enum;
struct snd_pcm_substream;
struct snd_soc_pcm_runtime;
struct snd_soc_dapm_context;

/* 该 widget 没有 PM 寄存器位。 */
#define SND_SOC_NOPM	-1

/*
 * DAPM = Dynamic Audio Power Management。
 * 作用是根据“当前真正连通的音频路径”动态开关 codec / path / stream /
 * 板级电源，尽量减少功耗和 pop/click。
 *
 * 常见可理解成四个电源域：
 * 1. Codec 域：VREF/VMID 等基础偏置电源
 * 2. Platform/Machine 域：耳机、喇叭、麦克风、GPIO、regulator 等板级资源
 * 3. Path 域：codec 内部 mixer/mux/pga 路径
 * 4. Stream 域：ADC/DAC、AIF 相关的流域
 */

/* codec 域 widget：表示偏置、基准电压等基础电源状态。 */
#define SND_SOC_DAPM_VMID(wname) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_vmid, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0}

/* platform 域 widget：板级输入输出端口、GPIO、机壳设备等。 */
#define SND_SOC_DAPM_SIGGEN(wname) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_siggen, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM }
#define SND_SOC_DAPM_SINK(wname) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_sink, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM }
#define SND_SOC_DAPM_INPUT(wname) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_input, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM }
#define SND_SOC_DAPM_OUTPUT(wname) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_output, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM }
#define SND_SOC_DAPM_MIC(wname, wevent) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mic, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM, .event = wevent, \
	.event_flags = SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD}
#define SND_SOC_DAPM_HP(wname, wevent) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_hp, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM, .event = wevent, \
	.event_flags = SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD}
#define SND_SOC_DAPM_SPK(wname, wevent) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_spk, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM, .event = wevent, \
	.event_flags = SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD}
#define SND_SOC_DAPM_LINE(wname, wevent) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_line, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM, .event = wevent, \
	.event_flags = SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD}

#define SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert) \
	.reg = wreg, .mask = 1, .shift = wshift, \
	.on_val = winvert ? 0 : 1, .off_val = winvert ? 1 : 0

/* path 域 widget：codec 内部路径中的放大器、混音器、开关。 */
#define SND_SOC_DAPM_PGA(wname, wreg, wshift, winvert,\
	 wcontrols, wncontrols) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_pga, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = wncontrols}
#define SND_SOC_DAPM_OUT_DRV(wname, wreg, wshift, winvert,\
	 wcontrols, wncontrols) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_out_drv, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = wncontrols}
#define SND_SOC_DAPM_MIXER(wname, wreg, wshift, winvert, \
	 wcontrols, wncontrols)\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = wncontrols}
#define SND_SOC_DAPM_MIXER_NAMED_CTL(wname, wreg, wshift, winvert, \
	 wcontrols, wncontrols)\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer_named_ctl, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = wncontrols}
/* DEPRECATED: use SND_SOC_DAPM_SUPPLY */
#define SND_SOC_DAPM_MICBIAS(wname, wreg, wshift, winvert) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_micbias, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = NULL, .num_kcontrols = 0}
#define SND_SOC_DAPM_SWITCH(wname, wreg, wshift, winvert, wcontrols) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_switch, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = 1}
#define SND_SOC_DAPM_MUX(wname, wreg, wshift, winvert, wcontrols) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mux, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = 1}
#define SND_SOC_DAPM_DEMUX(wname, wreg, wshift, winvert, wcontrols) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_demux, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = 1}

/* 简化版宏：默认控件数组长度就是 ARRAY_SIZE(wcontrols)。 */
#define SOC_PGA_ARRAY(wname, wreg, wshift, winvert,\
	 wcontrols) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_pga, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = ARRAY_SIZE(wcontrols)}
#define SOC_MIXER_ARRAY(wname, wreg, wshift, winvert, \
	 wcontrols)\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = ARRAY_SIZE(wcontrols)}
#define SOC_MIXER_NAMED_CTL_ARRAY(wname, wreg, wshift, winvert, \
	 wcontrols)\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer_named_ctl, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = ARRAY_SIZE(wcontrols)}

/* 带事件回调的 path widget。事件回调必须返回 0 表示成功。 */
#define SND_SOC_DAPM_PGA_E(wname, wreg, wshift, winvert, wcontrols, \
	wncontrols, wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_pga, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = wncontrols, \
	.event = wevent, .event_flags = wflags}
#define SND_SOC_DAPM_OUT_DRV_E(wname, wreg, wshift, winvert, wcontrols, \
	wncontrols, wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_out_drv, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = wncontrols, \
	.event = wevent, .event_flags = wflags}
#define SND_SOC_DAPM_MIXER_E(wname, wreg, wshift, winvert, wcontrols, \
	wncontrols, wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = wncontrols, \
	.event = wevent, .event_flags = wflags}
#define SND_SOC_DAPM_MIXER_NAMED_CTL_E(wname, wreg, wshift, winvert, \
	wcontrols, wncontrols, wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, \
	.num_kcontrols = wncontrols, .event = wevent, .event_flags = wflags}
#define SND_SOC_DAPM_SWITCH_E(wname, wreg, wshift, winvert, wcontrols, \
	wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_switch, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = 1, \
	.event = wevent, .event_flags = wflags}
#define SND_SOC_DAPM_MUX_E(wname, wreg, wshift, winvert, wcontrols, \
	wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mux, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = 1, \
	.event = wevent, .event_flags = wflags}

/* 同一类事件下，再细分顺序编号。 */
#define SND_SOC_DAPM_PGA_S(wname, wsubseq, wreg, wshift, winvert, \
	wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_pga, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.event = wevent, .event_flags = wflags, \
	.subseq = wsubseq}
#define SND_SOC_DAPM_SUPPLY_S(wname, wsubseq, wreg, wshift, winvert, wevent, \
	wflags)	\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_supply, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.event = wevent, .event_flags = wflags, .subseq = wsubseq}

/* 上面宏的简化版：默认 wncontrols = ARRAY_SIZE(wcontrols)。 */
#define SOC_PGA_E_ARRAY(wname, wreg, wshift, winvert, wcontrols, \
	wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_pga, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = ARRAY_SIZE(wcontrols), \
	.event = wevent, .event_flags = wflags}
#define SOC_MIXER_E_ARRAY(wname, wreg, wshift, winvert, wcontrols, \
	wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = ARRAY_SIZE(wcontrols), \
	.event = wevent, .event_flags = wflags}
#define SOC_MIXER_NAMED_CTL_E_ARRAY(wname, wreg, wshift, winvert, \
	wcontrols, wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_mixer, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.kcontrol_news = wcontrols, .num_kcontrols = ARRAY_SIZE(wcontrols), \
	.event = wevent, .event_flags = wflags}

/* DAPM 前后置事件类型。 */
#define SND_SOC_DAPM_PRE(wname, wevent) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_pre, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM, .event = wevent, \
	.event_flags = SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD}
#define SND_SOC_DAPM_POST(wname, wevent) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_post, .name = wname, .kcontrol_news = NULL, \
	.num_kcontrols = 0, .reg = SND_SOC_NOPM, .event = wevent, \
	.event_flags = SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD}

/* stream 域 widget：与 AIF/ADC/DAC 等流相关。 */
#define SND_SOC_DAPM_AIF_IN(wname, stname, wchan, wreg, wshift, winvert) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_aif_in, .name = wname, .sname = stname, \
	.channel = wchan, SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), }
#define SND_SOC_DAPM_AIF_IN_E(wname, stname, wchan, wreg, wshift, winvert, \
			      wevent, wflags)				\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_aif_in, .name = wname, .sname = stname, \
	.channel = wchan, SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.event = wevent, .event_flags = wflags }
#define SND_SOC_DAPM_AIF_OUT(wname, stname, wchan, wreg, wshift, winvert) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_aif_out, .name = wname, .sname = stname, \
	.channel = wchan, SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), }
#define SND_SOC_DAPM_AIF_OUT_E(wname, stname, wchan, wreg, wshift, winvert, \
			     wevent, wflags)				\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_aif_out, .name = wname, .sname = stname, \
	.channel = wchan, SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.event = wevent, .event_flags = wflags }
#define SND_SOC_DAPM_DAC(wname, stname, wreg, wshift, winvert) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_dac, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert) }
#define SND_SOC_DAPM_DAC_E(wname, stname, wreg, wshift, winvert, \
			   wevent, wflags)				\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_dac, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.event = wevent, .event_flags = wflags}

#define SND_SOC_DAPM_ADC(wname, stname, wreg, wshift, winvert) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_adc, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), }
#define SND_SOC_DAPM_ADC_E(wname, stname, wreg, wshift, winvert, \
			   wevent, wflags)				\
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_adc, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.event = wevent, .event_flags = wflags}
#define SND_SOC_DAPM_CLOCK_SUPPLY(wname) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_clock_supply, .name = wname, \
	.reg = SND_SOC_NOPM, .event = snd_soc_dapm_clock_event, \
	.event_flags = SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD }

/* 通用 widget：供电、regulator、pinctrl、clock 等。 */
#define SND_SOC_DAPM_REG(wid, wname, wreg, wshift, wmask, won_val, woff_val) \
(struct snd_soc_dapm_widget) { \
	.id = wid, .name = wname, .kcontrol_news = NULL, .num_kcontrols = 0, \
	.reg = wreg, .shift = wshift, .mask = wmask, \
	.on_val = won_val, .off_val = woff_val, }
#define SND_SOC_DAPM_SUPPLY(wname, wreg, wshift, winvert, wevent, wflags) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_supply, .name = wname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), \
	.event = wevent, .event_flags = wflags}
#define SND_SOC_DAPM_REGULATOR_SUPPLY(wname, wdelay, wflags)	    \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_regulator_supply, .name = wname, \
	.reg = SND_SOC_NOPM, .shift = wdelay, .event = snd_soc_dapm_regulator_event, \
	.event_flags = SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD, \
	.on_val = wflags}
#define SND_SOC_DAPM_PINCTRL(wname, active, sleep) \
(struct snd_soc_dapm_widget) { \
	.id = snd_soc_dapm_pinctrl, .name = wname, \
	.priv = (&(struct snd_soc_dapm_pinctrl_priv) \
		{ .active_state = active, .sleep_state = sleep,}), \
	.reg = SND_SOC_NOPM, .event = snd_soc_dapm_pinctrl_event, \
	.event_flags = SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD }



/* DAPM 相关的 kcontrol 宏。 */
#define SOC_DAPM_DOUBLE(xname, reg, lshift, rshift, max, invert) \
	SOC_DOUBLE_EXT(xname, reg, lshift, rshift, max, invert, \
		       snd_soc_dapm_get_volsw, snd_soc_dapm_put_volsw)
#define SOC_DAPM_DOUBLE_R(xname, lreg, rreg, shift, max, invert) \
	SOC_DOUBLE_R_EXT(xname, lreg, rreg, shift, max, invert, \
			 snd_soc_dapm_get_volsw, snd_soc_dapm_put_volsw)
#define SOC_DAPM_SINGLE(xname, reg, shift, max, invert) \
	SOC_SINGLE_EXT(xname, reg, shift, max, invert, \
		       snd_soc_dapm_get_volsw, snd_soc_dapm_put_volsw)
#define SOC_DAPM_SINGLE_VIRT(xname, max) \
	SOC_DAPM_SINGLE(xname, SND_SOC_NOPM, 0, max, 0)
#define SOC_DAPM_DOUBLE_R_TLV(xname, lreg, rreg, shift, max, invert, tlv_array) \
	SOC_DOUBLE_R_EXT_TLV(xname, lreg, rreg, shift, max, invert, \
			     snd_soc_dapm_get_volsw, snd_soc_dapm_put_volsw, \
			     tlv_array)
#define SOC_DAPM_SINGLE_TLV(xname, reg, shift, max, invert, tlv_array) \
	SOC_SINGLE_EXT_TLV(xname, reg, shift, max, invert, \
			   snd_soc_dapm_get_volsw, snd_soc_dapm_put_volsw, \
			   tlv_array)
#define SOC_DAPM_SINGLE_TLV_VIRT(xname, max, tlv_array) \
	SOC_DAPM_SINGLE(xname, SND_SOC_NOPM, 0, max, 0, tlv_array)
#define SOC_DAPM_ENUM(xname, xenum) \
	SOC_ENUM_EXT(xname, xenum, snd_soc_dapm_get_enum_double, \
		     snd_soc_dapm_put_enum_double)
#define SOC_DAPM_ENUM_EXT(xname, xenum, xget, xput) \
	SOC_ENUM_EXT(xname, xenum, xget, xput)

#define SOC_DAPM_SINGLE_AUTODISABLE(xname, reg, shift, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_dapm_get_volsw, .put = snd_soc_dapm_put_volsw, \
	.private_value = SOC_SINGLE_VALUE(reg, shift, 0, max, invert, 1) }
#define SOC_DAPM_SINGLE_TLV_AUTODISABLE(xname, reg, shift, max, invert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.get = snd_soc_dapm_get_volsw, .put = snd_soc_dapm_put_volsw, \
	.private_value = SOC_SINGLE_VALUE(reg, shift, 0, max, invert, 1) }
#define SOC_DAPM_PIN_SWITCH(xname) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname " Switch", \
	.info = snd_soc_dapm_info_pin_switch, \
	.get = snd_soc_dapm_get_pin_switch, \
	.put = snd_soc_dapm_put_pin_switch, \
	.private_value = (unsigned long)xname }

/* DAPM 在流启动/停止/挂起/恢复时使用的状态标记。 */
#define SND_SOC_DAPM_STREAM_NOP			0x0
#define SND_SOC_DAPM_STREAM_START		0x1
#define SND_SOC_DAPM_STREAM_STOP		0x2
#define SND_SOC_DAPM_STREAM_SUSPEND		0x4
#define SND_SOC_DAPM_STREAM_RESUME		0x8
#define SND_SOC_DAPM_STREAM_PAUSE_PUSH		0x10
#define SND_SOC_DAPM_STREAM_PAUSE_RELEASE	0x20

/* DAPM 事件触发时机。 */
#define SND_SOC_DAPM_PRE_PMU		0x1	/* before widget power up */
#define SND_SOC_DAPM_POST_PMU		0x2	/* after  widget power up */
#define SND_SOC_DAPM_PRE_PMD		0x4	/* before widget power down */
#define SND_SOC_DAPM_POST_PMD		0x8	/* after  widget power down */
#define SND_SOC_DAPM_PRE_REG		0x10	/* before audio path setup */
#define SND_SOC_DAPM_POST_REG		0x20	/* after  audio path setup */
#define SND_SOC_DAPM_WILL_PMU		0x40	/* called at start of sequence */
#define SND_SOC_DAPM_WILL_PMD		0x80	/* called at start of sequence */
#define SND_SOC_DAPM_PRE_POST_PMD	(SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD)
#define SND_SOC_DAPM_PRE_POST_PMU	(SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU)

/* 便捷判断事件是上电还是下电。 */
#define SND_SOC_DAPM_EVENT_ON(e)	(e & (SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU))
#define SND_SOC_DAPM_EVENT_OFF(e)	(e & (SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD))

/* regulator widget 的附加标志。 */
#define SND_SOC_DAPM_REGULATOR_BYPASS	0x1	/* bypass when disabled */

/*
 * Bias level 表示 codec 当前的偏置/电源状态。
 * 这是 DAPM 的核心状态机之一，控制 codec 从关闭到完全工作的切换。
 */
enum snd_soc_bias_level {
	SND_SOC_BIAS_OFF = 0,
	SND_SOC_BIAS_STANDBY = 1,
	SND_SOC_BIAS_PREPARE = 2,
	SND_SOC_BIAS_ON = 3,
};

/*
 * DAPM widget 类型。
 * 每一种类型对应音频路径中的一个节点，例如输入、输出、ADC、DAC、
 * mixer、mux、supply、clock、AIF 等。
 */
enum snd_soc_dapm_type {
	snd_soc_dapm_input = 0,		/* input pin */
	snd_soc_dapm_output,		/* output pin */
	snd_soc_dapm_mux,		/* selects 1 analog signal from many inputs */
	snd_soc_dapm_mux_named_ctl,	/* mux with named controls */
	snd_soc_dapm_demux,		/* connects the input to one of multiple outputs */
	snd_soc_dapm_mixer,		/* mixes several analog signals together */
	snd_soc_dapm_mixer_named_ctl,	/* mixer with named controls */
	snd_soc_dapm_pga,		/* programmable gain/attenuation (volume) */
	snd_soc_dapm_out_drv,		/* output driver */
	snd_soc_dapm_adc,		/* analog to digital converter */
	snd_soc_dapm_dac,		/* digital to analog converter */
	snd_soc_dapm_micbias,		/* microphone bias (power) - DEPRECATED: use snd_soc_dapm_supply */
	snd_soc_dapm_mic,		/* microphone */
	snd_soc_dapm_hp,		/* headphones */
	snd_soc_dapm_spk,		/* speaker */
	snd_soc_dapm_line,		/* line input/output */
	snd_soc_dapm_switch,		/* analog switch */
	snd_soc_dapm_vmid,		/* codec bias/vmid - to minimise pops */
	snd_soc_dapm_pre,		/* machine specific pre widget - exec first */
	snd_soc_dapm_post,		/* machine specific post widget - exec last */
	snd_soc_dapm_supply,		/* power/clock supply */
	snd_soc_dapm_pinctrl,		/* pinctrl */
	snd_soc_dapm_regulator_supply,	/* external regulator */
	snd_soc_dapm_clock_supply,	/* external clock */
	snd_soc_dapm_aif_in,		/* audio interface input */
	snd_soc_dapm_aif_out,		/* audio interface output */
	snd_soc_dapm_siggen,		/* signal generator */
	snd_soc_dapm_sink,
	snd_soc_dapm_dai_in,		/* link to DAI structure */
	snd_soc_dapm_dai_out,
	snd_soc_dapm_dai_link,		/* link between two DAI structures */
	snd_soc_dapm_kcontrol,		/* Auto-disabled kcontrol */
	snd_soc_dapm_buffer,		/* DSP/CODEC internal buffer */
	snd_soc_dapm_scheduler,		/* DSP/CODEC internal scheduler */
	snd_soc_dapm_effect,		/* DSP/CODEC effect component */
	snd_soc_dapm_src,		/* DSP/CODEC SRC component */
	snd_soc_dapm_asrc,		/* DSP/CODEC ASRC component */
	snd_soc_dapm_encoder,		/* FW/SW audio encoder component */
	snd_soc_dapm_decoder,		/* FW/SW audio decoder component */

	/* Don't edit below this line */
	SND_SOC_DAPM_TYPE_COUNT
};

/*
 * DAPM route。
 * 定义 source -> sink 的音频连线，可选 control 用于描述受哪个控件控制。
 */
struct snd_soc_dapm_route {
	/* 路由的 sink/source/control 名称都来自 topology 或 machine driver。 */
	const char *sink;
	const char *control;
	const char *source;

	/* 仅在供电类路径上支持的动态连通判定回调。 */
	int (*connected)(struct snd_soc_dapm_widget *source,
			 struct snd_soc_dapm_widget *sink);

	/* route 对象的拓扑动态标记。 */
	struct snd_soc_dobj dobj;
};

/* 两个 widget 之间的实际运行时路径。 */
struct snd_soc_dapm_path {
	/* 路径名，通常对应 control 或 route 名。 */
	const char *name;

	/*
	 * source（输入）和 sink（输出）widget。
	 * 这里用 union 只是为了更方便书写，直接访问 p->source
	 * 比 p->node[SND_SOC_DAPM_DIR_IN] 更直观。
	 */
	union {
		struct {
			struct snd_soc_dapm_widget *source;
			struct snd_soc_dapm_widget *sink;
		};
		struct snd_soc_dapm_widget *node[2];
	};

	/* status */
	u32 connect:1;		/* source 和 sink 当前是否连通 */
	u32 walking:1;		/* 正在被 DAPM 遍历，避免重复处理 */
	u32 is_supply:1;	/* 至少一端是 supply widget */

	int (*connected)(struct snd_soc_dapm_widget *source,
			 struct snd_soc_dapm_widget *sink);

	/* 分别挂到 source/sink 的边表中。 */
	struct list_head list_node[2];
	/* 与该 path 关联的 kcontrol 链表。 */
	struct list_head list_kcontrol;
	/* card->paths 全局链表节点。 */
	struct list_head list;
};

/* dapm widget */
struct snd_soc_dapm_widget {
	/* widget 类型和名字。 */
	enum snd_soc_dapm_type id;
	const char *name;			/* widget name */
	const char *sname;			/* stream name */
	struct list_head list;
	struct snd_soc_dapm_context *dapm;

	/* widget 私有数据和外部资源句柄。 */
	void *priv;				/* widget specific data */
	struct regulator *regulator;		/* attached regulator */
	struct pinctrl *pinctrl;		/* attached pinctrl */

	/* dapm control */
	/* 寄存器和位段定义。 */
	int reg;				/* negative reg = no direct dapm */
	unsigned char shift;			/* bits to shift */
	unsigned int mask;			/* non-shifted mask */
	unsigned int on_val;			/* on state value */
	unsigned int off_val;			/* off state value */
	unsigned char power:1;			/* 当前是否上电 */
	unsigned char active:1;			/* 当前是否有活跃流 */
	unsigned char connected:1;		/* 连接点是否有效 */
	unsigned char new:1;			/* widget 是否已完整构建 */
	unsigned char force:1;			/* 强制保持状态 */
	unsigned char ignore_suspend:1;		/* suspend 时保持启用 */
	unsigned char new_power:1;		/* 本轮计算出的电源状态 */
	unsigned char power_checked:1;		/* 本轮是否已检查过 power */
	unsigned char is_supply:1;		/* 是否为供电节点 */
	unsigned char is_ep:2;			/* 是否为端点节点 */
	unsigned char no_wname_in_kcontrol_name:1; /* kcontrol 名称里不加 widget 前缀 */
	int subseq;				/* sort within widget type */

	int (*power_check)(struct snd_soc_dapm_widget *w);

	/* external events */
	unsigned short event_flags;		/* 触发哪些 power 事件 */
	int (*event)(struct snd_soc_dapm_widget*, struct snd_kcontrol *, int);

	/* 与该 widget 关联的 kcontrol。 */
	int num_kcontrols;
	const struct snd_kcontrol_new *kcontrol_news;
	struct snd_kcontrol **kcontrols;
	struct snd_soc_dobj dobj;

	/* 输入和输出边。 */
	struct list_head edges[2];

	/* DAPM 更新过程中的工作队列。 */
	struct list_head work_list;
	struct list_head power_list;
	struct list_head dirty;
	int endpoints[2];

	/* 额外时钟和通道号。 */
	struct clk *clk;

	int channel;
};

struct snd_soc_dapm_update {
	struct snd_kcontrol *kcontrol;
	int reg;
	int mask;
	int val;
	int reg2;
	int mask2;
	int val2;
	bool has_second_set;
};

/* A list of widgets associated with an object, typically a snd_kcontrol */
struct snd_soc_dapm_widget_list {
	/* widget 列表长度。 */
	int num_widgets;
	/* 变长数组，保存被连接到同一对象上的所有 widget。 */
	struct snd_soc_dapm_widget *widgets[] __counted_by(num_widgets);
};

struct snd_soc_dapm_stats {
	int power_checks;
	int path_checks;
	int neighbour_checks;
};

struct snd_soc_dapm_pinctrl_priv {
	const char *active_state;
	const char *sleep_state;
};

enum snd_soc_dapm_direction {
	SND_SOC_DAPM_DIR_IN,
	SND_SOC_DAPM_DIR_OUT
};

#define SND_SOC_DAPM_DIR_TO_EP(x) BIT(x)

#define SND_SOC_DAPM_EP_SOURCE	SND_SOC_DAPM_DIR_TO_EP(SND_SOC_DAPM_DIR_IN)
#define SND_SOC_DAPM_EP_SINK	SND_SOC_DAPM_DIR_TO_EP(SND_SOC_DAPM_DIR_OUT)

struct snd_soc_dapm_context *snd_soc_dapm_alloc(struct device *dev);

int snd_soc_dapm_regulator_event(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event);
int snd_soc_dapm_clock_event(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event);
int snd_soc_dapm_pinctrl_event(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event);

/* dapm controls */
int snd_soc_dapm_put_volsw(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol);
int snd_soc_dapm_get_volsw(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol);
int snd_soc_dapm_get_enum_double(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol);
int snd_soc_dapm_put_enum_double(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol);
int snd_soc_dapm_info_pin_switch(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo);
int snd_soc_dapm_get_pin_switch(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *uncontrol);
int snd_soc_dapm_put_pin_switch(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *uncontrol);
int snd_soc_dapm_get_component_pin_switch(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *uncontrol);
int snd_soc_dapm_put_component_pin_switch(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *uncontrol);
int snd_soc_dapm_new_controls(struct snd_soc_dapm_context *dapm,
	const struct snd_soc_dapm_widget *widget, unsigned int num);
struct snd_soc_dapm_widget *snd_soc_dapm_new_control(struct snd_soc_dapm_context *dapm,
		const struct snd_soc_dapm_widget *widget);
struct snd_soc_dapm_widget *snd_soc_dapm_new_control_unlocked(struct snd_soc_dapm_context *dapm,
		const struct snd_soc_dapm_widget *widget);
int snd_soc_dapm_new_dai_widgets(struct snd_soc_dapm_context *dapm, struct snd_soc_dai *dai);
void snd_soc_dapm_free_widget(struct snd_soc_dapm_widget *w);
int snd_soc_dapm_link_dai_widgets(struct snd_soc_card *card);
void snd_soc_dapm_connect_dai_link_widgets(struct snd_soc_card *card);

int snd_soc_dapm_update_dai(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params, struct snd_soc_dai *dai);
int snd_soc_dapm_widget_name_cmp(struct snd_soc_dapm_widget *widget, const char *s);
struct device *snd_soc_dapm_to_dev(struct snd_soc_dapm_context *dapm);
struct snd_soc_card *snd_soc_dapm_to_card(struct snd_soc_dapm_context *dapm);
struct snd_soc_component *snd_soc_dapm_to_component(struct snd_soc_dapm_context *dapm);

bool snd_soc_dapm_get_idle_bias(struct snd_soc_dapm_context *dapm);
void snd_soc_dapm_set_idle_bias(struct snd_soc_dapm_context *dapm, bool on);

/* dapm path setup */
int snd_soc_dapm_new_widgets(struct snd_soc_card *card);
void snd_soc_dapm_free(struct snd_soc_dapm_context *dapm);
void snd_soc_dapm_init(struct snd_soc_dapm_context *dapm,
		       struct snd_soc_card *card, struct snd_soc_component *component);
int snd_soc_dapm_add_routes(struct snd_soc_dapm_context *dapm,
			    const struct snd_soc_dapm_route *route, int num);
int snd_soc_dapm_del_routes(struct snd_soc_dapm_context *dapm,
			    const struct snd_soc_dapm_route *route, int num);
void snd_soc_dapm_free_widget(struct snd_soc_dapm_widget *w);

/* dapm events */
void snd_soc_dapm_stream_event(struct snd_soc_pcm_runtime *rtd, int stream, int event);
void snd_soc_dapm_stream_stop(struct snd_soc_pcm_runtime *rtd, int stream);
void snd_soc_dapm_shutdown(struct snd_soc_card *card);

/* 外部 DAPM widget 事件。 */
int snd_soc_dapm_mixer_update_power(struct snd_soc_dapm_context *dapm,
		struct snd_kcontrol *kcontrol, int connect, struct snd_soc_dapm_update *update);
int snd_soc_dapm_mux_update_power(struct snd_soc_dapm_context *dapm,
		struct snd_kcontrol *kcontrol, int mux, struct soc_enum *e,
		struct snd_soc_dapm_update *update);

/* DAPM sysfs 接口，由 core 使用。 */
extern struct attribute *snd_soc_dapm_dev_attrs[];
void snd_soc_dapm_debugfs_init(struct snd_soc_dapm_context *dapm, struct dentry *parent);

/* DAPM 音频 pin 的控制与状态。 */
int snd_soc_dapm_enable_pin(struct snd_soc_dapm_context *dapm, const char *pin);
int snd_soc_dapm_enable_pin_unlocked(struct snd_soc_dapm_context *dapm, const char *pin);
int snd_soc_dapm_disable_pin(struct snd_soc_dapm_context *dapm, const char *pin);
int snd_soc_dapm_disable_pin_unlocked(struct snd_soc_dapm_context *dapm, const char *pin);
int snd_soc_dapm_get_pin_status(struct snd_soc_dapm_context *dapm, const char *pin);
int snd_soc_dapm_sync(struct snd_soc_dapm_context *dapm);
int snd_soc_dapm_sync_unlocked(struct snd_soc_dapm_context *dapm);
int snd_soc_dapm_force_enable_pin(struct snd_soc_dapm_context *dapm, const char *pin);
int snd_soc_dapm_force_enable_pin_unlocked(struct snd_soc_dapm_context *dapm, const char *pin);
int snd_soc_dapm_ignore_suspend(struct snd_soc_dapm_context *dapm, const char *pin);
void snd_soc_dapm_mark_endpoints_dirty(struct snd_soc_card *card);

/* DAPM 路径查询接口。 */
int snd_soc_dapm_dai_get_connected_widgets(struct snd_soc_dai *dai, int stream,
	struct snd_soc_dapm_widget_list **list,
	bool (*custom_stop_condition)(struct snd_soc_dapm_widget *, enum snd_soc_dapm_direction));
void snd_soc_dapm_dai_free_widgets(struct snd_soc_dapm_widget_list **list);

struct snd_soc_dapm_context *snd_soc_dapm_kcontrol_to_dapm(struct snd_kcontrol *kcontrol);
struct snd_soc_dapm_widget *snd_soc_dapm_kcontrol_to_widget(struct snd_kcontrol *kcontrol);
struct snd_soc_component *snd_soc_dapm_kcontrol_to_component(struct snd_kcontrol *kcontrol);
unsigned int snd_soc_dapm_kcontrol_get_value(const struct snd_kcontrol *kcontrol);

int snd_soc_dapm_force_bias_level(struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level);
enum snd_soc_bias_level snd_soc_dapm_get_bias_level(struct snd_soc_dapm_context *dapm);
void snd_soc_dapm_init_bias_level(struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level);

#define for_each_dapm_widgets(list, i, widget)				\
	for ((i) = 0;							\
	     (i) < list->num_widgets && (widget = list->widgets[i]);	\
	     (i)++)

/**
 * snd_soc_dapm_widget_for_each_path - 遍历 widget 指定方向上的所有路径
 * @w: widget
 * @dir: 遍历方向，决定该 widget 是路径的输入端还是输出端
 * @p: 路径迭代变量
 */
#define snd_soc_dapm_widget_for_each_path(w, dir, p) \
	list_for_each_entry(p, &w->edges[dir], list_node[dir])

/**
 * snd_soc_dapm_widget_for_each_path_safe - 遍历 widget 指定方向上的所有路径
 * @w: widget
 * @dir: 遍历方向，决定该 widget 是路径的输入端还是输出端
 * @p: 路径迭代变量
 * @next_p: 下一条路径的临时保存变量
 *
 * 这个版本与 snd_soc_dapm_widget_for_each_path 类似，但允许在遍历时
 * 安全删除当前路径。
 */
#define snd_soc_dapm_widget_for_each_path_safe(w, dir, p, next_p) \
	list_for_each_entry_safe(p, next_p, &w->edges[dir], list_node[dir])

/**
 * snd_soc_dapm_widget_for_each_sink_path - 遍历从 widget 发出的所有路径
 * @w: widget
 * @p: 路径迭代变量
 */
#define snd_soc_dapm_widget_for_each_sink_path(w, p) \
	snd_soc_dapm_widget_for_each_path(w, SND_SOC_DAPM_DIR_IN, p)

/**
 * snd_soc_dapm_widget_for_each_source_path - 遍历指向 widget 的所有路径
 * @w: widget
 * @p: 路径迭代变量
 */
#define snd_soc_dapm_widget_for_each_source_path(w, p) \
	snd_soc_dapm_widget_for_each_path(w, SND_SOC_DAPM_DIR_OUT, p)

#endif
