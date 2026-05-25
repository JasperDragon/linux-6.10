/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/kernel/head-common.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * 概述: head-common.S -- ARM32内核启动公共代码
 * 本文件通过 #include 被 head.S 包含,提供内核启动过程中的公共函数。
 * 主要内容:
 *   __vet_atags        - 验证引导参数(atags/dtb)指针的合法性
 *   __mmap_switched    - MMU启用后的跳转目标,完成BSS清零、数据复制、
 *                        保存处理器ID/机器类型/atags指针,然后进入start_kernel
 *   __lookup_processor_type - 根据CPUID在链接器构建的处理器列表中查找匹配项
 *   __error_p          - 处理器类型不匹配时的错误处理
 *   __error_lpae       - LPAE不支持时的错误处理
 */
#include <asm/assembler.h>

#define ATAG_CORE 0x54410001
#define ATAG_CORE_SIZE ((2*4 + 3*4) >> 2)
#define ATAG_CORE_SIZE_EMPTY ((2*4) >> 2)

#ifdef CONFIG_CPU_BIG_ENDIAN
#define OF_DT_MAGIC 0xd00dfeed
#else
#define OF_DT_MAGIC 0xedfe0dd0 /* 0xd00dfeed in big-endian */
#endif

/*
 * Exception handling.  Something went wrong and we can't proceed.  We
 * ought to tell the user, but since we don't have any guarantee that
 * we're even running on the right architecture, we do virtually nothing.
 *
 * If CONFIG_DEBUG_LL is set we try to print out something about the error
 * and hope for the best (useful if bootloader fails to pass a proper
 * machine ID for example).
 *
 * 错误处理: 当启动过程中出现严重错误时,由于无法保证我们运行在正确的
 * 架构上,几乎不做任何操作。如果启用了DEBUG_LL,会尝试输出错误信息。
 */
	__HEAD

/*
 * __vet_atags: 验证r2中atags/dtb指针的合法性
 * 启发式验证条件:
 *   1. 指针必须4字节对齐
 *   2. 指针必须在物理内存的前16KB范围内
 *   3. 如果是ATAG格式,第一个tag必须是ATAG_CORE
 *   4. 如果启用CONFIG_OF_FLATTREE,也接受DTB格式(magic 0xedfe0dd0)
 *
 * 输入: r2 = atags或dtb指针(待验证)
 * 输出: r2 = 有效的atags/dtb指针,或0(无效)
 * 破坏: r5, r6
 */
