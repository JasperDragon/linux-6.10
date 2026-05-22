// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * i2c-dev.c - I2C 总线驱动的字符设备接口
 *
 * Copyright (C) 1995-97 Simon G. Vogl
 * Copyright (C) 1998-99 Frodo Looijaard <frodol@dds.nl>
 * Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
 */

/*
 * 注意：这个文件是对 Simon Vogl 的 i2c-dev 模块的完整重写。
 * 不过这里沿用了他原始代码和设计中的大量内容，所以仍应把
 * 他视为共同作者。
 */

/* I2C_RDWR ioctl 代码由 Kolja Waschk <waschk@telos.de> 编写。 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/*
 * 一个 i2c_dev 对应一个 i2c_adapter，也就是 I2C/SMBus 主机，
 * 而不是与之交换消息的从设备（i2c_client）。
 * 它绑定到一个字符设备节点，供用户态驱动访问。
 *
 * i2c_dev 列表与驱动模型维护的 i2c_adapter 列表保持同步，
 * 通过总线通知机制更新。
 */
struct i2c_dev {
	struct list_head list;
	struct i2c_adapter *adap;
	struct device dev;
	struct cdev cdev;
};

#define I2C_MINORS	(MINORMASK + 1)
static LIST_HEAD(i2c_dev_list);
static DEFINE_SPINLOCK(i2c_dev_list_lock);

static struct i2c_dev *i2c_dev_get_by_minor(unsigned index)
{
	struct i2c_dev *i2c_dev;

	spin_lock(&i2c_dev_list_lock);
	list_for_each_entry(i2c_dev, &i2c_dev_list, list) {
		if (i2c_dev->adap->nr == index)
			goto found;
	}
	i2c_dev = NULL;
found:
	spin_unlock(&i2c_dev_list_lock);
	return i2c_dev;
}

static struct i2c_dev *get_free_i2c_dev(struct i2c_adapter *adap)
{
	struct i2c_dev *i2c_dev;

	if (adap->nr >= I2C_MINORS) {
		pr_err("Out of device minors (%d)\n", adap->nr);
		return ERR_PTR(-ENODEV);
	}

	i2c_dev = kzalloc_obj(*i2c_dev);
	if (!i2c_dev)
		return ERR_PTR(-ENOMEM);
	i2c_dev->adap = adap;

	spin_lock(&i2c_dev_list_lock);
	list_add_tail(&i2c_dev->list, &i2c_dev_list);
	spin_unlock(&i2c_dev_list_lock);
	return i2c_dev;
}

static void put_i2c_dev(struct i2c_dev *i2c_dev, bool del_cdev)
{
	spin_lock(&i2c_dev_list_lock);
	list_del(&i2c_dev->list);
	spin_unlock(&i2c_dev_list_lock);
	if (del_cdev)
		cdev_device_del(&i2c_dev->cdev, &i2c_dev->dev);
	put_device(&i2c_dev->dev);
}

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct i2c_dev *i2c_dev = i2c_dev_get_by_minor(MINOR(dev->devt));

	if (!i2c_dev)
		return -ENODEV;
	return sysfs_emit(buf, "%s\n", i2c_dev->adap->name);
}
static DEVICE_ATTR_RO(name);

static struct attribute *i2c_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(i2c);

/* ------------------------------------------------------------------------- */

/*
 * 打开这个字符设备后，文件描述符最初只和一个 i2c_adapter（总线）
 * 关联。
 *
 * 如果使用 I2C_RDWR ioctl，就可以立即向该适配器所在总线上的任意
 * 设备发送 i2c_msg，因为 i2c_msg 本身就包含了所需的地址信息，
 * 并且是直接提交给 i2c_adapter 的。不过，只支持 SMBus 的控制器
 * 不支持这个接口。
 *
 * 如果想用 read()/write() 系统调用，或者想使用 SMBus 接口（并
 * 同时兼容只支持 SMBus 的主机），则必须先执行 I2C_SLAVE 或
 * I2C_SLAVE_FORCE ioctl。这样会配置一个匿名的、从未注册过的
 * i2c_client，用来保存这些系统调用和 SMBus 接口所需的地址信息。
 */

