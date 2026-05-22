// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright © 2024-2025 Intel Corporation
 */

/**
 * DOC: DRM 页面映射层概述 (中文)
 *
 * 该文件实现了 DRM 页面映射（pagemap）层，扩展了内核的 dev_pagemap 功能，
 * 为 GPU 设备内存管理提供辅助支持。核心功能包括：
 *
 * 1. 设备内存分配与迁移：支持在设备内存和系统 RAM 之间双向迁移内存页面。
 *    迁移粒度通常遵循 GPU SVM range 请求，支持分配设备内存、填充 PFN、
 *    以及通过驱动提供的 copy_to_devmem/copy_to_ram 回调进行数据复制。
 *
 * 2. 虚拟地址范围填充：提供 drm_pagemap_populate_mm() 将 CPU 虚拟地址范围
 *    填充为设备内存页面，支持指定时间片以防止迁移活锁。
 *
 * 3. 设备内存驱逐：drm_pagemap_evict_to_ram() 将设备内存分配的全部页面
 *    迁移回系统 RAM，在设备卸载时使用。
 *
 * 4. 设备内存分配生命周期管理：通过 drm_pagemap_devmem_init() 初始化设备
 *    内存分配结构体，包含设备引用、内存操作回调、预迁移 fence 等。
 *
 * 关键组件包括：
 *   - struct drm_pagemap：封装了 dev_pagemap、drm_device 引用和操作回调
 *   - struct drm_pagemap_devmem：设备内存分配描述，包含迁移所需信息
 *   - struct drm_pagemap_zdd：页面 zone_device_data 包装器，支持异步释放
 */

#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/migrate.h>
#include <linux/pagemap.h>
#include <drm/drm_drv.h>
#include <drm/drm_pagemap.h>
#include <drm/drm_pagemap_util.h>
#include <drm/drm_print.h>

/**
 * DOC: Overview
 *
 * The DRM pagemap layer is intended to augment the dev_pagemap functionality by
 * providing a way to populate a struct mm_struct virtual range with device
 * private pages and to provide helpers to abstract device memory allocations,
 * to migrate memory back and forth between device memory and system RAM and
 * to handle access (and in the future migration) between devices implementing
 * a fast interconnect that is not necessarily visible to the rest of the
 * system.
 *
 * Typically the DRM pagemap receives requests from one or more DRM GPU SVM
 * instances to populate struct mm_struct virtual ranges with memory, and the
 * migration is best effort only and may thus fail. The implementation should
 * also handle device unbinding by blocking (return an -ENODEV) error for new
 * population requests and after that migrate all device pages to system ram.
 */

/**
 * DOC: Migration
 *
 * Migration granularity typically follows the GPU SVM range requests, but
 * if there are clashes, due to races or due to the fact that multiple GPU
 * SVM instances have different views of the ranges used, and because of that
 * parts of a requested range is already present in the requested device memory,
 * the implementation has a variety of options. It can fail and it can choose
 * to populate only the part of the range that isn't already in device memory,
 * and it can evict the range to system before trying to migrate. Ideally an
 * implementation would just try to migrate the missing part of the range and
 * allocate just enough memory to do so.
 *
 * When migrating to system memory as a response to a cpu fault or a device
 * memory eviction request, currently a full device memory allocation is
 * migrated back to system. Moving forward this might need improvement for
 * situations where a single page needs bouncing between system memory and
 * device memory due to, for example, atomic operations.
 *
 * Key DRM pagemap components:
 *
 * - Device Memory Allocations:
 *      Embedded structure containing enough information for the drm_pagemap to
 *      migrate to / from device memory.
 *
 * - Device Memory Operations:
 *      Define the interface for driver-specific device memory operations
 *      release memory, populate pfns, and copy to / from device memory.
 */

/**
 * struct drm_pagemap_zdd - GPU SVM zone device data
 *
 * @refcount: Reference count for the zdd
 * @devmem_allocation: device memory allocation
 * @dpagemap: Refcounted pointer to the underlying struct drm_pagemap.
 *
 * This structure serves as a generic wrapper installed in
 * page->zone_device_data. It provides infrastructure for looking up a device
 * memory allocation upon CPU page fault and asynchronously releasing device
 * memory once the CPU has no page references. Asynchronous release is useful
 * because CPU page references can be dropped in IRQ contexts, while releasing
 * device memory likely requires sleeping locks.
 */
struct drm_pagemap_zdd {
	struct kref refcount;
	struct drm_pagemap_devmem *devmem_allocation;
	struct drm_pagemap *dpagemap;
};

/**
 * drm_pagemap_zdd_alloc() - Allocate a zdd structure.
 * @dpagemap: Pointer to the underlying struct drm_pagemap.
 *
 * This function allocates and initializes a new zdd structure. It sets up the
 * reference count and initializes the destroy work.
 *
 * Return: Pointer to the allocated zdd on success, ERR_PTR() on failure.
 */
static struct drm_pagemap_zdd *
drm_pagemap_zdd_alloc(struct drm_pagemap *dpagemap)
{
	struct drm_pagemap_zdd *zdd;

	zdd = kmalloc_obj(*zdd);
	if (!zdd)
		return NULL;

	kref_init(&zdd->refcount);
	zdd->devmem_allocation = NULL;
	zdd->dpagemap = drm_pagemap_get(dpagemap);

	return zdd;
}

/**
 * drm_pagemap_zdd_get() - Get a reference to a zdd structure.
 * @zdd: Pointer to the zdd structure.
 *
 * This function increments the reference count of the provided zdd structure.
 *
 * Return: Pointer to the zdd structure.
 */
static struct drm_pagemap_zdd *drm_pagemap_zdd_get(struct drm_pagemap_zdd *zdd)
{
	kref_get(&zdd->refcount);
	return zdd;
}

/**
 * drm_pagemap_zdd_destroy() - Destroy a zdd structure.
 * @ref: Pointer to the reference count structure.
 *
 * This function queues the destroy_work of the zdd for asynchronous destruction.
 */
static void drm_pagemap_zdd_destroy(struct kref *ref)
{
	struct drm_pagemap_zdd *zdd =
		container_of(ref, struct drm_pagemap_zdd, refcount);
	struct drm_pagemap_devmem *devmem = zdd->devmem_allocation;
	struct drm_pagemap *dpagemap = zdd->dpagemap;

	if (devmem) {
		complete_all(&devmem->detached);
		if (devmem->ops->devmem_release)
			devmem->ops->devmem_release(devmem);
	}
	kfree(zdd);
	drm_pagemap_put(dpagemap);
}

