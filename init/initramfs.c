// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/async.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/dirent.h>
#include <linux/syscalls.h>
#include <linux/utime.h>
#include <linux/file.h>
#include <linux/kstrtox.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/init_syscalls.h>
#include <linux/umh.h>
#include <linux/security.h>
#include <linux/overflow.h>

#include "do_mounts.h"
#include "initramfs_internal.h"

static __initdata bool csum_present;
static __initdata u32 io_csum;

static ssize_t __init xwrite(struct file *file, const unsigned char *p,
		size_t count, loff_t *pos)
{
	ssize_t out = 0;

	/* sys_write only can write MAX_RW_COUNT aka 2G-4K bytes at most */
	while (count) {
		ssize_t rv = kernel_write(file, p, count, pos);

		if (rv < 0) {
			if (rv == -EINTR || rv == -EAGAIN)
				continue;
			return out ? out : rv;
		} else if (rv == 0)
			break;

		if (csum_present) {
			ssize_t i;

			for (i = 0; i < rv; i++)
				io_csum += p[i];
		}

		p += rv;
		out += rv;
		count -= rv;
	}

	return out;
}

static __initdata char *message;
static void __init error(char *x)
{
	if (!message)
		message = x;
}

#define panic_show_mem(fmt, ...) \
	({ show_mem(); panic(fmt, ##__VA_ARGS__); })

/* link hash */

#define N_ALIGN(len) ((((len) + 1) & ~3) + 2)

static __initdata struct hash {
	int ino, minor, major;
	umode_t mode;
	struct hash *next;
	char name[N_ALIGN(PATH_MAX)];
} *head[32];
static __initdata bool hardlink_seen;

static inline int hash(int major, int minor, int ino)
{
	unsigned long tmp = ino + minor + (major << 3);
	tmp += tmp >> 5;
	return tmp & 31;
}

static char __init *find_link(int major, int minor, int ino,
			      umode_t mode, char *name)
{
	struct hash **p, *q;
	for (p = head + hash(major, minor, ino); *p; p = &(*p)->next) {
		if ((*p)->ino != ino)
			continue;
		if ((*p)->minor != minor)
			continue;
		if ((*p)->major != major)
			continue;
		if (((*p)->mode ^ mode) & S_IFMT)
			continue;
		return (*p)->name;
	}
	q = kmalloc_obj(struct hash);
	if (!q)
		panic_show_mem("can't allocate link hash entry");
	q->major = major;
	q->minor = minor;
	q->ino = ino;
	q->mode = mode;
	strscpy(q->name, name);
	q->next = NULL;
	*p = q;
	hardlink_seen = true;
	return NULL;
}

static void __init free_hash(void)
{
	struct hash **p, *q;
	for (p = head; hardlink_seen && p < head + 32; p++) {
		while (*p) {
			q = *p;
			*p = q->next;
			kfree(q);
		}
	}
	hardlink_seen = false;
}

#ifdef CONFIG_INITRAMFS_PRESERVE_MTIME
static void __init do_utime(char *filename, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	init_utimes(filename, t);
}

static void __init do_utime_path(const struct path *path, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	vfs_utimes(path, t);
}

static __initdata LIST_HEAD(dir_list);
struct dir_entry {
	struct list_head list;
	time64_t mtime;
	char name[];
};

static void __init dir_add(const char *name, size_t nlen, time64_t mtime)
{
	struct dir_entry *de;

	de = kmalloc_flex(*de, name, nlen);
	if (!de)
		panic_show_mem("can't allocate dir_entry buffer");
	INIT_LIST_HEAD(&de->list);
	strscpy(de->name, name, nlen);
	de->mtime = mtime;
	list_add(&de->list, &dir_list);
}

static void __init dir_utime(void)
{
	struct dir_entry *de, *tmp;
	list_for_each_entry_safe(de, tmp, &dir_list, list) {
		list_del(&de->list);
		do_utime(de->name, de->mtime);
		kfree(de);
	}
}
#else
static void __init do_utime(char *filename, time64_t mtime) {}
static void __init do_utime_path(const struct path *path, time64_t mtime) {}
static void __init dir_add(const char *name, size_t nlen, time64_t mtime) {}
static void __init dir_utime(void) {}
#endif

static __initdata time64_t mtime;

/*
 * CPIO newc 格式 header 解析。
 *
 * newc header 是 110 字节的 ASCII 十六进制字段：
 *   magic(6) ino(8) mode(8) uid(8) gid(8) nlink(8) mtime(8)
 *   filesize(8) devmajor(8) devminor(8) rdevmajor(8) rdevminor(8)
 *   namesize(8) check(8)
 * 后跟 namesize 字节的文件名（4 字节对齐填充）和 filesize 字节的文件体。
 */
static __initdata unsigned long ino, major, minor, nlink;
static __initdata umode_t mode;
static __initdata unsigned long body_len, name_len;
static __initdata uid_t uid;
static __initdata gid_t gid;
static __initdata unsigned rdev;
static __initdata u32 hdr_csum;

static void __init parse_header(char *s)
{
	unsigned long parsed[13];
	int i;

	for (i = 0, s += 6; i < 13; i++, s += 8)
		parsed[i] = simple_strntoul(s, NULL, 16, 8);

	ino = parsed[0];
	mode = parsed[1];
	uid = parsed[2];
	gid = parsed[3];
	nlink = parsed[4];
	mtime = parsed[5]; /* breaks in y2106 */
	body_len = parsed[6];
	major = parsed[7];
	minor = parsed[8];
	rdev = new_encode_dev(MKDEV(parsed[9], parsed[10]));
	name_len = parsed[11];
	hdr_csum = parsed[12];
}

/*
 * initramfs CPIO 解包状态机。
 *
 * 状态流转（正常文件）：
 *   Start → GotHeader → SkipIt → GotName → CopyFile → Reset → Start...
 *
 * 状态流转（符号链接）：
 *   Start → GotHeader → Collect → GotSymlink → Reset → Start...
 *
 * 每个状态对应一个 do_xxx() 处理函数，在 write_buffer() 的循环中被驱动。
 * 状态机一次只处理一段连续输入，当输入不够时返回让调用方补充更多数据。
 */
static __initdata enum state {
	Start,		/* 等待/跳过 CPIO 条目间填充，读取下一个 header */
	Collect,	/* 收集分散到达的数据到连续缓冲区 */
	GotHeader,	/* 解析 cpio header (magic + 字段) */
	SkipIt,		/* 跳过当前条目的未读部分（如非 REG 文件的 body） */
	GotName,	/* 根据文件名和 mode 创建文件/目录/设备节点 */
	CopyFile,	/* 把 body 内容写入已创建的文件 */
	GotSymlink,	/* 创建符号链接 */
	Reset		/* 跳过条目间 padding，返回 Start */
} state, next_state;

static __initdata char *victim;
static unsigned long byte_count __initdata;
static __initdata loff_t this_header, next_header;

static inline void __init eat(unsigned n)
{
	victim += n;
	this_header += n;
	byte_count -= n;
}

static __initdata char *collected;
static long remains __initdata;
static __initdata char *collect;

static void __init read_into(char *buf, unsigned size, enum state next)
{
	if (byte_count >= size) {
		collected = victim;
		eat(size);
		state = next;
	} else {
		collect = collected = buf;
		remains = size;
		next_state = next;
		state = Collect;
	}
}

static __initdata char *header_buf, *symlink_buf, *name_buf;

static int __init do_start(void)
{
	read_into(header_buf, CPIO_HDRLEN, GotHeader);
	return 0;
}

static int __init do_collect(void)
{
	unsigned long n = remains;
	if (byte_count < n)
		n = byte_count;
	memcpy(collect, victim, n);
	eat(n);
	collect += n;
	if ((remains -= n) != 0)
		return 1;
	state = next_state;
	return 0;
}

/*
 * 状态：GotHeader — 解析 CPIO header，决定下一步走向。
 * - 符号链接：收集 name+symlink target 后转 GotSymlink
 * - 普通文件或无 body：读入文件名后转 GotName
 * - 其他（设备节点等）：先跳至 SkipIt 再自动进入 GotName
 */
static int __init do_header(void)
{
	if (!memcmp(collected, "070701", 6)) {
		csum_present = false;
	} else if (!memcmp(collected, "070702", 6)) {
		csum_present = true;
	} else {
		if (memcmp(collected, "070707", 6) == 0)
			error("incorrect cpio method used: use -H newc option");
		else
			error("no cpio magic");
		return 1;
	}
	parse_header(collected);
	next_header = this_header + N_ALIGN(name_len) + body_len;
	next_header = (next_header + 3) & ~3;
	state = SkipIt;
	if (name_len <= 0 || name_len > PATH_MAX)
		return 0;
	if (S_ISLNK(mode)) {
		if (body_len > PATH_MAX)
			return 0;
		collect = collected = symlink_buf;
		remains = N_ALIGN(name_len) + body_len;
		next_state = GotSymlink;
		state = Collect;
		return 0;
	}
	if (S_ISREG(mode) || !body_len)
		read_into(name_buf, N_ALIGN(name_len), GotName);
	return 0;
}

/*
 * 状态：SkipIt — 跳过当前条目的未读字节，直到到达 next_header 边界。
 */
static int __init do_skip(void)
{
	if (this_header + byte_count < next_header) {
		eat(byte_count);
		return 1;
	} else {
		eat(next_header - this_header);
		state = next_state;
		return 0;
	}
}

/*
 * 状态：Reset — 消耗条目间的 '\0' 填充字节（4 字节对齐 padding），
 * 然后回到 Start 等待下一个 header。
 */
static int __init do_reset(void)
{
	while (byte_count && *victim == '\0')
		eat(1);
	if (byte_count && (this_header & 3))
		error("broken padding");
	return 1;
}

static void __init clean_path(char *path, umode_t fmode)
{
	struct kstat st;

	if (!init_stat(path, &st, AT_SYMLINK_NOFOLLOW) &&
	    (st.mode ^ fmode) & S_IFMT) {
		if (S_ISDIR(st.mode))
			init_rmdir(path);
		else
			init_unlink(path);
	}
}

static int __init maybe_link(void)
{
	if (nlink >= 2) {
		char *old = find_link(major, minor, ino, mode, collected);
		if (old) {
			clean_path(collected, 0);
			return (init_link(old, collected) < 0) ? -1 : 1;
		}
	}
	return 0;
}

static __initdata struct file *wfile;
static __initdata loff_t wfile_pos;

/*
 * 状态：GotName — 根据文件名和 mode 在 rootfs 上创建对应的文件系统对象。
 * - REG: 打开文件准备写入内容 → CopyFile
 * - DIR: mkdir + chown + chmod
 * - BLK/CHR/FIFO/SOCK: mknod + chown + chmod
 * - TRAILER!!!: CPIO 归档结束标记，释放硬链接缓存
 */
static int __init do_name(void)
{
	state = SkipIt;
	next_state = Reset;

	/* name_len > 0 && name_len <= PATH_MAX checked in do_header */
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs name without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}

	if (strcmp(collected, "TRAILER!!!") == 0) {
		free_hash();
		return 0;
	}
	clean_path(collected, mode);
	if (S_ISREG(mode)) {
		int ml = maybe_link();
		if (ml >= 0) {
			int openflags = O_WRONLY|O_CREAT|O_LARGEFILE;
			if (ml != 1)
				openflags |= O_TRUNC;
			wfile = filp_open(collected, openflags, mode);
			if (IS_ERR(wfile))
				return 0;
			wfile_pos = 0;
			io_csum = 0;

			vfs_fchown(wfile, uid, gid);
			vfs_fchmod(wfile, mode);
			if (body_len)
				vfs_truncate(&wfile->f_path, body_len);
			state = CopyFile;
		}
	} else if (S_ISDIR(mode)) {
		init_mkdir(collected, mode);
		init_chown(collected, uid, gid, 0);
		init_chmod(collected, mode);
		dir_add(collected, name_len, mtime);
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (maybe_link() == 0) {
			init_mknod(collected, mode, rdev);
			init_chown(collected, uid, gid, 0);
			init_chmod(collected, mode);
			do_utime(collected, mtime);
		}
	}
	return 0;
}

/*
 * 状态：CopyFile — 把 body 内容写入由 do_name() 打开的文件。
 * 支持跨多次回调的分段写入（输入缓冲区可能装不下整个 body）。
 */
static int __init do_copy(void)
{
	if (byte_count >= body_len) {
		if (xwrite(wfile, victim, body_len, &wfile_pos) != body_len)
			error("write error");

		do_utime_path(&wfile->f_path, mtime);
		fput(wfile);
		if (csum_present && io_csum != hdr_csum)
			error("bad data checksum");
		eat(body_len);
		state = SkipIt;
		return 0;
	} else {
		if (xwrite(wfile, victim, byte_count, &wfile_pos) != byte_count)
			error("write error");
		body_len -= byte_count;
		eat(byte_count);
		return 1;
	}
}

/*
 * 状态：GotSymlink — 创建符号链接。
 */
static int __init do_symlink(void)
{
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs symlink without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}
	collected[N_ALIGN(name_len) + body_len] = '\0';
	clean_path(collected, 0);
	init_symlink(collected + N_ALIGN(name_len), collected);
	init_chown(collected, uid, gid, AT_SYMLINK_NOFOLLOW);
	do_utime(collected, mtime);
	state = SkipIt;
	next_state = Reset;
	return 0;
}

static __initdata int (*actions[])(void) = {
	[Start]		= do_start,
	[Collect]	= do_collect,
	[GotHeader]	= do_header,
	[SkipIt]	= do_skip,
	[GotName]	= do_name,
	[CopyFile]	= do_copy,
	[GotSymlink]	= do_symlink,
	[Reset]		= do_reset,
};

/*
 * 向状态机喂入一段输入数据，然后驱动状态循环直到数据耗尽或回到等待态。
 *
 * 返回值：实际消费的字节数。若返回值 < len，则状态机需要更多数据才能继续。
 */
static long __init write_buffer(char *buf, unsigned long len)
{
	byte_count = len;
	victim = buf;

	while (!actions[state]())
		;
	return len - byte_count;
}

/*
 * 解压回调：解压库每产生一块输出就回调这里，把解压后数据喂入 CPIO 状态机。
 */
static long __init flush_buffer(void *bufv, unsigned long len)
{
	char *buf = bufv;
	long written;
	long origLen = len;
	if (message)
		return -1;
	while ((written = write_buffer(buf, len)) < len && !message) {
		char c = buf[written];
		if (c == '0') {
			buf += written;
			len -= written;
			state = Start;
		} else if (c == 0) {
			buf += written;
			len -= written;
			state = Reset;
		} else
			error("junk within compressed archive");
	}
	return origLen;
}

static unsigned long my_inptr __initdata; /* index of next byte to be processed in inbuf */

#include <linux/decompress/generic.h>

/**
 * unpack_to_rootfs - 解压并提取 initramfs 归档到 rootfs。
 * @buf: initramfs 归档数据起始地址
 * @len: 数据长度
 *
 * 该函数支持两种输入格式：
 * 1. 纯 CPIO 归档（无压缩）：'0707...' header 直接在 buf 开头
 * 2. 压缩 CPIO 归档：先用 decompress_method() 检测压缩格式，解压后再喂入状态机
 *
 * 内部是一个由 write_buffer() 驱动的有限状态机，逐个解析 cpio 条目
 * 并在 rootfs 上创建对应的文件、目录、设备节点和符号链接。
 *
 * Returns: NULL 表示成功，非 NULL 为错误消息字符串。
 */
char * __init unpack_to_rootfs(char *buf, unsigned long len)
{
	long written;
	decompress_fn decompress;
	const char *compress_name;
	struct {
		char header[CPIO_HDRLEN];
		char symlink[PATH_MAX + N_ALIGN(PATH_MAX) + 1];
		char name[N_ALIGN(PATH_MAX)];
	} *bufs = kmalloc_obj(*bufs);

	if (!bufs)
		panic_show_mem("can't allocate buffers");

	header_buf = bufs->header;
	symlink_buf = bufs->symlink;
	name_buf = bufs->name;

	state = Start;
	this_header = 0;
	message = NULL;
	while (!message && len) {
		loff_t saved_offset = this_header;
		if (*buf == '0' && !(this_header & 3)) {
			state = Start;
			written = write_buffer(buf, len);
			buf += written;
			len -= written;
			continue;
		}
		if (!*buf) {
			buf++;
			len--;
			this_header++;
			continue;
		}
		this_header = 0;
		decompress = decompress_method(buf, len, &compress_name);
		pr_debug("Detected %s compressed data\n", compress_name);
		if (decompress) {
			int res = decompress(buf, len, NULL, flush_buffer, NULL,
				   &my_inptr, error);
			if (res)
				error("decompressor failed");
		} else if (compress_name) {
			pr_err("compression method %s not configured\n",
			       compress_name);
			error("decompressor failed");
		} else
			error("invalid magic at start of compressed archive");
		if (state != Reset)
			error("junk at the end of compressed archive");
		this_header = saved_offset + my_inptr;
		buf += my_inptr;
		len -= my_inptr;
	}
	dir_utime();
	/* free any hardlink state collected without optional TRAILER!!! */
	free_hash();
	kfree(bufs);
	return message;
}

static int __initdata do_retain_initrd;

static int __init retain_initrd_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initrd = 1;
	return 1;
}
__setup("retain_initrd", retain_initrd_param);

