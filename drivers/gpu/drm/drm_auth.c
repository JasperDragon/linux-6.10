/*
 * Created: Tue Feb  2 08:37:54 1999 by faith@valinux.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Author Rickard E. (Rik) Faith <faith@valinux.com>
 * Author Gareth Hughes <gareth@valinux.com>
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
 * DRM 主控与认证管理 - 管理 DRM 设备的主控（Master）权限和客户端认证
 *
 * 本文件实现了 DRM 设备的主控权管理和客户端认证机制。DRM 主控（Master）
 * 用于跟踪打开主设备节点的客户端组，控制对显示硬件的独占访问权限。
 *
 * 核心概念：
 *   - Master：拥有显示硬件控制权的客户端，通过 SET_MASTER/DROP_MASTER
 *     IOCTL 或隐式地通过打开/关闭主设备节点进行切换
 *   - 认证（Authentication）：通过 GETMAGIC/AUTHMAGIC IOCTL 实现
 *     客户端之间的相互信任，认证后的客户端可以访问受控资源
 *   - Magic：每个 DRM 文件上下文关联的整型标识符，用于认证过程
 *
 * 关键函数包括主控权的获取/释放、客户端认证、主控权委派的权限检查，
 * 以及内核内部使用的辅助函数。
 */

#include <linux/export.h>
#include <linux/slab.h>

#include <drm/drm_auth.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_lease.h>
#include <drm/drm_print.h>

#include "drm_internal.h"

/**
 * DOC: master and authentication
 *
 * &struct drm_master is used to track groups of clients with open
 * primary device nodes. For every &struct drm_file which has had at
 * least once successfully became the device master (either through the
 * SET_MASTER IOCTL, or implicitly through opening the primary device node when
 * no one else is the current master that time) there exists one &drm_master.
 * This is noted in &drm_file.is_master. All other clients have just a pointer
 * to the &drm_master they are associated with.
 *
 * In addition only one &drm_master can be the current master for a &drm_device.
 * It can be switched through the DROP_MASTER and SET_MASTER IOCTL, or
 * implicitly through closing/opening the primary device node. See also
 * drm_is_current_master().
 *
 * Clients can authenticate against the current master (if it matches their own)
 * using the GETMAGIC and AUTHMAGIC IOCTLs. Together with exchanging masters,
 * this allows controlled access to the device for an entire group of mutually
 * trusted clients.
 */

static bool drm_is_current_master_locked(struct drm_file *fpriv)
{
	lockdep_assert_once(lockdep_is_held(&fpriv->master_lookup_lock) ||
			    lockdep_is_held(&fpriv->minor->dev->master_mutex));

	return fpriv->is_master && drm_lease_owner(fpriv->master) == fpriv->minor->dev->master;
}

/**
 * drm_is_current_master - 检查 @fpriv 是否为当前设备主控
 * @fpriv: DRM 文件私有数据
 *
 * 检查 @fpriv 是否是其设备的当前主控。这决定了客户端是否有权
 * 执行需要 DRM_MASTER 权限的 IOCTL 操作。
 *
 * 大多数需要 DRM_MASTER 的现代 IOCTL 与内核模式设置（KMS）相关 -
 * 当前主控被假定拥有不可共享的显示硬件资源。
 */
bool drm_is_current_master(struct drm_file *fpriv)
{
	bool ret;

	spin_lock(&fpriv->master_lookup_lock);
	ret = drm_is_current_master_locked(fpriv);
	spin_unlock(&fpriv->master_lookup_lock);

	return ret;
}
EXPORT_SYMBOL(drm_is_current_master);

int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_auth *auth = data;
	int ret = 0;

	guard(mutex)(&dev->master_mutex);
	if (!file_priv->magic) {
		ret = idr_alloc(&file_priv->master->magic_map, file_priv,
				1, 0, GFP_KERNEL);
		if (ret >= 0)
			file_priv->magic = ret;
	}
	auth->magic = file_priv->magic;

	drm_dbg_core(dev, "%u\n", auth->magic);

	return ret < 0 ? ret : 0;
}

int drm_authmagic(struct drm_device *dev, void *data,
		  struct drm_file *file_priv)
{
	struct drm_auth *auth = data;
	struct drm_file *file;

	drm_dbg_core(dev, "%u\n", auth->magic);

	guard(mutex)(&dev->master_mutex);
	file = idr_find(&file_priv->master->magic_map, auth->magic);
	if (file) {
		file->authenticated = 1;
		idr_replace(&file_priv->master->magic_map, NULL, auth->magic);
	}

	return file ? 0 : -EINVAL;
}

struct drm_master *drm_master_create(struct drm_device *dev)
{
	struct drm_master *master;

	master = kzalloc_obj(*master);
	if (!master)
		return NULL;

	kref_init(&master->refcount);
	idr_init_base(&master->magic_map, 1);
	master->dev = dev;

