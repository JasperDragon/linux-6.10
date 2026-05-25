// SPDX-License-Identifier: GPL-2.0-only
/* binder_alloc.c
 *
 * Android IPC Subsystem
 *
 * Copyright (C) 2007-2017 Google, Inc.
 */

/*
 * binder_alloc — Binder 驱动共享内存分配器概述
 * ==============================================
 *
 * binder_alloc 负责管理 Binder IPC 驱动中每个进程 (binder_proc) 的共享内存。
 * 每个 binder_proc 在打开 /dev/binder 设备时创建一个 binder_alloc 实例，
 * 该实例通过 mmap(/dev/binder) 将内核分配的物理内存映射到用户空间，
 * 从而实现进程间通信时零拷贝的数据传输。
 *
 * 核心数据结构:
 *   - free_buffers (红黑树): 按大小排序的空闲缓冲区集合，用于 best-fit 分配
 *   - allocated_buffers (红黑树): 按地址排序的已分配缓冲区集合，用于快速查找
 *   - buffers (链表): 所有缓冲区按地址顺序连接，便于合并操作
 *   - pages[]: 物理页面数组，维护每个页面的分配状态和 LRU 回收信息
 *
 * 分配策略 (best-fit):
 *   1. 在 free_buffers 红黑树中查找大小 >= 请求大小的最小空闲块
 *   2. 如果找到的块比请求大，将其分裂为已分配块和新的空闲块
 *   3. 新空闲块重新插入 free_buffers 供后续使用
 *
 * 释放策略:
 *   1. 将释放的缓冲区标记为空闲
 *   2. 检查前后相邻缓冲区是否也为空闲，若是则合并
 *   3. 合并操作通过 binder_delete_free_buffer() 删除被吞并的缓冲区节点
 *
 * 页面管理:
 *   - 按需分配物理页面，通过 vm_insert_page() 插入进程地址空间
 *   - 页面回收使用 LRU 链表 + shrinker 机制，在系统内存压力时释放
 *   - binder_buffer 在 Binder 事务中携带 RPC 数据 (Parcel)
 *
 * 异步事务空间:
 *   每个 binder_alloc 预留 buffer_size/2 的空间给异步 (oneway) 事务，
 *   通过 free_async_space 字段跟踪剩余异步空间。
 *   当异步空间不足时，检测并限制单发垃圾调用者 (oneway spam)。
 *
 * 线程安全:
 *   使用 alloc->mutex 保护内部数据结构 (红黑树、链表)，
 *   页面操作需同时持有 mmap_lock 和 alloc->mutex。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/list.h>
#include <linux/sched/mm.h>
#include <linux/module.h>
#include <linux/rtmutex.h>
#include <linux/rbtree.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/list_lru.h>
#include <linux/ratelimit.h>
#include <asm/cacheflush.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/sizes.h>
#include <kunit/visibility.h>
#include "binder_alloc.h"
#include "binder_trace.h"

static struct list_lru binder_freelist;

static DEFINE_MUTEX(binder_alloc_mmap_lock);

enum {
	BINDER_DEBUG_USER_ERROR             = 1U << 0,
	BINDER_DEBUG_OPEN_CLOSE             = 1U << 1,
	BINDER_DEBUG_BUFFER_ALLOC           = 1U << 2,
	BINDER_DEBUG_BUFFER_ALLOC_ASYNC     = 1U << 3,
};
static uint32_t binder_alloc_debug_mask = BINDER_DEBUG_USER_ERROR;

module_param_named(debug_mask, binder_alloc_debug_mask,
		   uint, 0644);

#define binder_alloc_debug(mask, x...) \
	do { \
		if (binder_alloc_debug_mask & mask) \
			pr_info_ratelimited(x); \
	} while (0)

/*
 * binder_buffer_next / binder_buffer_prev - 获取 buffers 链表中相邻的缓冲区
 * @buffer: 当前缓冲区
 *
 * 通过 buffers 链表 (按地址排序的双向链表) 获取前驱/后继缓冲区。
 * 用于释放时的合并逻辑: 检查相邻缓冲区是否为空闲。
 */
static struct binder_buffer *binder_buffer_next(struct binder_buffer *buffer)
{
	return list_entry(buffer->entry.next, struct binder_buffer, entry);
}

/*
 * binder_buffer_prev - 获取 buffers 链表中前一个缓冲区
 */
static struct binder_buffer *binder_buffer_prev(struct binder_buffer *buffer)
{
	return list_entry(buffer->entry.prev, struct binder_buffer, entry);
}

VISIBLE_IF_KUNIT size_t binder_alloc_buffer_size(struct binder_alloc *alloc,
						 struct binder_buffer *buffer)
{
	if (list_is_last(&buffer->entry, &alloc->buffers))
		return alloc->vm_start + alloc->buffer_size - buffer->user_data;
	return binder_buffer_next(buffer)->user_data - buffer->user_data;
}
EXPORT_SYMBOL_IF_KUNIT(binder_alloc_buffer_size);

/*
 * binder_insert_free_buffer - 将空闲缓冲区插入 free_buffers 红黑树
 * @alloc: binder_alloc 实例
 * @new_buffer: 要插入的空闲缓冲区
 *
 * 以缓冲区大小 (binder_alloc_buffer_size) 为键值插入红黑树。
 * best-fit 分配算法依赖此排序: 在树中查找大小 >= 请求大小的最小节点。
 * 调用者必须确保 new_buffer->free == 1。
 */
static void binder_insert_free_buffer(struct binder_alloc *alloc,
				      struct binder_buffer *new_buffer)
{
	struct rb_node **p = &alloc->free_buffers.rb_node;
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;
	size_t buffer_size;
	size_t new_buffer_size;

	BUG_ON(!new_buffer->free);

	new_buffer_size = binder_alloc_buffer_size(alloc, new_buffer);

	binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: add free buffer, size %zd, at %p\n",
		      alloc->pid, new_buffer_size, new_buffer);

	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);
		BUG_ON(!buffer->free);

		buffer_size = binder_alloc_buffer_size(alloc, buffer);

		if (new_buffer_size < buffer_size)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &alloc->free_buffers);
}

/*
 * binder_insert_allocated_buffer_locked - 将已分配缓冲区插入 allocated_buffers 红黑树
 * @alloc: binder_alloc 实例
 * @new_buffer: 要插入的已分配缓冲区
 *
 * 以用户空间地址 (user_data) 为键值插入红黑树。
 * 用于高效查找: 给定用户空间指针，在 O(log N) 时间内找到对应的 binder_buffer。
 * 调用者必须持有 alloc->mutex。
 */
static void binder_insert_allocated_buffer_locked(
		struct binder_alloc *alloc, struct binder_buffer *new_buffer)
{
	struct rb_node **p = &alloc->allocated_buffers.rb_node;
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;

	BUG_ON(new_buffer->free);

	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);
		BUG_ON(buffer->free);

		if (new_buffer->user_data < buffer->user_data)
			p = &parent->rb_left;
		else if (new_buffer->user_data > buffer->user_data)
			p = &parent->rb_right;
		else
			BUG();
	}
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &alloc->allocated_buffers);
}

/*
 * binder_alloc_prepare_to_free_locked - 根据用户空间地址查找待释放的缓冲区
 * @alloc: binder_alloc 实例
 * @user_ptr: 用户空间缓冲区起始地址
 *
 * 在 allocated_buffers 红黑树中按地址二分查找 binder_buffer。
 * 找到后检查 allow_user_free 标志，防止内核正在使用中的缓冲区被用户线程误释放。
 * 调用者必须持有 alloc->mutex。
 *
 * 返回: 缓冲区指针，未找到返回 NULL，权限错误返回 ERR_PTR(-EPERM)
 */
static struct binder_buffer *binder_alloc_prepare_to_free_locked(
		struct binder_alloc *alloc,
		unsigned long user_ptr)
{
	struct rb_node *n = alloc->allocated_buffers.rb_node;
	struct binder_buffer *buffer;

	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		BUG_ON(buffer->free);

		if (user_ptr < buffer->user_data) {
			n = n->rb_left;
		} else if (user_ptr > buffer->user_data) {
			n = n->rb_right;
		} else {
			/*
			 * Guard against user threads attempting to
			 * free the buffer when in use by kernel or
			 * after it's already been freed.
			 */
			if (!buffer->allow_user_free)
				return ERR_PTR(-EPERM);
			buffer->allow_user_free = 0;
			return buffer;
		}
	}
	return NULL;
}

/*
 * binder_alloc_prepare_to_free - 根据用户空间地址查找待释放缓冲区 (对外接口)
 * @alloc: binder_alloc 实例
 * @user_ptr: 用户空间缓冲区起始地址
 *
 * 持有 alloc->mutex 后调用 binder_alloc_prepare_to_free_locked()。
 * 这是从 Binder 驱动 ioctl (BINDER_FREE) 路径调用的入口。
 *
 * 返回: 缓冲区指针，未找到返回 NULL，权限错误返回 ERR_PTR(-EPERM)
 */

