/*
 * MIPI DSI Bus
 *
 * Copyright (C) 2012-2013, Samsung Electronics, Co., Ltd.
 * Andrzej Hajda <a.hajda@samsung.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * MIPI DSI（Display Serial Interface）总线基础设施
 *
 * MIPI DSI 是 MIPI 联盟定义的一种显示串行接口标准，广泛应用于
 * 智能手机、平板电脑等移动设备的显示面板连接。DSI 使用高速串行
 * 差分信号传输，相比传统的并行 RGB 接口，具有引脚数少、传输速率高、
 * 功耗低、抗干扰能力强等优点。
 *
 * 本文件实现了 MIPI DSI 总线的基础架构，包括：
 *
 * 1. 总线类型管理：
 *    - 定义 mipi_dsi_bus_type 总线类型，用于 DSI 设备和驱动的匹配
 *    - 提供设备和驱动的注册/注销函数
 *    - 支持 OF（Device Tree）风格的设备匹配
 *
 * 2. DSI 设备管理：
 *    - DSI 设备的创建、注册和注销（mipi_dsi_device_register_full 等）
 *    - 设备生命周期的托管版本（devm_ 系列函数）
 *    - DSI 虚拟通道管理（支持 4 个虚拟通道）
 *
 * 3. DSI 主机管理：
 *    - DSI 主机的注册和注销（mipi_dsi_host_register/unregister）
 *    - 主机与从设备的连接和断开（mipi_dsi_attach/detach）
 *    - 设备树节点的遍历和 DSI 设备创建
 *
 * 4. DSI 数据包传输：
 *    - 短数据包和长数据包的创建
 *    - 通用写/读操作（mipi_dsi_generic_write/read）
 *    - DCS（Display Command Set）命令的写/读操作
 *    - 支持压缩数据流（DSC）传输
 *
 * 5. DCS 标准命令集：
 *    - 显示控制：开/关、睡眠模式、复位
 *    - 显示参数设置：列地址、页地址、像素格式、亮度
 *    - 状态读取：电源模式、像素格式、亮度
 *    - 撕裂效果（Tearing Effect）控制
 *
 * 6. 批量操作支持：
 *    - 多个 DSI 事务的累积错误处理（_multi 系列函数）
 *    - 双通道 DSI 的同时写入支持
 */

#include <linux/device.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <drm/display/drm_dsc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>

#include <linux/media-bus-format.h>

#include <video/mipi_display.h>

/**
 * DOC: dsi helpers
 *
 * These functions contain some common logic and helpers to deal with MIPI DSI
 * peripherals.
 *
 * Helpers are provided for a number of standard MIPI DSI command as well as a
 * subset of the MIPI DCS command set.
 */

static int mipi_dsi_device_match(struct device *dev, const struct device_driver *drv)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);

	/* attempt OF style match */
	if (of_driver_match_device(dev, drv))
		return 1;

	/* compare DSI device and driver names */
	if (!strcmp(dsi->name, drv->name))
		return 1;

	return 0;
}

static int mipi_dsi_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	int err;

	err = of_device_uevent_modalias(dev, env);
	if (err != -ENODEV)
		return err;

	add_uevent_var(env, "MODALIAS=%s%s", MIPI_DSI_MODULE_PREFIX,
		       dsi->name);

	return 0;
}

static const struct dev_pm_ops mipi_dsi_device_pm_ops = {
	.runtime_suspend = pm_generic_runtime_suspend,
	.runtime_resume = pm_generic_runtime_resume,
	.suspend = pm_generic_suspend,
	.resume = pm_generic_resume,
	.freeze = pm_generic_freeze,
	.thaw = pm_generic_thaw,
	.poweroff = pm_generic_poweroff,
	.restore = pm_generic_restore,
};

const struct bus_type mipi_dsi_bus_type = {
	.name = "mipi-dsi",
	.match = mipi_dsi_device_match,
	.uevent = mipi_dsi_uevent,
	.pm = &mipi_dsi_device_pm_ops,
};
EXPORT_SYMBOL_GPL(mipi_dsi_bus_type);

/**
 * of_find_mipi_dsi_device_by_node() - 通过设备树节点查找 MIPI DSI 设备
 * @np: 设备树节点
 *
 * 在 DSI 总线中查找与指定设备树节点对应的 MIPI DSI 设备。
 * 该函数遍历已注册的 DSI 设备，找到与给定 OF 节点匹配的设备。
 *
 * 返回：与 @np 对应的 MIPI DSI 设备指针，如果没有找到（或尚未注册）
 * 则返回 NULL。
 */
struct mipi_dsi_device *of_find_mipi_dsi_device_by_node(struct device_node *np)
{
	struct device *dev;

	dev = bus_find_device_by_of_node(&mipi_dsi_bus_type, np);

	return dev ? to_mipi_dsi_device(dev) : NULL;
}
EXPORT_SYMBOL(of_find_mipi_dsi_device_by_node);

static void mipi_dsi_dev_release(struct device *dev)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);

	of_node_put(dev->of_node);
	kfree(dsi);
}

static const struct device_type mipi_dsi_device_type = {
	.release = mipi_dsi_dev_release,
};

static struct mipi_dsi_device *mipi_dsi_device_alloc(struct mipi_dsi_host *host)
{
	struct mipi_dsi_device *dsi;

	dsi = kzalloc_obj(*dsi);
	if (!dsi)
		return ERR_PTR(-ENOMEM);

	dsi->host = host;
	dsi->dev.bus = &mipi_dsi_bus_type;
	dsi->dev.parent = host->dev;
	dsi->dev.type = &mipi_dsi_device_type;

	device_initialize(&dsi->dev);

	return dsi;
}

static int mipi_dsi_device_add(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_host *host = dsi->host;

	dev_set_name(&dsi->dev, "%s.%d", dev_name(host->dev),  dsi->channel);

	return device_add(&dsi->dev);
}

#if IS_ENABLED(CONFIG_OF)
static struct mipi_dsi_device *
of_mipi_dsi_device_add(struct mipi_dsi_host *host, struct device_node *node)
{
	struct mipi_dsi_device_info info = { };
	int ret;
	u32 reg;

	if (of_alias_from_compatible(node, info.type, sizeof(info.type)) < 0) {
		dev_err(host->dev, "modalias failure on %pOF\n", node);
		return ERR_PTR(-EINVAL);
	}

	ret = of_property_read_u32(node, "reg", &reg);
	if (ret) {
		dev_err(host->dev, "device node %pOF has no valid reg property: %d\n",
			node, ret);
		return ERR_PTR(-EINVAL);
	}

	info.channel = reg;
	info.node = of_node_get(node);

	return mipi_dsi_device_register_full(host, &info);
}
#else
static struct mipi_dsi_device *
of_mipi_dsi_device_add(struct mipi_dsi_host *host, struct device_node *node)
{
	return ERR_PTR(-ENODEV);
}
#endif

/**
 * mipi_dsi_device_register_full - 创建 MIPI DSI 设备
 * @host: 此设备连接的 DSI 主机
 * @info: 包含 DSI 设备信息的模板指针
 *
 * 使用 mipi_dsi_device_info 模板提供的设备信息创建 MIPI DSI 设备。
 * 该函数会：
 *   1. 验证 info 指针和虚拟通道号（0-3）的有效性
 *   2. 分配并初始化 DSI 设备结构体
 *   3. 设置设备名称、通道号和 OF 节点
 *   4. 将设备添加到系统中
 *
 * 返回：
 * 成功返回新创建的 MIPI DSI 设备指针，失败返回错误编码指针。
 */
