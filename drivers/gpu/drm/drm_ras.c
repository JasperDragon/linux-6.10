// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/*
 * 文件名: drm_ras.c
 *
 * 中文描述: DRM RAS（可靠性、可用性、可服务性）节点管理
 *
 * RAS（Reliability, Availability, Serviceability）是计算系统用于增强
 * 可靠性、提高可用性和简化维护的一组特性集合。在 DRM 子系统中，RAS 框架
 * 用于管理和报告显卡硬件组件的错误计数和可靠性指标。
 *
 * 本文件实现了 RAS 节点的注册和管理基础设施：
 *   1. RAS 节点注册/注销 - 驱动程序可动态注册一个或多个 RAS 节点
 *   2. 节点通过全局 xarray (drm_ras_xa) 进行管理，支持按 ID 高效查找
 *   3. 支持 Generic Netlink 接口与用户空间通信
 *
 * Netlink 操作：
 *   1. LIST_NODES - 列出所有已注册的 RAS 节点
 *   2. GET_ERROR_COUNTER - 获取指定节点的错误计数器值
 *
 * 目前仅支持 ERROR_COUNTER 类型的节点，驱动程序需实现
 * query_error_counter() 回调函数来提供错误计数器和名称。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/xarray.h>
#include <net/genetlink.h>

#include <drm/drm_ras.h>

#include "drm_ras_nl.h"

/**
 * DOC: DRM RAS Node Management
 *
 * This module provides the infrastructure to manage RAS (Reliability,
 * Availability, and Serviceability) nodes for DRM drivers. Each
 * DRM driver may register one or more RAS nodes, which represent
 * logical components capable of reporting error counters and other
 * reliability metrics.
 *
 * The nodes are stored in a global xarray `drm_ras_xa` to allow
 * efficient lookup by ID. Nodes can be registered or unregistered
 * dynamically at runtime.
 *
 * A Generic Netlink family `drm_ras` exposes two main operations to
 * userspace:
 *
 * 1. LIST_NODES: Dump all currently registered RAS nodes.
 *    The user receives an array of node IDs, names, and types.
 *
 * 2. GET_ERROR_COUNTER: Get error counters of a given node.
 *    Userspace must provide Node ID, Error ID (Optional for specific counter).
 *    Returns all counters of a node if only Node ID is provided or specific
 *    error counters.
 *
 * Node registration:
 *
 * - drm_ras_node_register(): Registers a new node and assigns
 *   it a unique ID in the xarray.
 * - drm_ras_node_unregister(): Removes a previously registered
 *   node from the xarray.
 *
 * Node type:
 *
 * - ERROR_COUNTER:
 *     + Currently, only error counters are supported.
 *     + The driver must implement the query_error_counter() callback to provide
 *       the name and the value of the error counter.
 *     + The driver must provide a error_counter_range.last value informing the
 *       last valid error ID.
 *     + The driver can provide a error_counter_range.first value informing the
 *       first valid error ID.
 *     + The error counters in the driver doesn't need to be contiguous, but the
 *       driver must return -ENOENT to the query_error_counter as an indication
 *       that the ID should be skipped and not listed in the netlink API.
 *
 * Netlink handlers:
 *
 * - drm_ras_nl_list_nodes_dumpit(): Implements the LIST_NODES
 *   operation, iterating over the xarray.
 * - drm_ras_nl_get_error_counter_dumpit(): Implements the GET_ERROR_COUNTER dumpit
 *   operation, fetching all counters from a specific node.
 * - drm_ras_nl_get_error_counter_doit(): Implements the GET_ERROR_COUNTER doit
 *   operation, fetching a counter value from a specific node.
 */

static DEFINE_XARRAY_ALLOC(drm_ras_xa);

/*
 * The netlink callback context carries dump state across multiple dumpit calls
 */
struct drm_ras_ctx {
	/* Which xarray id to restart the dump from */
	unsigned long restart;
};

