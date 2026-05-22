// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * Copyright (c) 2012 David Airlie <airlied@linux.ie>
 * Copyright (c) 2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * 中文注释: DRM VMA 偏移管理器 (VMA Offset Manager)
 *
 * 本文件实现了 DRM 子系统的 VMA (虚拟内存区域) 偏移管理器。该管理器负责
 * 将驱动程序依赖的任意内存区域映射到线性的用户地址空间中。它为调用者提供
 * 偏移量, 这些偏移量可用于 DRM 设备的地址空间 (address_space) 中。
 *
 * 核心功能:
 *   1. 偏移分配: 使用 drm_mm 作为后端来管理对象分配, 为每个 GEM 对象分配
 *      唯一的用户空间可见偏移量
 *   2. 快速查找: 使用红黑树加速偏移量查找 (优于 drm_mm 的 alloc/free 优化)
 *   3. 访问控制: 管理每个 open-file 上下文的节点访问权限, 确保只有被授权的
 *      文件描述符可以 mmap 特定节点
 *
 * 重要说明:
 *   - 所有参数和返回值 (除 drm_vma_node_offset_addr() 外) 均以页面数为单位
 *   - 在一个 address_space 上不能使用多个偏移管理器, 否则 mm 核心将无法
 *     正确拆除内存映射
 *   - 节点添加/删除操作内部有锁保护, 但节点分配和销毁由调用者负责
 */

#include <linux/export.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <drm/drm_mm.h>
#include <drm/drm_vma_manager.h>

/**
 * DOC: vma offset manager
 *
 * The vma-manager is responsible to map arbitrary driver-dependent memory
 * regions into the linear user address-space. It provides offsets to the
 * caller which can then be used on the address_space of the drm-device. It
 * takes care to not overlap regions, size them appropriately and to not
 * confuse mm-core by inconsistent fake vm_pgoff fields.
 * Drivers shouldn't use this for object placement in VMEM. This manager should
 * only be used to manage mappings into linear user-space VMs.
 *
 * We use drm_mm as backend to manage object allocations. But it is highly
 * optimized for alloc/free calls, not lookups. Hence, we use an rb-tree to
 * speed up offset lookups.
 *
 * You must not use multiple offset managers on a single address_space.
 * Otherwise, mm-core will be unable to tear down memory mappings as the VM will
 * no longer be linear.
 *
 * This offset manager works on page-based addresses. That is, every argument
 * and return code (with the exception of drm_vma_node_offset_addr()) is given
 * in number of pages, not number of bytes. That means, object sizes and offsets
 * must always be page-aligned (as usual).
 * If you want to get a valid byte-based user-space address for a given offset,
 * please see drm_vma_node_offset_addr().
 *
 * Additionally to offset management, the vma offset manager also handles access
 * management. For every open-file context that is allowed to access a given
 * node, you must call drm_vma_node_allow(). Otherwise, an mmap() call on this
 * open-file with the offset of the node will fail with -EACCES. To revoke
 * access again, use drm_vma_node_revoke(). However, the caller is responsible
 * for destroying already existing mappings, if required.
 */

/**
 * 中文注释: 初始化 VMA 偏移管理器
 * 创建一个新的偏移管理器实例。管理器内部使用 drm_mm 范围分配器来管理
 * 地址空间, 并使用读写锁保护并发访问。@page_offset 和 @size 均以页面
 * 数为单位。
 *
 * drm_vma_offset_manager_init - Initialize new offset-manager
 * @mgr: Manager object
 * @page_offset: Offset of available memory area (page-based)
 * @size: Size of available address space range (page-based)
 *
 * Initialize a new offset-manager. The offset and area size available for the
 * manager are given as @page_offset and @size. Both are interpreted as
 * page-numbers, not bytes.
 *
 * Adding/removing nodes from the manager is locked internally and protected
 * against concurrent access. However, node allocation and destruction is left
 * for the caller. While calling into the vma-manager, a given node must
 * always be guaranteed to be referenced.
 */
void drm_vma_offset_manager_init(struct drm_vma_offset_manager *mgr,
				 unsigned long page_offset, unsigned long size)
{
	rwlock_init(&mgr->vm_lock);
	drm_mm_init(&mgr->vm_addr_space_mm, page_offset, size);
}
EXPORT_SYMBOL(drm_vma_offset_manager_init);

