// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIPI Display Bus Interface (DBI) LCD controller support
 *
 * Copyright 2016 Noralf Trønnes
 */

/*
 * MIPI DBI（Display Bus Interface）LCD 控制器支持
 *
 * 本文件实现了 MIPI DBI 兼容显示控制器的通用辅助库。MIPI DBI 是
 * MIPI 联盟定义的一种显示总线接口标准，广泛用于小型 LCD 显示屏。
 *
 * MIPI DBI 有三种实现类型：
 *   A. Motorola 6800 类型并行总线
 *   B. Intel 8080 类型并行总线
 *   C. SPI 类型，有三种子选项：
 *      1. 9位模式，Data/Command 信号作为第9位
 *      2. 与上述相同，但以16位发送
 *      3. 8位模式，Data/Command 信号使用独立的 D/CX 引脚
 *
 * 当前只支持 MIPI Type 1 显示（需要完整的帧内存）和 Type C 的
 * 选项1和选项3（通过 mipi_dbi_spi_init() 初始化）。
 *
 * 符合 MIPI 标准的控制器通常使用寄存器 0x2A 和 0x2B 设置更新区域，
 * 使用寄存器 0x2C 写入帧内存。
 *
 * 本库提供的主要功能包括：
 *   - DCS 命令读写（mipi_dbi_command_read/buf）
 *   - 帧缓冲区复制和格式转换（mipi_dbi_buf_copy）
 *   - 显示初始化、电源管理和重置
 *   - SPI 传输封装
 *   - CRTC/平面/连接器的辅助函数
 *   - 调试接口
 */

#include <linux/backlight.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <drm/drm_atomic.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modes.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>
#include <video/mipi_display.h>

#define MIPI_DBI_MAX_SPI_READ_SPEED 2000000 /* 2MHz */

#define DCS_POWER_MODE_DISPLAY			BIT(2)
#define DCS_POWER_MODE_DISPLAY_NORMAL_MODE	BIT(3)
#define DCS_POWER_MODE_SLEEP_MODE		BIT(4)
#define DCS_POWER_MODE_PARTIAL_MODE		BIT(5)
#define DCS_POWER_MODE_IDLE_MODE		BIT(6)
#define DCS_POWER_MODE_RESERVED_MASK		(BIT(0) | BIT(1) | BIT(7))

/**
 * DOC: overview
 *
 * This library provides helpers for MIPI Display Bus Interface (DBI)
 * compatible display controllers.
 *
 * Many controllers for tiny lcd displays are MIPI compliant and can use this
 * library. If a controller uses registers 0x2A and 0x2B to set the area to
 * update and uses register 0x2C to write to frame memory, it is most likely
 * MIPI compliant.
 *
 * Only MIPI Type 1 displays are supported since a full frame memory is needed.
 *
 * There are 3 MIPI DBI implementation types:
 *
 * A. Motorola 6800 type parallel bus
 *
 * B. Intel 8080 type parallel bus
 *
 * C. SPI type with 3 options:
 *
 *    1. 9-bit with the Data/Command signal as the ninth bit
 *    2. Same as above except it's sent as 16 bits
 *    3. 8-bit with the Data/Command signal as a separate D/CX pin
 *
 * Currently mipi_dbi only supports Type C options 1 and 3 with
 * mipi_dbi_spi_init().
 */

#define MIPI_DBI_DEBUG_COMMAND(cmd, data, len) \
({ \
	if (!len) \
		DRM_DEBUG_DRIVER("cmd=%02x\n", cmd); \
	else if (len <= 32) \
		DRM_DEBUG_DRIVER("cmd=%02x, par=%*ph\n", cmd, (int)len, data);\
	else \
		DRM_DEBUG_DRIVER("cmd=%02x, len=%zu\n", cmd, len); \
})

static const u8 mipi_dbi_dcs_read_commands[] = {
	MIPI_DCS_GET_DISPLAY_ID,
	MIPI_DCS_GET_RED_CHANNEL,
	MIPI_DCS_GET_GREEN_CHANNEL,
	MIPI_DCS_GET_BLUE_CHANNEL,
	MIPI_DCS_GET_DISPLAY_STATUS,
	MIPI_DCS_GET_POWER_MODE,
	MIPI_DCS_GET_ADDRESS_MODE,
	MIPI_DCS_GET_PIXEL_FORMAT,
	MIPI_DCS_GET_DISPLAY_MODE,
	MIPI_DCS_GET_SIGNAL_MODE,
	MIPI_DCS_GET_DIAGNOSTIC_RESULT,
	MIPI_DCS_READ_MEMORY_START,
	MIPI_DCS_READ_MEMORY_CONTINUE,
	MIPI_DCS_GET_SCANLINE,
	MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
	MIPI_DCS_GET_CONTROL_DISPLAY,
	MIPI_DCS_GET_POWER_SAVE,
	MIPI_DCS_GET_CABC_MIN_BRIGHTNESS,
	MIPI_DCS_READ_DDB_START,
	MIPI_DCS_READ_DDB_CONTINUE,
	0, /* sentinel */
};

static bool mipi_dbi_command_is_read(struct mipi_dbi *dbi, u8 cmd)
{
	unsigned int i;

	if (!dbi->read_commands)
		return false;

	for (i = 0; i < 0xff; i++) {
		if (!dbi->read_commands[i])
			return false;
		if (cmd == dbi->read_commands[i])
			return true;
	}

	return false;
}

/**
 * mipi_dbi_command_read - MIPI DCS 读取命令
 * @dbi: MIPI DBI 结构体
 * @cmd: 命令码
 * @val: 读取到的值
 *
 * 向控制器发送 MIPI DCS 读取命令。在执行读取之前，会验证该命令
 * 是否在控制器的可读命令列表中，以防止对不支持读取的控制器或
 * 命令执行读取操作。
 *
 * 返回：
 * 成功返回零，失败返回负错误码。
 */
int mipi_dbi_command_read(struct mipi_dbi *dbi, u8 cmd, u8 *val)
{
	if (!dbi->read_commands)
		return -EACCES;

	if (!mipi_dbi_command_is_read(dbi, cmd))
		return -EINVAL;

	return mipi_dbi_command_buf(dbi, cmd, val, 1);
}
EXPORT_SYMBOL(mipi_dbi_command_read);

