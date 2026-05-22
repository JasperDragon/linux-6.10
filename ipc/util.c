// SPDX-License-Identifier: GPL-2.0
/*
 * linux/ipc/util.c — System V IPC 基础设施
 * Copyright (C) 1992 Krishna Balasubramanian
 *
 * 历史修订:
 *   Sep 1997 - 将 suser() 权限检查放在普通权限检查之后 (BSD 风格)
 *   Nov 1999 - IPC 辅助函数, 统一 SMP 锁
 *   Oct 2002 - 每个 IPC ID 独立 spinlock + RCU free
 *   Mar 2006 - IPC 对象审计 (audit) 支持
 *   Jun 2006 - IPC namespace 支持 (OpenVZ)
 *
 * ============================================================================
 * SysV IPC 通用锁协议
 * ============================================================================
 *
 * 内核中有两级 IPC 锁，按粒度从粗到细：
 *
 *   1) ipc_ids.rwsem (读写信号量)
 *      - 保护 IPC ID 集合 (ids) 中条目的创建、删除和遍历
 *      - 保护 /proc/sysvipc/ 下的遍历
 *      - 写锁用于增删条目，读锁用于遍历
 *
 *   2) kern_ipc_perm.lock (每对象 spinlock)
 *      - 保护单个 IPC 对象的数据字段
 *      - 保护需要原子性的读操作 (如 STAT 命令)
 *      - 保护所有数据更新操作 (SET, RMID, semop, msgsnd 等)
 *
 * 典型操作流程:
 *   rcu_read_lock()                        ← RCU 读临界区开始
 *     ipc_obtain_object_check()            ← 通过 ID 找到 IPC 对象 (无锁)
 *       ├─ 快速检查 (能力、审计、权限等)    ← 不持对象锁, 不用原子性
 *       │
 *       └─ ipc_lock_object()              ← 获取对象 spinlock
 *            ├─ 需要原子性的读操作 (STAT)
 *            ├─ 数据更新 (SET, RMID)
 *            └─ 类型特有操作 (semop, msgsnd, shmat 等)
 *          ipc_unlock_object()            ← 释放对象 spinlock
 *   rcu_read_unlock()                      ← RCU 读临界区结束
 *
 * 特别注意:
 *   - 信号量有特殊的快速路径 (sem_lock)，可绕过 kern_ipc_perm.lock
 *     详见 sem.c 的注释
 *   - RCU 保证了 IDR 查找的无锁安全: 对象释放走 call_rcu() 延迟回收
 *   - refcount 机制: 每个对象从 refcount=1 开始，getref 加, putref 减到 0 时
 *     触发 RCU 延迟销毁
 */

#include <linux/mm.h>
#include <linux/shm.h>
#include <linux/init.h>
#include <linux/msg.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/capability.h>
#include <linux/highuid.h>
#include <linux/security.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/audit.h>
#include <linux/nsproxy.h>
#include <linux/rwsem.h>
#include <linux/memory.h>
#include <linux/ipc_namespace.h>
#include <linux/rhashtable.h>
#include <linux/log2.h>

#include <asm/unistd.h>

#include "util.h"

struct ipc_proc_iface {
	const char *path;
	const char *header;
	int ids;
	int (*show)(struct seq_file *, void *);
};

/**
 * ipc_init - initialise ipc subsystem
 *
 * The various sysv ipc resources (semaphores, messages and shared
 * memory) are initialised.
 *
 * A callback routine is registered into the memory hotplug notifier
 * chain: since msgmni scales to lowmem this callback routine will be
 * called upon successful memory add / remove to recompute msmgni.
 */
static int __init ipc_init(void)
{
	proc_mkdir("sysvipc", NULL);
	sem_init();
	msg_init();
	shm_init();

	return 0;
}
device_initcall(ipc_init);

static const struct rhashtable_params ipc_kht_params = {
	.head_offset		= offsetof(struct kern_ipc_perm, khtnode),
	.key_offset		= offsetof(struct kern_ipc_perm, key),
	.key_len		= sizeof_field(struct kern_ipc_perm, key),
	.automatic_shrinking	= true,
};

