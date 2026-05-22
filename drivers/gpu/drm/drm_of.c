// SPDX-License-Identifier: GPL-2.0-only
/*
 * 文件名: drm_of.c
 *
 * 中文描述: DRM Device Tree（设备树）辅助函数
 *
 * 本文件提供了一组辅助函数，帮助 DRM 驱动程序解析标准设备树（DT）属性。
 * 设备树是嵌入式系统和 ARM 平台上描述硬件拓扑结构的标准方式，DRM 驱动通过
 * DT 节点来发现和连接显示管线的各个组件。
 *
 * 核心功能包括：
 *   1. CRTC 端口查询 (drm_of_crtc_port_mask) - 通过端口 DT 节点查找对应 CRTC 的掩码
 *   2. 可能的 CRTC 查找 (drm_of_find_possible_crtcs) - 扫描编码器端口的所有端点，生成可连接的 CRTC 掩码
 *   3. 组件驱动探针 (drm_of_component_probe) - 基于 DT 解析结果自动探测和绑定组件
 *   4. 面板/桥接器查找 (drm_of_find_panel_or_bridge) - 通过 DT 端口和端点号查找连接的 panel 或 bridge
 *   5. LVDS 双链路支持 (drm_of_lvds_get_dual_link_pixel_order) - 获取双链路 LVDS 的像素传输顺序
 *   6. LVDS 数据映射 (drm_of_lvds_get_data_mapping) - 将 DT "data-mapping" 属性转换为媒体总线格式
 *   7. 数据通道计数 (drm_of_get_data_lanes_count) - 获取 DSI/(e)DP 数据通道数量
 *   8. DSI 总线查找 (drm_of_get_dsi_bus) - 通过 DT 查找 DSI 总线主机
 */

#include <linux/component.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/media-bus-format.h>
#include <linux/of.h>
#include <linux/of_graph.h>

#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>

/**
 * DOC: overview
 *
 * A set of helper functions to aid DRM drivers in parsing standard DT
 * properties.
 */

/**
 * drm_of_crtc_port_mask - 通过端口 DT 节点查找已注册 CRTC 的掩码
 * @dev: DRM 设备
 * @port: 端口 OF 节点
 *
 * 给定一个端口 OF 节点，遍历设备的所有 CRTC，查找端口匹配的 CRTC。
 * 返回该 CRTC 在 possible_crtc 掩码中的对应位（1 << index）。
 * 如果未找到则返回 0。
 *
 * 此掩码用于设置编码器（encoder）的 possible_crtcs 字段，
 * 表示该编码器可以连接到哪些 CRTC。
 */
uint32_t drm_of_crtc_port_mask(struct drm_device *dev,
			    struct device_node *port)
{
	unsigned int index = 0;
	struct drm_crtc *tmp;

	drm_for_each_crtc(tmp, dev) {
		if (tmp->port == port)
			return 1 << index;

		index++;
	}

	return 0;
}
EXPORT_SYMBOL(drm_of_crtc_port_mask);

/**
 * drm_of_find_possible_crtcs - 为编码器端口查找可能的 CRTC
 * @dev: DRM 设备
 * @port: 要扫描端点的编码器端口
 *
 * 扫描附加到指定端口的所有端点，找到每个端点连接的远程端口，
 * 确定对应的 CRTC，并生成该编码器可连接的 CRTC 掩码。
 *
 * 这是 DRM 驱动 probe 流程中的关键步骤，确保编码器能够正确
 * 关联到其支持的 CRTC。
 *
 * See https://github.com/devicetree-org/dt-schema/blob/main/dtschema/schemas/graph.yaml
 * for the bindings.
 */
uint32_t drm_of_find_possible_crtcs(struct drm_device *dev,
				    struct device_node *port)
{
	struct device_node *remote_port, *ep;
	uint32_t possible_crtcs = 0;

	for_each_endpoint_of_node(port, ep) {
		remote_port = of_graph_get_remote_port(ep);
		if (!remote_port) {
			of_node_put(ep);
			return 0;
		}

		possible_crtcs |= drm_of_crtc_port_mask(dev, remote_port);

		of_node_put(remote_port);
	}