/**
 * binder_alloc_prepare_to_free() - get buffer given user ptr
 * @alloc:	binder_alloc for this proc
 * @user_ptr:	User pointer to buffer data
 *
 * Validate userspace pointer to buffer data and return buffer corresponding to
 * that user pointer. Search the rb tree for buffer that matches user data
 * pointer.
 *
 * Return:	Pointer to buffer or NULL
 */
struct binder_buffer *binder_alloc_prepare_to_free(struct binder_alloc *alloc,
						   unsigned long user_ptr)
{
	guard(mutex)(&alloc->mutex);
	return binder_alloc_prepare_to_free_locked(alloc, user_ptr);
}

static inline void
binder_set_installed_page(struct binder_alloc *alloc,
			  unsigned long index,
			  struct page *page)
{
	/* Pairs with acquire in binder_get_installed_page() */
	smp_store_release(&alloc->pages[index], page);
}

/*
 * binder_get_installed_page - 获取指定索引处已安装的物理页面
 * @alloc: binder_alloc 实例
 * @index: pages[] 数组索引
 *
 * 使用 smp_load_acquire 与 binder_set_installed_page 的 smp_store_release
 * 配对，确保读取页面指针时能看到其他 CPU 的完整初始化结果。
 *
 * 返回: page 指针，未安装返回 NULL
 */
static inline struct page *
binder_get_installed_page(struct binder_alloc *alloc, unsigned long index)
{
	/* Pairs with release in binder_set_installed_page() */
	return smp_load_acquire(&alloc->pages[index]);
}

/*
 * binder_lru_freelist_add - 将指定页面范围加入 LRU 空闲链表
 * @alloc: binder_alloc 实例
 * @start: 起始页面地址 (页对齐)
 * @end: 结束页面地址 (页对齐)
 *
 * 遍历 [start, end) 范围内的所有页面，将已安装的 (alloc->pages[index] != NULL)
 * 页面加入 alloc->freelist (LRU 链表)，供内核 shrinker 在内存压力下回收。
 * 调用此函数前，调用者已确保这些页面不再被任何活跃缓冲区使用。
 */
static void binder_lru_freelist_add(struct binder_alloc *alloc,
				    unsigned long start, unsigned long end)
{
	unsigned long page_addr;
	struct page *page;

	trace_binder_update_page_range(alloc, false, start, end);

	for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
		size_t index;
		int ret;

		index = (page_addr - alloc->vm_start) / PAGE_SIZE;
		page = binder_get_installed_page(alloc, index);
		if (!page)
			continue;

		trace_binder_free_lru_start(alloc, index);

		ret = list_lru_add(alloc->freelist,
				   page_to_lru(page),
				   page_to_nid(page),
				   NULL);
		WARN_ON(!ret);

		trace_binder_free_lru_end(alloc, index);
	}
}

static inline
void binder_alloc_set_mapped(struct binder_alloc *alloc, bool state)
{
	/* pairs with smp_load_acquire in binder_alloc_is_mapped() */
	smp_store_release(&alloc->mapped, state);
}

static inline bool binder_alloc_is_mapped(struct binder_alloc *alloc)
{
	/* pairs with smp_store_release in binder_alloc_set_mapped() */
	return smp_load_acquire(&alloc->mapped);
}

/*
 * binder_page_lookup - 在远程进程的地址空间中查找已安装的物理页面
 * @alloc: binder_alloc 实例
 * @addr: 用户空间地址
 *
 * 通过 get_user_pages_remote(FOLL_NOFAULT) 查找指定地址是否已有页面映射。
 * 用于 binder_install_single_page 的 -EBUSY 恢复路径:
 * 当 vm_insert_page 返回 EBUSY (其他线程先安装了 PTE) 时，
 * 查找已被安装的页面并复用。
 *
 * 返回: page 指针，未找到返回 NULL
 */
static struct page *binder_page_lookup(struct binder_alloc *alloc,
				       unsigned long addr)
{
	struct mm_struct *mm = alloc->mm;
	struct page *page;
	long npages = 0;

	/*
	 * Find an existing page in the remote mm. If missing,
	 * don't attempt to fault-in just propagate an error.
	 */
	mmap_read_lock(mm);
	if (binder_alloc_is_mapped(alloc))
		npages = get_user_pages_remote(mm, addr, 1, FOLL_NOFAULT,
					       &page, NULL);
	mmap_read_unlock(mm);

	return npages > 0 ? page : NULL;
}

/*
 * binder_page_insert - 将物理页面插入到进程的用户空间地址映射
 * @alloc: binder_alloc 实例
 * @addr: 目标用户空间地址
 * @page: 要映射的物理页面
 *
 * 通过 vm_insert_page() 将分配的物理页面插入进程的 VMA。
 * 优先使用 per-vma lock (lock_vma_under_rcu) 减少锁竞争，
 * 如果 per-vma lock 不可用，回退到 mmap_read_lock。
 *
 * 返回: 0 成功，-EBUSY 页面已存在，负值错误码
 */
static int binder_page_insert(struct binder_alloc *alloc,
			      unsigned long addr,
			      struct page *page)
{
	struct mm_struct *mm = alloc->mm;
	struct vm_area_struct *vma;
	int ret = -ESRCH;

	/* attempt per-vma lock first */
	vma = lock_vma_under_rcu(mm, addr);
	if (vma) {
		if (binder_alloc_is_mapped(alloc))
			ret = vm_insert_page(vma, addr, page);
		vma_end_read(vma);
		return ret;
	}

	/* fall back to mmap_lock */
	mmap_read_lock(mm);
	vma = vma_lookup(mm, addr);
	if (vma && binder_alloc_is_mapped(alloc))
		ret = vm_insert_page(vma, addr, page);
	mmap_read_unlock(mm);

	return ret;
}

/*
 * binder_page_alloc - 分配一个新的物理页面及其 shrinker 元数据
 * @alloc: binder_alloc 实例
 * @index: 页面在 alloc->pages[] 数组中的索引
 *
 * 分配一个零初始化页面 (GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO)，
 * 同时分配 binder_shrinker_mdata 结构体挂载在 page->private 中，
 * 用于页面回收时追踪所属的 alloc 实例和页面索引。
 *
 * 返回: page 指针，失败返回 NULL
 */
static struct page *binder_page_alloc(struct binder_alloc *alloc,
				      unsigned long index)
{
	struct binder_shrinker_mdata *mdata;
	struct page *page;

	page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);
	if (!page)
		return NULL;

	/* allocate and install shrinker metadata under page->private */
	mdata = kzalloc_obj(*mdata);
	if (!mdata) {
		__free_page(page);
		return NULL;
	}

	mdata->alloc = alloc;
	mdata->page_index = index;
	INIT_LIST_HEAD(&mdata->lru);
	set_page_private(page, (unsigned long)mdata);

	return page;
}

static void binder_free_page(struct page *page)
{
	kfree((struct binder_shrinker_mdata *)page_private(page));
	__free_page(page);
}

/*
 * binder_install_single_page - 分配并安装单个物理页面到进程地址空间
 * @alloc: binder_alloc 实例
 * @index: 页面在 alloc->pages[] 数组中的索引
 * @addr: 目标用户空间地址 (已页对齐)
 *
 * 工作流程:
 *   1. 通过 mmget_not_zero() 增加 mm 引用计数，确保进程地址空间有效
 *   2. 调用 binder_page_alloc() 分配物理页面
 *   3. 通过 vm_insert_page() 将页面映射到用户空间 (优先使用 per-vma lock)
 *   4. 使用 smp_store_release() 更新 alloc->pages[index]，确保页面安装
 *      对其他 CPU 立即可见 (与 binder_get_installed_page 的 acquire 配对)
 *
 * 返回: 0 成功，负值错误码
 */
static int binder_install_single_page(struct binder_alloc *alloc,
				      unsigned long index,
				      unsigned long addr)
{
	struct page *page;
	int ret;

	if (!mmget_not_zero(alloc->mm))
		return -ESRCH;

	page = binder_page_alloc(alloc, index);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	ret = binder_page_insert(alloc, addr, page);
	switch (ret) {
	case -EBUSY:
		/*
		 * EBUSY is ok. Someone installed the pte first but the
		 * alloc->pages[index] has not been updated yet. Discard
		 * our page and look up the one already installed.
		 */
		ret = 0;
		binder_free_page(page);
		page = binder_page_lookup(alloc, addr);
		if (!page) {
			pr_err("%d: failed to find page at offset %lx\n",
			       alloc->pid, addr - alloc->vm_start);
			ret = -ESRCH;
			break;
		}
		fallthrough;
	case 0:
		/* Mark page installation complete and safe to use */
		binder_set_installed_page(alloc, index, page);
		break;
	default:
		binder_free_page(page);
		pr_err("%d: %s failed to insert page at offset %lx with %d\n",
		       alloc->pid, __func__, addr - alloc->vm_start, ret);
		break;
	}
out:
	mmput_async(alloc->mm);
	return ret;
}

