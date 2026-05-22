// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/*
 * 文件名: drm_ras_genl_family.c
 *
 * 中文描述: DRM RAS Generic Netlink 家族注册
 *
 * 本文件负责在 Generic Netlink 框架中注册 "drm_ras" 协议家族。
 * Generic Netlink 是 Linux 内核中用于内核与用户空间通信的灵活机制，
 * 相比传统 IOCTL，它提供了更丰富的消息格式和更好的可扩展性。
 *
 * 核心功能：
 *   1. drm_ras_genl_family_register() - 注册 drm_ras netlink 家族
 *      在 drm_drv_init() 时调用一次
 *   2. drm_ras_genl_family_unregister() - 注销 drm_ras netlink 家族
 *      在 drm_drv_exit() 时调用
 *
 * 使用 registered 标志位确保注销操作在任何时候都能安全执行。
 */

#include <drm/drm_ras_genl_family.h>
#include "drm_ras_nl.h"

/* Track family registration so the drm_exit can be called at any time */
static bool registered;

/**
 * drm_ras_genl_family_register() - 注册 drm_ras Generic Netlink 家族
 *
 * 在 Generic Netlink 子系统中注册 drm_ras 协议家族。注册成功后，
 * 用户空间可以通过 NETLINK_GENERIC 套接字与 DRM RAS 功能通信。
 *
 * 必须在模块初始化期间调用且仅调用一次（在 drm_drv_init() 中）。
 *
 * 返回：0 成功，负错误码失败
 */
int drm_ras_genl_family_register(void)
{
	int ret;

	registered = false;

	ret = genl_register_family(&drm_ras_nl_family);
	if (ret)
		return ret;

	registered = true;
	return 0;
}

/**
 * drm_ras_genl_family_unregister() - 注销 drm_ras Generic Netlink 家族
 *
 * 从 Generic Netlink 子系统中移除 drm_ras 协议家族。
 * 可以在任何时候安全调用（通过 registered 标志保护），
 * 但仅应调用一次（在 drm_drv_exit() 中）。
 */
void drm_ras_genl_family_unregister(void)
{
	if (registered) {
		genl_unregister_family(&drm_ras_nl_family);
		registered = false;
	}
}