/**
 * ipc_init_ids - 初始化 IPC 标识符集合。
 * @ids: 待初始化的 IPC ID 集合
 *
 * 完成三项初始化:
 * 1. 初始化 rwsem (保护 ID 集合的创建/删除/遍历)
 * 2. 初始化 key_ht (key → IPC 对象的哈希表, 用于 IPC_PRIVATE 以外的 key 查找)
 * 3. 初始化 ipcs_idr (ID → IPC 对象的 IDR 映射树)
 */
void ipc_init_ids(struct ipc_ids *ids)
{
	ids->in_use = 0;
	ids->seq = 0;
	init_rwsem(&ids->rwsem);
	rhashtable_init(&ids->key_ht, &ipc_kht_params);
	idr_init(&ids->ipcs_idr);
	ids->max_idx = -1;
	ids->last_idx = -1;
#ifdef CONFIG_CHECKPOINT_RESTORE
	ids->next_id = -1;
#endif
}

#ifdef CONFIG_PROC_FS
static const struct proc_ops sysvipc_proc_ops;
/**
 * ipc_init_proc_interface -  create a proc interface for sysipc types using a seq_file interface.
 * @path: Path in procfs
 * @header: Banner to be printed at the beginning of the file.
 * @ids: ipc id table to iterate.
 * @show: show routine.
 */
void __init ipc_init_proc_interface(const char *path, const char *header,
		int ids, int (*show)(struct seq_file *, void *))
{
	struct proc_dir_entry *pde;
	struct ipc_proc_iface *iface;

	iface = kmalloc_obj(*iface);
	if (!iface)
		return;
	iface->path	= path;
	iface->header	= header;
	iface->ids	= ids;
	iface->show	= show;

	pde = proc_create_data(path,
			       S_IRUGO,        /* world readable */
			       NULL,           /* parent dir */
			       &sysvipc_proc_ops,
			       iface);
	if (!pde)
		kfree(iface);
}
#endif

/**
 * ipc_findkey	- find a key in an ipc identifier set
 * @ids: ipc identifier set
 * @key: key to find
 *
 * Returns the locked pointer to the ipc structure if found or NULL
 * otherwise. If key is found ipc points to the owning ipc structure
 *
 * Called with writer ipc_ids.rwsem held.
 */
static struct kern_ipc_perm *ipc_findkey(struct ipc_ids *ids, key_t key)
{
	struct kern_ipc_perm *ipcp;

	ipcp = rhashtable_lookup_fast(&ids->key_ht, &key,
					      ipc_kht_params);
	if (!ipcp)
		return NULL;

	rcu_read_lock();
	ipc_lock_object(ipcp);
	return ipcp;
}

/*
 * Insert new IPC object into idr tree, and set sequence number and id
 * in the correct order.
 * Especially:
 * - the sequence number must be set before inserting the object into the idr,
 *   because the sequence number is accessed without a lock.
 * - the id can/must be set after inserting the object into the idr.
 *   All accesses must be done after getting kern_ipc_perm.lock.
 *
 * The caller must own kern_ipc_perm.lock.of the new object.
 * On error, the function returns a (negative) error code.
 *
 * To conserve sequence number space, especially with extended ipc_mni,
 * the sequence number is incremented only when the returned ID is less than
 * the last one.
 */
