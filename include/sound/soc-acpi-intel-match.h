/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2017, Intel Corporation
 */

#ifndef __LINUX_SND_SOC_ACPI_INTEL_MATCH_H
#define __LINUX_SND_SOC_ACPI_INTEL_MATCH_H

#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/acpi.h>

/*
 * 这些表不是 const，因为部分字段会在运行时被 machine driver
 * 改写成 pdata 或 machine ops。
 */
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_broadwell_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_baytrail_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_cherrytrail_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_skl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_kbl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_bxt_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_glk_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_cnl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_cfl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_cml_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_icl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_tgl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_ehl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_jsl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_adl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_rpl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_mtl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_lnl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_arl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_ptl_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_nvl_machines[];

extern struct snd_soc_acpi_mach snd_soc_acpi_intel_cnl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_cfl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_cml_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_icl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_tgl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_adl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_rpl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_mtl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_lnl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_arl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_ptl_sdw_machines[];
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_nvl_sdw_machines[];

/* HDA codec 平台通用匹配表，可能还会带额外的 ACPI 枚举 codec。 */
extern struct snd_soc_acpi_mach snd_soc_acpi_intel_hda_machines[];

#endif
