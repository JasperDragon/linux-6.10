/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017 Google, Inc.
 */

// ============================================================================
// binder_alloc -- Binder 驱动的内存分配器
// ============================================================================
//
// 概述
// binder_alloc 是 Binder IPC 驱动中负责管理进程间通信缓冲区的内存分配器。
// 每个 Binder 进程上下文（binder_proc）拥有一个独立的 binder_alloc 实例，
// 用于管理通过 mmap() 系统调用创建的共享内存区域。
//
// 内存布局
// 每个进程通过 mmap() 映射一块连续的虚拟地址空间，起始地址由 @vm_start 记录，
// 总大小由 @buffer_size 指定。这块空间被分割成多个 binder_buffer，每个 buffer
// 要么是空闲的（free），要么已被分配用于承载一次 Binder 事务的数据。
//
// 数据结构 -- buffer 管理
// - @buffers: 双向链表，按地址顺序链接该进程的所有 buffer（包括空闲和已分配），
//   用于遍历和合并操作。
// - @free_buffers: 红黑树，按 buffer 大小排序，用于快速查找最适合的空闲块。
// - @allocated_buffers: 红黑树，按 buffer 地址排序，用于根据用户空间指针
//   快速定位已分配的 buffer。
//
// 分配策略 -- Best-fit
// 当需要分配一个大小为 S 的 buffer 时，分配器在 @free_buffers 红黑树中查找
// 第一个大小 >= S 的空闲节点（即"最佳适应"）。如果找到的块比所需大得多，
// 则将其分裂为两块：一块用于分配，剩余部分作为一个新的空闲块重新插入
// @free_buffers。这种策略有助于减少内存碎片。
//
// 释放与合并
// 当 buffer 被释放时，分配器通过 @buffers 链表查找其地址前后的相邻 buffer。
// 如果相邻的 buffer 也是空闲的，则将它们合并成一个更大的连续空闲块，以
// 减少外部碎片。合并操作会同时更新 @free_buffers 红黑树。
//
// 异步事务流控
// @free_async_space 记录可用于异步事务的地址空间余量，初始化为总空间的一半。
// 每个异步事务分配时扣减该值，释放时加回。当异步事务占用空间超过阈值时，
// 触发 oneway 垃圾检测（@oneway_spam_detected 置位），后续异步请求将被拒绝，
// 直到空间恢复到健康水平。
//
// 物理页面回收
// @pages 数组记录每个虚拟页面对应的 struct page 指针，@freelist 是一个 LRU
// 链表，在系统内存压力下通过 shrinker 机制回收不再使用的物理页面。回收由
// binder_alloc_free_page() 回调函数完成。
//
// 线程安全
// 所有对 binder_alloc 内部数据结构的修改都由 @mutex 保护，确保在多线程
// 并发访问 Binder 驱动时的安全性。
// ============================================================================

#ifndef _LINUX_BINDER_ALLOC_H
#define _LINUX_BINDER_ALLOC_H

#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/rtmutex.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/list_lru.h>
#include <uapi/linux/android/binder.h>

struct binder_transaction;

/**
 * struct binder_buffer - buffer used for binder transactions
 * @entry:              entry alloc->buffers
 * @rb_node:            node for allocated_buffers/free_buffers rb trees
 * @free:               %true if buffer is free
 * @clear_on_free:      %true if buffer must be zeroed after use
 * @allow_user_free:    %true if user is allowed to free buffer
 * @async_transaction:  %true if buffer is in use for an async txn
 * @oneway_spam_suspect: %true if total async allocate size just exceed
 * spamming detect threshold
 * @debug_id:           unique ID for debugging
 * @transaction:        pointer to associated struct binder_transaction
 * @target_node:        struct binder_node associated with this buffer
 * @data_size:          size of @transaction data
 * @offsets_size:       size of array of offsets
 * @extra_buffers_size: size of space for other objects (like sg lists)
 * @user_data:          user pointer to base of buffer space
 * @pid:                pid to attribute the buffer to (caller)
 *
 * Bookkeeping structure for binder transaction buffers
 */
struct binder_buffer {
	struct list_head entry; /* free and allocated entries by address */
	struct rb_node rb_node; /* free entry by size or allocated entry */
				/* by address */
	unsigned free:1;			// 标记此 buffer 是否空闲（1=空闲，0=已分配）
	unsigned clear_on_free:1;		// 释放时是否需要将缓冲区内容清零
	unsigned allow_user_free:1;		// 是否允许用户空间主动释放此 buffer
	unsigned async_transaction:1;		// 是否用于承载异步（oneway）事务
	unsigned oneway_spam_suspect:1;		// 异步分配大小是否超过垃圾检测阈值
	unsigned debug_id:27;			// 调试用途的唯一标识符
	struct binder_transaction *transaction;	// 指向使用此 buffer 的事务对象
	struct binder_node *target_node;	// 指向事务目标 binder_node
	size_t data_size;			// 事务数据的大小（字节）
	size_t offsets_size;			// 偏移数组的大小（字节）
	size_t extra_buffers_size;		// 额外对象（如 sg 列表）的空间大小
	unsigned long user_data;		// 用户空间缓冲区基地址
	int pid;				// 分配此 buffer 的调用者 PID
};

/**
 * struct binder_shrinker_mdata - binder metadata used to reclaim pages
 * @lru:         LRU entry in binder_freelist
 * @alloc:       binder_alloc owning the page to reclaim
 * @page_index:  offset in @alloc->pages[] into the page to reclaim
 */
