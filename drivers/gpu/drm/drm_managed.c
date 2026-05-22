// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Intel
 *
 * Based on drivers/base/devres.c
 */

/*
 * DRM 托管资源管理 - 为 DRM 设备提供生命周期管理的内存和资源分配机制
 *
 * 本文件实现了 drm_device 级别的托管资源分配与释放框架，灵感来源于
 * Linux 内核的 devres 框架。与 devres 绑定于物理设备生命周期不同，
 * DRM 托管资源绑定于 drm_device 的生命周期，当用户空间仍持有打开的文件
 * 描述符时，drm_device 可能比底层物理设备存活更久。
 *
 * 核心功能包括：
 *   - drmm_add_action() / drmm_add_action_or_reset() - 添加资源释放回调
 *   - drmm_kmalloc() / drmm_kfree() - 托管内存分配与释放
 *   - drmm_kstrdup() - 托管字符串复制
 *   - drmm_release_action() - 提前释放指定的托管资源
 *   - drm_managed_release() - 在设备销毁时释放所有托管资源
 *
 * 所有函数都是完全并发安全的，支持在驱动生命周期的任意阶段添加和移除
 * 托管资源。建议仅对生命周期中很少变化的资源使用托管机制。
 */

#include <drm/drm_managed.h>

#include <linux/export.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <drm/drm_device.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

/**
 * DOC: managed resources
 *
 * Inspired by struct &device managed resources, but tied to the lifetime of
 * struct &drm_device, which can outlive the underlying physical device, usually
 * when userspace has some open files and other handles to resources still open.
 *
 * Release actions can be added with drmm_add_action(), memory allocations can
 * be done directly with drmm_kmalloc() and the related functions. Everything
 * will be released on the final drm_dev_put() in reverse order of how the
 * release actions have been added and memory has been allocated since driver
 * loading started with devm_drm_dev_alloc().
 *
 * Note that release actions and managed memory can also be added and removed
 * during the lifetime of the driver, all the functions are fully concurrent
 * safe. But it is recommended to use managed resources only for resources that
 * change rarely, if ever, during the lifetime of the &drm_device instance.
 */

struct drmres_node {
	struct list_head	entry;
	drmres_release_t	release;
	const char		*name;
	size_t			size;
};

struct drmres {
	struct drmres_node		node;
	/*
	 * Some archs want to perform DMA into kmalloc caches
	 * and need a guaranteed alignment larger than
	 * the alignment of a 64-bit integer.
	 * Thus we use ARCH_DMA_MINALIGN for data[] which will force the same
	 * alignment for struct drmres when allocated by kmalloc().
	 */
	u8 __aligned(ARCH_DMA_MINALIGN) data[];
};

static void free_dr(struct drmres *dr)
{
	kfree_const(dr->node.name);
	kfree(dr);
}

/*
 * drm_managed_release - 释放设备所有的托管资源
 * @dev: DRM 设备
 *
 * 在 drm_device 销毁的最后阶段调用此函数，遍历所有托管资源并以
 * 添加顺序的逆序依次释放。这是 DRM 托管资源框架的最终清理入口。
 * 通常在 drm_dev_put() 引用计数归零时被调用。
 */
void drm_managed_release(struct drm_device *dev)
{
	struct drmres *dr, *tmp;

	drm_dbg_drmres(dev, "drmres release begin\n");
	list_for_each_entry_safe(dr, tmp, &dev->managed.resources, node.entry) {
		drm_dbg_drmres(dev, "REL %p %s (%zu bytes)\n",
			       dr, dr->node.name, dr->node.size);

		if (dr->node.release)
			dr->node.release(dev, dr->node.size ? *(void **)&dr->data : NULL);

		list_del(&dr->node.entry);
		free_dr(dr);
	}
	drm_dbg_drmres(dev, "drmres release end\n");
}

/*
 * Always inline so that kmalloc_track_caller tracks the actual interesting
 * caller outside of drm_managed.c.
 */
static __always_inline struct drmres * alloc_dr(drmres_release_t release,
						size_t size, gfp_t gfp, int nid)
{
	size_t tot_size;
	struct drmres *dr;

	/* We must catch any near-SIZE_MAX cases that could overflow. */
	if (unlikely(check_add_overflow(sizeof(*dr), size, &tot_size)))
		return NULL;

	dr = kmalloc_node_track_caller(tot_size, gfp, nid);
	if (unlikely(!dr))
		return NULL;

	memset(dr, 0, offsetof(struct drmres, data));

	INIT_LIST_HEAD(&dr->node.entry);
	dr->node.release = release;
	dr->node.size = size;

	return dr;
}

static void del_dr(struct drm_device *dev, struct drmres *dr)
{
	list_del_init(&dr->node.entry);

	drm_dbg_drmres(dev, "DEL %p %s (%lu bytes)\n",
		       dr, dr->node.name, (unsigned long) dr->node.size);
}

