// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 Noralf Trønnes
 */

/**
 * DOC: SHMEM GEM 对象管理概述 (中文)
 *
 * 该文件实现了基于 shmem（共享内存）的 GEM（图形执行管理器）对象辅助函数。
 * SHMEM GEM 对象使用匿名可分页内存作为后端存储，是 DRM 子系统中最为通用的
 * GEM 内存分配方式之一。
 *
 * 核心功能包括：
 *   - 创建和初始化 shmem 类型的 GEM 对象
 *   - 管理对象的物理页面生命周期（获取、释放、固定）
 *   - 提供内核虚拟地址映射（vmap/vunmap）
 *   - 支持 mmap 操作，将缓冲区映射到用户空间
 *   - 生成和管理散列/聚集表（SG table），用于 DMA 操作和 PRIME 缓冲区共享
 *   - 支持内存容量建议（madvise）和页面回收（purge）
 *
 * 该辅助层通过 struct drm_gem_shmem_object 结构体管理每个 shmem GEM 对象，
 * 并提供了 _object_ 后缀的包装函数（如 drm_gem_shmem_object_vmap() 封装
 * drm_gem_shmem_vmap()），自动完成类型转换。
 */

#include <linux/dma-buf.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#ifdef CONFIG_X86
#include <asm/set_memory.h>
#endif

#include <kunit/visibility.h>

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_dumb_buffers.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>

MODULE_IMPORT_NS("DMA_BUF");

/**
 * DOC: overview
 *
 * This library provides helpers for GEM objects backed by shmem buffers
 * allocated using anonymous pageable memory.
 *
 * Functions that operate on the GEM object receive struct &drm_gem_shmem_object.
 * For GEM callback helpers in struct &drm_gem_object functions, see likewise
 * named functions with an _object_ infix (e.g., drm_gem_shmem_object_vmap() wraps
 * drm_gem_shmem_vmap()). These helpers perform the necessary type conversion.
 */

static const struct drm_gem_object_funcs drm_gem_shmem_funcs = {
	.free = drm_gem_shmem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static int __drm_gem_shmem_init(struct drm_device *dev, struct drm_gem_shmem_object *shmem,
				size_t size, bool private)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret = 0;

	if (!obj->funcs)
		obj->funcs = &drm_gem_shmem_funcs;

	if (private) {
		drm_gem_private_object_init(dev, obj, size);
		shmem->map_wc = false; /* dma-buf mappings use always writecombine */
	} else {
		ret = drm_gem_object_init(dev, obj, size);
	}
	if (ret) {
		drm_gem_private_object_fini(obj);
		return ret;
	}

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto err_release;

	INIT_LIST_HEAD(&shmem->madv_list);

	if (!private) {
		/*
		 * Our buffers are kept pinned, so allocating them
		 * from the MOVABLE zone is a really bad idea, and
		 * conflicts with CMA. See comments above new_inode()
		 * why this is required _and_ expected if you're
		 * going to pin these pages.
		 */
		mapping_set_gfp_mask(obj->filp->f_mapping, GFP_HIGHUSER |
				     __GFP_RETRY_MAYFAIL | __GFP_NOWARN);
	}

	return 0;
err_release:
	drm_gem_object_release(obj);
	return ret;
}

/**
 * drm_gem_shmem_init - 初始化已分配的 shmem GEM 对象
 *
 * 中文: 初始化一个已分配内存的 shmem GEM 对象。该函数设置对象的基本属性，
 * 包括 GEM 对象初始化、内存映射偏移量创建以及 GFP 掩码配置。对于非私有对象，
 * 会配置 GFP_HIGHUSER 标志并禁用 MOVABLE 区域分配，因为 shmem 缓冲区通常
 * 需要保持固定（pinned）状态，从 MOVABLE 区域分配会与 CMA 产生冲突。
 *
 * @dev: DRM 设备
 * @shmem: 要初始化的 shmem GEM 对象
 * @size: 缓冲区大小（字节）
 *
 * Returns:
 * 0 on success, or a negative error code on failure.
 */