/**
 * mipi_dbi_command_buf - 带数组参数的 MIPI DCS 命令
 * @dbi: MIPI DBI 结构体
 * @cmd: 命令码
 * @data: 参数缓冲区
 * @len: 缓冲区长度
 *
 * 向控制器发送 MIPI DCS 命令及其参数。命令码和参数字节会通过
 * dbi->command() 回调发送。注意，SPI 传输需要 DMA 安全的缓冲区，
 * 因此该函数会复制命令码到独立的内存中。
 *
 * 返回：
 * 成功返回零，失败返回负错误码。
 */
int mipi_dbi_command_buf(struct mipi_dbi *dbi, u8 cmd, u8 *data, size_t len)
{
	u8 *cmdbuf;
	int ret;

	/* SPI requires dma-safe buffers */
	cmdbuf = kmemdup(&cmd, 1, GFP_KERNEL);
	if (!cmdbuf)
		return -ENOMEM;

	mutex_lock(&dbi->cmdlock);
	ret = dbi->command(dbi, cmdbuf, data, len);
	mutex_unlock(&dbi->cmdlock);

	kfree(cmdbuf);

	return ret;
}
EXPORT_SYMBOL(mipi_dbi_command_buf);

/* This should only be used by mipi_dbi_command() */
int mipi_dbi_command_stackbuf(struct mipi_dbi *dbi, u8 cmd, const u8 *data,
			      size_t len)
{
	u8 *buf;
	int ret;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = mipi_dbi_command_buf(dbi, cmd, buf, len);

	kfree(buf);

	return ret;
}
EXPORT_SYMBOL(mipi_dbi_command_stackbuf);

/**
 * mipi_dbi_buf_copy - 复制帧缓冲区，必要时进行格式转换
 * @dst: 目标缓冲区
 * @src: 源缓冲区
 * @fb: 源帧缓冲区
 * @clip: 要复制区域的裁剪矩形
 * @swap: 为 true 时交换 16 位值的高位字节和低位字节
 * @fmtcnv_state: 格式转换状态
 *
 * 将帧缓冲区内容复制到目标缓冲区，并根据目标显示控制器的格式
 * 需求进行必要的颜色格式转换。支持的转换包括：
 *   - RGB565 -> RGB565（可选字节交换）
 *   - RGB888 -> RGB888（直接复制）
 *   - XRGB8888 -> RGB565/RGB888（格式降位转换）
 *
 * 返回：
 * 成功返回零，失败返回负错误码。
 */
int mipi_dbi_buf_copy(void *dst, struct iosys_map *src, struct drm_framebuffer *fb,
		      struct drm_rect *clip, bool swap,
		      struct drm_format_conv_state *fmtcnv_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(fb->dev);
	struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
	struct iosys_map dst_map = IOSYS_MAP_INIT_VADDR(dst);
	int ret;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return ret;

	switch (fb->format->format) {
	case DRM_FORMAT_RGB565:
		if (swap)
			drm_fb_swab(&dst_map, NULL, src, fb, clip, !drm_gem_is_imported(gem),
				    fmtcnv_state);
		else
			drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
		break;
	case DRM_FORMAT_RGB888:
		drm_fb_memcpy(&dst_map, NULL, src, fb, clip);
		break;
	case DRM_FORMAT_XRGB8888:
		switch (dbidev->pixel_format) {
		case DRM_FORMAT_RGB565:
			if (swap) {
				drm_fb_xrgb8888_to_rgb565be(&dst_map, NULL, src, fb, clip,
							    fmtcnv_state);
			} else {
				drm_fb_xrgb8888_to_rgb565(&dst_map, NULL, src, fb, clip,
							  fmtcnv_state);
			}
			break;
		case DRM_FORMAT_RGB888:
			drm_fb_xrgb8888_to_rgb888(&dst_map, NULL, src, fb, clip, fmtcnv_state);
			break;
		}
		break;
	default:
		drm_err_once(fb->dev, "Format is not supported: %p4cc\n",
			     &fb->format->format);
		ret = -EINVAL;
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

	return ret;
}
EXPORT_SYMBOL(mipi_dbi_buf_copy);

static void mipi_dbi_set_window_address(struct mipi_dbi_dev *dbidev,
					unsigned int xs, unsigned int xe,
					unsigned int ys, unsigned int ye)
{
	struct mipi_dbi *dbi = &dbidev->dbi;

	xs += dbidev->left_offset;
	xe += dbidev->left_offset;
	ys += dbidev->top_offset;
	ye += dbidev->top_offset;

	mipi_dbi_command(dbi, MIPI_DCS_SET_COLUMN_ADDRESS, (xs >> 8) & 0xff,
			 xs & 0xff, (xe >> 8) & 0xff, xe & 0xff);
	mipi_dbi_command(dbi, MIPI_DCS_SET_PAGE_ADDRESS, (ys >> 8) & 0xff,
			 ys & 0xff, (ye >> 8) & 0xff, ye & 0xff);
}

static void mipi_dbi_fb_dirty(struct iosys_map *src, struct drm_framebuffer *fb,
			      struct drm_rect *rect, struct drm_format_conv_state *fmtcnv_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(fb->dev);
	unsigned int height = rect->y2 - rect->y1;
	unsigned int width = rect->x2 - rect->x1;
	const struct drm_format_info *dst_format;
	struct mipi_dbi *dbi = &dbidev->dbi;
	bool swap = dbi->swap_bytes;
	int ret = 0;
	size_t len;
	bool full;
	void *tr;

	full = width == fb->width && height == fb->height;

	DRM_DEBUG_KMS("Flushing [FB:%d] " DRM_RECT_FMT "\n", fb->base.id, DRM_RECT_ARG(rect));

	if (!dbi->dc || !full || swap ||
	    fb->format->format == DRM_FORMAT_XRGB8888) {
		tr = dbidev->tx_buf;
		ret = mipi_dbi_buf_copy(tr, src, fb, rect, swap, fmtcnv_state);
		if (ret)
			goto err_msg;
	} else {
		tr = src->vaddr; /* TODO: Use mapping abstraction properly */
	}

	mipi_dbi_set_window_address(dbidev, rect->x1, rect->x2 - 1, rect->y1,
				    rect->y2 - 1);

	if (fb->format->format == DRM_FORMAT_XRGB8888)
		dst_format = drm_format_info(dbidev->pixel_format);
	else
		dst_format = fb->format;
	len = drm_format_info_min_pitch(dst_format, 0, width) * height;

	ret = mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START, tr, len);
err_msg:
	if (ret)
		drm_err_once(fb->dev, "Failed to update display %d\n", ret);
}

