// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Copyright 2018 Noralf Trønnes
 */

/*
 * DRM 客户端基础设施
 *
 * 本文件实现了 DRM 客户端（client）的创建、注册、释放以及客户端缓冲区的管理。
 * DRM 客户端是运行在内核空间中的 DRM 使用者，例如 fbdev 模拟和启动画面（bootsplash）。
 * 它通过打开一个内部的 DRM 文件句柄来与 DRM 驱动交互，并进行模式设置和帧缓冲操作。
 *
 * 主要功能：
 *   - 客户端的初始化、注册和释放（drm_client_init/register/release）
 *   - 客户端缓冲区的创建、删除与映射管理（drm_client_buffer_*）
 *   - 客户端缓冲区的脏区域刷新（drm_client_buffer_flush）
 */

#include <linux/export.h>
#include <linux/iosys-map.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include <drm/drm_client.h>
#include <drm/drm_client_event.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>

#include "drm_crtc_internal.h"
#include "drm_internal.h"

/**
 * DOC: overview
 *
 * This library provides support for clients running in the kernel like fbdev and bootsplash.
 *
 * GEM drivers which provide a GEM based dumb buffer with a virtual address are supported.
 */

static int drm_client_open(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	struct drm_file *file;

	file = drm_file_alloc(dev->primary);
	if (IS_ERR(file))
		return PTR_ERR(file);

	mutex_lock(&dev->filelist_mutex);
	list_add(&file->lhead, &dev->filelist_internal);
	mutex_unlock(&dev->filelist_mutex);

	client->file = file;

	return 0;
}

static void drm_client_close(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;

	mutex_lock(&dev->filelist_mutex);
	list_del(&client->file->lhead);
	mutex_unlock(&dev->filelist_mutex);

	drm_file_free(client->file);
}

/**
 * drm_client_init - 初始化 DRM 客户端
 * @dev: DRM 设备
 * @client: DRM 客户端
 * @name: 客户端名称
 * @funcs: DRM 客户端函数表（可选）
 *
 * 初始化 DRM 客户端并为其打开一个内部的 &drm_file。
 * 调用 drm_client_register() 来完成注册流程。
 * 调用者需要在此函数之前持有 @dev 的引用计数。
 * 当 &drm_device 被注销时，客户端会自动释放。参见 drm_client_release()。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_init(struct drm_device *dev, struct drm_client_dev *client,
		    const char *name, const struct drm_client_funcs *funcs)
{
	int ret;

	if (!drm_core_check_feature(dev, DRIVER_MODESET) || !dev->driver->dumb_create)
		return -EOPNOTSUPP;

	client->dev = dev;
	client->name = name;
	client->funcs = funcs;

	ret = drm_client_modeset_create(client);
	if (ret)
		return ret;

	ret = drm_client_open(client);
	if (ret)
		goto err_free;

	drm_dev_get(dev);

	return 0;

err_free:
	drm_client_modeset_free(client);
	return ret;
}
EXPORT_SYMBOL(drm_client_init);

/**
 * drm_client_register - 注册客户端
 * @client: DRM 客户端
 *
 * 将客户端添加到 &drm_device 的客户端列表中，以激活其回调函数。
 * @client 必须已经通过 drm_client_init() 初始化。
 * 调用 drm_client_register() 之后，不能再直接调用 drm_client_release() 来释放
 * （通过注销回调的方式除外），而是由驱动卸载时自动完成清理。
 *
 * 注册客户端会触发一个热插拔事件，使客户端能够从已有的输出设备中
 * 设置其显示配置。客户端必须已经完成状态初始化，以便能够成功处理该热插拔事件。
 */
void drm_client_register(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	int ret;

	mutex_lock(&dev->clientlist_mutex);
	list_add(&client->list, &dev->clientlist);

	if (client->funcs && client->funcs->hotplug) {
		/*
		 * Perform an initial hotplug event to pick up the
		 * display configuration for the client. This step
		 * has to be performed *after* registering the client
		 * in the list of clients, or a concurrent hotplug
		 * event might be lost; leaving the display off.
		 *
		 * Hold the clientlist_mutex as for a regular hotplug
		 * event.
		 */
		ret = client->funcs->hotplug(client);
		if (ret)
			drm_dbg_kms(dev, "client hotplug ret=%d\n", ret);
	}
	mutex_unlock(&dev->clientlist_mutex);
}
EXPORT_SYMBOL(drm_client_register);