static ssize_t i2cdev_read(struct file *file, char __user *buf, size_t count,
		loff_t *offset)
{
	char *tmp;
	int ret;

	struct i2c_client *client = file->private_data;

	/* 适配器必须支持 I2C 传输。 */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	if (count > 8192)
		count = 8192;

	tmp = kzalloc(count, GFP_KERNEL);
	if (tmp == NULL)
		return -ENOMEM;

	pr_debug("i2c-%d reading %zu bytes.\n", iminor(file_inode(file)), count);

	ret = i2c_master_recv(client, tmp, count);
	if (ret >= 0)
		if (copy_to_user(buf, tmp, ret))
			ret = -EFAULT;
	kfree(tmp);
	return ret;
}

static ssize_t i2cdev_write(struct file *file, const char __user *buf,
		size_t count, loff_t *offset)
{
	int ret;
	char *tmp;
	struct i2c_client *client = file->private_data;

	/* 适配器必须支持 I2C 传输。 */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	if (count > 8192)
		count = 8192;

	tmp = memdup_user(buf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	pr_debug("i2c-%d writing %zu bytes.\n", iminor(file_inode(file)), count);

	ret = i2c_master_send(client, tmp, count);
	kfree(tmp);
	return ret;
}

static int i2cdev_check(struct device *dev, void *addrp)
{
	struct i2c_client *client = i2c_verify_client(dev);

	if (!client || client->addr != *(unsigned int *)addrp)
		return 0;

	return dev->driver ? -EBUSY : 0;
}

/* 向上遍历 mux 树。 */
static int i2cdev_check_mux_parents(struct i2c_adapter *adapter, int addr)
{
	struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(adapter);
	int result;

	result = device_for_each_child(&adapter->dev, &addr, i2cdev_check);
	if (!result && parent)
		result = i2cdev_check_mux_parents(parent, addr);

	return result;
}

/* 向下递归遍历 mux 树。 */
static int i2cdev_check_mux_children(struct device *dev, void *addrp)
{
	int result;

	if (dev->type == &i2c_adapter_type)
		result = device_for_each_child(dev, addrp,
						i2cdev_check_mux_children);
	else
		result = i2cdev_check(dev, addrp);

	return result;
}

/*
 * 这个地址检查函数与 i2c-core 里的实现不同：
 * 它把“已经注册了设备但还没有绑定驱动”的地址视为未占用。
 */
static int i2cdev_check_addr(struct i2c_adapter *adapter, unsigned int addr)
{
	struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(adapter);
	int result = 0;

	if (parent)
		result = i2cdev_check_mux_parents(parent, addr);

	if (!result)
		result = device_for_each_child(&adapter->dev, &addr,
						i2cdev_check_mux_children);

	return result;
}

static noinline int i2cdev_ioctl_rdwr(struct i2c_client *client,
		unsigned nmsgs, struct i2c_msg *msgs)
{
	u8 __user **data_ptrs;
	int i, res;

	/*
	 * I2C_RDWR 是 i2c-dev 里最接近“原生 I2C 消息”的接口。
	 *
	 * 用户态可以一次提交多条 i2c_msg，每条消息自己带地址/方向/长度。
	 * 内核这里要做的主要是：
	 * - 把每条用户缓冲区拷进内核
	 * - 做基本长度和 RECV_LEN 合法性检查
	 * - 标记为 DMA_SAFE，方便底层控制器直接复用
	 * - 调 i2c_transfer() 真正下发，再把读结果拷回用户态
	 */
	/* 适配器必须支持 I2C 传输。 */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data_ptrs = kmalloc_array(nmsgs, sizeof(u8 __user *), GFP_KERNEL);
	if (!data_ptrs)
		return -ENOMEM;

	res = 0;
	for (i = 0; i < nmsgs; i++) {
		/* 把消息长度限制在一个合理范围内。 */
		if (msgs[i].len > 8192) {
			res = -EINVAL;
			break;
		}

		data_ptrs[i] = (u8 __user *)msgs[i].buf;
		msgs[i].buf = memdup_user(data_ptrs[i], msgs[i].len);
		if (IS_ERR(msgs[i].buf)) {
			res = PTR_ERR(msgs[i].buf);
			break;
		}
		/* memdup_user 使用 GFP_KERNEL 分配，因此可以作为 DMA 缓冲区。 */
		msgs[i].flags |= I2C_M_DMA_SAFE;

		/*
		 * 如果消息长度需要从从设备返回（类似 SMBus block read），
		 * 必须确保缓冲区足够大，至少能容纳 I2C_SMBUS_BLOCK_MAX
		 * 字节，因为底层总线驱动允许的最大块长度就是这个值。
		 * 缓冲区第一个字节必须预先填入额外字节数，它至少要能容纳
		 * 长度字节本身，但也可以更大（例如给消息尾部校验字节留空间）。
		 */
		if (msgs[i].flags & I2C_M_RECV_LEN) {
			if (!(msgs[i].flags & I2C_M_RD) ||
			    msgs[i].len < 1 || msgs[i].buf[0] < 1 ||
			    msgs[i].len < msgs[i].buf[0] +
					     I2C_SMBUS_BLOCK_MAX) {
				i++;
				res = -EINVAL;
				break;
			}

			msgs[i].len = msgs[i].buf[0];
		}
	}
	if (res < 0) {
		int j;
		for (j = 0; j < i; ++j)
			kfree(msgs[j].buf);
		kfree(data_ptrs);
		return res;
	}

	res = i2c_transfer(client->adapter, msgs, nmsgs);
	while (i-- > 0) {
		if (res >= 0 && (msgs[i].flags & I2C_M_RD)) {
			if (copy_to_user(data_ptrs[i], msgs[i].buf,
					 msgs[i].len))
				res = -EFAULT;
		}
		kfree(msgs[i].buf);
	}
	kfree(data_ptrs);
	return res;
}

static noinline int i2cdev_ioctl_smbus(struct i2c_client *client,
		u8 read_write, u8 command, u32 size,
		union i2c_smbus_data __user *data)
{
	union i2c_smbus_data temp = {};
	int datasize, res;

	/*
	 * 这是 i2c-dev 对 I2C_SMBUS ioctl 的解析层。
	 *
	 * 相比 I2C_RDWR，这里用户态提交的是“协议语义”而不是消息数组。
	 * 因此内核先校验协议类型和参数，再把用户缓冲区折叠成
	 * union i2c_smbus_data，最后统一交给 i2c_smbus_xfer()。
	 */

	if ((size != I2C_SMBUS_BYTE) &&
	    (size != I2C_SMBUS_QUICK) &&
	    (size != I2C_SMBUS_BYTE_DATA) &&
	    (size != I2C_SMBUS_WORD_DATA) &&
	    (size != I2C_SMBUS_PROC_CALL) &&
	    (size != I2C_SMBUS_BLOCK_DATA) &&
	    (size != I2C_SMBUS_I2C_BLOCK_BROKEN) &&
	    (size != I2C_SMBUS_I2C_BLOCK_DATA) &&
	    (size != I2C_SMBUS_BLOCK_PROC_CALL)) {
		dev_dbg(&client->adapter->dev,
			"size out of range (%x) in ioctl I2C_SMBUS.\n",
			size);
		return -EINVAL;
	}
	/*
	 * 注意，I2C_SMBUS_READ 和 I2C_SMBUS_WRITE 分别是 0 和 1，
	 * 所以即使 size == I2C_SMBUS_QUICK，这个检查也仍然成立。
	 */
	if ((read_write != I2C_SMBUS_READ) &&
	    (read_write != I2C_SMBUS_WRITE)) {
		dev_dbg(&client->adapter->dev,
			"read_write out of range (%x) in ioctl I2C_SMBUS.\n",
			read_write);
		return -EINVAL;
	}

	/* 注意 command 值总是有效的。 */

	if ((size == I2C_SMBUS_QUICK) ||
	    ((size == I2C_SMBUS_BYTE) &&
	    (read_write == I2C_SMBUS_WRITE)))
		/* 这些情况比较特殊，不需要 data。 */
		return i2c_smbus_xfer(client->adapter, client->addr,
				      client->flags, read_write,
				      command, size, NULL);

	if (data == NULL) {
		dev_dbg(&client->adapter->dev,
			"data is NULL pointer in ioctl I2C_SMBUS.\n");
		return -EINVAL;
	}

	if ((size == I2C_SMBUS_BYTE_DATA) ||
	    (size == I2C_SMBUS_BYTE))
		datasize = sizeof(data->byte);
	else if ((size == I2C_SMBUS_WORD_DATA) ||
		 (size == I2C_SMBUS_PROC_CALL))
		datasize = sizeof(data->word);
	else /* size == smbus block, i2c block, or block proc. call */
		datasize = sizeof(data->block);

	if ((size == I2C_SMBUS_PROC_CALL) ||
	    (size == I2C_SMBUS_BLOCK_PROC_CALL) ||
	    (size == I2C_SMBUS_I2C_BLOCK_DATA) ||
	    (read_write == I2C_SMBUS_WRITE)) {
		if (copy_from_user(&temp, data, datasize))
			return -EFAULT;
	}
	if (size == I2C_SMBUS_I2C_BLOCK_BROKEN) {
			/* 把旧的 I2C block 命令转换成新约定，保持二进制兼容。 */
		size = I2C_SMBUS_I2C_BLOCK_DATA;
		if (read_write == I2C_SMBUS_READ)
			temp.block[0] = I2C_SMBUS_BLOCK_MAX;
	}
	res = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
	      read_write, command, size, &temp);
	if (!res && ((size == I2C_SMBUS_PROC_CALL) ||
		     (size == I2C_SMBUS_BLOCK_PROC_CALL) ||
		     (read_write == I2C_SMBUS_READ))) {
		if (copy_to_user(data, &temp, datasize))
			return -EFAULT;
	}
	return res;
}