/**
 * drm_mipi_dbi_crtc_helper_mode_valid - MIPI DBI 模式验证辅助函数
 * @crtc: CRTC 对象
 * @mode: 要测试的显示模式
 *
 * 验证给定的显示模式是否与 MIPI DBI 的硬件显示器兼容。
 * 驱动可以使用此函数作为 struct &drm_crtc_helper_funcs.mode_valid
 * 回调函数。
 */
enum drm_mode_status drm_mipi_dbi_crtc_helper_mode_valid(struct drm_crtc *crtc,
							 const struct drm_display_mode *mode)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &dbidev->mode);
}
EXPORT_SYMBOL(drm_mipi_dbi_crtc_helper_mode_valid);

/**
 * drm_mipi_dbi_plane_helper_atomic_check - MIPI DBI 平面检查辅助函数
 * @plane: 要检查的平面
 * @state: 原子状态
 *
 * 对 MIPI DBI 设备的主平面执行默认检查。该函数会验证平面状态
 * 是否满足要求（如无缩放、无旋转等）。驱动可以使用此函数作为
 * struct &drm_crtc_helper_funcs.atomic_check 回调函数。
 *
 * 返回：
 * 成功返回 0，否则返回负 errno 码。
 */
int drm_mipi_dbi_plane_helper_atomic_check(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *new_crtc_state = NULL;
	int ret;

	if (new_plane_state->crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_plane_state->crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret)
		return ret;
	else if (!new_plane_state->visible)
		return 0;

	return 0;
}
EXPORT_SYMBOL(drm_mipi_dbi_plane_helper_atomic_check);

/**
 * drm_mipi_dbi_plane_helper_atomic_update - 显示更新辅助函数
 * @plane: 平面
 * @state: 原子状态
 *
 * 处理帧缓冲区的刷新和 vblank 事件。该函数会计算新老状态的损伤
 * 区域（damage），并将变化的部分刷新到显示控制器。驱动可以使用
 * 此函数作为 struct &drm_plane_helper_funcs.atomic_update 回调。
 */
void drm_mipi_dbi_plane_helper_atomic_update(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct drm_plane_state *plane_state = plane->state;
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_rect rect;
	int idx;

	if (!fb)
		return;

	if (drm_dev_enter(plane->dev, &idx)) {
		if (drm_atomic_helper_damage_merged(old_plane_state, plane_state, &rect))
			mipi_dbi_fb_dirty(&shadow_plane_state->data[0], fb, &rect,
					  &shadow_plane_state->fmtcnv_state);
		drm_dev_exit(idx);
	}
}
EXPORT_SYMBOL(drm_mipi_dbi_plane_helper_atomic_update);

static void mipi_dbi_blank(struct mipi_dbi_dev *dbidev)
{
	struct drm_device *drm = &dbidev->drm;
	u16 height = drm->mode_config.min_height;
	u16 width = drm->mode_config.min_width;
	struct mipi_dbi *dbi = &dbidev->dbi;
	const struct drm_format_info *dst_format;
	size_t len;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;

	dst_format = drm_format_info(dbidev->pixel_format);
	len = drm_format_info_min_pitch(dst_format, 0, width) * height;

	memset(dbidev->tx_buf, 0, len);

	mipi_dbi_set_window_address(dbidev, 0, width - 1, 0, height - 1);
	mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START,
			     (u8 *)dbidev->tx_buf, len);

	drm_dev_exit(idx);
}

/**
 * drm_mipi_dbi_crtc_helper_atomic_check - MIPI DBI CRTC 检查辅助函数
 * @crtc: 要检查的 CRTC
 * @state: 原子状态
 *
 * 对 MIPI DBI 设备的 CRTC 执行默认检查，确保主平面已正确设置。
 * 驱动可以使用此函数作为 struct &drm_crtc_helper_funcs.atomic_check
 * 回调函数。
 *
 * 返回：
 * 成功返回 0，否则返回负 errno 码。
 */
int drm_mipi_dbi_crtc_helper_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (!crtc_state->enable)
		goto out;

	ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
	if (ret)
		return ret;

out:
	return drm_atomic_add_affected_planes(state, crtc);
}
EXPORT_SYMBOL(drm_mipi_dbi_crtc_helper_atomic_check);

/**
 * drm_mipi_dbi_crtc_helper_atomic_disable - MIPI DBI CRTC 禁用辅助函数
 * @crtc: 要禁用的 CRTC
 * @state: 原子状态
 *
 * 禁用 MIPI DBI 设备的显示输出。该函数会：
 *   1. 如果有背光，关闭背光；否则清空显示内存（黑屏）
 *   2. 如果使用稳压器，关闭稳压器电源
 *   3. 如果使用 I/O 稳压器，关闭 I/O 稳压器电源
 *
 * 驱动可以使用此函数作为 struct &drm_crtc_helper_funcs.atomic_disable
 * 回调函数。
 */
void drm_mipi_dbi_crtc_helper_atomic_disable(struct drm_crtc *crtc,
					     struct drm_atomic_state *state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(crtc->dev);

	if (dbidev->backlight)
		backlight_disable(dbidev->backlight);
	else
		mipi_dbi_blank(dbidev);

	if (dbidev->regulator)
		regulator_disable(dbidev->regulator);
	if (dbidev->io_regulator)
		regulator_disable(dbidev->io_regulator);
}
EXPORT_SYMBOL(drm_mipi_dbi_crtc_helper_atomic_disable);

/**
 * drm_mipi_dbi_connector_helper_get_modes - 复制 MIPI DBI 模式到连接器
 * @connector: 连接器
 *
 * 将 MIPI DBI 的固定显示模式复制到连接器的模式列表中。由于 MIPI DBI
 * 设备通常使用固定分辨率的显示屏，该函数提供了一种简单的方式来设置
 * 连接器模式。驱动可以使用此函数作为 &drm_connector_helper_funcs->get_modes
 * 回调函数。
 *
 * 返回：
 * 创建的模式数量。
 */