#ifdef CONFIG_ARCH_HAS_KEEPINITRD
static int __init keepinitrd_setup(char *__unused)
{
	do_retain_initrd = 1;
	return 1;
}
__setup("keepinitrd", keepinitrd_setup);
#endif

static bool __initdata initramfs_async = true;
static int __init initramfs_async_setup(char *str)
{
	return kstrtobool(str, &initramfs_async) == 0;
}
__setup("initramfs_async=", initramfs_async_setup);

extern char __initramfs_start[];
extern unsigned long __initramfs_size;
#include <linux/initrd.h>
#include <linux/kexec.h>

static BIN_ATTR(initrd, 0440, sysfs_bin_attr_simple_read, NULL, 0);

/*
 * 在 memblock 中预留外部 initrd 的物理内存区域。
 *
 * 调用时机：在 arm64_memblock_init() 已计算出线性映射基址之后，
 * 由 bootmem_init() → early_init_fdt_scan_reserved_mem() 间接触发。
 * 预留后算出 initrd 的虚拟地址（initrd_start/end），供后续解包使用。
 */
void __init reserve_initrd_mem(void)
{
	phys_addr_t start;
	unsigned long size;

	/* Ignore the virtual address computed during device tree parsing */
	initrd_start = initrd_end = 0;

	if (!phys_initrd_size)
		return;
	/*
	 * Round the memory region to page boundaries as per free_initrd_mem()
	 * This allows us to detect whether the pages overlapping the initrd
	 * are in use, but more importantly, reserves the entire set of pages
	 * as we don't want these pages allocated for other purposes.
	 */
	start = round_down(phys_initrd_start, PAGE_SIZE);
	size = phys_initrd_size + (phys_initrd_start - start);
	size = round_up(size, PAGE_SIZE);

	if (!memblock_is_region_memory(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx is not a memory region",
		       (u64)start, size);
		goto disable;
	}

	if (memblock_is_region_reserved(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx overlaps in-use memory region\n",
		       (u64)start, size);
		goto disable;
	}

	memblock_reserve(start, size);
	/* Now convert initrd to virtual addresses */
	initrd_start = (unsigned long)__va(phys_initrd_start);
	initrd_end = initrd_start + phys_initrd_size;
	initrd_below_start_ok = 1;

	return;
disable:
	pr_cont(" - disabling initrd\n");
	initrd_start = 0;
	initrd_end = 0;
}

