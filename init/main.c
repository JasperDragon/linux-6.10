// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org>
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/export.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/binfmts.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/acpi.h>
#include <linux/bootconfig.h>
#include <linux/console.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/kprobes.h>
#include <linux/kmsan.h>
#include <linux/ksysfs.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/kfence.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/buildid.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/memcontrol.h>
#include <linux/cgroup.h>
#include <linux/tick.h>
#include <linux/sched/isolation.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/utsname.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/padata.h>
#include <linux/pid_namespace.h>
#include <linux/device/driver.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/init.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/pti.h>
#include <linux/blkdev.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/moduleloader.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/cache.h>
#include <linux/rodata_test.h>
#include <linux/jump_label.h>
#include <linux/kcsan.h>
#include <linux/init_syscalls.h>
#include <linux/stackdepot.h>
#include <linux/randomize_kstack.h>
#include <linux/pidfs.h>
#include <linux/ptdump.h>
#include <linux/time_namespace.h>
#include <linux/unaligned.h>
#include <linux/vdso_datastore.h>
#include <net/net_namespace.h>

#include <asm/io.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#define CREATE_TRACE_POINTS
#include <trace/events/initcall.h>

#include <kunit/test.h>

static int kernel_init(void *);

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line __ro_after_init;
unsigned int saved_command_line_len __ro_after_init;
/* Command line for parameter parsing */
static char *static_command_line;
/* Untouched extra command line */
static char *extra_command_line;
/* Extra init arguments */
static char *extra_init_args;

#ifdef CONFIG_BOOT_CONFIG
/* Is bootconfig on command line? */
static bool bootconfig_found;
static size_t initargs_offs;
#else
# define bootconfig_found false
# define initargs_offs 0
#endif

static char *execute_command;
static char *ramdisk_execute_command = "/init";
static bool __initdata ramdisk_execute_command_set;

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

/*
 * 遍历 __setup_start..__setup_end，尝试用已注册的废弃/常规参数处理器匹配命令行。
 *
 * 返回 true 表示已匹配（即使是已废弃的参数）；返回 false 表示未匹配，调用方应继续
 * 尝试其他处理路径（如 unknown_bootoption 转交 init）。
 */
static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

#ifdef CONFIG_BLK_DEV_INITRD
/*
 * 从 initrd 尾部提取 bootconfig 数据块。
 *
 * bootconfig 数据以 "#BOOTCONFIG\n" 魔数开头，紧接 4 字节 size + 4 字节 checksum，
 * 放在 initrd 的最后。提取成功后把 initrd_end 前移，使这块元数据不会在后续
 * initramfs 解包时被当成 CPIO 条目的一部分。
 */
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	u32 size, csum;
	char *data;
	u8 *hdr;
	int i;

	if (!initrd_end)
		return NULL;

	data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;
	/*
	 * Since Grub may align the size of initrd to 4, we must
	 * check the preceding 3 bytes as well.
	 */
	for (i = 0; i < 4; i++) {
		if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))
			goto found;
		data--;
	}
	return NULL;

found:
	hdr = (u8 *)(data - 8);
	size = get_unaligned_le32(hdr);
	csum = get_unaligned_le32(hdr + 4);

	data = ((void *)hdr) - size;
	if ((unsigned long)data < initrd_start) {
		pr_err("bootconfig size %d is greater than initrd size %ld\n",
			size, initrd_end - initrd_start);
		return NULL;
	}

	if (xbc_calc_checksum(data, size) != csum) {
		pr_err("bootconfig checksum failed\n");
		return NULL;
	}

	/* Remove bootconfig from initramfs/initrd */
	initrd_end = (unsigned long)data;
	if (_size)
		*_size = size;

	return data;
}
#else
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	return NULL;
}
#endif

#ifdef CONFIG_BOOT_CONFIG

static char xbc_namebuf[XBC_KEYLEN_MAX] __initdata;

#define rest(dst, end) ((end) > (dst) ? (end) - (dst) : 0)

static int __init xbc_snprint_cmdline(char *buf, size_t size,
				      struct xbc_node *root)
{
	struct xbc_node *knode, *vnode;
	char *end = buf + size;
	const char *val, *q;
	int ret;

	xbc_node_for_each_key_value(root, knode, val) {
		ret = xbc_node_compose_key_after(root, knode,
					xbc_namebuf, XBC_KEYLEN_MAX);
		if (ret < 0)
			return ret;

		vnode = xbc_node_get_child(knode);
		if (!vnode) {
			ret = snprintf(buf, rest(buf, end), "%s ", xbc_namebuf);
			if (ret < 0)
				return ret;
			buf += ret;
			continue;
		}
		xbc_array_for_each_value(vnode, val) {
			/*
			 * For prettier and more readable /proc/cmdline, only
			 * quote the value when necessary, i.e. when it contains
			 * whitespace.
			 */
			q = strpbrk(val, " \t\r\n") ? "\"" : "";
			ret = snprintf(buf, rest(buf, end), "%s=%s%s%s ",
				       xbc_namebuf, q, val, q);
			if (ret < 0)
				return ret;
			buf += ret;
		}
	}

	return buf - (end - size);
}
#undef rest