int drm_mipi_dbi_connector_helper_get_modes(struct drm_connector *connector)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &dbidev->mode);
}
EXPORT_SYMBOL(drm_mipi_dbi_connector_helper_get_modes);

static int mipi_dbi_rotate_mode(struct drm_display_mode *mode,
				unsigned int rotation)
{
	if (rotation == 0 || rotation == 180) {
		return 0;
	} else if (rotation == 90 || rotation == 270) {
		swap(mode->hdisplay, mode->vdisplay);
		swap(mode->hsync_start, mode->vsync_start);
		swap(mode->hsync_end, mode->vsync_end);
		swap(mode->htotal, mode->vtotal);
		swap(mode->width_mm, mode->height_mm);
		return 0;
	} else {
		return -EINVAL;
	}
}

/**
 * drm_mipi_dbi_dev_init - MIPI DBI 设备初始化
 * @dbidev: 要初始化的 MIPI DBI 设备结构体
 * @mode: 硬件显示模式
 * @format: 硬件颜色格式（DRM_FORMAT\_\*）
 * @rotation: 初始旋转角度（逆时针度数）
 * @tx_buf_size: 分配的发送缓冲区大小（至少为此值）
 *
 * 初始化 MIPI DBI 设备。该函数会：
 *   1. 验证 dbi->command 回调已设置
 *   2. 分配发送缓冲区（如果 tx_buf_size 为 0，则自动计算
 *      足以传输一整帧的大小）
 *   3. 复制并处理显示模式（根据旋转角度交换宽高）
 *   4. 保存像素格式
 *
 * @tx_buf_size 是可选的。传入 0 将分配足以传输显示一整行的内存。
 *
 * 返回：
 * 成功返回零，失败返回负错误码。
 */
int drm_mipi_dbi_dev_init(struct mipi_dbi_dev *dbidev, const struct drm_display_mode *mode,
			  u32 format, unsigned int rotation, size_t tx_buf_size)
{
	struct drm_device *drm = &dbidev->drm;
	int ret;

	if (!dbidev->dbi.command)
		return -EINVAL;

	if (!tx_buf_size) {
		const struct drm_format_info *info = drm_format_info(format);

		tx_buf_size = drm_format_info_min_pitch(info, 0, mode->hdisplay) *
			      mode->vdisplay;
	}

	dbidev->tx_buf = devm_kmalloc(drm->dev, tx_buf_size, GFP_KERNEL);
	if (!dbidev->tx_buf)
		return -ENOMEM;

	drm_mode_copy(&dbidev->mode, mode);
	ret = mipi_dbi_rotate_mode(&dbidev->mode, rotation);
	if (ret) {
		drm_err(drm, "Illegal rotation value %u\n", rotation);
		return -EINVAL;
	}

	dbidev->rotation = rotation;
	drm_dbg(drm, "rotation = %u\n", rotation);

	dbidev->pixel_format = format;
	if (dbidev->pixel_format == DRM_FORMAT_RGB888)
		dbidev->dbi.write_memory_bpw = 8;

	return 0;
}
EXPORT_SYMBOL(drm_mipi_dbi_dev_init);

/**
 * mipi_dbi_hw_reset - 控制器的硬件复位
 * @dbi: MIPI DBI 结构体
 *
 * 如果设置了 &mipi_dbi->reset GPIO，则对控制器执行硬件复位。
 * 复位时序：
 *   1. 将复位引脚拉低至少 20us
 *   2. 将复位引脚拉高
 *   3. 等待 120ms 让控制器完成启动
 */
void mipi_dbi_hw_reset(struct mipi_dbi *dbi)
{
	if (!dbi->reset)
		return;

	gpiod_set_value_cansleep(dbi->reset, 0);
	usleep_range(20, 1000);
	gpiod_set_value_cansleep(dbi->reset, 1);
	msleep(120);
}
EXPORT_SYMBOL(mipi_dbi_hw_reset);

/**
 * mipi_dbi_display_is_on - 检查显示器是否已开启
 * @dbi: MIPI DBI 结构体
 *
 * 该函数读取电源模式寄存器（如果可读）来检查显示输出是否已开启。
 * 这可以用来判断引导加载程序是否已开启了显示器，从而避免在启用
 * 显示管道时出现闪烁。
 *
 * 如果显示器已被引导加载程序初始化并开启，驱动程序可以跳过复位
 * 和初始化序列，直接接管显示输出，实现无闪烁切换。
 *
 * 返回：
 * 如果可以确认显示器已开启则返回 true，否则返回 false。
 */
bool mipi_dbi_display_is_on(struct mipi_dbi *dbi)
{
	u8 val;

	if (mipi_dbi_command_read(dbi, MIPI_DCS_GET_POWER_MODE, &val))
		return false;

	val &= ~DCS_POWER_MODE_RESERVED_MASK;

	/* The poweron/reset value is 08h DCS_POWER_MODE_DISPLAY_NORMAL_MODE */
	if (val != (DCS_POWER_MODE_DISPLAY |
	    DCS_POWER_MODE_DISPLAY_NORMAL_MODE | DCS_POWER_MODE_SLEEP_MODE))
		return false;

	DRM_DEBUG_DRIVER("Display is ON\n");

	return true;
}
EXPORT_SYMBOL(mipi_dbi_display_is_on);

