// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C target/slave 模式测试单元
 *
 * Copyright (C) 2020 by Wolfram Sang, Sang Engineering <wsa@sang-engineering.com>
 * Copyright (C) 2020 by Renesas Electronics Corporation
 */

#include <generated/utsrelease.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define TU_VERSION_MAX_LENGTH 128

enum testunit_cmds {
	TU_CMD_READ_BYTES = 1,	/* 保留 0 给 ABORT、RESET 之类的扩展命令 */
	TU_CMD_SMBUS_HOST_NOTIFY,
	TU_CMD_SMBUS_BLOCK_PROC_CALL,
	TU_CMD_GET_VERSION_WITH_REP_START,
	TU_CMD_SMBUS_ALERT_REQUEST,
	TU_NUM_CMDS
};

enum testunit_regs {
	TU_REG_CMD,
	TU_REG_DATAL,
	TU_REG_DATAH,
	TU_REG_DELAY,
	TU_NUM_REGS
};

enum testunit_flags {
	TU_FLAG_IN_PROCESS,
	TU_FLAG_NACK,
};

struct testunit_data {
	unsigned long flags;
	u8 regs[TU_NUM_REGS];
	u8 reg_idx;		/* 当前写入到了哪一个命令寄存器 */
	u8 read_idx;		/* 重复起始读版本字符串时的读取偏移 */
	struct i2c_client *client;
	struct delayed_work worker;
	struct gpio_desc *gpio;	/* SMBus Alert 测试场景使用的告警 GPIO */
	struct completion alert_done;	/* 等待 alert 主机侧应答完成 */
};

static char tu_version_info[] = "v" UTS_RELEASE "\n\0";

static int i2c_slave_testunit_smbalert_cb(struct i2c_client *client,
					  enum i2c_slave_event event, u8 *val)
{
	struct testunit_data *tu = i2c_get_clientdata(client);

