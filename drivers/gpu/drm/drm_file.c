/*
 * \author Rickard E. (Rik) Faith <faith@valinux.com>
 * \author Daryll Strauss <daryll@valinux.com>
 * \author Gareth Hughes <gareth@valinux.com>
 */

/*
 * Created: Mon Jan  4 08:58:31 1999 by faith@valinux.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * DRM 文件操作管理 - 实现 DRM 设备文件的打开、关闭、读写和事件管理
 *
 * 本文件实现了 DRM 框架的文件操作层，是用户空间与 DRM 内核驱动之间
 * 的主要接口。驱动程序必须在其 &file_operations 结构中注册此文件提供
 * 的函数，作为 DRM 用户空间 API 的入口点。
 *
 * 核心功能：
 *
 *   文件生命周期：
 *     - drm_file_alloc() / drm_file_free() - DRM 文件上下文的分配和释放
 *     - drm_open() - 打开 DRM 设备节点，创建文件上下文并初始化资源
 *     - drm_release() / drm_release_noglobal() - 关闭文件，释放资源
 *
 *   事件管理：
 *     - drm_event_reserve_init() - 预留事件空间并初始化事件
 *     - drm_event_cancel_free() - 取消并释放未投递的事件
 *     - drm_send_event() / drm_send_event_locked() - 发送事件到用户空间
 *     - drm_read() - 从事件队列读取 DRM 事件
 *     - drm_poll() - 轮询事件队列状态
 *
 *   fdinfo 支持：
 *     - drm_show_fdinfo() - 显示文件描述符信息（驱动、客户端 ID、内存统计）
 *     - drm_print_memory_stats() - 打印 GEM 对象内存统计
 *     - drm_show_memory_stats() - 收集并显示内存使用统计
 *
 *   其他：
 *     - mock_drm_getfile() - 创建虚拟 DRM 文件（用于测试）
 *     - drm_file_update_pid() - 更新文件关联的进程 ID
 */

#include <linux/anon_inodes.h>
#include <linux/dma-fence.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/vga_switcheroo.h>

#include <drm/drm_client_event.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_print.h>
#include <drm/drm_debugfs.h>

#include "drm_crtc_internal.h"
#include "drm_internal.h"

/* from BKL pushdown */
DEFINE_MUTEX(drm_global_mutex);

bool drm_dev_needs_global_mutex(struct drm_device *dev)
{
	/*
	 * The deprecated ->load callback must be called after the driver is
	 * already registered. This means such drivers rely on the BKL to make
	 * sure an open can't proceed until the driver is actually fully set up.
	 * Similar hilarity holds for the unload callback.
	 */
	if (dev->driver->load || dev->driver->unload)
		return true;

	return false;
}

/**
 * DOC: file operations
 *
 * Drivers must define the file operations structure that forms the DRM
 * userspace API entry point, even though most of those operations are
 * implemented in the DRM core. The resulting &struct file_operations must be
 * stored in the &drm_driver.fops field. The mandatory functions are drm_open(),
 * drm_read(), drm_ioctl() and drm_compat_ioctl() if CONFIG_COMPAT is enabled
 * Note that drm_compat_ioctl will be NULL if CONFIG_COMPAT=n, so there's no
 * need to sprinkle #ifdef into the code. Drivers which implement private ioctls
 * that require 32/64 bit compatibility support must provide their own
 * &file_operations.compat_ioctl handler that processes private ioctls and calls
 * drm_compat_ioctl() for core ioctls.
 *
 * In addition drm_read() and drm_poll() provide support for DRM events. DRM
 * events are a generic and extensible means to send asynchronous events to
 * userspace through the file descriptor. They are used to send vblank event and
 * page flip completions by the KMS API. But drivers can also use it for their
 * own needs, e.g. to signal completion of rendering.
 *
 * For the driver-side event interface see drm_event_reserve_init() and
 * drm_send_event() as the main starting points.
 *
 * The memory mapping implementation will vary depending on how the driver
 * manages memory. For GEM-based drivers this is drm_gem_mmap().
 *
 * No other file operations are supported by the DRM userspace API. Overall the
 * following is an example &file_operations structure::
 *
 *     static const example_drm_fops = {
 *             .owner = THIS_MODULE,
 *             .open = drm_open,
 *             .release = drm_release,
 *             .unlocked_ioctl = drm_ioctl,
 *             .compat_ioctl = drm_compat_ioctl, // NULL if CONFIG_COMPAT=n
 *             .poll = drm_poll,
 *             .read = drm_read,
 *             .mmap = drm_gem_mmap,
 *     };
 *
 * For plain GEM based drivers there is the DEFINE_DRM_GEM_FOPS() macro, and for
 * DMA based drivers there is the DEFINE_DRM_GEM_DMA_FOPS() macro to make this
 * simpler.
 *
 * The driver's &file_operations must be stored in &drm_driver.fops.
 *
 * For driver-private IOCTL handling see the more detailed discussion in
 * :ref:`IOCTL support in the userland interfaces chapter<drm_driver_ioctl>`.
 */

/*
 * drm_file_alloc - 分配 DRM 文件上下文
 * @minor: 要关联的 DRM minor
 *
 * 创建并初始化一个新的 DRM 文件上下文，包括：
 *   - 分配唯一的客户端 ID（用于 fdinfo）
 *   - 初始化事件列表、帧缓冲列表、blob 列表
 *   - 初始化 GEM、syncobj、prime 等子系统相关的文件私有数据
 *   - 调用驱动注册的 open 回调
 *
 * 该上下文不会自动链接到任何链表，调用者需要负责后续的链接操作。
 * 注意：上下文持有 @minor 的指针，必须在 @minor 释放之前释放此上下文。
 *
 * 返回：指向新分配的文件上下文的指针，失败时返回 ERR_PTR。
 */
