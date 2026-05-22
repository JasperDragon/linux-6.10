// SPDX-License-Identifier: GPL-2.0
#include <linux/unistd.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/initrd.h>

#include "do_mounts.h"

unsigned long initrd_start, initrd_end;
int initrd_below_start_ok;
static int __initdata mount_initrd = 1;

phys_addr_t phys_initrd_start __initdata;
unsigned long phys_initrd_size __initdata;

static int __init no_initrd(char *str)
{
	pr_warn("noinitrd option is deprecated and will be removed soon\n");
	mount_initrd = 0;
	return 1;
}

__setup("noinitrd", no_initrd);

static int __init early_initrdmem(char *p)
{
	phys_addr_t start;
	unsigned long size;
	char *endp;

	/*
	 * initrdmem=/initrd= 允许 bootloader 直接用“物理起始地址,长度”的方式告诉
	 * 内核旧式 initrd 放在哪里。这和内建 initramfs 是两条完全不同的链路。
	 */
	start = memparse(p, &endp);
	if (*endp == ',') {
		size = memparse(endp + 1, NULL);

		phys_initrd_start = start;
		phys_initrd_size = size;
	}
	return 0;
}
early_param("initrdmem", early_initrdmem);

static int __init early_initrd(char *p)
{
	return early_initrdmem(p);
}
early_param("initrd", early_initrd);

void __init initrd_load(void)
{
	if (mount_initrd) {
		create_dev("/dev/ram", Root_RAM0);
		/*
		 * 旧式 initrd 的语义是“先把镜像搬进 /dev/ram0，再把 /dev/ram0 当作
		 * 根设备去挂载”。它不是现代 initramfs 那种直接解包到 rootfs。
		 */
		if (rd_load_image()) {
			pr_warn("using deprecated initrd support, will be removed in January 2027; "
				"use initramfs instead or (as a last resort) /sys/firmware/initrd; "
				"see section \"Workaround\" in "
				"https://lore.kernel.org/lkml/20251010094047.3111495-1-safinaskar@gmail.com\n");
		}
	}
	/* 无论是否成功装载，临时暴露的原始镜像文件都不再保留。 */
	init_unlink("/initrd.image");
}
