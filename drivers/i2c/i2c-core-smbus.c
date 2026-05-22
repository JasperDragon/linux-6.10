// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux I2C core 的 SMBus 与 SMBus 仿真代码
 *
 * 这个文件包含始终编进 I2C core 的 SMBus 基本接口，因为这些能力
 * 可以通过 I2C 消息进行仿真实现。SMBus 的专用扩展功能（例如
 * SMBALERT）则放在独立的 i2c-smbus 模块中处理。
 *
 * SMBus 相关代码最初由 Frodo Looijaard <frodol@dds.nl> 编写。
 * SMBus 2.0 支持由 Mark Studebaker <mdsxyz123@yahoo.com> 和
 * Jean Delvare <jdelvare@suse.de> 添加。
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string_choices.h>

#include "i2c-core.h"

#define CREATE_TRACE_POINTS
#include <trace/events/smbus.h>


/* SMBus 协议相关的公共部分。 */

#define POLY    (0x1070U << 3)
static u8 crc8(u16 data)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (data & 0x8000)
			data = data ^ POLY;
		data = data << 1;
	}
	return (u8)(data >> 8);
}

/**
 * i2c_smbus_pec - 对给定数据数组做增量 CRC8 计算
 * @crc: 上一次返回的 crc8 值
 * @p: 数据缓冲区指针
 * @count: 数据缓冲区中的字节数
 *
 * 对 p 指向的数组前 count 个字节做增量 CRC8 计算。
 */
u8 i2c_smbus_pec(u8 crc, u8 *p, size_t count)
{
	int i;

	for (i = 0; i < count; i++)
		crc = crc8((crc ^ p[i]) << 8);
	return crc;
}
EXPORT_SYMBOL(i2c_smbus_pec);

/* SMBus 默认按 7 位地址处理，这通常是合理假设。 */
static u8 i2c_smbus_msg_pec(u8 pec, struct i2c_msg *msg)
{
	/* 地址会先被纳入 PEC 计算。 */
	u8 addr = i2c_8bit_addr_from_msg(msg);
	pec = i2c_smbus_pec(pec, &addr, 1);

	/* 随后再把数据缓冲区纳入 PEC 计算。 */
	return i2c_smbus_pec(pec, msg->buf, msg->len);
}

/* 仅用于写方向的事务。 */
static inline void i2c_smbus_add_pec(struct i2c_msg *msg)
{
	msg->buf[msg->len] = i2c_smbus_msg_pec(0, msg);
	msg->len++;
}

/*
 * CRC 校验失败时返回 <0。
 *
 * 如果这次读操作前面还有一段写操作（大多数情况都是这样），
 * 就需要把写入部分已经累计的 CRC 也算进去。
 *
 * 注意，这个函数会修改消息本身：它会把 msg->len 减 1，
 * 从而把 CRC 字节对调用者隐藏起来。
 */
static int i2c_smbus_check_pec(u8 cpec, struct i2c_msg *msg)
{
	u8 rpec = msg->buf[--msg->len];
	cpec = i2c_smbus_msg_pec(cpec, msg);

	if (rpec != cpec) {
		pr_debug("Bad PEC 0x%02x vs. 0x%02x\n",
			rpec, cpec);
		return -EBADMSG;
	}
	return 0;
}

/**
 * i2c_smbus_read_byte - SMBus “接收字节”协议
 * @client: 从设备句柄
 *
 * 执行 SMBus “接收字节”协议，失败返回负 errno，成功返回
 * 从设备发回的那个字节。
 */
s32 i2c_smbus_read_byte(const struct i2c_client *client)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, 0,
				I2C_SMBUS_BYTE, &data);
	return (status < 0) ? status : data.byte;
}
EXPORT_SYMBOL(i2c_smbus_read_byte);

/**
 * i2c_smbus_write_byte - SMBus “发送字节”协议
 * @client: 从设备句柄
 * @value: 要发送的字节
 *
 * 执行 SMBus “发送字节”协议，失败返回负 errno，成功返回 0。
 */
s32 i2c_smbus_write_byte(const struct i2c_client *client, u8 value)
{
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, NULL);
}
EXPORT_SYMBOL(i2c_smbus_write_byte);

