// SPDX-License-Identifier: GPL-2.0 OR MIT

/*
 * 中文注释: DRM 执行上下文 (Execution Context)
 *
 * 本文件实现了 DRM 执行上下文, 用于管理多个 GEM 对象的锁定和 fence
 * 预留。该组件主要抽象了锁定多个 GEM 对象时的重试循环 (retry loop),
 * 确保在准备硬件操作 (如命令提交、页表更新等) 时能够安全地锁定所有
 * 需要的缓冲区对象。
 *
 * 核心设计:
 *   当锁定一个 GEM 对象时检测到争用 (contention), 清理函数会解锁
 *   所有已锁定的对象, 优先锁定争用的对象, 然后再锁定其余对象。
 *   这种策略避免了经典的"锁顺序反转"问题。
 *
 * 典型用法:
 *   drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
 *   drm_exec_until_all_locked(&exec) {
 *       ret = drm_exec_prepare_obj(&exec, boA, 1);
 *       drm_exec_retry_on_contention(&exec);
 *       if (ret) goto error;
 *       ret = drm_exec_prepare_obj(&exec, boB, 1);
 *       drm_exec_retry_on_contention(&exec);
 *       if (ret) goto error;
 *   }
 *   drm_exec_for_each_locked_object(&exec, index, obj) {
 *       dma_resv_add_fence(obj->resv, fence, DMA_RESV_USAGE_READ);
 *   }
 *   drm_exec_fini(&exec);
 *
 * 主要接口:
 *   - drm_exec_init / drm_exec_fini: 初始化/销毁执行上下文
 *   - drm_exec_lock_obj / drm_exec_unlock_obj: 锁定/解锁单个 GEM 对象
 *   - drm_exec_prepare_obj: 锁定并预留 fence 槽位
 *   - drm_exec_prepare_array: 批量准备 GEM 对象数组
 *   - drm_exec_cleanup: 检测到争用时的清理和重试
 */

#include <drm/drm_exec.h>
#include <drm/drm_gem.h>

#include <linux/dma-resv.h>
#include <linux/export.h>

/**
 * DOC: Overview
 *
 * This component mainly abstracts the retry loop necessary for locking
 * multiple GEM objects while preparing hardware operations (e.g. command
 * submissions, page table updates etc..).
 *
 * If a contention is detected while locking a GEM object the cleanup procedure
 * unlocks all previously locked GEM objects and locks the contended one first
 * before locking any further objects.
 *
 * After an object is locked fences slots can optionally be reserved on the
 * dma_resv object inside the GEM object.
 *
 * A typical usage pattern should look like this::
 *
 *	struct drm_gem_object *obj;
 *	struct drm_exec exec;
 *	unsigned long index;
 *	int ret;
 *
 *	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT);
 *	drm_exec_until_all_locked(&exec) {
 *		ret = drm_exec_prepare_obj(&exec, boA, 1);
 *		drm_exec_retry_on_contention(&exec);
 *		if (ret)
 *			goto error;
 *
 *		ret = drm_exec_prepare_obj(&exec, boB, 1);
 *		drm_exec_retry_on_contention(&exec);
 *		if (ret)
 *			goto error;
 *	}
 *
 *	drm_exec_for_each_locked_object(&exec, index, obj) {
 *		dma_resv_add_fence(obj->resv, fence, DMA_RESV_USAGE_READ);
 *		...
 *	}
 *	drm_exec_fini(&exec);
 *
 * See struct dma_exec for more details.
 */

/* Dummy value used to initially enter the retry loop */
#define DRM_EXEC_DUMMY ((void *)~0)

/* Unlock all objects and drop references */
static void drm_exec_unlock_all(struct drm_exec *exec)
{
	struct drm_gem_object *obj;
	unsigned long index;

	drm_exec_for_each_locked_object_reverse(exec, index, obj) {
		dma_resv_unlock(obj->resv);
		drm_gem_object_put(obj);
	}

	drm_gem_object_put(exec->prelocked);
	exec->prelocked = NULL;
}

/**
 * 中文注释: 初始化执行上下文
 * 初始化 drm_exec 对象。内部分配一个动态对象数组 (初始大小为 nr,
 * 如果 nr 为 0 则默认为 PAGE_SIZE / sizeof(void*)) 用于跟踪已锁
 * 定的 GEM 对象。数组在需要时自动增长。设置初始状态为
 * DRM_EXEC_DUMMY, 表示还没有产生争用。
 *
 * drm_exec_init - initialize a drm_exec object
 * @exec: the drm_exec object to initialize
 * @flags: controls locking behavior, see DRM_EXEC_* defines
 * @nr: the initial # of objects
 *
 * Initialize the object and make sure that we can track locked objects.
 *
 * If nr is non-zero then it is used as the initial objects table size.
 * In either case, the table will grow (be re-allocated) on demand.
 */
void drm_exec_init(struct drm_exec *exec, u32 flags, unsigned nr)
{
	if (!nr)
		nr = PAGE_SIZE / sizeof(void *);

	exec->flags = flags;
	exec->objects = kvmalloc_array(nr, sizeof(void *), GFP_KERNEL);

	/* If allocation here fails, just delay that till the first use */
	exec->max_objects = exec->objects ? nr : 0;
	exec->num_objects = 0;
	exec->contended = DRM_EXEC_DUMMY;
	exec->prelocked = NULL;
}
EXPORT_SYMBOL(drm_exec_init);