/**
 * drm_client_release - 释放 DRM 客户端资源
 * @client: DRM 客户端
 *
 * 释放由 drm_client_init() 打开的 &drm_file 等相关资源。
 * 如果 &drm_client_funcs.unregister 回调未设置，则由系统自动调用此函数完成清理。
 *
 * 此函数应当仅在注销回调中被调用。例外情况是 fbdev，
 * 因为如果用户空间持有打开的文件描述符，fbdev 无法释放缓冲区。
 *
 * 注意：
 * 客户端不能自行发起释放操作（这是为了保持代码简单）。
 * 必须先卸载驱动，然后才能卸载客户端。
 */
void drm_client_release(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;

	drm_dbg_kms(dev, "%s\n", client->name);

	drm_client_modeset_free(client);
	drm_client_close(client);

	if (client->funcs && client->funcs->free)
		client->funcs->free(client);

	drm_dev_put(dev);
}
EXPORT_SYMBOL(drm_client_release);

/**
 * drm_client_buffer_delete - 删除客户端缓冲区
 * @buffer: DRM 客户端缓冲区
 *
 * 删除一个客户端缓冲区对象：释放其映射、从 DRM 中移除帧缓冲（framebuffer）、
 * 释放 GEM 对象的引用，最后释放缓冲区结构体本身。
 */
void drm_client_buffer_delete(struct drm_client_buffer *buffer)
{
	struct drm_gem_object *gem;
	int ret;

	if (!buffer)
		return;

	gem = buffer->fb->obj[0];
	drm_gem_vunmap(gem, &buffer->map);

	ret = drm_mode_rmfb(buffer->client->dev, buffer->fb->base.id, buffer->client->file);
	if (ret)
		drm_err(buffer->client->dev,
			"Error removing FB:%u (%d)\n", buffer->fb->base.id, ret);

	drm_gem_object_put(buffer->gem);

	kfree(buffer);
}
EXPORT_SYMBOL(drm_client_buffer_delete);

/**
 * drm_client_buffer_create - 创建客户端缓冲区
 * @client: DRM 客户端
 * @width: 宽度
 * @height: 高度
 * @format: 像素格式
 * @handle: GEM 对象句柄
 * @pitch: 每行字节数（跨距）
 *
 * 使用已有的 GEM 对象句柄创建一个客户端缓冲区。
 * 该函数会创建一个 &drm_framebuffer 并将其与给定的句柄关联。
 * 调用者应使用 drm_client_buffer_delete() 释放返回的缓冲区。
 *
 * 返回值：
 * 成功返回指向客户端缓冲区的指针，失败返回错误指针。
 */
{
	struct drm_mode_fb_cmd2 fb_req = {
		.width = width,
		.height = height,
		.pixel_format = format,
		.handles = {
			handle,
		},
		.pitches = {
			pitch,
		},
	};
	struct drm_device *dev = client->dev;
	struct drm_client_buffer *buffer;
	struct drm_gem_object *obj;
	struct drm_framebuffer *fb;
	int ret;

	buffer = kzalloc_obj(*buffer);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	buffer->client = client;

	obj = drm_gem_object_lookup(client->file, handle);
	if (!obj)  {
		ret = -ENOENT;
		goto err_delete;
	}

	ret = drm_mode_addfb2(dev, &fb_req, client->file);
	if (ret)
		goto err_drm_gem_object_put;

	fb = drm_framebuffer_lookup(dev, client->file, fb_req.fb_id);
	if (drm_WARN_ON(dev, !fb)) {
		ret = -ENOENT;
		goto err_drm_mode_rmfb;
	}

	/* drop the reference we picked up in framebuffer lookup */
	drm_framebuffer_put(fb);

	strscpy(fb->comm, client->name, TASK_COMM_LEN);

	buffer->gem = obj;
	buffer->fb = fb;

	return buffer;

err_drm_mode_rmfb:
	drm_mode_rmfb(dev, fb_req.fb_id, client->file);
err_drm_gem_object_put:
	drm_gem_object_put(obj);
err_delete:
	kfree(buffer);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_client_buffer_create);