/**
 * i2c_smbus_read_byte_data - SMBus “读字节”协议
 * @client: 从设备句柄
 * @command: 由从设备解释的命令字节
 *
 * 执行 SMBus “读字节”协议，失败返回负 errno，成功返回
 * 从设备发回的数据字节。
 */
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command,
				I2C_SMBUS_BYTE_DATA, &data);
	return (status < 0) ? status : data.byte;
}
EXPORT_SYMBOL(i2c_smbus_read_byte_data);

/**
 * i2c_smbus_write_byte_data - SMBus “写字节”协议
 * @client: 从设备句柄
 * @command: 由从设备解释的命令字节
 * @value: 要写入的字节
 *
 * 执行 SMBus “写字节”协议，失败返回负 errno，成功返回 0。
 */
s32 i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command,
			      u8 value)
{
	union i2c_smbus_data data;
	data.byte = value;
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command,
			      I2C_SMBUS_BYTE_DATA, &data);
}
EXPORT_SYMBOL(i2c_smbus_write_byte_data);

/**
 * i2c_smbus_read_word_data - SMBus “读字”协议
 * @client: 从设备句柄
 * @command: 由从设备解释的命令字节
 *
 * 执行 SMBus “读字”协议，失败返回负 errno，成功返回从设备
 * 发回的 16 位无符号“字”。
 */
s32 i2c_smbus_read_word_data(const struct i2c_client *client, u8 command)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command,
				I2C_SMBUS_WORD_DATA, &data);
	return (status < 0) ? status : data.word;
}
EXPORT_SYMBOL(i2c_smbus_read_word_data);

/**
 * i2c_smbus_write_word_data - SMBus“写字”协议
 * @client: 从设备句柄
 * @command: 由从设备解释的命令字节
 * @value: 要写入的 16 位“字”
 *
 * 执行 SMBus“写字”协议，失败返回负 errno，成功返回 0。
 */
s32 i2c_smbus_write_word_data(const struct i2c_client *client, u8 command,
			      u16 value)
{
	union i2c_smbus_data data;
	data.word = value;
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command,
			      I2C_SMBUS_WORD_DATA, &data);
}
EXPORT_SYMBOL(i2c_smbus_write_word_data);

/**
 * i2c_smbus_read_block_data - SMBus“块读”协议
 * @client: 从设备句柄
 * @command: 由从设备解释的命令字节
 * @values: 用于接收数据的字节数组；必须足够大，以容纳从设备
 *	返回的数据。SMBus 最多允许 32 字节。
 *
 * 执行 SMBus“块读”协议，失败返回负 errno，成功返回从设备响应
 * 中的数据字节数。
 *
 * 注意，调用这个函数要求 client 所属适配器支持
 * I2C_FUNC_SMBUS_READ_BLOCK_DATA 功能。并不是所有适配器驱动都
 * 支持这个能力；通过 I2C 消息仿真它时依赖特定机制
 * (I2C_M_RECV_LEN)，而该机制也可能未实现。
 */
s32 i2c_smbus_read_block_data(const struct i2c_client *client, u8 command,
			      u8 *values)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command,
				I2C_SMBUS_BLOCK_DATA, &data);
	if (status)
		return status;

	memcpy(values, &data.block[1], data.block[0]);
	return data.block[0];
}
EXPORT_SYMBOL(i2c_smbus_read_block_data);

/**
 * i2c_smbus_write_block_data - SMBus“块写”协议
 * @client: 从设备句柄
 * @command: 由从设备解释的命令字节
 * @length: 数据块大小；SMBus 最多允许 32 字节
 * @values: 要写入的字节数组
 *
 * 执行 SMBus“块写”协议，失败返回负 errno，成功返回 0。
 */
s32 i2c_smbus_write_block_data(const struct i2c_client *client, u8 command,
			       u8 length, const u8 *values)
{
	union i2c_smbus_data data;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	memcpy(&data.block[1], values, length);
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command,
			      I2C_SMBUS_BLOCK_DATA, &data);
}
EXPORT_SYMBOL(i2c_smbus_write_block_data);

/* 返回读取到的字节数。 */
s32 i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command,
				  u8 length, u8 *values)
{
	union i2c_smbus_data data;
	int status;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command,
				I2C_SMBUS_I2C_BLOCK_DATA, &data);
	if (status < 0)
		return status;

	memcpy(values, &data.block[1], data.block[0]);
	return data.block[0];
}
EXPORT_SYMBOL(i2c_smbus_read_i2c_block_data);