struct mipi_dsi_device *
mipi_dsi_device_register_full(struct mipi_dsi_host *host,
			      const struct mipi_dsi_device_info *info)
{
	struct mipi_dsi_device *dsi;
	int ret;

	if (!info) {
		dev_err(host->dev, "invalid mipi_dsi_device_info pointer\n");
		return ERR_PTR(-EINVAL);
	}

	if (info->channel > 3) {
		dev_err(host->dev, "invalid virtual channel: %u\n", info->channel);
		return ERR_PTR(-EINVAL);
	}

	dsi = mipi_dsi_device_alloc(host);
	if (IS_ERR(dsi)) {
		dev_err(host->dev, "failed to allocate DSI device %ld\n",
			PTR_ERR(dsi));
		return dsi;
	}

	device_set_node(&dsi->dev, of_fwnode_handle(info->node));
	dsi->channel = info->channel;
	strscpy(dsi->name, info->type, sizeof(dsi->name));

	ret = mipi_dsi_device_add(dsi);
	if (ret) {
		dev_err(host->dev, "failed to add DSI device %d\n", ret);
		kfree(dsi);
		return ERR_PTR(ret);
	}

	return dsi;
}
EXPORT_SYMBOL(mipi_dsi_device_register_full);

/**
 * mipi_dsi_device_unregister - 注销 MIPI DSI 设备
 * @dsi: DSI 外设设备
 *
 * 从系统中注销一个之前注册的 MIPI DSI 设备，释放相关资源。
 */
void mipi_dsi_device_unregister(struct mipi_dsi_device *dsi)
{
	device_unregister(&dsi->dev);
}
EXPORT_SYMBOL(mipi_dsi_device_unregister);

static void devm_mipi_dsi_device_unregister(void *arg)
{
	struct mipi_dsi_device *dsi = arg;

	mipi_dsi_device_unregister(dsi);
}

/**
 * devm_mipi_dsi_device_register_full - 创建托管的 MIPI DSI 设备
 * @dev: 与 MIPI DSI 设备生命周期绑定的设备
 * @host: 此设备连接的 DSI 主机
 * @info: 包含 DSI 设备信息的模板指针
 *
 * 使用 mipi_dsi_device_info 模板提供的设备信息创建 MIPI DSI 设备。
 * 这是 mipi_dsi_device_register_full() 的托管版本，会在 @dev
 * 解除绑定时自动调用 mipi_dsi_device_unregister() 进行清理。
 *
 * 使用托管版本可以简化驱动的错误处理路径，无需手动管理
 * DSI 设备的注销。
 *
 * 返回：
 * 成功返回新创建的 MIPI DSI 设备指针，失败返回错误编码指针。
 */
struct mipi_dsi_device *
devm_mipi_dsi_device_register_full(struct device *dev,
				   struct mipi_dsi_host *host,
				   const struct mipi_dsi_device_info *info)
{
	struct mipi_dsi_device *dsi;
	int ret;

	dsi = mipi_dsi_device_register_full(host, info);
	if (IS_ERR(dsi))
		return dsi;

	ret = devm_add_action_or_reset(dev,
				       devm_mipi_dsi_device_unregister,
				       dsi);
	if (ret)
		return ERR_PTR(ret);

	return dsi;
}
EXPORT_SYMBOL_GPL(devm_mipi_dsi_device_register_full);

static DEFINE_MUTEX(host_lock);
static LIST_HEAD(host_list);

/**
 * of_find_mipi_dsi_host_by_node() - 通过设备树节点查找 MIPI DSI 主机
 * @node: 设备树节点
 *
 * 在已注册的 DSI 主机列表中查找与指定设备树节点匹配的主机。
 * 遍历全局主机列表，通过比较 OF 节点指针来进行匹配。
 *
 * 返回：
 * 与 @node 对应的 MIPI DSI 主机指针，如果没有找到（或尚未注册）
 * 则返回 NULL。
 */
struct mipi_dsi_host *of_find_mipi_dsi_host_by_node(struct device_node *node)
{
	struct mipi_dsi_host *host;

	mutex_lock(&host_lock);

	list_for_each_entry(host, &host_list, list) {
		if (host->dev->of_node == node) {
			mutex_unlock(&host_lock);
			return host;
		}
	}

	mutex_unlock(&host_lock);

	return NULL;
}
EXPORT_SYMBOL(of_find_mipi_dsi_host_by_node);

int mipi_dsi_host_register(struct mipi_dsi_host *host)
{
	struct device_node *node;

	for_each_available_child_of_node(host->dev->of_node, node) {
		/* skip nodes without reg property */
		if (!of_property_present(node, "reg"))
			continue;
		of_mipi_dsi_device_add(host, node);
	}

	mutex_lock(&host_lock);
	list_add_tail(&host->list, &host_list);
	mutex_unlock(&host_lock);

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_host_register);

static int mipi_dsi_remove_device_fn(struct device *dev, void *priv)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);

	if (dsi->attached)
		mipi_dsi_detach(dsi);
	mipi_dsi_device_unregister(dsi);

	return 0;
}

void mipi_dsi_host_unregister(struct mipi_dsi_host *host)
{
	device_for_each_child(host->dev, NULL, mipi_dsi_remove_device_fn);

	mutex_lock(&host_lock);
	list_del_init(&host->list);
	mutex_unlock(&host_lock);
}
EXPORT_SYMBOL(mipi_dsi_host_unregister);

/**
 * mipi_dsi_attach - 将 DSI 设备连接到其 DSI 主机
 * @dsi: DSI 外设
 *
 * 将 DSI 外设设备连接到其所属的 DSI 主机。连接成功后，主机和
 * 外设之间可以开始数据传输。该函数会调用主机的 attach 回调
 * 来完成实际的硬件连接操作。
 *
 * 返回：成功返回 0，失败返回负错误码（如 -ENOSYS 表示主机未
 * 实现 attach 操作）。
 */
int mipi_dsi_attach(struct mipi_dsi_device *dsi)
{
	const struct mipi_dsi_host_ops *ops = dsi->host->ops;
	int ret;

	if (!ops || !ops->attach)
		return -ENOSYS;

	ret = ops->attach(dsi->host, dsi);
	if (ret)
		return ret;

	dsi->attached = true;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_attach);

/**
 * mipi_dsi_detach - 将 DSI 设备从其 DSI 主机断开
 * @dsi: DSI 外设
 *
 * 将 DSI 外设设备从其所属的 DSI 主机断开。断开后，主机和外设
 * 之间的数据传输将停止。该函数会检查设备是否已连接，如果未连接
 * 则发出警告。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_detach(struct mipi_dsi_device *dsi)
{
	const struct mipi_dsi_host_ops *ops = dsi->host->ops;

	if (WARN_ON(!dsi->attached))
		return -EINVAL;

	if (!ops || !ops->detach)
		return -ENOSYS;

	dsi->attached = false;

	return ops->detach(dsi->host, dsi);
}
EXPORT_SYMBOL(mipi_dsi_detach);

static void devm_mipi_dsi_detach(void *arg)
{
	struct mipi_dsi_device *dsi = arg;

	mipi_dsi_detach(dsi);
}

/**
 * devm_mipi_dsi_attach - 托管的 MIPI DSI 设备连接
 * @dev: 与 MIPI DSI 设备连接生命周期绑定的设备
 * @dsi: DSI 外设
 *
 * 这是 mipi_dsi_attach() 的托管版本，会在 @dev 解除绑定时
 * 自动调用 mipi_dsi_detach() 断开连接。使用托管版本可以简化
 * 驱动的错误处理和清理路径。
 *
 * 返回：
 * 成功返回 0，失败返回负错误码。
 */
int devm_mipi_dsi_attach(struct device *dev,
			 struct mipi_dsi_device *dsi)
{
	int ret;