static int mipi_dbi_poweron_reset_conditional(struct mipi_dbi_dev *dbidev, bool cond)
{
	struct device *dev = dbidev->drm.dev;
	struct mipi_dbi *dbi = &dbidev->dbi;
	int ret;

	if (dbidev->regulator) {
		ret = regulator_enable(dbidev->regulator);
		if (ret) {
			DRM_DEV_ERROR(dev, "Failed to enable regulator (%d)\n", ret);
			return ret;
		}
	}

	if (dbidev->io_regulator) {
		ret = regulator_enable(dbidev->io_regulator);
		if (ret) {
			DRM_DEV_ERROR(dev, "Failed to enable I/O regulator (%d)\n", ret);
			if (dbidev->regulator)
				regulator_disable(dbidev->regulator);
			return ret;
		}
	}

	if (cond && mipi_dbi_display_is_on(dbi))
		return 1;

	mipi_dbi_hw_reset(dbi);
	ret = mipi_dbi_command(dbi, MIPI_DCS_SOFT_RESET);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to send reset command (%d)\n", ret);
		if (dbidev->regulator)
			regulator_disable(dbidev->regulator);
		if (dbidev->io_regulator)
			regulator_disable(dbidev->io_regulator);
		return ret;
	}

	/*
	 * If we did a hw reset, we know the controller is in Sleep mode and
	 * per MIPI DSC spec should wait 5ms after soft reset. If we didn't,
	 * we assume worst case and wait 120ms.
	 */
	if (dbi->reset)
		usleep_range(5000, 20000);
	else
		msleep(120);

	return 0;
}

/**
 * mipi_dbi_poweron_reset - MIPI DBI 上电和复位
 * @dbidev: MIPI DBI 设备结构体
 *
 * 该函数执行完整的显示控制器上电和复位流程：
 *   1. 使能稳压器（如果使用）
 *   2. 使能 I/O 稳压器（如果使用）
 *   3. 执行硬件复位（通过 GPIO）
 *   4. 发送软件复位命令（DCS 软复位）
 *
 * 返回：
 * 成功返回零，失败返回负错误码。
 */
int mipi_dbi_poweron_reset(struct mipi_dbi_dev *dbidev)
{
	return mipi_dbi_poweron_reset_conditional(dbidev, false);
}
EXPORT_SYMBOL(mipi_dbi_poweron_reset);

/**
 * mipi_dbi_poweron_conditional_reset - MIPI DBI 上电和条件复位
 * @dbidev: MIPI DBI 设备结构体
 *
 * 该函数使能稳压器，然后根据显示器当前状态决定是否执行复位：
 *   - 如果显示器已开启（由引导加载程序初始化），则跳过复位，
 *     实现无闪烁启动
 *   - 如果显示器未开启，执行完整的硬件和软件复位
 *
 * 返回：
 * 控制器被复位时返回 0，显示器已开启时返回 1，失败时返回负错误码。
 */
int mipi_dbi_poweron_conditional_reset(struct mipi_dbi_dev *dbidev)
{
	return mipi_dbi_poweron_reset_conditional(dbidev, true);
}
EXPORT_SYMBOL(mipi_dbi_poweron_conditional_reset);

#if IS_ENABLED(CONFIG_SPI)

/**
 * mipi_dbi_spi_cmd_max_speed - 获取最大 SPI 总线速度
 * @spi: SPI 设备
 * @len: 传输缓冲区长度
 *
 * 许多控制器标称最大速度为 10MHz，但实际上可以运行在更高的速度。
 * 为了提高可靠性，像素数据以最大速度传输，而命令和配置数据以
 * 10MHz 传输，防止传输异常影响初始化设置。
 *
 * 返回：
 * 如果传输长度超过 64 字节（通常为像素数据），返回 0 表示使用
 * SPI 控制器的默认速度。否则返回 10MHz 和控制器最大速度中的
 * 较小值。
 */
u32 mipi_dbi_spi_cmd_max_speed(struct spi_device *spi, size_t len)
{
	if (len > 64)
		return 0; /* use default */

	return min_t(u32, 10000000, spi->max_speed_hz);
}
EXPORT_SYMBOL(mipi_dbi_spi_cmd_max_speed);

/*
 * MIPI DBI Type C Option 1
 *
 * If the SPI controller doesn't have 9 bits per word support,
 * use blocks of 9 bytes to send 8x 9-bit words using a 8-bit SPI transfer.
 * Pad partial blocks with MIPI_DCS_NOP (zero).
 * This is how the D/C bit (x) is added:
 *     x7654321
 *     0x765432
 *     10x76543
 *     210x7654
 *     3210x765
 *     43210x76
 *     543210x7
 *     6543210x
 *     76543210
 */

static int mipi_dbi_spi1e_transfer(struct mipi_dbi *dbi, int dc,
				   const void *buf, size_t len,
				   unsigned int bpw)
{
	bool swap_bytes = (bpw == 16);
	size_t chunk, max_chunk = dbi->tx_buf9_len;
	struct spi_device *spi = dbi->spi;
	struct spi_transfer tr = {
		.tx_buf = dbi->tx_buf9,
		.bits_per_word = 8,
	};
	struct spi_message m;
	const u8 *src = buf;
	int i, ret;
	u8 *dst;

	if (drm_debug_enabled(DRM_UT_DRIVER))
		pr_debug("[drm:%s] dc=%d, max_chunk=%zu, transfers:\n",
			 __func__, dc, max_chunk);

	tr.speed_hz = mipi_dbi_spi_cmd_max_speed(spi, len);
	spi_message_init_with_transfers(&m, &tr, 1);

	if (!dc) {
		if (WARN_ON_ONCE(len != 1))
			return -EINVAL;

		/* Command: pad no-op's (zeroes) at beginning of block */
		dst = dbi->tx_buf9;
		memset(dst, 0, 9);
		dst[8] = *src;
		tr.len = 9;

		return spi_sync(spi, &m);
	}

	/* max with room for adding one bit per byte */
	max_chunk = max_chunk / 9 * 8;
	/* but no bigger than len */
	max_chunk = min(max_chunk, len);
	/* 8 byte blocks */
	max_chunk = max_t(size_t, 8, max_chunk & ~0x7);

	while (len) {
		size_t added = 0;

		chunk = min(len, max_chunk);
		len -= chunk;
		dst = dbi->tx_buf9;

		if (chunk < 8) {
			u8 val, carry = 0;

			/* Data: pad no-op's (zeroes) at end of block */
			memset(dst, 0, 9);

			if (swap_bytes) {
				for (i = 1; i < (chunk + 1); i++) {
					val = src[1];
					*dst++ = carry | BIT(8 - i) | (val >> i);
					carry = val << (8 - i);
					i++;
					val = src[0];
					*dst++ = carry | BIT(8 - i) | (val >> i);
					carry = val << (8 - i);
					src += 2;
				}
				*dst++ = carry;
			} else {
				for (i = 1; i < (chunk + 1); i++) {
					val = *src++;
					*dst++ = carry | BIT(8 - i) | (val >> i);
					carry = val << (8 - i);
				}
				*dst++ = carry;
			}

			chunk = 8;
			added = 1;
		} else {
			for (i = 0; i < chunk; i += 8) {
				if (swap_bytes) {
					*dst++ =                 BIT(7) | (src[1] >> 1);
					*dst++ = (src[1] << 7) | BIT(6) | (src[0] >> 2);
					*dst++ = (src[0] << 6) | BIT(5) | (src[3] >> 3);
					*dst++ = (src[3] << 5) | BIT(4) | (src[2] >> 4);
					*dst++ = (src[2] << 4) | BIT(3) | (src[5] >> 5);
					*dst++ = (src[5] << 3) | BIT(2) | (src[4] >> 6);
					*dst++ = (src[4] << 2) | BIT(1) | (src[7] >> 7);
					*dst++ = (src[7] << 1) | BIT(0);
					*dst++ = src[6];
				} else {
					*dst++ =                 BIT(7) | (src[0] >> 1);
					*dst++ = (src[0] << 7) | BIT(6) | (src[1] >> 2);
					*dst++ = (src[1] << 6) | BIT(5) | (src[2] >> 3);
					*dst++ = (src[2] << 5) | BIT(4) | (src[3] >> 4);
					*dst++ = (src[3] << 4) | BIT(3) | (src[4] >> 5);
					*dst++ = (src[4] << 3) | BIT(2) | (src[5] >> 6);
					*dst++ = (src[5] << 2) | BIT(1) | (src[6] >> 7);
					*dst++ = (src[6] << 1) | BIT(0);
					*dst++ = src[7];
				}

				src += 8;
				added++;
			}
		}

		tr.len = chunk + added;

		ret = spi_sync(spi, &m);
		if (ret)
			return ret;
	}

	return 0;
}

