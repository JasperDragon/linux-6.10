// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drm gem DMA helper functions
 *
 * Copyright (C) 2012 Sascha Hauer, Pengutronix
 *
 * Based on Samsung Exynos code
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 */

/**
 * DOC: DMA GEM 对象管理概述 (中文)
 *
 * 该文件实现了基于 DMA 的 GEM 对象辅助函数。DMA GEM 对象为设备提供连续的
 * 内存缓冲区，适用于以下两种场景：
 *
 * 1. 不支持散列/聚集 DMA（SG DMA）的设备——通过 CMA（连续内存分配器）
 *    提供物理连续的内存。
 * 2. 通过 IOMMU 访问内存总线的设备——内存页可以物理离散但在 IOVA 空间中
 *    连续，对设备表现为连续内存。
 *
 * 核心功能包括：
 *   - 创建和初始化 DMA GEM 对象，分配 DMA 一致内存
 *   - 释放对象及其后端 DMA 内存
 *   - 生成 SG 表用于 PRIME 缓冲区共享
 *   - 提供内核虚拟地址映射（vmap）
 *   - 支持 mmap 操作映射到用户空间
 *   - 创建 dumb 缓冲区对象
 *
 * 该辅助层通过 struct drm_gem_dma_object 结构体管理每个 DMA GEM 对象，
 * 并提供了 _object_ 后缀的包装函数自动完成类型转换。
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_dumb_buffers.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vma_manager.h>

/**
 * DOC: dma helpers
 *
 * The DRM GEM/DMA helpers are a means to provide buffer objects that are
 * presented to the device as a contiguous chunk of memory. This is useful
 * for devices that do not support scatter-gather DMA (either directly or
 * by using an intimately attached IOMMU).
 *
 * For devices that access the memory bus through an (external) IOMMU then
 * the buffer objects are allocated using a traditional page-based
 * allocator and may be scattered through physical memory. However they
 * are contiguous in the IOVA space so appear contiguous to devices using
 * them.
 *
 * For other devices then the helpers rely on CMA to provide buffer
 * objects that are physically contiguous in memory.
 *
 * For GEM callback helpers in struct &drm_gem_object functions, see likewise
 * named functions with an _object_ infix (e.g., drm_gem_dma_object_vmap() wraps
 * drm_gem_dma_vmap()). These helpers perform the necessary type conversion.
 */

static const struct drm_gem_object_funcs drm_gem_dma_default_funcs = {
	.free = drm_gem_dma_object_free,
	.print_info = drm_gem_dma_object_print_info,
	.get_sg_table = drm_gem_dma_object_get_sg_table,
	.vmap = drm_gem_dma_object_vmap,
	.mmap = drm_gem_dma_object_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

/**
 * __drm_gem_dma_create - Create a GEM DMA object without allocating memory
 * @drm: DRM device
 * @size: size of the object to allocate
 * @private: true if used for internal purposes
 *
 * This function creates and initializes a GEM DMA object of the given size,
 * but doesn't allocate any memory to back the object.
 *
 * Returns:
 * A struct drm_gem_dma_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
static struct drm_gem_dma_object *
__drm_gem_dma_create(struct drm_device *drm, size_t size, bool private)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;
	int ret = 0;

	if (drm->driver->gem_create_object) {
		gem_obj = drm->driver->gem_create_object(drm, size);
		if (IS_ERR(gem_obj))
			return ERR_CAST(gem_obj);
		dma_obj = to_drm_gem_dma_obj(gem_obj);
	} else {
		dma_obj = kzalloc_obj(*dma_obj);
		if (!dma_obj)
			return ERR_PTR(-ENOMEM);
		gem_obj = &dma_obj->base;
	}

	if (!gem_obj->funcs)
		gem_obj->funcs = &drm_gem_dma_default_funcs;

	if (private) {
		drm_gem_private_object_init(drm, gem_obj, size);

		/* Always use writecombine for dma-buf mappings */
		dma_obj->map_noncoherent = false;
	} else {
		ret = drm_gem_object_init(drm, gem_obj, size);
	}
	if (ret)
		goto error;

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret) {
		drm_gem_object_release(gem_obj);
		goto error;
	}

	return dma_obj;

