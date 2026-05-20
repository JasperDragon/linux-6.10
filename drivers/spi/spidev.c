// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 面向 SPI 设备的简单同步用户态接口
 *
 * Copyright (C) 2006 SWAPP
 *	Andrea Paterniani <a.paterniani@swapp-eng.it>
 * Copyright (C) 2007 David Brownell (simplification, cleanup)
 */

#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/compat.h>

#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>

#include <linux/uaccess.h>


/*
 * 这个驱动通过普通的用户态 I/O 调用来访问 SPI 设备。
 * 需要注意的是，传统 UNIX/POSIX I/O 语义通常偏向半双工，
 * 而且常常会掩盖消息边界；但完整的 SPI 访问往往需要全双工
 * 传输。为此，驱动内部要显式处理消息边界、片选切换以及
 * 其它协议选项。
 *
 * SPI 已经分配了字符设备主设备号。这里通过位图动态分配
 * 次设备号。由于次设备号与具体的 SPI 总线或设备没有固定
 * 对应关系，因此必须依赖 udev 或 mdev 这类热插拔工具来
 * 创建和删除 /dev/spidevB.C 节点。
 */
#define SPIDEV_MAJOR			153	/* assigned */
#define N_SPI_MINORS			32	/* ... up to 256 */

static DECLARE_BITMAP(minors, N_SPI_MINORS);

static_assert(N_SPI_MINORS > 0 && N_SPI_MINORS <= 256);

/* spi_device.mode 的位掩码。错误的设置会给共享总线上的其他设备
 * 带来很大麻烦：
 *
 *  - CS_HIGH ... 设备会在不该激活时被激活
 *  - 3WIRE ... 设备激活时不会按预期工作
 *  - NO_CS ... 没有明确的消息边界；这与共享总线模型完全冲突
 *  - READY ... 传输可能在不该继续时继续
 *
 * 是否应该把这些标志的修改限制为特权操作，仍然值得重新评估。
 */
#define SPI_MODE_MASK		(SPI_MODE_X_MASK | SPI_CS_HIGH \
				| SPI_LSB_FIRST | SPI_3WIRE | SPI_LOOP \
				| SPI_NO_CS | SPI_READY | SPI_TX_DUAL \
				| SPI_TX_QUAD | SPI_TX_OCTAL | SPI_RX_DUAL \
				| SPI_RX_QUAD | SPI_RX_OCTAL \
				| SPI_RX_CPHA_FLIP | SPI_3WIRE_HIZ \
				| SPI_MOSI_IDLE_LOW)

struct spidev_data {
	dev_t			devt;
	struct mutex		spi_lock;
	struct spi_device	*spi;
	struct list_head	device_entry;

	/* 除非设备已打开（users > 0），否则 TX/RX 缓冲区保持为 NULL。 */
	unsigned		users;
	u8			*tx_buffer;
	u8			*rx_buffer;
	u32			speed_hz;
};

static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);

static unsigned bufsiz = 4096;
module_param(bufsiz, uint, S_IRUGO);
MODULE_PARM_DESC(bufsiz, "data bytes in biggest supported SPI message");

/*-------------------------------------------------------------------------*/

static ssize_t
spidev_sync_unlocked(struct spi_device *spi, struct spi_message *message)
{
	ssize_t status;

	status = spi_sync(spi, message);
	if (status == 0)
		status = message->actual_length;

	return status;
}

static inline ssize_t
spidev_sync_write(struct spidev_data *spidev, size_t len)
{
	struct spi_transfer	t = {
			.tx_buf		= spidev->tx_buffer,
			.len		= len,
			.speed_hz	= spidev->speed_hz,
		};
	struct spi_message	m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	return spidev_sync_unlocked(spidev->spi, &m);
}

static inline ssize_t
spidev_sync_read(struct spidev_data *spidev, size_t len)
{
	struct spi_transfer	t = {
			.rx_buf		= spidev->rx_buffer,
			.len		= len,
			.speed_hz	= spidev->speed_hz,
		};
	struct spi_message	m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	return spidev_sync_unlocked(spidev->spi, &m);
}

/*-------------------------------------------------------------------------*/