s32 i2c_smbus_write_i2c_block_data(const struct i2c_client *client, u8 command,
				   u8 length, const u8 *values)
{
	union i2c_smbus_data data;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	memcpy(data.block + 1, values, length);
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command,
			      I2C_SMBUS_I2C_BLOCK_DATA, &data);
}
EXPORT_SYMBOL(i2c_smbus_write_i2c_block_data);

static void i2c_smbus_try_get_dmabuf(struct i2c_msg *msg, u8 init_val)
{
	bool is_read = msg->flags & I2C_M_RD;
	unsigned char *dma_buf;

	dma_buf = kzalloc(I2C_SMBUS_BLOCK_MAX + (is_read ? 2 : 3), GFP_KERNEL);
	if (!dma_buf)
		return;

	msg->buf = dma_buf;
	msg->flags |= I2C_M_DMA_SAFE;

	if (init_val)
		msg->buf[0] = init_val;
}

/*
 * 使用 I2C 协议仿真一个 SMBus 命令。
 * 这里不做参数检查。
 *
 * 这条路径是给“不支持原生 smbus_xfer、但支持普通 I2C message”的控制器
 * 兜底用的。它会把 SMBus 协议翻译成 1 条或 2 条 I2C 消息，再调用
 * __i2c_transfer() 下发。
 *
 * 关键点：
 * - 不同 SMBus 协议会生成不同的消息组合
 * - 需要时会附加或校验 PEC
 * - block read / block proc call 这类变长协议要特别处理 RECV_LEN
 */
static s32 i2c_smbus_xfer_emulated(struct i2c_adapter *adapter, u16 addr,
				   unsigned short flags,
				   char read_write, u8 command, int size,
				   union i2c_smbus_data *data)
{
	/*
	 * 因此需要生成一组消息。写操作只需要一条消息；读操作需要
	 * 两条消息。这里把大多数字段初始化成合理默认值，以便让下面
	 * 的代码更简单。
	 */
	unsigned char msgbuf0[I2C_SMBUS_BLOCK_MAX+3];
	unsigned char msgbuf1[I2C_SMBUS_BLOCK_MAX+2];
	int nmsgs = read_write == I2C_SMBUS_READ ? 2 : 1;
	u8 partial_pec = 0;
	int status;
	struct i2c_msg msg[2] = {
		{
			.addr = addr,
			.flags = flags,
			.len = 1,
			.buf = msgbuf0,
		}, {
			.addr = addr,
			.flags = flags | I2C_M_RD,
			.len = 0,
			.buf = msgbuf1,
		},
	};
	bool wants_pec = ((flags & I2C_CLIENT_PEC) && size != I2C_SMBUS_QUICK
			  && size != I2C_SMBUS_I2C_BLOCK_DATA);