/**
 * drm_file_alloc - allocate file context
 * @minor: minor to allocate on
 *
 * This allocates a new DRM file context. It is not linked into any context and
 * can be used by the caller freely. Note that the context keeps a pointer to
 * @minor, so it must be freed before @minor is.
 *
 * RETURNS:
 * Pointer to newly allocated context, ERR_PTR on failure.
 */
struct drm_file *drm_file_alloc(struct drm_minor *minor)
{
	static atomic64_t ident = ATOMIC64_INIT(0);
	struct drm_device *dev = minor->dev;
	struct drm_file *file;
	int ret;

	file = kzalloc_obj(*file);
	if (!file)
		return ERR_PTR(-ENOMEM);

	/* Get a unique identifier for fdinfo: */
	file->client_id = atomic64_inc_return(&ident);
	rcu_assign_pointer(file->pid, get_pid(task_tgid(current)));
	file->minor = minor;

	/* for compatibility root is always authenticated */
	file->authenticated = capable(CAP_SYS_ADMIN);

	INIT_LIST_HEAD(&file->lhead);
	INIT_LIST_HEAD(&file->fbs);
	mutex_init(&file->fbs_lock);
	INIT_LIST_HEAD(&file->blobs);
	INIT_LIST_HEAD(&file->pending_event_list);
	INIT_LIST_HEAD(&file->event_list);
	init_waitqueue_head(&file->event_wait);
	file->event_space = 4096; /* set aside 4k for event buffer */

	spin_lock_init(&file->master_lookup_lock);
	mutex_init(&file->event_read_lock);
	mutex_init(&file->client_name_lock);

	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_open(dev, file);

	if (drm_core_check_feature(dev, DRIVER_SYNCOBJ))
		drm_syncobj_open(file);

	drm_prime_init_file_private(&file->prime);

	if (!drm_core_check_feature(dev, DRIVER_COMPUTE_ACCEL))
		drm_debugfs_clients_add(file);

	if (dev->driver->open) {
		ret = dev->driver->open(dev, file);
		if (ret < 0)
			goto out_prime_destroy;
	}

	return file;

out_prime_destroy:
	drm_prime_destroy_file_private(&file->prime);
	if (drm_core_check_feature(dev, DRIVER_SYNCOBJ))
		drm_syncobj_release(file);
	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_release(dev, file);

	if (!drm_core_check_feature(dev, DRIVER_COMPUTE_ACCEL))
		drm_debugfs_clients_remove(file);

	put_pid(rcu_access_pointer(file->pid));
	kfree(file);

	return ERR_PTR(ret);
}

