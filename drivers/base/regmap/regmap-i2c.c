// SPDX-License-Identifier: GPL-2.0
//
// Register map 访问 API - I2C/SMBus 后端支持
//
// Copyright 2011 Wolfson Microelectronics plc
//
// Author: Mark Brown <broonie@opensource.wolfsonmicro.com>

#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include "internal.h"

static int regmap_smbus_byte_reg_read(void *context, unsigned int reg,
				      unsigned int *val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;

	if (reg > 0xff)
		return -EINVAL;

	ret = i2c_smbus_read_byte_data(i2c, reg);
	if (ret < 0)
		return ret;

	*val = ret;

	return 0;
}

static int regmap_smbus_byte_reg_write(void *context, unsigned int reg,
				       unsigned int val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);

	if (val > 0xff || reg > 0xff)
		return -EINVAL;

	return i2c_smbus_write_byte_data(i2c, reg, val);
}

static const struct regmap_bus regmap_smbus_byte = {
	.reg_write = regmap_smbus_byte_reg_write,
	.reg_read = regmap_smbus_byte_reg_read,
};

/* 适用于“8bit 寄存器地址 + 16bit 数据”的标准 SMBus word data 模式。 */
static int regmap_smbus_word_reg_read(void *context, unsigned int reg,
				      unsigned int *val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;

	if (reg > 0xff)
		return -EINVAL;

	ret = i2c_smbus_read_word_data(i2c, reg);
	if (ret < 0)
		return ret;

	*val = ret;

	return 0;
}

static int regmap_smbus_word_reg_write(void *context, unsigned int reg,
				       unsigned int val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);

	if (val > 0xffff || reg > 0xff)
		return -EINVAL;

	return i2c_smbus_write_word_data(i2c, reg, val);
}

static const struct regmap_bus regmap_smbus_word = {
	.reg_write = regmap_smbus_word_reg_write,
	.reg_read = regmap_smbus_word_reg_read,
};

static int regmap_smbus_word_read_swapped(void *context, unsigned int reg,
					  unsigned int *val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;

	if (reg > 0xff)
		return -EINVAL;

	ret = i2c_smbus_read_word_swapped(i2c, reg);
	if (ret < 0)
		return ret;

	*val = ret;

	return 0;
}

static int regmap_smbus_word_write_swapped(void *context, unsigned int reg,
					   unsigned int val)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);

	if (val > 0xffff || reg > 0xff)
		return -EINVAL;

	return i2c_smbus_write_word_swapped(i2c, reg, val);
}

static const struct regmap_bus regmap_smbus_word_swapped = {
	.reg_write = regmap_smbus_word_write_swapped,
	.reg_read = regmap_smbus_word_read_swapped,
};

/* 纯 I2C 模式下，一次 write 直接发送“寄存器地址 + 数据载荷”整包。 */
static int regmap_i2c_write(void *context, const void *data, size_t count)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;

	ret = i2c_master_send(i2c, data, count);
	if (ret == count)
		return 0;
	else if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int regmap_i2c_gather_write(void *context,
				   const void *reg, size_t reg_size,
				   const void *val, size_t val_size)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	struct i2c_msg xfer[2];
	int ret;

	/* 如果控制器不支持 gather write / NOSTART，就返回 -ENOTSUPP，
	 * regmap core 会自动退化成线性拼包后再写。
	 */
	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_NOSTART))
		return -ENOTSUPP;

	xfer[0].addr = i2c->addr;
	xfer[0].flags = 0;
	xfer[0].len = reg_size;
	xfer[0].buf = (void *)reg;

	xfer[1].addr = i2c->addr;
	xfer[1].flags = I2C_M_NOSTART;
	xfer[1].len = val_size;
	xfer[1].buf = (void *)val;

	ret = i2c_transfer(i2c->adapter, xfer, 2);
	if (ret == 2)
		return 0;
	if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int regmap_i2c_read(void *context,
			   const void *reg, size_t reg_size,
			   void *val, size_t val_size)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	struct i2c_msg xfer[2];
	int ret;

	xfer[0].addr = i2c->addr;
	xfer[0].flags = 0;
	xfer[0].len = reg_size;
	xfer[0].buf = (void *)reg;

	xfer[1].addr = i2c->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = val_size;
	xfer[1].buf = val;

	ret = i2c_transfer(i2c->adapter, xfer, 2);
	if (ret == 2)
		return 0;
	else if (ret < 0)
		return ret;
	else
		return -EIO;
}

