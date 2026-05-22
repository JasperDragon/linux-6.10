// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright © 2024 Intel Corporation
 *
 * Authors:
 *     Matthew Brost <matthew.brost@intel.com>
 */

/**
 * DOC: GPU 共享虚拟内存概述 (中文)
 *
 * 该文件实现了 DRM GPU 共享虚拟内存（SVM）层，用于管理 CPU 和 GPU 之间的
 * 共享虚拟地址空间。它使 GPU 和 CPU 能够通过相同的虚拟地址访问内存，从而
 * 实现高效的数据交换和处理。
 *
 * 核心组件包括：
 *   1. Notifiers（通知器）：跟踪内存区间并在 CPU 修改时通知 GPU。
 *      基于 mmu_interval_notifier 实现，维护红黑树和区间列表。
 *   2. Ranges（范围）：表示 DRM 设备中映射的内存范围，由 GPU SVM 管理。
 *      范围在 GPU 缺页时动态分配，在 MMU 通知 UNMAP 事件时移除。
 *   3. Operations（操作）：驱动特定的 GPU SVM 操作接口，包括范围分配、
 *      notifier 分配和无效化回调。
 *   4. 设备内存分配：包含迁移所需的信息，支持在设备内存和系统内存之间迁移。
 *
 * 典型组件集成：
 *   - GPU 缺页处理程序：基于缺页地址创建范围和 notifier，迁移到设备内存
 *   - 垃圾回收器：解除并销毁 GPU 绑定
 *   - Notifier 回调：无效化和 DMA 解除映射 GPU 绑定
 */

#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/hmm.h>
#include <linux/hugetlb_inline.h>
#include <linux/memremap.h>
#include <linux/mm_types.h>
#include <linux/slab.h>

#include <drm/drm_device.h>
#include <drm/drm_gpusvm.h>
#include <drm/drm_pagemap.h>
#include <drm/drm_print.h>

/**
 * DOC: Overview
 *
 * GPU Shared Virtual Memory (GPU SVM) layer for the Direct Rendering Manager (DRM)
 * is a component of the DRM framework designed to manage shared virtual memory
 * between the CPU and GPU. It enables efficient data exchange and processing
 * for GPU-accelerated applications by allowing memory sharing and
 * synchronization between the CPU's and GPU's virtual address spaces.
 *
 * Key GPU SVM Components:
 *
 * - Notifiers:
 *	Used for tracking memory intervals and notifying the GPU of changes,
 *	notifiers are sized based on a GPU SVM initialization parameter, with a
 *	recommendation of 512M or larger. They maintain a Red-BlacK tree and a
 *	list of ranges that fall within the notifier interval.  Notifiers are
 *	tracked within a GPU SVM Red-BlacK tree and list and are dynamically
 *	inserted or removed as ranges within the interval are created or
 *	destroyed.
 * - Ranges:
 *	Represent memory ranges mapped in a DRM device and managed by GPU SVM.
 *	They are sized based on an array of chunk sizes, which is a GPU SVM
 *	initialization parameter, and the CPU address space.  Upon GPU fault,
 *	the largest aligned chunk that fits within the faulting CPU address
 *	space is chosen for the range size. Ranges are expected to be
 *	dynamically allocated on GPU fault and removed on an MMU notifier UNMAP
 *	event. As mentioned above, ranges are tracked in a notifier's Red-Black
 *	tree.
 *
 * - Operations:
 *	Define the interface for driver-specific GPU SVM operations such as
 *	range allocation, notifier allocation, and invalidations.
 *
 * - Device Memory Allocations:
 *	Embedded structure containing enough information for GPU SVM to migrate
 *	to / from device memory.
 *
 * - Device Memory Operations:
 *	Define the interface for driver-specific device memory operations
 *	release memory, populate pfns, and copy to / from device memory.
 *
 * This layer provides interfaces for allocating, mapping, migrating, and
 * releasing memory ranges between the CPU and GPU. It handles all core memory
 * management interactions (DMA mapping, HMM, and migration) and provides
 * driver-specific virtual functions (vfuncs). This infrastructure is sufficient
 * to build the expected driver components for an SVM implementation as detailed
 * below.
 *
 * Expected Driver Components:
 *
 * - GPU page fault handler:
 *	Used to create ranges and notifiers based on the fault address,
 *	optionally migrate the range to device memory, and create GPU bindings.
 *
 * - Garbage collector:
 *	Used to unmap and destroy GPU bindings for ranges.  Ranges are expected
 *	to be added to the garbage collector upon a MMU_NOTIFY_UNMAP event in
 *	notifier callback.
 *
 * - Notifier callback:
 *	Used to invalidate and DMA unmap GPU bindings for ranges.
 */

/**
 * DOC: Locking
 *
 * GPU SVM handles locking for core MM interactions, i.e., it locks/unlocks the
 * mmap lock as needed.
 *
 * GPU SVM introduces a global notifier lock, which safeguards the notifier's
 * range RB tree and list, as well as the range's DMA mappings and sequence
 * number. GPU SVM manages all necessary locking and unlocking operations,
 * except for the recheck range's pages being valid
 * (drm_gpusvm_range_pages_valid) when the driver is committing GPU bindings.
 * This lock corresponds to the ``driver->update`` lock mentioned in
 * Documentation/mm/hmm.rst. Future revisions may transition from a GPU SVM
 * global lock to a per-notifier lock if finer-grained locking is deemed
 * necessary.
 *
 * In addition to the locking mentioned above, the driver should implement a
 * lock to safeguard core GPU SVM function calls that modify state, such as
 * drm_gpusvm_range_find_or_insert and drm_gpusvm_range_remove. This lock is
 * denoted as 'driver_svm_lock' in code examples. Finer grained driver side
 * locking should also be possible for concurrent GPU fault processing within a
 * single GPU SVM. The 'driver_svm_lock' can be via drm_gpusvm_driver_set_lock
 * to add annotations to GPU SVM.
 */

/**
 * DOC: Partial Unmapping of Ranges
 *
 * Partial unmapping of ranges (e.g., 1M out of 2M is unmapped by CPU resulting
 * in MMU_NOTIFY_UNMAP event) presents several challenges, with the main one
 * being that a subset of the range still has CPU and GPU mappings. If the
 * backing store for the range is in device memory, a subset of the backing
 * store has references. One option would be to split the range and device
 * memory backing store, but the implementation for this would be quite
 * complicated. Given that partial unmappings are rare and driver-defined range
 * sizes are relatively small, GPU SVM does not support splitting of ranges.
 *
 * With no support for range splitting, upon partial unmapping of a range, the
 * driver is expected to invalidate and destroy the entire range. If the range
 * has device memory as its backing, the driver is also expected to migrate any
 * remaining pages back to RAM.
 */