/**
 * drm_pagemap_zdd_put() - Put a zdd reference.
 * @zdd: Pointer to the zdd structure.
 *
 * This function decrements the reference count of the provided zdd structure
 * and schedules its destruction if the count drops to zero.
 */
static void drm_pagemap_zdd_put(struct drm_pagemap_zdd *zdd)
{
	kref_put(&zdd->refcount, drm_pagemap_zdd_destroy);
}

/**
 * drm_pagemap_migration_unlock_put_folio() - Put a migration folio
 * @folio: Pointer to the folio to put
 *
 * This function unlocks and puts a folio.
 */
static void drm_pagemap_migration_unlock_put_folio(struct folio *folio)
{
	folio_unlock(folio);
	folio_put(folio);
}

/**
 * drm_pagemap_migration_unlock_put_pages() - Put migration pages
 * @npages: Number of pages
 * @migrate_pfn: Array of migrate page frame numbers
 *
 * This function unlocks and puts an array of pages.
 */
static void drm_pagemap_migration_unlock_put_pages(unsigned long npages,
						   unsigned long *migrate_pfn)
{
	unsigned long i;

	for (i = 0; i < npages;) {
		struct page *page;
		struct folio *folio;
		unsigned int order = 0;

		if (!migrate_pfn[i])
			goto next;

		page = migrate_pfn_to_page(migrate_pfn[i]);
		folio = page_folio(page);
		order = folio_order(folio);

		drm_pagemap_migration_unlock_put_folio(folio);
		migrate_pfn[i] = 0;

next:
		i += NR_PAGES(order);
	}
}

/**
 * drm_pagemap_get_devmem_page() - Get a reference to a device memory page
 * @page: Pointer to the page
 * @order: Order
 * @zdd: Pointer to the GPU SVM zone device data
 *
 * This function associates the given page with the specified GPU SVM zone
 * device data and initializes it for zone device usage.
 */
static void drm_pagemap_get_devmem_page(struct page *page,
					unsigned int order,
					struct drm_pagemap_zdd *zdd)
{
	zone_device_folio_init((struct folio *)page, zdd->dpagemap->pagemap,
			       order);
	folio_set_zone_device_data(page_folio(page), drm_pagemap_zdd_get(zdd));
}

/**
 * drm_pagemap_migrate_map_pages() - Map migration pages for GPU SVM migration
 * @dev: The device performing the migration.
 * @local_dpagemap: The drm_pagemap local to the migrating device.
 * @pagemap_addr: Array to store DMA information corresponding to mapped pages.
 * @migrate_pfn: Array of page frame numbers of system pages or peer pages to map.
 * @npages: Number of system pages or peer pages to map.
 * @dir: Direction of data transfer (e.g., DMA_BIDIRECTIONAL)
 * @mdetails: Details governing the migration behaviour.
 *
 * This function maps pages of memory for migration usage in GPU SVM. It
 * iterates over each page frame number provided in @migrate_pfn, maps the
 * corresponding page, and stores the DMA address in the provided @dma_addr
 * array.
 *
 * Returns: 0 on success, -EFAULT if an error occurs during mapping.
 */
static int drm_pagemap_migrate_map_pages(struct device *dev,
					 struct drm_pagemap *local_dpagemap,
					 struct drm_pagemap_addr *pagemap_addr,
					 unsigned long *migrate_pfn,
					 unsigned long npages,
					 enum dma_data_direction dir,
					 const struct drm_pagemap_migrate_details *mdetails)
{
	unsigned long num_peer_pages = 0, num_local_pages = 0, i;

	for (i = 0; i < npages;) {
		struct page *page = migrate_pfn_to_page(migrate_pfn[i]);
		dma_addr_t dma_addr;
		struct folio *folio;
		unsigned int order = 0;

		if (!page)
			goto next;

		folio = page_folio(page);
		order = folio_order(folio);

		if (is_device_private_page(page)) {
			struct drm_pagemap_zdd *zdd = drm_pagemap_page_zone_device_data(page);
			struct drm_pagemap *dpagemap = zdd->dpagemap;
			struct drm_pagemap_addr addr;

			if (dpagemap == local_dpagemap) {
				if (!mdetails->can_migrate_same_pagemap)
					goto next;

				num_local_pages += NR_PAGES(order);
			} else {
				num_peer_pages += NR_PAGES(order);
			}

			addr = dpagemap->ops->device_map(dpagemap, dev, page, order, dir);
			if (dma_mapping_error(dev, addr.addr))
				return -EFAULT;

			pagemap_addr[i] = addr;
		} else {
			dma_addr = dma_map_page(dev, page, 0, page_size(page), dir);
			if (dma_mapping_error(dev, dma_addr))
				return -EFAULT;

			pagemap_addr[i] =
				drm_pagemap_addr_encode(dma_addr,
							DRM_INTERCONNECT_SYSTEM,
							order, dir);
		}

next:
		i += NR_PAGES(order);
	}

	if (num_peer_pages)
		drm_dbg(local_dpagemap->drm, "Migrating %lu peer pages over interconnect.\n",
			num_peer_pages);
	if (num_local_pages)
		drm_dbg(local_dpagemap->drm, "Migrating %lu local pages over interconnect.\n",
			num_local_pages);

	return 0;
}

/**
 * drm_pagemap_migrate_unmap_pages() - Unmap pages previously mapped for GPU SVM migration
 * @dev: The device for which the pages were mapped
 * @migrate_pfn: Array of migrate pfns set up for the mapped pages. Used to
 * determine the drm_pagemap of a peer device private page.
 * @pagemap_addr: Array of DMA information corresponding to mapped pages
 * @npages: Number of pages to unmap
 * @dir: Direction of data transfer (e.g., DMA_BIDIRECTIONAL)
 *
 * This function unmaps previously mapped pages of memory for GPU Shared Virtual
 * Memory (SVM). It iterates over each DMA address provided in @dma_addr, checks
 * if it's valid and not already unmapped, and unmaps the corresponding page.
 */
static void drm_pagemap_migrate_unmap_pages(struct device *dev,
					    struct drm_pagemap_addr *pagemap_addr,
					    unsigned long *migrate_pfn,
					    unsigned long npages,
					    enum dma_data_direction dir)
{
	unsigned long i;