/* 基于当前设备配置执行只读消息。 */
static ssize_t
spidev_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct spidev_data	*spidev;
	ssize_t			status = -ESHUTDOWN;

	/* 片选只在操作开始或结束时翻转。 */
	if (count > bufsiz)
		return -EMSGSIZE;

	spidev = filp->private_data;

	mutex_lock(&spidev->spi_lock);

	if (spidev->spi == NULL)
		goto err_spi_removed;

	status = spidev_sync_read(spidev, count);
	if (status > 0) {
		unsigned long	missing;

		missing = copy_to_user(buf, spidev->rx_buffer, status);
		if (missing == status)
			status = -EFAULT;
		else
			status = status - missing;
	}

err_spi_removed:
	mutex_unlock(&spidev->spi_lock);

	return status;
}

/* 基于当前设备配置执行只写消息。 */
static ssize_t
spidev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	struct spidev_data	*spidev;
	ssize_t			status = -ESHUTDOWN;
	unsigned long		missing;

	/* 片选只在操作开始或结束时翻转。 */
	if (count > bufsiz)
		return -EMSGSIZE;

	spidev = filp->private_data;

	mutex_lock(&spidev->spi_lock);

	if (spidev->spi == NULL)
		goto err_spi_removed;

	missing = copy_from_user(spidev->tx_buffer, buf, count);
	if (missing == 0)
		status = spidev_sync_write(spidev, count);
	else
		status = -EFAULT;

err_spi_removed:
	mutex_unlock(&spidev->spi_lock);

	return status;
}

static int spidev_message(struct spidev_data *spidev,
		struct spi_ioc_transfer *u_xfers, unsigned n_xfers)
{
	struct spi_message	msg;
	struct spi_transfer	*k_xfers;
	struct spi_transfer	*k_tmp;
	struct spi_ioc_transfer *u_tmp;
	unsigned		n, total, tx_total, rx_total;
	u8			*tx_buf, *rx_buf;
	int			status = -EFAULT;

	spi_message_init(&msg);
	k_xfers = kzalloc_objs(*k_tmp, n_xfers);
	if (k_xfers == NULL)
		return -ENOMEM;

	/* 构造 spi_message，并把所有发送数据复制到 bounce buffer。
	 * 我们遍历用户提供的 transfer 数组，为每一项生成对应的
	 * 内核态 transfer 结构。
	 */
	tx_buf = spidev->tx_buffer;
	rx_buf = spidev->rx_buffer;
	total = 0;
	tx_total = 0;
	rx_total = 0;
	for (n = n_xfers, k_tmp = k_xfers, u_tmp = u_xfers;
			n;
			n--, k_tmp++, u_tmp++) {
		/* 确保后续从 rx_buf/tx_buf 继续分配时也满足 DMA 对齐要求。 */
		unsigned int len_aligned = ALIGN(u_tmp->len, ARCH_DMA_MINALIGN);

		k_tmp->len = u_tmp->len;

		total += k_tmp->len;
		/* 由于函数成功时返回总传输长度，因此必须把总长度限制在
		 * 正的 int 范围内，避免返回值看起来像错误码。同时还要检查
		 * 每个 transfer 的长度，防止算术溢出。
		 */
		if (total > INT_MAX || k_tmp->len > INT_MAX) {
			status = -EMSGSIZE;
			goto done;
		}

		if (u_tmp->rx_buf) {
			/* 这个 transfer 需要 RX bounce buffer 空间。 */
			rx_total += len_aligned;
			if (rx_total > bufsiz) {
				status = -EMSGSIZE;
				goto done;
			}
			k_tmp->rx_buf = rx_buf;
			rx_buf += len_aligned;
		}
		if (u_tmp->tx_buf) {
			/* 这个 transfer 需要 TX bounce buffer 空间。 */
			tx_total += len_aligned;
			if (tx_total > bufsiz) {
				status = -EMSGSIZE;
				goto done;
			}
			k_tmp->tx_buf = tx_buf;
			if (copy_from_user(tx_buf, (const u8 __user *)
						(uintptr_t) u_tmp->tx_buf,
					u_tmp->len))
				goto done;
			tx_buf += len_aligned;
		}

		k_tmp->cs_change = !!u_tmp->cs_change;
		k_tmp->tx_nbits = u_tmp->tx_nbits;
		k_tmp->rx_nbits = u_tmp->rx_nbits;
		k_tmp->bits_per_word = u_tmp->bits_per_word;
		k_tmp->delay.value = u_tmp->delay_usecs;
		k_tmp->delay.unit = SPI_DELAY_UNIT_USECS;
		k_tmp->speed_hz = u_tmp->speed_hz;
		k_tmp->word_delay.value = u_tmp->word_delay_usecs;
		k_tmp->word_delay.unit = SPI_DELAY_UNIT_USECS;
		if (!k_tmp->speed_hz)
			k_tmp->speed_hz = spidev->speed_hz;
#ifdef VERBOSE
		dev_dbg(&spidev->spi->dev,
			"  xfer len %u %s%s%s%dbits %u usec %u usec %uHz\n",
			k_tmp->len,
			k_tmp->rx_buf ? "rx " : "",
			k_tmp->tx_buf ? "tx " : "",
			k_tmp->cs_change ? "cs " : "",
			k_tmp->bits_per_word ? : spidev->spi->bits_per_word,
			k_tmp->delay.value,
			k_tmp->word_delay.value,
			k_tmp->speed_hz ? : spidev->spi->max_speed_hz);
#endif
		spi_message_add_tail(k_tmp, &msg);
	}