static int mipi_dbi_spi1_transfer(struct mipi_dbi *dbi, int dc,
				  const void *buf, size_t len,
				  unsigned int bpw)
{
	struct spi_device *spi = dbi->spi;
	struct spi_transfer tr = {
		.bits_per_word = 9,
	};
	const u16 *src16 = buf;
	const u8 *src8 = buf;
	struct spi_message m;
	size_t max_chunk;
	u16 *dst16;
	int ret;

	if (!spi_is_bpw_supported(spi, 9))
		return mipi_dbi_spi1e_transfer(dbi, dc, buf, len, bpw);

	tr.speed_hz = mipi_dbi_spi_cmd_max_speed(spi, len);
	max_chunk = dbi->tx_buf9_len;
	dst16 = dbi->tx_buf9;

	if (drm_debug_enabled(DRM_UT_DRIVER))
		pr_debug("[drm:%s] dc=%d, max_chunk=%zu, transfers:\n",
			 __func__, dc, max_chunk);

	max_chunk = min(max_chunk / 2, len);

	spi_message_init_with_transfers(&m, &tr, 1);
	tr.tx_buf = dst16;

	while (len) {
		size_t chunk = min(len, max_chunk);
		unsigned int i;

		if (bpw == 16) {
			for (i = 0; i < (chunk * 2); i += 2) {
				dst16[i]     = *src16 >> 8;
				dst16[i + 1] = *src16++ & 0xFF;
				if (dc) {
					dst16[i]     |= 0x0100;
					dst16[i + 1] |= 0x0100;
				}
			}
		} else {
			for (i = 0; i < chunk; i++) {
				dst16[i] = *src8++;
				if (dc)
					dst16[i] |= 0x0100;
			}
		}

		tr.len = chunk * 2;
		len -= chunk;

		ret = spi_sync(spi, &m);
		if (ret)
			return ret;
	}

	return 0;
}

static int mipi_dbi_typec1_command_read(struct mipi_dbi *dbi, u8 *cmd,
					u8 *data, size_t len)
{
	struct spi_device *spi = dbi->spi;
	u32 speed_hz = min_t(u32, MIPI_DBI_MAX_SPI_READ_SPEED,
			     spi->max_speed_hz / 2);
	struct spi_transfer tr[2] = {
		{
			.speed_hz = speed_hz,
			.bits_per_word = 9,
			.tx_buf = dbi->tx_buf9,
			.len = 2,
		}, {
			.speed_hz = speed_hz,
			.bits_per_word = 8,
			.len = len,
			.rx_buf = data,
		},
	};
	struct spi_message m;
	u16 *dst16;
	int ret;

	if (!len)
		return -EINVAL;

	if (!spi_is_bpw_supported(spi, 9)) {
		/*
		 * FIXME: implement something like mipi_dbi_spi1e_transfer() but
		 * for reads using emulation.
		 */
		dev_err(&spi->dev,
			"reading on host not supporting 9 bpw not yet implemented\n");
		return -EOPNOTSUPP;
	}

	/*
	 * Turn the 8bit command into a 16bit version of the command in the
	 * buffer. Only 9 bits of this will be used when executing the actual
	 * transfer.
	 */
	dst16 = dbi->tx_buf9;
	dst16[0] = *cmd;

	spi_message_init_with_transfers(&m, tr, ARRAY_SIZE(tr));
	ret = spi_sync(spi, &m);

	if (!ret)
		MIPI_DBI_DEBUG_COMMAND(*cmd, data, len);

	return ret;
}

static int mipi_dbi_typec1_command(struct mipi_dbi *dbi, u8 *cmd,
				   u8 *parameters, size_t num)
{
	unsigned int bpw = 8;
	int ret;

	if (mipi_dbi_command_is_read(dbi, *cmd))
		return mipi_dbi_typec1_command_read(dbi, cmd, parameters, num);

	MIPI_DBI_DEBUG_COMMAND(*cmd, parameters, num);

	ret = mipi_dbi_spi1_transfer(dbi, 0, cmd, 1, 8);
	if (ret || !num)
		return ret;

	if (*cmd == MIPI_DCS_WRITE_MEMORY_START)
		bpw = dbi->write_memory_bpw;

	return mipi_dbi_spi1_transfer(dbi, 1, parameters, num, bpw);
}

/* MIPI DBI Type C Option 3 */

