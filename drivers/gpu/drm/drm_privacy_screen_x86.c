// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * Authors:
 * Hans de Goede <hdegoede@redhat.com>
 */

/*
 * x86 平台隐私屏幕支持
 *
 * 本文件实现了 x86 平台上隐私屏幕（privacy screen）提供者的自动
 * 检测和查找功能。隐私屏幕是一种特殊的显示功能，可以在屏幕上覆盖
 * 一层可切换的过滤层，使得只有正面视角的用户能够看到屏幕内容，
 * 侧方视角则看不清，从而保护用户隐私。
 *
 * 在 x86 平台上，隐私屏幕功能通常通过以下方式实现：
 *   - ThinkPad 笔记本：通过嵌入式控制器（EC）的 ACPI 接口控制
 *   - ChromeOS 设备：通过专用的 ACPI 设备（GOOG0010）控制
 *
 * 本模块在启动时自动检测平台是否支持隐私屏幕功能，如果检测到，
 * 则向隐私屏幕核心注册一个查找条目，使得 DRM 驱动程序能够找到
 * 并使用对应的隐私屏幕提供者。
 */

#include <linux/acpi.h>
#include <drm/drm_privacy_screen_machine.h>

#ifdef CONFIG_X86
static struct drm_privacy_screen_lookup arch_lookup;

struct arch_init_data {
	struct drm_privacy_screen_lookup lookup;
	bool (*detect)(void);
};

#if IS_ENABLED(CONFIG_THINKPAD_ACPI)
static acpi_status __init acpi_set_handle(acpi_handle handle, u32 level,
					  void *context, void **return_value)
{
	*(acpi_handle *)return_value = handle;
	return AE_CTRL_TERMINATE;
}

static bool __init detect_thinkpad_privacy_screen(void)
{
	union acpi_object obj = { .type = ACPI_TYPE_INTEGER };
	struct acpi_object_list args = { .count = 1, .pointer = &obj, };
	acpi_handle ec_handle = NULL;
	unsigned long long output;
	acpi_status status;

	if (acpi_disabled)
		return false;

	/* Get embedded-controller handle */
	status = acpi_get_devices("PNP0C09", acpi_set_handle, NULL, &ec_handle);
	if (ACPI_FAILURE(status) || !ec_handle)
		return false;

	/* And call the privacy-screen get-status method */
	status = acpi_evaluate_integer(ec_handle, "HKEY.GSSS", &args, &output);
	if (ACPI_FAILURE(status))
		return false;

	return (output & 0x10000) ? true : false;
}
#endif

#if IS_ENABLED(CONFIG_CHROMEOS_PRIVACY_SCREEN)
static bool __init detect_chromeos_privacy_screen(void)
{
	return acpi_dev_present("GOOG0010", NULL, -1);
}
#endif

static const struct arch_init_data arch_init_data[] __initconst = {
#if IS_ENABLED(CONFIG_THINKPAD_ACPI)
	{
		.lookup = {
			.dev_id = NULL,
			.con_id = NULL,
			.provider = "privacy_screen-thinkpad_acpi",
		},
		.detect = detect_thinkpad_privacy_screen,
	},
#endif
#if IS_ENABLED(CONFIG_CHROMEOS_PRIVACY_SCREEN)
	{
		.lookup = {
			.dev_id = NULL,
			.con_id = NULL,
			.provider = "privacy_screen-GOOG0010:00",
		},
		.detect = detect_chromeos_privacy_screen,
	},
#endif
};

/**
 * drm_privacy_screen_lookup_init - 初始化 x86 平台隐私屏幕查找表
 *
 * 该函数在系统启动时被调用，用于检测当前 x86 平台是否支持隐私屏幕
 * 功能。它遍历平台特定的检测函数列表，依次尝试检测已知的隐私屏幕
 * 实现方式（如 ThinkPad EC 接口、ChromeOS ACPI 设备等）。
 *
 * 当检测到某个隐私屏幕提供者时：
 *   1. 记录日志信息，指示找到了哪个提供者
 *   2. 将对应的查找条目添加到隐私屏幕核心的静态查找列表中
 *   3. 停止进一步检测（只使用第一个匹配的提供者）
 *
 * 此函数在系统启动早期被调用，确保 DRM 驱动初始化时能够找到
 * 对应的隐私屏幕设备。
 */
void __init drm_privacy_screen_lookup_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(arch_init_data); i++) {
		if (!arch_init_data[i].detect())
			continue;

		pr_info("Found '%s' privacy-screen provider\n",
			arch_init_data[i].lookup.provider);

		/* Make a copy because arch_init_data is __initconst */
		arch_lookup = arch_init_data[i].lookup;
		drm_privacy_screen_lookup_add(&arch_lookup);
		break;
	}
}

/**
 * drm_privacy_screen_lookup_exit - 清理 x86 平台隐私屏幕查找表
 *
 * 该函数在系统关闭或模块卸载时被调用。如果之前已成功添加了隐私屏幕
 * 查找条目，则将其从核心的静态查找列表中移除，完成清理工作。
 */
void drm_privacy_screen_lookup_exit(void)
{
	if (arch_lookup.provider)
		drm_privacy_screen_lookup_remove(&arch_lookup);
}
#endif /* ifdef CONFIG_X86 */