	for (i = 0; i < npages;) {
		struct page *page = migrate_pfn_to_page(migrate_pfn[i]);

		if (!page || !pagemap_addr[i].addr || dma_mapping_error(dev, pagemap_addr[i].addr))
			goto next;

		if (is_zone_device_page(page)) {
			struct drm_pagemap_zdd *zdd = drm_pagemap_page_zone_device_data(page);
			struct drm_pagemap *dpagemap = zdd->dpagemap;

			dpagemap->ops->device_unmap(dpagemap, dev, &pagemap_addr[i]);
		} else {
			dma_unmap_page(dev, pagemap_addr[i].addr,
				       PAGE_SIZE << pagemap_addr[i].order, dir);
		}

next:
		i += NR_PAGES(pagemap_addr[i].order);
	}
}

static unsigned long
npages_in_range(unsigned long start, unsigned long end)
{
	return (end - start) >> PAGE_SHIFT;
}

static int
drm_pagemap_migrate_remote_to_local(struct drm_pagemap_devmem *devmem,
				    struct device *remote_device,
				    struct drm_pagemap *remote_dpagemap,
				    unsigned long local_pfns[],
				    struct page *remote_pages[],
				    struct drm_pagemap_addr pagemap_addr[],
				    unsigned long npages,
				    const struct drm_pagemap_devmem_ops *ops,
				    const struct drm_pagemap_migrate_details *mdetails)

{
	int err = drm_pagemap_migrate_map_pages(remote_device, remote_dpagemap,
						pagemap_addr, local_pfns,
						npages, DMA_FROM_DEVICE, mdetails);

	if (err)
		goto out;

	err = ops->copy_to_ram(remote_pages, pagemap_addr, npages,
			       devmem->pre_migrate_fence);
out:
	drm_pagemap_migrate_unmap_pages(remote_device, pagemap_addr, local_pfns,
					npages, DMA_FROM_DEVICE);
	return err;
}

static int
drm_pagemap_migrate_sys_to_dev(struct drm_pagemap_devmem *devmem,
			       unsigned long sys_pfns[],
			       struct page *local_pages[],
			       struct drm_pagemap_addr pagemap_addr[],
			       unsigned long npages,
			       const struct drm_pagemap_devmem_ops *ops,
			       const struct drm_pagemap_migrate_details *mdetails)
{
	int err = drm_pagemap_migrate_map_pages(devmem->dev, devmem->dpagemap,
						pagemap_addr, sys_pfns, npages,
						DMA_TO_DEVICE, mdetails);

	if (err)
		goto out;

	err = ops->copy_to_devmem(local_pages, pagemap_addr, npages,
				  devmem->pre_migrate_fence);
out:
	drm_pagemap_migrate_unmap_pages(devmem->dev, pagemap_addr, sys_pfns, npages,
					DMA_TO_DEVICE);
	return err;
}

/**
 * struct migrate_range_loc - Cursor into the loop over migrate_pfns for migrating to
 * device.
 * @start: The current loop index.
 * @device: migrating device.
 * @dpagemap: Pointer to struct drm_pagemap used by the migrating device.
 * @ops: The copy ops to be used for the migrating device.
 */
struct migrate_range_loc {
	unsigned long start;
	struct device *device;
	struct drm_pagemap *dpagemap;
	const struct drm_pagemap_devmem_ops *ops;
};

static int drm_pagemap_migrate_range(struct drm_pagemap_devmem *devmem,
				     unsigned long src_pfns[],
				     unsigned long dst_pfns[],
				     struct page *pages[],
				     struct drm_pagemap_addr pagemap_addr[],
				     struct migrate_range_loc *last,
				     const struct migrate_range_loc *cur,
				     const struct drm_pagemap_migrate_details *mdetails)
{
	int ret = 0;

	if (cur->start == 0)
		goto out;

	if (cur->start <= last->start)
		return 0;

	if (cur->dpagemap == last->dpagemap && cur->ops == last->ops)
		return 0;

	if (last->dpagemap)
		ret = drm_pagemap_migrate_remote_to_local(devmem,
							  last->device,
							  last->dpagemap,
							  &dst_pfns[last->start],
							  &pages[last->start],
							  &pagemap_addr[last->start],
							  cur->start - last->start,
							  last->ops, mdetails);

	else
		ret = drm_pagemap_migrate_sys_to_dev(devmem,
						     &src_pfns[last->start],
						     &pages[last->start],
						     &pagemap_addr[last->start],
						     cur->start - last->start,
						     last->ops, mdetails);

out:
	*last = *cur;
	return ret;
}

/**
 * drm_pagemap_cpages() - Count collected pages
 * @migrate_pfn: Array of migrate_pfn entries to account
 * @npages: Number of entries in @migrate_pfn
 *
 * Compute the total number of minimum-sized pages represented by the
 * collected entries in @migrate_pfn. The total is derived from the
 * order encoded in each entry.
 *
 * Return: Total number of minimum-sized pages.
 */
static int drm_pagemap_cpages(unsigned long *migrate_pfn, unsigned long npages)
{
	unsigned long i, cpages = 0;

	for (i = 0; i < npages;) {
		struct page *page = migrate_pfn_to_page(migrate_pfn[i]);
		struct folio *folio;
		unsigned int order = 0;

		if (page) {
			folio = page_folio(page);
			order = folio_order(folio);
			cpages += NR_PAGES(order);
		} else if (migrate_pfn[i] & MIGRATE_PFN_COMPOUND) {
			order = HPAGE_PMD_ORDER;
			cpages += NR_PAGES(order);
		}

		i += NR_PAGES(order);
	}

	return cpages;
}

/**
 * drm_pagemap_migrate_to_devmem() - 将 mm_struct 范围的虚拟地址迁移到设备内存
 *
 * 中文: 将指定的虚拟地址范围从系统内存迁移到设备内存。使用内核的 migrate_vma
 * 框架完成页面迁移。迁移流程：
 * 1. 通过 migrate_vma_setup() 收集源页面的 PFN
 * 2. 通过驱动提供的 populate_devmem_pfn() 回调填充目标设备内存 PFN
 * 3. 对每个页面，如果是系统内存页面则通过 copy_to_devmem 复制到设备；
 *    如果是其他设备的内存页面（peer pages），则通过 copy_to_ram 先读到
 *    本设备（当前版本暂不支持直接的设备间复制）
 * 4. 通过 migrate_vma_pages() 和 migrate_vma_finalize() 完成迁移
 *
 * @timeslice_ms 参数用于防止迁移活锁——数据将在设备内存中停留足够长的时间
 * 供 GPU 完成任务。调用者需持有 mmap 锁（至少读模式）。
 *
 * @devmem_allocation: 要迁移到的设备内存分配。调用者应持有其引用，
 *                     此函数会消费该引用（即使返回错误）
 * @mm: 指向 struct mm_struct 的指针
 * @start: 要迁移的虚拟地址范围起始
 * @end: 要迁移的虚拟地址范围结束
 * @mdetails: 控制迁移行为的详细信息
 *
 * Return: %0 on success, negative error code on failure.
 */