/*
 * binder_install_buffer_pages - 为缓冲区分配所有所需的物理页面
 * @alloc: binder_alloc 实例
 * @buffer: 需要分配页面的缓冲区
 * @size: 缓冲区数据大小
 *
 * 计算缓冲区覆盖的页面范围 [start, final):
 *   start = buffer->user_data & PAGE_MASK      (起始页)
 *   final = PAGE_ALIGN(buffer->user_data + size) (结束页)
 * 对每个尚未安装页面的页框调用 binder_install_single_page()。
 * 已安装的页面跳过 (可能与其他缓冲区共享)，避免重复分配。
 *
 * 返回: 0 成功，负值错误码
 */
static int binder_install_buffer_pages(struct binder_alloc *alloc,
				       struct binder_buffer *buffer,
				       size_t size)
{
	unsigned long start, final;
	unsigned long page_addr;

	start = buffer->user_data & PAGE_MASK;
	final = PAGE_ALIGN(buffer->user_data + size);

	for (page_addr = start; page_addr < final; page_addr += PAGE_SIZE) {
		unsigned long index;
		int ret;

		index = (page_addr - alloc->vm_start) / PAGE_SIZE;
		if (binder_get_installed_page(alloc, index))
			continue;

		trace_binder_alloc_page_start(alloc, index);

		ret = binder_install_single_page(alloc, index, page_addr);
		if (ret)
			return ret;

		trace_binder_alloc_page_end(alloc, index);
	}

	return 0;
}

/*
 * binder_lru_freelist_del - 从 LRU 空闲链表中移除指定页面范围
 * @alloc: binder_alloc 实例
 * @start: 起始页面地址 (页对齐)
 * @end: 结束页面地址 (页对齐)
 *
 * 当缓冲区从空闲转为已分配时，其覆盖的页面需要从 freelist 中移除，
 * 防止 shrinker 回收正在使用的页面。范围 [start, end) 应排除与其他
 * 活跃缓冲区共享的页面 (调用者通过 min(next_used_page, curr_last_page)
 * 计算排除)。对于尚未安装页面的页框 (alloc->pages[index] == NULL)，
 * 更新 pages_high 水位线用于调试监控。
 */
/* The range of pages should exclude those shared with other buffers */
static void binder_lru_freelist_del(struct binder_alloc *alloc,
				    unsigned long start, unsigned long end)
{
	unsigned long page_addr;
	struct page *page;

	trace_binder_update_page_range(alloc, true, start, end);

	for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
		unsigned long index;
		bool on_lru;

		index = (page_addr - alloc->vm_start) / PAGE_SIZE;
		page = binder_get_installed_page(alloc, index);

		if (page) {
			trace_binder_alloc_lru_start(alloc, index);

			on_lru = list_lru_del(alloc->freelist,
					      page_to_lru(page),
					      page_to_nid(page),
					      NULL);
			WARN_ON(!on_lru);

			trace_binder_alloc_lru_end(alloc, index);
			continue;
		}

		if (index + 1 > alloc->pages_high)
			alloc->pages_high = index + 1;
	}
}

/*
 * debug_no_space_locked - 调试: 打印当前空闲和已分配缓冲区的统计信息
 * @alloc: binder_alloc 实例
 *
 * 当 best-fit 分配失败时调用，遍历 allocated_buffers 和 free_buffers
 * 红黑树，输出缓冲区总数、总大小和最大块大小，用于诊断 ENOSPC 原因。
 */
static void debug_no_space_locked(struct binder_alloc *alloc)
{
	size_t largest_alloc_size = 0;
	struct binder_buffer *buffer;
	size_t allocated_buffers = 0;
	size_t largest_free_size = 0;
	size_t total_alloc_size = 0;
	size_t total_free_size = 0;
	size_t free_buffers = 0;
	size_t buffer_size;
	struct rb_node *n;

	for (n = rb_first(&alloc->allocated_buffers); n; n = rb_next(n)) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		buffer_size = binder_alloc_buffer_size(alloc, buffer);
		allocated_buffers++;
		total_alloc_size += buffer_size;
		if (buffer_size > largest_alloc_size)
			largest_alloc_size = buffer_size;
	}

	for (n = rb_first(&alloc->free_buffers); n; n = rb_next(n)) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		buffer_size = binder_alloc_buffer_size(alloc, buffer);
		free_buffers++;
		total_free_size += buffer_size;
		if (buffer_size > largest_free_size)
			largest_free_size = buffer_size;
	}

	binder_alloc_debug(BINDER_DEBUG_USER_ERROR,
			   "allocated: %zd (num: %zd largest: %zd), free: %zd (num: %zd largest: %zd)\n",
			   total_alloc_size, allocated_buffers,
			   largest_alloc_size, total_free_size,
			   free_buffers, largest_free_size);
}

/*
 * debug_low_async_space_locked - 检测 oneway 异步事务滥用 (spam)
 * @alloc: binder_alloc 实例
 *
 * 当异步空间低于 10% 总缓冲区大小时触发检测:
 *   1. 遍历 allocated_buffers，统计当前进程 (current->tgid) 的异步事务
 *   2. 如果该进程有 >50 个异步事务，或占用 >25% 总缓冲区大小，标记为可疑
 *   3. oneway_spam_detected 标志只设置一次，用于日志告警
 *
 * 返回: true 表示检测到 spam
 */
static bool debug_low_async_space_locked(struct binder_alloc *alloc)
{
	/*
	 * Find the amount and size of buffers allocated by the current caller;
	 * The idea is that once we cross the threshold, whoever is responsible
	 * for the low async space is likely to try to send another async txn,
	 * and at some point we'll catch them in the act. This is more efficient
	 * than keeping a map per pid.
	 */
	struct binder_buffer *buffer;
	size_t total_alloc_size = 0;
	int pid = current->tgid;
	size_t num_buffers = 0;
	struct rb_node *n;

	/*
	 * Only start detecting spammers once we have less than 20% of async
	 * space left (which is less than 10% of total buffer size).
	 */
	if (alloc->free_async_space >= alloc->buffer_size / 10) {
		alloc->oneway_spam_detected = false;
		return false;
	}

	for (n = rb_first(&alloc->allocated_buffers); n != NULL;
		 n = rb_next(n)) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		if (buffer->pid != pid)
			continue;
		if (!buffer->async_transaction)
			continue;
		total_alloc_size += binder_alloc_buffer_size(alloc, buffer);
		num_buffers++;
	}

	/*
	 * Warn if this pid has more than 50 transactions, or more than 50% of
	 * async space (which is 25% of total buffer size). Oneway spam is only
	 * detected when the threshold is exceeded.
	 */
	if (num_buffers > 50 || total_alloc_size > alloc->buffer_size / 4) {
		binder_alloc_debug(BINDER_DEBUG_USER_ERROR,
			     "%d: pid %d spamming oneway? %zd buffers allocated for a total size of %zd\n",
			      alloc->pid, pid, num_buffers, total_alloc_size);
		if (!alloc->oneway_spam_detected) {
			alloc->oneway_spam_detected = true;
			return true;
		}
	}
	return false;
}

/*
 * binder_alloc_new_buf_locked - best-fit 分配算法的核心实现
 * @alloc: binder_alloc 实例
 * @new_buffer: 调用者预分配的缓冲区结构体 (分裂时用作新空闲块，未使用则释放)
 * @size: 请求分配的字节数 (已对齐到指针大小)
 * @is_async: 是否为异步 (oneway) 事务缓冲区
 *
 * best-fit 分配策略详细流程:
 *   1. 异步事务检查: 如果 is_async 且 free_async_space < size，直接返回 ENOSPC
 *   2. 在 free_buffers 红黑树中遍历寻找最合适的空闲块:
 *      - 当前节点大小 > size: 记录为候选 best_fit，继续向左 (更小) 搜索
 *      - 当前节点大小 == size: 精确匹配，直接选中
 *      - 当前节点大小 < size: 向右 (更大) 搜索
 *      (这是标准的 best-fit 算法 —— 找到 >= size 的最小块)
 *   3. 未找到候选块: 返回 ENOSPC (地址空间不足)
 *   4. 块分裂: 如果选中的块比请求大，将其分裂为两部分:
 *      - 前半部分 (size 字节) 作为本次的已分配块
 *      - 后半部分 (buffer_size - size 字节) 作为新空闲块插入 free_buffers
 *   5. 页面记账: 从 LRU freelist 中移除本缓冲区覆盖页面的引用
 *   6. 红黑树转移: 从 free_buffers 擦除，插入 allocated_buffers
 *   7. 异步空间更新: 减少 free_async_space，检测 oneway spam
 *
 * 调用者必须持有 alloc->mutex。
 * 首次调用时 new_buffer 由 binder_alloc_new_buf() 预分配。
 *
 * 返回: 分配的缓冲区指针，失败返回 ERR_PTR(-ENOSPC)
 */