	/* initialize the tree of output resource lessees */
	INIT_LIST_HEAD(&master->lessees);
	INIT_LIST_HEAD(&master->lessee_list);
	idr_init(&master->leases);
	idr_init_base(&master->lessee_idr, 1);

	return master;
}

static void drm_set_master(struct drm_device *dev, struct drm_file *fpriv,
			   bool new_master)
{
	dev->master = drm_master_get(fpriv->master);
	if (dev->driver->master_set)
		dev->driver->master_set(dev, fpriv, new_master);

	fpriv->was_master = true;
}

static int drm_new_set_master(struct drm_device *dev, struct drm_file *fpriv)
{
	struct drm_master *old_master;
	struct drm_master *new_master;

	lockdep_assert_held_once(&dev->master_mutex);

	WARN_ON(fpriv->is_master);
	old_master = fpriv->master;
	new_master = drm_master_create(dev);
	if (!new_master)
		return -ENOMEM;
	spin_lock(&fpriv->master_lookup_lock);
	fpriv->master = new_master;
	spin_unlock(&fpriv->master_lookup_lock);

	fpriv->is_master = 1;
	fpriv->authenticated = 1;

	drm_set_master(dev, fpriv, true);

	if (old_master)
		drm_master_put(&old_master);

	return 0;
}

/*
 * In the olden days the SET/DROP_MASTER ioctls used to return EACCES when
 * CAP_SYS_ADMIN was not set. This was used to prevent rogue applications
 * from becoming master and/or failing to release it.
 *
 * At the same time, the first client (for a given VT) is _always_ master.
 * Thus in order for the ioctls to succeed, one had to _explicitly_ run the
 * application as root or flip the setuid bit.
 *
 * If the CAP_SYS_ADMIN was missing, no other client could become master...
 * EVER :-( Leading to a) the graphics session dying badly or b) a completely
 * locked session.
 *
 *
 * As some point systemd-logind was introduced to orchestrate and delegate
 * master as applicable. It does so by opening the fd and passing it to users
 * while in itself logind a) does the set/drop master per users' request and
 * b)  * implicitly drops master on VT switch.
 *
 * Even though logind looks like the future, there are a few issues:
 *  - some platforms don't have equivalent (Android, CrOS, some BSDs) so
 * root is required _solely_ for SET/DROP MASTER.
 *  - applications may not be updated to use it,
 *  - any client which fails to drop master* can DoS the application using
 * logind, to a varying degree.
 *
 * * Either due missing CAP_SYS_ADMIN or simply not calling DROP_MASTER.
 *
 *
 * Here we implement the next best thing:
 *  - ensure the logind style of fd passing works unchanged, and
 *  - allow a client to drop/set master, iff it is/was master at a given point
 * in time.
 *
 * Note: DROP_MASTER cannot be free for all, as an arbitrator user could:
 *  - DoS/crash the arbitrator - details would be implementation specific
 *  - open the node, become master implicitly and cause issues
 *
 * As a result this fixes the following when using root-less build w/o logind
 * - startx
 * - weston
 * - various compositors based on wlroots
 */
static int
drm_master_check_perm(struct drm_device *dev, struct drm_file *file_priv)
{
	if (file_priv->was_master &&
	    rcu_access_pointer(file_priv->pid) == task_tgid(current))
		return 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	return 0;
}

int drm_setmaster_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	int ret;

	guard(mutex)(&dev->master_mutex);

	ret = drm_master_check_perm(dev, file_priv);
	if (ret)
		return ret;

	if (drm_is_current_master_locked(file_priv))
		return ret;

	if (dev->master)
		return -EBUSY;

	if (!file_priv->master)
		return -EINVAL;

	if (!file_priv->is_master)
		return drm_new_set_master(dev, file_priv);

	if (file_priv->master->lessor != NULL) {
		drm_dbg_lease(dev,
			      "Attempt to set lessee %d as master\n",
			      file_priv->master->lessee_id);
		return -EINVAL;
	}

	drm_set_master(dev, file_priv, false);

	return ret;
}

static void drm_drop_master(struct drm_device *dev,
			    struct drm_file *fpriv)
{
	if (dev->driver->master_drop)
		dev->driver->master_drop(dev, fpriv);
	drm_master_put(&dev->master);
}

int drm_dropmaster_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	int ret;

	guard(mutex)(&dev->master_mutex);

	ret = drm_master_check_perm(dev, file_priv);
	if (ret)
		return ret;

	if (!drm_is_current_master_locked(file_priv))
		return -EINVAL;

	if (!dev->master)
		return -EINVAL;

	if (file_priv->master->lessor != NULL) {
		drm_dbg_lease(dev,
			      "Attempt to drop lessee %d as master\n",
			      file_priv->master->lessee_id);
		return -EINVAL;
	}

	drm_drop_master(dev, file_priv);

	return ret;
}