	msgbuf0[0] = command;
	switch (size) {
	case I2C_SMBUS_QUICK:
		msg[0].len = 0;
		/* Special case: The read/write field is used as data */
		msg[0].flags = flags | (read_write == I2C_SMBUS_READ ?
					I2C_M_RD : 0);
		nmsgs = 1;
		break;
	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_READ) {
			/* Special case: only a read! */
			msg[0].flags = I2C_M_RD | flags;
			nmsgs = 1;
		}
		break;
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ)
			msg[1].len = 1;
		else {
			msg[0].len = 2;
			msgbuf0[1] = data->byte;
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_READ)
			msg[1].len = 2;
		else {
			msg[0].len = 3;
			msgbuf0[1] = data->word & 0xff;
			msgbuf0[2] = data->word >> 8;
		}
		break;
	case I2C_SMBUS_PROC_CALL:
		nmsgs = 2; /* Special case */
		read_write = I2C_SMBUS_READ;
		msg[0].len = 3;
		msg[1].len = 2;
		msgbuf0[1] = data->word & 0xff;
		msgbuf0[2] = data->word >> 8;
		break;
	case I2C_SMBUS_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ) {
			msg[1].flags |= I2C_M_RECV_LEN;
			msg[1].len = 1; /* block length will be added by
					   the underlying bus driver */
			i2c_smbus_try_get_dmabuf(&msg[1], 0);
		} else {
			msg[0].len = data->block[0] + 2;
			if (msg[0].len > I2C_SMBUS_BLOCK_MAX + 2) {
				dev_err(&adapter->dev,
					"Invalid block write size %d\n",
					data->block[0]);
				return -EINVAL;
			}

			i2c_smbus_try_get_dmabuf(&msg[0], command);
			memcpy(msg[0].buf + 1, data->block, msg[0].len - 1);
		}
		break;
	case I2C_SMBUS_BLOCK_PROC_CALL:
		nmsgs = 2; /* Another special case */
		read_write = I2C_SMBUS_READ;
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX) {
			dev_err(&adapter->dev,
				"Invalid block write size %d\n",
				data->block[0]);
			return -EINVAL;
		}

		msg[0].len = data->block[0] + 2;
		i2c_smbus_try_get_dmabuf(&msg[0], command);
		memcpy(msg[0].buf + 1, data->block, msg[0].len - 1);

		msg[1].flags |= I2C_M_RECV_LEN;
		msg[1].len = 1; /* block length will be added by
				   the underlying bus driver */
		i2c_smbus_try_get_dmabuf(&msg[1], 0);
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX) {
			dev_err(&adapter->dev, "Invalid block %s size %d\n",
				str_read_write(read_write == I2C_SMBUS_READ),
				data->block[0]);
			return -EINVAL;
		}

		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = data->block[0];
			i2c_smbus_try_get_dmabuf(&msg[1], 0);
		} else {
			msg[0].len = data->block[0] + 1;

			i2c_smbus_try_get_dmabuf(&msg[0], command);
			memcpy(msg[0].buf + 1, data->block + 1, data->block[0]);
		}
		break;
	default:
		dev_err(&adapter->dev, "Unsupported transaction %d\n", size);
		return -EOPNOTSUPP;
	}

	if (wants_pec) {
		/* Compute PEC if first message is a write */
		if (!(msg[0].flags & I2C_M_RD)) {
			if (nmsgs == 1) /* Write only */
				i2c_smbus_add_pec(&msg[0]);
			else /* Write followed by read */
				partial_pec = i2c_smbus_msg_pec(0, &msg[0]);
		}
		/* Ask for PEC if last message is a read */
		if (msg[nmsgs - 1].flags & I2C_M_RD)
			msg[nmsgs - 1].len++;
	}

	status = __i2c_transfer(adapter, msg, nmsgs);
	if (status < 0)
		goto cleanup;
	if (status != nmsgs) {
		status = -EIO;
		goto cleanup;
	}
	status = 0;

	/* Check PEC if last message is a read */
	if (wants_pec && (msg[nmsgs - 1].flags & I2C_M_RD)) {
		status = i2c_smbus_check_pec(partial_pec, &msg[nmsgs - 1]);
		if (status < 0)
			goto cleanup;
	}

	if (read_write == I2C_SMBUS_READ)
		switch (size) {
		case I2C_SMBUS_BYTE:
			data->byte = msgbuf0[0];
			break;
		case I2C_SMBUS_BYTE_DATA:
			data->byte = msgbuf1[0];
			break;
		case I2C_SMBUS_WORD_DATA:
		case I2C_SMBUS_PROC_CALL:
			data->word = msgbuf1[0] | (msgbuf1[1] << 8);
			break;
		case I2C_SMBUS_I2C_BLOCK_DATA:
			memcpy(data->block + 1, msg[1].buf, data->block[0]);
			break;
		case I2C_SMBUS_BLOCK_DATA:
		case I2C_SMBUS_BLOCK_PROC_CALL:
			if (msg[1].buf[0] > I2C_SMBUS_BLOCK_MAX) {
				dev_err(&adapter->dev,
					"Invalid block size returned: %d\n",
					msg[1].buf[0]);
				status = -EPROTO;
				goto cleanup;
			}
			memcpy(data->block, msg[1].buf, msg[1].buf[0] + 1);
			break;
		}

cleanup:
	if (msg[0].flags & I2C_M_DMA_SAFE)
		kfree(msg[0].buf);
	if (msg[1].flags & I2C_M_DMA_SAFE)
		kfree(msg[1].buf);

	return status;
}

