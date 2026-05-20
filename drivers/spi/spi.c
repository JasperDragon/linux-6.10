// SPDX-License-Identifier: GPL-2.0-or-later
// SPI init/core code
//
// Copyright (C) 2005 David Brownell
// Copyright (C) 2008 Secret Lab Technologies Ltd.

#include <linux/acpi.h>
#include <linux/cache.h>
#include <linux/clk/clk-conf.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/platform_data/x86/apple.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/sched/rt.h>
#include <linux/slab.h>
#include <linux/spi/offload/types.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <uapi/linux/sched/types.h>

#define CREATE_TRACE_POINTS
#include <trace/events/spi.h>
EXPORT_TRACEPOINT_SYMBOL(spi_transfer_start);
EXPORT_TRACEPOINT_SYMBOL(spi_transfer_stop);

#include "internals.h"

static int __spi_setup(struct spi_device *spi, bool initial_setup);

static DEFINE_IDR(spi_controller_idr);

static void spidev_release(struct device *dev)
{
	struct spi_device	*spi = to_spi_device(dev);

	spi_controller_put(spi->controller);
	free_percpu(spi->pcpu_statistics);
	kfree(spi);
}

static ssize_t
modalias_show(struct device *dev, struct device_attribute *a, char *buf)
{
	const struct spi_device	*spi = to_spi_device(dev);
	int len;

	len = acpi_device_modalias(dev, buf, PAGE_SIZE - 1);
	if (len != -ENODEV)
		return len;

	return sysfs_emit(buf, "%s%s\n", SPI_MODULE_PREFIX, spi->modalias);
}
static DEVICE_ATTR_RO(modalias);

static ssize_t driver_override_store(struct device *dev,
				     struct device_attribute *a,
				     const char *buf, size_t count)
{
	int ret;

	ret = __device_set_driver_override(dev, buf, count);
	if (ret)
		return ret;

	return count;
}

static ssize_t driver_override_show(struct device *dev,
				    struct device_attribute *a, char *buf)
{
	guard(spinlock)(&dev->driver_override.lock);
	return sysfs_emit(buf, "%s\n", dev->driver_override.name ?: "");
}
static DEVICE_ATTR_RW(driver_override);

static struct spi_statistics __percpu *spi_alloc_pcpu_stats(void)
{
	struct spi_statistics __percpu *pcpu_stats;
	int cpu;

	pcpu_stats = alloc_percpu_gfp(struct spi_statistics, GFP_KERNEL);
	if (!pcpu_stats)
		return NULL;

	for_each_possible_cpu(cpu) {
		struct spi_statistics *stat;

		stat = per_cpu_ptr(pcpu_stats, cpu);
		u64_stats_init(&stat->syncp);
	}

	return pcpu_stats;
}

static ssize_t spi_emit_pcpu_stats(struct spi_statistics __percpu *stat,
				   char *buf, size_t offset)
{
	u64 val = 0;
	int i;

	for_each_possible_cpu(i) {
		const struct spi_statistics *pcpu_stats;
		u64_stats_t *field;
		unsigned int start;
		u64 inc;

		pcpu_stats = per_cpu_ptr(stat, i);
		field = (void *)pcpu_stats + offset;
		do {
			start = u64_stats_fetch_begin(&pcpu_stats->syncp);
			inc = u64_stats_read(field);
		} while (u64_stats_fetch_retry(&pcpu_stats->syncp, start));
		val += inc;
	}
	return sysfs_emit(buf, "%llu\n", val);
}

#define SPI_STATISTICS_ATTRS(field, file)				\
static ssize_t spi_controller_##field##_show(struct device *dev,	\
					     struct device_attribute *attr, \
					     char *buf)			\
{									\
	struct spi_controller *ctlr = container_of(dev,			\
					 struct spi_controller, dev);	\
	return spi_statistics_##field##_show(ctlr->pcpu_statistics, buf); \
}									\
static struct device_attribute dev_attr_spi_controller_##field = {	\
	.attr = { .name = file, .mode = 0444 },				\
	.show = spi_controller_##field##_show,				\
};									\
static ssize_t spi_device_##field##_show(struct device *dev,		\
					 struct device_attribute *attr,	\
					char *buf)			\
{									\
	struct spi_device *spi = to_spi_device(dev);			\
	return spi_statistics_##field##_show(spi->pcpu_statistics, buf); \
}									\
static struct device_attribute dev_attr_spi_device_##field = {		\
	.attr = { .name = file, .mode = 0444 },				\
	.show = spi_device_##field##_show,				\
}

#define SPI_STATISTICS_SHOW_NAME(name, file, field)			\
static ssize_t spi_statistics_##name##_show(struct spi_statistics __percpu *stat, \
					    char *buf)			\
{									\
	return spi_emit_pcpu_stats(stat, buf,				\
			offsetof(struct spi_statistics, field));	\
}									\
SPI_STATISTICS_ATTRS(name, file)

#define SPI_STATISTICS_SHOW(field)					\
	SPI_STATISTICS_SHOW_NAME(field, __stringify(field),		\
				 field)

SPI_STATISTICS_SHOW(messages);
SPI_STATISTICS_SHOW(transfers);
SPI_STATISTICS_SHOW(errors);
SPI_STATISTICS_SHOW(timedout);

SPI_STATISTICS_SHOW(spi_sync);
SPI_STATISTICS_SHOW(spi_sync_immediate);
SPI_STATISTICS_SHOW(spi_async);

SPI_STATISTICS_SHOW(bytes);
SPI_STATISTICS_SHOW(bytes_rx);
SPI_STATISTICS_SHOW(bytes_tx);

#define SPI_STATISTICS_TRANSFER_BYTES_HISTO(index, number)		\
	SPI_STATISTICS_SHOW_NAME(transfer_bytes_histo##index,		\
				 "transfer_bytes_histo_" number,	\
				 transfer_bytes_histo[index])
SPI_STATISTICS_TRANSFER_BYTES_HISTO(0,  "0-1");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(1,  "2-3");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(2,  "4-7");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(3,  "8-15");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(4,  "16-31");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(5,  "32-63");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(6,  "64-127");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(7,  "128-255");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(8,  "256-511");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(9,  "512-1023");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(10, "1024-2047");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(11, "2048-4095");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(12, "4096-8191");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(13, "8192-16383");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(14, "16384-32767");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(15, "32768-65535");
SPI_STATISTICS_TRANSFER_BYTES_HISTO(16, "65536+");

SPI_STATISTICS_SHOW(transfers_split_maxsize);

static struct attribute *spi_dev_attrs[] = {
	&dev_attr_modalias.attr,
	&dev_attr_driver_override.attr,
	NULL,
};

static const struct attribute_group spi_dev_group = {
	.attrs  = spi_dev_attrs,
};

static struct attribute *spi_device_statistics_attrs[] = {
	&dev_attr_spi_device_messages.attr,
	&dev_attr_spi_device_transfers.attr,
	&dev_attr_spi_device_errors.attr,
	&dev_attr_spi_device_timedout.attr,
	&dev_attr_spi_device_spi_sync.attr,
	&dev_attr_spi_device_spi_sync_immediate.attr,
	&dev_attr_spi_device_spi_async.attr,
	&dev_attr_spi_device_bytes.attr,
	&dev_attr_spi_device_bytes_rx.attr,
	&dev_attr_spi_device_bytes_tx.attr,
	&dev_attr_spi_device_transfer_bytes_histo0.attr,
	&dev_attr_spi_device_transfer_bytes_histo1.attr,
	&dev_attr_spi_device_transfer_bytes_histo2.attr,
	&dev_attr_spi_device_transfer_bytes_histo3.attr,
	&dev_attr_spi_device_transfer_bytes_histo4.attr,
	&dev_attr_spi_device_transfer_bytes_histo5.attr,
	&dev_attr_spi_device_transfer_bytes_histo6.attr,
	&dev_attr_spi_device_transfer_bytes_histo7.attr,
	&dev_attr_spi_device_transfer_bytes_histo8.attr,
	&dev_attr_spi_device_transfer_bytes_histo9.attr,
	&dev_attr_spi_device_transfer_bytes_histo10.attr,
	&dev_attr_spi_device_transfer_bytes_histo11.attr,
	&dev_attr_spi_device_transfer_bytes_histo12.attr,
	&dev_attr_spi_device_transfer_bytes_histo13.attr,
	&dev_attr_spi_device_transfer_bytes_histo14.attr,
	&dev_attr_spi_device_transfer_bytes_histo15.attr,
	&dev_attr_spi_device_transfer_bytes_histo16.attr,
	&dev_attr_spi_device_transfers_split_maxsize.attr,
	NULL,
};

static const struct attribute_group spi_device_statistics_group = {
	.name  = "statistics",
	.attrs  = spi_device_statistics_attrs,
};

static const struct attribute_group *spi_dev_groups[] = {
	&spi_dev_group,
	&spi_device_statistics_group,
	NULL,
};

static struct attribute *spi_controller_statistics_attrs[] = {
	&dev_attr_spi_controller_messages.attr,
	&dev_attr_spi_controller_transfers.attr,
	&dev_attr_spi_controller_errors.attr,
	&dev_attr_spi_controller_timedout.attr,
	&dev_attr_spi_controller_spi_sync.attr,
	&dev_attr_spi_controller_spi_sync_immediate.attr,
	&dev_attr_spi_controller_spi_async.attr,
	&dev_attr_spi_controller_bytes.attr,
	&dev_attr_spi_controller_bytes_rx.attr,
	&dev_attr_spi_controller_bytes_tx.attr,
	&dev_attr_spi_controller_transfer_bytes_histo0.attr,
	&dev_attr_spi_controller_transfer_bytes_histo1.attr,
	&dev_attr_spi_controller_transfer_bytes_histo2.attr,
	&dev_attr_spi_controller_transfer_bytes_histo3.attr,
	&dev_attr_spi_controller_transfer_bytes_histo4.attr,
	&dev_attr_spi_controller_transfer_bytes_histo5.attr,
	&dev_attr_spi_controller_transfer_bytes_histo6.attr,
	&dev_attr_spi_controller_transfer_bytes_histo7.attr,
	&dev_attr_spi_controller_transfer_bytes_histo8.attr,
	&dev_attr_spi_controller_transfer_bytes_histo9.attr,
	&dev_attr_spi_controller_transfer_bytes_histo10.attr,
	&dev_attr_spi_controller_transfer_bytes_histo11.attr,
	&dev_attr_spi_controller_transfer_bytes_histo12.attr,
	&dev_attr_spi_controller_transfer_bytes_histo13.attr,
	&dev_attr_spi_controller_transfer_bytes_histo14.attr,
	&dev_attr_spi_controller_transfer_bytes_histo15.attr,
	&dev_attr_spi_controller_transfer_bytes_histo16.attr,
	&dev_attr_spi_controller_transfers_split_maxsize.attr,
	NULL,
};

static const struct attribute_group spi_controller_statistics_group = {
	.name  = "statistics",
	.attrs  = spi_controller_statistics_attrs,
};

static const struct attribute_group *spi_controller_groups[] = {
	&spi_controller_statistics_group,
	NULL,
};

static void spi_statistics_add_transfer_stats(struct spi_statistics __percpu *pcpu_stats,
					      struct spi_transfer *xfer,
					      struct spi_message *msg)
{
	int l2len = min(fls(xfer->len), SPI_STATISTICS_HISTO_SIZE) - 1;
	struct spi_statistics *stats;

	if (l2len < 0)
		l2len = 0;

	get_cpu();
	stats = this_cpu_ptr(pcpu_stats);
	u64_stats_update_begin(&stats->syncp);

	u64_stats_inc(&stats->transfers);
	u64_stats_inc(&stats->transfer_bytes_histo[l2len]);

	u64_stats_add(&stats->bytes, xfer->len);
	if (spi_valid_txbuf(msg, xfer))
		u64_stats_add(&stats->bytes_tx, xfer->len);
	if (spi_valid_rxbuf(msg, xfer))
		u64_stats_add(&stats->bytes_rx, xfer->len);

	u64_stats_update_end(&stats->syncp);
	put_cpu();
}

/*
 * modalias support makes "modprobe $MODALIAS" new-style hotplug work,
 * and the sysfs version makes coldplug work too.
 */
static const struct spi_device_id *spi_match_id(const struct spi_device_id *id, const char *name)
{
	while (id->name[0]) {
		if (!strcmp(name, id->name))
			return id;
		id++;
	}
	return NULL;
}

const struct spi_device_id *spi_get_device_id(const struct spi_device *sdev)
{
	const struct spi_driver *sdrv = to_spi_driver(sdev->dev.driver);

	return spi_match_id(sdrv->id_table, sdev->modalias);
}
EXPORT_SYMBOL_GPL(spi_get_device_id);

const void *spi_get_device_match_data(const struct spi_device *sdev)
{
	const void *match;

	match = device_get_match_data(&sdev->dev);
	if (match)
		return match;

	return (const void *)spi_get_device_id(sdev)->driver_data;
}
EXPORT_SYMBOL_GPL(spi_get_device_match_data);

static int spi_match_device(struct device *dev, const struct device_driver *drv)
{
	const struct spi_device	*spi = to_spi_device(dev);
	const struct spi_driver	*sdrv = to_spi_driver(drv);
	int ret;

	/* 先检查 override；如果设置了，就只使用指定名称的驱动。 */
	ret = device_match_driver_override(dev, drv);
	if (ret >= 0)
		return ret;

	/* 尝试进行 OF 风格匹配。 */
	if (of_driver_match_device(dev, drv))
		return 1;

	/* 然后再尝试 ACPI。 */
	if (acpi_driver_match_device(dev, drv))
		return 1;

	if (sdrv->id_table)
		return !!spi_match_id(sdrv->id_table, spi->modalias);

	return strcmp(spi->modalias, drv->name) == 0;
}

static int spi_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct spi_device		*spi = to_spi_device(dev);
	int rc;

	rc = acpi_device_uevent_modalias(dev, env);
	if (rc != -ENODEV)
		return rc;

	return add_uevent_var(env, "MODALIAS=%s%s", SPI_MODULE_PREFIX, spi->modalias);
}

static int spi_probe(struct device *dev)
{
	const struct spi_driver		*sdrv = to_spi_driver(dev->driver);
	struct spi_device		*spi = to_spi_device(dev);
	struct fwnode_handle		*fwnode = dev_fwnode(dev);
	int ret;

	ret = of_clk_set_defaults(dev->of_node, false);
	if (ret)
		return ret;

	if (is_of_node(fwnode))
		spi->irq = of_irq_get(dev->of_node, 0);
	else if (is_acpi_device_node(fwnode) && spi->irq < 0)
		spi->irq = acpi_dev_gpio_irq_get(to_acpi_device_node(fwnode), 0);
	if (spi->irq == -EPROBE_DEFER)
		return dev_err_probe(dev, spi->irq, "Failed to get irq\n");
	if (spi->irq < 0)
		spi->irq = 0;

	ret = dev_pm_domain_attach(dev, PD_FLAG_ATTACH_POWER_ON |
					PD_FLAG_DETACH_POWER_OFF);
	if (ret)
		return ret;

	if (sdrv->probe)
		ret = sdrv->probe(spi);

	return ret;
}

static void spi_remove(struct device *dev)
{
	const struct spi_driver		*sdrv = to_spi_driver(dev->driver);

	if (sdrv->remove)
		sdrv->remove(to_spi_device(dev));
}

static void spi_shutdown(struct device *dev)
{
	if (dev->driver) {
		const struct spi_driver	*sdrv = to_spi_driver(dev->driver);

		if (sdrv->shutdown)
			sdrv->shutdown(to_spi_device(dev));
	}
}

const struct bus_type spi_bus_type = {
	.name		= "spi",
	.dev_groups	= spi_dev_groups,
	.match		= spi_match_device,
	.uevent		= spi_uevent,
	.probe		= spi_probe,
	.remove		= spi_remove,
	.shutdown	= spi_shutdown,
};
EXPORT_SYMBOL_GPL(spi_bus_type);

/**
 * __spi_register_driver - 注册一个 SPI 驱动
 * @owner: 待注册驱动所属的模块
 * @sdrv: 待注册的驱动
 * Context: can sleep
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int __spi_register_driver(struct module *owner, struct spi_driver *sdrv)
{
	sdrv->driver.owner = owner;
	sdrv->driver.bus = &spi_bus_type;

	/*
	 * For Really Good Reasons we use spi: modaliases not of:
	 * modaliases for DT so module autoloading won't work if we
	 * don't have a spi_device_id as well as a compatible string.
	 */
	if (sdrv->driver.of_match_table) {
		const struct of_device_id *of_id;

		for (of_id = sdrv->driver.of_match_table; of_id->compatible[0];
		     of_id++) {
			const char *of_name;

			/* Strip off any vendor prefix */
			of_name = strnchr(of_id->compatible,
					  sizeof(of_id->compatible), ',');
			if (of_name)
				of_name++;
			else
				of_name = of_id->compatible;

			if (sdrv->id_table) {
				const struct spi_device_id *spi_id;

				spi_id = spi_match_id(sdrv->id_table, of_name);
				if (spi_id)
					continue;
			} else {
				if (strcmp(sdrv->driver.name, of_name) == 0)
					continue;
			}

			pr_warn("SPI driver %s has no spi_device_id for %s\n",
				sdrv->driver.name, of_id->compatible);
		}
	}

	return driver_register(&sdrv->driver);
}
EXPORT_SYMBOL_GPL(__spi_register_driver);

/*-------------------------------------------------------------------------*/

/*
 * SPI 设备通常不应该由 SPI 设备驱动自己创建，否则会变成
 * 板级绑定逻辑。SPI 控制器驱动也是同理。
 * 设备注册通常放在 arch/.../mach.../board-YYY.c 之类的地方，
 * 和主板设备的只读（可固化）信息放在一起。
 */

struct boardinfo {
	struct list_head	list;
	struct spi_board_info	board_info;
};

static LIST_HEAD(board_list);
static LIST_HEAD(spi_controller_list);

/*
 * 用于保护 board_info 列表和 spi_controller 列表的增删操作，
 * 以及它们的匹配过程，同时也保护 struct idr 对象。
 */
static DEFINE_MUTEX(board_lock);

/**
 * spi_alloc_device - 分配一个新的 SPI 设备
 * @ctlr: 设备所属的控制器
 * Context: can sleep
 *
 * 允许驱动先分配并初始化一个 spi_device，但不立即注册。
 * 这样驱动就可以在调用 spi_add_device() 之前，直接给 spi_device
 * 填好设备参数。
 *
 * 调用者需要在返回的 spi_device 上继续调用 spi_add_device()，
 * 才能把它挂到 SPI 控制器上。如果调用者最终不想添加这个设备，
 * 则应调用 spi_dev_put() 释放它。
 *
 * 返回：
 * 成功时返回新设备指针，失败返回 NULL。
 */
struct spi_device *spi_alloc_device(struct spi_controller *ctlr)
{
	struct spi_device	*spi;

	if (!spi_controller_get(ctlr))
		return NULL;

	spi = kzalloc_obj(*spi);
	if (!spi) {
		spi_controller_put(ctlr);
		return NULL;
	}

	spi->pcpu_statistics = spi_alloc_pcpu_stats();
	if (!spi->pcpu_statistics) {
		kfree(spi);
		spi_controller_put(ctlr);
		return NULL;
	}

	spi->controller = ctlr;
	spi->dev.parent = &ctlr->dev;
	spi->dev.bus = &spi_bus_type;
	spi->dev.release = spidev_release;
	spi->mode = ctlr->buswidth_override_bits;
	spi->num_chipselect = 1;

	device_initialize(&spi->dev);
	return spi;
}
EXPORT_SYMBOL_GPL(spi_alloc_device);

static void spi_dev_set_name(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev);

	if (is_acpi_device_node(fwnode)) {
		dev_set_name(dev, "spi-%s", acpi_dev_name(to_acpi_device_node(fwnode)));
		return;
	}

	if (is_software_node(fwnode)) {
		dev_set_name(dev, "spi-%pfwP", fwnode);
		return;
	}

	dev_set_name(&spi->dev, "%s.%u", dev_name(&spi->controller->dev),
		     spi_get_chipselect(spi, 0));
}

/*
 * Zero(0) is a valid physical CS value and can be located at any
 * logical CS in the spi->chip_select[]. If all the physical CS
 * are initialized to 0 then It would be difficult to differentiate
 * between a valid physical CS 0 & an unused logical CS whose physical
 * CS can be 0. As a solution to this issue initialize all the CS to -1.
 * Now all the unused logical CS will have -1 physical CS value & can be
 * ignored while performing physical CS validity checks.
 */
#define SPI_INVALID_CS		((s8)-1)

static inline int spi_dev_check_cs(struct device *dev,
				   struct spi_device *spi, u8 idx,
				   struct spi_device *new_spi, u8 new_idx)
{
	u8 cs, cs_new;
	u8 idx_new;

	cs = spi_get_chipselect(spi, idx);
	for (idx_new = new_idx; idx_new < new_spi->num_chipselect; idx_new++) {
		cs_new = spi_get_chipselect(new_spi, idx_new);
		if (cs == cs_new) {
			dev_err(dev, "chipselect %u already in use\n", cs_new);
			return -EBUSY;
		}
	}
	return 0;
}

