/* SPDX-License-Identifier: GPL-2.0 */
// ============================================================================
// 文件概述: ARM 处理器通用宏定义
// 该文件提供了 ARM 架构处理器支持中广泛使用的汇编宏, 被所有 ARM MM 汇编文件
// 通过 `#include "proc-macros.S"` 包含使用。
// 主要功能:
//   1. 内存描述符访问宏:
//      - vma_vm_mm: 从 vma 获取 mm_struct 指针
//      - vma_vm_flags: 从 vma 获取 vm_flags
//      - act_mm: 获取当前进程的 active_mm
//      - mmid: 获取 mm 的 context.id(ASID)
//      - asid: 从 context ID 提取 ASID(低 8 位)
//   2. 缓存系统操作宏:
//      - dcache_line_size: 动态读取 ARMv7 D-cache 行大小
//      - icache_line_size: 动态读取 ARMv7 I-cache 行大小
//      - crval: 生成 SCTLR 控制位掩码
//   3. 页表设置宏(PTE 权限翻译):
//      - armv6_mt_table: ARMv6/ARMv7 内存类型映射表
//      - armv6_set_pte_ext: ARMv6/ARMv7 页表项设置(含权限位翻译)
//      - armv3_set_pte_ext: ARMv3/ARMv4/ARMv5 页表项设置
//      - xscale_set_pte_ext: Xscale 页表项设置(分两部分)
//   4. 处理器功能表定义宏:
//      - define_processor_functions: 生成 processor 函数表
//      - globl_equ: 全局别名定义
//      - initfn: 计算初始化函数偏移
//   5. 内存保护区域宏:
//      - pr_sz: 计算保护区域大小的 log2 值
//      - pr_val: 生成保护区域寄存器值
// 该文件是连接 ARM MM 汇编代码和内核 C 代码的关键桥梁。
// ============================================================================
/*
 * We need constants.h for:
 *  VMA_VM_MM
 *  VMA_VM_FLAGS
 *  VM_EXEC
 */
#include <asm/asm-offsets.h>
#include <asm/pgtable.h>
#include <asm/thread_info.h>

#ifdef CONFIG_CPU_V7M
#include <asm/v7m.h>
#endif