/**
 * 中文注释: 销毁 (结束) 执行上下文
 * 解锁所有已锁定的 GEM 对象, 释放对象引用, 释放跟踪数组内存。
 * 如果有争用对象 (contended != DRM_EXEC_DUMMY), 还需要完成
 * ww_acquire_fini 清理。应在所有操作完成后调用。
 *
 * drm_exec_fini - finalize a drm_exec object
 * @exec: the drm_exec object to finalize
 *
 * Unlock all locked objects, drop the references to objects and free all memory
 * used for tracking the state.
 */
void drm_exec_fini(struct drm_exec *exec)
{
	drm_exec_unlock_all(exec);
	kvfree(exec->objects);
	if (exec->contended != DRM_EXEC_DUMMY) {
		drm_gem_object_put(exec->contended);
		ww_acquire_fini(&exec->ticket);
	}
}
EXPORT_SYMBOL(drm_exec_fini);

/**
 * 中文注释: 检测到争用时的清理操作
 * 核心的重试逻辑函数:
 *   - 如果没有争用: 调用 ww_acquire_done 并返回 false (退出重试循环)
 *   - 如果是首次进入 (DRM_EXEC_DUMMY): 初始化 ww_mutex 票证并返回
 *     true (进入重试循环)
 *   - 如果有争用对象: 解锁所有已锁对象, 清空跟踪数组, 返回 true
 *     (重试, 下轮会先锁定争用的对象)
 *
 * drm_exec_cleanup - cleanup when contention is detected
 * @exec: the drm_exec object to cleanup
 *
 * Cleanup the current state and return true if we should stay inside the retry
 * loop, false if there wasn't any contention detected and we can keep the
 * objects locked.
 */
bool drm_exec_cleanup(struct drm_exec *exec)
{
	if (likely(!exec->contended)) {
		ww_acquire_done(&exec->ticket);
		return false;
	}

	if (likely(exec->contended == DRM_EXEC_DUMMY)) {
		exec->contended = NULL;
		ww_acquire_init(&exec->ticket, &reservation_ww_class);
		return true;
	}

	drm_exec_unlock_all(exec);
	exec->num_objects = 0;
	return true;
}
EXPORT_SYMBOL(drm_exec_cleanup);

/* Track the locked object in the array */
static int drm_exec_obj_locked(struct drm_exec *exec,
			       struct drm_gem_object *obj)
{
	if (unlikely(exec->num_objects == exec->max_objects)) {
		size_t size = exec->max_objects * sizeof(void *);
		void *tmp;

		tmp = kvrealloc(exec->objects, size + PAGE_SIZE, GFP_KERNEL);
		if (!tmp)
			return -ENOMEM;

		exec->objects = tmp;
		exec->max_objects += PAGE_SIZE / sizeof(void *);
	}
	drm_gem_object_get(obj);
	exec->objects[exec->num_objects++] = obj;

	return 0;
}

/* Make sure the contended object is locked first */
static int drm_exec_lock_contended(struct drm_exec *exec)
{
	struct drm_gem_object *obj = exec->contended;
	int ret;

	if (likely(!obj))
		return 0;

	/* Always cleanup the contention so that error handling can kick in */
	exec->contended = NULL;
	if (exec->flags & DRM_EXEC_INTERRUPTIBLE_WAIT) {
		ret = dma_resv_lock_slow_interruptible(obj->resv,
						       &exec->ticket);
		if (unlikely(ret))
			goto error_dropref;
	} else {
		dma_resv_lock_slow(obj->resv, &exec->ticket);
	}

	ret = drm_exec_obj_locked(exec, obj);
	if (unlikely(ret))
		goto error_unlock;

	exec->prelocked = obj;
	return 0;

error_unlock:
	dma_resv_unlock(obj->resv);

error_dropref:
	drm_gem_object_put(obj);
	return ret;
}

/**
 * 中文注释: 锁定 GEM 对象
 * 对 GEM 对象的 dma_resv 进行 ww_mutex 方式的锁定。如果对象之前是
 * 争用对象 (contended), 先以慢路径 (slowpath) 锁定它。如果对象是
 * prelocked (之前在争用恢复时已锁定), 直接返回成功。
 * 支持可中断等待 (DRM_EXEC_INTERRUPTIBLE_WAIT 标志) 和重复锁定
 * 忽略 (DRM_EXEC_IGNORE_DUPLICATES 标志)。
 *
 * drm_exec_lock_obj - lock a GEM object for use
 * @exec: the drm_exec object with the state
 * @obj: the GEM object to lock
 *
 * Lock a GEM object for use and grab a reference to it.
 *
 * Returns: -EDEADLK if a contention is detected, -EALREADY when object is
 * already locked (can be suppressed by setting the DRM_EXEC_IGNORE_DUPLICATES
 * flag), -ENOMEM when memory allocation failed and zero for success.
 */
