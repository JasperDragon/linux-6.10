// SPDX-License-Identifier: GPL-2.0-or-later
/*
   drm_edid_load.c: use a built-in EDID data set or load it via the firmware
		    interface

   Copyright (C) 2012 Carsten Emde <C.Emde@osadl.org>

*/

/*
 * 文件名: drm_edid_load.c
 *
 * 中文描述: 外部 EDID 固件加载
 *
 * 本文件实现了从固件文件系统（通常为 /lib/firmware/）加载 EDID 数据的机制。
 * 当需要覆盖显示器的原生 EDID（如调试、测试或特殊显示配置）时，可以通过
 * "edid_firmware" 内核参数指定一个固件文件路径来替代实际的显示器 EDID。
 *
 * 核心功能：
 *   1. edid_firmware 内核模块参数 - 指定要加载的 EDID 固件文件
 *   2. edid_load() - 从固件子系统请求并验证 EDID 数据
 *   3. drm_edid_load_firmware() - 解析模块参数并加载对应的 EDID 固件
 *
 * 支持逗号分隔的多个固件文件，可为不同连接器指定不同的 EDID 固件。
 * 格式: "edid_firmware=connector_name:edid_file[,connector_name:edid_file,...]"
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>

#include "drm_crtc_internal.h"

static char edid_firmware[PATH_MAX];
module_param_string(edid_firmware, edid_firmware, sizeof(edid_firmware), 0644);
MODULE_PARM_DESC(edid_firmware,
		 "Do not probe monitor, use specified EDID blob from /lib/firmware instead.");

/*
 * edid_load - 从固件子系统加载并验证 EDID 数据
 * @connector: DRM 连接器
 * @name: 固件文件名
 *
 * 通过 request_firmware() 从固件文件系统（通常为 /lib/firmware/）
 * 加载指定的 EDID 数据文件。加载后通过 drm_edid_alloc() 分配内存，
 * 并使用 drm_edid_valid() 验证其格式是否正确。
 *
 * 返回：有效的 drm_edid 指针，或 ERR_PTR 错误码
 */
static const struct drm_edid *edid_load(struct drm_connector *connector, const char *name)
{
	const struct firmware *fw = NULL;
	const struct drm_edid *drm_edid;
	int err;

	err = request_firmware(&fw, name, connector->dev->dev);
	if (err) {
		drm_err(connector->dev,
			"[CONNECTOR:%d:%s] Requesting EDID firmware \"%s\" failed (err=%d)\n",
			connector->base.id, connector->name,
			name, err);
		return ERR_PTR(err);
	}

	drm_dbg_kms(connector->dev, "[CONNECTOR:%d:%s] Loaded external firmware EDID \"%s\"\n",
		    connector->base.id, connector->name, name);

	drm_edid = drm_edid_alloc(fw->data, fw->size);
	if (!drm_edid_valid(drm_edid)) {
		drm_err(connector->dev, "Invalid firmware EDID \"%s\"\n", name);
		drm_edid_free(drm_edid);
		drm_edid = ERR_PTR(-EINVAL);
	}

	release_firmware(fw);

	return drm_edid;
}

/*
 * drm_edid_load_firmware - 根据模块参数加载外部 EDID 固件
 * @connector: DRM 连接器
 *
 * 解析 edid_firmware 内核模块参数，查找与指定连接器匹配的 EDID
 * 固件文件，然后加载并验证该固件。
 *
 * 模块参数格式：
 *   edid_firmware="connector_name:edid_file[,connector_name:edid_file,...]"
 *
 * 如果不带连接器限定符（即只有文件名），则作为后备选项。
 * 如果有多个未限定的文件名，最后一个作为后备。
 *
 * 返回：有效的 drm_edid 指针，-ENOENT 无匹配固件，-ENOMEM 内存不足
 */
const struct drm_edid *drm_edid_load_firmware(struct drm_connector *connector)
{
	char *edidname, *last, *colon, *fwstr, *edidstr, *fallback = NULL;
	const struct drm_edid *drm_edid;

	if (edid_firmware[0] == '\0')
		return ERR_PTR(-ENOENT);

	/*
	 * If there are multiple edid files specified and separated
	 * by commas, search through the list looking for one that
	 * matches the connector.
	 *
	 * If there's one or more that doesn't specify a connector, keep
	 * the last one found one as a fallback.
	 */
	fwstr = kstrdup(edid_firmware, GFP_KERNEL);
	if (!fwstr)
		return ERR_PTR(-ENOMEM);
	edidstr = fwstr;

	while ((edidname = strsep(&edidstr, ","))) {
		colon = strchr(edidname, ':');
		if (colon != NULL) {
			if (strncmp(connector->name, edidname, colon - edidname))
				continue;
			edidname = colon + 1;
			break;
		}

		if (*edidname != '\0') /* corner case: multiple ',' */
			fallback = edidname;
	}

	if (!edidname) {
		if (!fallback) {
			kfree(fwstr);
			return ERR_PTR(-ENOENT);
		}
		edidname = fallback;
	}

	last = edidname + strlen(edidname) - 1;
	if (*last == '\n')
		*last = '\0';

	drm_edid = edid_load(connector, edidname);

	kfree(fwstr);

	return drm_edid;
}