/* Callers preallocate @new_buffer, it is freed by this function if unused */
static struct binder_buffer *binder_alloc_new_buf_locked(
				struct binder_alloc *alloc,
				struct binder_buffer *new_buffer,
				size_t size,
				int is_async)
{
	struct rb_node *n = alloc->free_buffers.rb_node;
	struct rb_node *best_fit = NULL;
	struct binder_buffer *buffer;
	unsigned long next_used_page;
	unsigned long curr_last_page;
	size_t buffer_size;

	if (is_async && alloc->free_async_space < size) {
		binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC,
			     "%d: binder_alloc_buf size %zd failed, no async space left\n",
			      alloc->pid, size);
		buffer = ERR_PTR(-ENOSPC);
		goto out;
	}

	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		BUG_ON(!buffer->free);
		buffer_size = binder_alloc_buffer_size(alloc, buffer);

		if (size < buffer_size) {
			best_fit = n;
			n = n->rb_left;
		} else if (size > buffer_size) {
			n = n->rb_right;
		} else {
			best_fit = n;
			break;
		}
	}

	if (unlikely(!best_fit)) {
		binder_alloc_debug(BINDER_DEBUG_USER_ERROR,
				   "%d: binder_alloc_buf size %zd failed, no address space\n",
				   alloc->pid, size);
		debug_no_space_locked(alloc);
		buffer = ERR_PTR(-ENOSPC);
		goto out;
	}

	if (buffer_size != size) {
		/* Found an oversized buffer and needs to be split */
		buffer = rb_entry(best_fit, struct binder_buffer, rb_node);
		buffer_size = binder_alloc_buffer_size(alloc, buffer);

		WARN_ON(n || buffer_size == size);
		new_buffer->user_data = buffer->user_data + size;
		list_add(&new_buffer->entry, &buffer->entry);
		new_buffer->free = 1;
		binder_insert_free_buffer(alloc, new_buffer);
		new_buffer = NULL;
	}

	binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_alloc_buf size %zd got buffer %p size %zd\n",
		      alloc->pid, size, buffer, buffer_size);

	/*
	 * Now we remove the pages from the freelist. A clever calculation
	 * with buffer_size determines if the last page is shared with an
	 * adjacent in-use buffer. In such case, the page has been already
	 * removed from the freelist so we trim our range short.
	 */
	next_used_page = (buffer->user_data + buffer_size) & PAGE_MASK;
	curr_last_page = PAGE_ALIGN(buffer->user_data + size);
	binder_lru_freelist_del(alloc, PAGE_ALIGN(buffer->user_data),
				min(next_used_page, curr_last_page));

	rb_erase(&buffer->rb_node, &alloc->free_buffers);
	buffer->free = 0;
	buffer->allow_user_free = 0;
	binder_insert_allocated_buffer_locked(alloc, buffer);
	buffer->async_transaction = is_async;
	buffer->oneway_spam_suspect = false;
	if (is_async) {
		alloc->free_async_space -= size;
		binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC_ASYNC,
			     "%d: binder_alloc_buf size %zd async free %zd\n",
			      alloc->pid, size, alloc->free_async_space);
		if (debug_low_async_space_locked(alloc))
			buffer->oneway_spam_suspect = true;
	}

out:
	/* Discard possibly unused new_buffer */
	kfree(new_buffer);
	return buffer;
}

/*
 * sanitized_size - 计算安全的总缓冲区大小 (对齐 + 溢出检查)
 * @data_size: 数据部分大小
 * @offsets_size: 偏移数组大小
 * @extra_buffers_size: 额外缓冲区大小
 *
 * 将三个部分分别对齐到指针大小后求和，同时检测整数溢出。
 * 空缓冲区也会被填充到 sizeof(void *) 以确保每个缓冲区有唯一的地址。
 *
 * 返回: 对齐后的总大小，溢出返回 0
 */
/* Calculate the sanitized total size, returns 0 for invalid request */
static inline size_t sanitized_size(size_t data_size,
				    size_t offsets_size,
				    size_t extra_buffers_size)
{
	size_t total, tmp;

	/* Align to pointer size and check for overflows */
	tmp = ALIGN(data_size, sizeof(void *)) +
		ALIGN(offsets_size, sizeof(void *));
	if (tmp < data_size || tmp < offsets_size)
		return 0;
	total = tmp + ALIGN(extra_buffers_size, sizeof(void *));
	if (total < tmp || total < extra_buffers_size)
		return 0;

	/* Pad 0-sized buffers so they get a unique address */
	total = max(total, sizeof(void *));

	return total;
}

/*
 * binder_alloc_new_buf - Binder 缓冲区分配接口 (对外 API)
 * @alloc: binder_alloc 实例
 * @data_size: 用户数据大小
 * @offsets_size: 偏移数组大小
 * @extra_buffers_size: 额外元数据大小 (如安全上下文)
 * @is_async: 是否为异步 (oneway) 事务
 *
 * 对外提供的缓冲区分配接口。内部流程:
 *   1. 检查 binder_alloc 是否已通过 mmap 初始化
 *   2. 调用 sanitized_size() 计算对齐后的总大小 (含溢出检查)
 *   3. 预分配下一个缓冲区结构体 (用于 best-fit 分裂时作为新空闲块)
 *   4. 持有 alloc->mutex，调用 binder_alloc_new_buf_locked() 执行分配
 *   5. 设置缓冲区元数据 (data_size, offsets_size, extra_buffers_size)
 *   6. 调用 binder_install_buffer_pages() 为缓冲区按需分配物理页面
 *   7. 页面分配失败时调用 binder_alloc_free_buf() 回滚
 *
 * 返回: 分配的缓冲区指针，失败返回 ERR_PTR(-errno)
 */

/**
 * binder_alloc_new_buf() - Allocate a new binder buffer
 * @alloc:              binder_alloc for this proc
 * @data_size:          size of user data buffer
 * @offsets_size:       user specified buffer offset
 * @extra_buffers_size: size of extra space for meta-data (eg, security context)
 * @is_async:           buffer for async transaction
 *
 * Allocate a new buffer given the requested sizes. Returns
 * the kernel version of the buffer pointer. The size allocated
 * is the sum of the three given sizes (each rounded up to
 * pointer-sized boundary)
 *
 * Return:	The allocated buffer or %ERR_PTR(-errno) if error
 */
struct binder_buffer *binder_alloc_new_buf(struct binder_alloc *alloc,
					   size_t data_size,
					   size_t offsets_size,
					   size_t extra_buffers_size,
					   int is_async)
{
	struct binder_buffer *buffer, *next;
	size_t size;
	int ret;

	/* Check binder_alloc is fully initialized */
	if (!binder_alloc_is_mapped(alloc)) {
		binder_alloc_debug(BINDER_DEBUG_USER_ERROR,
				   "%d: binder_alloc_buf, no vma\n",
				   alloc->pid);
		return ERR_PTR(-ESRCH);
	}

	size = sanitized_size(data_size, offsets_size, extra_buffers_size);
	if (unlikely(!size)) {
		binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC,
				   "%d: got transaction with invalid size %zd-%zd-%zd\n",
				   alloc->pid, data_size, offsets_size,
				   extra_buffers_size);
		return ERR_PTR(-EINVAL);
	}

	/* Preallocate the next buffer */
	next = kzalloc_obj(*next);
	if (!next)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&alloc->mutex);
	buffer = binder_alloc_new_buf_locked(alloc, next, size, is_async);
	if (IS_ERR(buffer)) {
		mutex_unlock(&alloc->mutex);
		goto out;
	}

	buffer->data_size = data_size;
	buffer->offsets_size = offsets_size;
	buffer->extra_buffers_size = extra_buffers_size;
	buffer->pid = current->tgid;
	mutex_unlock(&alloc->mutex);

	ret = binder_install_buffer_pages(alloc, buffer, size);
	if (ret) {
		binder_alloc_free_buf(alloc, buffer);
		buffer = ERR_PTR(ret);
	}
out:
	return buffer;
}
EXPORT_SYMBOL_IF_KUNIT(binder_alloc_new_buf);

/*
 * buffer_start_page - 获取缓冲区起始地址的页基址
 * @buffer: 缓冲区
 *
 * 通过 PAGE_MASK 屏蔽页内偏移得到页面基地址。
 * 用于判断两个缓冲区是否共享同一个物理页面。
 */
static unsigned long buffer_start_page(struct binder_buffer *buffer)
{
	return buffer->user_data & PAGE_MASK;
}

/*
 * prev_buffer_end_page - 获取前一个缓冲区最后字节所在的页基址
 * @buffer: 当前缓冲区
 *
 * 通过 (user_data - 1) & PAGE_MASK 计算前一个缓冲区最后地址的页基址。
 * 如果此值与 buffer_start_page(buffer) 相同，说明当前缓冲区和前一个
 * 缓冲区共享同一个物理页面，此时释放该页面会破坏前一个缓冲区的数据。
 * 用于 binder_delete_free_buffer 中的页面回收决策。
 */
static unsigned long prev_buffer_end_page(struct binder_buffer *buffer)
{
	return (buffer->user_data - 1) & PAGE_MASK;
}

