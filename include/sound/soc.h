/* SPDX-License-Identifier: GPL-2.0
 *
 * linux/sound/soc.h -- ALSA SoC Layer
 *
 * Author:	Liam Girdwood
 * Created:	Aug 11th 2005
 * Copyright:	Wolfson Microelectronics. PLC.
 */

#ifndef __LINUX_SND_SOC_H
#define __LINUX_SND_SOC_H

#include <linux/args.h>
#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/lockdep.h>
#include <linux/log2.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <sound/ac97_codec.h>
#include <sound/compress_driver.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/pcm.h>

struct module;
struct platform_device;

/* For the current users of sound/soc.h to avoid build issues */
#include <linux/platform_device.h>
#include <linux/regmap.h>

/*
 * 这些宏用于快速构造 ASoC 的 kcontrol 描述。
 *
 * ASoC 里的很多 mixer / enum / bytes 控件并不是直接手写一套
 * get/put/info 函数，而是把寄存器地址、bit 偏移、取值范围、
 * 是否反向、是否自动 disable 等信息编码进 private_value，
 * 再交给通用回调解释。
 *
 * 这样做的结果是：驱动侧只需要声明“这是什么控制”，而不用为
 * 每个寄存器重复实现一套样板操作。
 */
#define SOC_DOUBLE_S_VALUE(xreg, shift_left, shift_right, xmin, xmax, xsign_bit, \
			   xinvert, xautodisable) \
	((unsigned long)&(struct soc_mixer_control) \
	{.reg = xreg, .rreg = xreg, .shift = shift_left, \
	.rshift = shift_right, .min = xmin, .max = xmax, \
	.sign_bit = xsign_bit, .invert = xinvert, .autodisable = xautodisable})
#define SOC_DOUBLE_VALUE(xreg, shift_left, shift_right, xmin, xmax, xinvert, xautodisable) \
	SOC_DOUBLE_S_VALUE(xreg, shift_left, shift_right, xmin, xmax, 0, xinvert, \
			   xautodisable)
#define SOC_SINGLE_VALUE(xreg, xshift, xmin, xmax, xinvert, xautodisable) \
	SOC_DOUBLE_VALUE(xreg, xshift, xshift, xmin, xmax, xinvert, xautodisable)
#define SOC_DOUBLE_R_S_VALUE(xlreg, xrreg, xshift, xmin, xmax, xsign_bit, xinvert) \
	((unsigned long)&(struct soc_mixer_control) \
	{.reg = xlreg, .rreg = xrreg, .shift = xshift, .rshift = xshift, \
	.max = xmax, .min = xmin, .sign_bit = xsign_bit, \
	.invert = xinvert})
#define SOC_DOUBLE_R_VALUE(xlreg, xrreg, xshift, xmin, xmax, xinvert) \
	SOC_DOUBLE_R_S_VALUE(xlreg, xrreg, xshift, xmin, xmax, 0, xinvert)

#define SOC_SINGLE(xname, reg, shift, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
	.put = snd_soc_put_volsw, \
	.private_value = SOC_SINGLE_VALUE(reg, shift, 0, max, invert, 0) }
#define SOC_SINGLE_RANGE(xname, xreg, xshift, xmin, xmax, xinvert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw, \
	.put = snd_soc_put_volsw, \
	.private_value = SOC_SINGLE_VALUE(xreg, xshift, xmin, xmax, xinvert, 0) }
#define SOC_SINGLE_TLV(xname, reg, shift, max, invert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
	.put = snd_soc_put_volsw, \
	.private_value = SOC_SINGLE_VALUE(reg, shift, 0, max, invert, 0) }
#define SOC_SINGLE_SX_TLV(xname, xreg, xshift, xmin, xmax, tlv_array) \
{       .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
	SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p  = (tlv_array),\
	.info = snd_soc_info_volsw_sx, \
	.get = snd_soc_get_volsw_sx,\
	.put = snd_soc_put_volsw_sx, \
	.private_value = SOC_SINGLE_VALUE(xreg, xshift, xmin, xmax, 0, 0) }
#define SOC_SINGLE_RANGE_TLV(xname, xreg, xshift, xmin, xmax, xinvert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw, .put = snd_soc_put_volsw, \
	.private_value = SOC_SINGLE_VALUE(xreg, xshift, xmin, xmax, xinvert, 0) }
#define SOC_DOUBLE(xname, reg, shift_left, shift_right, max, invert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw, \
	.put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_VALUE(reg, shift_left, shift_right, \
					  0, max, invert, 0) }
#define SOC_DOUBLE_STS(xname, reg, shift_left, shift_right, max, invert) \
{									\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),		\
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,		\
	.access = SNDRV_CTL_ELEM_ACCESS_READ |				\
		SNDRV_CTL_ELEM_ACCESS_VOLATILE,				\
	.private_value = SOC_DOUBLE_VALUE(reg, shift_left, shift_right,	\
					  0, max, invert, 0) }
#define SOC_DOUBLE_R(xname, reg_left, reg_right, xshift, xmax, xinvert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw, .put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_R_VALUE(reg_left, reg_right, xshift, \
					    0, xmax, xinvert) }
#define SOC_DOUBLE_R_RANGE(xname, reg_left, reg_right, xshift, xmin, \
			   xmax, xinvert)		\
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw, .put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_R_VALUE(reg_left, reg_right, \
					    xshift, xmin, xmax, xinvert) }
#define SOC_DOUBLE_TLV(xname, reg, shift_left, shift_right, max, invert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw, \
	.put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_VALUE(reg, shift_left, shift_right, \
					  0, max, invert, 0) }
#define SOC_DOUBLE_SX_TLV(xname, xreg, shift_left, shift_right, xmin, xmax, tlv_array) \
{       .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
	SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p  = (tlv_array), \
	.info = snd_soc_info_volsw_sx, \
	.get = snd_soc_get_volsw_sx, \
	.put = snd_soc_put_volsw_sx, \
	.private_value = SOC_DOUBLE_VALUE(xreg, shift_left, shift_right, \
					  xmin, xmax, 0, 0) }
#define SOC_DOUBLE_RANGE_TLV(xname, xreg, xshift_left, xshift_right, xmin, xmax, \
			     xinvert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		  SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw, .put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_VALUE(xreg, xshift_left, xshift_right, \
					  xmin, xmax, xinvert, 0) }
#define SOC_DOUBLE_R_TLV(xname, reg_left, reg_right, xshift, xmax, xinvert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw, .put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_R_VALUE(reg_left, reg_right, xshift, \
					    0, xmax, xinvert) }
#define SOC_DOUBLE_R_RANGE_TLV(xname, reg_left, reg_right, xshift, xmin, \
			       xmax, xinvert, tlv_array)		\
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw, .put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_R_VALUE(reg_left, reg_right, \
					    xshift, xmin, xmax, xinvert) }
#define SOC_DOUBLE_R_SX_TLV(xname, xreg, xrreg, xshift, xmin, xmax, tlv_array) \
{       .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
	SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p  = (tlv_array), \
	.info = snd_soc_info_volsw_sx, \
	.get = snd_soc_get_volsw_sx, \
	.put = snd_soc_put_volsw_sx, \
	.private_value = SOC_DOUBLE_R_VALUE(xreg, xrreg, xshift, xmin, xmax, 0) }
#define SOC_DOUBLE_R_S_TLV(xname, reg_left, reg_right, xshift, xmin, xmax, xsign_bit, xinvert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw, .put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_R_S_VALUE(reg_left, reg_right, xshift, \
					    xmin, xmax, xsign_bit, xinvert) }
#define SOC_SINGLE_S_TLV(xname, xreg, xshift, xmin, xmax, xsign_bit, xinvert, tlv_array) \
	SOC_DOUBLE_R_S_TLV(xname, xreg, xreg, xshift, xmin, xmax, xsign_bit, xinvert, tlv_array)
#define SOC_SINGLE_S8_TLV(xname, xreg, xmin, xmax, tlv_array) \
{	.iface  = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		  SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p  = (tlv_array), \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
	.put = snd_soc_put_volsw, \
	.private_value = (unsigned long)&(struct soc_mixer_control) \
	{.reg = xreg, .rreg = xreg,  \
	 .min = xmin, .max = xmax, \
	.sign_bit = 7,} }
#define SOC_DOUBLE_S8_TLV(xname, xreg, xmin, xmax, tlv_array) \
{	.iface  = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		  SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p  = (tlv_array), \
	.info = snd_soc_info_volsw, .get = snd_soc_get_volsw,\
	.put = snd_soc_put_volsw, \
	.private_value = SOC_DOUBLE_S_VALUE(xreg, 0, 8, xmin, xmax, 7, 0, 0) }