error:
	kfree(dma_obj);
	return ERR_PTR(ret);
}

/**
 * drm_gem_dma_create - 分配指定大小的 DMA GEM 对象
 *
 * 中文: 创建 DMA GEM 对象并分配后端存储内存。分配的内存占用连续的
 * 总线地址空间。对于直接连接到内存总线的设备，分配的内存将是物理连续的；
 * 对于通过 IOMMU 访问的设备，只需 IOVA 连续即可满足 DMA 要求。
 * 该函数使用 dma_alloc_wc()（或 dma_alloc_noncoherent() 用于非一致性映射）
 * 分配写结合（write-combine）DMA 内存。
 *
 * @drm: DRM 设备
 * @size: 要分配的对象大小
 *
 * Returns:
 * A struct drm_gem_dma_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_dma_object *drm_gem_dma_create(struct drm_device *drm,
					      size_t size)
{
	struct drm_gem_dma_object *dma_obj;
	int ret;

	size = round_up(size, PAGE_SIZE);

	dma_obj = __drm_gem_dma_create(drm, size, false);
	if (IS_ERR(dma_obj))
		return dma_obj;

	if (dma_obj->map_noncoherent) {
		dma_obj->vaddr = dma_alloc_noncoherent(drm_dev_dma_dev(drm),
						       size,
						       &dma_obj->dma_addr,
						       DMA_TO_DEVICE,
						       GFP_KERNEL | __GFP_NOWARN);
	} else {
		dma_obj->vaddr = dma_alloc_wc(drm_dev_dma_dev(drm), size,
					      &dma_obj->dma_addr,
					      GFP_KERNEL | __GFP_NOWARN);
	}
	if (!dma_obj->vaddr) {
		drm_dbg(drm, "failed to allocate buffer with size %zu\n",
			 size);
		ret = -ENOMEM;
		goto error;
	}

	return dma_obj;

error:
	drm_gem_object_put(&dma_obj->base);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gem_dma_create);

/**
 * drm_gem_dma_create_with_handle - allocate an object with the given size and
 *     return a GEM handle to it
 * @file_priv: DRM file-private structure to register the handle for
 * @drm: DRM device
 * @size: size of the object to allocate
 * @handle: return location for the GEM handle
 *
 * This function creates a DMA GEM object, allocating a chunk of memory as
 * backing store. The GEM object is then added to the list of object associated
 * with the given file and a handle to it is returned.
 *
 * The allocated memory will occupy a contiguous chunk of bus address space.
 * See drm_gem_dma_create() for more details.
 *
 * Returns:
 * A struct drm_gem_dma_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
static struct drm_gem_dma_object *
drm_gem_dma_create_with_handle(struct drm_file *file_priv,
			       struct drm_device *drm, size_t size,
			       uint32_t *handle)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	dma_obj = drm_gem_dma_create(drm, size);
	if (IS_ERR(dma_obj))
		return dma_obj;

	gem_obj = &dma_obj->base;

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(gem_obj);
	if (ret)
		return ERR_PTR(ret);

	return dma_obj;
}

/**
 * drm_gem_dma_free - 释放 DMA GEM 对象占用的资源
 *
 * 中文: 释放 DMA GEM 对象的后端内存，清理 GEM 对象状态并释放存储对象
 * 本身的内存。对于 dma-buf 导入的缓冲区，如果设置了虚拟地址，会先解除
 * vmap 映射；对于本地分配的对象，使用 dma_free_wc() 或 dma_free_noncoherent()
 * 释放 DMA 内存。
 *
 * @dma_obj: 要释放的 DMA GEM 对象
 */