void __weak __init free_initrd_mem(unsigned long start, unsigned long end)
{
	free_reserved_area((void *)start, (void *)end, POISON_FREE_INITMEM,
			"initrd");
}

#ifdef CONFIG_CRASH_RESERVE
static bool __init kexec_free_initrd(void)
{
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end   = (unsigned long)__va(crashk_res.end);

	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (initrd_start >= crashk_end || initrd_end <= crashk_start)
		return false;

	/*
	 * Initialize initrd memory region since the kexec boot does not do.
	 */
	memset((void *)initrd_start, 0, initrd_end - initrd_start);
	if (initrd_start < crashk_start)
		free_initrd_mem(initrd_start, crashk_start);
	if (initrd_end > crashk_end)
		free_initrd_mem(crashk_end, initrd_end);
	return true;
}
#else
static inline bool kexec_free_initrd(void)
{
	return false;
}
#endif /* CONFIG_KEXEC_CORE */

#ifdef CONFIG_BLK_DEV_RAM
static void __init populate_initrd_image(char *err)
{
	ssize_t written;
	struct file *file;
	loff_t pos = 0;

	printk(KERN_INFO "rootfs image is not initramfs (%s); looks like an initrd\n",
			err);
	file = filp_open("/initrd.image", O_WRONLY|O_CREAT|O_LARGEFILE, 0700);
	if (IS_ERR(file))
		return;

	written = xwrite(file, (char *)initrd_start, initrd_end - initrd_start,
			&pos);
	if (written != initrd_end - initrd_start)
		pr_err("/initrd.image: incomplete write (%zd != %ld)\n",
		       written, initrd_end - initrd_start);
	fput(file);
}
#endif /* CONFIG_BLK_DEV_RAM */

