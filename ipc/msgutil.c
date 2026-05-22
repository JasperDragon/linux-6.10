// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * linux/ipc/msgutil.c — 消息存储管理 (SysV msg + POSIX mqueue 共享)
 * Copyright (C) 1999, 2004 Manfred Spraul
 *
 * ============================================================================
 * 消息内存布局
 * ============================================================================
 *
 * 消息可能很大 (最大可达 8192*PAGE_SIZE), 单页往往装不下整条消息。
 * 因此内核使用链表分页存储:
 *
 *   msg_msg (首页)
 *     ├─ struct msg_msg header (m_type, m_ts, next, security)
 *     ├─ 消息数据 (最多 DATALEN_MSG = PAGE_SIZE - sizeof(msg_msg) 字节)
 *     └─ msg->next ─→ msg_msgseg (续页 #1)
 *                      ├─ next → msg_msgseg (续页 #2)
 *                      │   └─ next → NULL
 *                      └─ 消息数据 (最多 DATALEN_SEG 字节)
 *
 * 每条消息体紧跟在 header 之后 (header + 1 即数据起始),
 * 利用 C 语言的灵活数组成员语义简化地址计算。
 *
 * 关键函数:
 *   alloc_msg()  — 只分配内核内存, 不拷贝数据
 *   load_msg()   — 分配 + 从用户空间 copy_from_user (用于 msgsnd)
 *   copy_msg()   — 内核空间内拷贝 (用于 CRIU 检查点/恢复)
 *   store_msg()  — 拷贝到用户空间 copy_to_user (用于 msgrcv)
 *   free_msg()   — 遍历链表释放所有页
 */

#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/ipc_namespace.h>
#include <linux/utsname.h>
#include <linux/proc_ns.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/nstree.h>

#include "util.h"

DEFINE_SPINLOCK(mq_lock);

/*
 * init_ipc_ns: 初始 (根) IPC namespace.
 *
 * 这是系统中第一个 IPC namespace, 所有的 IPC 资源默认创建在这里。
 * 容器化环境中的新 namespace 通过 copy_ipcs() 从这里派生。
 */
struct ipc_namespace init_ipc_ns = {
	.ns = NS_COMMON_INIT(init_ipc_ns),
	.user_ns = &init_user_ns,
};

/*
 * msg_msgseg — 消息的续页 (非首页).
 *
 * 当消息体积超过一页能容纳的量时, 剩余数据存储在一个或多个
 * msg_msgseg 中, 通过 next 指针串联成单链表。
 */
struct msg_msgseg {
	struct msg_msgseg *next;
	/* the next part of the message follows immediately */
};

/* 首页可存储的消息数据量 = PAGE_SIZE - sizeof(msg_msg header) */
#define DATALEN_MSG	((size_t)PAGE_SIZE-sizeof(struct msg_msg))
/* 续页可存储的消息数据量 = PAGE_SIZE - sizeof(msg_msgseg header) */
#define DATALEN_SEG	((size_t)PAGE_SIZE-sizeof(struct msg_msgseg))

static kmem_buckets *msg_buckets __ro_after_init;

static int __init init_msg_buckets(void)
{
	msg_buckets = kmem_buckets_create("msg_msg", SLAB_ACCOUNT,
					  sizeof(struct msg_msg),
					  DATALEN_MSG, NULL);

	return 0;
}
subsys_initcall(init_msg_buckets);

/*
 * alloc_msg — 分配一个 msg_msg 链表 (仅分配内核内存, 不拷贝用户数据).
 *
 * 根据 len 计算出需要多少页: 首页 + 若干续页。
 * 每页的实际数据容量不同: 首页是 DATALEN_MSG, 续页是 DATALEN_SEG。
 * 返回的 msg 链表的 m_ts 字段尚未设置 (由调用方如 load_msg 设置).
 */
static struct msg_msg *alloc_msg(size_t len)
{
	struct msg_msg *msg;
	struct msg_msgseg **pseg;
	size_t alen;

	alen = min(len, DATALEN_MSG);
	msg = kmem_buckets_alloc(msg_buckets, sizeof(*msg) + alen, GFP_KERNEL);
	if (msg == NULL)
		return NULL;

	msg->next = NULL;
	msg->security = NULL;

	len -= alen;
	pseg = &msg->next;
	while (len > 0) {
		struct msg_msgseg *seg;

		cond_resched();

		alen = min(len, DATALEN_SEG);
		seg = kmalloc(sizeof(*seg) + alen, GFP_KERNEL_ACCOUNT);
		if (seg == NULL)
			goto out_err;
		*pseg = seg;
		seg->next = NULL;
		pseg = &seg->next;
		len -= alen;
	}

	return msg;

out_err:
	free_msg(msg);
	return NULL;
}

/*
 * load_msg — 分配消息并从用户空间拷贝数据 (用于 msgsnd/mq_timedsend).
 *
 * 这是 msgsnd 和 mq_timedsend 的共同路径:
 *   1) alloc_msg() 分配内核消息链表
 *   2) 逐页从用户空间 copy_from_user
 *   3) security_msg_msg_alloc() LSM 审计
 *
 * 返回值: 成功返回 msg_msg 指针, 失败返回 ERR_PTR.
 */
struct msg_msg *load_msg(const void __user *src, size_t len)
{
	struct msg_msg *msg;
	struct msg_msgseg *seg;
	int err = -EFAULT;
	size_t alen;

	msg = alloc_msg(len);
	if (msg == NULL)
		return ERR_PTR(-ENOMEM);

	alen = min(len, DATALEN_MSG);
	if (copy_from_user(msg + 1, src, alen))
		goto out_err;

	for (seg = msg->next; seg != NULL; seg = seg->next) {
		len -= alen;
		src = (char __user *)src + alen;
		alen = min(len, DATALEN_SEG);
		if (copy_from_user(seg + 1, src, alen))
			goto out_err;
	}

	err = security_msg_msg_alloc(msg);
	if (err)
		goto out_err;

	return msg;

out_err:
	free_msg(msg);
	return ERR_PTR(err);
}

#ifdef CONFIG_CHECKPOINT_RESTORE
/*
 * copy_msg — CRIU 检查点/恢复的内核内消息拷贝.
 */
struct msg_msg *copy_msg(struct msg_msg *src, struct msg_msg *dst)
{
	struct msg_msgseg *dst_pseg, *src_pseg;
	size_t len = src->m_ts;
	size_t alen;

	if (src->m_ts > dst->m_ts)
		return ERR_PTR(-EINVAL);

	alen = min(len, DATALEN_MSG);
	memcpy(dst + 1, src + 1, alen);

	for (dst_pseg = dst->next, src_pseg = src->next;
	     src_pseg != NULL;
	     dst_pseg = dst_pseg->next, src_pseg = src_pseg->next) {

		len -= alen;
		alen = min(len, DATALEN_SEG);
		memcpy(dst_pseg + 1, src_pseg + 1, alen);
	}

	dst->m_type = src->m_type;
	dst->m_ts = src->m_ts;

	return dst;
}
#else
struct msg_msg *copy_msg(struct msg_msg *src, struct msg_msg *dst)
{
	return ERR_PTR(-ENOSYS);
}
#endif

/*
 * store_msg — 将内核消息拷贝到用户空间 (用于 msgrcv/mq_timedreceive).
 *
 * 返回值: 0 = 成功, -1 = copy_to_user 失败 (EFAULT).
 */
int store_msg(void __user *dest, struct msg_msg *msg, size_t len)
{
	size_t alen;
	struct msg_msgseg *seg;

	alen = min(len, DATALEN_MSG);
	if (copy_to_user(dest, msg + 1, alen))
		return -1;

	for (seg = msg->next; seg != NULL; seg = seg->next) {
		len -= alen;
		dest = (char __user *)dest + alen;
		alen = min(len, DATALEN_SEG);
		if (copy_to_user(dest, seg + 1, alen))
			return -1;
	}
	return 0;
}

/*
 * free_msg — 释放 msg_msg 链表占用的所有内存.
 *
 * 遍历首页 + 所有续页, 逐页 kfree.
 * 先调 LSM 钩子 security_msg_msg_free 清理安全上下文。
 */
void free_msg(struct msg_msg *msg)
{
	struct msg_msgseg *seg;

	security_msg_msg_free(msg);

	seg = msg->next;
	kfree(msg);
	while (seg != NULL) {
		struct msg_msgseg *tmp = seg->next;

		cond_resched();
		kfree(seg);
		seg = tmp;
	}
}
