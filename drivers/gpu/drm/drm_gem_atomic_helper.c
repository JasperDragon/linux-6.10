// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * DOC: GEM 原子提交辅助函数概述 (中文)
 *
 * 该文件为使用 GEM 对象的驱动程序实现了通用的原子提交（atomic-commit）辅助
 * 函数。主要包括两个核心功能模块：
 *
 * 1. 平面同步辅助：
 *    在扫描输出之前，需要同步可能向帧缓冲写入数据的生产者（如 GPU 渲染）。
 *    drm_gem_plane_helper_prepare_fb() 从帧缓冲的 dma_resv 对象中提取
 *    排他性 fence（围栏），并将其附加到平面状态中，供原子框架等待。
 *    所有驱动程序都应在 &drm_plane_helper_funcs.prepare_fb 实现中调用此函数。
 *
 * 2. 阴影缓冲平面辅助：
 *    对于使用阴影缓冲区（shadow buffer）的驱动程序，在原子更新期间需要将
 *    阴影缓冲区的内容复制到硬件的帧缓冲内存中。这需要将阴影缓冲区映射到
 *    内核地址空间。这些辅助函数建立了映射管理机制，通过
 *    struct drm_shadow_plane_state 在提交尾函数（如 atomic_update）中
 *    提供映射地址。
 *    阴影缓冲平面可以通过 DRM_GEM_SHADOW_PLANE_FUNCS 和
 *    DRM_GEM_SHADOW_PLANE_HELPER_FUNCS 宏轻松启用。
 */

#include <linux/dma-resv.h>
#include <linux/dma-fence-chain.h>
#include <linux/export.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "drm_internal.h"

/**
 * DOC: overview
 *
 * The GEM atomic helpers library implements generic atomic-commit
 * functions for drivers that use GEM objects. Currently, it provides
 * synchronization helpers, and plane state and framebuffer BO mappings
 * for planes with shadow buffers.
 *
 * Before scanout, a plane's framebuffer needs to be synchronized with
 * possible writers that draw into the framebuffer. All drivers should
 * call drm_gem_plane_helper_prepare_fb() from their implementation of
 * struct &drm_plane_helper.prepare_fb . It sets the plane's fence from
 * the framebuffer so that the DRM core can synchronize access automatically.
 * drm_gem_plane_helper_prepare_fb() can also be used directly as
 * implementation of prepare_fb.
 *
 * .. code-block:: c
 *
 *	#include <drm/drm_gem_atomic_helper.h>
 *
 *	struct drm_plane_helper_funcs driver_plane_helper_funcs = {
 *		...,
 *		. prepare_fb = drm_gem_plane_helper_prepare_fb,
 *	};
 *
 * A driver using a shadow buffer copies the content of the shadow buffers
 * into the HW's framebuffer memory during an atomic update. This requires
 * a mapping of the shadow buffer into kernel address space. The mappings
 * cannot be established by commit-tail functions, such as atomic_update,
 * as this would violate locking rules around dma_buf_vmap().
 *
 * The helpers for shadow-buffered planes establish and release mappings,
 * and provide struct drm_shadow_plane_state, which stores the plane's mapping
 * for commit-tail functions.
 *
 * Shadow-buffered planes can easily be enabled by using the provided macros
 * %DRM_GEM_SHADOW_PLANE_FUNCS and %DRM_GEM_SHADOW_PLANE_HELPER_FUNCS.
 * These macros set up the plane and plane-helper callbacks to point to the
 * shadow-buffer helpers.
 *
 * .. code-block:: c
 *
 *	#include <drm/drm_gem_atomic_helper.h>
 *
 *	struct drm_plane_funcs driver_plane_funcs = {
 *		...,
 *		DRM_GEM_SHADOW_PLANE_FUNCS,
 *	};
 *
 *	struct drm_plane_helper_funcs driver_plane_helper_funcs = {
 *		...,
 *		DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
 *	};
 *
 * In the driver's atomic-update function, shadow-buffer mappings are available
 * from the plane state. Use to_drm_shadow_plane_state() to upcast from
 * struct drm_plane_state.
 *
 * .. code-block:: c
 *
 *	void driver_plane_atomic_update(struct drm_plane *plane,
 *					struct drm_plane_state *old_plane_state)
 *	{
 *		struct drm_plane_state *plane_state = plane->state;
 *		struct drm_shadow_plane_state *shadow_plane_state =
 *			to_drm_shadow_plane_state(plane_state);
 *
 *		// access shadow buffer via shadow_plane_state->map
 *	}
 *
 * A mapping address for each of the framebuffer's buffer object is stored in
 * struct &drm_shadow_plane_state.map. The mappings are valid while the state
 * is being used.
 */