static void drm_events_release(struct drm_file *file_priv)
{
	struct drm_device *dev = file_priv->minor->dev;
	struct drm_pending_event *e, *et;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	/* Unlink pending events */
	list_for_each_entry_safe(e, et, &file_priv->pending_event_list,
				 pending_link) {
		list_del(&e->pending_link);
		e->file_priv = NULL;
	}

	/* Remove unconsumed events */
	list_for_each_entry_safe(e, et, &file_priv->event_list, link) {
		list_del(&e->link);
		kfree(e);
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

/*
 * drm_file_free - 释放 DRM 文件上下文
 * @file: 要释放的文件上下文（允许为 NULL）
 *
 * 销毁由 drm_file_alloc() 分配的 DRM 文件上下文。在调用此函数前，
 * 调用者必须确保已将其从任何上下文中取消链接。
 * 释放过程包括：关闭 GEM 句柄、释放 syncobj、释放事件队列、
 * 释放 prime 文件私有数据、调用驱动的 postclose 回调等。
 *
 * 如果传入 NULL，此函数无操作。
 */
/**
 * drm_file_free - free file context
 * @file: context to free, or NULL
 *
 * This destroys and deallocates a DRM file context previously allocated via
 * drm_file_alloc(). The caller must make sure to unlink it from any contexts
 * before calling this.
 *
 * If NULL is passed, this is a no-op.
 */
void drm_file_free(struct drm_file *file)
{
	struct drm_device *dev;

	if (!file)
		return;

	dev = file->minor->dev;

	drm_dbg_core(dev, "comm=\"%s\", pid=%d, dev=0x%lx, open_count=%d\n",
		     current->comm, task_pid_nr(current),
		     (long)old_encode_dev(file->minor->kdev->devt),
		     atomic_read(&dev->open_count));

	if (!drm_core_check_feature(dev, DRIVER_COMPUTE_ACCEL))
		drm_debugfs_clients_remove(file);

	drm_events_release(file);

	if (drm_core_check_feature(dev, DRIVER_MODESET)) {
		drm_fb_release(file);
		drm_property_destroy_user_blobs(dev, file);
	}

	if (drm_core_check_feature(dev, DRIVER_SYNCOBJ))
		drm_syncobj_release(file);

	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_release(dev, file);

	if (drm_is_primary_client(file))
		drm_master_release(file);

	if (dev->driver->postclose)
		dev->driver->postclose(dev, file);

	drm_prime_destroy_file_private(&file->prime);

	WARN_ON(!list_empty(&file->event_list));

	put_pid(rcu_access_pointer(file->pid));

	mutex_destroy(&file->client_name_lock);
	kfree(file->client_name);

	kfree(file);
}

static void drm_close_helper(struct file *filp)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev = file_priv->minor->dev;

	mutex_lock(&dev->filelist_mutex);
	list_del(&file_priv->lhead);
	mutex_unlock(&dev->filelist_mutex);

	drm_file_free(file_priv);
}

/*
 * Check whether DRI will run on this CPU.
 *
 * \return non-zero if the DRI will run on this CPU, or zero otherwise.
 */
static int drm_cpu_valid(void)
{
#if defined(__sparc__) && !defined(__sparc_v9__)
	return 0;		/* No cmpxchg before v9 sparc. */
#endif
	return 1;
}

/*
 * Called whenever a process opens a drm node
 *
 * \param filp file pointer.
 * \param minor acquired minor-object.
 * \return zero on success or a negative number on failure.
 *
 * Creates and initializes a drm_file structure for the file private data in \p
 * filp and add it into the double linked list in \p dev.
 */
int drm_open_helper(struct file *filp, struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct drm_file *priv;
	int ret;

	if (filp->f_flags & O_EXCL)
		return -EBUSY;	/* No exclusive opens */
	if (!drm_cpu_valid())
		return -EINVAL;
	if (dev->switch_power_state != DRM_SWITCH_POWER_ON &&
	    dev->switch_power_state != DRM_SWITCH_POWER_DYNAMIC_OFF)
		return -EINVAL;
	if (WARN_ON_ONCE(!(filp->f_op->fop_flags & FOP_UNSIGNED_OFFSET)))
		return -EINVAL;

	drm_dbg_core(dev, "comm=\"%s\", pid=%d, minor=%d\n",
		     current->comm, task_pid_nr(current), minor->index);

	priv = drm_file_alloc(minor);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	if (drm_is_primary_client(priv)) {
		ret = drm_master_open(priv);
		if (ret) {
			drm_file_free(priv);
			return ret;
		}
	}

	filp->private_data = priv;
	priv->filp = filp;

	mutex_lock(&dev->filelist_mutex);
	list_add(&priv->lhead, &dev->filelist);
	mutex_unlock(&dev->filelist_mutex);

	return 0;
}

/*
 * drm_open - DRM 设备文件的打开方法
 * @inode: 设备 inode
 * @filp: 文件指针
 *
 * 驱动程序必须将此函数用作其 file_operations.open 方法。它负责：
 *   1. 通过 inode 中的次设备号查找对应的 DRM minor 设备
 *   2. 分配 drm_file 文件上下文并初始化所有每文件资源
 *   3. 如果是主设备节点，处理主控权分配
 *   4. 调用驱动的 open 回调
 *   5. 将文件上下文添加到设备的文件列表中
 *
 * 返回：0 表示成功，负错误码表示失败。
 */
/**
 * drm_open - open method for DRM file
 * @inode: device inode
 * @filp: file pointer.
 *
 * This function must be used by drivers as their &file_operations.open method.
 * It looks up the correct DRM device and instantiates all the per-file
 * resources for it. It also calls the &drm_driver.open driver callback.
 *
 * RETURNS:
 * 0 on success or negative errno value on failure.
 */
int drm_open(struct inode *inode, struct file *filp)
{
	struct drm_device *dev;
	struct drm_minor *minor;
	int retcode;

	minor = drm_minor_acquire(&drm_minors_xa, iminor(inode));
	if (IS_ERR(minor))
		return PTR_ERR(minor);

	dev = minor->dev;
	if (drm_dev_needs_global_mutex(dev))
		mutex_lock(&drm_global_mutex);

	atomic_fetch_inc(&dev->open_count);

	/* share address_space across all char-devs of a single device */
	filp->f_mapping = dev->anon_inode->i_mapping;

	retcode = drm_open_helper(filp, minor);
	if (retcode)
		goto err_undo;

	if (drm_dev_needs_global_mutex(dev))
		mutex_unlock(&drm_global_mutex);

	return 0;

err_undo:
	atomic_dec(&dev->open_count);
	if (drm_dev_needs_global_mutex(dev))
		mutex_unlock(&drm_global_mutex);
	drm_minor_release(minor);
	return retcode;
}
EXPORT_SYMBOL(drm_open);

static void drm_lastclose(struct drm_device *dev)
{
	drm_client_dev_restore(dev, false);

	if (dev_is_pci(dev->dev))
		vga_switcheroo_process_delayed_switch();
}

/*
 * drm_release - DRM 设备文件的释放方法
 * @inode: 设备 inode
 * @filp: 文件指针
 *
 * 驱动程序必须将此函数用作其 file_operations.release 方法。
 * 释放所有与打开文件关联的资源。如果这是该 DRM 设备的最后一个
 * 打开文件，还会恢复活跃的内核 DRM 客户端并触发延迟的显卡切换。
 *
 * 此版本会获取 drm_global_mutex，适用于需要全局锁的驱动。
 * 对于不需要全局锁的驱动，应使用 drm_release_noglobal()。
 *
 * 返回：总是成功并返回 0。
 */
/**
 * drm_release - release method for DRM file
 * @inode: device inode
 * @filp: file pointer.
 *
 * This function must be used by drivers as their &file_operations.release
 * method. It frees any resources associated with the open file. If this
 * is the last open file for the DRM device, it also restores the active
 * in-kernel DRM client.
 *
 * RETURNS:
 * Always succeeds and returns 0.
 */
int drm_release(struct inode *inode, struct file *filp)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_minor *minor = file_priv->minor;
	struct drm_device *dev = minor->dev;

	if (drm_dev_needs_global_mutex(dev))
		mutex_lock(&drm_global_mutex);

	drm_dbg_core(dev, "open_count = %d\n", atomic_read(&dev->open_count));

	drm_close_helper(filp);

	if (atomic_dec_and_test(&dev->open_count))
		drm_lastclose(dev);

	if (drm_dev_needs_global_mutex(dev))
		mutex_unlock(&drm_global_mutex);

	drm_minor_release(minor);

	return 0;
}
EXPORT_SYMBOL(drm_release);