	return possible_crtcs;
}
EXPORT_SYMBOL(drm_of_find_possible_crtcs);

/**
 * drm_of_component_match_add - 添加组件 helper 的 OF 节点匹配规则
 * @master: master 设备
 * @matchptr: 组件匹配指针
 * @compare: 用于匹配组件的比较函数
 * @node: OF 节点
 *
 * 将设备树节点添加到组件匹配列表中。获取节点引用并调用
 * component_match_add_release() 注册匹配规则，释放时
 * 使用 component_release_of 自动释放 OF 节点引用。
 */
void drm_of_component_match_add(struct device *master,
				struct component_match **matchptr,
				int (*compare)(struct device *, void *),
				struct device_node *node)
{
	of_node_get(node);
	component_match_add_release(master, matchptr, component_release_of,
				    compare, node);
}
EXPORT_SYMBOL_GPL(drm_of_component_match_add);

/**
 * drm_of_component_probe - 基于组件驱动模型的通用 probe 函数
 * @dev: master device containing the OF node
 * @compare_of: compare function used for matching components
 * @m_ops: component master ops to be used
 *
 * Parse the platform device OF node and bind all the components associated
 * with the master. Interface ports are added before the encoders in order to
 * satisfy their .bind requirements
 *
 * See https://github.com/devicetree-org/dt-schema/blob/main/dtschema/schemas/graph.yaml
 * for the bindings.
 *
 * Returns zero if successful, or one of the standard error codes if it fails.
 */
int drm_of_component_probe(struct device *dev,
			   int (*compare_of)(struct device *, void *),
			   const struct component_master_ops *m_ops)
{
	struct device_node *ep, *port, *remote;
	struct component_match *match = NULL;
	int i;

	if (!dev->of_node)
		return -EINVAL;

	/*
	 * Bind the crtc's ports first, so that drm_of_find_possible_crtcs()
	 * called from encoder's .bind callbacks works as expected
	 */
	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		if (of_device_is_available(port->parent))
			drm_of_component_match_add(dev, &match, compare_of,
						   port);

		of_node_put(port);
	}

	if (i == 0) {
		dev_err(dev, "missing 'ports' property\n");
		return -ENODEV;
	}

	if (!match) {
		dev_err(dev, "no available port\n");
		return -ENODEV;
	}

	/*
	 * For bound crtcs, bind the encoders attached to their remote endpoint
	 */
	for (i = 0; ; i++) {
		port = of_parse_phandle(dev->of_node, "ports", i);
		if (!port)
			break;

		if (!of_device_is_available(port->parent)) {
			of_node_put(port);
			continue;
		}

		for_each_child_of_node(port, ep) {
			remote = of_graph_get_remote_port_parent(ep);
			if (!remote || !of_device_is_available(remote)) {
				of_node_put(remote);
				continue;
			} else if (!of_device_is_available(remote->parent)) {
				dev_warn(dev, "parent device of %pOF is not available\n",
					 remote);
				of_node_put(remote);
				continue;
			}

			drm_of_component_match_add(dev, &match, compare_of,
						   remote);
			of_node_put(remote);
		}
		of_node_put(port);
	}

	return component_master_add_with_match(dev, m_ops, match);
}
EXPORT_SYMBOL(drm_of_component_probe);

/*
 * drm_of_encoder_active_endpoint - 获取激活的编码器端点
 * @node: 包含编码器输入端口的设备树节点
 * @encoder: DRM 编码器
 *
 * 给定编码器设备节点和一个已连接 CRTC 的 drm_encoder，
 * 解析连接到该 CRTC 端口的编码器端点信息。
 *
 * 返回：0 成功，-EINVAL 参数无效
 */
int drm_of_encoder_active_endpoint(struct device_node *node,
				   struct drm_encoder *encoder,
				   struct of_endpoint *endpoint)
{
	struct device_node *ep;
	struct drm_crtc *crtc = encoder->crtc;
	struct device_node *port;
	int ret;

	if (!node || !crtc)
		return -EINVAL;

