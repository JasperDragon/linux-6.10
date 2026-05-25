/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/mm/cache-v6.S
 *
 *  Copyright (C) 2001 Deep Blue Solutions Ltd.
 *
 *  This is the "shell" of the ARMv6 processor support.
 */
// ============================================================================
// 文件概述: ARMv6 Cache 缓存维护操作汇编实现
// 该文件实现了 ARMv6 架构的缓存维护函数，是内核 MM 子系统的重要组成部分。
// ARMv6 具有哈佛缓存架构(Harvard Cache: 指令和数据缓存分离)，
// 定义了固定缓存行大小(32字节)和 BTB 刷新大小。
// 主要内容包括:
//   - 指令缓存刷新(v6_flush_icache_all): 包含 Errata 411920 修复
//   - 全部缓存刷新(v6_flush_kern_cache_all): D-cache + I-cache 联合刷新
//   - 缓存一致性维护(v6_coherent_user_range/v6_coherent_kern_range)
//   - D-cache 区域刷新(v6_flush_kern_dcache_area)
//   - DMA 缓存管理(v6_dma_inv_range/clean_range/flush_range)
// 核心特点:
//   - 使用固定缓存行大小(CACHE_LINE_SIZE=32)，而非动态读取
//   - 通过 HARVARD_CACHE 宏选择分离缓存或统一缓存操作
//   - 所有 DMA 操作结束写缓冲排空
//   - 用户空间操作使用 USER() 宏处理地址访问异常
// ============================================================================
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/cfi_types.h>
#include <asm/assembler.h>
#include <asm/errno.h>
#include <asm/unwind.h>

#include "proc-macros.S"

#define HARVARD_CACHE
#define CACHE_LINE_SIZE		32
#define D_CACHE_LINE_SIZE	32
#define BTB_FLUSH_SIZE		8

.arch armv6

/*
 *	v6_flush_icache_all()
 *
 *	Flush the whole I-cache.
 *
 *	ARM1136 erratum 411920 - Invalidate Instruction Cache operation can fail.
 *	This erratum is present in 1136, 1156 and 1176. It does not affect the
 *	MPCore.
 *
 *	Registers:
 *	r0 - set to 0
 *	r1 - corrupted
 */
// --------------------------------------------------------------------------
// v6_flush_icache_all: 刷新整个指令缓存
// ARM Errata 411920: ARM1136/1156/1176 的 I-cache 失效操作可能失败。
// 修复方案: 重复执行 4 次失效指令，然后插入 11 个 NOP 等待完成。
// 在修复期间禁用中断保证原子性。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6_flush_icache_all)
	mov	r0, #0
#ifdef CONFIG_ARM_ERRATA_411920
	mrs	r1, cpsr
	cpsid	ifa				@ disable interrupts
	mcr	p15, 0, r0, c7, c5, 0		@ invalidate entire I-cache
	mcr	p15, 0, r0, c7, c5, 0		@ invalidate entire I-cache
	mcr	p15, 0, r0, c7, c5, 0		@ invalidate entire I-cache
	mcr	p15, 0, r0, c7, c5, 0		@ invalidate entire I-cache
	msr	cpsr_cx, r1			@ restore interrupts
	.rept	11				@ ARM Ltd recommends at least
	nop					@ 11 NOPs
	.endr
#else
	mcr	p15, 0, r0, c7, c5, 0		@ invalidate I-cache
#endif
	ret	lr
SYM_FUNC_END(v6_flush_icache_all)

/*
 *	v6_flush_cache_all()
 *
 *	Flush the entire cache.
 *
 *	It is assumed that:
 */
// --------------------------------------------------------------------------
// v6_flush_kern_cache_all: 刷新所有内核缓存(D-cache + I-cache)
// 哈佛架构: D-cache clean+invalidate, I-cache invalidate
// 统一架构: 使用单一指令 clean+invalidate 所有缓存
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6_flush_kern_cache_all)
	mov	r0, #0
#ifdef HARVARD_CACHE
	mcr	p15, 0, r0, c7, c14, 0		@ D cache clean+invalidate
#ifndef CONFIG_ARM_ERRATA_411920
	mcr	p15, 0, r0, c7, c5, 0		@ I+BTB cache invalidate
#else
	b	v6_flush_icache_all
#endif
#else
	mcr	p15, 0, r0, c7, c15, 0		@ Cache clean+invalidate
#endif
	ret	lr
SYM_FUNC_END(v6_flush_kern_cache_all)

