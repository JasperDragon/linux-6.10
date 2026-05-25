/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * V4L2 asynchronous subdevice registration API
 *
 * Copyright (C) 2012-2013, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 */

#ifndef V4L2_ASYNC_H
#define V4L2_ASYNC_H

/*
 * 中文概述：
 *
 * 异步子设备注册框架。V4L2 子设备（如传感器、解码器）通常独立于
 * bridge 主设备探测，本框架提供了一种异步匹配和绑定机制。
 *
 * 核心工作流程：
 *  1. bridge 驱动创建一个 v4l2_async_notifier，并通过
 *     v4l2_async_nf_add_fwnode() / v4l2_async_nf_add_i2c() 添加
 *     期望匹配的子设备描述（fwnode 或 I2C 地址）。
 *  2. 调用 v4l2_async_nf_register() 注册 notifier。
 *  3. 子设备驱动在探测时调用 v4l2_async_register_subdev()。
 *  4. 框架自动匹配：比较子设备的 fwnode/I2C 地址与 notifier 中
 *     的等待列表（waiting_list），匹配成功则调用 bound 回调，
 *     并将 asc 移至 done_list。
 *  5. 当 notifier 中所有 asc 都完成绑定后，调用 complete 回调。
 *  6. 子设备移除时触发 unbind 回调。
 *
 * 匹配类型：
 *  - V4L2_ASYNC_MATCH_TYPE_FWNODE：基于 firmware node（设备树或
 *    ACPI）句柄匹配。
 *  - V4L2_ASYNC_MATCH_TYPE_I2C：基于 I2C adapter ID 和地址匹配。
 *
 * 关键数据结构：
 *  - v4l2_async_connection (asc)：描述一个待匹配的子设备连接，
 *    包含匹配信息、所属 notifier、匹配后的子设备指针。
 *  - v4l2_async_notifier：notifier 包含 waiting_list（待匹配）、
 *    done_list（已完成）、操作回调（bound/complete/unbind）。
 *  - v4l2_async_subdev_endpoint：用于从单个设备注册多个端点。
 *
 * 连接生命周期：
 *  create -> add to notifier -> register -> match -> bound callback
 *  -> complete (all done) -> unbind (on removal) -> cleanup
 */

#include <linux/list.h>
#include <linux/mutex.h>

struct dentry;
struct device;
struct device_node;
struct v4l2_device;
struct v4l2_subdev;
struct v4l2_async_notifier;

/**
 * enum v4l2_async_match_type - type of asynchronous subdevice logic to be used
 *	in order to identify a match
 *
 * @V4L2_ASYNC_MATCH_TYPE_I2C: Match will check for I2C adapter ID and address
 * @V4L2_ASYNC_MATCH_TYPE_FWNODE: Match will use firmware node
 *
 * This enum is used by the asynchronous connection logic to define the
 * algorithm that will be used to match an asynchronous device.
 */
enum v4l2_async_match_type {
	V4L2_ASYNC_MATCH_TYPE_I2C,
	V4L2_ASYNC_MATCH_TYPE_FWNODE,
};

/**
 * struct v4l2_async_match_desc - async connection match information
 *
 * @type:	type of match that will be used
 * @fwnode:	pointer to &struct fwnode_handle to be matched.
 *		Used if @match_type is %V4L2_ASYNC_MATCH_TYPE_FWNODE.
 * @i2c:	embedded struct with I2C parameters to be matched.
 *		Both @match.i2c.adapter_id and @match.i2c.address
 *		should be matched.
 *		Used if @match_type is %V4L2_ASYNC_MATCH_TYPE_I2C.
 * @i2c.adapter_id:
 *		I2C adapter ID to be matched.
 *		Used if @match_type is %V4L2_ASYNC_MATCH_TYPE_I2C.
 * @i2c.address:
 *		I2C address to be matched.
 *		Used if @match_type is %V4L2_ASYNC_MATCH_TYPE_I2C.
 */
