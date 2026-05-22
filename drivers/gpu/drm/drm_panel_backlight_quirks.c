// SPDX-License-Identifier: GPL-2.0

/*
 * 面板背光特殊处理（quirks）模块
 *
 * 本文件提供面板背光相关的特殊处理机制。在某些硬件平台上，由于
 * 固件或硬件的设计缺陷/差异，背光亮度值无法通过标准方式正确探测。
 * 本模块通过 DMI（桌面管理接口）系统信息来匹配特定硬件平台，
 * 并为其提供正确的背光参数覆盖。
 *
 * 主要功能：
 *   1. 最小背光亮度值修正：某些面板将背光关闭（亮度0）误解为
 *      最大亮度，需要强制设置最小亮度为1来避免此问题。
 *   2. 亮度值掩码修正：某些 OLED 面板在亮度值的最后几位为 0/1
 *      时会出现显示异常，需要通过掩码来修正。
 *
 * 通过维护一个 DMI 匹配表，本模块能够为已知有问题的硬件平台
 * 提供正确的背光参数，而无需修改各个显示驱动本身。
 */

#include <linux/array_size.h>
#include <linux/dmi.h>
#include <linux/export.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <drm/drm_edid.h>
#include <drm/drm_utils.h>

struct drm_panel_match {
	enum dmi_field field;
	const char * const value;
};

struct drm_get_panel_backlight_quirk {
	struct drm_panel_match dmi_match;
	struct drm_panel_match dmi_match_other;
	struct drm_edid_ident ident;
	struct drm_panel_backlight_quirk quirk;
};

static const struct drm_get_panel_backlight_quirk drm_panel_min_backlight_quirks[] = {
	/* 13 inch matte panel */
	{
		.dmi_match.field = DMI_BOARD_VENDOR,
		.dmi_match.value = "Framework",
		.ident.panel_id = drm_edid_encode_panel_id('B', 'O', 'E', 0x0bca),
		.ident.name = "NE135FBM-N41",
		.quirk = { .min_brightness = 1, },
	},
	/* 13 inch glossy panel */
	{
		.dmi_match.field = DMI_BOARD_VENDOR,
		.dmi_match.value = "Framework",
		.ident.panel_id = drm_edid_encode_panel_id('B', 'O', 'E', 0x095f),
		.ident.name = "NE135FBM-N41",
		.quirk = { .min_brightness = 1, },
	},
	/* 13 inch 2.8k panel */
	{
		.dmi_match.field = DMI_BOARD_VENDOR,
		.dmi_match.value = "Framework",
		.ident.panel_id = drm_edid_encode_panel_id('B', 'O', 'E', 0x0cb4),
		.ident.name = "NE135A1M-NY1",
		.quirk = { .min_brightness = 1, },
	},
	/* Steam Deck models */
	{
		.dmi_match.field = DMI_SYS_VENDOR,
		.dmi_match.value = "Valve",
		.dmi_match_other.field = DMI_PRODUCT_NAME,
		.dmi_match_other.value = "Jupiter",
		.quirk = { .min_brightness = 1, },
	},
	{
		.dmi_match.field = DMI_SYS_VENDOR,
		.dmi_match.value = "Valve",
		.dmi_match_other.field = DMI_PRODUCT_NAME,
		.dmi_match_other.value = "Galileo",
		.quirk = { .min_brightness = 1, },
	},
	/* Have OLED Panels with brightness issue when last byte is 0/1 */
	{
		.dmi_match.field = DMI_SYS_VENDOR,
		.dmi_match.value = "AYANEO",
		.dmi_match_other.field = DMI_PRODUCT_NAME,
		.dmi_match_other.value = "AYANEO 3",
		.quirk = { .brightness_mask = 3, },
	},
	{
		.dmi_match.field = DMI_SYS_VENDOR,
		.dmi_match.value = "ZOTAC",
		.dmi_match_other.field = DMI_BOARD_NAME,
		.dmi_match_other.value = "G0A1W",
		.quirk = { .brightness_mask = 3, },
	},
	{
		.dmi_match.field = DMI_SYS_VENDOR,
		.dmi_match.value = "ZOTAC",
		.dmi_match_other.field = DMI_BOARD_NAME,
		.dmi_match_other.value = "G1A1W",
		.quirk = { .brightness_mask = 3, },
	},
	{
		.dmi_match.field = DMI_SYS_VENDOR,
		.dmi_match.value = "ONE-NETBOOK",
		.dmi_match_other.field = DMI_PRODUCT_NAME,
		.dmi_match_other.value = "ONEXPLAYER F1Pro",
		.quirk = { .brightness_mask = 3, },
	},
	{
		.dmi_match.field = DMI_SYS_VENDOR,
		.dmi_match.value = "ONE-NETBOOK",
		.dmi_match_other.field = DMI_PRODUCT_NAME,
		.dmi_match_other.value = "ONEXPLAYER F1 EVA-02",
		.quirk = { .brightness_mask = 3, },
	},
};

static bool drm_panel_min_backlight_quirk_matches(
	const struct drm_get_panel_backlight_quirk *quirk,
	const struct drm_edid *edid)
{
	if (quirk->dmi_match.field &&
	    !dmi_match(quirk->dmi_match.field, quirk->dmi_match.value))
		return false;

	if (quirk->dmi_match_other.field &&
	    !dmi_match(quirk->dmi_match_other.field,
		       quirk->dmi_match_other.value))
		return false;

	if (quirk->ident.panel_id && !drm_edid_match(edid, &quirk->ident))
		return false;

	return true;
}

/**
 * drm_get_panel_backlight_quirk - 获取面板背光特殊处理参数
 * @edid: 要检查的面板的 EDID 数据
 *
 * 该函数检查平台特定（如基于 DMI 的）特殊处理配置，提供无法从
 * 硬件/固件或其他来源正确探测的面板最小背光亮度等信息。
 *
 * 背光问题有两种常见情况：
 *   1. 最小亮度问题：某些面板在亮度设为0时会异常显示（如全黑或全亮），
 *      需要将最小值设为1来解决。
 *   2. 亮度掩码问题：某些 OLED 面板在亮度值的最后几位为特定值时会出现
 *      显示异常，需要通过掩码来修正这些值。
 *
 * 返回：
 * 找到匹配的特殊处理配置时返回 drm_panel_backlight_quirk 结构体指针，
 * 否则返回错误指针。
 */
const struct drm_panel_backlight_quirk *
drm_get_panel_backlight_quirk(const struct drm_edid *edid)
{
	const struct drm_get_panel_backlight_quirk *quirk;
	size_t i;

	if (!IS_ENABLED(CONFIG_DMI))
		return ERR_PTR(-ENODATA);

	if (!edid)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < ARRAY_SIZE(drm_panel_min_backlight_quirks); i++) {
		quirk = &drm_panel_min_backlight_quirks[i];

		if (drm_panel_min_backlight_quirk_matches(quirk, edid))
			return &quirk->quirk;
	}

	return ERR_PTR(-ENODATA);
}
EXPORT_SYMBOL(drm_get_panel_backlight_quirk);

MODULE_DESCRIPTION("Quirks for panel backlight overrides");
MODULE_LICENSE("GPL");