static long i2cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct i2c_client *client = file->private_data;
	unsigned long funcs;

	dev_dbg(&client->adapter->dev, "ioctl, cmd=0x%02x, arg=0x%02lx\n",
		cmd, arg);

	switch (cmd) {
	case I2C_SLAVE:
	case I2C_SLAVE_FORCE:
		if ((arg > 0x3ff) ||
		    (((client->flags & I2C_M_TEN) == 0) && arg > 0x7f))
			return -EINVAL;
		if (cmd == I2C_SLAVE && i2cdev_check_addr(client->adapter, arg))
			return -EBUSY;
		/* REVISIT: 这个地址后续可能还会变成 busy。 */
		client->addr = arg;
		return 0;
	case I2C_TENBIT:
		if (arg)
			client->flags |= I2C_M_TEN;
		else
			client->flags &= ~I2C_M_TEN;
		return 0;
	case I2C_PEC:
		/*
		 * 这里改的是“匿名 i2c-dev client”的标志，不会影响内核里
		 * 已注册、已绑定驱动的真实 i2c_client。
		 */
		if (arg)
			client->flags |= I2C_CLIENT_PEC;
		else
			client->flags &= ~I2C_CLIENT_PEC;
		return 0;
	case I2C_FUNCS:
		funcs = i2c_get_functionality(client->adapter);
		return put_user(funcs, (unsigned long __user *)arg);

	case I2C_RDWR: {
		struct i2c_rdwr_ioctl_data rdwr_arg;
		struct i2c_msg *rdwr_pa;
		int res;

		if (copy_from_user(&rdwr_arg,
				   (struct i2c_rdwr_ioctl_data __user *)arg,
				   sizeof(rdwr_arg)))
			return -EFAULT;

		if (!rdwr_arg.msgs || rdwr_arg.nmsgs == 0)
			return -EINVAL;

		/* 对一次能发送的消息数量加一个任意上限。 */
		if (rdwr_arg.nmsgs > I2C_RDWR_IOCTL_MAX_MSGS)
			return -EINVAL;

		rdwr_pa = memdup_array_user(rdwr_arg.msgs,
					    rdwr_arg.nmsgs, sizeof(struct i2c_msg));
		if (IS_ERR(rdwr_pa))
			return PTR_ERR(rdwr_pa);

		res = i2cdev_ioctl_rdwr(client, rdwr_arg.nmsgs, rdwr_pa);
		kfree(rdwr_pa);
		return res;
	}

	case I2C_SMBUS: {
		struct i2c_smbus_ioctl_data data_arg;
		if (copy_from_user(&data_arg,
				   (struct i2c_smbus_ioctl_data __user *) arg,
				   sizeof(struct i2c_smbus_ioctl_data)))
			return -EFAULT;
		return i2cdev_ioctl_smbus(client, data_arg.read_write,
					  data_arg.command,
					  data_arg.size,
					  data_arg.data);
	}
	case I2C_RETRIES:
		if (arg > INT_MAX)
			return -EINVAL;

		client->adapter->retries = arg;
		break;
	case I2C_TIMEOUT:
		/* 历史原因：用户态设置的超时单位是 10 ms。 */
		if (arg > INT_MAX / 10)
			return -EINVAL;

		client->adapter->timeout = msecs_to_jiffies(arg * 10);
		break;
	default:
		/*
		 * 注意：这里如果返回故障码，可能会让有缺陷的用户态代码出问题。
		 * 某些旧内核 bug 在这种情况下返回 0，而用户态代码可能无意中
		 * 依赖了这个错误行为。
		 */
		return -ENOTTY;
	}
	return 0;
}

