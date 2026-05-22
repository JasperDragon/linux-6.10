// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/ext2_fs.h>
#include <linux/romfs_fs.h>
#include <uapi/linux/cramfs_fs.h>
#include <linux/initrd.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/slab.h>

#include "do_mounts.h"
#include "../fs/squashfs/squashfs_fs.h"

#include <linux/decompress/generic.h>

static struct file *in_file, *out_file;
static loff_t in_pos, out_pos;

int __initdata rd_image_start;		/* starting block # of image */

static int __init ramdisk_start_setup(char *str)
{
	pr_warn("ramdisk_start= option is deprecated and will be removed soon\n");
	return kstrtoint(str, 0, &rd_image_start) == 0;
}
__setup("ramdisk_start=", ramdisk_start_setup);

static int __init crd_load(decompress_fn deco);

/*
 * 识别 /initrd.image 里的镜像类型。
 *
 * 返回值语义：
 * - >0: 非压缩文件系统镜像，返回需要拷贝到 ramdisk 的 KiB 数
 * -  0: 压缩镜像，需要走解压路径
 * - -1: 没识别出合法 RAM disk 镜像
 *
 * 这里按若干已知 magic number/压缩头去猜测镜像格式，而不是依赖外部元数据。
 */
static int __init
identify_ramdisk_image(struct file *file, loff_t pos,
		decompress_fn *decompressor)
{
	const int size = 512;
	struct minix_super_block *minixsb;
	struct romfs_super_block *romfsb;
	struct cramfs_super *cramfsb;
	struct squashfs_super_block *squashfsb;
	int nblocks = -1;
	unsigned char *buf;
	const char *compress_name;
	unsigned long n;
	int start_block = rd_image_start;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	minixsb = (struct minix_super_block *) buf;
	romfsb = (struct romfs_super_block *) buf;
	cramfsb = (struct cramfs_super *) buf;
	squashfsb = (struct squashfs_super_block *) buf;
	memset(buf, 0xe5, size);

	/* 先看 block 0：压缩格式、romfs、cramfs、squashfs 都可能直接从这里起。 */
	pos = start_block * BLOCK_SIZE;
	kernel_read(file, buf, size, &pos);

	*decompressor = decompress_method(buf, size, &compress_name);
	if (compress_name) {
		printk(KERN_NOTICE "RAMDISK: %s image found at block %d\n",
		       compress_name, start_block);
		if (!*decompressor)
			printk(KERN_EMERG
			       "RAMDISK: %s decompressor not configured!\n",
			       compress_name);
		nblocks = 0;
		goto done;
	}

	/* romfs 也可能直接从 block 0 开始。 */
	if (romfsb->word0 == ROMSB_WORD0 &&
	    romfsb->word1 == ROMSB_WORD1) {
		printk(KERN_NOTICE
		       "RAMDISK: romfs filesystem found at block %d\n",
		       start_block);
		nblocks = (ntohl(romfsb->size)+BLOCK_SIZE-1)>>BLOCK_SIZE_BITS;
		goto done;
	}

	if (cramfsb->magic == CRAMFS_MAGIC) {
		printk(KERN_NOTICE
		       "RAMDISK: cramfs filesystem found at block %d\n",
		       start_block);
		nblocks = (cramfsb->size + BLOCK_SIZE - 1) >> BLOCK_SIZE_BITS;
		goto done;
	}

	/* squashfs 同样可能直接落在 block 0。 */
	if (le32_to_cpu(squashfsb->s_magic) == SQUASHFS_MAGIC) {
		printk(KERN_NOTICE
		       "RAMDISK: squashfs filesystem found at block %d\n",
		       start_block);
		nblocks = (le64_to_cpu(squashfsb->bytes_used) + BLOCK_SIZE - 1)
			 >> BLOCK_SIZE_BITS;
		goto done;
	}

	/* 有些 cramfs 镜像前面会补 512B 头，这里再向后探一次。 */
	pos = start_block * BLOCK_SIZE + 0x200;
	kernel_read(file, buf, size, &pos);

	if (cramfsb->magic == CRAMFS_MAGIC) {
		printk(KERN_NOTICE
		       "RAMDISK: cramfs filesystem found at block %d\n",
		       start_block);
		nblocks = (cramfsb->size + BLOCK_SIZE - 1) >> BLOCK_SIZE_BITS;
		goto done;
	}

	/* minix/ext2 的 superblock 不在 block 0，而在更靠后的位置。 */
	pos = (start_block + 1) * BLOCK_SIZE;
	kernel_read(file, buf, size, &pos);

	/* Try minix */
	if (minixsb->s_magic == MINIX_SUPER_MAGIC ||
	    minixsb->s_magic == MINIX_SUPER_MAGIC2) {
		printk(KERN_NOTICE
		       "RAMDISK: Minix filesystem found at block %d\n",
		       start_block);
		nblocks = minixsb->s_nzones << minixsb->s_log_zone_size;
		goto done;
	}

	/* Try ext2 */
	n = ext2_image_size(buf);
	if (n) {
		printk(KERN_NOTICE
		       "RAMDISK: ext2 filesystem found at block %d\n",
		       start_block);
		nblocks = n;
		goto done;
	}

	printk(KERN_NOTICE
	       "RAMDISK: Couldn't find valid RAM disk image starting at %d.\n",
	       start_block);

done:
	kfree(buf);
	return nblocks;
}

static unsigned long nr_blocks(struct file *file)
{
	struct inode *inode = file->f_mapping->host;

	if (!S_ISBLK(inode->i_mode))
		return 0;
	return i_size_read(inode) >> 10;
}

