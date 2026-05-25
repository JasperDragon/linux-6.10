/* SPDX-License-Identifier: GPL-2.0 */
//
// 文件: arch/arm/boot/compressed/debug.S
// 描述: zImage 解压缩期间的早期调试输出支持
// 功能: 提供 putc() 函数用于在解压缩阶段向串口输出调试字符
// 路径: 通过 CONFIG_DEBUG_LL_INCLUDE 引用底层串口操作，
//       或者通过半主机 (semihosting) 方式输出
//
#include <linux/linkage.h>
#include <asm/assembler.h>

#ifndef CONFIG_DEBUG_SEMIHOSTING

#include CONFIG_DEBUG_LL_INCLUDE

ENTRY(putc)
// 使用底层串口 (UART) 输出一个字符
// 参数: r0 = 要输出的字符
// 步骤: 获取串口地址 -> 等待 CTS -> 等待发送就绪 -> 发送 -> 等待完成
	addruart r1, r2, r3
#ifdef CONFIG_DEBUG_UART_FLOW_CONTROL
	waituartcts r3, r1
#endif
	waituarttxrdy r3, r1
	senduart r0, r1
	busyuart r3, r1
	mov	 pc, lr
ENDPROC(putc)

#else

ENTRY(putc)
// 使用半主机 (Semihosting) 方式输出一个字符
// 通过 SVC 指令请求调试器 (如 JTAG) 代为输出
	adr	r1, 1f
	ldmia	r1, {r2, r3}
	add	r2, r2, r1
	ldr	r1, [r2, r3]
	strb	r0, [r1]
	mov	r0, #0x03		@ SYS_WRITEC
   ARM(	svc	#0x123456	)
#ifdef CONFIG_CPU_V7M
 THUMB(	bkpt	#0xab		)
#else
 THUMB(	svc	#0xab		)
#endif
	mov	pc, lr
	.align	2
1:	.word	_GLOBAL_OFFSET_TABLE_ - .
	.word	semi_writec_buf(GOT)
ENDPROC(putc)

	.bss
	.global	semi_writec_buf
	.type   semi_writec_buf, %object
semi_writec_buf:
	.space	4
	.size	semi_writec_buf, 4

#endif