struct spi_dev_check_info {
	struct spi_device *new_spi;
	struct spi_device *parent;	/* set for ancillary devices */
};

static int spi_dev_check(struct device *dev, void *data)
{
	struct spi_device *spi = to_spi_device(dev);
	struct spi_dev_check_info *info = data;
	struct spi_device *new_spi = info->new_spi;
	int status, idx;

	/*
	 * When registering an ancillary device, skip checking against the
	 * parent device since the ancillary is intentionally using one of
	 * the parent's chip selects.
	 */
	if (info->parent && spi == info->parent)
		return 0;

	if (spi->controller == new_spi->controller) {
		for (idx = 0; idx < spi->num_chipselect; idx++) {
			status = spi_dev_check_cs(dev, spi, idx, new_spi, 0);
			if (status)
				return status;
		}
	}
	return 0;
}

static void spi_cleanup(struct spi_device *spi)
{
	if (spi->controller->cleanup)
		spi->controller->cleanup(spi);
}

static int __spi_add_device(struct spi_device *spi, struct spi_device *parent)
{
	struct spi_controller *ctlr = spi->controller;
	struct device *dev = ctlr->dev.parent;
	struct spi_dev_check_info check_info;
	int status, idx;
	u8 cs;

	if (spi->num_chipselect > SPI_DEVICE_CS_CNT_MAX) {
		dev_err(dev, "num_cs %d > max %d\n", spi->num_chipselect,
			SPI_DEVICE_CS_CNT_MAX);
		return -EOVERFLOW;
	}

	for (idx = 0; idx < spi->num_chipselect; idx++) {
		/* chipselect 的编号范围是 0..max，这里做校验。 */
		cs = spi_get_chipselect(spi, idx);
		if (cs >= ctlr->num_chipselect) {
			dev_err(dev, "cs%d >= max %d\n", spi_get_chipselect(spi, idx),
				ctlr->num_chipselect);
			return -EINVAL;
		}
	}

	/*
	 * 确保多个逻辑 CS 不会映射到同一个物理 CS。
	 * 例如，spi->chip_select[0] 不能等于 spi->chip_select[1]。
	 */
	if (!spi_controller_is_target(ctlr)) {
		for (idx = 0; idx < spi->num_chipselect; idx++) {
			status = spi_dev_check_cs(dev, spi, idx, spi, idx + 1);
			if (status)
				return status;
		}
	}

	/* Initialize unused logical CS as invalid */
	for (idx = spi->num_chipselect; idx < SPI_DEVICE_CS_CNT_MAX; idx++)
		spi_set_chipselect(spi, idx, SPI_INVALID_CS);

	/* Set the bus ID string */
	spi_dev_set_name(spi);

	/*
	 * 必须在调用 setup() 之前确认没有其它设备占用这个 chipselect，
	 * 否则会破坏对方的配置。
	 */
	check_info.new_spi = spi;
	check_info.parent = parent;
	status = bus_for_each_dev(&spi_bus_type, NULL, &check_info, spi_dev_check);
	if (status)
		return status;

	/* 控制器可能会并发注销。 */
	if (IS_ENABLED(CONFIG_SPI_DYNAMIC) &&
	    !device_is_registered(&ctlr->dev)) {
		return -ENODEV;
	}

	if (ctlr->cs_gpiods) {
		u8 cs;

		for (idx = 0; idx < spi->num_chipselect; idx++) {
			cs = spi_get_chipselect(spi, idx);
			spi_set_csgpiod(spi, idx, ctlr->cs_gpiods[cs]);
		}
	}

	/*
	 * Drivers may modify this initial i/o setup, but will
	 * normally rely on the device being setup.  Devices
	 * using SPI_CS_HIGH can't coexist well otherwise...
	 */
	status = __spi_setup(spi, true);
	if (status < 0) {
		dev_err(dev, "can't setup %s, status %d\n",
				dev_name(&spi->dev), status);
		return status;
	}

	/* 返回时，该设备可能已经绑定到一个正在工作的驱动。 */
	status = device_add(&spi->dev);
	if (status < 0) {
		dev_err(dev, "can't add %s, status %d\n",
				dev_name(&spi->dev), status);
		spi_cleanup(spi);
	} else {
		dev_dbg(dev, "registered child %s\n", dev_name(&spi->dev));
	}

	return status;
}

/**
 * spi_add_device - 注册通过 spi_alloc_device 分配的 spi_device
 * @spi: 要注册的 spi_device
 *
 * 这是 spi_alloc_device 的配套函数。通过 spi_alloc_device 分配的设备，
 * 可以用这个接口挂到 SPI 总线上。
 *
 * 返回：
 * 成功返回 0，失败返回负 errno。
 */
int spi_add_device(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;
	int status;

	/* 设置总线 ID 字符串。 */
	spi_dev_set_name(spi);

	mutex_lock(&ctlr->add_lock);
	status = __spi_add_device(spi, NULL);
	mutex_unlock(&ctlr->add_lock);
	return status;
}
EXPORT_SYMBOL_GPL(spi_add_device);

/**
 * spi_new_device - 实例化一个新的 SPI 设备
 * @ctlr: 设备所属的控制器
 * @chip: SPI 设备描述信息
 * Context: can sleep
 *
 * 在典型主板上，这个接口主要用于内部板级初始化；
 * 一旦板级代码把硬连线设备创建完成，通常就不再需要它。
 * 但有些开发平台可能无法使用 spi_register_board_info()，
 * 因此这里导出该接口，方便例如基于 USB 或并口的适配器驱动
 * 在运行时添加它们通过其它途径获知的设备。
 *
 * 返回：
 * 新设备指针，失败返回 NULL。
 */
struct spi_device *spi_new_device(struct spi_controller *ctlr,
				  struct spi_board_info *chip)
{
	struct spi_device	*proxy;
	int			status;

	/*
	 * 注意：调用者已经完成了对 chip->bus_num 的必要检查。
	 *
	 * 另外，除非我们把返回值约定改成 error-or-pointer
	 * （而不是 NULL-or-pointer），否则从可调试性考虑，
	 * 这里最好还是打印日志诊断信息。
	 */

	proxy = spi_alloc_device(ctlr);
	if (!proxy)
		return NULL;

	WARN_ON(strlen(chip->modalias) >= sizeof(proxy->modalias));

	/* 代理设备使用传入的 chip-select。 */
	spi_set_chipselect(proxy, 0, chip->chip_select);

	proxy->max_speed_hz = chip->max_speed_hz;
	proxy->mode = chip->mode;
	proxy->irq = chip->irq;
	strscpy(proxy->modalias, chip->modalias, sizeof(proxy->modalias));
	proxy->dev.platform_data = (void *) chip->platform_data;
	proxy->controller_data = chip->controller_data;
	proxy->controller_state = NULL;
	/*
	 * 默认情况下，spi->chip_select[0] 保存物理 CS 编号，
	 * 因此要在 spi->cs_index_mask 里设置 bit 0。
	 */
	proxy->cs_index_mask = BIT(0);

	if (chip->swnode) {
		status = device_add_software_node(&proxy->dev, chip->swnode);
		if (status) {
			dev_err(&ctlr->dev, "failed to add software node to '%s': %d\n",
				chip->modalias, status);
			goto err_dev_put;
		}
	}

	status = spi_add_device(proxy);
	if (status < 0)
		goto err_dev_put;

	return proxy;

err_dev_put:
	device_remove_software_node(&proxy->dev);
	spi_dev_put(proxy);
	return NULL;
}
EXPORT_SYMBOL_GPL(spi_new_device);

/**
 * spi_unregister_device - 注销单个 SPI 设备
 * @spi: 要注销的 spi_device
 *
 * 开始让传入的 SPI 设备消失。通常这类工作会由
 * spi_unregister_controller() 统一处理。
 */
void spi_unregister_device(struct spi_device *spi)
{
	struct fwnode_handle *fwnode;

	if (!spi)
		return;

	fwnode = dev_fwnode(&spi->dev);
	if (is_of_node(fwnode)) {
		of_node_clear_flag(to_of_node(fwnode), OF_POPULATED);
		of_node_put(to_of_node(fwnode));
	} else if (is_acpi_device_node(fwnode)) {
		acpi_device_clear_enumerated(to_acpi_device_node(fwnode));
	}
	device_remove_software_node(&spi->dev);
	device_del(&spi->dev);
	spi_cleanup(spi);
	put_device(&spi->dev);
}
EXPORT_SYMBOL_GPL(spi_unregister_device);

static void spi_match_controller_to_boardinfo(struct spi_controller *ctlr,
					      struct spi_board_info *bi)
{
	struct spi_device *dev;

	if (ctlr->bus_num != bi->bus_num)
		return;

	dev = spi_new_device(ctlr, bi);
	if (!dev)
		dev_err(ctlr->dev.parent, "can't create new device for %s\n",
			bi->modalias);
}

/**
 * spi_register_board_info - 为指定板子注册 SPI 设备
 * @info: 芯片描述数组
 * @n: 描述符数量
 * Context: can sleep
 *
 * 板级早期初始化代码会调用这个接口（通常是在 arch_initcall 阶段），
 * 并传入 SPI 设备表的一部分。等到对应的父 SPI 控制器
 * （bus_num）定义完成之后，设备节点才会在后续创建。
 * 我们会永久保存这张设备表，这样即使重新加载控制器驱动，
 * Linux 也不会忘记这些硬连线设备。
 *
 * 其他代码也可以调用这个接口，例如某些扩展板会通过扩展连接器
 * 提供 SPI 设备，那么初始化该扩展板的代码自然也可以声明这些设备。
 *
 * 传入的 board info 可以安全地放在 __initdata 中；不过要注意其中
 * 的嵌入指针（如 platform_data 等）会按原样拷贝。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_register_board_info(struct spi_board_info const *info, unsigned n)
{
	struct boardinfo *bi;
	int i;

	if (!n)
		return 0;

	bi = kzalloc_objs(*bi, n);
	if (!bi)
		return -ENOMEM;

	for (i = 0; i < n; i++, bi++, info++) {
		struct spi_controller *ctlr;

		memcpy(&bi->board_info, info, sizeof(*info));

		mutex_lock(&board_lock);
		list_add_tail(&bi->list, &board_list);
		list_for_each_entry(ctlr, &spi_controller_list, list)
			spi_match_controller_to_boardinfo(ctlr,
							  &bi->board_info);
		mutex_unlock(&board_lock);
	}

	return 0;
}

/*-------------------------------------------------------------------------*/

/* SPI 资源管理的核心方法。 */

/**
 * spi_res_alloc - 分配一个由 spi_message 生命周期管理的 SPI 资源
 *                该资源在使用 spi_transfer_one 处理消息期间有效
 * @spi:     为哪个 SPI 设备分配内存
 * @release: 释放该资源时要执行的回调
 * @size:    需要分配并返回的大小
 * @gfp:     GFP 分配标志
 *
 * 返回：
 * 指向已分配数据的指针。
 *
 * 未来这里也许会扩展为从 @spi_device 或 @spi_controller 的内存池
 * 中分配，以减少重复分配。
 */
static void *spi_res_alloc(struct spi_device *spi, spi_res_release_t release,
			   size_t size, gfp_t gfp)
{
	struct spi_res *sres;

	sres = kzalloc(sizeof(*sres) + size, gfp);
	if (!sres)
		return NULL;

	INIT_LIST_HEAD(&sres->entry);
	sres->release = release;

	return sres->data;
}

/**
 * spi_res_free - 释放一个 SPI 资源
 * @res: 资源自定义数据的指针
 */
static void spi_res_free(void *res)
{
	struct spi_res *sres = container_of(res, struct spi_res, data);

	WARN_ON(!list_empty(&sres->entry));
	kfree(sres);
}

/**
 * spi_res_add - 把一个 spi_res 添加到 spi_message
 * @message: SPI 消息
 * @res:     spi 资源
 */
static void spi_res_add(struct spi_message *message, void *res)
{
	struct spi_res *sres = container_of(res, struct spi_res, data);

	WARN_ON(!list_empty(&sres->entry));
	list_add_tail(&sres->entry, &message->resources);
}

/**
 * spi_res_release - 释放该消息相关的所有 SPI 资源
 * @ctlr:  @spi_controller
 * @message: @spi_message
 */
static void spi_res_release(struct spi_controller *ctlr, struct spi_message *message)
{
	struct spi_res *res, *tmp;

	list_for_each_entry_safe_reverse(res, tmp, &message->resources, entry) {
		if (res->release)
			res->release(ctlr, message, res->data);

		list_del(&res->entry);

		kfree(res);
	}
}

/*-------------------------------------------------------------------------*/
#define spi_for_each_valid_cs(spi, idx)				\
	for (idx = 0; idx < spi->num_chipselect; idx++)		\
		if (!(spi->cs_index_mask & BIT(idx))) {} else

static inline bool spi_is_last_cs(struct spi_device *spi)
{
	u8 idx;
	bool last = false;

	spi_for_each_valid_cs(spi, idx) {
		if (spi->controller->last_cs[idx] == spi_get_chipselect(spi, idx))
			last = true;
	}
	return last;
}

static void spi_toggle_csgpiod(struct spi_device *spi, u8 idx, bool enable, bool activate)
{
	/*
	 * Historically ACPI has no means of the GPIO polarity and
	 * thus the SPISerialBus() resource defines it on the per-chip
	 * basis. In order to avoid a chain of negations, the GPIO
	 * polarity is considered being Active High. Even for the cases
	 * when _DSD() is involved (in the updated versions of ACPI)
	 * the GPIO CS polarity must be defined Active High to avoid
	 * ambiguity. That's why we use enable, that takes SPI_CS_HIGH
	 * into account.
	 */
	if (is_acpi_device_node(dev_fwnode(&spi->dev)))
		gpiod_set_value_cansleep(spi_get_csgpiod(spi, idx), !enable);
	else
		/* Polarity handled by GPIO library */
		gpiod_set_value_cansleep(spi_get_csgpiod(spi, idx), activate);

	if (activate)
		spi_delay_exec(&spi->cs_setup, NULL);
	else
		spi_delay_exec(&spi->cs_inactive, NULL);
}

static void spi_set_cs(struct spi_device *spi, bool enable, bool force)
{
	bool activate = enable;
	u8 idx;

		/*
		 * 如果 chip select 和上次调用相比并没有真正变化，就不要再调用驱动，
		 * 也不要重复做延迟处理。
		 */
	if (!force && (enable == spi_is_last_cs(spi)) &&
	    (spi->controller->last_cs_index_mask == spi->cs_index_mask) &&
	    (spi->controller->last_cs_mode_high == (spi->mode & SPI_CS_HIGH)))
		return;

	trace_spi_set_cs(spi, activate);

	spi->controller->last_cs_index_mask = spi->cs_index_mask;
	for (idx = 0; idx < SPI_DEVICE_CS_CNT_MAX; idx++) {
		if (enable && idx < spi->num_chipselect)
			spi->controller->last_cs[idx] = spi_get_chipselect(spi, 0);
		else
			spi->controller->last_cs[idx] = SPI_INVALID_CS;
	}

	spi->controller->last_cs_mode_high = spi->mode & SPI_CS_HIGH;
	if (spi->controller->last_cs_mode_high)
		enable = !enable;

	/*
	 * Handle chip select delays for GPIO based CS or controllers without
	 * programmable chip select timing.
	 */
	if ((spi_is_csgpiod(spi) || !spi->controller->set_cs_timing) && !activate)
		spi_delay_exec(&spi->cs_hold, NULL);

	if (spi_is_csgpiod(spi)) {
		if (!(spi->mode & SPI_NO_CS)) {
			spi_for_each_valid_cs(spi, idx) {
				if (spi_get_csgpiod(spi, idx))
					spi_toggle_csgpiod(spi, idx, enable, activate);
			}
		}
		/* Some SPI controllers need both GPIO CS & ->set_cs() */
		if ((spi->controller->flags & SPI_CONTROLLER_GPIO_SS) &&
		    spi->controller->set_cs)
			spi->controller->set_cs(spi, !enable);
	} else if (spi->controller->set_cs) {
		spi->controller->set_cs(spi, !enable);
	}

	if (spi_is_csgpiod(spi) || !spi->controller->set_cs_timing) {
		if (activate)
			spi_delay_exec(&spi->cs_setup, NULL);
		else
			spi_delay_exec(&spi->cs_inactive, NULL);
	}
}

#ifdef CONFIG_HAS_DMA
static int spi_map_buf_attrs(struct spi_controller *ctlr, struct device *dev,
			     struct sg_table *sgt, void *buf, size_t len,
			     enum dma_data_direction dir, unsigned long attrs)
{
	const bool vmalloced_buf = is_vmalloc_addr(buf);
	unsigned int max_seg_size = dma_get_max_seg_size(dev);
#ifdef CONFIG_HIGHMEM
	const bool kmap_buf = ((unsigned long)buf >= PKMAP_BASE &&
				(unsigned long)buf < (PKMAP_BASE +
					(LAST_PKMAP * PAGE_SIZE)));
#else
	const bool kmap_buf = false;
#endif
	int desc_len;
	int sgs;
	struct page *vm_page;
	struct scatterlist *sg;
	void *sg_buf;
	size_t min;
	int i, ret;

	if (vmalloced_buf || kmap_buf) {
		desc_len = min_t(unsigned long, max_seg_size, PAGE_SIZE);
		sgs = DIV_ROUND_UP(len + offset_in_page(buf), desc_len);
	} else if (virt_addr_valid(buf)) {
		desc_len = min_t(size_t, max_seg_size, ctlr->max_dma_len);
		sgs = DIV_ROUND_UP(len, desc_len);
	} else {
		return -EINVAL;
	}

	ret = sg_alloc_table(sgt, sgs, GFP_KERNEL);
	if (ret != 0)
		return ret;

	sg = &sgt->sgl[0];
	for (i = 0; i < sgs; i++) {

		if (vmalloced_buf || kmap_buf) {
			/*
			 * Next scatterlist entry size is the minimum between
			 * the desc_len and the remaining buffer length that
			 * fits in a page.
			 */
			min = min_t(size_t, desc_len,
				    min_t(size_t, len,
					  PAGE_SIZE - offset_in_page(buf)));
			if (vmalloced_buf)
				vm_page = vmalloc_to_page(buf);
			else
				vm_page = kmap_to_page(buf);
			if (!vm_page) {
				sg_free_table(sgt);
				return -ENOMEM;
			}
			sg_set_page(sg, vm_page,
				    min, offset_in_page(buf));
		} else {
			min = min_t(size_t, len, desc_len);
			sg_buf = buf;
			sg_set_buf(sg, sg_buf, min);
		}

		buf += min;
		len -= min;
		sg = sg_next(sg);
	}

	ret = dma_map_sgtable(dev, sgt, dir, attrs);
	if (ret < 0) {
		sg_free_table(sgt);
		return ret;
	}

	return 0;
}

int spi_map_buf(struct spi_controller *ctlr, struct device *dev,
		struct sg_table *sgt, void *buf, size_t len,
		enum dma_data_direction dir)
{
	return spi_map_buf_attrs(ctlr, dev, sgt, buf, len, dir, 0);
}

static void spi_unmap_buf_attrs(struct spi_controller *ctlr,
				struct device *dev, struct sg_table *sgt,
				enum dma_data_direction dir,
				unsigned long attrs)
{
	dma_unmap_sgtable(dev, sgt, dir, attrs);
	sg_free_table(sgt);
	sgt->orig_nents = 0;
	sgt->nents = 0;
}

void spi_unmap_buf(struct spi_controller *ctlr, struct device *dev,
		   struct sg_table *sgt, enum dma_data_direction dir)
{
	spi_unmap_buf_attrs(ctlr, dev, sgt, dir, 0);
}

static int __spi_map_msg(struct spi_controller *ctlr, struct spi_message *msg)
{
	struct device *tx_dev, *rx_dev;
	struct spi_transfer *xfer;
	int ret;

	if (!ctlr->can_dma)
		return 0;

	if (ctlr->dma_tx)
		tx_dev = ctlr->dma_tx->device->dev;
	else if (ctlr->dma_map_dev)
		tx_dev = ctlr->dma_map_dev;
	else
		tx_dev = ctlr->dev.parent;

	if (ctlr->dma_rx)
		rx_dev = ctlr->dma_rx->device->dev;
	else if (ctlr->dma_map_dev)
		rx_dev = ctlr->dma_map_dev;
	else
		rx_dev = ctlr->dev.parent;

	ret = -ENOMSG;
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
			/* 每个 transfer 之前都要先完成同步。 */
		unsigned long attrs = DMA_ATTR_SKIP_CPU_SYNC;

		if (!ctlr->can_dma(ctlr, msg->spi, xfer))
			continue;

		if (xfer->tx_buf != NULL) {
			ret = spi_map_buf_attrs(ctlr, tx_dev, &xfer->tx_sg,
						(void *)xfer->tx_buf,
						xfer->len, DMA_TO_DEVICE,
						attrs);
			if (ret != 0)
				return ret;

			xfer->tx_sg_mapped = true;
		}

		if (xfer->rx_buf != NULL) {
			ret = spi_map_buf_attrs(ctlr, rx_dev, &xfer->rx_sg,
						xfer->rx_buf, xfer->len,
						DMA_FROM_DEVICE, attrs);
			if (ret != 0) {
				spi_unmap_buf_attrs(ctlr, tx_dev,
						&xfer->tx_sg, DMA_TO_DEVICE,
						attrs);

				return ret;
			}

			xfer->rx_sg_mapped = true;
		}
	}
	/* 没有任何 transfer 被映射，直接成功返回。 */
	if (ret)
		return 0;

	ctlr->cur_rx_dma_dev = rx_dev;
	ctlr->cur_tx_dma_dev = tx_dev;

	return 0;
}

