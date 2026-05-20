// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018 Exceet Electronics GmbH
 * Copyright (C) 2018 Bootlin
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */
#include <linux/dmaengine.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>
#include <linux/sched/task_stack.h>

#define CREATE_TRACE_POINTS
#include <trace/events/spi-mem.h>

#include "internals.h"

#define SPI_MEM_MAX_BUSWIDTH		8

/**
 * spi_controller_dma_map_mem_op_data() - 为 memory operation 关联的缓冲区做 DMA 映射
 * @ctlr: 请求执行 dma_map() 的 SPI 控制器
 * @op: 包含待映射缓冲区的 memory operation
 * @sgt: 一个尚未初始化的 sg_table 指针，本函数会把它填好
 *
 * 某些控制器希望直接对 @op 里嵌入的数据缓冲区做 DMA。
 * 这个 helper 会把相关准备工作都做好，并提供可直接使用的
 * sg_table。它不应该由普通 SPI 协议驱动调用，只应由 SPI
 * 控制器驱动使用。
 * 需要注意的是，调用前必须确保 op->data.buf.{in,out} 指向的
 * 内存区域可以被 DMA 访问。
 *
 * Return: 成功返回 0，否则返回负错误码。
 */
int spi_controller_dma_map_mem_op_data(struct spi_controller *ctlr,
				       const struct spi_mem_op *op,
				       struct sg_table *sgt)
{
	struct device *dmadev;

	if (!op->data.nbytes)
		return -EINVAL;

	if (op->data.dir == SPI_MEM_DATA_OUT && ctlr->dma_tx)
		dmadev = ctlr->dma_tx->device->dev;
	else if (op->data.dir == SPI_MEM_DATA_IN && ctlr->dma_rx)
		dmadev = ctlr->dma_rx->device->dev;
	else
		dmadev = ctlr->dev.parent;

	if (!dmadev)
		return -EINVAL;

	return spi_map_buf(ctlr, dmadev, sgt, op->data.buf.in, op->data.nbytes,
			   op->data.dir == SPI_MEM_DATA_IN ?
			   DMA_FROM_DEVICE : DMA_TO_DEVICE);
}
EXPORT_SYMBOL_GPL(spi_controller_dma_map_mem_op_data);

/**
 * spi_controller_dma_unmap_mem_op_data() - 取消 memory operation 关联缓冲区的 DMA 映射
 * @ctlr: 请求执行 dma_unmap() 的 SPI 控制器
 * @op: 包含待取消映射缓冲区的 memory operation
 * @sgt: 之前由 spi_controller_dma_map_mem_op_data() 初始化过的 sg_table 指针
 *
 * 某些控制器会对 @op 中嵌入的数据缓冲区做 DMA。
 * 这个 helper 会把映射收回，以便 CPU 能再次访问
 * op->data.buf.{in,out} 缓冲区。
 *
 * 它不应由普通 SPI 协议驱动调用，只应由 SPI 控制器驱动使用。
 *
 * 该函数必须在 DMA 操作完成后调用，并且只有在之前
 * spi_controller_dma_map_mem_op_data() 成功返回 0 时才有效。
 *
 * Return: 成功返回 0，否则返回负错误码。
 */
void spi_controller_dma_unmap_mem_op_data(struct spi_controller *ctlr,
					  const struct spi_mem_op *op,
					  struct sg_table *sgt)
{
	struct device *dmadev;

	if (!op->data.nbytes)
		return;

	if (op->data.dir == SPI_MEM_DATA_OUT && ctlr->dma_tx)
		dmadev = ctlr->dma_tx->device->dev;
	else if (op->data.dir == SPI_MEM_DATA_IN && ctlr->dma_rx)
		dmadev = ctlr->dma_rx->device->dev;
	else
		dmadev = ctlr->dev.parent;

	spi_unmap_buf(ctlr, dmadev, sgt,
		      op->data.dir == SPI_MEM_DATA_IN ?
		      DMA_FROM_DEVICE : DMA_TO_DEVICE);
}
EXPORT_SYMBOL_GPL(spi_controller_dma_unmap_mem_op_data);

static int spi_check_buswidth_req(struct spi_mem *mem, u8 buswidth, bool tx)
{
	u32 mode = mem->spi->mode;

	switch (buswidth) {
	case 1:
		return 0;

	case 2:
		if ((tx &&
		     (mode & (SPI_TX_DUAL | SPI_TX_QUAD | SPI_TX_OCTAL))) ||
		    (!tx &&
		     (mode & (SPI_RX_DUAL | SPI_RX_QUAD | SPI_RX_OCTAL))))
			return 0;

		break;

	case 4:
		if ((tx && (mode & (SPI_TX_QUAD | SPI_TX_OCTAL))) ||
		    (!tx && (mode & (SPI_RX_QUAD | SPI_RX_OCTAL))))
			return 0;

		break;

	case 8:
		if ((tx && (mode & SPI_TX_OCTAL)) ||
		    (!tx && (mode & SPI_RX_OCTAL)))
			return 0;

		break;

	default:
		break;
	}

	return -ENOTSUPP;
}