void drm_gem_dma_free(struct drm_gem_dma_object *dma_obj)
{
	struct drm_gem_object *gem_obj = &dma_obj->base;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(dma_obj->vaddr);

	if (drm_gem_is_imported(gem_obj)) {
		if (dma_obj->vaddr)
			dma_buf_vunmap_unlocked(gem_obj->import_attach->dmabuf, &map);
		drm_prime_gem_destroy(gem_obj, dma_obj->sgt);
	} else if (dma_obj->vaddr) {
		if (dma_obj->map_noncoherent)
			dma_free_noncoherent(drm_dev_dma_dev(gem_obj->dev),
					     dma_obj->base.size,
					     dma_obj->vaddr, dma_obj->dma_addr,
					     DMA_TO_DEVICE);
		else
			dma_free_wc(drm_dev_dma_dev(gem_obj->dev),
				    dma_obj->base.size, dma_obj->vaddr,
				    dma_obj->dma_addr);
	}

	drm_gem_object_release(gem_obj);

	kfree(dma_obj);
}
EXPORT_SYMBOL_GPL(drm_gem_dma_free);

/**
 * drm_gem_dma_dumb_create_internal - create a dumb buffer object
 * @file_priv: DRM file-private structure to create the dumb buffer for
 * @drm: DRM device
 * @args: IOCTL data
 *
 * This aligns the pitch and size arguments to the minimum required. This is
 * an internal helper that can be wrapped by a driver to account for hardware
 * with more specific alignment requirements. It should not be used directly
 * as their &drm_driver.dumb_create callback.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_dma_dumb_create_internal(struct drm_file *file_priv,
				     struct drm_device *drm,
				     struct drm_mode_create_dumb *args)
{
	unsigned int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	struct drm_gem_dma_object *dma_obj;

	if (args->pitch < min_pitch)
		args->pitch = min_pitch;

	if (args->size < args->pitch * args->height)
		args->size = args->pitch * args->height;

	dma_obj = drm_gem_dma_create_with_handle(file_priv, drm, args->size,
						 &args->handle);
	return PTR_ERR_OR_ZERO(dma_obj);
}
EXPORT_SYMBOL_GPL(drm_gem_dma_dumb_create_internal);

/**
 * drm_gem_dma_dumb_create - 创建 dumb DMA 缓冲区对象
 *
 * 中文: 计算 dumb 缓冲区的 pitch 并向上取整到整数字节/像素。
 * 对于没有额外硬件 pitch 限制的驱动程序，可直接将此函数用作
 * &drm_driver.dumb_create 回调。对于有特殊硬件对齐要求的驱动，
 * 可调整用户空间设置的字段后调用 drm_gem_dma_dumb_create_internal()。
 *
 * @file_priv: 创建 dumb 缓冲区的 DRM 文件私有结构
 * @drm: DRM 设备
 * @args: IOCTL 数据
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_dma_dumb_create(struct drm_file *file_priv,
			    struct drm_device *drm,
			    struct drm_mode_create_dumb *args)
{
	struct drm_gem_dma_object *dma_obj;
	int ret;

	ret = drm_mode_size_dumb(drm, args, 0, 0);
	if (ret)
		return ret;

	dma_obj = drm_gem_dma_create_with_handle(file_priv, drm, args->size,
						 &args->handle);
	return PTR_ERR_OR_ZERO(dma_obj);
}
EXPORT_SYMBOL_GPL(drm_gem_dma_dumb_create);

const struct vm_operations_struct drm_gem_dma_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};
EXPORT_SYMBOL_GPL(drm_gem_dma_vm_ops);

#ifndef CONFIG_MMU
/**
 * drm_gem_dma_get_unmapped_area - propose address for mapping in noMMU cases
 * @filp: file object
 * @addr: memory address
 * @len: buffer size
 * @pgoff: page offset
 * @flags: memory flags
 *
 * This function is used in noMMU platforms to propose address mapping
 * for a given buffer.
 * It's intended to be used as a direct handler for the struct
 * &file_operations.get_unmapped_area operation.
 *
 * Returns:
 * mapping address on success or a negative error code on failure.
 */
