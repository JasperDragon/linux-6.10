// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/fd.h>
#include <linux/tty.h>
#include <linux/suspend.h>
#include <linux/root_dev.h>
#include <linux/security.h>
#include <linux/delay.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/initrd.h>
#include <linux/async.h>
#include <linux/fs_struct.h>
#include <linux/slab.h>
#include <linux/ramfs.h>
#include <linux/shmem_fs.h>
#include <linux/ktime.h>

#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/nfs_mount.h>
#include <linux/raid/detect.h>
#include <uapi/linux/mount.h>

#include "do_mounts.h"

int root_mountflags = MS_RDONLY | MS_SILENT;
static char __initdata saved_root_name[64];
static int root_wait;

dev_t ROOT_DEV;

static int __init readonly(char *str)
{
	if (*str)
		return 0;
	root_mountflags |= MS_RDONLY;
	return 1;
}

static int __init readwrite(char *str)
{
	if (*str)
		return 0;
	root_mountflags &= ~MS_RDONLY;
	return 1;
}

__setup("ro", readonly);
__setup("rw", readwrite);

static int __init root_dev_setup(char *line)
{
	strscpy(saved_root_name, line, sizeof(saved_root_name));
	return 1;
}

__setup("root=", root_dev_setup);

static int __init rootwait_setup(char *str)
{
	if (*str)
		return 0;
	root_wait = -1;
	return 1;
}

__setup("rootwait", rootwait_setup);

static int __init rootwait_timeout_setup(char *str)
{
	int sec;

	if (kstrtoint(str, 0, &sec) || sec < 0) {
		pr_warn("ignoring invalid rootwait value\n");
		goto ignore;
	}

	if (check_mul_overflow(sec, MSEC_PER_SEC, &root_wait)) {
		pr_warn("ignoring excessive rootwait value\n");
		goto ignore;
	}

	return 1;

ignore:
	/* Fallback to indefinite wait */
	root_wait = -1;

	return 1;
}

__setup("rootwait=", rootwait_timeout_setup);

static char * __initdata root_mount_data;
static int __init root_data_setup(char *str)
{
	root_mount_data = str;
	return 1;
}

static char * __initdata root_fs_names;
static int __init fs_names_setup(char *str)
{
	root_fs_names = str;
	return 1;
}

static unsigned int __initdata root_delay;
static int __init root_delay_setup(char *str)
{
	if (kstrtouint(str, 0, &root_delay))
		return 0;
	return 1;
}

__setup("rootflags=", root_data_setup);
__setup("rootfstype=", fs_names_setup);
__setup("rootdelay=", root_delay_setup);

/*
 * rootfstype= 允许传入逗号分隔的多个候选文件系统。这里原地把它拆成一串
 * '\0' 结尾的小字符串，方便后面逐个尝试挂载。
 *
 * 该函数可能返回空字符串项，调用方需要自行跳过。
 */
static int __init split_fs_names(char *page, size_t size)
{
	int count = 1;
	char *p = page;

	strscpy(p, root_fs_names, size);
	while (*p++) {
		if (p[-1] == ',') {
			p[-1] = '\0';
			count++;
		}
	}

	return count;
}