static bool spi_mem_check_buswidth(struct spi_mem *mem,
				   const struct spi_mem_op *op)
{
	if (spi_check_buswidth_req(mem, op->cmd.buswidth, true))
		return false;

	if (op->addr.nbytes &&
	    spi_check_buswidth_req(mem, op->addr.buswidth, true))
		return false;

	if (op->dummy.nbytes &&
	    spi_check_buswidth_req(mem, op->dummy.buswidth, true))
		return false;

	if (op->data.dir != SPI_MEM_NO_DATA &&
	    spi_check_buswidth_req(mem, op->data.buswidth,
				   op->data.dir == SPI_MEM_DATA_OUT))
		return false;

	return true;
}

bool spi_mem_default_supports_op(struct spi_mem *mem,
				 const struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;
	bool op_is_dtr =
		op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr;

	if (op_is_dtr) {
		if (!spi_mem_controller_is_capable(ctlr, dtr))
			return false;

		if (op->data.swap16 && !spi_mem_controller_is_capable(ctlr, swap16))
			return false;

		/* 额外的 8D-8D-8D 限制。 */
		if (op->cmd.dtr && op->cmd.buswidth == 8) {
			if (op->cmd.nbytes != 2)
				return false;

			if ((op->addr.nbytes % 2) ||
			    (op->dummy.nbytes % 2) ||
			    (op->data.nbytes % 2)) {
				dev_err(&ctlr->dev,
					"Even byte numbers not allowed in octal DTR operations\n");
				return false;
			}
		}
	} else {
		if (op->cmd.nbytes != 1)
			return false;
	}

	if (op->data.ecc) {
		if (!spi_mem_controller_is_capable(ctlr, ecc))
			return false;
	}

	if (op->max_freq && mem->spi->controller->min_speed_hz &&
	    op->max_freq < mem->spi->controller->min_speed_hz)
		return false;

	if (op->max_freq &&
	    op->max_freq < mem->spi->max_speed_hz) {
		if (!spi_mem_controller_is_capable(ctlr, per_op_freq))
			return false;
	}

	return spi_mem_check_buswidth(mem, op);
}
EXPORT_SYMBOL_GPL(spi_mem_default_supports_op);

static bool spi_mem_buswidth_is_valid(u8 buswidth)
{
	if (hweight8(buswidth) > 1 || buswidth > SPI_MEM_MAX_BUSWIDTH)
		return false;

	return true;
}

static int spi_mem_check_op(const struct spi_mem_op *op)
{
	if (!op->cmd.buswidth || !op->cmd.nbytes)
		return -EINVAL;

	if ((op->addr.nbytes && !op->addr.buswidth) ||
	    (op->dummy.nbytes && !op->dummy.buswidth) ||
	    (op->data.nbytes && !op->data.buswidth))
		return -EINVAL;

	if (!spi_mem_buswidth_is_valid(op->cmd.buswidth) ||
	    !spi_mem_buswidth_is_valid(op->addr.buswidth) ||
	    !spi_mem_buswidth_is_valid(op->dummy.buswidth) ||
	    !spi_mem_buswidth_is_valid(op->data.buswidth))
		return -EINVAL;

	/* 缓冲区必须可以用于 DMA。 */
	if (WARN_ON_ONCE(op->data.dir == SPI_MEM_DATA_IN &&
			 object_is_on_stack(op->data.buf.in)))
		return -EINVAL;

	if (WARN_ON_ONCE(op->data.dir == SPI_MEM_DATA_OUT &&
			 object_is_on_stack(op->data.buf.out)))
		return -EINVAL;

	return 0;
}

static bool spi_mem_internal_supports_op(struct spi_mem *mem,
					 const struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;

	if (ctlr->mem_ops && ctlr->mem_ops->supports_op)
		return ctlr->mem_ops->supports_op(mem, op);

	return spi_mem_default_supports_op(mem, op);
}

/**
 * spi_mem_supports_op() - 检查 memory 设备及其所连接的控制器是否支持某个内存操作
 * @mem: SPI memory 对象
 * @op: 需要检查的 memory operation
 *
 * 有些控制器只支持单线或双线 IO；有些只支持特定 opcode；
 * 甚至可能控制器和设备都支持 Quad IO，但由于硬件只接了两根 IO 线，
 * 实际上仍然不能使用。
 *
 * 这个函数用于检查某个具体操作是否受支持。
 *
 * Return: 如果 @op 支持则返回 true，否则返回 false。
 */