static int __spi_unmap_msg(struct spi_controller *ctlr, struct spi_message *msg)
{
	struct device *rx_dev = ctlr->cur_rx_dma_dev;
	struct device *tx_dev = ctlr->cur_tx_dma_dev;
	struct spi_transfer *xfer;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		/* 每个 transfer 之后的同步已经完成。 */
		unsigned long attrs = DMA_ATTR_SKIP_CPU_SYNC;

		if (xfer->rx_sg_mapped)
			spi_unmap_buf_attrs(ctlr, rx_dev, &xfer->rx_sg,
					    DMA_FROM_DEVICE, attrs);
		xfer->rx_sg_mapped = false;

		if (xfer->tx_sg_mapped)
			spi_unmap_buf_attrs(ctlr, tx_dev, &xfer->tx_sg,
					    DMA_TO_DEVICE, attrs);
		xfer->tx_sg_mapped = false;
	}

	return 0;
}

static void spi_dma_sync_for_device(struct spi_controller *ctlr,
				    struct spi_transfer *xfer)
{
	struct device *rx_dev = ctlr->cur_rx_dma_dev;
	struct device *tx_dev = ctlr->cur_tx_dma_dev;

	if (xfer->tx_sg_mapped)
		dma_sync_sgtable_for_device(tx_dev, &xfer->tx_sg, DMA_TO_DEVICE);
	if (xfer->rx_sg_mapped)
		dma_sync_sgtable_for_device(rx_dev, &xfer->rx_sg, DMA_FROM_DEVICE);
}

static void spi_dma_sync_for_cpu(struct spi_controller *ctlr,
				 struct spi_transfer *xfer)
{
	struct device *rx_dev = ctlr->cur_rx_dma_dev;
	struct device *tx_dev = ctlr->cur_tx_dma_dev;

	if (xfer->rx_sg_mapped)
		dma_sync_sgtable_for_cpu(rx_dev, &xfer->rx_sg, DMA_FROM_DEVICE);
	if (xfer->tx_sg_mapped)
		dma_sync_sgtable_for_cpu(tx_dev, &xfer->tx_sg, DMA_TO_DEVICE);
}
#else /* !CONFIG_HAS_DMA */
static inline int __spi_map_msg(struct spi_controller *ctlr,
				struct spi_message *msg)
{
	return 0;
}

static inline int __spi_unmap_msg(struct spi_controller *ctlr,
				  struct spi_message *msg)
{
	return 0;
}

static void spi_dma_sync_for_device(struct spi_controller *ctrl,
				    struct spi_transfer *xfer)
{
}

static void spi_dma_sync_for_cpu(struct spi_controller *ctrl,
				 struct spi_transfer *xfer)
{
}
#endif /* !CONFIG_HAS_DMA */

static inline int spi_unmap_msg(struct spi_controller *ctlr,
				struct spi_message *msg)
{
	struct spi_transfer *xfer;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		/*
		 * Restore the original value of tx_buf or rx_buf if they are
		 * NULL.
		 */
		if (xfer->tx_buf == ctlr->dummy_tx)
			xfer->tx_buf = NULL;
		if (xfer->rx_buf == ctlr->dummy_rx)
			xfer->rx_buf = NULL;
	}

	return __spi_unmap_msg(ctlr, msg);
}

static int spi_map_msg(struct spi_controller *ctlr, struct spi_message *msg)
{
	struct spi_transfer *xfer;
	void *tmp;
	unsigned int max_tx, max_rx;

	if ((ctlr->flags & (SPI_CONTROLLER_MUST_RX | SPI_CONTROLLER_MUST_TX))
		&& !(msg->spi->mode & SPI_3WIRE)) {
		max_tx = 0;
		max_rx = 0;

		list_for_each_entry(xfer, &msg->transfers, transfer_list) {
			if ((ctlr->flags & SPI_CONTROLLER_MUST_TX) &&
			    !xfer->tx_buf)
				max_tx = max(xfer->len, max_tx);
			if ((ctlr->flags & SPI_CONTROLLER_MUST_RX) &&
			    !xfer->rx_buf)
				max_rx = max(xfer->len, max_rx);
		}

		if (max_tx) {
			tmp = krealloc(ctlr->dummy_tx, max_tx,
				       GFP_KERNEL | GFP_DMA | __GFP_ZERO);
			if (!tmp)
				return -ENOMEM;
			ctlr->dummy_tx = tmp;
		}

		if (max_rx) {
			tmp = krealloc(ctlr->dummy_rx, max_rx,
				       GFP_KERNEL | GFP_DMA);
			if (!tmp)
				return -ENOMEM;
			ctlr->dummy_rx = tmp;
		}

		if (max_tx || max_rx) {
			list_for_each_entry(xfer, &msg->transfers,
					    transfer_list) {
				if (!xfer->len)
					continue;
				if (!xfer->tx_buf)
					xfer->tx_buf = ctlr->dummy_tx;
				if (!xfer->rx_buf)
					xfer->rx_buf = ctlr->dummy_rx;
			}
		}
	}

	return __spi_map_msg(ctlr, msg);
}

static int spi_transfer_wait(struct spi_controller *ctlr,
			     struct spi_message *msg,
			     struct spi_transfer *xfer)
{
	struct spi_statistics __percpu *statm = ctlr->pcpu_statistics;
	struct spi_statistics __percpu *stats = msg->spi->pcpu_statistics;
	u32 speed_hz = xfer->speed_hz;
	unsigned long long ms;

	if (spi_controller_is_target(ctlr)) {
		if (wait_for_completion_interruptible(&ctlr->xfer_completion)) {
			dev_dbg(&msg->spi->dev, "SPI transfer interrupted\n");
			return -EINTR;
		}
	} else {
		if (!speed_hz)
			speed_hz = 100000;

		/*
		 * 每个字节都要等待 8 个 SPI 时钟周期。
		 * 由于速度单位是 Hz，而这里需要的是毫秒，
		 * 计算时要先乘再除，否则短传输可能会得到 0。
		 */
		ms = 8LL * MSEC_PER_SEC * xfer->len;
		do_div(ms, speed_hz);

		/*
		 * 再把估算值乘 2 并加上 200ms 容差，
		 * 如果发生溢出则使用预定义的最大值。
		 */
		ms += ms + 200;
		if (ms > UINT_MAX)
			ms = UINT_MAX;

		ms = wait_for_completion_timeout(&ctlr->xfer_completion,
						 msecs_to_jiffies(ms));

		if (ms == 0) {
			SPI_STATISTICS_INCREMENT_FIELD(statm, timedout);
			SPI_STATISTICS_INCREMENT_FIELD(stats, timedout);
			dev_err(&msg->spi->dev,
				"SPI transfer timed out\n");
			return -ETIMEDOUT;
		}

		if (xfer->error & SPI_TRANS_FAIL_IO)
			return -EIO;
	}

	return 0;
}

static void _spi_transfer_delay_ns(u32 ns)
{
	if (!ns)
		return;
	if (ns <= NSEC_PER_USEC) {
		ndelay(ns);
	} else {
		u32 us = DIV_ROUND_UP(ns, NSEC_PER_USEC);

		fsleep(us);
	}
}

int spi_delay_to_ns(struct spi_delay *_delay, struct spi_transfer *xfer)
{
	u32 delay = _delay->value;
	u32 unit = _delay->unit;
	u32 hz;

	if (!delay)
		return 0;

	switch (unit) {
	case SPI_DELAY_UNIT_USECS:
		delay *= NSEC_PER_USEC;
		break;
	case SPI_DELAY_UNIT_NSECS:
		/* 这里不需要额外处理。 */
		break;
	case SPI_DELAY_UNIT_SCK:
		/* 时钟周期需要从 spi_transfer 中获取。 */
		if (!xfer)
			return -EINVAL;
		/*
		 * If there is unknown effective speed, approximate it
		 * by underestimating with half of the requested Hz.
		 */
		hz = xfer->effective_speed_hz ?: xfer->speed_hz / 2;
		if (!hz)
			return -EINVAL;

		/* Convert delay to nanoseconds */
		delay *= DIV_ROUND_UP(NSEC_PER_SEC, hz);
		break;
	default:
		return -EINVAL;
	}

	return delay;
}
EXPORT_SYMBOL_GPL(spi_delay_to_ns);

int spi_delay_exec(struct spi_delay *_delay, struct spi_transfer *xfer)
{
	int delay;

	might_sleep();

	if (!_delay)
		return -EINVAL;

	delay = spi_delay_to_ns(_delay, xfer);
	if (delay < 0)
		return delay;

	_spi_transfer_delay_ns(delay);

	return 0;
}
EXPORT_SYMBOL_GPL(spi_delay_exec);

static void _spi_transfer_cs_change_delay(struct spi_message *msg,
					  struct spi_transfer *xfer)
{
	u32 default_delay_ns = 10 * NSEC_PER_USEC;
	u32 delay = xfer->cs_change_delay.value;
	u32 unit = xfer->cs_change_delay.unit;
	int ret;

	/* Return early on "fast" mode - for everything but USECS */
	if (!delay) {
		if (unit == SPI_DELAY_UNIT_USECS)
			_spi_transfer_delay_ns(default_delay_ns);
		return;
	}

	ret = spi_delay_exec(&xfer->cs_change_delay, xfer);
	if (ret) {
		dev_err_once(&msg->spi->dev,
			     "Use of unsupported delay unit %i, using default of %luus\n",
			     unit, default_delay_ns / NSEC_PER_USEC);
		_spi_transfer_delay_ns(default_delay_ns);
	}
}

void spi_transfer_cs_change_delay_exec(struct spi_message *msg,
						  struct spi_transfer *xfer)
{
	_spi_transfer_cs_change_delay(msg, xfer);
}
EXPORT_SYMBOL_GPL(spi_transfer_cs_change_delay_exec);

/*
 * spi_transfer_one_message - transfer_one_message() 的默认实现
 *
 * 这是给只实现了 transfer_one() 的控制器驱动准备的标准
 * transfer_one_message() 实现。它统一处理延迟和 chip select 管理。
 */
static int spi_transfer_one_message(struct spi_controller *ctlr,
				    struct spi_message *msg)
{
	struct spi_transfer *xfer;
	bool keep_cs = false;
	int ret = 0;
	struct spi_statistics __percpu *statm = ctlr->pcpu_statistics;
	struct spi_statistics __percpu *stats = msg->spi->pcpu_statistics;

	xfer = list_first_entry(&msg->transfers, struct spi_transfer, transfer_list);
	spi_set_cs(msg->spi, !xfer->cs_off, false);

	SPI_STATISTICS_INCREMENT_FIELD(statm, messages);
	SPI_STATISTICS_INCREMENT_FIELD(stats, messages);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		trace_spi_transfer_start(msg, xfer);

		spi_statistics_add_transfer_stats(statm, xfer, msg);
		spi_statistics_add_transfer_stats(stats, xfer, msg);

		if (!ctlr->ptp_sts_supported) {
			xfer->ptp_sts_word_pre = 0;
			ptp_read_system_prets(xfer->ptp_sts);
		}

		if ((xfer->tx_buf || xfer->rx_buf) && xfer->len) {
			reinit_completion(&ctlr->xfer_completion);

fallback_pio:
			spi_dma_sync_for_device(ctlr, xfer);
			ret = ctlr->transfer_one(ctlr, msg->spi, xfer);
			if (ret < 0) {
				spi_dma_sync_for_cpu(ctlr, xfer);

				if ((xfer->tx_sg_mapped || xfer->rx_sg_mapped) &&
				    (xfer->error & SPI_TRANS_FAIL_NO_START)) {
					__spi_unmap_msg(ctlr, msg);
					ctlr->fallback = true;
					xfer->error &= ~SPI_TRANS_FAIL_NO_START;
					goto fallback_pio;
				}

				SPI_STATISTICS_INCREMENT_FIELD(statm,
							       errors);
				SPI_STATISTICS_INCREMENT_FIELD(stats,
							       errors);
				dev_err(&msg->spi->dev,
					"SPI transfer failed: %d\n", ret);
				goto out;
			}

			if (ret > 0) {
				ret = spi_transfer_wait(ctlr, msg, xfer);
				if (ret < 0)
					msg->status = ret;
			}

			spi_dma_sync_for_cpu(ctlr, xfer);
		} else {
			if (xfer->len)
				dev_err(&msg->spi->dev,
					"Bufferless transfer has length %u\n",
					xfer->len);
		}

		if (!ctlr->ptp_sts_supported) {
			ptp_read_system_postts(xfer->ptp_sts);
			xfer->ptp_sts_word_post = xfer->len;
		}

		trace_spi_transfer_stop(msg, xfer);

		if (msg->status != -EINPROGRESS)
			goto out;

		spi_transfer_delay_exec(xfer);

		if (xfer->cs_change) {
			if (list_is_last(&xfer->transfer_list,
					 &msg->transfers)) {
				keep_cs = true;
			} else {
				if (!xfer->cs_off)
					spi_set_cs(msg->spi, false, false);
				_spi_transfer_cs_change_delay(msg, xfer);
				if (!list_next_entry(xfer, transfer_list)->cs_off)
					spi_set_cs(msg->spi, true, false);
			}
		} else if (!list_is_last(&xfer->transfer_list, &msg->transfers) &&
			   xfer->cs_off != list_next_entry(xfer, transfer_list)->cs_off) {
			spi_set_cs(msg->spi, xfer->cs_off, false);
		}

		msg->actual_length += xfer->len;
	}

out:
	if (ret != 0 || !keep_cs)
		spi_set_cs(msg->spi, false, false);

	if (msg->status == -EINPROGRESS)
		msg->status = ret;

	if (msg->status && ctlr->handle_err)
		ctlr->handle_err(ctlr, msg);

	spi_finalize_current_message(ctlr);

	return ret;
}

/**
 * spi_finalize_current_transfer - 报告一次传输已经完成
 * @ctlr: 报告完成的控制器
 *
 * 使用 core transfer_one_message() 实现的 SPI 驱动会调用它，
 * 通知 core 当前这个由中断驱动的传输已经结束，下一次传输
 * 可以安排执行了。
 */
void spi_finalize_current_transfer(struct spi_controller *ctlr)
{
	complete(&ctlr->xfer_completion);
}
EXPORT_SYMBOL_GPL(spi_finalize_current_transfer);

static void spi_idle_runtime_pm(struct spi_controller *ctlr)
{
	if (ctlr->auto_runtime_pm) {
		pm_runtime_put_autosuspend(ctlr->dev.parent);
	}
}

static int __spi_pump_transfer_message(struct spi_controller *ctlr,
		struct spi_message *msg, bool was_busy)
{
	struct spi_transfer *xfer;
	int ret;

	if (!was_busy && ctlr->auto_runtime_pm) {
		ret = pm_runtime_get_sync(ctlr->dev.parent);
		if (ret < 0) {
			pm_runtime_put_noidle(ctlr->dev.parent);
			dev_err(&ctlr->dev, "Failed to power device: %d\n",
				ret);

			msg->status = ret;
			spi_finalize_current_message(ctlr);

			return ret;
		}
	}

	if (!was_busy)
		trace_spi_controller_busy(ctlr);

	if (!was_busy && ctlr->prepare_transfer_hardware) {
		ret = ctlr->prepare_transfer_hardware(ctlr);
		if (ret) {
			dev_err(&ctlr->dev,
				"failed to prepare transfer hardware: %d\n",
				ret);

			if (ctlr->auto_runtime_pm)
				pm_runtime_put(ctlr->dev.parent);

			msg->status = ret;
			spi_finalize_current_message(ctlr);

			return ret;
		}
	}

	trace_spi_message_start(msg);

	if (ctlr->prepare_message) {
		ret = ctlr->prepare_message(ctlr, msg);
		if (ret) {
			dev_err(&ctlr->dev, "failed to prepare message: %d\n",
				ret);
			msg->status = ret;
			spi_finalize_current_message(ctlr);
			return ret;
		}
		msg->prepared = true;
	}

	ret = spi_map_msg(ctlr, msg);
	if (ret) {
		msg->status = ret;
		spi_finalize_current_message(ctlr);
		return ret;
	}

	if (!ctlr->ptp_sts_supported && !ctlr->transfer_one) {
		list_for_each_entry(xfer, &msg->transfers, transfer_list) {
			xfer->ptp_sts_word_pre = 0;
			ptp_read_system_prets(xfer->ptp_sts);
		}
	}

		/*
		 * 驱动实现 transfer_one_message() 时，必须安排调用
		 * spi_finalize_current_message()。多数驱动会在当前调用上下文
		 * 中完成这件事，但也有一些不会。对于这些情况，这里会使用
		 * completion，确保在 spi_finalize_current_message() 完成对
		 * ctlr->cur_msg 的访问之前，本函数不会返回。
		 * 下面这两个标志允许在合适情况下跳过 completion，因为
		 * completion 的开销里包含昂贵的自旋锁。
		 * 如果与调用 spi_finalize_current_message() 的上下文发生竞态，
		 * 由于这些标志通过屏障严格排序，最终仍会回退到 completion。
		 */
	WRITE_ONCE(ctlr->cur_msg_incomplete, true);
	WRITE_ONCE(ctlr->cur_msg_need_completion, false);
	reinit_completion(&ctlr->cur_msg_completion);
	smp_wmb(); /* Make these available to spi_finalize_current_message() */

	ret = ctlr->transfer_one_message(ctlr, msg);
	if (ret) {
		dev_err(&ctlr->dev,
			"failed to transfer one message from queue\n");
		return ret;
	}

	WRITE_ONCE(ctlr->cur_msg_need_completion, true);
	smp_mb(); /* See spi_finalize_current_message()... */
	if (READ_ONCE(ctlr->cur_msg_incomplete))
		wait_for_completion(&ctlr->cur_msg_completion);

	return 0;
}

/**
 * __spi_pump_messages - 处理 SPI 消息队列的函数
 * @ctlr: 要处理队列的控制器
 * @in_kthread: 如果当前处于消息泵线程上下文，则为 true
 *
 * 该函数会检查队列里是否有需要处理的 SPI message；如果有，
 * 就调用驱动初始化硬件并依次传输这些 message。
 *
 * 注意，它既可能从 kthread 本身调用，也可能从 spi_sync()
 * 内部调用；函数开头的队列提取逻辑必须同时安全地处理这两种情况。
 */
static void __spi_pump_messages(struct spi_controller *ctlr, bool in_kthread)
{
	struct spi_message *msg;
	bool was_busy = false;
	unsigned long flags;
	int ret;

	/* Take the I/O mutex */
	mutex_lock(&ctlr->io_mutex);

	/* Lock queue */
	spin_lock_irqsave(&ctlr->queue_lock, flags);

	/* Make sure we are not already running a message */
	if (ctlr->cur_msg)
		goto out_unlock;

	/* Check if the queue is idle */
	if (list_empty(&ctlr->queue) || !ctlr->running) {
		if (!ctlr->busy)
			goto out_unlock;

		/* Defer any non-atomic teardown to the thread */
		if (!in_kthread) {
			if (!ctlr->dummy_rx && !ctlr->dummy_tx &&
			    !ctlr->unprepare_transfer_hardware) {
				spi_idle_runtime_pm(ctlr);
				ctlr->busy = false;
				ctlr->queue_empty = true;
				trace_spi_controller_idle(ctlr);
			} else {
				kthread_queue_work(ctlr->kworker,
						   &ctlr->pump_messages);
			}
			goto out_unlock;
		}

		ctlr->busy = false;
		spin_unlock_irqrestore(&ctlr->queue_lock, flags);

		kfree(ctlr->dummy_rx);
		ctlr->dummy_rx = NULL;
		kfree(ctlr->dummy_tx);
		ctlr->dummy_tx = NULL;
		if (ctlr->unprepare_transfer_hardware &&
		    ctlr->unprepare_transfer_hardware(ctlr))
			dev_err(&ctlr->dev,
				"failed to unprepare transfer hardware\n");
		spi_idle_runtime_pm(ctlr);
		trace_spi_controller_idle(ctlr);

		spin_lock_irqsave(&ctlr->queue_lock, flags);
		ctlr->queue_empty = true;
		goto out_unlock;
	}

	/* Extract head of queue */
	msg = list_first_entry(&ctlr->queue, struct spi_message, queue);
	ctlr->cur_msg = msg;

	list_del_init(&msg->queue);
	if (ctlr->busy)
		was_busy = true;
	else
		ctlr->busy = true;
	spin_unlock_irqrestore(&ctlr->queue_lock, flags);

	ret = __spi_pump_transfer_message(ctlr, msg, was_busy);
	kthread_queue_work(ctlr->kworker, &ctlr->pump_messages);

	ctlr->cur_msg = NULL;
	ctlr->fallback = false;

	mutex_unlock(&ctlr->io_mutex);

	/* 如果 transfer_one() 一直忙等，就唤醒调度器。 */
	if (!ret)
		cond_resched();
	return;

out_unlock:
	spin_unlock_irqrestore(&ctlr->queue_lock, flags);
	mutex_unlock(&ctlr->io_mutex);
}