int drm_gem_shmem_init(struct drm_device *dev, struct drm_gem_shmem_object *shmem, size_t size)
{
	return __drm_gem_shmem_init(dev, shmem, size, false);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_init);

static struct drm_gem_shmem_object *
__drm_gem_shmem_create(struct drm_device *dev, size_t size, bool private)
{
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	int ret = 0;

	size = PAGE_ALIGN(size);

	if (dev->driver->gem_create_object) {
		obj = dev->driver->gem_create_object(dev, size);
		if (IS_ERR(obj))
			return ERR_CAST(obj);
		shmem = to_drm_gem_shmem_obj(obj);
	} else {
		shmem = kzalloc_obj(*shmem);
		if (!shmem)
			return ERR_PTR(-ENOMEM);
		obj = &shmem->base;
	}

	ret = __drm_gem_shmem_init(dev, shmem, size, private);
	if (ret) {
		kfree(obj);
		return ERR_PTR(ret);
	}

	return shmem;
}
/**
 * drm_gem_shmem_create - 分配指定大小的 shmem GEM 对象
 *
 * 中文: 创建并初始化一个 shmem GEM 对象。该函数首先尝试通过驱动程序提供的
 * gem_create_object 回调创建对象，否则直接分配新的 shmem 对象。对象大小
 * 会向上对齐到 PAGE_SIZE。创建过程包括初始化 GEM 对象、创建 mmap 偏移量等步骤。
 *
 * @dev: DRM 设备
 * @size: 要分配的对象大小
 *
 * Returns:
 * A struct drm_gem_shmem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *dev, size_t size)
{
	return __drm_gem_shmem_create(dev, size, false);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_create);

/**
 * drm_gem_shmem_release - Release resources associated with a shmem GEM object.
 * @shmem: shmem GEM object
 *
 * This function cleans up the GEM object state, but does not free the memory used to store the
 * object itself. This function is meant to be a dedicated helper for the Rust GEM bindings.
 */
void drm_gem_shmem_release(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	if (drm_gem_is_imported(obj)) {
		drm_prime_gem_destroy(obj, shmem->sgt);
	} else {
		dma_resv_lock(shmem->base.resv, NULL);

		drm_WARN_ON(obj->dev, refcount_read(&shmem->vmap_use_count));

		if (shmem->sgt) {
			dma_unmap_sgtable(obj->dev->dev, shmem->sgt,
					  DMA_BIDIRECTIONAL, 0);
			sg_free_table(shmem->sgt);
			kfree(shmem->sgt);
		}
		if (shmem->pages)
			drm_gem_shmem_put_pages_locked(shmem);

		drm_WARN_ON(obj->dev, refcount_read(&shmem->pages_use_count));
		drm_WARN_ON(obj->dev, refcount_read(&shmem->pages_pin_count));

		dma_resv_unlock(shmem->base.resv);
	}

	drm_gem_object_release(obj);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_release);

/**
 * drm_gem_shmem_free - 释放 shmem GEM 对象占用的资源
 *
 * 中文: 清理 shmem GEM 对象状态并释放存储对象本身的内存。
 * 该函数会调用 drm_gem_shmem_release() 完成底层资源释放（包括 sg 表、
 * 物理页面、dma 映射等），然后通过 kfree 释放对象内存。
 * 对于 Rust GEM 绑定场景，驱动程序可直接使用 drm_gem_shmem_release()。
 *
 * @shmem: 要释放的 shmem GEM 对象
 */
void drm_gem_shmem_free(struct drm_gem_shmem_object *shmem)
{
	drm_gem_shmem_release(shmem);
	kfree(shmem);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_free);