/**
 * drm_ras_nl_list_nodes_dumpit() - 转储所有已注册的 RAS 节点
 * @skb: Netlink 消息缓冲区
 * @cb: 多部分转储的回调上下文
 *
 * 遍历全局 xarray 中所有已注册的 RAS 节点，将其属性（ID、设备名称、
 * 节点名称、类型）附加到给定的 netlink 消息缓冲区。
 *
 * 使用 @cb->ctx 跟踪进度以支持多部分（multi-part）转储：当消息缓冲区
 * 填满时，记录当前位置，下次调用时从中断处继续。
 *
 * 返回：0 所有节点已写入，-EMSGSIZE 缓冲区不足（需要继续），其他负错误码
 */
int drm_ras_nl_list_nodes_dumpit(struct sk_buff *skb,
				 struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	struct drm_ras_ctx *ctx = (void *)cb->ctx;
	struct drm_ras_node *node;
	struct nlattr *hdr;
	unsigned long id;
	int ret;

	xa_for_each_start(&drm_ras_xa, id, node, ctx->restart) {
		hdr = genlmsg_iput(skb, info);
		if (!hdr) {
			ret = -EMSGSIZE;
			break;
		}

		ret = nla_put_u32(skb, DRM_RAS_A_NODE_ATTRS_NODE_ID, node->id);
		if (ret) {
			genlmsg_cancel(skb, hdr);
			break;
		}

		ret = nla_put_string(skb, DRM_RAS_A_NODE_ATTRS_DEVICE_NAME,
				     node->device_name);
		if (ret) {
			genlmsg_cancel(skb, hdr);
			break;
		}

		ret = nla_put_string(skb, DRM_RAS_A_NODE_ATTRS_NODE_NAME,
				     node->node_name);
		if (ret) {
			genlmsg_cancel(skb, hdr);
			break;
		}

		ret = nla_put_u32(skb, DRM_RAS_A_NODE_ATTRS_NODE_TYPE,
				  node->type);
		if (ret) {
			genlmsg_cancel(skb, hdr);
			break;
		}

		genlmsg_end(skb, hdr);
	}

	if (ret == -EMSGSIZE)
		ctx->restart = id;

	return ret;
}

static int get_node_error_counter(u32 node_id, u32 error_id,
				  const char **name, u32 *value)
{
	struct drm_ras_node *node;

	node = xa_load(&drm_ras_xa, node_id);
	if (!node || !node->query_error_counter)
		return -ENOENT;

	if (error_id < node->error_counter_range.first ||
	    error_id > node->error_counter_range.last)
		return -EINVAL;

	return node->query_error_counter(node, error_id, name, value);
}

static int msg_reply_value(struct sk_buff *msg, u32 error_id,
			   const char *error_name, u32 value)
{
	int ret;

	ret = nla_put_u32(msg, DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID, error_id);
	if (ret)
		return ret;

	ret = nla_put_string(msg, DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_NAME,
			     error_name);
	if (ret)
		return ret;

	return nla_put_u32(msg, DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_VALUE,
			   value);
}

static int doit_reply_value(struct genl_info *info, u32 node_id,
			    u32 error_id)
{
	struct sk_buff *msg;
	struct nlattr *hdr;
	const char *error_name;
	u32 value;
	int ret;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_iput(msg, info);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	ret = get_node_error_counter(node_id, error_id,
				     &error_name, &value);
	if (ret)
		return ret;

	ret = msg_reply_value(msg, error_id, error_name, value);
	if (ret) {
		genlmsg_cancel(msg, hdr);
		nlmsg_free(msg);
		return ret;
	}

	genlmsg_end(msg, hdr);

	return genlmsg_reply(msg, info);
}

/**
 * drm_ras_nl_get_error_counter_dumpit() - 转储指定节点的所有错误计数器
 * @skb: Netlink 消息缓冲区
 * @cb: 多部分转储的回调上下文
 *
 * 遍历指定 RAS 节点中的所有错误计数器，将其属性（ID、名称、值）
 * 附加到 netlink 消息缓冲区。支持非连续的错误 ID 范围：
 * 如果驱动程序的 query_error_counter 回调返回 -ENOENT，
 * 则跳过该 ID 继续遍历。
 *
 * 支持多部分转储：缓冲区满时记录当前位置以便后续继续。
 *
 * 返回：0 所有计数器已写入，-EMSGSIZE 缓冲区不足，负错误码失败
 */