/*
 * initramfs/initrd 解包的主入口（异步执行）。
 *
 * 执行顺序：
 * 1. 先解包编译期内嵌的 __initramfs_start..__initramfs_size（内建 initramfs）
 * 2. 如果存在外部 initrd（bootloader 传入），再解包它
 * 3. 如果外部 initrd 不是 CPIO 归档而是文件系统镜像（如 ext2），则把它作为
 *    /initrd.image 保留，供后续旧式 initrd 路径使用
 * 4. 做完后释放 initrd 占用的物理内存，或根据 retain_initrd 保留在 sysfs
 */
static void __init do_populate_rootfs(void *unused, async_cookie_t cookie)
{
	/* 第一步：解包编译期内嵌的 initramfs。若失败则直接 panic。 */
	char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
	if (err)
		panic_show_mem("%s", err);

	if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE))
		goto done;

	if (IS_ENABLED(CONFIG_BLK_DEV_RAM))
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");
	else
		printk(KERN_INFO "Unpacking initramfs...\n");

	/* 第二步：尝试把外部 initrd 当作 initramfs 解包。 */
	err = unpack_to_rootfs((char *)initrd_start, initrd_end - initrd_start);
	if (err) {
#ifdef CONFIG_BLK_DEV_RAM
		/* 解包失败时，外部镜像可能是旧式 ramdisk 文件系统镜像。
		 * 把它保存到 /initrd.image，后面的 rd_load_image() 会来处理。 */
		populate_initrd_image(err);
#else
		printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
#endif
	}

