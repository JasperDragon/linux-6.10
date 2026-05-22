/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ------------------------------------------------------------------------- */
/* adap-pcf.h - PCF8584 适配器的 I2C 算法层接口                              */
/* ------------------------------------------------------------------------- */
/*   Copyright (C) 1995-97 Simon G. Vogl
                   1998-99 Hans Berglund

 */
/* ------------------------------------------------------------------------- */

/* 其中还合入了 Kyösti Mälkki <kmalkki@cc.hut.fi> 以及
 * Frodo Looijaard <frodol@dds.nl> 的修改。
 */

#ifndef _LINUX_I2C_ALGO_PCF_H
#define _LINUX_I2C_ALGO_PCF_H

struct i2c_algo_pcf_data {
	void *data;		/* 底层寄存器访问例程使用的私有上下文 */
	void (*setpcf) (void *data, int ctl, int val);
	int  (*getpcf) (void *data, int ctl);
	int  (*getown) (void *data);
	int  (*getclock) (void *data);
	void (*waitforpin) (void *data);

	void (*xfer_begin) (void *data);
	void (*xfer_end) (void *data);

	/* 多主场景下丢失仲裁后的退避时间，单位毫秒。
	 * 如果总线存在多个主设备，应由适配器驱动或了解硬件拓扑的上层调用者
	 * 提供一个合适的退避值；若总线为单主模式，则保持为 0 即可。
	 */
	unsigned long lab_mdelay;
};

int i2c_pcf_add_bus(struct i2c_adapter *);

#endif /* _LINUX_I2C_ALGO_PCF_H */