/**
 * i2c_smbus_xfer - 执行 SMBus 协议操作
 * @adapter: I2C 总线句柄
 * @addr: 该总线上 SMBus 从设备的地址
 * @flags: I2C_CLIENT_* 标志（通常为 0 或 I2C_CLIENT_PEC）
 * @read_write: I2C_SMBUS_READ 或 I2C_SMBUS_WRITE
 * @command: 由从设备解释的命令字节，适用于使用该字节的协议
 * @protocol: 要执行的 SMBus 协议操作，例如 I2C_SMBUS_PROC_CALL
 * @data: 需要读出或写入的数据
 *
 * 这是给上层调用者使用的常规 SMBus API。它在 __i2c_smbus_xfer() 外围
 * 补上了 adapter 级总线锁，因此调用者不需要自己关心与其它 I2C/SMBus
 * 事务的串行化。
 *
 * 执行一个 SMBus 协议操作，失败返回负 errno，成功返回 0。
 */
s32 i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		   unsigned short flags, char read_write,
		   u8 command, int protocol, union i2c_smbus_data *data)
{
	s32 res;

	res = __i2c_lock_bus_helper(adapter);
	if (res)
		return res;

	res = __i2c_smbus_xfer(adapter, addr, flags, read_write,
			       command, protocol, data);
	i2c_unlock_bus(adapter, I2C_LOCK_SEGMENT);

	return res;
}
EXPORT_SYMBOL(i2c_smbus_xfer);

/*
 * SMBus 传输核心入口。
 *
 * 优先走控制器提供的原生 smbus_xfer 回调；如果硬件或驱动不支持某个
 * 协议，再退回到 i2c_smbus_xfer_emulated() 用 I2C message 仿真。
 * 这样上层驱动可以统一使用 SMBus API，而不必关心底层控制器到底是
 * “原生支持”还是“消息拼装仿真”。
 */
s32 __i2c_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		     unsigned short flags, char read_write,
		     u8 command, int protocol, union i2c_smbus_data *data)
{
	int (*xfer_func)(struct i2c_adapter *adap, u16 addr,
			 unsigned short flags, char read_write,
			 u8 command, int size, union i2c_smbus_data *data);
	unsigned long orig_jiffies;
	int try;
	s32 res;

	res = __i2c_check_suspended(adapter);
	if (res)
		return res;

	/*
	 * 在任何 tracepoint 或原生 smbus_xfer 回调执行前，
	 * 先拒绝调用者传入的非法块长度。
	 */
	if (data &&
	    (protocol == I2C_SMBUS_I2C_BLOCK_DATA ||
	     protocol == I2C_SMBUS_BLOCK_PROC_CALL ||
	     (protocol == I2C_SMBUS_BLOCK_DATA &&
	      read_write == I2C_SMBUS_WRITE)) &&
	    (data->block[0] == 0 ||
	     data->block[0] > I2C_SMBUS_BLOCK_MAX))
		return -EINVAL;

	/*
	 * 如果启用了 trace，下面两个 tracepoint 会根据 read_write
	 * 和 protocol 条件触发。
	 */
	trace_smbus_write(adapter, addr, flags, read_write,
			  command, protocol, data);
	trace_smbus_read(adapter, addr, flags, read_write,
			 command, protocol);

	flags &= I2C_M_TEN | I2C_CLIENT_PEC | I2C_CLIENT_SCCB;

	xfer_func = adapter->algo->smbus_xfer;
	if (i2c_in_atomic_xfer_mode()) {
		if (adapter->algo->smbus_xfer_atomic)
			xfer_func = adapter->algo->smbus_xfer_atomic;
		else if (adapter->algo->master_xfer_atomic)
			xfer_func = NULL; /* fallback to I2C emulation */
	}

	if (xfer_func) {
		/* 遇到仲裁丢失时自动重试。 */
		orig_jiffies = jiffies;
		for (res = 0, try = 0; try <= adapter->retries; try++) {
			res = xfer_func(adapter, addr, flags, read_write,
					command, protocol, data);
			if (res != -EAGAIN)
				break;
			if (time_after(jiffies,
				       orig_jiffies + adapter->timeout))
				break;
		}

		if (res != -EOPNOTSUPP || !adapter->algo->master_xfer)
			goto trace;
		/*
		 * 如果适配器没有为该 SMBus 操作提供原生实现，
		 * 就回退到 i2c_smbus_xfer_emulated。
		 */
	}

	res = i2c_smbus_xfer_emulated(adapter, addr, flags, read_write,
				      command, protocol, data);

trace:
	/* 如果启用了 trace，reply tracepoint 也会根据 read_write 条件触发。 */
	trace_smbus_reply(adapter, addr, flags, read_write,
			  command, protocol, data, res);
	trace_smbus_result(adapter, addr, flags, read_write,
			   command, protocol, res);

	return res;
}
EXPORT_SYMBOL(__i2c_smbus_xfer);