struct binder_shrinker_mdata {
	struct list_head lru;			// 在 binder_freelist LRU 链表中的节点
	struct binder_alloc *alloc;		// 拥有此待回收页面的 binder_alloc
	unsigned long page_index;		// 在 alloc->pages[] 数组中的索引
};

static inline struct list_head *page_to_lru(struct page *p)
{
	struct binder_shrinker_mdata *mdata;

	mdata = (struct binder_shrinker_mdata *)page_private(p);

	return &mdata->lru;
}

/**
 * struct binder_alloc - per-binder proc state for binder allocator
 * @mutex:              protects binder_alloc fields
 * @mm:                 copy of task->mm (invariant after open)
 * @vm_start:           base of per-proc address space mapped via mmap
 * @buffers:            list of all buffers for this proc
 * @free_buffers:       rb tree of buffers available for allocation
 *                      sorted by size
 * @allocated_buffers:  rb tree of allocated buffers sorted by address
 * @free_async_space:   VA space available for async buffers. This is
 *                      initialized at mmap time to 1/2 the full VA space
 * @pages:              array of struct page *
 * @freelist:           lru list to use for free pages (invariant after init)
 * @buffer_size:        size of address space specified via mmap
 * @pid:                pid for associated binder_proc (invariant after init)
 * @pages_high:         high watermark of offset in @pages
 * @mapped:             whether the vm area is mapped, each binder instance is
 *                      allowed a single mapping throughout its lifetime
 * @oneway_spam_detected: %true if oneway spam detection fired, clear that
 * flag once the async buffer has returned to a healthy state
 *
 * Bookkeeping structure for per-proc address space management for binder
 * buffers. It is normally initialized during binder_init() and binder_mmap()
 * calls. The address space is used for both user-visible buffers and for
 * struct binder_buffer objects used to track the user buffers
 */
struct binder_alloc {
	struct mutex mutex;			// 保护 binder_alloc 内部状态的互斥锁
	struct mm_struct *mm;			// 关联进程的 mm_struct（打开后不变）
	unsigned long vm_start;			// mmap 映射的虚拟地址空间基址
	struct list_head buffers;		// 所有 buffer 的链表，按地址升序排列
	struct rb_root free_buffers;		// 空闲 buffer 的红黑树，按大小排序（best-fit 查找）
	struct rb_root allocated_buffers;	// 已分配 buffer 的红黑树，按地址排序
	size_t free_async_space;		// 异步事务可用空间余量，初始为总 VA 空间的一半
	struct page **pages;			// 页指针数组，记录每个虚拟页映射的 struct page
	struct list_lru *freelist;		// 空闲页面的 LRU 链表，用于 shrinker 回收
	size_t buffer_size;			// 通过 mmap 指定的地址空间总大小
	int pid;				// 所属 binder_proc 的 PID（初始化后不变）
	size_t pages_high;			// pages 数组已使用偏移的高水位标记
	bool mapped;				// VMA 是否已映射（每个实例只允许一次映射）
	bool oneway_spam_detected;		// 是否触发了 oneway 垃圾检测
};

enum lru_status binder_alloc_free_page(struct list_head *item,
				       struct list_lru_one *lru,
				       void *cb_arg);
struct binder_buffer *binder_alloc_new_buf(struct binder_alloc *alloc,
					   size_t data_size,
					   size_t offsets_size,
					   size_t extra_buffers_size,
					   int is_async);
void binder_alloc_init(struct binder_alloc *alloc);
int binder_alloc_shrinker_init(void);
void binder_alloc_shrinker_exit(void);
void binder_alloc_vma_close(struct binder_alloc *alloc);
struct binder_buffer *
binder_alloc_prepare_to_free(struct binder_alloc *alloc,
			     unsigned long user_ptr);
void binder_alloc_free_buf(struct binder_alloc *alloc,
			   struct binder_buffer *buffer);
int binder_alloc_mmap_handler(struct binder_alloc *alloc,
			      struct vm_area_struct *vma);
void binder_alloc_deferred_release(struct binder_alloc *alloc);
int binder_alloc_get_allocated_count(struct binder_alloc *alloc);
void binder_alloc_print_allocated(struct seq_file *m,
				  struct binder_alloc *alloc);
void binder_alloc_print_pages(struct seq_file *m,
			      struct binder_alloc *alloc);

/**
 * binder_alloc_get_free_async_space() - get free space available for async
 * @alloc:	binder_alloc for this proc
 *
 * Return:	the bytes remaining in the address-space for async transactions
 */
static inline size_t
binder_alloc_get_free_async_space(struct binder_alloc *alloc)
{
	guard(mutex)(&alloc->mutex);
	return alloc->free_async_space;
}

unsigned long
binder_alloc_copy_user_to_buffer(struct binder_alloc *alloc,
				 struct binder_buffer *buffer,
				 binder_size_t buffer_offset,
				 const void __user *from,
				 size_t bytes);

int binder_alloc_copy_to_buffer(struct binder_alloc *alloc,
				struct binder_buffer *buffer,
				binder_size_t buffer_offset,
				void *src,
				size_t bytes);

int binder_alloc_copy_from_buffer(struct binder_alloc *alloc,
				  void *dest,
				  struct binder_buffer *buffer,
				  binder_size_t buffer_offset,
				  size_t bytes);

#if IS_ENABLED(CONFIG_KUNIT)
void __binder_alloc_init(struct binder_alloc *alloc, struct list_lru *freelist);
size_t binder_alloc_buffer_size(struct binder_alloc *alloc,
				struct binder_buffer *buffer);
#endif

#endif /* _LINUX_BINDER_ALLOC_H */