/* Make an extra command line under given key word */
static char * __init xbc_make_cmdline(const char *key)
{
	struct xbc_node *root;
	char *new_cmdline;
	int ret, len = 0;

	root = xbc_find_node(key);
	if (!root)
		return NULL;

	/* Count required buffer size */
	len = xbc_snprint_cmdline(NULL, 0, root);
	if (len <= 0)
		return NULL;

	new_cmdline = memblock_alloc(len + 1, SMP_CACHE_BYTES);
	if (!new_cmdline) {
		pr_err("Failed to allocate memory for extra kernel cmdline.\n");
		return NULL;
	}

	ret = xbc_snprint_cmdline(new_cmdline, len + 1, root);
	if (ret < 0 || ret > len) {
		pr_err("Failed to print extra kernel cmdline.\n");
		memblock_free(new_cmdline, len + 1);
		return NULL;
	}

	return new_cmdline;
}

static int __init bootconfig_params(char *param, char *val,
				    const char *unused, void *arg)
{
	if (strcmp(param, "bootconfig") == 0) {
		bootconfig_found = true;
	}
	return 0;
}

static int __init warn_bootconfig(char *str)
{
	/* The 'bootconfig' has been handled by bootconfig_params(). */
	return 0;
}

/*
 * 解析 bootconfig——一种在 initramfs 末尾或内核镜像嵌入区中附加的键值配置。
 *
 * 它是对内核命令行的增强：bootconfig 中的 "kernel.*" 键会合入 extra_command_line，
 * "init.*" 键会合入 extra_init_args，在主命令行解析前被 prepend。
 */
static void __init setup_boot_config(void)
{
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
	const char *msg, *data;
	int pos, ret;
	size_t size;
	char *err;

	/* 先从 initrd 尾部切出 bootconfig 数据；若无，再尝试内嵌 bootconfig。 */
	data = get_boot_config_from_initrd(&size);
	if (!data)
		data = xbc_get_embedded_bootconfig(&size);

	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	err = parse_args("bootconfig", tmp_cmdline, NULL, 0, 0, 0, NULL,
			 bootconfig_params);

	if (IS_ERR(err) || !(bootconfig_found || IS_ENABLED(CONFIG_BOOT_CONFIG_FORCE)))
		return;

	/* parse_args() stops at the next param of '--' and returns an address */
	if (err)
		initargs_offs = err - tmp_cmdline;

	if (!data) {
		/* If user intended to use bootconfig, show an error level message */
		if (bootconfig_found)
			pr_err("'bootconfig' found on command line, but no bootconfig found\n");
		else
			pr_info("No bootconfig data provided, so skipping bootconfig");
		return;
	}

	if (size >= XBC_DATA_MAX) {
		pr_err("bootconfig size %ld greater than max size %d\n",
			(long)size, XBC_DATA_MAX);
		return;
	}

	ret = xbc_init(data, size, &msg, &pos);
	if (ret < 0) {
		if (pos < 0)
			pr_err("Failed to init bootconfig: %s.\n", msg);
		else
			pr_err("Failed to parse bootconfig: %s at %d.\n",
				msg, pos);
	} else {
		xbc_get_info(&ret, NULL);
		pr_info("Load bootconfig: %ld bytes %d nodes\n", (long)size, ret);
		/* keys starting with "kernel." are passed via cmdline */
		extra_command_line = xbc_make_cmdline("kernel");
		/* Also, "init." keys are init arguments */
		extra_init_args = xbc_make_cmdline("init");
	}
	return;
}

static void __init exit_boot_config(void)
{
	xbc_exit();
}

#else	/* !CONFIG_BOOT_CONFIG */

static void __init setup_boot_config(void)
{
	/* Remove bootconfig data from initrd */
	get_boot_config_from_initrd(NULL);
}

static int __init warn_bootconfig(char *str)
{
	pr_warn("WARNING: 'bootconfig' found on the kernel command line but CONFIG_BOOT_CONFIG is not set.\n");
	return 0;
}

#define exit_boot_config()	do {} while (0)

#endif	/* CONFIG_BOOT_CONFIG */

early_param("bootconfig", warn_bootconfig);

bool __init cmdline_has_extra_options(void)
{
	return extra_command_line || extra_init_args;
}

/* Change NUL term back to "=", to make "param" the whole string. */
static void __init repair_env_string(char *param, char *val)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
		} else
			BUG();
	}
}

/*
 * "--" 分隔符之后的所有参数都无条件进入 argv_init[]，不经过内核解析。
 * 这允许用户安全地向 init 传递可能与内核参数同名的选项而不会产生歧义。
 */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * 处理内核无法识别的命令行参数。
 *
 * 核心策略：不认识的参数不报错，而是按规则判断后转交给用户态 init。
 * - sysctl 别名 → 内核内部消化
 * - bootloader 标识符 (BOOT_IMAGE=, kexec) → 静默忽略
 * - 含 '.' 的参数 → 视为未匹配的内核模块参数，忽略
 * - 带 '=' 的参数 → 加入 envp_init[] 作为环境变量传给 init
 * - 不带 '=' 的参数 → 加入 argv_init[] 作为命令行参数传给 init
 *
 * 大量参数超限时设置 panic_later，在 console_init() 之后再统一 panic。
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	size_t len = strlen(param);
	/*
	 * Well-known bootloader identifiers:
	 * 1. LILO/Grub pass "BOOT_IMAGE=...";
	 * 2. kexec/kdump (kexec-tools) pass "kexec".
	 */
	const char *bootloader[] = { "BOOT_IMAGE=", "kexec", NULL };

	/* Handle params aliased to sysctls */
	if (sysctl_is_alias(param))
		return 0;

	repair_env_string(param, val);

	/* Handle bootloader identifier */
	for (int i = 0; bootloader[i]; i++) {
		if (strstarts(param, bootloader[i]))
			return 0;
	}

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strnchr(param, len, '.'))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option → 传给 init 的环境变量 */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], len+1))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option → 传给 init 的命令行参数 */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