void drm_file_update_pid(struct drm_file *filp)
{
	struct drm_device *dev;
	struct pid *pid, *old;

	/*
	 * Master nodes need to keep the original ownership in order for
	 * drm_master_check_perm to keep working correctly. (See comment in
	 * drm_auth.c.)
	 */
	if (filp->was_master)
		return;

	pid = task_tgid(current);

	/*
	 * Quick unlocked check since the model is a single handover followed by
	 * exclusive repeated use.
	 */
	if (pid == rcu_access_pointer(filp->pid))
		return;

	dev = filp->minor->dev;
	mutex_lock(&dev->filelist_mutex);
	get_pid(pid);
	old = rcu_replace_pointer(filp->pid, pid, 1);
	mutex_unlock(&dev->filelist_mutex);

	synchronize_rcu();
	put_pid(old);
}

/**
 * drm_release_noglobal - DRM 文件释放函数（无全局锁版本）
 * @inode: 设备 inode
 * @filp: 文件指针
 *
 * 驱动程序可以使用此函数作为其 &file_operations.release 方法。
 * 它在获取 drm_global_mutex 之前释放与打开文件关联的所有资源。
 * 如果这是 DRM 设备的最后一个打开文件，则会恢复到活跃的内核态
 * DRM 客户端。
 *
 * 与 drm_release() 的区别在于此函数不获取 drm_global_mutex，
 * 适用于不需要全局锁的驱动程序。
 *
 * 返回：
 * 始终成功并返回 0。
 */
int drm_release_noglobal(struct inode *inode, struct file *filp)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_minor *minor = file_priv->minor;
	struct drm_device *dev = minor->dev;

	drm_close_helper(filp);

	if (atomic_dec_and_mutex_lock(&dev->open_count, &drm_global_mutex)) {
		drm_lastclose(dev);
		mutex_unlock(&drm_global_mutex);
	}

	drm_minor_release(minor);

	return 0;
}
EXPORT_SYMBOL(drm_release_noglobal);

/*
 * drm_read - DRM 文件的读取方法（事件读取）
 * @filp: 文件指针
 * @buffer: 用户空间的目标缓冲区
 * @count: 要读取的字节数
 * @offset: 读取偏移量（被忽略，DRM 事件以管道方式读取）
 *
 * 驱动程序必须将此函数用作其 file_operations.read 方法（如果它们使用
 * DRM 事件进行异步通知）。由于 KMS API 使用事件进行 vblank 和页面翻转
 * 完成通知，所有现代显示驱动都必须使用此函数。
 *
 * 此函数只会读取完整的事件。用户空间必须提供足够大的缓冲区来容纳
 * 任何事件以确保向前推进。由于最大事件空间目前为 4K，建议安全地使用
 * 此大小。轮询支持由 drm_poll() 提供。
 *
 * 返回：读取的字节数（总是对齐到完整事件，可能为 0），
 *       或负错误码。
 */
/**
 * drm_read - read method for DRM file
 * @filp: file pointer
 * @buffer: userspace destination pointer for the read
 * @count: count in bytes to read
 * @offset: offset to read
 *
 * This function must be used by drivers as their &file_operations.read
 * method if they use DRM events for asynchronous signalling to userspace.
 * Since events are used by the KMS API for vblank and page flip completion this
 * means all modern display drivers must use it.
 *
 * @offset is ignored, DRM events are read like a pipe. Polling support is
 * provided by drm_poll().
 *
 * This function will only ever read a full event. Therefore userspace must
 * supply a big enough buffer to fit any event to ensure forward progress. Since
 * the maximum event space is currently 4K it's recommended to just use that for
 * safety.
 *
 * RETURNS:
 * Number of bytes read (always aligned to full events, and can be 0) or a
 * negative error code on failure.
 */
ssize_t drm_read(struct file *filp, char __user *buffer,
		 size_t count, loff_t *offset)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev = file_priv->minor->dev;
	ssize_t ret;

	ret = mutex_lock_interruptible(&file_priv->event_read_lock);
	if (ret)
		return ret;

	for (;;) {
		struct drm_pending_event *e = NULL;

		spin_lock_irq(&dev->event_lock);
		if (!list_empty(&file_priv->event_list)) {
			e = list_first_entry(&file_priv->event_list,
					struct drm_pending_event, link);
			file_priv->event_space += e->event->length;
			list_del(&e->link);
		}
		spin_unlock_irq(&dev->event_lock);

		if (e == NULL) {
			if (ret)
				break;

			if (filp->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}

			mutex_unlock(&file_priv->event_read_lock);
			ret = wait_event_interruptible(file_priv->event_wait,
						       !list_empty(&file_priv->event_list));
			if (ret >= 0)
				ret = mutex_lock_interruptible(&file_priv->event_read_lock);
			if (ret)
				return ret;
		} else {
			unsigned length = e->event->length;

			if (length > count - ret) {
put_back_event:
				spin_lock_irq(&dev->event_lock);
				file_priv->event_space -= length;
				list_add(&e->link, &file_priv->event_list);
				spin_unlock_irq(&dev->event_lock);
				wake_up_interruptible_poll(&file_priv->event_wait,
					EPOLLIN | EPOLLRDNORM);
				break;
			}

			if (copy_to_user(buffer + ret, e->event, length)) {
				if (ret == 0)
					ret = -EFAULT;
				goto put_back_event;
			}

			ret += length;
			kfree(e);
		}
	}
	mutex_unlock(&file_priv->event_read_lock);

	return ret;
}
EXPORT_SYMBOL(drm_read);