/**
 * spi_pump_messages - 处理 SPI 消息队列的 kthread 工作函数
 * @work: 控制器结构体中包含的 kthread work 指针
 */
static void spi_pump_messages(struct kthread_work *work)
{
	struct spi_controller *ctlr =
		container_of(work, struct spi_controller, pump_messages);

	__spi_pump_messages(ctlr, true);
}

/**
 * spi_take_timestamp_pre - 采集发送时间戳开始点的辅助函数
 * @ctlr: 驱动的 spi_controller 结构体指针
 * @xfer: 需要采样时间戳的传输
 * @progress: 当前已经传输了多少个 word（不是 byte）
 * @irqs_off: 如果为 true，则在整个采样期间关闭 IRQ 和抢占，
 *	      以减小时间测量抖动。该模式只适用于 PIO 驱动。
 *	      若启用，则必须随后调用 spi_take_timestamp_post，
 *	      否则系统可能崩溃。
 *	      警告：如果要获得完全可预测的结果，还必须把 CPU
 *	      频率控制住（governor）。
 *
 * 这是驱动用来采集 SPI 传输中请求字节对应发送时间戳开始点的辅助函数。
 * 调用频率可以按 word、按整条传输，或者按一批 word 来定，只要在调用
 * 时 @tx 缓冲区偏移已经大于或等于所请求的字节即可。时间戳只会在第一次
 * 满足条件的调用时采样一次。这里假定驱动会单调地推进 @tx 缓冲区指针。
 */
void spi_take_timestamp_pre(struct spi_controller *ctlr,
			    struct spi_transfer *xfer,
			    size_t progress, bool irqs_off)
{
	if (!xfer->ptp_sts)
		return;

	if (xfer->timestamped)
		return;

	if (progress > xfer->ptp_sts_word_pre)
		return;

	/* Capture the resolution of the timestamp */
	xfer->ptp_sts_word_pre = progress;

	if (irqs_off) {
		local_irq_save(ctlr->irq_flags);
		preempt_disable();
	}

	ptp_read_system_prets(xfer->ptp_sts);
}
EXPORT_SYMBOL_GPL(spi_take_timestamp_pre);

/**
 * spi_take_timestamp_post - 采集发送时间戳结束点的辅助函数
 * @ctlr: 驱动的 spi_controller 结构体指针
 * @xfer: 需要采样时间戳的传输
 * @progress: 当前已经传输了多少个 word（不是 byte）
 * @irqs_off: 如果为 true，则重新为本地 CPU 使能 IRQ 和抢占。
 *
 * 这是驱动用来采集 SPI 传输中请求字节对应发送时间戳结束点的辅助函数。
 * 调用频率可以任意：只有第一次满足 @tx 已经超过或等于目标 word 的调用
 * 会真正采样时间戳。
 */
void spi_take_timestamp_post(struct spi_controller *ctlr,
			     struct spi_transfer *xfer,
			     size_t progress, bool irqs_off)
{
	if (!xfer->ptp_sts)
		return;

	if (xfer->timestamped)
		return;

	if (progress < xfer->ptp_sts_word_post)
		return;

	ptp_read_system_postts(xfer->ptp_sts);

	if (irqs_off) {
		local_irq_restore(ctlr->irq_flags);
		preempt_enable();
	}

	/* Capture the resolution of the timestamp */
	xfer->ptp_sts_word_post = progress;

	xfer->timestamped = 1;
}
EXPORT_SYMBOL_GPL(spi_take_timestamp_post);

/**
 * spi_set_thread_rt - 将控制器消息泵设置为实时优先级
 * @ctlr: 需要提升优先级的控制器
 *
 * 可以在控制器请求实时优先级时调用（即在调用
 * spi_register_controller() 之前把 ->rt 设为 true），也可以在
 * 总线上的某个设备声明其传输需要实时优先级时调用。
 *
 * 注意：目前只要总线上有任何设备要求实时优先级，那么该控制器上
 * 的所有传输都会由实时优先级线程处理。如果以后这成为问题，可能
 * 需要寻找一种只在相关传输期间临时提升优先级的方法。
 */
static void spi_set_thread_rt(struct spi_controller *ctlr)
{
	dev_info(&ctlr->dev,
		"will run message pump with realtime priority\n");
	sched_set_fifo(ctlr->kworker->task);
}

static int spi_init_queue(struct spi_controller *ctlr)
{
	ctlr->running = false;
	ctlr->busy = false;
	ctlr->queue_empty = true;

	ctlr->kworker = kthread_run_worker(0, dev_name(&ctlr->dev));
	if (IS_ERR(ctlr->kworker)) {
		dev_err(&ctlr->dev, "failed to create message pump kworker\n");
		return PTR_ERR(ctlr->kworker);
	}

	kthread_init_work(&ctlr->pump_messages, spi_pump_messages);

	/*
	 * 控制器配置会指明是否应该让消息泵以高优先级（实时）
	 * 运行，以通过尽量缩短传输请求和消息泵线程调度之间的
	 * 延迟来降低总线传输延迟。如果没有这个设置，消息泵线程
	 * 会保持默认优先级。
	 */
	if (ctlr->rt)
		spi_set_thread_rt(ctlr);

	return 0;
}

/**
 * spi_get_next_queued_message() - 由驱动调用，用于检查队列中的消息
 * @ctlr: 要检查消息队列的控制器
 *
 * 如果队列中还有消息，这个接口会返回下一条消息。
 *
 * 返回：
 * 队列中的下一条消息；如果队列为空则返回 NULL。
 */
struct spi_message *spi_get_next_queued_message(struct spi_controller *ctlr)
{
	struct spi_message *next;
	unsigned long flags;

	/* 获取下一条消息的指针（如果存在）。 */
	spin_lock_irqsave(&ctlr->queue_lock, flags);
	next = list_first_entry_or_null(&ctlr->queue, struct spi_message,
					queue);
	spin_unlock_irqrestore(&ctlr->queue_lock, flags);

	return next;
}
EXPORT_SYMBOL_GPL(spi_get_next_queued_message);

/*
 * __spi_unoptimize_message - shared implementation of spi_unoptimize_message()
 *                            and spi_maybe_unoptimize_message()
 * @msg: the message to unoptimize
 *
 * 外设驱动应使用 spi_unoptimize_message()，而 core 内部调用者应
 * 使用 spi_maybe_unoptimize_message()，不要直接调用这个函数。
 *
 * 不能对当前并未处于优化状态的消息调用这个函数。
 */
static void __spi_unoptimize_message(struct spi_message *msg)
{
	struct spi_controller *ctlr = msg->spi->controller;

	if (ctlr->unoptimize_message)
		ctlr->unoptimize_message(msg);

	spi_res_release(ctlr, msg);

	msg->optimized = false;
	msg->opt_state = NULL;
}

/*
 * spi_maybe_unoptimize_message - 仅在消息由 core 优化时才执行反优化
 * @msg: 需要反优化的消息
 *
 * 该函数只会在消息确实由 core 通过 spi_maybe_optimize_message()
 * 优化过时，才对其执行反优化。
 */
static void spi_maybe_unoptimize_message(struct spi_message *msg)
{
	if (!msg->pre_optimized && msg->optimized &&
	    !msg->spi->controller->defer_optimize_message)
		__spi_unoptimize_message(msg);
}

/**
 * spi_finalize_current_message() - 当前消息已经完成
 * @ctlr: 需要把消息归还给的控制器
 *
 * 驱动调用它来通知 core：队列头部的这条消息已经完成，
 * 可以从队列中移除了。
 */
void spi_finalize_current_message(struct spi_controller *ctlr)
{
	struct spi_transfer *xfer;
	struct spi_message *mesg;
	int ret;

	mesg = ctlr->cur_msg;

	if (!ctlr->ptp_sts_supported && !ctlr->transfer_one) {
		list_for_each_entry(xfer, &mesg->transfers, transfer_list) {
			ptp_read_system_postts(xfer->ptp_sts);
			xfer->ptp_sts_word_post = xfer->len;
		}
	}

	if (unlikely(ctlr->ptp_sts_supported))
		list_for_each_entry(xfer, &mesg->transfers, transfer_list)
			WARN_ON_ONCE(xfer->ptp_sts && !xfer->timestamped);

	spi_unmap_msg(ctlr, mesg);

	if (mesg->prepared && ctlr->unprepare_message) {
		ret = ctlr->unprepare_message(ctlr, mesg);
		if (ret) {
			dev_err(&ctlr->dev, "failed to unprepare message: %d\n",
				ret);
		}
	}

	mesg->prepared = false;

	spi_maybe_unoptimize_message(mesg);

	WRITE_ONCE(ctlr->cur_msg_incomplete, false);
	smp_mb(); /* 参见 __spi_pump_transfer_message()... */
	if (READ_ONCE(ctlr->cur_msg_need_completion))
		complete(&ctlr->cur_msg_completion);

	trace_spi_message_done(mesg);

	mesg->state = NULL;
	if (mesg->complete)
		mesg->complete(mesg->context);
}
EXPORT_SYMBOL_GPL(spi_finalize_current_message);

static int spi_start_queue(struct spi_controller *ctlr)
{
	unsigned long flags;

	spin_lock_irqsave(&ctlr->queue_lock, flags);

	if (ctlr->running || ctlr->busy) {
		spin_unlock_irqrestore(&ctlr->queue_lock, flags);
		return -EBUSY;
	}

	ctlr->running = true;
	ctlr->cur_msg = NULL;
	spin_unlock_irqrestore(&ctlr->queue_lock, flags);

	kthread_queue_work(ctlr->kworker, &ctlr->pump_messages);

	return 0;
}

static int spi_stop_queue(struct spi_controller *ctlr)
{
	unsigned int limit = 500;
	unsigned long flags;

	/*
	 * 这个实现不算优雅，但它针对常见执行路径做了优化。
	 * 也可以在 ctlr->busy 上使用 wait_queue，但那样的话，常见路径
	 * （pump_messages）就必须在每条 SPI message 上调用 wake_up 或
	 * 类似函数。这里选择了另一种实现方式。
	 */
	do {
		spin_lock_irqsave(&ctlr->queue_lock, flags);
		if (list_empty(&ctlr->queue) && !ctlr->busy) {
			ctlr->running = false;
			spin_unlock_irqrestore(&ctlr->queue_lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&ctlr->queue_lock, flags);
		usleep_range(10000, 11000);
	} while (--limit);

	return -EBUSY;
}

static int spi_destroy_queue(struct spi_controller *ctlr)
{
	int ret;

	ret = spi_stop_queue(ctlr);

	/*
	 * kthread_flush_worker 会一直阻塞到所有 work 执行完毕。
	 * 如果 stop_queue 超时的原因是 work 根本不会结束，
	 * 那么此时再去 flush/stop 线程也没有意义，所以直接返回即可。
	 */
	if (ret) {
		dev_err(&ctlr->dev, "problem destroying queue\n");
		return ret;
	}

	kthread_destroy_worker(ctlr->kworker);

	return 0;
}

static int __spi_queued_transfer(struct spi_device *spi,
				 struct spi_message *msg,
				 bool need_pump)
{
	struct spi_controller *ctlr = spi->controller;
	unsigned long flags;

	spin_lock_irqsave(&ctlr->queue_lock, flags);

	if (!ctlr->running) {
		spin_unlock_irqrestore(&ctlr->queue_lock, flags);
		return -ESHUTDOWN;
	}
	msg->actual_length = 0;
	msg->status = -EINPROGRESS;

	list_add_tail(&msg->queue, &ctlr->queue);
	ctlr->queue_empty = false;
	if (!ctlr->busy && need_pump)
		kthread_queue_work(ctlr->kworker, &ctlr->pump_messages);

	spin_unlock_irqrestore(&ctlr->queue_lock, flags);
	return 0;
}

/**
 * spi_queued_transfer - 队列化传输使用的传输函数
 * @spi: 请求传输的 SPI 设备
 * @msg: 将要交给驱动队列处理的 SPI 消息
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static int spi_queued_transfer(struct spi_device *spi, struct spi_message *msg)
{
	return __spi_queued_transfer(spi, msg, true);
}

static int spi_controller_initialize_queue(struct spi_controller *ctlr)
{
	int ret;

	ctlr->transfer = spi_queued_transfer;
	if (!ctlr->transfer_one_message)
		ctlr->transfer_one_message = spi_transfer_one_message;

	/* Initialize and start queue */
	ret = spi_init_queue(ctlr);
	if (ret) {
		dev_err(&ctlr->dev, "problem initializing queue\n");
		goto err_init_queue;
	}
	ctlr->queued = true;
	ret = spi_start_queue(ctlr);
	if (ret) {
		dev_err(&ctlr->dev, "problem starting queue\n");
		goto err_start_queue;
	}

	return 0;

err_start_queue:
	spi_destroy_queue(ctlr);
err_init_queue:
	return ret;
}

/**
 * spi_flush_queue - 从调用者上下文发送完队列里所有待处理消息
 * @ctlr: 要处理队列的控制器
 *
 * 当希望在执行某个操作之前，确保所有待处理消息都已经发出时，
 * 应该使用这个接口。spi-mem 代码会使用它，确保 SPI memory 操作
 * 不会抢占那些在它之前已经排队的普通 SPI 传输。
 */
void spi_flush_queue(struct spi_controller *ctlr)
{
	if (ctlr->transfer == spi_queued_transfer)
		__spi_pump_messages(ctlr, false);
}

/*-------------------------------------------------------------------------*/

#if defined(CONFIG_OF)
static void of_spi_parse_dt_cs_delay(struct device_node *nc,
				     struct spi_delay *delay, const char *prop)
{
	u32 value;

	if (!of_property_read_u32(nc, prop, &value)) {
		if (value > U16_MAX) {
			delay->value = DIV_ROUND_UP(value, 1000);
			delay->unit = SPI_DELAY_UNIT_USECS;
		} else {
			delay->value = value;
			delay->unit = SPI_DELAY_UNIT_NSECS;
		}
	}
}

static int of_spi_parse_dt(struct spi_controller *ctlr, struct spi_device *spi,
			   struct device_node *nc)
{
	u32 value, cs[SPI_DEVICE_CS_CNT_MAX], map[SPI_DEVICE_DATA_LANE_CNT_MAX];
	int rc, idx, max_num_data_lanes;

	/* Mode (clock phase/polarity/etc.) */
	if (of_property_read_bool(nc, "spi-cpha"))
		spi->mode |= SPI_CPHA;
	if (of_property_read_bool(nc, "spi-cpol"))
		spi->mode |= SPI_CPOL;
	if (of_property_read_bool(nc, "spi-3wire"))
		spi->mode |= SPI_3WIRE;
	if (of_property_read_bool(nc, "spi-lsb-first"))
		spi->mode |= SPI_LSB_FIRST;
	if (of_property_read_bool(nc, "spi-cs-high"))
		spi->mode |= SPI_CS_HIGH;

	/* Device DUAL/QUAD mode */

	rc = of_property_read_variable_u32_array(nc, "spi-tx-lane-map", map, 1,
						 ARRAY_SIZE(map));
	if (rc >= 0) {
		max_num_data_lanes = rc;
		for (idx = 0; idx < max_num_data_lanes; idx++)
			spi->tx_lane_map[idx] = map[idx];
	} else if (rc == -EINVAL) {
		/* Default lane map is identity mapping. */
		max_num_data_lanes = ARRAY_SIZE(spi->tx_lane_map);
		for (idx = 0; idx < max_num_data_lanes; idx++)
			spi->tx_lane_map[idx] = idx;
	} else {
		dev_err(&ctlr->dev,
			"failed to read spi-tx-lane-map property: %d\n", rc);
		return rc;
	}

	rc = of_property_count_u32_elems(nc, "spi-tx-bus-width");
	if (rc < 0 && rc != -EINVAL) {
		dev_err(&ctlr->dev,
			"failed to read spi-tx-bus-width property: %d\n", rc);
		return rc;
	}
	if (rc > max_num_data_lanes) {
		dev_err(&ctlr->dev,
			"spi-tx-bus-width has more elements (%d) than spi-tx-lane-map (%d)\n",
			rc, max_num_data_lanes);
		return -EINVAL;
	}

	if (rc == -EINVAL) {
		/* Default when property is not present. */
		spi->num_tx_lanes = 1;
	} else {
		u32 first_value;

		spi->num_tx_lanes = rc;

		for (idx = 0; idx < spi->num_tx_lanes; idx++) {
			rc = of_property_read_u32_index(nc, "spi-tx-bus-width",
							idx, &value);
			if (rc)
				return rc;

			/*
			 * For now, we only support all lanes having the same
			 * width so we can keep using the existing mode flags.
			 */
			if (!idx)
				first_value = value;
			else if (first_value != value) {
				dev_err(&ctlr->dev,
					"spi-tx-bus-width has inconsistent values: first %d vs later %d\n",
					first_value, value);
				return -EINVAL;
			}
		}

		switch (value) {
		case 0:
			spi->mode |= SPI_NO_TX;
			break;
		case 1:
			break;
		case 2:
			spi->mode |= SPI_TX_DUAL;
			break;
		case 4:
			spi->mode |= SPI_TX_QUAD;
			break;
		case 8:
			spi->mode |= SPI_TX_OCTAL;
			break;
		default:
			dev_warn(&ctlr->dev,
				"spi-tx-bus-width %d not supported\n",
				value);
			break;
		}
	}

	for (idx = 0; idx < spi->num_tx_lanes; idx++) {
		if (spi->tx_lane_map[idx] >= spi->controller->num_data_lanes) {
			dev_err(&ctlr->dev,
				"spi-tx-lane-map has invalid value %d (num_data_lanes=%d)\n",
				spi->tx_lane_map[idx],
				spi->controller->num_data_lanes);
			return -EINVAL;
		}
	}

	rc = of_property_read_variable_u32_array(nc, "spi-rx-lane-map", map, 1,
						 ARRAY_SIZE(map));
	if (rc >= 0) {
		max_num_data_lanes = rc;
		for (idx = 0; idx < max_num_data_lanes; idx++)
			spi->rx_lane_map[idx] = map[idx];
	} else if (rc == -EINVAL) {
		/* Default lane map is identity mapping. */
		max_num_data_lanes = ARRAY_SIZE(spi->rx_lane_map);
		for (idx = 0; idx < max_num_data_lanes; idx++)
			spi->rx_lane_map[idx] = idx;
	} else {
		dev_err(&ctlr->dev,
			"failed to read spi-rx-lane-map property: %d\n", rc);
		return rc;
	}

	rc = of_property_count_u32_elems(nc, "spi-rx-bus-width");
	if (rc < 0 && rc != -EINVAL) {
		dev_err(&ctlr->dev,
			"failed to read spi-rx-bus-width property: %d\n", rc);
		return rc;
	}
	if (rc > max_num_data_lanes) {
		dev_err(&ctlr->dev,
			"spi-rx-bus-width has more elements (%d) than spi-rx-lane-map (%d)\n",
			rc, max_num_data_lanes);
		return -EINVAL;
	}

	if (rc == -EINVAL) {
		/* Default when property is not present. */
		spi->num_rx_lanes = 1;
	} else {
		u32 first_value;

		spi->num_rx_lanes = rc;

		for (idx = 0; idx < spi->num_rx_lanes; idx++) {
			rc = of_property_read_u32_index(nc, "spi-rx-bus-width",
							idx, &value);
			if (rc)
				return rc;

			/*
			 * For now, we only support all lanes having the same
			 * width so we can keep using the existing mode flags.
			 */
			if (!idx)
				first_value = value;
			else if (first_value != value) {
				dev_err(&ctlr->dev,
					"spi-rx-bus-width has inconsistent values: first %d vs later %d\n",
					first_value, value);
				return -EINVAL;
			}
		}

		switch (value) {
		case 0:
			spi->mode |= SPI_NO_RX;
			break;
		case 1:
			break;
		case 2:
			spi->mode |= SPI_RX_DUAL;
			break;
		case 4:
			spi->mode |= SPI_RX_QUAD;
			break;
		case 8:
			spi->mode |= SPI_RX_OCTAL;
			break;
		default:
			dev_warn(&ctlr->dev,
				"spi-rx-bus-width %d not supported\n",
				value);
			break;
		}
	}

	for (idx = 0; idx < spi->num_rx_lanes; idx++) {
		if (spi->rx_lane_map[idx] >= spi->controller->num_data_lanes) {
			dev_err(&ctlr->dev,
				"spi-rx-lane-map has invalid value %d (num_data_lanes=%d)\n",
				spi->rx_lane_map[idx],
				spi->controller->num_data_lanes);
			return -EINVAL;
		}
	}

	if (spi_controller_is_target(ctlr)) {
		if (!of_node_name_eq(nc, "slave")) {
			dev_err(&ctlr->dev, "%pOF is not called 'slave'\n",
				nc);
			return -EINVAL;
		}
		return 0;
	}

	/* Device address */
	rc = of_property_read_variable_u32_array(nc, "reg", &cs[0], 1,
						 SPI_DEVICE_CS_CNT_MAX);
	if (rc < 0) {
		dev_err(&ctlr->dev, "%pOF has no valid 'reg' property (%d)\n",
			nc, rc);
		return rc;
	}