/*
 *	v6_flush_cache_all()
 *
 *	Flush all TLB entries in a particular address space
 *
 *	- mm    - mm_struct describing address space
 */
SYM_TYPED_FUNC_START(v6_flush_user_cache_all)
	ret	lr
SYM_FUNC_END(v6_flush_user_cache_all)

/*
 *	v6_flush_cache_range(start, end, flags)
 *
 *	Flush a range of TLB entries in the specified address space.
 *
 *	- start - start address (may not be aligned)
 *	- end   - end address (exclusive, may not be aligned)
 *	- flags	- vm_area_struct flags describing address space
 *
 *	It is assumed that:
 *	- we have a VIPT cache.
 */
SYM_TYPED_FUNC_START(v6_flush_user_cache_range)
	ret	lr
SYM_FUNC_END(v6_flush_user_cache_range)

/*
 *	v6_coherent_kern_range(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified
 *	region.  This is typically used when code has been written to
 *	a memory region, and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 *
 *	It is assumed that:
 *	- the Icache does not read data from the write buffer
 */
SYM_TYPED_FUNC_START(v6_coherent_kern_range)
#ifdef CONFIG_CFI /* Fallthrough if !CFI */
	b	v6_coherent_user_range
#endif
SYM_FUNC_END(v6_coherent_kern_range)

/*
 *	v6_coherent_user_range(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified
 *	region.  This is typically used when code has been written to
 *	a memory region, and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 *
 *	It is assumed that:
 *	- the Icache does not read data from the write buffer
 */
// --------------------------------------------------------------------------
// v6_coherent_user_range: 确保用户空间范围的 I/D 缓存一致性
// 哈佛架构流程:
//   1. 对齐起始地址, 逐行 clean D-cache(DCCMVAC)
//   2. 写缓冲排空(DSB)
//   3. 使整个 I-cache 失效(含 BTB)
// 统一架构: 仅使 BTB 失效
// 若虚拟地址未映射, 返回 -EFAULT
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6_coherent_user_range)
 UNWIND(.fnstart		)
#ifdef HARVARD_CACHE
	bic	r0, r0, #CACHE_LINE_SIZE - 1
1:
 USER(	mcr	p15, 0, r0, c7, c10, 1	)	@ clean D line
	add	r0, r0, #CACHE_LINE_SIZE
	cmp	r0, r1
	blo	1b
#endif
	mov	r0, #0
#ifdef HARVARD_CACHE
	mcr	p15, 0, r0, c7, c10, 4		@ drain write buffer
#ifndef CONFIG_ARM_ERRATA_411920
	mcr	p15, 0, r0, c7, c5, 0		@ I+BTB cache invalidate
#else
	b	v6_flush_icache_all
#endif
#else
	mcr	p15, 0, r0, c7, c5, 6		@ invalidate BTB
#endif
	ret	lr

/*
 * Fault handling for the cache operation above. If the virtual address in r0
 * isn't mapped, fail with -EFAULT.
 */
9001:
	mov	r0, #-EFAULT
	ret	lr
 UNWIND(.fnend		)
SYM_FUNC_END(v6_coherent_user_range)

/*
 *	v6_flush_kern_dcache_area(void *addr, size_t size)
 *
 *	Ensure that the data held in the page kaddr is written back
 *	to the page in question.
 *
 *	- addr	- kernel address
 *	- size	- region size
 */
// --------------------------------------------------------------------------
// v6_flush_kern_dcache_area: 刷新内核 D-cache 指定区域
// 对齐地址后逐缓存行 clean+invalidate。
// 哈佛架构最后排空写缓冲，统一架构不需要。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6_flush_kern_dcache_area)
	add	r1, r0, r1
	bic	r0, r0, #D_CACHE_LINE_SIZE - 1
1:
#ifdef HARVARD_CACHE
	mcr	p15, 0, r0, c7, c14, 1		@ clean & invalidate D line
#else
	mcr	p15, 0, r0, c7, c15, 1		@ clean & invalidate unified line
#endif	
	add	r0, r0, #D_CACHE_LINE_SIZE
	cmp	r0, r1
	blo	1b
#ifdef HARVARD_CACHE
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
#endif
	ret	lr
SYM_FUNC_END(v6_flush_kern_dcache_area)