/*
 * drm_poll - DRM 文件的轮询方法
 * @filp: 文件指针
 * @wait: 轮询等待表
 *
 * 如果驱动使用 DRM 事件进行异步通知，则必须将次函数用作
 * file_operations.poll 方法。它允许用户空间通过 poll/select/epoll
 * 机制等待 DRM 事件（如 vblank、页面翻转完成）的到来。
 *
 * 另请参阅 drm_read()。
 *
 * 返回：表示文件当前状态的 POLL 标志掩码。
 */
/**
 * drm_poll - poll method for DRM file
 * @filp: file pointer
 * @wait: poll waiter table
 *
 * This function must be used by drivers as their &file_operations.read method
 * if they use DRM events for asynchronous signalling to userspace.  Since
 * events are used by the KMS API for vblank and page flip completion this means
 * all modern display drivers must use it.
 *
 * See also drm_read().
 *
 * RETURNS:
 * Mask of POLL flags indicating the current status of the file.
 */
__poll_t drm_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct drm_file *file_priv = filp->private_data;
	__poll_t mask = 0;

	poll_wait(filp, &file_priv->event_wait, wait);

	if (!list_empty(&file_priv->event_list))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}
EXPORT_SYMBOL(drm_poll);

/*
 * drm_event_reserve_init_locked - 初始化 DRM 事件并预留空间（已加锁版本）
 * @dev: DRM 设备
 * @file_priv: DRM 文件私有数据
 * @p: 待处理事件的跟踪结构
 * @e: 要投递给用户空间的实际事件数据
 *
 * 准备传递的事件并预留事件缓冲区空间。如果事件最终未投递（例如 IOCTL
 * 后续步骤失败），必须使用 drm_event_cancel_free() 取消并释放。
 * 成功初始化的事件应通过 drm_send_event() 或 drm_send_event_locked()
 * 在异步操作完成时发送给用户空间。
 *
 * 如果调用者将 @p 嵌入到更大的结构中，该结构必须通过 kmalloc 分配，
 * 且 @p 必须是第一个成员元素。
 *
 * 此版本适用于已持有 drm_device.event_lock 的调用者。
 *
 * 返回：0 表示成功，-ENOMEM 表示事件空间不足。
 */
/**
 * drm_event_reserve_init_locked - init a DRM event and reserve space for it
 * @dev: DRM device
 * @file_priv: DRM file private data
 * @p: tracking structure for the pending event
 * @e: actual event data to deliver to userspace
 *
 * This function prepares the passed in event for eventual delivery. If the event
 * doesn't get delivered (because the IOCTL fails later on, before queuing up
 * anything) then the even must be cancelled and freed using
 * drm_event_cancel_free(). Successfully initialized events should be sent out
 * using drm_send_event() or drm_send_event_locked() to signal completion of the
 * asynchronous event to userspace.
 *
 * If callers embedded @p into a larger structure it must be allocated with
 * kmalloc and @p must be the first member element.
 *
 * This is the locked version of drm_event_reserve_init() for callers which
 * already hold &drm_device.event_lock.
 *
 * RETURNS:
 * 0 on success or a negative error code on failure.
 */
int drm_event_reserve_init_locked(struct drm_device *dev,
				  struct drm_file *file_priv,
				  struct drm_pending_event *p,
				  struct drm_event *e)
{
	if (file_priv->event_space < e->length)
		return -ENOMEM;

	file_priv->event_space -= e->length;

	p->event = e;
	list_add(&p->pending_link, &file_priv->pending_event_list);
	p->file_priv = file_priv;

	return 0;
}
EXPORT_SYMBOL(drm_event_reserve_init_locked);

/*
 * drm_event_reserve_init - 初始化 DRM 事件并预留空间（自动加锁版本）
 * @dev: DRM 设备
 * @file_priv: DRM 文件私有数据
 * @p: 待处理事件的跟踪结构
 * @e: 要投递给用户空间的实际事件数据
 *
 * 与 drm_event_reserve_init_locked() 功能相同，但此函数会自动获取
 * drm_device.event_lock。适用于尚未持有 event_lock 的调用者。
 * 已持有 event_lock 的调用者应使用 drm_event_reserve_init_locked()。
 *
 * 返回：0 表示成功，-ENOMEM 表示事件空间不足。
 */
/**
 * drm_event_reserve_init - init a DRM event and reserve space for it
 * @dev: DRM device
 * @file_priv: DRM file private data
 * @p: tracking structure for the pending event
 * @e: actual event data to deliver to userspace
 *
 * This function prepares the passed in event for eventual delivery. If the event
 * doesn't get delivered (because the IOCTL fails later on, before queuing up
 * anything) then the even must be cancelled and freed using
 * drm_event_cancel_free(). Successfully initialized events should be sent out
 * using drm_send_event() or drm_send_event_locked() to signal completion of the
 * asynchronous event to userspace.
 *
 * If callers embedded @p into a larger structure it must be allocated with
 * kmalloc and @p must be the first member element.
 *
 * Callers which already hold &drm_device.event_lock should use
 * drm_event_reserve_init_locked() instead.
 *
 * RETURNS:
 * 0 on success or a negative error code on failure.
 */
int drm_event_reserve_init(struct drm_device *dev,
			   struct drm_file *file_priv,
			   struct drm_pending_event *p,
			   struct drm_event *e)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->event_lock, flags);
	ret = drm_event_reserve_init_locked(dev, file_priv, p, e);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return ret;
}
EXPORT_SYMBOL(drm_event_reserve_init);

/*
 * drm_event_cancel_free - 取消并释放 DRM 事件及其预留空间
 * @dev: DRM 设备
 * @p: 待处理事件的跟踪结构
 *
 * 释放通过 drm_event_reserve_init() 初始化的事件，并归还其预留的
 * 事件缓冲区空间。用于在非阻塞操作无法提交需要中止时取消事件。
 * 此函数会处理 fence 引用的释放。
 */
