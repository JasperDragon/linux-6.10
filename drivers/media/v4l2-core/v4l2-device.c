// SPDX-License-Identifier: GPL-2.0-or-later
/*
    V4L2 device support.

    Copyright (C) 2008  Hans Verkuil <hverkuil@kernel.org>

 */

/*
 * ============================================================================
 * v4l2_device — V4L2 顶层设备管理实现
 * ============================================================================
 *
 * 本文件实现了 v4l2_device 的完整生命周期管理，是整个 V4L2 框架的入口。
 *
 * 【架构位置】
 *   v4l2_device 位于 V4L2 框架的最顶层，代表一个完整的视频硬件设备
 *   （如一个 PCI 采集卡、USB 摄像头、或 SoC 视频处理单元）。
 *
 * 【核心职责】
 *   1. 设备注册/注销 — v4l2_device_register/unregister
 *   2. 引用计数管理 — kref 机制，支持热插拔时的安全释放
 *   3. 子设备管理   — 通过链表 subdevs 聚合所有 v4l2_subdev
 *   4. Media Controller 集成 — 将子设备注册到 media_device 图拓扑中
 *   5. 子设备节点创建 — 为带 V4L2_SUBDEV_FL_HAS_DEVNODE 标志的子设备
 *      自动创建 /dev/v4l-subdevX 设备节点
 *
 * 【子设备注册流程】 __v4l2_device_register_subdev()
 *   1. 模块引用计数保护（try_module_get）
 *   2. 控制处理器继承（v4l2_ctrl_add_handler）
 *   3. 注册到 Media Controller（media_device_register_entity）
 *   4. 调用子设备的 registered 回调
 *   5. 将子设备加入 v4l2_device->subdevs 链表
 *
 * 【引用计数与生命周期】
 *   v4l2_device 使用 kref 管理生命周期：
 *   - v4l2_device_get() 增加引用
 *   - v4l2_device_put() 减少引用，计数归零时调用 release() 回调
 *   - 当 USB 设备拔出时，v4l2_device_disconnect() 将 dev 指针置 NULL
 *     防止访问已释放的父设备
 *
 * 【与 Media Controller 的关系】
 *   v4l2_device->mdev 指向关联的 media_device。
 *   当注册子设备时，如果 mdev 非空，会自动将子设备的 media_entity
 *   注册到 media_device 的拓扑图中。
 */

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>

/*
 * 【设备注册】v4l2_device_register — 初始化并注册 V4L2 顶层设备
 *
 * 调用时机：bridge 驱动 probe 时最先调用此函数，创建 V4L2 框架的根实例。
 *
 * 初始化内容：
 * - 子设备链表 (subdevs)：用于管理所有挂载的 v4l2_subdev
 * - 自旋锁 (lock)：保护 subdevs 链表
 * - 优先级状态 (prio)：多用户访问控制
 * - 引用计数 (ref)：kref 计数，支持安全释放
 * - 设备指针 (dev)：指向父 struct device
 *   dev->driver_data 被设为指向此 v4l2_dev，形成双向关联
 * - 名称 (name)：默认使用 "驱动名 设备名" 格式
 *
 * dev 可以为 NULL（仅 ISA 设备等罕见场景），此时调用者必须预填 name 字段。
 */
int v4l2_device_register(struct device *dev, struct v4l2_device *v4l2_dev)
{
	if (v4l2_dev == NULL)
		return -EINVAL;

	INIT_LIST_HEAD(&v4l2_dev->subdevs);
	spin_lock_init(&v4l2_dev->lock);
	v4l2_prio_init(&v4l2_dev->prio);
	kref_init(&v4l2_dev->ref);
	get_device(dev);
	v4l2_dev->dev = dev;
	if (dev == NULL) {
		/* If dev == NULL, then name must be filled in by the caller */
		if (WARN_ON(!v4l2_dev->name[0]))
			return -EINVAL;
		return 0;
	}

	/* Set name to driver name + device name if it is empty. */
	if (!v4l2_dev->name[0])
		snprintf(v4l2_dev->name, sizeof(v4l2_dev->name), "%s %s",
			dev->driver->name, dev_name(dev));
	if (!dev_get_drvdata(dev))
		dev_set_drvdata(dev, v4l2_dev);
	return 0;
}
EXPORT_SYMBOL_GPL(v4l2_device_register);

static void v4l2_device_release(struct kref *ref)
{
	struct v4l2_device *v4l2_dev =
		container_of(ref, struct v4l2_device, ref);

	if (v4l2_dev->release)
		v4l2_dev->release(v4l2_dev);
}

int v4l2_device_put(struct v4l2_device *v4l2_dev)
{
	return kref_put(&v4l2_dev->ref, v4l2_device_release);
}
EXPORT_SYMBOL_GPL(v4l2_device_put);