__vet_atags:
		tst	r2, #0x3			@ 检查指针是否4字节对齐?
		bne	1f				@ 不对齐, 无效

		ldr	r5, [r2, #0]			@ 读取第一个tag/magic
	#ifdef CONFIG_OF_FLATTREE
		ldr	r6, =OF_DT_MAGIC		@ 检查是否为DTB magic
		cmp	r5, r6
		beq	2f				@ 是DTB, 合法
	#endif
		cmp	r5, #ATAG_CORE_SIZE		@ 检查第一个tag大小是否为ATAG_CORE
		cmpne	r5, #ATAG_CORE_SIZE_EMPTY	@ 或空的ATAG_CORE
		bne	1f				@ 都不是, 无效
		ldr	r5, [r2, #4]			@ 检查tag类型
		ldr	r6, =ATAG_CORE
		cmp	r5, r6
		bne	1f				@ 类型不匹配, 无效

	2:	ret	lr				@ atags/dtb指针合法, 返回

	1:	mov	r2, #0				@ 指针无效, 置为0
		ret	lr
	ENDPROC(__vet_atags)

/*
 * __mmap_switched: MMU启用后的跳转目标
 * 此时CPU已运行在虚拟地址空间。本函数完成:
 *   1. 保存机器ID和atags指针
 *   2. (XIP内核)解压或复制.data段到RAM
 *   3. 清零.bss段
 *   4. 设置初始进程(swapper)的线程信息
 *   5. 保存处理器ID、机器类型、atags指针到全局变量
 *   6. 跳转到start_kernel(C语言入口)
 *
 * 输入:
 *   r0  = cp#15 control register (或M-class的exc_ret)
 *   r1  = machine ID (机器ID)
 *   r2  = atags/dtb pointer (引导参数指针)
 *   r9  = processor ID (处理器ID)
 */
		__INIT
	__mmap_switched:

		mov	r7, r1				@ 保存机器ID到r7
		mov	r8, r2				@ 保存atags指针到r8
		mov	r10, r0				@ 保存控制寄存器值到r10

		adr	r4, __mmap_switched_data	@ 获取数据表地址
		mov	fp, #0				@ 栈帧指针清零

	#if defined(CONFIG_XIP_DEFLATED_DATA)
	   ARM(	ldr	sp, [r4], #4 )			@ 加载临时栈指针
	 THUMB(	ldr	sp, [r4] )
	 THUMB(	add	r4, #4 )
		bl	__inflate_kernel_data		@ 解压.data段到RAM
		teq	r0, #0
		bne	__error				@ 解压失败
	#elif defined(CONFIG_XIP_KERNEL)
	   ARM(	ldmia	r4!, {r0, r1, r2, sp} )	@ 加载数据段参数和栈指针
	 THUMB(	ldmia	r4!, {r0, r1, r2, r3} )
	 THUMB(	mov	sp, r3 )
		sub	r2, r2, r1			@ 计算数据段大小
		bl	__memcpy			@ 复制.data段到RAM
	#endif

	   ARM(	ldmia	r4!, {r0, r1, sp} )		@ 加载BSS段起止地址和栈指针
	 THUMB(	ldmia	r4!, {r0, r1, r3} )
	 THUMB(	mov	sp, r3 )
		sub	r2, r1, r0			@ 计算BSS段大小
		mov	r1, #0				@ 要填充的值(0)
		bl	__memset			@ 清零.bss段

		adr_l	r0, init_task			@ 获取swapper task_struct地址
		set_current r0, r1			@ 设置current指针

		ldmia	r4, {r0, r1, r2, r3}		@ 加载全局变量地址表
		str	r9, [r0]			@ 保存处理器ID到processor_id
		str	r7, [r1]			@ 保存机器类型到__machine_arch_type
		str	r8, [r2]			@ 保存atags指针到__atags_pointer
		cmp	r3, #0
		strne	r10, [r3]			@ 保存控制寄存器值到cr_alignment
	#ifdef CONFIG_KASAN
		bl	kasan_early_init		@ KASAN早期初始化
	#endif
		mov	lr, #0				@ 清理返回地址
		b	start_kernel			@ 跳转到C语言入口, 不返回
	ENDPROC(__mmap_switched)

		.align	2
		.type	__mmap_switched_data, %object
/*
 * __mmap_switched_data: __mmap_switched使用的数据表
 * 包含BSS段地址、栈指针、全局变量地址等信息。
 * XIP内核还包含数据段地址信息。
 */
	__mmap_switched_data:
	#ifdef CONFIG_XIP_KERNEL
	#ifndef CONFIG_XIP_DEFLATED_DATA
		.long	_sdata				@ r0: 数据段源起始(Flash中)
		.long	__data_loc			@ r1: 数据段目标起始(RAM中)
		.long	_edata_loc			@ r2: 数据段结束
	#endif
		.long	__bss_stop			@ sp: 临时栈(在BSS末尾)
	#endif

		.long	__bss_start			@ r0: BSS段起始
		.long	__bss_stop			@ r1: BSS段结束
		.long	init_thread_union + THREAD_START_SP @ sp: 初始内核栈

		.long	processor_id			@ r0: 保存processor_id的地址
		.long	__machine_arch_type		@ r1: 保存__machine_arch_type的地址
		.long	__atags_pointer			@ r2: 保存__atags_pointer的地址
	#ifdef CONFIG_CPU_CP15
		.long	cr_alignment			@ r3: 保存cr_alignment的地址
	#else
	M_CLASS(.long	exc_ret)			@ r3: M-class: 保存exc_ret
	AR_CLASS(.long	0)				@ r3: A/R-class: 占位
	#endif
		.size	__mmap_switched_data, . - __mmap_switched_data

		__FINIT
		.text

/*
 * lookup_processor_type: C语言可调用的处理器类型查找函数
 * 参数: r0 = CPU ID (MIDR值)
 * 返回: r0 = 匹配的proc_info指针, 或0(未找到)
 */
ENTRY(lookup_processor_type)
		stmfd	sp!, {r4 - r6, r9, lr}
		mov	r9, r0				@ CPU ID存入r9
		bl	__lookup_processor_type		@ 调用底层查找函数
		mov	r0, r5				@ 返回procinfo指针
		ldmfd	sp!, {r4 - r6, r9, pc}
	ENDPROC(lookup_processor_type)

/*
 * __lookup_processor_type: 根据CPU ID查找匹配的处理器信息结构
 * 遍历__proc_info_begin到__proc_info_end之间的proc_info_list结构,
 * 使用每个结构中的mask和value字段与CPU ID进行匹配。
 * 注意: 由于此时MMU可能未开启,不能使用绝对地址,必须计算偏移。
 *
 * 输入: r9 = cpuid (CPU标识符)
 * 输出: r5 = proc_info指针(物理地址空间), 0=未找到
 * 破坏: r3, r4, r6
 * 保留: r9 = cpuid
 */
	__lookup_processor_type:
		/*
		 * Look in <asm/procinfo.h> for information about the __proc_info
		 * structure.
		 */
		adr_l	r5, __proc_info_begin		@ 处理器信息列表起始
		adr_l	r6, __proc_info_end		@ 处理器信息列表结束
	1:	ldmia	r5, {r3, r4}			@ 加载value(CPU ID)和mask
		and	r4, r4, r9			@ 用mask提取CPU ID相关位
		teq	r3, r4				@ 与预期value匹配?
		beq	2f				@ 匹配成功
		add	r5, r5, #PROC_INFO_SZ		@ 不匹配, 移动到下一个条目
		cmp	r5, r6
		blo	1b				@ 继续查找
		mov	r5, #0				@ 未找到, 返回0
	2:	ret	lr				@ 返回匹配结果
	ENDPROC(__lookup_processor_type)

/*
 * __error_lpae: LPAE配置但CPU不支持的错误处理
 * 当内核配置了ARM_LPAE但当前CPU不支持长描述符页表格式时调用。
 */
	__error_lpae:
	#ifdef CONFIG_DEBUG_LL
		adr	r0, str_lpae
		bl 	printascii			@ 打印错误信息
		b	__error				@ 进入死循环
	str_lpae: .asciz "\nError: Kernel with LPAE support, but CPU does not support LPAE.\n"
	#else
		b	__error				@ 直接进入死循环
	#endif
		.align
	ENDPROC(__error_lpae)

/*
 * __error_p: 未识别的处理器变种错误处理
 * 当CPU类型无法匹配proc_info列表中的任何条目时调用。
 */
	__error_p:
	#ifdef CONFIG_DEBUG_LL
		adr	r0, str_p1
		bl	printascii			@ 打印错误前缀
		mov	r0, r9				@ 打印CPU ID
		bl	printhex8
		adr	r0, str_p2
		bl	printascii			@ 打印错误后缀
		b	__error				@ 进入死循环
	str_p1:	.asciz	"\nError: unrecognized/unsupported processor variant (0x"
	str_p2:	.asciz	").\n"
		.align
	#endif
	ENDPROC(__error_p)

/*
 * __error: 致命错误,进入无限循环
 * 如果配置了ARCH_RPC,会先尝试将屏幕设置为红色作为视觉提示。
 */
	__error:
	#ifdef CONFIG_ARCH_RPC
	/*
	 * Turn the screen red on a error - RiscPC only.
	 * RiscPC平台: 将屏幕设为红色以提示错误
	 */
		mov	r0, #0x02000000
		mov	r3, #0x11
		orr	r3, r3, r3, lsl #8
		orr	r3, r3, r3, lsl #16
		str	r3, [r0], #4
		str	r3, [r0], #4
		str	r3, [r0], #4
		str	r3, [r0], #4
	#endif
	1:	mov	r0, r0				@ NOP: 空操作
		b	1b				@ 无限循环
	ENDPROC(__error)