int drm_pagemap_migrate_to_devmem(struct drm_pagemap_devmem *devmem_allocation,
				  struct mm_struct *mm,
				  unsigned long start, unsigned long end,
				  const struct drm_pagemap_migrate_details *mdetails)
{
	const struct drm_pagemap_devmem_ops *ops = devmem_allocation->ops;
	struct drm_pagemap *dpagemap = devmem_allocation->dpagemap;
	struct dev_pagemap *pagemap = dpagemap->pagemap;
	struct migrate_vma migrate = {
		.start		= start,
		.end		= end,
		.pgmap_owner	= pagemap->owner,
		.flags		= MIGRATE_VMA_SELECT_SYSTEM | MIGRATE_VMA_SELECT_DEVICE_COHERENT |
		MIGRATE_VMA_SELECT_DEVICE_PRIVATE | MIGRATE_VMA_SELECT_COMPOUND,
	};
	unsigned long i, npages = npages_in_range(start, end);
	unsigned long own_pages = 0, migrated_pages = 0;
	struct migrate_range_loc cur, last = {.device = dpagemap->drm->dev, .ops = ops};
	struct vm_area_struct *vas;
	struct drm_pagemap_zdd *zdd = NULL;
	struct page **pages;
	struct drm_pagemap_addr *pagemap_addr;
	void *buf;
	int err;

	mmap_assert_locked(mm);

	if (!ops->populate_devmem_pfn || !ops->copy_to_devmem ||
	    !ops->copy_to_ram)
		return -EOPNOTSUPP;

	vas = vma_lookup(mm, start);
	if (!vas) {
		err = -ENOENT;
		goto err_out;
	}

	if (end > vas->vm_end || start < vas->vm_start) {
		err = -EINVAL;
		goto err_out;
	}

	if (!vma_is_anonymous(vas)) {
		err = -EBUSY;
		goto err_out;
	}

	buf = kvcalloc(npages, 2 * sizeof(*migrate.src) + sizeof(*pagemap_addr) +
		       sizeof(*pages), GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto err_out;
	}
	pagemap_addr = buf + (2 * sizeof(*migrate.src) * npages);
	pages = buf + (2 * sizeof(*migrate.src) + sizeof(*pagemap_addr)) * npages;

	zdd = drm_pagemap_zdd_alloc(dpagemap);
	if (!zdd) {
		err = -ENOMEM;
		kvfree(buf);
		goto err_out;
	}
	zdd->devmem_allocation = devmem_allocation;	/* Owns ref */

	migrate.vma = vas;
	migrate.src = buf;
	migrate.dst = migrate.src + npages;

	err = migrate_vma_setup(&migrate);
	if (err)
		goto err_free;

	if (!migrate.cpages) {
		/* No pages to migrate. Raced or unknown device pages. */
		err = -EBUSY;
		goto err_free;
	}

	if (migrate.cpages != npages &&
	    drm_pagemap_cpages(migrate.src, npages) != npages) {
		/*
		 * Some pages to migrate. But we want to migrate all or
		 * nothing. Raced or unknown device pages.
		 */
		err = -EBUSY;
		goto err_aborted_migration;
	}

	/* Count device-private pages to migrate */
	for (i = 0; i < npages;) {
		struct page *src_page = migrate_pfn_to_page(migrate.src[i]);
		unsigned long nr_pages = src_page ? NR_PAGES(folio_order(page_folio(src_page))) : 1;

		if (src_page && is_zone_device_page(src_page)) {
			if (page_pgmap(src_page) == pagemap)
				own_pages += nr_pages;
		}

		i += nr_pages;
	}

	drm_dbg(dpagemap->drm, "Total pages %lu; Own pages: %lu.\n",
		npages, own_pages);
	if (own_pages == npages) {
		err = 0;
		drm_dbg(dpagemap->drm, "Migration wasn't necessary.\n");
		goto err_aborted_migration;
	} else if (own_pages && !mdetails->can_migrate_same_pagemap) {
		err = -EBUSY;
		drm_dbg(dpagemap->drm, "Migration aborted due to fragmentation.\n");
		goto err_aborted_migration;
	}

	err = ops->populate_devmem_pfn(devmem_allocation, npages, migrate.dst);
	if (err)
		goto err_aborted_migration;

	own_pages = 0;

	for (i = 0; i < npages;) {
		unsigned long j;
		struct page *page = pfn_to_page(migrate.dst[i]);
		struct page *src_page = migrate_pfn_to_page(migrate.src[i]);
		unsigned int order = 0;

		cur.start = i;
		pages[i] = NULL;
		if (src_page && is_device_private_page(src_page)) {
			struct drm_pagemap_zdd *src_zdd =
				drm_pagemap_page_zone_device_data(src_page);

			if (page_pgmap(src_page) == pagemap &&
			    !mdetails->can_migrate_same_pagemap) {
				migrate.dst[i] = 0;
				own_pages++;
				goto next;
			}
			if (mdetails->source_peer_migrates) {
				cur.dpagemap = src_zdd->dpagemap;
				cur.ops = src_zdd->devmem_allocation->ops;
				cur.device = cur.dpagemap->drm->dev;
				pages[i] = src_page;
			}
		}
		if (!pages[i]) {
			cur.dpagemap = NULL;
			cur.ops = ops;
			cur.device = dpagemap->drm->dev;
			pages[i] = page;
		}
		migrate.dst[i] = migrate_pfn(migrate.dst[i]);

		if (migrate.src[i] & MIGRATE_PFN_COMPOUND) {
			drm_WARN_ONCE(dpagemap->drm, src_page &&
				      folio_order(page_folio(src_page)) != HPAGE_PMD_ORDER,
				      "Unexpected folio order\n");

			order = HPAGE_PMD_ORDER;
			migrate.dst[i] |= MIGRATE_PFN_COMPOUND;

			for (j = 1; j < NR_PAGES(order) && i + j < npages; j++)
				migrate.dst[i + j] = 0;
		}

		drm_pagemap_get_devmem_page(page, order, zdd);

		/* If we switched the migrating drm_pagemap, migrate previous pages now */
		err = drm_pagemap_migrate_range(devmem_allocation, migrate.src, migrate.dst,
						pages, pagemap_addr, &last, &cur,
						mdetails);
		if (err) {
			npages = i + 1;
			goto err_finalize;
		}

next:
		i += NR_PAGES(order);
	}

	cur.start = npages;
	cur.ops = NULL; /* Force migration */
	err = drm_pagemap_migrate_range(devmem_allocation, migrate.src, migrate.dst,
					pages, pagemap_addr, &last, &cur, mdetails);
	if (err)
		goto err_finalize;

	drm_WARN_ON(dpagemap->drm, !!own_pages);

	dma_fence_put(devmem_allocation->pre_migrate_fence);
	devmem_allocation->pre_migrate_fence = NULL;

	/* Upon success bind devmem allocation to range and zdd */
	devmem_allocation->timeslice_expiration = get_jiffies_64() +
		msecs_to_jiffies(mdetails->timeslice_ms);

err_finalize:
	if (err)
		drm_pagemap_migration_unlock_put_pages(npages, migrate.dst);
err_aborted_migration:
	migrate_vma_pages(&migrate);

	for (i = 0; !err && i < npages;) {
		struct page *page = migrate_pfn_to_page(migrate.src[i]);
		unsigned long nr_pages = page ? NR_PAGES(folio_order(page_folio(page))) : 1;

		if (migrate.src[i] & MIGRATE_PFN_MIGRATE)
			migrated_pages += nr_pages;

		i += nr_pages;
	}

	if (!err && migrated_pages < npages - own_pages) {
		drm_dbg(dpagemap->drm, "Raced while finalizing migration.\n");
		err = -EBUSY;
	}

	migrate_vma_finalize(&migrate);
err_free:
	drm_pagemap_zdd_put(zdd);
	kvfree(buf);
	return err;

err_out:
	devmem_allocation->ops->devmem_release(devmem_allocation);
	return err;
}
EXPORT_SYMBOL_GPL(drm_pagemap_migrate_to_devmem);