	status = spidev_sync_unlocked(spidev->spi, &msg);
	if (status < 0)
		goto done;

	/* 将收到的数据从 bounce buffer 拷贝出去。 */
	for (n = n_xfers, k_tmp = k_xfers, u_tmp = u_xfers;
			n;
			n--, k_tmp++, u_tmp++) {
		if (u_tmp->rx_buf) {
			if (copy_to_user((u8 __user *)
					(uintptr_t) u_tmp->rx_buf, k_tmp->rx_buf,
					u_tmp->len)) {
				status = -EFAULT;
				goto done;
			}
		}
	}
	status = total;

done:
	kfree(k_xfers);
	return status;
}

static struct spi_ioc_transfer *
spidev_get_ioc_message(unsigned int cmd, struct spi_ioc_transfer __user *u_ioc,
		unsigned *n_ioc)
{
	u32	tmp;

	/* 检查类型、命令号和方向。 */
	if (_IOC_TYPE(cmd) != SPI_IOC_MAGIC
			|| _IOC_NR(cmd) != _IOC_NR(SPI_IOC_MESSAGE(0))
			|| _IOC_DIR(cmd) != _IOC_WRITE)
		return ERR_PTR(-ENOTTY);

	tmp = _IOC_SIZE(cmd);
	if ((tmp % sizeof(struct spi_ioc_transfer)) != 0)
		return ERR_PTR(-EINVAL);
	*n_ioc = tmp / sizeof(struct spi_ioc_transfer);
	if (*n_ioc == 0)
		return NULL;

	/* 拷贝到临时缓冲区。 */
	return memdup_user(u_ioc, tmp);
}