static inline int ipc_idr_alloc(struct ipc_ids *ids, struct kern_ipc_perm *new)
{
	int idx, next_id = -1;

#ifdef CONFIG_CHECKPOINT_RESTORE
	next_id = ids->next_id;
	ids->next_id = -1;
#endif

	/*
	 * As soon as a new object is inserted into the idr,
	 * ipc_obtain_object_idr() or ipc_obtain_object_check() can find it,
	 * and the lockless preparations for ipc operations can start.
	 * This means especially: permission checks, audit calls, allocation
	 * of undo structures, ...
	 *
	 * Thus the object must be fully initialized, and if something fails,
	 * then the full tear-down sequence must be followed.
	 * (i.e.: set new->deleted, reduce refcount, call_rcu())
	 */

	if (next_id < 0) { /* !CHECKPOINT_RESTORE or next_id is unset */
		int max_idx;

		max_idx = max(ids->in_use*3/2, ipc_min_cycle);
		max_idx = min(max_idx, ipc_mni);

		/* allocate the idx, with a NULL struct kern_ipc_perm */
		idx = idr_alloc_cyclic(&ids->ipcs_idr, NULL, 0, max_idx,
					GFP_NOWAIT);

		if (idx >= 0) {
			/*
			 * idx got allocated successfully.
			 * Now calculate the sequence number and set the
			 * pointer for real.
			 */
			if (idx <= ids->last_idx) {
				ids->seq++;
				if (ids->seq >= ipcid_seq_max())
					ids->seq = 0;
			}
			ids->last_idx = idx;

			new->seq = ids->seq;
			/* no need for smp_wmb(), this is done
			 * inside idr_replace, as part of
			 * rcu_assign_pointer
			 */
			idr_replace(&ids->ipcs_idr, new, idx);
		}
	} else {
		new->seq = ipcid_to_seqx(next_id);
		idx = idr_alloc(&ids->ipcs_idr, new, ipcid_to_idx(next_id),
				0, GFP_NOWAIT);
	}
	if (idx >= 0)
		new->id = (new->seq << ipcmni_seq_shift()) + idx;
	return idx;
}

/**
 * ipc_addid - 向 IPC 集合中添加一个新对象，分配 ID。
 * @ids:    IPC 标识符集合
 * @new:    新 IPC 对象 (kern_ipc_perm 基类)
 * @limit:  in_use 的上限 (sem/shm/msg 各自的最大对象数)
 *
 * 核心流程:
 * 1. 初始化 refcount=1, spinlock
 * 2. 设置创建者的 uid/gid (cuid/cgid = euid/egid)
 * 3. 通过 ipc_idr_alloc() 在 IDR 中分配 index, 计算 seq 并组装 ID
 *    - seq 空间保存: 只有新 index <= last_idx 时才递增 seq
 *    - 确保 ID 在对象生命周期内唯一 (ABA 保护)
 * 4. 若 key != IPC_PRIVATE, 注册到 key 哈希表
 * 5. 更新 in_use 计数和 max_idx 缓存
 *
 * 调用条件: 持有 ids->rwsem 写锁
 * 返回: 成功时返回锁定的对象 (spinlock 已持有), 失败返回负 errno
 */
int ipc_addid(struct ipc_ids *ids, struct kern_ipc_perm *new, int limit)
{
	kuid_t euid;
	kgid_t egid;
	int idx, err;

	/* 1) Initialize the refcount so that ipc_rcu_putref works */
	refcount_set(&new->refcount, 1);

	if (limit > ipc_mni)
		limit = ipc_mni;

	if (ids->in_use >= limit)
		return -ENOSPC;

	idr_preload(GFP_KERNEL);

	spin_lock_init(&new->lock);
	rcu_read_lock();
	spin_lock(&new->lock);

	current_euid_egid(&euid, &egid);
	new->cuid = new->uid = euid;
	new->gid = new->cgid = egid;

	new->deleted = false;

	idx = ipc_idr_alloc(ids, new);
	idr_preload_end();

	if (idx >= 0 && new->key != IPC_PRIVATE) {
		err = rhashtable_insert_fast(&ids->key_ht, &new->khtnode,
					     ipc_kht_params);
		if (err < 0) {
			idr_remove(&ids->ipcs_idr, idx);
			idx = err;
		}
	}
	if (idx < 0) {
		new->deleted = true;
		spin_unlock(&new->lock);
		rcu_read_unlock();
		return idx;
	}

	ids->in_use++;
	if (idx > ids->max_idx)
		ids->max_idx = idx;
	return idx;
}

/**
 * ipcget_new -	create a new ipc object
 * @ns: ipc namespace
 * @ids: ipc identifier set
 * @ops: the actual creation routine to call
 * @params: its parameters
 *
 * This routine is called by sys_msgget, sys_semget() and sys_shmget()
 * when the key is IPC_PRIVATE.
 */