/**
 * drm_event_cancel_free - free a DRM event and release its space
 * @dev: DRM device
 * @p: tracking structure for the pending event
 *
 * This function frees the event @p initialized with drm_event_reserve_init()
 * and releases any allocated space. It is used to cancel an event when the
 * nonblocking operation could not be submitted and needed to be aborted.
 */
void drm_event_cancel_free(struct drm_device *dev,
			   struct drm_pending_event *p)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (p->file_priv) {
		p->file_priv->event_space += p->event->length;
		list_del(&p->pending_link);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (p->fence)
		dma_fence_put(p->fence);

	kfree(p);
}
EXPORT_SYMBOL(drm_event_cancel_free);

static void drm_send_event_helper(struct drm_device *dev,
			   struct drm_pending_event *e, ktime_t timestamp)
{
	assert_spin_locked(&dev->event_lock);

	if (e->completion) {
		complete_all(e->completion);
		e->completion_release(e->completion);
		e->completion = NULL;
	}

	if (e->fence) {
		if (timestamp)
			dma_fence_signal_timestamp(e->fence, timestamp);
		else
			dma_fence_signal(e->fence);
		dma_fence_put(e->fence);
	}

	if (!e->file_priv) {
		kfree(e);
		return;
	}

	list_del(&e->pending_link);
	list_add_tail(&e->link,
		      &e->file_priv->event_list);
	wake_up_interruptible_poll(&e->file_priv->event_wait,
		EPOLLIN | EPOLLRDNORM);
}

/*
 * drm_send_event_timestamp_locked - 发送带时间戳的 DRM 事件（已加锁版本）
 * @dev: DRM 设备
 * @e: 要投递的 DRM 事件
 * @timestamp: fence 事件的时间戳（CLOCK_MONOTONIC 时间域）
 *
 * 将通过 drm_event_reserve_init() 初始化的事件 @e 发送到关联的
 * DRM 文件描述符。调用者必须已持有 drm_device.event_lock。
 *
 * 核心会在对应的 DRM 文件关闭时自动取消链接和解除事件。驱动无需担心
 * 此事件的 DRM 文件是否仍然存在，可以在异步工作完成时无条件调用。
 */
/**
 * drm_send_event_timestamp_locked - send DRM event to file descriptor
 * @dev: DRM device
 * @e: DRM event to deliver
 * @timestamp: timestamp to set for the fence event in kernel's CLOCK_MONOTONIC
 * time domain
 *
 * This function sends the event @e, initialized with drm_event_reserve_init(),
 * to its associated userspace DRM file. Callers must already hold
 * &drm_device.event_lock.
 *
 * Note that the core will take care of unlinking and disarming events when the
 * corresponding DRM file is closed. Drivers need not worry about whether the
 * DRM file for this event still exists and can call this function upon
 * completion of the asynchronous work unconditionally.
 */
void drm_send_event_timestamp_locked(struct drm_device *dev,
				     struct drm_pending_event *e, ktime_t timestamp)
{
	drm_send_event_helper(dev, e, timestamp);
}
EXPORT_SYMBOL(drm_send_event_timestamp_locked);

/*
 * drm_send_event_locked - 发送 DRM 事件到文件描述符（已加锁版本）
 * @dev: DRM 设备
 * @e: 要投递的 DRM 事件
 *
 * 将通过 drm_event_reserve_init() 初始化的事件发送到关联的 DRM 文件
 * 描述符。调用者必须已持有 drm_device.event_lock。
 * 参见 drm_send_event() 获取未加锁版本。
 *
 * 核心会在对应的 DRM 文件关闭时自动处理事件的取消链接和解除。
 */
/**
 * drm_send_event_locked - send DRM event to file descriptor
 * @dev: DRM device
 * @e: DRM event to deliver
 *
 * This function sends the event @e, initialized with drm_event_reserve_init(),
 * to its associated userspace DRM file. Callers must already hold
 * &drm_device.event_lock, see drm_send_event() for the unlocked version.
 *
 * Note that the core will take care of unlinking and disarming events when the
 * corresponding DRM file is closed. Drivers need not worry about whether the
 * DRM file for this event still exists and can call this function upon
 * completion of the asynchronous work unconditionally.
 */
void drm_send_event_locked(struct drm_device *dev, struct drm_pending_event *e)
{
	drm_send_event_helper(dev, e, 0);
}
EXPORT_SYMBOL(drm_send_event_locked);

/*
 * drm_send_event - 发送 DRM 事件到文件描述符（自动加锁版本）
 * @dev: DRM 设备
 * @e: 要投递的 DRM 事件
 *
 * 将通过 drm_event_reserve_init() 初始化的事件发送到关联的 DRM 文件
 * 描述符。此函数会自动获取 drm_device.event_lock。
 * 已持有 event_lock 的调用者应使用 drm_send_event_locked()。
 *
 * 核心会在对应的 DRM 文件关闭时自动处理事件的取消链接和解除。
 */
/**
 * drm_send_event - send DRM event to file descriptor
 * @dev: DRM device
 * @e: DRM event to deliver
 *
 * This function sends the event @e, initialized with drm_event_reserve_init(),
 * to its associated userspace DRM file. This function acquires
 * &drm_device.event_lock, see drm_send_event_locked() for callers which already
 * hold this lock.
 *
 * Note that the core will take care of unlinking and disarming events when the
 * corresponding DRM file is closed. Drivers need not worry about whether the
 * DRM file for this event still exists and can call this function upon
 * completion of the asynchronous work unconditionally.
 */