static long
spidev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int			retval = 0;
	struct spidev_data	*spidev;
	struct spi_device	*spi;
	struct spi_controller	*ctlr;
	u32			tmp;
	unsigned		n_ioc;
	struct spi_ioc_transfer	*ioc;

	/* 检查类型和命令号。 */
	if (_IOC_TYPE(cmd) != SPI_IOC_MAGIC)
		return -ENOTTY;

	/* 防止在执行 ioctl 之前或执行期间设备被移除。 */
	spidev = filp->private_data;
	mutex_lock(&spidev->spi_lock);
	spi = spi_dev_get(spidev->spi);
	if (spi == NULL) {
		mutex_unlock(&spidev->spi_lock);
		return -ESHUTDOWN;
	}

	ctlr = spi->controller;

	switch (cmd) {
	/* 读请求。 */
	case SPI_IOC_RD_MODE:
	case SPI_IOC_RD_MODE32:
		tmp = spi->mode & SPI_MODE_MASK;

		if (ctlr->use_gpio_descriptors && spi_get_csgpiod(spi, 0))
			tmp &= ~SPI_CS_HIGH;

		if (cmd == SPI_IOC_RD_MODE)
			retval = put_user(tmp, (__u8 __user *)arg);
		else
			retval = put_user(tmp, (__u32 __user *)arg);
		break;
	case SPI_IOC_RD_LSB_FIRST:
		retval = put_user((spi->mode & SPI_LSB_FIRST) ?  1 : 0,
					(__u8 __user *)arg);
		break;
	case SPI_IOC_RD_BITS_PER_WORD:
		retval = put_user(spi->bits_per_word, (__u8 __user *)arg);
		break;
	case SPI_IOC_RD_MAX_SPEED_HZ:
		retval = put_user(spidev->speed_hz, (__u32 __user *)arg);
		break;

	/* 写请求。 */
	case SPI_IOC_WR_MODE:
	case SPI_IOC_WR_MODE32:
		if (cmd == SPI_IOC_WR_MODE)
			retval = get_user(tmp, (u8 __user *)arg);
		else
			retval = get_user(tmp, (u32 __user *)arg);
		if (retval == 0) {
			u32	save = spi->mode;

			if (tmp & ~SPI_MODE_MASK) {
				retval = -EINVAL;
				break;
			}

			if (ctlr->use_gpio_descriptors && spi_get_csgpiod(spi, 0))
				tmp |= SPI_CS_HIGH;

			tmp |= spi->mode & ~SPI_MODE_MASK;
			spi->mode = tmp & SPI_MODE_USER_MASK;
			retval = spi_setup(spi);
			if (retval < 0)
				spi->mode = save;
			else
				dev_dbg(&spi->dev, "spi mode %x\n", tmp);
		}
		break;
	case SPI_IOC_WR_LSB_FIRST:
		retval = get_user(tmp, (__u8 __user *)arg);
		if (retval == 0) {
			u32	save = spi->mode;

			if (tmp)
				spi->mode |= SPI_LSB_FIRST;
			else
				spi->mode &= ~SPI_LSB_FIRST;
			retval = spi_setup(spi);
			if (retval < 0)
				spi->mode = save;
			else
				dev_dbg(&spi->dev, "%csb first\n",
						tmp ? 'l' : 'm');
		}
		break;
	case SPI_IOC_WR_BITS_PER_WORD:
		retval = get_user(tmp, (__u8 __user *)arg);
		if (retval == 0) {
			u8	save = spi->bits_per_word;

			spi->bits_per_word = tmp;
			retval = spi_setup(spi);
			if (retval < 0)
				spi->bits_per_word = save;
			else
				dev_dbg(&spi->dev, "%d bits per word\n", tmp);
		}
		break;
	case SPI_IOC_WR_MAX_SPEED_HZ: {
		u32 save;

		retval = get_user(tmp, (__u32 __user *)arg);
		if (retval)
			break;
		if (tmp == 0) {
			retval = -EINVAL;
			break;
		}

		save = spi->max_speed_hz;

		spi->max_speed_hz = tmp;
		retval = spi_setup(spi);
		if (retval == 0) {
			spidev->speed_hz = tmp;
			dev_dbg(&spi->dev, "%d Hz (max)\n", spidev->speed_hz);
		}

		spi->max_speed_hz = save;
		break;
	}
	default:
		/* 分段和/或全双工 I/O 请求。 */
		/* 检查消息并拷贝到临时缓冲区。 */
		ioc = spidev_get_ioc_message(cmd,
				(struct spi_ioc_transfer __user *)arg, &n_ioc);
		if (IS_ERR(ioc)) {
			retval = PTR_ERR(ioc);
			break;
		}
		if (!ioc)
			break;	/* n_ioc is also 0 */

		/* 转换为 spi_message 并执行。 */
		retval = spidev_message(spidev, ioc, n_ioc);
		kfree(ioc);
		break;
	}

	spi_dev_put(spi);
	mutex_unlock(&spidev->spi_lock);
	return retval;
}

