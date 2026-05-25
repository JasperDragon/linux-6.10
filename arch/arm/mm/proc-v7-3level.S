/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arch/arm/mm/proc-v7-3level.S
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2011 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 *   based on arch/arm/mm/proc-v7-2level.S
 */
// ============================================================================
// 文件概述: ARMv7 LPAE(Large Physical Address Extension)三级页表支持
// 该文件为 ARMv7 架构的大物理地址扩展提供汇编级支持，是内核 MM 子系统
// 中处理 64 位页表项的核心组件。
// 主要内容:
//   - cpu_v7_switch_mm: 切换进程地址空间(写 TTBR0 并设置 ASID)
//   - cpu_v7_set_pte_ext: 设置 PTE 页表项(含权限位翻译)
//   - v7_ttb_setup: 宏, 初始化 TTBCR 和 TTBR1(内核页表基址)
// 关键寄存器:
//   - TTBR0: 用户空间页表基址(含 ASID)
//   - TTBR1: 内核空间页表基址
//   - TTBCR: TTB 控制寄存器(配置地址空间分割)
//   - MAIR0/MAIR1: 内存属性指示寄存器(PRRR/NMRR)
// LPAE 特点: 64 位页表项, 3 级页表(页表基址+页目录+页表),
// 支持最大 40 位物理地址(1TB)。
// ============================================================================
#include <asm/assembler.h>

#define TTB_IRGN_NC	(0 << 8)
#define TTB_IRGN_WBWA	(1 << 8)
#define TTB_IRGN_WT	(2 << 8)
#define TTB_IRGN_WB	(3 << 8)
#define TTB_RGN_NC	(0 << 10)
#define TTB_RGN_OC_WBWA	(1 << 10)
#define TTB_RGN_OC_WT	(2 << 10)
#define TTB_RGN_OC_WB	(3 << 10)
#define TTB_S		(3 << 12)
#define TTB_EAE		(1 << 31)

/* PTWs cacheable, inner WB not shareable, outer WB not shareable */
#define TTB_FLAGS_UP	(TTB_IRGN_WB|TTB_RGN_OC_WB)
#define PMD_FLAGS_UP	(PMD_SECT_WB)

/* PTWs cacheable, inner WBWA shareable, outer WBWA not shareable */
#define TTB_FLAGS_SMP	(TTB_IRGN_WBWA|TTB_S|TTB_RGN_OC_WBWA)
#define PMD_FLAGS_SMP	(PMD_SECT_WBWA|PMD_SECT_S)

#ifndef __ARMEB__
#  define rpgdl	r0
#  define rpgdh	r1
#else
#  define rpgdl	r1
#  define rpgdh	r0
#endif

/*
 * cpu_v7_switch_mm(pgd_phys, tsk)
 *
 * Set the translation table base pointer to be pgd_phys (physical address of
 * the new TTB).
 */
// --------------------------------------------------------------------------
// cpu_v7_switch_mm: 切换进程地址空间(LPAE 版本)
// 参数: r0=pgd_phys(低32位), r1=pgd_phys(高32位), r2=tsk
// 流程: 获取 ASID -> 合并到 TTBR0 高32位 -> MCRR 写 TTBR0 -> ISB
// 注意: LPAE 下 TTBR0 为 64 位, 低 32 位是页表基址, 高 32 位含 ASID。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(cpu_v7_switch_mm)
#ifdef CONFIG_MMU
		mmid	r2, r2
		asid	r2, r2
		orr	rpgdh, rpgdh, r2, lsl #(48 - 32)	@ upper 32-bits of pgd
		mcrr	p15, 0, rpgdl, rpgdh, c2		@ set TTB 0
		isb
#endif
		ret	lr
SYM_FUNC_END(cpu_v7_switch_mm)

#ifdef __ARMEB__
#define rl r3
#define rh r2
#else
#define rl r2
#define rh r3
#endif

/*
 * cpu_v7_set_pte_ext(ptep, pte)
 *
 * Set a level 2 translation table entry.
 * - ptep - pointer to level 3 translation table entry
 * - pte - PTE value to store (64-bit in r2 and r3)
 */