unsigned long drm_gem_dma_get_unmapped_area(struct file *filp,
					    unsigned long addr,
					    unsigned long len,
					    unsigned long pgoff,
					    unsigned long flags)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *obj = NULL;
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_vma_offset_node *node;

	if (drm_dev_is_unplugged(dev))
		return -ENODEV;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  pgoff,
						  len >> PAGE_SHIFT);
	if (likely(node)) {
		obj = container_of(node, struct drm_gem_object, vma_node);
		/*
		 * When the object is being freed, after it hits 0-refcnt it
		 * proceeds to tear down the object. In the process it will
		 * attempt to remove the VMA offset and so acquire this
		 * mgr->vm_lock.  Therefore if we find an object with a 0-refcnt
		 * that matches our range, we know it is in the process of being
		 * destroyed and will be freed as soon as we release the lock -
		 * so we have to check for the 0-refcnted object and treat it as
		 * invalid.
		 */
		if (!kref_get_unless_zero(&obj->refcount))
			obj = NULL;
	}

	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!obj)
		return -EINVAL;

	if (!drm_vma_node_is_allowed(node, priv)) {
		drm_gem_object_put(obj);
		return -EACCES;
	}

	dma_obj = to_drm_gem_dma_obj(obj);

	drm_gem_object_put(obj);

	return dma_obj->vaddr ? (unsigned long)dma_obj->vaddr : -EINVAL;
}
EXPORT_SYMBOL_GPL(drm_gem_dma_get_unmapped_area);
#endif

/**
 * drm_gem_dma_print_info() - Print &drm_gem_dma_object info for debugfs
 * @dma_obj: DMA GEM object
 * @p: DRM printer
 * @indent: Tab indentation level
 *
 * This function prints dma_addr and vaddr for use in e.g. debugfs output.
 */
void drm_gem_dma_print_info(const struct drm_gem_dma_object *dma_obj,
			    struct drm_printer *p, unsigned int indent)
{
	drm_printf_indent(p, indent, "dma_addr=%pad\n", &dma_obj->dma_addr);
	drm_printf_indent(p, indent, "vaddr=%p\n", dma_obj->vaddr);
}
EXPORT_SYMBOL(drm_gem_dma_print_info);

/**
 * drm_gem_dma_get_sg_table - 提供 DMA GEM 对象的散列/聚集表
 *
 * 中文: 通过调用标准 DMA 映射 API 导出散列/聚集（SG）表。
 * 该函数从 dma_addr 和 vaddr 创建 SG 表，适用于 PRIME 缓冲区共享。
 * 调用 dma_get_sgtable() 获取底层设备的 DMA 映射信息。
 *
 * @dma_obj: DMA GEM 对象
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or NULL on failure.
 */
struct sg_table *drm_gem_dma_get_sg_table(struct drm_gem_dma_object *dma_obj)
{
	struct drm_gem_object *obj = &dma_obj->base;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(drm_dev_dma_dev(obj->dev), sgt, dma_obj->vaddr,
			      dma_obj->dma_addr, obj->size);
	if (ret < 0)
		goto out;

	return sgt;

out:
	kfree(sgt);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gem_dma_get_sg_table);

/**
 * drm_gem_dma_prime_import_sg_table - 从其他驱动的 SG 表导入 DMA GEM 对象
 *
 * 中文: 从另一个驱动通过 DMA-BUF 导出的散列/聚集表创建 DMA GEM 对象。
 * 注意：导入的缓冲区必须在内存中物理连续（即 SG 表只能包含一个条目）。
 * 使用 DMA 助手的驱动程序应将此函数设置为 &drm_driver.gem_prime_import_sg_table
 * 回调。该函数会检查 SG 表的连续性，不连续则返回错误。
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
drm_gem_dma_prime_import_sg_table(struct drm_device *dev,
				  struct dma_buf_attachment *attach,
				  struct sg_table *sgt)
{
	struct drm_gem_dma_object *dma_obj;

	/* check if the entries in the sg_table are contiguous */
	if (drm_prime_get_contiguous_size(sgt) < attach->dmabuf->size)
		return ERR_PTR(-EINVAL);

	/* Create a DMA GEM buffer. */
	dma_obj = __drm_gem_dma_create(dev, attach->dmabuf->size, true);
	if (IS_ERR(dma_obj))
		return ERR_CAST(dma_obj);

	dma_obj->dma_addr = sg_dma_address(sgt->sgl);
	dma_obj->sgt = sgt;

	drm_dbg_prime(dev, "dma_addr = %pad, size = %zu\n", &dma_obj->dma_addr,
		      attach->dmabuf->size);

	return &dma_obj->base;
}
EXPORT_SYMBOL_GPL(drm_gem_dma_prime_import_sg_table);