static int drm_gem_shmem_get_pages_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct page **pages;

	dma_resv_assert_held(shmem->base.resv);

	if (refcount_inc_not_zero(&shmem->pages_use_count))
		return 0;

	pages = drm_gem_get_pages(obj);
	if (IS_ERR(pages)) {
		drm_dbg_kms(obj->dev, "Failed to get pages (%ld)\n",
			    PTR_ERR(pages));
		return PTR_ERR(pages);
	}

	/*
	 * TODO: Allocating WC pages which are correctly flushed is only
	 * supported on x86. Ideal solution would be a GFP_WC flag, which also
	 * ttm_pool.c could use.
	 */
#ifdef CONFIG_X86
	if (shmem->map_wc)
		set_pages_array_wc(pages, obj->size >> PAGE_SHIFT);
#endif

	shmem->pages = pages;

	refcount_set(&shmem->pages_use_count, 1);

	return 0;
}

/*
 * drm_gem_shmem_put_pages_locked - Decrease use count on the backing pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function decreases the use count and puts the backing pages when use drops to zero.
 */
void drm_gem_shmem_put_pages_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	dma_resv_assert_held(shmem->base.resv);

	if (refcount_dec_and_test(&shmem->pages_use_count)) {
#ifdef CONFIG_X86
		if (shmem->map_wc)
			set_pages_array_wb(shmem->pages, obj->size >> PAGE_SHIFT);
#endif

		drm_gem_put_pages(obj, shmem->pages,
				  shmem->pages_mark_dirty_on_put,
				  shmem->pages_mark_accessed_on_put);
		shmem->pages = NULL;
		shmem->pages_mark_accessed_on_put = false;
		shmem->pages_mark_dirty_on_put = false;
	}
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_put_pages_locked);

int drm_gem_shmem_pin_locked(struct drm_gem_shmem_object *shmem)
{
	int ret;

	dma_resv_assert_held(shmem->base.resv);

	drm_WARN_ON(shmem->base.dev, drm_gem_is_imported(&shmem->base));

	if (refcount_inc_not_zero(&shmem->pages_pin_count))
		return 0;

	ret = drm_gem_shmem_get_pages_locked(shmem);
	if (!ret)
		refcount_set(&shmem->pages_pin_count, 1);

	return ret;
}
EXPORT_SYMBOL(drm_gem_shmem_pin_locked);

void drm_gem_shmem_unpin_locked(struct drm_gem_shmem_object *shmem)
{
	dma_resv_assert_held(shmem->base.resv);

	if (refcount_dec_and_test(&shmem->pages_pin_count))
		drm_gem_shmem_put_pages_locked(shmem);
}
EXPORT_SYMBOL(drm_gem_shmem_unpin_locked);

/**
 * drm_gem_shmem_pin - 固定 shmem GEM 对象的后端物理页面
 *
 * 中文: 确保后端物理页面在缓冲区导出期间被固定（pinned）在内存中。
 * 当一个 shmem GEM 对象被导出到其他设备（如 PRIME 共享）时，需要保证
 * 其物理页面不会被交换出去。该函数使用引用计数管理 pin 状态：首次 pin
 * 会触发页面分配和固定，后续 pin 仅递增引用计数。
 * 与外部的 drm_gem_shmem_pin_locked() 不同，该函数会自行获取 dma_resv 锁。
 *
 * @shmem: shmem GEM 对象
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_pin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	if (refcount_inc_not_zero(&shmem->pages_pin_count))
		return 0;

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return ret;
	ret = drm_gem_shmem_pin_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_pin);

/**
 * drm_gem_shmem_unpin - 解除 shmem GEM 对象后端页面的固定状态
 *
 * 中文: 解除对后端物理页面的固定要求。当 pin 引用计数递减至零时，
 * 允许物理页面被交换出去。该函数与外部的 drm_gem_shmem_unpin_locked()
 * 不同，会自行获取 dma_resv 锁后再执行实际的解除固定操作。
 *
 * @shmem: shmem GEM 对象
 */
void drm_gem_shmem_unpin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	if (refcount_dec_not_one(&shmem->pages_pin_count))
		return;

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_unpin_locked(shmem);
	dma_resv_unlock(shmem->base.resv);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_unpin);