#ifdef CONFIG_COMPAT

struct i2c_smbus_ioctl_data32 {
	u8 read_write;
	u8 command;
	u32 size;
	compat_caddr_t data; /* union i2c_smbus_data *data */
};

struct i2c_msg32 {
	u16 addr;
	u16 flags;
	u16 len;
	compat_caddr_t buf;
};

struct i2c_rdwr_ioctl_data32 {
	compat_caddr_t msgs; /* struct i2c_msg __user *msgs */
	u32 nmsgs;
};

static long compat_i2cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct i2c_client *client = file->private_data;
	unsigned long funcs;
	switch (cmd) {
	case I2C_FUNCS:
		funcs = i2c_get_functionality(client->adapter);
		return put_user(funcs, (compat_ulong_t __user *)arg);
	case I2C_RDWR: {
		struct i2c_rdwr_ioctl_data32 rdwr_arg;
		struct i2c_msg32 __user *p;
		struct i2c_msg *rdwr_pa;
		int i, res;

		if (copy_from_user(&rdwr_arg,
				   (struct i2c_rdwr_ioctl_data32 __user *)arg,
				   sizeof(rdwr_arg)))
			return -EFAULT;

		if (!rdwr_arg.msgs || rdwr_arg.nmsgs == 0)
			return -EINVAL;

		if (rdwr_arg.nmsgs > I2C_RDWR_IOCTL_MAX_MSGS)
			return -EINVAL;

		rdwr_pa = kmalloc_objs(struct i2c_msg, rdwr_arg.nmsgs);
		if (!rdwr_pa)
			return -ENOMEM;

		p = compat_ptr(rdwr_arg.msgs);
		for (i = 0; i < rdwr_arg.nmsgs; i++) {
			struct i2c_msg32 umsg;
			if (copy_from_user(&umsg, p + i, sizeof(umsg))) {
				kfree(rdwr_pa);
				return -EFAULT;
			}
			rdwr_pa[i] = (struct i2c_msg) {
				.addr = umsg.addr,
				.flags = umsg.flags,
				.len = umsg.len,
				.buf = (__force __u8 *)compat_ptr(umsg.buf),
			};
		}

		res = i2cdev_ioctl_rdwr(client, rdwr_arg.nmsgs, rdwr_pa);
		kfree(rdwr_pa);
		return res;
	}
	case I2C_SMBUS: {
		struct i2c_smbus_ioctl_data32	data32;
		if (copy_from_user(&data32,
				   (void __user *) arg,
				   sizeof(data32)))
			return -EFAULT;
		return i2cdev_ioctl_smbus(client, data32.read_write,
					  data32.command,
					  data32.size,
					  compat_ptr(data32.data));
	}
	default:
		return i2cdev_ioctl(file, cmd, arg);
	}
}
#else
#define compat_i2cdev_ioctl NULL
#endif