void drm_send_event(struct drm_device *dev, struct drm_pending_event *e)
{
	unsigned long irqflags;

	spin_lock_irqsave(&dev->event_lock, irqflags);
	drm_send_event_helper(dev, e, 0);
	spin_unlock_irqrestore(&dev->event_lock, irqflags);
}
EXPORT_SYMBOL(drm_send_event);

/**
 * drm_fdinfo_print_size - 格式化并打印 DRM fdinfo 内存大小信息
 * @p: DRM 打印机
 * @prefix: 前缀字符串（如 "drm-memory"）
 * @stat: 统计类型（如 "total", "shared", "active"）
 * @region: 内存区域名称（如 "gem"）
 * @sz: 大小值（字节）
 *
 * 以可读的格式打印 DRM 文件描述符信息中的内存使用统计。
 * 根据大小自动选择合适的单位（字节、KiB、MiB），
 * 便于用户空间工具（如 gputop）解析 GPU 内存使用情况。
 */
void drm_fdinfo_print_size(struct drm_printer *p,
			   const char *prefix,
			   const char *stat,
			   const char *region,
			   u64 sz)
{
	const char *units[] = {"", " KiB", " MiB"};
	unsigned u;

	for (u = 0; u < ARRAY_SIZE(units) - 1; u++) {
		if (sz == 0 || !IS_ALIGNED(sz, SZ_1K))
			break;
		sz = div_u64(sz, SZ_1K);
	}

	drm_printf(p, "%s-%s-%s:\t%llu%s\n",
		   prefix, stat, region, sz, units[u]);
}
EXPORT_SYMBOL(drm_fdinfo_print_size);

int drm_memory_stats_is_zero(const struct drm_memory_stats *stats)
{
	return (stats->shared == 0 &&
		stats->private == 0 &&
		stats->resident == 0 &&
		stats->purgeable == 0 &&
		stats->active == 0);
}
EXPORT_SYMBOL(drm_memory_stats_is_zero);

/*
 * drm_print_memory_stats - 打印内存统计信息的辅助函数
 * @p: 输出打印机
 * @stats: 已收集的内存统计信息
 * @supported_status: 可用的可选统计信息位掩码
 * @region: 内存区域名称
 *
 * 输出包括 total（总大小）、shared（共享大小）以及可选的
 * active（活跃）、resident（驻留）和 purgeable（可回收）统计。
 */
/**
 * drm_print_memory_stats - A helper to print memory stats
 * @p: The printer to print output to
 * @stats: The collected memory stats
 * @supported_status: Bitmask of optional stats which are available
 * @region: The memory region
 *
 */
void drm_print_memory_stats(struct drm_printer *p,
			    const struct drm_memory_stats *stats,
			    enum drm_gem_object_status supported_status,
			    const char *region)
{
	const char *prefix = "drm";

	drm_fdinfo_print_size(p, prefix, "total", region,
			      stats->private + stats->shared);
	drm_fdinfo_print_size(p, prefix, "shared", region, stats->shared);

	if (supported_status & DRM_GEM_OBJECT_ACTIVE)
		drm_fdinfo_print_size(p, prefix, "active", region, stats->active);

	if (supported_status & DRM_GEM_OBJECT_RESIDENT)
		drm_fdinfo_print_size(p, prefix, "resident", region,
				      stats->resident);

	if (supported_status & DRM_GEM_OBJECT_PURGEABLE)
		drm_fdinfo_print_size(p, prefix, "purgeable", region,
				      stats->purgeable);
}
EXPORT_SYMBOL(drm_print_memory_stats);

/*
 * drm_show_memory_stats - 收集并显示标准 fdinfo 内存统计信息的辅助函数
 * @p: 输出打印机
 * @file: DRM 文件
 *
 * 遍历指定文件中已分配句柄的所有 GEM 对象，收集其内存使用统计
 * （包括共用、私有、活跃、驻留、可回收大小），并使用
 * drm_print_memory_stats() 输出。
 */
/**
 * drm_show_memory_stats - Helper to collect and show standard fdinfo memory stats
 * @p: the printer to print output to
 * @file: the DRM file
 *
 * Helper to iterate over GEM objects with a handle allocated in the specified
 * file.
 */
void drm_show_memory_stats(struct drm_printer *p, struct drm_file *file)
{
	struct drm_gem_object *obj;
	struct drm_memory_stats status = {};
	enum drm_gem_object_status supported_status = 0;
	int id;

	spin_lock(&file->table_lock);
	idr_for_each_entry (&file->object_idr, obj, id) {
		enum drm_gem_object_status s = 0;
		size_t add_size = (obj->funcs && obj->funcs->rss) ?
			obj->funcs->rss(obj) : obj->size;

		if (obj->funcs && obj->funcs->status) {
			s = obj->funcs->status(obj);
			supported_status |= s;
		}

		if (drm_gem_object_is_shared_for_memory_stats(obj))
			status.shared += obj->size;
		else
			status.private += obj->size;

		if (s & DRM_GEM_OBJECT_RESIDENT) {
			status.resident += add_size;
		} else {
			/* If already purged or not yet backed by pages, don't
			 * count it as purgeable:
			 */
			s &= ~DRM_GEM_OBJECT_PURGEABLE;
		}

		if (!dma_resv_test_signaled(obj->resv, dma_resv_usage_rw(true))) {
			status.active += add_size;
			supported_status |= DRM_GEM_OBJECT_ACTIVE;

			/* If still active, don't count as purgeable: */
			s &= ~DRM_GEM_OBJECT_PURGEABLE;
		}

		if (s & DRM_GEM_OBJECT_PURGEABLE)
			status.purgeable += add_size;
	}
	spin_unlock(&file->table_lock);

	drm_print_memory_stats(p, &status, supported_status, "memory");
}
EXPORT_SYMBOL(drm_show_memory_stats);