	switch (event) {
	case I2C_SLAVE_READ_PROCESSED:
		gpiod_set_value(tu->gpio, 0);
		fallthrough;
	case I2C_SLAVE_READ_REQUESTED:
		*val = tu->regs[TU_REG_DATAL];
		break;

	case I2C_SLAVE_STOP:
		complete(&tu->alert_done);
		break;

	case I2C_SLAVE_WRITE_REQUESTED:
	case I2C_SLAVE_WRITE_RECEIVED:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int i2c_slave_testunit_slave_cb(struct i2c_client *client,
				     enum i2c_slave_event event, u8 *val)
{
	struct testunit_data *tu = i2c_get_clientdata(client);
	/* SMBus Block Process Call:
	 * 主机先写入命令和参数，再在重复起始后的读阶段取回结果长度。
	 */
	bool is_proc_call = tu->reg_idx == 3 && tu->regs[TU_REG_DATAL] == 1 &&
			    tu->regs[TU_REG_CMD] == TU_CMD_SMBUS_BLOCK_PROC_CALL;
	/* GET_VERSION_WITH_REP_START:
	 * 主机通过重复起始连续读出内核版本字符串，直到遇到 NUL 结尾。
	 */
	bool is_get_version = tu->reg_idx == 3 &&
			      tu->regs[TU_REG_CMD] == TU_CMD_GET_VERSION_WITH_REP_START;
	int ret = 0;

	switch (event) {
	case I2C_SLAVE_WRITE_REQUESTED:
		/* 一旦已有后台命令在执行，或之前的传输已出错进入 NACK 状态，
		 * 整个后续写阶段都统一拒绝，直到看到 STOP 为止。
		 */
		if (test_bit(TU_FLAG_IN_PROCESS | TU_FLAG_NACK, &tu->flags)) {
			ret = -EBUSY;
			break;
		}

		memset(tu->regs, 0, TU_NUM_REGS);
		tu->reg_idx = 0;
		tu->read_idx = 0;
		break;

	case I2C_SLAVE_WRITE_RECEIVED:
		if (test_bit(TU_FLAG_IN_PROCESS | TU_FLAG_NACK, &tu->flags)) {
			ret = -EBUSY;
			break;
		}

		if (tu->reg_idx < TU_NUM_REGS)
			tu->regs[tu->reg_idx] = *val;
		else
			ret = -EMSGSIZE;

		if (tu->reg_idx <= TU_NUM_REGS)
			tu->reg_idx++;

		/* 走到这里时，TU_REG_CMD 一定位于第一个字节，已经写入完成。 */
		if (tu->regs[TU_REG_CMD] >= TU_NUM_CMDS)
			ret = -EINVAL;

		break;

	case I2C_SLAVE_STOP:
		/* 只有完整收到 4 个寄存器字节，才把命令交给后台工作队列执行。 */
		if (tu->reg_idx == TU_NUM_REGS) {
			set_bit(TU_FLAG_IN_PROCESS, &tu->flags);
			queue_delayed_work(system_dfl_long_wq, &tu->worker,
					   msecs_to_jiffies(10 * tu->regs[TU_REG_DELAY]));
		}

		/*
		 * 把 reg_idx 清零，避免后续读消息结束时再次看到 STOP 又重复排队。
		 * 但这里不能清空寄存器镜像，因为后台工作队列仍要读取这些参数。
		 */
		tu->reg_idx = 0;

		clear_bit(TU_FLAG_NACK, &tu->flags);
		break;

	case I2C_SLAVE_READ_PROCESSED:
		/* 版本字符串场景下逐字推进，直到读到结尾 NUL。 */
		if (is_get_version && tu_version_info[tu->read_idx] != 0)
			tu->read_idx++;
		else if (is_proc_call && tu->regs[TU_REG_DATAH])
			/* Proc call 场景借用 DATAH 作为剩余返回长度计数器。 */
			tu->regs[TU_REG_DATAH]--;

		fallthrough;

	case I2C_SLAVE_READ_REQUESTED:
		if (is_get_version)
			*val = tu_version_info[tu->read_idx];
		else if (is_proc_call)
			*val = tu->regs[TU_REG_DATAH];
		else
			*val = test_bit(TU_FLAG_IN_PROCESS, &tu->flags) ?
					tu->regs[TU_REG_CMD] : 0;
		break;
	}

	/* 一旦本轮事务任何阶段出错，就持续 NACK 到下一个 STOP 为止。 */
	if (ret)
		set_bit(TU_FLAG_NACK, &tu->flags);

	return ret;
}

static void i2c_slave_testunit_work(struct work_struct *work)
{
	struct testunit_data *tu = container_of(work, struct testunit_data, worker.work);
	unsigned long time_left;
	struct i2c_msg msg;
	u8 msgbuf[256];
	u16 orig_addr;
	int ret = 0;

	/* 默认视为“本次命令不需要发起主模式传输”，后续按命令类型覆盖。 */
	msg.addr = I2C_CLIENT_END;
	msg.buf = msgbuf;

	switch (tu->regs[TU_REG_CMD]) {
	case TU_CMD_READ_BYTES:
		msg.addr = tu->regs[TU_REG_DATAL];
		msg.flags = I2C_M_RD;
		msg.len = tu->regs[TU_REG_DATAH];
		break;

	case TU_CMD_SMBUS_HOST_NOTIFY:
		msg.addr = 0x08;
		msg.flags = 0;
		msg.len = 3;
		msgbuf[0] = tu->client->addr;
		msgbuf[1] = tu->regs[TU_REG_DATAL];
		msgbuf[2] = tu->regs[TU_REG_DATAH];
		break;

	case TU_CMD_SMBUS_ALERT_REQUEST:
		if (!tu->gpio) {
			ret = -ENOENT;
			break;
		}
		/* Alert 协议固定使用地址 0x0c。这里临时把当前 client 切换到该地址，
		 * 完成从机侧应答后再恢复原地址并重新注册普通 slave 回调。
		 */
		i2c_slave_unregister(tu->client);
		orig_addr = tu->client->addr;
		tu->client->addr = 0x0c;
		ret = i2c_slave_register(tu->client, i2c_slave_testunit_smbalert_cb);
		if (ret)
			goto out_smbalert;

		reinit_completion(&tu->alert_done);
		gpiod_set_value(tu->gpio, 1);
		time_left = wait_for_completion_timeout(&tu->alert_done, HZ);
		if (!time_left)
			ret = -ETIMEDOUT;

		i2c_slave_unregister(tu->client);
out_smbalert:
		tu->client->addr = orig_addr;
		i2c_slave_register(tu->client, i2c_slave_testunit_slave_cb);
		break;

	default:
		break;
	}

	if (msg.addr != I2C_CLIENT_END) {
		ret = i2c_transfer(tu->client->adapter, &msg, 1);
		/* 把“成功返回但未实际传输任何消息”折算成 errno，便于测试判错。 */
		ret = (ret == 0) ? -EIO : ret;
	}

	if (ret < 0)
		dev_err(&tu->client->dev, "CMD%02X failed (%d)\n", tu->regs[TU_REG_CMD], ret);

	clear_bit(TU_FLAG_IN_PROCESS, &tu->flags);
}

static int i2c_slave_testunit_probe(struct i2c_client *client)
{
	struct testunit_data *tu;

	tu = devm_kzalloc(&client->dev, sizeof(struct testunit_data), GFP_KERNEL);
	if (!tu)
		return -ENOMEM;

	tu->client = client;
	i2c_set_clientdata(client, tu);
	init_completion(&tu->alert_done);
	INIT_DELAYED_WORK(&tu->worker, i2c_slave_testunit_work);

	tu->gpio = devm_gpiod_get_index_optional(&client->dev, NULL, 0, GPIOD_OUT_LOW);
	if (IS_ERR(tu->gpio))
		return PTR_ERR(tu->gpio);

	if (gpiod_cansleep(tu->gpio)) {
		dev_err(&client->dev, "GPIO access which may sleep is not allowed\n");
		return -EDEADLK;
	}

	if (sizeof(tu_version_info) > TU_VERSION_MAX_LENGTH)
		tu_version_info[TU_VERSION_MAX_LENGTH - 1] = 0;

	return i2c_slave_register(client, i2c_slave_testunit_slave_cb);
}

static void i2c_slave_testunit_remove(struct i2c_client *client)
{
	struct testunit_data *tu = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&tu->worker);
	i2c_slave_unregister(client);
}

static const struct i2c_device_id i2c_slave_testunit_id[] = {
	{ "slave-testunit" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, i2c_slave_testunit_id);

static struct i2c_driver i2c_slave_testunit_driver = {
	.driver = {
		.name = "i2c-slave-testunit",
	},
	.probe = i2c_slave_testunit_probe,
	.remove = i2c_slave_testunit_remove,
	.id_table = i2c_slave_testunit_id,
};
module_i2c_driver(i2c_slave_testunit_driver);

MODULE_AUTHOR("Wolfram Sang <wsa@sang-engineering.com>");
MODULE_DESCRIPTION("I2C slave mode test unit");
MODULE_LICENSE("GPL v2");