/*
 *	v6_dma_inv_range(start,end)
 *
 *	Invalidate the data cache within the specified region; we will
 *	be performing a DMA operation in this region and we want to
 *	purge old data in the cache.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
// --------------------------------------------------------------------------
// v6_dma_inv_range: DMA 操作前使数据缓存失效
// 首尾对齐处理:
//   - 起始地址未对齐: clean 起始行(确保脏数据写回)
//   - 结束地址未对齐: clean+invalidate 最后行
//   - 中间完整行: 直接 invalidate
// 最后排空写缓冲。
// --------------------------------------------------------------------------
v6_dma_inv_range:
	tst	r0, #D_CACHE_LINE_SIZE - 1
	bic	r0, r0, #D_CACHE_LINE_SIZE - 1
#ifdef HARVARD_CACHE
	mcrne	p15, 0, r0, c7, c10, 1		@ clean D line
#else
	mcrne	p15, 0, r0, c7, c11, 1		@ clean unified line
#endif
	tst	r1, #D_CACHE_LINE_SIZE - 1
	bic	r1, r1, #D_CACHE_LINE_SIZE - 1
#ifdef HARVARD_CACHE
	mcrne	p15, 0, r1, c7, c14, 1		@ clean & invalidate D line
#else
	mcrne	p15, 0, r1, c7, c15, 1		@ clean & invalidate unified line
#endif
1:
#ifdef HARVARD_CACHE
	mcr	p15, 0, r0, c7, c6, 1		@ invalidate D line
#else
	mcr	p15, 0, r0, c7, c7, 1		@ invalidate unified line
#endif
	add	r0, r0, #D_CACHE_LINE_SIZE
	cmp	r0, r1
	blo	1b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4		@ drain write buffer
	ret	lr

/*
 *	v6_dma_clean_range(start,end)
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
// --------------------------------------------------------------------------
// v6_dma_clean_range: DMA 操作前回写数据缓存(不失效)
// 对齐地址后逐行 clean D-cache，最后排空写缓冲。
// 用于 CPU 写完数据等待 DMA 设备读取的场景。
// --------------------------------------------------------------------------
v6_dma_clean_range:
	bic	r0, r0, #D_CACHE_LINE_SIZE - 1
1:
#ifdef HARVARD_CACHE
	mcr	p15, 0, r0, c7, c10, 1		@ clean D line
#else
	mcr	p15, 0, r0, c7, c11, 1		@ clean unified line
#endif
	add	r0, r0, #D_CACHE_LINE_SIZE
	cmp	r0, r1
	blo	1b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4		@ drain write buffer
	ret	lr

/*
 *	v6_dma_flush_range(start,end)
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
// --------------------------------------------------------------------------
// v6_dma_flush_range: DMA 前后回写并使缓存失效
// 对齐后逐行 clean+invalidate，最后排空写缓冲。
// 最安全的 DMA 缓存操作，适用于双向传输。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6_dma_flush_range)
	bic	r0, r0, #D_CACHE_LINE_SIZE - 1
1:
#ifdef HARVARD_CACHE
	mcr	p15, 0, r0, c7, c14, 1		@ clean & invalidate D line
#else
	mcr	p15, 0, r0, c7, c15, 1		@ clean & invalidate line
#endif
	add	r0, r0, #D_CACHE_LINE_SIZE
	cmp	r0, r1
	blo	1b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4		@ drain write buffer
	ret	lr
SYM_FUNC_END(v6_dma_flush_range)

/*
 *	dma_map_area(start, size, dir)
 *	- start	- kernel virtual start address
 *	- size	- size of region
 *	- dir	- DMA direction
 */
// --------------------------------------------------------------------------
// v6_dma_map_area/v6_dma_unmap_area: DMA 映射/取消映射时的缓存操作
// map:   DMA_FROM_DEVICE -> invalidate, 否则 -> clean
// unmap: DMA_TO_DEVICE -> 不操作, 否则 -> invalidate
// 与 v7 行为相同，但写缓冲排空在具体函数内部完成。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v6_dma_map_area)
	add	r1, r1, r0
	teq	r2, #DMA_FROM_DEVICE
	beq	v6_dma_inv_range
	b	v6_dma_clean_range
SYM_FUNC_END(v6_dma_map_area)

/*
 *	dma_unmap_area(start, size, dir)
 *	- start	- kernel virtual start address
 *	- size	- size of region
 *	- dir	- DMA direction
 */
SYM_TYPED_FUNC_START(v6_dma_unmap_area)
	add	r1, r1, r0
	teq	r2, #DMA_TO_DEVICE
	bne	v6_dma_inv_range
	ret	lr
SYM_FUNC_END(v6_dma_unmap_area)