int drm_master_open(struct drm_file *file_priv)
{
	struct drm_device *dev = file_priv->minor->dev;
	int ret = 0;

	/* if there is no current master make this fd it, but do not create
	 * any master object for render clients
	 */
	guard(mutex)(&dev->master_mutex);
	if (!dev->master) {
		ret = drm_new_set_master(dev, file_priv);
	} else {
		spin_lock(&file_priv->master_lookup_lock);
		file_priv->master = drm_master_get(dev->master);
		spin_unlock(&file_priv->master_lookup_lock);
	}

	return ret;
}

void drm_master_release(struct drm_file *file_priv)
{
	struct drm_device *dev = file_priv->minor->dev;
	struct drm_master *master;

	guard(mutex)(&dev->master_mutex);
	master = file_priv->master;
	if (file_priv->magic)
		idr_remove(&file_priv->master->magic_map, file_priv->magic);

	if (!drm_is_current_master_locked(file_priv))
		goto out;

	if (dev->master == file_priv->master)
		drm_drop_master(dev, file_priv);
out:
	if (drm_core_check_feature(dev, DRIVER_MODESET) && file_priv->is_master) {
		/* Revoke any leases held by this or lessees, but only if
		 * this is the "real" master
		 */
		drm_lease_revoke(master);
	}

	/* drop the master reference held by the file priv */
	if (file_priv->master)
		drm_master_put(&file_priv->master);
}

/**
 * drm_master_get - 引用一个 master 指针
 * @master: &struct drm_master
 *
 * 递增 @master 的引用计数并返回指向 @master 的指针。
 */
struct drm_master *drm_master_get(struct drm_master *master)
{
	kref_get(&master->refcount);
	return master;
}
EXPORT_SYMBOL(drm_master_get);

/**
 * drm_file_get_master - 引用 @file_priv 的 &drm_file.master
 * @file_priv: DRM 文件私有数据
 *
 * 递增 @file_priv 的 &drm_file.master 引用计数并返回该 master。
 * 如果 @file_priv 没有 &drm_file.master，则返回 NULL。
 *
 * 从此函数返回的 master 指针应使用 drm_master_put() 解除引用。
 */
struct drm_master *drm_file_get_master(struct drm_file *file_priv)
{
	struct drm_master *master = NULL;

	spin_lock(&file_priv->master_lookup_lock);
	if (!file_priv->master)
		goto unlock;
	master = drm_master_get(file_priv->master);

unlock:
	spin_unlock(&file_priv->master_lookup_lock);
	return master;
}
EXPORT_SYMBOL(drm_file_get_master);

static void drm_master_destroy(struct kref *kref)
{
	struct drm_master *master = container_of(kref, struct drm_master, refcount);
	struct drm_device *dev = master->dev;

	if (drm_core_check_feature(dev, DRIVER_MODESET))
		drm_lease_destroy(master);

	idr_destroy(&master->magic_map);
	idr_destroy(&master->leases);
	idr_destroy(&master->lessee_idr);

	kfree(master->unique);
	kfree(master);
}

/**
 * drm_master_put - 解除引用并清除 master 指针
 * @master: 指向 &struct drm_master 指针的指针
 *
 * 递减 @master 所指向的 &drm_master 的引用计数，并将其设置为 NULL。
 */
void drm_master_put(struct drm_master **master)
{
	kref_put(&(*master)->refcount, drm_master_destroy);
	*master = NULL;
}
EXPORT_SYMBOL(drm_master_put);

/*
 * drm_master_internal_acquire - 获取设备主控权的内部锁（供 drm_client/fb_helper 使用）
 * @dev: DRM 设备
 *
 * 尝试获取 master_mutex 并检查是否没有其他主控持有设备。
 * 如果当前没有主控，则持有锁并返回 true，调用者可以安全地操作显示硬件。
 * 如果有主控，则释放锁并返回 false。
 *
 * 此函数仅供 drm_client 和 drm_fb_helper 内部使用。
 *
 * 返回：没有主控时返回 true（持有锁），有主控时返回 false。
 */
bool drm_master_internal_acquire(struct drm_device *dev)
{
	mutex_lock(&dev->master_mutex);
	if (dev->master) {
		mutex_unlock(&dev->master_mutex);
		return false;
	}

	return true;
}
EXPORT_SYMBOL(drm_master_internal_acquire);

/*
 * drm_master_internal_release - 释放设备主控权的内部锁（供 drm_client/fb_helper 使用）
 * @dev: DRM 设备
 *
 * 释放由 drm_master_internal_acquire() 获取的 master_mutex。
 * 此函数仅供 drm_client 和 drm_fb_helper 内部使用。
 */
void drm_master_internal_release(struct drm_device *dev)
{
	mutex_unlock(&dev->master_mutex);
}
EXPORT_SYMBOL(drm_master_internal_release);