#define SOC_ENUM_DOUBLE(xreg, xshift_l, xshift_r, xitems, xtexts) \
{	.reg = xreg, .shift_l = xshift_l, .shift_r = xshift_r, \
	.items = xitems, .texts = xtexts, \
	.mask = xitems ? roundup_pow_of_two(xitems) - 1 : 0}
#define SOC_ENUM_SINGLE(xreg, xshift, xitems, xtexts) \
	SOC_ENUM_DOUBLE(xreg, xshift, xshift, xitems, xtexts)
#define SOC_ENUM_SINGLE_EXT(xitems, xtexts) \
{	.items = xitems, .texts = xtexts }
#define SOC_VALUE_ENUM_DOUBLE(xreg, xshift_l, xshift_r, xmask, xitems, xtexts, xvalues) \
{	.reg = xreg, .shift_l = xshift_l, .shift_r = xshift_r, \
	.mask = xmask, .items = xitems, .texts = xtexts, .values = xvalues}
#define SOC_VALUE_ENUM_SINGLE(xreg, xshift, xmask, xitems, xtexts, xvalues) \
	SOC_VALUE_ENUM_DOUBLE(xreg, xshift, xshift, xmask, xitems, xtexts, xvalues)
#define SOC_VALUE_ENUM_SINGLE_AUTODISABLE(xreg, xshift, xmask, xitems, xtexts, xvalues) \
{	.reg = xreg, .shift_l = xshift, .shift_r = xshift, \
	.mask = xmask, .items = xitems, .texts = xtexts, \
	.values = xvalues, .autodisable = 1}
#define SOC_ENUM_SINGLE_VIRT(xitems, xtexts) \
	SOC_ENUM_SINGLE(SND_SOC_NOPM, 0, xitems, xtexts)
#define SOC_ENUM(xname, xenum) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname,\
	.info = snd_soc_info_enum_double, \
	.get = snd_soc_get_enum_double, .put = snd_soc_put_enum_double, \
	.private_value = (unsigned long)&xenum }
#define SOC_SINGLE_EXT(xname, xreg, xshift, xmax, xinvert,\
	 xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_SINGLE_VALUE(xreg, xshift, 0, xmax, xinvert, 0) }
#define SOC_DOUBLE_EXT(xname, reg, shift_left, shift_right, max, invert,\
	 xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = \
		SOC_DOUBLE_VALUE(reg, shift_left, shift_right, 0, max, invert, 0) }
#define SOC_DOUBLE_R_EXT(xname, reg_left, reg_right, xshift, xmax, xinvert,\
	 xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_DOUBLE_R_VALUE(reg_left, reg_right, xshift, \
					    0, xmax, xinvert) }
#define SOC_SINGLE_EXT_TLV(xname, xreg, xshift, xmax, xinvert,\
	 xhandler_get, xhandler_put, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_SINGLE_VALUE(xreg, xshift, 0, xmax, xinvert, 0) }
#define SOC_SINGLE_RANGE_EXT_TLV(xname, xreg, xshift, xmin, xmax, xinvert, \
				 xhandler_get, xhandler_put, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_SINGLE_VALUE(xreg, xshift, xmin, xmax, xinvert, 0) }
#define SOC_DOUBLE_EXT_TLV(xname, xreg, shift_left, shift_right, xmax, xinvert,\
	 xhandler_get, xhandler_put, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		 SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_DOUBLE_VALUE(xreg, shift_left, shift_right, \
					  0, xmax, xinvert, 0) }
#define SOC_DOUBLE_R_EXT_TLV(xname, reg_left, reg_right, xshift, xmax, xinvert,\
	 xhandler_get, xhandler_put, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		 SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_DOUBLE_R_VALUE(reg_left, reg_right, xshift, \
					    0, xmax, xinvert) }
#define SOC_DOUBLE_R_S_EXT_TLV(xname, reg_left, reg_right, xshift, xmin, xmax, \
			       xsign_bit, xinvert, xhandler_get, xhandler_put, \
			       tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | \
		  SNDRV_CTL_ELEM_ACCESS_READWRITE, \
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = SOC_DOUBLE_R_S_VALUE(reg_left, reg_right, xshift, \
					      xmin, xmax, xsign_bit, xinvert) }
#define SOC_SINGLE_S_EXT_TLV(xname, xreg, xshift, xmin, xmax, \
			     xsign_bit, xinvert, xhandler_get, xhandler_put, \
			     tlv_array) \
	SOC_DOUBLE_R_S_EXT_TLV(xname, xreg, xreg, xshift, xmin, xmax, \
			       xsign_bit, xinvert, xhandler_get, xhandler_put, \
			       tlv_array)
#define SOC_SINGLE_BOOL_EXT(xname, xdata, xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_bool_ext, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = xdata }
#define SOC_SINGLE_BOOL_EXT_ACC(xname, xdata, xhandler_get, xhandler_put, xaccess) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = xaccess, \
	.info = snd_soc_info_bool_ext, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = xdata }
#define SOC_ENUM_EXT(xname, xenum, xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_info_enum_double, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = (unsigned long)&xenum }
#define SOC_VALUE_ENUM_EXT(xname, xenum, xhandler_get, xhandler_put) \
	SOC_ENUM_EXT(xname, xenum, xhandler_get, xhandler_put)

#define SOC_ENUM_EXT_ACC(xname, xenum, xhandler_get, xhandler_put, xaccess) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = xaccess, \
	.info = snd_soc_info_enum_double, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = (unsigned long)&xenum }

#define SND_SOC_BYTES(xname, xbase, xregs)		      \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname,   \
	.info = snd_soc_bytes_info, .get = snd_soc_bytes_get, \
	.put = snd_soc_bytes_put, .private_value =	      \
		((unsigned long)&(struct soc_bytes)           \
		{.base = xbase, .num_regs = xregs }) }
#define SND_SOC_BYTES_E(xname, xbase, xregs, xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_bytes_info, .get = xhandler_get, \
	.put = xhandler_put, .private_value = \
		((unsigned long)&(struct soc_bytes) \
		{.base = xbase, .num_regs = xregs }) }
#define SND_SOC_BYTES_E_ACC(xname, xbase, xregs, xhandler_get, xhandler_put, xaccess) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = xaccess, \
	.info = snd_soc_bytes_info, .get = xhandler_get, \
	.put = xhandler_put, .private_value = \
		((unsigned long)&(struct soc_bytes) \
		{.base = xbase, .num_regs = xregs }) }

#define SND_SOC_BYTES_MASK(xname, xbase, xregs, xmask)	      \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname,   \
	.info = snd_soc_bytes_info, .get = snd_soc_bytes_get, \
	.put = snd_soc_bytes_put, .private_value =	      \
		((unsigned long)&(struct soc_bytes)           \
		{.base = xbase, .num_regs = xregs,	      \
		 .mask = xmask }) }

/*
 * SND_SOC_BYTES_EXT is deprecated, please USE SND_SOC_BYTES_TLV instead
 */
#define SND_SOC_BYTES_EXT(xname, xcount, xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.info = snd_soc_bytes_info_ext, \
	.get = xhandler_get, .put = xhandler_put, \
	.private_value = (unsigned long)&(struct soc_bytes_ext) \
		{.max = xcount} }
#define SND_SOC_BYTES_TLV(xname, xcount, xhandler_get, xhandler_put) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE | \
		  SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK, \
	.tlv.c = (snd_soc_bytes_tlv_callback), \
	.info = snd_soc_bytes_info_ext, \
	.private_value = (unsigned long)&(struct soc_bytes_ext) \
		{.max = xcount, .get = xhandler_get, .put = xhandler_put, } }
#define SOC_SINGLE_XR_SX(xname, xregbase, xregcount, xnbits, \
		xmin, xmax, xinvert) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
	.info = snd_soc_info_xr_sx, .get = snd_soc_get_xr_sx, \
	.put = snd_soc_put_xr_sx, \
	.private_value = (unsigned long)&(struct soc_mreg_control) \
		{.regbase = xregbase, .regcount = xregcount, .nbits = xnbits, \
		.invert = xinvert, .min = xmin, .max = xmax} }

#define SOC_SINGLE_STROBE(xname, xreg, xshift, xinvert) \
	SOC_SINGLE_EXT(xname, xreg, xshift, 1, xinvert, \
		snd_soc_get_strobe, snd_soc_put_strobe)

/*
 * Simplified versions of above macros, declaring a struct and calculating
 * ARRAY_SIZE internally
 */
#define SOC_ENUM_DOUBLE_DECL(name, xreg, xshift_l, xshift_r, xtexts) \
	const struct soc_enum name = SOC_ENUM_DOUBLE(xreg, xshift_l, xshift_r, \
						ARRAY_SIZE(xtexts), xtexts)
