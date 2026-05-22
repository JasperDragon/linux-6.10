// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/*
 * 中文注释: DRM 页面映射工具 (Page Mapping Utilities)
 *
 * 本文件提供了 DRM 子系统的高级页面映射管理工具, 包括页面映射缓存
 * (pagemap cache) 和回收器 (shrinker), 以及设备互联组 (interconnect
 * group) 管理。
 *
 * 核心组件:
 *   1. drm_pagemap_cache: 单条目页面映射缓存。管理一个活跃 (引用计数 > 0)
 *      或非活跃 (引用计数 == 0) 的页面映射。非活跃页面映射可被回收器
 *      回收或重新激活使用。
 *   2. drm_pagemap_shrinker: 页面映射回收器。在内存压力下自动回收
 *      未使用的页面映射, 减少内存占用。通过 Linux 内核的 shrinker
 *      框架实现。
 *   3. drm_pagemap_owner / drm_pagemap_peer: 设备互联组管理。用于
 *      识别和共享具有快速互联 (fast interconnect) 的 peer 之间的
 *      共同页面映射所有者, 优化共享内存场景。
 *
 * 典型用法:
 *   1. 创建 shrinker: drm_pagemap_shrinker_create_devm()
 *   2. 创建 cache: drm_pagemap_cache_create_devm(shrinker)
 *   3. 查找缓存: drm_pagemap_lock_lookup() + drm_pagemap_get_from_cache()
 *   4. 设置缓存: drm_pagemap_cache_set_pagemap()
 *   5. 获取活跃: drm_pagemap_get_from_cache_if_active()
 */

#include <linux/slab.h>

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_pagemap.h>
#include <drm/drm_pagemap_util.h>
#include <drm/drm_print.h>

/**
 * struct drm_pagemap_cache - Lookup structure for pagemaps
 *
 * Structure to keep track of active (refcount > 1) and inactive
 * (refcount == 0) pagemaps. Inactive pagemaps can be made active
 * again by waiting for the @queued completion (indicating that the
 * pagemap has been put on the @shrinker's list of shrinkable
 * pagemaps, and then successfully removing it from @shrinker's
 * list. The latter may fail if the shrinker is already in the
 * process of freeing the pagemap. A struct drm_pagemap_cache can
 * hold a single struct drm_pagemap.
 */
struct drm_pagemap_cache {
	/** @lookup_mutex: Mutex making the lookup process atomic */
	struct mutex lookup_mutex;
	/** @lock: Lock protecting the @dpagemap pointer */
	spinlock_t lock;
	/** @shrinker: Pointer to the shrinker used for this cache. Immutable. */
	struct drm_pagemap_shrinker *shrinker;
	/** @dpagemap: Non-refcounted pointer to the drm_pagemap */
	struct drm_pagemap *dpagemap;
	/**
	 * @queued: Signals when an inactive drm_pagemap has been put on
	 * @shrinker's list.
	 */
	struct completion queued;
};

/**
 * struct drm_pagemap_shrinker - Shrinker to remove unused pagemaps
 */
struct drm_pagemap_shrinker {
	/** @drm: Pointer to the drm device. */
	struct drm_device *drm;
	/** @lock: Spinlock to protect the @dpagemaps list. */
	spinlock_t lock;
	/** @dpagemaps: List of unused dpagemaps. */
	struct list_head dpagemaps;
	/** @num_dpagemaps: Number of unused dpagemaps in @dpagemaps. */
	atomic_t num_dpagemaps;
	/** @shrink: Pointer to the struct shrinker. */
	struct shrinker *shrink;
};

static bool drm_pagemap_shrinker_cancel(struct drm_pagemap *dpagemap);

static void drm_pagemap_cache_fini(void *arg)
{
	struct drm_pagemap_cache *cache = arg;
	struct drm_pagemap *dpagemap;

	drm_dbg(cache->shrinker->drm, "Destroying dpagemap cache.\n");
	spin_lock(&cache->lock);
	dpagemap = cache->dpagemap;
	cache->dpagemap = NULL;
	if (dpagemap && !drm_pagemap_shrinker_cancel(dpagemap))
		dpagemap = NULL;
	spin_unlock(&cache->lock);

	if (dpagemap)
		drm_pagemap_destroy(dpagemap, false);

	mutex_destroy(&cache->lookup_mutex);
	kfree(cache);
}