	if ((of_property_present(nc, "parallel-memories")) &&
	    (!(ctlr->flags & SPI_CONTROLLER_MULTI_CS))) {
		dev_err(&ctlr->dev, "SPI controller doesn't support multi CS\n");
		return -EINVAL;
	}

	spi->num_chipselect = rc;
	for (idx = 0; idx < rc; idx++)
		spi_set_chipselect(spi, idx, cs[idx]);

	/*
	 * By default spi->chip_select[0] will hold the physical CS number,
	 * so set bit 0 in spi->cs_index_mask.
	 */
	spi->cs_index_mask = BIT(0);

	/* Device speed */
	if (!of_property_read_u32(nc, "spi-max-frequency", &value))
		spi->max_speed_hz = value;

	/* Device CS delays */
	of_spi_parse_dt_cs_delay(nc, &spi->cs_setup, "spi-cs-setup-delay-ns");
	of_spi_parse_dt_cs_delay(nc, &spi->cs_hold, "spi-cs-hold-delay-ns");
	of_spi_parse_dt_cs_delay(nc, &spi->cs_inactive, "spi-cs-inactive-delay-ns");

	return 0;
}

static struct spi_device *
of_register_spi_device(struct spi_controller *ctlr, struct device_node *nc)
{
	struct spi_device *spi;
	int rc;

	/* Alloc an spi_device */
	spi = spi_alloc_device(ctlr);
	if (!spi) {
		dev_err(&ctlr->dev, "spi_device alloc error for %pOF\n", nc);
		rc = -ENOMEM;
		goto err_out;
	}

	/* 选择设备驱动。 */
	rc = of_alias_from_compatible(nc, spi->modalias,
				      sizeof(spi->modalias));
	if (rc < 0) {
		dev_err(&ctlr->dev, "cannot find modalias for %pOF\n", nc);
		goto err_out;
	}

	rc = of_spi_parse_dt(ctlr, spi, nc);
	if (rc)
		goto err_out;

	/* Store a pointer to the node in the device structure */
	of_node_get(nc);

	device_set_node(&spi->dev, of_fwnode_handle(nc));

	/* Register the new device */
	rc = spi_add_device(spi);
	if (rc) {
		dev_err(&ctlr->dev, "spi_device register error %pOF\n", nc);
		goto err_of_node_put;
	}

	return spi;

err_of_node_put:
	of_node_put(nc);
err_out:
	spi_dev_put(spi);
	return ERR_PTR(rc);
}

/**
 * of_register_spi_devices() - 把子设备注册到 SPI 总线上
 * @ctlr:	spi_controller 设备指针
 *
 * 为控制器节点下每一个表示有效 SPI 目标设备的子节点注册一个
 * spi_device。
 */
static void of_register_spi_devices(struct spi_controller *ctlr)
{
	struct spi_device *spi;
	struct device_node *nc;

	for_each_available_child_of_node(ctlr->dev.of_node, nc) {
		if (of_node_test_and_set_flag(nc, OF_POPULATED))
			continue;
		spi = of_register_spi_device(ctlr, nc);
		if (IS_ERR(spi)) {
			dev_warn(&ctlr->dev,
				 "Failed to create SPI device for %pOF\n", nc);
			of_node_clear_flag(nc, OF_POPULATED);
		}
	}
}
#else
static void of_register_spi_devices(struct spi_controller *ctlr) { }
#endif

/**
 * spi_new_ancillary_device() - 注册辅助 SPI 设备
 * @spi:         注册该辅助设备的主 SPI 设备指针
 * @chip_select: 辅助设备的 chip select
 *
 * 注册一个辅助 SPI 设备；例如有些芯片会有一个 chip select
 * 用于正常工作，另一个则用于初始化或固件下载。
 *
 * 这个接口只能在主 SPI 设备的 probe 过程中调用。
 *
 * 返回：
 * 成功返回 0，失败返回负 errno。
 */
struct spi_device *spi_new_ancillary_device(struct spi_device *spi,
					     u8 chip_select)
{
	struct spi_controller *ctlr = spi->controller;
	struct spi_device *ancillary;
	int rc;

	/* 分配一个 spi_device。 */
	ancillary = spi_alloc_device(ctlr);
	if (!ancillary) {
		rc = -ENOMEM;
		goto err_out;
	}

	strscpy(ancillary->modalias, "dummy", sizeof(ancillary->modalias));

	/* 辅助设备使用传入的 chip-select。 */
	spi_set_chipselect(ancillary, 0, chip_select);

	/* 继承主 SPI 设备的 mode / 速率配置。 */
	ancillary->max_speed_hz = spi->max_speed_hz;
	ancillary->mode = spi->mode;
	/*
	 * 默认情况下 spi->chip_select[0] 保存物理 CS 编号，
	 * 因此要在 spi->cs_index_mask 中设置 bit 0。
	 */
	ancillary->cs_index_mask = BIT(0);

	WARN_ON(!mutex_is_locked(&ctlr->add_lock));

	/*
	 * 注册这个新设备，并传入 parent 以跳过与父设备的
	 * chip select 冲突检查。
	 */
	rc = __spi_add_device(ancillary, spi);
	if (rc) {
		dev_err(&spi->dev, "failed to register ancillary device\n");
		goto err_out;
	}

	return ancillary;

err_out:
	spi_dev_put(ancillary);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL_GPL(spi_new_ancillary_device);

static void devm_spi_unregister_device(void *spi)
{
	spi_unregister_device(spi);
}

/**
 * devm_spi_new_ancillary_device() - 注册受管理的辅助 SPI 设备
 * @spi:         注册该辅助设备的主 SPI 设备指针
 * @chip_select: 辅助设备的 chip select
 *
 * 注册一个辅助 SPI 设备；例如有些芯片会有一个 chip select
 * 用于正常工作，另一个则用于初始化或固件下载。
 *
 * 这是 spi_new_ancillary_device() 的受管理版本。主 SPI 设备注销时，
 * 该辅助设备也会被自动注销。
 *
 * 这个接口只能在主 SPI 设备的 probe 过程中调用。
 *
 * 返回：
 * 成功返回新的辅助设备指针；失败返回 ERR_PTR。
 */
struct spi_device *devm_spi_new_ancillary_device(struct spi_device *spi,
						 u8 chip_select)
{
	struct spi_device *ancillary;
	int ret;

	ancillary = spi_new_ancillary_device(spi, chip_select);
	if (IS_ERR(ancillary))
		return ancillary;

	ret = devm_add_action_or_reset(&spi->dev, devm_spi_unregister_device,
				       ancillary);
	if (ret)
		return ERR_PTR(ret);

	return ancillary;
}
EXPORT_SYMBOL_GPL(devm_spi_new_ancillary_device);

#ifdef CONFIG_ACPI
struct acpi_spi_lookup {
	struct spi_controller 	*ctlr;
	u32			max_speed_hz;
	u32			mode;
	int			irq;
	u8			bits_per_word;
	u8			chip_select;
	int			n;
	int			index;
};

static int acpi_spi_count(struct acpi_resource *ares, void *data)
{
	struct acpi_resource_spi_serialbus *sb;
	int *count = data;

	if (ares->type != ACPI_RESOURCE_TYPE_SERIAL_BUS)
		return 1;

	sb = &ares->data.spi_serial_bus;
	if (sb->type != ACPI_RESOURCE_SERIAL_TYPE_SPI)
		return 1;

	*count = *count + 1;

	return 1;
}

/**
 * acpi_spi_count_resources - 统计 SpiSerialBus 资源数量
 * @adev:	ACPI 设备
 *
 * 返回：
 * ACPI 设备资源列表中的 SpiSerialBus 数量；或者返回负错误码。
 */
int acpi_spi_count_resources(struct acpi_device *adev)
{
	LIST_HEAD(r);
	int count = 0;
	int ret;

	ret = acpi_dev_get_resources(adev, &r, acpi_spi_count, &count);
	if (ret < 0)
		return ret;

	acpi_dev_free_resource_list(&r);

	return count;
}
EXPORT_SYMBOL_GPL(acpi_spi_count_resources);

static void acpi_spi_parse_apple_properties(struct acpi_device *dev,
					    struct acpi_spi_lookup *lookup)
{
	const union acpi_object *obj;

	if (!x86_apple_machine)
		return;

	if (!acpi_dev_get_property(dev, "spiSclkPeriod", ACPI_TYPE_BUFFER, &obj)
	    && obj->buffer.length >= 4)
		lookup->max_speed_hz  = NSEC_PER_SEC / *(u32 *)obj->buffer.pointer;

	if (!acpi_dev_get_property(dev, "spiWordSize", ACPI_TYPE_BUFFER, &obj)
	    && obj->buffer.length == 8)
		lookup->bits_per_word = *(u64 *)obj->buffer.pointer;

	if (!acpi_dev_get_property(dev, "spiBitOrder", ACPI_TYPE_BUFFER, &obj)
	    && obj->buffer.length == 8 && !*(u64 *)obj->buffer.pointer)
		lookup->mode |= SPI_LSB_FIRST;

	if (!acpi_dev_get_property(dev, "spiSPO", ACPI_TYPE_BUFFER, &obj)
	    && obj->buffer.length == 8 &&  *(u64 *)obj->buffer.pointer)
		lookup->mode |= SPI_CPOL;

	if (!acpi_dev_get_property(dev, "spiSPH", ACPI_TYPE_BUFFER, &obj)
	    && obj->buffer.length == 8 &&  *(u64 *)obj->buffer.pointer)
		lookup->mode |= SPI_CPHA;
}

static int acpi_spi_add_resource(struct acpi_resource *ares, void *data)
{
	struct acpi_spi_lookup *lookup = data;
	struct spi_controller *ctlr = lookup->ctlr;

	if (ares->type == ACPI_RESOURCE_TYPE_SERIAL_BUS) {
		struct acpi_resource_spi_serialbus *sb;
		acpi_handle parent_handle;
		acpi_status status;

		sb = &ares->data.spi_serial_bus;
		if (sb->type == ACPI_RESOURCE_SERIAL_TYPE_SPI) {

			if (lookup->index != -1 && lookup->n++ != lookup->index)
				return 1;

			status = acpi_get_handle(NULL,
						 sb->resource_source.string_ptr,
						 &parent_handle);

			if (ACPI_FAILURE(status))
				return -ENODEV;

			if (ctlr) {
				if (!device_match_acpi_handle(ctlr->dev.parent, parent_handle))
					return -ENODEV;
			} else {
				struct acpi_device *adev;

				adev = acpi_fetch_acpi_dev(parent_handle);
				if (!adev)
					return -ENODEV;

				ctlr = acpi_spi_find_controller_by_adev(adev);
				if (!ctlr)
					return -EPROBE_DEFER;

				lookup->ctlr = ctlr;
			}

			/*
			 * ACPI 的 DeviceSelection 编号方式由 Windows 中的 host
			 * controller driver 决定，而且不同驱动之间可能不一致。
			 * 在 Linux 中，我们始终期望使用 0 .. max - 1 这套编号，
			 * 因此需要让驱动在两套方案之间做转换。
			 */
			if (ctlr->fw_translate_cs) {
				int cs = ctlr->fw_translate_cs(ctlr,
						sb->device_selection);
				if (cs < 0)
					return cs;
				lookup->chip_select = cs;
			} else {
				lookup->chip_select = sb->device_selection;
			}

			lookup->max_speed_hz = sb->connection_speed;
			lookup->bits_per_word = sb->data_bit_length;

			if (sb->clock_phase == ACPI_SPI_SECOND_PHASE)
				lookup->mode |= SPI_CPHA;
			if (sb->clock_polarity == ACPI_SPI_START_HIGH)
				lookup->mode |= SPI_CPOL;
			if (sb->device_polarity == ACPI_SPI_ACTIVE_HIGH)
				lookup->mode |= SPI_CS_HIGH;
		}
	} else if (lookup->irq < 0) {
		struct resource r;

		if (acpi_dev_resource_interrupt(ares, 0, &r))
			lookup->irq = r.start;
	}

	/* Always tell the ACPI core to skip this resource */
	return 1;
}

/**
 * acpi_spi_device_alloc - 分配一个 SPI 设备并填入 ACPI 信息
 * @ctlr: 该 SPI 设备所属的控制器
 * @adev: 该 SPI 设备对应的 ACPI 设备
 * @index: ACPI 节点中 SPI 资源的索引
 *
 * 应使用这个接口从 ACPI 设备节点中分配一个新的 SPI 设备。
 * 调用者负责随后调用 spi_add_device() 来注册该 SPI 设备。
 *
 * 如果 ctlr 设为 NULL，则会根据资源反查该 SPI 设备所属的控制器。
 * 如果 index 设为 -1，则不使用索引。
 * 注意：如果 index 为 -1，则必须设置 ctlr。
 *
 * 返回：
 * 成功时返回新设备指针；出错时返回 ERR_PTR。
 */
struct spi_device *acpi_spi_device_alloc(struct spi_controller *ctlr,
					 struct acpi_device *adev,
					 int index)
{
	acpi_handle parent_handle = NULL;
	struct list_head resource_list;
	struct acpi_spi_lookup lookup = {};
	struct spi_device *spi;
	int ret;

	if (!ctlr && index == -1)
		return ERR_PTR(-EINVAL);

	lookup.ctlr		= ctlr;
	lookup.irq		= -1;
	lookup.index		= index;
	lookup.n		= 0;

	INIT_LIST_HEAD(&resource_list);
	ret = acpi_dev_get_resources(adev, &resource_list,
				     acpi_spi_add_resource, &lookup);
	acpi_dev_free_resource_list(&resource_list);

	if (ret < 0)
		/* Found SPI in _CRS but it points to another controller */
		return ERR_PTR(ret);

	if (!lookup.max_speed_hz &&
	    ACPI_SUCCESS(acpi_get_parent(adev->handle, &parent_handle)) &&
	    device_match_acpi_handle(lookup.ctlr->dev.parent, parent_handle)) {
		/* Apple does not use _CRS but nested devices for SPI target devices */
		acpi_spi_parse_apple_properties(adev, &lookup);
	}

	if (!lookup.max_speed_hz)
		return ERR_PTR(-ENODEV);

	spi = spi_alloc_device(lookup.ctlr);
	if (!spi) {
		dev_err(&lookup.ctlr->dev, "failed to allocate SPI device for %s\n",
			dev_name(&adev->dev));
		return ERR_PTR(-ENOMEM);
	}

	spi_set_chipselect(spi, 0, lookup.chip_select);

	ACPI_COMPANION_SET(&spi->dev, adev);
	spi->max_speed_hz	= lookup.max_speed_hz;
	spi->mode		|= lookup.mode;
	spi->irq		= lookup.irq;
	spi->bits_per_word	= lookup.bits_per_word;
	/*
	 * By default spi->chip_select[0] will hold the physical CS number,
	 * so set bit 0 in spi->cs_index_mask.
	 */
	spi->cs_index_mask	= BIT(0);

	return spi;
}
EXPORT_SYMBOL_GPL(acpi_spi_device_alloc);

static acpi_status acpi_register_spi_device(struct spi_controller *ctlr,
					    struct acpi_device *adev)
{
	struct spi_device *spi;

	if (acpi_bus_get_status(adev) || !adev->status.present ||
	    acpi_device_enumerated(adev))
		return AE_OK;

	spi = acpi_spi_device_alloc(ctlr, adev, -1);
	if (IS_ERR(spi)) {
		if (PTR_ERR(spi) == -ENOMEM)
			return AE_NO_MEMORY;
		else
			return AE_OK;
	}

	acpi_set_modalias(adev, acpi_device_hid(adev), spi->modalias,
			  sizeof(spi->modalias));

	/*
	 * 这里会在 spi_probe() 中为了处理 -EPROBE_DEFER 再重试一次，
	 * 因为 GPIO 控制器此时可能还没有驱动。
	 * 之所以必须在这里做，是因为这个调用会设置 GPIO 方向和/或偏置。
	 * 即使没有驱动，这些设置也必须完成；在那种情况下，spi_probe()
	 * 根本不会被调用。
	 * TODO：理想情况下，GPIO 的设置应该由 ACPI/gpiolib core 以
	 * 更通用的方式来处理。
	 */
	if (spi->irq < 0)
		spi->irq = acpi_dev_gpio_irq_get(adev, 0);

	acpi_device_set_enumerated(adev);

	adev->power.flags.ignore_parent = true;
	if (spi_add_device(spi)) {
		adev->power.flags.ignore_parent = false;
		dev_err(&ctlr->dev, "failed to add SPI device %s from ACPI\n",
			dev_name(&adev->dev));
		spi_dev_put(spi);
	}

	return AE_OK;
}

static acpi_status acpi_spi_add_device(acpi_handle handle, u32 level,
				       void *data, void **return_value)
{
	struct acpi_device *adev = acpi_fetch_acpi_dev(handle);
	struct spi_controller *ctlr = data;

	if (!adev)
		return AE_OK;

	return acpi_register_spi_device(ctlr, adev);
}

#define SPI_ACPI_ENUMERATE_MAX_DEPTH		32

static void acpi_register_spi_devices(struct spi_controller *ctlr)
{
	acpi_status status;
	acpi_handle handle;

	handle = ACPI_HANDLE(ctlr->dev.parent);
	if (!handle)
		return;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
				     SPI_ACPI_ENUMERATE_MAX_DEPTH,
				     acpi_spi_add_device, NULL, ctlr, NULL);
	if (ACPI_FAILURE(status))
		dev_warn(&ctlr->dev, "failed to enumerate SPI target devices\n");
}
#else
static inline void acpi_register_spi_devices(struct spi_controller *ctlr) {}
#endif /* CONFIG_ACPI */

static void spi_controller_release(struct device *dev)
{
	struct spi_controller *ctlr;

	ctlr = container_of(dev, struct spi_controller, dev);

	free_percpu(ctlr->pcpu_statistics);
	kfree(ctlr);
}

static const struct class spi_controller_class = {
	.name		= "spi_master",
	.dev_release	= spi_controller_release,
	.dev_groups	= spi_controller_groups,
};

#ifdef CONFIG_SPI_SLAVE
/**
 * spi_target_abort - 中止 SPI target 控制器上正在进行的传输请求
 * @spi: 当前传输所使用的设备
 */
int spi_target_abort(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;

	if (spi_controller_is_target(ctlr) && ctlr->target_abort)
		return ctlr->target_abort(ctlr);

	return -ENOTSUPP;
}
EXPORT_SYMBOL_GPL(spi_target_abort);

static ssize_t slave_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct spi_controller *ctlr = container_of(dev, struct spi_controller,
						   dev);
	struct device *child;
	int ret;

	child = device_find_any_child(&ctlr->dev);
	ret = sysfs_emit(buf, "%s\n", child ? to_spi_device(child)->modalias : NULL);
	put_device(child);

	return ret;
}

static ssize_t slave_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct spi_controller *ctlr = container_of(dev, struct spi_controller,
						   dev);
	struct spi_device *spi;
	struct device *child;
	char name[32];
	int rc;

	rc = sscanf(buf, "%31s", name);
	if (rc != 1 || !name[0])
		return -EINVAL;

	child = device_find_any_child(&ctlr->dev);
	if (child) {
		/* Remove registered target device */
		device_unregister(child);
		put_device(child);
	}

	if (strcmp(name, "(null)")) {
		/* Register new target device */
		spi = spi_alloc_device(ctlr);
		if (!spi)
			return -ENOMEM;

		strscpy(spi->modalias, name, sizeof(spi->modalias));

		rc = spi_add_device(spi);
		if (rc) {
			spi_dev_put(spi);
			return rc;
		}
	}

	return count;
}

static DEVICE_ATTR_RW(slave);

static struct attribute *spi_target_attrs[] = {
	&dev_attr_slave.attr,
	NULL,
};

static const struct attribute_group spi_target_group = {
	.attrs = spi_target_attrs,
};

static const struct attribute_group *spi_target_groups[] = {
	&spi_controller_statistics_group,
	&spi_target_group,
	NULL,
};

static const struct class spi_target_class = {
	.name		= "spi_slave",
	.dev_release	= spi_controller_release,
	.dev_groups	= spi_target_groups,
};
#else
extern struct class spi_target_class;	/* dummy */
#endif

/**
 * __spi_alloc_controller - 分配一个 SPI host 或 target 控制器
 * @dev: 控制器所属的设备，可能挂在 platform_bus 上
 * @size: 需要分配多少清零的驱动私有数据；返回设备的 driver_data
 *	字段会指向这块内存，可通过 spi_controller_get_devdata() 访问；
 *	这块内存按 cacheline 对齐；如果驱动要让 DMA 访问其私有数据的
 *	某些部分，则需要使用 ALIGN(size, dma_get_cache_alignment())
 *	对 @size 向上对齐。
 * @target: 表示是分配 SPI host（false）还是 SPI target（true）
 * Context: can sleep
 *
 * 这个接口只供 SPI 控制器驱动使用，因为只有它们会直接接触芯片寄存器。
 * 这也是它们在调用 spi_register_controller() 之前分配 spi_controller
 * 结构体的方式。
 *
 * 该函数必须在可睡眠上下文中调用。
 *
 * 调用者负责在调用 spi_register_controller() 之前设置总线号并初始化
 * 控制器的方法；如果后续添加设备失败，还需要调用 spi_controller_put()
 * 来避免内存泄漏。
 *
 * 返回：
 * 成功时返回 SPI 控制器结构体，失败返回 NULL。
 */