/**
 * DOC: Examples
 *
 * This section provides three examples of how to build the expected driver
 * components: the GPU page fault handler, the garbage collector, and the
 * notifier callback.
 *
 * The generic code provided does not include logic for complex migration
 * policies, optimized invalidations, fined grained driver locking, or other
 * potentially required driver locking (e.g., DMA-resv locks).
 *
 * 1) GPU page fault handler
 *
 * .. code-block:: c
 *
 *	int driver_bind_range(struct drm_gpusvm *gpusvm, struct drm_gpusvm_range *range)
 *	{
 *		int err = 0;
 *
 *		driver_alloc_and_setup_memory_for_bind(gpusvm, range);
 *
 *		drm_gpusvm_notifier_lock(gpusvm);
 *		if (drm_gpusvm_range_pages_valid(range))
 *			driver_commit_bind(gpusvm, range);
 *		else
 *			err = -EAGAIN;
 *		drm_gpusvm_notifier_unlock(gpusvm);
 *
 *		return err;
 *	}
 *
 *	int driver_gpu_fault(struct drm_gpusvm *gpusvm, unsigned long fault_addr,
 *			     unsigned long gpuva_start, unsigned long gpuva_end)
 *	{
 *		struct drm_gpusvm_ctx ctx = {};
 *		int err;
 *
 *		driver_svm_lock();
 *	retry:
 *		// Always process UNMAPs first so view of GPU SVM ranges is current
 *		driver_garbage_collector(gpusvm);
 *
 *		range = drm_gpusvm_range_find_or_insert(gpusvm, fault_addr,
 *							gpuva_start, gpuva_end,
 *						        &ctx);
 *		if (IS_ERR(range)) {
 *			err = PTR_ERR(range);
 *			goto unlock;
 *		}
 *
 *		if (driver_migration_policy(range)) {
 *			err = drm_pagemap_populate_mm(driver_choose_drm_pagemap(),
 *						      gpuva_start, gpuva_end, gpusvm->mm,
 *						      ctx->timeslice_ms);
 *			if (err)	// CPU mappings may have changed
 *				goto retry;
 *		}
 *
 *		err = drm_gpusvm_range_get_pages(gpusvm, range, &ctx);
 *		if (err == -EOPNOTSUPP || err == -EFAULT || err == -EPERM) {	// CPU mappings changed
 *			if (err == -EOPNOTSUPP)
 *				drm_gpusvm_range_evict(gpusvm, range);
 *			goto retry;
 *		} else if (err) {
 *			goto unlock;
 *		}
 *
 *		err = driver_bind_range(gpusvm, range);
 *		if (err == -EAGAIN)	// CPU mappings changed
 *			goto retry
 *
 *	unlock:
 *		driver_svm_unlock();
 *		return err;
 *	}
 *
 * 2) Garbage Collector
 *
 * .. code-block:: c
 *
 *	void __driver_garbage_collector(struct drm_gpusvm *gpusvm,
 *					struct drm_gpusvm_range *range)
 *	{
 *		assert_driver_svm_locked(gpusvm);
 *
 *		// Partial unmap, migrate any remaining device memory pages back to RAM
 *		if (range->flags.partial_unmap)
 *			drm_gpusvm_range_evict(gpusvm, range);
 *
 *		driver_unbind_range(range);
 *		drm_gpusvm_range_remove(gpusvm, range);
 *	}
 *
 *	void driver_garbage_collector(struct drm_gpusvm *gpusvm)
 *	{
 *		assert_driver_svm_locked(gpusvm);
 *
 *		for_each_range_in_garbage_collector(gpusvm, range)
 *			__driver_garbage_collector(gpusvm, range);
 *	}
 *
 * 3) Notifier callback
 *
 * .. code-block:: c
 *
 *	void driver_invalidation(struct drm_gpusvm *gpusvm,
 *				 struct drm_gpusvm_notifier *notifier,
 *				 const struct mmu_notifier_range *mmu_range)
 *	{
 *		struct drm_gpusvm_ctx ctx = { .in_notifier = true, };
 *		struct drm_gpusvm_range *range = NULL;
 *
 *		driver_invalidate_device_pages(gpusvm, mmu_range->start, mmu_range->end);
 *
 *		drm_gpusvm_for_each_range(range, notifier, mmu_range->start,
 *					  mmu_range->end) {
 *			drm_gpusvm_range_unmap_pages(gpusvm, range, &ctx);
 *
 *			if (mmu_range->event != MMU_NOTIFY_UNMAP)
 *				continue;
 *
 *			drm_gpusvm_range_set_unmapped(range, mmu_range);
 *			driver_garbage_collector_add(gpusvm, range);
 *		}
 *	}
 */

/**
 * npages_in_range() - Calculate the number of pages in a given range
 * @start: The start address of the range
 * @end: The end address of the range
 *
 * This macro calculates the number of pages in a given memory range,
 * specified by the start and end addresses. It divides the difference
 * between the end and start addresses by the page size (PAGE_SIZE) to
 * determine the number of pages in the range.
 *
 * Return: The number of pages in the specified range.
 */
static unsigned long
npages_in_range(unsigned long start, unsigned long end)
{
	return (end - start) >> PAGE_SHIFT;
}

/**
 * drm_gpusvm_notifier_find() - 在 GPU SVM 中查找 GPU SVM notifier
 *
 * 中文: 在 GPU SVM 的红黑树（gpusvm->root）中查找覆盖指定地址范围
 * （@start 到 @end - 1）的 notifier。使用 interval_tree_iter_first()
 * 进行区间树搜索，找到第一个与范围重叠的 notifier。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @start: notifier 的起始地址
 * @end: notifier 的结束地址
 *
 * Return: A pointer to the drm_gpusvm_notifier if found or NULL
 */
struct drm_gpusvm_notifier *
drm_gpusvm_notifier_find(struct drm_gpusvm *gpusvm, unsigned long start,
			 unsigned long end)
{
	struct interval_tree_node *itree;

	itree = interval_tree_iter_first(&gpusvm->root, start, end - 1);

	if (itree)
		return container_of(itree, struct drm_gpusvm_notifier, itree);
	else
		return NULL;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_notifier_find);

/**
 * drm_gpusvm_range_find() - 从 GPU SVM notifier 中查找 GPU SVM range
 *
 * 中文: 在指定 notifier 的红黑树（notifier->root）中查找覆盖指定地址
 * 范围的 GPU SVM range。返回第一个与范围重叠的 range，未找到时返回 NULL。
 * 该函数不获取任何锁，调用者需确保适当的同步。
 *
 * @notifier: 指向 GPU SVM notifier 结构体的指针
 * @start: range 的起始地址
 * @end: range 的结束地址
 *
 * Return: A pointer to the drm_gpusvm_range if found or NULL
 */
struct drm_gpusvm_range *
drm_gpusvm_range_find(struct drm_gpusvm_notifier *notifier, unsigned long start,
		      unsigned long end)
{
	struct interval_tree_node *itree;

	itree = interval_tree_iter_first(&notifier->root, start, end - 1);

	if (itree)
		return container_of(itree, struct drm_gpusvm_range, itree);
	else
		return NULL;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_find);

/**
 * drm_gpusvm_notifier_invalidate() - Invalidate a GPU SVM notifier.
 * @mni: Pointer to the mmu_interval_notifier structure.
 * @mmu_range: Pointer to the mmu_notifier_range structure.
 * @cur_seq: Current sequence number.
 *
 * This function serves as a generic MMU notifier for GPU SVM. It sets the MMU
 * notifier sequence number and calls the driver invalidate vfunc under
 * gpusvm->notifier_lock.
 *
 * Return: true if the operation succeeds, false otherwise.
 */
static bool
drm_gpusvm_notifier_invalidate(struct mmu_interval_notifier *mni,
			       const struct mmu_notifier_range *mmu_range,
			       unsigned long cur_seq)
{
	struct drm_gpusvm_notifier *notifier =
		container_of(mni, typeof(*notifier), notifier);
	struct drm_gpusvm *gpusvm = notifier->gpusvm;

	if (!mmu_notifier_range_blockable(mmu_range))
		return false;

	down_write(&gpusvm->notifier_lock);
	mmu_interval_set_seq(mni, cur_seq);
	gpusvm->ops->invalidate(gpusvm, notifier, mmu_range);
	up_write(&gpusvm->notifier_lock);

	return true;
}

/*
 * drm_gpusvm_notifier_ops - MMU interval notifier operations for GPU SVM
 */
static const struct mmu_interval_notifier_ops drm_gpusvm_notifier_ops = {
	.invalidate = drm_gpusvm_notifier_invalidate,
};

/**
 * drm_gpusvm_init() - 初始化 GPU SVM
 *
 * 中文: 初始化 GPU 共享虚拟内存管理器。如果提供 @mm 参数则启用完整的 SVM
 * 模式（需要设置 @ops、@chunk_sizes、@mm_range 等参数）；如果仅使用简单的
 * drm_gpusvm_pages API（get/unmap/free），则只需提供 @gpusvm、@name 和
 * @drm，@mm 为 NULL。两种模式可以共存在同一个 @gpusvm 实例中。
 * 初始化包括设置 notifier 红黑树、列表、notifier_lock 读写信号量等。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @name: GPU SVM 的名称
 * @drm: 指向 DRM 设备结构体的指针
 * @mm: 指向地址空间 mm_struct 的指针
 * @mm_start: GPU SVM 的起始地址
 * @mm_range: GPU SVM 的范围大小
 * @notifier_size: 单个 notifier 的大小
 * @ops: GPU SVM 操作结构体指针
 * @chunk_sizes: 范围分配使用的块大小数组（2 的幂降序，最后一项须为 SZ_4K）
 * @num_chunks: 块数量
 *
 * Return: 0 on success, a negative error code on failure.
 */
int drm_gpusvm_init(struct drm_gpusvm *gpusvm,
		    const char *name, struct drm_device *drm,
		    struct mm_struct *mm,
		    unsigned long mm_start, unsigned long mm_range,
		    unsigned long notifier_size,
		    const struct drm_gpusvm_ops *ops,
		    const unsigned long *chunk_sizes, int num_chunks)
{
	if (mm) {
		if (!ops->invalidate || !num_chunks)
			return -EINVAL;
		mmgrab(mm);
	} else {
		/* No full SVM mode, only core drm_gpusvm_pages API. */
		if (ops || num_chunks || mm_range || notifier_size)
			return -EINVAL;
	}

	gpusvm->name = name;
	gpusvm->drm = drm;
	gpusvm->mm = mm;
	gpusvm->mm_start = mm_start;
	gpusvm->mm_range = mm_range;
	gpusvm->notifier_size = notifier_size;
	gpusvm->ops = ops;
	gpusvm->chunk_sizes = chunk_sizes;
	gpusvm->num_chunks = num_chunks;

	gpusvm->root = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&gpusvm->notifier_list);

	init_rwsem(&gpusvm->notifier_lock);

	fs_reclaim_acquire(GFP_KERNEL);
	might_lock(&gpusvm->notifier_lock);
	fs_reclaim_release(GFP_KERNEL);