// --------------------------------------------------------------------------
// cpu_v7_set_pte_ext: 设置 LPAE 页表项(L3 描述符)
// 参数: r0=ptep, r2/r3=64位 PTE 值
// 功能:
//   1. 测试 L_PTE_VALID, 无效则直接写入 0
//   2. 测试 L_PTE_NONE, 清除 VALID 位标记空映射
//   3. 测试 DIRTY/RDONLY 位组合, 设置/清除 AP[2] 权限位
//   4. strd 写入 64 位 PTE, UP 下需 flush_pte 写回 D-cache
// 权限位翻译:
//   L_PTE_DIRTY && !L_PTE_RDONLY -> 可写(AP[2]=0)
//   !L_PTE_DIRTY || L_PTE_RDONLY -> 只读(AP[2]=1)
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(cpu_v7_set_pte_ext)
#ifdef CONFIG_MMU
		tst	rl, #L_PTE_VALID
		beq	1f
		tst	rh, #1 << (57 - 32)		@ L_PTE_NONE
		bicne	rl, #L_PTE_VALID
		bne	1f

		eor	ip, rh, #1 << (55 - 32)	@ toggle L_PTE_DIRTY in temp reg to
						@ test for !L_PTE_DIRTY || L_PTE_RDONLY
		tst	ip, #1 << (55 - 32) | 1 << (58 - 32)
		orrne	rl, #PTE_AP2
		biceq	rl, #PTE_AP2

	1:	strd	r2, r3, [r0]
		ALT_SMP(W(nop))
		ALT_UP (mcr	p15, 0, r0, c7, c10, 1)		@ flush_pte
#endif
		ret	lr
SYM_FUNC_END(cpu_v7_set_pte_ext)

		/*
		 * Memory region attributes for LPAE (defined in pgtable-3level.h):
		 *
		 *   n = AttrIndx[2:0]
		 *
		 *			n	MAIR
		 *   UNCACHED		000	00000000
		 *   BUFFERABLE		001	01000100
		 *   DEV_WC		001	01000100
		 *   WRITETHROUGH	010	10101010
		 *   WRITEBACK		011	11101110
		 *   DEV_CACHED		011	11101110
		 *   DEV_SHARED		100	00000100
		 *   DEV_NONSHARED	100	00000100
		 *   unused		101
		 *   unused		110
		 *   WRITEALLOC		111	11111111
		 */
// MAIR0/MAIR1 定义了 ARMv7 LPAE 的内存属性编码
// PRRR(MAIR0)=0xeeaa4400: AttrIndx[0:2] -> TEX/C/B 映射
// NMRR(MAIR1)=0xff000004: 附加属性(非暂存/共享)
	.equ	PRRR,	0xeeaa4400			@ MAIR0
	.equ	NMRR,	0xff000004			@ MAIR1

		/*
		 * Macro for setting up the TTBRx and TTBCR registers.
		 * - \ttbr1 updated.
		 */
		// ------------------------------------------------------------------
		// v7_ttb_setup: 初始化 TTBCR 和 TTBR1(LPAE 版本)
		// 参数: zero=r0(清零), ttbr0l/r=TTBR0 低/高32位,
		//       ttbr1=TTBR1(内核页表), tmp=临时寄存器
		// 功能: 配置 TTBCR(EAE + SMP/UP 缓存属性 + 可选 T1SZ)，
		//       加载 TTBR1(内核页表基址)。
		// 条件分割: 若 PHYS_OFFSET <= PAGE_OFFSET 则设置 TTBR1_SIZE，
		// 否则辅助核心的恒等映射在 TTBR0 中，不能分割地址空间。
		// ------------------------------------------------------------------
		.macro	v7_ttb_setup, zero, ttbr0l, ttbr0h, ttbr1, tmp
		ldr	\tmp, =swapper_pg_dir		@ swapper_pg_dir virtual address
		cmp	\ttbr1, \tmp, lsr #12		@ PHYS_OFFSET > PAGE_OFFSET?
		mov	\tmp, #TTB_EAE			@ for TTB control egister
		ALT_SMP(orr	\tmp, \tmp, #TTB_FLAGS_SMP)
		ALT_UP(orr	\tmp, \tmp, #TTB_FLAGS_UP)
		ALT_SMP(orr	\tmp, \tmp, #TTB_FLAGS_SMP << 16)
		ALT_UP(orr	\tmp, \tmp, #TTB_FLAGS_UP << 16)
		/*
		 * Only use split TTBRs if PHYS_OFFSET <= PAGE_OFFSET (cmp above),
		 * otherwise booting secondary CPUs would end up using TTBR1 for the
		 * identity mapping set up in TTBR0.
		 */
		orrls	\tmp, \tmp, #TTBR1_SIZE				@ TTBCR.T1SZ
		mcr	p15, 0, \tmp, c2, c0, 2				@ TTBCR
		mov	\tmp, \ttbr1, lsr #20
		mov	\ttbr1, \ttbr1, lsl #12
		addls	\ttbr1, \ttbr1, #TTBR1_OFFSET
		mcrr	p15, 1, \ttbr1, \tmp, c2			@ load TTBR1
		.endm

		/*
		 *   AT
		 *  TFR   EV X F   IHD LR    S
		 * .EEE ..EE PUI. .TAT 4RVI ZWRS BLDP WCAM
		 * rxxx rrxx xxx0 0101 xxxx xxxx x111 xxxx < forced
		 *   11    0 110    0  0011 1100 .111 1101 < we want
		 */
		.align	2
		.type	v7_crval, #object
	v7_crval:
		crval	clear=0x0122c302, mmuset=0x30c03c7d, ucset=0x00c01c7c
