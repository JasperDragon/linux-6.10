/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/vfp/vfphw.S
 *
 *  Copyright (C) 2004 ARM Limited.
 *  Written by Deep Blue Solutions Limited.
 */
//
// 文件: arch/arm/vfp/vfphw.S
// 描述: ARM VFP (Vector Floating Point) 硬件支持层
// 功能: 提供 VFP 寄存器状态的保存/恢复、以及单精度/双精度
//       浮点寄存器的读写访问入口
// 调用: 由 VFP 异常处理 (vfphandler) 在上下文切换时调用
//
#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/thread_info.h>
#include <asm/vfpmacros.h>
#include <linux/kern_levels.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>

	.macro  DBGSTR1, str, arg
#ifdef DEBUG
	stmfd	sp!, {r0-r3, ip, lr}
	mov	r1, \arg
	ldr	r0, =1f
	bl	_printk
	ldmfd	sp!, {r0-r3, ip, lr}

	.pushsection .rodata, "a"
1:	.ascii	KERN_DEBUG "VFP: \str\n"
	.byte	0
	.previous
#endif
	.endm

ENTRY(vfp_load_state)
// 从内存加载 VFP 状态 (上下文恢复)
// 参数: r0 = 保存状态的基地址
// 返回: FPEXC (浮点异常控制寄存器) 值
// 步骤: 加载 VFP 工作寄存器 -> 恢复 FPSCR/FPINST/FPINST2
	@ Load the current VFP state
	@ r0 - load location
	@ returns FPEXC
	DBGSTR1	"load VFP state %p", r0
					@ Load the saved state back into the VFP
	VFPFLDMIA r0, r1		@ reload the working registers while
					@ FPEXC is in a safe state
	ldmia	r0, {r0-r3}		@ load FPEXC, FPSCR, FPINST, FPINST2
	tst	r0, #FPEXC_EX		@ is there additional state to restore?
	beq	1f
	VFPFMXR	FPINST, r2		@ restore FPINST (only if FPEXC.EX is set)
	tst	r0, #FPEXC_FP2V		@ is there an FPINST2 to write?
	beq	1f
	VFPFMXR	FPINST2, r3		@ FPINST2 if needed (and present)
1:
	VFPFMXR	FPSCR, r1		@ restore status
	ret	lr
ENDPROC(vfp_load_state)

ENTRY(vfp_save_state)
// 将 VFP 状态保存到内存 (上下文保存)
// 参数: r0 = 保存目标地址, r1 = FPEXC 值
// 步骤: 保存 VFP 工作寄存器 -> 读取 FPSCR/FPINST/FPINST2 -> 存储控制寄存器
	@ Save the current VFP state
	@ r0 - save location
	@ r1 - FPEXC
	DBGSTR1	"save VFP state %p", r0
	VFPFSTMIA r0, r2		@ save the working registers
	VFPFMRX	r2, FPSCR		@ current status
	tst	r1, #FPEXC_EX		@ is there additional state to save?
	beq	1f
	VFPFMRX	r3, FPINST		@ FPINST (only if FPEXC.EX is set)
	tst	r1, #FPEXC_FP2V		@ is there an FPINST2 to read?
	beq	1f
	VFPFMRX	r12, FPINST2		@ FPINST2 if needed (and present)
1:
	stmia	r0, {r1, r2, r3, r12}	@ save FPEXC, FPSCR, FPINST, FPINST2
	ret	lr
ENDPROC(vfp_save_state)

	.macro	tbl_branch, base, tmp, shift
// 跳转表分发宏: 根据 \base 索引跳转到对应的寄存器访问入口
// 每个表项占用 8 字节 (2 条指令)
#ifdef CONFIG_THUMB2_KERNEL
	adr	\tmp, 1f
	add	\tmp, \tmp, \base, lsl \shift
	ret	\tmp
#else
	add	pc, pc, \base, lsl \shift
	mov	r0, r0
#endif
1:
	.endm

ENTRY(vfp_get_float)
// 读取单精度浮点寄存器 (s0-s31) -> r0
// 参数: r0 = 寄存器编号 (0-31)
// 使用跳转表将任意 s 寄存器的值读入 r0
	tbl_branch r0, r3, #3
	.fpu	vfpv2
	.irp	dr,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
1:	vmov	r0, s\dr
	ret	lr
	.org	1b + 8
	.endr
	.irp	dr,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
1:	vmov	r0, s\dr
	ret	lr
	.org	1b + 8
	.endr
ENDPROC(vfp_get_float)

ENTRY(vfp_put_float)
// 写入单精度浮点寄存器 (s0-s31)
// 参数: r0 = 要写入的值, r1 = 寄存器编号 (0-31)
// 使用跳转表将 r0 的值写入对应的 s 寄存器
	tbl_branch r1, r3, #3
	.fpu	vfpv2
	.irp	dr,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
1:	vmov	s\dr, r0
	ret	lr
	.org	1b + 8
	.endr
	.irp	dr,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
1:	vmov	s\dr, r0
	ret	lr
	.org	1b + 8
	.endr
ENDPROC(vfp_put_float)

ENTRY(vfp_get_double)
// 读取双精度浮点寄存器 (d0-d15, VFPv3 下含 d16-d31) -> {r0, r1}
// 参数: r0 = 寄存器编号
// 返回: r0:r1 = 64 位双精度浮点值
// 注意: 编号 16/32 为虚拟寄存器，用于与零比较 (返回 0)
	tbl_branch r0, r3, #3
	.fpu	vfpv2
	.irp	dr,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
1:	vmov	r0, r1, d\dr
	ret	lr
	.org	1b + 8
	.endr
#ifdef CONFIG_VFPv3
	@ d16 - d31 registers
	.fpu	vfpv3
	.irp	dr,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
1:	vmov	r0, r1, d\dr
	ret	lr
	.org	1b + 8
	.endr
#endif
	@ virtual register 16 (or 32 if VFPv3) for compare with zero
	mov	r0, #0
	mov	r1, #0
	ret	lr
ENDPROC(vfp_get_double)

ENTRY(vfp_put_double)
// 写入双精度浮点寄存器 (d0-d15, VFPv3 下含 d16-d31)
// 参数: r0:r1 = 64 位双精度值, r2 = 寄存器编号
	tbl_branch r2, r3, #3
	.fpu	vfpv2
	.irp	dr,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
1:	vmov	d\dr, r0, r1
	ret	lr
	.org	1b + 8
	.endr
#ifdef CONFIG_VFPv3
	.fpu	vfpv3
	@ d16 - d31 registers
	.irp	dr,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
1:	vmov	d\dr, r0, r1
	ret	lr
	.org	1b + 8
	.endr
#endif
ENDPROC(vfp_put_double)