static const struct regmap_bus regmap_i2c = {
	.write = regmap_i2c_write,
	.gather_write = regmap_i2c_gather_write,
	.read = regmap_i2c_read,
	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,
};

/* 借助 SMBus I2C block 协议模拟“1 字节寄存器地址 + N 字节数据块”的访问。 */
static int regmap_i2c_smbus_i2c_write(void *context, const void *data,
				      size_t count)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);

	if (count < 1)
		return -EINVAL;

	--count;
	return i2c_smbus_write_i2c_block_data(i2c, ((u8 *)data)[0], count,
					      ((u8 *)data + 1));
}

static int regmap_i2c_smbus_i2c_read(void *context, const void *reg,
				     size_t reg_size, void *val,
				     size_t val_size)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret;

	if (reg_size != 1 || val_size < 1)
		return -EINVAL;

	ret = i2c_smbus_read_i2c_block_data(i2c, ((u8 *)reg)[0], val_size, val);
	if (ret == val_size)
		return 0;
	else if (ret < 0)
		return ret;
	else
		return -EIO;
}

static const struct regmap_bus regmap_i2c_smbus_i2c_block = {
	.write = regmap_i2c_smbus_i2c_write,
	.read = regmap_i2c_smbus_i2c_read,
	.max_raw_read = I2C_SMBUS_BLOCK_MAX - 1,
	.max_raw_write = I2C_SMBUS_BLOCK_MAX - 1,
};

static int regmap_i2c_smbus_i2c_write_reg16(void *context, const void *data,
				      size_t count)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);

	if (count < 2)
		return -EINVAL;

	count--;
	return i2c_smbus_write_i2c_block_data(i2c, ((u8 *)data)[0], count,
					      (u8 *)data + 1);
}

static int regmap_i2c_smbus_i2c_read_reg16(void *context, const void *reg,
				     size_t reg_size, void *val,
				     size_t val_size)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	int ret, count, len = val_size;

	if (reg_size != 2)
		return -EINVAL;

	ret = i2c_smbus_write_byte_data(i2c, ((u16 *)reg)[0] & 0xff,
					((u16 *)reg)[0] >> 8);
	if (ret < 0)
		return ret;

	count = 0;
	do {
		/* EEPROM 类器件常见的“当前地址读”模式：连续 read_byte()
		 * 由器件内部地址指针自动递增。
		 */
		ret = i2c_smbus_read_byte(i2c);
		if (ret < 0)
			break;

		*((u8 *)val++) = ret;
		count++;
		len--;
	} while (len > 0);

	if (count == val_size)
		return 0;
	else if (ret < 0)
		return ret;
	else
		return -EIO;
}

static const struct regmap_bus regmap_i2c_smbus_i2c_block_reg16 = {
	.write = regmap_i2c_smbus_i2c_write_reg16,
	.read = regmap_i2c_smbus_i2c_read_reg16,
	.max_raw_read = I2C_SMBUS_BLOCK_MAX - 2,
	.max_raw_write = I2C_SMBUS_BLOCK_MAX - 2,
};

/*
 * 针对这类适配器提供 reg16 的 SMBus 兼容路径：
 * 它们支持 SMBUS_BYTE_DATA / SMBUS_WORD_DATA，却不支持原生 I2C
 * 和 SMBUS_I2C_BLOCK，例如 AMD PIIX4。
 *
 * READ:
 *   先用 write_byte_data(addr_lo, addr_hi) 写入 16bit EEPROM 地址，
 *   再用 read_byte() 连续取数，依赖 EEPROM 内部地址指针自动递增。
 *   这与上面的 I2C-block reg16 读路径本质相同。
 *
 * WRITE:
 *   把“低地址字节 + 数据字节”编码进一个 word transaction：
 *   write_word_data(addr_hi, (data_byte << 8) | addr_lo)。
 *   因此这里只支持单字节写，每次事务只落一个数据字节。
 */
static int regmap_smbus_word_write_reg16(void *context, const void *data,
					 size_t count)
{
	struct device *dev = context;
	struct i2c_client *i2c = to_i2c_client(dev);
	u8 addr_hi, addr_lo, val;

	/*
	 * 数据布局是 [addr_hi, addr_lo, val0, val1, ...]。
	 * 但这里只支持单字节值写入；多字节写必须依赖原生 I2C，
	 * 或者在更高层手工循环发多次递增地址的 word write。
	 */
	if (count != 3)
		return -EINVAL;

	addr_hi = ((u8 *)data)[0];
	addr_lo = ((u8 *)data)[1];
	val = ((u8 *)data)[2];

	return i2c_smbus_write_word_data(i2c, addr_hi,
					 cpu_to_le16(((u16)val << 8) | addr_lo));
}