	for_each_endpoint_of_node(node, ep) {
		port = of_graph_get_remote_port(ep);
		of_node_put(port);
		if (port == crtc->port) {
			ret = of_graph_parse_endpoint(ep, endpoint);
			of_node_put(ep);
			return ret;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(drm_of_encoder_active_endpoint);

/**
 * drm_of_find_panel_or_bridge - 返回连接的 panel 或 bridge 设备
 *
 * 注意：此函数已废弃，新驱动应使用 devm_drm_of_get_bridge()。
 * @np: device tree node containing encoder output ports
 * @port: port in the device tree node
 * @endpoint: endpoint in the device tree node
 * @panel: pointer to hold returned drm_panel
 * @bridge: pointer to hold returned drm_bridge
 *
 * Given a DT node's port and endpoint number, find the connected node and
 * return either the associated struct drm_panel or drm_bridge device. Either
 * @panel or @bridge must not be NULL.
 *
 * This function is deprecated and should not be used in new drivers. Use
 * devm_drm_of_get_bridge() instead.
 *
 * Returns zero if successful, or one of the standard error codes if it fails.
 */
int drm_of_find_panel_or_bridge(const struct device_node *np,
				int port, int endpoint,
				struct drm_panel **panel,
				struct drm_bridge **bridge)
{
	int ret = -EPROBE_DEFER;
	struct device_node *remote;

	if (!panel && !bridge)
		return -EINVAL;
	if (panel)
		*panel = NULL;

	/*
	 * of_graph_get_remote_node() produces a noisy error message if port
	 * node isn't found and the absence of the port is a legit case here,
	 * so at first we silently check whether graph presents in the
	 * device-tree node.
	 */
	if (!of_graph_is_present(np))
		return -ENODEV;

	remote = of_graph_get_remote_node(np, port, endpoint);
	if (!remote)
		return -ENODEV;

	if (panel) {
		*panel = of_drm_find_panel(remote);
		if (!IS_ERR(*panel))
			ret = 0;
		else
			*panel = NULL;
	}

	if (bridge) {
		if (ret) {
			/* No panel found yet, check for a bridge next. */
			*bridge = of_drm_find_bridge(remote);
			if (*bridge)
				ret = 0;
		} else {
			*bridge = NULL;
		}

	}

	of_node_put(remote);
	return ret;
}
EXPORT_SYMBOL_GPL(drm_of_find_panel_or_bridge);

enum drm_of_lvds_pixels {
	DRM_OF_LVDS_EVEN = BIT(0),
	DRM_OF_LVDS_ODD = BIT(1),
};

static int drm_of_lvds_get_port_pixels_type(struct device_node *port_node)
{
	bool even_pixels =
		of_property_read_bool(port_node, "dual-lvds-even-pixels");
	bool odd_pixels =
		of_property_read_bool(port_node, "dual-lvds-odd-pixels");

	return (even_pixels ? DRM_OF_LVDS_EVEN : 0) |
	       (odd_pixels ? DRM_OF_LVDS_ODD : 0);
}

static int drm_of_lvds_get_remote_pixels_type(
			const struct device_node *port_node)
{
	struct device_node *endpoint = NULL;
	int pixels_type = -EPIPE;

	for_each_child_of_node(port_node, endpoint) {
		struct device_node *remote_port;
		int current_pt;

		if (!of_node_name_eq(endpoint, "endpoint"))
			continue;

		remote_port = of_graph_get_remote_port(endpoint);
		if (!remote_port) {
			of_node_put(endpoint);
			return -EPIPE;
		}

		current_pt = drm_of_lvds_get_port_pixels_type(remote_port);
		of_node_put(remote_port);
		if (pixels_type < 0)
			pixels_type = current_pt;

		/*
		 * Sanity check, ensure that all remote endpoints have the same
		 * pixel type. We may lift this restriction later if we need to
		 * support multiple sinks with different dual-link
		 * configurations by passing the endpoints explicitly to
		 * drm_of_lvds_get_dual_link_pixel_order().
		 */
		if (!current_pt || pixels_type != current_pt) {
			of_node_put(endpoint);
			return -EINVAL;
		}
	}

	return pixels_type;
}

static int __drm_of_lvds_get_dual_link_pixel_order(int p1_pt, int p2_pt)
{
	/*
	 * A valid dual-lVDS bus is found when one port is marked with
	 * "dual-lvds-even-pixels", and the other port is marked with
	 * "dual-lvds-odd-pixels", bail out if the markers are not right.
	 */
	if (p1_pt + p2_pt != DRM_OF_LVDS_EVEN + DRM_OF_LVDS_ODD)
		return -EINVAL;

	return p1_pt == DRM_OF_LVDS_EVEN ?
		DRM_LVDS_DUAL_LINK_EVEN_ODD_PIXELS :
		DRM_LVDS_DUAL_LINK_ODD_EVEN_PIXELS;
}

/**
 * drm_of_lvds_get_dual_link_pixel_order - 获取 LVDS 双链路源的像素传输顺序
 *
 * LVDS 双链路连接由两条链路组成，偶数像素在一链路上传输，
 * 奇数像素在另一链路上。此函数根据连接 sink 的需求，返回
 * LVDS 双链路源的两个端口分别传输偶数和奇数像素的顺序。
 *
 * 像素顺序由 sink 的 DT 端口节点中的 dual-lvds-even-pixels 和
 * dual-lvds-odd-pixels 属性决定。
 * @port1: First DT port node of the Dual-link LVDS source
 * @port2: Second DT port node of the Dual-link LVDS source
 *
 * An LVDS dual-link connection is made of two links, with even pixels
 * transitting on one link, and odd pixels on the other link. This function
 * returns, for two ports of an LVDS dual-link source, which port shall transmit
 * the even and odd pixels, based on the requirements of the connected sink.
 *
 * The pixel order is determined from the dual-lvds-even-pixels and
 * dual-lvds-odd-pixels properties in the sink's DT port nodes. If those
 * properties are not present, or if their usage is not valid, this function
 * returns -EINVAL.
 *
 * If either port is not connected, this function returns -EPIPE.
 *
 * @port1 and @port2 are typically DT sibling nodes, but may have different
 * parents when, for instance, two separate LVDS encoders carry the even and odd
 * pixels.
 *
 * Return:
 * * DRM_LVDS_DUAL_LINK_EVEN_ODD_PIXELS - @port1 carries even pixels and @port2
 *   carries odd pixels
 * * DRM_LVDS_DUAL_LINK_ODD_EVEN_PIXELS - @port1 carries odd pixels and @port2
 *   carries even pixels
 * * -EINVAL - @port1 and @port2 are not connected to a dual-link LVDS sink, or
 *   the sink configuration is invalid
 * * -EPIPE - when @port1 or @port2 are not connected
 */
int drm_of_lvds_get_dual_link_pixel_order(const struct device_node *port1,
					  const struct device_node *port2)
{
	int remote_p1_pt, remote_p2_pt;

	if (!port1 || !port2)
		return -EINVAL;

	remote_p1_pt = drm_of_lvds_get_remote_pixels_type(port1);
	if (remote_p1_pt < 0)
		return remote_p1_pt;

	remote_p2_pt = drm_of_lvds_get_remote_pixels_type(port2);
	if (remote_p2_pt < 0)
		return remote_p2_pt;

	return __drm_of_lvds_get_dual_link_pixel_order(remote_p1_pt, remote_p2_pt);
}
EXPORT_SYMBOL_GPL(drm_of_lvds_get_dual_link_pixel_order);

/**
 * drm_of_lvds_get_dual_link_pixel_order_sink - 获取 LVDS 双链路 sink 的像素顺序
 *
 * 与 drm_of_lvds_get_dual_link_pixel_order 类似，但直接从 sink 侧
 * 的端口属性确定像素顺序，不通过远程端点间接获取。
 * @port1: First DT port node of the Dual-link LVDS sink
 * @port2: Second DT port node of the Dual-link LVDS sink
 *
 * An LVDS dual-link connection is made of two links, with even pixels
 * transitting on one link, and odd pixels on the other link. This function
 * returns, for two ports of an LVDS dual-link sink, which port shall transmit
 * the even and odd pixels, based on the requirements of the sink.
 *
 * The pixel order is determined from the dual-lvds-even-pixels and
 * dual-lvds-odd-pixels properties in the sink's DT port nodes. If those
 * properties are not present, or if their usage is not valid, this function
 * returns -EINVAL.
 *
 * If either port is not connected, this function returns -EPIPE.
 *
 * @port1 and @port2 are typically DT sibling nodes, but may have different
 * parents when, for instance, two separate LVDS decoders receive the even and
 * odd pixels.
 *
 * Return:
 * * DRM_LVDS_DUAL_LINK_EVEN_ODD_PIXELS - @port1 receives even pixels and @port2
 *   receives odd pixels
 * * DRM_LVDS_DUAL_LINK_ODD_EVEN_PIXELS - @port1 receives odd pixels and @port2
 *   receives even pixels
 * * -EINVAL - @port1 or @port2 are NULL
 * * -EPIPE - when @port1 or @port2 are not connected
 */
int drm_of_lvds_get_dual_link_pixel_order_sink(struct device_node *port1,
					       struct device_node *port2)
{
	int sink_p1_pt, sink_p2_pt;

	if (!port1 || !port2)
		return -EINVAL;

	sink_p1_pt = drm_of_lvds_get_port_pixels_type(port1);
	if (!sink_p1_pt)
		return -EPIPE;

	sink_p2_pt = drm_of_lvds_get_port_pixels_type(port2);
	if (!sink_p2_pt)
		return -EPIPE;

	return __drm_of_lvds_get_dual_link_pixel_order(sink_p1_pt, sink_p2_pt);
}
EXPORT_SYMBOL_GPL(drm_of_lvds_get_dual_link_pixel_order_sink);

/**
 * drm_of_lvds_get_data_mapping - 获取 LVDS 数据映射格式
 * @port: LVDS 源或 sink 的 DT 端口节点
 *
 * 将设备树中 "data-mapping" 属性的字符串值转换为媒体总线格式值。
 * 支持的映射关系：
 *   "jeida-18" -> MEDIA_BUS_FMT_RGB666_1X7X3_SPWG
 *   "jeida-24" -> MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA
 *   "jeida-30" -> MEDIA_BUS_FMT_RGB101010_1X7X5_JEIDA
 *   "vesa-24"  -> MEDIA_BUS_FMT_RGB888_1X7X4_SPWG
 *   "vesa-30"  -> MEDIA_BUS_FMT_RGB101010_1X7X5_SPWG
 *
 * Return:
 * * MEDIA_BUS_FMT_RGB666_1X7X3_SPWG - data-mapping is "jeida-18"
 * * MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA - data-mapping is "jeida-24"
 * * MEDIA_BUS_FMT_RGB101010_1X7X5_JEIDA - data-mapping is "jeida-30"
 * * MEDIA_BUS_FMT_RGB888_1X7X4_SPWG - data-mapping is "vesa-24"
 * * MEDIA_BUS_FMT_RGB101010_1X7X5_SPWG - data-mapping is "vesa-30"
 * * -EINVAL - the "data-mapping" property is unsupported
 * * -ENODEV - the "data-mapping" property is missing
 */
int drm_of_lvds_get_data_mapping(const struct device_node *port)
{
	const char *mapping;
	int ret;

	ret = of_property_read_string(port, "data-mapping", &mapping);
	if (ret < 0)
		return -ENODEV;

	if (!strcmp(mapping, "jeida-18"))
		return MEDIA_BUS_FMT_RGB666_1X7X3_SPWG;
	if (!strcmp(mapping, "jeida-24"))
		return MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA;
	if (!strcmp(mapping, "jeida-30"))
		return MEDIA_BUS_FMT_RGB101010_1X7X5_JEIDA;
	if (!strcmp(mapping, "vesa-24"))
		return MEDIA_BUS_FMT_RGB888_1X7X4_SPWG;
	if (!strcmp(mapping, "vesa-30"))
		return MEDIA_BUS_FMT_RGB101010_1X7X5_SPWG;

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(drm_of_lvds_get_data_mapping);

/**
 * drm_of_get_data_lanes_count - 获取 DSI/(e)DP 数据通道数
 * @endpoint: DSI/(e)DP 源或 sink 的 DT 端点节点
 * @min: 支持的最少数据通道数
 * @max: 支持的最多数据通道数
 *
 * 统计 DT 中 "data-lanes" 属性的元素数量，并检查是否在有效范围内。
 * 用于 DSI（显示串行接口）或 eDP（嵌入式 DisplayPort）的数据通道配置。
 *
 * Return:
 * * min..max - positive integer count of "data-lanes" elements
 * * -ve - the "data-lanes" property is missing or invalid
 * * -EINVAL - the "data-lanes" property is unsupported
 */
int drm_of_get_data_lanes_count(const struct device_node *endpoint,
				const unsigned int min, const unsigned int max)
{
	int ret;

	ret = of_property_count_u32_elems(endpoint, "data-lanes");
	if (ret < 0)
		return ret;

	if (ret < min || ret > max)
		return -EINVAL;

	return ret;
}
EXPORT_SYMBOL_GPL(drm_of_get_data_lanes_count);

/**
 * drm_of_get_data_lanes_count_ep - 通过端点规格获取数据通道数
 * @port: DSI/(e)DP 源或 sink 的 DT 端口节点
 * @port_reg: 父端口节点的 reg 属性值
 * @reg: 端点节点的 reg 属性值
 * @min: 支持的最少数据通道数
 * @max: 支持的最多数据通道数
 *
 * 与 drm_of_get_data_lanes_count 功能相同，但使用端口和端点
 * 寄存器值来定位端点，适用于需要精确指定端点的场景。
 *
 * Return:
 * * min..max - positive integer count of "data-lanes" elements
 * * -EINVAL - the "data-mapping" property is unsupported
 * * -ENODEV - the "data-mapping" property is missing
 */
int drm_of_get_data_lanes_count_ep(const struct device_node *port,
				   int port_reg, int reg,
				   const unsigned int min,
				   const unsigned int max)
{
	struct device_node *endpoint;
	int ret;

	endpoint = of_graph_get_endpoint_by_regs(port, port_reg, reg);
	ret = drm_of_get_data_lanes_count(endpoint, min, max);
	of_node_put(endpoint);

	return ret;
}
EXPORT_SYMBOL_GPL(drm_of_get_data_lanes_count_ep);

#if IS_ENABLED(CONFIG_DRM_MIPI_DSI)

/**
 * drm_of_get_dsi_bus - 查找给定设备的 DSI 总线
 * @dev: 显示设备（SPI、I2C 等）的父设备
 *
 * 为通过非 MIPI-DCS 总线（如 SPI、I2C 等）控制的 DSI 设备
 * 查找其父 DSI 总线。此函数假设设备的 port@0 是 DSI 输入。
 *
 * 通过遍历设备树图，从设备端点跟踪到远程端口，找到 DSI 主机节点
 * 并返回对应的 mipi_dsi_host 实例。
 *
 * 返回：成功时返回 mipi_dsi_host 指针，-EPROBE_DEFER 表示 DSI 主机
 * 已找到但尚未可用，-ENODEV 表示未找到，-EINVAL 表示请求不支持。
 */
struct mipi_dsi_host *drm_of_get_dsi_bus(struct device *dev)
{
	struct mipi_dsi_host *dsi_host;
	struct device_node *endpoint, *dsi_host_node;

	/*
	 * Get first endpoint child from device.
	 */
	endpoint = of_graph_get_endpoint_by_regs(dev->of_node, 0, -1);
	if (!endpoint)
		return ERR_PTR(-ENODEV);

	/*
	 * Follow the first endpoint to get the DSI host node and then
	 * release the endpoint since we no longer need it.
	 */
	dsi_host_node = of_graph_get_remote_port_parent(endpoint);
	of_node_put(endpoint);
	if (!dsi_host_node)
		return ERR_PTR(-ENODEV);

	/*
	 * Get the DSI host from the DSI host node. If we get an error
	 * or the return is null assume we're not ready to probe just
	 * yet. Release the DSI host node since we're done with it.
	 */
	dsi_host = of_find_mipi_dsi_host_by_node(dsi_host_node);
	of_node_put(dsi_host_node);
	if (IS_ERR_OR_NULL(dsi_host))
		return ERR_PTR(-EPROBE_DEFER);

	return dsi_host;
}
EXPORT_SYMBOL_GPL(drm_of_get_dsi_bus);

#endif /* CONFIG_DRM_MIPI_DSI */