struct v4l2_async_match_desc {
	enum v4l2_async_match_type type;
	union {
		struct fwnode_handle *fwnode;
		struct {
			int adapter_id;
			unsigned short address;
		} i2c;
	};
};

/**
 * struct v4l2_async_connection - sub-device connection descriptor, as known to
 *				  a bridge
 *
 * @match:	struct of match type and per-bus type matching data sets
 * @notifier:	the async notifier the connection is related to
 * @asc_entry:	used to add struct v4l2_async_connection objects to the
 *		notifier @waiting_list or @done_list
 * @asc_subdev_entry:	entry in struct v4l2_async_subdev.asc_list list
 * @sd:		the related sub-device
 *
 * When this struct is used as a member in a driver specific struct, the driver
 * specific struct shall contain the &struct v4l2_async_connection as its first
 * member.
 */
struct v4l2_async_connection {
	struct v4l2_async_match_desc match;
	struct v4l2_async_notifier *notifier;
	struct list_head asc_entry;
	struct list_head asc_subdev_entry;
	struct v4l2_subdev *sd;
};

/**
 * struct v4l2_async_notifier_operations - Asynchronous V4L2 notifier operations
 * @bound:	a sub-device has been bound by the given connection
 * @complete:	All connections have been bound successfully. The complete
 *		callback is only executed for the root notifier.
 * @unbind:	a subdevice is leaving
 * @destroy:	the asc is about to be freed
 */
struct v4l2_async_notifier_operations {
	int (*bound)(struct v4l2_async_notifier *notifier,
		     struct v4l2_subdev *subdev,
		     struct v4l2_async_connection *asc);
	int (*complete)(struct v4l2_async_notifier *notifier);
	void (*unbind)(struct v4l2_async_notifier *notifier,
		       struct v4l2_subdev *subdev,
		       struct v4l2_async_connection *asc);
	void (*destroy)(struct v4l2_async_connection *asc);
};

/**
 * struct v4l2_async_notifier - v4l2_device notifier data
 *
 * @ops:	notifier operations
 * @v4l2_dev:	v4l2_device of the root notifier, NULL otherwise
 * @sd:		sub-device that registered the notifier, NULL otherwise
 * @parent:	parent notifier
 * @waiting_list: list of struct v4l2_async_connection, waiting for their
 *		  drivers
 * @done_list:	list of struct v4l2_subdev, already probed
 * @notifier_entry: member in a global list of notifiers
 */
struct v4l2_async_notifier {
	const struct v4l2_async_notifier_operations *ops;
	struct v4l2_device *v4l2_dev;
	struct v4l2_subdev *sd;
	struct v4l2_async_notifier *parent;
	struct list_head waiting_list;
	struct list_head done_list;
	struct list_head notifier_entry;
};

/**
 * struct v4l2_async_subdev_endpoint - Entry in sub-device's fwnode list
 *
 * @async_subdev_endpoint_entry: An entry in async_subdev_endpoint_list of
 *				 &struct v4l2_subdev
 * @endpoint: Endpoint fwnode agains which to match the sub-device
 */
struct v4l2_async_subdev_endpoint {
	struct list_head async_subdev_endpoint_entry;
	struct fwnode_handle *endpoint;
};

/**
 * v4l2_async_debug_init - Initialize debugging tools.
 *
 * @debugfs_dir: pointer to the parent debugfs &struct dentry
 */
void v4l2_async_debug_init(struct dentry *debugfs_dir);

/**
 * v4l2_async_nf_init - Initialize a notifier.
 *
 * @notifier: pointer to &struct v4l2_async_notifier
 * @v4l2_dev: pointer to &struct v4l2_device
 *
 * This function initializes the notifier @asc_entry. It must be called
 * before adding a subdevice to a notifier, using one of:
 * v4l2_async_nf_add_fwnode_remote(),
 * v4l2_async_nf_add_fwnode() or
 * v4l2_async_nf_add_i2c().
 */