static int mipi_dbi_typec3_command_read(struct mipi_dbi *dbi, u8 *cmd,
					u8 *data, size_t len)
{
	struct spi_device *spi = dbi->spi;
	u32 speed_hz = min_t(u32, MIPI_DBI_MAX_SPI_READ_SPEED,
			     spi->max_speed_hz / 2);
	struct spi_transfer tr[2] = {
		{
			.speed_hz = speed_hz,
			.tx_buf = cmd,
			.len = 1,
		}, {
			.speed_hz = speed_hz,
			.len = len,
		},
	};
	struct spi_message m;
	u8 *buf;
	int ret;

	if (!len)
		return -EINVAL;

	/*
	 * Support non-standard 24-bit and 32-bit Nokia read commands which
	 * start with a dummy clock, so we need to read an extra byte.
	 */
	if (*cmd == MIPI_DCS_GET_DISPLAY_ID ||
	    *cmd == MIPI_DCS_GET_DISPLAY_STATUS) {
		if (!(len == 3 || len == 4))
			return -EINVAL;

		tr[1].len = len + 1;
	}

	buf = kmalloc(tr[1].len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	tr[1].rx_buf = buf;

	spi_bus_lock(spi->controller);
	gpiod_set_value_cansleep(dbi->dc, 0);

	spi_message_init_with_transfers(&m, tr, ARRAY_SIZE(tr));
	ret = spi_sync_locked(spi, &m);
	spi_bus_unlock(spi->controller);
	if (ret)
		goto err_free;

	if (tr[1].len == len) {
		memcpy(data, buf, len);
	} else {
		unsigned int i;

		for (i = 0; i < len; i++)
			data[i] = (buf[i] << 1) | (buf[i + 1] >> 7);
	}

	MIPI_DBI_DEBUG_COMMAND(*cmd, data, len);

err_free:
	kfree(buf);

	return ret;
}

static int mipi_dbi_typec3_command(struct mipi_dbi *dbi, u8 *cmd,
				   u8 *par, size_t num)
{
	struct spi_device *spi = dbi->spi;
	unsigned int bpw = 8;
	u32 speed_hz;
	int ret;

	if (mipi_dbi_command_is_read(dbi, *cmd))
		return mipi_dbi_typec3_command_read(dbi, cmd, par, num);

	MIPI_DBI_DEBUG_COMMAND(*cmd, par, num);

	spi_bus_lock(spi->controller);
	gpiod_set_value_cansleep(dbi->dc, 0);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, 1);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, 8, cmd, 1);
	spi_bus_unlock(spi->controller);
	if (ret || !num)
		return ret;

	if (*cmd == MIPI_DCS_WRITE_MEMORY_START)
		bpw = dbi->write_memory_bpw;

	spi_bus_lock(spi->controller);
	gpiod_set_value_cansleep(dbi->dc, 1);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, num);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, bpw, par, num);
	spi_bus_unlock(spi->controller);

	return ret;
}

/**
 * mipi_dbi_spi_init - 初始化 MIPI DBI SPI 接口
 * @spi: SPI 设备
 * @dbi: 要初始化的 MIPI DBI 结构体
 * @dc: D/C GPIO（可选）
 *
 * 初始化 MIPI DBI 的 SPI 接口。该函数设置 &mipi_dbi->command 回调，
 * 并启用常用读取命令的 &mipi_dbi->read_commands。之后应调用
 * mipi_dbi_dev_init() 或驱动特定的初始化函数。
 *
 * 接口类型选择：
 *   - 如果设置了 @dc GPIO，则使用 Type C Option 3（D/C 为独立引脚）
 *   - 如果未设置 @dc，则使用 Type C Option 1（D/C 作为第9位）
 *
 * 字节序处理：
 * MIPI DBI 串行接口是大端（big endian）的，而帧缓冲区在内存中
 * 存储为小端（little endian）格式（%DRM_FORMAT_BIG_ENDIAN 不支持）。
 *
 *   Option 1（D/C 作为位）：缓冲区逐字节发送，16位缓冲区在传输前
 *                         需要字节交换。
 *   Option 3（D/C 作为 GPIO）：如果 SPI 控制器支持 16 位每字，则
 *                           缓冲区可直接发送。否则调用者需要自行
 *                           在调用 mipi_dbi_command_buf() 之前交换
 *                           字节，并以 8 bpw 发送。
 *
 * 端序处理针对 %DRM_FORMAT_RGB565 帧缓冲区进行了优化。
 *
 * 如果接口为 Option 1 且 SPI 控制器不支持 9 位每字，缓冲区将以
 * 9 个 8 位字的形式发送，必要时用 MIPI DCS no-op 命令填充。
 *
 * 返回：
 * 成功返回零，失败返回负错误码。
 */
int mipi_dbi_spi_init(struct spi_device *spi, struct mipi_dbi *dbi,
		      struct gpio_desc *dc)
{
	struct device *dev = &spi->dev;
	int ret;

	/*
	 * Even though it's not the SPI device that does DMA (the master does),
	 * the dma mask is necessary for the dma_alloc_wc() in the GEM code
	 * (e.g., drm_gem_dma_create()). The dma_addr returned will be a physical
	 * address which might be different from the bus address, but this is
	 * not a problem since the address will not be used.
	 * The virtual address is used in the transfer and the SPI core
	 * re-maps it on the SPI master device using the DMA streaming API
	 * (spi_map_buf()).
	 */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	dbi->spi = spi;
	dbi->read_commands = mipi_dbi_dcs_read_commands;
	dbi->write_memory_bpw = 16;

	if (dc) {
		dbi->command = mipi_dbi_typec3_command;
		dbi->dc = dc;
		if (!spi_is_bpw_supported(spi, 16)) {
			dbi->write_memory_bpw = 8;
			dbi->swap_bytes = true;
		}
	} else {
		dbi->command = mipi_dbi_typec1_command;
		dbi->tx_buf9_len = SZ_16K;
		dbi->tx_buf9 = devm_kmalloc(dev, dbi->tx_buf9_len, GFP_KERNEL);
		if (!dbi->tx_buf9)
			return -ENOMEM;
	}

	mutex_init(&dbi->cmdlock);

