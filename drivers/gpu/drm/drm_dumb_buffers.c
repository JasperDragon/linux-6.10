/*
 * Copyright (c) 2006-2008 Intel Corporation
 * Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
 * Copyright (c) 2008 Red Hat Inc.
 * Copyright (c) 2016 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/*
 * DRM 傻瓜缓冲（Dumb Buffer）API - 提供标准化的显存缓冲区创建接口
 *
 * 本文件实现了 DRM 框架的傻瓜缓冲区 API，为早期启动图形和简单显示场景
 * 提供一种标准化的显存缓冲区创建、映射和销毁机制。不同于需要专有用户空间
 * 组件（如 libdrm）的复杂 GPU 缓冲区分配，Dumb Buffer 提供了一套简单、
 * 通用的接口，适用于 KMS（内核模式设置）的扫描输出，可在没有完整图形栈
 * 的环境中使用。
 *
 * 要支持 Dumb Buffer，驱动程序需要实现以下回调：
 *   - &drm_driver.dumb_create - 创建傻瓜缓冲区
 *   - &drm_driver.dumb_map_offset - 获取 mmap 偏移量（默认使用 drm_gem_dumb_map_offset）
 *   - &drm_driver.dumb_destroy - 销毁傻瓜缓冲区（非 GEM 驱动需要）
 *
 * 注意：Dumb Buffer 不应用于 GPU 加速，这已在一些 ARM 嵌入式平台上被尝试过，
 * 此类驱动确实需要硬件特定的 IOCTL 来分配适当的缓冲对象。
 */

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_dumb_buffers.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>

#include "drm_crtc_internal.h"
#include "drm_internal.h"

/**
 * DOC: overview
 *
 * The KMS API doesn't standardize backing storage object creation and leaves it
 * to driver-specific ioctls. Furthermore actually creating a buffer object even
 * for GEM-based drivers is done through a driver-specific ioctl - GEM only has
 * a common userspace interface for sharing and destroying objects. While not an
 * issue for full-fledged graphics stacks that include device-specific userspace
 * components (in libdrm for instance), this limit makes DRM-based early boot
 * graphics unnecessarily complex.
 *
 * Dumb objects partly alleviate the problem by providing a standard API to
 * create dumb buffers suitable for scanout, which can then be used to create
 * KMS frame buffers.
 *
 * To support dumb objects drivers must implement the &drm_driver.dumb_create
 * and &drm_driver.dumb_map_offset operations (the latter defaults to
 * drm_gem_dumb_map_offset() if not set). Drivers that don't use GEM handles
 * additionally need to implement the &drm_driver.dumb_destroy operation. See
 * the callbacks for further details.
 *
 * Note that dumb objects may not be used for gpu acceleration, as has been
 * attempted on some ARM embedded platforms. Such drivers really must have
 * a hardware-specific ioctl to allocate suitable buffer objects.
 */

static int drm_mode_align_dumb(struct drm_mode_create_dumb *args,
			       unsigned long hw_pitch_align,
			       unsigned long hw_size_align)
{
	u32 pitch = args->pitch;
	u32 size;

	if (!pitch)
		return -EINVAL;

	if (hw_pitch_align)
		pitch = roundup(pitch, hw_pitch_align);

	if (!hw_size_align)
		hw_size_align = PAGE_SIZE;
	else if (!IS_ALIGNED(hw_size_align, PAGE_SIZE))
		return -EINVAL; /* TODO: handle this if necessary */

	if (check_mul_overflow(args->height, pitch, &size))
		return -EINVAL;
	size = ALIGN(size, hw_size_align);
	if (!size)
		return -EINVAL;

	args->pitch = pitch;
	args->size = size;

	return 0;
}

/**
 * drm_mode_size_dumb - 计算傻瓜缓冲区的扫描行和缓冲区大小
 * @dev: DRM 设备
 * @args: 傻瓜缓冲区的参数
 * @hw_pitch_align: 硬件要求的扫描行对齐字节数
 * @hw_size_align: 硬件要求的缓冲区大小对齐字节数
 *
 * 辅助函数 drm_mode_size_dumb() 计算傻瓜缓冲区的分配大小和扫描行大小。
 * 调用者需要在参数 @arg 中设置缓冲区的宽度、高度和颜色模式。
 * 该函数验证输入的正确性并检查可能的溢出。如果成功，则返回所需
 * 的扫描行步幅（pitch）和缓冲区大小。
 *
 * 参数 @hw_pitch_align 允许驱动程序指定扫描行对齐要求（如果硬件需要）。
 * 计算出的步幅将是对齐值的整数倍。参数 @hw_size_align 允许指定
 * 缓冲区大小的对齐要求。提供的对齐值应反映图形硬件的实际需求。
 * drm_mode_size_dumb() 自动处理跨所有驱动和硬件的 GEM 相关约束。
 * 例如，返回的缓冲区大小始终是 PAGE_SIZE 的整数倍，以满足 mmap()
 * 的内存页对齐要求。
 *
 * 返回：
 * 成功返回 0，失败返回负错误码。
 */
