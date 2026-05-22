/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *
 */

/*
 * DRM KMS 辅助模块通用文件
 *
 * 本文件是 DRM KMS（内核模式设置）辅助模块的入口点。
 * KMS 辅助模块提供了一组通用的辅助函数，用于简化 DRM 驱动程序的开发。
 * 这些函数涵盖了模式设置、显示管道管理、输出轮询、framebuffer 处理等
 * 多个方面的通用操作，帮助驱动开发者避免重复实现相同的功能逻辑。
 *
 * 本模块的核心功能分布在多个辅助文件中：
 *   - drm_atomic_helper.c: 原子接口辅助函数
 *   - drm_crtc_helper.c: CRTC 辅助函数
 *   - drm_plane_helper.c: 平面辅助函数
 *   - drm_probe_helper.c: 输出探测辅助函数
 *   - drm_fb_helper.c: framebuffer 辅助函数
 *
 * 本文件仅包含模块的许可证和作者信息，实际的辅助函数实现分散在
 * 各个子模块中。
 */

#include <linux/module.h>

MODULE_AUTHOR("David Airlie, Jesse Barnes");
MODULE_DESCRIPTION("DRM KMS helper");
MODULE_LICENSE("GPL and additional rights");