	ret = mipi_dsi_attach(dsi);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, devm_mipi_dsi_detach, dsi);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(devm_mipi_dsi_attach);

static ssize_t mipi_dsi_device_transfer(struct mipi_dsi_device *dsi,
					struct mipi_dsi_msg *msg)
{
	const struct mipi_dsi_host_ops *ops = dsi->host->ops;

	if (!ops || !ops->transfer)
		return -ENOSYS;

	if (dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg->flags |= MIPI_DSI_MSG_USE_LPM;

	return ops->transfer(dsi->host, msg);
}

/**
 * mipi_dsi_packet_format_is_short - 检查数据包是否为短格式
 * @type: 数据包的 MIPI DSI 数据类型
 *
 * MIPI DSI 协议定义了两类数据包：
 *   短数据包（4字节）：包含一个字节的数据类型和最多两个字节的参数，
 *   用于命令和控制信息传输。
 *   长数据包（可变长度）：包含头部、数据负载和 CRC 校验，用于传输
 *   大量数据（如图像数据）。
 *
 * 返回：如果给定数据类型的包是短数据包则返回 true，否则返回 false。
 */
bool mipi_dsi_packet_format_is_short(u8 type)
{
	switch (type) {
	case MIPI_DSI_V_SYNC_START:
	case MIPI_DSI_V_SYNC_END:
	case MIPI_DSI_H_SYNC_START:
	case MIPI_DSI_H_SYNC_END:
	case MIPI_DSI_COMPRESSION_MODE:
	case MIPI_DSI_END_OF_TRANSMISSION:
	case MIPI_DSI_COLOR_MODE_OFF:
	case MIPI_DSI_COLOR_MODE_ON:
	case MIPI_DSI_SHUTDOWN_PERIPHERAL:
	case MIPI_DSI_TURN_ON_PERIPHERAL:
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
	case MIPI_DSI_DCS_READ:
	case MIPI_DSI_EXECUTE_QUEUE:
	case MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE:
		return true;
	}

	return false;
}
EXPORT_SYMBOL(mipi_dsi_packet_format_is_short);

/**
 * mipi_dsi_packet_format_is_long - 检查数据包是否为长格式
 * @type: 数据包的 MIPI DSI 数据类型
 *
 * 长数据包用于传输大量数据，如图像数据流。长数据包包含：
 *   1. 头部（4字节）：数据类型 + 字计数
 *   2. 数据负载：字计数指定长度的有效数据
 *   3. CRC（2字节）：校验和
 *
 * 返回：如果给定数据类型的包是长数据包则返回 true，否则返回 false。
 */
bool mipi_dsi_packet_format_is_long(u8 type)
{
	switch (type) {
	case MIPI_DSI_NULL_PACKET:
	case MIPI_DSI_BLANKING_PACKET:
	case MIPI_DSI_GENERIC_LONG_WRITE:
	case MIPI_DSI_DCS_LONG_WRITE:
	case MIPI_DSI_PICTURE_PARAMETER_SET:
	case MIPI_DSI_COMPRESSED_PIXEL_STREAM:
	case MIPI_DSI_LOOSELY_PACKED_PIXEL_STREAM_YCBCR20:
	case MIPI_DSI_PACKED_PIXEL_STREAM_YCBCR24:
	case MIPI_DSI_PACKED_PIXEL_STREAM_YCBCR16:
	case MIPI_DSI_PACKED_PIXEL_STREAM_30:
	case MIPI_DSI_PACKED_PIXEL_STREAM_36:
	case MIPI_DSI_PACKED_PIXEL_STREAM_YCBCR12:
	case MIPI_DSI_PACKED_PIXEL_STREAM_16:
	case MIPI_DSI_PACKED_PIXEL_STREAM_18:
	case MIPI_DSI_PIXEL_STREAM_3BYTE_18:
	case MIPI_DSI_PACKED_PIXEL_STREAM_24:
		return true;
	}

	return false;
}
EXPORT_SYMBOL(mipi_dsi_packet_format_is_long);

/**
 * mipi_dsi_create_packet - 根据 DSI 协议从消息创建数据包
 * @packet: DSI 数据包结构指针
 * @msg: 要转换为数据包的消息
 *
 * 根据 MIPI DSI 协议将消息转换为数据包格式。该函数会：
 *   1. 验证输入的包和消息指针的有效性
 *   2. 检查数据类型是否为有效的短包或长包类型
 *   3. 验证虚拟通道号（0-3）的有效性
 *   4. 构建数据包头：编码通道号和数据类型
 *   5. 对于长数据包，在头部设置字计数并复制负载
 *   6. 对于短数据包，在头部参数位置填充最多两个参数字节
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_create_packet(struct mipi_dsi_packet *packet,
			   const struct mipi_dsi_msg *msg)
{
	if (!packet || !msg)
		return -EINVAL;

	/* do some minimum sanity checking */
	if (!mipi_dsi_packet_format_is_short(msg->type) &&
	    !mipi_dsi_packet_format_is_long(msg->type))
		return -EINVAL;

	if (msg->channel > 3)
		return -EINVAL;

	memset(packet, 0, sizeof(*packet));
	packet->header[0] = ((msg->channel & 0x3) << 6) | (msg->type & 0x3f);

	/* TODO: compute ECC if hardware support is not available */

	/*
	 * Long write packets contain the word count in header bytes 1 and 2.
	 * The payload follows the header and is word count bytes long.
	 *
	 * Short write packets encode up to two parameters in header bytes 1
	 * and 2.
	 */
	if (mipi_dsi_packet_format_is_long(msg->type)) {
		packet->header[1] = (msg->tx_len >> 0) & 0xff;
		packet->header[2] = (msg->tx_len >> 8) & 0xff;

		packet->payload_length = msg->tx_len;
		packet->payload = msg->tx_buf;
	} else {
		const u8 *tx = msg->tx_buf;

		packet->header[1] = (msg->tx_len > 0) ? tx[0] : 0;
		packet->header[2] = (msg->tx_len > 1) ? tx[1] : 0;
	}

	packet->size = sizeof(packet->header) + packet->payload_length;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_create_packet);

/**
 * mipi_dsi_shutdown_peripheral() - 发送关闭外设命令
 * @dsi: DSI 外设设备
 *
 * 向 DSI 外设发送 Shutdown Peripheral 命令。该命令用于关闭
 * 显示外设的电源，进入低功耗状态。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_shutdown_peripheral(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_SHUTDOWN_PERIPHERAL,
		.tx_buf = (u8 [2]) { 0, 0 },
		.tx_len = 2,
	};
	int ret = mipi_dsi_device_transfer(dsi, &msg);

	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(mipi_dsi_shutdown_peripheral);

/**
 * mipi_dsi_turn_on_peripheral() - 发送开启外设命令
 * @dsi: DSI 外设设备
 *
 * 向 DSI 外设发送 Turn On Peripheral 命令，使外设从关闭状态
 * 恢复到正常工作状态。
 *
 * 此函数已被弃用，请使用 mipi_dsi_turn_on_peripheral_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_turn_on_peripheral(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_TURN_ON_PERIPHERAL,
		.tx_buf = (u8 [2]) { 0, 0 },
		.tx_len = 2,
	};
	int ret = mipi_dsi_device_transfer(dsi, &msg);

	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(mipi_dsi_turn_on_peripheral);

/*
 * mipi_dsi_set_maximum_return_packet_size() - specify the maximum size of
 *    the payload in a long packet transmitted from the peripheral back to the
 *    host processor
 * @dsi: DSI peripheral device
 * @value: the maximum size of the payload
 *
 * Return: 0 on success or a negative error code on failure.
 */
