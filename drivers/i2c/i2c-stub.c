// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * i2c-stub.c - I2C/SMBus 芯片模拟器
 *
 * Copyright (c) 2004 Mark M. Hoffman <mhoffman@lightlink.com>
 * Copyright (C) 2007-2014 Jean Delvare <jdelvare@suse.de>
 */

#define pr_fmt(fmt) "i2c-stub: " fmt

#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>

#define MAX_CHIPS 10

/*
 * 默认不启用 I2C_FUNC_SMBUS_BLOCK_DATA。
 * 如果测试需要这项能力，必须显式在模块参数 functionality 里打开对应位。
 */
#define STUB_FUNC_DEFAULT \
		(I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE | \
		 I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA | \
		 I2C_FUNC_SMBUS_I2C_BLOCK)

#define STUB_FUNC_ALL \
		(STUB_FUNC_DEFAULT | I2C_FUNC_SMBUS_BLOCK_DATA)

static unsigned short chip_addr[MAX_CHIPS];
module_param_array(chip_addr, ushort, NULL, S_IRUGO);
MODULE_PARM_DESC(chip_addr,
		 "Chip addresses (up to 10, between 0x03 and 0x77)");

static unsigned long functionality = STUB_FUNC_DEFAULT;
module_param(functionality, ulong, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(functionality, "Override functionality bitfield");

/* 有些模拟芯片带 banked register 区间。 */

static u8 bank_reg[MAX_CHIPS];
module_param_array(bank_reg, byte, NULL, S_IRUGO);
MODULE_PARM_DESC(bank_reg, "Bank register");

static u8 bank_mask[MAX_CHIPS];
module_param_array(bank_mask, byte, NULL, S_IRUGO);
MODULE_PARM_DESC(bank_mask, "Bank value mask");

static u8 bank_start[MAX_CHIPS];
module_param_array(bank_start, byte, NULL, S_IRUGO);
MODULE_PARM_DESC(bank_start, "First banked register");

static u8 bank_end[MAX_CHIPS];
module_param_array(bank_end, byte, NULL, S_IRUGO);
MODULE_PARM_DESC(bank_end, "Last banked register");

struct smbus_block_data {
	struct list_head node;
	u8 command;
	u8 len;
	u8 block[I2C_SMBUS_BLOCK_MAX];
};

struct stub_chip {
	u8 pointer;
	u16 words[256];		/* 按 SMBus 规范，字节访问使用低 8 位 */
	struct list_head smbus_blocks;

	/* 对带 bank 的芯片，额外寄存器空间按需动态分配。 */
	u8 bank_reg;
	u8 bank_shift;
	u8 bank_mask;
	u8 bank_sel;		/* 当前选中的 bank 编号 */
	u8 bank_start;
	u8 bank_end;
	u16 bank_size;
	u16 *bank_words;	/* 为 bank_mask * bank_size 个寄存器预留空间 */
};

static struct stub_chip *stub_chips;
static int stub_chips_nr;

/* 按 command 查找对应的 SMBus block 缓冲区。
 * 只有在显式允许 create 时，才为首次访问的 block 命令动态分配存储。
 */
static struct smbus_block_data *stub_find_block(struct device *dev,
						struct stub_chip *chip,
						u8 command, bool create)
{
	struct smbus_block_data *b, *rb = NULL;

	list_for_each_entry(b, &chip->smbus_blocks, node) {
		if (b->command == command) {
			rb = b;
			break;
		}
	}
	if (rb == NULL && create) {
		rb = devm_kzalloc(dev, sizeof(*rb), GFP_KERNEL);
		if (rb == NULL)
			return rb;
		rb->command = command;
		list_add(&rb->node, &chip->smbus_blocks);
	}
	return rb;
}

/* 统一根据 bank 选择状态返回目标“寄存器字”的存储位置。
 * 如果当前命令落在 banked window 内，并且 bank_sel 非 0，
 * 就切到 bank_words；否则退回主 words[] 数组。
 */
static u16 *stub_get_wordp(struct stub_chip *chip, u8 offset)
{
	if (chip->bank_sel &&
	    offset >= chip->bank_start && offset <= chip->bank_end)
		return chip->bank_words +
		       (chip->bank_sel - 1) * chip->bank_size +
		       offset - chip->bank_start;
	else
		return chip->words + offset;
}

/* 出错返回负 errno。 */
static s32 stub_xfer(struct i2c_adapter *adap, u16 addr, unsigned short flags,
	char read_write, u8 command, int size, union i2c_smbus_data *data)
{
	s32 ret;
	int i, len;
	struct stub_chip *chip = NULL;
	struct smbus_block_data *b;
	u16 *wordp;

	/* 先按地址找到要模拟的那颗芯片。 */
	for (i = 0; i < stub_chips_nr; i++) {
		if (addr == chip_addr[i]) {
			chip = stub_chips + i;
			break;
		}
	}
	if (!chip)
		return -ENODEV;

	switch (size) {

	case I2C_SMBUS_QUICK:
		dev_dbg(&adap->dev, "smbus quick - addr 0x%02x\n", addr);
		ret = 0;
		break;

	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_WRITE) {
			chip->pointer = command;
			dev_dbg(&adap->dev,
				"smbus byte - addr 0x%02x, wrote 0x%02x.\n",
				addr, command);
		} else {
			wordp = stub_get_wordp(chip, chip->pointer++);
			data->byte = *wordp & 0xff;
			dev_dbg(&adap->dev,
				"smbus byte - addr 0x%02x, read  0x%02x.\n",
				addr, data->byte);
		}

		ret = 0;
		break;

	case I2C_SMBUS_BYTE_DATA:
		wordp = stub_get_wordp(chip, command);
		if (read_write == I2C_SMBUS_WRITE) {
			*wordp &= 0xff00;
			*wordp |= data->byte;
			dev_dbg(&adap->dev,
				"smbus byte data - addr 0x%02x, wrote 0x%02x at 0x%02x.\n",
				addr, data->byte, command);

			/* 如果写的是 bank 选择寄存器，就同步切换当前 bank。 */
			if (chip->bank_words && command == chip->bank_reg) {
				chip->bank_sel =
					(data->byte >> chip->bank_shift)
					& chip->bank_mask;
				dev_dbg(&adap->dev,
					"switching to bank %u.\n",
					chip->bank_sel);
			}
		} else {
			data->byte = *wordp & 0xff;
			dev_dbg(&adap->dev,
				"smbus byte data - addr 0x%02x, read  0x%02x at 0x%02x.\n",
				addr, data->byte, command);
		}
		chip->pointer = command + 1;

		ret = 0;
		break;

	case I2C_SMBUS_WORD_DATA:
		wordp = stub_get_wordp(chip, command);
		if (read_write == I2C_SMBUS_WRITE) {
			*wordp = data->word;
			dev_dbg(&adap->dev,
				"smbus word data - addr 0x%02x, wrote 0x%04x at 0x%02x.\n",
				addr, data->word, command);
		} else {
			data->word = *wordp;
			dev_dbg(&adap->dev,
				"smbus word data - addr 0x%02x, read  0x%04x at 0x%02x.\n",
				addr, data->word, command);
		}

		ret = 0;
		break;

	case I2C_SMBUS_I2C_BLOCK_DATA:
		/*
		 * 这里忽略 bank 机制，因为带 bank 的芯片通常不会再同时提供
		 * I2C block transfer 语义。
		 */
		if (data->block[0] == 0 ||
		    data->block[0] > I2C_SMBUS_BLOCK_MAX) {
			ret = -EINVAL;
			break;
		}
		if (data->block[0] > 256 - command)	/* 避免越界覆盖 */
			data->block[0] = 256 - command;
		len = data->block[0];
		if (read_write == I2C_SMBUS_WRITE) {
			for (i = 0; i < len; i++) {
				chip->words[command + i] &= 0xff00;
				chip->words[command + i] |= data->block[1 + i];
			}
			dev_dbg(&adap->dev,
				"i2c block data - addr 0x%02x, wrote %d bytes at 0x%02x.\n",
				addr, len, command);
		} else {
			for (i = 0; i < len; i++) {
				data->block[1 + i] =
					chip->words[command + i] & 0xff;
			}
			dev_dbg(&adap->dev,
				"i2c block data - addr 0x%02x, read  %d bytes at 0x%02x.\n",
				addr, len, command);
		}

		ret = 0;
		break;

	case I2C_SMBUS_BLOCK_DATA:
		/*
		 * 同样忽略 bank 机制，因为真实芯片通常不会同时组合
		 * “寄存器 bank” 和 “SMBus block transfer” 这两套语义。
		 */
		b = stub_find_block(&adap->dev, chip, command, false);
		if (read_write == I2C_SMBUS_WRITE) {
			len = data->block[0];
			if (len == 0 || len > I2C_SMBUS_BLOCK_MAX) {
				ret = -EINVAL;
				break;
			}
			if (b == NULL) {
				b = stub_find_block(&adap->dev, chip, command,
						    true);
				if (b == NULL) {
					ret = -ENOMEM;
					break;
				}
			}
			/* 按模拟器语义，历史上最大一次写入长度会成为后续读长度。 */
			if (len > b->len)
				b->len = len;
			for (i = 0; i < len; i++)
				b->block[i] = data->block[i + 1];
			/* 同步维护 byte/word 命令视角下的镜像寄存器值。 */
			chip->words[command] = (b->block[0] << 8) | b->len;
			dev_dbg(&adap->dev,
				"smbus block data - addr 0x%02x, wrote %d bytes at 0x%02x.\n",
				addr, len, command);
		} else {
			if (b == NULL) {
				dev_dbg(&adap->dev,
					"SMBus block read command without prior block write not supported\n");
				ret = -EOPNOTSUPP;
				break;
			}
			len = b->len;
			data->block[0] = len;
			for (i = 0; i < len; i++)
				data->block[i + 1] = b->block[i];
			dev_dbg(&adap->dev,
				"smbus block data - addr 0x%02x, read  %d bytes at 0x%02x.\n",
				addr, len, command);
		}

		ret = 0;
		break;

	default:
		dev_dbg(&adap->dev, "Unsupported I2C/SMBus command\n");
		ret = -EOPNOTSUPP;
		break;
	} /* switch (size) */

	return ret;
}

