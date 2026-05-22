/*
 * Copyright 2003 José Fonseca.
 * Copyright 2003 Leif Delgass.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * 文件名: drm_pci.c
 *
 * 中文描述: DRM PCI 辅助函数
 *
 * 本文件提供了 DRM 子系统与 PCI 总线相关的辅助功能。PCI（Peripheral Component
 * Interconnect，外设组件互连标准）是显卡设备最常用的总线接口之一。
 *
 * 核心功能：
 *   1. drm_pci_set_busid() - 为 DRM master 设置 PCI 总线标识符
 *   2. drm_get_pci_domain() - 获取 PCI domain 号（处理了 Alpha 架构的特殊情况）
 *
 * 总线标识符的格式为 "pci:domain:bus:device.function"，用于在用户空间唯一
 * 标识一个 PCI 显卡设备。
 */

#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include <drm/drm_auth.h>
#include <drm/drm.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

/*
 * drm_get_pci_domain - 获取 PCI domain 号
 * @dev: DRM 设备
 *
 * 获取 PCI domain（域）号。在大多数架构上，由于历史原因，
 * 为了保持用户空间接口兼容性，当驱动接口版本低于 1.4 时返回 0。
 * 只有 Alpha 架构从一开始就正确地返回了 domain 号。
 *
 * 返回：PCI domain 号
 */
static int drm_get_pci_domain(struct drm_device *dev)
{
#ifndef __alpha__
	/* For historical reasons, drm_get_pci_domain() is busticated
	 * on most archs and has to remain so for userspace interface
	 * < 1.4, except on alpha which was right from the beginning
	 */
	if (dev->if_version < 0x10004)
		return 0;
#endif /* __alpha__ */

	return pci_domain_nr(to_pci_dev(dev->dev)->bus);
}

/*
 * drm_pci_set_busid - 为 DRM master 设置 PCI 总线标识符
 * @dev: DRM 设备
 * @master: DRM master 对象
 *
 * 生成 PCI 总线标识符字符串，格式为 "pci:domain:bus:device.function"，
 * 并将其设置到 master->unique 中。该标识符用于在用户空间唯一标识
 * PCI 显卡设备，是 DRM 设备认证和查找的重要依据。
 *
 * 返回：0 成功，-ENOMEM 内存不足
 */
int drm_pci_set_busid(struct drm_device *dev, struct drm_master *master)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	master->unique = kasprintf(GFP_KERNEL, "pci:%04x:%02x:%02x.%d",
					drm_get_pci_domain(dev),
					pdev->bus->number,
					PCI_SLOT(pdev->devfn),
					PCI_FUNC(pdev->devfn));
	if (!master->unique)
		return -ENOMEM;

	master->unique_len = strlen(master->unique);
	return 0;
}