/*
 * init= 参数处理。用户显式指定最终 init 程式的路径。
 * 与 rdinit= 的区别：init= 的优先级低于 rdinit=，但如果 init= 指定的
 * 程序执行失败会直接 panic，不再尝试其他候选。
 */
static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

/*
 * rdinit= 参数处理。指定 initramfs 中 early userspace 的入口程序。
 * 这是优先级最高的 init 入口：如果 rdinit= 指定的程序可执行，
 * 后续 block rootfs 上的 /sbin/init 等都不会被尝试。
 */
static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	ramdisk_execute_command_set = true;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * 保存两份命令行副本：
 * - saved_command_line: 原始未改动的命令行，供 /proc/cmdline 和后续参考
 * - static_command_line: setup_arch() 可能就地修改过的命令行，供参数解析使用
 *
 * 同时把 extra_command_line（来自 bootconfig 的 kernel.* 键）和
 * extra_init_args（来自 bootconfig 的 init.* 键/独立的 -- init 参数）
 * 也拼接到合适位置。
 */
static void __init setup_command_line(char *command_line)
{
	size_t len, xlen = 0, ilen = 0;

	if (extra_command_line)
		xlen = strlen(extra_command_line);
	if (extra_init_args) {
		extra_init_args = strim(extra_init_args); /* remove trailing space */
		ilen = strlen(extra_init_args) + 4; /* for " -- " */
	}

	len = xlen + strlen(boot_command_line) + ilen + 1;

	saved_command_line = memblock_alloc_or_panic(len, SMP_CACHE_BYTES);

	len = xlen + strlen(command_line) + 1;

	static_command_line = memblock_alloc_or_panic(len, SMP_CACHE_BYTES);

	if (xlen) {
		/*
		 * We have to put extra_command_line before boot command
		 * lines because there could be dashes (separator of init
		 * command line) in the command lines.
		 */
		strcpy(saved_command_line, extra_command_line);
		strcpy(static_command_line, extra_command_line);
	}
	strcpy(saved_command_line + xlen, boot_command_line);
	strcpy(static_command_line + xlen, command_line);

	if (ilen) {
		/*
		 * Append supplemental init boot args to saved_command_line
		 * so that user can check what command line options passed
		 * to init.
		 * The order should always be
		 * " -- "[bootconfig init-param][cmdline init-param]
		 */
		if (initargs_offs) {
			len = xlen + initargs_offs;
			strcpy(saved_command_line + len, extra_init_args);
			len += ilen - 4;	/* strlen(extra_init_args) */
			strcpy(saved_command_line + len,
				boot_command_line + initargs_offs - 1);
		} else {
			len = strlen(saved_command_line);
			strcpy(saved_command_line + len, " -- ");
			len += 4;
			strcpy(saved_command_line + len, extra_init_args);
		}
	}

	saved_command_line_len = strlen(saved_command_line);
}

/*
 * 这里必须在一个非 __init 函数里完成启动主线的最后收尾。
 * 否则一旦 root thread 和 init thread 之间出现竞争，
 * start_kernel() 所在的 __init 代码段可能会在 boot idle 线程真正进入
 * cpu_idle() 之前，就被 free_initmem() 提前回收。
 *
 * 另外，老版本 gcc 可能错误地把这个函数内联，因此显式加 noinline。
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

static noinline void __ref __noreturn rest_init(void)
{
	struct task_struct *tsk;
	int pid;

	/* 到这里为止，早期单线程初始化主线已经完成。
	 * 接下来开始把系统切入“正常调度 + 内核线程 + idle 线程”三者并存的运行态。
	 */
	rcu_scheduler_starting();
	/*
	 * 必须先创建 init，使其拿到 pid 1。
	 * 但 init 后面又会依赖 kthreadd 来创建更多内核线程，
	 * 所以顺序必须是：先 fork 出 init，再尽快把 kthreadd 建起来，
	 * 否则如果过早调度 init，就可能在它创建 kthread 时出错。
	 */
	pid = user_mode_thread(kernel_init, NULL, CLONE_FS);
	/*
	 * 在 sched_init_smp() 之前，任务迁移语义还不完整。
	 * 因此先把 init 固定在 boot CPU 上，等 SMP 调度拓扑完全建立后，
	 * 再由 sched_init_smp() 重新调整它允许运行的 CPU 集。
	 */
	rcu_read_lock();
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);
	tsk->flags |= PF_NO_SETAFFINITY;
	set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()));
	rcu_read_unlock();

	numa_default_policy();
	pid = kernel_thread(kthreadd, NULL, NULL, CLONE_FS | CLONE_FILES);
	rcu_read_lock();
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	/*
	 * 到这一步才允许打开 might_sleep() 和 smp_processor_id() 的严格检查。
	 * 再早一些的话，kernel_thread() 自身就可能触发误报；
	 * 对 voluntary preempt 场景，init 即使已经被调度，也仍卡在 kthreadd_done 上。
	 */
	system_state = SYSTEM_SCHEDULING;

	complete(&kthreadd_done);

	/*
	 * boot idle 线程必须至少真正 schedule() 一次，
	 * 否则系统虽然把核心线程都建好了，但调度器还没完全“转起来”。
	 */
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if (p->early && parameq(param, p->str)) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
/*
 * 解析 early param——需要在内核启动最早阶段就生效的参数。
 *
 * 与普通内核参数不同，early param 的消费者必须在 parse_early_param() 之前就通过
 * early_param() 宏注册好（而非 __setup()）。典型例子包括 earlyprintk、mem=、
 * earlycon 等必须在大量子系统初始化前就确定行为的选项。
 */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (done)
		return;

	/* All fall through to do_early_param. */
	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

