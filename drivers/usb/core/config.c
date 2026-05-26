// SPDX-License-Identifier: GPL-2.0
/*
 * Released under the GPLv2 only.
 */

/*
 * config.c -- USB 配置描述符解析器
 *
 * 本文件实现了 USB 配置描述符(configuration descriptor)的读取和解析
 * 功能。它将 USB 设备返回的扁平二进制描述符流解析为层次化的
 * struct usb_host_config 树形结构, 供 USB 核心和驱动使用。
 *
 * ==================== 解析层次结构 ====================
 * 一个 USB 设备的描述符按如下层次组织:
 *
 *   struct usb_host_config          (一个配置)
 *     +-- struct usb_interface_cache (接口缓存, 每个接口一个)
 *     |     +-- struct usb_host_interface (备选设置, altsetting)
 *     |           +-- struct usb_host_endpoint (端点描述符)
 *     |           +-- extra (类/厂商特定描述符)
 *     +-- extra                     (配置级类/厂商特定描述符)
 *     +-- intf_assoc[]              (接口关联描述符, IAD)
 *
 * 解析流程:
 *   usb_get_configuration()
 *     → 读取原始描述符二进制数据
 *     → usb_parse_configuration()   [配置级解析]
 *         → 前向扫描: 统计接口和备选设置数量
 *         → 分配 intf_cache + altsetting 数组
 *         → 解析 IAD (接口关联描述符)
 *         → usb_parse_interface()   [接口级解析]
 *             → usb_parse_endpoint() [端点级解析]
 *                 → usb_parse_ss_endpoint_companion()    [SuperSpeed 扩展]
 *                 → usb_parse_ssp_isoc_endpoint_companion() [SuperSpeedPlus 扩展]
 *                 → usb_parse_eusb2_isoc_endpoint_companion() [eUSB2 扩展]
 *
 * ==================== 功能要点 ====================
 *  - 容错解析: 对描述符长度错误、字段越界、重复端点等异常情况
 *    进行容错处理, 尽可能使设备可用而非直接拒绝。
 *  - 最大值限制: 接口数(USB_MAXINTERFACES=32)、备选设置数
 *    (USB_MAXALTSETTING=128)、端点数(USB_MAXENDPOINTS=30)、
 *    配置数(USB_MAXCONFIG=8)等均有硬限制。
 *  - 速度适配: 根据设备速度(Low/Full/High/Super/SuperSpeedPlus)
 *    校验 wMaxPacketSize 的合法性。
 *  - 描述符类型识别: 识别并跳过类/厂商特定描述符, 同时保留
 *    它们的原始数据供驱动使用(保存在 extra/extralen 字段)。
 *  - BOS 解析: usb_get_bos_descriptor() 解析 BOS (Binary Object
 *    Store) 描述符集, 包含 USB 3.0 及更高版本的能力描述。
 *  - SuperSpeed 扩展: 解析 SS Endpoint Companion、SSP Isoc Endpoint
 *    Companion 和 eUSB2 Isoc Endpoint Companion 描述符。
 */

#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/hcd.h>
#include <linux/usb/quirks.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string_choices.h>
#include <linux/device.h>
#include <asm/byteorder.h>
#include "usb.h"


#define USB_MAXALTSETTING		128	/* Hard limit */

#define USB_MAXCONFIG			8	/* Arbitrary limit */

/*
 * find_next_descriptor - 在描述符流中查找下一个指定类型的描述符
 *
 * 这是解析器的核心辅助函数。它在原始二进制描述符缓冲区中顺序扫描,
 * 寻找类型为 dt1 或 dt2 的下一个描述符, 跳过所有其他类型的描述符。
 *
 * @buffer: 描述符缓冲区的起始位置
 * @size:   缓冲区剩余大小
 * @dt1:    要查找的第一个描述符类型(如 USB_DT_ENDPOINT)
 * @dt2:    要查找的第二个描述符类型(如 USB_DT_INTERFACE),
 *          可以与 dt1 相同(用于跳过同一类型间的其他描述符)
 * @num_skipped: [输出] 跳过的描述符数量
 *
 * 返回: 从 buffer 到找到的描述符之间的字节偏移量。
 * 如果未找到匹配类型, 返回 buffer - buffer + size。
 *
 * 典型用例:
 *   解析完一个端点描述符后, 使用此函数跳过类/厂商特定描述符,
 *   定位到下一个端点或接口描述符。
 */
static int find_next_descriptor(unsigned char *buffer, int size,
    int dt1, int dt2, int *num_skipped)
{
	struct usb_descriptor_header *h;
	int n = 0;
	unsigned char *buffer0 = buffer;

	/* Find the next descriptor of type dt1 or dt2 */
	while (size > 0) {
		h = (struct usb_descriptor_header *) buffer;
		if (h->bDescriptorType == dt1 || h->bDescriptorType == dt2)
			break;
		buffer += h->bLength;
		size -= h->bLength;
		++n;
	}

	/* Store the number of descriptors skipped and return the
	 * number of bytes skipped */
	if (num_skipped)
		*num_skipped = n;
	return buffer - buffer0;
}

/*
 * usb_parse_ssp_isoc_endpoint_companion - 解析 SuperSpeedPlus 同步端点伴生描述符
 *
 * SuperSpeedPlus (USB 3.1+) 引入了额外的同步端点伴生描述符
 * (USB_DT_SSP_ISOC_ENDPOINT_COMP), 紧跟在 SuperSpeed 端点伴生
 * 描述符(USB_DT_SS_ENDPOINT_COMP)之后。仅当 SS 伴生描述符的
 * bmAttributes 中标记了 SSP 同步补偿(SSP_ISOC_COMP)时才存在。
 *
 * 该描述符提供了更精细的同步端点带宽管理信息, 如每个服务间隔的
 * 最大字节数等(用于替代 SS 伴生描述符中的 wBytesPerInterval)。
 */
static void usb_parse_ssp_isoc_endpoint_companion(struct device *ddev,
		int cfgno, int inum, int asnum, struct usb_host_endpoint *ep,
		unsigned char *buffer, int size)
{
	struct usb_ssp_isoc_ep_comp_descriptor *desc;

	/*
	 * The SuperSpeedPlus Isoc endpoint companion descriptor immediately
	 * follows the SuperSpeed Endpoint Companion descriptor
	 */
	desc = (struct usb_ssp_isoc_ep_comp_descriptor *) buffer;
	if (size < USB_DT_SSP_ISOC_EP_COMP_SIZE ||
	    desc->bDescriptorType != USB_DT_SSP_ISOC_ENDPOINT_COMP) {
		dev_notice(ddev, "Invalid SuperSpeedPlus isoc endpoint companion"
			 "for config %d interface %d altsetting %d ep %d.\n",
			 cfgno, inum, asnum, ep->desc.bEndpointAddress);
		return;
	}
	memcpy(&ep->ssp_isoc_ep_comp, desc, USB_DT_SSP_ISOC_EP_COMP_SIZE);
}

/*
 * usb_parse_eusb2_isoc_endpoint_companion - 解析 eUSB2 同步端点伴生描述符
 *
 * eUSB2 (embedded USB 2.0) 规范定义了一个同步端点伴生描述符
 * (USB_DT_EUSB2_ISOC_ENDPOINT_COMP), 用于 eUSB2 设备在高带宽
 * 同步传输时的额外配置。
 *
 * 该描述符应出现在端点描述符之后、下一个端点或接口描述符之前。
 * 本函数在缓冲区中搜索该描述符类型, 找到则复制到 endpoint 的
 * eusb2_isoc_ep_comp 字段。
 *
 * eUSB2 是 USB-IF 为嵌入式设备定义的低功耗、高集成度 USB 标准,
 * 主要用于手机、IoT 等嵌入式场景。
 */