static u32 stub_func(struct i2c_adapter *adapter)
{
	return STUB_FUNC_ALL & functionality;
}

static const struct i2c_algorithm smbus_algorithm = {
	.functionality	= stub_func,
	.smbus_xfer	= stub_xfer,
};

static struct i2c_adapter stub_adapter = {
	.owner		= THIS_MODULE,
	.class		= I2C_CLASS_HWMON,
	.algo		= &smbus_algorithm,
	.name		= "SMBus stub driver",
};

static int __init i2c_stub_allocate_banks(int i)
{
	struct stub_chip *chip = stub_chips + i;

	chip->bank_reg = bank_reg[i];
	chip->bank_start = bank_start[i];
	chip->bank_end = bank_end[i];
	chip->bank_size = bank_end[i] - bank_start[i] + 1;

	/* 假定 bank mask 的有效位是连续分布的，这样才能通过右移得到 bank 编号。 */
	chip->bank_mask = bank_mask[i];
	while (!(chip->bank_mask & 1)) {
		chip->bank_shift++;
		chip->bank_mask >>= 1;
	}

	chip->bank_words = kcalloc(chip->bank_mask * chip->bank_size,
				   sizeof(u16),
				   GFP_KERNEL);
	if (!chip->bank_words)
		return -ENOMEM;

	pr_debug("Allocated %u banks of %u words each (registers 0x%02x to 0x%02x)\n",
		 chip->bank_mask, chip->bank_size, chip->bank_start,
		 chip->bank_end);

	return 0;
}