void __init __weak arch_post_acpi_subsys_init(void) { }

void __init __weak smp_setup_processor_id(void)
{
}

void __init __weak smp_prepare_boot_cpu(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

void __init __weak poking_init(void) { }

void __init __weak pgtable_cache_init(void) { }

void __init __weak trap_init(void) { }

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void);
#else
static inline void initcall_debug_enable(void)
{
}
#endif

#ifdef CONFIG_RANDOMIZE_KSTACK_OFFSET
DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,
			   randomize_kstack_offset);
DEFINE_PER_CPU(struct rnd_state, kstack_rnd_state);

static int __init random_kstack_init(void)
{
	prandom_seed_full_state(&kstack_rnd_state);
	return 0;
}
late_initcall(random_kstack_init);

static int __init early_randomize_kstack_offset(char *buf)
{
	int ret;
	bool bool_result;

	ret = kstrtobool(buf, &bool_result);
	if (ret)
		return ret;

	if (bool_result)
		static_branch_enable(&randomize_kstack_offset);
	else
		static_branch_disable(&randomize_kstack_offset);
	return 0;
}
early_param("randomize_kstack_offset", early_randomize_kstack_offset);
#endif

/*
 * 把未被内核识别的、将转交给 init 的参数汇集打印出来。
 * 只在有未知参数且不会立即 panic 时才输出，避免无关噪声。
 */
static void __init print_unknown_bootoptions(void)
{
	char *unknown_options;
	char *end;
	const char *const *p;
	size_t len;

	if (panic_later || (!argv_init[1] && !envp_init[2]))
		return;

	/*
	 * Determine how many options we have to print out, plus a space
	 * before each
	 */
	len = 1; /* null terminator */
	for (p = &argv_init[1]; *p; p++) {
		len++;
		len += strlen(*p);
	}
	for (p = &envp_init[2]; *p; p++) {
		len++;
		len += strlen(*p);
	}

	unknown_options = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!unknown_options) {
		pr_err("%s: Failed to allocate %zu bytes\n",
			__func__, len);
		return;
	}
	end = unknown_options;

	for (p = &argv_init[1]; *p; p++)
		end += sprintf(end, " %s", *p);
	for (p = &envp_init[2]; *p; p++)
		end += sprintf(end, " %s", *p);

	/* Start at unknown_options[1] to skip the initial space */
	pr_notice("Unknown kernel command line parameters \"%s\", will be passed to user space.\n",
		&unknown_options[1]);
	memblock_free(unknown_options, len);
}

/*
 * 把 early_cpu_to_node() 的映射结果刷入每个 possible CPU 的 per-CPU numa_node。
 * 此时 SRAT/DT 的 NUMA 拓扑信息已经可用，但很多 per-CPU 结构还没完全落地，
 * 所以必须尽早把节点分配固化下来。
 */
static void __init early_numa_node_init(void)
{
#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
#ifndef cpu_to_node
	int cpu;

	/* The early_cpu_to_node() should be ready here. */
	for_each_possible_cpu(cpu)
		set_cpu_numa_node(cpu, early_cpu_to_node(cpu));
#endif
#endif
}

#define KERNEL_CMDLINE_PREFIX		"Kernel command line: "
#define KERNEL_CMDLINE_PREFIX_LEN	(sizeof(KERNEL_CMDLINE_PREFIX) - 1)
#define KERNEL_CMDLINE_CONTINUATION	" \\"
#define KERNEL_CMDLINE_CONTINUATION_LEN	(sizeof(KERNEL_CMDLINE_CONTINUATION) - 1)

#define MIN_CMDLINE_LOG_WRAP_IDEAL_LEN	(KERNEL_CMDLINE_PREFIX_LEN + \
					 KERNEL_CMDLINE_CONTINUATION_LEN)
#define CMDLINE_LOG_WRAP_IDEAL_LEN	(CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN > \
					 MIN_CMDLINE_LOG_WRAP_IDEAL_LEN ? \
					 CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN : \
					 MIN_CMDLINE_LOG_WRAP_IDEAL_LEN)

#define IDEAL_CMDLINE_LEN		(CMDLINE_LOG_WRAP_IDEAL_LEN - KERNEL_CMDLINE_PREFIX_LEN)
#define IDEAL_CMDLINE_SPLIT_LEN		(IDEAL_CMDLINE_LEN - KERNEL_CMDLINE_CONTINUATION_LEN)

/**
 * print_kernel_cmdline() - Print the kernel cmdline with wrapping.
 * @cmdline: The cmdline to print.
 *
 * Print the kernel command line, trying to wrap based on the Kconfig knob
 * CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN.
 *
 * Wrapping is based on spaces, ignoring quotes. All lines are prefixed
 * with "Kernel command line: " and lines that are not the last line have
 * a " \" suffix added to them. The prefix and suffix count towards the
 * line length for wrapping purposes. The ideal length will be exceeded
 * if no appropriate place to wrap is found.
 *
 * Example output if CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN is 40:
 *   Kernel command line: loglevel=7 \
 *   Kernel command line: init=/sbin/init \
 *   Kernel command line: root=PARTUUID=8c3efc1a-768b-6642-8d0c-89eb782f19f0/PARTNROFF=1 \
 *   Kernel command line: rootwait ro \
 *   Kernel command line: my_quoted_arg="The \
 *   Kernel command line: quick brown fox \
 *   Kernel command line: jumps over the \
 *   Kernel command line: lazy dog."
 */