#ifdef CONFIG_LOCKDEP
	gpusvm->lock_dep_map = NULL;
#endif

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_init);

/**
 * to_drm_gpusvm_notifier() - retrieve the container struct for a given rbtree node
 * @node: a pointer to the rbtree node embedded within a drm_gpusvm_notifier struct
 *
 * Return: A pointer to the containing drm_gpusvm_notifier structure.
 */
static struct drm_gpusvm_notifier *to_drm_gpusvm_notifier(struct rb_node *node)
{
	return container_of(node, struct drm_gpusvm_notifier, itree.rb);
}

/**
 * drm_gpusvm_notifier_insert() - Insert GPU SVM notifier
 * @gpusvm: Pointer to the GPU SVM structure
 * @notifier: Pointer to the GPU SVM notifier structure
 *
 * This function inserts the GPU SVM notifier into the GPU SVM RB tree and list.
 */
static void drm_gpusvm_notifier_insert(struct drm_gpusvm *gpusvm,
				       struct drm_gpusvm_notifier *notifier)
{
	struct rb_node *node;
	struct list_head *head;

	interval_tree_insert(&notifier->itree, &gpusvm->root);

	node = rb_prev(&notifier->itree.rb);
	if (node)
		head = &(to_drm_gpusvm_notifier(node))->entry;
	else
		head = &gpusvm->notifier_list;

	list_add(&notifier->entry, head);
}

/**
 * drm_gpusvm_notifier_remove() - Remove GPU SVM notifier
 * @gpusvm: Pointer to the GPU SVM tructure
 * @notifier: Pointer to the GPU SVM notifier structure
 *
 * This function removes the GPU SVM notifier from the GPU SVM RB tree and list.
 */
static void drm_gpusvm_notifier_remove(struct drm_gpusvm *gpusvm,
				       struct drm_gpusvm_notifier *notifier)
{
	interval_tree_remove(&notifier->itree, &gpusvm->root);
	list_del(&notifier->entry);
}

/**
 * drm_gpusvm_fini() - 清理 GPU SVM
 *
 * 中文: 清理 GPU SVM，移除所有剩余的 notifier 和 range。首先遍历所有
 * notifier，对每个 notifier 先移除 mmu_interval_notifier 以避免与无效化
 * 操作并发，然后移除该 notifier 中的所有 range。最后释放 mm_struct 引用。
 * 完成后验证红黑树为空。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 */
void drm_gpusvm_fini(struct drm_gpusvm *gpusvm)
{
	struct drm_gpusvm_notifier *notifier, *next;

	drm_gpusvm_for_each_notifier_safe(notifier, next, gpusvm, 0, LONG_MAX) {
		struct drm_gpusvm_range *range, *__next;

		/*
		 * Remove notifier first to avoid racing with any invalidation
		 */
		mmu_interval_notifier_remove(&notifier->notifier);
		notifier->flags.removed = true;

		drm_gpusvm_for_each_range_safe(range, __next, notifier, 0,
					       LONG_MAX)
			drm_gpusvm_range_remove(gpusvm, range);
	}

	if (gpusvm->mm)
		mmdrop(gpusvm->mm);
	WARN_ON(!RB_EMPTY_ROOT(&gpusvm->root.rb_root));
}
EXPORT_SYMBOL_GPL(drm_gpusvm_fini);

/**
 * drm_gpusvm_notifier_alloc() - Allocate GPU SVM notifier
 * @gpusvm: Pointer to the GPU SVM structure
 * @fault_addr: Fault address
 *
 * This function allocates and initializes the GPU SVM notifier structure.
 *
 * Return: Pointer to the allocated GPU SVM notifier on success, ERR_PTR() on failure.
 */
static struct drm_gpusvm_notifier *
drm_gpusvm_notifier_alloc(struct drm_gpusvm *gpusvm, unsigned long fault_addr)
{
	struct drm_gpusvm_notifier *notifier;

	if (gpusvm->ops->notifier_alloc)
		notifier = gpusvm->ops->notifier_alloc();
	else
		notifier = kzalloc_obj(*notifier);

	if (!notifier)
		return ERR_PTR(-ENOMEM);

	notifier->gpusvm = gpusvm;
	notifier->itree.start = ALIGN_DOWN(fault_addr, gpusvm->notifier_size);
	notifier->itree.last = ALIGN(fault_addr + 1, gpusvm->notifier_size) - 1;
	INIT_LIST_HEAD(&notifier->entry);
	notifier->root = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&notifier->range_list);

	return notifier;
}

/**
 * drm_gpusvm_notifier_free() - Free GPU SVM notifier
 * @gpusvm: Pointer to the GPU SVM structure
 * @notifier: Pointer to the GPU SVM notifier structure
 *
 * This function frees the GPU SVM notifier structure.
 */
static void drm_gpusvm_notifier_free(struct drm_gpusvm *gpusvm,
				     struct drm_gpusvm_notifier *notifier)
{
	WARN_ON(!RB_EMPTY_ROOT(&notifier->root.rb_root));

	if (gpusvm->ops->notifier_free)
		gpusvm->ops->notifier_free(notifier);
	else
		kfree(notifier);
}

/**
 * to_drm_gpusvm_range() - retrieve the container struct for a given rbtree node
 * @node: a pointer to the rbtree node embedded within a drm_gpusvm_range struct
 *
 * Return: A pointer to the containing drm_gpusvm_range structure.
 */
static struct drm_gpusvm_range *to_drm_gpusvm_range(struct rb_node *node)
{
	return container_of(node, struct drm_gpusvm_range, itree.rb);
}

/**
 * drm_gpusvm_range_insert() - Insert GPU SVM range
 * @notifier: Pointer to the GPU SVM notifier structure
 * @range: Pointer to the GPU SVM range structure
 *
 * This function inserts the GPU SVM range into the notifier RB tree and list.
 */
static void drm_gpusvm_range_insert(struct drm_gpusvm_notifier *notifier,
				    struct drm_gpusvm_range *range)
{
	struct rb_node *node;
	struct list_head *head;

	drm_gpusvm_notifier_lock(notifier->gpusvm);
	interval_tree_insert(&range->itree, &notifier->root);

	node = rb_prev(&range->itree.rb);
	if (node)
		head = &(to_drm_gpusvm_range(node))->entry;
	else
		head = &notifier->range_list;

	list_add(&range->entry, head);
	drm_gpusvm_notifier_unlock(notifier->gpusvm);
}

/**
 * __drm_gpusvm_range_remove() - Remove GPU SVM range
 * @notifier: Pointer to the GPU SVM notifier structure
 * @range: Pointer to the GPU SVM range structure
 *
 * This macro removes the GPU SVM range from the notifier RB tree and list.
 */
static void __drm_gpusvm_range_remove(struct drm_gpusvm_notifier *notifier,
				      struct drm_gpusvm_range *range)
{
	interval_tree_remove(&range->itree, &notifier->root);
	list_del(&range->entry);
}

/**
 * drm_gpusvm_range_alloc() - Allocate GPU SVM range
 * @gpusvm: Pointer to the GPU SVM structure
 * @notifier: Pointer to the GPU SVM notifier structure
 * @fault_addr: Fault address
 * @chunk_size: Chunk size
 * @migrate_devmem: Flag indicating whether to migrate device memory
 *
 * This function allocates and initializes the GPU SVM range structure.
 *
 * Return: Pointer to the allocated GPU SVM range on success, ERR_PTR() on failure.
 */
static struct drm_gpusvm_range *
drm_gpusvm_range_alloc(struct drm_gpusvm *gpusvm,
		       struct drm_gpusvm_notifier *notifier,
		       unsigned long fault_addr, unsigned long chunk_size,
		       bool migrate_devmem)
{
	struct drm_gpusvm_range *range;

	if (gpusvm->ops->range_alloc)
		range = gpusvm->ops->range_alloc(gpusvm);
	else
		range = kzalloc_obj(*range);

	if (!range)
		return ERR_PTR(-ENOMEM);

	kref_init(&range->refcount);
	range->gpusvm = gpusvm;
	range->notifier = notifier;
	range->itree.start = ALIGN_DOWN(fault_addr, chunk_size);
	range->itree.last = ALIGN(fault_addr + 1, chunk_size) - 1;
	INIT_LIST_HEAD(&range->entry);
	range->pages.notifier_seq = LONG_MAX;
	range->pages.flags.migrate_devmem = migrate_devmem ? 1 : 0;

	return range;
}