/**
 * 中文注释: 创建设备托管的 drm_pagemap_cache
 * 分配并初始化一个设备托管的页面映射缓存。缓存在设备移除时自动销毁,
 * 届时所有非活跃的 drm_pagemap 也会被销毁。缓存持有 shrinker 指针,
 * 用于将非活跃页面映射加入回收列表。
 *
 * drm_pagemap_cache_create_devm() - Create a drm_pagemap_cache
 * @shrinker: Pointer to a struct drm_pagemap_shrinker.
 *
 * Create a device-managed drm_pagemap cache. The cache is automatically
 * destroyed on struct device removal, at which point any *inactive*
 * drm_pagemap's are destroyed.
 *
 * Return: Pointer to a struct drm_pagemap_cache on success. Error pointer
 * on failure.
 */
struct drm_pagemap_cache *drm_pagemap_cache_create_devm(struct drm_pagemap_shrinker *shrinker)
{
	struct drm_pagemap_cache *cache = kzalloc_obj(*cache);
	int err;

	if (!cache)
		return ERR_PTR(-ENOMEM);

	mutex_init(&cache->lookup_mutex);
	spin_lock_init(&cache->lock);
	cache->shrinker = shrinker;
	init_completion(&cache->queued);
	err = devm_add_action_or_reset(shrinker->drm->dev, drm_pagemap_cache_fini, cache);
	if (err)
		return ERR_PTR(err);

	return cache;
}
EXPORT_SYMBOL(drm_pagemap_cache_create_devm);

/**
 * DOC: Cache lookup
 *
 * Cache lookup should be done under a locked mutex, so that a
 * failed drm_pagemap_get_from_cache() and a following
 * drm_pagemap_cache_setpagemap() are carried out as an atomic
 * operation WRT other lookups. Otherwise, racing lookups may
 * unnecessarily concurrently create pagemaps to fulfill a
 * failed lookup. The API provides two functions to perform this lock,
 * drm_pagemap_lock_lookup() and drm_pagemap_unlock_lookup() and they
 * should be used in the following way:
 *
 * .. code-block:: c
 *
 *		drm_pagemap_lock_lookup(cache);
 *		dpagemap = drm_pagemap_get_from_cache(cache);
 *		if (dpagemap)
 *			goto out_unlock;
 *
 *		dpagemap = driver_create_new_dpagemap();
 *		if (!IS_ERR(dpagemap))
 *			drm_pagemap_cache_set_pagemap(cache, dpagemap);
 *
 *     out_unlock:
 *		drm_pagemap_unlock_lookup(cache);
 */

/**
 * 中文注释: 锁定 drm_pagemap_cache 以执行查找
 * 获取缓存的查找互斥锁。查找操作应在锁定状态下进行, 以确保"
 * 查找失败 -> 创建新 pagemap -> 设置到缓存"的原子性, 避免
 * 并发查找引起不必要的重复创建。
 *
 * drm_pagemap_cache_lock_lookup() - Lock a drm_pagemap_cache for lookup.
 * @cache: The drm_pagemap_cache to lock.
 *
 * Return: %-EINTR if interrupted while blocking. %0 otherwise.
 */
int drm_pagemap_cache_lock_lookup(struct drm_pagemap_cache *cache)
{
	return mutex_lock_interruptible(&cache->lookup_mutex);
}
EXPORT_SYMBOL(drm_pagemap_cache_lock_lookup);

/**
 * 中文注释: 解锁 drm_pagemap_cache (查找后)
 * 释放之前通过 drm_pagemap_cache_lock_lookup() 获取的互斥锁,
 * 允许其他查找操作继续进行。
 *
 * drm_pagemap_cache_unlock_lookup() - Unlock a drm_pagemap_cache after lookup.
 * @cache: The drm_pagemap_cache to unlock.
 */
void drm_pagemap_cache_unlock_lookup(struct drm_pagemap_cache *cache)
{
	mutex_unlock(&cache->lookup_mutex);
}
EXPORT_SYMBOL(drm_pagemap_cache_unlock_lookup);