void v4l2_async_nf_init(struct v4l2_async_notifier *notifier,
			struct v4l2_device *v4l2_dev);

/**
 * v4l2_async_subdev_nf_init - Initialize a sub-device notifier.
 *
 * @notifier: pointer to &struct v4l2_async_notifier
 * @sd: pointer to &struct v4l2_subdev
 *
 * This function initializes the notifier @asc_list. It must be called
 * before adding a subdevice to a notifier, using one of:
 * v4l2_async_nf_add_fwnode_remote(), v4l2_async_nf_add_fwnode() or
 * v4l2_async_nf_add_i2c().
 */
void v4l2_async_subdev_nf_init(struct v4l2_async_notifier *notifier,
			       struct v4l2_subdev *sd);

struct v4l2_async_connection *
__v4l2_async_nf_add_fwnode(struct v4l2_async_notifier *notifier,
			   struct fwnode_handle *fwnode,
			   unsigned int asc_struct_size);
/**
 * v4l2_async_nf_add_fwnode - Allocate and add a fwnode async
 *				subdev to the notifier's master asc_list.
 *
 * @notifier: pointer to &struct v4l2_async_notifier
 * @fwnode: fwnode handle of the sub-device to be matched, pointer to
 *	    &struct fwnode_handle
 * @type: Type of the driver's async sub-device or connection struct. The
 *	  &struct v4l2_async_connection shall be the first member of the
 *	  driver's async struct, i.e. both begin at the same memory address.
 *
 * Allocate a fwnode-matched asc of size asc_struct_size, and add it to the
 * notifiers @asc_list. The function also gets a reference of the fwnode which
 * is released later at notifier cleanup time.
 */
#define v4l2_async_nf_add_fwnode(notifier, fwnode, type)		\
	((type *)__v4l2_async_nf_add_fwnode(notifier, fwnode, sizeof(type)))

struct v4l2_async_connection *
__v4l2_async_nf_add_fwnode_remote(struct v4l2_async_notifier *notif,
				  struct fwnode_handle *endpoint,
				  unsigned int asc_struct_size);
/**
 * v4l2_async_nf_add_fwnode_remote - Allocate and add a fwnode
 *						  remote async subdev to the
 *						  notifier's master asc_list.
 *
 * @notifier: pointer to &struct v4l2_async_notifier
 * @ep: local endpoint pointing to the remote connection to be matched,
 *	pointer to &struct fwnode_handle
 * @type: Type of the driver's async connection struct. The &struct
 *	  v4l2_async_connection shall be the first member of the driver's async
 *	  connection struct, i.e. both begin at the same memory address.
 *
 * Gets the remote endpoint of a given local endpoint, set it up for fwnode
 * matching and adds the async connection to the notifier's @asc_list. The
 * function also gets a reference of the fwnode which is released later at
 * notifier cleanup time.
 *
 * This is just like v4l2_async_nf_add_fwnode(), but with the
 * exception that the fwnode refers to a local endpoint, not the remote one.
 */
#define v4l2_async_nf_add_fwnode_remote(notifier, ep, type) \
	((type *)__v4l2_async_nf_add_fwnode_remote(notifier, ep, sizeof(type)))

struct v4l2_async_connection *
__v4l2_async_nf_add_i2c(struct v4l2_async_notifier *notifier,
			int adapter_id, unsigned short address,
			unsigned int asc_struct_size);
/**
 * v4l2_async_nf_add_i2c - Allocate and add an i2c async
 *				subdev to the notifier's master asc_list.
 *
 * @notifier: pointer to &struct v4l2_async_notifier
 * @adapter: I2C adapter ID to be matched
 * @address: I2C address of connection to be matched
 * @type: Type of the driver's async connection struct. The &struct
 *	  v4l2_async_connection shall be the first member of the driver's async
 *	  connection struct, i.e. both begin at the same memory address.
 *
 * Same as v4l2_async_nf_add_fwnode() but for I2C matched
 * connections.
 */