static void __init print_kernel_cmdline(const char *cmdline)
{
	size_t len;

	/* Config option of 0 or anything longer than the max disables wrapping */
	if (CONFIG_CMDLINE_LOG_WRAP_IDEAL_LEN == 0 ||
	    IDEAL_CMDLINE_LEN >= COMMAND_LINE_SIZE - 1) {
		pr_notice("%s%s\n", KERNEL_CMDLINE_PREFIX, cmdline);
		return;
	}

	len = strlen(cmdline);
	while (len > IDEAL_CMDLINE_LEN) {
		const char *first_space;
		const char *prev_cutoff;
		const char *cutoff;
		int to_print;
		size_t used;

		/* Find the last ' ' that wouldn't make the line too long */
		prev_cutoff = NULL;
		cutoff = cmdline;
		while (true) {
			cutoff = strchr(cutoff + 1, ' ');
			if (!cutoff || cutoff - cmdline > IDEAL_CMDLINE_SPLIT_LEN)
				break;
			prev_cutoff = cutoff;
		}
		if (prev_cutoff)
			cutoff = prev_cutoff;
		else if (!cutoff)
			break;

		/* Find the beginning and end of the string of spaces */
		first_space = cutoff;
		while (first_space > cmdline && first_space[-1] == ' ')
			first_space--;
		to_print = first_space - cmdline;
		while (*cutoff == ' ')
			cutoff++;
		used = cutoff - cmdline;

		/* If the whole string is used, break and do the final printout */
		if (len == used)
			break;

		if (to_print)
			pr_notice("%s%.*s%s\n", KERNEL_CMDLINE_PREFIX,
				  to_print, cmdline, KERNEL_CMDLINE_CONTINUATION);

		len -= used;
		cmdline += used;
	}
	if (len)
		pr_notice("%s%s\n", KERNEL_CMDLINE_PREFIX, cmdline);
}

asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector
void start_kernel(void)
{
	char *command_line;
	char *after_dashes;

	/* 下面这段是整个内核启动最核心的通用主线：
	 * 从“架构刚跳进 C 代码的早期引导态”，逐步推进到“能进入 rest_init()”
	 * 的完整内核态。
	 */
	set_task_stack_end_magic(&init_task);
	smp_setup_processor_id();
	debug_objects_early_init();
	init_vmlinux_build_id();

	cgroup_init_early();

	/* 在 IRQ、时钟、调度器都还没完全建立前，必须维持本地中断关闭。 */
	local_irq_disable();
	early_boot_irqs_disabled = true;

	/*
	 * 现在仍处于“强制关中断”的早期阶段。
	 * 先完成架构、内存、命令行和调度等必要初始化，稍后再打开中断。
	 */
	boot_cpu_init();
	page_address_init();
	pr_notice("%s", linux_banner);
	setup_arch(&command_line);
	mm_core_init_early();
	/* LSM 和不少早期框架会依赖 static key/static call。 */
	jump_label_init();
	static_call_init();
	early_security_init();
	setup_boot_config();
	setup_command_line(command_line);
	setup_nr_cpu_ids();
	setup_per_cpu_areas();
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */
	early_numa_node_init();
	boot_cpu_hotplug_init();

	print_kernel_cmdline(saved_command_line);
	/* 启动参数本身可能影响 static key，必须尽早解析。 */
	parse_early_param();
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);
	print_unknown_bootoptions();
	if (!IS_ERR_OR_NULL(after_dashes))
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);
	if (extra_init_args)
		parse_args("Setting extra init args", extra_init_args,
			   NULL, 0, -1, -1, NULL, set_init_arg);

	/* 先做与体系结构相关、且尚不依赖完整 timekeeping 的随机源初始化。 */
	random_init_early(command_line);

	/*
	 * 这些步骤会消耗较大的早期 bootmem/memblock 分配，
	 * 必须发生在正式页分配器完全接管之前。
	 */
	setup_log_buf(0);
	vfs_caches_init_early();
	sort_main_extable();
	trap_init();
	mm_core_init();
	maple_tree_init();
	poking_init();
	ftrace_init();

	/* 从这里开始 trace_printk 已经具备基础可用条件。 */
	early_trace_init();

	/*
	 * 在打开任何中断（尤其是时钟中断）之前，必须先把调度器建好。
	 * 完整 SMP 拓扑会在 smp_init() 后补齐，但此时已经需要一个可工作的调度器。
	 */
	sched_init();

	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	radix_tree_init();

	/*
	 * 先建立 housekeeping CPU 语义，再初始化 workqueue，
	 * 这样 unbound workqueue 才能从一开始就避开不适合的 CPU。
	 */
	housekeeping_init();

	/*
	 * 允许早期代码先创建 workqueue、排队或取消 work item。
	 * 但真正执行 work 仍依赖后面 kthreadd/workqueue_init() 之后的线程环境。
	 */
	workqueue_init_early();

	rcu_init();
	kvfree_rcu_init();

	/* 走到这里后，trace event 框架已经可用。 */
	trace_init();

	if (initcall_debug)
		initcall_debug_enable();

	context_tracking_init();
	/* 在正式 IRQ 初始化前，先把部分早期中断结构链起来。 */
	early_irq_init();
	init_IRQ();
	tick_init();
	rcu_init_nohz();
	timers_init();
	srcu_init();
	hrtimers_init();
	softirq_init();
	vdso_setup_data_pages();
	timekeeping_init();
	time_init();

	/* 完整随机数初始化依赖 timekeeping，因此必须放在其后。 */
	random_init();

	/* 这些机制会使用已经完整建立的随机源。 */
	kfence_init();
	boot_init_stack_canary();

	perf_event_init();
	profile_init();
	call_function_init();
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");

	early_boot_irqs_disabled = false;
	local_irq_enable();

	/* 真正开中断后，系统才进入常规“运行态初始化”的后半程。 */
	kmem_cache_init_late();

	/*
	 * 这里把控制台拉起来的时机偏早：
	 * 此时不少总线和设备初始化还没完成，但为了尽早看到错误输出，只能这么做。
	 * 因此 console_init() 必须自己知道当前仍处于“早控制台”阶段。
	 */
	console_init();
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	lockdep_init();

	/*
	 * locking_selftest() 必须在 IRQ 已开启后运行，
	 * 因为它还要自测 hardirq/softirq 开关场景下的锁反转问题。
	 */
	locking_selftest();

