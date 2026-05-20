/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2013-15, Intel Corporation
 */

#ifndef __LINUX_SND_SOC_ACPI_H
#define __LINUX_SND_SOC_ACPI_H

#include <linux/stddef.h>
#include <linux/acpi.h>
#include <linux/mod_devicetable.h>
#include <linux/soundwire/sdw.h>
#include <sound/soc.h>

struct snd_soc_acpi_package_context {
	/* 解析到的 package 名称。 */
	char *name;
	/* package 里包含的元素个数。 */
	int length;
	/* 解析后的数据缓冲区。 */
	struct acpi_buffer *format;
	/* ACPI 状态缓冲区。 */
	struct acpi_buffer *state;
	/* 解析结果是否有效。 */
	bool data_valid;
};

/* codec name 的命名长度上限：i2c-<HID>:00 这类格式需要预留空间。 */
#define SND_ACPI_I2C_ID_LEN (4 + ACPI_ID_LEN + 3 + 1)

#if IS_ENABLED(CONFIG_ACPI)
/* ACPI 机器表匹配入口。 */
struct snd_soc_acpi_mach *
snd_soc_acpi_find_machine(struct snd_soc_acpi_mach *machines);

/* 从 HID 中解析出一份 ACPI package。 */
bool snd_soc_acpi_find_package_from_hid(const u8 hid[ACPI_ID_LEN],
				    struct snd_soc_acpi_package_context *ctx);

/* 扫描当前 ACPI 设备并返回匹配的 codec 机器描述。 */
struct snd_soc_acpi_mach *snd_soc_acpi_codec_list(void *arg);

#else
/* 非 ACPI 配置下的空实现。 */
static inline struct snd_soc_acpi_mach *
snd_soc_acpi_find_machine(struct snd_soc_acpi_mach *machines)
{
	return NULL;
}

static inline bool
snd_soc_acpi_find_package_from_hid(const u8 hid[ACPI_ID_LEN],
				   struct snd_soc_acpi_package_context *ctx)
{
	return false;
}

/* 非 ACPI 配置下的空实现。 */
static inline struct snd_soc_acpi_mach *snd_soc_acpi_codec_list(void *arg)
{
	return NULL;
}
#endif

/*
 * ACPI machine driver 的运行参数。
 * 这里保存的是平台探测后，给 machine driver / topology 使用的
 * 板级描述信息。
 */
struct snd_soc_acpi_mach_params {
	u32 acpi_ipc_irq_index;
	const char *platform;
	u32 codec_mask;
	u32 dmic_num;
	u32 link_mask;
	const struct snd_soc_acpi_link_adr *links;
	u32 i2s_link_mask;
	u32 num_dai_drivers;
	struct snd_soc_dai_driver *dai_drivers;
	unsigned short subsystem_vendor;
	unsigned short subsystem_device;
	unsigned short subsystem_rev;
	bool subsystem_id_set;
	u32 bt_link_mask;
};

/* ACPI 枚举出的 endpoint 描述。 */
struct snd_soc_acpi_endpoint {
	u8 num;
	u8 aggregated;
	u8 group_position;
	u8 group_id;
};

/* 通过 _ADR 枚举出来的设备描述。 */
struct snd_soc_acpi_adr_device {
	u64 adr;
	u8 num_endpoints;
	const struct snd_soc_acpi_endpoint *endpoints;
	const char *name_prefix;
};

/*
 * 一个 link 上的 _ADR 设备列表。
 * 典型场景包括 SoundWire multi-drop，同一条 link 上挂多个设备。
 */
struct snd_soc_acpi_link_adr {
	u32 mask;
	u32 num_adr;
	const struct snd_soc_acpi_adr_device *adr_d;
};

/* topology 使用 -ssp<N> 后缀，N 由 BIOS / DMI 决定。 */
#define SND_SOC_ACPI_TPLG_INTEL_SSP_NUMBER BIT(0)

/* 当多个 SSP 都可用时，优先使用最高位那一路。 */
#define SND_SOC_ACPI_TPLG_INTEL_SSP_MSB BIT(1)

/* topology 使用 -dmic<N>ch 后缀。 */
#define SND_SOC_ACPI_TPLG_INTEL_DMIC_NUMBER BIT(2)

/* topology 文件名附加 speaker amplifier 后缀。 */
#define SND_SOC_ACPI_TPLG_INTEL_AMP_NAME BIT(3)

/* topology 文件名附加 headphone codec 后缀。 */
#define SND_SOC_ACPI_TPLG_INTEL_CODEC_NAME BIT(4)

/*
 * ACPI 平台的 machine 描述。
 * 这里把硬件匹配信息、固件/拓扑文件名、quirk、以及运行时参数都
 * 集中放到一份表里，供 machine driver 和 SOF 路径共用。
 */
/* Descriptor for SST ASoC machine driver */
struct snd_soc_acpi_mach {
	u8 id[ACPI_ID_LEN];
	const char *uid;
	const struct snd_soc_acpi_codecs *comp_ids;
	const u32 link_mask;
	const struct snd_soc_acpi_link_adr *links;
	const char *drv_name;
	const char *fw_filename;
	const char *tplg_filename;
	const char *board;
	struct snd_soc_acpi_mach * (*machine_quirk)(void *arg);
	const void *quirk_data;
	bool (*machine_check)(void *arg);
	void *pdata;
	struct snd_soc_acpi_mach_params mach_params;
	const char *sof_tplg_filename;
	const u32 tplg_quirk_mask;
	int (*get_function_tplg_files)(struct snd_soc_card *card,
				       const struct snd_soc_acpi_mach *mach,
				       const char *prefix, const char ***tplg_files,
				       bool best_effort);
};

#define SND_SOC_ACPI_MAX_CODECS 3

/* 额外 codec 列表，用于 quirk / machine 匹配。 */
struct snd_soc_acpi_codecs {
	int num_codecs;
	u8 codecs[SND_SOC_ACPI_MAX_CODECS][ACPI_ID_LEN];
};

static inline bool snd_soc_acpi_sof_parent(struct device *dev)
{
	/* 判断父设备是否属于 SOF ACPI 驱动树。 */
	return dev->parent && dev->parent->driver && dev->parent->driver->name &&
		!strncmp(dev->parent->driver->name, "sof-audio-acpi", strlen("sof-audio-acpi"));
}

/* SoundWire link 上是否找到 ACPI 描述的 slave 设备。 */
bool snd_soc_acpi_sdw_link_slaves_found(struct device *dev,
					const struct snd_soc_acpi_link_adr *link,
					struct sdw_peripherals *peripherals);

#endif