#define SOC_ENUM_SINGLE_DECL(name, xreg, xshift, xtexts) \
	SOC_ENUM_DOUBLE_DECL(name, xreg, xshift, xshift, xtexts)
#define SOC_ENUM_SINGLE_EXT_DECL(name, xtexts) \
	const struct soc_enum name = SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(xtexts), xtexts)
#define SOC_VALUE_ENUM_DOUBLE_DECL(name, xreg, xshift_l, xshift_r, xmask, xtexts, xvalues) \
	const struct soc_enum name = SOC_VALUE_ENUM_DOUBLE(xreg, xshift_l, xshift_r, xmask, \
							ARRAY_SIZE(xtexts), xtexts, xvalues)
#define SOC_VALUE_ENUM_SINGLE_DECL(name, xreg, xshift, xmask, xtexts, xvalues) \
	SOC_VALUE_ENUM_DOUBLE_DECL(name, xreg, xshift, xshift, xmask, xtexts, xvalues)

#define SOC_VALUE_ENUM_SINGLE_AUTODISABLE_DECL(name, xreg, xshift, xmask, xtexts, xvalues) \
	const struct soc_enum name = SOC_VALUE_ENUM_SINGLE_AUTODISABLE(xreg, \
		xshift, xmask, ARRAY_SIZE(xtexts), xtexts, xvalues)

#define SOC_ENUM_SINGLE_VIRT_DECL(name, xtexts) \
	const struct soc_enum name = SOC_ENUM_SINGLE_VIRT(ARRAY_SIZE(xtexts), xtexts)

/*
 * 这些 helper 宏把 CPU / CODEC / PLATFORM 端点组织成标准数组，
 * 再由 snd_soc_dai_link 统一描述一条链路。
 */
struct snd_soc_card;
struct snd_soc_pcm_runtime;
struct snd_soc_dai;
struct snd_soc_dai_driver;
struct snd_soc_dai_link;
struct snd_soc_component;
struct snd_soc_component_driver;
struct snd_soc_jack;
struct snd_soc_jack_pin;

#include <sound/soc-dapm.h>
#include <sound/soc-dpcm.h>
#include <sound/soc-topology.h>

/* card / component / PCM 的注册入口。 */
int snd_soc_register_card(struct snd_soc_card *card);
void snd_soc_unregister_card(struct snd_soc_card *card);
int devm_snd_soc_register_card(struct device *dev, struct snd_soc_card *card);
int devm_snd_soc_register_deferrable_card(struct device *dev, struct snd_soc_card *card);
#ifdef CONFIG_PM_SLEEP
int snd_soc_suspend(struct device *dev);
int snd_soc_resume(struct device *dev);
#else
static inline int snd_soc_suspend(struct device *dev)
{
	return 0;
}

static inline int snd_soc_resume(struct device *dev)
{
	return 0;
}
#endif
/* component 设备的创建、注册、查找和释放。 */
int snd_soc_poweroff(struct device *dev);
int snd_soc_component_initialize(struct snd_soc_component *component,
				 const struct snd_soc_component_driver *driver,
				 struct device *dev);
int snd_soc_add_component(struct snd_soc_component *component,
			  struct snd_soc_dai_driver *dai_drv,
			  int num_dai);
int snd_soc_register_component(struct device *dev,
			 const struct snd_soc_component_driver *component_driver,
			 struct snd_soc_dai_driver *dai_drv, int num_dai);
int devm_snd_soc_register_component(struct device *dev,
			 const struct snd_soc_component_driver *component_driver,
			 struct snd_soc_dai_driver *dai_drv, int num_dai);
#define snd_soc_unregister_component(dev) snd_soc_unregister_component_by_driver(dev, NULL)
void snd_soc_unregister_component_by_driver(struct device *dev,
			 const struct snd_soc_component_driver *component_driver);
struct snd_soc_component *snd_soc_lookup_component_nolocked(struct device *dev,
							    const char *driver_name);
struct snd_soc_component *snd_soc_lookup_component(struct device *dev,
						   const char *driver_name);
struct snd_soc_component *snd_soc_lookup_component_by_name(const char *component_name);

/* 为 runtime 创建标准 PCM / compress 设备。 */
int soc_new_pcm(struct snd_soc_pcm_runtime *rtd);
#ifdef CONFIG_SND_SOC_COMPRESS
int snd_soc_new_compress(struct snd_soc_pcm_runtime *rtd);
#else
static inline int snd_soc_new_compress(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}
#endif

struct snd_soc_pcm_runtime *snd_soc_get_pcm_runtime(struct snd_soc_card *card,
				struct snd_soc_dai_link *dai_link);

bool snd_soc_runtime_ignore_pmdown_time(struct snd_soc_pcm_runtime *rtd);

void snd_soc_runtime_action(struct snd_soc_pcm_runtime *rtd,
			    int stream, int action);
static inline void snd_soc_runtime_activate(struct snd_soc_pcm_runtime *rtd,
				     int stream)
{
	snd_soc_runtime_action(rtd, stream, 1);
}
static inline void snd_soc_runtime_deactivate(struct snd_soc_pcm_runtime *rtd,
				       int stream)
{
	snd_soc_runtime_action(rtd, stream, -1);
}

/* 根据 link / DAI 能力推导 runtime 的 PCM 硬件参数。 */
int snd_soc_runtime_calc_hw(struct snd_soc_pcm_runtime *rtd,
			    struct snd_pcm_hardware *hw, int stream);

/* 把 runtime 级别的 dai_fmt 配置下发到底层 DAI。 */
int snd_soc_runtime_set_dai_fmt(struct snd_soc_pcm_runtime *rtd,
	unsigned int dai_fmt);

/* 根据 pcm 参数推导 frame size / bclk 等基础时钟信息。 */
int snd_soc_calc_frame_size(int sample_size, int channels, int tdm_slots);
int snd_soc_params_to_frame_size(const struct snd_pcm_hw_params *params);
int snd_soc_calc_bclk(int fs, int sample_size, int channels, int tdm_slots);
int snd_soc_params_to_bclk(const struct snd_pcm_hw_params *parms);
int snd_soc_tdm_params_to_bclk(const struct snd_pcm_hw_params *params,
			       int tdm_width, int tdm_slots, int slot_multiple);
int snd_soc_ret(const struct device *dev, int ret, const char *fmt, ...);

/* set runtime hw params */
static inline int snd_soc_set_runtime_hwparams(struct snd_pcm_substream *substream,
					       const struct snd_pcm_hardware *hw)
{
	substream->runtime->hw = *hw;

	return 0;
}

struct snd_ac97 *snd_soc_alloc_ac97_component(struct snd_soc_component *component);
struct snd_ac97 *snd_soc_new_ac97_component(struct snd_soc_component *component,
	unsigned int id, unsigned int id_mask);
void snd_soc_free_ac97_component(struct snd_ac97 *ac97);

#ifdef CONFIG_SND_SOC_AC97_BUS
int snd_soc_set_ac97_ops(struct snd_ac97_bus_ops *ops);
int snd_soc_set_ac97_ops_of_reset(struct snd_ac97_bus_ops *ops,
		struct platform_device *pdev);

extern struct snd_ac97_bus_ops *soc_ac97_ops;
#else
static inline int snd_soc_set_ac97_ops_of_reset(struct snd_ac97_bus_ops *ops,
	struct platform_device *pdev)
{
	return 0;
}

static inline int snd_soc_set_ac97_ops(struct snd_ac97_bus_ops *ops)
{
	return 0;
}
#endif

/*
 * ALSA 控件相关接口。
 *
 * 这里包括创建通用 kcontrol、把控件挂到 card/component/dai 上，
 * 以及各种 mixer / enum / bytes / bool / TLV helper。
 */
struct snd_kcontrol *snd_soc_cnew(const struct snd_kcontrol_new *_template,
				  void *data, const char *long_name,
				  const char *prefix);
int snd_soc_add_component_controls(struct snd_soc_component *component,
	const struct snd_kcontrol_new *controls, unsigned int num_controls);
int snd_soc_add_card_controls(struct snd_soc_card *soc_card,
	const struct snd_kcontrol_new *controls, int num_controls);
int snd_soc_add_dai_controls(struct snd_soc_dai *dai,
	const struct snd_kcontrol_new *controls, int num_controls);