int mipi_dsi_set_maximum_return_packet_size(struct mipi_dsi_device *dsi,
					    u16 value)
{
	u8 tx[2] = { value & 0xff, value >> 8 };
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE,
		.tx_len = sizeof(tx),
		.tx_buf = tx,
	};
	int ret = mipi_dsi_device_transfer(dsi, &msg);

	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(mipi_dsi_set_maximum_return_packet_size);

/**
 * mipi_dsi_compression_mode_ext() - 在外设上启用/禁用 DSC（显示流压缩）
 * @dsi: DSI 外设设备
 * @enable: 是否启用 DSC
 * @algo: 选择的压缩算法
 * @pps_selector: 从预存或上传的 PPS 条目表中选择 PPS
 *
 * 在外设上启用或禁用显示流压缩（Display Stream Compression）功能。
 * DSC 是一种高效的图像压缩技术，可以在高分辨率显示中显著降低
 * 带宽需求。参数编码方式：
 *   - bit 0: 启用/禁用
 *   - bits 1-2: 压缩算法选择
 *   - bits 4-5: PPS 条目选择
 *
 * 此函数已被弃用，请使用 mipi_dsi_compression_mode_ext_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_compression_mode_ext(struct mipi_dsi_device *dsi, bool enable,
				  enum mipi_dsi_compression_algo algo,
				  unsigned int pps_selector)
{
	u8 tx[2] = { };
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_COMPRESSION_MODE,
		.tx_len = sizeof(tx),
		.tx_buf = tx,
	};
	int ret;

	if (algo > 3 || pps_selector > 3)
		return -EINVAL;

	tx[0] = (enable << 0) |
		(algo << 1) |
		(pps_selector << 4);

	ret = mipi_dsi_device_transfer(dsi, &msg);

	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(mipi_dsi_compression_mode_ext);

/**
 * mipi_dsi_compression_mode() - 在外设上启用/禁用 DSC（使用默认参数）
 * @dsi: DSI 外设设备
 * @enable: 是否启用 DSC
 *
 * 使用默认的 Picture Parameter Set 和 VESA DSC 1.1 算法在外设上
 * 启用或禁用显示流压缩。这是 mipi_dsi_compression_mode_ext() 的
 * 简化封装，使用 MIPI_DSI_COMPRESSION_DSC 算法和 PPS 选择器 0。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_compression_mode(struct mipi_dsi_device *dsi, bool enable)
{
	return mipi_dsi_compression_mode_ext(dsi, enable, MIPI_DSI_COMPRESSION_DSC, 0);
}
EXPORT_SYMBOL(mipi_dsi_compression_mode);

/**
 * mipi_dsi_picture_parameter_set() - 向外设传输 DSC PPS（图像参数集）
 * @dsi: DSI 外设设备
 * @pps: VESA DSC 1.1 图像参数集
 *
 * 向 DSI 外设传输 VESA DSC 1.1 图像参数集（Picture Parameter Set）。
 * PPS 包含了 DSC 压缩过程中所需的全部参数，如图片尺寸、切片配置、
 * 比特率、初始延迟等。外设根据 PPS 中的参数正确解码压缩后的
 * 显示数据流。
 *
 * 此函数已被弃用，请使用 mipi_dsi_picture_parameter_set_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_picture_parameter_set(struct mipi_dsi_device *dsi,
				   const struct drm_dsc_picture_parameter_set *pps)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_PICTURE_PARAMETER_SET,
		.tx_len = sizeof(*pps),
		.tx_buf = pps,
	};
	int ret = mipi_dsi_device_transfer(dsi, &msg);

	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL(mipi_dsi_picture_parameter_set);

/**
 * mipi_dsi_generic_write() - 使用通用写数据包传输数据
 * @dsi: DSI 外设设备
 * @payload: 包含负载的缓冲区
 * @size: 负载缓冲区大小
 *
 * 通过通用写数据包向 DSI 外设发送数据。该函数会根据负载长度
 * 自动选择合适的数据类型：
 *   - size == 0: 无参数短写 (GENERIC_SHORT_WRITE_0_PARAM)
 *   - size == 1: 1参数短写 (GENERIC_SHORT_WRITE_1_PARAM)
 *   - size == 2: 2参数短写 (GENERIC_SHORT_WRITE_2_PARAM)
 *   - size >= 3: 长写 (GENERIC_LONG_WRITE)
 *
 * 返回：成功返回传输的字节数，失败返回负错误码。
 */
ssize_t mipi_dsi_generic_write(struct mipi_dsi_device *dsi, const void *payload,
			       size_t size)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.tx_buf = payload,
		.tx_len = size
	};

	switch (size) {
	case 0:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM;
		break;

	case 1:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM;
		break;

	case 2:
		msg.type = MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM;
		break;

	default:
		msg.type = MIPI_DSI_GENERIC_LONG_WRITE;
		break;
	}

	return mipi_dsi_device_transfer(dsi, &msg);
}
EXPORT_SYMBOL(mipi_dsi_generic_write);

/**
 * mipi_dsi_generic_write_multi() - mipi_dsi_generic_write() w/ accum_err
 * @ctx: Context for multiple DSI transactions
 * @payload: buffer containing the payload
 * @size: size of payload buffer
 *
 * A wrapper around mipi_dsi_generic_write() that deals with errors in a way
 * that makes it convenient to make several calls in a row.
 */
void mipi_dsi_generic_write_multi(struct mipi_dsi_multi_context *ctx,
				  const void *payload, size_t size)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_generic_write(dsi, payload, size);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending generic data %*ph failed: %d\n",
			(int)size, payload, ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_generic_write_multi);

/**
 * mipi_dsi_dual_generic_write_multi() - mipi_dsi_generic_write_multi() for
 * two dsi channels, one after the other
 * @ctx: Context for multiple DSI transactions
 * @dsi1: First dsi channel to write buffer to
 * @dsi2: Second dsi channel to write buffer to
 * @payload: Buffer containing the payload
 * @size: Size of payload buffer
 *
 * A wrapper around mipi_dsi_generic_write_multi() that allows the user to
 * conveniently write to two dsi channels, one after the other.
 */
void mipi_dsi_dual_generic_write_multi(struct mipi_dsi_multi_context *ctx,
				       struct mipi_dsi_device *dsi1,
				       struct mipi_dsi_device *dsi2,
				       const void *payload, size_t size)
{
	ctx->dsi = dsi1;
	mipi_dsi_generic_write_multi(ctx, payload, size);
	ctx->dsi = dsi2;
	mipi_dsi_generic_write_multi(ctx, payload, size);
}
EXPORT_SYMBOL(mipi_dsi_dual_generic_write_multi);

/**
 * mipi_dsi_generic_read() - 使用通用读数据包接收数据
 * @dsi: DSI 外设设备
 * @params: 包含请求参数的缓冲区
 * @num_params: 请求参数数量
 * @data: 用于返回接收数据的缓冲区
 * @size: 接收缓冲区大小
 *
 * 通过通用读请求数据包从 DSI 外设读取数据。该函数会根据参数数量
 * 自动选择合适的数据类型：
 *   - num_params == 0: 无参数读请求
 *   - num_params == 1: 1参数读请求
 *   - num_params == 2: 2参数读请求
 *   - num_params > 2: 不支持，返回 -EINVAL
 *
 * 返回：成功返回读取的字节数，失败返回负错误码。
 */
ssize_t mipi_dsi_generic_read(struct mipi_dsi_device *dsi, const void *params,
			      size_t num_params, void *data, size_t size)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.tx_len = num_params,
		.tx_buf = params,
		.rx_len = size,
		.rx_buf = data
	};

	switch (num_params) {
	case 0:
		msg.type = MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM;
		break;

	case 1:
		msg.type = MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM;
		break;

	case 2:
		msg.type = MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM;
		break;

	default:
		return -EINVAL;
	}

	return mipi_dsi_device_transfer(dsi, &msg);
}
EXPORT_SYMBOL(mipi_dsi_generic_read);