/**
 * 中文注释: 从缓存中查找 drm_pagemap
 * 缓存的查找逻辑:
 *   1. 如果缓存中有活跃的 pagemap (引用计数 > 0), 直接返回 (增加引用)
 *   2. 如果有非活跃的 pagemap, 尝试从回收器列表中取消并重新初始化:
 *      a. 等待 "queued" completion (确保 pagemap 已加入回收列表)
 *      b. 从回收器列表中取消该 pagemap
 *      c. 重新初始化 (drm_pagemap_reinit)
 *      d. 设置回缓存
 *   3. 如果没有 pagemap 或重新激活失败, 返回 NULL 表示调用者应创建新
 *      的 pagemap 并插入缓存
 *
 * drm_pagemap_get_from_cache() - Lookup of drm_pagemaps.
 * @cache: The cache used for lookup.
 *
 * If an active pagemap is present in the cache, it is immediately returned.
 * If an inactive pagemap is present, it's removed from the shrinker list and
 * an attempt is made to make it active.
 * If no pagemap present or the attempt to make it active failed, %NULL is returned
 * to indicate to the caller to create a new drm_pagemap and insert it into
 * the cache.
 *
 * Return: A reference-counted pointer to a drm_pagemap if successful. An error
 * pointer if an error occurred, or %NULL if no drm_pagemap was found and
 * the caller should insert a new one.
 */
struct drm_pagemap *drm_pagemap_get_from_cache(struct drm_pagemap_cache *cache)
{
	struct drm_pagemap *dpagemap;
	int err;

	lockdep_assert_held(&cache->lookup_mutex);
retry:
	spin_lock(&cache->lock);
	dpagemap = cache->dpagemap;
	if (drm_pagemap_get_unless_zero(dpagemap)) {
		spin_unlock(&cache->lock);
		return dpagemap;
	}

	if (!dpagemap) {
		spin_unlock(&cache->lock);
		return NULL;
	}

	if (!try_wait_for_completion(&cache->queued)) {
		spin_unlock(&cache->lock);
		err = wait_for_completion_interruptible(&cache->queued);
		if (err)
			return ERR_PTR(err);
		goto retry;
	}

	if (drm_pagemap_shrinker_cancel(dpagemap)) {
		cache->dpagemap = NULL;
		spin_unlock(&cache->lock);
		err = drm_pagemap_reinit(dpagemap);
		if (err) {
			drm_pagemap_destroy(dpagemap, false);
			return ERR_PTR(err);
		}
		drm_pagemap_cache_set_pagemap(cache, dpagemap);
	} else {
		cache->dpagemap = NULL;
		spin_unlock(&cache->lock);
		dpagemap = NULL;
	}

	return dpagemap;
}
EXPORT_SYMBOL(drm_pagemap_get_from_cache);

/**
 * 中文注释: 将 drm_pagemap 分配给缓存
 * 在 drm_pagemap_get_from_cache() 返回 NULL 后调用此函数来填充
 * 缓存。使用 swap 安全地替换缓存中的 pagemap 指针, 并重设
 * completion 信号量以备后续等待。此函数必须在持有查找锁时调用。
 *
 * drm_pagemap_cache_set_pagemap() - Assign a drm_pagemap to a drm_pagemap_cache
 * @cache: The cache to assign the drm_pagemap to.
 * @dpagemap: The drm_pagemap to assign.
 *
 * The function must be called to populate a drm_pagemap_cache only
 * after a call to drm_pagemap_get_from_cache() returns NULL.
 */
void drm_pagemap_cache_set_pagemap(struct drm_pagemap_cache *cache, struct drm_pagemap *dpagemap)
{
	struct drm_device *drm = dpagemap->drm;

	lockdep_assert_held(&cache->lookup_mutex);
	spin_lock(&cache->lock);
	dpagemap->cache = cache;
	swap(cache->dpagemap, dpagemap);
	reinit_completion(&cache->queued);
	spin_unlock(&cache->lock);
	drm_WARN_ON(drm, !!dpagemap);
}
EXPORT_SYMBOL(drm_pagemap_cache_set_pagemap);

/**
 * 中文注释: 快速的活跃 drm_pagemap 查找
 * 非阻塞地检查缓存中是否存在活跃的 drm_pagemap (引用计数 > 0)。
 * 如果存在则增加引用计数并返回。此函数不需要持有查找锁, 适用于
 * 仅需在活跃状态下获取 pagemap 的快速路径场景。
 *
 * drm_pagemap_get_from_cache_if_active() - Quick lookup of active drm_pagemaps
 * @cache: The cache to lookup from.
 *
 * Function that should be used to lookup a drm_pagemap that is already active.
 * (refcount > 0).
 *
 * Return: A pointer to the cache's drm_pagemap if it's active; %NULL otherwise.
 */
struct drm_pagemap *drm_pagemap_get_from_cache_if_active(struct drm_pagemap_cache *cache)
{
	struct drm_pagemap *dpagemap;