#define v4l2_async_nf_add_i2c(notifier, adapter, address, type) \
	((type *)__v4l2_async_nf_add_i2c(notifier, adapter, address, \
					 sizeof(type)))

/**
 * v4l2_async_subdev_endpoint_add - Add an endpoint fwnode to async sub-device
 *				    matching list
 *
 * @sd: the sub-device
 * @fwnode: the endpoint fwnode to match
 *
 * Add a fwnode to the async sub-device's matching list. This allows registering
 * multiple async sub-devices from a single device.
 *
 * Note that calling v4l2_subdev_cleanup() as part of the sub-device's cleanup
 * if endpoints have been added to the sub-device's fwnode matching list.
 *
 * Returns an error on failure, 0 on success.
 */
int v4l2_async_subdev_endpoint_add(struct v4l2_subdev *sd,
				   struct fwnode_handle *fwnode);

/**
 * v4l2_async_connection_unique - return a unique &struct v4l2_async_connection
 *				  for a sub-device
 * @sd: the sub-device
 *
 * Return an async connection for a sub-device, when there is a single
 * one only.
 */
struct v4l2_async_connection *
v4l2_async_connection_unique(struct v4l2_subdev *sd);

/**
 * v4l2_async_nf_register - registers a subdevice asynchronous notifier
 *
 * @notifier: pointer to &struct v4l2_async_notifier
 */
int v4l2_async_nf_register(struct v4l2_async_notifier *notifier);

/**
 * v4l2_async_nf_unregister - unregisters a subdevice
 *	asynchronous notifier
 *
 * @notifier: pointer to &struct v4l2_async_notifier
 */
void v4l2_async_nf_unregister(struct v4l2_async_notifier *notifier);

/**
 * v4l2_async_nf_cleanup - clean up notifier resources
 * @notifier: the notifier the resources of which are to be cleaned up
 *
 * Release memory resources related to a notifier, including the async
 * connections allocated for the purposes of the notifier but not the notifier
 * itself. The user is responsible for calling this function to clean up the
 * notifier after calling v4l2_async_nf_add_fwnode_remote(),
 * v4l2_async_nf_add_fwnode() or v4l2_async_nf_add_i2c().
 *
 * There is no harm from calling v4l2_async_nf_cleanup() in other
 * cases as long as its memory has been zeroed after it has been
 * allocated.
 */
void v4l2_async_nf_cleanup(struct v4l2_async_notifier *notifier);

/**
 * v4l2_async_register_subdev - registers a sub-device to the asynchronous
 *	subdevice framework
 *
 * @sd: pointer to &struct v4l2_subdev
 */
#define v4l2_async_register_subdev(sd) \
	__v4l2_async_register_subdev(sd, THIS_MODULE)
int __v4l2_async_register_subdev(struct v4l2_subdev *sd, struct module *module);

/**
 * v4l2_async_register_subdev_sensor - registers a sensor sub-device to the
 *				       asynchronous sub-device framework and
 *				       parse set up common sensor related
 *				       devices
 *
 * @sd: pointer to struct &v4l2_subdev
 *
 * This function is just like v4l2_async_register_subdev() with the exception
 * that calling it will also parse firmware interfaces for remote references
 * using v4l2_async_nf_parse_fwnode_sensor() and registers the
 * async sub-devices. The sub-device is similarly unregistered by calling
 * v4l2_async_unregister_subdev().
 *
 * While registered, the subdev module is marked as in-use.
 *
 * An error is returned if the module is no longer loaded on any attempts
 * to register it.
 */
int __must_check
v4l2_async_register_subdev_sensor(struct v4l2_subdev *sd);

/**
 * v4l2_async_unregister_subdev - unregisters a sub-device to the asynchronous
 *	subdevice framework
 *
 * @sd: pointer to &struct v4l2_subdev
 */
void v4l2_async_unregister_subdev(struct v4l2_subdev *sd);
#endif