static int __init do_mount_root(const char *name, const char *fs,
				 const int flags, const void *data)
{
	struct super_block *s;
	struct page *p = NULL;
	char *data_page = NULL;
	int ret;

	if (data) {
		/* init_mount() 的 data 参数按整页复制，这里提前准备好一页临时缓冲。 */
		p = alloc_page(GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		data_page = page_address(p);
		strscpy_pad(data_page, data, PAGE_SIZE);
	}

	ret = init_mount(name, "/root", fs, flags, data_page);
	if (ret)
		goto out;

	/*
	 * 挂载成功后，当前任务的 cwd 直接切到 /root。后面的 pivot_root 流程会把
	 * 这里作为“新根候选”继续处理。
	 */
	init_chdir("/root");
	s = current->fs->pwd.dentry->d_sb;
	ROOT_DEV = s->s_dev;
	printk(KERN_INFO
	       "VFS: Mounted root (%s filesystem)%s on device %u:%u.\n",
	       s->s_type->name,
	       sb_rdonly(s) ? " readonly" : "",
	       MAJOR(ROOT_DEV), MINOR(ROOT_DEV));

out:
	if (p)
		put_page(p);
	return ret;
}

void __init mount_root_generic(char *name, char *pretty_name, int flags)
{
	struct page *page = alloc_page(GFP_KERNEL);
	char *fs_names = page_address(page);
	char *p;
	char b[BDEVNAME_SIZE];
	int num_fs, i;

	/*
	 * 常规块设备根文件系统路径：
	 * - 若用户显式指定 rootfstype=，按给定顺序尝试
	 * - 否则枚举所有 block-based 文件系统逐个试挂
	 * - 若读写挂载失败，再回退成只读重试一次
	 */
	scnprintf(b, BDEVNAME_SIZE, "unknown-block(%u,%u)",
		  MAJOR(ROOT_DEV), MINOR(ROOT_DEV));
	if (root_fs_names)
		num_fs = split_fs_names(fs_names, PAGE_SIZE);
	else
		num_fs = list_bdev_fs_names(fs_names, PAGE_SIZE);
retry:
	for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1) {
		int err;

		if (!*p)
			continue;
		err = do_mount_root(name, p, flags, root_mount_data);
		switch (err) {
			case 0:
				goto out;
			case -EACCES:
			case -EINVAL:
#ifdef CONFIG_BLOCK
				init_flush_fput();
#endif
				continue;
		}
		/*
		 * 走到这里说明不是“该 fs 不识别这个 superblock”这一类可继续尝试的错误，
		 * 而是根设备本身打不开或更严重的问题。此时直接把已知分区和文件系统都
		 * 打出来，方便用户修正 root= / rootfstype=。
		 */
		printk("VFS: Cannot open root device \"%s\" or %s: error %d\n",
				pretty_name, b, err);
		printk("Please append a correct \"root=\" boot option; here are the available partitions:\n");
		printk_all_partitions();

		if (root_fs_names)
			num_fs = list_bdev_fs_names(fs_names, PAGE_SIZE);
		if (!num_fs)
			pr_err("Can't find any bdev filesystem to be used for mount!\n");
		else {
			pr_err("List of all bdev filesystems:\n");
			for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1)
				pr_err(" %s", p);
			pr_err("\n");
		}

		panic("VFS: Unable to mount root fs on %s", b);
	}
	if (!(flags & SB_RDONLY)) {
		flags |= SB_RDONLY;
		goto retry;
	}

	printk("List of all partitions:\n");
	printk_all_partitions();
	printk("No filesystem could mount root, tried: ");
	for (i = 0, p = fs_names; i < num_fs; i++, p += strlen(p)+1)
		printk(" %s", p);
	printk("\n");
	panic("VFS: Unable to mount root fs on \"%s\" or %s", pretty_name, b);
out:
	put_page(page);
}
 
#ifdef CONFIG_ROOT_NFS

#define NFSROOT_TIMEOUT_MIN	5
#define NFSROOT_TIMEOUT_MAX	30
#define NFSROOT_RETRY_MAX	5

static void __init mount_nfs_root(void)
{
	char *root_dev, *root_data;
	unsigned int timeout;
	int try;

	if (nfs_root_data(&root_dev, &root_data))
		goto fail;

	/*
	 * 网络根文件系统的特点是“设备存在”不代表“服务端已可用”，因此这里做带退避
	 * 的重试，而不是像本地块设备那样一次失败就立即 panic。
	 */
	timeout = NFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		if (!do_mount_root(root_dev, "nfs", root_mountflags, root_data))
			return;
		if (try > NFSROOT_RETRY_MAX)
			break;

		/* Wait, in case the server refused us immediately */
		ssleep(timeout);
		timeout <<= 1;
		if (timeout > NFSROOT_TIMEOUT_MAX)
			timeout = NFSROOT_TIMEOUT_MAX;
	}
fail:
	pr_err("VFS: Unable to mount root fs via NFS.\n");
}
#else
static inline void mount_nfs_root(void)
{
}
#endif /* CONFIG_ROOT_NFS */

#ifdef CONFIG_CIFS_ROOT

#define CIFSROOT_TIMEOUT_MIN	5
#define CIFSROOT_TIMEOUT_MAX	30
#define CIFSROOT_RETRY_MAX	5

/*
 * SMB/CIFS 网络根文件系统挂载。与 NFS root 类似，需要对远端服务做退避重试——
 * 网络上的 SMB 服务器可能尚未就绪。
 */
