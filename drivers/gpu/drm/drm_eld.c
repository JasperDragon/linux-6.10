// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/*
 * 文件名: drm_eld.c
 *
 * 中文描述: EDID-Like Data (ELD) 音频信息处理
 *
 * ELD（EDID-Like Data）是一种音频数据块，通常嵌入在 HDMI/DP 的 EDID 信息中，
 * 用于描述显示设备支持的音频能力，包括音频格式、采样率、声道数等。
 *
 * 本文件提供了 SAD（Short Audio Descriptor，短音频描述符）在 ELD 缓冲区中的
 * 读取和写入功能：
 *   1. drm_eld_sad_get() - 从 ELD 缓冲区中读取指定索引的 SAD 数据
 *   2. drm_eld_sad_set() - 将 SAD 数据写入 ELD 缓冲区的指定位置
 *
 * 这些数据被 ALSA 音频驱动用于配置 HDMI/DP 的音频输出能力。
 */

#include <linux/export.h>

#include <drm/drm_edid.h>
#include <drm/drm_eld.h>

#include "drm_crtc_internal.h"

/**
 * drm_eld_sad_get - 从 ELD 中获取 SAD（短音频描述符）数据
 *
 * 从 ELD 缓冲区中读取指定索引的 SAD 数据，并转换为 struct cea_sad
 * 结构体。SAD 描述了显示设备支持的一种音频格式（如 LPCM、AC-3、DTS 等），
 * 包括声道数、采样率和采样大小等信息。
 *
 * ALSA 音频驱动使用这些信息来配置通过 HDMI/DP 输出的音频格式。
 */
 * @eld: ELD buffer
 * @sad_index: SAD index
 * @cta_sad: destination struct cea_sad
 *
 * @return: 0 on success, or negative on errors
 */
int drm_eld_sad_get(const u8 *eld, int sad_index, struct cea_sad *cta_sad)
{
	const u8 *sad;

	if (sad_index >= drm_eld_sad_count(eld))
		return -EINVAL;

	sad = eld + DRM_ELD_CEA_SAD(drm_eld_mnl(eld), sad_index);

	drm_edid_cta_sad_set(cta_sad, sad);

	return 0;
}
EXPORT_SYMBOL(drm_eld_sad_get);

/**
 * drm_eld_sad_set - 将 SAD（短音频描述符）设置到 ELD 中
 *
 * 将 struct cea_sad 结构体中的 SAD 数据写入 ELD 缓冲区的指定位置。
 * 此函数与 drm_eld_sad_get 相反，用于构造或修改 ELD 中的音频描述信息。
 */
 * @eld: ELD buffer
 * @sad_index: SAD index
 * @cta_sad: source struct cea_sad
 *
 * @return: 0 on success, or negative on errors
 */
int drm_eld_sad_set(u8 *eld, int sad_index, const struct cea_sad *cta_sad)
{
	u8 *sad;

	if (sad_index >= drm_eld_sad_count(eld))
		return -EINVAL;

	sad = eld + DRM_ELD_CEA_SAD(drm_eld_mnl(eld), sad_index);

	drm_edid_cta_sad_get(cta_sad, sad);

	return 0;
}
EXPORT_SYMBOL(drm_eld_sad_set);