/*
 * binder_delete_free_buffer - 释放空闲缓冲区节点 (合并辅助函数)
 * @alloc: binder_alloc 实例
 * @buffer: 要删除的空闲缓冲区
 *
 * 在合并空闲块时使用，负责从链表中删除缓冲区节点并释放其内存。
 * 关键逻辑: 判断是否需要将缓冲区所占的最后一页加入 LRU freelist。
 * 如果缓冲区起始地址不是页对齐，说明它的第一页可能与前一缓冲区共享；
 * 如果缓冲区最后一页与后一缓冲区共享，则不应回收该页。
 * 这些情况通过 buffer_start_page() 和 prev_buffer_end_page() 的比较判断。
 */
static void binder_delete_free_buffer(struct binder_alloc *alloc,
				      struct binder_buffer *buffer)
{
	struct binder_buffer *prev, *next;

	if (PAGE_ALIGNED(buffer->user_data))
		goto skip_freelist;

	BUG_ON(alloc->buffers.next == &buffer->entry);
	prev = binder_buffer_prev(buffer);
	BUG_ON(!prev->free);
	if (prev_buffer_end_page(prev) == buffer_start_page(buffer))
		goto skip_freelist;

	if (!list_is_last(&buffer->entry, &alloc->buffers)) {
		next = binder_buffer_next(buffer);
		if (buffer_start_page(next) == buffer_start_page(buffer))
			goto skip_freelist;
	}

	binder_lru_freelist_add(alloc, buffer_start_page(buffer),
				buffer_start_page(buffer) + PAGE_SIZE);
skip_freelist:
	list_del(&buffer->entry);
	kfree(buffer);
}

/*
 * binder_free_buf_locked - 释放缓冲区 (内核加锁版本)
 * @alloc: binder_alloc 实例
 * @buffer: 要释放的缓冲区
 *
 * 释放流程与相邻空闲块合并策略:
 *   1. 恢复异步事务空间 (如果 buffer->async_transaction 为真)
 *   2. 将缓冲区覆盖的页面加入 LRU 空闲链表 (binder_lru_freelist_add)
 *   3. 从 allocated_buffers 红黑树中移除
 *   4. 标记 buffer->free = 1
 *   5. 向前合并 (检查下一个相邻缓冲区 next):
 *      - 如果 next 存在且 free == 1，从 free_buffers 中删除 next
 *      - 调用 binder_delete_free_buffer() 释放 next 节点
 *        (该函数会处理跨页面边界的特殊情况)
 *   6. 向后合并 (检查上一个相邻缓冲区 prev):
 *      - 如果 prev 存在且 free == 1，删除当前 buffer 节点
 *      - 将当前指针指向 prev (合并后的更大空闲块)
 *   7. 将 (可能合并后的) 缓冲区插入 free_buffers 红黑树
 *
 * 通过双向合并策略，有效减少内存碎片化。
 * binder_delete_free_buffer() 处理页面级特殊情况:
 *   - 如果缓冲区跨页面边界且与相邻缓冲区共享页面，则不回收该页面
 *   - 通过 buffer_start_page 和 prev_buffer_end_page 的比较判断
 *
 * 调用者必须持有 alloc->mutex。
 */
static void binder_free_buf_locked(struct binder_alloc *alloc,
				   struct binder_buffer *buffer)
{
	size_t size, buffer_size;

	buffer_size = binder_alloc_buffer_size(alloc, buffer);

	size = ALIGN(buffer->data_size, sizeof(void *)) +
		ALIGN(buffer->offsets_size, sizeof(void *)) +
		ALIGN(buffer->extra_buffers_size, sizeof(void *));

	binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_free_buf %p size %zd buffer_size %zd\n",
		      alloc->pid, buffer, size, buffer_size);

	BUG_ON(buffer->free);
	BUG_ON(size > buffer_size);
	BUG_ON(buffer->transaction != NULL);
	BUG_ON(buffer->user_data < alloc->vm_start);
	BUG_ON(buffer->user_data > alloc->vm_start + alloc->buffer_size);

	if (buffer->async_transaction) {
		alloc->free_async_space += buffer_size;
		binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC_ASYNC,
			     "%d: binder_free_buf size %zd async free %zd\n",
			      alloc->pid, size, alloc->free_async_space);
	}

	binder_lru_freelist_add(alloc, PAGE_ALIGN(buffer->user_data),
				(buffer->user_data + buffer_size) & PAGE_MASK);

	rb_erase(&buffer->rb_node, &alloc->allocated_buffers);
	buffer->free = 1;
	if (!list_is_last(&buffer->entry, &alloc->buffers)) {
		struct binder_buffer *next = binder_buffer_next(buffer);

		if (next->free) {
			rb_erase(&next->rb_node, &alloc->free_buffers);
			binder_delete_free_buffer(alloc, next);
		}
	}
	if (alloc->buffers.next != &buffer->entry) {
		struct binder_buffer *prev = binder_buffer_prev(buffer);

		if (prev->free) {
			binder_delete_free_buffer(alloc, buffer);
			rb_erase(&prev->rb_node, &alloc->free_buffers);
			buffer = prev;
		}
	}
	binder_insert_free_buffer(alloc, buffer);
}

/**
 * binder_alloc_get_page() - get kernel pointer for given buffer offset
 * @alloc: binder_alloc for this proc
 * @buffer: binder buffer to be accessed
 * @buffer_offset: offset into @buffer data
 * @pgoffp: address to copy final page offset to
 *
 * Lookup the struct page corresponding to the address
 * at @buffer_offset into @buffer->user_data. If @pgoffp is not
 * NULL, the byte-offset into the page is written there.
 *
 * The caller is responsible to ensure that the offset points
 * to a valid address within the @buffer and that @buffer is
 * not freeable by the user. Since it can't be freed, we are
 * guaranteed that the corresponding elements of @alloc->pages[]
 * cannot change.
 *
 * Return: struct page
 */
/*
 * binder_alloc_get_page - 根据缓冲区偏移量获取对应的物理页面和页内偏移
 * @alloc: binder_alloc 实例
 * @buffer: 目标缓冲区
 * @buffer_offset: 在缓冲区内的字节偏移
 * @pgoffp: 输出参数，返回页内偏移
 *
 * 计算 buffer_offset 对应的全局地址空间偏移，
 * 然后通过 pages[] 数组查找该页框对应的 struct page。
 * 用于数据拷贝时的逐页遍历。
 *
 * 返回: struct page 指针
 */
static struct page *binder_alloc_get_page(struct binder_alloc *alloc,
					  struct binder_buffer *buffer,
					  binder_size_t buffer_offset,
					  pgoff_t *pgoffp)
{
	binder_size_t buffer_space_offset = buffer_offset +
		(buffer->user_data - alloc->vm_start);
	pgoff_t pgoff = buffer_space_offset & ~PAGE_MASK;
	size_t index = buffer_space_offset >> PAGE_SHIFT;

	*pgoffp = pgoff;

	return alloc->pages[index];
}

/*
 * binder_alloc_clear_buf - 将缓冲区数据清零 (安全擦除)
 * @alloc: binder_alloc 实例
 * @buffer: 要清零的缓冲区
 *
 * 使用 memset_page 逐页将缓冲区内容清零。
 * 用于 clear_on_free 功能：释放前清除敏感数据防止信息泄露。
 * 注意: 此函数在持有 alloc->mutex 锁之外调用，以避免大缓冲区
 * 清零时长时间持有锁导致其他线程阻塞。
 *
 * 返回: 无
 */

/**
 * binder_alloc_clear_buf() - zero out buffer
 * @alloc: binder_alloc for this proc
 * @buffer: binder buffer to be cleared
 *
 * memset the given buffer to 0
 */
static void binder_alloc_clear_buf(struct binder_alloc *alloc,
				   struct binder_buffer *buffer)
{
	size_t bytes = binder_alloc_buffer_size(alloc, buffer);
	binder_size_t buffer_offset = 0;

	while (bytes) {
		unsigned long size;
		struct page *page;
		pgoff_t pgoff;

		page = binder_alloc_get_page(alloc, buffer,
					     buffer_offset, &pgoff);
		size = min_t(size_t, bytes, PAGE_SIZE - pgoff);
		memset_page(page, pgoff, 0, size);
		bytes -= size;
		buffer_offset += size;
	}
}

/*
 * binder_alloc_free_buf - Binder 缓冲区释放接口 (对外 API)
 * @alloc: binder_alloc 实例
 * @buffer: 要释放的缓冲区内核指针
 *
 * 对外提供的缓冲区释放接口，由 Binder 驱动在事务完成时调用。
 * 内部流程:
 *   1. 如果设置了 clear_on_free (安全清除标记)，先清零缓冲区数据
 *      (在持有 mutex 锁之前执行清零，避免大缓冲区长时间持锁)
 *   2. 持有 alloc->mutex，调用 binder_free_buf_locked() 执行实际释放和合并
 *
 * 返回: 无
 */

/**
 * binder_alloc_free_buf() - free a binder buffer
 * @alloc:	binder_alloc for this proc
 * @buffer:	kernel pointer to buffer
 *
 * Free the buffer allocated via binder_alloc_new_buf()
 */