/**
 * drm_mipi_dsi_get_input_bus_fmt() - 根据 DSI 输出格式获取所需输入总线格式
 * @dsi_format: DSI 主机需要输出的像素格式
 *
 * 各种 DSI 主机可以在其 &drm_bridge_funcs.atomic_get_input_bus_fmts
 * 操作中使用此函数来确定所需的输入 MEDIA_BUS_FMT_* 像素格式。
 *
 * 支持的格式映射：
 *   - MIPI_DSI_FMT_RGB888  -> MEDIA_BUS_FMT_RGB888_1X24
 *   - MIPI_DSI_FMT_RGB666  -> MEDIA_BUS_FMT_RGB666_1X24_CPADHI
 *   - MIPI_DSI_FMT_RGB666_PACKED -> MEDIA_BUS_FMT_RGB666_1X18
 *   - MIPI_DSI_FMT_RGB565  -> MEDIA_BUS_FMT_RGB565_1X16
 *
 * 返回：
 * 成功返回 32 位 MEDIA_BUS_FMT_* 值，失败返回 0。
 */
u32 drm_mipi_dsi_get_input_bus_fmt(enum mipi_dsi_pixel_format dsi_format)
{
	switch (dsi_format) {
	case MIPI_DSI_FMT_RGB888:
		return MEDIA_BUS_FMT_RGB888_1X24;

	case MIPI_DSI_FMT_RGB666:
		return MEDIA_BUS_FMT_RGB666_1X24_CPADHI;

	case MIPI_DSI_FMT_RGB666_PACKED:
		return MEDIA_BUS_FMT_RGB666_1X18;

	case MIPI_DSI_FMT_RGB565:
		return MEDIA_BUS_FMT_RGB565_1X16;

	default:
		/* Unsupported DSI Format */
		return 0;
	}
}
EXPORT_SYMBOL(drm_mipi_dsi_get_input_bus_fmt);

/**
 * mipi_dsi_dcs_write_buffer() - 传输包含负载的 DCS 命令
 * @dsi: DSI 外设设备
 * @data: 包含要传输数据的缓冲区（第一个字节为 DCS 命令码）
 * @len: 传输缓冲区大小
 *
 * 传输包含负载的 DCS（Display Command Set）命令。该函数会根据
 * 数据长度自动选择合适的数据类型：
 *   - len == 1: DCS 短写（仅命令码）
 *   - len == 2: DCS 短写带参数（命令码 + 1字节参数）
 *   - len >= 3: DCS 长写
 *
 * 注意：len 不能为 0，否则返回 -EINVAL。
 *
 * 返回：成功返回传输的字节数，失败返回负错误码。
 */
ssize_t mipi_dsi_dcs_write_buffer(struct mipi_dsi_device *dsi,
				  const void *data, size_t len)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.tx_buf = data,
		.tx_len = len
	};

	switch (len) {
	case 0:
		return -EINVAL;

	case 1:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE;
		break;

	case 2:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		break;

	default:
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		break;
	}

	return mipi_dsi_device_transfer(dsi, &msg);
}
EXPORT_SYMBOL(mipi_dsi_dcs_write_buffer);

/**
 * mipi_dsi_dcs_write_buffer_chatty - mipi_dsi_dcs_write_buffer() w/ an error log
 * @dsi: DSI peripheral device
 * @data: buffer containing data to be transmitted
 * @len: size of transmission buffer
 *
 * Like mipi_dsi_dcs_write_buffer() but includes a dev_err()
 * call for you and returns 0 upon success, not the number of bytes sent.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int mipi_dsi_dcs_write_buffer_chatty(struct mipi_dsi_device *dsi,
				     const void *data, size_t len)
{
	struct device *dev = &dsi->dev;
	ssize_t ret;

	ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	if (ret < 0) {
		dev_err(dev, "sending dcs data %*ph failed: %zd\n",
			(int)len, data, ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_write_buffer_chatty);

/**
 * mipi_dsi_dcs_write_buffer_multi - mipi_dsi_dcs_write_buffer_chatty() w/ accum_err
 * @ctx: Context for multiple DSI transactions
 * @data: buffer containing data to be transmitted
 * @len: size of transmission buffer
 *
 * Like mipi_dsi_dcs_write_buffer_chatty() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_write_buffer_multi(struct mipi_dsi_multi_context *ctx,
				     const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending dcs data %*ph failed: %d\n",
			(int)len, data, ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_write_buffer_multi);

/**
 * mipi_dsi_dual_dcs_write_buffer_multi - mipi_dsi_dcs_write_buffer_multi() for
 * two dsi channels, one after the other
 * @ctx: Context for multiple DSI transactions
 * @dsi1: First dsi channel to write buffer to
 * @dsi2: Second dsi channel to write buffer to
 * @data: Buffer containing data to be transmitted
 * @len: Size of transmission buffer
 *
 * A wrapper around mipi_dsi_dcs_write_buffer_multi() that allows the user to
 * conveniently write to two dsi channels, one after the other.
 */
void mipi_dsi_dual_dcs_write_buffer_multi(struct mipi_dsi_multi_context *ctx,
					  struct mipi_dsi_device *dsi1,
					  struct mipi_dsi_device *dsi2,
					  const void *data, size_t len)
{
	ctx->dsi = dsi1;
	mipi_dsi_dcs_write_buffer_multi(ctx, data, len);
	ctx->dsi = dsi2;
	mipi_dsi_dcs_write_buffer_multi(ctx, data, len);
}
EXPORT_SYMBOL(mipi_dsi_dual_dcs_write_buffer_multi);

/**
 * mipi_dsi_dcs_write() - 发送 DCS 写命令
 * @dsi: DSI 外设设备
 * @cmd: DCS 命令码
 * @data: 包含命令负载的缓冲区
 * @len: 命令负载长度
 *
 * 发送 DCS 写命令，自动将命令码和负载合并到一个缓冲区中传输。
 * 该函数会根据总长度（1字节命令码 + len字节负载）自动选择
 * 合适的数据类型。对于小负载（总长度 <= 8），使用栈上缓冲区
 * 避免堆内存分配。
 *
 * 返回：成功返回传输的字节数，失败返回负错误码。
 */
ssize_t mipi_dsi_dcs_write(struct mipi_dsi_device *dsi, u8 cmd,
			   const void *data, size_t len)
{
	ssize_t err;
	size_t size;
	u8 stack_tx[8];
	u8 *tx;

	size = 1 + len;
	if (len > ARRAY_SIZE(stack_tx) - 1) {
		tx = kmalloc(size, GFP_KERNEL);
		if (!tx)
			return -ENOMEM;
	} else {
		tx = stack_tx;
	}

	/* concatenate the DCS command byte and the payload */
	tx[0] = cmd;
	if (data)
		memcpy(&tx[1], data, len);

	err = mipi_dsi_dcs_write_buffer(dsi, tx, size);

	if (tx != stack_tx)
		kfree(tx);

	return err;
}
EXPORT_SYMBOL(mipi_dsi_dcs_write);

/**
 * mipi_dsi_dcs_read() - 发送 DCS 读请求命令
 * @dsi: DSI 外设设备
 * @cmd: DCS 命令码
 * @data: 用于接收数据的缓冲区
 * @len: 接收缓冲区大小
 *
 * 发送 DCS 读请求命令，从外设读取指定寄存器或状态的值。
 * 该函数使用 MIPI_DSI_DCS_READ 数据类型发送读请求。
 *
 * 返回：成功返回读取的字节数，失败返回负错误码。
 */