/* 释放所有虚拟芯片以及各自扩展出来的 bank 寄存器空间。 */
static void i2c_stub_free(void)
{
	int i;

	for (i = 0; i < stub_chips_nr; i++)
		kfree(stub_chips[i].bank_words);
	kfree(stub_chips);
}

static int __init i2c_stub_init(void)
{
	int i, ret;

	if (!chip_addr[0]) {
		pr_err("Please specify a chip address\n");
		return -ENODEV;
	}

	for (i = 0; i < MAX_CHIPS && chip_addr[i]; i++) {
		if (chip_addr[i] < 0x03 || chip_addr[i] > 0x77) {
			pr_err("Invalid chip address 0x%02x\n",
			       chip_addr[i]);
			return -EINVAL;
		}

		pr_info("Virtual chip at 0x%02x\n", chip_addr[i]);
	}

	/* 一次性为所有虚拟芯片分配主体结构，便于统一清理。 */
	stub_chips_nr = i;
	stub_chips = kzalloc_objs(struct stub_chip, stub_chips_nr);
	if (!stub_chips)
		return -ENOMEM;

	for (i = 0; i < stub_chips_nr; i++) {
		INIT_LIST_HEAD(&stub_chips[i].smbus_blocks);

		/* 对带 bank 的寄存器窗口，再补一块额外存储空间。 */
		if (bank_mask[i]) {
			ret = i2c_stub_allocate_banks(i);
			if (ret)
				goto fail_free;
		}
	}

	ret = i2c_add_adapter(&stub_adapter);
	if (ret)
		goto fail_free;

	return 0;

 fail_free:
	i2c_stub_free();
	return ret;
}

static void __exit i2c_stub_exit(void)
{
	i2c_del_adapter(&stub_adapter);
	i2c_stub_free();
}

MODULE_AUTHOR("Mark M. Hoffman <mhoffman@lightlink.com>");
MODULE_DESCRIPTION("I2C stub driver");
MODULE_LICENSE("GPL");

module_init(i2c_stub_init);
module_exit(i2c_stub_exit);