/*
 * Plane Helpers
 */

/**
 * drm_gem_plane_helper_prepare_fb() - 准备基于 GEM 的帧缓冲用于扫描输出
 *
 * 中文: 从 &drm_gem_object.resv 中提取排他性 fence 并附加到平面状态中，
 * 供原子辅助框架等待。这对于正确实现通过 &dma_buf 共享的缓冲区的隐式同步
 * 至关重要。此函数处理显式 fence 和隐式 fence 的合并：如果已有通过原子 IOCTL
 * 显式设置的 fence，则使用 DMA_RESV_USAGE_KERNEL 获取隐式 fence；否则使用
 * DMA_RESV_USAGE_WRITE。两者的组合通过 dma_fence_chain 链接。
 *
 * 对于始终将缓冲区固定在内存中的简单 GEM 驱动程序，不需要实现
 * &drm_plane_helper_funcs.cleanup_fb 钩子。
 *
 * 如果未提供回调，此函数是 GEM 驱动程序的 &drm_plane_helper_funcs.prepare_fb
 * 的默认实现。
 *
 * @plane: 平面
 * @state: fence 将被附加到的平面状态
 */
int drm_gem_plane_helper_prepare_fb(struct drm_plane *plane,
				    struct drm_plane_state *state)
{
	struct dma_fence *fence = dma_fence_get(state->fence);
	enum dma_resv_usage usage;
	size_t i;
	int ret;

	if (!state->fb)
		return 0;

	/*
	 * Only add the kernel fences here if there is already a fence set via
	 * explicit fencing interfaces on the atomic ioctl.
	 *
	 * This way explicit fencing can be used to overrule implicit fencing,
	 * which is important to make explicit fencing use-cases work: One
	 * example is using one buffer for 2 screens with different refresh
	 * rates. Implicit fencing will clamp rendering to the refresh rate of
	 * the slower screen, whereas explicit fence allows 2 independent
	 * render and display loops on a single buffer. If a driver allows
	 * obeys both implicit and explicit fences for plane updates, then it
	 * will break all the benefits of explicit fencing.
	 */
	usage = fence ? DMA_RESV_USAGE_KERNEL : DMA_RESV_USAGE_WRITE;

	for (i = 0; i < state->fb->format->num_planes; ++i) {
		struct drm_gem_object *obj = drm_gem_fb_get_obj(state->fb, i);
		struct dma_fence *new;

		if (!obj) {
			ret = -EINVAL;
			goto error;
		}

		ret = dma_resv_get_singleton(obj->resv, usage, &new);
		if (ret)
			goto error;

		if (new && fence) {
			struct dma_fence_chain *chain = dma_fence_chain_alloc();

			if (!chain) {
				ret = -ENOMEM;
				goto error;
			}

			dma_fence_chain_init(chain, fence, new, 1);
			fence = &chain->base;

		} else if (new) {
			fence = new;
		}
	}

	dma_fence_put(state->fence);
	state->fence = fence;
	return 0;

error:
	dma_fence_put(fence);
	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_plane_helper_prepare_fb);

/*
 * Shadow-buffered Planes
 */