struct spi_controller *__spi_alloc_controller(struct device *dev,
					      unsigned int size, bool target)
{
	struct spi_controller	*ctlr;
	size_t ctlr_size = ALIGN(sizeof(*ctlr), dma_get_cache_alignment());

	if (!dev)
		return NULL;

	ctlr = kzalloc(size + ctlr_size, GFP_KERNEL);
	if (!ctlr)
		return NULL;

	ctlr->pcpu_statistics = spi_alloc_pcpu_stats();
	if (!ctlr->pcpu_statistics) {
		kfree(ctlr);
		return NULL;
	}

	device_initialize(&ctlr->dev);
	INIT_LIST_HEAD(&ctlr->queue);
	spin_lock_init(&ctlr->queue_lock);
	spin_lock_init(&ctlr->bus_lock_spinlock);
	mutex_init(&ctlr->bus_lock_mutex);
	mutex_init(&ctlr->io_mutex);
	mutex_init(&ctlr->add_lock);
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->num_data_lanes = 1;
	ctlr->target = target;
	if (IS_ENABLED(CONFIG_SPI_SLAVE) && target)
		ctlr->dev.class = &spi_target_class;
	else
		ctlr->dev.class = &spi_controller_class;
	ctlr->dev.parent = dev;

	device_set_node(&ctlr->dev, dev_fwnode(dev));

	pm_suspend_ignore_children(&ctlr->dev, true);
	spi_controller_set_devdata(ctlr, (void *)ctlr + ctlr_size);

	return ctlr;
}
EXPORT_SYMBOL_GPL(__spi_alloc_controller);

static void devm_spi_release_controller(void *ctlr)
{
	spi_controller_put(ctlr);
}

/**
 * __devm_spi_alloc_controller - 受资源管理的 __spi_alloc_controller()
 * @dev: SPI 控制器的物理设备
 * @size: 需要分配多少清零的驱动私有数据
 * @target: 是否分配 SPI host（false）还是 SPI target（true）控制器
 * Context: can sleep
 *
 * 分配一个 SPI 控制器，并在 @dev 从驱动解绑时自动释放对它的引用。
 * 这样驱动就不需要手动调用 spi_controller_put() 了。
 *
 * 这个函数的参数与 __spi_alloc_controller() 完全一致。
 *
 * 返回：
 * 成功时返回 SPI 控制器结构体，失败返回 NULL。
 */
struct spi_controller *__devm_spi_alloc_controller(struct device *dev,
						   unsigned int size,
						   bool target)
{
	struct spi_controller *ctlr;
	int ret;

	ctlr = __spi_alloc_controller(dev, size, target);
	if (!ctlr)
		return NULL;

	ret = devm_add_action_or_reset(dev, devm_spi_release_controller, ctlr);
	if (ret)
		return NULL;

	ctlr->devm_allocated = true;

	return ctlr;
}
EXPORT_SYMBOL_GPL(__devm_spi_alloc_controller);

/**
 * spi_get_gpio_descs() - 为控制器获取 chip select 的 GPIO 描述符
 * @ctlr: 需要获取 GPIO 描述符的 SPI 控制器
 */
static int spi_get_gpio_descs(struct spi_controller *ctlr)
{
	int nb, i;
	struct gpio_desc **cs;
	struct device *dev = &ctlr->dev;
	unsigned long native_cs_mask = 0;
	unsigned int num_cs_gpios = 0;

	nb = gpiod_count(dev, "cs");
	if (nb < 0) {
		/* 完全没有 GPIO 也是允许的，否则就返回错误。 */
		if (nb == -ENOENT)
			return 0;
		return nb;
	}

	ctlr->num_chipselect = max_t(int, nb, ctlr->num_chipselect);

	cs = devm_kcalloc(dev, ctlr->num_chipselect, sizeof(*cs),
			  GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	ctlr->cs_gpiods = cs;

	for (i = 0; i < nb; i++) {
			/*
			 * 大多数 chip select 都是低有效的，gpiolib 会通过
			 * 特殊 quirks 处理反相语义，所以这里用 GPIOD_OUT_LOW
			 * 初始化，表示“未选中”；在大多数情况下，这会把
			 * 物理引脚拉高。
			 */
		cs[i] = devm_gpiod_get_index_optional(dev, "cs", i,
						      GPIOD_OUT_LOW);
		if (IS_ERR(cs[i]))
			return PTR_ERR(cs[i]);

		if (cs[i]) {
			/*
			 * 如果找到了 CS GPIO，就按设备名和 chip select
			 * 编号给它命名。
			 */
			char *gpioname;

			gpioname = devm_kasprintf(dev, GFP_KERNEL, "%s CS%d",
						  dev_name(dev), i);
			if (!gpioname)
				return -ENOMEM;
			gpiod_set_consumer_name(cs[i], gpioname);
			num_cs_gpios++;
			continue;
		}

		if (ctlr->max_native_cs && i >= ctlr->max_native_cs) {
			dev_err(dev, "Invalid native chip select %d\n", i);
			return -EINVAL;
		}
		native_cs_mask |= BIT(i);
	}

	ctlr->unused_native_cs = ffs(~native_cs_mask) - 1;

	if ((ctlr->flags & SPI_CONTROLLER_GPIO_SS) && num_cs_gpios &&
	    ctlr->max_native_cs && ctlr->unused_native_cs >= ctlr->max_native_cs) {
		dev_err(dev, "No unused native chip select available\n");
		return -EINVAL;
	}

	return 0;
}

static int spi_controller_check_ops(struct spi_controller *ctlr)
{
	/*
	 * 如果控制器不支持普通 SPI 传输，也可以只实现高层的
	 * SPI-memory 类操作，这在某些场景下是完全合法的。
	 * 但如果 ->mem_ops 或 ->mem_ops->exec_op 为空，
	 * 那么至少要实现一种 ->transfer_xxx() 方法。
	 */
	if (!ctlr->mem_ops || !ctlr->mem_ops->exec_op) {
		if (!ctlr->transfer && !ctlr->transfer_one &&
		   !ctlr->transfer_one_message) {
			return -EINVAL;
		}
	}

	return 0;
}

/* 使用 Linux idr 分配动态总线号。 */
static int spi_controller_id_alloc(struct spi_controller *ctlr, int start, int end)
{
	int id;

	mutex_lock(&board_lock);
	id = idr_alloc(&spi_controller_idr, ctlr, start, end, GFP_KERNEL);
	mutex_unlock(&board_lock);
	if (WARN(id < 0, "couldn't get idr"))
		return id == -ENOSPC ? -EBUSY : id;
	ctlr->bus_num = id;
	return 0;
}

/**
 * spi_register_controller - 注册 SPI host 或 target 控制器
 * @ctlr: 已初始化的控制器，通常来自 spi_alloc_host() 或
 *	spi_alloc_target()
 * Context: can sleep
 *
 * SPI 控制器通常通过其它非 SPI 总线与驱动连接，例如 platform bus。
 * 对应驱动的 probe() 最后阶段会调用 spi_register_controller()，
 * 将控制器挂接到这层 SPI glue 上。
 *
 * SPI 控制器通常使用板级（往往也是 SoC 相关的）总线号；
 * SPI 设备的板级寻址会把这些总线号和 chip select 号组合起来。
 * 由于 SPI 本身不直接支持动态设备发现，因此板子需要配置表来
 * 指明哪颗芯片挂在哪个地址上。
 *
 * 这个接口必须在可睡眠上下文中调用。
 *
 * 成功返回后，调用者需要负责再调用 spi_unregister_controller()。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_register_controller(struct spi_controller *ctlr)
{
	struct device		*dev = ctlr->dev.parent;
	struct boardinfo	*bi;
	int			first_dynamic;
	int			status;
	int			idx;

	if (!dev)
		return -ENODEV;

	/*
	 * 在注册 SPI 控制器之前，先确认必要的钩子都已经实现。
	 */
	status = spi_controller_check_ops(ctlr);
	if (status)
		return status;

	if (ctlr->bus_num < 0)
		ctlr->bus_num = of_alias_get_id(ctlr->dev.of_node, "spi");
	if (ctlr->bus_num >= 0) {
		/* 固定总线号的设备必须直接使用该编号登记。 */
		status = spi_controller_id_alloc(ctlr, ctlr->bus_num, ctlr->bus_num + 1);
		if (status)
			return status;
	}
	if (ctlr->bus_num < 0) {
		first_dynamic = of_alias_get_highest_id("spi");
		if (first_dynamic < 0)
			first_dynamic = 0;
		else
			first_dynamic++;

		status = spi_controller_id_alloc(ctlr, first_dynamic, 0);
		if (status)
			return status;
	}
	ctlr->bus_lock_flag = 0;
	init_completion(&ctlr->xfer_completion);
	init_completion(&ctlr->cur_msg_completion);
	if (!ctlr->max_dma_len)
		ctlr->max_dma_len = INT_MAX;

	/*
	 * 先注册设备，用户态随后就能看到它。
	 * 如果总线 ID 已被占用，注册会失败。
	 */
	dev_set_name(&ctlr->dev, "spi%u", ctlr->bus_num);

	if (!spi_controller_is_target(ctlr) && ctlr->use_gpio_descriptors) {
		status = spi_get_gpio_descs(ctlr);
		if (status)
			goto free_bus_id;
		/*
		 * 使用 GPIO 描述符的控制器在需要时总是支持 SPI_CS_HIGH。
		 */
		ctlr->mode_bits |= SPI_CS_HIGH;
	}

	/*
	 * 即使只有一个始终被选中的设备，也至少必须有一个 chipselect。
	 */
	if (!ctlr->num_chipselect) {
		status = -EINVAL;
		goto free_bus_id;
	}

	/* 把 last_cs 设为 SPI_INVALID_CS 表示当前没有选中任何 chip。 */
	for (idx = 0; idx < SPI_DEVICE_CS_CNT_MAX; idx++)
		ctlr->last_cs[idx] = SPI_INVALID_CS;

	status = device_add(&ctlr->dev);
	if (status < 0)
		goto free_bus_id;
	dev_dbg(dev, "registered %s %s\n",
			spi_controller_is_target(ctlr) ? "target" : "host",
			dev_name(&ctlr->dev));

	/*
	 * 如果使用的是队列化驱动，就启动队列。
	 * 注意，如果驱动只支持高层 memory 操作，就不需要队列逻辑。
	 */
	if (ctlr->transfer) {
		dev_info(dev, "controller is unqueued, this is deprecated\n");
	} else if (ctlr->transfer_one || ctlr->transfer_one_message) {
		status = spi_controller_initialize_queue(ctlr);
		if (status)
			goto del_ctrl;
	}

	mutex_lock(&board_lock);
	list_add_tail(&ctlr->list, &spi_controller_list);
	list_for_each_entry(bi, &board_list, list)
		spi_match_controller_to_boardinfo(ctlr, &bi->board_info);
	mutex_unlock(&board_lock);

	/*
	 * 从设备树和 ACPI 注册设备。
	 */
	of_register_spi_devices(ctlr);
	acpi_register_spi_devices(ctlr);
	return status;

del_ctrl:
	device_del(&ctlr->dev);
free_bus_id:
	mutex_lock(&board_lock);
	idr_remove(&spi_controller_idr, ctlr->bus_num);
	mutex_unlock(&board_lock);
	return status;
}
EXPORT_SYMBOL_GPL(spi_register_controller);

static void devm_spi_unregister_controller(void *ctlr)
{
	spi_unregister_controller(ctlr);
}

/**
 * devm_spi_register_controller - 注册受管理的 SPI host 或 target 控制器
 * @dev:    管理该 SPI 控制器的设备
 * @ctlr: 已初始化的控制器，通常来自 spi_alloc_host() 或
 *	spi_alloc_target()
 * Context: 可以睡眠
 *
 * 以 spi_register_controller() 的方式注册一个 SPI 控制器，
 * 并在设备释放时自动注销；如果它不是通过 devm_spi_alloc_host/target()
 * 分配的，还会同时释放。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int devm_spi_register_controller(struct device *dev,
				 struct spi_controller *ctlr)
{
	int ret;

	ret = spi_register_controller(ctlr);
	if (ret)
		return ret;

	/*
	 * 如果 devm_add_action_or_reset() 失败，而控制器又不是由
	 * devres 分配的，就要避免它被 spi_unregister_controller()
	 * 提前释放掉。
	 */
	spi_controller_get(ctlr);

	ret = devm_add_action_or_reset(dev, devm_spi_unregister_controller, ctlr);

	if (ret == 0 || ctlr->devm_allocated)
		spi_controller_put(ctlr);

	return ret;
}
EXPORT_SYMBOL_GPL(devm_spi_register_controller);

static int __unregister(struct device *dev, void *null)
{
	spi_unregister_device(to_spi_device(dev));
	return 0;
}

/**
 * spi_unregister_controller - 注销 SPI host 或 target 控制器
 * @ctlr: 要注销的控制器
 * Context: can sleep
 *
 * 这个接口只供 SPI 控制器驱动使用，因为只有它们会直接操作芯片寄存器。
 *
 * 该函数必须在可睡眠上下文中调用。
 *
 * 注意，除非控制器是通过 devm_spi_alloc_host/target() 分配的，
 * 否则该函数还会额外释放一个控制器引用。
 */
void spi_unregister_controller(struct spi_controller *ctlr)
{
	struct spi_controller *found;
	int id = ctlr->bus_num;

	/* 阻止新增设备，并注销已有设备。 */
	if (IS_ENABLED(CONFIG_SPI_DYNAMIC))
		mutex_lock(&ctlr->add_lock);

	device_for_each_child(&ctlr->dev, NULL, __unregister);

	/* 先确认这个控制器确实曾经注册过。 */
	mutex_lock(&board_lock);
	found = idr_find(&spi_controller_idr, id);
	mutex_unlock(&board_lock);
	if (ctlr->queued) {
		if (spi_destroy_queue(ctlr))
			dev_err(&ctlr->dev, "queue remove failed\n");
	}
	mutex_lock(&board_lock);
	list_del(&ctlr->list);
	mutex_unlock(&board_lock);

	device_del(&ctlr->dev);

	/* 释放总线编号。 */
	mutex_lock(&board_lock);
	if (found == ctlr)
		idr_remove(&spi_controller_idr, id);
	mutex_unlock(&board_lock);

	if (IS_ENABLED(CONFIG_SPI_DYNAMIC))
		mutex_unlock(&ctlr->add_lock);

	/*
	 * 如果驱动还没有迁移到 devm_spi_alloc_host/target()，
	 * 那就在这里释放控制器的最后一个引用。
	 */
	if (!ctlr->devm_allocated)
		put_device(&ctlr->dev);
}
EXPORT_SYMBOL_GPL(spi_unregister_controller);

static inline int __spi_check_suspended(const struct spi_controller *ctlr)
{
	return ctlr->flags & SPI_CONTROLLER_SUSPENDED ? -ESHUTDOWN : 0;
}

static inline void __spi_mark_suspended(struct spi_controller *ctlr)
{
	mutex_lock(&ctlr->bus_lock_mutex);
	ctlr->flags |= SPI_CONTROLLER_SUSPENDED;
	mutex_unlock(&ctlr->bus_lock_mutex);
}

static inline void __spi_mark_resumed(struct spi_controller *ctlr)
{
	mutex_lock(&ctlr->bus_lock_mutex);
	ctlr->flags &= ~SPI_CONTROLLER_SUSPENDED;
	mutex_unlock(&ctlr->bus_lock_mutex);
}

int spi_controller_suspend(struct spi_controller *ctlr)
{
	int ret = 0;

	/* 对非队列化控制器来说，基本上是空操作。 */
	if (ctlr->queued) {
		ret = spi_stop_queue(ctlr);
		if (ret)
			dev_err(&ctlr->dev, "queue stop failed\n");
	}

	__spi_mark_suspended(ctlr);
	return ret;
}
EXPORT_SYMBOL_GPL(spi_controller_suspend);

int spi_controller_resume(struct spi_controller *ctlr)
{
	int ret = 0;

	__spi_mark_resumed(ctlr);

	if (ctlr->queued) {
		ret = spi_start_queue(ctlr);
		if (ret)
			dev_err(&ctlr->dev, "queue restart failed\n");
	}
	return ret;
}
EXPORT_SYMBOL_GPL(spi_controller_resume);

/*-------------------------------------------------------------------------*/

/* spi_message 变更相关的核心方法。 */

static void __spi_replace_transfers_release(struct spi_controller *ctlr,
					    struct spi_message *msg,
					    void *res)
{
	struct spi_replaced_transfers *rxfer = res;
	size_t i;

	/* 如有需要，先调用额外回调。 */
	if (rxfer->release)
		rxfer->release(ctlr, msg, res);

	/* 把被替换掉的 transfers 插回 message。 */
	list_splice(&rxfer->replaced_transfers, rxfer->replaced_after);

	/* 移除先前插入的条目。 */
	for (i = 0; i < rxfer->inserted; i++)
		list_del(&rxfer->inserted_transfers[i].transfer_list);
}

/**
 * spi_replace_transfers - 用多个 transfer 替换一组 transfer，并把变更登记到 spi_message.resources
 * @msg:           当前正在处理的 spi_message
 * @xfer_first:    需要被替换的第一个 spi_transfer
 * @remove:        需要移除的 transfer 数量
 * @insert:        需要插入的 transfer 数量
 * @release:       某些场景下需要的额外释放回调
 * @extradatasize: 要额外分配的数据大小（会保证与 struct @spi_transfer 对齐）
 * @gfp:           GFP 分配标志
 *
 * 返回：
 * 成功时返回 @spi_replaced_transfers 指针，
 * 出错时返回 PTR_ERR(...)。
 */
static struct spi_replaced_transfers *spi_replace_transfers(
	struct spi_message *msg,
	struct spi_transfer *xfer_first,
	size_t remove,
	size_t insert,
	spi_replaced_release_t release,
	size_t extradatasize,
	gfp_t gfp)
{
	struct spi_replaced_transfers *rxfer;
	struct spi_transfer *xfer;
	size_t i;

	/* 使用 spi_res 分配该结构。 */
	rxfer = spi_res_alloc(msg->spi, __spi_replace_transfers_release,
			      struct_size(rxfer, inserted_transfers, insert)
			      + extradatasize,
			      gfp);
	if (!rxfer)
		return ERR_PTR(-ENOMEM);

	/* 在执行通用释放逻辑前要调用的释放回调。 */
	rxfer->release = release;

	/* 设置额外数据区。 */
	if (extradatasize)
		rxfer->extradata =
			&rxfer->inserted_transfers[insert];

	/* 初始化 replaced_transfers 链表。 */
	INIT_LIST_HEAD(&rxfer->replaced_transfers);

	/*
	 * 记录应该在其后重新插入 @replaced_transfers 的 list_entry；
	 * 它甚至可能是 spi_message.messages 本身。
	 */
	rxfer->replaced_after = xfer_first->transfer_list.prev;

	/* 移除请求数量的 transfer。 */
	for (i = 0; i < remove; i++) {
		/*
		 * 如果 replaced_after 后面已经是 msg->transfers，
		 * 那么说明请求移除的 transfer 数量超过了链表中实际数量。
		 */
		if (rxfer->replaced_after->next == &msg->transfers) {
			dev_err(&msg->spi->dev,
				"requested to remove more spi_transfers than are available\n");
			/* 把已替换的 transfers 插回 message。 */
			list_splice(&rxfer->replaced_transfers,
				    rxfer->replaced_after);

			/* 释放 spi_replace_transfer 结构体。 */
			spi_res_free(rxfer);

			/* ...然后返回错误。 */
			return ERR_PTR(-EINVAL);
		}

		/*
		 * 把 replaced_after 后面的那个 transfer 从原列表中移除，
		 * 并加入 replaced_transfers 列表。
		 */
		list_move_tail(rxfer->replaced_after->next,
			       &rxfer->replaced_transfers);
	}

	/*
	 * 根据要移除的第一个 transfer，创建若干个设置完全相同的副本。
	 */
	for (i = 0; i < insert; i++) {
		/* 这里需要逆序处理。 */
		xfer = &rxfer->inserted_transfers[insert - 1 - i];

		/* 复制所有 spi_transfer 数据。 */
		memcpy(xfer, xfer_first, sizeof(*xfer));

		/* 加入链表。 */
		list_add(&xfer->transfer_list, rxfer->replaced_after);

		/* 除最后一个外，清除 cs_change 和 delay。 */
		if (i) {
			xfer->cs_change = false;
			xfer->delay.value = 0;
		}
	}

	/* 设置插入数量。 */
	rxfer->inserted = insert;

	/* ...然后把它登记到 spi_res / spi_message 中。 */
	spi_res_add(msg, rxfer);

	return rxfer;
}