/**
 * drm_gem_shmem_vmap_locked - 为 shmem GEM 对象创建内核虚拟地址映射
 *
 * 中文: 确保 shmem GEM 对象的后端缓冲区存在连续的内核虚拟地址映射。
 * 该函数隐藏了 dma-buf 导入对象和本地分配对象之间的差异：
 * 对于导入对象，调用 dma_buf_vmap()；对于本地对象，使用 vmap() 创建映射。
 * 映射引用计数确保仅在最后一个使用者释放后才真正解除映射。
 * 获取的映射应通过 drm_gem_shmem_vunmap_locked() 清理。
 * 调用者需持有 dma_resv 锁。
 *
 * @shmem: shmem GEM 对象
 * @map: 返回 SHMEM GEM 对象后端存储的内核虚拟地址
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_vmap_locked(struct drm_gem_shmem_object *shmem,
			      struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret = 0;

	dma_resv_assert_held(obj->resv);

	if (drm_gem_is_imported(obj)) {
		ret = dma_buf_vmap(obj->import_attach->dmabuf, map);
	} else {
		pgprot_t prot = PAGE_KERNEL;

		dma_resv_assert_held(shmem->base.resv);

		if (refcount_inc_not_zero(&shmem->vmap_use_count)) {
			iosys_map_set_vaddr(map, shmem->vaddr);
			return 0;
		}

		ret = drm_gem_shmem_pin_locked(shmem);
		if (ret)
			return ret;

		if (shmem->map_wc)
			prot = pgprot_writecombine(prot);
		shmem->vaddr = vmap(shmem->pages, obj->size >> PAGE_SHIFT,
				    VM_MAP, prot);
		if (!shmem->vaddr) {
			ret = -ENOMEM;
		} else {
			iosys_map_set_vaddr(map, shmem->vaddr);
			refcount_set(&shmem->vmap_use_count, 1);
			shmem->pages_mark_accessed_on_put = true;
			shmem->pages_mark_dirty_on_put = true;
		}
	}

	if (ret) {
		drm_dbg_kms(obj->dev, "Failed to vmap pages, error %d\n", ret);
		goto err_put_pages;
	}

	return 0;

err_put_pages:
	if (!drm_gem_is_imported(obj))
		drm_gem_shmem_unpin_locked(shmem);

	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_vmap_locked);

/**
 * drm_gem_shmem_vunmap_locked - 解除 shmem GEM 对象的内核虚拟地址映射
 *
 * 中文: 清理由 drm_gem_shmem_vmap_locked() 获取的内核虚拟地址映射。
 * 仅当 vmap 引用计数降至零时才真正移除映射。对于 dma-buf 导入对象，
 * 调用 dma_buf_vunmap()；对于本地分配对象，使用 vunmap() 解除映射。
 * 调用者需持有 dma_resv 锁。
 *
 * @shmem: shmem GEM 对象
 * @map: SHMEM GEM 对象映射到的内核虚拟地址
 */
void drm_gem_shmem_vunmap_locked(struct drm_gem_shmem_object *shmem,
				 struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;

	dma_resv_assert_held(obj->resv);

	if (drm_gem_is_imported(obj)) {
		dma_buf_vunmap(obj->import_attach->dmabuf, map);
	} else {
		dma_resv_assert_held(shmem->base.resv);

		if (refcount_dec_and_test(&shmem->vmap_use_count)) {
			vunmap(shmem->vaddr);
			shmem->vaddr = NULL;

			drm_gem_shmem_unpin_locked(shmem);
		}
	}
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_vunmap_locked);

static int
drm_gem_shmem_create_with_handle(struct drm_file *file_priv,
				 struct drm_device *dev, size_t size,
				 uint32_t *handle)
{
	struct drm_gem_shmem_object *shmem;
	int ret;

	shmem = drm_gem_shmem_create(dev, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, &shmem->base, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&shmem->base);

	return ret;
}

