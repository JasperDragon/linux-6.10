/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/kernel/head.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 *  Kernel startup code for all 32-bit CPUs
 *
 * 概述: head.S -- ARM32 Linux内核启动入口代码
 * 本文件包含ARM32 Linux内核的早期启动流程。
 * 主CPU从引导加载程序(或解压缩后的zImage)跳转至stext入口,
 * 依次完成:CPU类型检测(__lookup_processor_type)、
 * 初始页表创建(__create_page_tables)、MMU配置启用(__enable_mmu),
 * 最终跳转到__mmap_switched并进入start_kernel(C语言世界)。
 * 辅助CPU则通过secondary_startup完成类似但简化的流程。
 *
 * 关键约定(来自解压缩代码):
 *   r0 = 0
 *   r1 = machine nr (机器类型编号)
 *   r2 = atags或dtb指针
 *   MMU = off, D-cache = off, I-cache = 无所谓
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/pgtable.h>

#include <asm/assembler.h>
#include <asm/cp15.h>
#include <asm/domain.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include CONFIG_DEBUG_LL_INCLUDE
#include "head-common.S"
#include <asm/domain.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/page.h>
#include <asm/thread_info.h>

#if defined(CONFIG_DEBUG_LL) && !defined(CONFIG_DEBUG_SEMIHOSTING)
#include CONFIG_DEBUG_LL_INCLUDE
#endif
/*
 * swapper_pg_dir is the virtual address of the initial page table.
 * We place the page tables 16K below KERNEL_RAM_VADDR.  Therefore, we must
 * make sure that KERNEL_RAM_VADDR is correctly set.  Currently, we expect
 * the least significant 16 bits to be 0x8000, but we could probably
 * relax this restriction to KERNEL_RAM_VADDR >= PAGE_OFFSET + 0x4000.
 */
#define KERNEL_RAM_VADDR	(KERNEL_OFFSET + TEXT_OFFSET)
#if (KERNEL_RAM_VADDR & 0xffff) != 0x8000
#error KERNEL_RAM_VADDR must start at 0xXXXX8000
#endif

#ifdef CONFIG_ARM_LPAE
	/* LPAE requires an additional page for the PGD */
#define PG_DIR_SIZE	0x5000
#define PMD_ENTRY_ORDER	3	/* PMD entry size is 2^PMD_ENTRY_ORDER */
#else
#define PG_DIR_SIZE	0x4000
#define PMD_ENTRY_ORDER	2
#endif

/*
 * swapper_pg_dir: 内核初始化页表的虚拟地址
 * 该页表位于KERNEL_RAM_VADDR下方,大小为PG_DIR_SIZE。
 * 非LPAE: 0x4000(16KB), LPAE: 0x5000(20KB, 多一页给PGD)。
 * 该页表在内核初始化阶段使用,最终由paging_init接管。
 */
	.globl	swapper_pg_dir
	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - PG_DIR_SIZE

	/*
	 * This needs to be assigned at runtime when the linker symbols are
	 * resolved. These are unsigned 64bit really, but in this assembly code
	 * We store them as 32bit.
	 */
	.pushsection .data
	.align	2
	.globl	kernel_sec_start
	.globl	kernel_sec_end
kernel_sec_start:
	.long	0	/* 内核映像物理起始地址(运行时填充) */
	.long	0
kernel_sec_end:
	.long	0	/* 内核映像物理结束地址(运行时填充) */
	.long	0
	.popsection

/*
 * pgtbl 宏: 根据物理基址计算页表(PGD)的物理地址
 * @rd   = 输出寄存器,存放页表基址
 * @phys = 输入寄存器,存放物理内存基址(PHYS_OFFSET)
 * 计算过程: rd = phys + TEXT_OFFSET - PG_DIR_SIZE
 * 即内核映像起始地址下方PG_DIR_SIZE处存放页表
 */
	.macro	pgtbl, rd, phys
	add	\rd, \phys, #TEXT_OFFSET	@ rd = phys + TEXT_OFFSET
	sub	\rd, \rd, #PG_DIR_SIZE		@ rd = rd - PG_DIR_SIZE (页表位置)
	.endm