static int i2cdev_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct i2c_client *client;
	struct i2c_adapter *adap;

	adap = i2c_get_adapter(minor);
	if (!adap)
		return -ENODEV;

	/*
	 * 这里会创建一个匿名 i2c_client，之后可以通过 I2C_SLAVE 或
	 * I2C_SLAVE_FORCE 把它指向某个具体地址。
	 *
	 * 这个 client **绝不会** 注册到 driver model 或 I2C core。
	 * 它只是一个“每个文件描述符私有的地址上下文”，用于承载：
	 * - read()/write() 的目标地址
	 * - I2C_SMBUS ioctl 的目标地址
	 * - 当前 fd 视角下的 PEC / TENBIT 等标志
	 */
	client = kzalloc_obj(*client);
	if (!client) {
		i2c_put_adapter(adap);
		return -ENOMEM;
	}
	snprintf(client->name, I2C_NAME_SIZE, "i2c-dev %d", adap->nr);

	client->adapter = adap;
	file->private_data = client;

	return 0;
}

static int i2cdev_release(struct inode *inode, struct file *file)
{
	struct i2c_client *client = file->private_data;

	i2c_put_adapter(client->adapter);
	kfree(client);
	file->private_data = NULL;

	return 0;
}

static const struct file_operations i2cdev_fops = {
	.owner		= THIS_MODULE,
	.read		= i2cdev_read,
	.write		= i2cdev_write,
	.unlocked_ioctl	= i2cdev_ioctl,
	.compat_ioctl	= compat_i2cdev_ioctl,
	.open		= i2cdev_open,
	.release	= i2cdev_release,
};