/* Update madvise status, returns true if not purged, else
 * false or -errno.
 */
int drm_gem_shmem_madvise_locked(struct drm_gem_shmem_object *shmem, int madv)
{
	dma_resv_assert_held(shmem->base.resv);

	if (shmem->madv >= 0)
		shmem->madv = madv;

	madv = shmem->madv;

	return (madv >= 0);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_madvise_locked);

void drm_gem_shmem_purge_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct drm_device *dev = obj->dev;

	dma_resv_assert_held(shmem->base.resv);

	drm_WARN_ON(obj->dev, !drm_gem_shmem_is_purgeable(shmem));

	dma_unmap_sgtable(dev->dev, shmem->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(shmem->sgt);
	kfree(shmem->sgt);
	shmem->sgt = NULL;

	drm_gem_shmem_put_pages_locked(shmem);

	shmem->madv = -1;

	drm_vma_node_unmap(&obj->vma_node, dev->anon_inode->i_mapping);
	drm_gem_free_mmap_offset(obj);

	/* Our goal here is to return as much of the memory as
	 * is possible back to the system as we are called from OOM.
	 * To do this we must instruct the shmfs to drop all of its
	 * backing pages, *now*.
	 */
	shmem_truncate_range(file_inode(obj->filp), 0, (loff_t)-1);

	invalidate_mapping_pages(file_inode(obj->filp)->i_mapping, 0, (loff_t)-1);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_purge_locked);

/**
 * drm_gem_shmem_dumb_create - 创建 dumb shmem 缓冲区对象
 *
 * 中文: 计算 dumb 缓冲区的 pitch（每行字节数）并向上取整到整数字节/像素。
 * 对于没有额外 pitch 限制的硬件，驱动程序可直接将此函数用作
 * &drm_driver.dumb_create 回调。对于有特定硬件对齐要求的驱动，
 * 可在调用此函数前调整用户空间设置的字段。
 *
 * @file: 创建 dumb 缓冲区的 DRM 文件结构
 * @dev: DRM 设备
 * @args: IOCTL 数据
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
			      struct drm_mode_create_dumb *args)
{
	int ret;

	ret = drm_mode_size_dumb(dev, args, 0, 0);
	if (ret)
		return ret;

	return drm_gem_shmem_create_with_handle(file, dev, args->size, &args->handle);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_dumb_create);

static void drm_gem_shmem_record_mkwrite(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	loff_t num_pages = obj->size >> PAGE_SHIFT;
	pgoff_t page_offset = vmf->pgoff - vma->vm_pgoff; /* page offset within VMA */

	if (drm_WARN_ON(obj->dev, !shmem->pages || page_offset >= num_pages))
		return;

	file_update_time(vma->vm_file);
	folio_mark_dirty(page_folio(shmem->pages[page_offset]));
}

static vm_fault_t try_insert_pfn(struct vm_fault *vmf, unsigned int order,
				 unsigned long pfn)
{
	if (!order) {
		return vmf_insert_pfn(vmf->vma, vmf->address, pfn);
#ifdef CONFIG_ARCH_SUPPORTS_PMD_PFNMAP
	} else if (order == PMD_ORDER) {
		unsigned long paddr = pfn << PAGE_SHIFT;
		bool aligned = (vmf->address & ~PMD_MASK) == (paddr & ~PMD_MASK);

		if (aligned &&
		    folio_test_pmd_mappable(page_folio(pfn_to_page(pfn)))) {
			vm_fault_t ret;

			pfn &= PMD_MASK >> PAGE_SHIFT;

			/* Unlike PTEs which are automatically upgraded to
			 * writeable entries, the PMD upgrades go through
			 * .huge_fault(). Make sure we pass the "write" info
			 * along in that case.
			 * This also means we have to record the write fault
			 * here, instead of in .pfn_mkwrite().
			 */
			ret = vmf_insert_pfn_pmd(vmf, pfn,
						 vmf->flags & FAULT_FLAG_WRITE);
			if (ret == VM_FAULT_NOPAGE && (vmf->flags & FAULT_FLAG_WRITE))
				drm_gem_shmem_record_mkwrite(vmf);

			return ret;
		}
#endif
	}
	return VM_FAULT_FALLBACK;
}