ssize_t mipi_dsi_dcs_read(struct mipi_dsi_device *dsi, u8 cmd, void *data,
			  size_t len)
{
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_DCS_READ,
		.tx_buf = &cmd,
		.tx_len = 1,
		.rx_buf = data,
		.rx_len = len
	};

	return mipi_dsi_device_transfer(dsi, &msg);
}
EXPORT_SYMBOL(mipi_dsi_dcs_read);

/**
 * mipi_dsi_dcs_read_multi() - mipi_dsi_dcs_read() w/ accum_err
 * @ctx: Context for multiple DSI transactions
 * @cmd: DCS command
 * @data: buffer in which to receive data
 * @len: size of receive buffer
 *
 * Like mipi_dsi_dcs_read() but deals with errors in a way that makes it
 * convenient to make several calls in a row.
 */
void mipi_dsi_dcs_read_multi(struct mipi_dsi_multi_context *ctx, u8 cmd,
			     void *data, size_t len)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_DCS_READ,
		.tx_buf = &cmd,
		.tx_len = 1,
		.rx_buf = data,
		.rx_len = len
	};
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_device_transfer(dsi, &msg);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "dcs read with command %#x failed: %d\n", cmd,
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_read_multi);

/**
 * mipi_dsi_dcs_nop() - 发送 DCS NOP（空操作）数据包
 * @dsi: DSI 外设设备
 *
 * 发送 DCS NOP（No Operation）命令。该命令不执行任何操作，
 * 通常用于保持通信链路的活跃状态或作为填充命令。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_nop_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_nop(struct mipi_dsi_device *dsi)
{
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_NOP, NULL, 0);
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_nop);

/**
 * mipi_dsi_dcs_soft_reset() - 执行显示模块的软件复位
 * @dsi: DSI 外设设备
 *
 * 发送 DCS 软件复位命令，将显示模块恢复到上电初始状态。
 * 软件复位通常比硬件复位轻量，会重置内部状态机但不会
 * 影响电源状态。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_soft_reset_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_soft_reset(struct mipi_dsi_device *dsi)
{
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SOFT_RESET, NULL, 0);
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_soft_reset);

/**
 * mipi_dsi_dcs_get_power_mode() - 查询显示模块当前的电源模式
 * @dsi: DSI 外设设备
 * @mode: 返回当前电源模式的位置
 *
 * 读取 DCS 电源模式寄存器（0x0A），获取显示模块当前的电源状态，
 * 包括睡眠模式、显示开启/关闭、部分模式等状态位。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_get_power_mode(struct mipi_dsi_device *dsi, u8 *mode)
{
	ssize_t err;

	err = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_POWER_MODE, mode,
				sizeof(*mode));
	if (err <= 0) {
		if (err == 0)
			err = -ENODATA;

		return err;
	}

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_get_power_mode);

/**
 * mipi_dsi_dcs_get_pixel_format() - 获取接口使用的 RGB 图像像素格式
 * @dsi: DSI 外设设备
 * @format: 返回像素格式的位置
 *
 * 读取 DCS 像素格式寄存器（0x0C），获取显示模块当前使用的
 * RGB 像素格式（如 RGB565、RGB666、RGB888 等）。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_get_pixel_format(struct mipi_dsi_device *dsi, u8 *format)
{
	ssize_t err;

	err = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_PIXEL_FORMAT, format,
				sizeof(*format));
	if (err <= 0) {
		if (err == 0)
			err = -ENODATA;

		return err;
	}

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_get_pixel_format);

/**
 * mipi_dsi_dcs_enter_sleep_mode() - 让显示模块进入睡眠模式
 * @dsi: DSI 外设设备
 *
 * 发送 DCS 进入睡眠模式命令（0x10）。在睡眠模式下，显示模块关闭
 * 除接口通信外的所有不必要的内部功能模块，以降低功耗。
 * 从睡眠模式恢复到正常工作需要一定延迟（通常为 120ms 以上）。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_enter_sleep_mode_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_enter_sleep_mode(struct mipi_dsi_device *dsi)
{
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_ENTER_SLEEP_MODE, NULL, 0);
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_enter_sleep_mode);

/**
 * mipi_dsi_dcs_exit_sleep_mode() - 让显示模块退出睡眠模式
 * @dsi: DSI 外设设备
 *
 * 发送 DCS 退出睡眠模式命令（0x11），启用显示模块内部的所有功能
 * 模块，使显示模块恢复到正常工作状态。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_exit_sleep_mode_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_exit_sleep_mode(struct mipi_dsi_device *dsi)
{
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_EXIT_SLEEP_MODE, NULL, 0);
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_exit_sleep_mode);

/**
 * mipi_dsi_dcs_set_display_off() - 关闭显示设备的图像数据显示
 * @dsi: DSI 外设设备
 *
 * 发送 DCS 关闭显示命令（0x28），停止在显示设备上显示图像数据。
 * 显示模块进入空白状态，但帧内存中的数据得以保留。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_display_off_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_display_off(struct mipi_dsi_device *dsi)
{
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_display_off);

/**
 * mipi_dsi_dcs_set_display_on() - 开始显示设备的图像数据
 * @dsi: DSI 外设设备
 *
 * 发送 DCS 开启显示命令（0x29），开始在显示设备上显示图像数据。
 * 该命令通常在完成所有显示参数设置后调用，使显示内容可见。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_display_on_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_display_on(struct mipi_dsi_device *dsi)
{
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_display_on);

/**
 * mipi_dsi_dcs_set_column_address() - 定义主机处理器访问的帧内存列范围
 * @dsi: DSI 外设设备
 * @start: 帧内存的起始列
 * @end: 帧内存的结束列
 *
 * 发送 DCS 设置列地址命令（0x2A），定义主机处理器可以访问的帧内存
 * 的水平范围。配合设置页地址命令，可以指定一个矩形区域用于后续的
 * 内存写入/读取操作。
 *
 * 列地址以大端字节序编码为 4 个字节（起始列高8位、起始列低8位、
 * 结束列高8位、结束列低8位）。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_column_address_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_column_address(struct mipi_dsi_device *dsi, u16 start,
				    u16 end)
{
	u8 payload[4] = { start >> 8, start & 0xff, end >> 8, end & 0xff };
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_COLUMN_ADDRESS, payload,
				 sizeof(payload));
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_column_address);

/**
 * mipi_dsi_dcs_set_page_address() - 定义主机处理器访问的帧内存页范围
 * @dsi: DSI 外设设备
 * @start: 帧内存的起始页
 * @end: 帧内存的结束页
 *
 * 发送 DCS 设置页地址命令（0x2B），定义主机处理器可以访问的帧内存
 * 的垂直范围。与设置列地址命令结合使用，可以定位到帧内存中的
 * 任意矩形区域。
 *
 * 页地址以大端字节序编码为 4 个字节（起始页高8位、起始页低8位、
 * 结束页高8位、结束页低8位）。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_page_address_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_page_address(struct mipi_dsi_device *dsi, u16 start,
				  u16 end)
{
	u8 payload[4] = { start >> 8, start & 0xff, end >> 8, end & 0xff };
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_PAGE_ADDRESS, payload,
				 sizeof(payload));
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_page_address);

/**
 * mipi_dsi_dcs_set_tear_on() - 开启显示模块的 TE（撕裂效应）输出信号
 * @dsi: DSI 外设设备
 * @mode: TE 输出线模式
 *
 * 发送 DCS 开启撕裂效应命令（0x35），使能显示模块在 TE 信号线上
 * 输出撕裂效应信号。TE 信号用于指示显示模块的刷新状态，帮助主机
 * 同步帧缓冲区更新，避免出现画面撕裂。
 *
 * TE 模式有两种：
 *   - 模式 0：每帧在垂直空白期输出单个脉冲
 *   - 模式 1：每帧在指定的扫描线位置输出信号
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_tear_on_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_tear_on(struct mipi_dsi_device *dsi,
			     enum mipi_dsi_dcs_tear_mode mode)
{
	u8 value = mode;
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_TEAR_ON, &value,
				 sizeof(value));
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_tear_on);

/**
 * mipi_dsi_dcs_set_pixel_format() - 设置接口使用的 RGB 图像像素格式
 * @dsi: DSI 外设设备
 * @format: 像素格式值
 *
 * 发送 DCS 设置像素格式命令（0x3A），配置显示模块使用的 RGB
 * 像素格式，如 RGB565（16位）、RGB666（18位）、RGB888（24位）等。
 * 格式值定义在 MIPI DCS 标准规范中。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_pixel_format_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_pixel_format(struct mipi_dsi_device *dsi, u8 format)
{
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_PIXEL_FORMAT, &format,
				 sizeof(format));
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_pixel_format);

/**
 * mipi_dsi_dcs_set_tear_scanline() - 设置 TE 信号的触发扫描线
 * @dsi: DSI 外设设备
 * @scanline: 用作触发信号的扫描线
 *
 * 发送 DCS 设置撕裂效应扫描线命令（0x44），当 TE 模式设置为模式1时，
 * 此命令指定在哪个扫描线位置触发 TE 信号。扫描线值以大端字节序
 * 编码。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_tear_scanline_multi() 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_tear_scanline(struct mipi_dsi_device *dsi, u16 scanline)
{
	u8 payload[2] = { scanline >> 8, scanline & 0xff };
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_TEAR_SCANLINE, payload,
				 sizeof(payload));
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_tear_scanline);

/**
 * mipi_dsi_dcs_set_display_brightness() - 设置显示器的亮度值
 * @dsi: DSI 外设设备
 * @brightness: 亮度值
 *
 * 发送 DCS 设置显示亮度命令（0x51），配置显示模块的背光亮度。
 * 亮度值以小端字节序编码（低字节在前，高字节在后）。
 *
 * 此函数已被弃用，请使用 mipi_dsi_dcs_set_display_brightness_multi()
 * 替代。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_display_brightness(struct mipi_dsi_device *dsi,
					u16 brightness)
{
	u8 payload[2] = { brightness & 0xff, brightness >> 8 };
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 payload, sizeof(payload));
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_display_brightness);

/**
 * mipi_dsi_dcs_get_display_brightness() - 获取显示器的当前亮度值
 * @dsi: DSI 外设设备
 * @brightness: 返回亮度值的位置
 *
 * 读取 DCS 显示亮度寄存器（0x52），获取显示器当前的背光亮度值。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_get_display_brightness(struct mipi_dsi_device *dsi,
					u16 *brightness)
{
	ssize_t err;

	err = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
				brightness, sizeof(*brightness));
	if (err <= 0) {
		if (err == 0)
			err = -ENODATA;

		return err;
	}

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_get_display_brightness);

/**
 * mipi_dsi_dcs_set_display_brightness_large() - 设置显示器的 16 位亮度值
 * @dsi: DSI 外设设备
 * @brightness: 亮度值（大端字节序）
 *
 * 发送 DCS 设置显示亮度命令（0x51），以大端字节序（高字节在前，
 * 低字节在后）配置 16 位显示亮度值。与 mipi_dsi_dcs_set_display_brightness()
 * 的区别在于字节序不同。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_set_display_brightness_large(struct mipi_dsi_device *dsi,
					     u16 brightness)
{
	u8 payload[2] = { brightness >> 8, brightness & 0xff };
	ssize_t err;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 payload, sizeof(payload));
	if (err < 0)
		return err;

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_display_brightness_large);

/**
 * mipi_dsi_dcs_get_display_brightness_large() - 获取显示器的当前 16 位亮度值
 * @dsi: DSI 外设设备
 * @brightness: 返回亮度值的位置
 *
 * 读取 DCS 显示亮度寄存器（0x52），以大端字节序解析并返回 16 位
 * 显示亮度值。与 mipi_dsi_dcs_get_display_brightness() 的区别在于
 * 字节序处理方式不同。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_dcs_get_display_brightness_large(struct mipi_dsi_device *dsi,
					     u16 *brightness)
{
	u8 brightness_be[2];
	ssize_t err;

	err = mipi_dsi_dcs_read(dsi, MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
				brightness_be, sizeof(brightness_be));
	if (err <= 0) {
		if (err == 0)
			err = -ENODATA;

		return err;
	}

	*brightness = (brightness_be[0] << 8) | brightness_be[1];

	return 0;
}
EXPORT_SYMBOL(mipi_dsi_dcs_get_display_brightness_large);

/**
 * mipi_dsi_picture_parameter_set_multi() - transmit the DSC PPS to the peripheral
 * @ctx: Context for multiple DSI transactions
 * @pps: VESA DSC 1.1 Picture Parameter Set
 *
 * Like mipi_dsi_picture_parameter_set() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_picture_parameter_set_multi(struct mipi_dsi_multi_context *ctx,
				   const struct drm_dsc_picture_parameter_set *pps)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_picture_parameter_set(dsi, pps);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending PPS failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_picture_parameter_set_multi);

/**
 * mipi_dsi_compression_mode_ext_multi() - enable/disable DSC on the peripheral
 * @ctx: Context for multiple DSI transactions
 * @enable: Whether to enable or disable the DSC
 * @algo: Selected compression algorithm
 * @pps_selector: Select PPS from the table of pre-stored or uploaded PPS entries
 *
 * Like mipi_dsi_compression_mode_ext() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_compression_mode_ext_multi(struct mipi_dsi_multi_context *ctx,
					 bool enable,
					 enum mipi_dsi_compression_algo algo,
					 unsigned int pps_selector)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_compression_mode_ext(dsi, enable, algo, pps_selector);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending COMPRESSION_MODE failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_compression_mode_ext_multi);

/**
 * mipi_dsi_compression_mode_multi() - enable/disable DSC on the peripheral
 * @ctx: Context for multiple DSI transactions
 * @enable: Whether to enable or disable the DSC
 *
 * Enable or disable Display Stream Compression on the peripheral using the
 * default Picture Parameter Set and VESA DSC 1.1 algorithm.
 */
