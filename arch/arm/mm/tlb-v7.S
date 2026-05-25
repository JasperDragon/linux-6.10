/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/mm/tlb-v7.S
 *
 *  Copyright (C) 1997-2002 Russell King
 *  Modified for ARMv7 by Catalin Marinas
 *
 *  ARM architecture version 6 TLB handling functions.
 *  These assume a split I/D TLB.
 */
// ============================================================================
// 文件概述: ARMv7 TLB 维护操作汇编实现
// 该文件实现了 ARMv7 架构的 TLB(Translation Lookaside Buffer)维护函数，
// 用于管理用户空间和内核空间的 TLB 条目失效。
// 主要功能:
//   - 用户空间 TLB 范围失效(v7wbi_flush_user_tlb_range)
//   - 内核空间 TLB 范围失效(v7wbi_flush_kern_tlb_range)
// 核心机制:
//   - 通过 ASID(Address Space ID)区分不同进程的 TLB 条目
//   - SMP 系统使用共享 TLB 失效操作(TLBIASID/TLBIMVAA)
//   - DSB/ISB 保证 TLB 维护操作的顺序和完成
//   - ARM Errata 720789 修复: 某些 Cortex-A9 使用 all ASID 变体
// ============================================================================
#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/cfi_types.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/page.h>
#include <asm/tlbflush.h>
#include "proc-macros.S"

.arch armv7-a

/*
 *	v7wbi_flush_user_tlb_range(start, end, vma)
 *
 *	Invalidate a range of TLB entries in the specified address space.
 *
 *	- start - start address (may not be aligned)
 *	- end   - end address (exclusive, may not be aligned)
 *	- vma   - vm_area_struct describing address range
 *
 *	It is assumed that:
 *	- the "Invalidate single entry" instruction will invalidate
 *	  both the I and the D TLBs on Harvard-style TLBs
 */
// --------------------------------------------------------------------------
// v7wbi_flush_user_tlb_range: 使指定地址空间内的用户 TLB 条目范围失效
// 参数: r0=起始地址, r1=结束地址, r2=vma
// 流程:
//   1. 从 vma 获取 mm 结构, 进而获取 ASID
//   2. DSB 确保之前页表访问已完成
//   3. 地址对齐到页边界，将 ASID 合并到 MVA
//   4. 逐页执行 TLBIMVA 失效(TLBIMVAA 用于 Errata 720789)
//   5. DSB 保证失效传播到所有核心
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7wbi_flush_user_tlb_range)
	vma_vm_mm r3, r2			@ get vma->vm_mm
	mmid	r3, r3				@ get vm_mm->context.id
	dsb	ish
	mov	r0, r0, lsr #PAGE_SHIFT		@ align address
	mov	r1, r1, lsr #PAGE_SHIFT
	asid	r3, r3				@ mask ASID
#ifdef CONFIG_ARM_ERRATA_720789
	ALT_SMP(W(mov)	r3, #0	)
	ALT_UP(W(nop)		)
#endif
	orr	r0, r3, r0, lsl #PAGE_SHIFT	@ Create initial MVA
	mov	r1, r1, lsl #PAGE_SHIFT
1:
#ifdef CONFIG_ARM_ERRATA_720789
	ALT_SMP(mcr	p15, 0, r0, c8, c3, 3)	@ TLB invalidate U MVA all ASID (shareable) [Cortex-A9 Errata 720789 使用 all ASID 变体广播]
#else
	ALT_SMP(mcr	p15, 0, r0, c8, c3, 1)	@ TLB invalidate U MVA (shareable) [SMP: 按 ASID 广播到所有核心]
#endif
	ALT_UP(mcr	p15, 0, r0, c8, c7, 1)	@ TLB invalidate U MVA

	add	r0, r0, #PAGE_SZ
	cmp	r0, r1
	blo	1b
	dsb	ish
	ret	lr
SYM_FUNC_END(v7wbi_flush_user_tlb_range)

/*
 *	v7wbi_flush_kern_tlb_range(start,end)
 *
 *	Invalidate a range of kernel TLB entries
 *
 *	- start - start address (may not be aligned)
 *	- end   - end address (exclusive, may not be aligned)
 */
// --------------------------------------------------------------------------
// v7wbi_flush_kern_tlb_range: 使内核 TLB 条目范围失效
// 内核 TLB 条目没有 ASID(使用全局页表)，只需按页地址失效。
// 流程: DSB -> 逐页 TLBIMVA -> DSB + ISB
// 与用户版不同: 无需获取和合并 ASID，末尾需 ISB 同步。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7wbi_flush_kern_tlb_range)
	dsb	ish
	mov	r0, r0, lsr #PAGE_SHIFT		@ align address
	mov	r1, r1, lsr #PAGE_SHIFT
	mov	r0, r0, lsl #PAGE_SHIFT
	mov	r1, r1, lsl #PAGE_SHIFT
1:
#ifdef CONFIG_ARM_ERRATA_720789
	ALT_SMP(mcr	p15, 0, r0, c8, c3, 3)	@ TLB invalidate U MVA all ASID (shareable) [Cortex-A9 Errata 720789 使用 all ASID 变体广播]
#else
	ALT_SMP(mcr	p15, 0, r0, c8, c3, 1)	@ TLB invalidate U MVA (shareable) [SMP: 按 ASID 广播到所有核心]
#endif
	ALT_UP(mcr	p15, 0, r0, c8, c7, 1)	@ TLB invalidate U MVA
	add	r0, r0, #PAGE_SZ
	cmp	r0, r1
	blo	1b
	dsb	ish
	isb
	ret	lr
SYM_FUNC_END(v7wbi_flush_kern_tlb_range)