static void __init mount_cifs_root(void)
{
	char *root_dev, *root_data;
	unsigned int timeout;
	int try;

	if (cifs_root_data(&root_dev, &root_data))
		goto fail;

	timeout = CIFSROOT_TIMEOUT_MIN;
	for (try = 1; ; try++) {
		if (!do_mount_root(root_dev, "cifs", root_mountflags,
				   root_data))
			return;
		if (try > CIFSROOT_RETRY_MAX)
			break;

		ssleep(timeout);
		timeout <<= 1;
		if (timeout > CIFSROOT_TIMEOUT_MAX)
			timeout = CIFSROOT_TIMEOUT_MAX;
	}
fail:
	pr_err("VFS: Unable to mount root fs via SMB.\n");
}
#else
static inline void mount_cifs_root(void)
{
}
#endif /* CONFIG_CIFS_ROOT */

static bool __init fs_is_nodev(char *fstype)
{
	struct file_system_type *fs = get_fs_type(fstype);
	bool ret = false;

	if (fs) {
		ret = !(fs->fs_flags & FS_REQUIRES_DEV);
		put_filesystem(fs);
	}

	return ret;
}

/*
 * 尝试用 rootfstype= 中指定的 nodev 文件系统去挂载根。
 *
 * nodev 文件系统（如 tmpfs、ramfs、overlayfs 的下层）不需要底层块设备，
 * 只要文件系统驱动本身已编译进内核就可以直接挂载。
 * 传统嵌入式场景里，mtd/ubi 根也走类似路径：Root_Generic + mount_root_generic()。
 */
static int __init mount_nodev_root(char *root_device_name)
{
	char *fs_names, *fstype;
	int err = -EINVAL;
	int num_fs, i;

	/*
	 * 对 mtd/ubi 等没有传统块设备节点的场景，rootfstype= 里可能出现 nodev
	 * 文件系统。这里仅在用户显式指定根类型时尝试这些不要求底层设备节点的实现。
	 */
	fs_names = (void *)__get_free_page(GFP_KERNEL);
	if (!fs_names)
		return -EINVAL;
	num_fs = split_fs_names(fs_names, PAGE_SIZE);

	for (i = 0, fstype = fs_names; i < num_fs;
	     i++, fstype += strlen(fstype) + 1) {
		if (!*fstype)
			continue;
		if (!fs_is_nodev(fstype))
			continue;
		err = do_mount_root(root_device_name, fstype, root_mountflags,
				    root_mount_data);
		if (!err)
			break;
	}

	free_page((unsigned long)fs_names);
	return err;
}

#ifdef CONFIG_BLOCK
/*
 * 传统块设备根文件系统挂载路径。
 *
 * 先创建 /dev/root 设备节点，指向 root= 解析出的真实块设备。
 * 然后交给 mount_root_generic() 去按 rootfstype= 逐一尝试挂载各个候选文件系统。
 */
static void __init mount_block_root(char *root_device_name)
{
	int err = create_dev("/dev/root", ROOT_DEV);

	if (err < 0)
		pr_emerg("Failed to create /dev/root: %d\n", err);
	mount_root_generic("/dev/root", root_device_name, root_mountflags);
}
#else
static inline void mount_block_root(char *root_device_name)
{
}
#endif /* CONFIG_BLOCK */

void __init mount_root(char *root_device_name)
{
	/*
	 * ROOT_DEV 的值把“根文件系统来源”粗分成几类：
	 * - Root_NFS / Root_CIFS: 网络根
	 * - Root_Generic: 交给具体文件系统自己解释名字，如 mtd/ubi
	 * - 0: 尚未解析到具体设备，可能走 nodev 或后续块设备路径
	 * - 其他 dev_t: 传统块设备根文件系统
	 */
	switch (ROOT_DEV) {
	case Root_NFS:
		mount_nfs_root();
		break;
	case Root_CIFS:
		mount_cifs_root();
		break;
	case Root_Generic:
		mount_root_generic(root_device_name, root_device_name,
				   root_mountflags);
		break;
	case 0:
		if (root_device_name && root_fs_names &&
		    mount_nodev_root(root_device_name) == 0)
			break;
		fallthrough;
	default:
		mount_block_root(root_device_name);
		break;
	}
}

/*
 * rootwait 不是盲等一段固定时间，而是等待两件事：
 * - 异步驱动探测基本收敛
 * - root= 指向的设备可以被 early_lookup_bdev() 解析到
 */
