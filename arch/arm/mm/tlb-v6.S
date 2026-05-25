/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/mm/tlb-v6.S
 *
 *  Copyright (C) 1997-2002 Russell King
 *
 *  ARM architecture version 6 TLB handling functions.
 *  These assume a split I/D TLB.
 */
// ============================================================================
// 文件概述: ARMv6 TLB 维护操作汇编实现
// 该文件实现了 ARMv6 架构的 TLB(页表缓存)维护函数。
// ARMv6 具有哈佛 TLB(Harvard TLB: 指令和数据 TLB 分离)，
// 通过 HARVARD_TLB 宏控制分离/统一 TLB 操作。
// 主要功能:
//   - 用户空间 TLB 范围失效(v6wbi_flush_user_tlb_range)
//   - 内核空间 TLB 范围失效(v6wbi_flush_kern_tlb_range)
// 与 ARMv7 的关键差异:
//   - 使用写缓冲排空(DSB)而非数据同步屏障(DSB ISH)
//   - 哈佛 TLB 区分 D-TLB 和 I-TLB 失效指令
//   - 非可执行页跳过 I-TLB 失效(性能优化)
//   - 内核 TLB 末尾需执行预取刷新(ISB)
// ============================================================================
#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/cfi_types.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/page.h>
#include <asm/tlbflush.h>
#include "proc-macros.S"

#define HARVARD_TLB

.arch armv6

/*
 *	v6wbi_flush_user_tlb_range(start, end, vma)
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
// v6wbi_flush_user_tlb_range: 使指定地址空间内的用户 TLB 条目范围失效
// 参数: r0=起始地址, r1=结束地址, r2=vma
// 特点(与 v7 的区别):
//   1. 先排空写缓冲(DWB)确保页表更新可见
//   2. 哈佛 TLB: 检查 VM_EXEC 标志, 仅对可执行页失效 I-TLB
//   3. 非哈佛: 使用统一失效指令失效所有 TLB
//   4. 最后再次排空写缓冲(DSB)保证 TLB 维护完成
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6wbi_flush_user_tlb_range)
	vma_vm_mm r3, r2			@ get vma->vm_mm
	mov	ip, #0
	mmid	r3, r3				@ get vm_mm->context.id
	mcr	p15, 0, ip, c7, c10, 4		@ drain write buffer
	mov	r0, r0, lsr #PAGE_SHIFT		@ align address
	mov	r1, r1, lsr #PAGE_SHIFT
	asid	r3, r3				@ mask ASID
	orr	r0, r3, r0, lsl #PAGE_SHIFT	@ Create initial MVA
	mov	r1, r1, lsl #PAGE_SHIFT
	vma_vm_flags r2, r2			@ get vma->vm_flags
1:
#ifdef HARVARD_TLB
	mcr	p15, 0, r0, c8, c6, 1		@ TLB invalidate D MVA (was 1)
	tst	r2, #VM_EXEC			@ Executable area ?
	mcrne	p15, 0, r0, c8, c5, 1		@ TLB invalidate I MVA (was 1)
#else
	mcr	p15, 0, r0, c8, c7, 1		@ TLB invalidate MVA (was 1)
#endif
	add	r0, r0, #PAGE_SZ
	cmp	r0, r1
	blo	1b
	mcr	p15, 0, ip, c7, c10, 4		@ data synchronization barrier
	ret	lr
SYM_FUNC_END(v6wbi_flush_user_tlb_range)

/*
 *	v6wbi_flush_kern_tlb_range(start,end)
 *
 *	Invalidate a range of kernel TLB entries
 *
 *	- start - start address (may not be aligned)
 *	- end   - end address (exclusive, may not be aligned)
 */
// --------------------------------------------------------------------------
// v6wbi_flush_kern_tlb_range: 使内核 TLB 条目范围失效
// 内核页表无 ASID, 直接按 MVA 地址失效。
// 流程: 排空写缓冲 -> 逐页失效 D-TLB 和 I-TLB -> DSB -> ISB
// 与用户版区别: 不需要检查 VM_EXEC, 总是失效 I-TLB + D-TLB;
// 末尾增加 ISB(预取刷新)保证后续指令获取使用新页表。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6wbi_flush_kern_tlb_range)
	mov	r2, #0
	mcr	p15, 0, r2, c7, c10, 4		@ drain write buffer
	mov	r0, r0, lsr #PAGE_SHIFT		@ align address
	mov	r1, r1, lsr #PAGE_SHIFT
	mov	r0, r0, lsl #PAGE_SHIFT
	mov	r1, r1, lsl #PAGE_SHIFT
1:
#ifdef HARVARD_TLB
	mcr	p15, 0, r0, c8, c6, 1		@ TLB invalidate D MVA
	mcr	p15, 0, r0, c8, c5, 1		@ TLB invalidate I MVA
#else
	mcr	p15, 0, r0, c8, c7, 1		@ TLB invalidate MVA
#endif
	add	r0, r0, #PAGE_SZ
	cmp	r0, r1
	blo	1b
	mcr	p15, 0, r2, c7, c10, 4		@ data synchronization barrier
	mcr	p15, 0, r2, c7, c5, 4		@ prefetch flush (isb)
	ret	lr
SYM_FUNC_END(v6wbi_flush_kern_tlb_range)