/**
 * i2c_smbus_read_i2c_block_data_or_emulated - 读取块数据或使用兼容方式仿真
 * @client: 从设备句柄
 * @command: 由从设备解释的命令字节
 * @length: 数据块大小；SMBus 最多允许 I2C_SMBUS_BLOCK_MAX 字节
 * @values: 接收数据的字节数组，必须足够大以容纳从设备返回的数据。
 *	SMBus 最多允许 I2C_SMBUS_BLOCK_MAX 字节。
 *
 * 如果适配器支持 SMBus“块读”，就直接执行该协议。
 * 如果不支持，就根据可用能力，使用“字读”或“字节读”来仿真。
 *
 * 这个 helper 本质上是在做“按能力降级”：
 * - 最好情况：直接用 I2C block read
 * - 次优情况：退化成 word read 循环
 * - 最差情况：退化成 byte read 循环
 *
 * 通过这个函数访问的 I2C 从设备地址必须映射到线性地址区域，
 * 这样一次块读和一次字节读的效果才一致。在使用该函数前，必须
 * 先确认这个从设备是否真的支持块传输与字节传输之间的等价交换。
 */
s32 i2c_smbus_read_i2c_block_data_or_emulated(const struct i2c_client *client,
					      u8 command, u8 length, u8 *values)
{
	u8 i = 0;
	int status;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;

	if (i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		return i2c_smbus_read_i2c_block_data(client, command, length, values);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_READ_BYTE_DATA))
		return -EOPNOTSUPP;

	if (i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_READ_WORD_DATA)) {
		while ((i + 2) <= length) {
			status = i2c_smbus_read_word_data(client, command + i);
			if (status < 0)
				return status;
			values[i] = status & 0xff;
			values[i + 1] = status >> 8;
			i += 2;
		}
	}

	while (i < length) {
		status = i2c_smbus_read_byte_data(client, command + i);
		if (status < 0)
			return status;
		values[i] = status;
		i++;
	}

	return i;
}
EXPORT_SYMBOL(i2c_smbus_read_i2c_block_data_or_emulated);

/**
 * i2c_new_smbus_alert_device - 获取 SMBus alert 支持所需的 ARA client
 * @adapter: 目标适配器
 * @setup: SMBus alert 处理器的设置数据
 * Context: can sleep
 *
 * 在给定的 I2C 总线分段上建立 SMBus alert 协议处理。
 *
 * 处理可以通过我们的 IRQ handler 完成，也可以由适配器完成
 * （例如在自己的 handler 中、通过周期性轮询，或者别的方式）。
 *
 * 该函数返回 ARA client，后续应保存起来，供
 * i2c_handle_smbus_alert() 使用，并在最后调用 i2c_unregister_device()；
 * 如果出错，则返回 ERR_PTR。
 */
struct i2c_client *i2c_new_smbus_alert_device(struct i2c_adapter *adapter,
					      struct i2c_smbus_alert_setup *setup)
{
	struct i2c_board_info ara_board_info = {
		I2C_BOARD_INFO("smbus_alert", 0x0c),
		.platform_data = setup,
	};

	return i2c_new_client_device(adapter, &ara_board_info);
}
EXPORT_SYMBOL_GPL(i2c_new_smbus_alert_device);

#if IS_ENABLED(CONFIG_I2C_SMBUS)
int i2c_setup_smbus_alert(struct i2c_adapter *adapter)
{
	struct device *parent = adapter->dev.parent;
	int irq;

	/* 如果适配器实例化时没有父设备，就跳过 SMBus alert 设置。 */
	if (!parent)
		return 0;

	/* 对严重错误直接上报。 */
	irq = device_property_match_string(parent, "interrupt-names", "smbus_alert");
	if (irq < 0 && irq != -EINVAL && irq != -ENODATA)
		return irq;

	/* 如果没有找到 IRQ，就跳过设置。 */
	if (irq < 0 && !device_property_present(parent, "smbalert-gpios"))
		return 0;

	return PTR_ERR_OR_ZERO(i2c_new_smbus_alert_device(adapter, NULL));
}
#endif