static void __init wait_for_root(char *root_device_name)
{
	ktime_t end;

	if (ROOT_DEV != 0)
		return;

	pr_info("Waiting for root device %s...\n", root_device_name);

	end = ktime_add_ms(ktime_get_raw(), root_wait);

	while (!driver_probe_done() ||
	       early_lookup_bdev(root_device_name, &ROOT_DEV) < 0) {
		msleep(5);
		if (root_wait > 0 && ktime_after(ktime_get_raw(), end))
			break;
	}

	async_synchronize_full();

}

static dev_t __init parse_root_device(char *root_device_name)
{
	int error;
	dev_t dev;

	/*
	 * 某些根文件系统不是经典块设备路径：
	 * - mtd/ubi 交给具体文件系统处理
	 * - /dev/nfs、/dev/cifs 走网络挂载分支
	 * - /dev/ram 指向 RAM disk
	 */
	if (!strncmp(root_device_name, "mtd", 3) ||
	    !strncmp(root_device_name, "ubi", 3))
		return Root_Generic;
	if (strcmp(root_device_name, "/dev/nfs") == 0)
		return Root_NFS;
	if (strcmp(root_device_name, "/dev/cifs") == 0)
		return Root_CIFS;
	if (strcmp(root_device_name, "/dev/ram") == 0)
		return Root_RAM0;

	error = early_lookup_bdev(root_device_name, &dev);
	if (error) {
		/* rootwait 只对“设备还没出现”有意义，对非法 root= 参数没有意义。 */
		if (error == -EINVAL && root_wait) {
			pr_err("Disabling rootwait; root= is invalid.\n");
			root_wait = 0;
		}
		return 0;
	}
	return dev;
}

/*
 * prepare_namespace() 是“从早期 rootfs 切到真正根文件系统”的主入口。
 * 它做的事情按顺序看是：
 * 1. 处理 rootdelay=
 * 2. 等待关键设备探测完成
 * 3. 让 MD/RAID 等早期块设备组装起来
 * 4. 解析 root=
 * 5. 如存在旧式 initrd，则先装载到 /dev/ram
 * 6. 如指定 rootwait，则等待根设备真正出现
 * 7. 挂载真正的根文件系统到 /root
 * 8. 挂 devtmpfs
 * 9. 通过 pivot_root 把当前 rootfs 让位给新根
 */
void __init prepare_namespace(void)
{
	if (root_delay) {
		printk(KERN_INFO "Waiting %d sec before mounting root device...\n",
		       root_delay);
		ssleep(root_delay);
	}

	/*
	 * 先等待“当前已知设备”的探测流程跑完。这个等待点偏保守，代价是某些不相干
	 * 的慢设备也可能把启动时间拉长，但它能显著降低根设备还没注册完成就开始挂载
	 * 的概率。
	 */
	wait_for_device_probe();

	md_run_setup();

	if (saved_root_name[0])
		ROOT_DEV = parse_root_device(saved_root_name);

	initrd_load();

	if (root_wait)
		wait_for_root(saved_root_name);
	mount_root(saved_root_name);
	devtmpfs_mount();

	/*
	 * 到这里，真正的根文件系统已经挂在 /root。接下来通过 pivot_root 把最早期
	 * 的 rootfs 退到旧根位置，再把它卸掉，完成命名空间切换。
	 */
	if (init_pivot_root(".", ".")) {
		pr_err("VFS: Failed to pivot into new rootfs\n");
		return;
	}
	if (init_umount(".", MNT_DETACH)) {
		pr_err("VFS: Failed to unmount old rootfs\n");
		return;
	}
	pr_info("VFS: Pivoted into new rootfs\n");
}

static bool is_tmpfs;
static int rootfs_init_fs_context(struct fs_context *fc)
{
	if (IS_ENABLED(CONFIG_TMPFS) && is_tmpfs)
		return shmem_init_fs_context(fc);

	return ramfs_init_fs_context(fc);
}

struct file_system_type rootfs_fs_type = {
	.name		= "rootfs",
	.init_fs_context = rootfs_init_fs_context,
	.kill_sb	= kill_anon_super,
};

void __init init_rootfs(void)
{
	/*
	 * 没有外部 root= 时，rootfs 本身可以选择落在 tmpfs 上；否则默认就是轻量级
	 * ramfs。这样早期用户空间(/init、initramfs 解包内容等)总有一块可用根。
	 */
	if (IS_ENABLED(CONFIG_TMPFS)) {
		if (!saved_root_name[0] && !root_fs_names)
			is_tmpfs = true;
		else if (root_fs_names && !!strstr(root_fs_names, "tmpfs"))
			is_tmpfs = true;
	}
}