/**
 * __drm_gem_duplicate_shadow_plane_state - 复制阴影缓冲平面状态（内部）
 *
 * 中文: 复制阴影缓冲平面状态。适用于子类化 struct drm_shadow_plane_state
 * 的驱动程序。注意：该函数不会复制现有的阴影缓冲映射，映射由 prepare_fb
 * 和 cleanup_fb 辅助函数在原子提交期间维护。此函数还会复制格式转换状态
 * （fmtcnv_state）。
 *
 * @plane: 平面
 * @new_shadow_plane_state: 新的阴影缓冲平面状态
 */
void
__drm_gem_duplicate_shadow_plane_state(struct drm_plane *plane,
				       struct drm_shadow_plane_state *new_shadow_plane_state)
{
	struct drm_plane_state *plane_state = plane->state;
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(plane_state);

	__drm_atomic_helper_plane_duplicate_state(plane, &new_shadow_plane_state->base);

	drm_format_conv_state_copy(&new_shadow_plane_state->fmtcnv_state,
				   &shadow_plane_state->fmtcnv_state);
}
EXPORT_SYMBOL(__drm_gem_duplicate_shadow_plane_state);

/**
 * drm_gem_duplicate_shadow_plane_state - 复制阴影缓冲平面状态
 *
 * 中文: 实现 struct &drm_plane_funcs.atomic_duplicate_state 用于阴影缓冲
 * 平面。该函数假设现有状态为 struct drm_shadow_plane_state 类型，并分配
 * 同类型的新状态。不会复制现有的阴影缓冲映射——映射由 prepare_fb 和
 * cleanup_fb 在原子提交期间维护。
 *
 * @plane: 平面
 *
 * Returns:
 * A pointer to a new plane state on success, or NULL otherwise.
 */
struct drm_plane_state *
drm_gem_duplicate_shadow_plane_state(struct drm_plane *plane)
{
	struct drm_plane_state *plane_state = plane->state;
	struct drm_shadow_plane_state *new_shadow_plane_state;

	if (!plane_state)
		return NULL;

	new_shadow_plane_state = kzalloc_obj(*new_shadow_plane_state);
	if (!new_shadow_plane_state)
		return NULL;
	__drm_gem_duplicate_shadow_plane_state(plane, new_shadow_plane_state);

	return &new_shadow_plane_state->base;
}
EXPORT_SYMBOL(drm_gem_duplicate_shadow_plane_state);

/**
 * __drm_gem_destroy_shadow_plane_state - 清理阴影缓冲平面状态（内部）
 *
 * 中文: 清理阴影缓冲平面状态，释放格式转换状态并销毁底层原子平面状态。
 * 适用于子类化 struct drm_shadow_plane_state 的驱动程序。
 * 注意：此函数不释放 shadow_plane_state 结构体本身的内存，仅清理其内部资源。
 *
 * @shadow_plane_state: 要清理的阴影缓冲平面状态
 */
void __drm_gem_destroy_shadow_plane_state(struct drm_shadow_plane_state *shadow_plane_state)
{
	drm_format_conv_state_release(&shadow_plane_state->fmtcnv_state);
	__drm_atomic_helper_plane_destroy_state(&shadow_plane_state->base);
}
EXPORT_SYMBOL(__drm_gem_destroy_shadow_plane_state);

/**
 * drm_gem_destroy_shadow_plane_state - 销毁阴影缓冲平面状态
 *
 * 中文: 实现 struct &drm_plane_funcs.atomic_destroy_state 用于阴影缓冲平面。
 * 该函数假设阴影缓冲的映射已被释放（通过 cleanup_fb），然后销毁格式转换
 * 状态并释放平面状态内存。
 *
 * @plane: 平面
 * @plane_state: 类型为 struct drm_shadow_plane_state 的平面状态
 */
void drm_gem_destroy_shadow_plane_state(struct drm_plane *plane,
					struct drm_plane_state *plane_state)
{
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(plane_state);

	__drm_gem_destroy_shadow_plane_state(shadow_plane_state);
	kfree(shadow_plane_state);
}
EXPORT_SYMBOL(drm_gem_destroy_shadow_plane_state);