/**
 * drm_gpusvm_hmm_pfn_to_order() - Get the largest CPU mapping order.
 * @hmm_pfn: The current hmm_pfn.
 * @hmm_pfn_index: Index of the @hmm_pfn within the pfn array.
 * @npages: Number of pages within the pfn array i.e the hmm range size.
 *
 * To allow skipping PFNs with the same flags (like when they belong to
 * the same huge PTE) when looping over the pfn array, take a given a hmm_pfn,
 * and return the largest order that will fit inside the CPU PTE, but also
 * crucially accounting for the original hmm range boundaries.
 *
 * Return: The largest order that will safely fit within the size of the hmm_pfn
 * CPU PTE.
 */
static unsigned int drm_gpusvm_hmm_pfn_to_order(unsigned long hmm_pfn,
						unsigned long hmm_pfn_index,
						unsigned long npages)
{
	unsigned long size;

	size = 1UL << hmm_pfn_to_map_order(hmm_pfn);
	size -= (hmm_pfn & ~HMM_PFN_FLAGS) & (size - 1);
	hmm_pfn_index += size;
	if (hmm_pfn_index > npages)
		size -= (hmm_pfn_index - npages);

	return ilog2(size);
}

/**
 * drm_gpusvm_check_pages() - Check pages
 * @gpusvm: Pointer to the GPU SVM structure
 * @notifier: Pointer to the GPU SVM notifier structure
 * @start: Start address
 * @end: End address
 * @dev_private_owner: The device private page owner
 *
 * Check if pages between start and end have been faulted in on the CPU. Use to
 * prevent migration of pages without CPU backing store.
 *
 * Return: True if pages have been faulted into CPU, False otherwise
 */