/**
 * drm_gem_dma_vmap - 将 DMA GEM 对象映射到内核虚拟地址空间
 *
 * 中文: 将缓冲区映射到内核虚拟地址空间。由于 DMA 缓冲区在分配时已经映射到
 * 内核虚拟地址空间（dma_alloc_wc() 返回内核虚拟地址），此函数直接返回缓存的
 * 虚拟地址，无需实际建立新映射。
 *
 * @dma_obj: DMA GEM 对象
 * @map: 返回 DMA GEM 对象后端存储的内核虚拟地址
 *
 * Returns:
 * 0 on success, or a negative error code otherwise.
 */
int drm_gem_dma_vmap(struct drm_gem_dma_object *dma_obj,
		     struct iosys_map *map)
{
	iosys_map_set_vaddr(map, dma_obj->vaddr);

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gem_dma_vmap);

/**
 * drm_gem_dma_mmap - 对导出的 DMA GEM 对象执行内存映射
 *
 * 中文: 将 DMA 缓冲区映射到用户空间进程的地址空间。与通常的 GEM VMA 设置
 * 不同，该函数立即将整个对象映射到页表中（通过 dma_mmap_wc() 或
 * dma_mmap_pages()），而不是使用按需缺页处理。对于非一致性映射使用
 * dma_mmap_pages()，对于写结合映射使用 dma_mmap_wc()。
 *
 * @dma_obj: DMA GEM 对象
 * @vma: 要映射区域的 VMA
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_dma_mmap(struct drm_gem_dma_object *dma_obj, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = &dma_obj->base;
	int ret;

	/*
	 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
	vm_flags_mod(vma, VM_DONTDUMP | VM_DONTEXPAND, VM_PFNMAP);

	if (dma_obj->map_noncoherent) {
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

		ret = dma_mmap_pages(drm_dev_dma_dev(dma_obj->base.dev),
				     vma, vma->vm_end - vma->vm_start,
				     virt_to_page(dma_obj->vaddr));
	} else {
		ret = dma_mmap_wc(drm_dev_dma_dev(dma_obj->base.dev), vma,
				  dma_obj->vaddr, dma_obj->dma_addr,
				  vma->vm_end - vma->vm_start);
	}
	if (ret)
		drm_gem_vm_close(vma);

	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_dma_mmap);

/**
 * drm_gem_dma_prime_import_sg_table_vmap - PRIME import another driver's
 *	scatter/gather table and get the virtual address of the buffer
 * @dev: DRM device
 * @attach: DMA-BUF attachment
 * @sgt: Scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table using
 * drm_gem_dma_prime_import_sg_table() and uses dma_buf_vmap() to get the kernel
 * virtual address. This ensures that a DMA GEM object always has its virtual
 * address set. This address is released when the object is freed.
 *
 * This function can be used as the &drm_driver.gem_prime_import_sg_table
 * callback. The &DRM_GEM_DMA_DRIVER_OPS_VMAP macro provides a shortcut to set
 * the necessary DRM driver operations.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object *
drm_gem_dma_prime_import_sg_table_vmap(struct drm_device *dev,
				       struct dma_buf_attachment *attach,
				       struct sg_table *sgt)
{
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *obj;
	struct iosys_map map;
	int ret;

	ret = dma_buf_vmap_unlocked(attach->dmabuf, &map);
	if (ret) {
		drm_err(dev, "Failed to vmap PRIME buffer\n");
		return ERR_PTR(ret);
	}

	obj = drm_gem_dma_prime_import_sg_table(dev, attach, sgt);
	if (IS_ERR(obj)) {
		dma_buf_vunmap_unlocked(attach->dmabuf, &map);
		return obj;
	}

	dma_obj = to_drm_gem_dma_obj(obj);
	dma_obj->vaddr = map.vaddr;

	return obj;
}
EXPORT_SYMBOL(drm_gem_dma_prime_import_sg_table_vmap);

MODULE_DESCRIPTION("DRM DMA memory-management helpers");
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