int snd_soc_info_enum_double(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo);
int snd_soc_get_enum_double(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_put_enum_double(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_info_volsw(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo);
int snd_soc_info_volsw_sx(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_info *uinfo);
#define snd_soc_info_bool_ext		snd_ctl_boolean_mono_info
int snd_soc_get_volsw(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_put_volsw(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
#define snd_soc_get_volsw_2r snd_soc_get_volsw
#define snd_soc_put_volsw_2r snd_soc_put_volsw
int snd_soc_get_volsw_sx(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_put_volsw_sx(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_limit_volume(struct snd_soc_card *card,
	const char *name, int max);
int snd_soc_bytes_info(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_info *uinfo);
int snd_soc_bytes_get(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol);
int snd_soc_bytes_put(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol);
int snd_soc_bytes_info_ext(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *ucontrol);
int snd_soc_bytes_tlv_callback(struct snd_kcontrol *kcontrol, int op_flag,
	unsigned int size, unsigned int __user *tlv);
int snd_soc_info_xr_sx(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo);
int snd_soc_get_xr_sx(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_put_xr_sx(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_get_strobe(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);
int snd_soc_put_strobe(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol);

/*
 * 触发顺序枚举。
 *
 * 用于控制 start / stop 时 link、component 和 DAI 的调用顺序。
 */
enum snd_soc_trigger_order {
						/* start			stop		     */
	SND_SOC_TRIGGER_ORDER_DEFAULT	= 0,	/* Link->Component->DAI		DAI->Component->Link */
	SND_SOC_TRIGGER_ORDER_LDC,		/* Link->DAI->Component		Component->DAI->Link */

	SND_SOC_TRIGGER_ORDER_MAX,
};

/*
 * 一个 PCM 流能力描述。
 *
 * 这是 DAI/codec 对外宣告自己支持哪些播放/录音能力的地方：
 * - 可用格式
 * - 可用采样率
 * - 支持的最小/最大通道数
 * - 有效位宽
 */
struct snd_soc_pcm_stream {
	const char *stream_name;	/* 流名称，通常对应用户态看到的播放/采集名字 */
	u64 formats;			/* SNDRV_PCM_FMTBIT_* 的位图 */
	u32 subformats;			/* 额外子格式位图，例如 S32_LE 的子格式 */
	unsigned int rates;		/* SNDRV_PCM_RATE_* 位图 */
	unsigned int rate_min;		/* 最小支持采样率 */
	unsigned int rate_max;		/* 最大支持采样率 */
	unsigned int channels_min;	/* 最小声道数 */
	unsigned int channels_max;	/* 最大声道数 */
	unsigned int sig_bits;		/* 有效数据位数 */
};

/*
 * Machine driver 的 PCM 操作回调。
 *
 * 这些回调比 DAI ops 更偏“板级行为”，通常用于把一条音频链路的
 * 启动/停止/HW 参数配置按机器需求串起来。
 */
struct snd_soc_ops {
	int (*startup)(struct snd_pcm_substream *);
	void (*shutdown)(struct snd_pcm_substream *);
	int (*hw_params)(struct snd_pcm_substream *, struct snd_pcm_hw_params *);
	int (*hw_free)(struct snd_pcm_substream *);
	int (*prepare)(struct snd_pcm_substream *);
	int (*trigger)(struct snd_pcm_substream *, int);
};

struct snd_soc_compr_ops {
	int (*startup)(struct snd_compr_stream *);
	void (*shutdown)(struct snd_compr_stream *);
	int (*set_params)(struct snd_compr_stream *);
};

struct snd_soc_component*
snd_soc_rtdcom_lookup(struct snd_soc_pcm_runtime *rtd,
		       const char *driver_name);

/*
 * 一个 DAI link 里的单个端点描述。
 *
 * 这个结构体本身不是硬件对象，而是“如何定位一个端点”的描述：
 * - name: 用设备名匹配
 * - of_node: 用 DT 节点匹配
 * - dai_name: 用 DAI 名字匹配
 * - dai_args: 用 OF phandle 参数匹配
 *
 * 同一个 link 中可以有多个 CPU / CODEC / platform 端点。
 */
struct snd_soc_dai_link_component {
	const char *name;
	struct device_node *of_node;
	const char *dai_name;
	const struct of_phandle_args *dai_args;

	/*
	 * 额外格式位。
	 *
	 * 这部分表示 Bx_Fx 级别的补充约束，会和 dai_link->dai_fmt
	 * 一起参与最终格式配置。常用于在链接层面附加更细的时序要求。
	 */
	unsigned int ext_fmt;
};

/*
 * ch_maps 用来描述多 CPU / 多 CODEC link 中的通道对应关系。
 *
 * 当一个 link 不是简单的一对一，而是多端点复用或扇出时，
 * core 不能仅靠端点顺序猜声道映射，因此需要显式给出 ch_map。
 *
 * [dai_link->ch_maps Image sample]
 *
 *-------------------------
 * CPU0 <---> Codec0
 *
 * ch-map[0].cpu = 0	ch-map[0].codec = 0
 *
 *-------------------------
 * CPU0 <---> Codec0
 * CPU1 <---> Codec1
 * CPU2 <---> Codec2
 *
 * ch-map[0].cpu = 0	ch-map[0].codec = 0
 * ch-map[1].cpu = 1	ch-map[1].codec = 1
 * ch-map[2].cpu = 2	ch-map[2].codec = 2
 *
 *-------------------------
 * CPU0 <---> Codec0
 * CPU1 <-+-> Codec1
 * CPU2 <-/
 *
 * ch-map[0].cpu = 0	ch-map[0].codec = 0
 * ch-map[1].cpu = 1	ch-map[1].codec = 1
 * ch-map[2].cpu = 2	ch-map[2].codec = 1
 *
 *-------------------------
 * CPU0 <---> Codec0
 * CPU1 <-+-> Codec1
 *	  \-> Codec2
 *
 * ch-map[0].cpu = 0	ch-map[0].codec = 0
 * ch-map[1].cpu = 1	ch-map[1].codec = 1
 * ch-map[2].cpu = 1	ch-map[2].codec = 2
 *
 */
/* 一个通道映射条目：把某个 CPU 通道对应到某个 CODEC 通道。 */
struct snd_soc_dai_link_ch_map {
	unsigned int cpu;
	unsigned int codec;
	unsigned int ch_mask;
};

/*
 * DAI link 是 ASoC 最核心的板级连接描述。
 *
 * 它描述一条音频路径上的两端或多端连接：
 * - CPU DAI：SoC 侧 I2S/PCM 控制器
 * - CODEC DAI：codec 侧音频接口
 * - platform：DMA / PCM 平台
 *
 * 同时它还携带这条连接的行为：
 * - 格式、主从、时钟
 * - DPCM 前后端属性
 * - probe/init/exit/hw_params_fixup
 * - start/stop 顺序与 symmetry 约束
 */
struct snd_soc_dai_link {
	/* 由 machine driver 配置的静态属性。 */
	const char *name;			/* 逻辑链接名，便于调试和识别 */
	const char *stream_name;		/* 用户态可见的 stream 名称 */

	/*
	 * CPU 端点列表。
	 *
	 * 这里可以通过设备名、DT 节点或 DAI 名字来匹配 CPU DAI。
	 * 对于多 CPU link 或 codec-to-codec 场景，可能会有多个 CPU 端点。
	 */
	struct snd_soc_dai_link_component *cpus;
	unsigned int num_cpus;

	/*
	 * CODEC 端点列表。
	 *
	 * 对传统声卡来说这里至少要有一个 codec。
	 * 对 codec-to-codec、DPCM、虚拟链路等场景也可能出现多个 codec。
	 */
	struct snd_soc_dai_link_component *codecs;
	unsigned int num_codecs;

	/* CPU 与 CODEC 的通道映射表。 */
	struct snd_soc_dai_link_ch_map *ch_maps;

	/*
	 * platform 端点列表。
	 *
	 * 很多简单卡只有一个 platform，通常就是 PCM/DMA 平台。
	 * 某些纯 codec-to-codec 或特殊链路不需要 platform。
	 */
	struct snd_soc_dai_link_component *platforms;
	unsigned int num_platforms;

	/* machine driver 自定义的链路 ID，便于区分多个 link。 */
	int id;

	/* codec-to-codec 链路的参数模板。 */
	const struct snd_soc_pcm_stream *c2c_params;
	unsigned int num_c2c_params;

	/* 初始化时要设置给 DAI 的总线格式。 */
	unsigned int dai_fmt;

	/* DPCM 下前端和后端的触发顺序策略。 */
	enum snd_soc_dpcm_trigger trigger[2];

	/* link 建立后的板级初始化，常用于加控件或注册 jack。 */
	int (*init)(struct snd_soc_pcm_runtime *rtd);

	/* init() 的反向清理函数。 */
	void (*exit)(struct snd_soc_pcm_runtime *rtd);

	/* 允许机器驱动在 hw_params 阶段改写参数，常用于 FE/BE 同步。 */
	int (*be_hw_params_fixup)(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params);

	/* 板级 stream 操作，最终会被 soc-link.c 代理调用。 */
	const struct snd_soc_ops *ops;
	const struct snd_soc_compr_ops *compr_ops;

	/*
	 * PCM trigger 的上下电顺序。
	 *
	 * 有些板子需要先动 codec 再动 platform，有些则相反。
	 * 这两个字段允许 machine driver 控制 start/stop 的调用顺序。
	 */
	enum snd_soc_trigger_order trigger_start;
	enum snd_soc_trigger_order trigger_stop;

	/* 该 link 的 PCM 操作是否允许睡眠/非原子调用。 */
	unsigned int nonatomic:1;

	/* 单向链路：仅播放或仅录音。 */
	unsigned int playback_only:1;
	unsigned int capture_only:1;

	/* suspend 时保持 DAI 活动，不强制关断。 */
	unsigned int ignore_suspend:1;

	/* 格式/通道/采样位宽对称约束。 */
	unsigned int symmetric_rate:1;
	unsigned int symmetric_channels:1;
	unsigned int symmetric_sample_bits:1;

	/* BE 链路不创建独立 PCM 设备。 */
	unsigned int no_pcm:1;

	/* 该 link 允许运行时路由到其他 link，常见于 FE。 */
	unsigned int dynamic:1;

	/* DPCM 下 FE/BE 的格式是否合并处理。 */
	unsigned int dpcm_merged_format:1;
	/* DPCM 下 FE/BE 的通道是否合并处理。 */
	unsigned int dpcm_merged_chan:1;
	/* DPCM 下 FE/BE 的采样率是否合并处理。 */
	unsigned int dpcm_merged_rate:1;

	/* stop 时忽略 pmdown_time。 */
	unsigned int ignore_pmdown_time:1;

	/* 彻底忽略这个 link，不创建 PCM 设备。 */
	unsigned int ignore:1;

#ifdef CONFIG_SND_SOC_TOPOLOGY
	struct snd_soc_dobj dobj; /* For topology */
#endif
};

static inline int snd_soc_link_num_ch_map(const struct snd_soc_dai_link *link)
{
	return max(link->num_cpus, link->num_codecs);
}

static inline struct snd_soc_dai_link_component*
snd_soc_link_to_cpu(struct snd_soc_dai_link *link, int n) {
	return &(link)->cpus[n];
}

static inline struct snd_soc_dai_link_component*
snd_soc_link_to_codec(struct snd_soc_dai_link *link, int n) {
	return &(link)->codecs[n];
}

static inline struct snd_soc_dai_link_component*
snd_soc_link_to_platform(struct snd_soc_dai_link *link, int n) {
	return &(link)->platforms[n];
}

/*
 * 这些 for_each_* 宏把 link 的端点数组当作统一迭代对象。
 *
 * 它们隐藏了“单端点”和“多端点”之间的差异，让调用方只关心
 * 当前遍历到哪一个 CPU / CODEC / PLATFORM / ch_map。
 */
#define for_each_link_codecs(link, i, codec)				\
	for ((i) = 0;							\
	     ((i) < link->num_codecs) &&				\
		     ((codec) = snd_soc_link_to_codec(link, i));		\
	     (i)++)

#define for_each_link_platforms(link, i, platform)			\
	for ((i) = 0;							\
	     ((i) < link->num_platforms) &&				\
		     ((platform) = snd_soc_link_to_platform(link, i));	\
	     (i)++)

#define for_each_link_cpus(link, i, cpu)				\
	for ((i) = 0;							\
	     ((i) < link->num_cpus) &&					\
		     ((cpu) = snd_soc_link_to_cpu(link, i));		\
	     (i)++)

#define for_each_link_ch_maps(link, i, ch_map)			\
	for ((i) = 0;						\
	     ((i) < snd_soc_link_num_ch_map(link) &&		\
		      ((ch_map) = link->ch_maps + i));		\
	     (i)++)

/*
 * DAILINK_* 宏负责把“端点数组 + 运行时代码”拼成一个完整 link。
 *
 * 这些宏的目标是减少 board driver 中的重复定义，让 CPU / CODEC /
 * PLATFORM 的数量、名字和排列关系都能统一通过一个入口表达出来。
 *
 * Sample 1 : Single CPU/Codec/Platform
 *
 * SND_SOC_DAILINK_DEFS(test,
 *	DAILINK_COMP_ARRAY(COMP_CPU("cpu_dai")),
 *	DAILINK_COMP_ARRAY(COMP_CODEC("codec", "codec_dai")),
 *	DAILINK_COMP_ARRAY(COMP_PLATFORM("platform")));
 *
 * struct snd_soc_dai_link link = {
 *	...
 *	SND_SOC_DAILINK_REG(test),
 * };
 *
 * Sample 2 : Multi CPU/Codec, no Platform
 *
 * SND_SOC_DAILINK_DEFS(test,
 *	DAILINK_COMP_ARRAY(COMP_CPU("cpu_dai1"),
 *			   COMP_CPU("cpu_dai2")),
 *	DAILINK_COMP_ARRAY(COMP_CODEC("codec1", "codec_dai1"),
 *			   COMP_CODEC("codec2", "codec_dai2")));
 *
 * struct snd_soc_dai_link link = {
 *	...
 *	SND_SOC_DAILINK_REG(test),
 * };
 *
 * Sample 3 : Define each CPU/Codec/Platform manually
 *
 * SND_SOC_DAILINK_DEF(test_cpu,
 *		DAILINK_COMP_ARRAY(COMP_CPU("cpu_dai1"),
 *				   COMP_CPU("cpu_dai2")));
 * SND_SOC_DAILINK_DEF(test_codec,
 *		DAILINK_COMP_ARRAY(COMP_CODEC("codec1", "codec_dai1"),
 *				   COMP_CODEC("codec2", "codec_dai2")));
 * SND_SOC_DAILINK_DEF(test_platform,
 *		DAILINK_COMP_ARRAY(COMP_PLATFORM("platform")));
 *
 * struct snd_soc_dai_link link = {
 *	...
 *	SND_SOC_DAILINK_REG(test_cpu,
 *			    test_codec,
 *			    test_platform),
 * };
 *
 * Sample 4 : Sample3 without platform
 *
 * struct snd_soc_dai_link link = {
 *	...
 *	SND_SOC_DAILINK_REG(test_cpu,
 *			    test_codec);
 * };
 */

#define SND_SOC_DAILINK_REG1(name)	 SND_SOC_DAILINK_REG3(name##_cpus, name##_codecs, name##_platforms)
#define SND_SOC_DAILINK_REG2(cpu, codec) SND_SOC_DAILINK_REG3(cpu, codec, null_dailink_component)
#define SND_SOC_DAILINK_REG3(cpu, codec, platform)	\
	.cpus		= cpu,				\
	.num_cpus	= ARRAY_SIZE(cpu),		\
	.codecs		= codec,			\
	.num_codecs	= ARRAY_SIZE(codec),		\
	.platforms	= platform,			\
	.num_platforms	= ARRAY_SIZE(platform)

#define SND_SOC_DAILINK_REG(...) \
	CONCATENATE(SND_SOC_DAILINK_REG, COUNT_ARGS(__VA_ARGS__))(__VA_ARGS__)

#define SND_SOC_DAILINK_DEF(name, def...)		\
	static struct snd_soc_dai_link_component name[]	= { def }

#define SND_SOC_DAILINK_DEFS(name, cpu, codec, platform...)	\
	SND_SOC_DAILINK_DEF(name##_cpus, cpu);			\
	SND_SOC_DAILINK_DEF(name##_codecs, codec);		\
	SND_SOC_DAILINK_DEF(name##_platforms, platform)

#define DAILINK_COMP_ARRAY(param...)	param
#define COMP_EMPTY()			{ }
#define COMP_CPU(_dai)			{ .dai_name = _dai, }
#define COMP_CODEC(_name, _dai)		{ .name = _name, .dai_name = _dai, }
#define COMP_PLATFORM(_name)		{ .name = _name }
#define COMP_AUX(_name)			{ .name = _name }
#define COMP_CODEC_CONF(_name)		{ .name = _name }
#define COMP_DUMMY()			/* see snd_soc_fill_dummy_dai() */

extern struct snd_soc_dai_link_component null_dailink_component[0];
extern struct snd_soc_dai_link_component snd_soc_dummy_dlc;
int snd_soc_dlc_is_dummy(struct snd_soc_dai_link_component *dlc);

/*
 * 每个 codec 的额外板级配置。
 *
 * 常见用途是给同一颗 codec 的不同实例加不同的 name_prefix，
 * 从而让 mixer/widget/path 名字在用户态里能区分开。
 */
/* codec 级别的额外板级配置。 */
struct snd_soc_codec_conf {
	/*
	 * specify device either by device name, or by
	 * DT/OF node, but not both.
	 */
	struct snd_soc_dai_link_component dlc;

	/*
	 * optional map of kcontrol, widget and path name prefixes that are
	 * associated per device
	 */
	const char *name_prefix;
};

/*
 * 辅助设备。
 *
 * 这类设备通常不是通过 DAI link 直接连到 card 上，而是作为附属
 * component 参与 DAPM、控制和电源管理，例如外置功放或 codec-less IC。
 */
/* 板级辅助设备。 */
struct snd_soc_aux_dev {
	/*
	 * specify multi-codec either by device name, or by
	 * DT/OF node, but not both.
	 */
	struct snd_soc_dai_link_component dlc;

	/* codec/machine specific init - e.g. add machine controls */
	int (*init)(struct snd_soc_component *component);
};

/*
 * snd_soc_card 描述一整块声卡/音频板。
 *
 * 这是 machine driver 的核心对象，代表“这一整套板级音频系统”。
 * 它管理：
 * - 多个 DAI link
 * - card 级 controls/widgets/routes
 * - DAPM 上下电
 * - jack、aux dev、codec prefix
 * - card 级 probe/remove/PM 回调
 */
/* 整块声卡/音频板的总描述。 */
struct snd_soc_card {
	/* card 的短名称、长名称、驱动名以及组成字符串。 */
	const char *name;
	const char *long_name;
	const char *driver_name;
	const char *components;
#ifdef CONFIG_DMI
		/* DMI 识别后生成的长名称缓存。 */
		char dmi_longname[80];
#endif /* CONFIG_DMI */

#ifdef CONFIG_PCI
	/*
	 * PCI does not define 0 as invalid, so pci_subsystem_set indicates
	 * whether a value has been written to these fields.
	 */
		/* PCI 子系统 ID，用于区分同一驱动的不同板卡。 */
		unsigned short pci_subsystem_vendor;
		unsigned short pci_subsystem_device;
		bool pci_subsystem_set;
#endif /* CONFIG_PCI */

	/* topology 场景下的短名缓存。 */
	char topology_shortname[32];

	/* 对应的 device、底层 ALSA card、模块 owner。 */
	struct device *dev;
	struct snd_card *snd_card;
	struct module *owner;

	/* card 总锁和 DAPM 专用锁。 */
	struct mutex mutex;
	struct mutex dapm_mutex;

	/* PCM 相关操作的专用锁。 */
	struct mutex pcm_mutex;

	/* card 生命周期回调。 */
	int (*probe)(struct snd_soc_card *card);
	int (*late_probe)(struct snd_soc_card *card);
	void (*fixup_controls)(struct snd_soc_card *card);
	int (*remove)(struct snd_soc_card *card);

	/* suspend/resume 的前后阶段钩子。 */
	int (*suspend_pre)(struct snd_soc_card *card);
	int (*suspend_post)(struct snd_soc_card *card);
	int (*resume_pre)(struct snd_soc_card *card);
	int (*resume_post)(struct snd_soc_card *card);

	/* DAPM bias 级别切换的前后回调。 */
	int (*set_bias_level)(struct snd_soc_card *,
			      struct snd_soc_dapm_context *dapm,
			      enum snd_soc_bias_level level);
	int (*set_bias_level_post)(struct snd_soc_card *,
				   struct snd_soc_dapm_context *dapm,
				   enum snd_soc_bias_level level);

	/* 动态增加/删除 DAI link 的回调。 */
	int (*add_dai_link)(struct snd_soc_card *,
			    struct snd_soc_dai_link *link);
	void (*remove_dai_link)(struct snd_soc_card *,
			    struct snd_soc_dai_link *link);

	/* 静态预定义的 CPU <-> CODEC DAI link 列表。 */
	struct snd_soc_dai_link *dai_link;
	int num_links;

	/* 绑定成功后的 runtime 列表。 */
	struct list_head rtd_list;
	int num_rtd;

	/* codec 级别的额外配置。 */
	struct snd_soc_codec_conf *codec_conf;
	int num_configs;

	/*
	 * 可选辅助设备，例如外置功放或不占用 DAI link 的 codec 组件。
	 */
	struct snd_soc_aux_dev *aux_dev;
	int num_aux_devs;
	struct list_head aux_comp_list;

	/* card 级别的控制项。 */
	const struct snd_kcontrol_new *controls;
	int num_controls;

	/*
	 * card 专属的 DAPM widgets/routes。
	 * of_dapm_* 用于 Device Tree 场景；普通驱动则用内置数组。
	 */
	const struct snd_soc_dapm_widget *dapm_widgets;
	int num_dapm_widgets;
	const struct snd_soc_dapm_route *dapm_routes;
	int num_dapm_routes;
	const struct snd_soc_dapm_widget *of_dapm_widgets;
	int num_of_dapm_widgets;
	const struct snd_soc_dapm_route *of_dapm_routes;
	int num_of_dapm_routes;

	/* 已绑定到该 card 的 component 设备列表。 */
	struct list_head component_dev_list;
	struct list_head list;

	/* card 内部 DAPM 节点、路径和待处理列表。 */
	struct list_head widgets;
	struct list_head paths;
	struct list_head dapm_list;
	struct list_head dapm_dirty;

	/* card 级 DAPM 上下文和统计信息。 */
	struct snd_soc_dapm_context *dapm;
	struct snd_soc_dapm_stats dapm_stats;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_card_root;
#endif
#ifdef CONFIG_PM_SLEEP
	struct work_struct deferred_resume_work;
#endif
	/* 用于控制开机/停流时的 pop/click 延迟。 */
	u32 pop_time;

	/* card 生命周期标志位。 */
	unsigned int instantiated:1;
	unsigned int topology_shortname_created:1;
	unsigned int fully_routed:1;
	unsigned int probed:1;
	unsigned int component_chaining:1;
	/* devres 绑定用的设备。 */
	struct device *devres_dev;

	/* 任意驱动私有数据。 */
	void *drvdata;
};
#define for_each_card_prelinks(card, i, link)				\
	for ((i) = 0;							\
	     ((i) < (card)->num_links) && ((link) = &(card)->dai_link[i]); \
	     (i)++)
#define for_each_card_pre_auxs(card, i, aux)				\
	for ((i) = 0;							\
	     ((i) < (card)->num_aux_devs) && ((aux) = &(card)->aux_dev[i]); \
	     (i)++)

#define for_each_card_rtds(card, rtd)			\
	list_for_each_entry(rtd, &(card)->rtd_list, list)
#define for_each_card_rtds_safe(card, rtd, _rtd)	\
	list_for_each_entry_safe(rtd, _rtd, &(card)->rtd_list, list)

#define for_each_card_auxs(card, component)			\
	list_for_each_entry(component, &card->aux_comp_list, card_aux_list)
#define for_each_card_auxs_safe(card, component, _comp)	\
	list_for_each_entry_safe(component, _comp,	\
				 &card->aux_comp_list, card_aux_list)

#define for_each_card_components(card, component)			\
	list_for_each_entry(component, &(card)->component_dev_list, card_list)

#define for_each_card_dapms(card, dapm)					\
	list_for_each_entry(dapm, &card->dapm_list, list)

#define for_each_card_widgets(card, w)\
	list_for_each_entry(w, &card->widgets, list)
#define for_each_card_widgets_safe(card, w, _w)	\
	list_for_each_entry_safe(w, _w, &card->widgets, list)


static inline int snd_soc_card_is_instantiated(struct snd_soc_card *card)
{
	return card && card->instantiated;
}

static inline struct snd_soc_dapm_context *snd_soc_card_to_dapm(struct snd_soc_card *card)
{
	return card->dapm;
}

/*
 * runtime 级对象。
 *
 * 一个 dai_link 真正绑定成功后，会生成一个 pcm_runtime。
 * 它是一次 PCM 打开后的运行时载体，存放：
 * - 当前 rtd 对应的 card/link
 * - 绑定到该 link 的 dai/component 列表
 * - PCM/compress 设备对象
 * - DPCM runtime 状态
 * - delayed work / debugfs / pmdown_time / 标记位
 */
/* 某条 DAI link 在运行时对应的 PCM 状态。 */
struct snd_soc_pcm_runtime {
	/* rtd 所属设备、card 和 dai_link。 */
	struct device *dev;
	struct snd_soc_card *card;
	struct snd_soc_dai_link *dai_link;
	struct snd_pcm_ops ops;

	/* codec-to-codec 场景下当前选择的参数索引。 */
	unsigned int c2c_params_select;

	/* DPCM 后端运行时数据。 */
	struct snd_soc_dpcm_runtime dpcm[SNDRV_PCM_STREAM_LAST + 1];
	struct snd_soc_dapm_widget *c2c_widget[SNDRV_PCM_STREAM_LAST + 1];

	/* 关闭后的延迟下电时间。 */
	long pmdown_time;

	/* 运行时关联的 PCM / compressed 设备。 */
	struct snd_pcm *pcm;
	struct snd_compr *compr;

	/*
	 * 绑定到这个 runtime 的所有 DAI。
	 * 排列顺序通常是 cpu_dais 在前，codec_dais 在后。
	 */
	struct snd_soc_dai **dais;

	/* 关闭后的延迟 work，用于 pmdown_time。 */
	struct delayed_work delayed_work;
	void (*close_delayed_work_func)(struct snd_soc_pcm_runtime *rtd);
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_dpcm_root;
#endif

	/* runtime 序号和挂入 card->rtd_list 的节点。 */
	unsigned int id;
	struct list_head list;

	/* 用于 rollback / 防重入 的调用标记。 */
	struct snd_pcm_substream *mark_startup;
	struct snd_pcm_substream *mark_hw_params;
	struct snd_pcm_substream *mark_trigger;
	struct snd_compr_stream  *mark_compr_startup;

	/* runtime 状态位。 */
	unsigned int pop_wait:1;
	unsigned int fe_compr:1; /* for Dynamic PCM */
	unsigned int initialized:1;

	/* 当前 runtime 绑定到的 component 数组。 */
	int num_components;
	struct snd_soc_component *components[] __counted_by(num_components);
};

/* see soc_new_pcm_runtime()  */
#define snd_soc_rtd_to_cpu(rtd, n)   (rtd)->dais[n]
#define snd_soc_rtd_to_codec(rtd, n) (rtd)->dais[n + (rtd)->dai_link->num_cpus]

static inline struct snd_soc_pcm_runtime *
snd_soc_substream_to_rtd(const struct snd_pcm_substream *substream)
{
	return snd_pcm_substream_chip(substream);
}

#define for_each_rtd_components(rtd, i, component)			\
	for ((i) = 0, component = NULL;					\
	     ((i) < rtd->num_components) && ((component) = rtd->components[i]);\
	     (i)++)
#define for_each_rtd_cpu_dais(rtd, i, dai)				\
	for ((i) = 0;							\
	     ((i) < rtd->dai_link->num_cpus) && ((dai) = snd_soc_rtd_to_cpu(rtd, i)); \
	     (i)++)
#define for_each_rtd_codec_dais(rtd, i, dai)				\
	for ((i) = 0;							\
	     ((i) < rtd->dai_link->num_codecs) && ((dai) = snd_soc_rtd_to_codec(rtd, i)); \
	     (i)++)
#define for_each_rtd_dais(rtd, i, dai)					\
	for ((i) = 0;							\
	     ((i) < (rtd)->dai_link->num_cpus + (rtd)->dai_link->num_codecs) &&	\
		     ((dai) = (rtd)->dais[i]);				\
	     (i)++)
#define for_each_rtd_dais_reverse(rtd, i, dai)					\
	for ((i) = (rtd)->dai_link->num_cpus + (rtd)->dai_link->num_codecs - 1;	\
	     (i) >= 0 && ((dai) = (rtd)->dais[i]);				\
	     (i)--)
#define for_each_rtd_ch_maps(rtd, i, ch_maps) for_each_link_ch_maps(rtd->dai_link, i, ch_maps)

void snd_soc_close_delayed_work(struct snd_soc_pcm_runtime *rtd);

/*
 * mixer / enum / bytes 控件的私有描述结构。
 *
 * 这些结构体通常不会被用户态直接看到，而是藏在 kcontrol 的
 * private_value 里，供通用的 get/put/info 回调解释。
 */
struct soc_mixer_control {
	/* 硬件写入值的最小/最大范围。 */
	int min, max;
	/* 用户态可见的最大值上限。 */
	int platform_max;
	int reg, rreg;
	unsigned int shift, rshift;
	u32 num_channels;
	unsigned int sign_bit;
	unsigned int invert:1;
	unsigned int autodisable:1;
#ifdef CONFIG_SND_SOC_TOPOLOGY
	struct snd_soc_dobj dobj;
#endif
};

/* bytes 型控件：通常用于批量寄存器或厂商私有 blob。 */
struct soc_bytes {
	int base;
	int num_regs;
	u32 mask;
};

/* 扩展 bytes 控件：允许自定义 get/put 处理。 */
struct soc_bytes_ext {
	int max;
#ifdef CONFIG_SND_SOC_TOPOLOGY
	struct snd_soc_dobj dobj;
#endif
	/* used for TLV byte control */
	int (*get)(struct snd_kcontrol *kcontrol, unsigned int __user *bytes,
			unsigned int size);
	int (*put)(struct snd_kcontrol *kcontrol, const unsigned int __user *bytes,
			unsigned int size);
};

/* 多寄存器范围型控件。 */
struct soc_mreg_control {
	long min, max;
	unsigned int regbase, regcount, nbits, invert;
};

/*
 * 枚举型控件。
 *
 * 这类控件把寄存器中的某个值映射成一组文字选项，例如：
 * "I2S" / "Left Justified" / "DSP A" 等。
 */
struct soc_enum {
	int reg;
	unsigned char shift_l;
	unsigned char shift_r;
	unsigned int items;
	unsigned int mask;
	const char * const *texts;
	const unsigned int *values;
	unsigned int autodisable:1;
#ifdef CONFIG_SND_SOC_TOPOLOGY
	struct snd_soc_dobj dobj;
#endif
};

static inline bool snd_soc_volsw_is_stereo(const struct soc_mixer_control *mc)
{
	if (mc->reg == mc->rreg && mc->shift == mc->rshift)
		return false;
	/*
	 * mc->reg == mc->rreg && mc->shift != mc->rshift, or
	 * mc->reg != mc->rreg means that the control is
	 * stereo (bits in one register or in two registers)
	 */
	return true;
}

static inline unsigned int snd_soc_enum_val_to_item(const struct soc_enum *e,
	unsigned int val)
{
	unsigned int i;

	/* 把寄存器值翻译成枚举项索引。 */
	if (!e->values)
		return val;

	for (i = 0; i < e->items; i++)
		if (val == e->values[i])
			return i;

	return 0;
}

static inline unsigned int snd_soc_enum_item_to_val(const struct soc_enum *e,
	unsigned int item)
{
	/* 把枚举项索引翻译回真正要写进寄存器的值。 */
	if (!e->values)
		return item;

	return e->values[item];
}

int snd_soc_util_init(void);
void snd_soc_util_exit(void);

int snd_soc_of_parse_card_name(struct snd_soc_card *card,
			       const char *propname);
int snd_soc_of_parse_audio_simple_widgets(struct snd_soc_card *card,
					  const char *propname);
int snd_soc_of_parse_pin_switches(struct snd_soc_card *card, const char *prop);
int snd_soc_of_get_slot_mask(struct device_node *np,
			     const char *prop_name,
			     unsigned int *mask);
int snd_soc_of_parse_tdm_slot(struct device_node *np,
			      unsigned int *tx_mask,
			      unsigned int *rx_mask,
			      unsigned int *slots,
			      unsigned int *slot_width);
void snd_soc_of_parse_node_prefix(struct device_node *np,
				   struct snd_soc_codec_conf *codec_conf,
				   struct device_node *of_node,
				   const char *propname);

int snd_soc_of_parse_audio_routing(struct snd_soc_card *card,
				   const char *propname);
int snd_soc_of_parse_aux_devs(struct snd_soc_card *card, const char *propname);

unsigned int snd_soc_daifmt_clock_provider_flipped(unsigned int dai_fmt);
unsigned int snd_soc_daifmt_clock_provider_from_bitmap(unsigned int bit_frame);

unsigned int snd_soc_daifmt_parse_format(struct device_node *np, const char *prefix);
unsigned int snd_soc_daifmt_parse_clock_provider_raw(struct device_node *np,
						     const char *prefix,
						     struct device_node **bitclkmaster,
						     struct device_node **framemaster);
#define snd_soc_daifmt_parse_clock_provider_as_bitmap(np, prefix)	\
	snd_soc_daifmt_parse_clock_provider_raw(np, prefix, NULL, NULL)
#define snd_soc_daifmt_parse_clock_provider_as_phandle			\
	snd_soc_daifmt_parse_clock_provider_raw
#define snd_soc_daifmt_parse_clock_provider_as_flag(np, prefix)		\
	snd_soc_daifmt_clock_provider_from_bitmap(			\
		snd_soc_daifmt_parse_clock_provider_as_bitmap(np, prefix))

int snd_soc_get_stream_cpu(const struct snd_soc_dai_link *dai_link, int stream);
int snd_soc_get_dlc(const struct of_phandle_args *args,
		    struct snd_soc_dai_link_component *dlc);
int snd_soc_of_get_dlc(struct device_node *of_node,
		       struct of_phandle_args *args,
		       struct snd_soc_dai_link_component *dlc,
		       int index);
int snd_soc_get_dai_id(struct device_node *ep);
int snd_soc_get_dai_name(const struct of_phandle_args *args,
			 const char **dai_name);
int snd_soc_of_get_dai_name(struct device_node *of_node,
			    const char **dai_name, int index);
int snd_soc_of_get_dai_link_codecs(struct device *dev,
				   struct device_node *of_node,
				   struct snd_soc_dai_link *dai_link);
void snd_soc_of_put_dai_link_codecs(struct snd_soc_dai_link *dai_link);
int snd_soc_of_get_dai_link_cpus(struct device *dev,
				 struct device_node *of_node,
				 struct snd_soc_dai_link *dai_link);
void snd_soc_of_put_dai_link_cpus(struct snd_soc_dai_link *dai_link);

int snd_soc_add_pcm_runtimes(struct snd_soc_card *card,
			     struct snd_soc_dai_link *dai_link,
			     int num_dai_link);
void snd_soc_remove_pcm_runtime(struct snd_soc_card *card,
				struct snd_soc_pcm_runtime *rtd);

void snd_soc_dlc_use_cpu_as_platform(struct snd_soc_dai_link_component *platforms,
				     struct snd_soc_dai_link_component *cpus);
struct of_phandle_args *snd_soc_copy_dai_args(struct device *dev,
					      const struct of_phandle_args *args);
struct snd_soc_dai *snd_soc_get_dai_via_args(const struct of_phandle_args *dai_args);
/* card / component / DAI 的注册、查找、解绑逻辑入口。 */
struct snd_soc_dai *snd_soc_register_dai(struct snd_soc_component *component,
					 struct snd_soc_dai_driver *dai_drv,
					 bool legacy_dai_naming);
void snd_soc_unregister_dai(struct snd_soc_dai *dai);

struct snd_soc_dai *snd_soc_find_dai(
	const struct snd_soc_dai_link_component *dlc);
struct snd_soc_dai *snd_soc_find_dai_with_mutex(
	const struct snd_soc_dai_link_component *dlc);

void soc_pcm_set_dai_params(struct snd_soc_dai *dai,
			    struct snd_pcm_hw_params *params);

#include <sound/soc-dai.h>

static inline
int snd_soc_fixup_dai_links_platform_name(struct snd_soc_card *card,
					  const char *platform_name)
{
	struct snd_soc_dai_link *dai_link;
	const char *name;
	int i;

	/* 某些板子共享同一个 platform 名字，core 这里会给每个 link 补副本。 */
	if (!platform_name) /* nothing to do */
		return 0;

	/* set platform name for each dailink */
	for_each_card_prelinks(card, i, dai_link) {
		/* only single platform is supported for now */
		if (dai_link->num_platforms != 1)
			return -EINVAL;

		if (!dai_link->platforms)
			return -EINVAL;

		name = devm_kstrdup(card->dev, platform_name, GFP_KERNEL);
		if (!name)
			return -ENOMEM;

		/* only single platform is supported for now */
		dai_link->platforms->name = name;
	}

	return 0;
}

#ifdef CONFIG_DEBUG_FS
extern struct dentry *snd_soc_debugfs_root;
#endif

extern const struct dev_pm_ops snd_soc_pm_ops;

/*
 *	DAPM helper functions
 *
 * 这里的重点是锁封装和上下文统一，让 card 级与 runtime 级的
 * DAPM 操作都走同一套锁语义。
 */
enum snd_soc_dapm_subclass {
	SND_SOC_DAPM_CLASS_ROOT		= 0,
	SND_SOC_DAPM_CLASS_RUNTIME	= 1,
};

static inline void _snd_soc_dapm_mutex_lock_root_c(struct snd_soc_card *card)
{
	/* root 级操作优先级更高，用独立 subclass 标记。 */
	mutex_lock_nested(&card->dapm_mutex, SND_SOC_DAPM_CLASS_ROOT);
}

static inline void _snd_soc_dapm_mutex_lock_c(struct snd_soc_card *card)
{
	/* runtime 级 DAPM 操作默认归到同一个锁域。 */
	mutex_lock_nested(&card->dapm_mutex, SND_SOC_DAPM_CLASS_RUNTIME);
}

static inline void _snd_soc_dapm_mutex_unlock_c(struct snd_soc_card *card)
{
	mutex_unlock(&card->dapm_mutex);
}

static inline void _snd_soc_dapm_mutex_assert_held_c(struct snd_soc_card *card)
{
	lockdep_assert_held(&card->dapm_mutex);
}

static inline void _snd_soc_dapm_mutex_lock_root_d(struct snd_soc_dapm_context *dapm)
{
	_snd_soc_dapm_mutex_lock_root_c(snd_soc_dapm_to_card(dapm));
}

static inline void _snd_soc_dapm_mutex_lock_d(struct snd_soc_dapm_context *dapm)
{
	_snd_soc_dapm_mutex_lock_c(snd_soc_dapm_to_card(dapm));
}

static inline void _snd_soc_dapm_mutex_unlock_d(struct snd_soc_dapm_context *dapm)
{
	_snd_soc_dapm_mutex_unlock_c(snd_soc_dapm_to_card(dapm));
}

static inline void _snd_soc_dapm_mutex_assert_held_d(struct snd_soc_dapm_context *dapm)
{
	_snd_soc_dapm_mutex_assert_held_c(snd_soc_dapm_to_card(dapm));
}

#define snd_soc_dapm_mutex_lock_root(x) _Generic((x),			\
	struct snd_soc_card * :		_snd_soc_dapm_mutex_lock_root_c, \
	struct snd_soc_dapm_context * :	_snd_soc_dapm_mutex_lock_root_d)(x)
#define snd_soc_dapm_mutex_lock(x) _Generic((x),			\
	struct snd_soc_card * :		_snd_soc_dapm_mutex_lock_c,	\
	struct snd_soc_dapm_context * :	_snd_soc_dapm_mutex_lock_d)(x)
#define snd_soc_dapm_mutex_unlock(x) _Generic((x),			\
	struct snd_soc_card * :		_snd_soc_dapm_mutex_unlock_c,	\
	struct snd_soc_dapm_context * :	_snd_soc_dapm_mutex_unlock_d)(x)
#define snd_soc_dapm_mutex_assert_held(x) _Generic((x),			\
	struct snd_soc_card * :		_snd_soc_dapm_mutex_assert_held_c, \
	struct snd_soc_dapm_context * :	_snd_soc_dapm_mutex_assert_held_d)(x)

/*
 *	PCM helper functions
 *
 * 这部分围绕 DPCM/PCM 的 card 级锁展开，并通过 _Generic 把
 * card 和 runtime 两类入口统一起来。
 */
static inline void _snd_soc_dpcm_mutex_lock_c(struct snd_soc_card *card)
{
	/* DPCM 共享 card->pcm_mutex，保证 FE/BE 协商不会乱序。 */
	mutex_lock(&card->pcm_mutex);
}

static inline void _snd_soc_dpcm_mutex_unlock_c(struct snd_soc_card *card)
{
	mutex_unlock(&card->pcm_mutex);
}

static inline void _snd_soc_dpcm_mutex_assert_held_c(struct snd_soc_card *card)
{
	lockdep_assert_held(&card->pcm_mutex);
}

static inline void _snd_soc_dpcm_mutex_lock_r(struct snd_soc_pcm_runtime *rtd)
{
	_snd_soc_dpcm_mutex_lock_c(rtd->card);
}

static inline void _snd_soc_dpcm_mutex_unlock_r(struct snd_soc_pcm_runtime *rtd)
{
	_snd_soc_dpcm_mutex_unlock_c(rtd->card);
}

static inline void _snd_soc_dpcm_mutex_assert_held_r(struct snd_soc_pcm_runtime *rtd)
{
	_snd_soc_dpcm_mutex_assert_held_c(rtd->card);
}

#define snd_soc_dpcm_mutex_lock(x) _Generic((x),			\
	 struct snd_soc_card * :	_snd_soc_dpcm_mutex_lock_c,	\
	 struct snd_soc_pcm_runtime * :	_snd_soc_dpcm_mutex_lock_r)(x)

#define snd_soc_dpcm_mutex_unlock(x) _Generic((x),			\
	 struct snd_soc_card * :	_snd_soc_dpcm_mutex_unlock_c,	\
	 struct snd_soc_pcm_runtime * :	_snd_soc_dpcm_mutex_unlock_r)(x)

#define snd_soc_dpcm_mutex_assert_held(x) _Generic((x),		\
	struct snd_soc_card * :		_snd_soc_dpcm_mutex_assert_held_c, \
	struct snd_soc_pcm_runtime * :	_snd_soc_dpcm_mutex_assert_held_r)(x)

#include <sound/soc-component.h>
#include <sound/soc-card.h>
#include <sound/soc-jack.h>

#endif