#ifdef CONFIG_COMPAT
static long
spidev_compat_ioc_message(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct spi_ioc_transfer __user	*u_ioc;
	int				retval = 0;
	struct spidev_data		*spidev;
	struct spi_device		*spi;
	unsigned			n_ioc, n;
	struct spi_ioc_transfer		*ioc;

	u_ioc = (struct spi_ioc_transfer __user *) compat_ptr(arg);

	/* 防止在执行 ioctl 之前或执行期间设备被移除。 */
	spidev = filp->private_data;
	mutex_lock(&spidev->spi_lock);
	spi = spi_dev_get(spidev->spi);
	if (spi == NULL) {
		mutex_unlock(&spidev->spi_lock);
		return -ESHUTDOWN;
	}

	/* 检查消息并拷贝到临时缓冲区。 */
	ioc = spidev_get_ioc_message(cmd, u_ioc, &n_ioc);
	if (IS_ERR(ioc)) {
		retval = PTR_ERR(ioc);
		goto done;
	}
	if (!ioc)
		goto done;	/* n_ioc is also 0 */

	/* 转换缓冲区指针。 */
	for (n = 0; n < n_ioc; n++) {
		ioc[n].rx_buf = (uintptr_t) compat_ptr(ioc[n].rx_buf);
		ioc[n].tx_buf = (uintptr_t) compat_ptr(ioc[n].tx_buf);
	}

	/* 转换为 spi_message 并执行。 */
	retval = spidev_message(spidev, ioc, n_ioc);
	kfree(ioc);

done:
	spi_dev_put(spi);
	mutex_unlock(&spidev->spi_lock);
	return retval;
}

static long
spidev_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	if (_IOC_TYPE(cmd) == SPI_IOC_MAGIC
			&& _IOC_NR(cmd) == _IOC_NR(SPI_IOC_MESSAGE(0))
			&& _IOC_DIR(cmd) == _IOC_WRITE)
		return spidev_compat_ioc_message(filp, cmd, arg);

	return spidev_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#else
#define spidev_compat_ioctl NULL
#endif /* CONFIG_COMPAT */

static int spidev_open(struct inode *inode, struct file *filp)
{
	struct spidev_data	*spidev = NULL, *iter;
	int			status = -ENXIO;

	mutex_lock(&device_list_lock);

	list_for_each_entry(iter, &device_list, device_entry) {
		if (iter->devt == inode->i_rdev) {
			status = 0;
			spidev = iter;
			break;
		}
	}

	if (!spidev) {
		pr_debug("spidev: nothing for minor %d\n", iminor(inode));
		goto err_find_dev;
	}

	if (!spidev->tx_buffer) {
		spidev->tx_buffer = kmalloc(bufsiz, GFP_KERNEL);
		if (!spidev->tx_buffer) {
			status = -ENOMEM;
			goto err_find_dev;
		}
	}

	if (!spidev->rx_buffer) {
		spidev->rx_buffer = kmalloc(bufsiz, GFP_KERNEL);
		if (!spidev->rx_buffer) {
			status = -ENOMEM;
			goto err_alloc_rx_buf;
		}
	}

	spidev->users++;
	filp->private_data = spidev;
	stream_open(inode, filp);

	mutex_unlock(&device_list_lock);
	return 0;

err_alloc_rx_buf:
	kfree(spidev->tx_buffer);
	spidev->tx_buffer = NULL;
err_find_dev:
	mutex_unlock(&device_list_lock);
	return status;
}

static int spidev_release(struct inode *inode, struct file *filp)
{
	struct spidev_data	*spidev;
	int			dofree;

	mutex_lock(&device_list_lock);
	spidev = filp->private_data;
	filp->private_data = NULL;

	mutex_lock(&spidev->spi_lock);
	/* ... 在我们从底层设备解绑之后？ */
	dofree = (spidev->spi == NULL);
	mutex_unlock(&spidev->spi_lock);

	/* 最后一次关闭？ */
	spidev->users--;
	if (!spidev->users) {

		kfree(spidev->tx_buffer);
		spidev->tx_buffer = NULL;

		kfree(spidev->rx_buffer);
		spidev->rx_buffer = NULL;

		if (dofree)
			kfree(spidev);
		else
			spidev->speed_hz = spidev->spi->max_speed_hz;
	}
#ifdef CONFIG_SPI_SLAVE
	if (!dofree)
		spi_target_abort(spidev->spi);
#endif
	mutex_unlock(&device_list_lock);

	return 0;
}

static const struct file_operations spidev_fops = {
	.owner =	THIS_MODULE,
	/* 仍需考虑切换到 AIO 原语，这样用户态能获得更完整的 API 覆盖；
	 * 除了锁处理之外，整体实现也会更简洁。
	 */
	.write =	spidev_write,
	.read =		spidev_read,
	.unlocked_ioctl = spidev_ioctl,
	.compat_ioctl = spidev_compat_ioctl,
	.open =		spidev_open,
	.release =	spidev_release,
};

