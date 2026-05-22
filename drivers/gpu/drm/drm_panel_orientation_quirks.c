/* SPDX-License-Identifier: MIT */
/*
 * drm_panel_orientation_quirks.c -- Quirks for non-normal panel orientation
 *
 * Copyright (C) 2017 Hans de Goede <hdegoede@redhat.com>
 *
 * Note the quirks in this file are shared with fbdev/efifb and as such
 * must not depend on other drm code.
 */

/*
 * 面板方向特殊处理（quirks）模块
 *
 * 本文件提供面板方向（orientation）的特殊处理机制。在某些 x86
 * 硬件平台上，设备使用的屏幕是竖屏（portrait）面板，但显示引擎
 * 无法在硬件层面进行旋转。为了正确显示（特别是在 fbcon 控制台中），
 * 需要通过软件方式对显示内容进行旋转补偿。
 *
 * 主要场景：
 *   这类问题通常出现在一些廉价的小型平板/变形本设备上，这些设备
 *   为了降低成本而使用了竖屏面板，但没有配套的硬件旋转能力。
 *
 * 工作原理：
 *   通过 DMI 系统信息（厂商、产品名、BIOS 日期等）匹配特定设备，
 *   并结合屏幕分辨率进行二次确认以避免误匹配。返回正确的面板方向
 *   值（如右旋90度、左旋90度等），供显示驱动进行软件旋转。
 *
 * 注意：
 *   本文件中的 quirks 同时被 fbdev/efifb 共享，因此不能依赖其他
 *   DRM 特有代码。这使得在早期的启动阶段（efifb）也能正确显示。
 */

#include <linux/dmi.h>
#include <linux/export.h>
#include <linux/module.h>
#include <drm/drm_connector.h>
#include <drm/drm_utils.h>

#ifdef CONFIG_DMI

/*
 * Some x86 clamshell design devices use portrait tablet screens and a display
 * engine which cannot rotate in hardware, so we need to rotate the fbcon to
 * compensate. Unfortunately these (cheap) devices also typically have quite
 * generic DMI data, so we match on a combination of DMI data, screen resolution
 * and a list of known BIOS dates to avoid false positives.
 */

struct drm_dmi_panel_orientation_data {
	int width;
	int height;
	const char * const *bios_dates;
	int orientation;
};