static vm_fault_t drm_gem_shmem_any_fault(struct vm_fault *vmf, unsigned int order)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_device *dev = obj->dev;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	loff_t num_pages = obj->size >> PAGE_SHIFT;
	vm_fault_t ret = VM_FAULT_SIGBUS;
	struct page **pages = shmem->pages;
	pgoff_t page_offset = vmf->pgoff - vma->vm_pgoff; /* page offset within VMA */
	struct page *page;
	struct folio *folio;
	unsigned long pfn;

	if (order && order != PMD_ORDER)
		return VM_FAULT_FALLBACK;

	dma_resv_lock(obj->resv, NULL);

	if (page_offset >= num_pages || drm_WARN_ON_ONCE(dev, !shmem->pages) ||
	    shmem->madv < 0)
		goto out;

	page = pages[page_offset];
	if (drm_WARN_ON_ONCE(dev, !page))
		goto out;
	folio = page_folio(page);

	pfn = page_to_pfn(page);

	ret = try_insert_pfn(vmf, order, pfn);
	if (ret == VM_FAULT_NOPAGE)
		folio_mark_accessed(folio);

out:
	dma_resv_unlock(obj->resv);

	return ret;
}

static vm_fault_t drm_gem_shmem_fault(struct vm_fault *vmf)
{
	return drm_gem_shmem_any_fault(vmf, 0);
}

static void drm_gem_shmem_vm_open(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	dma_resv_lock(shmem->base.resv, NULL);

	/*
	 * We should have already pinned the pages when the buffer was first
	 * mmap'd, vm_open() just grabs an additional reference for the new
	 * mm the vma is getting copied into (ie. on fork()).
	 */
	drm_WARN_ON_ONCE(obj->dev,
			 !refcount_inc_not_zero(&shmem->pages_use_count));

	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_open(vma);
}

static void drm_gem_shmem_vm_close(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_put_pages_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_close(vma);
}

static vm_fault_t drm_gem_shmem_pfn_mkwrite(struct vm_fault *vmf)
{
	drm_gem_shmem_record_mkwrite(vmf);
	return 0;
}

const struct vm_operations_struct drm_gem_shmem_vm_ops = {
	.fault = drm_gem_shmem_fault,
#ifdef CONFIG_ARCH_SUPPORTS_PMD_PFNMAP
	.huge_fault = drm_gem_shmem_any_fault,
#endif
	.open = drm_gem_shmem_vm_open,
	.close = drm_gem_shmem_vm_close,
	.pfn_mkwrite = drm_gem_shmem_pfn_mkwrite,
};
EXPORT_SYMBOL_GPL(drm_gem_shmem_vm_ops);

/**
 * drm_gem_shmem_mmap - 对 shmem GEM 对象执行内存映射
 *
 * 中文: 实现 shmem GEM 对象的增强版文件 mmap 操作。对于 dma-buf 导入的对象，
 * 委托给 dma_buf_mmap() 处理；对于本地对象，确保物理页面已获取，并设置
 * VM_PFNMAP、VM_DONTEXPAND 和 VM_DONTDUMP 标志。若设置了 map_wc 属性，
 * 则使用 write-combine 页保护属性。
 *
 * @shmem: shmem GEM 对象
 * @vma: 要映射区域的 VMA
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_mmap(struct drm_gem_shmem_object *shmem, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	if (drm_gem_is_imported(obj)) {
		/* Reset both vm_ops and vm_private_data, so we don't end up with
		 * vm_ops pointing to our implementation if the dma-buf backend
		 * doesn't set those fields.
		 */
		vma->vm_private_data = NULL;
		vma->vm_ops = NULL;

		ret = dma_buf_mmap(obj->dma_buf, vma, 0);

		/* Drop the reference drm_gem_mmap_obj() acquired.*/
		if (!ret)
			drm_gem_object_put(obj);

		return ret;
	}

	if (is_cow_mapping(vma->vm_flags))
		return -EINVAL;

	dma_resv_lock(shmem->base.resv, NULL);
	ret = drm_gem_shmem_get_pages_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	if (ret)
		return ret;

	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (shmem->map_wc)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_mmap);