/*
 * 旧式 initrd 的核心加载函数。
 *
 * 路径分为两条：
 * 1. 压缩镜像 (nblocks==0): 交给 crd_load() 边读边解压到 /dev/ram
 * 2. 非压缩文件系统镜像 (nblocks>0): 直接从 /initrd.image 逐块复制到 /dev/ram
 *
 * 返回值：1 表示加载成功，0 表示失败。
 */
int __init rd_load_image(void)
{
	int res = 0;
	unsigned long rd_blocks, devblocks, nr_disks;
	int nblocks, i;
	char *buf = NULL;
	unsigned short rotate = 0;
	decompress_fn decompressor = NULL;
	char rotator[4] = { '|' , '/' , '-' , '\\' };

	/* 打开 /dev/ram 作为输出目标，即 ramdisk 设备。 */
	out_file = filp_open("/dev/ram", O_RDWR, 0);
	if (IS_ERR(out_file))
		goto out;

	/* 打开 /initrd.image 作为输入源（由 initrd_load() 预先创建）。 */
	in_file = filp_open("/initrd.image", O_RDONLY, 0);
	if (IS_ERR(in_file))
		goto noclose_input;

	/* 先识别镜像类型：压缩/文件系统/unrecognized。 */
	in_pos = rd_image_start * BLOCK_SIZE;
	nblocks = identify_ramdisk_image(in_file, in_pos, &decompressor);
	if (nblocks < 0)
		goto done;

	/* nblocks==0 表示压缩镜像，走解压路径。 */
	if (nblocks == 0) {
		if (crd_load(decompressor) == 0)
			goto successful_load;
		goto done;
	}

	/*
	 * 历史包袱：这里的 nblocks 实际上不是块数，而是需要装入 ramdisk 的 KiB 数。
	 */
	rd_blocks = nr_blocks(out_file);
	if (nblocks > rd_blocks) {
		printk("RAMDISK: image too big! (%dKiB/%ldKiB)\n",
		       nblocks, rd_blocks);
		goto done;
	}

	/*
	 * 非压缩镜像路径：直接把镜像逐块复制到 /dev/ram。
	 * 注意它可能是多卷格式——第一块拷贝完后立即结束（旧式 ext2 disk 1），
	 * 不会继续读取 input。
	 */
	devblocks = nblocks;

	if (devblocks == 0) {
		printk(KERN_ERR "RAMDISK: could not determine device size\n");
		goto done;
	}

	buf = kmalloc(BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		printk(KERN_ERR "RAMDISK: could not allocate buffer\n");
		goto done;
	}

	nr_disks = (nblocks - 1) / devblocks + 1;
	pr_notice("RAMDISK: Loading %dKiB [%ld disk%s] into ram disk... ",
		  nblocks, nr_disks, str_plural(nr_disks));
	for (i = 0; i < nblocks; i++) {
		if (i && (i % devblocks == 0)) {
			pr_cont("done disk #1.\n");
			rotate = 0;
			fput(in_file);
			break;
		}
		kernel_read(in_file, buf, BLOCK_SIZE, &in_pos);
		kernel_write(out_file, buf, BLOCK_SIZE, &out_pos);
		if (!IS_ENABLED(CONFIG_S390) && !(i % 16)) {
			pr_cont("%c\b", rotator[rotate & 0x3]);
			rotate++;
		}
	}
	pr_cont("done.\n");

successful_load:
	res = 1;
done:
	fput(in_file);
noclose_input:
	fput(out_file);
out:
	kfree(buf);
	init_unlink("/dev/ram");
	return res;
}

static int exit_code;
static int decompress_error;

/*
 * 压缩解压适配层：compr_fill / compr_flush / error 三个回调。
 *
 * 它们是解压库（gzip/lz4/xz 等）与内核文件 IO 之间的桥接：
 * - compr_fill: 从 /initrd.image 读取压缩数据
 * - compr_flush: 把解压后数据写入 /dev/ram
 * - error: 记录错误并终止解压
 */
static long __init compr_fill(void *buf, unsigned long len)
{
	long r = kernel_read(in_file, buf, len, &in_pos);
	if (r < 0)
		printk(KERN_ERR "RAMDISK: error while reading compressed data");
	else if (r == 0)
		printk(KERN_ERR "RAMDISK: EOF while reading compressed data");
	return r;
}

static long __init compr_flush(void *window, unsigned long outcnt)
{
	long written = kernel_write(out_file, window, outcnt, &out_pos);
	if (written != outcnt) {
		if (decompress_error == 0)
			printk(KERN_ERR
			       "RAMDISK: incomplete write (%ld != %ld)\n",
			       written, outcnt);
		decompress_error = 1;
		return -1;
	}
	return outcnt;
}

static void __init error(char *x)
{
	printk(KERN_ERR "%s\n", x);
	exit_code = 1;
	decompress_error = 1;
}

/*
 * 压缩 ramdisk 解压主函数。
 *
 * 调用方已通过 identify_ramdisk_image() 确定了正确的解压器。
 * 这里把解压器、输入回调 (compr_fill) 和输出回调 (compr_flush) 串起来，
 * 实现"边读边解压边写 /dev/ram"的流式解压。
 */
static int __init crd_load(decompress_fn deco)
{
	int result;

	if (!deco) {
		pr_emerg("Invalid ramdisk decompression routine.  "
			 "Select appropriate config option.\n");
		panic("Could not decompress initial ramdisk image.");
	}

	result = deco(NULL, 0, compr_fill, compr_flush, NULL, NULL, error);
	if (decompress_error)
		result = 1;
	return result;
}