int v4l2_device_set_name(struct v4l2_device *v4l2_dev, const char *basename,
						atomic_t *instance)
{
	int num = atomic_inc_return(instance) - 1;
	int len = strlen(basename);

	if (basename[len - 1] >= '0' && basename[len - 1] <= '9')
		snprintf(v4l2_dev->name, sizeof(v4l2_dev->name),
				"%s-%d", basename, num);
	else
		snprintf(v4l2_dev->name, sizeof(v4l2_dev->name),
				"%s%d", basename, num);
	return num;
}
EXPORT_SYMBOL_GPL(v4l2_device_set_name);

void v4l2_device_disconnect(struct v4l2_device *v4l2_dev)
{
	if (v4l2_dev->dev == NULL)
		return;

	if (dev_get_drvdata(v4l2_dev->dev) == v4l2_dev)
		dev_set_drvdata(v4l2_dev->dev, NULL);
	put_device(v4l2_dev->dev);
	v4l2_dev->dev = NULL;
}
EXPORT_SYMBOL_GPL(v4l2_device_disconnect);

void v4l2_device_unregister(struct v4l2_device *v4l2_dev)
{
	struct v4l2_subdev *sd, *next;

	/* Just return if v4l2_dev is NULL or if it was already
	 * unregistered before. */
	if (v4l2_dev == NULL || !v4l2_dev->name[0])
		return;
	v4l2_device_disconnect(v4l2_dev);

	/* Unregister subdevs */
	list_for_each_entry_safe(sd, next, &v4l2_dev->subdevs, list) {
		v4l2_device_unregister_subdev(sd);
		if (sd->flags & V4L2_SUBDEV_FL_IS_I2C)
			v4l2_i2c_subdev_unregister(sd);
		else if (sd->flags & V4L2_SUBDEV_FL_IS_SPI)
			v4l2_spi_subdev_unregister(sd);
	}
	/* Mark as unregistered, thus preventing duplicate unregistrations */
	v4l2_dev->name[0] = '\0';
}
EXPORT_SYMBOL_GPL(v4l2_device_unregister);

/*
 * 【子设备注册】__v4l2_device_register_subdev — 将子设备注册到 V4L2 设备
 *
 * 这是子设备生命周期的核心入口。每个 I2C 传感器、编码器、解码器
 * 都需要通过此函数挂载到 bridge 驱动的 v4l2_device 上。
 *
 * 注册步骤：
 *   1. 模块引用保护 — 防止子设备驱动模块被意外卸载
 *      - 如果子设备 owner 与 v4l2_device 的 owner 相同，跳过 try_module_get
 *      - 否则获取模块引用，保证模块在子设备使用期间不被卸载
 *   2. 控制继承 — v4l2_ctrl_add_handler 将子设备的控制并入父设备
 *      - 用户可通过父设备的设备节点访问子设备的控制项
 *   3. Media Controller 注册 — 将子设备的 media_entity 加入 media_device 拓扑图
 *   4. 回调通知 — 调用子设备的 registered() 内部回调
 *   5. 链表管理 — 加入 v4l2_device->subdevs 链表（自旋锁保护）
 *
 * 错误处理：任一步骤失败都会回滚（unregister entity, module_put）
 */
int __v4l2_device_register_subdev(struct v4l2_device *v4l2_dev,
				  struct v4l2_subdev *sd, struct module *module)
{
	int err;

	/* Check for valid input */
	if (!v4l2_dev || !sd || sd->v4l2_dev || !sd->name[0])
		return -EINVAL;

	/*
	 * The reason to acquire the module here is to avoid unloading
	 * a module of sub-device which is registered to a media
	 * device. To make it possible to unload modules for media
	 * devices that also register sub-devices, do not
	 * try_module_get() such sub-device owners.
	 */
	sd->owner_v4l2_dev = v4l2_dev->dev && v4l2_dev->dev->driver &&
		module == v4l2_dev->dev->driver->owner;

	if (!sd->owner_v4l2_dev && !try_module_get(module))
		return -ENODEV;

	sd->v4l2_dev = v4l2_dev;
	/* This just returns 0 if either of the two args is NULL */
	err = v4l2_ctrl_add_handler(v4l2_dev->ctrl_handler, sd->ctrl_handler,
				    NULL, true);
	if (err)
		goto error_module;

#if defined(CONFIG_MEDIA_CONTROLLER)
	/* Register the entity. */
	if (v4l2_dev->mdev) {
		err = media_device_register_entity(v4l2_dev->mdev, &sd->entity);
		if (err < 0)
			goto error_module;
	}
#endif

	if (sd->internal_ops && sd->internal_ops->registered) {
		err = sd->internal_ops->registered(sd);
		if (err)
			goto error_unregister;
	}

	sd->owner = module;

	spin_lock(&v4l2_dev->lock);
	list_add_tail(&sd->list, &v4l2_dev->subdevs);
	spin_unlock(&v4l2_dev->lock);

	return 0;

error_unregister:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_device_unregister_entity(&sd->entity);
#endif
error_module:
	if (!sd->owner_v4l2_dev)
		module_put(sd->owner);
	sd->v4l2_dev = NULL;
	return err;
}
EXPORT_SYMBOL_GPL(__v4l2_device_register_subdev);