/**
 * 中文注释: 销毁偏移管理器
 * 销毁之前通过 drm_vma_offset_manager_init() 创建的偏移管理器。调用者
 * 必须在销毁管理器之前移除所有已分配的节点, 否则 drm_mm 会拒绝释放
 * 资源。销毁后不得再访问该管理器。
 *
 * drm_vma_offset_manager_destroy() - Destroy offset manager
 * @mgr: Manager object
 *
 * Destroy an object manager which was previously created via
 * drm_vma_offset_manager_init(). The caller must remove all allocated nodes
 * before destroying the manager. Otherwise, drm_mm will refuse to free the
 * requested resources.
 *
 * The manager must not be accessed after this function is called.
 */
void drm_vma_offset_manager_destroy(struct drm_vma_offset_manager *mgr)
{
	drm_mm_takedown(&mgr->vm_addr_space_mm);
}
EXPORT_SYMBOL(drm_vma_offset_manager_destroy);

/**
 * 中文注释: 在偏移空间中查找节点 (已加锁)
 * 在 VMA 偏移管理器中根据起始地址和大小查找最匹配的节点。@start 可以
 * 指向某个有效区域内部的任意位置, 只要该节点覆盖了整个请求区域即可。
 * 查找使用红黑树实现, 时间复杂度 O(log n)。此函数要求在调用前已持有
 * 查找锁 (通过 drm_vma_offset_lock_lookup())。
 *
 * 典型用法是在持有查找锁的情况下进行弱引用查找:
 *   drm_vma_offset_lock_lookup(mgr);
 *   node = drm_vma_offset_lookup_locked(mgr, start, pages);
 *   if (node) kref_get_unless_zero(container_of(node, ...));
 *   drm_vma_offset_unlock_lookup(mgr);
 *
 * drm_vma_offset_lookup_locked() - Find node in offset space
 * @mgr: Manager object
 * @start: Start address for object (page-based)
 * @pages: Size of object (page-based)
 *
 * Find a node given a start address and object size. This returns the _best_
 * match for the given node. That is, @start may point somewhere into a valid
 * region and the given node will be returned, as long as the node spans the
 * whole requested area (given the size in number of pages as @pages).
 *
 * Note that before lookup the vma offset manager lookup lock must be acquired
 * with drm_vma_offset_lock_lookup(). See there for an example. This can then be
 * used to implement weakly referenced lookups using kref_get_unless_zero().
 *
 * Example:
 *
 * ::
 *
 *     drm_vma_offset_lock_lookup(mgr);
 *     node = drm_vma_offset_lookup_locked(mgr);
 *     if (node)
 *         kref_get_unless_zero(container_of(node, sth, entr));
 *     drm_vma_offset_unlock_lookup(mgr);
 *
 * RETURNS:
 * Returns NULL if no suitable node can be found. Otherwise, the best match
 * is returned. It's the caller's responsibility to make sure the node doesn't
 * get destroyed before the caller can access it.
 */
struct drm_vma_offset_node *drm_vma_offset_lookup_locked(struct drm_vma_offset_manager *mgr,
							 unsigned long start,
							 unsigned long pages)
{
	struct drm_mm_node *node, *best;
	struct rb_node *iter;
	unsigned long offset;

	iter = mgr->vm_addr_space_mm.interval_tree.rb_root.rb_node;
	best = NULL;

	while (likely(iter)) {
		node = rb_entry(iter, struct drm_mm_node, rb);
		offset = node->start;
		if (start >= offset) {
			iter = iter->rb_right;
			best = node;
			if (start == offset)
				break;
		} else {
			iter = iter->rb_left;
		}
	}

	/* verify that the node spans the requested area */
	if (best) {
		offset = best->start + best->size;
		if (offset < start + pages)
			best = NULL;
	}

	if (!best)
		return NULL;

	return container_of(best, struct drm_vma_offset_node, vm_node);
}
EXPORT_SYMBOL(drm_vma_offset_lookup_locked);

/**
 * 中文注释: 向管理器添加偏移节点
 * 将节点添加到 VMA 偏移管理器中。如果节点已添加过, 则不执行任何操作
 * 并返回 0。@pages 指定用户空间可见的分配大小 (以页面数为单位), 它不
 * 需要与底层内存对象的实际大小一致, 只是限制了用户空间可以映射的大小。
 *
 * 插入操作通过 drm_mm_insert_node() 在管理器的地址空间中分配一个空闲
 * 区域。使用写锁保护, 线程安全。
 *
 * drm_vma_offset_add() - Add offset node to manager
 * @mgr: Manager object
 * @node: Node to be added
 * @pages: Allocation size visible to user-space (in number of pages)
 *
 * Add a node to the offset-manager. If the node was already added, this does
 * nothing and return 0. @pages is the size of the object given in number of
 * pages.
 * After this call succeeds, you can access the offset of the node until it
 * is removed again.
 *
 * If this call fails, it is safe to retry the operation or call
 * drm_vma_offset_remove(), anyway. However, no cleanup is required in that
 * case.
 *
 * @pages is not required to be the same size as the underlying memory object
 * that you want to map. It only limits the size that user-space can map into
 * their address space.
 *
 * RETURNS:
 * 0 on success, negative error code on failure.
 */