/**
 * drm_gem_shmem_print_info() - Print &drm_gem_shmem_object info for debugfs
 * @shmem: shmem GEM object
 * @p: DRM printer
 * @indent: Tab indentation level
 */
void drm_gem_shmem_print_info(const struct drm_gem_shmem_object *shmem,
			      struct drm_printer *p, unsigned int indent)
{
	if (drm_gem_is_imported(&shmem->base))
		return;

	drm_printf_indent(p, indent, "pages_pin_count=%u\n", refcount_read(&shmem->pages_pin_count));
	drm_printf_indent(p, indent, "pages_use_count=%u\n", refcount_read(&shmem->pages_use_count));
	drm_printf_indent(p, indent, "vmap_use_count=%u\n", refcount_read(&shmem->vmap_use_count));
	drm_printf_indent(p, indent, "vaddr=%p\n", shmem->vaddr);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_print_info);

/**
 * drm_gem_shmem_get_sg_table - 提供固定页面的散列/聚集表
 *
 * 中文: 导出适用于 PRIME 使用的散列/聚集（SG）表。该函数通过标准 DMA 映射
 * API 将 shmem GEM 对象的固定物理页面转换为 SG 表。驱动程序如果要获取对象的
 * SG 表，应使用 drm_gem_shmem_get_pages_sgt() 而非直接调用此函数。
 *
 * @shmem: shmem GEM 对象
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or error pointer on failure.
 */
struct sg_table *drm_gem_shmem_get_sg_table(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	return drm_prime_pages_to_sg(obj->dev, shmem->pages, obj->size >> PAGE_SHIFT);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_get_sg_table);

static struct sg_table *drm_gem_shmem_get_pages_sgt_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;
	struct sg_table *sgt;

	if (shmem->sgt)
		return shmem->sgt;

	drm_WARN_ON(obj->dev, drm_gem_is_imported(obj));

	ret = drm_gem_shmem_get_pages_locked(shmem);
	if (ret)
		return ERR_PTR(ret);

	sgt = drm_gem_shmem_get_sg_table(shmem);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_put_pages;
	}
	/* Map the pages for use by the h/w. */
	ret = dma_map_sgtable(obj->dev->dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err_free_sgt;

	shmem->sgt = sgt;

	return sgt;

err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
err_put_pages:
	drm_gem_shmem_put_pages_locked(shmem);
	return ERR_PTR(ret);
}

/**
 * drm_gem_shmem_get_pages_sgt - 固定页面、执行 DMA 映射并返回 SG 表
 *
 * 中文: 这是驱动程序获取后端存储的主要函数。如果 SG 表尚不存在，会固定页面、
 * 执行 DMA 映射并创建 SG 表。该函数隐藏了 dma-buf 导入对象和本地分配对象
 * 之间的差异。驱动程序不应直接调用 drm_gem_shmem_get_sg_table()，
 * 而应使用此函数。
 *
 * @shmem: shmem GEM 对象
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or errno on failure.
 */
struct sg_table *drm_gem_shmem_get_pages_sgt(struct drm_gem_shmem_object *shmem)
{
	int ret;
	struct sg_table *sgt;

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return ERR_PTR(ret);
	sgt = drm_gem_shmem_get_pages_sgt_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	return sgt;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_get_pages_sgt);