static void v4l2_subdev_release(struct v4l2_subdev *sd)
{
	struct module *owner = !sd->owner_v4l2_dev ? sd->owner : NULL;

	if (sd->internal_ops && sd->internal_ops->release)
		sd->internal_ops->release(sd);
	sd->devnode = NULL;
	module_put(owner);
}

static void v4l2_device_release_subdev_node(struct video_device *vdev)
{
	v4l2_subdev_release(video_get_drvdata(vdev));
	kfree(vdev);
}

/*
 * 【子设备节点创建】__v4l2_device_register_subdev_nodes
 *
 * 为所有带有 V4L2_SUBDEV_FL_HAS_DEVNODE 标志的子设备创建 /dev/v4l-subdevX 节点。
 * 这允许用户空间直接通过 ioctl 与子设备（如 sensor）交互，实现格式协商等功能。
 *
 * 每个子设备节点：
 * - 类型为 VFL_TYPE_SUBDEV
 * - 使用 v4l2_subdev_fops（子设备专用的文件操作集）
 * - 通过 video_set_drvdata(vdev, sd) 将子设备指针绑定到 video_device
 * - read_only 模式下，设置 V4L2_FL_SUBDEV_RO_DEVNODE 标志限制写操作
 * - 在 Media Controller 拓扑中创建 entity 到 interface 的 IMMUTABLE 链接
 *
 * 错误回滚：如果某个节点创建失败，clean_up 路径会注销所有已创建节点
 */
int __v4l2_device_register_subdev_nodes(struct v4l2_device *v4l2_dev,
					bool read_only)
{
	struct video_device *vdev;
	struct v4l2_subdev *sd;
	int err;

	/* Register a device node for every subdev marked with the
	 * V4L2_SUBDEV_FL_HAS_DEVNODE flag.
	 */
	list_for_each_entry(sd, &v4l2_dev->subdevs, list) {
		if (!(sd->flags & V4L2_SUBDEV_FL_HAS_DEVNODE))
			continue;

		if (sd->devnode)
			continue;

		vdev = kzalloc_obj(*vdev);
		if (!vdev) {
			err = -ENOMEM;
			goto clean_up;
		}

		video_set_drvdata(vdev, sd);
		strscpy(vdev->name, sd->name, sizeof(vdev->name));
		vdev->dev_parent = sd->dev;
		vdev->v4l2_dev = v4l2_dev;
		vdev->fops = &v4l2_subdev_fops;
		vdev->release = v4l2_device_release_subdev_node;
		vdev->ctrl_handler = sd->ctrl_handler;
		if (read_only)
			set_bit(V4L2_FL_SUBDEV_RO_DEVNODE, &vdev->flags);
		sd->devnode = vdev;
		err = __video_register_device(vdev, VFL_TYPE_SUBDEV, -1, 1,
					      sd->owner);
		if (err < 0) {
			sd->devnode = NULL;
			kfree(vdev);
			goto clean_up;
		}
#if defined(CONFIG_MEDIA_CONTROLLER)
		sd->entity.info.dev.major = VIDEO_MAJOR;
		sd->entity.info.dev.minor = vdev->minor;

		/* Interface is created by __video_register_device() */
		if (vdev->v4l2_dev->mdev) {
			struct media_link *link;

			link = media_create_intf_link(&sd->entity,
						      &vdev->intf_devnode->intf,
						      MEDIA_LNK_FL_ENABLED |
						      MEDIA_LNK_FL_IMMUTABLE);
			if (!link) {
				err = -ENOMEM;
				goto clean_up;
			}
		}
#endif
	}
	return 0;

clean_up:
	list_for_each_entry(sd, &v4l2_dev->subdevs, list) {
		if (!sd->devnode)
			break;
		video_unregister_device(sd->devnode);
	}

	return err;
}
EXPORT_SYMBOL_GPL(__v4l2_device_register_subdev_nodes);

void v4l2_device_unregister_subdev(struct v4l2_subdev *sd)
{
	struct v4l2_device *v4l2_dev;

	/* return if it isn't registered */
	if (sd == NULL || sd->v4l2_dev == NULL)
		return;

	v4l2_dev = sd->v4l2_dev;

	spin_lock(&v4l2_dev->lock);
	list_del(&sd->list);
	spin_unlock(&v4l2_dev->lock);

	if (sd->internal_ops && sd->internal_ops->unregistered)
		sd->internal_ops->unregistered(sd);
	sd->v4l2_dev = NULL;

#if defined(CONFIG_MEDIA_CONTROLLER)
	if (v4l2_dev->mdev) {
		/*
		 * No need to explicitly remove links, as both pads and
		 * links are removed by the function below, in the right order
		 */
		media_device_unregister_entity(&sd->entity);
	}
#endif
	if (sd->devnode)
		video_unregister_device(sd->devnode);
	else
		v4l2_subdev_release(sd);
}
EXPORT_SYMBOL_GPL(v4l2_device_unregister_subdev);