static const struct drm_dmi_panel_orientation_data gpd_micropc = {
	.width = 720,
	.height = 1280,
	.bios_dates = (const char * const []){ "04/26/2019",
		NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data gpd_onemix2s = {
	.width = 1200,
	.height = 1920,
	.bios_dates = (const char * const []){ "05/21/2018", "10/26/2018",
		"03/04/2019", NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data gpd_pocket = {
	.width = 1200,
	.height = 1920,
	.bios_dates = (const char * const []){ "05/26/2017", "06/28/2017",
		"07/05/2017", "08/07/2017", NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data gpd_pocket2 = {
	.width = 1200,
	.height = 1920,
	.bios_dates = (const char * const []){ "06/28/2018", "08/28/2018",
		"12/07/2018", NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data gpd_win = {
	.width = 720,
	.height = 1280,
	.bios_dates = (const char * const []){
		"10/25/2016", "11/18/2016", "12/23/2016", "12/26/2016",
		"02/21/2017", "03/20/2017", "05/25/2017", NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data gpd_win2 = {
	.width = 720,
	.height = 1280,
	.bios_dates = (const char * const []){
		"12/07/2017", "05/24/2018", "06/29/2018", NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data itworks_tw891 = {
	.width = 800,
	.height = 1280,
	.bios_dates = (const char * const []){ "10/16/2015", NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data onegx1_pro = {
	.width = 1200,
	.height = 1920,
	.bios_dates = (const char * const []){ "12/17/2020", NULL },
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd640x960_leftside_up = {
	.width = 640,
	.height = 960,
	.orientation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd720x1280_rightside_up = {
	.width = 720,
	.height = 1280,
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd800x1280_leftside_up = {
	.width = 800,
	.height = 1280,
	.orientation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd800x1280_rightside_up = {
	.width = 800,
	.height = 1280,
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd1080x1920_leftside_up = {
	.width = 1080,
	.height = 1920,
	.orientation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd1080x1920_rightside_up = {
	.width = 1080,
	.height = 1920,
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd1200x1920_leftside_up = {
	.width = 1200,
	.height = 1920,
	.orientation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd1200x1920_rightside_up = {
	.width = 1200,
	.height = 1920,
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd1280x1920_rightside_up = {
	.width = 1280,
	.height = 1920,
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd1600x2560_leftside_up = {
	.width = 1600,
	.height = 2560,
	.orientation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
};

static const struct drm_dmi_panel_orientation_data lcd1600x2560_rightside_up = {
	.width = 1600,
	.height = 2560,
	.orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

static const struct dmi_system_id orientation_data[] = {
	{	/* Acer One 10 (S1003) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "One S1003"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Acer Switch V 10 (SW5-017) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SW5-017"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Anbernic Win600 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Anbernic"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Win600"),
		},
		.driver_data = (void *)&lcd720x1280_rightside_up,
	}, {	/* Asus T100HA */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "T100HAN"),
		},
		.driver_data = (void *)&lcd800x1280_leftside_up,
	}, {	/* Asus T101HA */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "T101HA"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Asus T103HAF */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "T103HAF"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* AYA NEO AYANEO 2/2S */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
		  DMI_MATCH(DMI_PRODUCT_NAME, "AYANEO 2"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* AYA NEO 2021 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYADEVICE"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "AYA NEO 2021"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* AYA NEO AIR */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
		  DMI_MATCH(DMI_PRODUCT_NAME, "AIR"),
		},
		.driver_data = (void *)&lcd1080x1920_leftside_up,
	}, {    /* AYA NEO Flip DS Bottom Screen */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "FLIP DS"),
		},
		.driver_data = (void *)&lcd640x960_leftside_up,
	}, {    /* AYA NEO Flip KB/DS Top Screen */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
		  DMI_MATCH(DMI_PRODUCT_NAME, "FLIP"),
		},
		.driver_data = (void *)&lcd1080x1920_leftside_up,
	}, {	/* AYA NEO Founder */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYA NEO"),
		  DMI_MATCH(DMI_PRODUCT_NAME, "AYA NEO Founder"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* AYA NEO GEEK */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
		  DMI_MATCH(DMI_PRODUCT_NAME, "GEEK"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* AYA NEO NEXT */
		.matches = {
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
		  DMI_MATCH(DMI_BOARD_NAME, "NEXT"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* AYA NEO KUN */
		.matches = {
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "AYANEO"),
		  DMI_MATCH(DMI_BOARD_NAME, "KUN"),
		},
		.driver_data = (void *)&lcd1600x2560_rightside_up,
	}, {	/* AYA NEO SLIDE */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AYANEO"),
		  DMI_MATCH(DMI_PRODUCT_NAME, "SLIDE"),
		},
		.driver_data = (void *)&lcd1080x1920_leftside_up,
	}, {    /* AYN Loki Max */
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ayn"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Loki Max"),
		},
		.driver_data = (void *)&lcd1080x1920_leftside_up,
	}, {	/* AYN Loki Zero */
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ayn"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Loki Zero"),
		},
		.driver_data = (void *)&lcd1080x1920_leftside_up,
	}, {	/* Chuwi HiBook (CWI514) */
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Hampoo"),
			DMI_MATCH(DMI_BOARD_NAME, "Cherry Trail CR"),
			/* Above matches are too generic, add bios-date match */
			DMI_MATCH(DMI_BIOS_DATE, "05/07/2016"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Chuwi Hi10 Pro (CWI529) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Hampoo"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Hi10 pro tablet"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Dynabook K50 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Dynabook Inc."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "dynabook K50/FR"),
		},
		.driver_data = (void *)&lcd800x1280_leftside_up,
	}, {	/* GPD MicroPC (generic strings, also match on bios date) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string"),
		},
		.driver_data = (void *)&gpd_micropc,
	}, {	/* GPD MicroPC (later BIOS versions with proper DMI strings) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "MicroPC"),
		},
		.driver_data = (void *)&lcd720x1280_rightside_up,
	}, {	/* GPD Win Max */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "G1619-01"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/*
		 * GPD Pocket, note that the DMI data is less generic then
		 * it seems, devices with a board-vendor of "AMI Corporation"
		 * are quite rare, as are devices which have both board- *and*
		 * product-id set to "Default String"
		 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "AMI Corporation"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_SERIAL, "Default string"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
		},
		.driver_data = (void *)&gpd_pocket,
	}, {	/* GPD Pocket 2 (generic strings, also match on bios date) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string"),
		},
		.driver_data = (void *)&gpd_pocket2,
	}, {	/* GPD Win (same note on DMI match as GPD Pocket) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "AMI Corporation"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_SERIAL, "Default string"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
		},
		.driver_data = (void *)&gpd_win,
	}, {	/* GPD Win 2 (too generic strings, also match on bios date) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string"),
		},
		.driver_data = (void *)&gpd_win2,
	}, {	/* GPD Win 2 (correct DMI strings) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "WIN2")
		},
		.driver_data = (void *)&lcd720x1280_rightside_up,
	}, {	/* GPD Win 3 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "G1618-03")
		},
		.driver_data = (void *)&lcd720x1280_rightside_up,
	}, {	/* GPD Win Mini */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "GPD"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "G1617-01")
		},
		.driver_data = (void *)&lcd1080x1920_rightside_up,
	}, {	/* I.T.Works TW891 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "To be filled by O.E.M."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "TW891"),
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "To be filled by O.E.M."),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "TW891"),
		},
		.driver_data = (void *)&itworks_tw891,
	}, {	/* KD Kurio Smart C15200 2-in-1 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "KD Interactive"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Kurio Smart"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "KDM960BCP"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/*
		 * Lenovo Ideapad Miix 310 laptop, only some production batches
		 * have a portrait screen, the resolution checks makes the quirk
		 * apply only to those batches.
		 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "80SG"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "MIIX 310-10ICR"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Lenovo Ideapad Miix 320 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "80XF"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo MIIX 320-10ICR"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Lenovo Ideapad D330-10IGM (HD) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGM"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Lenovo Ideapad D330-10IGM (FHD) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGM"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Lenovo Ideapad D330-10IGL (HD) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Lenovo ideapad D330-10IGL"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Lenovo IdeaPad Duet 3 10IGL5 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "IdeaPad Duet 3 10IGL5"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Lenovo Legion Go 8APU1 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "LENOVO"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8APU1"),
		},
		.driver_data = (void *)&lcd1600x2560_leftside_up,
	}, {	/* Lenovo Yoga Book X90F / X90L */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "CHERRYVIEW D1 PLATFORM"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "YETI-11"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Lenovo Yoga Book X91F / X91L */
		.matches = {
		  /* Non exact match to match F + L versions */
		  DMI_MATCH(DMI_PRODUCT_NAME, "Lenovo YB1-X91"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Lenovo Yoga Tablet 2 830F / 830L */
		.matches = {
		 /*
		  * Note this also matches the Lenovo Yoga Tablet 2 1050F/L
		  * since that uses the same mainboard. The resolution match
		  * will limit this to only matching on the 830F/L. Neither has
		  * any external video outputs so those are not a concern.
		  */
		 DMI_MATCH(DMI_SYS_VENDOR, "Intel Corp."),
		 DMI_MATCH(DMI_PRODUCT_NAME, "VALLEYVIEW C0 PLATFORM"),
		 DMI_MATCH(DMI_BOARD_NAME, "BYT-T FFD8"),
		 /* Partial match on beginning of BIOS version */
		 DMI_MATCH(DMI_BIOS_VERSION, "BLADE_21"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Lenovo Yoga Tab 3 X90F */
		.matches = {
		 DMI_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
		 DMI_MATCH(DMI_PRODUCT_VERSION, "Blade3-10A-001"),
		},
		.driver_data = (void *)&lcd1600x2560_rightside_up,
	}, {	/* Nanote UMPC-01 */
		.matches = {
		 DMI_MATCH(DMI_SYS_VENDOR, "RWC CO.,LTD"),
		 DMI_MATCH(DMI_PRODUCT_NAME, "UMPC-01"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* OneGX1 Pro */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "SYSTEM_MANUFACTURER"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SYSTEM_PRODUCT_NAME"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Default string"),
		},
		.driver_data = (void *)&onegx1_pro,
	}, {	/* OneXPlayer */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ONE-NETBOOK TECHNOLOGY CO., LTD."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "ONE XPLAYER"),
		},
		.driver_data = (void *)&lcd1600x2560_leftside_up,
	}, {	/* OneXPlayer Mini (Intel) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ONE-NETBOOK TECHNOLOGY CO., LTD."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "ONE XPLAYER"),
		},
		.driver_data = (void *)&lcd1200x1920_leftside_up,
	}, {	/* OrangePi Neo */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "OrangePi"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "NEO-01"),
		},
		.driver_data = (void *)&lcd1200x1920_rightside_up,
	}, {	/* Samsung GalaxyBook 10.6 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "SAMSUNG ELECTRONICS CO., LTD."),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Galaxy Book 10.6"),
		},
		.driver_data = (void *)&lcd1280x1920_rightside_up,
	}, {	/* Valve Steam Deck (Jupiter) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Jupiter"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* Valve Steam Deck (Galileo) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Valve"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Galileo"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "1"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* VIOS LTH17 */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "VIOS"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "LTH17"),
		},
		.driver_data = (void *)&lcd800x1280_rightside_up,
	}, {	/* ZOTAC Gaming Zone */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ZOTAC"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "G0A1W"),
		},
		.driver_data = (void *)&lcd1080x1920_leftside_up,
	}, {	/* One Mix 2S (generic strings, also match on bios date) */
		.matches = {
		  DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "Default string"),
		  DMI_EXACT_MATCH(DMI_BOARD_NAME, "Default string"),
		},
		.driver_data = (void *)&gpd_onemix2s,
	},
	{}
};

/**
 * drm_get_panel_orientation_quirk - 检查面板方向特殊处理
 * @width: 面板宽度（像素）
 * @height: 面板高度（像素）
 *
 * 该函数检查平台特定（如基于 DMI 的）特殊处理配置，提供无法从
 * 硬件/固件正确探测的面板方向信息。为了避免误匹配，该函数接受
 * 面板分辨率作为参数，并与特殊处理表中的分辨率进行对比验证。
 *
 * 需要注意的是，此函数也用于 DRM 子系统之外的其他组件（如 efifb
 * 代码）。因此，该函数在作为模块构建时会被编译到独立的内核模块中。
 *
 * 匹配策略：
 *   1. 首先通过 DMI 数据匹配设备型号
 *   2. 然后验证面板分辨率是否匹配预期值
 *   3. 如果条目包含 BIOS 日期限制，还需要验证 BIOS 日期
 *
 * 返回：
 * 找到匹配时返回 DRM_MODE_PANEL_ORIENTATION_* 值，
 * 未找到时返回 DRM_MODE_PANEL_ORIENTATION_UNKNOWN。
 */
int drm_get_panel_orientation_quirk(int width, int height)
{
	const struct dmi_system_id *match;
	const struct drm_dmi_panel_orientation_data *data;
	const char *bios_date;
	int i;

	for (match = dmi_first_match(orientation_data);
	     match;
	     match = dmi_first_match(match + 1)) {
		data = match->driver_data;

		if (data->width != width ||
		    data->height != height)
			continue;

		if (!data->bios_dates)
			return data->orientation;

		bios_date = dmi_get_system_info(DMI_BIOS_DATE);
		if (!bios_date)
			continue;

		i = match_string(data->bios_dates, -1, bios_date);
		if (i >= 0)
			return data->orientation;
	}

	return DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
}
EXPORT_SYMBOL(drm_get_panel_orientation_quirk);

#else

/* There are no quirks for non x86 devices yet */
int drm_get_panel_orientation_quirk(int width, int height)
{
	return DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
}
EXPORT_SYMBOL(drm_get_panel_orientation_quirk);

#endif

MODULE_DESCRIPTION("Quirks for non-normal panel orientation");
MODULE_LICENSE("Dual MIT/GPL");