/**
 * __drm_gem_reset_shadow_plane - 重置阴影缓冲平面（内部）
 *
 * 中文: 重置阴影缓冲平面的状态。如果提供了 @shadow_plane_state，则初始化
 * 原子平面状态和格式转换状态；如果为 NULL，则仅调用原子平面重置。
 * 适用于子类化 struct drm_shadow_plane_state 的驱动程序。
 *
 * @plane: 平面
 * @shadow_plane_state: 阴影缓冲平面状态（可为 NULL）
 */
void __drm_gem_reset_shadow_plane(struct drm_plane *plane,
				  struct drm_shadow_plane_state *shadow_plane_state)
{
	if (shadow_plane_state) {
		__drm_atomic_helper_plane_reset(plane, &shadow_plane_state->base);
		drm_format_conv_state_init(&shadow_plane_state->fmtcnv_state);
	} else {
		__drm_atomic_helper_plane_reset(plane, NULL);
	}
}
EXPORT_SYMBOL(__drm_gem_reset_shadow_plane);

/**
 * drm_gem_reset_shadow_plane - 重置阴影缓冲平面
 *
 * 中文: 实现 struct &drm_plane_funcs.reset_plane 用于阴影缓冲平面。
 * 该函数销毁现有的平面状态（应为 struct drm_shadow_plane_state 类型），
 * 然后分配并初始化一个新的阴影缓冲平面状态。
 *
 * @plane: 平面
 */
void drm_gem_reset_shadow_plane(struct drm_plane *plane)
{
	struct drm_shadow_plane_state *shadow_plane_state;

	if (plane->state) {
		drm_gem_destroy_shadow_plane_state(plane, plane->state);
		plane->state = NULL; /* must be set to NULL here */
	}

	shadow_plane_state = kzalloc_obj(*shadow_plane_state);
	__drm_gem_reset_shadow_plane(plane, shadow_plane_state);
}
EXPORT_SYMBOL(drm_gem_reset_shadow_plane);

/**
 * drm_gem_begin_shadow_fb_access - 为 CPU 访问准备阴影帧缓冲
 *
 * 中文: 实现 struct &drm_plane_helper_funcs.begin_fb_access。该函数将平面
 * 帧缓冲的所有缓冲区对象映射到内核地址空间，并存储在
 * struct &drm_shadow_plane_state.map 中。对于非零偏移的平面，数据起始地址
 * 存储在 struct &drm_shadow_plane_state.data 中，方便 commit-tail 函数访问。
 * 完成访问后应调用 drm_gem_end_shadow_fb_access() 解除映射。
 *
 * @plane: 平面
 * @plane_state: 类型为 struct drm_shadow_plane_state 的平面状态
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_gem_begin_shadow_fb_access(struct drm_plane *plane, struct drm_plane_state *plane_state)
{
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;

	if (!fb)
		return 0;

	return drm_gem_fb_vmap(fb, shadow_plane_state->map, shadow_plane_state->data);
}
EXPORT_SYMBOL(drm_gem_begin_shadow_fb_access);

/**
 * drm_gem_end_shadow_fb_access - 释放阴影帧缓冲的 CPU 访问
 *
 * 中文: 实现 struct &drm_plane_helper_funcs.end_fb_access。它撤销
 * drm_gem_begin_shadow_fb_access() 的所有效果，解除所有缓冲区对象的内核
 * 地址空间映射。
 *
 * @plane: 平面
 * @plane_state: 类型为 struct drm_shadow_plane_state 的平面状态
 */
void drm_gem_end_shadow_fb_access(struct drm_plane *plane, struct drm_plane_state *plane_state)
{
	struct drm_shadow_plane_state *shadow_plane_state = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;

	if (!fb)
		return;

	drm_gem_fb_vunmap(fb, shadow_plane_state->map);
}
EXPORT_SYMBOL(drm_gem_end_shadow_fb_access);