static void add_dr(struct drm_device *dev, struct drmres *dr)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->managed.lock, flags);
	list_add(&dr->node.entry, &dev->managed.resources);
	spin_unlock_irqrestore(&dev->managed.lock, flags);

	drm_dbg_drmres(dev, "ADD %p %s (%lu bytes)\n",
		       dr, dr->node.name, (unsigned long) dr->node.size);
}

void drmm_add_final_kfree(struct drm_device *dev, void *container)
{
	WARN_ON(dev->managed.final_kfree);
	WARN_ON(dev < (struct drm_device *) container);
	WARN_ON(dev + 1 > (struct drm_device *) (container + ksize(container)));
	dev->managed.final_kfree = container;
}

/*
 * __drmm_add_action - 注册一个 DRM 设备托管资源释放回调
 * @dev: DRM 设备
 * @action: 释放回调函数，在设备销毁时调用
 * @data: 传递给回调函数的不透明指针
 * @name: 资源的调试名称
 *
 * 注册一个在 drm_device 销毁时自动调用的释放回调。回调将以
 * 注册顺序的逆序执行。与 devm 框架不同，DRM 托管资源绑定于
 * drm_device 的生命周期，而非物理设备的生命周期。
 *
 * 返回：0 表示成功，负错误码表示失败。
 */