/**
 * drm_client_buffer_vmap_local - 映射客户端缓冲区到内核地址空间（局部映射）
 * @buffer: DRM 客户端缓冲区
 * @map_copy: 返回映射后的内存地址
 *
 * 将客户端缓冲区映射到内核地址空间。如果缓冲区已经映射，则返回已有映射的地址。
 * 该映射是"局部"的，要求在函数返回后尽快解除映射。
 *
 * 客户端缓冲区映射不进行引用计数。每次调用 drm_client_buffer_vmap_local()
 * 应当紧随一个 drm_client_buffer_vunmap_local() 调用。
 * 对于长期持有的映射，请使用 drm_client_buffer_vmap()。
 *
 * 返回的地址是内部值的副本。与其他 vmap 接口不同，你不需要用它来调用
 * 客户端的 vunmap 函数。因此可以在 blit 和绘图操作中随意修改它。
 *
 * 返回值：
 *	成功返回 0，失败返回负的错误码。
 */
int drm_client_buffer_vmap_local(struct drm_client_buffer *buffer,
				 struct iosys_map *map_copy)
{
	struct drm_gem_object *gem = buffer->fb->obj[0];
	struct iosys_map *map = &buffer->map;
	int ret;

	drm_gem_lock(gem);

	ret = drm_gem_vmap_locked(gem, map);
	if (ret)
		goto err_drm_gem_vmap_unlocked;
	*map_copy = *map;

	return 0;

err_drm_gem_vmap_unlocked:
	drm_gem_unlock(gem);
	return ret;
}
EXPORT_SYMBOL(drm_client_buffer_vmap_local);

/**
 * drm_client_buffer_vunmap_local - 解除 DRM 客户端缓冲区的局部映射
 * @buffer: DRM 客户端缓冲区
 *
 * 移除之前通过 drm_client_buffer_vmap_local() 建立的映射。
 * 仅当客户端自行管理其缓冲区映射时才需要手动调用此函数。
 */
void drm_client_buffer_vunmap_local(struct drm_client_buffer *buffer)
{
	struct drm_gem_object *gem = buffer->fb->obj[0];
	struct iosys_map *map = &buffer->map;

	drm_gem_vunmap_locked(gem, map);
	drm_gem_unlock(gem);
}
EXPORT_SYMBOL(drm_client_buffer_vunmap_local);

/**
 * drm_client_buffer_vmap - 映射客户端缓冲区到内核地址空间（长期映射）
 * @buffer: DRM 客户端缓冲区
 * @map_copy: 返回映射后的内存地址
 *
 * 将客户端缓冲区映射到内核地址空间。如果缓冲区已经映射，则返回已有映射的地址。
 * 该映射是"长期"的，用于在整个生命周期内保持映射。
 *
 * 客户端缓冲区映射不进行引用计数。每次调用 drm_client_buffer_vmap()
 * 应当对应一次 drm_client_buffer_vunmap()；或者让客户端缓冲区
 * 在整个生命周期内保持映射状态。
 *
 * 返回的地址是内部值的副本。与其他 vmap 接口不同，你不需要用它来调用
 * 客户端的 vunmap 函数。因此可以在 blit 和绘图操作中随意修改它。
 *
 * 返回值：
 *	成功返回 0，失败返回负的错误码。
 */
int drm_client_buffer_vmap(struct drm_client_buffer *buffer,
			   struct iosys_map *map_copy)
{
	struct drm_gem_object *gem = buffer->fb->obj[0];
	int ret;

	ret = drm_gem_vmap(gem, &buffer->map);
	if (ret)
		return ret;
	*map_copy = buffer->map;