static int ipcget_new(struct ipc_namespace *ns, struct ipc_ids *ids,
		const struct ipc_ops *ops, struct ipc_params *params)
{
	int err;

	down_write(&ids->rwsem);
	err = ops->getnew(ns, params);
	up_write(&ids->rwsem);
	return err;
}

/**
 * ipc_check_perms - check security and permissions for an ipc object
 * @ns: ipc namespace
 * @ipcp: ipc permission set
 * @ops: the actual security routine to call
 * @params: its parameters
 *
 * This routine is called by sys_msgget(), sys_semget() and sys_shmget()
 * when the key is not IPC_PRIVATE and that key already exists in the
 * ds IDR.
 *
 * On success, the ipc id is returned.
 *
 * It is called with ipc_ids.rwsem and ipcp->lock held.
 */
static int ipc_check_perms(struct ipc_namespace *ns,
			   struct kern_ipc_perm *ipcp,
			   const struct ipc_ops *ops,
			   struct ipc_params *params)
{
	int err;

	if (ipcperms(ns, ipcp, params->flg))
		err = -EACCES;
	else {
		err = ops->associate(ipcp, params->flg);
		if (!err)
			err = ipcp->id;
	}

	return err;
}

/**
 * ipcget_public - get an ipc object or create a new one
 * @ns: ipc namespace
 * @ids: ipc identifier set
 * @ops: the actual creation routine to call
 * @params: its parameters
 *
 * This routine is called by sys_msgget, sys_semget() and sys_shmget()
 * when the key is not IPC_PRIVATE.
 * It adds a new entry if the key is not found and does some permission
 * / security checkings if the key is found.
 *
 * On success, the ipc id is returned.
 */
static int ipcget_public(struct ipc_namespace *ns, struct ipc_ids *ids,
		const struct ipc_ops *ops, struct ipc_params *params)
{
	struct kern_ipc_perm *ipcp;
	int flg = params->flg;
	int err;

	/*
	 * Take the lock as a writer since we are potentially going to add
	 * a new entry + read locks are not "upgradable"
	 */
	down_write(&ids->rwsem);
	ipcp = ipc_findkey(ids, params->key);
	if (ipcp == NULL) {
		/* key not used */
		if (!(flg & IPC_CREAT))
			err = -ENOENT;
		else
			err = ops->getnew(ns, params);
	} else {
		/* ipc object has been locked by ipc_findkey() */

		if (flg & IPC_CREAT && flg & IPC_EXCL)
			err = -EEXIST;
		else {
			err = 0;
			if (ops->more_checks)
				err = ops->more_checks(ipcp, params);
			if (!err)
				/*
				 * ipc_check_perms returns the IPC id on
				 * success
				 */
				err = ipc_check_perms(ns, ipcp, ops, params);
		}
		ipc_unlock(ipcp);
	}
	up_write(&ids->rwsem);

	return err;
}

/**
 * ipc_kht_remove - remove an ipc from the key hashtable
 * @ids: ipc identifier set
 * @ipcp: ipc perm structure containing the key to remove
 *
 * ipc_ids.rwsem (as a writer) and the spinlock for this ID are held
 * before this function is called, and remain locked on the exit.
 */
static void ipc_kht_remove(struct ipc_ids *ids, struct kern_ipc_perm *ipcp)
{
	if (ipcp->key != IPC_PRIVATE)
		WARN_ON_ONCE(rhashtable_remove_fast(&ids->key_ht, &ipcp->khtnode,
				       ipc_kht_params));
}

/**
 * ipc_search_maxidx - search for the highest assigned index
 * @ids: ipc identifier set
 * @limit: known upper limit for highest assigned index
 *
 * The function determines the highest assigned index in @ids. It is intended
 * to be called when ids->max_idx needs to be updated.
 * Updating ids->max_idx is necessary when the current highest index ipc
 * object is deleted.
 * If no ipc object is allocated, then -1 is returned.
 *
 * ipc_ids.rwsem needs to be held by the caller.
 */