	DRM_DEBUG_DRIVER("SPI speed: %uMHz\n", spi->max_speed_hz / 1000000);

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_spi_init);

/**
 * mipi_dbi_spi_transfer - SPI 传输辅助函数
 * @spi: SPI 设备
 * @speed_hz: 速度覆盖值（可选，0 表示使用默认值）
 * @bpw: 每字位数
 * @buf: 要传输的缓冲区
 * @len: 缓冲区长度
 *
 * 此 SPI 传输辅助函数将 @buf 的传输分割为 SPI 控制器驱动可以
 * 处理的块大小。调用此函数前必须先锁定 SPI 总线。
 *
 * 注意：
 *   - 根据 SPI 验证规则，传输长度必须对齐到字大小（w_size），
 *     因此最大块大小会向下对齐到 2 的倍数
 *   - 该函数通过循环逐个发送数据块，直到所有数据都传输完毕
 *
 * 返回：
 * 成功返回零，失败返回负错误码。
 */
int mipi_dbi_spi_transfer(struct spi_device *spi, u32 speed_hz,
			  u8 bpw, const void *buf, size_t len)
{
	size_t max_chunk = spi_max_transfer_size(spi);
	struct spi_transfer tr = {
		.bits_per_word = bpw,
		.speed_hz = speed_hz,
	};
	struct spi_message m;
	size_t chunk;
	int ret;

	/* In __spi_validate, there's a validation that no partial transfers
	 * are accepted (xfer->len % w_size must be zero).
	 * Here we align max_chunk to multiple of 2 (16bits),
	 * to prevent transfers from being rejected.
	 */
	max_chunk = ALIGN_DOWN(max_chunk, 2);

	spi_message_init_with_transfers(&m, &tr, 1);

	while (len) {
		chunk = min(len, max_chunk);

		tr.tx_buf = buf;
		tr.len = chunk;
		buf += chunk;
		len -= chunk;

		ret = spi_sync_locked(spi, &m);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(mipi_dbi_spi_transfer);

#endif /* CONFIG_SPI */

#ifdef CONFIG_DEBUG_FS

static ssize_t mipi_dbi_debugfs_command_write(struct file *file,
					      const char __user *ubuf,
					      size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct mipi_dbi_dev *dbidev = m->private;
	u8 val, cmd = 0, parameters[64];
	char *buf, *pos, *token;
	int i, ret, idx;

	if (!drm_dev_enter(&dbidev->drm, &idx))
		return -ENODEV;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto err_exit;
	}

	/* strip trailing whitespace */
	for (i = count - 1; i > 0; i--)
		if (isspace(buf[i]))
			buf[i] = '\0';
		else
			break;
	i = 0;
	pos = buf;
	while (pos) {
		token = strsep(&pos, " ");
		if (!token) {
			ret = -EINVAL;
			goto err_free;
		}

		ret = kstrtou8(token, 16, &val);
		if (ret < 0)
			goto err_free;

		if (token == buf)
			cmd = val;
		else
			parameters[i++] = val;

		if (i == 64) {
			ret = -E2BIG;
			goto err_free;
		}
	}

	ret = mipi_dbi_command_buf(&dbidev->dbi, cmd, parameters, i);

err_free:
	kfree(buf);
err_exit:
	drm_dev_exit(idx);

	return ret < 0 ? ret : count;
}

static int mipi_dbi_debugfs_command_show(struct seq_file *m, void *unused)
{
	struct mipi_dbi_dev *dbidev = m->private;
	struct mipi_dbi *dbi = &dbidev->dbi;
	u8 cmd, val[4];
	int ret, idx;
	size_t len;

	if (!drm_dev_enter(&dbidev->drm, &idx))
		return -ENODEV;

	for (cmd = 0; cmd < 255; cmd++) {
		if (!mipi_dbi_command_is_read(dbi, cmd))
			continue;

		switch (cmd) {
		case MIPI_DCS_READ_MEMORY_START:
		case MIPI_DCS_READ_MEMORY_CONTINUE:
			len = 2;
			break;
		case MIPI_DCS_GET_DISPLAY_ID:
			len = 3;
			break;
		case MIPI_DCS_GET_DISPLAY_STATUS:
			len = 4;
			break;
		default:
			len = 1;
			break;
		}

		seq_printf(m, "%02x: ", cmd);
		ret = mipi_dbi_command_buf(dbi, cmd, val, len);
		if (ret) {
			seq_puts(m, "XX\n");
			continue;
		}
		seq_printf(m, "%*phN\n", (int)len, val);
	}

	drm_dev_exit(idx);

	return 0;
}

static int mipi_dbi_debugfs_command_open(struct inode *inode,
					 struct file *file)
{
	return single_open(file, mipi_dbi_debugfs_command_show,
			   inode->i_private);
}

static const struct file_operations mipi_dbi_debugfs_command_fops = {
	.owner = THIS_MODULE,
	.open = mipi_dbi_debugfs_command_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = mipi_dbi_debugfs_command_write,
};

/**
 * mipi_dbi_debugfs_init - 创建 debugfs 条目
 * @minor: DRM minor
 *
 * 该函数创建一个名为 'command' 的 debugfs 文件，用于向控制器发送
 * 命令或读取命令的返回值。这个调试接口对于开发阶段的显示控制器
 * 调试非常有用，可以直接通过文件系统与硬件交互。
 *
 * 写操作：向该文件写入十六进制值可以发送 DCS 命令和参数
 * 读操作：读取该文件会遍历所有可读命令并返回它们的当前值
 *
 * 如果控制器不支持读取命令，则文件只有写权限。
 * 驱动可以使用此函数作为 &drm_driver->debugfs_init 回调。
 */
void mipi_dbi_debugfs_init(struct drm_minor *minor)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(minor->dev);
	umode_t mode = S_IFREG | S_IWUSR;

	if (dbidev->dbi.read_commands)
		mode |= S_IRUGO;
	debugfs_create_file("command", mode, minor->debugfs_root, dbidev,
			    &mipi_dbi_debugfs_command_fops);
}
EXPORT_SYMBOL(mipi_dbi_debugfs_init);

#endif

MODULE_DESCRIPTION("MIPI Display Bus Interface (DBI) LCD controller support");
MODULE_LICENSE("GPL");
