/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * v4l2-fh.h
 *
 * V4L2 file handle. Store per file handle data for the V4L2
 * framework. Using file handles is mandatory for the drivers.
 *
 * Copyright (C) 2009--2010 Nokia Corporation.
 *
 * Contact: Sakari Ailus <sakari.ailus@iki.fi>
 */

#ifndef V4L2_FH_H
#define V4L2_FH_H

/*
 * 中文概述：
 *
 * v4l2_fh（V4L2 File Handle）是 V4L2 框架中每个打开的文件描述符
 * 对应的私有数据结构，存储 per-filehandle 的状态信息。
 *
 * 关联链：
 *  struct file (private_data)
 *      -> struct v4l2_fh (vdev, ctrl_handler, prio, events)
 *          -> struct video_device (v4l2_dev, 设备节点)
 *
 * 主要功能：
 *  - 优先级管理：prio 字段记录当前文件句柄的访问优先级
 *    (V4L2_PRIORITY_*)，用于控制多个应用程序同时访问设备时的
 *    资源竞争。
 *  - 事件订阅：通过 subscribed 链表跟踪已订阅的事件类型，
 *    available 链表存储待读取的事件，navailable 计数可用事件数。
 *  - 控制句柄：ctrl_handler 指针关联到 v4l2_ctrl_handler，实现
 *    每个文件句柄独立的控制值设置。
 *  - M2M 上下文：m2m_ctx 用于 memory-to-memory 设备，保存编解码
 *    会话状态。
 *
 * 生命周期：
 *  - v4l2_fh_init() 初始化 fh 并与 video_device 关联
 *  - v4l2_fh_add() 将 fh 添加到 video_device 的文件句柄列表，
 *    并将 fh 指针存入 filp->private_data
 *  - v4l2_fh_del() 从列表中移除 fh
 *  - v4l2_fh_exit() 释放事件和控制资源
 *  - v4l2_fh_open() / v4l2_fh_release() 是上述步骤的快捷封装
 */

#include <linux/fs.h>
#include <linux/kconfig.h>
#include <linux/list.h>
#include <linux/videodev2.h>

struct video_device;
struct v4l2_ctrl_handler;

/**
 * struct v4l2_fh - Describes a V4L2 file handler
 *
 * @list: list of file handlers
 * @vdev: pointer to &struct video_device
 * @ctrl_handler: pointer to &struct v4l2_ctrl_handler
 * @prio: priority of the file handler, as defined by &enum v4l2_priority
 *
 * @wait: event' s wait queue
 * @subscribe_lock: serialise changes to the subscribed list; guarantee that
 *		    the add and del event callbacks are orderly called
 * @subscribed: list of subscribed events
 * @available: list of events waiting to be dequeued
 * @navailable: number of available events at @available list
 * @sequence: event sequence number
 *
 * @m2m_ctx: pointer to &struct v4l2_m2m_ctx
 */
struct v4l2_fh {
	struct list_head	list;
	struct video_device	*vdev;
	struct v4l2_ctrl_handler *ctrl_handler;
	enum v4l2_priority	prio;

	/* Events */
	wait_queue_head_t	wait;
	struct mutex		subscribe_lock;
	struct list_head	subscribed;
	struct list_head	available;
	unsigned int		navailable;
	u32			sequence;

	struct v4l2_m2m_ctx	*m2m_ctx;
};

/**
 * file_to_v4l2_fh - Return the v4l2_fh associated with a struct file
 *
 * @filp: pointer to &struct file
 *
 * This function should be used by drivers to retrieve the &struct v4l2_fh
 * instance pointer stored in the file private_data instead of accessing the
 * private_data field directly.
 */
static inline struct v4l2_fh *file_to_v4l2_fh(struct file *filp)
{
	return filp->private_data;
}

/**
 * v4l2_fh_init - Initialise the file handle.
 *
 * @fh: pointer to &struct v4l2_fh
 * @vdev: pointer to &struct video_device
 *
 * Parts of the V4L2 framework using the
 * file handles should be initialised in this function. Must be called
 * from driver's v4l2_file_operations->open\(\) handler if the driver
 * uses &struct v4l2_fh.
 */
void v4l2_fh_init(struct v4l2_fh *fh, struct video_device *vdev);

/**
 * v4l2_fh_add - Add the fh to the list of file handles on a video_device.
 *
 * @fh: pointer to &struct v4l2_fh
 * @filp: pointer to &struct file associated with @fh
 *
 * The function sets filp->private_data to point to @fh.
 *
 * .. note::
 *    The @fh file handle must be initialised first.
 */
void v4l2_fh_add(struct v4l2_fh *fh, struct file *filp);

/**
 * v4l2_fh_open - Ancillary routine that can be used as the open\(\) op
 *	of v4l2_file_operations.
 *
 * @filp: pointer to struct file
 *
 * It allocates a v4l2_fh and inits and adds it to the &struct video_device
 * associated with the file pointer.
 *
 * On error filp->private_data will be %NULL, otherwise it will point to
 * the &struct v4l2_fh.
 */
int v4l2_fh_open(struct file *filp);

/**
 * v4l2_fh_del - Remove file handle from the list of file handles.
 *
 * @fh: pointer to &struct v4l2_fh
 * @filp: pointer to &struct file associated with @fh
 *
 * The function resets filp->private_data to NULL.
 *
 * .. note::
 *    Must be called in v4l2_file_operations->release\(\) handler if the driver
 *    uses &struct v4l2_fh.
 */
void v4l2_fh_del(struct v4l2_fh *fh, struct file *filp);

/**
 * v4l2_fh_exit - Release resources related to a file handle.
 *
 * @fh: pointer to &struct v4l2_fh
 *
 * Parts of the V4L2 framework using the v4l2_fh must release their
 * resources here, too.
 *
 * .. note::
 *    Must be called in v4l2_file_operations->release\(\) handler if the
 *    driver uses &struct v4l2_fh.
 */
void v4l2_fh_exit(struct v4l2_fh *fh);

/**
 * v4l2_fh_release - Ancillary routine that can be used as the release\(\) op
 *	of v4l2_file_operations.
 *
 * @filp: pointer to struct file
 *
 * It deletes and exits the v4l2_fh associated with the file pointer and
 * frees it. It will do nothing if filp->private_data (the pointer to the
 * v4l2_fh struct) is %NULL.
 *
 * This function always returns 0.
 */
int v4l2_fh_release(struct file *filp);

/**
 * v4l2_fh_is_singular - Returns 1 if this filehandle is the only filehandle
 *	 opened for the associated video_device.
 *
 * @fh: pointer to &struct v4l2_fh
 *
 * If @fh is NULL, then it returns 0.
 */
int v4l2_fh_is_singular(struct v4l2_fh *fh);

/**
 * v4l2_fh_is_singular_file - Returns 1 if this filehandle is the only
 *	filehandle opened for the associated video_device.
 *
 * @filp: pointer to struct file
 *
 * This is a helper function variant of v4l2_fh_is_singular() with uses
 * struct file as argument.
 *
 * If filp->private_data is %NULL, then it will return 0.
 */
static inline int v4l2_fh_is_singular_file(struct file *filp)
{
	return v4l2_fh_is_singular(filp->private_data);
}

#endif /* V4L2_EVENT_H */