static int ipc_search_maxidx(struct ipc_ids *ids, int limit)
{
	int tmpidx;
	int i;
	int retval;

	i = ilog2(limit+1);

	retval = 0;
	for (; i >= 0; i--) {
		tmpidx = retval | (1<<i);
		/*
		 * "0" is a possible index value, thus search using
		 * e.g. 15,7,3,1,0 instead of 16,8,4,2,1.
		 */
		tmpidx = tmpidx-1;
		if (idr_get_next(&ids->ipcs_idr, &tmpidx))
			retval |= (1<<i);
	}
	return retval - 1;
}

/**
 * ipc_rmid - remove an ipc identifier
 * @ids: ipc identifier set
 * @ipcp: ipc perm structure containing the identifier to remove
 *
 * ipc_ids.rwsem (as a writer) and the spinlock for this ID are held
 * before this function is called, and remain locked on the exit.
 */
void ipc_rmid(struct ipc_ids *ids, struct kern_ipc_perm *ipcp)
{
	int idx = ipcid_to_idx(ipcp->id);

	WARN_ON_ONCE(idr_remove(&ids->ipcs_idr, idx) != ipcp);
	ipc_kht_remove(ids, ipcp);
	ids->in_use--;
	ipcp->deleted = true;

	if (unlikely(idx == ids->max_idx)) {
		idx = ids->max_idx-1;
		if (idx >= 0)
			idx = ipc_search_maxidx(ids, idx);
		ids->max_idx = idx;
	}
}

/**
 * ipc_set_key_private - switch the key of an existing ipc to IPC_PRIVATE
 * @ids: ipc identifier set
 * @ipcp: ipc perm structure containing the key to modify
 *
 * ipc_ids.rwsem (as a writer) and the spinlock for this ID are held
 * before this function is called, and remain locked on the exit.
 */
void ipc_set_key_private(struct ipc_ids *ids, struct kern_ipc_perm *ipcp)
{
	ipc_kht_remove(ids, ipcp);
	ipcp->key = IPC_PRIVATE;
}

/*
 * ipc_rcu_getref — 尝试增加 IPC 对象的引用计数。
 *
 * 返回 true: 成功, refcount > 0, 对象仍然存活
 * 返回 false: 失败, refcount 已经为 0 (对象正在被销毁)
 *
 * 这是 RCU + refcount 组合模式的关键:
 * - RCU 保护对象指针的可见性
 * - refcount 保护对象的内存生命周期
 * - 两者配合允许无锁地访问正在被并发删除的 IPC 对象
 */
bool ipc_rcu_getref(struct kern_ipc_perm *ptr)
{
	return refcount_inc_not_zero(&ptr->refcount);
}

/*
 * ipc_rcu_putref — 减少引用计数, 归零时通过 call_rcu 调度延迟销毁。
 *
 * refcount 从 1 → 0 时调用 call_rcu(), 保证在所有的 RCU 读者都退出
 * 临界区之后才执行 func 真正释放内存。
 *
 * "ipc_addid 初始化 refcount=1" + "putref 归零时 RCU free"
 * 构成了 IPC 对象的完整生命周期管理。
 */
void ipc_rcu_putref(struct kern_ipc_perm *ptr,
			void (*func)(struct rcu_head *head))
{
	if (!refcount_dec_and_test(&ptr->refcount))
		return;

	call_rcu(&ptr->rcu, func);
}

/**
 * ipcperms - check ipc permissions
 * @ns: ipc namespace
 * @ipcp: ipc permission set
 * @flag: desired permission set
 *
 * Check user, group, other permissions for access
 * to ipc resources. return 0 if allowed
 *
 * @flag will most probably be 0 or ``S_...UGO`` from <linux/stat.h>
 */
int ipcperms(struct ipc_namespace *ns, struct kern_ipc_perm *ipcp, short flag)
{
	kuid_t euid = current_euid();
	int requested_mode, granted_mode;

	audit_ipc_obj(ipcp);
	requested_mode = (flag >> 6) | (flag >> 3) | flag;
	granted_mode = ipcp->mode;
	if (uid_eq(euid, ipcp->cuid) ||
	    uid_eq(euid, ipcp->uid))
		granted_mode >>= 6;
	else if (in_group_p(ipcp->cgid) || in_group_p(ipcp->gid))
		granted_mode >>= 3;
	/* is there some bit set in requested_mode but not in granted_mode? */
	if ((requested_mode & ~granted_mode & 0007) &&
	    !ns_capable(ns->user_ns, CAP_IPC_OWNER))
		return -1;

	return security_ipc_permission(ipcp, flag);
}