int __drmm_add_action(struct drm_device *dev,
		      drmres_release_t action,
{
	struct drmres *dr;
	void **void_ptr;

	dr = alloc_dr(action, data ? sizeof(void*) : 0,
		      GFP_KERNEL | __GFP_ZERO,
		      dev_to_node(dev->dev));
	if (!dr) {
		drm_dbg_drmres(dev, "failed to add action %s for %p\n",
			       name, data);
		return -ENOMEM;
	}

	dr->node.name = kstrdup_const(name, GFP_KERNEL);
	if (data) {
		void_ptr = (void **)&dr->data;
		*void_ptr = data;
	}

	add_dr(dev, dr);

	return 0;
}
EXPORT_SYMBOL(__drmm_add_action);

/*
 * __drmm_add_action_or_reset - 注册托管资源释放回调（失败时自动清理）
 * @dev: DRM 设备
 * @action: 释放回调函数
 * @data: 传递给回调函数的数据指针
 * @name: 资源的调试名称
 *
 * 与 __drmm_add_action() 功能相同，但在分配失败时会立即调用 action
 * 回调执行清理，确保不会发生资源泄漏。适用于在初始化过程中注册
 * 托管资源，且注册失败时需要立即清理已分配资源的场景。
 *
 * 返回：0 表示成功，负错误码表示失败（此时 action 已被调用）。
 */
int __drmm_add_action_or_reset(struct drm_device *dev,
			       drmres_release_t action,
			       void *data, const char *name)
{
	int ret;

	ret = __drmm_add_action(dev, action, data, name);
	if (ret)
		action(dev, data);

	return ret;
}
EXPORT_SYMBOL(__drmm_add_action_or_reset);

/*
 * drmm_release_action - 提前释放指定的托管资源回调
 * @dev: DRM 设备
 * @action: 要释放的回调函数
 * @data: 传递给回调函数的数据指针
 *
 * 立即调用之前通过 drmm_add_action() 注册的 action 回调，
 * 并将其从设备的清理回调列表中移除，确保在最终的 drm_dev_put()
 * 中不会再次调用该回调。适用于在设备生命周期结束前提前释放
 * 某些托管资源的场景。
 */
/**
 * drmm_release_action - release a managed action from a &drm_device
 * @dev: DRM device
 * @action: function which would be called when @dev is released
 * @data: opaque pointer, passed to @action
 *
 * This function calls the @action previously added by drmm_add_action()
 * immediately.
 * The @action is removed from the list of cleanup actions for @dev,
 * which means that it won't be called in the final drm_dev_put().
 */
void drmm_release_action(struct drm_device *dev,
			 drmres_release_t action,
			 void *data)
{
	struct drmres *dr_match = NULL, *dr;
	unsigned long flags;

	spin_lock_irqsave(&dev->managed.lock, flags);
	list_for_each_entry_reverse(dr, &dev->managed.resources, node.entry) {
		if (dr->node.release == action) {
			if (!data || *(void **)dr->data == data) {
				dr_match = dr;
				del_dr(dev, dr_match);
				break;
			}
		}
	}
	spin_unlock_irqrestore(&dev->managed.lock, flags);

	if (WARN_ON(!dr_match))
		return;

	action(dev, data);

	free_dr(dr_match);
}
EXPORT_SYMBOL(drmm_release_action);

/*
 * drmm_kmalloc - DRM 设备托管的 kmalloc()
 * @dev: DRM 设备
 * @size: 内存分配大小
 * @gfp: GFP 分配标志
 *
 * 这是 kmalloc() 的 DRM 设备托管版本。分配的内存将在最终的
 * drm_dev_put() 调用时自动释放。也可以通过 drmm_kfree() 提前释放。
 *
 * 返回：指向已分配内存的指针，失败时返回 NULL。
 */
/**
 * drmm_kmalloc - &drm_device managed kmalloc()
 * @dev: DRM device
 * @size: size of the memory allocation
 * @gfp: GFP allocation flags
 *
 * This is a &drm_device managed version of kmalloc(). The allocated memory is
 * automatically freed on the final drm_dev_put(). Memory can also be freed
 * before the final drm_dev_put() by calling drmm_kfree().
 */
void *drmm_kmalloc(struct drm_device *dev, size_t size, gfp_t gfp)
{
	struct drmres *dr;

	dr = alloc_dr(NULL, size, gfp, dev_to_node(dev->dev));
	if (!dr) {
		drm_dbg_drmres(dev, "failed to allocate %zu bytes, %u flags\n",
			       size, gfp);
		return NULL;
	}
	dr->node.name = kstrdup_const("kmalloc", gfp);

	add_dr(dev, dr);

	return dr->data;
}
EXPORT_SYMBOL(drmm_kmalloc);

/*
 * drmm_kstrdup - DRM 设备托管的 kstrdup()
 * @dev: DRM 设备
 * @s: 要复制的以 NULL 结尾的字符串
 * @gfp: GFP 分配标志
 *
 * 这是 kstrdup() 的 DRM 设备托管版本。其行为与 drmm_kmalloc()
 * 分配的内存完全一致，在最终的 drm_dev_put() 时自动释放。
 *
 * 返回：指向复制后字符串的指针，失败或输入为 NULL 时返回 NULL。
 */
/**
 * drmm_kstrdup - &drm_device managed kstrdup()
 * @dev: DRM device
 * @s: 0-terminated string to be duplicated
 * @gfp: GFP allocation flags
 *
 * This is a &drm_device managed version of kstrdup(). The allocated memory is
 * automatically freed on the final drm_dev_put() and works exactly like a
 * memory allocation obtained by drmm_kmalloc().
 */
char *drmm_kstrdup(struct drm_device *dev, const char *s, gfp_t gfp)
{
	size_t size;
	char *buf;

	if (!s)
		return NULL;

	size = strlen(s) + 1;
	buf = drmm_kmalloc(dev, size, gfp);
	if (buf)
		memcpy(buf, s, size);
	return buf;
}
EXPORT_SYMBOL_GPL(drmm_kstrdup);

/*
 * drmm_kfree - DRM 设备托管的 kfree()
 * @dev: DRM 设备
 * @data: 要释放的内存指针
 *
 * 这是 kfree() 的 DRM 设备托管版本，用于在最终的 drm_dev_put()
 * 之前提前释放通过 drmm_kmalloc() 或相关函数分配的内存。
 * 如果 data 为 NULL，则此函数无操作。
 */
/**
 * drmm_kfree - &drm_device managed kfree()
 * @dev: DRM device
 * @data: memory allocation to be freed
 *
 * This is a &drm_device managed version of kfree() which can be used to
 * release memory allocated through drmm_kmalloc() or any of its related
 * functions before the final drm_dev_put() of @dev.
 */
void drmm_kfree(struct drm_device *dev, void *data)
{
	struct drmres *dr_match = NULL, *dr;
	unsigned long flags;

	if (!data)
		return;

	spin_lock_irqsave(&dev->managed.lock, flags);
	list_for_each_entry(dr, &dev->managed.resources, node.entry) {
		if (dr->data == data) {
			dr_match = dr;
			del_dr(dev, dr_match);
			break;
		}
	}
	spin_unlock_irqrestore(&dev->managed.lock, flags);

	if (WARN_ON(!dr_match))
		return;

	free_dr(dr_match);
}
EXPORT_SYMBOL(drmm_kfree);

/*
 * __drmm_mutex_release - 托管互斥锁的释放回调
 * @dev: DRM 设备
 * @res: 指向 mutex 的指针
 *
 * 作为 drmm_add_action() 的回调函数使用，用于在设备销毁时
 * 自动销毁通过 drmm_add_action 注册的 mutex。
 * 对应的添加函数通常是 drmm_add_action(dev, __drmm_mutex_release, lock)。
 */
void __drmm_mutex_release(struct drm_device *dev, void *res)
{
	struct mutex *lock = res;

	mutex_destroy(lock);
}
EXPORT_SYMBOL(__drmm_mutex_release);

/*
 * __drmm_workqueue_release - 托管工作队列的释放回调
 * @dev: DRM 设备
 * @res: 指向 workqueue_struct 的指针
 *
 * 作为 drmm_add_action() 的回调函数使用，用于在设备销毁时
 * 自动销毁通过 drmm_add_action 注册的工作队列。
 * 对应的添加函数通常是 drmm_add_action(dev, __drmm_workqueue_release, wq)。
 */
void __drmm_workqueue_release(struct drm_device *device, void *res)
{
	struct workqueue_struct *wq = res;

	destroy_workqueue(wq);
}
EXPORT_SYMBOL(__drmm_workqueue_release);