static bool drm_gpusvm_check_pages(struct drm_gpusvm *gpusvm,
				   struct drm_gpusvm_notifier *notifier,
				   unsigned long start, unsigned long end,
				   void *dev_private_owner)
{
	struct hmm_range hmm_range = {
		.default_flags = 0,
		.notifier = &notifier->notifier,
		.start = start,
		.end = end,
		.dev_private_owner = dev_private_owner,
	};
	unsigned long timeout =
		jiffies + msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
	unsigned long *pfns;
	unsigned long npages = npages_in_range(start, end);
	int err, i;

	mmap_assert_locked(gpusvm->mm);

	pfns = kvmalloc_array(npages, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return false;

	hmm_range.notifier_seq = mmu_interval_read_begin(&notifier->notifier);
	hmm_range.hmm_pfns = pfns;

	while (true) {
		err = hmm_range_fault(&hmm_range);
		if (err == -EBUSY) {
			if (time_after(jiffies, timeout))
				break;

			hmm_range.notifier_seq =
				mmu_interval_read_begin(&notifier->notifier);
			continue;
		}
		break;
	}
	if (err)
		goto err_free;

	for (i = 0; i < npages;) {
		if (!(pfns[i] & HMM_PFN_VALID)) {
			err = -EFAULT;
			goto err_free;
		}
		i += 0x1 << drm_gpusvm_hmm_pfn_to_order(pfns[i], i, npages);
	}

err_free:
	kvfree(pfns);
	return err ? false : true;
}

/**
 * drm_gpusvm_scan_mm() - 检查 drm_gpusvm_range 的迁移状态
 *
 * 中文: 扫描 @range 对应的 CPU 地址空间，返回当前的迁移状态。
 * 使用 HMM 的 hmm_range_fault() 获取物理页面信息，然后分析每个页面
 * 的类型（系统内存、本地设备内存、其他设备内存）和分布情况。
 * 注意：该结果在函数返回后可能立即失效，仅作为建议性的状态检查。
 * 返回值指示了扫描结果，如全部在系统内存、全部在设备内存、混合等状态。
 *
 * @range: 要检查的 struct drm_gpusvm_range 指针
 * @dev_private_owner: 用于确定兼容设备私有页面的所有者
 * @pagemap: 用于特定 pagemap 检查的 struct dev_pagemap 指针
 *
 * Return: See &enum drm_gpusvm_scan_result.
 */
enum drm_gpusvm_scan_result drm_gpusvm_scan_mm(struct drm_gpusvm_range *range,
					       void *dev_private_owner,
					       const struct dev_pagemap *pagemap)
{
	struct mmu_interval_notifier *notifier = &range->notifier->notifier;
	unsigned long start = drm_gpusvm_range_start(range);
	unsigned long end = drm_gpusvm_range_end(range);
	struct hmm_range hmm_range = {
		.default_flags = 0,
		.notifier = notifier,
		.start = start,
		.end = end,
		.dev_private_owner = dev_private_owner,
	};
	unsigned long timeout =
		jiffies + msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
	enum drm_gpusvm_scan_result state = DRM_GPUSVM_SCAN_UNPOPULATED, new_state;
	unsigned long *pfns;
	unsigned long npages = npages_in_range(start, end);
	const struct dev_pagemap *other = NULL;
	int err, i;

	pfns = kvmalloc_array(npages, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return DRM_GPUSVM_SCAN_UNPOPULATED;

	hmm_range.hmm_pfns = pfns;

retry:
	hmm_range.notifier_seq = mmu_interval_read_begin(notifier);
	mmap_read_lock(range->gpusvm->mm);

	while (true) {
		err = hmm_range_fault(&hmm_range);
		if (err == -EBUSY) {
			if (time_after(jiffies, timeout))
				break;

			hmm_range.notifier_seq =
				mmu_interval_read_begin(notifier);
			continue;
		}
		break;
	}
	mmap_read_unlock(range->gpusvm->mm);
	if (err)
		goto err_free;

	drm_gpusvm_notifier_lock(range->gpusvm);
	if (mmu_interval_read_retry(notifier, hmm_range.notifier_seq)) {
		drm_gpusvm_notifier_unlock(range->gpusvm);
		goto retry;
	}

	for (i = 0; i < npages;) {
		struct page *page;
		const struct dev_pagemap *cur = NULL;

		if (!(pfns[i] & HMM_PFN_VALID)) {
			state = DRM_GPUSVM_SCAN_UNPOPULATED;
			break;
		}

		page = hmm_pfn_to_page(pfns[i]);
		if (is_device_private_page(page) ||
		    is_device_coherent_page(page))
			cur = page_pgmap(page);

		if (cur == pagemap) {
			new_state = DRM_GPUSVM_SCAN_EQUAL;
		} else if (cur && (cur == other || !other)) {
			new_state = DRM_GPUSVM_SCAN_OTHER;
			other = cur;
		} else if (cur) {
			new_state = DRM_GPUSVM_SCAN_MIXED_DEVICE;
		} else {
			new_state = DRM_GPUSVM_SCAN_SYSTEM;
		}

		/*
		 * TODO: Could use an array for state
		 * transitions, and caller might want it
		 * to bail early for some results.
		 */
		if (state == DRM_GPUSVM_SCAN_UNPOPULATED) {
			state = new_state;
		} else if (state != new_state) {
			if (new_state == DRM_GPUSVM_SCAN_SYSTEM ||
			    state == DRM_GPUSVM_SCAN_SYSTEM)
				state = DRM_GPUSVM_SCAN_MIXED;
			else if (state != DRM_GPUSVM_SCAN_MIXED)
				state = DRM_GPUSVM_SCAN_MIXED_DEVICE;
		}

		i += 1ul << drm_gpusvm_hmm_pfn_to_order(pfns[i], i, npages);
	}

	drm_gpusvm_notifier_unlock(range->gpusvm);

err_free:
	kvfree(pfns);
	return state;
}
EXPORT_SYMBOL(drm_gpusvm_scan_mm);

/**
 * drm_gpusvm_range_chunk_size() - Determine chunk size for GPU SVM range
 * @gpusvm: Pointer to the GPU SVM structure
 * @notifier: Pointer to the GPU SVM notifier structure
 * @vas: Pointer to the virtual memory area structure
 * @fault_addr: Fault address
 * @gpuva_start: Start address of GPUVA which mirrors CPU
 * @gpuva_end: End address of GPUVA which mirrors CPU
 * @check_pages_threshold: Check CPU pages for present threshold
 * @dev_private_owner: The device private page owner
 *
 * This function determines the chunk size for the GPU SVM range based on the
 * fault address, GPU SVM chunk sizes, existing GPU SVM ranges, and the virtual
 * memory area boundaries.
 *
 * Return: Chunk size on success, LONG_MAX on failure.
 */
static unsigned long
drm_gpusvm_range_chunk_size(struct drm_gpusvm *gpusvm,
			    struct drm_gpusvm_notifier *notifier,
			    struct vm_area_struct *vas,
			    unsigned long fault_addr,
			    unsigned long gpuva_start,
			    unsigned long gpuva_end,
			    unsigned long check_pages_threshold,
			    void *dev_private_owner)
{
	unsigned long start, end;
	int i = 0;

retry:
	for (; i < gpusvm->num_chunks; ++i) {
		start = ALIGN_DOWN(fault_addr, gpusvm->chunk_sizes[i]);
		end = ALIGN(fault_addr + 1, gpusvm->chunk_sizes[i]);

		if (start >= vas->vm_start && end <= vas->vm_end &&
		    start >= drm_gpusvm_notifier_start(notifier) &&
		    end <= drm_gpusvm_notifier_end(notifier) &&
		    start >= gpuva_start && end <= gpuva_end)
			break;
	}

	if (i == gpusvm->num_chunks)
		return LONG_MAX;

	/*
	 * If allocation more than page, ensure not to overlap with existing
	 * ranges.
	 */
	if (end - start != SZ_4K) {
		struct drm_gpusvm_range *range;

		range = drm_gpusvm_range_find(notifier, start, end);
		if (range) {
			++i;
			goto retry;
		}

		/*
		 * XXX: Only create range on pages CPU has faulted in. Without
		 * this check, or prefault, on BMG 'xe_exec_system_allocator --r
		 * process-many-malloc' fails. In the failure case, each process
		 * mallocs 16k but the CPU VMA is ~128k which results in 64k SVM
		 * ranges. When migrating the SVM ranges, some processes fail in
		 * drm_pagemap_migrate_to_devmem with 'migrate.cpages != npages'
		 * and then upon drm_gpusvm_range_get_pages device pages from
		 * other processes are collected + faulted in which creates all
		 * sorts of problems. Unsure exactly how this happening, also
		 * problem goes away if 'xe_exec_system_allocator --r
		 * process-many-malloc' mallocs at least 64k at a time.
		 */
		if (end - start <= check_pages_threshold &&
		    !drm_gpusvm_check_pages(gpusvm, notifier, start, end, dev_private_owner)) {
			++i;
			goto retry;
		}
	}

	return end - start;
}

#ifdef CONFIG_LOCKDEP
/**
 * drm_gpusvm_driver_lock_held() - Assert GPU SVM driver lock is held
 * @gpusvm: Pointer to the GPU SVM structure.
 *
 * Ensure driver lock is held.
 */
static void drm_gpusvm_driver_lock_held(struct drm_gpusvm *gpusvm)
{
	if ((gpusvm)->lock_dep_map)
		lockdep_assert(lock_is_held_type((gpusvm)->lock_dep_map, 0));
}
#else
static void drm_gpusvm_driver_lock_held(struct drm_gpusvm *gpusvm)
{
}
#endif

/**
 * drm_gpusvm_find_vma_start() - 查找范围内第一个 VMA 的起始地址
 *
 * 中文: 在指定的用户地址范围内查找第一个 VMA（虚拟内存区域）的起始地址。
 * 使用 find_vma_intersection() 在 mm_struct 中搜索与范围重叠的 VMA。
 * 如果没有找到 VMA，返回 ULONG_MAX。用于 GPU 缺页处理时确定可用的
 * CPU 地址空间边界。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @start: 用户地址范围起始（包含）
 * @end: 用户地址范围结束（不包含）
 *
 * Returns: The start address of first VMA within the provided range,
 * ULONG_MAX otherwise. Assumes start_addr < end_addr.
 */
unsigned long
drm_gpusvm_find_vma_start(struct drm_gpusvm *gpusvm,
			  unsigned long start,
			  unsigned long end)
{
	struct mm_struct *mm = gpusvm->mm;
	struct vm_area_struct *vma;
	unsigned long addr = ULONG_MAX;

	if (!mmget_not_zero(mm))
		return addr;

	mmap_read_lock(mm);

	vma = find_vma_intersection(mm, start, end);
	if (vma)
		addr =  vma->vm_start;

	mmap_read_unlock(mm);
	mmput(mm);

	return addr;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_find_vma_start);

/**
 * drm_gpusvm_range_find_or_insert() - 查找或插入 GPU SVM range
 *
 * 中文: 基于缺页地址查找或插入 GPU SVM range。如果不存在覆盖缺页地址的
 * notifier，则分配并插入新 notifier。然后在 notifier 中查找现有 range，
 * 如果未找到则计算最优块大小（chunk_size）并分配新的 range，插入到 notifier
 * 的红黑树和列表中。范围大小根据缺页地址、GPU VA 边界、VMA 边界和现有 range
 * 位置综合确定。调用者必须持有驱动锁以保护查找和插入操作。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @fault_addr: 缺页地址
 * @gpuva_start: GPUVA 起始地址（镜像 CPU）
 * @gpuva_end: GPUVA 结束地址（镜像 CPU）
 * @ctx: GPU SVM 上下文
 *
 * Return: Pointer to the GPU SVM range on success, ERR_PTR() on failure.
 */
struct drm_gpusvm_range *
drm_gpusvm_range_find_or_insert(struct drm_gpusvm *gpusvm,
				unsigned long fault_addr,
				unsigned long gpuva_start,
				unsigned long gpuva_end,
				const struct drm_gpusvm_ctx *ctx)
{
	struct drm_gpusvm_notifier *notifier;
	struct drm_gpusvm_range *range;
	struct mm_struct *mm = gpusvm->mm;
	struct vm_area_struct *vas;
	bool notifier_alloc = false;
	unsigned long chunk_size;
	int err;
	bool migrate_devmem;

	drm_gpusvm_driver_lock_held(gpusvm);

	if (fault_addr < gpusvm->mm_start ||
	    fault_addr > gpusvm->mm_start + gpusvm->mm_range)
		return ERR_PTR(-EINVAL);

	if (!mmget_not_zero(mm))
		return ERR_PTR(-EFAULT);

	notifier = drm_gpusvm_notifier_find(gpusvm, fault_addr, fault_addr + 1);
	if (!notifier) {
		notifier = drm_gpusvm_notifier_alloc(gpusvm, fault_addr);
		if (IS_ERR(notifier)) {
			err = PTR_ERR(notifier);
			goto err_mmunlock;
		}
		notifier_alloc = true;
		err = mmu_interval_notifier_insert(&notifier->notifier,
						   mm,
						   drm_gpusvm_notifier_start(notifier),
						   drm_gpusvm_notifier_size(notifier),
						   &drm_gpusvm_notifier_ops);
		if (err)
			goto err_notifier;
	}

	mmap_read_lock(mm);

	vas = vma_lookup(mm, fault_addr);
	if (!vas) {
		err = -ENOENT;
		goto err_notifier_remove;
	}

	if (!ctx->read_only && !(vas->vm_flags & VM_WRITE)) {
		err = -EPERM;
		goto err_notifier_remove;
	}

	range = drm_gpusvm_range_find(notifier, fault_addr, fault_addr + 1);
	if (range)
		goto out_mmunlock;
	/*
	 * XXX: Short-circuiting migration based on migrate_vma_* current
	 * limitations. If/when migrate_vma_* add more support, this logic will
	 * have to change.
	 */
	migrate_devmem = ctx->devmem_possible &&
		vma_is_anonymous(vas) && !is_vm_hugetlb_page(vas);

	chunk_size = drm_gpusvm_range_chunk_size(gpusvm, notifier, vas,
						 fault_addr, gpuva_start,
						 gpuva_end,
						 ctx->check_pages_threshold,
						 ctx->device_private_page_owner);
	if (chunk_size == LONG_MAX) {
		err = -EINVAL;
		goto err_notifier_remove;
	}

	range = drm_gpusvm_range_alloc(gpusvm, notifier, fault_addr, chunk_size,
				       migrate_devmem);
	if (IS_ERR(range)) {
		err = PTR_ERR(range);
		goto err_notifier_remove;
	}

	drm_gpusvm_range_insert(notifier, range);
	if (notifier_alloc)
		drm_gpusvm_notifier_insert(gpusvm, notifier);

out_mmunlock:
	mmap_read_unlock(mm);
	mmput(mm);

	return range;

err_notifier_remove:
	mmap_read_unlock(mm);
	if (notifier_alloc)
		mmu_interval_notifier_remove(&notifier->notifier);
err_notifier:
	if (notifier_alloc)
		drm_gpusvm_notifier_free(gpusvm, notifier);
err_mmunlock:
	mmput(mm);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_find_or_insert);

/**
 * __drm_gpusvm_unmap_pages() - Unmap pages associated with GPU SVM pages (internal)
 * @gpusvm: Pointer to the GPU SVM structure
 * @svm_pages: Pointer to the GPU SVM pages structure
 * @npages: Number of pages to unmap
 *
 * This function unmap pages associated with a GPU SVM pages struct. Assumes and
 * asserts correct locking is in place when called.
 */
static void __drm_gpusvm_unmap_pages(struct drm_gpusvm *gpusvm,
				     struct drm_gpusvm_pages *svm_pages,
				     unsigned long npages)
{
	struct drm_pagemap *dpagemap = svm_pages->dpagemap;
	struct device *dev = gpusvm->drm->dev;
	unsigned long i, j;

	lockdep_assert_held(&gpusvm->notifier_lock);

	if (svm_pages->flags.has_dma_mapping) {
		struct drm_gpusvm_pages_flags flags = {
			.__flags = svm_pages->flags.__flags,
		};

		for (i = 0, j = 0; i < npages; j++) {
			struct drm_pagemap_addr *addr = &svm_pages->dma_addr[j];

			if (addr->proto == DRM_INTERCONNECT_SYSTEM)
				dma_unmap_page(dev,
					       addr->addr,
					       PAGE_SIZE << addr->order,
					       addr->dir);
			else if (dpagemap && dpagemap->ops->device_unmap)
				dpagemap->ops->device_unmap(dpagemap,
							    dev, addr);
			i += 1 << addr->order;
		}

		/* WRITE_ONCE pairs with READ_ONCE for opportunistic checks */
		flags.has_devmem_pages = false;
		flags.has_dma_mapping = false;
		WRITE_ONCE(svm_pages->flags.__flags, flags.__flags);

		drm_pagemap_put(svm_pages->dpagemap);
		svm_pages->dpagemap = NULL;
	}
}

/**
 * __drm_gpusvm_free_pages() - Free dma array associated with GPU SVM pages
 * @gpusvm: Pointer to the GPU SVM structure
 * @svm_pages: Pointer to the GPU SVM pages structure
 *
 * This function frees the dma address array associated with a GPU SVM range.
 */
static void __drm_gpusvm_free_pages(struct drm_gpusvm *gpusvm,
				    struct drm_gpusvm_pages *svm_pages)
{
	lockdep_assert_held(&gpusvm->notifier_lock);

	if (svm_pages->dma_addr) {
		kvfree(svm_pages->dma_addr);
		svm_pages->dma_addr = NULL;
	}
}

/**
 * drm_gpusvm_free_pages() - 释放 GPU SVM pages 结构体关联的 DMA 映射
 *
 * 中文: 解除 GPU SVM pages 结构体关联页面的 DMA 映射并释放 DMA 地址数组。
 * 在 notifier_lock 保护下，依次调用 __drm_gpusvm_unmap_pages() 解除映射，
* 然后调用 __drm_gpusvm_free_pages() 释放 dma_addr 数组。
 * 此函数用于清理不再需要的页面映射。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @svm_pages: 指向 GPU SVM pages 结构体的指针
 * @npages: 映射的页面数量
 */
void drm_gpusvm_free_pages(struct drm_gpusvm *gpusvm,
			   struct drm_gpusvm_pages *svm_pages,
			   unsigned long npages)
{
	drm_gpusvm_notifier_lock(gpusvm);
	__drm_gpusvm_unmap_pages(gpusvm, svm_pages, npages);
	__drm_gpusvm_free_pages(gpusvm, svm_pages);
	drm_gpusvm_notifier_unlock(gpusvm);
}
EXPORT_SYMBOL_GPL(drm_gpusvm_free_pages);

/**
 * drm_gpusvm_range_remove() - 移除 GPU SVM range
 *
 * 中文: 移除指定的 GPU SVM range，解除其 DMA 映射，释放页面相关资源。
 * 如果移除后 notifier 中不再有 range，一并移除 notifier（包括从 mmu 区间
 * 通知系统中移除）。调用者必须持有驱动锁以保护操作。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @range: 要移除的 GPU SVM range
 */
void drm_gpusvm_range_remove(struct drm_gpusvm *gpusvm,
			     struct drm_gpusvm_range *range)
{
	unsigned long npages = npages_in_range(drm_gpusvm_range_start(range),
					       drm_gpusvm_range_end(range));
	struct drm_gpusvm_notifier *notifier;

	drm_gpusvm_driver_lock_held(gpusvm);

	notifier = drm_gpusvm_notifier_find(gpusvm,
					    drm_gpusvm_range_start(range),
					    drm_gpusvm_range_start(range) + 1);
	if (WARN_ON_ONCE(!notifier))
		return;

	drm_gpusvm_notifier_lock(gpusvm);
	__drm_gpusvm_unmap_pages(gpusvm, &range->pages, npages);
	__drm_gpusvm_free_pages(gpusvm, &range->pages);
	__drm_gpusvm_range_remove(notifier, range);
	drm_gpusvm_notifier_unlock(gpusvm);

	drm_gpusvm_range_put(range);

	if (RB_EMPTY_ROOT(&notifier->root.rb_root)) {
		if (!notifier->flags.removed)
			mmu_interval_notifier_remove(&notifier->notifier);
		drm_gpusvm_notifier_remove(gpusvm, notifier);
		drm_gpusvm_notifier_free(gpusvm, notifier);
	}
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_remove);

/**
 * drm_gpusvm_range_get() - 获取 GPU SVM range 的引用
 *
 * 中文: 递增指定 GPU SVM range 的引用计数。用于在使用 range 时确保其在
 * 期间不会被释放。每个 get 必须对应一个 put。
 *
 * @range: 指向 GPU SVM range 的指针
 *
 * Return: Pointer to the GPU SVM range.
 */
struct drm_gpusvm_range *
drm_gpusvm_range_get(struct drm_gpusvm_range *range)
{
	kref_get(&range->refcount);

	return range;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_get);

/**
 * drm_gpusvm_range_destroy() - Destroy GPU SVM range
 * @refcount: Pointer to the reference counter embedded in the GPU SVM range
 *
 * This function destroys the specified GPU SVM range when its reference count
 * reaches zero. If a custom range-free function is provided, it is invoked to
 * free the range; otherwise, the range is deallocated using kfree().
 */
static void drm_gpusvm_range_destroy(struct kref *refcount)
{
	struct drm_gpusvm_range *range =
		container_of(refcount, struct drm_gpusvm_range, refcount);
	struct drm_gpusvm *gpusvm = range->gpusvm;

	if (gpusvm->ops->range_free)
		gpusvm->ops->range_free(range);
	else
		kfree(range);
}

/**
 * drm_gpusvm_range_put() - 释放 GPU SVM range 的引用
 *
 * 中文: 递减指定 GPU SVM range 的引用计数，当计数归零时销毁 range。
 * 销毁时如果驱动提供了 range_free 回调则调用之，否则使用 kfree() 释放。
 * 此函数可能触发 range 的最终释放，因此调用后不应再访问 range 指针。
 *
 * @range: 指向 GPU SVM range 的指针
 */
void drm_gpusvm_range_put(struct drm_gpusvm_range *range)
{
	kref_put(&range->refcount, drm_gpusvm_range_destroy);
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_put);

/**
 * drm_gpusvm_pages_valid() - GPU SVM range pages valid
 * @gpusvm: Pointer to the GPU SVM structure
 * @svm_pages: Pointer to the GPU SVM pages structure
 *
 * This function determines if a GPU SVM range pages are valid. Expected be
 * called holding gpusvm->notifier_lock and as the last step before committing a
 * GPU binding. This is akin to a notifier seqno check in the HMM documentation
 * but due to wider notifiers (i.e., notifiers which span multiple ranges) this
 * function is required for finer grained checking (i.e., per range) if pages
 * are valid.
 *
 * Return: True if GPU SVM range has valid pages, False otherwise
 */
static bool drm_gpusvm_pages_valid(struct drm_gpusvm *gpusvm,
				   struct drm_gpusvm_pages *svm_pages)
{
	lockdep_assert_held(&gpusvm->notifier_lock);

	return svm_pages->flags.has_devmem_pages || svm_pages->flags.has_dma_mapping;
}

/**
 * drm_gpusvm_range_pages_valid() - GPU SVM range 页面是否有效
 *
 * 中文: 检查 GPU SVM range 的页面是否仍然有效。应在持有 gpusvm->notifier_lock
 * 时调用，并作为提交 GPU 绑定的最后一步。这类似于 HMM 文档中的 notifier
 * seqno 检查，但由于 notifier 可能覆盖多个 range，需要此函数提供更细粒度的
 * （按 range）有效性检查。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @range: 指向 GPU SVM range 结构体的指针
 *
 * Return: True if GPU SVM range has valid pages, False otherwise
 */
bool drm_gpusvm_range_pages_valid(struct drm_gpusvm *gpusvm,
				  struct drm_gpusvm_range *range)
{
	return drm_gpusvm_pages_valid(gpusvm, &range->pages);
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_pages_valid);

/**
 * drm_gpusvm_pages_valid_unlocked() - GPU SVM pages valid unlocked
 * @gpusvm: Pointer to the GPU SVM structure
 * @svm_pages: Pointer to the GPU SVM pages structure
 *
 * This function determines if a GPU SVM pages are valid. Expected be called
 * without holding gpusvm->notifier_lock.
 *
 * Return: True if GPU SVM pages are valid, False otherwise
 */
static bool drm_gpusvm_pages_valid_unlocked(struct drm_gpusvm *gpusvm,
					    struct drm_gpusvm_pages *svm_pages)
{
	bool pages_valid;

	if (!svm_pages->dma_addr)
		return false;

	drm_gpusvm_notifier_lock(gpusvm);
	pages_valid = drm_gpusvm_pages_valid(gpusvm, svm_pages);
	if (!pages_valid)
		__drm_gpusvm_free_pages(gpusvm, svm_pages);
	drm_gpusvm_notifier_unlock(gpusvm);

	return pages_valid;
}

/**
 * drm_gpusvm_get_pages() - 获取页面并填充 GPU SVM pages 结构体
 *
 * 中文: 获取 CPU 地址范围对应的物理页面并执行 DMA 映射。使用 HMM
 * （ Heterogeneous Memory Management）的 hmm_range_fault() 获取页面的
 * PFN 数组。然后对每个页面执行 DMA 映射——系统内存页面使用 dma_map_page()
 * 映射，设备私有页面使用驱动提供的 device_map() 回调映射。
 * 所有 DMA 映射在 notifier_lock 保护下执行，以避免与 notifier 回调并发
 * 访问已释放的页面。该函数支持多个设备内存分配来源的页面，支持混合模式
 * 检查。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @svm_pages: 要填充的 SVM pages 结构体，包含 DMA 地址
 * @mm: CPU 范围对应的 mm_struct
 * @notifier: CPU 范围对应的 mmu_interval_notifier
 * @pages_start: 页面的 CPU 起始地址
 * @pages_end: 页面的 CPU 结束地址（不包含）
 * @ctx: GPU SVM 上下文
 *
 * Return: 0 on success, negative error code on failure.
 */
int drm_gpusvm_get_pages(struct drm_gpusvm *gpusvm,
			 struct drm_gpusvm_pages *svm_pages,
			 struct mm_struct *mm,
			 struct mmu_interval_notifier *notifier,
			 unsigned long pages_start, unsigned long pages_end,
			 const struct drm_gpusvm_ctx *ctx)
{
	struct hmm_range hmm_range = {
		.default_flags = HMM_PFN_REQ_FAULT | (ctx->read_only ? 0 :
			HMM_PFN_REQ_WRITE),
		.notifier = notifier,
		.start = pages_start,
		.end = pages_end,
		.dev_private_owner = ctx->device_private_page_owner,
	};
	void *zdd;
	unsigned long timeout =
		jiffies + msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
	unsigned long i, j;
	unsigned long npages = npages_in_range(pages_start, pages_end);
	unsigned long num_dma_mapped;
	unsigned int order = 0;
	unsigned long *pfns;
	int err = 0;
	struct dev_pagemap *pagemap;
	struct drm_pagemap *dpagemap;
	struct drm_gpusvm_pages_flags flags;
	enum dma_data_direction dma_dir = ctx->read_only ? DMA_TO_DEVICE :
							   DMA_BIDIRECTIONAL;

retry:
	if (time_after(jiffies, timeout))
		return -EBUSY;

	hmm_range.notifier_seq = mmu_interval_read_begin(notifier);
	if (drm_gpusvm_pages_valid_unlocked(gpusvm, svm_pages))
		goto set_seqno;

	pfns = kvmalloc_array(npages, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return -ENOMEM;

	if (!mmget_not_zero(mm)) {
		err = -EFAULT;
		goto err_free;
	}

	hmm_range.hmm_pfns = pfns;
	while (true) {
		mmap_read_lock(mm);
		err = hmm_range_fault(&hmm_range);
		mmap_read_unlock(mm);

		if (err == -EBUSY) {
			if (time_after(jiffies, timeout))
				break;

			hmm_range.notifier_seq =
				mmu_interval_read_begin(notifier);
			continue;
		}
		break;
	}
	mmput(mm);
	if (err)
		goto err_free;

map_pages:
	/*
	 * Perform all dma mappings under the notifier lock to not
	 * access freed pages. A notifier will either block on
	 * the notifier lock or unmap dma.
	 */
	drm_gpusvm_notifier_lock(gpusvm);

	flags.__flags = svm_pages->flags.__flags;
	if (flags.unmapped) {
		drm_gpusvm_notifier_unlock(gpusvm);
		err = -EFAULT;
		goto err_free;
	}

	if (mmu_interval_read_retry(notifier, hmm_range.notifier_seq)) {
		drm_gpusvm_notifier_unlock(gpusvm);
		kvfree(pfns);
		goto retry;
	}

	if (!svm_pages->dma_addr) {
		/* Unlock and restart mapping to allocate memory. */
		drm_gpusvm_notifier_unlock(gpusvm);
		svm_pages->dma_addr =
			kvmalloc_objs(*svm_pages->dma_addr, npages);
		if (!svm_pages->dma_addr) {
			err = -ENOMEM;
			goto err_free;
		}
		goto map_pages;
	}

	zdd = NULL;
	pagemap = NULL;
	num_dma_mapped = 0;
	for (i = 0, j = 0; i < npages; ++j) {
		struct page *page = hmm_pfn_to_page(pfns[i]);

		order = drm_gpusvm_hmm_pfn_to_order(pfns[i], i, npages);
		if (is_device_private_page(page) ||
		    is_device_coherent_page(page)) {
			struct drm_pagemap_zdd *__zdd =
				drm_pagemap_page_zone_device_data(page);

			if (!ctx->allow_mixed &&
			    zdd != __zdd && i > 0) {
				err = -EOPNOTSUPP;
				goto err_unmap;
			}
			zdd = __zdd;
			if (pagemap != page_pgmap(page)) {
				if (pagemap) {
					err = -EOPNOTSUPP;
					goto err_unmap;
				}

				pagemap = page_pgmap(page);
				dpagemap = drm_pagemap_page_to_dpagemap(page);
				if (drm_WARN_ON(gpusvm->drm, !dpagemap)) {
					/*
					 * Raced. This is not supposed to happen
					 * since hmm_range_fault() should've migrated
					 * this page to system.
					 */
					err = -EAGAIN;
					goto err_unmap;
				}
			}
			svm_pages->dma_addr[j] =
				dpagemap->ops->device_map(dpagemap,
							  gpusvm->drm->dev,
							  page, order,
							  dma_dir);
			if (dma_mapping_error(gpusvm->drm->dev,
					      svm_pages->dma_addr[j].addr)) {
				err = -EFAULT;
				goto err_unmap;
			}
		} else {
			dma_addr_t addr;

			if (is_zone_device_page(page) ||
			    (pagemap && !ctx->allow_mixed)) {
				err = -EOPNOTSUPP;
				goto err_unmap;
			}

			if (ctx->devmem_only) {
				err = -EFAULT;
				goto err_unmap;
			}

			addr = dma_map_page(gpusvm->drm->dev,
					    page, 0,
					    PAGE_SIZE << order,
					    dma_dir);
			if (dma_mapping_error(gpusvm->drm->dev, addr)) {
				err = -EFAULT;
				goto err_unmap;
			}

			svm_pages->dma_addr[j] = drm_pagemap_addr_encode
				(addr, DRM_INTERCONNECT_SYSTEM, order,
				 dma_dir);
		}
		i += 1 << order;
		num_dma_mapped = i;
		flags.has_dma_mapping = true;
	}

	if (pagemap) {
		flags.has_devmem_pages = true;
		drm_pagemap_get(dpagemap);
		drm_pagemap_put(svm_pages->dpagemap);
		svm_pages->dpagemap = dpagemap;
	}

	/* WRITE_ONCE pairs with READ_ONCE for opportunistic checks */
	WRITE_ONCE(svm_pages->flags.__flags, flags.__flags);

	drm_gpusvm_notifier_unlock(gpusvm);
	kvfree(pfns);
set_seqno:
	svm_pages->notifier_seq = hmm_range.notifier_seq;

	return 0;

err_unmap:
	svm_pages->flags.has_dma_mapping = true;
	__drm_gpusvm_unmap_pages(gpusvm, svm_pages, num_dma_mapped);
	drm_gpusvm_notifier_unlock(gpusvm);
err_free:
	kvfree(pfns);
	if (err == -EAGAIN)
		goto retry;
	return err;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_get_pages);

/**
 * drm_gpusvm_range_get_pages() - 获取 GPU SVM range 的页面
 *
 * 中文: 获取 GPU SVM range 对应的 CPU 页面并确保它们已为 DMA 访问完成映射。
 * 该函数是 drm_gpusvm_get_pages() 的便捷封装，自动传入 range 的起始/结束
 * 地址及其关联的 mm 和 notifier。用于 GPU 缺页处理流程中，在建立 GPU 页表
 * 映射前获取页面的 DMA 地址。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @range: 指向 GPU SVM range 结构体的指针
 * @ctx: GPU SVM 上下文
 *
 * Return: 0 on success, negative error code on failure.
 */
int drm_gpusvm_range_get_pages(struct drm_gpusvm *gpusvm,
			       struct drm_gpusvm_range *range,
			       const struct drm_gpusvm_ctx *ctx)
{
	return drm_gpusvm_get_pages(gpusvm, &range->pages, gpusvm->mm,
				    &range->notifier->notifier,
				    drm_gpusvm_range_start(range),
				    drm_gpusvm_range_end(range), ctx);
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_get_pages);

/**
 * drm_gpusvm_unmap_pages() - 解除 GPU SVM pages 的 DMA 映射
 *
 * 中文: 解除 GPU SVM pages 结构体关联页面的 DMA 映射。如果在 notifier 回调
 * 中调用（ctx->in_notifier 为 true），调用者需已持有 notifier_lock 写锁；
 * 否则自动获取 notifier_lock 读锁。此函数必须在对应 notifier 的 invalidate()
 * 回调中调用，以确保 IOMMU 安全模型——在 CPU 修改页面后，GPU 无法再通过
 * 旧 DMA 映射访问这些页面。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @svm_pages: 指向 GPU SVM pages 结构体的指针
 * @npages: @svm_pages 中的页面数量
 * @ctx: GPU SVM 上下文
 */
void drm_gpusvm_unmap_pages(struct drm_gpusvm *gpusvm,
			    struct drm_gpusvm_pages *svm_pages,
			    unsigned long npages,
			    const struct drm_gpusvm_ctx *ctx)
{
	if (ctx->in_notifier)
		lockdep_assert_held_write(&gpusvm->notifier_lock);
	else
		drm_gpusvm_notifier_lock(gpusvm);

	__drm_gpusvm_unmap_pages(gpusvm, svm_pages, npages);

	if (!ctx->in_notifier)
		drm_gpusvm_notifier_unlock(gpusvm);
}
EXPORT_SYMBOL_GPL(drm_gpusvm_unmap_pages);

/**
 * drm_gpusvm_range_unmap_pages() - 解除 GPU SVM range 关联页面的 DMA 映射
 *
 * 中文: drm_gpusvm_unmap_pages() 的 range 封装版本，用于解除指定 range
 * 关联页面的 DMA 映射。必须在 gpusvm->ops->invalidate 中对每个附加到
 * notifier 的 GPU SVM range 调用此函数，以维护 IOMMU 安全模型。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @range: 指向 GPU SVM range 结构体的指针
 * @ctx: GPU SVM 上下文
 */
void drm_gpusvm_range_unmap_pages(struct drm_gpusvm *gpusvm,
				  struct drm_gpusvm_range *range,
				  const struct drm_gpusvm_ctx *ctx)
{
	unsigned long npages = npages_in_range(drm_gpusvm_range_start(range),
					       drm_gpusvm_range_end(range));

	return drm_gpusvm_unmap_pages(gpusvm, &range->pages, npages, ctx);
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_unmap_pages);

/**
 * drm_gpusvm_range_evict() - 驱逐 GPU SVM range
 *
 * 中文: 将指定 GPU SVM range 从设备内存驱逐回系统内存。使用 HMM 的
 * hmm_range_fault() 触发 CPU 页面错误，将设备私有页面迁移回系统 RAM。
 * 此函数在垃圾回收流程中使用，当 range 被部分取消映射或需要将设备内存
 * 页面迁回时调用。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @range: 要驱逐的 GPU SVM range
 *
 * Return: 0 on success, a negative error code on failure.
 */
int drm_gpusvm_range_evict(struct drm_gpusvm *gpusvm,
			   struct drm_gpusvm_range *range)
{
	struct mmu_interval_notifier *notifier = &range->notifier->notifier;
	struct hmm_range hmm_range = {
		.default_flags = HMM_PFN_REQ_FAULT,
		.notifier = notifier,
		.start = drm_gpusvm_range_start(range),
		.end = drm_gpusvm_range_end(range),
		.dev_private_owner = NULL,
	};
	unsigned long timeout =
		jiffies + msecs_to_jiffies(HMM_RANGE_DEFAULT_TIMEOUT);
	unsigned long *pfns;
	unsigned long npages = npages_in_range(drm_gpusvm_range_start(range),
					       drm_gpusvm_range_end(range));
	int err = 0;
	struct mm_struct *mm = gpusvm->mm;

	if (!mmget_not_zero(mm))
		return -EFAULT;

	pfns = kvmalloc_array(npages, sizeof(*pfns), GFP_KERNEL);
	if (!pfns)
		return -ENOMEM;

	hmm_range.hmm_pfns = pfns;
	while (!time_after(jiffies, timeout)) {
		hmm_range.notifier_seq = mmu_interval_read_begin(notifier);
		if (time_after(jiffies, timeout)) {
			err = -ETIME;
			break;
		}

		mmap_read_lock(mm);
		err = hmm_range_fault(&hmm_range);
		mmap_read_unlock(mm);
		if (err != -EBUSY)
			break;
	}

	kvfree(pfns);
	mmput(mm);

	return err;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_evict);

/**
 * drm_gpusvm_has_mapping() - 检查 GPU SVM 是否具有指定地址范围的映射
 *
 * 中文: 遍历所有与指定地址范围重叠的 notifier，检查是否存在任何 range
 * 覆盖该范围。用于判断某个 CPU 地址范围是否已有 GPU SVM 映射，
 * 避免重复创建映射。
 *
 * @gpusvm: 指向 GPU SVM 结构体的指针
 * @start: 起始地址
 * @end: 结束地址
 *
 * Return: True if GPU SVM has mapping, False otherwise
 */
bool drm_gpusvm_has_mapping(struct drm_gpusvm *gpusvm, unsigned long start,
			    unsigned long end)
{
	struct drm_gpusvm_notifier *notifier;

	drm_gpusvm_for_each_notifier(notifier, gpusvm, start, end) {
		struct drm_gpusvm_range *range = NULL;

		drm_gpusvm_for_each_range(range, notifier, start, end)
			return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_has_mapping);

/**
 * drm_gpusvm_range_set_unmapped() - 将 GPU SVM range 标记为已取消映射
 *
 * 中文: 将 GPU SVM range 标记为 unmapped 状态。如果 range 仅部分位于
 * MMU notifier 指定的范围内，还会设置 partial_unmap 标志，表示该 range
 * 被部分取消映射。partial_unmap 标志在垃圾回收时用于决定是否需要将剩余
 * 的设备内存页面迁移回系统 RAM。调用者需持有 notifier_lock 写锁。
 *
 * @range: 指向 GPU SVM range 结构体的指针
 * @mmu_range: 指向 MMU notifier range 结构体的指针
 */
void drm_gpusvm_range_set_unmapped(struct drm_gpusvm_range *range,
				   const struct mmu_notifier_range *mmu_range)
{
	lockdep_assert_held_write(&range->gpusvm->notifier_lock);

	range->pages.flags.unmapped = true;
	if (drm_gpusvm_range_start(range) < mmu_range->start ||
	    drm_gpusvm_range_end(range) > mmu_range->end)
		range->pages.flags.partial_unmap = true;
}
EXPORT_SYMBOL_GPL(drm_gpusvm_range_set_unmapped);

MODULE_DESCRIPTION("DRM GPUSVM");
MODULE_LICENSE("GPL");