int drm_mode_size_dumb(struct drm_device *dev,
		       struct drm_mode_create_dumb *args,
		       unsigned long hw_pitch_align,
		       unsigned long hw_size_align)
{
	u64 pitch = 0;
	u32 fourcc;

	/*
	 * The scanline pitch depends on the buffer width and the color
	 * format. The latter is specified as a color-mode constant for
	 * which we first have to find the corresponding color format.
	 *
	 * Different color formats can have the same color-mode constant.
	 * For example XRGB8888 and BGRX8888 both have a color mode of 32.
	 * It is possible to use different formats for dumb-buffer allocation
	 * and rendering as long as all involved formats share the same
	 * color-mode constant.
	 */
	fourcc = drm_driver_color_mode_format(dev, args->bpp);
	if (fourcc != DRM_FORMAT_INVALID) {
		const struct drm_format_info *info = drm_format_info(fourcc);

		if (!info)
			return -EINVAL;
		pitch = drm_format_info_min_pitch(info, 0, args->width);
	} else if (args->bpp) {
		/*
		 * Some userspace throws in arbitrary values for bpp and
		 * relies on the kernel to figure it out. In this case we
		 * fall back to the old method of using bpp directly. The
		 * over-commitment of memory from the rounding is acceptable
		 * for compatibility with legacy userspace. We have a number
		 * of deprecated legacy values that are explicitly supported.
		 */
		switch (args->bpp) {
		default:
			drm_warn_once(dev,
				      "Unknown color mode %u; guessing buffer size.\n",
				      args->bpp);
			fallthrough;
		/*
		 * These constants represent various YUV formats supported by
		 * drm_gem_afbc_get_bpp().
		 */
		case 12: // DRM_FORMAT_YUV420_8BIT
		case 15: // DRM_FORMAT_YUV420_10BIT
		case 30: // DRM_FORMAT_VUY101010
			fallthrough;
		/*
		 * Used by Mesa and Gstreamer to allocate NV formats and others
		 * as RGB buffers. Technically, XRGB16161616F formats are RGB,
		 * but the dumb buffers are not supposed to be used for anything
		 * beyond 32 bits per pixels.
		 */
		case 10: // DRM_FORMAT_NV{15,20,30}, DRM_FORMAT_P010
		case 64: // DRM_FORMAT_{XRGB,XBGR,ARGB,ABGR}16161616F
			pitch = args->width * DIV_ROUND_UP(args->bpp, SZ_8);
			break;
		}
	}

	if (!pitch || pitch > U32_MAX)
		return -EINVAL;

	args->pitch = pitch;

	return drm_mode_align_dumb(args, hw_pitch_align, hw_size_align);
}
EXPORT_SYMBOL(drm_mode_size_dumb);

int drm_mode_create_dumb(struct drm_device *dev,
			 struct drm_mode_create_dumb *args,
			 struct drm_file *file_priv)
{
	u32 cpp, stride, size;

	if (!dev->driver->dumb_create)
		return -ENOSYS;
	if (!args->width || !args->height || !args->bpp)
		return -EINVAL;

	/* overflow checks for 32bit size calculations */
	if (args->bpp > U32_MAX - 8)
		return -EINVAL;
	cpp = DIV_ROUND_UP(args->bpp, 8);
	if (cpp > U32_MAX / args->width)
		return -EINVAL;
	stride = cpp * args->width;
	if (args->height > U32_MAX / stride)
		return -EINVAL;

	/* test for wrap-around */
	size = args->height * stride;
	if (PAGE_ALIGN(size) == 0)
		return -EINVAL;