	spin_lock(&cache->lock);
	dpagemap = drm_pagemap_get_unless_zero(cache->dpagemap);
	spin_unlock(&cache->lock);

	return dpagemap;
}
EXPORT_SYMBOL(drm_pagemap_get_from_cache_if_active);

static bool drm_pagemap_shrinker_cancel(struct drm_pagemap *dpagemap)
{
	struct drm_pagemap_cache *cache = dpagemap->cache;
	struct drm_pagemap_shrinker *shrinker = cache->shrinker;

	spin_lock(&shrinker->lock);
	if (list_empty(&dpagemap->shrink_link)) {
		spin_unlock(&shrinker->lock);
		return false;
	}

	list_del_init(&dpagemap->shrink_link);
	atomic_dec(&shrinker->num_dpagemaps);
	spin_unlock(&shrinker->lock);
	return true;
}

#ifdef CONFIG_PROVE_LOCKING
/**
 * 中文注释: lockdep 检测函数 - 用于 drm_pagemap_shrinker_add()
 * 在可能调用 drm_pagemap_shrinker_add() 的代码路径中调用此函数,
 * 可以提前检测锁依赖问题。在 PROVE_LOCKING 配置下, might_lock()
 * 会验证锁获取顺序是否可能导致死锁。
 *
 * drm_pagemap_shrinker_might_lock() - lockdep test for drm_pagemap_shrinker_add()
 * @dpagemap: The drm pagemap.
 *
 * The drm_pagemap_shrinker_add() function performs some locking.
 * This function can be called in code-paths that might
 * call drm_pagemap_shrinker_add() to detect any lockdep problems early.
 */
void drm_pagemap_shrinker_might_lock(struct drm_pagemap *dpagemap)
{
	int idx;

	if (drm_dev_enter(dpagemap->drm, &idx)) {
		struct drm_pagemap_cache *cache = dpagemap->cache;

		if (cache)
			might_lock(&cache->shrinker->lock);

		drm_dev_exit(idx);
	}
}
#endif

/**
 * 中文注释: 将 drm_pagemap 添加到回收器列表或直接销毁
 * 当页面映射不再活跃 (引用计数降为零) 时调用此函数。
 * 如果 pagemap 有关联的缓存且设备仍存在, 将其添加到回收器列表,
 * 以便在内存压力下被回收 (复用而非销毁)。
 * 如果设备已移除或没有关联缓存, 则直接销毁 pagemap。
 * 同时触发 completion 通知等待的 drm_pagemap_get_from_cache()。
 *
 * drm_pagemap_shrinker_add() - Add a drm_pagemap to the shrinker list or destroy
 * @dpagemap: The drm_pagemap.
 *
 * If @dpagemap is associated with a &struct drm_pagemap_cache AND the
 * struct device backing the drm device is still alive, add @dpagemap to
 * the &struct drm_pagemap_shrinker list of shrinkable drm_pagemaps.
 *
 * Otherwise destroy the pagemap directly using drm_pagemap_destroy().
 *
 * This is an internal function which is not intended to be exposed to drivers.
 */
void drm_pagemap_shrinker_add(struct drm_pagemap *dpagemap)
{
	struct drm_pagemap_cache *cache;
	struct drm_pagemap_shrinker *shrinker;
	int idx;

	/*
	 * The pagemap cache and shrinker are disabled at
	 * pci device remove time. After that, dpagemaps
	 * are freed directly.
	 */
	if (!drm_dev_enter(dpagemap->drm, &idx))
		goto out_no_cache;

	cache = dpagemap->cache;
	if (!cache) {
		drm_dev_exit(idx);
		goto out_no_cache;
	}

	shrinker = cache->shrinker;
	spin_lock(&shrinker->lock);
	list_add_tail(&dpagemap->shrink_link, &shrinker->dpagemaps);
	atomic_inc(&shrinker->num_dpagemaps);
	spin_unlock(&shrinker->lock);
	complete_all(&cache->queued);
	drm_dev_exit(idx);
	return;

out_no_cache:
	drm_pagemap_destroy(dpagemap, true);
}

static unsigned long
drm_pagemap_shrinker_count(struct shrinker *shrink, struct shrink_control *sc)
{
	struct drm_pagemap_shrinker *shrinker = shrink->private_data;
	unsigned long count = atomic_read(&shrinker->num_dpagemaps);

	return count ? : SHRINK_EMPTY;
}