int drm_exec_lock_obj(struct drm_exec *exec, struct drm_gem_object *obj)
{
	int ret;

	ret = drm_exec_lock_contended(exec);
	if (unlikely(ret))
		return ret;

	if (exec->prelocked == obj) {
		drm_gem_object_put(exec->prelocked);
		exec->prelocked = NULL;
		return 0;
	}

	if (exec->flags & DRM_EXEC_INTERRUPTIBLE_WAIT)
		ret = dma_resv_lock_interruptible(obj->resv, &exec->ticket);
	else
		ret = dma_resv_lock(obj->resv, &exec->ticket);

	if (unlikely(ret == -EDEADLK)) {
		drm_gem_object_get(obj);
		exec->contended = obj;
		return -EDEADLK;
	}

	if (unlikely(ret == -EALREADY) &&
	    exec->flags & DRM_EXEC_IGNORE_DUPLICATES)
		return 0;

	if (unlikely(ret))
		return ret;

	ret = drm_exec_obj_locked(exec, obj);
	if (ret)
		goto error_unlock;

	return 0;

error_unlock:
	dma_resv_unlock(obj->resv);
	return ret;
}
EXPORT_SYMBOL(drm_exec_lock_obj);

/**
 * 中文注释: 在执行上下文中解锁 GEM 对象
 * 解锁指定的 GEM 对象并将其从已锁定对象集合中移除。该函数通过遍历
 * 数组查找对象, 因此解锁最近锁定的对象效率较高, 解锁较早锁定的
 * 对象效率较低。主要用于在 prepare_obj 中 fence 预留失败时的回滚。
 *
 * drm_exec_unlock_obj - unlock a GEM object in this exec context
 * @exec: the drm_exec object with the state
 * @obj: the GEM object to unlock
 *
 * Unlock the GEM object and remove it from the collection of locked objects.
 * Should only be used to unlock the most recently locked objects. It's not time
 * efficient to unlock objects locked long ago.
 */
void drm_exec_unlock_obj(struct drm_exec *exec, struct drm_gem_object *obj)
{
	unsigned int i;

	for (i = exec->num_objects; i--;) {
		if (exec->objects[i] == obj) {
			dma_resv_unlock(obj->resv);
			for (++i; i < exec->num_objects; ++i)
				exec->objects[i - 1] = exec->objects[i];
			--exec->num_objects;
			drm_gem_object_put(obj);
			return;
		}

	}
}
EXPORT_SYMBOL(drm_exec_unlock_obj);

/**
 * 中文注释: 准备 GEM 对象供使用
 * 锁定 GEM 对象并在其 dma_resv 中预留 fence 槽位。这是
 * drm_exec_lock_obj() 和 dma_resv_reserve_fences() 的组合封装。
 * 如果 fence 预留失败, 自动解锁对象。
 * 在命令提交、页表更新等操作前应调用此函数准备所有需要的对象。
 *
 * drm_exec_prepare_obj - prepare a GEM object for use
 * @exec: the drm_exec object with the state
 * @obj: the GEM object to prepare
 * @num_fences: how many fences to reserve
 *
 * Prepare a GEM object for use by locking it and reserving fence slots.
 *
 * Returns: -EDEADLK if a contention is detected, -EALREADY when object is
 * already locked, -ENOMEM when memory allocation failed and zero for success.
 */
int drm_exec_prepare_obj(struct drm_exec *exec, struct drm_gem_object *obj,
			 unsigned int num_fences)
{
	int ret;

	ret = drm_exec_lock_obj(exec, obj);
	if (ret)
		return ret;

	ret = dma_resv_reserve_fences(obj->resv, num_fences);
	if (ret) {
		drm_exec_unlock_obj(exec, obj);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_exec_prepare_obj);

/**
 * 中文注释: 批量准备 GEM 对象数组
 * 遍历 GEM 对象数组, 对每个对象调用 drm_exec_prepare_obj() 进行
 * 锁定和 fence 预留。遇到第一个错误时立即中止并返回错误码。
 * 适用于需要批量锁定多个对象的场景, 如命令提交时的缓冲区列表。
 *
 * drm_exec_prepare_array - helper to prepare an array of objects
 * @exec: the drm_exec object with the state
 * @objects: array of GEM object to prepare
 * @num_objects: number of GEM objects in the array
 * @num_fences: number of fences to reserve on each GEM object
 *
 * Prepares all GEM objects in an array, aborts on first error.
 * Reserves @num_fences on each GEM object after locking it.
 *
 * Returns: -EDEADLOCK on contention, -EALREADY when object is already locked,
 * -ENOMEM when memory allocation failed and zero for success.
 */
int drm_exec_prepare_array(struct drm_exec *exec,
			   struct drm_gem_object **objects,
			   unsigned int num_objects,
			   unsigned int num_fences)
{
	int ret;

	for (unsigned int i = 0; i < num_objects; ++i) {
		ret = drm_exec_prepare_obj(exec, objects[i], num_fences);
		if (unlikely(ret))
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_exec_prepare_array);

MODULE_DESCRIPTION("DRM execution context");
MODULE_LICENSE("Dual MIT/GPL");