/*
 * Functions to convert between the kern_ipc_perm structure and the
 * old/new ipc_perm structures
 */

/**
 * kernel_to_ipc64_perm	- convert kernel ipc permissions to user
 * @in: kernel permissions
 * @out: new style ipc permissions
 *
 * Turn the kernel object @in into a set of permissions descriptions
 * for returning to userspace (@out).
 */
void kernel_to_ipc64_perm(struct kern_ipc_perm *in, struct ipc64_perm *out)
{
	out->key	= in->key;
	out->uid	= from_kuid_munged(current_user_ns(), in->uid);
	out->gid	= from_kgid_munged(current_user_ns(), in->gid);
	out->cuid	= from_kuid_munged(current_user_ns(), in->cuid);
	out->cgid	= from_kgid_munged(current_user_ns(), in->cgid);
	out->mode	= in->mode;
	out->seq	= in->seq;
}

/**
 * ipc64_perm_to_ipc_perm - convert new ipc permissions to old
 * @in: new style ipc permissions
 * @out: old style ipc permissions
 *
 * Turn the new style permissions object @in into a compatibility
 * object and store it into the @out pointer.
 */
void ipc64_perm_to_ipc_perm(struct ipc64_perm *in, struct ipc_perm *out)
{
	out->key	= in->key;
	SET_UID(out->uid, in->uid);
	SET_GID(out->gid, in->gid);
	SET_UID(out->cuid, in->cuid);
	SET_GID(out->cgid, in->cgid);
	out->mode	= in->mode;
	out->seq	= in->seq;
}

/**
 * ipc_obtain_object_idr — 通过 ID 在 IDR 树中查找 IPC 对象 (无锁)。
 * @ids: IPC 标识符集合
 * @id:  用户态传入的 IPC ID (包含 index + seq 的合成值)
 *
 * 此函数仅做 index 查找, 不验证 sequence number。
 * 调用方必须处于 RCU 读临界区中, 且返回的对象 *未加锁*。
 *
 * RCU 保证: 即使对象正在被 ipc_rmid() 删除, idr_find 返回的指针仍然有效,
 * 因为真正的内存释放通过 call_rcu() 延迟到所有 CPU 的 RCU grace period 之后。
 */
struct kern_ipc_perm *ipc_obtain_object_idr(struct ipc_ids *ids, int id)
{
	struct kern_ipc_perm *out;
	int idx = ipcid_to_idx(id);

	out = idr_find(&ids->ipcs_idr, idx);
	if (!out)
		return ERR_PTR(-EINVAL);

	return out;
}

/**
 * ipc_obtain_object_check — 查找并验证 IPC 对象 (无锁, 带 seq 校验)。
 * @ids: IPC 标识符集合
 * @id:  用户态传入的 IPC ID
 *
 * 在 ipc_obtain_object_idr() 的基础上增加 sequence number 校验,
 * 防止 ABA 问题: 如果旧 ID 恰好在对象被删除重建后复用同一个 index,
 * seq 的不同可以检测出"这不是你要的那个对象"。
 */
struct kern_ipc_perm *ipc_obtain_object_check(struct ipc_ids *ids, int id)
{
	struct kern_ipc_perm *out = ipc_obtain_object_idr(ids, id);

	if (IS_ERR(out))
		goto out;

	if (ipc_checkid(out, id))
		return ERR_PTR(-EINVAL);
out:
	return out;
}

/**
 * ipcget - SysV IPC 获取/创建的公共分发函数。
 * @ns:     IPC namespace
 * @ids:    IPC 标识符集合
 * @ops:    IPC 类型相关的操作向量 (getnew/associate/more_checks)
 * @params: key + flag + 类型特定参数
 *
 * 被 sys_msgget() / sys_semget() / sys_shmget() 共同调用。
 *
 * 根据 key 分两条路径:
 *   - IPC_PRIVATE (key=0): → ipcget_new()  无条件创建新对象
 *   - 其他 key:            → ipcget_public() key 查找 → 已有则检查权限
 *                                                  → 无 + IPC_CREAT → 创建
 *                                                  → 无 + 无IPC_CREAT → -ENOENT
 *                                                  → 有 + IPC_EXCL → -EEXIST
 */