static const struct regmap_bus regmap_smbus_byte_word_reg16 = {
	.write = regmap_smbus_word_write_reg16,
	.read = regmap_i2c_smbus_i2c_read_reg16,
	.max_raw_read = I2C_SMBUS_BLOCK_MAX - 2,
	.max_raw_write = 1,
};

static const struct regmap_bus *regmap_get_i2c_bus(struct i2c_client *i2c,
					const struct regmap_config *config)
{
	const struct i2c_adapter_quirks *quirks;
	const struct regmap_bus *bus = NULL;
	struct regmap_bus *ret_bus;
	u16 max_read = 0, max_write = 0;

	if (i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		bus = &regmap_i2c;
	else if (config->val_bits == 8 && config->reg_bits == 8 &&
		 i2c_check_functionality(i2c->adapter,
					 I2C_FUNC_SMBUS_I2C_BLOCK))
		bus = &regmap_i2c_smbus_i2c_block;
	else if (config->val_bits == 8 && config->reg_bits == 16 &&
		i2c_check_functionality(i2c->adapter,
					I2C_FUNC_SMBUS_I2C_BLOCK))
		bus = &regmap_i2c_smbus_i2c_block_reg16;
	else if (config->val_bits == 8 && config->reg_bits == 16 &&
		 i2c_check_functionality(i2c->adapter,
					I2C_FUNC_SMBUS_BYTE_DATA |
					I2C_FUNC_SMBUS_WORD_DATA))
		bus = &regmap_smbus_byte_word_reg16;
	else if (config->val_bits == 16 && config->reg_bits == 8 &&
		 i2c_check_functionality(i2c->adapter,
					 I2C_FUNC_SMBUS_WORD_DATA))
		switch (regmap_get_val_endian(&i2c->dev, NULL, config)) {
		case REGMAP_ENDIAN_LITTLE:
			bus = &regmap_smbus_word;
			break;
		case REGMAP_ENDIAN_BIG:
			bus = &regmap_smbus_word_swapped;
			break;
		default:		/* 其他字节序组合在该后端上不支持 */
			break;
		}
	else if (config->val_bits == 8 && config->reg_bits == 8 &&
		 i2c_check_functionality(i2c->adapter,
					 I2C_FUNC_SMBUS_BYTE_DATA))
		bus = &regmap_smbus_byte;

	if (!bus)
		return ERR_PTR(-ENOTSUPP);

	quirks = i2c->adapter->quirks;
	if (quirks) {
		if (quirks->max_read_len &&
		    (bus->max_raw_read == 0 || bus->max_raw_read > quirks->max_read_len))
			max_read = quirks->max_read_len;

		if (quirks->max_write_len &&
		    (bus->max_raw_write == 0 || bus->max_raw_write > quirks->max_write_len))
			max_write = quirks->max_write_len -
				(config->reg_bits + config->pad_bits) / BITS_PER_BYTE;

		if (max_read || max_write) {
			ret_bus = kmemdup(bus, sizeof(*bus), GFP_KERNEL);
			if (!ret_bus)
				return ERR_PTR(-ENOMEM);
			ret_bus->free_on_exit = true;
			ret_bus->max_raw_read = max_read;
			ret_bus->max_raw_write = max_write;
			bus = ret_bus;
		}
	}

	return bus;
}

struct regmap *__regmap_init_i2c(struct i2c_client *i2c,
				 const struct regmap_config *config,
				 struct lock_class_key *lock_key,
				 const char *lock_name)
{
	const struct regmap_bus *bus = regmap_get_i2c_bus(i2c, config);

	if (IS_ERR(bus))
		return ERR_CAST(bus);

	return __regmap_init(&i2c->dev, bus, &i2c->dev, config,
			     lock_key, lock_name);
}
EXPORT_SYMBOL_GPL(__regmap_init_i2c);

struct regmap *__devm_regmap_init_i2c(struct i2c_client *i2c,
				      const struct regmap_config *config,
				      struct lock_class_key *lock_key,
				      const char *lock_name)
{
	const struct regmap_bus *bus = regmap_get_i2c_bus(i2c, config);

	if (IS_ERR(bus))
		return ERR_CAST(bus);

	return __devm_regmap_init(&i2c->dev, bus, &i2c->dev, config,
				  lock_key, lock_name);
}
EXPORT_SYMBOL_GPL(__devm_regmap_init_i2c);

MODULE_DESCRIPTION("Register map access API - I2C support");
MODULE_LICENSE("GPL");