static void usb_parse_eusb2_isoc_endpoint_companion(struct device *ddev,
		int cfgno, int inum, int asnum, struct usb_host_endpoint *ep,
		unsigned char *buffer, int size)
{
	struct usb_eusb2_isoc_ep_comp_descriptor *desc;
	struct usb_descriptor_header *h;

	/*
	 * eUSB2 isochronous endpoint companion descriptor for this endpoint
	 * shall be declared before the next endpoint or interface descriptor
	 */
	while (size >= USB_DT_EUSB2_ISOC_EP_COMP_SIZE) {
		h = (struct usb_descriptor_header *)buffer;

		if (h->bDescriptorType == USB_DT_EUSB2_ISOC_ENDPOINT_COMP) {
			desc = (struct usb_eusb2_isoc_ep_comp_descriptor *)buffer;
			ep->eusb2_isoc_ep_comp = *desc;
			return;
		}
		if (h->bDescriptorType == USB_DT_ENDPOINT ||
		    h->bDescriptorType == USB_DT_INTERFACE)
			break;

		buffer += h->bLength;
		size -= h->bLength;
	}

	dev_notice(ddev, "No eUSB2 isoc ep %d companion for config %d interface %d altsetting %d\n",
		   ep->desc.bEndpointAddress, cfgno, inum, asnum);
}

/*
 * usb_parse_ss_endpoint_companion - 解析 SuperSpeed 端点伴生描述符
 *
 * USB 3.0 (SuperSpeed) 引入了一个新的端点伴生描述符
 * (USB_DT_SS_ENDPOINT_COMP), 紧跟在每个端点描述符之后。
 * 该描述符提供了 SuperSpeed 特有的端点属性:
 *
 *   - bMaxBurst: 最大突发包数(0-15), 决定每个服务间隔可发送的
 *     最大数据包数量
 *   - bmAttributes:
 *       - Bulk 端点: 位 0-4 表示流能力(最多 65536 个流)
 *       - Isoch 端点: 位 0-1 表示 Mult (每个微帧的突发次数)
 *       - 位 7 表示 SSP 同步补偿(SSP_ISOC_COMP)
 *   - wBytesPerInterval: 每个服务间隔传输的字节数
 *
 * 如果缺少 USB_DT_SS_ENDPOINT_COMP 描述符, 函数使用默认值填充
 * (单包传输), 确保设备仍然可用。
 *
 * 验证逻辑:
 *   - Control 端点不允许 bMaxBurst > 0
 *   - 所有端点的 bMaxBurst 不能超过 15
 *   - Control/Interrupt 端点的 bmAttributes 必须为 0
 *   - Bulk 端点的流数不能超过 65536 (bmAttributes > 16)
 *   - Isoch 端点的 Mult 不能超过 3
 *   - wBytesPerInterval 不能超过计算的理论最大值
 *
 * 解析完成后, 如果设备是 SuperSpeedPlus 且端点标记了 SSP 同步补偿,
 * 继续解析 SSP Isochronous Endpoint Companion 描述符。
 */