/**
 * drm_gem_shmem_prime_import_sg_table - 从其他驱动的 SG 表导入 shmem GEM 对象
 *
 * 中文: 从通过 DMA-BUF 导出的其他驱动的 SG 表创建 shmem GEM 对象。
 * 使用 shmem 助手的驱动程序应将此函数设置为 &drm_driver.gem_prime_import_sg_table
 * 回调。该函数通过 __drm_gem_shmem_create() 创建一个私有 shmem 对象，
 * 并将传入的 SG 表直接赋给新对象。
 *
 * @dev: 导入目标设备
 * @attach: DMA-BUF 附件
 * @sgt: 固定页面的散列/聚集表
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object *
drm_gem_shmem_prime_import_sg_table(struct drm_device *dev,
				    struct dma_buf_attachment *attach,
				    struct sg_table *sgt)
{
	size_t size = PAGE_ALIGN(attach->dmabuf->size);
	struct drm_gem_shmem_object *shmem;

	shmem = __drm_gem_shmem_create(dev, size, true);
	if (IS_ERR(shmem))
		return ERR_CAST(shmem);

	shmem->sgt = sgt;

	drm_dbg_prime(dev, "size = %zu\n", size);

	return &shmem->base;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_import_sg_table);

/**
 * drm_gem_shmem_prime_import_no_map - Import dmabuf without mapping its sg_table
 * @dev: Device to import into
 * @dma_buf: dma-buf object to import
 *
 * Drivers that use the shmem helpers but also wants to import dmabuf without
 * mapping its sg_table can use this as their &drm_driver.gem_prime_import
 * implementation.
 */
struct drm_gem_object *drm_gem_shmem_prime_import_no_map(struct drm_device *dev,
							 struct dma_buf *dma_buf)
{
	struct dma_buf_attachment *attach;
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	size_t size;
	int ret;

	if (drm_gem_is_prime_exported_dma_buf(dev, dma_buf)) {
		/*
		 * Importing dmabuf exported from our own gem increases
		 * refcount on gem itself instead of f_count of dmabuf.
		 */
		obj = dma_buf->priv;
		drm_gem_object_get(obj);
		return obj;
	}

	attach = dma_buf_attach(dma_buf, dev->dev);
	if (IS_ERR(attach))
		return ERR_CAST(attach);

	get_dma_buf(dma_buf);

	size = PAGE_ALIGN(attach->dmabuf->size);

	shmem = __drm_gem_shmem_create(dev, size, true);
	if (IS_ERR(shmem)) {
		ret = PTR_ERR(shmem);
		goto fail_detach;
	}

	drm_dbg_prime(dev, "size = %zu\n", size);

	shmem->base.import_attach = attach;
	shmem->base.resv = dma_buf->resv;

	return &shmem->base;

fail_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_import_no_map);

/*
 * Kunit helpers
 */

#if IS_ENABLED(CONFIG_KUNIT)
int drm_gem_shmem_vmap(struct drm_gem_shmem_object *shmem, struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		return ret;
	ret = drm_gem_shmem_vmap_locked(shmem, map);
	dma_resv_unlock(obj->resv);

	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(drm_gem_shmem_vmap);

void drm_gem_shmem_vunmap(struct drm_gem_shmem_object *shmem, struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;

	dma_resv_lock_interruptible(obj->resv, NULL);
	drm_gem_shmem_vunmap_locked(shmem, map);
	dma_resv_unlock(obj->resv);
}
EXPORT_SYMBOL_IF_KUNIT(drm_gem_shmem_vunmap);

int drm_gem_shmem_madvise(struct drm_gem_shmem_object *shmem, int madv)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		return ret;
	ret = drm_gem_shmem_madvise_locked(shmem, madv);
	dma_resv_unlock(obj->resv);

	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(drm_gem_shmem_madvise);

int drm_gem_shmem_purge(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		return ret;
	drm_gem_shmem_purge_locked(shmem);
	dma_resv_unlock(obj->resv);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(drm_gem_shmem_purge);
#endif

MODULE_DESCRIPTION("DRM SHMEM memory-management helpers");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