// ============================================================================
// 内存描述符访问宏集合
// 这些宏用于从 task_struct/vm_area_struct 等内核数据结构中提取 MM 信息，
// 包括 mm_struct 指针、vm_flags、ASID 等，供 TLB 维护和页表操作使用。
// ============================================================================
/*
 * vma_vm_mm - get mm pointer from vma pointer (vma->vm_mm)
 */
	.macro	vma_vm_mm, rd, rn
	ldr	\rd, [\rn, #VMA_VM_MM]
	.endm

/*
 * vma_vm_flags - get vma->vm_flags
 */
	.macro	vma_vm_flags, rd, rn
	ldr	\rd, [\rn, #VMA_VM_FLAGS]
	.endm

/*
 * act_mm - get current->active_mm
 */
	.macro	act_mm, rd
	get_current \rd
	.if (TSK_ACTIVE_MM > IMM12_MASK)
	add	\rd, \rd, #TSK_ACTIVE_MM & ~IMM12_MASK
	.endif
	ldr	\rd, [\rd, #TSK_ACTIVE_MM & IMM12_MASK]
	.endm

/*
 * mmid - get context id from mm pointer (mm->context.id)
 * note, this field is 64bit, so in big-endian the two words are swapped too.
 */
	.macro	mmid, rd, rn
#ifdef __ARMEB__
	ldr	\rd, [\rn, #MM_CONTEXT_ID + 4 ]
#else
	ldr	\rd, [\rn, #MM_CONTEXT_ID]
#endif
	.endm

/*
 * mask_asid - mask the ASID from the context ID
 */
	.macro	asid, rd, rn
	and	\rd, \rn, #255
	.endm

	.macro	crval, clear, mmuset, ucset
#ifdef CONFIG_MMU
	.word	\clear
	.word	\mmuset
#else
	.word	\clear
	.word	\ucset
#endif
	.endm

// ============================================================================
// 缓存行大小获取宏
// ARMv7 通过 CTR(Cache Type Register)的 DminLine 和 IminLine 字段
// 动态获取数据和指令缓存行大小（以字为单位），通过 (4 << field) 计算。
// V7M 使用内存映射的系统控制块中的相同寄存器。
// ============================================================================
/*
 * dcache_line_size - get the minimum D-cache line size from the CTR register
 * on ARMv7.
 */
	.macro	dcache_line_size, reg, tmp
#ifdef CONFIG_CPU_V7M
	movw	\tmp, #:lower16:BASEADDR_V7M_SCB + V7M_SCB_CTR
	movt	\tmp, #:upper16:BASEADDR_V7M_SCB + V7M_SCB_CTR
	ldr     \tmp, [\tmp]
#else
	mrc	p15, 0, \tmp, c0, c0, 1		@ read ctr
#endif
	lsr	\tmp, \tmp, #16
	and	\tmp, \tmp, #0xf		@ cache line size encoding
	mov	\reg, #4			@ bytes per word
	mov	\reg, \reg, lsl \tmp		@ actual cache line size
	.endm

/*
 * icache_line_size - get the minimum I-cache line size from the CTR register
 * on ARMv7.
 */
	.macro	icache_line_size, reg, tmp
#ifdef CONFIG_CPU_V7M
	movw	\tmp, #:lower16:BASEADDR_V7M_SCB + V7M_SCB_CTR
	movt	\tmp, #:upper16:BASEADDR_V7M_SCB + V7M_SCB_CTR
	ldr     \tmp, [\tmp]
#else
	mrc	p15, 0, \tmp, c0, c0, 1		@ read ctr
#endif
	and	\tmp, \tmp, #0xf		@ cache line size encoding
	mov	\reg, #4			@ bytes per word
	mov	\reg, \reg, lsl \tmp		@ actual cache line size
	.endm

/*
 * Sanity check the PTE configuration for the code below - which makes
 * certain assumptions about how these bits are laid out.
 */
#ifdef CONFIG_MMU
#if L_PTE_SHARED != PTE_EXT_SHARED
#error PTE shared bit mismatch
#endif
#if !defined (CONFIG_ARM_LPAE) && \
	(L_PTE_XN+L_PTE_USER+L_PTE_RDONLY+L_PTE_DIRTY+L_PTE_YOUNG+\
	 L_PTE_PRESENT) > L_PTE_SHARED
#error Invalid Linux PTE bit settings
#endif
#endif	/* CONFIG_MMU */

// ============================================================================
// ARMv6/v7 页表项设置宏（PTE 权限位翻译）
// 将 Linux PTE 的权限位(YUWD: Young, User, Write, Dirty)翻译为 ARM 硬件
// 页表项的 APX/AP1/AP0 权限控制位。
// 同时通过内存类型映射表(mt_table)将 Linux 内存类型(L_PTE_MT_*)
// 映射为 ARM 硬件 TEX/C/B 属性位。
// 权限翻译表:
//  YUWD  APX AP1 AP0  SVC    User
//  0xxx   0   0   0   none   none
//  100x   1   0   1   r/o    none
//  1011   0   0   1   r/w    none
//  110x   1   1   1   r/o    r/o
//  1111   0   1   1   r/w    r/w
// ============================================================================
/*
 * The ARMv6 and ARMv7 set_pte_ext translation function.
 *
 * Permission translation:
 *  YUWD  APX AP1 AP0	SVC	User
 *  0xxx   0   0   0	no acc	no acc
 *  100x   1   0   1	r/o	no acc
 *  10x0   1   0   1	r/o	no acc
 *  1011   0   0   1	r/w	no acc
 *  110x   1   1   1	r/o	r/o
 *  11x0   1   1   1	r/o	r/o
 *  1111   0   1   1	r/w	r/w
 */
	.macro	armv6_mt_table pfx
\pfx\()_mt_table:
	.long	0x00						@ L_PTE_MT_UNCACHED
	.long	PTE_EXT_TEX(1)					@ L_PTE_MT_BUFFERABLE
	.long	PTE_CACHEABLE					@ L_PTE_MT_WRITETHROUGH
	.long	PTE_CACHEABLE | PTE_BUFFERABLE			@ L_PTE_MT_WRITEBACK
	.long	PTE_BUFFERABLE					@ L_PTE_MT_DEV_SHARED
	.long	0x00						@ unused
	.long	0x00						@ L_PTE_MT_MINICACHE (not present)
	.long	PTE_EXT_TEX(1) | PTE_CACHEABLE | PTE_BUFFERABLE	@ L_PTE_MT_WRITEALLOC
	.long	0x00						@ unused
	.long	PTE_EXT_TEX(1)					@ L_PTE_MT_DEV_WC
	.long	0x00						@ unused
	.long	PTE_CACHEABLE | PTE_BUFFERABLE			@ L_PTE_MT_DEV_CACHED
	.long	PTE_EXT_TEX(2)					@ L_PTE_MT_DEV_NONSHARED
	.long	0x00						@ unused
	.long	0x00						@ unused
	.long	PTE_CACHEABLE | PTE_BUFFERABLE | PTE_EXT_APX	@ L_PTE_MT_VECTORS
	.endm

	.macro	armv6_set_pte_ext pfx
	str	r1, [r0], #2048			@ linux version

	bic	r3, r1, #0x000003fc
	bic	r3, r3, #PTE_TYPE_MASK
	orr	r3, r3, r2
	orr	r3, r3, #PTE_EXT_AP0 | 2

	adr	ip, \pfx\()_mt_table
	and	r2, r1, #L_PTE_MT_MASK
	ldr	r2, [ip, r2]

	eor	r1, r1, #L_PTE_DIRTY
	tst	r1, #L_PTE_DIRTY|L_PTE_RDONLY
	orrne	r3, r3, #PTE_EXT_APX

	tst	r1, #L_PTE_USER
	orrne	r3, r3, #PTE_EXT_AP1
	tstne	r3, #PTE_EXT_APX

	@ user read-only -> kernel read-only
	bicne	r3, r3, #PTE_EXT_AP0

	tst	r1, #L_PTE_XN
	orrne	r3, r3, #PTE_EXT_XN

	eor	r3, r3, r2

	tst	r1, #L_PTE_YOUNG
	tstne	r1, #L_PTE_PRESENT
	moveq	r3, #0
	tstne	r1, #L_PTE_NONE
	movne	r3, #0

	str	r3, [r0]
	mcr	p15, 0, r0, c7, c10, 1		@ flush_pte
	.endm


// ============================================================================
// ARMv3/v4/v5 页表项设置宏（兼容旧架构）
// 使用 8 位 AP 域权限模型而非 APX/AP1/AP0 三级控制。
// 可选支持 writethrough 模式(CONFIG_CPU_DCACHE_WRITETHROUGH)。
// ============================================================================
/*
 * The ARMv3, ARMv4 and ARMv5 set_pte_ext translation function,
 * covering most CPUs except Xscale and Xscale 3.
 *
 * Permission translation:
 *  YUWD   AP	SVC	User
 *  0xxx  0x00	no acc	no acc
 *  100x  0x00	r/o	no acc
 *  10x0  0x00	r/o	no acc
 *  1011  0x55	r/w	no acc
 *  110x  0xaa	r/w	r/o
 *  11x0  0xaa	r/w	r/o
 *  1111  0xff	r/w	r/w
 */
	.macro	armv3_set_pte_ext wc_disable=1
	str	r1, [r0], #2048			@ linux version

	eor	r3, r1, #L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY

	bic	r2, r1, #PTE_SMALL_AP_MASK	@ keep C, B bits
	bic	r2, r2, #PTE_TYPE_MASK
	orr	r2, r2, #PTE_TYPE_SMALL

	tst	r3, #L_PTE_USER			@ user?
	orrne	r2, r2, #PTE_SMALL_AP_URO_SRW

	tst	r3, #L_PTE_RDONLY | L_PTE_DIRTY	@ write and dirty?
	orreq	r2, r2, #PTE_SMALL_AP_UNO_SRW

	tst	r3, #L_PTE_PRESENT | L_PTE_YOUNG	@ present and young?
	movne	r2, #0

	.if	\wc_disable
#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
	tst	r2, #PTE_CACHEABLE
	bicne	r2, r2, #PTE_BUFFERABLE
#endif
	.endif
	str	r2, [r0]		@ hardware version
	.endm


// ============================================================================
// Xscale 页表项设置宏（分两部分以适应工作区）
// xscale_set_pte_ext_prologue: 设置页表前半部分（地址映射）
// xscale_set_pte_ext_epilogue: 完成页表设置（清理缓存）
// r3 必须在两个宏调用之间保留。
// ============================================================================
/*
 * Xscale set_pte_ext translation, split into two halves to cope
 * with work-arounds.  r3 must be preserved by code between these
 * two macros.
 *
 * Permission translation:
 *  YUWD  AP	SVC	User
 *  0xxx  00	no acc	no acc
 *  100x  00	r/o	no acc
 *  10x0  00	r/o	no acc
 *  1011  01	r/w	no acc
 *  110x  10	r/w	r/o
 *  11x0  10	r/w	r/o
 *  1111  11	r/w	r/w
 */
	.macro	xscale_set_pte_ext_prologue
	str	r1, [r0]			@ linux version

	eor	r3, r1, #L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY

	bic	r2, r1, #PTE_SMALL_AP_MASK	@ keep C, B bits
	orr	r2, r2, #PTE_TYPE_EXT		@ extended page

	tst	r3, #L_PTE_USER			@ user?
	orrne	r2, r2, #PTE_EXT_AP_URO_SRW	@ yes -> user r/o, system r/w

	tst	r3, #L_PTE_RDONLY | L_PTE_DIRTY	@ write and dirty?
	orreq	r2, r2, #PTE_EXT_AP_UNO_SRW	@ yes -> user n/a, system r/w
						@ combined with user -> user r/w
	.endm

	.macro	xscale_set_pte_ext_epilogue
	tst	r3, #L_PTE_PRESENT | L_PTE_YOUNG	@ present and young?
	movne	r2, #0				@ no -> fault

	str	r2, [r0, #2048]!		@ hardware version
	mov	ip, #0
	mcr	p15, 0, r0, c7, c10, 1		@ clean L1 D line
	mcr	p15, 0, ip, c7, c10, 4		@ data write barrier
	.endm

// ============================================================================
// define_processor_functions: 定义处理器函数表
// 生成 struct processor 类型的函数表（定义在 <asm/proc-fns.h>），
// 包含该处理器架构的所有关键操作函数指针。
// 函数表布局:
//   .word dabort(数据中止处理)
//   .word pabort(预取中止处理)
//   .word proc_init(初始化)
//   .word bugs(勘误表检测函数)
//   .word proc_fin(关闭)
//   .word reset(软重启)
//   .word do_idle(空闲)
//   .word dcache_clean_area(D-cache 清理)
//   .word switch_mm(地址空间切换)
//   .word set_pte_ext(页表设置)
//   .word suspend_size(挂起保存大小)
//   .word do_suspend(挂起)
//   .word do_resume(恢复)
// big.Little 分支预测硬化版本保存在 .rodata 段保持可用。
// ============================================================================
.macro define_processor_functions name:req, dabort:req, pabort:req, nommu=0, suspend=0, bugs=0
/*
 * If we are building for big.Little with branch predictor hardening,
 * we need the processor function tables to remain available after boot.
 */
#if defined(CONFIG_BIG_LITTLE) && defined(CONFIG_HARDEN_BRANCH_PREDICTOR)
	.section ".rodata"
#endif
	.type	\name\()_processor_functions, #object
	.align 2
ENTRY(\name\()_processor_functions)
	.word	\dabort
	.word	\pabort
	.word	cpu_\name\()_proc_init
	.word	\bugs
	.word	cpu_\name\()_proc_fin
	.word	cpu_\name\()_reset
	.word	cpu_\name\()_do_idle
	.word	cpu_\name\()_dcache_clean_area
	.word	cpu_\name\()_switch_mm

	.if \nommu
	.word	0
	.else
	.word	cpu_\name\()_set_pte_ext
	.endif

	.if \suspend
	.word	cpu_\name\()_suspend_size
#ifdef CONFIG_ARM_CPU_SUSPEND
	.word	cpu_\name\()_do_suspend
	.word	cpu_\name\()_do_resume
#else
	.word	0
	.word	0
#endif
	.else
	.word	0
	.word	0
	.word	0
	.endif

	.size	\name\()_processor_functions, . - \name\()_processor_functions
#if defined(CONFIG_BIG_LITTLE) && defined(CONFIG_HARDEN_BRANCH_PREDICTOR)
	.previous
#endif
.endm

// globl_equ: 定义全局符号别名（类似 C 语言 #define）
// 用于在不同处理器变体之间共享同一函数实现。
.macro globl_equ x, y
	.globl	\x
	.equ	\x, \y
.endm

.macro	initfn, func, base
	.long	\func - \base
.endm

	/*
	 * Macro to calculate the log2 size for the protection region
	 * registers. This calculates rd = log2(size) - 1.  tmp must
	 * not be the same register as rd.
	 */
// --------------------------------------------------------------------------
// pr_sz: 计算 log2(size) - 1, 用于保护区域寄存器的编码
// 通过循环右移计算, tmp 不能与 rd 相同。
// --------------------------------------------------------------------------
.macro	pr_sz, rd, size, tmp
	mov	\tmp, \size, lsr #12
	mov	\rd, #11
1:	movs	\tmp, \tmp, lsr #1
	addne	\rd, \rd, #1
	bne	1b
.endm

	/*
	 * Macro to generate a protection region register value
	 * given a pre-masked address, size, and enable bit.
	 * Corrupts size.
	 */
// pr_val: 生成保护区域寄存器值, 给定预掩码地址、大小和使能位
.macro	pr_val, dest, addr, size, enable
	pr_sz	\dest, \size, \size		@ calculate log2(size) - 1
	orr	\dest, \addr, \dest, lsl #1	@ mask in the region size
	orr	\dest, \dest, \enable
.endm