#ifdef CONFIG_BLK_DEV_INITRD
	/* 检查 initrd 是否被早期的内核分配意外覆盖。
	 * 如果 initrd 落在 buddy allocator 已经接管的范围 (below min_low_pfn)，
	 * 说明它所在的内存可能已经被复用，继续解包会导致数据损坏。 */
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	/*
	 * 中断开启后，继续补全"完整多核操作系统"所需的基础设施。
	 * 下面这几个仍属于 NUMA/时间/延迟等偏底层初始化。
	 */
	setup_per_cpu_pageset();
	numa_policy_init();
	acpi_early_init();
	if (late_time_init)
		late_time_init();
	sched_clock_init();
	calibrate_delay();

	/* 架构最后的 CPU 收尾：包括 speculative 漏洞缓解的最终 enable 等。 */
	arch_cpu_finalize_init();

	/*
	 * 进程管理基础设施：pid、匿名 vma、线程栈缓存、cred、fork 和进程级缓存
	 * 必须在创建任何内核线程之前全部就绪。
	 */
	pid_idr_init();
	anon_vma_init();
	thread_stack_cache_init();
	cred_init();
	fork_init();
	proc_caches_init();
	/* 命名空间：UTS / time namespace。 */
	uts_ns_init();
	time_ns_init();
	/* 安全框架、key retention 和 debug lock 晚期初始化。 */
	key_init();
	security_init();
	dbg_late_init();
	/*
	 * 网络命名空间 + VFS caches + pagecache。
	 * VFS 初始化完成后，/proc、nsfs、pidfs 这些伪文件系统才能挂载。
	 */
	net_ns_init();
	vfs_caches_init();
	pagecache_init();
	signals_init();
	seq_file_init();
	proc_root_init();
	nsfs_init();
	pidfs_init();
	/*
	 * cgroup 体系：cpuset → memory cgroup → 通用 cgroup。
	 * 此时进程管理已经可用，所以 cgroup 能正常创建控制组并施加限制。
	 */
	cpuset_init();
	mem_cgroup_init();
	cgroup_init();
	/* 进程统计和延迟记账。 */
	taskstats_init_early();
	delayacct_init();

	/* ACPI 子系统最终上线 + 架构补充钩子 + KCSAN 数据竞争检测器。 */
	acpi_subsystem_init();
	arch_post_acpi_subsys_init();
	kcsan_init();

	/* 到这里，系统已经“真正活过来”。
	 * 后续切到非 __init 上下文，开始进入常规线程与用户态过渡阶段。
	 */
	rest_init();

	/*
	 * Avoid stack canaries in callers of boot_init_stack_canary for gcc-10
	 * and older.
	 */
#if !__has_attribute(__no_stack_protector__)
	prevent_tail_call_optimization();
#endif
}

/*
 * 执行所有编译进内核的 C++ 风格构造函数（.ctors 段）。
 * 某些内核子系统用 __attribute__((constructor)) 注册早期回调，需要在这里统一调用。
 * UML 用户模式在普通 ELF 加载时已经跑过构造函数，跳过即可。
 */
static void __init do_ctors(void)
{
/*
 * For UML, the constructors have already been called by the
 * normal setup code as it's just a normal ELF binary, so we
 * cannot do it again - but we do need CONFIG_CONSTRUCTORS
 * even on UML for modules.
 */
#if defined(CONFIG_CONSTRUCTORS) && !defined(CONFIG_UML)
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = memblock_alloc_or_panic(sizeof(*entry),
					       SMP_CACHE_BYTES);
			entry->buf = memblock_alloc_or_panic(strlen(str_entry) + 1,
						    SMP_CACHE_BYTES);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 1;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static __init_or_module void
trace_initcall_start_cb(void *data, initcall_t fn)
{
	ktime_t *calltime = data;

	printk(KERN_DEBUG "calling  %pS @ %i\n", fn, task_pid_nr(current));
	*calltime = ktime_get();
}

static __init_or_module void
trace_initcall_finish_cb(void *data, initcall_t fn, int ret)
{
	ktime_t rettime, *calltime = data;

	rettime = ktime_get();
	printk(KERN_DEBUG "initcall %pS returned %d after %lld usecs\n",
		 fn, ret, (unsigned long long)ktime_us_delta(rettime, *calltime));
}

static __init_or_module void
trace_initcall_level_cb(void *data, const char *level)
{
	printk(KERN_DEBUG "entering initcall level: %s\n", level);
}

static ktime_t initcall_calltime;

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void)
{
	int ret;

	ret = register_trace_initcall_start(trace_initcall_start_cb,
					    &initcall_calltime);
	ret |= register_trace_initcall_finish(trace_initcall_finish_cb,
					      &initcall_calltime);
	ret |= register_trace_initcall_level(trace_initcall_level_cb, NULL);
	WARN(ret, "Failed to register initcall tracepoints\n");
}
# define do_trace_initcall_start	trace_initcall_start
# define do_trace_initcall_finish	trace_initcall_finish
# define do_trace_initcall_level	trace_initcall_level
#else
static inline void do_trace_initcall_start(initcall_t fn)
{
	if (!initcall_debug)
		return;
	trace_initcall_start_cb(&initcall_calltime, fn);
}
static inline void do_trace_initcall_finish(initcall_t fn, int ret)
{
	if (!initcall_debug)
		return;
	trace_initcall_finish_cb(&initcall_calltime, fn, ret);
}
static inline void do_trace_initcall_level(const char *level)
{
	if (!initcall_debug)
		return;
	trace_initcall_level_cb(NULL, level);
}
#endif /* !TRACEPOINTS_ENABLED */