/**
 * drm_pagemap_migrate_populate_ram_pfn() - Populate RAM PFNs for a VM area
 * @vas: Pointer to the VM area structure, can be NULL
 * @fault_page: Fault page
 * @npages: Number of pages to populate
 * @mpages: Number of pages to migrate
 * @src_mpfn: Source array of migrate PFNs
 * @mpfn: Array of migrate PFNs to populate
 * @addr: Start address for PFN allocation
 *
 * This function populates the RAM migrate page frame numbers (PFNs) for the
 * specified VM area structure. It allocates and locks pages in the VM area for
 * RAM usage. If vas is non-NULL use alloc_page_vma for allocation, if NULL use
 * alloc_page for allocation.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int drm_pagemap_migrate_populate_ram_pfn(struct vm_area_struct *vas,
						struct page *fault_page,
						unsigned long npages,
						unsigned long *mpages,
						unsigned long *src_mpfn,
						unsigned long *mpfn,
						unsigned long addr)
{
	unsigned long i;

	for (i = 0; i < npages;) {
		struct page *page = NULL, *src_page;
		struct folio *folio;
		unsigned int order = 0;

		if (!(src_mpfn[i] & MIGRATE_PFN_MIGRATE))
			goto next;

		src_page = migrate_pfn_to_page(src_mpfn[i]);
		if (!src_page)
			goto next;

		if (fault_page) {
			if (drm_pagemap_page_zone_device_data(src_page) !=
			    drm_pagemap_page_zone_device_data(fault_page))
				goto next;
		}

		order = folio_order(page_folio(src_page));

		/* TODO: Support fallback to single pages if THP allocation fails */
		if (vas)
			folio = vma_alloc_folio(GFP_HIGHUSER, order, vas, addr);
		else
			folio = folio_alloc(GFP_HIGHUSER, order);

		if (!folio)
			goto free_pages;

		page = folio_page(folio, 0);
		mpfn[i] = migrate_pfn(page_to_pfn(page));

		if (order)
			mpfn[i] |= MIGRATE_PFN_COMPOUND;
next:
		if (page)
			addr += page_size(page);
		else
			addr += PAGE_SIZE;

		i += NR_PAGES(order);
	}

	for (i = 0; i < npages;) {
		struct page *page = migrate_pfn_to_page(mpfn[i]);
		unsigned int order = 0;

		if (!page)
			goto next_lock;

		WARN_ON_ONCE(!folio_trylock(page_folio(page)));

		order = folio_order(page_folio(page));
		*mpages += NR_PAGES(order);

next_lock:
		i += NR_PAGES(order);
	}

	return 0;

free_pages:
	for (i = 0; i < npages;) {
		struct page *page = migrate_pfn_to_page(mpfn[i]);
		unsigned int order = 0;

		if (!page)
			goto next_put;

		put_page(page);
		mpfn[i] = 0;

		order = folio_order(page_folio(page));

next_put:
		i += NR_PAGES(order);
	}
	return -ENOMEM;
}

static void drm_pagemap_dev_unhold_work(struct work_struct *work);
static LLIST_HEAD(drm_pagemap_unhold_list);
static DECLARE_WORK(drm_pagemap_work, drm_pagemap_dev_unhold_work);

/**
 * struct drm_pagemap_dev_hold - Struct to aid in drm_device release.
 * @link: Link into drm_pagemap_unhold_list for deferred reference releases.
 * @drm: drm device to put.
 *
 * When a struct drm_pagemap is released, we also need to release the
 * reference it holds on the drm device. However, typically that needs
 * to be done separately from a system-wide workqueue.
 * Each time a struct drm_pagemap is initialized
 * (or re-initialized if cached) therefore allocate a separate
 * drm_pagemap_dev_hold item, from which we put the drm device and
 * associated module.
 */
struct drm_pagemap_dev_hold {
	struct llist_node link;
	struct drm_device *drm;
};

static void drm_pagemap_release(struct kref *ref)
{
	struct drm_pagemap *dpagemap = container_of(ref, typeof(*dpagemap), ref);
	struct drm_pagemap_dev_hold *dev_hold = dpagemap->dev_hold;

	/*
	 * We know the pagemap provider is alive at this point, since
	 * the struct drm_pagemap_dev_hold holds a reference to the
	 * pagemap provider drm_device and its module.
	 */
	dpagemap->dev_hold = NULL;
	drm_pagemap_shrinker_add(dpagemap);
	llist_add(&dev_hold->link, &drm_pagemap_unhold_list);
	schedule_work(&drm_pagemap_work);
	/*
	 * Here, either the provider device is still alive, since if called from
	 * page_free(), the caller is holding a reference on the dev_pagemap,
	 * or if called from drm_pagemap_put(), the direct caller is still alive.
	 * This ensures we can't race with THIS module unload.
	 */
}