static unsigned long
drm_pagemap_shrinker_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	struct drm_pagemap_shrinker *shrinker = shrink->private_data;
	struct drm_pagemap *dpagemap;
	struct drm_pagemap_cache *cache;
	unsigned long nr_freed = 0;

	sc->nr_scanned = 0;
	spin_lock(&shrinker->lock);
	do {
		dpagemap = list_first_entry_or_null(&shrinker->dpagemaps, typeof(*dpagemap),
						    shrink_link);
		if (!dpagemap)
			break;

		atomic_dec(&shrinker->num_dpagemaps);
		list_del_init(&dpagemap->shrink_link);
		spin_unlock(&shrinker->lock);

		sc->nr_scanned++;
		nr_freed++;

		cache = dpagemap->cache;
		spin_lock(&cache->lock);
		cache->dpagemap = NULL;
		spin_unlock(&cache->lock);

		drm_dbg(dpagemap->drm, "Shrinking dpagemap %p.\n", dpagemap);
		drm_pagemap_destroy(dpagemap, true);
		spin_lock(&shrinker->lock);
	} while (sc->nr_scanned < sc->nr_to_scan);
	spin_unlock(&shrinker->lock);

	return sc->nr_scanned ? nr_freed : SHRINK_STOP;
}

static void drm_pagemap_shrinker_fini(void *arg)
{
	struct drm_pagemap_shrinker *shrinker = arg;

	drm_dbg(shrinker->drm, "Destroying dpagemap shrinker.\n");
	drm_WARN_ON(shrinker->drm, !!atomic_read(&shrinker->num_dpagemaps));
	shrinker_free(shrinker->shrink);
	kfree(shrinker);
}

/**
 * 中文注释: 创建并注册页面映射回收器 (设备托管)
 * 分配并注册一个 Linux shrinker, 在系统内存压力下自动回收未使用的
 * drm_pagemap。回收器通过 drm_device 托管, 在设备移除时自动注销。
 * shrinker 的 count_objects 返回待回收的 pagemap 数量,
 * scan_objects 执行实际的回收操作 (销毁 pagemap 并释放内存)。
 *
 * drm_pagemap_shrinker_create_devm() - Create and register a pagemap shrinker
 * @drm: The drm device
 *
 * Create and register a pagemap shrinker that shrinks unused pagemaps
 * and thereby reduces memory footprint.
 * The shrinker is drm_device managed and unregisters itself when
 * the drm device is removed.
 *
 * Return: %0 on success, negative error code on failure.
 */
struct drm_pagemap_shrinker *drm_pagemap_shrinker_create_devm(struct drm_device *drm)
{
	struct drm_pagemap_shrinker *shrinker;
	struct shrinker *shrink;
	int err;

	shrinker = kzalloc_obj(*shrinker);
	if (!shrinker)
		return ERR_PTR(-ENOMEM);