void mipi_dsi_compression_mode_multi(struct mipi_dsi_multi_context *ctx,
				     bool enable)
{
	return mipi_dsi_compression_mode_ext_multi(ctx, enable,
						   MIPI_DSI_COMPRESSION_DSC, 0);
}
EXPORT_SYMBOL(mipi_dsi_compression_mode_multi);

/**
 * mipi_dsi_dcs_nop_multi() - send DCS NOP packet
 * @ctx: Context for multiple DSI transactions
 *
 * Like mipi_dsi_dcs_nop() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_nop_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_nop(dsi);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending DCS NOP failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_nop_multi);

/**
 * mipi_dsi_dcs_enter_sleep_mode_multi() - send DCS ENTER_SLEEP_MODE  packet
 * @ctx: Context for multiple DSI transactions
 *
 * Like mipi_dsi_dcs_enter_sleep_mode() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_enter_sleep_mode_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending DCS ENTER_SLEEP_MODE failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_enter_sleep_mode_multi);

/**
 * mipi_dsi_dcs_exit_sleep_mode_multi() - send DCS EXIT_SLEEP_MODE packet
 * @ctx: Context for multiple DSI transactions
 *
 * Like mipi_dsi_dcs_exit_sleep_mode() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_exit_sleep_mode_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending DCS EXIT_SLEEP_MODE failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_exit_sleep_mode_multi);

/**
 * mipi_dsi_dcs_set_display_off_multi() - send DCS SET_DISPLAY_OFF packet
 * @ctx: Context for multiple DSI transactions
 *
 * Like mipi_dsi_dcs_set_display_off() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_display_off_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending DCS SET_DISPLAY_OFF failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_display_off_multi);

/**
 * mipi_dsi_dcs_set_display_on_multi() - send DCS SET_DISPLAY_ON packet
 * @ctx: Context for multiple DSI transactions
 *
 * Like mipi_dsi_dcs_set_display_on() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_display_on_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending DCS SET_DISPLAY_ON failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_display_on_multi);

/**
 * mipi_dsi_dcs_set_tear_on_multi() - send DCS SET_TEAR_ON packet
 * @ctx: Context for multiple DSI transactions
 * @mode: the Tearing Effect Output Line mode
 *
 * Like mipi_dsi_dcs_set_tear_on() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_tear_on_multi(struct mipi_dsi_multi_context *ctx,
				    enum mipi_dsi_dcs_tear_mode mode)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_tear_on(dsi, mode);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "sending DCS SET_TEAR_ON failed: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_tear_on_multi);

/**
 * mipi_dsi_turn_on_peripheral_multi() - sends a Turn On Peripheral command
 * @ctx: Context for multiple DSI transactions
 *
 * Like mipi_dsi_turn_on_peripheral() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_turn_on_peripheral_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_turn_on_peripheral(dsi);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "Failed to turn on peripheral: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_turn_on_peripheral_multi);

/**
 * mipi_dsi_dcs_set_tear_off_multi() - turn off the display module's Tearing Effect
 *    output signal on the TE signal line
 * @ctx: Context for multiple DSI transactions
 */