/*
 * drm_show_fdinfo - DRM 文件 fdinfo 的辅助函数
 * @m: 输出流（seq_file）
 * @f: 设备文件实例
 *
 * 实现 fdinfo 接口，供用户空间查询进程使用 GPU 的统计信息。
 * 输出内容包括驱动名称、客户端 ID、PCI 设备地址、客户端名称
 * 以及驱动特定信息（通过 show_fdinfo 回调）。
 *
 * 输出格式说明请参见 Documentation/gpu/drm-usage-stats.rst
 * 另请参阅 &drm_driver.show_fdinfo。
 */
/**
 * drm_show_fdinfo - helper for drm file fops
 * @m: output stream
 * @f: the device file instance
 *
 * Helper to implement fdinfo, for userspace to query usage stats, etc, of a
 * process using the GPU.  See also &drm_driver.show_fdinfo.
 *
 * For text output format description please see Documentation/gpu/drm-usage-stats.rst
 */
void drm_show_fdinfo(struct seq_file *m, struct file *f)
{
	struct drm_file *file = f->private_data;
	struct drm_device *dev = file->minor->dev;
	struct drm_printer p = drm_seq_file_printer(m);
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	drm_printf(&p, "drm-driver:\t%s\n", dev->driver->name);
	drm_printf(&p, "drm-client-id:\t%llu\n", file->client_id);

	if (dev_is_pci(dev->dev)) {
		struct pci_dev *pdev = to_pci_dev(dev->dev);

		drm_printf(&p, "drm-pdev:\t%04x:%02x:%02x.%d\n",
			   pci_domain_nr(pdev->bus), pdev->bus->number,
			   PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
	}

	mutex_lock(&file->client_name_lock);
	if (file->client_name)
		drm_printf(&p, "drm-client-name:\t%s\n", file->client_name);
	mutex_unlock(&file->client_name_lock);

	if (dev->driver->show_fdinfo)
		dev->driver->show_fdinfo(&p, file);

	drm_dev_exit(idx);
}
EXPORT_SYMBOL(drm_show_fdinfo);

/**
 * drm_file_err - 记录与 drm_file 关联的进程名、PID 和客户端名称的错误信息
 * @file_priv: 关联的 DRM 文件上下文
 * @fmt: printf() 风格的格式化字符串
 *
 * 辅助函数，用于在记录错误时同时输出与 DRM 文件关联的进程
 * 详细信息，包括进程名、PID 和客户端名称。有助于在调试时
 * 快速定位哪个用户空间进程导致了错误。
 */
void drm_file_err(struct drm_file *file_priv, const char *fmt, ...)
{
	va_list args;
	struct va_format vaf;
	struct pid *pid;
	struct task_struct *task;
	struct drm_device *dev = file_priv->minor->dev;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;

	mutex_lock(&file_priv->client_name_lock);
	rcu_read_lock();
	pid = rcu_dereference(file_priv->pid);
	task = pid_task(pid, PIDTYPE_TGID);

	drm_err(dev, "comm: %s pid: %d client-id:%llu client: %s ... %pV",
		task ? task->comm : "Unset",
		task ? task->pid : 0, file_priv->client_id,
		file_priv->client_name ?: "Unset", &vaf);

	va_end(args);
	rcu_read_unlock();
	mutex_unlock(&file_priv->client_name_lock);
}
EXPORT_SYMBOL(drm_file_err);

/*
 * mock_drm_getfile - 为 DRM 设备创建新的 struct file（用于测试）
 * @minor: 要包装的 DRM minor（如 drm_device.primary）
 * @flags: 文件创建模式（O_RDWR 等）
 *
 * 创建一个包装了 DRM 文件上下文的 struct file，模拟用户空间打开
 * /dev/dri/card0 的行为，但不涉及实际用户空间。可以使用其 f_op
 * （drm_device.driver.fops）操作该 struct file 来模拟用户空间操作，
* 或将其作为内部/匿名客户端提供给面向用户空间的函数。
 *
 * 此函数仅应用于测试目的，通过 EXPORT_SYMBOL_FOR_TESTS_ONLY 导出。
 *
 * 返回：指向新创建的 struct file 的指针，失败时返回 ERR_PTR。
 */
/**
 * mock_drm_getfile - Create a new struct file for the drm device
 * @minor: drm minor to wrap (e.g. #drm_device.primary)
 * @flags: file creation mode (O_RDWR etc)
 *
 * This create a new struct file that wraps a DRM file context around a
 * DRM minor. This mimicks userspace opening e.g. /dev/dri/card0, but without
 * invoking userspace. The struct file may be operated on using its f_op
 * (the drm_device.driver.fops) to mimick userspace operations, or be supplied
 * to userspace facing functions as an internal/anonymous client.
 *
 * RETURNS:
 * Pointer to newly created struct file, ERR_PTR on failure.
 */
struct file *mock_drm_getfile(struct drm_minor *minor, unsigned int flags)
{
	struct drm_device *dev = minor->dev;
	struct drm_file *priv;
	struct file *file;

	priv = drm_file_alloc(minor);
	if (IS_ERR(priv))
		return ERR_CAST(priv);

	file = anon_inode_getfile("drm", dev->driver->fops, priv, flags);
	if (IS_ERR(file)) {
		drm_file_free(priv);
		return file;
	}

	/* Everyone shares a single global address space */
	file->f_mapping = dev->anon_inode->i_mapping;

	drm_dev_get(dev);
	priv->filp = file;

	return file;
}
EXPORT_SYMBOL_FOR_TESTS_ONLY(mock_drm_getfile);