void binder_alloc_free_buf(struct binder_alloc *alloc,
			    struct binder_buffer *buffer)
{
	/*
	 * We could eliminate the call to binder_alloc_clear_buf()
	 * from binder_alloc_deferred_release() by moving this to
	 * binder_free_buf_locked(). However, that could
	 * increase contention for the alloc mutex if clear_on_free
	 * is used frequently for large buffers. The mutex is not
	 * needed for correctness here.
	 */
	if (buffer->clear_on_free) {
		binder_alloc_clear_buf(alloc, buffer);
		buffer->clear_on_free = false;
	}
	mutex_lock(&alloc->mutex);
	binder_free_buf_locked(alloc, buffer);
	mutex_unlock(&alloc->mutex);
}
EXPORT_SYMBOL_IF_KUNIT(binder_alloc_free_buf);

/*
 * binder_alloc_mmap_handler - mmap 初始化 Binder 共享内存
 * @alloc: binder_alloc 实例
 * @vma: mmap 系统调用传入的 VMA
 *
 * 当用户进程对 /dev/binder 执行 mmap() 时调用，完成共享内存的初始化:
 *   1. 校验 vma->vm_mm 与 alloc->mm 一致
 *   2. 检查是否已映射 (只能 mmap 一次)，防止重复映射
 *   3. 确定缓冲区大小: 取 vma 大小和 SZ_4M 的较小值 (上限 4MB)
 *   4. 分配 pages[] 数组: kvzalloc_objs，按页数分配
 *   5. 创建初始空闲缓冲区 (binder_buffer)，覆盖整个 mmap 区域
 *      - user_data = vm_start (从 VMA 起始地址开始)
 *      - 插入 buffers 链表和 free_buffers 红黑树
 *   6. 初始化异步空间: free_async_space = buffer_size / 2
 *   7. 设置 mapped = true，表示初始化完成，可以开始分配缓冲区
 *
 * 错误处理:
 *   - -EBUSY: 重复映射
 *   - -ENOMEM: 内存分配失败
 *   - -EINVAL: vma 所属 mm 不匹配
 *
 * 返回: 0 成功，负值错误码
 */

/**
 * binder_alloc_mmap_handler() - map virtual address space for proc
 * @alloc:	alloc structure for this proc
 * @vma:	vma passed to mmap()
 *
 * Called by binder_mmap() to initialize the space specified in
 * vma for allocating binder buffers
 *
 * Return:
 *      0 = success
 *      -EBUSY = address space already mapped
 *      -ENOMEM = failed to map memory to given address space
 */
int binder_alloc_mmap_handler(struct binder_alloc *alloc,
			      struct vm_area_struct *vma)
{
	struct binder_buffer *buffer;
	const char *failure_string;
	int ret;

	if (unlikely(vma->vm_mm != alloc->mm)) {
		ret = -EINVAL;
		failure_string = "invalid vma->vm_mm";
		goto err_invalid_mm;
	}

	mutex_lock(&binder_alloc_mmap_lock);
	if (alloc->buffer_size) {
		ret = -EBUSY;
		failure_string = "already mapped";
		goto err_already_mapped;
	}
	alloc->buffer_size = min_t(unsigned long, vma->vm_end - vma->vm_start,
				   SZ_4M);
	mutex_unlock(&binder_alloc_mmap_lock);

	alloc->vm_start = vma->vm_start;

	alloc->pages = kvzalloc_objs(alloc->pages[0],
				     alloc->buffer_size / PAGE_SIZE);
	if (!alloc->pages) {
		ret = -ENOMEM;
		failure_string = "alloc page array";
		goto err_alloc_pages_failed;
	}

	buffer = kzalloc_obj(*buffer);
	if (!buffer) {
		ret = -ENOMEM;
		failure_string = "alloc buffer struct";
		goto err_alloc_buf_struct_failed;
	}

	buffer->user_data = alloc->vm_start;
	list_add(&buffer->entry, &alloc->buffers);
	buffer->free = 1;
	binder_insert_free_buffer(alloc, buffer);
	alloc->free_async_space = alloc->buffer_size / 2;

	/* Signal binder_alloc is fully initialized */
	binder_alloc_set_mapped(alloc, true);

	return 0;

err_alloc_buf_struct_failed:
	kvfree(alloc->pages);
	alloc->pages = NULL;
err_alloc_pages_failed:
	alloc->vm_start = 0;
	mutex_lock(&binder_alloc_mmap_lock);
	alloc->buffer_size = 0;
err_already_mapped:
	mutex_unlock(&binder_alloc_mmap_lock);
err_invalid_mm:
	binder_alloc_debug(BINDER_DEBUG_USER_ERROR,
			   "%s: %d %lx-%lx %s failed %d\n", __func__,
			   alloc->pid, vma->vm_start, vma->vm_end,
			   failure_string, ret);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(binder_alloc_mmap_handler);

/*
 * binder_alloc_deferred_release - 延迟释放 binder_alloc 所有资源
 * @alloc: 要释放的 binder_alloc 实例
 *
 * 在 binder 文件描述符关闭且 mapped 已被清除后调用。
 * 清理流程:
 *   1. 遍历 allocated_buffers: 对每个已分配缓冲区调用 binder_free_buf_locked()
 *      (事务应在之前已被清理，否则 BUG_ON 触发)
 *   2. 遍历 buffers 链表: 释放所有剩余的 (空闲) 缓冲区节点
 *   3. 遍历 alloc->pages[]: 将所有已安装的页面从 LRU 链表移除并释放
 *   4. 释放 pages[] 数组内存
 *   5. 减少 mm 引用计数
 */
void binder_alloc_deferred_release(struct binder_alloc *alloc)
{
	struct rb_node *n;
	int buffers, page_count;
	struct binder_buffer *buffer;

	buffers = 0;
	mutex_lock(&alloc->mutex);
	BUG_ON(alloc->mapped);

	while ((n = rb_first(&alloc->allocated_buffers))) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);

		/* Transaction should already have been freed */
		BUG_ON(buffer->transaction);

		if (buffer->clear_on_free) {
			binder_alloc_clear_buf(alloc, buffer);
			buffer->clear_on_free = false;
		}
		binder_free_buf_locked(alloc, buffer);
		buffers++;
	}

	while (!list_empty(&alloc->buffers)) {
		buffer = list_first_entry(&alloc->buffers,
					  struct binder_buffer, entry);
		WARN_ON(!buffer->free);

		list_del(&buffer->entry);
		WARN_ON_ONCE(!list_empty(&alloc->buffers));
		kfree(buffer);
	}

	page_count = 0;
	if (alloc->pages) {
		int i;

		for (i = 0; i < alloc->buffer_size / PAGE_SIZE; i++) {
			struct page *page;
			bool on_lru;

			page = binder_get_installed_page(alloc, i);
			if (!page)
				continue;

			on_lru = list_lru_del(alloc->freelist,
					      page_to_lru(page),
					      page_to_nid(page),
					      NULL);
			binder_alloc_debug(BINDER_DEBUG_BUFFER_ALLOC,
				     "%s: %d: page %d %s\n",
				     __func__, alloc->pid, i,
				     on_lru ? "on lru" : "active");
			binder_free_page(page);
			page_count++;
		}
	}
	mutex_unlock(&alloc->mutex);
	kvfree(alloc->pages);
	if (alloc->mm)
		mmdrop(alloc->mm);

	binder_alloc_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "%s: %d buffers %d, pages %d\n",
		     __func__, alloc->pid, buffers, page_count);
}
EXPORT_SYMBOL_IF_KUNIT(binder_alloc_deferred_release);

/*
 * binder_alloc_print_allocated - 打印已分配缓冲区信息 (procfs)
 * @m: seq_file 输出对象
 * @alloc: binder_alloc 实例
 *
 * 遍历 allocated_buffers 红黑树，输出每个缓冲区的 debug_id、
 * 偏移、数据大小和事务状态。用于 /proc/binder/proc/* 调试接口。
 */

/**
 * binder_alloc_print_allocated() - print buffer info
 * @m:     seq_file for output via seq_printf()
 * @alloc: binder_alloc for this proc
 *
 * Prints information about every buffer associated with
 * the binder_alloc state to the given seq_file
 */
void binder_alloc_print_allocated(struct seq_file *m,
				  struct binder_alloc *alloc)
{
	struct binder_buffer *buffer;
	struct rb_node *n;

	guard(mutex)(&alloc->mutex);
	for (n = rb_first(&alloc->allocated_buffers); n; n = rb_next(n)) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		seq_printf(m, "  buffer %d: %lx size %zd:%zd:%zd %s\n",
			   buffer->debug_id,
			   buffer->user_data - alloc->vm_start,
			   buffer->data_size, buffer->offsets_size,
			   buffer->extra_buffers_size,
			   buffer->transaction ? "active" : "delivered");
	}
}