static int __spi_split_transfer_maxsize(struct spi_controller *ctlr,
					struct spi_message *msg,
					struct spi_transfer **xferp,
					size_t maxsize)
{
	struct spi_transfer *xfer = *xferp, *xfers;
	struct spi_replaced_transfers *srt;
	size_t offset;
	size_t count, i;

	/* 计算需要替换多少个 transfer。 */
	count = DIV_ROUND_UP(xfer->len, maxsize);

	/* 创建替换项。 */
	srt = spi_replace_transfers(msg, xfer, 1, count, NULL, 0, GFP_KERNEL);
	if (IS_ERR(srt))
		return PTR_ERR(srt);
	xfers = srt->inserted_transfers;

	/*
	 * 接下来处理每个新插入的 spi_transfer。
	 * 注意，这些替换出来的 spi_transfer 一开始都和 *xferp
	 * 保持相同的设置，因此 tx_buf、rx_buf 和 len 等大多相同；
	 * 我们只需要修正 len 和指针即可。
	 */

	/*
	 * 第一个 transfer 只需要修改长度，因此单独处理。
	 */
	xfers[0].len = min_t(size_t, maxsize, xfer[0].len);

	/* 其余的 transfer 还需要设置 rx_buf / tx_buf。 */
	for (i = 1, offset = maxsize; i < count; offset += maxsize, i++) {
		/* 更新 rx_buf、tx_buf 和 DMA 相关指针。 */
		if (xfers[i].rx_buf)
			xfers[i].rx_buf += offset;
		if (xfers[i].tx_buf)
			xfers[i].tx_buf += offset;

		/* 更新长度。 */
		xfers[i].len = min(maxsize, xfers[i].len - offset);
	}

	/*
	 * 将 xferp 指向我们插入的最后一个条目，
	 * 这样后面就会跳过已经拆分过的 transfers。
	 */
	*xferp = &xfers[count - 1];

	/* 增加统计计数。 */
	SPI_STATISTICS_INCREMENT_FIELD(ctlr->pcpu_statistics,
				       transfers_split_maxsize);
	SPI_STATISTICS_INCREMENT_FIELD(msg->spi->pcpu_statistics,
				       transfers_split_maxsize);

	return 0;
}

/**
 * spi_split_transfers_maxsize - 当单个 transfer 超过指定大小时，将其拆成多个 transfer
 * @ctlr:    处理该 transfer 的 @spi_controller
 * @msg:     需要转换的 @spi_message
 * @maxsize: 拆分所使用的最大大小
 *
 * 该函数分配的资源会在 spi message unoptimize 阶段自动释放，
 * 因此它只应该从 optimize_message 回调中调用。
 *
 * 返回：
 * 转换状态。
 */