static void usb_parse_ss_endpoint_companion(struct device *ddev, int cfgno,
		int inum, int asnum, struct usb_host_endpoint *ep,
		unsigned char *buffer, int size)
{
	struct usb_ss_ep_comp_descriptor *desc;
	int max_tx;

	/* The SuperSpeed endpoint companion descriptor is supposed to
	 * be the first thing immediately following the endpoint descriptor.
	 */
	desc = (struct usb_ss_ep_comp_descriptor *) buffer;

	if (size < USB_DT_SS_EP_COMP_SIZE) {
		dev_notice(ddev,
			   "invalid SuperSpeed endpoint companion descriptor "
			   "of length %d, skipping\n", size);
		return;
	}

	if (desc->bDescriptorType != USB_DT_SS_ENDPOINT_COMP) {
		dev_notice(ddev, "No SuperSpeed endpoint companion for config %d "
				" interface %d altsetting %d ep %d: "
				"using minimum values\n",
				cfgno, inum, asnum, ep->desc.bEndpointAddress);

		/* Fill in some default values.
		 * Leave bmAttributes as zero, which will mean no streams for
		 * bulk, and isoc won't support multiple bursts of packets.
		 * With bursts of only one packet, and a Mult of 1, the max
		 * amount of data moved per endpoint service interval is one
		 * packet.
		 */
		ep->ss_ep_comp.bLength = USB_DT_SS_EP_COMP_SIZE;
		ep->ss_ep_comp.bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
		if (usb_endpoint_xfer_isoc(&ep->desc) ||
				usb_endpoint_xfer_int(&ep->desc))
			ep->ss_ep_comp.wBytesPerInterval =
					ep->desc.wMaxPacketSize;
		return;
	}
	buffer += desc->bLength;
	size -= desc->bLength;
	memcpy(&ep->ss_ep_comp, desc, USB_DT_SS_EP_COMP_SIZE);

	/* Check the various values */
	if (usb_endpoint_xfer_control(&ep->desc) && desc->bMaxBurst != 0) {
		dev_notice(ddev, "Control endpoint with bMaxBurst = %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to zero\n", desc->bMaxBurst,
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		ep->ss_ep_comp.bMaxBurst = 0;
	} else if (desc->bMaxBurst > 15) {
		dev_notice(ddev, "Endpoint with bMaxBurst = %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to 15\n", desc->bMaxBurst,
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		ep->ss_ep_comp.bMaxBurst = 15;
	}

	if ((usb_endpoint_xfer_control(&ep->desc) ||
			usb_endpoint_xfer_int(&ep->desc)) &&
				desc->bmAttributes != 0) {
		dev_notice(ddev, "%s endpoint with bmAttributes = %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to zero\n",
				usb_endpoint_xfer_control(&ep->desc) ? "Control" : "Bulk",
				desc->bmAttributes,
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		ep->ss_ep_comp.bmAttributes = 0;
	} else if (usb_endpoint_xfer_bulk(&ep->desc) &&
			desc->bmAttributes > 16) {
		dev_notice(ddev, "Bulk endpoint with more than 65536 streams in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to max\n",
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		ep->ss_ep_comp.bmAttributes = 16;
	} else if (usb_endpoint_xfer_isoc(&ep->desc) &&
		   !USB_SS_SSP_ISOC_COMP(desc->bmAttributes) &&
		   USB_SS_MULT(desc->bmAttributes) > 3) {
		dev_notice(ddev, "Isoc endpoint has Mult of %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to 3\n",
				USB_SS_MULT(desc->bmAttributes),
				cfgno, inum, asnum, ep->desc.bEndpointAddress);
		ep->ss_ep_comp.bmAttributes = 2;
	}

	if (usb_endpoint_xfer_isoc(&ep->desc))
		max_tx = (desc->bMaxBurst + 1) *
			(USB_SS_MULT(desc->bmAttributes)) *
			usb_endpoint_maxp(&ep->desc);
	else if (usb_endpoint_xfer_int(&ep->desc))
		max_tx = usb_endpoint_maxp(&ep->desc) *
			(desc->bMaxBurst + 1);
	else
		max_tx = 999999;
	if (le16_to_cpu(desc->wBytesPerInterval) > max_tx) {
		dev_notice(ddev, "%s endpoint with wBytesPerInterval of %d in "
				"config %d interface %d altsetting %d ep %d: "
				"setting to %d\n",
				usb_endpoint_xfer_isoc(&ep->desc) ? "Isoc" : "Int",
				le16_to_cpu(desc->wBytesPerInterval),
				cfgno, inum, asnum, ep->desc.bEndpointAddress,
				max_tx);
		ep->ss_ep_comp.wBytesPerInterval = cpu_to_le16(max_tx);
	}
	/* Parse a possible SuperSpeedPlus isoc ep companion descriptor */
	if (usb_endpoint_xfer_isoc(&ep->desc) &&
	    USB_SS_SSP_ISOC_COMP(desc->bmAttributes))
		usb_parse_ssp_isoc_endpoint_companion(ddev, cfgno, inum, asnum,
							ep, buffer, size);
}

static const unsigned short low_speed_maxpacket_maxes[4] = {
	[USB_ENDPOINT_XFER_CONTROL] = 8,
	[USB_ENDPOINT_XFER_ISOC] = 0,
	[USB_ENDPOINT_XFER_BULK] = 0,
	[USB_ENDPOINT_XFER_INT] = 8,
};
static const unsigned short full_speed_maxpacket_maxes[4] = {
	[USB_ENDPOINT_XFER_CONTROL] = 64,
	[USB_ENDPOINT_XFER_ISOC] = 1023,
	[USB_ENDPOINT_XFER_BULK] = 64,
	[USB_ENDPOINT_XFER_INT] = 64,
};
static const unsigned short high_speed_maxpacket_maxes[4] = {
	[USB_ENDPOINT_XFER_CONTROL] = 64,
	[USB_ENDPOINT_XFER_ISOC] = 1024,

	/* Bulk should be 512, but some devices use 1024: we will warn below */
	[USB_ENDPOINT_XFER_BULK] = 1024,
	[USB_ENDPOINT_XFER_INT] = 1024,
};
static const unsigned short super_speed_maxpacket_maxes[4] = {
	[USB_ENDPOINT_XFER_CONTROL] = 512,
	[USB_ENDPOINT_XFER_ISOC] = 1024,
	[USB_ENDPOINT_XFER_BULK] = 1024,
	[USB_ENDPOINT_XFER_INT] = 1024,
};

static bool endpoint_is_duplicate(struct usb_endpoint_descriptor *e1,
		struct usb_endpoint_descriptor *e2)
{
	if (e1->bEndpointAddress == e2->bEndpointAddress)
		return true;

	if (usb_endpoint_xfer_control(e1) || usb_endpoint_xfer_control(e2)) {
		if (usb_endpoint_num(e1) == usb_endpoint_num(e2))
			return true;
	}

	return false;
}

/*
 * Check for duplicate endpoint addresses in other interfaces and in the
 * altsetting currently being parsed.
 */
static bool config_endpoint_is_duplicate(struct usb_host_config *config,
		int inum, int asnum, struct usb_endpoint_descriptor *d)
{
	struct usb_endpoint_descriptor *epd;
	struct usb_interface_cache *intfc;
	struct usb_host_interface *alt;
	int i, j, k;

	for (i = 0; i < config->desc.bNumInterfaces; ++i) {
		intfc = config->intf_cache[i];

		for (j = 0; j < intfc->num_altsetting; ++j) {
			alt = &intfc->altsetting[j];

			if (alt->desc.bInterfaceNumber == inum &&
					alt->desc.bAlternateSetting != asnum)
				continue;

			for (k = 0; k < alt->desc.bNumEndpoints; ++k) {
				epd = &alt->endpoint[k].desc;

				if (endpoint_is_duplicate(epd, d))
					return true;
			}
		}
	}

	return false;
}

/*
 * usb_parse_endpoint - 解析单个端点描述符
 *
 * 从描述符流中解析一个 USB 端点描述符, 执行以下处理:
 *
 *   1) 长度校验: 检查描述符长度是否至少为 USB_DT_ENDPOINT_SIZE,
 *      如果是 USB 音频端点则接受 USB_DT_ENDPOINT_AUDIO_SIZE。
 *   2) 端点号校验: 端点 0 不允许出现在非默认配置中。
 *   3) 数量限制: 不超过 ifp->desc.bNumEndpoints 声明的数量。
 *   4) 复制描述符并清理 bEndpointAddress 的保留位。
 *   5) 重复检查: 通过 config_endpoint_is_duplicate() 检查是否与
 *      其他接口或其他备选设置中的端点地址重复。
 *   6) 忽略检查: 如果设备有 USB_QUIRK_ENDPOINT_IGNORE, 检查此
 *      端点是否应被忽略。
 *   7) bInterval 修正: 根据传输类型和速度修复超出合法范围的
 *      bInterval 值。
 *       - 高速中断端点: 修正厂商误用的全速 bInterval
 *       - 低速设备的批量端点: 改为中断端点(USB 规范禁止低速批量)
 *   8) wMaxPacketSize 校验: 根据速度级别检查最大包大小。
 *       - 高速批量端点必须为 512 字节, 否则发出警告
 *       - 低速/全速/高速各有不同的最大包大小表
 *   9) 解析伴生描述符:
 *       - 高速且 bcdUSB == 0x0220 (eUSB2): 解析 eUSB2 同步伴生
 *       - SuperSpeed/SuperSpeedPlus: 解析 SS 端点伴生描述符
 *   10) 跳过类/厂商特定描述符: 使用 find_next_descriptor() 定位
 *       到下一个端点或接口描述符, 并将这些附加描述符保存在
 *       endpoint->extra / endpoint->extralen 中供驱动使用。
 */
static int usb_parse_endpoint(struct device *ddev, int cfgno,
		struct usb_host_config *config, int inum, int asnum,
		struct usb_host_interface *ifp, int num_ep,
		unsigned char *buffer, int size)
{
	struct usb_device *udev = to_usb_device(ddev);
	unsigned char *buffer0 = buffer;
	struct usb_endpoint_descriptor *d;
	struct usb_host_endpoint *endpoint;
	int n, i, j, retval;
	unsigned int maxp;
	const unsigned short *maxpacket_maxes;
	u16 bcdUSB;

	d = (struct usb_endpoint_descriptor *) buffer;
	bcdUSB = le16_to_cpu(udev->descriptor.bcdUSB);
	buffer += d->bLength;
	size -= d->bLength;

	if (d->bLength >= USB_DT_ENDPOINT_AUDIO_SIZE)
		n = USB_DT_ENDPOINT_AUDIO_SIZE;
	else if (d->bLength >= USB_DT_ENDPOINT_SIZE)
		n = USB_DT_ENDPOINT_SIZE;
	else {
		dev_notice(ddev, "config %d interface %d altsetting %d has an "
		    "invalid endpoint descriptor of length %d, skipping\n",
		    cfgno, inum, asnum, d->bLength);
		goto skip_to_next_endpoint_or_interface_descriptor;
	}

	i = usb_endpoint_num(d);
	if (i == 0) {
		dev_notice(ddev, "config %d interface %d altsetting %d has an "
		    "invalid descriptor for endpoint zero, skipping\n",
		    cfgno, inum, asnum);
		goto skip_to_next_endpoint_or_interface_descriptor;
	}

	/* Only store as many endpoints as we have room for */
	if (ifp->desc.bNumEndpoints >= num_ep)
		goto skip_to_next_endpoint_or_interface_descriptor;

	/* Save a copy of the descriptor and use it instead of the original */
	endpoint = &ifp->endpoint[ifp->desc.bNumEndpoints];
	memcpy(&endpoint->desc, d, n);
	d = &endpoint->desc;

	/* Clear the reserved bits in bEndpointAddress */
	i = d->bEndpointAddress &
			(USB_ENDPOINT_DIR_MASK | USB_ENDPOINT_NUMBER_MASK);
	if (i != d->bEndpointAddress) {
		dev_notice(ddev, "config %d interface %d altsetting %d has an endpoint descriptor with address 0x%X, changing to 0x%X\n",
		    cfgno, inum, asnum, d->bEndpointAddress, i);
		endpoint->desc.bEndpointAddress = i;
	}

	/* Check for duplicate endpoint addresses */
	if (config_endpoint_is_duplicate(config, inum, asnum, d)) {
		dev_notice(ddev, "config %d interface %d altsetting %d has a duplicate endpoint with address 0x%X, skipping\n",
				cfgno, inum, asnum, d->bEndpointAddress);
		goto skip_to_next_endpoint_or_interface_descriptor;
	}

	/* Ignore some endpoints */
	if (udev->quirks & USB_QUIRK_ENDPOINT_IGNORE) {
		if (usb_endpoint_is_ignored(udev, ifp, d)) {
			dev_notice(ddev, "config %d interface %d altsetting %d has an ignored endpoint with address 0x%X, skipping\n",
					cfgno, inum, asnum,
					d->bEndpointAddress);
			goto skip_to_next_endpoint_or_interface_descriptor;
		}
	}

	/* Accept this endpoint */
	++ifp->desc.bNumEndpoints;
	INIT_LIST_HEAD(&endpoint->urb_list);

	/*
	 * Fix up bInterval values outside the legal range.
	 * Use 10 or 8 ms if no proper value can be guessed.
	 */
	i = 0;		/* i = min, j = max, n = default */
	j = 255;
	if (usb_endpoint_xfer_int(d)) {
		i = 1;
		switch (udev->speed) {
		case USB_SPEED_SUPER_PLUS:
		case USB_SPEED_SUPER:
		case USB_SPEED_HIGH:
			/*
			 * Many device manufacturers are using full-speed
			 * bInterval values in high-speed interrupt endpoint
			 * descriptors. Try to fix those and fall back to an
			 * 8-ms default value otherwise.
			 */
			n = fls(d->bInterval*8);
			if (n == 0)
				n = 7;	/* 8 ms = 2^(7-1) uframes */
			j = 16;

			/*
			 * Adjust bInterval for quirked devices.
			 */
			/*
			 * This quirk fixes bIntervals reported in ms.
			 */
			if (udev->quirks & USB_QUIRK_LINEAR_FRAME_INTR_BINTERVAL) {
				n = clamp(fls(d->bInterval) + 3, i, j);
				i = j = n;
			}
			/*
			 * This quirk fixes bIntervals reported in
			 * linear microframes.
			 */
			if (udev->quirks & USB_QUIRK_LINEAR_UFRAME_INTR_BINTERVAL) {
				n = clamp(fls(d->bInterval), i, j);
				i = j = n;
			}
			break;
		default:		/* USB_SPEED_FULL or _LOW */
			/*
			 * For low-speed, 10 ms is the official minimum.
			 * But some "overclocked" devices might want faster
			 * polling so we'll allow it.
			 */
			n = 10;
			break;
		}
	} else if (usb_endpoint_xfer_isoc(d)) {
		i = 1;
		j = 16;
		switch (udev->speed) {
		case USB_SPEED_HIGH:
			n = 7;		/* 8 ms = 2^(7-1) uframes */
			break;
		default:		/* USB_SPEED_FULL */
			n = 4;		/* 8 ms = 2^(4-1) frames */
			break;
		}
	}
	if (d->bInterval < i || d->bInterval > j) {
		dev_notice(ddev, "config %d interface %d altsetting %d "
		    "endpoint 0x%X has an invalid bInterval %d, "
		    "changing to %d\n",
		    cfgno, inum, asnum,
		    d->bEndpointAddress, d->bInterval, n);
		endpoint->desc.bInterval = n;
	}

	/* Some buggy low-speed devices have Bulk endpoints, which is
	 * explicitly forbidden by the USB spec.  In an attempt to make
	 * them usable, we will try treating them as Interrupt endpoints.
	 */
	if (udev->speed == USB_SPEED_LOW && usb_endpoint_xfer_bulk(d)) {
		dev_notice(ddev, "config %d interface %d altsetting %d "
		    "endpoint 0x%X is Bulk; changing to Interrupt\n",
		    cfgno, inum, asnum, d->bEndpointAddress);
		endpoint->desc.bmAttributes = USB_ENDPOINT_XFER_INT;
		endpoint->desc.bInterval = 1;
		if (usb_endpoint_maxp(&endpoint->desc) > 8)
			endpoint->desc.wMaxPacketSize = cpu_to_le16(8);
	}

	/*
	 * Validate the wMaxPacketSize field.
	 * eUSB2 devices (see USB 2.0 Double Isochronous IN ECN 9.6.6 Endpoint)
	 * and devices with isochronous endpoints in altsetting 0 (see USB 2.0
	 * end of section 5.6.3) have wMaxPacketSize = 0.
	 * So don't warn about those.
	 */
	maxp = le16_to_cpu(endpoint->desc.wMaxPacketSize);

	if (maxp == 0 && bcdUSB != 0x0220 &&
	    !(usb_endpoint_xfer_isoc(d) && asnum == 0))
		dev_notice(ddev, "config %d interface %d altsetting %d endpoint 0x%X has invalid wMaxPacketSize 0\n",
		    cfgno, inum, asnum, d->bEndpointAddress);

	/* Find the highest legal maxpacket size for this endpoint */
	i = 0;		/* additional transactions per microframe */
	switch (udev->speed) {
	case USB_SPEED_LOW:
		maxpacket_maxes = low_speed_maxpacket_maxes;
		break;
	case USB_SPEED_FULL:
		maxpacket_maxes = full_speed_maxpacket_maxes;
		break;
	case USB_SPEED_HIGH:
		/* Multiple-transactions bits are allowed only for HS periodic endpoints */
		if (usb_endpoint_xfer_int(d) || usb_endpoint_xfer_isoc(d)) {
			i = maxp & USB_EP_MAXP_MULT_MASK;
			maxp &= ~i;
		}
		fallthrough;
	default:
		maxpacket_maxes = high_speed_maxpacket_maxes;
		break;
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		maxpacket_maxes = super_speed_maxpacket_maxes;
		break;
	}
	j = maxpacket_maxes[usb_endpoint_type(&endpoint->desc)];

	if (maxp > j) {
		dev_notice(ddev, "config %d interface %d altsetting %d endpoint 0x%X has invalid maxpacket %d, setting to %d\n",
		    cfgno, inum, asnum, d->bEndpointAddress, maxp, j);
		maxp = j;
		endpoint->desc.wMaxPacketSize = cpu_to_le16(i | maxp);
	}

	/*
	 * Some buggy high speed devices have bulk endpoints using
	 * maxpacket sizes other than 512.  High speed HCDs may not
	 * be able to handle that particular bug, so let's warn...
	 */
	if (udev->speed == USB_SPEED_HIGH && usb_endpoint_xfer_bulk(d)) {
		if (maxp != 512)
			dev_notice(ddev, "config %d interface %d altsetting %d "
				"bulk endpoint 0x%X has invalid maxpacket %d\n",
				cfgno, inum, asnum, d->bEndpointAddress,
				maxp);
	}

	/* Parse a possible eUSB2 periodic endpoint companion descriptor */
	if (udev->speed == USB_SPEED_HIGH && bcdUSB == 0x0220 &&
	    !le16_to_cpu(d->wMaxPacketSize) && usb_endpoint_is_isoc_in(d))
		usb_parse_eusb2_isoc_endpoint_companion(ddev, cfgno, inum, asnum,
							endpoint, buffer, size);

	/* Parse a possible SuperSpeed endpoint companion descriptor */
	if (udev->speed >= USB_SPEED_SUPER)
		usb_parse_ss_endpoint_companion(ddev, cfgno,
				inum, asnum, endpoint, buffer, size);

	/* Skip over any Class Specific or Vendor Specific descriptors;
	 * find the next endpoint or interface descriptor */
	endpoint->extra = buffer;
	i = find_next_descriptor(buffer, size, USB_DT_ENDPOINT,
			USB_DT_INTERFACE, &n);
	endpoint->extralen = i;
	retval = buffer - buffer0 + i;
	if (n > 0)
		dev_dbg(ddev, "skipped %d descriptor%s after %s\n",
		    n, str_plural(n), "endpoint");
	return retval;

skip_to_next_endpoint_or_interface_descriptor:
	i = find_next_descriptor(buffer, size, USB_DT_ENDPOINT,
	    USB_DT_INTERFACE, NULL);
	return buffer - buffer0 + i;
}

void usb_release_interface_cache(struct kref *ref)
{
	struct usb_interface_cache *intfc = ref_to_usb_interface_cache(ref);
	int j;

	for (j = 0; j < intfc->num_altsetting; j++) {
		struct usb_host_interface *alt = &intfc->altsetting[j];

		kfree(alt->endpoint);
		kfree(alt->string);
	}
	kfree(intfc);
}

/*
 * usb_parse_interface - 解析单个接口描述符及所有备选设置
 *
 * 从描述符流中解析一个 USB 接口描述符及其所有端点。
 *
 * 处理流程:
 *   1) 校验接口描述符长度(至少 USB_DT_INTERFACE_SIZE)。
 *   2) 通过 inums[] 映射找到对应的 intf_cache 条目。
 *   3) 检查备选设置编号(asnum)是否重复。
 *   4) 复制接口描述符到缓存, 递增 num_altsetting。
 *   5) 跳过类/厂商特定描述符(保存在 alt->extra), 定位到第一个
 *      端点或下一个接口描述符。
 *   6) 分配端点数组(struct usb_host_endpoint), 数量为接口描述符
 *      中声明的 bNumEndpoints (但不超过 USB_MAXENDPOINTS)。
 *   7) 循环调用 usb_parse_endpoint() 解析所有端点。
 *   8) 检查实际解析的端点数是否与 bNumEndpoints 声明的数量一致,
 *      不一致时发出通知但继续使用。
 *
 * 错误处理: 如果跳过类/厂商特定描述符后发现下一个描述符类型为
 * USB_DT_INTERFACE, 说明当前接口没有端点, 这是合法的。
 */
static int usb_parse_interface(struct device *ddev, int cfgno,
    struct usb_host_config *config, unsigned char *buffer, int size,
    u8 inums[], u8 nalts[])
{
	unsigned char *buffer0 = buffer;
	struct usb_interface_descriptor	*d;
	int inum, asnum;
	struct usb_interface_cache *intfc;
	struct usb_host_interface *alt;
	int i, n;
	int len, retval;
	int num_ep, num_ep_orig;

	d = (struct usb_interface_descriptor *) buffer;
	buffer += d->bLength;
	size -= d->bLength;

	if (d->bLength < USB_DT_INTERFACE_SIZE)
		goto skip_to_next_interface_descriptor;

	/* Which interface entry is this? */
	intfc = NULL;
	inum = d->bInterfaceNumber;
	for (i = 0; i < config->desc.bNumInterfaces; ++i) {
		if (inums[i] == inum) {
			intfc = config->intf_cache[i];
			break;
		}
	}
	if (!intfc || intfc->num_altsetting >= nalts[i])
		goto skip_to_next_interface_descriptor;

	/* Check for duplicate altsetting entries */
	asnum = d->bAlternateSetting;
	for ((i = 0, alt = &intfc->altsetting[0]);
	      i < intfc->num_altsetting;
	     (++i, ++alt)) {
		if (alt->desc.bAlternateSetting == asnum) {
			dev_notice(ddev, "Duplicate descriptor for config %d "
			    "interface %d altsetting %d, skipping\n",
			    cfgno, inum, asnum);
			goto skip_to_next_interface_descriptor;
		}
	}

	++intfc->num_altsetting;
	memcpy(&alt->desc, d, USB_DT_INTERFACE_SIZE);

	/* Skip over any Class Specific or Vendor Specific descriptors;
	 * find the first endpoint or interface descriptor */
	alt->extra = buffer;
	i = find_next_descriptor(buffer, size, USB_DT_ENDPOINT,
	    USB_DT_INTERFACE, &n);
	alt->extralen = i;
	if (n > 0)
		dev_dbg(ddev, "skipped %d descriptor%s after %s\n",
		    n, str_plural(n), "interface");
	buffer += i;
	size -= i;

	/* Allocate space for the right(?) number of endpoints */
	num_ep = num_ep_orig = alt->desc.bNumEndpoints;
	alt->desc.bNumEndpoints = 0;		/* Use as a counter */
	if (num_ep > USB_MAXENDPOINTS) {
		dev_notice(ddev, "too many endpoints for config %d interface %d "
		    "altsetting %d: %d, using maximum allowed: %d\n",
		    cfgno, inum, asnum, num_ep, USB_MAXENDPOINTS);
		num_ep = USB_MAXENDPOINTS;
	}

	if (num_ep > 0) {
		/* Can't allocate 0 bytes */
		len = sizeof(struct usb_host_endpoint) * num_ep;
		alt->endpoint = kzalloc(len, GFP_KERNEL);
		if (!alt->endpoint)
			return -ENOMEM;
	}

	/* Parse all the endpoint descriptors */
	n = 0;
	while (size > 0) {
		if (((struct usb_descriptor_header *) buffer)->bDescriptorType
		     == USB_DT_INTERFACE)
			break;
		retval = usb_parse_endpoint(ddev, cfgno, config, inum, asnum,
				alt, num_ep, buffer, size);
		if (retval < 0)
			return retval;
		++n;

		buffer += retval;
		size -= retval;
	}

	if (n != num_ep_orig)
		dev_notice(ddev, "config %d interface %d altsetting %d has %d "
		    "endpoint descriptor%s, different from the interface "
		    "descriptor's value: %d\n",
		    cfgno, inum, asnum, n, str_plural(n), num_ep_orig);
	return buffer - buffer0;

skip_to_next_interface_descriptor:
	i = find_next_descriptor(buffer, size, USB_DT_INTERFACE,
	    USB_DT_INTERFACE, NULL);
	return buffer - buffer0 + i;
}

/*
 * usb_parse_configuration - 解析 USB 配置描述符集(核心解析器)
 *
 * 这是配置描述符解析的核心函数。它将属于一个配置的所有原始描述符
 * 数据解析为 struct usb_host_config 树形结构。
 *
 * 描述符类型识别 (在遍历过程中处理的类型):
 *
 *   USB_DT_CONFIG (0x02)
 *     配置描述符头, 包含 bNumInterfaces、bConfigurationValue、
 *     bmAttributes、bMaxPower 等信息。
 *
 *   USB_DT_INTERFACE (0x04)
 *     接口描述符, 包含 bInterfaceNumber、bAlternateSetting、
 *     bNumEndpoints、bInterfaceClass/SubClass/Protocol 等。
 *     接口关联描述符 (IAD, USB_DT_INTERFACE_ASSOCIATION, 0x0B)
 *     用于将多个接口分组为一个功能(如视频设备的控制+数据接口)。
 *
 *   USB_DT_ENDPOINT (0x05)
 *     端点描述符, 在 usb_parse_interface() 中被进一步解析。
 *     类/厂商特定描述符(Class/Vendor Specific)
 *     不直接解析, 但被保留在 extra/extralen 字段中供驱动访问。
 *
 *   意外的 USB_DT_DEVICE (0x01) 和 USB_DT_CONFIG 会被记录并跳过。
 *
 * 解析步骤:
 *   第一遍扫描(forward scan):
 *     - 遍历所有描述符, 统计每个接口的备选设置数量(nalts[])
 *     - 收集 IAD (接口关联描述符)
 *     - 验证描述符格式正确性
 *   第二遍:
 *     - 根据扫描结果分配 intf_cache 和 altsetting 数组
 *     - 跳过配置级类/厂商特定描述符
 *     - 逐个调用 usb_parse_interface() 解析接口
 *   收尾:
 *     - 检查缺失的备选设置编号
 *     - 验证配置中的接口数是否与声明一致
 */
static int usb_parse_configuration(struct usb_device *dev, int cfgidx,
    struct usb_host_config *config, unsigned char *buffer, int size)
{
	struct device *ddev = &dev->dev;
	unsigned char *buffer0 = buffer;
	int cfgno;
	int nintf, nintf_orig;
	int i, j, n;
	struct usb_interface_cache *intfc;
	unsigned char *buffer2;
	int size2;
	struct usb_descriptor_header *header;
	int retval;
	u8 inums[USB_MAXINTERFACES], nalts[USB_MAXINTERFACES];
	unsigned iad_num = 0;

	memcpy(&config->desc, buffer, USB_DT_CONFIG_SIZE);
	nintf = nintf_orig = config->desc.bNumInterfaces;
	config->desc.bNumInterfaces = 0;	// Adjusted later

	if (config->desc.bDescriptorType != USB_DT_CONFIG ||
	    config->desc.bLength < USB_DT_CONFIG_SIZE ||
	    config->desc.bLength > size) {
		dev_notice(ddev, "invalid descriptor for config index %d: "
		    "type = 0x%X, length = %d\n", cfgidx,
		    config->desc.bDescriptorType, config->desc.bLength);
		return -EINVAL;
	}
	cfgno = config->desc.bConfigurationValue;

	buffer += config->desc.bLength;
	size -= config->desc.bLength;

	if (nintf > USB_MAXINTERFACES) {
		dev_notice(ddev, "config %d has too many interfaces: %d, "
		    "using maximum allowed: %d\n",
		    cfgno, nintf, USB_MAXINTERFACES);
		nintf = USB_MAXINTERFACES;
	}

	/* Go through the descriptors, checking their length and counting the
	 * number of altsettings for each interface */
	n = 0;
	for ((buffer2 = buffer, size2 = size);
	      size2 > 0;
	     (buffer2 += header->bLength, size2 -= header->bLength)) {

		if (size2 < sizeof(struct usb_descriptor_header)) {
			dev_notice(ddev, "config %d descriptor has %d excess "
			    "byte%s, ignoring\n",
			    cfgno, size2, str_plural(size2));
			break;
		}

		header = (struct usb_descriptor_header *) buffer2;
		if ((header->bLength > size2) || (header->bLength < 2)) {
			dev_notice(ddev, "config %d has an invalid descriptor "
			    "of length %d, skipping remainder of the config\n",
			    cfgno, header->bLength);
			break;
		}

		if (header->bDescriptorType == USB_DT_INTERFACE) {
			struct usb_interface_descriptor *d;
			int inum;

			d = (struct usb_interface_descriptor *) header;
			if (d->bLength < USB_DT_INTERFACE_SIZE) {
				dev_notice(ddev, "config %d has an invalid "
				    "interface descriptor of length %d, "
				    "skipping\n", cfgno, d->bLength);
				continue;
			}

			inum = d->bInterfaceNumber;

			if ((dev->quirks & USB_QUIRK_HONOR_BNUMINTERFACES) &&
			    n >= nintf_orig) {
				dev_notice(ddev, "config %d has more interface "
				    "descriptors, than it declares in "
				    "bNumInterfaces, ignoring interface "
				    "number: %d\n", cfgno, inum);
				continue;
			}

			if (inum >= nintf_orig)
				dev_notice(ddev, "config %d has an invalid "
				    "interface number: %d but max is %d\n",
				    cfgno, inum, nintf_orig - 1);

			/* Have we already encountered this interface?
			 * Count its altsettings */
			for (i = 0; i < n; ++i) {
				if (inums[i] == inum)
					break;
			}
			if (i < n) {
				if (nalts[i] < 255)
					++nalts[i];
			} else if (n < USB_MAXINTERFACES) {
				inums[n] = inum;
				nalts[n] = 1;
				++n;
			}

		} else if (header->bDescriptorType ==
				USB_DT_INTERFACE_ASSOCIATION) {
			struct usb_interface_assoc_descriptor *d;

			d = (struct usb_interface_assoc_descriptor *)header;
			if (d->bLength < USB_DT_INTERFACE_ASSOCIATION_SIZE) {
				dev_notice(ddev,
					 "config %d has an invalid interface association descriptor of length %d, skipping\n",
					 cfgno, d->bLength);
				continue;
			}

			if (iad_num == USB_MAXIADS) {
				dev_notice(ddev, "found more Interface "
					       "Association Descriptors "
					       "than allocated for in "
					       "configuration %d\n", cfgno);
			} else {
				config->intf_assoc[iad_num] = d;
				iad_num++;
			}

		} else if (header->bDescriptorType == USB_DT_DEVICE ||
			    header->bDescriptorType == USB_DT_CONFIG)
			dev_notice(ddev, "config %d contains an unexpected "
			    "descriptor of type 0x%X, skipping\n",
			    cfgno, header->bDescriptorType);

	}	/* for ((buffer2 = buffer, size2 = size); ...) */
	size = buffer2 - buffer;
	config->desc.wTotalLength = cpu_to_le16(buffer2 - buffer0);

	if (n != nintf)
		dev_notice(ddev, "config %d has %d interface%s, different from "
		    "the descriptor's value: %d\n",
		    cfgno, n, str_plural(n), nintf_orig);
	else if (n == 0)
		dev_notice(ddev, "config %d has no interfaces?\n", cfgno);
	config->desc.bNumInterfaces = nintf = n;

	/* Check for missing interface numbers */
	for (i = 0; i < nintf; ++i) {
		for (j = 0; j < nintf; ++j) {
			if (inums[j] == i)
				break;
		}
		if (j >= nintf)
			dev_notice(ddev, "config %d has no interface number "
			    "%d\n", cfgno, i);
	}

	/* Allocate the usb_interface_caches and altsetting arrays */
	for (i = 0; i < nintf; ++i) {
		j = nalts[i];
		if (j > USB_MAXALTSETTING) {
			dev_notice(ddev, "too many alternate settings for "
			    "config %d interface %d: %d, "
			    "using maximum allowed: %d\n",
			    cfgno, inums[i], j, USB_MAXALTSETTING);
			nalts[i] = j = USB_MAXALTSETTING;
		}

		intfc = kzalloc_flex(*intfc, altsetting, j);
		config->intf_cache[i] = intfc;
		if (!intfc)
			return -ENOMEM;
		kref_init(&intfc->ref);
	}

	/* FIXME: parse the BOS descriptor */

	/* Skip over any Class Specific or Vendor Specific descriptors;
	 * find the first interface descriptor */
	config->extra = buffer;
	i = find_next_descriptor(buffer, size, USB_DT_INTERFACE,
	    USB_DT_INTERFACE, &n);
	config->extralen = i;
	if (n > 0)
		dev_dbg(ddev, "skipped %d descriptor%s after %s\n",
		    n, str_plural(n), "configuration");
	buffer += i;
	size -= i;

	/* Parse all the interface/altsetting descriptors */
	while (size > 0) {
		retval = usb_parse_interface(ddev, cfgno, config,
		    buffer, size, inums, nalts);
		if (retval < 0)
			return retval;

		buffer += retval;
		size -= retval;
	}

	/* Check for missing altsettings */
	for (i = 0; i < nintf; ++i) {
		intfc = config->intf_cache[i];
		for (j = 0; j < intfc->num_altsetting; ++j) {
			for (n = 0; n < intfc->num_altsetting; ++n) {
				if (intfc->altsetting[n].desc.
				    bAlternateSetting == j)
					break;
			}
			if (n >= intfc->num_altsetting)
				dev_notice(ddev, "config %d interface %d has no "
				    "altsetting %d\n", cfgno, inums[i], j);
		}
	}

	return 0;
}

/*
 * usb_destroy_configuration - 销毁 USB 设备的配置信息
 *
 * 释放通过 usb_get_configuration() 和 usb_parse_configuration()
 * 分配的所有配置相关资源, 包括:
 *   - rawdescriptors: 每个配置的原始描述符数据
 *   - config[] 中的每个 usb_host_config
 *   - intf_cache 和 altsetting 数组
 *   - 端点数据
 *   - 描述符字符串
 *
 * 此函数仅在复位/重新初始化路径中由 hub 驱动调用, 或在断开/销毁
 * 路径内部使用。
 */
/* hub-only!! ... and only exported for reset/reinit path.
 * otherwise used internally on disconnect/destroy path
 */
void usb_destroy_configuration(struct usb_device *dev)
{
	int c, i;

	if (!dev->config)
		return;

	if (dev->rawdescriptors) {
		for (i = 0; i < dev->descriptor.bNumConfigurations; i++)
			kfree(dev->rawdescriptors[i]);

		kfree(dev->rawdescriptors);
		dev->rawdescriptors = NULL;
	}

	for (c = 0; c < dev->descriptor.bNumConfigurations; c++) {
		struct usb_host_config *cf = &dev->config[c];

		kfree(cf->string);
		for (i = 0; i < cf->desc.bNumInterfaces; i++) {
			if (cf->intf_cache[i])
				kref_put(&cf->intf_cache[i]->ref,
					  usb_release_interface_cache);
		}
	}
	kfree(dev->config);
	dev->config = NULL;
}


/*
 * usb_get_configuration - 读取并解析 USB 设备的所有配置描述符
 *
 * 这是配置解析的最高层入口。为 USB 设备读取所有配置(configuration)
 * 的描述符, 并逐一调用 usb_parse_configuration() 解析。
 *
 * 处理流程:
 *   1) 读取设备描述符中的 bNumConfigurations, 确定配置数量。
 *   2) 为 dev->config[] 和 dev->rawdescriptors[] 分配内存。
 *   3) 对每个配置编号(cfgno):
 *      a) 先读取 USB_DT_CONFIG_SIZE 字节获取 wTotalLength。
 *      b) 根据 wTotalLength 分配缓冲区, 读取完整的配置描述符集。
 *      c) 保存原始数据到 dev->rawdescriptors[cfgno]。
 *      d) 调用 usb_parse_configuration() 解析。
 *   4) 如果读取某个配置失败(返回 -EPIPE), 截断配置列表。
 *
 * 注意: 此函数仅由 hub 驱动在复位路径或 usb_new_device() 中调用。
 * rawdescriptors 保存原始二进制数据, 供驱动通过
 * usb_get_raw_descriptor() 访问。
 *
 * 延迟初始化: 支持 USB_QUIRK_DELAY_INIT  quirks, 在读取某些需要
 * 启动延迟的设备的配置描述符前插入 200ms 等待。
 */
/*
 * Get the USB config descriptors, cache and parse'em
 *
 * hub-only!! ... and only in reset path, or usb_new_device()
 * (used by real hubs and virtual root hubs)
 */
int usb_get_configuration(struct usb_device *dev)
{
	struct device *ddev = &dev->dev;
	int ncfg = dev->descriptor.bNumConfigurations;
	unsigned int cfgno, length;
	unsigned char *bigbuffer;
	struct usb_config_descriptor *desc;
	int result;

	if (ncfg > USB_MAXCONFIG) {
		dev_notice(ddev, "too many configurations: %d, "
		    "using maximum allowed: %d\n", ncfg, USB_MAXCONFIG);
		dev->descriptor.bNumConfigurations = ncfg = USB_MAXCONFIG;
	}

	if (ncfg < 1 && dev->quirks & USB_QUIRK_FORCE_ONE_CONFIG) {
		dev_info(ddev, "Device claims zero configurations, forcing to 1\n");
		dev->descriptor.bNumConfigurations = 1;
		ncfg = 1;
	} else if (ncfg < 1) {
		dev_err(ddev, "no configurations\n");
		return -EINVAL;
	}

	length = ncfg * sizeof(struct usb_host_config);
	dev->config = kzalloc(length, GFP_KERNEL);
	if (!dev->config)
		return -ENOMEM;

	length = ncfg * sizeof(char *);
	dev->rawdescriptors = kzalloc(length, GFP_KERNEL);
	if (!dev->rawdescriptors)
		return -ENOMEM;

	desc = kmalloc(USB_DT_CONFIG_SIZE, GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	for (cfgno = 0; cfgno < ncfg; cfgno++) {
		/* We grab just the first descriptor so we know how long
		 * the whole configuration is */
		result = usb_get_descriptor(dev, USB_DT_CONFIG, cfgno,
		    desc, USB_DT_CONFIG_SIZE);
		if (result < 0) {
			dev_err(ddev, "unable to read config index %d "
			    "descriptor/%s: %d\n", cfgno, "start", result);
			if (result != -EPIPE)
				goto err;
			dev_notice(ddev, "chopping to %d config(s)\n", cfgno);
			dev->descriptor.bNumConfigurations = cfgno;
			break;
		} else if (result < 4) {
			dev_err(ddev, "config index %d descriptor too short "
			    "(expected %i, got %i)\n", cfgno,
			    USB_DT_CONFIG_SIZE, result);
			result = -EINVAL;
			goto err;
		}
		length = max_t(int, le16_to_cpu(desc->wTotalLength),
		    USB_DT_CONFIG_SIZE);

		/* Now that we know the length, get the whole thing */
		bigbuffer = kmalloc(length, GFP_KERNEL);
		if (!bigbuffer) {
			result = -ENOMEM;
			goto err;
		}

		if (dev->quirks & USB_QUIRK_DELAY_INIT)
			msleep(200);

		result = usb_get_descriptor(dev, USB_DT_CONFIG, cfgno,
		    bigbuffer, length);
		if (result < 0) {
			dev_err(ddev, "unable to read config index %d "
			    "descriptor/%s\n", cfgno, "all");
			kfree(bigbuffer);
			goto err;
		}
		if (result < length) {
			dev_notice(ddev, "config index %d descriptor too short "
			    "(expected %i, got %i)\n", cfgno, length, result);
			length = result;
		}

		dev->rawdescriptors[cfgno] = bigbuffer;

		result = usb_parse_configuration(dev, cfgno,
		    &dev->config[cfgno], bigbuffer, length);
		if (result < 0) {
			++cfgno;
			goto err;
		}
	}

err:
	kfree(desc);
	dev->descriptor.bNumConfigurations = cfgno;

	return result;
}

/*
 * usb_release_bos_descriptor - 释放 BOS 描述符
 *
 * 释放通过 usb_get_bos_descriptor() 分配的 BOS (Binary Object Store)
 * 描述符资源。BOS 是 USB 3.0 引入的扩展描述符机制, 用于描述设备
 * 的额外能力(如 SuperSpeed、容器 ID 等)。
 */
void usb_release_bos_descriptor(struct usb_device *dev)
{
	if (dev->bos) {
		kfree(dev->bos->desc);
		kfree(dev->bos);
		dev->bos = NULL;
	}
}

/*
 * bos_desc_len - BOS 设备能力描述符的最小长度表
 *
 * BOS (Binary Object Store) 描述符集包含多个设备能力描述符
 * (Device Capability Descriptor)。此表定义了各种已知能力类型
 * 的最小合法长度:
 *
 *   USB_CAP_TYPE_WIRELESS_USB (0x01): 无线 USB 能力
 *   USB_CAP_TYPE_EXT          (0x02): USB 扩展能力 (支持 LPM 等)
 *   USB_SS_CAP_TYPE           (0x03): SuperSpeed 能力
 *   USB_SSP_CAP_TYPE          (0x04): SuperSpeedPlus 能力
 *   CONTAINER_ID_TYPE         (0x05): 容器 ID (用于识别同一物理设备)
 *   USB_PTM_CAP_TYPE          (0x0B): PTM (精确时间测量) 能力
 *
 * 如果某个能力描述符的长度小于对应表项的值, 则该能力描述符被忽略。
 */
static const __u8 bos_desc_len[256] = {
	[USB_CAP_TYPE_WIRELESS_USB] = USB_DT_USB_WIRELESS_CAP_SIZE,
	[USB_CAP_TYPE_EXT]          = USB_DT_USB_EXT_CAP_SIZE,
	[USB_SS_CAP_TYPE]           = USB_DT_USB_SS_CAP_SIZE,
	[USB_SSP_CAP_TYPE]          = USB_DT_USB_SSP_CAP_SIZE(1),
	[CONTAINER_ID_TYPE]         = USB_DT_USB_SS_CONTN_ID_SIZE,
	[USB_PTM_CAP_TYPE]          = USB_DT_USB_PTM_ID_SIZE,
};

/*
 * usb_get_bos_descriptor - 获取并解析 BOS (Binary Object Store) 描述符集
 *
 * BOS 是 USB 3.0 规范引入的扩展描述符机制, 用于在标准配置描述符
 * 之外描述设备的额外能力。它包含一个 BOS 头描述符和多个设备能力
 * 描述符(Device Capability Descriptors)。
 *
 * 解析流程:
 *   1) 先读取 USB_DT_BOS_SIZE 字节的 BOS 头, 获取 wTotalLength
 *      和 bNumDeviceCaps。
 *   2) 分配 total_len 大小的缓冲区, 读取完整的 BOS 描述符集。
 *   3) 遍历每个设备能力描述符, 根据 bDevCapabilityType 识别:
 *      - USB_CAP_TYPE_EXT (0x02): 扩展能力 → dev->bos->ext_cap
 *        (包含 BESL/LPM 信息)
 *      - USB_SS_CAP_TYPE (0x03): SuperSpeed 能力 → dev->bos->ss_cap
 *        (包含 U1/U2 退出延迟等)
 *      - USB_SSP_CAP_TYPE (0x04): SuperSpeedPlus 能力 → dev->bos->ssp_cap
 *        (包含子链路速度属性)
 *      - CONTAINER_ID_TYPE (0x05): 容器 ID → dev->bos->ss_id
 *        (用于设备身份识别)
 *      - USB_PTM_CAP_TYPE (0x0B): PTM 能力 → dev->bos->ptm_cap
 *        (精确时间测量)
 *   4) 每个能力描述符都用 bos_desc_len[] 表验证最小长度。
 *   5) 更新 wTotalLength 为实际处理的字节数。
 *
 * 注意: 如果设备有 USB_QUIRK_NO_BOS quirk, 则跳过 BOS 解析。
 */
/* Get BOS descriptor set */
int usb_get_bos_descriptor(struct usb_device *dev)
{
	struct device *ddev = &dev->dev;
	struct usb_bos_descriptor *bos;
	struct usb_dev_cap_header *cap;
	struct usb_ssp_cap_descriptor *ssp_cap;
	unsigned char *buffer, *buffer0;
	int length, total_len, num, i, ssac;
	__u8 cap_type;
	int ret;

	if (dev->quirks & USB_QUIRK_NO_BOS) {
		dev_dbg(ddev, "skipping BOS descriptor\n");
		return -ENOMSG;
	}

	bos = kzalloc_obj(*bos);
	if (!bos)
		return -ENOMEM;

	/* Get BOS descriptor */
	ret = usb_get_descriptor(dev, USB_DT_BOS, 0, bos, USB_DT_BOS_SIZE);
	if (ret < USB_DT_BOS_SIZE || bos->bLength < USB_DT_BOS_SIZE) {
		dev_notice(ddev, "unable to get BOS descriptor or descriptor too short\n");
		if (ret >= 0)
			ret = -ENOMSG;
		kfree(bos);
		return ret;
	}

	length = bos->bLength;
	total_len = le16_to_cpu(bos->wTotalLength);
	num = bos->bNumDeviceCaps;
	kfree(bos);
	if (total_len < length)
		return -EINVAL;

	dev->bos = kzalloc_obj(*dev->bos);
	if (!dev->bos)
		return -ENOMEM;

	/* Now let's get the whole BOS descriptor set */
	buffer = kzalloc(total_len, GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto err;
	}
	dev->bos->desc = (struct usb_bos_descriptor *)buffer;

	ret = usb_get_descriptor(dev, USB_DT_BOS, 0, buffer, total_len);
	if (ret < total_len) {
		dev_notice(ddev, "unable to get BOS descriptor set\n");
		if (ret >= 0)
			ret = -ENOMSG;
		goto err;
	}

	buffer0 = buffer;
	total_len -= length;
	buffer += length;

	for (i = 0; i < num; i++) {
		cap = (struct usb_dev_cap_header *)buffer;

		if (total_len < sizeof(*cap) || total_len < cap->bLength) {
			dev->bos->desc->bNumDeviceCaps = i;
			break;
		}
		cap_type = cap->bDevCapabilityType;
		length = cap->bLength;
		if (bos_desc_len[cap_type] && length < bos_desc_len[cap_type]) {
			dev->bos->desc->bNumDeviceCaps = i;
			break;
		}

		if (cap->bDescriptorType != USB_DT_DEVICE_CAPABILITY) {
			dev_notice(ddev, "descriptor type invalid, skip\n");
			goto skip_to_next_descriptor;
		}

		switch (cap_type) {
		case USB_CAP_TYPE_EXT:
			dev->bos->ext_cap =
				(struct usb_ext_cap_descriptor *)buffer;
			break;
		case USB_SS_CAP_TYPE:
			dev->bos->ss_cap =
				(struct usb_ss_cap_descriptor *)buffer;
			break;
		case USB_SSP_CAP_TYPE:
			ssp_cap = (struct usb_ssp_cap_descriptor *)buffer;
			ssac = (le32_to_cpu(ssp_cap->bmAttributes) &
				USB_SSP_SUBLINK_SPEED_ATTRIBS);
			if (length >= USB_DT_USB_SSP_CAP_SIZE(ssac))
				dev->bos->ssp_cap = ssp_cap;
			break;
		case CONTAINER_ID_TYPE:
			dev->bos->ss_id =
				(struct usb_ss_container_id_descriptor *)buffer;
			break;
		case USB_PTM_CAP_TYPE:
			dev->bos->ptm_cap =
				(struct usb_ptm_cap_descriptor *)buffer;
			break;
		default:
			break;
		}

skip_to_next_descriptor:
		total_len -= length;
		buffer += length;
	}
	dev->bos->desc->wTotalLength = cpu_to_le16(buffer - buffer0);

	return 0;

err:
	usb_release_bos_descriptor(dev);
	return ret;
}