/*
 * binder_alloc_print_pages - 打印页面使用情况统计 (procfs)
 * @m: seq_file 输出对象
 * @alloc: binder_alloc 实例
 *
 * 遍历 alloc->pages[] 数组，统计:
 *   - active: 已被缓冲区使用且不在 LRU 中的页面
 *   - lru: 在 LRU 空闲链表中的页面 (可被回收)
 *   - free: 未安装的页面 (从未分配)
 * 同时显示历史最高页面使用量 (pages_high) 用于监控。
 */

/**
 * binder_alloc_print_pages() - print page usage
 * @m:     seq_file for output via seq_printf()
 * @alloc: binder_alloc for this proc
 */
void binder_alloc_print_pages(struct seq_file *m,
			      struct binder_alloc *alloc)
{
	struct page *page;
	int i;
	int active = 0;
	int lru = 0;
	int free = 0;

	mutex_lock(&alloc->mutex);
	/*
	 * Make sure the binder_alloc is fully initialized, otherwise we might
	 * read inconsistent state.
	 */
	if (binder_alloc_is_mapped(alloc)) {
		for (i = 0; i < alloc->buffer_size / PAGE_SIZE; i++) {
			page = binder_get_installed_page(alloc, i);
			if (!page)
				free++;
			else if (list_empty(page_to_lru(page)))
				active++;
			else
				lru++;
		}
	}
	mutex_unlock(&alloc->mutex);
	seq_printf(m, "  pages: %d:%d:%d\n", active, lru, free);
	seq_printf(m, "  pages high watermark: %zu\n", alloc->pages_high);
}

/*
 * binder_alloc_get_allocated_count - 获取已分配的缓冲区数量
 * @alloc: binder_alloc 实例
 *
 * 遍历 allocated_buffers 红黑树统计已分配的缓冲区个数。
 * 用于 procfs 调试接口输出。
 *
 * 返回: 已分配的缓冲区数量
 */

/**
 * binder_alloc_get_allocated_count() - return count of buffers
 * @alloc: binder_alloc for this proc
 *
 * Return: count of allocated buffers
 */
int binder_alloc_get_allocated_count(struct binder_alloc *alloc)
{
	struct rb_node *n;
	int count = 0;

	guard(mutex)(&alloc->mutex);
	for (n = rb_first(&alloc->allocated_buffers); n != NULL; n = rb_next(n))
		count++;
	return count;
}


/*
 * binder_alloc_vma_close - 关闭 VMA 时的回调处理
 * @alloc: binder_alloc 实例
 *
 * 当进程对 /dev/binder 的 mmap 区域被释放时 (如进程退出)，
 * 清除 mapped 标志阻止新的事务分配缓冲区。
 * 实际的资源释放由 binder_alloc_deferred_release() 完成。
 */

/**
 * binder_alloc_vma_close() - invalidate address space
 * @alloc: binder_alloc for this proc
 *
 * Called from binder_vma_close() when releasing address space.
 * Clears alloc->mapped to prevent new incoming transactions from
 * allocating more buffers.
 */
void binder_alloc_vma_close(struct binder_alloc *alloc)
{
	binder_alloc_set_mapped(alloc, false);
}
EXPORT_SYMBOL_IF_KUNIT(binder_alloc_vma_close);

/**
 * binder_alloc_free_page() - shrinker callback to free pages
 * @item:   item to free
 * @lru:    list_lru instance of the item
 * @cb_arg: callback argument
 *
 * Called from list_lru_walk() in binder_shrink_scan() to free
 * up pages when the system is under memory pressure.
 */
enum lru_status binder_alloc_free_page(struct list_head *item,
				       struct list_lru_one *lru,
				       void *cb_arg)
	__must_hold(&lru->lock)
{
	struct binder_shrinker_mdata *mdata = container_of(item, typeof(*mdata), lru);
	struct binder_alloc *alloc = mdata->alloc;
	struct mm_struct *mm = alloc->mm;
	struct vm_area_struct *vma;
	struct page *page_to_free;
	unsigned long page_addr;
	int mm_locked = 0;
	size_t index;

	if (!mmget_not_zero(mm))
		goto err_mmget;

	index = mdata->page_index;
	page_addr = alloc->vm_start + index * PAGE_SIZE;

	/* attempt per-vma lock first */
	vma = lock_vma_under_rcu(mm, page_addr);
	if (!vma) {
		/* fall back to mmap_lock */
		if (!mmap_read_trylock(mm))
			goto err_mmap_read_lock_failed;
		mm_locked = 1;
		vma = vma_lookup(mm, page_addr);
	}

	if (!mutex_trylock(&alloc->mutex))
		goto err_get_alloc_mutex_failed;

	/*
	 * Since a binder_alloc can only be mapped once, we ensure
	 * the vma corresponds to this mapping by checking whether
	 * the binder_alloc is still mapped.
	 */
	if (vma && !binder_alloc_is_mapped(alloc))
		goto err_invalid_vma;

	trace_binder_unmap_kernel_start(alloc, index);

	page_to_free = alloc->pages[index];
	binder_set_installed_page(alloc, index, NULL);

	trace_binder_unmap_kernel_end(alloc, index);

	list_lru_isolate(lru, item);
	spin_unlock(&lru->lock);

	if (vma) {
		trace_binder_unmap_user_start(alloc, index);

		zap_vma_range(vma, page_addr, PAGE_SIZE);

		trace_binder_unmap_user_end(alloc, index);
	}

	mutex_unlock(&alloc->mutex);
	if (mm_locked)
		mmap_read_unlock(mm);
	else
		vma_end_read(vma);
	mmput_async(mm);
	binder_free_page(page_to_free);

	return LRU_REMOVED_RETRY;

err_invalid_vma:
	mutex_unlock(&alloc->mutex);
err_get_alloc_mutex_failed:
	if (mm_locked)
		mmap_read_unlock(mm);
	else
		vma_end_read(vma);
err_mmap_read_lock_failed:
	mmput_async(mm);
err_mmget:
	return LRU_SKIP;
}
EXPORT_SYMBOL_IF_KUNIT(binder_alloc_free_page);

/*
 * binder_shrink_count - shrinker 回调: 返回可回收的空闲页面数量
 * @shrink: shrinker 实例
 * @sc: 回收控制参数
 *
 * 通过 list_lru_count 查询绑定空闲链表中的页面数量。
 * 内核内存管理系统在内存压力下调用此接口决定是否回收。
 */
static unsigned long
binder_shrink_count(struct shrinker *shrink, struct shrink_control *sc)
{
	return list_lru_count(&binder_freelist);
}

/*
 * binder_shrink_scan - shrinker 回调: 扫描并回收空闲页面
 * @shrink: shrinker 实例
 * @sc: 回收控制参数 (nr_to_scan 指定本次最多回收的页面数)
 *
 * 通过 list_lru_walk 遍历绑定空闲链表，对每个页面调用
 * binder_alloc_free_page() 执行实际的回收操作。
 * 回收的页面会从进程的页表中解除映射并释放物理内存。
 */
static unsigned long
binder_shrink_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	return list_lru_walk(&binder_freelist, binder_alloc_free_page,
			    NULL, sc->nr_to_scan);
}

static struct shrinker *binder_shrinker;

/*
 * __binder_alloc_init - 初始化 binder_alloc 实例 (内部实现)
 * @alloc: 要初始化的 binder_alloc 实例
 * @freelist: 该实例使用的 freelist (通常为全局 binder_freelist)
 *
 * 在 binder_open() 时调用，初始化:
 *   - pid: 当前进程 PID
 *   - mm: 当前进程的 mm_struct (增加引用计数)
 *   - mutex: 初始化互斥锁
 *   - buffers: 初始化缓冲区链表
 *   - freelist: 设置 LRU 空闲链表 (用于页面回收)
 */
VISIBLE_IF_KUNIT void __binder_alloc_init(struct binder_alloc *alloc,
					  struct list_lru *freelist)
{
	alloc->pid = current->tgid;
	alloc->mm = current->mm;
	mmgrab(alloc->mm);
	mutex_init(&alloc->mutex);
	INIT_LIST_HEAD(&alloc->buffers);
	alloc->freelist = freelist;
}
EXPORT_SYMBOL_IF_KUNIT(__binder_alloc_init);

/*
 * binder_alloc_init - binder_alloc 初始化 (对外接口)
 * @alloc: 要初始化的 binder_alloc 实例
 *
 * 在 binder_open() 中调用，使用全局 binder_freelist 初始化。
 * 每个打开 /dev/binder 的进程都有一个 binder_alloc 实例。
 * freelist 是所有 binder_alloc 实例共享的全局 LRU 链表，
 * 用于内核内存紧张时统一回收空闲页面。
 */

/**
 * binder_alloc_init() - called by binder_open() for per-proc initialization
 * @alloc: binder_alloc for this proc
 *
 * Called from binder_open() to initialize binder_alloc fields for
 * new binder proc
 */
void binder_alloc_init(struct binder_alloc *alloc)
{
	__binder_alloc_init(alloc, &binder_freelist);
}