/* ------------------------------------------------------------------------- */

static const struct class i2c_dev_class = {
	.name = "i2c-dev",
	.dev_groups = i2c_groups,
};

static void i2cdev_dev_release(struct device *dev)
{
	struct i2c_dev *i2c_dev;

	i2c_dev = container_of(dev, struct i2c_dev, dev);
	kfree(i2c_dev);
}

static int i2cdev_attach_adapter(struct device *dev)
{
	struct i2c_adapter *adap;
	struct i2c_dev *i2c_dev;
	int res;

	if (dev->type != &i2c_adapter_type)
		return NOTIFY_DONE;
	adap = to_i2c_adapter(dev);

	/*
	 * 一个 i2c_adapter 对应一个 /dev/i2c-X 字符设备节点。
	 * 这里负责把“内核里的 adapter 对象”桥接成“用户态可打开的 cdev”。
	 */

	i2c_dev = get_free_i2c_dev(adap);
	if (IS_ERR(i2c_dev))
		return NOTIFY_DONE;

	cdev_init(&i2c_dev->cdev, &i2cdev_fops);
	i2c_dev->cdev.owner = THIS_MODULE;

	device_initialize(&i2c_dev->dev);
	i2c_dev->dev.devt = MKDEV(I2C_MAJOR, adap->nr);
	i2c_dev->dev.class = &i2c_dev_class;
	i2c_dev->dev.parent = &adap->dev;
	i2c_dev->dev.release = i2cdev_dev_release;

	res = dev_set_name(&i2c_dev->dev, "i2c-%d", adap->nr);
	if (res)
		goto err_put_i2c_dev;

	res = cdev_device_add(&i2c_dev->cdev, &i2c_dev->dev);
	if (res)
		goto err_put_i2c_dev;

	pr_debug("adapter [%s] registered as minor %d\n", adap->name, adap->nr);
	return NOTIFY_OK;

err_put_i2c_dev:
	put_i2c_dev(i2c_dev, false);
	return NOTIFY_DONE;
}

