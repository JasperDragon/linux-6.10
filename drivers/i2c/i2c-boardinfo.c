// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * i2c-boardinfo.c - 收集预先声明的 I2C 设备
 *
 * 这一层保存板级代码提前登记的 i2c_board_info，等对应适配器注册后，
 * 再由 I2C core 统一实例化成真正的 i2c_client。
 */

#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/property.h>
#include <linux/rwsem.h>
#include <linux/slab.h>

#include "i2c-core.h"


/*
 * 这些符号只导出给 I2C core 使用，不支持其他用户直接调用。
 */
DECLARE_RWSEM(__i2c_board_lock);
EXPORT_SYMBOL_GPL(__i2c_board_lock);

LIST_HEAD(__i2c_board_list);
EXPORT_SYMBOL_GPL(__i2c_board_list);

int __i2c_first_dynamic_bus_num;
EXPORT_SYMBOL_GPL(__i2c_first_dynamic_bus_num);


/**
 * i2c_register_board_info - 静态声明 I2C 设备
 * @busnum: 这些设备所属的总线编号
 * @info: i2c 设备描述符数组
 * @len: 描述符数量；也可以为 0，用来预留指定总线号
 *
 * 使用 Linux I2C 驱动栈的系统，可以在初始化阶段声明一批板级设备
 * 信息。通常应当在接近 arch_initcall() 的板级初始化代码里完成，
 * 并且要早于任何 I2C 适配器驱动的注册。
 *
 * 例如，主板初始化代码可以声明若干设备；板级堆叠里的子板初始化
 * 代码也可以各自声明自己的设备。
 *
 * 这些 I2C 设备会在对应总线的适配器注册之后再创建。创建完成后，
 * 标准驱动模型会负责把“新式” I2C 驱动绑定到这些设备上。通过
 * 这个接口预先声明的总线号不能再被动态分配。
 *
 * 传入的 board info 可以安全地放在 __initdata 中，但要小心其中
 * 可能嵌入的指针（例如 platform_data、函数指针等），因为这些
 * 内容不会被深拷贝。
 */
int i2c_register_board_info(int busnum, struct i2c_board_info const *info, unsigned len)
{
	int status;

	down_write(&__i2c_board_lock);

	/* dynamic bus numbers will be assigned after the last static one */
	if (busnum >= __i2c_first_dynamic_bus_num)
		__i2c_first_dynamic_bus_num = busnum + 1;

	for (status = 0; len; len--, info++) {
		struct i2c_devinfo	*devinfo;

		devinfo = kzalloc_obj(*devinfo);
		if (!devinfo) {
			pr_debug("i2c-core: can't register boardinfo!\n");
			status = -ENOMEM;
			break;
		}

		devinfo->busnum = busnum;
		devinfo->board_info = *info;

		if (info->resources) {
			devinfo->board_info.resources =
				kmemdup(info->resources,
					info->num_resources *
						sizeof(*info->resources),
					GFP_KERNEL);
			if (!devinfo->board_info.resources) {
				status = -ENOMEM;
				kfree(devinfo);
				break;
			}
		}

		list_add_tail(&devinfo->list, &__i2c_board_list);
	}

	up_write(&__i2c_board_lock);

	return status;
}