void mipi_dsi_dcs_set_tear_off_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	ssize_t err;

	if (ctx->accum_err)
		return;

	err = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_TEAR_OFF, NULL, 0);
	if (err < 0) {
		ctx->accum_err = err;
		dev_err(dev, "Failed to set tear off: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_tear_off_multi);

/**
 * mipi_dsi_dcs_soft_reset_multi() - perform a software reset of the display module
 * @ctx: Context for multiple DSI transactions
 *
 * Like mipi_dsi_dcs_soft_reset() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_soft_reset_multi(struct mipi_dsi_multi_context *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_soft_reset(dsi);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "Failed to mipi_dsi_dcs_soft_reset: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_soft_reset_multi);

/**
 * mipi_dsi_dcs_set_display_brightness_multi() - sets the brightness value of
 *	the display
 * @ctx: Context for multiple DSI transactions
 * @brightness: brightness value
 *
 * Like mipi_dsi_dcs_set_display_brightness() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_display_brightness_multi(struct mipi_dsi_multi_context *ctx,
					       u16 brightness)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_display_brightness(dsi, brightness);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "Failed to write display brightness: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_display_brightness_multi);

/**
 * mipi_dsi_dcs_set_pixel_format_multi() - sets the pixel format for the RGB image
 *	data used by the interface
 * @ctx: Context for multiple DSI transactions
 * @format: pixel format
 *
 * Like mipi_dsi_dcs_set_pixel_format() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_pixel_format_multi(struct mipi_dsi_multi_context *ctx,
					 u8 format)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_pixel_format(dsi, format);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "Failed to set pixel format: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_pixel_format_multi);

/**
 * mipi_dsi_dcs_set_column_address_multi() - define the column extent of the
 *	frame memory accessed by the host processor
 * @ctx: Context for multiple DSI transactions
 * @start: first column of frame memory
 * @end: last column of frame memory
 *
 * Like mipi_dsi_dcs_set_column_address() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_column_address_multi(struct mipi_dsi_multi_context *ctx,
					   u16 start, u16 end)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_column_address(dsi, start, end);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "Failed to set column address: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_column_address_multi);

/**
 * mipi_dsi_dcs_set_page_address_multi() - define the page extent of the
 *	frame memory accessed by the host processor
 * @ctx: Context for multiple DSI transactions
 * @start: first page of frame memory
 * @end: last page of frame memory
 *
 * Like mipi_dsi_dcs_set_page_address() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_page_address_multi(struct mipi_dsi_multi_context *ctx,
					 u16 start, u16 end)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_page_address(dsi, start, end);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "Failed to set page address: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_page_address_multi);

/**
 * mipi_dsi_dcs_set_tear_scanline_multi() - set the scanline to use as trigger for
 *    the Tearing Effect output signal of the display module
 * @ctx: Context for multiple DSI transactions
 * @scanline: scanline to use as trigger
 *
 * Like mipi_dsi_dcs_set_tear_scanline() but deals with errors in a way that
 * makes it convenient to make several calls in a row.
 */
void mipi_dsi_dcs_set_tear_scanline_multi(struct mipi_dsi_multi_context *ctx,
					  u16 scanline)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	if (ctx->accum_err)
		return;

	ret = mipi_dsi_dcs_set_tear_scanline(dsi, scanline);
	if (ret < 0) {
		ctx->accum_err = ret;
		dev_err(dev, "Failed to set tear scanline: %d\n",
			ctx->accum_err);
	}
}
EXPORT_SYMBOL(mipi_dsi_dcs_set_tear_scanline_multi);

static int mipi_dsi_drv_probe(struct device *dev)
{
	struct mipi_dsi_driver *drv = to_mipi_dsi_driver(dev->driver);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);

	return drv->probe(dsi);
}

static int mipi_dsi_drv_remove(struct device *dev)
{
	struct mipi_dsi_driver *drv = to_mipi_dsi_driver(dev->driver);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);

	drv->remove(dsi);

	return 0;
}

static void mipi_dsi_drv_shutdown(struct device *dev)
{
	struct mipi_dsi_driver *drv = to_mipi_dsi_driver(dev->driver);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);

	drv->shutdown(dsi);
}

/**
 * mipi_dsi_driver_register_full() - 注册 DSI 设备的驱动程序
 * @drv: DSI 驱动结构体
 * @owner: 所属模块
 *
 * 注册一个 MIPI DSI 设备驱动程序。该函数会将驱动绑定到 DSI 总线
 * 类型，并设置 probe、remove 和 shutdown 回调函数（如果提供）。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
int mipi_dsi_driver_register_full(struct mipi_dsi_driver *drv,
				  struct module *owner)
{
	drv->driver.bus = &mipi_dsi_bus_type;
	drv->driver.owner = owner;

	if (drv->probe)
		drv->driver.probe = mipi_dsi_drv_probe;
	if (drv->remove)
		drv->driver.remove = mipi_dsi_drv_remove;
	if (drv->shutdown)
		drv->driver.shutdown = mipi_dsi_drv_shutdown;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(mipi_dsi_driver_register_full);

/**
 * mipi_dsi_driver_unregister() - 注销 DSI 设备的驱动程序
 * @drv: DSI 驱动结构体
 *
 * 注销一个之前注册的 MIPI DSI 设备驱动程序。
 *
 * 返回：成功返回 0，失败返回负错误码。
 */
void mipi_dsi_driver_unregister(struct mipi_dsi_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(mipi_dsi_driver_unregister);

static int __init mipi_dsi_bus_init(void)
{
	return bus_register(&mipi_dsi_bus_type);
}
postcore_initcall(mipi_dsi_bus_init);

MODULE_AUTHOR("Andrzej Hajda <a.hajda@samsung.com>");
MODULE_DESCRIPTION("MIPI DSI Bus");
MODULE_LICENSE("GPL and additional rights");