bool spi_mem_supports_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	/* 在继续之前先确保操作频率正确。 */
	spi_mem_adjust_op_freq(mem, (struct spi_mem_op *)op);

	if (spi_mem_check_op(op))
		return false;

	return spi_mem_internal_supports_op(mem, op);
}
EXPORT_SYMBOL_GPL(spi_mem_supports_op);

static int spi_mem_access_start(struct spi_mem *mem)
{
	struct spi_controller *ctlr = mem->spi->controller;

	/*
	 * 在执行 SPI memory 操作之前先刷新消息队列，
	 * 以避免普通 SPI 传输抢占该操作。
	 */
	spi_flush_queue(ctlr);

	if (ctlr->auto_runtime_pm) {
		int ret;

		ret = pm_runtime_resume_and_get(ctlr->dev.parent);
		if (ret < 0) {
			dev_err(&ctlr->dev, "Failed to power device: %d\n",
				ret);
			return ret;
		}
	}

	mutex_lock(&ctlr->bus_lock_mutex);
	mutex_lock(&ctlr->io_mutex);

	return 0;
}

static void spi_mem_access_end(struct spi_mem *mem)
{
	struct spi_controller *ctlr = mem->spi->controller;

	mutex_unlock(&ctlr->io_mutex);
	mutex_unlock(&ctlr->bus_lock_mutex);

	if (ctlr->auto_runtime_pm)
		pm_runtime_put(ctlr->dev.parent);
}

static void spi_mem_add_op_stats(struct spi_statistics __percpu *pcpu_stats,
				 const struct spi_mem_op *op, int exec_op_ret)
{
	struct spi_statistics *stats;
	u64 len, l2len;

	get_cpu();
	stats = this_cpu_ptr(pcpu_stats);
	u64_stats_update_begin(&stats->syncp);

	/*
	 * 这里没有 message 或 transfer 的概念，可以把一次 operation
	 * 等价看成一条 message 和一个 transfer。
	 */
	u64_stats_inc(&stats->messages);
	u64_stats_inc(&stats->transfers);

	/* 用所有长度之和作为字节计数和直方图统计值。 */
	len = op->cmd.nbytes + op->addr.nbytes;
	len += op->dummy.nbytes + op->data.nbytes;
	u64_stats_add(&stats->bytes, len);
	l2len = min(fls(len), SPI_STATISTICS_HISTO_SIZE) - 1;
	u64_stats_inc(&stats->transfer_bytes_histo[l2len]);

	/* 只把 data 字节统计到 transferred bytes 中。 */
	if (op->data.nbytes && op->data.dir == SPI_MEM_DATA_OUT)
		u64_stats_add(&stats->bytes_tx, op->data.nbytes);
	if (op->data.nbytes && op->data.dir == SPI_MEM_DATA_IN)
		u64_stats_add(&stats->bytes_rx, op->data.nbytes);

	/*
	 * 超时不算错误，这里沿用 spi_transfer_one_message() 的行为。
	 */
	if (exec_op_ret == -ETIMEDOUT)
		u64_stats_inc(&stats->timedout);
	else if (exec_op_ret)
		u64_stats_inc(&stats->errors);

	u64_stats_update_end(&stats->syncp);
	put_cpu();
}

/**
 * spi_mem_exec_op() - 执行一个 memory operation
 * @mem: SPI memory 对象
 * @op: 需要执行的 memory operation
 *
 * 该函数会先检查 @op 是否受支持，然后尝试执行它。
 *
 * Return: 成功返回 0，否则返回负错误码。
 */