int ipcget(struct ipc_namespace *ns, struct ipc_ids *ids,
			const struct ipc_ops *ops, struct ipc_params *params)
{
	if (params->key == IPC_PRIVATE)
		return ipcget_new(ns, ids, ops, params);
	else
		return ipcget_public(ns, ids, ops, params);
}

/**
 * ipc_update_perm - update the permissions of an ipc object
 * @in:  the permission given as input.
 * @out: the permission of the ipc to set.
 */
int ipc_update_perm(struct ipc64_perm *in, struct kern_ipc_perm *out)
{
	kuid_t uid = make_kuid(current_user_ns(), in->uid);
	kgid_t gid = make_kgid(current_user_ns(), in->gid);
	if (!uid_valid(uid) || !gid_valid(gid))
		return -EINVAL;

	out->uid = uid;
	out->gid = gid;
	out->mode = (out->mode & ~S_IRWXUGO)
		| (in->mode & S_IRWXUGO);

	return 0;
}

/**
 * ipcctl_obtain_check - retrieve an ipc object and check permissions
 * @ns:  ipc namespace
 * @ids:  the table of ids where to look for the ipc
 * @id:   the id of the ipc to retrieve
 * @cmd:  the cmd to check
 * @perm: the permission to set
 * @extra_perm: one extra permission parameter used by msq
 *
 * This function does some common audit and permissions check for some IPC_XXX
 * cmd and is called from semctl_down, shmctl_down and msgctl_down.
 *
 * It:
 *   - retrieves the ipc object with the given id in the given table.
 *   - performs some audit and permission check, depending on the given cmd
 *   - returns a pointer to the ipc object or otherwise, the corresponding
 *     error.
 *
 * Call holding the both the rwsem and the rcu read lock.
 */
struct kern_ipc_perm *ipcctl_obtain_check(struct ipc_namespace *ns,
					struct ipc_ids *ids, int id, int cmd,
					struct ipc64_perm *perm, int extra_perm)
{
	kuid_t euid;
	int err = -EPERM;
	struct kern_ipc_perm *ipcp;

	ipcp = ipc_obtain_object_check(ids, id);
	if (IS_ERR(ipcp)) {
		err = PTR_ERR(ipcp);
		goto err;
	}

	audit_ipc_obj(ipcp);
	if (cmd == IPC_SET)
		audit_ipc_set_perm(extra_perm, perm->uid,
				   perm->gid, perm->mode);

	euid = current_euid();
	if (uid_eq(euid, ipcp->cuid) || uid_eq(euid, ipcp->uid)  ||
	    ns_capable(ns->user_ns, CAP_SYS_ADMIN))
		return ipcp; /* successful lookup */
err:
	return ERR_PTR(err);
}

#ifdef CONFIG_ARCH_WANT_IPC_PARSE_VERSION


/**
 * ipc_parse_version - ipc call version
 * @cmd: pointer to command
 *
 * Return IPC_64 for new style IPC and IPC_OLD for old style IPC.
 * The @cmd value is turned from an encoding command and version into
 * just the command code.
 */
int ipc_parse_version(int *cmd)
{
	if (*cmd & IPC_64) {
		*cmd ^= IPC_64;
		return IPC_64;
	} else {
		return IPC_OLD;
	}
}

#endif /* CONFIG_ARCH_WANT_IPC_PARSE_VERSION */

#ifdef CONFIG_PROC_FS
struct ipc_proc_iter {
	struct ipc_namespace *ns;
	struct pid_namespace *pid_ns;
	struct ipc_proc_iface *iface;
};

struct pid_namespace *ipc_seq_pid_ns(struct seq_file *s)
{
	struct ipc_proc_iter *iter = s->private;
	return iter->pid_ns;
}