int spi_split_transfers_maxsize(struct spi_controller *ctlr,
				struct spi_message *msg,
				size_t maxsize)
{
	struct spi_transfer *xfer;
	int ret;

	/*
	 * 遍历 transfer_list。
	 * 注意 xfer 会前进到最后插入的 transfer，
	 * 这样就不会重复检查大小；而且在替换完成后，
	 * xfer 可能已经属于另一个链表。
	 */
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (xfer->len > maxsize) {
			ret = __spi_split_transfer_maxsize(ctlr, msg, &xfer,
							   maxsize);
			if (ret)
				return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(spi_split_transfers_maxsize);


/**
 * spi_split_transfers_maxwords - 当单个 transfer 超过指定 word 数时，将其拆成多个 transfer
 * @ctlr:     处理该 transfer 的 @spi_controller
 * @msg:      需要转换的 @spi_message
 * @maxwords: 每个 transfer 允许的最大 word 数
 *
 * 该函数分配的资源会在 spi message unoptimize 阶段自动释放，
 * 因此它只应该从 optimize_message 回调中调用。
 *
 * 返回：
 * 转换状态。
 */
int spi_split_transfers_maxwords(struct spi_controller *ctlr,
				 struct spi_message *msg,
				 size_t maxwords)
{
	struct spi_transfer *xfer;

	/*
	 * 遍历 transfer_list。
	 * 注意 xfer 会前进到最后插入的 transfer，
	 * 这样就不会重复检查大小；而且在替换完成后，
	 * xfer 可能已经属于另一个链表。
	 */
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		size_t maxsize;
		int ret;

		maxsize = maxwords * spi_bpw_to_bytes(xfer->bits_per_word);
		if (xfer->len > maxsize) {
			ret = __spi_split_transfer_maxsize(ctlr, msg, &xfer,
							   maxsize);
			if (ret)
				return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(spi_split_transfers_maxwords);

/*-------------------------------------------------------------------------*/

/*
 * SPI 控制器协议驱动的核心方法。
 * 其它一些核心方法当前以内联函数形式定义。
 */

static int __spi_validate_bits_per_word(struct spi_controller *ctlr,
					u8 bits_per_word)
{
	if (ctlr->bits_per_word_mask) {
		/* 掩码里最多只能放下 32 位。 */
		if (bits_per_word > 32)
			return -EINVAL;
		if (!(ctlr->bits_per_word_mask & SPI_BPW_MASK(bits_per_word)))
			return -EINVAL;
	}

	return 0;
}

/**
 * spi_set_cs_timing - 配置 CS 的 setup / hold / inactive 延迟
 * @spi: 需要特定 CS 时序配置的设备
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static int spi_set_cs_timing(struct spi_device *spi)
{
	struct device *parent = spi->controller->dev.parent;
	int status = 0;

	if (spi->controller->set_cs_timing && !spi_get_csgpiod(spi, 0)) {
		if (spi->controller->auto_runtime_pm) {
			status = pm_runtime_get_sync(parent);
			if (status < 0) {
				pm_runtime_put_noidle(parent);
				dev_err(&spi->controller->dev, "Failed to power device: %d\n",
					status);
				return status;
			}

			status = spi->controller->set_cs_timing(spi);
			pm_runtime_put_autosuspend(parent);
		} else {
			status = spi->controller->set_cs_timing(spi);
		}
	}
	return status;
}

static int __spi_setup(struct spi_device *spi, bool initial_setup)
{
	unsigned	bad_bits, ugly_bits;
	int		status;

	/*
	 * 检查 mode，避免 DUAL、QUAD 和 NO_MOSI/MISO 里的任意两个
	 * 同时被设置。
	 */
	if ((hweight_long(spi->mode &
		(SPI_TX_DUAL | SPI_TX_QUAD | SPI_NO_TX)) > 1) ||
	    (hweight_long(spi->mode &
		(SPI_RX_DUAL | SPI_RX_QUAD | SPI_NO_RX)) > 1)) {
		dev_err(&spi->dev,
		"setup: can not select any two of dual, quad and no-rx/tx at the same time\n");
		return -EINVAL;
	}
	/* 如果是 SPI_3WIRE 模式，则禁止 DUAL 和 QUAD。 */
	if ((spi->mode & SPI_3WIRE) && (spi->mode &
		(SPI_TX_DUAL | SPI_TX_QUAD | SPI_TX_OCTAL |
		 SPI_RX_DUAL | SPI_RX_QUAD | SPI_RX_OCTAL)))
		return -EINVAL;
	/* 检查是否配置了互相冲突的 MOSI 空闲电平。 */
	if ((spi->mode & SPI_MOSI_IDLE_LOW) && (spi->mode & SPI_MOSI_IDLE_HIGH)) {
		dev_err(&spi->dev,
			"setup: MOSI configured to idle low and high at the same time.\n");
		return -EINVAL;
	}
	/*
	 * 帮助驱动在需要当前控制器不支持的选项时干净地失败。
	 * SPI_CS_WORD 有软件回退实现，所以这里忽略它。
	 */
	bad_bits = spi->mode & ~(spi->controller->mode_bits | SPI_CS_WORD |
				 SPI_NO_TX | SPI_NO_RX);
	ugly_bits = bad_bits &
		    (SPI_TX_DUAL | SPI_TX_QUAD | SPI_TX_OCTAL |
		     SPI_RX_DUAL | SPI_RX_QUAD | SPI_RX_OCTAL);
	if (ugly_bits) {
		dev_warn(&spi->dev,
			 "setup: ignoring unsupported mode bits %x\n",
			 ugly_bits);
		spi->mode &= ~ugly_bits;
		bad_bits &= ~ugly_bits;
	}
	if (bad_bits) {
		dev_err(&spi->dev, "setup: unsupported mode bits %x\n",
			bad_bits);
		return -EINVAL;
	}

	if (!spi->bits_per_word) {
		spi->bits_per_word = 8;
	} else {
		/*
		 * 某些控制器可能不支持默认的 8 bits-per-word，
		 * 因此只有在显式指定时才执行检查。
		 */
		status = __spi_validate_bits_per_word(spi->controller,
						      spi->bits_per_word);
		if (status)
			return status;
	}

	if (spi->controller->max_speed_hz &&
	    (!spi->max_speed_hz ||
	     spi->max_speed_hz > spi->controller->max_speed_hz))
		spi->max_speed_hz = spi->controller->max_speed_hz;

	mutex_lock(&spi->controller->io_mutex);

	if (spi->controller->setup) {
		status = spi->controller->setup(spi);
		if (status) {
			mutex_unlock(&spi->controller->io_mutex);
			dev_err(&spi->controller->dev, "Failed to setup device: %d\n",
				status);
			return status;
		}
	}

	status = spi_set_cs_timing(spi);
	if (status) {
		mutex_unlock(&spi->controller->io_mutex);
		goto err_cleanup;
	}

	if (spi->controller->auto_runtime_pm && spi->controller->set_cs) {
		status = pm_runtime_resume_and_get(spi->controller->dev.parent);
		if (status < 0) {
			mutex_unlock(&spi->controller->io_mutex);
			dev_err(&spi->controller->dev, "Failed to power device: %d\n",
				status);
			goto err_cleanup;
		}

		/*
		 * 我们不希望 pm_runtime_get 返回正值，因为很多设备驱动
		 * 调用 spi_setup() 时会把“非零”误当作失败，而不是检查
		 * 负错误码。
		 */
		status = 0;

		spi_set_cs(spi, false, true);
		pm_runtime_put_autosuspend(spi->controller->dev.parent);
	} else {
		spi_set_cs(spi, false, true);
	}

	mutex_unlock(&spi->controller->io_mutex);

	if (spi->rt && !spi->controller->rt) {
		spi->controller->rt = true;
		spi_set_thread_rt(spi->controller);
	}

	trace_spi_setup(spi, status);

	dev_dbg(&spi->dev, "setup mode %lu, %s%s%s%s%u bits/w, %u Hz max --> %d\n",
			spi->mode & SPI_MODE_X_MASK,
			(spi->mode & SPI_CS_HIGH) ? "cs_high, " : "",
			(spi->mode & SPI_LSB_FIRST) ? "lsb, " : "",
			(spi->mode & SPI_3WIRE) ? "3wire, " : "",
			(spi->mode & SPI_LOOP) ? "loopback, " : "",
			spi->bits_per_word, spi->max_speed_hz,
			status);

	return status;

err_cleanup:
	if (initial_setup)
		spi_cleanup(spi);

	return status;
}

/**
 * spi_setup - 设置 SPI 模式和时钟速率
 * @spi: 需要修改设置的设备
 * Context: can sleep，且该设备当前没有排队请求
 *
 * 如果设备无法按默认设置工作，SPI 协议驱动可能需要更新传输模式。
 * 同样地，它们也可能需要从初始值修改时钟速率或每字位数。这个函数
 * 负责修改这些设置，而且必须在可睡眠上下文中调用。除了 SPI_CS_HIGH
 * 之外，其它更改会在下一次设备被选中并进行数据传输时生效。该函数
 * 返回时，SPI 设备会处于取消选择状态。
 *
 * 注意，如果协议驱动指定了底层控制器或其驱动不支持的选项，
 * 这个调用就会失败。例如，并不是所有硬件都支持 9 位字传输、
 * LSB 优先编码，或者高有效 chip select。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_setup(struct spi_device *spi)
{
	return __spi_setup(spi, false);
}
EXPORT_SYMBOL_GPL(spi_setup);

static int _spi_xfer_word_delay_update(struct spi_transfer *xfer,
				       struct spi_device *spi)
{
	int delay1, delay2;

	delay1 = spi_delay_to_ns(&xfer->word_delay, xfer);
	if (delay1 < 0)
		return delay1;

	delay2 = spi_delay_to_ns(&spi->word_delay, xfer);
	if (delay2 < 0)
		return delay2;

	if (delay1 < delay2)
		memcpy(&xfer->word_delay, &spi->word_delay,
		       sizeof(xfer->word_delay));

	return 0;
}

static int __spi_validate(struct spi_device *spi, struct spi_message *message)
{
	struct spi_controller *ctlr = spi->controller;
	struct spi_transfer *xfer;
	int w_size;

	if (list_empty(&message->transfers))
		return -EINVAL;

	message->spi = spi;

	/*
	 * 半双工链路包括原始的 MicroWire，以及只有一根数据线的接口，
	 * 例如 SPI_3WIRE（需要切换方向），或者缺少 MOSI / MISO 的情况。
	 * 软件限制也可能把链路限制成半双工。
	 */
	if ((ctlr->flags & SPI_CONTROLLER_HALF_DUPLEX) ||
	    (spi->mode & SPI_3WIRE)) {
		unsigned flags = ctlr->flags;

		list_for_each_entry(xfer, &message->transfers, transfer_list) {
			if (xfer->rx_buf && xfer->tx_buf)
				return -EINVAL;
			if ((flags & SPI_CONTROLLER_NO_TX) && xfer->tx_buf)
				return -EINVAL;
			if ((flags & SPI_CONTROLLER_NO_RX) && xfer->rx_buf)
				return -EINVAL;
		}
	}

	/*
	 * 如果 transfer 没有设置 bits_per_word 和最大速率，就使用 spi_device
	 * 的默认值。
	 * 如果 transfer 没有设置 tx_nbits / rx_nbits，就默认使用单线传输
	 * （SPI_NBITS_SINGLE）。
	 * 同时确保 transfer 的 word_delay 不短于设备自身要求。
	 */
	message->frame_length = 0;
	list_for_each_entry(xfer, &message->transfers, transfer_list) {
		xfer->effective_speed_hz = 0;
		message->frame_length += xfer->len;
		if (!xfer->bits_per_word)
			xfer->bits_per_word = spi->bits_per_word;

		if (!xfer->speed_hz)
			xfer->speed_hz = spi->max_speed_hz;

		if (ctlr->max_speed_hz && xfer->speed_hz > ctlr->max_speed_hz)
			xfer->speed_hz = ctlr->max_speed_hz;

		if (__spi_validate_bits_per_word(ctlr, xfer->bits_per_word))
			return -EINVAL;

		/*
		 * 只有控制器的 dtr_caps=true 时才支持 DDR 模式。
		 * 默认把 SPI / QSPI 控制器视为 SDR 模式。
		 * 注意：这里只适用于 QSPI 控制器。
		 */
		if (xfer->dtr_mode && !ctlr->dtr_caps)
			return -EINVAL;

		/*
		 * SPI transfer 长度必须是 SPI word size 的整数倍，
		 * 而 SPI word size 本身应为 2 的幂倍数。
		 */
		w_size = spi_bpw_to_bytes(xfer->bits_per_word);

		/* 不接受部分传输。 */
		if (xfer->len % w_size)
			return -EINVAL;

		if (xfer->speed_hz && ctlr->min_speed_hz &&
		    xfer->speed_hz < ctlr->min_speed_hz)
			return -EINVAL;

		if (xfer->tx_buf && !xfer->tx_nbits)
			xfer->tx_nbits = SPI_NBITS_SINGLE;
		if (xfer->rx_buf && !xfer->rx_nbits)
			xfer->rx_nbits = SPI_NBITS_SINGLE;
		/*
		 * 检查 transfer 的 tx/rx_nbits：
		 * 1. 值必须是 single / dual / quad / octal 之一
		 * 2. tx/rx_nbits 必须与 spi_device 的 mode 匹配
		 */
		if (xfer->tx_buf) {
			if (spi->mode & SPI_NO_TX)
				return -EINVAL;
			if (xfer->tx_nbits != SPI_NBITS_SINGLE &&
				xfer->tx_nbits != SPI_NBITS_DUAL &&
				xfer->tx_nbits != SPI_NBITS_QUAD &&
				xfer->tx_nbits != SPI_NBITS_OCTAL)
				return -EINVAL;
			if ((xfer->tx_nbits == SPI_NBITS_DUAL) &&
				!(spi->mode & (SPI_TX_DUAL | SPI_TX_QUAD | SPI_TX_OCTAL)))
				return -EINVAL;
			if ((xfer->tx_nbits == SPI_NBITS_QUAD) &&
				!(spi->mode & (SPI_TX_QUAD | SPI_TX_OCTAL)))
				return -EINVAL;
			if ((xfer->tx_nbits == SPI_NBITS_OCTAL) &&
				!(spi->mode & SPI_TX_OCTAL))
				return -EINVAL;
		}
		/* 检查 transfer 的 rx_nbits。 */
		if (xfer->rx_buf) {
			if (spi->mode & SPI_NO_RX)
				return -EINVAL;
			if (xfer->rx_nbits != SPI_NBITS_SINGLE &&
				xfer->rx_nbits != SPI_NBITS_DUAL &&
				xfer->rx_nbits != SPI_NBITS_QUAD &&
				xfer->rx_nbits != SPI_NBITS_OCTAL)
				return -EINVAL;
			if ((xfer->rx_nbits == SPI_NBITS_DUAL) &&
				!(spi->mode & (SPI_RX_DUAL | SPI_RX_QUAD | SPI_RX_OCTAL)))
				return -EINVAL;
			if ((xfer->rx_nbits == SPI_NBITS_QUAD) &&
				!(spi->mode & (SPI_RX_QUAD | SPI_RX_OCTAL)))
				return -EINVAL;
			if ((xfer->rx_nbits == SPI_NBITS_OCTAL) &&
				!(spi->mode & SPI_RX_OCTAL))
				return -EINVAL;
		}

		if (_spi_xfer_word_delay_update(xfer, spi))
			return -EINVAL;

		/* 确认控制器支持所需的 offload 特性。 */
		if (xfer->offload_flags) {
			if (!message->offload)
				return -EINVAL;

			if (xfer->offload_flags & ~message->offload->xfer_flags)
				return -EINVAL;
		}
	}

	message->status = -EINPROGRESS;

	return 0;
}

/*
 * spi_split_transfers - 传输拆分的通用处理
 * @msg: 需要拆分的消息
 *
 * 在某些条件下，SPI 控制器可能不支持任意长度的 transfer，
 * 或者不支持外设需要的某些特性。这个函数会把 message 里的 transfer
 * 拆成控制器能支持的更小 transfer。
 *
 * 这里没覆盖到的特殊控制器，也可以在 optimize_message() 回调里
 * 自己做拆分。
 *
 * Context: 可以睡眠
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static int spi_split_transfers(struct spi_message *msg)
{
	struct spi_controller *ctlr = msg->spi->controller;
	struct spi_transfer *xfer;
	int ret;

	/*
	 * 如果 SPI 控制器不支持每个 transfer 都切换 CS（SPI_CS_WORD），
	 * 或者 CS 线是通过 GPIO 实现的，就可以把 transfer 拆成单 word
	 * transfer，并确保每个 transfer 都设置 cs_change，来模拟
	 * “每个 word 都切一次 CS”的硬件行为。
	 */
	if ((msg->spi->mode & SPI_CS_WORD) &&
	    (!(ctlr->mode_bits & SPI_CS_WORD) || spi_is_csgpiod(msg->spi))) {
		ret = spi_split_transfers_maxwords(ctlr, msg, 1);
		if (ret)
			return ret;

		list_for_each_entry(xfer, &msg->transfers, transfer_list) {
			/* 不要修改链表最后一个元素的 cs_change。 */
			if (list_is_last(&xfer->transfer_list, &msg->transfers))
				break;

			xfer->cs_change = 1;
		}
	} else {
		ret = spi_split_transfers_maxsize(ctlr, msg,
						  spi_max_transfer_size(msg->spi));
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * __spi_optimize_message - spi_optimize_message() 与
 *                          spi_maybe_optimize_message() 的共享实现
 * @spi: 这条消息将使用的设备
 * @msg: 需要优化的消息
 *
 * 外设驱动会调用 spi_optimize_message()，而 spi core 会调用
 * spi_maybe_optimize_message()，不会直接调用这个内部函数。
 *
 * 已经优化过的消息不能再调用这个函数。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static int __spi_optimize_message(struct spi_device *spi,
				  struct spi_message *msg)
{
	struct spi_controller *ctlr = spi->controller;
	int ret;

	ret = __spi_validate(spi, msg);
	if (ret)
		return ret;

	ret = spi_split_transfers(msg);
	if (ret)
		return ret;

	if (ctlr->optimize_message) {
		ret = ctlr->optimize_message(msg);
		if (ret) {
			spi_res_release(ctlr, msg);
			return ret;
		}
	}

	msg->optimized = true;

	return 0;
}

/*
 * spi_maybe_optimize_message - 如果消息还没预优化，就执行优化
 * @spi: 这条消息将使用的设备
 * @msg: 需要优化的消息
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
static int spi_maybe_optimize_message(struct spi_device *spi,
				      struct spi_message *msg)
{
	if (spi->controller->defer_optimize_message) {
		msg->spi = spi;
		return 0;
	}

	if (msg->pre_optimized)
		return 0;

	return __spi_optimize_message(spi, msg);
}

/**
 * spi_optimize_message - 对 SPI 消息做一次性校验和准备
 * @spi: 这条消息将使用的设备
 * @msg: 需要优化的消息
 *
 * 反复重用同一条消息的外设驱动可以调用这个接口，把尽量多的消息准备
 * 工作一次性做完，而不是每次传输时都重复做，以提高吞吐并减少 CPU 开销。
 *
 * 一旦消息被优化，除了修改 xfer->tx_buf 指向的内存内容之外，
 * 不能再修改消息本身（指针本身不能改）。
 *
 * 调用该函数必须与 spi_unoptimize_message() 成对使用，避免资源泄漏。
 *
 * Context: 可以睡眠
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_optimize_message(struct spi_device *spi, struct spi_message *msg)
{
	int ret;

	/*
	 * 不支持预优化；当使用 spi-mux 时，优化会被延后。
	 */
	if (spi->controller->defer_optimize_message)
		return 0;

	ret = __spi_optimize_message(spi, msg);
	if (ret)
		return ret;

	/*
	 * 这个标志表示外设驱动调用过 spi_optimize_message()，
	 * 因此在消息结束时不应自动反优化，而应该等驱动显式调用
	 * spi_unoptimize_message()。
	 */
	msg->pre_optimized = true;

	return 0;
}
EXPORT_SYMBOL_GPL(spi_optimize_message);

/**
 * spi_unoptimize_message - 释放 spi_optimize_message() 分配的资源
 * @msg: 需要反优化的消息
 *
 * 调用该函数必须与 spi_optimize_message() 成对使用。
 *
 * Context: 可以睡眠
 */
void spi_unoptimize_message(struct spi_message *msg)
{
	if (msg->spi->controller->defer_optimize_message)
		return;

	__spi_unoptimize_message(msg);
	msg->pre_optimized = false;
}
EXPORT_SYMBOL_GPL(spi_unoptimize_message);

static int __spi_async(struct spi_device *spi, struct spi_message *message)
{
	struct spi_controller *ctlr = spi->controller;
	struct spi_transfer *xfer;

	/*
	 * 某些控制器不支持普通 SPI 传输。遇到这种情况返回 ENOTSUPP。
	 */
	if (!ctlr->transfer)
		return -ENOTSUPP;

	SPI_STATISTICS_INCREMENT_FIELD(ctlr->pcpu_statistics, spi_async);
	SPI_STATISTICS_INCREMENT_FIELD(spi->pcpu_statistics, spi_async);

	trace_spi_message_submit(message);

	if (!ctlr->ptp_sts_supported) {
		list_for_each_entry(xfer, &message->transfers, transfer_list) {
			xfer->ptp_sts_word_pre = 0;
			ptp_read_system_prets(xfer->ptp_sts);
		}
	}

	return ctlr->transfer(spi, message);
}

static void devm_spi_unoptimize_message(void *msg)
{
	spi_unoptimize_message(msg);
}

/**
 * devm_spi_optimize_message - spi_optimize_message() 的受管理版本
 * @dev: 管理 @msg 的设备（通常是 @spi->dev）
 * @spi: 这条消息将使用的设备
 * @msg: 需要优化的消息
 *
 * 设备移除时会自动调用 spi_unoptimize_message()。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int devm_spi_optimize_message(struct device *dev, struct spi_device *spi,
			      struct spi_message *msg)
{
	int ret;

	ret = spi_optimize_message(spi, msg);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, devm_spi_unoptimize_message, msg);
}
EXPORT_SYMBOL_GPL(devm_spi_optimize_message);

/**
 * spi_async - 异步 SPI 传输
 * @spi: 交换数据所使用的设备
 * @message: 描述数据传输的消息，包括完成回调
 * Context: 任意上下文（可以在 IRQ 等不能睡眠的环境中调用）
 *
 * 这个接口既可以在 in_irq 以及其他不能睡眠的上下文中使用，
 * 也可以在普通任务上下文中使用。
 *
 * 完成回调会在不能睡眠的上下文中执行。
 * 在回调执行之前，message->status 的值是不确定的。
 * 回调触发时，message->status 要么是 0（表示完全成功），要么是负错误码。
 * 回调返回后，提交传输请求的驱动可以释放相关内存；SPI core 和
 * controller 驱动都不会再使用它。
 *
 * 虽然同一个 spi_device 的消息会按 FIFO 顺序处理，但不同设备之间
 * 的消息顺序不一定一样。例如某些设备可能优先级更高，或者有严格的
 * 访问时序要求。
 *
 * 传输期间一旦检测到任何错误，整条 message 的处理都会被中止，并且
 * 设备会被取消选择。
 * 在关联的消息完成回调返回之前，排队到该设备的其它 spi_message 不会
 * 被处理。
 * 这一规则对所有同步传输接口同样适用，因为它们只是这个异步原语的封装。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_async(struct spi_device *spi, struct spi_message *message)
{
	struct spi_controller *ctlr = spi->controller;
	int ret;
	unsigned long flags;

	ret = spi_maybe_optimize_message(spi, message);
	if (ret)
		return ret;

	spin_lock_irqsave(&ctlr->bus_lock_spinlock, flags);

	if (ctlr->bus_lock_flag)
		ret = -EBUSY;
	else
		ret = __spi_async(spi, message);

	spin_unlock_irqrestore(&ctlr->bus_lock_spinlock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(spi_async);

static void __spi_transfer_message_noqueue(struct spi_controller *ctlr, struct spi_message *msg)
{
	bool was_busy;
	int ret;

	mutex_lock(&ctlr->io_mutex);

	was_busy = ctlr->busy;

	ctlr->cur_msg = msg;
	ret = __spi_pump_transfer_message(ctlr, msg, was_busy);
	if (ret)
		dev_err(&ctlr->dev, "noqueue transfer failed\n");
	ctlr->cur_msg = NULL;
	ctlr->fallback = false;

	if (!was_busy) {
		kfree(ctlr->dummy_rx);
		ctlr->dummy_rx = NULL;
		kfree(ctlr->dummy_tx);
		ctlr->dummy_tx = NULL;
		if (ctlr->unprepare_transfer_hardware &&
		    ctlr->unprepare_transfer_hardware(ctlr))
			dev_err(&ctlr->dev,
				"failed to unprepare transfer hardware\n");
		spi_idle_runtime_pm(ctlr);
	}

	mutex_unlock(&ctlr->io_mutex);
}

/*-------------------------------------------------------------------------*/

/*
 * SPI 协议驱动的辅助方法，建立在 core 之上。
 * 其他一些辅助方法会以内联函数形式定义。
 */

static void spi_complete(void *arg)
{
	complete(arg);
}

static int __spi_sync(struct spi_device *spi, struct spi_message *message)
{
	DECLARE_COMPLETION_ONSTACK(done);
	unsigned long flags;
	int status;
	struct spi_controller *ctlr = spi->controller;

	if (__spi_check_suspended(ctlr)) {
		dev_warn_once(&spi->dev, "Attempted to sync while suspend\n");
		return -ESHUTDOWN;
	}

	status = spi_maybe_optimize_message(spi, message);
	if (status)
		return status;

	SPI_STATISTICS_INCREMENT_FIELD(ctlr->pcpu_statistics, spi_sync);
	SPI_STATISTICS_INCREMENT_FIELD(spi->pcpu_statistics, spi_sync);

	/*
	 * 这里检查 queue_empty 只能保证来自同一上下文的 async/sync
	 * 消息顺序一致。
	 * 它不需要防止来自不同上下文的重入，那些情况会由 io_mutex 处理。
	 */
	if (READ_ONCE(ctlr->queue_empty) && !ctlr->must_async) {
		message->actual_length = 0;
		message->status = -EINPROGRESS;

		trace_spi_message_submit(message);

		SPI_STATISTICS_INCREMENT_FIELD(ctlr->pcpu_statistics, spi_sync_immediate);
		SPI_STATISTICS_INCREMENT_FIELD(spi->pcpu_statistics, spi_sync_immediate);

		__spi_transfer_message_noqueue(ctlr, message);

		return message->status;
	}

	/*
	 * async 队列里可能有来自同一上下文的消息，所以必须保序。
	 * 因此这里把消息送入 async 队列，然后等待它完成。
	 */
	message->complete = spi_complete;
	message->context = &done;

	spin_lock_irqsave(&ctlr->bus_lock_spinlock, flags);
	status = __spi_async(spi, message);
	spin_unlock_irqrestore(&ctlr->bus_lock_spinlock, flags);

	if (status == 0) {
		wait_for_completion(&done);
		status = message->status;
	}
	message->complete = NULL;
	message->context = NULL;

	return status;
}

/**
 * spi_sync - 阻塞式 / 同步 SPI 数据传输
 * @spi: 交换数据所使用的设备
 * @message: 描述数据传输的消息
 * Context: 可以睡眠
 *
 * 这个接口只能在可以睡眠的上下文中使用。这里的睡眠不可中断，也没有
 * 超时。低开销的控制器驱动可以直接 DMA 到 message buffer 中。
 *
 * 注意，SPI 设备在 message 期间会保持片选有效，通常在两条消息之间才
 * 解除片选。某些高频使用的设备驱动可能希望尽量减少选片成本，
 * 因而会在预期下一条消息仍然发给同一芯片时保持片选，但这可能增加功耗。
 *
 * 此外，调用者需要保证和 message 关联的内存不会在本调用返回前被释放。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_sync(struct spi_device *spi, struct spi_message *message)
{
	int ret;

	mutex_lock(&spi->controller->bus_lock_mutex);
	ret = __spi_sync(spi, message);
	mutex_unlock(&spi->controller->bus_lock_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(spi_sync);

/**
 * spi_sync_locked - 带独占总线使用的 spi_sync 版本
 * @spi: 交换数据所使用的设备
 * @message: 描述数据传输的消息
 * Context: 可以睡眠
 *
 * 这个接口只能在可以睡眠的上下文中使用。这里的睡眠不可中断，也没有
 * 超时。低开销的控制器驱动可以直接 DMA 到 message buffer 中。
 *
 * 这个接口适合需要独占 SPI 总线的驱动使用。它必须先调用 spi_bus_lock()，
 * 独占访问结束后必须调用 spi_bus_unlock() 释放总线。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_sync_locked(struct spi_device *spi, struct spi_message *message)
{
	return __spi_sync(spi, message);
}
EXPORT_SYMBOL_GPL(spi_sync_locked);

/**
 * spi_bus_lock - 获取 SPI 总线独占锁
 * @ctlr: 需要被锁定以供独占访问的 SPI 总线控制器
 * Context: 可以睡眠
 *
 * 这个接口只能在可以睡眠的上下文中使用。这里的睡眠不可中断，也没有
 * 超时。
 *
 * 需要独占访问 SPI 总线的驱动应该使用这个接口。独占访问结束后必须
 * 调用 spi_bus_unlock() 释放。持有 SPI 总线锁时，数据传输必须通过
 * spi_sync_locked() 和 spi_async_locked() 完成。
 *
 * 返回：
 * 始终返回 0。
 */
int spi_bus_lock(struct spi_controller *ctlr)
{
	unsigned long flags;

	mutex_lock(&ctlr->bus_lock_mutex);

	spin_lock_irqsave(&ctlr->bus_lock_spinlock, flags);
	ctlr->bus_lock_flag = 1;
	spin_unlock_irqrestore(&ctlr->bus_lock_spinlock, flags);

	/* 这个 mutex 会一直保持锁定，直到调用 spi_bus_unlock()。 */

	return 0;
}
EXPORT_SYMBOL_GPL(spi_bus_lock);

/**
 * spi_bus_unlock - 释放 SPI 总线独占锁
 * @ctlr: 之前被独占锁锁定的 SPI 总线控制器
 * Context: 可以睡眠
 *
 * 这个接口只能在可以睡眠的上下文中使用。这里的睡眠不可中断，也没有
 * 超时。
 *
 * 该接口释放之前通过 spi_bus_lock() 获取的 SPI 总线锁。
 *
 * 返回：
 * 始终返回 0。
 */
int spi_bus_unlock(struct spi_controller *ctlr)
{
	ctlr->bus_lock_flag = 0;

	mutex_unlock(&ctlr->bus_lock_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(spi_bus_unlock);

/* 可移植代码传输时不要超过 32 字节。 */
#define	SPI_BUFSIZ	max(32, SMP_CACHE_BYTES)

static u8	*buf;

/**
 * spi_write_then_read - SPI 同步写后读
 * @spi: 交换数据所使用的设备
 * @txbuf: 要写出的数据（不要求 DMA-safe）
 * @n_tx: txbuf 的字节数
 * @rxbuf: 用于读取数据的缓冲区（不要求 DMA-safe）
 * @n_rx: rxbuf 的字节数
 * Context: 可以睡眠
 *
 * 该接口对设备执行一次半双工 MicroWire 风格的事务：先发送 txbuf，
 * 再读取 rxbuf。返回值成功时为 0，否则为负 errno。
 * 这个接口只能在可以睡眠的上下文中使用。
 *
 * 这个接口的参数总是通过一个小缓冲区拷贝完成。
 * 对性能敏感或大批量传输的代码应改用 spi_{async,sync}() 并配合
 * DMA-safe buffer。
 *
 * 返回：
 * 成功返回 0，否则返回负错误码。
 */
int spi_write_then_read(struct spi_device *spi,
		const void *txbuf, unsigned n_tx,
		void *rxbuf, unsigned n_rx)
{
	static DEFINE_MUTEX(lock);

	int			status;
	struct spi_message	message;
	struct spi_transfer	x[2];
	u8			*local_buf;

	/*
	 * 如果可以，就使用预分配的 DMA-safe 缓冲区。
	 * 这里无法避免拷贝（这是为了方便），但可以把堆分配成本挡在热路径之外，
	 * 除非别的调用正在占用这个预分配缓冲区，或者传输长度太大。
	 */
	if ((n_tx + n_rx) > SPI_BUFSIZ || !mutex_trylock(&lock)) {
		local_buf = kmalloc(max((unsigned)SPI_BUFSIZ, n_tx + n_rx),
				    GFP_KERNEL | GFP_DMA);
		if (!local_buf)
			return -ENOMEM;
	} else {
		local_buf = buf;
	}

	spi_message_init(&message);
	memset(x, 0, sizeof(x));
	if (n_tx) {
		x[0].len = n_tx;
		spi_message_add_tail(&x[0], &message);
	}
	if (n_rx) {
		x[1].len = n_rx;
		spi_message_add_tail(&x[1], &message);
	}

	memcpy(local_buf, txbuf, n_tx);
	x[0].tx_buf = local_buf;
	x[1].rx_buf = local_buf + n_tx;

	/* 执行 I/O。 */
	status = spi_sync(spi, &message);
	if (status == 0)
		memcpy(rxbuf, x[1].rx_buf, n_rx);

	if (x[0].tx_buf == buf)
		mutex_unlock(&lock);
	else
		kfree(local_buf);

	return status;
}
EXPORT_SYMBOL_GPL(spi_write_then_read);

/*-------------------------------------------------------------------------*/

#if IS_ENABLED(CONFIG_OF)
/* SPI 控制器不使用 spi_bus，因此这里要通过别的方式查找。 */
struct spi_controller *of_find_spi_controller_by_node(struct device_node *node)
{
	struct device *dev;

	dev = class_find_device_by_of_node(&spi_controller_class, node);
	if (!dev && IS_ENABLED(CONFIG_SPI_SLAVE))
		dev = class_find_device_by_of_node(&spi_target_class, node);
	if (!dev)
		return NULL;

	/* class_find_device() 已经帮我们拿到了一次引用。 */
	return container_of(dev, struct spi_controller, dev);
}
EXPORT_SYMBOL_GPL(of_find_spi_controller_by_node);
#endif

#if IS_ENABLED(CONFIG_OF_DYNAMIC)
/* 用完返回的 spi_device 后必须调用 put_device()。 */
static struct spi_device *of_find_spi_device_by_node(struct device_node *node)
{
	struct device *dev = bus_find_device_by_of_node(&spi_bus_type, node);

	return dev ? to_spi_device(dev) : NULL;
}

static int of_spi_notify(struct notifier_block *nb, unsigned long action,
			 void *arg)
{
	struct of_reconfig_data *rd = arg;
	struct spi_controller *ctlr;
	struct spi_device *spi;

	switch (of_reconfig_get_state_change(action, arg)) {
	case OF_RECONFIG_CHANGE_ADD:
		ctlr = of_find_spi_controller_by_node(rd->dn->parent);
		if (ctlr == NULL)
			return NOTIFY_OK;	/* Not for us */

		if (of_node_test_and_set_flag(rd->dn, OF_POPULATED)) {
			put_device(&ctlr->dev);
			return NOTIFY_OK;
		}

		/*
		 * 在添加设备之前先清除该标志，这样 fw_devlink 就不会跳过
		 * 为该设备添加消费者。
		 */
		fwnode_clear_flag(&rd->dn->fwnode, FWNODE_FLAG_NOT_DEVICE);
		spi = of_register_spi_device(ctlr, rd->dn);
		put_device(&ctlr->dev);

		if (IS_ERR(spi)) {
			pr_err("%s: failed to create for '%pOF'\n",
					__func__, rd->dn);
			of_node_clear_flag(rd->dn, OF_POPULATED);
			return notifier_from_errno(PTR_ERR(spi));
		}
		break;

	case OF_RECONFIG_CHANGE_REMOVE:
		/* 已经完成 depopulate 了吗？ */
		if (!of_node_check_flag(rd->dn, OF_POPULATED))
			return NOTIFY_OK;

		/* 通过节点找到对应设备。 */
		spi = of_find_spi_device_by_node(rd->dn);
		if (spi == NULL)
			return NOTIFY_OK;	/* No? not meant for us */

		/* unregister 会减少一个引用计数。 */
		spi_unregister_device(spi);

		/* 然后释放查找时拿到的引用。 */
		put_device(&spi->dev);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block spi_of_notifier = {
	.notifier_call = of_spi_notify,
};
#else /* IS_ENABLED(CONFIG_OF_DYNAMIC) */
extern struct notifier_block spi_of_notifier;
#endif /* IS_ENABLED(CONFIG_OF_DYNAMIC) */

#if IS_ENABLED(CONFIG_ACPI)
static int spi_acpi_controller_match(struct device *dev, const void *data)
{
	return device_match_acpi_dev(dev->parent, data);
}

struct spi_controller *acpi_spi_find_controller_by_adev(struct acpi_device *adev)
{
	struct device *dev;

	dev = class_find_device(&spi_controller_class, NULL, adev,
				spi_acpi_controller_match);
	if (!dev && IS_ENABLED(CONFIG_SPI_SLAVE))
		dev = class_find_device(&spi_target_class, NULL, adev,
					spi_acpi_controller_match);
	if (!dev)
		return NULL;

	return container_of(dev, struct spi_controller, dev);
}
EXPORT_SYMBOL_GPL(acpi_spi_find_controller_by_adev);

static struct spi_device *acpi_spi_find_device_by_adev(struct acpi_device *adev)
{
	struct device *dev;

	dev = bus_find_device_by_acpi_dev(&spi_bus_type, adev);
	return to_spi_device(dev);
}

static int acpi_spi_notify(struct notifier_block *nb, unsigned long value,
			   void *arg)
{
	struct acpi_device *adev = arg;
	struct spi_controller *ctlr;
	struct spi_device *spi;

	switch (value) {
	case ACPI_RECONFIG_DEVICE_ADD:
		ctlr = acpi_spi_find_controller_by_adev(acpi_dev_parent(adev));
		if (!ctlr)
			break;

		acpi_register_spi_device(ctlr, adev);
		put_device(&ctlr->dev);
		break;
	case ACPI_RECONFIG_DEVICE_REMOVE:
		if (!acpi_device_enumerated(adev))
			break;

		spi = acpi_spi_find_device_by_adev(adev);
		if (!spi)
			break;

		spi_unregister_device(spi);
		put_device(&spi->dev);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block spi_acpi_notifier = {
	.notifier_call = acpi_spi_notify,
};
#else
extern struct notifier_block spi_acpi_notifier;
#endif

static int __init spi_init(void)
{
	int	status;

	buf = kmalloc(SPI_BUFSIZ, GFP_KERNEL);
	if (!buf) {
		status = -ENOMEM;
		goto err0;
	}

	status = bus_register(&spi_bus_type);
	if (status < 0)
		goto err1;

	status = class_register(&spi_controller_class);
	if (status < 0)
		goto err2;

	if (IS_ENABLED(CONFIG_SPI_SLAVE)) {
		status = class_register(&spi_target_class);
		if (status < 0)
			goto err3;
	}

	if (IS_ENABLED(CONFIG_OF_DYNAMIC))
		WARN_ON(of_reconfig_notifier_register(&spi_of_notifier));
	if (IS_ENABLED(CONFIG_ACPI))
		WARN_ON(acpi_reconfig_notifier_register(&spi_acpi_notifier));

	return 0;

err3:
	class_unregister(&spi_controller_class);
err2:
	bus_unregister(&spi_bus_type);
err1:
	kfree(buf);
	buf = NULL;
err0:
	return status;
}

/*
 * board_info 通常在 arch_initcall() 期间注册，
 * 但即使是关键驱动也可能要等到更晚才初始化。
 *
 * 这里值得重新考虑的是：真正需要静态链接的可能只有 boardinfo。
 * 其余部分（设备和驱动注册）其实可以动态链接（模块化）……
 * 代价是需要让 boardinfo 数据结构暴露得更多。
 */
postcore_initcall(spi_init);