int spi_mem_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	unsigned int tmpbufsize, xferpos = 0, totalxferlen = 0;
	struct spi_controller *ctlr = mem->spi->controller;
	struct spi_transfer xfers[4] = { };
	struct spi_message msg;
	u8 *tmpbuf;
	int ret;

	/* 在继续之前先确保操作频率正确。 */
	spi_mem_adjust_op_freq(mem, (struct spi_mem_op *)op);

	dev_vdbg(&mem->spi->dev, "[cmd: 0x%02x][%dB addr: %#8llx][%2dB dummy][%4dB data %s] %d%c-%d%c-%d%c-%d%c @ %uHz\n",
		 op->cmd.opcode,
		 op->addr.nbytes, (op->addr.nbytes ? op->addr.val : 0),
		 op->dummy.nbytes,
		 op->data.nbytes, (op->data.nbytes ? (op->data.dir == SPI_MEM_DATA_IN ? " read" : "write") : "     "),
		 op->cmd.buswidth, op->cmd.dtr ? 'D' : 'S',
		 op->addr.buswidth, op->addr.dtr ? 'D' : 'S',
		 op->dummy.buswidth, op->dummy.dtr ? 'D' : 'S',
		 op->data.buswidth, op->data.dtr ? 'D' : 'S',
		 op->max_freq ? op->max_freq : mem->spi->max_speed_hz);

	ret = spi_mem_check_op(op);
	if (ret)
		return ret;

	if (!spi_mem_internal_supports_op(mem, op))
		return -EOPNOTSUPP;

	if (ctlr->mem_ops && ctlr->mem_ops->exec_op && !spi_get_csgpiod(mem->spi, 0)) {
		ret = spi_mem_access_start(mem);
		if (ret)
			return ret;

		trace_spi_mem_start_op(mem, op);
		ret = ctlr->mem_ops->exec_op(mem, op);
		trace_spi_mem_stop_op(mem, op);

		spi_mem_access_end(mem);

		/*
		 * 有些控制器只优化特定路径（通常是读路径），
		 * 其它情况则希望 core 回退到普通 SPI 接口。
		 */
		if (!ret || (ret != -ENOTSUPP && ret != -EOPNOTSUPP)) {
			spi_mem_add_op_stats(ctlr->pcpu_statistics, op, ret);
			spi_mem_add_op_stats(mem->spi->pcpu_statistics, op, ret);

			return ret;
		}
	}

	tmpbufsize = op->cmd.nbytes + op->addr.nbytes + op->dummy.nbytes;

	/*
	 * 用 kmalloc() 分配一个缓冲区来承载 CMD / ADDR 周期，
	 * 这样可以保证该缓冲区可以被 DMA 访问，符合 SPI 层要求。
	 */
	tmpbuf = kzalloc(tmpbufsize, GFP_KERNEL | GFP_DMA);
	if (!tmpbuf)
		return -ENOMEM;

	spi_message_init(&msg);

	tmpbuf[0] = op->cmd.opcode;
	xfers[xferpos].tx_buf = tmpbuf;
	xfers[xferpos].len = op->cmd.nbytes;
	xfers[xferpos].tx_nbits = op->cmd.buswidth;
	xfers[xferpos].speed_hz = op->max_freq;
	spi_message_add_tail(&xfers[xferpos], &msg);
	xferpos++;
	totalxferlen++;

	if (op->addr.nbytes) {
		int i;

		for (i = 0; i < op->addr.nbytes; i++)
			tmpbuf[i + 1] = op->addr.val >>
					(8 * (op->addr.nbytes - i - 1));

		xfers[xferpos].tx_buf = tmpbuf + 1;
		xfers[xferpos].len = op->addr.nbytes;
		xfers[xferpos].tx_nbits = op->addr.buswidth;
		xfers[xferpos].speed_hz = op->max_freq;
		spi_message_add_tail(&xfers[xferpos], &msg);
		xferpos++;
		totalxferlen += op->addr.nbytes;
	}

	if (op->dummy.nbytes) {
		memset(tmpbuf + op->addr.nbytes + 1, 0xff, op->dummy.nbytes);
		xfers[xferpos].tx_buf = tmpbuf + op->addr.nbytes + 1;
		xfers[xferpos].len = op->dummy.nbytes;
		xfers[xferpos].tx_nbits = op->dummy.buswidth;
		xfers[xferpos].dummy_data = 1;
		xfers[xferpos].speed_hz = op->max_freq;
		spi_message_add_tail(&xfers[xferpos], &msg);
		xferpos++;
		totalxferlen += op->dummy.nbytes;
	}

	if (op->data.nbytes) {
		if (op->data.dir == SPI_MEM_DATA_IN) {
			xfers[xferpos].rx_buf = op->data.buf.in;
			xfers[xferpos].rx_nbits = op->data.buswidth;
		} else {
			xfers[xferpos].tx_buf = op->data.buf.out;
			xfers[xferpos].tx_nbits = op->data.buswidth;
		}

		xfers[xferpos].len = op->data.nbytes;
		xfers[xferpos].speed_hz = op->max_freq;
		spi_message_add_tail(&xfers[xferpos], &msg);
		xferpos++;
		totalxferlen += op->data.nbytes;
	}

	ret = spi_sync(mem->spi, &msg);

	kfree(tmpbuf);

	if (ret)
		return ret;

	if (msg.actual_length != totalxferlen)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL_GPL(spi_mem_exec_op);

/**
 * spi_mem_get_name() - 返回上层在需要时可使用的 SPI mem 设备名
 * @mem: SPI memory 对象
 *
 * 这个函数允许 SPI mem 使用者获取 SPI mem 设备名。
 * 当上层为了兼容性需要暴露自定义名称时，它很有用。
 *
 * Return: 返回 SPI mem 使用者应使用的设备名字符串。
 */
const char *spi_mem_get_name(struct spi_mem *mem)
{
	return mem->name;
}
EXPORT_SYMBOL_GPL(spi_mem_get_name);