int drm_vma_offset_add(struct drm_vma_offset_manager *mgr,
		       struct drm_vma_offset_node *node, unsigned long pages)
{
	int ret = 0;

	write_lock(&mgr->vm_lock);

	if (!drm_mm_node_allocated(&node->vm_node))
		ret = drm_mm_insert_node(&mgr->vm_addr_space_mm,
					 &node->vm_node, pages);

	write_unlock(&mgr->vm_lock);

	return ret;
}
EXPORT_SYMBOL(drm_vma_offset_add);

/**
 * 中文注释: 从管理器移除偏移节点
 * 从偏移管理器中移除节点。如果节点之前未添加过, 则不执行任何操作。
 * 移除后, 节点的偏移量和大小将重置为 0, 直到通过 drm_vma_offset_add()
 * 重新分配新偏移。辅助函数如 drm_vma_node_start() 和
 * drm_vma_node_offset_addr() 在未分配偏移时将返回 0。
 *
 * drm_vma_offset_remove() - Remove offset node from manager
 * @mgr: Manager object
 * @node: Node to be removed
 *
 * Remove a node from the offset manager. If the node wasn't added before, this
 * does nothing. After this call returns, the offset and size will be 0 until a
 * new offset is allocated via drm_vma_offset_add() again. Helper functions like
 * drm_vma_node_start() and drm_vma_node_offset_addr() will return 0 if no
 * offset is allocated.
 */
void drm_vma_offset_remove(struct drm_vma_offset_manager *mgr,
			   struct drm_vma_offset_node *node)
{
	write_lock(&mgr->vm_lock);

	if (drm_mm_node_allocated(&node->vm_node)) {
		drm_mm_remove_node(&node->vm_node);
		memset(&node->vm_node, 0, sizeof(node->vm_node));
	}

	write_unlock(&mgr->vm_lock);
}
EXPORT_SYMBOL(drm_vma_offset_remove);

static int vma_node_allow(struct drm_vma_offset_node *node,
			  struct drm_file *tag, bool ref_counted)
{
	struct rb_node **iter;
	struct rb_node *parent = NULL;
	struct drm_vma_offset_file *new, *entry;
	int ret = 0;

	/* Preallocate entry to avoid atomic allocations below. It is quite
	 * unlikely that an open-file is added twice to a single node so we
	 * don't optimize for this case. OOM is checked below only if the entry
	 * is actually used. */
	new = kmalloc_obj(*entry);

	write_lock(&node->vm_lock);

	iter = &node->vm_files.rb_node;

	while (likely(*iter)) {
		parent = *iter;
		entry = rb_entry(*iter, struct drm_vma_offset_file, vm_rb);

		if (tag == entry->vm_tag) {
			if (ref_counted)
				entry->vm_count++;
			goto unlock;
		} else if (tag > entry->vm_tag) {
			iter = &(*iter)->rb_right;
		} else {
			iter = &(*iter)->rb_left;
		}
	}

	if (!new) {
		ret = -ENOMEM;
		goto unlock;
	}

	new->vm_tag = tag;
	new->vm_count = 1;
	rb_link_node(&new->vm_rb, parent, iter);
	rb_insert_color(&new->vm_rb, &node->vm_files);
	new = NULL;

unlock:
	write_unlock(&node->vm_lock);
	kfree(new);
	return ret;
}

/**
 * 中文注释: 将 open-file 添加到允许用户列表 (引用计数)
 * 将 @tag 添加到节点的允许 open-file 列表中。如果 @tag 已在列表中,
 * 引用计数递增。访问控制列表在 drm_vma_offset_add() 和
 * drm_vma_offset_remove() 之间保持不变, 即使节点当前未添加到任何
 * 偏移管理器也可以调用。销毁节点前必须移除相同次数的 open-file。
 *
 * drm_vma_node_allow - Add open-file to list of allowed users
 * @node: Node to modify
 * @tag: Tag of file to remove
 *
 * Add @tag to the list of allowed open-files for this node. If @tag is
 * already on this list, the ref-count is incremented.
 *
 * The list of allowed-users is preserved across drm_vma_offset_add() and
 * drm_vma_offset_remove() calls. You may even call it if the node is currently
 * not added to any offset-manager.
 *
 * You must remove all open-files the same number of times as you added them
 * before destroying the node. Otherwise, you will leak memory.
 *
 * This is locked against concurrent access internally.
 *
 * RETURNS:
 * 0 on success, negative error code on internal failure (out-of-mem)
 */