/*
 * 执行单个 initcall 并做运行后状态检查。
 *
 * initcall 最重要的两条纪律：
 * - 返回后必须恢复调用前的 preempt count，不得永久改变抢占状态
 * - 返回后必须开中断，不得把中断关死留给后续代码
 *
 * 任何违反都会在 dmesg 中打出 WARN 并自动修复。
 */
int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64];
	int ret;

	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn();
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);

	add_latent_entropy();
	return ret;
}


static initcall_entry_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static const char *initcall_level_names[] __initdata = {
	"pure",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static int __init ignore_unknown_bootoption(char *param, char *val,
			       const char *unused, void *arg)
{
	return 0;
}

/*
 * 执行某一级的所有 initcall。
 *
 * 在执行该级 initcall 之前，先用该级专属的 level 参数重新解析一遍命令行，
 * 这样编译期通过 __setup() 注册的参数可以按 initcall 级别选择性生效。
 * level < 0 的参数在此级不可见，level > 当前级则留到后续。
 */
static void __init do_initcall_level(int level, char *command_line)
{
	initcall_entry_t *fn;

	parse_args(initcall_level_names[level],
		   command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, ignore_unknown_bootoption);

	do_trace_initcall_level(initcall_level_names[level]);
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static void __init do_initcalls(void)
{
	int level;
	size_t len = saved_command_line_len + 1;
	char *command_line;

	/* initcall 解析器会改写传入的命令行缓冲区，因此每一层都得用一份可修改副本。 */
	command_line = kzalloc(len, GFP_KERNEL);
	if (!command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		/* 每一层 initcall 都重新喂一遍原始命令行，避免上一层解析结果污染下一层。 */
		strcpy(command_line, saved_command_line);
		do_initcall_level(level, command_line);
	}

	kfree(command_line);
}

/*
 * 走到 do_basic_setup() 时，CPU、内存、调度和进程管理都已经可用，
 * 但绝大多数设备和驱动还没真正初始化。
 * 这里才开始进入“设备模型、driver core、构造函数和 initcall”这些实质性工作。
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();
	ksysfs_init();
	driver_init();
	init_irq_proc();
	do_ctors();
	do_initcalls();
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_entry_t *fn;

	/* 这些是比 pure_initcall 还更早的一批 initcall，
	 * 需要在 SMP 完整起来之前执行。
	 */
	do_trace_initcall_level("early");
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static int run_init_process(const char *init_filename)
{
	const char *const *p;

	/*
	 * init 进程并不是“fork 一个现成用户态程序”，而是当前这个内核线程直接
	 * exec 成用户态 1 号进程。也就是说，一旦 kernel_execve() 成功返回到
	 * 用户态，这条内核启动主线就到此结束，不会再继续执行下面的 C 代码。
	 */
	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	pr_debug("  with arguments:\n");
	for (p = argv_init; *p; p++)
		pr_debug("    %s\n", *p);
	pr_debug("  with environment:\n");
	for (p = envp_init; *p; p++)
		pr_debug("    %s\n", *p);
	return kernel_execve(init_filename, argv_init, envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	/*
	 * 对候选 init 路径做“尽力而为”的尝试：
	 * - ENOENT: 文件不存在，静默继续尝试下一个候选
	 * - 其他错误: 文件在，但无法执行，打印明确报错
	 */
	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
bool rodata_enabled __ro_after_init = true;

#ifndef arch_parse_debug_rodata
static inline bool arch_parse_debug_rodata(char *str) { return false; }
#endif

static int __init set_debug_rodata(char *str)
{
	if (arch_parse_debug_rodata(str))
		return 0;

	if (str && !strcmp(str, "on"))
		rodata_enabled = true;
	else if (str && !strcmp(str, "off"))
		rodata_enabled = false;
	else
		pr_warn("Invalid option string for rodata: '%s'\n", str);
	return 0;
}
early_param("rodata", set_debug_rodata);
#endif

/*
 * 内核内存保护收口：在释放完 __init 段后，把 rodata 和内核文本的线性别名设成只读，
 * 并做 W+X 页面检查。这是 kernel_init() 释放 init 内存后紧接着执行的加固步骤。
 */
static void mark_readonly(void)
{
	if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX) && rodata_enabled) {
		/*
		 * load_module() results in W+X mappings, which are cleaned
		 * up with init_free_wq. Let's make sure that queued work is
		 * flushed so that we don't hit false positives looking for
		 * insecure pages which are W+X.
		 */
		flush_module_init_free_work();
		jump_label_init_ro();
		mark_rodata_ro();
		debug_checkwx();
		rodata_test();
	} else if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX)) {
		pr_info("Kernel memory protection disabled.\n");
	} else if (IS_ENABLED(CONFIG_ARCH_HAS_STRICT_KERNEL_RWX)) {
		pr_warn("Kernel memory protection not selected by kernel config.\n");
	} else {
		pr_warn("This architecture does not have kernel memory protection.\n");
	}
}