/*-------------------------------------------------------------------------*/

/* 这个 class 的主要作用是让 mdev/udev 创建 /dev/spidevB.C 字符设备，
 * 以暴露用户态 API。同时它也简化了内存管理。
 */

static const struct class spidev_class = {
	.name = "spidev",
};

/*
 * 这里的 spi device id 需要与下面 spidev_dt_ids 数组里的设备名对应，
 * 并且两个数组的顺序必须保持一致。
 */
static const struct spi_device_id spidev_spi_ids[] = {
	{ .name = /* abb */ "spi-sensor" },
	{ .name = /* arduino */ "unoq-mcu" },
	{ .name = /* cisco */ "spi-petra" },
	{ .name = /* dh */ "dhcom-board" },
	{ .name = /* elgin */ "jg10309-01" },
	{ .name = /* gocontroll */ "moduline-module-slot"},
	{ .name = /* lineartechnology */ "ltc2488" },
	{ .name = /* lwn */ "bk4" },
	{ .name = /* lwn */ "bk4-spi" },
	{ .name = /* menlo */ "m53cpld" },
	{ .name = /* micron */ "spi-authenta" },
	{ .name = /* rohm */ "bh2228fv" },
	{ .name = /* rohm */ "dh2228fv" },
	{ .name = /* semtech */ "sx1301" },
	{ .name = /* silabs */ "em3581" },
	{ .name = /* silabs */ "si3210" },
	{},
};
MODULE_DEVICE_TABLE(spi, spidev_spi_ids);

/*
 * 在设备树里不应该直接引用 spidev，除非有明确的 compatible 字符串。
 * 因为它是 Linux 的实现细节，而不是硬件本身的描述。
 */
static int spidev_of_check(struct device *dev)
{
	if (device_property_match_string(dev, "compatible", "spidev") < 0)
		return 0;

	dev_err(dev, "spidev listed directly in DT is not supported\n");
	return -EINVAL;
}

static const struct of_device_id spidev_dt_ids[] = {
	{ .compatible = "abb,spi-sensor", .data = &spidev_of_check },
	{ .compatible = "arduino,unoq-mcu", .data = &spidev_of_check },
	{ .compatible = "cisco,spi-petra", .data = &spidev_of_check },
	{ .compatible = "dh,dhcom-board", .data = &spidev_of_check },
	{ .compatible = "elgin,jg10309-01", .data = &spidev_of_check },
	{ .compatible = "gocontroll,moduline-module-slot", .data = &spidev_of_check},
	{ .compatible = "lineartechnology,ltc2488", .data = &spidev_of_check },
	{ .compatible = "lwn,bk4", .data = &spidev_of_check },
	{ .compatible = "lwn,bk4-spi", .data = &spidev_of_check },
	{ .compatible = "menlo,m53cpld", .data = &spidev_of_check },
	{ .compatible = "micron,spi-authenta", .data = &spidev_of_check },
	{ .compatible = "rohm,bh2228fv", .data = &spidev_of_check },
	{ .compatible = "rohm,dh2228fv", .data = &spidev_of_check },
	{ .compatible = "semtech,sx1301", .data = &spidev_of_check },
	{ .compatible = "silabs,em3581", .data = &spidev_of_check },
	{ .compatible = "silabs,si3210", .data = &spidev_of_check },
	{},
};
MODULE_DEVICE_TABLE(of, spidev_dt_ids);

/* 这些只是演示/调试用的 SPI 设备，不应用于生产系统。 */
static int spidev_acpi_check(struct device *dev)
{
	dev_warn(dev, "do not use this driver in production systems!\n");
	return 0;
}

static const struct acpi_device_id spidev_acpi_ids[] = {
	/*
	 * ACPI 的 SPT000* 设备仅用于开发和测试。
	 * 生产系统应该为外设提供完整的 ACPI 描述，并使用正式驱动，
	 * 而不是直接去操作 SPI 总线。
	 */
	{ "SPT0001", (kernel_ulong_t)&spidev_acpi_check },
	{ "SPT0002", (kernel_ulong_t)&spidev_acpi_check },
	{ "SPT0003", (kernel_ulong_t)&spidev_acpi_check },
	{},
};
MODULE_DEVICE_TABLE(acpi, spidev_acpi_ids);