done:
	security_initramfs_populated();

	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (!do_retain_initrd && initrd_start && !kexec_free_initrd()) {
		free_initrd_mem(initrd_start, initrd_end);
	} else if (do_retain_initrd && initrd_start) {
		bin_attr_initrd.size = initrd_end - initrd_start;
		bin_attr_initrd.private = (void *)initrd_start;
		if (sysfs_create_bin_file(firmware_kobj, &bin_attr_initrd))
			pr_err("Failed to create initrd sysfs file");
	}
	initrd_start = 0;
	initrd_end = 0;

	init_flush_fput();
}

static ASYNC_DOMAIN_EXCLUSIVE(initramfs_domain);
static async_cookie_t initramfs_cookie;

/*
 * 等待 initramfs 解包完成。
 *
 * 由 kernel_init_freeable() 调用，确保在执行 rdinit= 或 prepare_namespace()
 * 之前 rootfs 上已经有完整的 early userspace 内容。
 */
void wait_for_initramfs(void)
{
	if (!initramfs_cookie) {
		/*
		 * Something before rootfs_initcall wants to access
		 * the filesystem/initramfs. Probably a bug. Make a
		 * note, avoid deadlocking the machine, and let the
		 * caller's access fail as it used to.
		 */
		pr_warn_once("wait_for_initramfs() called before rootfs_initcalls\n");
		return;
	}
	async_synchronize_cookie_domain(initramfs_cookie + 1, &initramfs_domain);
}
EXPORT_SYMBOL_GPL(wait_for_initramfs);

/*
 * rootfs_initcall 入口：异步启动 initramfs 解包。
 *
 * 选择异步执行的目的是让内核初始化主线不阻塞在解包上。
 * 若 initramfs_async=0，则同步等待解包完成。
 * 解包完成后，usermodehelper 才真正可用（某些 initramfs 内容可能依赖它）。
 */
static int __init populate_rootfs(void)
{
	initramfs_cookie = async_schedule_domain(do_populate_rootfs, NULL,
						 &initramfs_domain);
	usermodehelper_enable();
	if (!initramfs_async)
		wait_for_initramfs();
	return 0;
}
rootfs_initcall(populate_rootfs);