void __weak free_initmem(void)
{
	free_initmem_default(POISON_FREE_INITMEM);
}

static int __ref kernel_init(void *unused)
{
	int ret;

	/*
	 * init 线程虽然已经创建，但在这里先卡住，
	 * 等 kthreadd 真正就绪后再继续往下执行。
	 */
	wait_for_completion(&kthreadd_done);

	kernel_init_freeable();
	/* 在释放 __init 段之前，必须确保所有异步 __init 工作都已经跑完。 */
	async_synchronize_full();

	system_state = SYSTEM_FREEING_INITMEM;
	kprobe_free_init_mem();
	ftrace_free_init_mem();
	kgdb_free_init_mem();
	exit_boot_config();
	free_initmem();
	mark_readonly();

	/*
	 * 到这里内核映射已经定型，再去同步用户态页表侧的 PTI 最终状态。
	 */
	pti_finalize();

	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	/* 到这里才切换到常规运行态，允许用户通过内核参数进一步修改 sysctl。 */
	rcu_end_inkernel_boot();

	do_sysctl_args();

	if (ramdisk_execute_command) {
		/*
		 * rdinit= 是“早期用户空间接管”入口，优先级最高。它通常用于 initramfs
		 * 里的 /init，成功后不会再走真实根文件系统上的 /sbin/init 链路。
		 */
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	/*
	 * 后面这串 init 入口按顺序逐个尝试，直到某一个成功。
	 * 最后的 /bin/sh 是为“系统已经严重损坏但仍想抢救”这种场景保底。
	 */
	if (execute_command) {
		/* init= 是用户显式指定的最终 init，若失败则直接 panic，不再猜测。 */
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}

	if (CONFIG_DEFAULT_INIT[0] != '\0') {
		/* Kconfig 默认 init 是发行版/裁剪系统给出的静态兜底值。 */
		ret = run_init_process(CONFIG_DEFAULT_INIT);
		if (ret)
			pr_err("Default init %s failed (error %d)\n",
			       CONFIG_DEFAULT_INIT, ret);
		else
			return 0;
	}

	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/admin-guide/init.rst for guidance.");
}

/* 打开 /dev/console，作为最初的 stdin/stdout/stderr。
 * 这里理论上不应失败，但若失败只能发告警继续往前走。
 */
void __init console_on_rootfs(void)
{
	struct file *file = filp_open("/dev/console", O_RDWR, 0);

	if (IS_ERR(file)) {
		pr_err("Warning: unable to open an initial console.\n");
		return;
	}
	init_dup(file);
	init_dup(file);
	init_dup(file);
	fput(file);
}

static noinline void __init kernel_init_freeable(void)
{
	/* 走到这里，调度器已经完整可用，因此可以放心做会阻塞的内存分配。 */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init 线程此时可以从任意内存节点分配页，
	 * 不再受早期启动阶段那种严格绑定约束。
	 */
	set_mems_allowed(node_states[N_MEMORY]);

	cad_pid = get_pid(task_pid(current));

	/* 这一段是从“boot CPU 单线程主线”切到“完整 SMP + 完整 workqueue + 驱动初始化”
	 * 的关键桥接阶段。
	 */
	smp_prepare_cpus(setup_max_cpus);

	workqueue_init();

	init_mm_internals();

	do_pre_smp_initcalls();
	lockup_detector_init();

	smp_init();
	sched_init_smp();

	workqueue_init_topology();
	async_init();
	padata_init();
	page_alloc_init_late();

	do_basic_setup();

	kunit_run_all_tests();

	/*
	 * 等待内建 initramfs 解包结束。只有等这里完成，rdinit=/init 之类 early
	 * userspace 入口才真正有机会在 rootfs 上出现。
	 */
	wait_for_initramfs();
	console_on_rootfs();

	/*
	 * 检查是否存在 early userspace 的 rdinit= 入口。
	 * 如果能用，就让它接管后续根文件系统和用户态初始化工作；
	 * 否则回落到 prepare_namespace() 走常规 rootfs 准备流程。
	 */
	int ramdisk_command_access;
	ramdisk_command_access = init_eaccess(ramdisk_execute_command);
	if (ramdisk_command_access != 0) {
		if (ramdisk_execute_command_set)
			pr_warn("check access for rdinit=%s failed: %i, ignoring\n",
				ramdisk_execute_command, ramdisk_command_access);
		ramdisk_execute_command = NULL;
		/*
		 * 没有可执行的 rdinit= 时，才进入传统“准备真实根设备并切根”的路径。
		 * 这也是块设备 rootfs、NFS root、旧式 initrd 等方案的汇合点。
		 */
		prepare_namespace();
	}

	/*
	 * 到这里，初始引导基本完成，rootfs 也已经可用。
	 * 后面就只剩最后的安全/模块相关收尾，以及把控制权交给用户态。
	 */

	integrity_load_keys();
}