static void drm_pagemap_dev_unhold_work(struct work_struct *work)
{
	struct llist_node *node = llist_del_all(&drm_pagemap_unhold_list);
	struct drm_pagemap_dev_hold *dev_hold, *next;

	/*
	 * Deferred release of drm_pagemap provider device and module.
	 * THIS module is kept alive during the release by the
	 * flush_work() in the drm_pagemap_exit() function.
	 */
	llist_for_each_entry_safe(dev_hold, next, node, link) {
		struct drm_device *drm = dev_hold->drm;
		struct module *module = drm->driver->fops->owner;

		drm_dbg(drm, "Releasing reference on provider device and module.\n");
		drm_dev_put(drm);
		module_put(module);
		kfree(dev_hold);
	}
}

static struct drm_pagemap_dev_hold *
drm_pagemap_dev_hold(struct drm_pagemap *dpagemap)
{
	struct drm_pagemap_dev_hold *dev_hold;
	struct drm_device *drm = dpagemap->drm;

	dev_hold = kzalloc_obj(*dev_hold);
	if (!dev_hold)
		return ERR_PTR(-ENOMEM);

	init_llist_node(&dev_hold->link);
	dev_hold->drm = drm;
	(void)try_module_get(drm->driver->fops->owner);
	drm_dev_get(drm);

	return dev_hold;
}

/**
 * drm_pagemap_reinit() - 重新初始化 drm_pagemap
 *
 * 中文: 重新初始化一个已经被 drm_pagemap_release() 释放的 drm_pagemap。
 * 此接口适用于驱动程序缓存已销毁的 drm_pagemap 后需要重新使用的场景。
 * 重新分配 dev_hold 结构并重新初始化引用计数。
 *
 * @dpagemap: 要重新初始化的 drm_pagemap
 *
 * Return: 0 on success, negative error code on failure.
 */
int drm_pagemap_reinit(struct drm_pagemap *dpagemap)
{
	dpagemap->dev_hold = drm_pagemap_dev_hold(dpagemap);
	if (IS_ERR(dpagemap->dev_hold))
		return PTR_ERR(dpagemap->dev_hold);

	kref_init(&dpagemap->ref);
	return 0;
}
EXPORT_SYMBOL(drm_pagemap_reinit);

/**
 * drm_pagemap_init() - 初始化预分配的 drm_pagemap
 *
 * 中文: 初始化一个预分配的 &drm_pagemap 结构体，设置其引用计数为 1。
 * 初始化包括关联 dev_pagemap、DRM 设备、操作回调以及 shrinker 链接。
 * drm_pagemap 会持有 drm_device 和其模块的引用，直到 drm_pagemap_release()
 * 被调用。这支持了 drm_pagemap 的导出场景。
 * 成功后应通过 drm_pagemap_put() 销毁。
 *
 * @dpagemap: 要初始化的 drm_pagemap
 * @pagemap: 提供设备私有页面的关联 dev_pagemap
 * @drm: DRM 设备（drm_pagemap 持有其引用）
 * @ops: drm_pagemap 操作回调
 *
 * Return: 0 on success, negative error code on error.
 */
int drm_pagemap_init(struct drm_pagemap *dpagemap,
		     struct dev_pagemap *pagemap,
		     struct drm_device *drm,
		     const struct drm_pagemap_ops *ops)
{
	kref_init(&dpagemap->ref);
	dpagemap->ops = ops;
	dpagemap->pagemap = pagemap;
	dpagemap->drm = drm;
	dpagemap->cache = NULL;
	INIT_LIST_HEAD(&dpagemap->shrink_link);

	return drm_pagemap_reinit(dpagemap);
}
EXPORT_SYMBOL(drm_pagemap_init);

/**
 * drm_pagemap_put() - 释放 drm_pagemap 引用
 *
 * 中文: 递减 &drm_pagemap 的引用计数。当引用计数降至零时，触发
 * drm_pagemap_release()，该函数会将 drm_device 和模块的引用释放
 * 放入工作队列异步处理，并将 drm_pagemap 添加到 shrinker 列表以
 * 支持缓存重用。
 *
 * @dpagemap: 指向 struct drm_pagemap 对象的指针
 */
void drm_pagemap_put(struct drm_pagemap *dpagemap)
{
	if (likely(dpagemap)) {
		drm_pagemap_shrinker_might_lock(dpagemap);
		kref_put(&dpagemap->ref, drm_pagemap_release);
	}
}
EXPORT_SYMBOL(drm_pagemap_put);

/**
 * drm_pagemap_evict_to_ram() - 将设备内存分配驱逐回 RAM
 *
 * 中文: 将设备内存分配的所有页面迁移回系统 RAM。与 __drm_pagemap_migrate_to_ram()
 * 类似，但不需要 mmap 锁，使用 migrate_device_* 函数族进行迁移。
 * 迁移流程：填充设备内存 PFN -> migrate_device_pfns() -> 填充 RAM PFN ->
 * DMA 映射 -> copy_to_ram 复制数据 -> migrate_device_pages() 完成迁移。
 * 最多重试 2 次，处理竞态条件。
 *
 * @devmem_allocation: 指向设备内存分配的指针
 *
 * Return: 0 on success, negative error code on failure.
 */