int drm_vma_node_allow(struct drm_vma_offset_node *node, struct drm_file *tag)
{
	return vma_node_allow(node, tag, true);
}
EXPORT_SYMBOL(drm_vma_node_allow);

/**
 * 中文注释: 将 open-file 添加到允许用户列表 (非引用计数)
 * 与 drm_vma_node_allow() 类似, 但不维护引用计数。因此对于每个节点,
 * drm_vma_node_revoke() 只需调用一次。适用于仅需一次性授权的场景。
 *
 * drm_vma_node_allow_once - Add open-file to list of allowed users
 * @node: Node to modify
 * @tag: Tag of file to remove
 *
 * Add @tag to the list of allowed open-files for this node.
 *
 * The list of allowed-users is preserved across drm_vma_offset_add() and
 * drm_vma_offset_remove() calls. You may even call it if the node is currently
 * not added to any offset-manager.
 *
 * This is not ref-counted unlike drm_vma_node_allow() hence drm_vma_node_revoke()
 * should only be called once after this.
 *
 * This is locked against concurrent access internally.
 *
 * RETURNS:
 * 0 on success, negative error code on internal failure (out-of-mem)
 */
int drm_vma_node_allow_once(struct drm_vma_offset_node *node, struct drm_file *tag)
{
	return vma_node_allow(node, tag, false);
}
EXPORT_SYMBOL(drm_vma_node_allow_once);

/**
 * 中文注释: 从允许用户列表中移除 open-file
 * 递减 @tag 在节点允许列表中的引用计数。当引用计数降为零时, 从列表中
 * 移除该条目并释放内存。对于每次 drm_vma_node_allow() 调用, 必须对应
 * 调用一次此函数。如果 @tag 不在列表中, 则不执行任何操作。
 *
 * drm_vma_node_revoke - Remove open-file from list of allowed users
 * @node: Node to modify
 * @tag: Tag of file to remove
 *
 * Decrement the ref-count of @tag in the list of allowed open-files on @node.
 * If the ref-count drops to zero, remove @tag from the list. You must call
 * this once for every drm_vma_node_allow() on @tag.
 *
 * This is locked against concurrent access internally.
 *
 * If @tag is not on the list, nothing is done.
 */
void drm_vma_node_revoke(struct drm_vma_offset_node *node,
			 struct drm_file *tag)
{
	struct drm_vma_offset_file *entry;
	struct rb_node *iter;

	write_lock(&node->vm_lock);

	iter = node->vm_files.rb_node;
	while (likely(iter)) {
		entry = rb_entry(iter, struct drm_vma_offset_file, vm_rb);
		if (tag == entry->vm_tag) {
			if (!--entry->vm_count) {
				rb_erase(&entry->vm_rb, &node->vm_files);
				kfree(entry);
			}
			break;
		} else if (tag > entry->vm_tag) {
			iter = iter->rb_right;
		} else {
			iter = iter->rb_left;
		}
	}

	write_unlock(&node->vm_lock);
}
EXPORT_SYMBOL(drm_vma_node_revoke);

/**
 * 中文注释: 检查 open-file 是否被授权访问
 * 在节点的允许列表中搜索 @tag, 判断该文件描述符是否有权限访问此节点。
 * 使用读锁保护, 支持并发查找。mmap() 系统调用在处理时通常会调用此
 * 函数来验证权限。
 *
 * drm_vma_node_is_allowed - Check whether an open-file is granted access
 * @node: Node to check
 * @tag: Tag of file to remove
 *
 * Search the list in @node whether @tag is currently on the list of allowed
 * open-files (see drm_vma_node_allow()).
 *
 * This is locked against concurrent access internally.
 *
 * RETURNS:
 * true if @filp is on the list
 */
bool drm_vma_node_is_allowed(struct drm_vma_offset_node *node,
			     struct drm_file *tag)
{
	struct drm_vma_offset_file *entry;
	struct rb_node *iter;

	read_lock(&node->vm_lock);

	iter = node->vm_files.rb_node;
	while (likely(iter)) {
		entry = rb_entry(iter, struct drm_vma_offset_file, vm_rb);
		if (tag == entry->vm_tag)
			break;
		else if (tag > entry->vm_tag)
			iter = iter->rb_right;
		else
			iter = iter->rb_left;
	}

	read_unlock(&node->vm_lock);

	return iter;
}
EXPORT_SYMBOL(drm_vma_node_is_allowed);