/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * This is normally called from the decompressor code.  The requirements
 * are: MMU = off, D-cache = off, I-cache = dont care, r0 = 0,
 * r1 = machine nr, r2 = atags or dtb pointer.
 *
 * This code is mostly position independent, so if you link the kernel at
 * 0xc0008000, you call this at __pa(0xc0008000).
 *
 * See linux/arch/arm/tools/mach-types for the complete list of machine
 * numbers for r1.
 *
 * We're trying to keep crap to a minimum; DO NOT add any machine specific
 * crap here - that's what the boot loader (or in extreme, well justified
 * circumstances, zImage) is for.
 *
 * 中文说明:
 * stext 是主CPU启动入口,由解压缩代码跳转至此。
 * 此时MMU关闭、D-cache关闭,寄存器r0=0, r1=机器类型, r2=atags/dtb。
 * 执行流程: CPU类型检测 -> LPAE检查 -> 初始页表创建 -> MMU启用
 */
	.arm

	__HEAD
ENTRY(stext)
	 ARM_BE8(setend	be )			@ 确保处于大端BE8模式(如果配置启用)

	 THUMB(	badr	r9, 1f		)	@ 内核以ARM状态进入
	 THUMB(	bx	r9		)	@ 如果是Thumb-2内核,切换到Thumb模式
	 THUMB(	.thumb			)	@ 后续指令使用Thumb编码
	 THUMB(1:			)

	#ifdef CONFIG_ARM_VIRT_EXT
		bl	__hyp_stub_install	@ 安装虚拟化扩展的Hyp模式存根
	#endif
		@ 确保处于SVC模式且所有中断被屏蔽
		safe_svcmode_maskall r9		@ 安全设置SVC模式,屏蔽IRQ/FIQ

		mrc	p15, 0, r9, c0, c0		@ 读取CP15的MIDR寄存器(CPU ID)
		bl	__lookup_processor_type		@ 查找匹配的处理器类型, r5=procinfo指针
		movs	r10, r5				@ r5=0表示未找到匹配处理器?
	 THUMB( it	eq )		@ force fixup-able long branch encoding
		beq	__error_p			@ 未识别处理器,跳转到错误处理

	#ifdef CONFIG_ARM_LPAE
		mrc	p15, 0, r3, c0, c1, 4		@ 读取ID_MMFR0寄存器(检查LPAE能力)
		and	r3, r3, #0xf			@ 提取VMSA支持字段
		cmp	r3, #5				@ 是否支持长描述符转换表格式(LPAE)?
	 THUMB( it	lo )		@ force fixup-able long branch encoding
		blo	__error_lpae			@ 不支持LPAE则跳转到错误处理
	#endif

	#ifndef CONFIG_XIP_KERNEL
		adr_l	r8, _text			@ 获取_text段的物理地址
		sub	r8, r8, #TEXT_OFFSET		@ PHYS_OFFSET = _text物理地址 - TEXT_OFFSET
	#else
		ldr	r8, =PLAT_PHYS_OFFSET		@ XIP内核: 物理地址为平台常量
	#endif

		/*
		 * 此时各寄存器含义:
		 * r1 = machine no (机器类型编号)
		 * r2 = atags或dtb指针(引导参数)
		 * r8 = phys_offset (物理内存基址)
		 * r9 = cpuid (CPU标识符)
		 * r10 = procinfo (处理器信息结构体指针)
		 */
		bl	__vet_atags			@ 验证atags/dtb指针的合法性
	#ifdef CONFIG_SMP_ON_UP
		bl	__fixup_smp			@ 修正SMP/UP兼容性
	#endif
	#ifdef CONFIG_ARM_PATCH_PHYS_VIRT
		bl	__fixup_pv_table		@ 修正物理/虚拟地址映射
	#endif
		bl	__create_page_tables		@ 创建初始页表

		/*
		 * The following calls CPU specific code in a position independent
		 * manner.  See arch/arm/mm/proc-*.S for details.  r10 = base of
		 * xxx_proc_info structure selected by __lookup_processor_type
		 * above.
		 *
		 * The processor init function will be called with:
		 *  r1 - machine type
		 *  r2 - boot data (atags/dt) pointer
		 *  r4 - translation table base (low word)
		 *  r5 - translation table base (high word, if LPAE)
		 *  r8 - translation table base 1 (pfn if LPAE)
		 *  r9 - cpuid
		 *  r13 - virtual address for __enable_mmu -> __turn_mmu_on
		 *
		 * On return, the CPU will be ready for the MMU to be turned on,
		 * r0 will hold the CPU control register value, r1, r2, r4, and
		 * r9 will be preserved.  r5 will also be preserved if LPAE.
		 */
		ldr	r13, =__mmap_switched		@ MMU启用后的跳转目标(虚拟地址)
							@ mmu has been enabled
		badr	lr, 1f				@ 返回地址(PIC, 位置无关)
	#ifdef CONFIG_ARM_LPAE
		mov	r5, #0				@ TTBR0高32位清零
		mov	r8, r4, lsr #12			@ TTBR1使用swapper_pg_dir的pfn
	#else
		mov	r8, r4				@ TTBR1指向swapper_pg_dir
	#endif
		ldr	r12, [r10, #PROCINFO_INITFUNC]	@ 加载处理器初始化函数地址
		add	r12, r12, r10			@ 转换为绝对地址
		ret	r12				@ 调用处理器初始化函数
	1:	b	__enable_mmu			@ 跳转到MMU启用过程
	ENDPROC(stext)
		.ltorg

/*
 * __create_page_tables: 创建初始页表
 * 建立最基础的页表映射,包括:
 *   1. __turn_mmu_on的恒等映射(identity mapping)
 *   2. 内核代码段的Section映射
 *   3. 引导参数(atags/dtb)的映射
 *   4. (可选)串口调试IO空间的映射
 *
 * 输入: r8 = phys_offset, r9 = cpuid, r10 = procinfo
 * 输出: r4 = 页表物理地址
 * 破坏: r0, r3, r5-r7
 */
	__create_page_tables:
		pgtbl	r4, r8				@ 计算页表物理地址

		/*
		 * Clear the swapper page table
		 * 将页表区域全部清零(每次4个字, 即16字节)
		 */
		mov	r0, r4
		mov	r3, #0
		add	r6, r0, #PG_DIR_SIZE
	1:	str	r3, [r0], #4			@ 清零4字节
		str	r3, [r0], #4			@ 清零4字节
		str	r3, [r0], #4			@ 清零4字节
		str	r3, [r0], #4			@ 清零4字节
		teq	r0, r6				@ 是否已清完整个页表区域?
		bne	1b				@ 否,继续清零

	#ifdef CONFIG_ARM_LPAE
		/*
		 * Build the PGD table (first level) to point to the PMD table. A PGD
		 * entry is 64-bit wide.
		 * LPAE: 构建第一级页表(PGD),指向PMD表,每个PGD条目64位
		 */
		mov	r0, r4
		add	r3, r4, #0x1000			@ 第一个PMD表的地址
		orr	r3, r3, #3			@ PGD块类型(有效+可访问)
		mov	r6, #4				@ PTRS_PER_PGD (4个PGD条目)
		mov	r7, #1 << (55 - 32)		@ L_PGD_SWAPPER标志
	1:
	#ifdef CONFIG_CPU_ENDIAN_BE8
		str	r7, [r0], #4			@ BE8: 先存高32位
		str	r3, [r0], #4			@ 再存低32位
	#else
		str	r3, [r0], #4			@ 小端: 先存低32位
		str	r7, [r0], #4			@ 再存高32位
	#endif
		add	r3, r3, #0x1000			@ 指向下一个PMD表
		subs	r6, r6, #1			@ PGD条目计数减1
		bne	1b

		add	r4, r4, #0x1000			@ r4指向PMD表起始位置
	#ifdef CONFIG_CPU_ENDIAN_BE8
		add	r4, r4, #4			@ BE8模式只写入低32位字
	#endif
	#endif

		ldr	r7, [r10, #PROCINFO_MM_MMUFLAGS] @ 加载MMU标志(页表属性)

		/*
		 * Create identity mapping to cater for __enable_mmu.
		 * This identity mapping will be removed by paging_init().
		 * 为__enable_mmu创建恒等映射(物理地址=虚拟地址),
		 * 以便启用MMU的瞬间指令执行不中断。
		 * 该映射后续会被paging_init()移除。
		 */
		adr_l	r5, __turn_mmu_on		@ 获取__turn_mmu_on的物理地址
		adr_l	r6, __turn_mmu_on_end		@ 获取结束地址
		mov	r5, r5, lsr #SECTION_SHIFT	@ 转换为Section编号
		mov	r6, r6, lsr #SECTION_SHIFT

	1:	orr	r3, r7, r5, lsl #SECTION_SHIFT	@ 组合标志+基址
		str	r3, [r4, r5, lsl #PMD_ENTRY_ORDER] @ 写入恒等映射
		cmp	r5, r6
		addlo	r5, r5, #1			@ 下一个Section
		blo	1b

		/*
		 * The main matter: map in the kernel using section mappings, and
		 * set two variables to indicate the physical start and end of the
		 * kernel.
		 * 主体工作: 使用Section映射映射内核映像,
		 * 并保存内核的物理起始和结束地址到全局变量。
		 */
		add	r0, r4, #KERNEL_OFFSET >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
		ldr	r6, =(_end - 1)

		/* For XIP, kernel_sec_start/kernel_sec_end are currently in RO memory */
	#ifndef CONFIG_XIP_KERNEL
		adr_l	r5, kernel_sec_start		@ 获取kernel_sec_start物理地址
	#if defined CONFIG_CPU_ENDIAN_BE8 || defined CONFIG_CPU_ENDIAN_BE32
		str	r8, [r5, #4]			@ 大端:保存内核物理起始地址
	#else
		str	r8, [r5]			@ 小端:保存内核物理起始地址
	#endif
	#endif
		orr	r3, r8, r7			@ 组合物理基址和MMU标志
		add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ENTRY_ORDER)
	1:	str	r3, [r0], #1 << PMD_ENTRY_ORDER	@ 写入页表项
		add	r3, r3, #1 << SECTION_SHIFT	@ 下一个Section的基址
		cmp	r0, r6				@ 是否已完成所有映射?
		bls	1b				@ 否,继续
	#ifndef CONFIG_XIP_KERNEL
		eor	r3, r3, r7			@ 移除MMU标志,得到纯物理地址
		adr_l	r5, kernel_sec_end		@ 获取kernel_sec_end物理地址
	#if defined CONFIG_CPU_ENDIAN_BE8 || defined CONFIG_CPU_ENDIAN_BE32
		str	r3, [r5, #4]			@ 大端:保存内核物理结束地址
	#else
		str	r3, [r5]			@ 小端:保存内核物理结束地址
	#endif
	#else
		/*
		 * Map the kernel image separately as it is not located in RAM.
		 * XIP(就地执行内核): 内核映像不在RAM中,需要单独映射
		 */
	#define XIP_START XIP_VIRT_ADDR(CONFIG_XIP_PHYS_ADDR)
		mov	r3, pc
		mov	r3, r3, lsr #SECTION_SHIFT
		orr	r3, r7, r3, lsl #SECTION_SHIFT
		add	r0, r4,  #(XIP_START & 0xff000000) >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
		str	r3, [r0, #((XIP_START & 0x00f00000) >> SECTION_SHIFT) << PMD_ENTRY_ORDER]!
		ldr	r6, =(_edata_loc - 1)
		add	r0, r0, #1 << PMD_ENTRY_ORDER
		add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ENTRY_ORDER)
	1:	cmp	r0, r6
		add	r3, r3, #1 << SECTION_SHIFT
		strls	r3, [r0], #1 << PMD_ENTRY_ORDER
		bls	1b
	#endif

		/*
		 * Then map boot params address in r2 if specified.
		 * We map 2 sections in case the ATAGs/DTB crosses a section boundary.
		 * 映射引导参数(atags/dtb)地址,映射2个Section以防止参数跨边界。
		 */
		mov	r0, r2, lsr #SECTION_SHIFT
		cmp	r2, #0
		ldrne	r3, =FDT_FIXED_BASE >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
		addne	r3, r3, r4
		orrne	r6, r7, r0, lsl #SECTION_SHIFT
		strne	r6, [r3], #1 << PMD_ENTRY_ORDER	@ 写入第一个Section映射
		addne	r6, r6, #1 << SECTION_SHIFT
		strne	r6, [r3]			@ 写入第二个Section映射

	#if defined(CONFIG_ARM_LPAE) && defined(CONFIG_CPU_ENDIAN_BE8)
		sub	r4, r4, #4			@ 修正页表指针,适应64位描述符
							@ for 64-bit descriptors
	#endif

	#ifdef CONFIG_DEBUG_LL
	#if !defined(CONFIG_DEBUG_ICEDCC) && !defined(CONFIG_DEBUG_SEMIHOSTING)
		/*
		 * Map in IO space for serial debugging.
		 * This allows debug messages to be output
		 * via a serial console before paging_init.
		 * 映射串口调试IO空间,使得paging_init之前就能输出调试信息。
		 */
		addruart r7, r3, r0

		mov	r3, r3, lsr #SECTION_SHIFT
		mov	r3, r3, lsl #PMD_ENTRY_ORDER

		add	r0, r4, r3
		mov	r3, r7, lsr #SECTION_SHIFT
		ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags (IO空间MMU标志)
		orr	r3, r7, r3, lsl #SECTION_SHIFT
	#ifdef CONFIG_ARM_LPAE
		mov	r7, #1 << (54 - 32)		@ XN (不可执行标志)
	#ifdef CONFIG_CPU_ENDIAN_BE8
		str	r7, [r0], #4
		str	r3, [r0], #4
	#else
		str	r3, [r0], #4
		str	r7, [r0], #4
	#endif
	#else
		orr	r3, r3, #PMD_SECT_XN		@ 设置不可执行位
		str	r3, [r0], #4
	#endif

	#else /* CONFIG_DEBUG_ICEDCC || CONFIG_DEBUG_SEMIHOSTING */
		/* we don't need any serial debugging mappings */
		ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
	#endif

	#if defined(CONFIG_ARCH_NETWINDER)
		/*
		 * If we're using the NetWinder or CATS, we also need to map
		 * in the 16550-type serial port for the debug messages
		 * NetWinder/CATS平台: 额外映射16550串口调试端口
		 */
		add	r0, r4, #0xff000000 >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
		orr	r3, r7, #0x7c000000
		str	r3, [r0]
	#endif
	#ifdef CONFIG_ARCH_RPC
		/*
		 * Map in screen at 0x02000000 & SCREEN2_BASE
		 * Similar reasons here - for debug.  This is
		 * only for Acorn RiscPC architectures.
		 * Acorn RiscPC架构: 映射屏幕显示缓冲区用于调试
		 */
		add	r0, r4, #0x02000000 >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
		orr	r3, r7, #0x02000000
		str	r3, [r0]
		add	r0, r4, #0xd8000000 >> (SECTION_SHIFT - PMD_ENTRY_ORDER)
		str	r3, [r0]
	#endif
	#endif
	#ifdef CONFIG_ARM_LPAE
		sub	r4, r4, #0x1000		@ LPAE: 回退r4到PGD表起始位置
	#endif
		ret	lr			@ 页表创建完成,返回
	ENDPROC(__create_page_tables)
		.ltorg

	#if defined(CONFIG_SMP)
		.text
		.arm
/*
 * secondary_startup_arm / secondary_startup: 辅助CPU启动入口
 * 在SMP系统中,非主CPU(辅助CPU)通过此入口启动。
 * 与主CPU启动类似但简化:
 * - 不需要验证机器类型(已由主CPU验证)
 * - 页表由主CPU通过__cpu_up提供
 */
	ENTRY(secondary_startup_arm)
	 THUMB(	badr	r9, 1f		)	@ Kernel is entered in ARM.
	 THUMB(	bx	r9		)	@ If this is a Thumb-2 kernel,
	 THUMB(	.thumb			)	@ switch to Thumb now.
	 THUMB(1:			)
	ENTRY(secondary_startup)
		/*
		 * Common entry point for secondary CPUs.
		 *
		 * Ensure that we're in SVC mode, and IRQs are disabled.  Lookup
		 * the processor type - there is no need to check the machine type
		 * as it has already been validated by the primary processor.
		 */

	 ARM_BE8(setend	be)				@ ensure we are in BE8 mode

	#ifdef CONFIG_ARM_VIRT_EXT
		bl	__hyp_stub_install_secondary	@ 安装辅助CPU的Hyp存根
	#endif
		safe_svcmode_maskall r9			@ 安全设置SVC模式,屏蔽中断

		mrc	p15, 0, r9, c0, c0		@ 读取CPU ID
		bl	__lookup_processor_type		@ 查找处理器类型
		movs	r10, r5				@ 未找到?
		moveq	r0, #'p'			@ 是,返回错误字符'p'
	 THUMB( it	eq )		@ force fixup-able long branch encoding
		beq	__error_p

		/*
		 * Use the page tables supplied from  __cpu_up.
		 * 使用主CPU通过__cpu_up提供的页表
		 */
	#ifdef CONFIG_XIP_KERNEL
		ldr	r3, =(secondary_data + PLAT_PHYS_OFFSET - PAGE_OFFSET)
	#else
		adr_l	r3, secondary_data
	#endif
		mov_l	r12, __secondary_switched	@ 设置最终跳转目标
		ldrd	r4, r5, [r3, #0]		@ 获取secondary_data.pgdir
	ARM_BE8(eor	r4, r4, r5)			@ 大端模式下交换r4/r5
	ARM_BE8(eor	r5, r4, r5)			@ 三步交换法
	ARM_BE8(eor	r4, r4, r5)			@ 无需临时寄存器
		ldr	r8, [r3, #8]			@ 获取secondary_data.swapper_pg_dir
		badr	lr, __enable_mmu		@ 设置返回地址(启用MMU)
		mov	r13, r12			@ __secondary_switched地址
		ldr	r12, [r10, #PROCINFO_INITFUNC]	@ 加载处理器初始化函数
		add	r12, r12, r10			@ 转换为绝对地址
							@ (return control reg)
		ret	r12				@ 调用处理器初始化函数
	ENDPROC(secondary_startup)
	ENDPROC(secondary_startup_arm)

/*
 * __secondary_switched: 辅助CPU完成MMU切换后的入口
 * 切换到内核页表,设置栈指针,然后跳转到secondary_start_kernel
 */
	ENTRY(__secondary_switched)
	#if defined(CONFIG_VMAP_STACK) && !defined(CONFIG_ARM_LPAE)
		@ Before using the vmap'ed stack, we have to switch to swapper_pg_dir
		@ as the ID map does not cover the vmalloc region.
		@ 使用vmalloc栈之前,必须切换到swapper_pg_dir,
		@ 因为恒等映射不覆盖vmalloc区域。
		mrc	p15, 0, ip, c2, c0, 1	@ read TTBR1
		mcr	p15, 0, ip, c2, c0, 0	@ set TTBR0
		instr_sync
	#endif
		adr_l	r7, secondary_data + 12		@ 获取secondary_data.stack
		ldr	sp, [r7]			@ 设置栈指针
		ldr	r0, [r7, #4]			@ 获取secondary_data.task
		mov	fp, #0				@ 栈帧指针清零
		b	secondary_start_kernel		@ 跳转到C语言入口
	ENDPROC(__secondary_switched)

	#endif /* defined(CONFIG_SMP) */



/*
 * __enable_mmu: 配置系统寄存器并最终启用MMU
 * 在调用处理器初始化函数之后,启用MMU之前的最后设置。
 * 加载页表指针(TTBR)和域访问寄存器(DACR),
 * 然后跳转到__turn_mmu_on执行最终的MMU启用操作。
 *
 * 输入:
 *   r0  = cp#15 control register (控制寄存器值)
 *   r1  = machine ID (机器ID)
 *   r2  = atags or dtb pointer (引导参数指针)
 *   r4  = TTBR pointer (low word) (页表基址低32位)
 *   r5  = TTBR pointer (high word if LPAE) (页表基址高32位, 仅LPAE)
 *   r9  = processor ID (处理器ID)
 *   r13 = *virtual* address to jump to upon completion (跳转目标虚拟地址)
 */
	__enable_mmu:
	#if defined(CONFIG_ALIGNMENT_TRAP) && __LINUX_ARM_ARCH__ < 6
		orr	r0, r0, #CR_A		@ ARMv6以下: 使能对齐陷阱
	#else
		bic	r0, r0, #CR_A		@ ARMv6及以上: 关闭对齐陷阱
	#endif
	#ifdef CONFIG_CPU_DCACHE_DISABLE
		bic	r0, r0, #CR_C		@ 关闭D-cache(如果配置)
	#endif
	#ifdef CONFIG_CPU_BPREDICT_DISABLE
		bic	r0, r0, #CR_Z		@ 关闭分支预测(如果配置)
	#endif
	#ifdef CONFIG_CPU_ICACHE_DISABLE
		bic	r0, r0, #CR_I		@ 关闭I-cache(如果配置)
	#endif
	#ifdef CONFIG_ARM_LPAE
		mcrr	p15, 0, r4, r5, c2		@ LPAE: 加载TTBR0 (64位)
	#else
		mov	r5, #DACR_INIT			@ 初始化域访问寄存器值
		mcr	p15, 0, r5, c3, c0, 0		@ 写入域访问寄存器
		mcr	p15, 0, r4, c2, c0, 0		@ 写入页表基址寄存器(TTBR0)
	#endif
		b	__turn_mmu_on			@ 跳转到最终的MMU开启操作
	ENDPROC(__enable_mmu)

/*
 * __turn_mmu_on: 最终开启MMU的临界区代码。
 * 这是一个极其关键的操作——写入控制寄存器后,内存空间结构完全改变,
 * 后续指令将使用虚拟地址。此代码位于.idmap.text节(恒等映射区域),
 * 确保物理地址和虚拟地址一致。
 *
 * 输入:
 *   r0  = cp#15 control register (已配置好的控制寄存器值)
 *   r13 = *virtual* address to jump to upon completion (跳转目标)
 *
 * 注意: 写入MMU控制寄存器后不能直接跟踪执行流,
 *       因为地址空间已彻底改变。
 */
		.align	5
		.pushsection	.idmap.text, "ax"
	ENTRY(__turn_mmu_on)
		mov	r0, r0			@ NOP: 流水线停顿
		instr_sync			@ 指令同步屏障
		mcr	p15, 0, r0, c1, c0, 0		@ 写入系统控制寄存器(SCTLR),
							@ 此操作真正开启MMU!
		mrc	p15, 0, r3, c0, c0, 0		@ 读取MIDR(确保流水线已排空)
		instr_sync			@ 指令同步屏障
		mov	r3, r3			@ NOP: 额外停顿
		mov	r3, r13			@ 加载跳转目标(虚拟地址)
		ret	r3			@ 跳转到虚拟地址(此时MMU已开启)
	__turn_mmu_on_end:
	ENDPROC(__turn_mmu_on)
		.popsection


	#ifdef CONFIG_SMP_ON_UP
		__HEAD
/*
 * __fixup_smp: SMP/UP兼容性修正
 * 当SMP内核运行在单处理器(UP)系统上时,需要修正SMP-specific指令。
 * 检测流程:
 *   1. 检查CPU架构版本是否支持CPU ID
 *   2. 检查是否为ARM 11MPCore(原生支持SMP)
 *   3. 检查MPIDR多处理器扩展标志
 *   4. 特殊处理Aegis SoC(Cortex-A9单核但SMP操作会出错)
 */
	__fixup_smp:
		and	r3, r9, #0x000f0000	@ 提取架构版本号
		teq	r3, #0x000f0000		@ 是否支持CPU ID?
		bne	__fixup_smp_on_up	@ 不支持, 假设为UP

		bic	r3, r9, #0x00ff0000	@ 清除implementer和arch字段
		bic	r3, r3, #0x0000000f	@ 清除第4版号, mask 0xff00fff0
		mov	r4, #0x41000000
		orr	r4, r4, #0x0000b000
		orr	r4, r4, #0x00000020	@ val 0x4100b020 (ARM 11MPCore)
		teq	r3, r4			@ 是ARM 11MPCore?
		reteq	lr			@ 是, 保持SMP设置

		mrc	p15, 0, r0, c0, c0, 5	@ 读取MPIDR寄存器
		and	r0, r0, #0xc0000000	@ 提取多处理器扩展标志
		teq	r0, #0x80000000		@ 属于SMP系统且非单处理器?
		bne    __fixup_smp_on_up	@ 否, 假设为UP

		@ Core indicates it is SMP. Check for Aegis SOC where a single
		@ Cortex-A9 CPU is present but SMP operations fault.
		@ CPU表示支持SMP. 但检查Aegis SoC的特殊情况:
		@ 该SoC使用单核Cortex-A9但SMP操作会出错。
		mov	r4, #0x41000000
		orr	r4, r4, #0x0000c000
		orr	r4, r4, #0x00000090
		teq	r3, r4			@ 检查是否为ARM Cortex-A9
		retne	lr			@ 不是Cortex-A9, 保持SMP

		@ If a future SoC *does* use 0x0 as the PERIPH_BASE, then the
		@ below address check will need to be #ifdef'd or equivalent
		@ for the Aegis platform.
		mrc	p15, 4, r0, c15, c0	@ 读取SCU(嗅探控制单元)基址
		teq	r0, #0x0		@ '0'表示真正的UP A9硬件
		beq	__fixup_smp_on_up	@ 是, 修正为UP
		ldr	r0, [r0, #4]		@ 读取SCU配置寄存器
	ARM_BE8(rev	r0, r0)			@ 大端模式下字节交换
		and	r0, r0, #0x3		@ 提取CPU数量
		teq	r0, #0x0		@ 只有1个CPU?
		retne	lr			@ 不止1个, 保持SMP

	__fixup_smp_on_up:
		adr_l	r4, __smpalt_begin	@ SMP替换区域起始
		adr_l	r5, __smpalt_end	@ SMP替换区域结束
		b	__do_fixup_smp_on_up	@ 执行修正
	ENDPROC(__fixup_smp)

		.pushsection .data
		.align	2
		.globl	smp_on_up
	smp_on_up:
		ALT_SMP(.long	1)		@ SMP: 值为1(未修正)
		ALT_UP(.long	0)		@ UP: 值为0(已修正为UP)
		.popsection
	#endif

		.text
/*
 * __do_fixup_smp_on_up: 执行SMP指令修正
 * 遍历__smpalt_begin到__smpalt_end区域,
 * 将SMP版本的指令替换为UP版本。
 */
	__do_fixup_smp_on_up:
		cmp	r4, r5			@ 是否已完成所有修正?
		reths	lr			@ 是, 返回
		ldmia	r4, {r0, r6}		@ 加载目标地址(r0)和修正值(r6)
	 ARM(	str	r6, [r0, r4]	)	@ ARM: 直接写入修正指令
	 THUMB(	add	r0, r0, r4	)	@ Thumb: 计算绝对地址
		add	r4, r4, #8		@ 移动到下一个修正条目
	#ifdef __ARMEB__
	 THUMB(	mov	r6, r6, ror #16	)	@ 大端: 调整字顺序
	#endif
	 THUMB(	strh	r6, [r0], #2	)	@ Thumb-2: 存储高半字
	 THUMB(	mov	r6, r6, lsr #16	)	@ 准备低半字
	 THUMB(	strh	r6, [r0]	)	@ Thumb-2: 存储低半字
		b	__do_fixup_smp_on_up	@ 继续处理下一个
	ENDPROC(__do_fixup_smp_on_up)

/*
 * fixup_smp: C语言可调用的SMP指令修正函数
 * 参数: r0 = 起始地址, r1 = 大小
 */
	ENTRY(fixup_smp)
		stmfd	sp!, {r4 - r6, lr}
		mov	r4, r0			@ 起始地址
		add	r5, r0, r1		@ 结束地址
		bl	__do_fixup_smp_on_up
		ldmfd	sp!, {r4 - r6, pc}
	ENDPROC(fixup_smp)

	#include "head-common.S"