/**
 * drm_gem_simple_kms_begin_shadow_fb_access - 为简单 KMS 显示管道准备阴影帧缓冲
 *
 * 中文: 实现 struct drm_simple_display_funcs.begin_fb_access。
 * 这是 drm_gem_begin_shadow_fb_access() 针对简单显示管道的包装函数，
 * 将 pipe 参数转换为 plane 后调用底层实现。
 *
 * @pipe: 简单显示管道
 * @plane_state: 类型为 struct drm_shadow_plane_state 的平面状态
 *
 * Returns:
 * 0 on success, or a negative errno code otherwise.
 */
int drm_gem_simple_kms_begin_shadow_fb_access(struct drm_simple_display_pipe *pipe,
					      struct drm_plane_state *plane_state)
{
	return drm_gem_begin_shadow_fb_access(&pipe->plane, plane_state);
}
EXPORT_SYMBOL(drm_gem_simple_kms_begin_shadow_fb_access);

/**
 * drm_gem_simple_kms_end_shadow_fb_access - 释放简单 KMS 阴影帧缓冲的 CPU 访问
 *
 * 中文: 实现 struct drm_simple_display_funcs.end_fb_access，撤销
 * drm_gem_simple_kms_begin_shadow_fb_access() 的所有效果。
 * 针对简单显示管道的 drm_gem_end_shadow_fb_access() 包装函数。
 *
 * @pipe: 简单显示管道
 * @plane_state: 类型为 struct drm_shadow_plane_state 的平面状态
 */
void drm_gem_simple_kms_end_shadow_fb_access(struct drm_simple_display_pipe *pipe,
					     struct drm_plane_state *plane_state)
{
	drm_gem_end_shadow_fb_access(&pipe->plane, plane_state);
}
EXPORT_SYMBOL(drm_gem_simple_kms_end_shadow_fb_access);

/**
 * drm_gem_simple_kms_reset_shadow_plane - 重置简单 KMS 阴影缓冲平面
 *
 * 中文: 实现 struct drm_simple_display_funcs.reset_plane 用于阴影缓冲平面。
 * 针对简单显示管道的 drm_gem_reset_shadow_plane() 包装函数。
 *
 * @pipe: 简单显示管道
 */
void drm_gem_simple_kms_reset_shadow_plane(struct drm_simple_display_pipe *pipe)
{
	drm_gem_reset_shadow_plane(&pipe->plane);
}
EXPORT_SYMBOL(drm_gem_simple_kms_reset_shadow_plane);

/**
 * drm_gem_simple_kms_duplicate_shadow_plane_state - 复制简单 KMS 阴影缓冲平面状态
 *
 * 中文: 实现 struct drm_simple_display_funcs.duplicate_plane_state 用于阴影
 * 缓冲平面。不会复制现有的阴影缓冲映射（映射由 prepare_fb/cleanup_fb 维护）。
 * 针对简单显示管道的 drm_gem_duplicate_shadow_plane_state() 包装函数。
 *
 * @pipe: 简单显示管道
 *
 * Returns:
 * A pointer to a new plane state on success, or NULL otherwise.
 */
struct drm_plane_state *
drm_gem_simple_kms_duplicate_shadow_plane_state(struct drm_simple_display_pipe *pipe)
{
	return drm_gem_duplicate_shadow_plane_state(&pipe->plane);
}
EXPORT_SYMBOL(drm_gem_simple_kms_duplicate_shadow_plane_state);

/**
 * drm_gem_simple_kms_destroy_shadow_plane_state - 销毁简单 KMS 阴影缓冲平面状态
 *
 * 中文: 实现 struct drm_simple_display_funcs.destroy_plane_state 用于阴影
 * 缓冲平面。假设阴影缓冲的映射已被释放。针对简单显示管道的
 * drm_gem_destroy_shadow_plane_state() 包装函数。
 *
 * @pipe: 简单显示管道
 * @plane_state: 类型为 struct drm_shadow_plane_state 的平面状态
 */
void drm_gem_simple_kms_destroy_shadow_plane_state(struct drm_simple_display_pipe *pipe,
						   struct drm_plane_state *plane_state)
{
	drm_gem_destroy_shadow_plane_state(&pipe->plane, plane_state);
}
EXPORT_SYMBOL(drm_gem_simple_kms_destroy_shadow_plane_state);