/**
 * sysvipc_find_ipc - Find and lock the ipc structure based on seq pos
 * @ids: ipc identifier set
 * @pos: expected position
 *
 * The function finds an ipc structure, based on the sequence file
 * position @pos. If there is no ipc structure at position @pos, then
 * the successor is selected.
 * If a structure is found, then it is locked (both rcu_read_lock() and
 * ipc_lock_object()) and  @pos is set to the position needed to locate
 * the found ipc structure.
 * If nothing is found (i.e. EOF), @pos is not modified.
 *
 * The function returns the found ipc structure, or NULL at EOF.
 */
static struct kern_ipc_perm *sysvipc_find_ipc(struct ipc_ids *ids, loff_t *pos)
{
	int tmpidx;
	struct kern_ipc_perm *ipc;

	/* convert from position to idr index -> "-1" */
	tmpidx = *pos - 1;

	ipc = idr_get_next(&ids->ipcs_idr, &tmpidx);
	if (ipc != NULL) {
		rcu_read_lock();
		ipc_lock_object(ipc);

		/* convert from idr index to position  -> "+1" */
		*pos = tmpidx + 1;
	}
	return ipc;
}

static void *sysvipc_proc_next(struct seq_file *s, void *it, loff_t *pos)
{
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;
	struct kern_ipc_perm *ipc = it;

	/* If we had an ipc id locked before, unlock it */
	if (ipc && ipc != SEQ_START_TOKEN)
		ipc_unlock(ipc);

	/* Next -> search for *pos+1 */
	(*pos)++;
	return sysvipc_find_ipc(&iter->ns->ids[iface->ids], pos);
}

/*
 * File positions: pos 0 -> header, pos n -> ipc idx = n - 1.
 * SeqFile iterator: iterator value locked ipc pointer or SEQ_TOKEN_START.
 */
static void *sysvipc_proc_start(struct seq_file *s, loff_t *pos)
{
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;
	struct ipc_ids *ids;

	ids = &iter->ns->ids[iface->ids];

	/*
	 * Take the lock - this will be released by the corresponding
	 * call to stop().
	 */
	down_read(&ids->rwsem);

	/* pos < 0 is invalid */
	if (*pos < 0)
		return NULL;

	/* pos == 0 means header */
	if (*pos == 0)
		return SEQ_START_TOKEN;

	/* Otherwise return the correct ipc structure */
	return sysvipc_find_ipc(ids, pos);
}

static void sysvipc_proc_stop(struct seq_file *s, void *it)
{
	struct kern_ipc_perm *ipc = it;
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;
	struct ipc_ids *ids;

	/* If we had a locked structure, release it */
	if (ipc && ipc != SEQ_START_TOKEN)
		ipc_unlock(ipc);

	ids = &iter->ns->ids[iface->ids];
	/* Release the lock we took in start() */
	up_read(&ids->rwsem);
}

static int sysvipc_proc_show(struct seq_file *s, void *it)
{
	struct ipc_proc_iter *iter = s->private;
	struct ipc_proc_iface *iface = iter->iface;

	if (it == SEQ_START_TOKEN) {
		seq_puts(s, iface->header);
		return 0;
	}

	return iface->show(s, it);
}

static const struct seq_operations sysvipc_proc_seqops = {
	.start = sysvipc_proc_start,
	.stop  = sysvipc_proc_stop,
	.next  = sysvipc_proc_next,
	.show  = sysvipc_proc_show,
};

static int sysvipc_proc_open(struct inode *inode, struct file *file)
{
	struct ipc_proc_iter *iter;

	iter = __seq_open_private(file, &sysvipc_proc_seqops, sizeof(*iter));
	if (!iter)
		return -ENOMEM;

	iter->iface = pde_data(inode);
	iter->ns    = get_ipc_ns(current->nsproxy->ipc_ns);
	iter->pid_ns = get_pid_ns(task_active_pid_ns(current));

	return 0;
}

static int sysvipc_proc_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct ipc_proc_iter *iter = seq->private;
	put_ipc_ns(iter->ns);
	put_pid_ns(iter->pid_ns);
	return seq_release_private(inode, file);
}

static const struct proc_ops sysvipc_proc_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_open	= sysvipc_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= sysvipc_proc_release,
};
#endif /* CONFIG_PROC_FS */