int drm_pagemap_evict_to_ram(struct drm_pagemap_devmem *devmem_allocation)
{
	const struct drm_pagemap_devmem_ops *ops = devmem_allocation->ops;
	struct drm_pagemap_migrate_details mdetails = {};
	unsigned long npages, mpages = 0;
	struct page **pages;
	unsigned long *src, *dst;
	struct drm_pagemap_addr *pagemap_addr;
	void *buf;
	int i, err = 0;
	unsigned int retry_count = 2;

	npages = devmem_allocation->size >> PAGE_SHIFT;

retry:
	if (!mmget_not_zero(devmem_allocation->mm))
		return -EFAULT;

	buf = kvcalloc(npages, 2 * sizeof(*src) + sizeof(*pagemap_addr) +
		       sizeof(*pages), GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto err_out;
	}
	src = buf;
	dst = buf + (sizeof(*src) * npages);
	pagemap_addr = buf + (2 * sizeof(*src) * npages);
	pages = buf + (2 * sizeof(*src) + sizeof(*pagemap_addr)) * npages;

	err = ops->populate_devmem_pfn(devmem_allocation, npages, src);
	if (err)
		goto err_free;

	err = migrate_device_pfns(src, npages);
	if (err)
		goto err_free;

	err = drm_pagemap_migrate_populate_ram_pfn(NULL, NULL, npages, &mpages,
						   src, dst, 0);
	if (err || !mpages)
		goto err_finalize;

	err = drm_pagemap_migrate_map_pages(devmem_allocation->dev,
					    devmem_allocation->dpagemap, pagemap_addr,
					    dst, npages, DMA_FROM_DEVICE,
					    &mdetails);
	if (err)
		goto err_finalize;

	for (i = 0; i < npages;) {
		unsigned int order = 0;

		pages[i] = migrate_pfn_to_page(src[i]);
		if (pages[i])
			order = folio_order(page_folio(pages[i]));

		i += NR_PAGES(order);
	}

	err = ops->copy_to_ram(pages, pagemap_addr, npages, NULL);
	if (err)
		goto err_finalize;

err_finalize:
	if (err)
		drm_pagemap_migration_unlock_put_pages(npages, dst);
	migrate_device_pages(src, dst, npages);
	migrate_device_finalize(src, dst, npages);
	drm_pagemap_migrate_unmap_pages(devmem_allocation->dev, pagemap_addr, dst, npages,
					DMA_FROM_DEVICE);

err_free:
	kvfree(buf);
err_out:
	mmput_async(devmem_allocation->mm);

	if (completion_done(&devmem_allocation->detached))
		return 0;

	if (retry_count--) {
		cond_resched();
		goto retry;
	}

	return err ?: -EBUSY;
}
EXPORT_SYMBOL_GPL(drm_pagemap_evict_to_ram);

/**
 * __drm_pagemap_migrate_to_ram() - Migrate GPU SVM range to RAM (internal)
 * @vas: Pointer to the VM area structure
 * @page: Pointer to the page for fault handling.
 * @fault_addr: Fault address
 * @size: Size of migration
 *
 * This internal function performs the migration of the specified GPU SVM range
 * to RAM. It sets up the migration, populates + dma maps RAM PFNs, and
 * invokes the driver-specific operations for migration to RAM.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int __drm_pagemap_migrate_to_ram(struct vm_area_struct *vas,
					struct page *page,
					unsigned long fault_addr,
					unsigned long size)
{
	struct migrate_vma migrate = {
		.vma		= vas,
		.pgmap_owner	= page_pgmap(page)->owner,
		.flags		= MIGRATE_VMA_SELECT_DEVICE_PRIVATE |
				  MIGRATE_VMA_SELECT_DEVICE_COHERENT |
				  MIGRATE_VMA_SELECT_COMPOUND,
		.fault_page	= page,
	};
	struct drm_pagemap_migrate_details mdetails = {};
	struct drm_pagemap_zdd *zdd;
	const struct drm_pagemap_devmem_ops *ops;
	struct device *dev = NULL;
	unsigned long npages, mpages = 0;
	struct page **pages;
	struct drm_pagemap_addr *pagemap_addr;
	unsigned long start, end;
	void *buf;
	int i, err = 0;

	zdd = drm_pagemap_page_zone_device_data(page);
	if (time_before64(get_jiffies_64(), zdd->devmem_allocation->timeslice_expiration))
		return 0;

	start = ALIGN_DOWN(fault_addr, size);
	end = ALIGN(fault_addr + 1, size);

	/* Corner where VMA area struct has been partially unmapped */
	if (start < vas->vm_start)
		start = vas->vm_start;
	if (end > vas->vm_end)
		end = vas->vm_end;

	migrate.start = start;
	migrate.end = end;
	npages = npages_in_range(start, end);

	buf = kvcalloc(npages, 2 * sizeof(*migrate.src) + sizeof(*pagemap_addr) +
		       sizeof(*pages), GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto err_out;
	}
	pagemap_addr = buf + (2 * sizeof(*migrate.src) * npages);
	pages = buf + (2 * sizeof(*migrate.src) + sizeof(*pagemap_addr)) * npages;

	migrate.vma = vas;
	migrate.src = buf;
	migrate.dst = migrate.src + npages;

	err = migrate_vma_setup(&migrate);
	if (err)
		goto err_free;

	/* Raced with another CPU fault, nothing to do */
	if (!migrate.cpages)
		goto err_free;

	ops = zdd->devmem_allocation->ops;
	dev = zdd->devmem_allocation->dev;

	err = drm_pagemap_migrate_populate_ram_pfn(vas, page, npages, &mpages,
						   migrate.src, migrate.dst,
						   start);
	if (err)
		goto err_finalize;

	err = drm_pagemap_migrate_map_pages(dev, zdd->dpagemap, pagemap_addr, migrate.dst, npages,
					    DMA_FROM_DEVICE, &mdetails);
	if (err)
		goto err_finalize;

	for (i = 0; i < npages;) {
		unsigned int order = 0;

		pages[i] = migrate_pfn_to_page(migrate.src[i]);
		if (pages[i])
			order = folio_order(page_folio(pages[i]));

		i += NR_PAGES(order);
	}

	err = ops->copy_to_ram(pages, pagemap_addr, npages, NULL);
	if (err)
		goto err_finalize;

err_finalize:
	if (err)
		drm_pagemap_migration_unlock_put_pages(npages, migrate.dst);
	migrate_vma_pages(&migrate);
	migrate_vma_finalize(&migrate);
	if (dev)
		drm_pagemap_migrate_unmap_pages(dev, pagemap_addr, migrate.dst,
						npages, DMA_FROM_DEVICE);
err_free:
	kvfree(buf);
err_out:

	return err;
}

/**
 * drm_pagemap_folio_free() - Put GPU SVM zone device data associated with a folio
 * @folio: Pointer to the folio
 *
 * This function is a callback used to put the GPU SVM zone device data
 * associated with a page when it is being released.
 */
static void drm_pagemap_folio_free(struct folio *folio)
{
	struct page *page = folio_page(folio, 0);

	drm_pagemap_zdd_put(drm_pagemap_page_zone_device_data(page));
}

/**
 * drm_pagemap_migrate_to_ram() - Migrate a virtual range to RAM (page fault handler)
 * @vmf: Pointer to the fault information structure
 *
 * This function is a page fault handler used to migrate a virtual range
 * to ram. The device memory allocation in which the device page is found is
 * migrated in its entirety.
 *
 * Returns:
 * VM_FAULT_SIGBUS on failure, 0 on success.
 */
static vm_fault_t drm_pagemap_migrate_to_ram(struct vm_fault *vmf)
{
	struct drm_pagemap_zdd *zdd = drm_pagemap_page_zone_device_data(vmf->page);
	int err;

	err = __drm_pagemap_migrate_to_ram(vmf->vma,
					   vmf->page, vmf->address,
					   zdd->devmem_allocation->size);

	return err ? VM_FAULT_SIGBUS : 0;
}