/*
 * binder_alloc_shrinker_init - 初始化 Binder 页面回收机制
 *
 * 创建 binder_freelist (list_lru) 用于管理空闲页面，
 * 注册 android-binder shrinker 到内核内存管理系统。
 * 当系统内存不足时，shrinker 会回调 binder_shrink_scan()
 * 释放 Binder 占用的空闲页面。
 *
 * 返回: 0 成功，-ENOMEM 内存不足
 */
int binder_alloc_shrinker_init(void)
{
	int ret;

	ret = list_lru_init(&binder_freelist);
	if (ret)
		return ret;

	binder_shrinker = shrinker_alloc(0, "android-binder");
	if (!binder_shrinker) {
		list_lru_destroy(&binder_freelist);
		return -ENOMEM;
	}

	binder_shrinker->count_objects = binder_shrink_count;
	binder_shrinker->scan_objects = binder_shrink_scan;

	shrinker_register(binder_shrinker);

	return 0;
}

/*
 * binder_alloc_shrinker_exit - 销毁 Binder 页面回收机制
 *
 * 释放 shrinker 和 list_lru 资源。
 * 在 Binder 驱动模块卸载时调用。
 */
void binder_alloc_shrinker_exit(void)
{
	shrinker_free(binder_shrinker);
	list_lru_destroy(&binder_freelist);
}

/*
 * check_buffer - 缓冲区访问安全检查
 * @alloc: binder_alloc 实例
 * @buffer: 要访问的缓冲区
 * @offset: 缓冲区内的字节偏移
 * @bytes: 要访问的字节数
 *
 * 在数据拷贝前进行安全检查:
 *   - offset + bytes 不超过缓冲区大小
 *   - offset 必须 32 位对齐
 *   - 缓冲区必须处于活跃状态且不可被用户释放
 *     (buffer->free == 0 && !(allow_user_free && transaction))
 *   内核能在两种情况下访问缓冲区:
 *   1) 创建时: free == 0 && allow_user_free == 0
 *   2) 销毁时: free == 0 && transaction == NULL
 *
 * 返回: true 表示安全可访问
 */

/**
 * check_buffer() - verify that buffer/offset is safe to access
 * @alloc: binder_alloc for this proc
 * @buffer: binder buffer to be accessed
 * @offset: offset into @buffer data
 * @bytes: bytes to access from offset
 *
 * Check that the @offset/@bytes are within the size of the given
 * @buffer and that the buffer is currently active and not freeable.
 * Offsets must also be multiples of sizeof(u32). The kernel is
 * allowed to touch the buffer in two cases:
 *
 * 1) when the buffer is being created:
 *     (buffer->free == 0 && buffer->allow_user_free == 0)
 * 2) when the buffer is being torn down:
 *     (buffer->free == 0 && buffer->transaction == NULL).
 *
 * Return: true if the buffer is safe to access
 */
static inline bool check_buffer(struct binder_alloc *alloc,
				struct binder_buffer *buffer,
				binder_size_t offset, size_t bytes)
{
	size_t buffer_size = binder_alloc_buffer_size(alloc, buffer);

	return buffer_size >= bytes &&
		offset <= buffer_size - bytes &&
		IS_ALIGNED(offset, sizeof(u32)) &&
		!buffer->free &&
		(!buffer->allow_user_free || !buffer->transaction);
}

/*
 * binder_alloc_copy_user_to_buffer - 从用户空间拷贝数据到 Binder 缓冲区
 * @alloc: binder_alloc 实例
 * @buffer: 目标 Binder 缓冲区
 * @buffer_offset: 缓冲区内的偏移
 * @from: 用户空间源地址
 * @bytes: 拷贝字节数
 *
 * 使用 copy_from_user 安全地将用户空间数据逐页拷贝到 Binder 缓冲区。
 * 每页使用 kmap_local_page 临时映射，通过 binder_alloc_get_page()
 * 获取目标页面和页内偏移。剩余未拷贝的字节数通过返回值传递。
 *
 * 返回: 剩余未拷贝的字节数 (0 表示全部拷贝成功)
 */

/**
 * binder_alloc_copy_user_to_buffer() - copy src user to tgt user
 * @alloc: binder_alloc for this proc
 * @buffer: binder buffer to be accessed
 * @buffer_offset: offset into @buffer data
 * @from: userspace pointer to source buffer
 * @bytes: bytes to copy
 *
 * Copy bytes from source userspace to target buffer.
 *
 * Return: bytes remaining to be copied
 */
unsigned long
binder_alloc_copy_user_to_buffer(struct binder_alloc *alloc,
				 struct binder_buffer *buffer,
				 binder_size_t buffer_offset,
				 const void __user *from,
				 size_t bytes)
{
	if (!check_buffer(alloc, buffer, buffer_offset, bytes))
		return bytes;

	while (bytes) {
		unsigned long size;
		unsigned long ret;
		struct page *page;
		pgoff_t pgoff;
		void *kptr;

		page = binder_alloc_get_page(alloc, buffer,
					     buffer_offset, &pgoff);
		size = min_t(size_t, bytes, PAGE_SIZE - pgoff);
		kptr = kmap_local_page(page) + pgoff;
		ret = copy_from_user(kptr, from, size);
		kunmap_local(kptr);
		if (ret)
			return bytes - size + ret;
		bytes -= size;
		from += size;
		buffer_offset += size;
	}
	return 0;
}

/*
 * binder_alloc_do_buffer_copy - Binder 缓冲区内核空间数据拷贝 (内部函数)
 * @alloc: binder_alloc 实例
 * @to_buffer: true 表示从 ptr 拷贝到 buffer，false 表示从 buffer 拷贝到 ptr
 * @buffer: 目标/源 Binder 缓冲区
 * @buffer_offset: 缓冲区内的偏移
 * @ptr: 内核空间数据指针
 * @bytes: 拷贝字节数
 *
 * 使用 memcpy_to_page/memcpy_from_page 在 Binder 缓冲区和内核缓冲区之间
 * 逐页拷贝数据。所有拷贝必须 32 位对齐。
 * 被 binder_alloc_copy_to_buffer() 和 binder_alloc_copy_from_buffer() 调用。
 *
 * 返回: 0 成功，-EINVAL 安全检查失败
 */
static int binder_alloc_do_buffer_copy(struct binder_alloc *alloc,
				       bool to_buffer,
				       struct binder_buffer *buffer,
				       binder_size_t buffer_offset,
				       void *ptr,
				       size_t bytes)
{
	/* All copies must be 32-bit aligned and 32-bit size */
	if (!check_buffer(alloc, buffer, buffer_offset, bytes))
		return -EINVAL;

	while (bytes) {
		unsigned long size;
		struct page *page;
		pgoff_t pgoff;

		page = binder_alloc_get_page(alloc, buffer,
					     buffer_offset, &pgoff);
		size = min_t(size_t, bytes, PAGE_SIZE - pgoff);
		if (to_buffer)
			memcpy_to_page(page, pgoff, ptr, size);
		else
			memcpy_from_page(ptr, page, pgoff, size);
		bytes -= size;
		pgoff = 0;
		ptr = ptr + size;
		buffer_offset += size;
	}
	return 0;
}

/*
 * binder_alloc_copy_to_buffer - 从内核空间拷贝数据到 Binder 缓冲区
 * @alloc: binder_alloc 实例
 * @buffer: 目标 Binder 缓冲区
 * @buffer_offset: 缓冲区内的偏移
 * @src: 内核空间源数据
 * @bytes: 拷贝字节数
 *
 * 调用 binder_alloc_do_buffer_copy(to_buffer=true) 将内核数据逐页
 * 写入 Binder 缓冲区。用于 Binder 驱动序列化事务数据 (Parcel) 到缓冲区。
 *
 * 返回: 0 成功，-EINVAL 安全检查失败
 */
int binder_alloc_copy_to_buffer(struct binder_alloc *alloc,
				struct binder_buffer *buffer,
				binder_size_t buffer_offset,
				void *src,
				size_t bytes)
{
	return binder_alloc_do_buffer_copy(alloc, true, buffer, buffer_offset,
					   src, bytes);
}

/*
 * binder_alloc_copy_from_buffer - 从 Binder 缓冲区拷贝数据到内核空间
 * @alloc: binder_alloc 实例
 * @dest: 内核空间目标地址
 * @buffer: 源 Binder 缓冲区
 * @buffer_offset: 缓冲区内的偏移
 * @bytes: 拷贝字节数
 *
 * 调用 binder_alloc_do_buffer_copy(to_buffer=false) 从 Binder 缓冲区
 * 逐页读取数据到内核空间。用于 Binder 驱动在事务处理中读取 Parcel 数据。
 *
 * 返回: 0 成功，-EINVAL 安全检查失败
 */
int binder_alloc_copy_from_buffer(struct binder_alloc *alloc,
				  void *dest,
				  struct binder_buffer *buffer,
				  binder_size_t buffer_offset,
				  size_t bytes)
{
	return binder_alloc_do_buffer_copy(alloc, false, buffer, buffer_offset,
					   dest, bytes);
}