	return 0;
}
EXPORT_SYMBOL(drm_client_buffer_vmap);

/**
 * drm_client_buffer_vunmap - 解除 DRM 客户端缓冲区的长期映射
 * @buffer: DRM 客户端缓冲区
 *
 * 移除之前通过 drm_client_buffer_vmap() 建立的长期映射。
 * 仅当客户端自行管理其缓冲区映射时才需要手动调用此函数。
 */
void drm_client_buffer_vunmap(struct drm_client_buffer *buffer)
{
	struct drm_gem_object *gem = buffer->fb->obj[0];

	drm_gem_vunmap(gem, &buffer->map);
}
EXPORT_SYMBOL(drm_client_buffer_vunmap);

/**
 * drm_client_buffer_create_dumb - 创建由 dumb 缓冲区支持的客户端缓冲区
 * @client: DRM 客户端
 * @width: 帧缓冲宽度
 * @height: 帧缓冲高度
 * @format: 缓冲区格式
 *
 * 创建一个 &drm_client_buffer，其内部包含一个由 dumb 缓冲区支持的
 * &drm_framebuffer。dumb 缓冲区是一种简单的、驱动分配的连续内存缓冲区，
 * 适用于不需要 GPU 加速的简单显示场景。
 * 调用 drm_client_buffer_delete() 释放该缓冲区。
 *
 * 返回值：
 * 成功返回指向客户端缓冲区的指针，失败返回错误指针。
 */
struct drm_client_buffer *
drm_client_buffer_create_dumb(struct drm_client_dev *client, u32 width, u32 height, u32 format)
{
	const struct drm_format_info *info = drm_format_info(format);
	struct drm_device *dev = client->dev;
	struct drm_mode_create_dumb dumb_args = { };
	struct drm_client_buffer *buffer;
	int ret;

	dumb_args.width = width;
	dumb_args.height = height;
	dumb_args.bpp = drm_format_info_bpp(info, 0);
	ret = drm_mode_create_dumb(dev, &dumb_args, client->file);
	if (ret)
		return ERR_PTR(ret);

	buffer = drm_client_buffer_create(client, width, height, format,
					  dumb_args.handle, dumb_args.pitch);
	if (IS_ERR(buffer)) {
		ret = PTR_ERR(buffer);
		goto err_drm_mode_destroy_dumb;
	}

	/*
	 * The handle is only needed for creating the framebuffer, destroy it
	 * again to solve a circular dependency should anybody export the GEM
	 * object as DMA-buf. The framebuffer and our buffer structure are still
	 * holding references to the GEM object to prevent its destruction.
	 */
	drm_mode_destroy_dumb(client->dev, dumb_args.handle, client->file);

	return buffer;

err_drm_mode_destroy_dumb:
	drm_mode_destroy_dumb(client->dev, dumb_args.handle, client->file);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(drm_client_buffer_create_dumb);

/**
 * drm_client_buffer_flush - 手动刷新客户端缓冲区
 * @buffer: DRM 客户端缓冲区
 * @rect: 脏区域矩形（如果为 NULL 则刷新全部）
 *
 * 调用 &drm_framebuffer_funcs->dirty 回调（如果存在的话）来刷新缓冲区的更改。
 * 某些硬件驱动需要显式的脏区域通知来更新显示内容。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_buffer_flush(struct drm_client_buffer *buffer, struct drm_rect *rect)
{
	if (!buffer || !buffer->fb || !buffer->fb->funcs->dirty)
		return 0;

	if (rect) {
		struct drm_clip_rect clip = {
			.x1 = rect->x1,
			.y1 = rect->y1,
			.x2 = rect->x2,
			.y2 = rect->y2,
		};

		return buffer->fb->funcs->dirty(buffer->fb, buffer->client->file,
						0, 0, &clip, 1);
	}

	return buffer->fb->funcs->dirty(buffer->fb, buffer->client->file,
					0, 0, NULL, 0);
}
EXPORT_SYMBOL(drm_client_buffer_flush);