static void drm_pagemap_folio_split(struct folio *orig_folio, struct folio *new_folio)
{
	struct drm_pagemap_zdd *zdd;

	if (!new_folio)
		return;

	new_folio->pgmap = orig_folio->pgmap;
	zdd = folio_zone_device_data(orig_folio);
	folio_set_zone_device_data(new_folio, drm_pagemap_zdd_get(zdd));
}

static const struct dev_pagemap_ops drm_pagemap_pagemap_ops = {
	.folio_free = drm_pagemap_folio_free,
	.migrate_to_ram = drm_pagemap_migrate_to_ram,
	.folio_split = drm_pagemap_folio_split,
};

/**
 * drm_pagemap_pagemap_ops_get() - 获取设备页面映射操作回调
 *
 * 中文: 返回 dev_pagemap_ops 结构体指针，其中包含 folio_free（页面释放时
 * 关联 zdd 引用释放）、migrate_to_ram（CPU 缺页时从设备内存迁移回 RAM）
 * 和 folio_split（大页分裂时复制 zone_device_data）回调。驱动程序在注册
 * dev_pagemap 时应使用此函数获取标准操作回调。
 *
 * Returns:
 * Pointer to the GPU SVM device page map operations structure.
 */
const struct dev_pagemap_ops *drm_pagemap_pagemap_ops_get(void)
{
	return &drm_pagemap_pagemap_ops;
}
EXPORT_SYMBOL_GPL(drm_pagemap_pagemap_ops_get);

/**
 * drm_pagemap_devmem_init() - 初始化 drm_pagemap 设备内存分配
 *
 * 中文: 初始化 struct drm_pagemap_devmem 结构体，设置设备内存分配的所有
 * 必要字段：所属设备、地址空间 mm、设备内存操作回调、源 drm_pagemap、
 * 分配大小以及预迁移 fence。预迁移 fence 允许迁移操作等待或流水线化
 * 在其他 fence 之后执行（可为 NULL）。
 *
 * @devmem_allocation: 要初始化的 struct drm_pagemap_devmem
 * @dev: 设备内存分配所属的设备
 * @mm: 地址空间的 mm_struct
 * @ops: GPU SVM 设备内存的操作回调
 * @dpagemap: 分配来源的 struct drm_pagemap
 * @size: 设备内存分配大小
 * @pre_migrate_fence: 迁移开始前等待的 fence（可为 NULL）
 */
void drm_pagemap_devmem_init(struct drm_pagemap_devmem *devmem_allocation,
			     struct device *dev, struct mm_struct *mm,
			     const struct drm_pagemap_devmem_ops *ops,
			     struct drm_pagemap *dpagemap, size_t size,
			     struct dma_fence *pre_migrate_fence)
{
	init_completion(&devmem_allocation->detached);
	devmem_allocation->dev = dev;
	devmem_allocation->mm = mm;
	devmem_allocation->ops = ops;
	devmem_allocation->dpagemap = dpagemap;
	devmem_allocation->size = size;
	devmem_allocation->pre_migrate_fence = pre_migrate_fence;
}
EXPORT_SYMBOL_GPL(drm_pagemap_devmem_init);

/**
 * drm_pagemap_page_to_dpagemap() - 返回页面所属的 drm_pagemap 指针
 *
 * 中文: 返回设备私有页面所属的 struct drm_pagemap 指针。通过页面的
 * zone_device_data 找到 zdd 结构，再从 zdd 的 devmem_allocation 获取
 * dpagemap 引用。如果页面不是从 struct drm_pagemap 填充的，结果未定义。
 *
 * @page: 设备私有页面的 struct page
 *
 * Return: A pointer to the struct drm_pagemap of a device private page that
 * was populated from the struct drm_pagemap. If the page was *not* populated
 * from a struct drm_pagemap, the result is undefined and the function call
 * may result in dereferencing and invalid address.
 */
struct drm_pagemap *drm_pagemap_page_to_dpagemap(struct page *page)
{
	struct drm_pagemap_zdd *zdd = drm_pagemap_page_zone_device_data(page);

	return zdd->devmem_allocation->dpagemap;
}
EXPORT_SYMBOL_GPL(drm_pagemap_page_to_dpagemap);

/**
 * drm_pagemap_populate_mm() - 用设备内存页面填充虚拟地址范围
 *
 * 中文: 尝试用设备内存页面填充指定的虚拟地址范围。该函数会获取 mm 引用，
 * 持有 mmap 读锁后调用驱动提供的 populate_mm 回调。回调将清除现有页面
 * 或从现有页面迁移数据到设备内存。此函数仅为尽力而为（best effort）实现，
 * 不同实现的努力程度可能不同。如果硬件设备已被移除/解除绑定，返回 -ENODEV。
 * @timeslice_ms 参数确保迁移后的页面在 @mm 中停留指定时间后才允许被迁回。
 *
 * @dpagemap: 管理设备内存的 drm_pagemap 指针
 * @start: 要填充的虚拟范围起始地址
 * @end: 要填充的虚拟范围结束地址
 * @mm: 指向虚拟地址空间的指针
 * @timeslice_ms: 迁移页面在 @mm 中应保留的时间（毫秒）
 *
 * Return: %0 on success, negative error code on error. If the hardware
 * device was removed / unbound the function will return %-ENODEV.
 */
int drm_pagemap_populate_mm(struct drm_pagemap *dpagemap,
			    unsigned long start, unsigned long end,
			    struct mm_struct *mm,
			    unsigned long timeslice_ms)
{
	int err;

	if (!mmget_not_zero(mm))
		return -EFAULT;
	mmap_read_lock(mm);
	err = dpagemap->ops->populate_mm(dpagemap, start, end, mm,
					 timeslice_ms);
	mmap_read_unlock(mm);
	mmput(mm);

	return err;
}
EXPORT_SYMBOL(drm_pagemap_populate_mm);

void drm_pagemap_destroy(struct drm_pagemap *dpagemap, bool is_atomic_or_reclaim)
{
	if (dpagemap->ops->destroy)
		dpagemap->ops->destroy(dpagemap, is_atomic_or_reclaim);
	else
		kfree(dpagemap);
}

static void drm_pagemap_exit(void)
{
	flush_work(&drm_pagemap_work);
	if (WARN_ON(!llist_empty(&drm_pagemap_unhold_list)))
		disable_work_sync(&drm_pagemap_work);
}
module_exit(drm_pagemap_exit);