/*-------------------------------------------------------------------------*/

static int spidev_probe(struct spi_device *spi)
{
	int (*match)(struct device *dev);
	struct spidev_data	*spidev;
	int			status;
	unsigned long		minor;

	match = device_get_match_data(&spi->dev);
	if (match) {
		status = match(&spi->dev);
		if (status)
			return status;
	}

	/* 分配驱动私有数据。 */
	spidev = kzalloc_obj(*spidev);
	if (!spidev)
		return -ENOMEM;

	/* 初始化驱动数据。 */
	spidev->spi = spi;
	mutex_init(&spidev->spi_lock);

	INIT_LIST_HEAD(&spidev->device_entry);

	/* 如果能分配到次设备号，就把该设备挂上去。
	 * 只要 udev 或 mdev 正常工作，复用次设备号是没问题的。
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		struct device *dev;

		spidev->devt = MKDEV(SPIDEV_MAJOR, minor);
		dev = device_create(&spidev_class, &spi->dev, spidev->devt,
				    spidev, "spidev%d.%d",
				    spi->controller->bus_num, spi_get_chipselect(spi, 0));
		status = PTR_ERR_OR_ZERO(dev);
	} else {
		dev_dbg(&spi->dev, "no minor number available!\n");
		status = -ENODEV;
	}
	if (status == 0) {
		set_bit(minor, minors);
		list_add(&spidev->device_entry, &device_list);
	}
	mutex_unlock(&device_list_lock);

	spidev->speed_hz = spi->max_speed_hz;

	if (status == 0)
		spi_set_drvdata(spi, spidev);
	else
		kfree(spidev);

	return status;
}

static void spidev_remove(struct spi_device *spi)
{
	struct spidev_data	*spidev = spi_get_drvdata(spi);

	/* 阻止新的打开。 */
	mutex_lock(&device_list_lock);
	/* 确保现有文件描述符上的操作能够干净地中止。 */
	mutex_lock(&spidev->spi_lock);
	spidev->spi = NULL;
	mutex_unlock(&spidev->spi_lock);

	list_del(&spidev->device_entry);
	device_destroy(&spidev_class, spidev->devt);
	clear_bit(MINOR(spidev->devt), minors);
	if (spidev->users == 0)
		kfree(spidev);
	mutex_unlock(&device_list_lock);
}

static struct spi_driver spidev_spi_driver = {
	.driver = {
		.name =		"spidev",
		.of_match_table = spidev_dt_ids,
		.acpi_match_table = spidev_acpi_ids,
	},
	.probe =	spidev_probe,
	.remove =	spidev_remove,
	.id_table =	spidev_spi_ids,

	/* 注意：这里不需要 suspend/resume 方法。
	 * 我们只是把请求转发给/转回底层控制器，本身并不做额外处理。
	 * 大部分挂起/恢复问题由 refrigerator 处理，其余由控制器驱动处理。
	 */
};

/*-------------------------------------------------------------------------*/

static int __init spidev_init(void)
{
	int status;

	/* 先申请保留的 256 个设备号，再注册 class 以便 udev/mdev
	 * 增删 /dev 节点，最后注册管理这些设备号的驱动。
	 */
	status = register_chrdev(SPIDEV_MAJOR, "spi", &spidev_fops);
	if (status < 0)
		return status;

	status = class_register(&spidev_class);
	if (status) {
		unregister_chrdev(SPIDEV_MAJOR, spidev_spi_driver.driver.name);
		return status;
	}

	status = spi_register_driver(&spidev_spi_driver);
	if (status < 0) {
		class_unregister(&spidev_class);
		unregister_chrdev(SPIDEV_MAJOR, spidev_spi_driver.driver.name);
	}
	return status;
}
module_init(spidev_init);

static void __exit spidev_exit(void)
{
	spi_unregister_driver(&spidev_spi_driver);
	class_unregister(&spidev_class);
	unregister_chrdev(SPIDEV_MAJOR, spidev_spi_driver.driver.name);
}
module_exit(spidev_exit);

MODULE_AUTHOR("Andrea Paterniani, <a.paterniani@swapp-eng.it>");
MODULE_DESCRIPTION("User mode SPI device interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:spidev");
