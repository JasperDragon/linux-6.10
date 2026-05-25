/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <asm/assembler.h>
// ============================================================================
// 文件概述: ARMv7 早期数据中止异常处理
// 该文件实现了 ARMv7 架构的数据中止(Data Abort)早期处理函数。
// 当 CPU 访问内存时发生异常(如缺页、权限错误、对齐错误等)，
// 由 MMU 触发数据中止异常，CPU 跳转到该处理程序。
// 主要功能:
//   - 读取 DFSR(Data Fault Status Register)获取故障状态码
//   - 读取 FAR(Fault Address Register)获取故障虚拟地址
//   - 禁用用户空间访问后转发到通用 do_DataAbort 处理函数
// 该处理函数在 MMU 开启后全程使用, 是内核虚拟内存管理的核心入口之一。
// ============================================================================
/*
 * Function: v7_early_abort
 *
 * Params  : r2 = pt_regs
 *	   : r4 = aborted context pc
 *	   : r5 = aborted context psr
 *
 * Returns : r4 - r11, r13 preserved
 *
 * Purpose : obtain information about current aborted instruction.
 */
	.arch	armv7-a
	.align	5
ENTRY(v7_early_abort)
	mrc	p15, 0, r1, c5, c0, 0		@ get FSR
	mrc	p15, 0, r0, c6, c0, 0		@ get FAR
	uaccess_disable ip			@ disable userspace access
	b	do_DataAbort
ENDPROC(v7_early_abort)