static int i2cdev_detach_adapter(struct device *dev)
{
	struct i2c_adapter *adap;
	struct i2c_dev *i2c_dev;

	if (dev->type != &i2c_adapter_type)
		return NOTIFY_DONE;
	adap = to_i2c_adapter(dev);

	i2c_dev = i2c_dev_get_by_minor(adap->nr);
	if (!i2c_dev) /* attach_adapter must have failed */
		return NOTIFY_DONE;

	put_i2c_dev(i2c_dev, true);

	pr_debug("adapter [%s] unregistered\n", adap->name);
	return NOTIFY_OK;
}

static int i2cdev_notifier_call(struct notifier_block *nb, unsigned long action,
			 void *data)
{
	struct device *dev = data;

	/*
	 * i2c-dev 不自己维护 adapter 生命周期，而是订阅 I2C bus notifier。
	 * 这样无论 adapter 是先于 i2c-dev 还是后于 i2c-dev 出现，都能把
	 * /dev/i2c-X 节点和真实适配器同步起来。
	 */

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		return i2cdev_attach_adapter(dev);
	case BUS_NOTIFY_DEL_DEVICE:
		return i2cdev_detach_adapter(dev);
	}

	return NOTIFY_DONE;
}

static struct notifier_block i2cdev_notifier = {
	.notifier_call = i2cdev_notifier_call,
};

/* ------------------------------------------------------------------------- */

static int __init i2c_dev_attach_adapter(struct device *dev, void *dummy)
{
	i2cdev_attach_adapter(dev);
	return 0;
}

static int __exit i2c_dev_detach_adapter(struct device *dev, void *dummy)
{
	i2cdev_detach_adapter(dev);
	return 0;
}

/*
 * 模块加载/卸载时的记录维护。
 *
 * 初始化顺序是：
 * - 先申请字符设备号段
 * - 再注册 class，准备创建设备节点
 * - 再挂上 bus notifier，跟踪后续新增/删除的 adapter
 * - 最后遍历已有 adapter，补齐已有 /dev/i2c-X 节点
 */

static int __init i2c_dev_init(void)
{
	int res;

	pr_info("i2c /dev entries driver\n");

	res = register_chrdev_region(MKDEV(I2C_MAJOR, 0), I2C_MINORS, "i2c");
	if (res)
		goto out;

	res = class_register(&i2c_dev_class);
	if (res)
		goto out_unreg_chrdev;

	/* 记录之后可能被添加或移除的适配器。 */
	res = bus_register_notifier(&i2c_bus_type, &i2cdev_notifier);
	if (res)
		goto out_unreg_class;

	/* 立即绑定到已经存在的适配器。 */
	i2c_for_each_dev(NULL, i2c_dev_attach_adapter);

	return 0;

out_unreg_class:
	class_unregister(&i2c_dev_class);
out_unreg_chrdev:
	unregister_chrdev_region(MKDEV(I2C_MAJOR, 0), I2C_MINORS);
out:
	pr_err("Driver Initialisation failed\n");
	return res;
}

static void __exit i2c_dev_exit(void)
{
	bus_unregister_notifier(&i2c_bus_type, &i2cdev_notifier);
	i2c_for_each_dev(NULL, i2c_dev_detach_adapter);
	class_unregister(&i2c_dev_class);
	unregister_chrdev_region(MKDEV(I2C_MAJOR, 0), I2C_MINORS);
}

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl>");
MODULE_AUTHOR("Simon G. Vogl <simon@tk.uni-linz.ac.at>");
MODULE_DESCRIPTION("I2C /dev entries driver");
MODULE_LICENSE("GPL");

module_init(i2c_dev_init);
module_exit(i2c_dev_exit);