	shrink = shrinker_alloc(0, "drm-drm_pagemap:%s", drm->unique);
	if (!shrink) {
		kfree(shrinker);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock_init(&shrinker->lock);
	INIT_LIST_HEAD(&shrinker->dpagemaps);
	shrinker->drm = drm;
	shrinker->shrink = shrink;
	shrink->count_objects = drm_pagemap_shrinker_count;
	shrink->scan_objects = drm_pagemap_shrinker_scan;
	shrink->private_data = shrinker;
	shrinker_register(shrink);

	err = devm_add_action_or_reset(drm->dev, drm_pagemap_shrinker_fini, shrinker);
	if (err)
		return ERR_PTR(err);

	return shrinker;
}
EXPORT_SYMBOL(drm_pagemap_shrinker_create_devm);

/**
 * struct drm_pagemap_owner - Device interconnect group
 * @kref: Reference count.
 *
 * A struct drm_pagemap_owner identifies a device interconnect group.
 */
struct drm_pagemap_owner {
	struct kref kref;
};

static void drm_pagemap_owner_release(struct kref *kref)
{
	kfree(container_of(kref, struct drm_pagemap_owner, kref));
}

/**
 * 中文注释: 停止参与设备互联组
 * 当页面映射被移除时调用此函数, 表示该 peer 不再需要参与互联组。
 * 从 owner_list 中移除 peer, 释放 owner 的引用计数。当 owner 的
 * 引用计数降为零时, 自动释放 owner 结构体 (通过 kref 机制)。
 *
 * drm_pagemap_release_owner() - Stop participating in an interconnect group
 * @peer: Pointer to the struct drm_pagemap_peer used when joining the group
 *
 * Stop participating in an interconnect group. This function is typically
 * called when a pagemap is removed to indicate that it doesn't need to
 * be taken into account.
 */
void drm_pagemap_release_owner(struct drm_pagemap_peer *peer)
{
	struct drm_pagemap_owner_list *owner_list = peer->list;

	if (!owner_list)
		return;

	mutex_lock(&owner_list->lock);
	list_del(&peer->link);
	kref_put(&peer->owner->kref, drm_pagemap_owner_release);
	peer->owner = NULL;
	mutex_unlock(&owner_list->lock);
}
EXPORT_SYMBOL(drm_pagemap_release_owner);

/**
 * typedef interconnect_fn - Callback function to identify fast interconnects
 * @peer1: First endpoint.
 * @peer2: Second endpont.
 *
 * The function returns %true iff @peer1 and @peer2 have a fast interconnect.
 * Note that this is symmetrical. The function has no notion of client and provider,
 * which may not be sufficient in some cases. However, since the callback is intended
 * to guide in providing common pagemap owners, the notion of a common owner to
 * indicate fast interconnects would then have to change as well.
 *
 * Return: %true iff @peer1 and @peer2 have a fast interconnect. Otherwise @false.
 */
typedef bool (*interconnect_fn)(struct drm_pagemap_peer *peer1, struct drm_pagemap_peer *peer2);

/**
 * 中文注释: 加入设备互联组
 * 通过反复调用 @has_interconnect 回调函数, 确定 @peer 与 owner_list
 * 上其他 peer 之间是否具有快速互联 (fast interconnect)。如果找到
 * 具有快速互联的 peer 集合, 它们将共享同一个 drm_pagemap_owner。
 * 如果 @peer 与所有其他 peer 都没有快速互联, 则为其分配一个新的
 * 唯一 owner。
 *
 * 互联组的目的是优化共享内存场景: 具有快速互联的设备之间可以共用
 * 页面映射, 减少重复映射的开销。
 *
 * 当 peer 不再参与互联组时, 应调用 drm_pagemap_release_owner()。
 *
 * drm_pagemap_acquire_owner() - Join an interconnect group
 * @peer: A struct drm_pagemap_peer keeping track of the device interconnect
 * @owner_list: Pointer to the owner_list, keeping track of all interconnects
 * @has_interconnect: Callback function to determine whether two peers have a
 * fast local interconnect.
 *
 * Repeatedly calls @has_interconnect for @peer and other peers on @owner_list to
 * determine a set of peers for which @peer has a fast interconnect. That set will
 * have common &struct drm_pagemap_owner, and upon successful return, @peer::owner
 * will point to that struct, holding a reference, and @peer will be registered in
 * @owner_list. If @peer doesn't have any fast interconnects to other @peers, a
 * new unique &struct drm_pagemap_owner will be allocated for it, and that
 * may be shared with other peers that, at a later point, are determined to have
 * a fast interconnect with @peer.
 *
 * When @peer no longer participates in an interconnect group,
 * drm_pagemap_release_owner() should be called to drop the reference on the
 * struct drm_pagemap_owner.
 *
 * Return: %0 on success, negative error code on failure.
 */
int drm_pagemap_acquire_owner(struct drm_pagemap_peer *peer,
			      struct drm_pagemap_owner_list *owner_list,
			      interconnect_fn has_interconnect)
{
	struct drm_pagemap_peer *cur_peer;
	struct drm_pagemap_owner *owner = NULL;
	bool interconnect = false;

	mutex_lock(&owner_list->lock);
	might_alloc(GFP_KERNEL);
	list_for_each_entry(cur_peer, &owner_list->peers, link) {
		if (cur_peer->owner != owner) {
			if (owner && interconnect)
				break;
			owner = cur_peer->owner;
			interconnect = true;
		}
		if (interconnect && !has_interconnect(peer, cur_peer))
			interconnect = false;
	}

	if (!interconnect) {
		owner = kmalloc_obj(*owner);
		if (!owner) {
			mutex_unlock(&owner_list->lock);
			return -ENOMEM;
		}
		kref_init(&owner->kref);
		list_add_tail(&peer->link, &owner_list->peers);
	} else {
		kref_get(&owner->kref);
		list_add_tail(&peer->link, &cur_peer->link);
	}
	peer->owner = owner;
	peer->list = owner_list;
	mutex_unlock(&owner_list->lock);

	return 0;
}
EXPORT_SYMBOL(drm_pagemap_acquire_owner);