int drm_ras_nl_get_error_counter_dumpit(struct sk_buff *skb,
					struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	struct drm_ras_ctx *ctx = (void *)cb->ctx;
	struct drm_ras_node *node;
	struct nlattr *hdr;
	const char *error_name;
	u32 node_id, error_id, value;
	int ret;

	if (!info->attrs || GENL_REQ_ATTR_CHECK(info, DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID))
		return -EINVAL;

	node_id = nla_get_u32(info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID]);

	node = xa_load(&drm_ras_xa, node_id);
	if (!node)
		return -ENOENT;

	for (error_id = max(node->error_counter_range.first, ctx->restart);
	     error_id <= node->error_counter_range.last;
	     error_id++) {
		ret = get_node_error_counter(node_id, error_id,
					     &error_name, &value);
		/*
		 * For non-contiguous range, driver return -ENOENT as indication
		 * to skip this ID when listing all errors.
		 */
		if (ret == -ENOENT)
			continue;
		if (ret)
			return ret;

		hdr = genlmsg_iput(skb, info);

		if (!hdr) {
			ret = -EMSGSIZE;
			break;
		}

		ret = msg_reply_value(skb, error_id, error_name, value);
		if (ret) {
			genlmsg_cancel(skb, hdr);
			break;
		}

		genlmsg_end(skb, hdr);
	}

	if (ret == -EMSGSIZE)
		ctx->restart = error_id;

	return ret;
}

/**
 * drm_ras_nl_get_error_counter_doit() - 查询指定节点的某个错误计数器
 * @skb: Netlink 消息缓冲区
 * @info: 包含请求属性的 Generic Netlink 信息
 *
 * 从 netlink 属性中提取节点 ID 和错误 ID，查询对应的错误计数器当前值，
 * 并通过标准 Generic Netlink 回复机制将结果返回给请求的用户空间程序。
 *
 * 用户空间必须同时提供节点 ID 和错误 ID。
 *
 * 返回：0 成功，负错误码失败
 */
int drm_ras_nl_get_error_counter_doit(struct sk_buff *skb,
				      struct genl_info *info)
{
	u32 node_id, error_id;

	if (!info->attrs ||
	    GENL_REQ_ATTR_CHECK(info, DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID) ||
	    GENL_REQ_ATTR_CHECK(info, DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID))
		return -EINVAL;

	node_id = nla_get_u32(info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_NODE_ID]);
	error_id = nla_get_u32(info->attrs[DRM_RAS_A_ERROR_COUNTER_ATTRS_ERROR_ID]);

	return doit_reply_value(info, node_id, error_id);
}

/**
 * drm_ras_node_register() - 注册一个新的 RAS 节点
 * @node: 要注册的节点结构体
 *
 * 将给定的 RAS 节点添加到全局节点 xarray 中，并为其分配唯一 ID。
 * 节点必须提供 device_name 和 node_name。目前仅支持
 * DRM_RAS_NODE_TYPE_ERROR_COUNTER 类型的节点。
 *
 * 对于错误计数器类型的节点，还必须提供：
 *   - error_counter_range.last: 最后一个有效的错误 ID
 *   - query_error_counter 回调: 用于查询错误计数器的名称和值
 *
 * 返回：0 成功，-EINVAL 参数无效，其他负错误码失败
 */
int drm_ras_node_register(struct drm_ras_node *node)
{
	if (!node->device_name || !node->node_name)
		return -EINVAL;

	/* Currently, only Error Counter Endpoints are supported */
	if (node->type != DRM_RAS_NODE_TYPE_ERROR_COUNTER)
		return -EINVAL;

	/* Mandatory entries for Error Counter Node */
	if (node->type == DRM_RAS_NODE_TYPE_ERROR_COUNTER &&
	    (!node->error_counter_range.last || !node->query_error_counter))
		return -EINVAL;

	return xa_alloc(&drm_ras_xa, &node->id, node, xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL(drm_ras_node_register);

/**
 * drm_ras_node_unregister() - 注销一个已注册的 RAS 节点
 * @node: 要注销的节点结构体
 *
 * 使用节点的 ID 从全局节点 xarray 中移除该 RAS 节点。
 * 执行后节点不再对用户空间可见。
 */
void drm_ras_node_unregister(struct drm_ras_node *node)
{
	xa_erase(&drm_ras_xa, node->id);
}
EXPORT_SYMBOL(drm_ras_node_unregister);