/**
 * spi_mem_adjust_op_size() - 调整 SPI mem 操作的数据大小以适配控制器限制
 * @mem: SPI memory 对象
 * @op: 需要调整的操作
 *
 * 某些控制器存在 FIFO 限制，必须把一次数据传输拆成多次；
 * 另一些控制器则要求特定对齐以获得更好的访问性能。
 * 这个函数允许 SPI mem 驱动在需要时把单次操作拆成多个子操作。
 *
 * Return: 如果控制器无法正确调整 @op，则返回负错误码；
 *	   否则返回 0。注意，如果 @op 需要被拆分，
 *	   @op->data.nbytes 会被更新为可执行的长度。
 */
int spi_mem_adjust_op_size(struct spi_mem *mem, struct spi_mem_op *op)
{
	struct spi_controller *ctlr = mem->spi->controller;
	size_t len;

	if (ctlr->mem_ops && ctlr->mem_ops->adjust_op_size)
		return ctlr->mem_ops->adjust_op_size(mem, op);

	if (!ctlr->mem_ops || !ctlr->mem_ops->exec_op) {
		len = op->cmd.nbytes + op->addr.nbytes + op->dummy.nbytes;

		if (len > spi_max_transfer_size(mem->spi))
			return -EINVAL;

		op->data.nbytes = min3((size_t)op->data.nbytes,
				       spi_max_transfer_size(mem->spi),
				       spi_max_message_size(mem->spi) -
				       len);
		if (!op->data.nbytes)
			return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(spi_mem_adjust_op_size);

/**
 * spi_mem_adjust_op_freq() - 调整 SPI mem 操作频率以适配控制器、PCB 和芯片限制
 * @mem: SPI memory 对象
 * @op: 需要调整的操作
 *
 * 某些芯片对不同操作有各自的频率限制，必须调整最大速率。
 * 这个函数允许 SPI mem 驱动把 @op->max_freq 设为可支持的最大值。
 */
void spi_mem_adjust_op_freq(struct spi_mem *mem, struct spi_mem_op *op)
{
	if (!op->max_freq || op->max_freq > mem->spi->max_speed_hz)
		op->max_freq = mem->spi->max_speed_hz;
}
EXPORT_SYMBOL_GPL(spi_mem_adjust_op_freq);

/**
 * spi_mem_calc_op_duration() - 估算一个操作的理论持续时间（纳秒）
 *			       以便在多个候选方案中选出最优解。
 * @mem: SPI memory 对象
 * @op: 需要评估的操作
 *
 * 某些芯片对不同操作有频率限制，PCB 也会带来额外约束；
 * 控制器则可能支持 dual、quad 甚至 octal 模式，有时还支持 DTR。
 * 这些组合太多，无法静态地为所有场景列出最佳方案。
 * 如果想要准确，就必须给这些组合打分（例如估算时间），
 * 然后基于这些估算挑出最优解。
 *
 * Return: 返回该操作耗时的纳秒估计值。
 *         如果没有设置频率限制，则返回的是周期数，
 *         方便调用者在相同频率条件下比较不同方案的快慢。
 */
u64 spi_mem_calc_op_duration(struct spi_mem *mem, struct spi_mem_op *op)
{
	u64 ncycles = 0;
	u64 ps_per_cycles, duration;

	spi_mem_adjust_op_freq(mem, op);

	if (op->max_freq) {
		ps_per_cycles = 1000000000000ULL;
		do_div(ps_per_cycles, op->max_freq);
	} else {
		/* 在这种情况下，单位不再是时间单位。 */
		ps_per_cycles = 1;
	}

	ncycles += ((op->cmd.nbytes * 8) / op->cmd.buswidth) / (op->cmd.dtr ? 2 : 1);
	ncycles += ((op->addr.nbytes * 8) / op->addr.buswidth) / (op->addr.dtr ? 2 : 1);

	/* 对某些 SPI flash 操作来说，dummy bytes 是可选的。 */
	if (op->dummy.nbytes)
		ncycles += ((op->dummy.nbytes * 8) / op->dummy.buswidth) / (op->dummy.dtr ? 2 : 1);

	ncycles += ((op->data.nbytes * 8) / op->data.buswidth) / (op->data.dtr ? 2 : 1);

	/* 先计算皮秒级持续时间。 */
	duration = ncycles * ps_per_cycles;
	/* 再换算成纳秒。 */
	do_div(duration, 1000);

	return duration;
}
EXPORT_SYMBOL_GPL(spi_mem_calc_op_duration);

static ssize_t spi_mem_no_dirmap_read(struct spi_mem_dirmap_desc *desc,
				      u64 offs, size_t len, void *buf)
{
	struct spi_mem_op op = desc->info.op_tmpl;
	int ret;

	op.addr.val = desc->info.offset + offs;
	op.data.buf.in = buf;
	op.data.nbytes = len;
	ret = spi_mem_adjust_op_size(desc->mem, &op);
	if (ret)
		return ret;

	ret = spi_mem_exec_op(desc->mem, &op);
	if (ret)
		return ret;

	return op.data.nbytes;
}

static ssize_t spi_mem_no_dirmap_write(struct spi_mem_dirmap_desc *desc,
				       u64 offs, size_t len, const void *buf)
{
	struct spi_mem_op op = desc->info.op_tmpl;
	int ret;

	op.addr.val = desc->info.offset + offs;
	op.data.buf.out = buf;
	op.data.nbytes = len;
	ret = spi_mem_adjust_op_size(desc->mem, &op);
	if (ret)
		return ret;

	ret = spi_mem_exec_op(desc->mem, &op);
	if (ret)
		return ret;

	return op.data.nbytes;
}

/**
 * spi_mem_dirmap_create() - 创建一个直接映射描述符
 * @mem: 需要创建直接映射的 SPI mem 设备
 * @info: 直接映射信息
 *
 * 该函数会创建一个直接映射描述符，之后可通过
 * spi_mem_dirmap_read() 或 spi_mem_dirmap_write() 访问内存。
 * 如果 SPI 控制器驱动不支持直接映射，本函数会回退到
 * spi_mem_exec_op() 的实现，这样调用者就不用自己再写 fallback。
 *
 * Return: 成功时返回有效指针，失败时返回 ERR_PTR() 封装的错误指针。
 */
struct spi_mem_dirmap_desc *
spi_mem_dirmap_create(struct spi_mem *mem,
		      const struct spi_mem_dirmap_info *info)
{
	struct spi_controller *ctlr = mem->spi->controller;
	struct spi_mem_dirmap_desc *desc;
	int ret = -ENOTSUPP;

	/* 确保地址周期数在 1 到 8 字节之间。 */
	if (!info->op_tmpl.addr.nbytes || info->op_tmpl.addr.nbytes > 8)
		return ERR_PTR(-EINVAL);

	/* data.dir 必须是 SPI_MEM_DATA_IN 或 SPI_MEM_DATA_OUT。 */
	if (info->op_tmpl.data.dir == SPI_MEM_NO_DATA)
		return ERR_PTR(-EINVAL);

	desc = kzalloc_obj(*desc);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	desc->mem = mem;
	desc->info = *info;
	if (ctlr->mem_ops && ctlr->mem_ops->dirmap_create) {
		ret = spi_mem_access_start(mem);
		if (ret) {
			kfree(desc);
			return ERR_PTR(ret);
		}

		ret = ctlr->mem_ops->dirmap_create(desc);

		spi_mem_access_end(mem);
	}

	if (ret) {
		desc->nodirmap = true;
		if (!spi_mem_supports_op(desc->mem, &desc->info.op_tmpl))
			ret = -EOPNOTSUPP;
		else
			ret = 0;
	}

	if (ret) {
		kfree(desc);
		return ERR_PTR(ret);
	}

	return desc;
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_create);

/**
 * spi_mem_dirmap_destroy() - 销毁直接映射描述符
 * @desc: 需要销毁的直接映射描述符
 *
 * 这个函数销毁之前由 spi_mem_dirmap_create() 创建的直接映射描述符。
 */
void spi_mem_dirmap_destroy(struct spi_mem_dirmap_desc *desc)
{
	struct spi_controller *ctlr = desc->mem->spi->controller;

	if (!desc->nodirmap && ctlr->mem_ops && ctlr->mem_ops->dirmap_destroy)
		ctlr->mem_ops->dirmap_destroy(desc);

	kfree(desc);
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_destroy);

static void devm_spi_mem_dirmap_release(struct device *dev, void *res)
{
	struct spi_mem_dirmap_desc *desc = *(struct spi_mem_dirmap_desc **)res;

	spi_mem_dirmap_destroy(desc);
}

/**
 * devm_spi_mem_dirmap_create() - 创建直接映射描述符并把它挂到设备上
 * @dev: 这个 dirmap 描述符将要挂载到的设备
 * @mem: 需要创建直接映射的 SPI mem 设备
 * @info: 直接映射信息
 *
 * spi_mem_dirmap_create() 的 devm 版本。更多细节请参考
 * spi_mem_dirmap_create()。
 *
 * Return: 成功时返回有效指针，失败时返回 ERR_PTR() 封装的错误指针。
 */
struct spi_mem_dirmap_desc *
devm_spi_mem_dirmap_create(struct device *dev, struct spi_mem *mem,
			   const struct spi_mem_dirmap_info *info)
{
	struct spi_mem_dirmap_desc **ptr, *desc;

	ptr = devres_alloc(devm_spi_mem_dirmap_release, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	desc = spi_mem_dirmap_create(mem, info);
	if (IS_ERR(desc)) {
		devres_free(ptr);
	} else {
		*ptr = desc;
		devres_add(dev, ptr);
	}

	return desc;
}
EXPORT_SYMBOL_GPL(devm_spi_mem_dirmap_create);

static int devm_spi_mem_dirmap_match(struct device *dev, void *res, void *data)
{
	struct spi_mem_dirmap_desc **ptr = res;

	if (WARN_ON(!ptr || !*ptr))
		return 0;

	return *ptr == data;
}

/**
 * devm_spi_mem_dirmap_destroy() - 销毁挂在设备上的直接映射描述符
 * @dev: 这个 dirmap 描述符所属的设备
 * @desc: 需要销毁的直接映射描述符
 *
 * spi_mem_dirmap_destroy() 的 devm 版本。更多细节请参考
 * spi_mem_dirmap_destroy()。
 */
void devm_spi_mem_dirmap_destroy(struct device *dev,
				 struct spi_mem_dirmap_desc *desc)
{
	devres_release(dev, devm_spi_mem_dirmap_release,
		       devm_spi_mem_dirmap_match, desc);
}
EXPORT_SYMBOL_GPL(devm_spi_mem_dirmap_destroy);

/**
 * spi_mem_dirmap_read() - 通过直接映射读取数据
 * @desc: 直接映射描述符
 * @offs: 开始读取的偏移量。注意这不是绝对偏移，而是直接映射内部
 *        的偏移；该映射本身已经带有自己的基地址偏移。
 * @len: 长度，单位字节
 * @buf: 目的缓冲区。该缓冲区必须可用于 DMA
 *
 * 该函数使用之前通过 spi_mem_dirmap_create() 创建的直接映射
 * 从内存设备读取数据。
 *
 * Return: 返回从内存设备读取的数据量，或者一个负错误码。
 *         注意，返回值可能小于 @len；遇到这种情况时，需要调用者
 *         再次调用 spi_mem_dirmap_read() 继续读取剩余数据。
 */
ssize_t spi_mem_dirmap_read(struct spi_mem_dirmap_desc *desc,
			    u64 offs, size_t len, void *buf)
{
	struct spi_controller *ctlr = desc->mem->spi->controller;
	ssize_t ret;

	if (desc->info.op_tmpl.data.dir != SPI_MEM_DATA_IN)
		return -EINVAL;

	if (!len)
		return 0;

	if (desc->nodirmap) {
		ret = spi_mem_no_dirmap_read(desc, offs, len, buf);
	} else if (ctlr->mem_ops && ctlr->mem_ops->dirmap_read) {
		ret = spi_mem_access_start(desc->mem);
		if (ret)
			return ret;

		ret = ctlr->mem_ops->dirmap_read(desc, offs, len, buf);

		spi_mem_access_end(desc->mem);
	} else {
		ret = -ENOTSUPP;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_read);

/**
 * spi_mem_dirmap_write() - 通过直接映射写入数据
 * @desc: 直接映射描述符
 * @offs: 开始写入的偏移量。注意这不是绝对偏移，而是直接映射内部
 *        的偏移；该映射本身已经带有自己的基地址偏移。
 * @len: 长度，单位字节
 * @buf: 源缓冲区。该缓冲区必须可用于 DMA
 *
 * 该函数使用之前通过 spi_mem_dirmap_create() 创建的直接映射
 * 向内存设备写入数据。
 *
 * Return: 返回写入内存设备的数据量，或者一个负错误码。
 *         注意，返回值可能小于 @len；遇到这种情况时，需要调用者
 *         再次调用 spi_mem_dirmap_write() 继续写入剩余数据。
 */
ssize_t spi_mem_dirmap_write(struct spi_mem_dirmap_desc *desc,
			     u64 offs, size_t len, const void *buf)
{
	struct spi_controller *ctlr = desc->mem->spi->controller;
	ssize_t ret;

	if (desc->info.op_tmpl.data.dir != SPI_MEM_DATA_OUT)
		return -EINVAL;

	if (!len)
		return 0;

	if (desc->nodirmap) {
		ret = spi_mem_no_dirmap_write(desc, offs, len, buf);
	} else if (ctlr->mem_ops && ctlr->mem_ops->dirmap_write) {
		ret = spi_mem_access_start(desc->mem);
		if (ret)
			return ret;

		ret = ctlr->mem_ops->dirmap_write(desc, offs, len, buf);

		spi_mem_access_end(desc->mem);
	} else {
		ret = -ENOTSUPP;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(spi_mem_dirmap_write);

static inline struct spi_mem_driver *to_spi_mem_drv(struct device_driver *drv)
{
	return container_of(drv, struct spi_mem_driver, spidrv.driver);
}

static int spi_mem_read_status(struct spi_mem *mem,
			       const struct spi_mem_op *op,
			       u16 *status)
{
	const u8 *bytes = (u8 *)op->data.buf.in;
	int ret;

	ret = spi_mem_exec_op(mem, op);
	if (ret)
		return ret;

	if (op->data.nbytes > 1)
		*status = ((u16)bytes[0] << 8) | bytes[1];
	else
		*status = bytes[0];

	return 0;
}

/**
 * spi_mem_poll_status() - 轮询内存设备状态
 * @mem: SPI memory 设备
 * @op: 要执行的 memory operation
 * @mask: 需要检查的状态位掩码
 * @match: 期望满足的 (status & mask) 值
 * @initial_delay_us: 开始轮询前先等待的微秒数
 * @polling_delay_us: 两次读取之间的睡眠时间，单位微秒
 * @timeout_ms: 超时时间，单位毫秒
 *
 * 该函数轮询状态寄存器，当 (status & mask) == match
 * 或者超时后返回。
 *
 * Return: 成功返回 0，超时返回 -ETIMEDOUT，
 *         不支持时返回 -EOPNOTSUPP。
 */
int spi_mem_poll_status(struct spi_mem *mem,
			const struct spi_mem_op *op,
			u16 mask, u16 match,
			unsigned long initial_delay_us,
			unsigned long polling_delay_us,
			u16 timeout_ms)
{
	struct spi_controller *ctlr = mem->spi->controller;
	int ret = -EOPNOTSUPP;
	int read_status_ret;
	u16 status;

	if (op->data.nbytes < 1 || op->data.nbytes > 2 ||
	    op->data.dir != SPI_MEM_DATA_IN)
		return -EINVAL;

	if (ctlr->mem_ops && ctlr->mem_ops->poll_status && !spi_get_csgpiod(mem->spi, 0)) {
		ret = spi_mem_access_start(mem);
		if (ret)
			return ret;

		ret = ctlr->mem_ops->poll_status(mem, op, mask, match,
						 initial_delay_us, polling_delay_us,
						 timeout_ms);

		spi_mem_access_end(mem);
	}

	if (ret == -EOPNOTSUPP) {
		if (!spi_mem_supports_op(mem, op))
			return ret;

		if (initial_delay_us < 10)
			udelay(initial_delay_us);
		else
			usleep_range((initial_delay_us >> 2) + 1,
				     initial_delay_us);

		ret = read_poll_timeout(spi_mem_read_status, read_status_ret,
					(read_status_ret || ((status) & mask) == match),
					polling_delay_us, timeout_ms * 1000, false, mem,
					op, &status);
		if (read_status_ret)
			return read_status_ret;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(spi_mem_poll_status);

static int spi_mem_probe(struct spi_device *spi)
{
	struct spi_mem_driver *memdrv = to_spi_mem_drv(spi->dev.driver);
	struct spi_controller *ctlr = spi->controller;
	struct spi_mem *mem;

	mem = devm_kzalloc(&spi->dev, sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	mem->spi = spi;

	if (ctlr->mem_ops && ctlr->mem_ops->get_name)
		mem->name = ctlr->mem_ops->get_name(mem);
	else
		mem->name = dev_name(&spi->dev);

	if (IS_ERR_OR_NULL(mem->name))
		return PTR_ERR_OR_ZERO(mem->name);

	spi_set_drvdata(spi, mem);

	return memdrv->probe(mem);
}

static void spi_mem_remove(struct spi_device *spi)
{
	struct spi_mem_driver *memdrv = to_spi_mem_drv(spi->dev.driver);
	struct spi_mem *mem = spi_get_drvdata(spi);

	if (memdrv->remove)
		memdrv->remove(mem);
}

static void spi_mem_shutdown(struct spi_device *spi)
{
	struct spi_mem_driver *memdrv = to_spi_mem_drv(spi->dev.driver);
	struct spi_mem *mem = spi_get_drvdata(spi);

	if (memdrv->shutdown)
		memdrv->shutdown(mem);
}

/**
 * spi_mem_driver_register_with_owner() - 注册 SPI memory 驱动
 * @memdrv: 需要注册的 SPI memory 驱动
 * @owner: 该驱动的所有者
 *
 * 注册一个 SPI memory 驱动。
 *
 * Return: 成功返回 0，否则返回负错误码。
 */

int spi_mem_driver_register_with_owner(struct spi_mem_driver *memdrv,
				       struct module *owner)
{
	memdrv->spidrv.probe = spi_mem_probe;
	memdrv->spidrv.remove = spi_mem_remove;
	memdrv->spidrv.shutdown = spi_mem_shutdown;

	return __spi_register_driver(owner, &memdrv->spidrv);
}
EXPORT_SYMBOL_GPL(spi_mem_driver_register_with_owner);

/**
 * spi_mem_driver_unregister() - 注销 SPI memory 驱动
 * @memdrv: 需要注销的 SPI memory 驱动
 *
 * 注销一个 SPI memory 驱动。
 */
void spi_mem_driver_unregister(struct spi_mem_driver *memdrv)
{
	spi_unregister_driver(&memdrv->spidrv);
}
EXPORT_SYMBOL_GPL(spi_mem_driver_unregister);