	/*
	 * handle, pitch and size are output parameters. Zero them out to
	 * prevent drivers from accidentally using uninitialized data. Since
	 * not all existing userspace is clearing these fields properly we
	 * cannot reject IOCTL with garbage in them.
	 */
	args->handle = 0;
	args->pitch = 0;
	args->size = 0;

	return dev->driver->dumb_create(file_priv, dev, args);
}

/*
 * drm_mode_create_dumb_ioctl - 创建傻瓜缓冲区的 IOCTL 处理函数
 * @dev: DRM 设备
 * @data: IOCTL 参数，指向 drm_mode_create_dumb 结构
 * @file_priv: DRM 文件私有数据
 *
 * 处理 DRM_IOCTL_MODE_CREATE_DUMB IOCTL。创建适用于扫描输出的
 * 傻瓜缓冲区，返回句柄（handle）、步幅（pitch）和大小（size）。
 *
 * 内部调用 drm_mode_create_dumb()，后者进一步调用驱动程序注册的
 * dumb_create 回调。如果创建失败，输出参数会被清零以防止未初始化的
 * 数据泄漏给用户空间。
 *
 * 返回：0 表示成功，负错误码表示失败。
 */
int drm_mode_create_dumb_ioctl(struct drm_device *dev,
			       void *data, struct drm_file *file_priv)
{
	struct drm_mode_create_dumb *args = data;
	int err;

	err = drm_mode_create_dumb(dev, args, file_priv);
	if (err) {
		args->handle = 0;
		args->pitch = 0;
		args->size = 0;
	}
	return err;
}

static int drm_mode_mmap_dumb(struct drm_device *dev, struct drm_mode_map_dumb *args,
			      struct drm_file *file_priv)
{
	if (!dev->driver->dumb_create)
		return -ENOSYS;

	if (dev->driver->dumb_map_offset)
		return dev->driver->dumb_map_offset(file_priv, dev, args->handle,
						    &args->offset);
	else
		return drm_gem_dumb_map_offset(file_priv, dev, args->handle,
					       &args->offset);
}

/**
 * drm_mode_mmap_dumb_ioctl - 为傻瓜缓冲区创建 mmap 偏移量 IOCTL 处理函数
 * @dev: DRM 设备
 * @data: IOCTL 数据
 * @file_priv: DRM 文件信息
 *
 * 在 DRM 设备节点的地址空间中分配一个偏移量，以便能够对傻瓜缓冲区
 * 进行内存映射（mmap）。用户空间通过 IOCTL 调用此函数获取映射偏移，
 * 然后使用 mmap() 将缓冲区映射到进程地址空间。
 *
 * 返回：
 * 成功返回 0，失败返回负错误码。
 */
int drm_mode_mmap_dumb_ioctl(struct drm_device *dev,
			     void *data, struct drm_file *file_priv)
{
	struct drm_mode_map_dumb *args = data;
	int err;

	err = drm_mode_mmap_dumb(dev, args, file_priv);
	if (err)
		args->offset = 0;
	return err;
}

/*
 * drm_mode_destroy_dumb - 销毁傻瓜缓冲区
 * @dev: DRM 设备
 * @handle: 要销毁的缓冲区句柄
 * @file_priv: DRM 文件私有数据
 *
 * 通过删除 GEM 句柄来销毁先前创建的傻瓜缓冲区。
 * 如果驱动不支持 dumb_create，则返回 -ENOSYS。
 *
 * 返回：0 表示成功，负错误码表示失败。
 */
int drm_mode_destroy_dumb(struct drm_device *dev, u32 handle,
			  struct drm_file *file_priv)
{
	if (!dev->driver->dumb_create)
		return -ENOSYS;

	return drm_gem_handle_delete(file_priv, handle);
}

/*
 * drm_mode_destroy_dumb_ioctl - 销毁傻瓜缓冲区的 IOCTL 处理函数
 * @dev: DRM 设备
 * @data: IOCTL 参数，指向 drm_mode_destroy_dumb 结构
 * @file_priv: DRM 文件私有数据
 *
 * 处理 DRM_IOCTL_MODE_DESTROY_DUMB IOCTL，销毁指定的傻瓜缓冲区。
 *
 * 返回：0 表示成功，负错误码表示失败。
 */
int drm_mode_destroy_dumb_ioctl(struct drm_device *dev,
				void *data, struct drm_file *file_priv)
{
	struct drm_mode_destroy_dumb *args = data;

	return drm_mode_destroy_dumb(dev, args->handle, file_priv);
}
