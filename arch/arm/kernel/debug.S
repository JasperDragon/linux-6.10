/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/kernel/debug.S
 *
 *  Copyright (C) 1994-1999 Russell King
 *
 *  32-bit debugging code
 */

//
// 文件: arch/arm/kernel/debug.S
// 描述: 内核早期调试输出例程
// 功能: 提供 printhex8/printhex4/printhex2、printascii、printch 等调试函数，
//       用于在 MMU/printk 尚未就绪时输出调试信息
// 注意: 仅供调试使用，不应包含在生产内核中
//
#include <linux/linkage.h>
#include <asm/assembler.h>

		.text

/*
 * Some debugging routines (useful if you've got MM problems and
 * printk isn't working).  For DEBUGGING ONLY!!!  Do not leave
 * references to these in a production kernel!
 */

#if !defined(CONFIG_DEBUG_SEMIHOSTING)
#include CONFIG_DEBUG_LL_INCLUDE
#endif

#ifdef CONFIG_MMU
		.macro	addruart_current, rx, tmp1, tmp2
		addruart	\tmp1, \tmp2, \rx
		mrc		p15, 0, \rx, c1, c0
		tst		\rx, #1
		moveq		\rx, \tmp1
		movne		\rx, \tmp2
		.endm

#else /* !CONFIG_MMU */
		.macro	addruart_current, rx, tmp1, tmp2
		addruart	\rx, \tmp1, \tmp2
		.endm

#endif /* CONFIG_MMU */

/*
 * Useful debugging routines
 */
ENTRY(printhex8)
// 以 8 位十六进制格式打印 r0 的值
		mov	r1, #8
		b	printhex
ENDPROC(printhex8)

ENTRY(printhex4)
// 以 4 位十六进制格式打印 r0 的值
		mov	r1, #4
		b	printhex
ENDPROC(printhex4)

ENTRY(printhex2)
// 以 2 位十六进制格式打印 r0 的值
		mov	r1, #2
printhex:	adr	r2, hexbuf_rel
		ldr	r3, [r2]
		add	r2, r2, r3
		add	r3, r2, r1
		mov	r1, #0
		strb	r1, [r3]
1:		and	r1, r0, #15
		mov	r0, r0, lsr #4
		cmp	r1, #10
		addlt	r1, r1, #'0'
		addge	r1, r1, #'a' - 10
		strb	r1, [r3, #-1]!
		teq	r3, r2
		bne	1b
		mov	r0, r2
		b	printascii
ENDPROC(printhex2)

		.pushsection .bss
hexbuf_addr:	.space 16
		.popsection
		.align
hexbuf_rel:	.long	hexbuf_addr - .

		.ltorg

#ifndef CONFIG_DEBUG_SEMIHOSTING

ENTRY(printascii)
// 通过串口输出以 NULL 结尾的 ASCII 字符串
// 参数: r0 = 字符串指针
// 自动将 \n 转换为 \r\n
		addruart_current r3, r1, r2
1:		teq	r0, #0
		ldrbne	r1, [r0], #1
		teqne	r1, #0
		reteq	lr
2:		teq     r1, #'\n'
		bne	3f
		mov	r1, #'\r'
#ifdef CONFIG_DEBUG_UART_FLOW_CONTROL
		waituartcts r2, r3
#endif
		waituarttxrdy r2, r3
		senduart r1, r3
		busyuart r2, r3
		mov	r1, #'\n'
3:
#ifdef CONFIG_DEBUG_UART_FLOW_CONTROL
		waituartcts r2, r3
#endif
		waituarttxrdy r2, r3
		senduart r1, r3
		busyuart r2, r3
		b	1b
ENDPROC(printascii)

ENTRY(printch)
// 通过串口输出单个字符
// 参数: r0 = 要输出的字符
		addruart_current r3, r1, r2
		mov	r1, r0
		mov	r0, #0
		b	2b
ENDPROC(printch)

#ifdef CONFIG_MMU
ENTRY(debug_ll_addr)
// 获取底层调试串口的 MMIO 物理/虚拟地址
// 参数: r0 = 物理地址存储指针, r1 = 虚拟地址存储指针
		addruart r2, r3, ip
		str	r2, [r0]
		str	r3, [r1]
		ret	lr
ENDPROC(debug_ll_addr)
#endif

#else

ENTRY(printascii)
// 使用半主机 (Semihosting) 方式输出字符串
// 通过 SYS_WRITE0 (0x04) 请求调试器代为输出
		mov	r1, r0
		mov	r0, #0x04		@ SYS_WRITE0
	ARM(	svc	#0x123456	)
#ifdef CONFIG_CPU_V7M
	THUMB(	bkpt	#0xab		)
#else
	THUMB(	svc	#0xab		)
#endif
		ret	lr
ENDPROC(printascii)

ENTRY(printch)
// 使用半主机方式输出单个字符 (SYS_WRITEC = 0x03)
		adr	r1, hexbuf_rel
		ldr	r2, [r1]
		add	r1, r1, r2
		strb	r0, [r1]
		mov	r0, #0x03		@ SYS_WRITEC
	ARM(	svc	#0x123456	)
#ifdef CONFIG_CPU_V7M
	THUMB(	bkpt	#0xab		)
#else
	THUMB(	svc	#0xab		)
#endif
		ret	lr
ENDPROC(printch)

ENTRY(debug_ll_addr)
// 半主机模式下不支持 LL debug，返回空地址
		mov	r2, #0
		str	r2, [r0]
		str	r2, [r1]
		ret	lr
ENDPROC(debug_ll_addr)

#endif
