/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/mm/cache-v7.S
 *
 *  Copyright (C) 2001 Deep Blue Solutions Ltd.
 *  Copyright (C) 2005 ARM Ltd.
 *
 *  This is the "shell" of the ARMv7 processor support.
 */
// ============================================================================
// 文件概述: ARMv7 Cache 缓存维护操作汇编实现
// 该文件实现了 ARMv7 架构的完整缓存维护函数，是内核 MM 子系统的重要组成部分。
// 主要内容包括:
//   - L1 数据缓存失效(v7_invalidate_l1): 用于辅助核心启动和主核心唤醒
//   - I-cache 整体失效(v7_flush_icache_all): 刷新整个指令缓存
//   - D-cache 层次化刷新(v7_flush_dcache_louis/v7_flush_dcache_all):
//     通过读取 CLIDR 寄存器获取缓存层次结构，按"组/路"(Set/Way)方式逐层遍历刷新
//   - 内核/用户空间缓存一致性(v7_coherent_kern_range/v7_coherent_user_range):
//     通过 MVA(Modified Virtual Address)方式逐缓存行操作
//   - DMA 相关缓存管理(v7_dma_inv_range/v7_dma_clean_range/v7_dma_flush_range):
//     保证 DMA 操作时缓存与内存之间的一致性
// 核心机制: 使用 CP15 协处理器指令操作缓存，
// 支持 ARM Errata 工作区修复，通过 ALT_SMP/ALT_UP 宏适配 SMP/UP 场景。
// ============================================================================
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/cfi_types.h>
#include <asm/assembler.h>
#include <asm/errno.h>
#include <asm/unwind.h>
#include <asm/hardware/cache-b15-rac.h>

#include "proc-macros.S"

.arch armv7-a

#ifdef CONFIG_CPU_ICACHE_MISMATCH_WORKAROUND
.globl icache_size
	.data
	.align	2
icache_size:
	.long	64
	.text
#endif
/*
 * The secondary kernel init calls v7_flush_dcache_all before it enables
 * the L1; however, the L1 comes out of reset in an undefined state, so
 * the clean + invalidate performed by v7_flush_dcache_all causes a bunch
 * of cache lines with uninitialized data and uninitialized tags to get
 * written out to memory, which does really unpleasant things to the main
 * processor.  We fix this by performing an invalidate, rather than a
 * clean + invalidate, before jumping into the kernel.
 *
 * This function needs to be called for both secondary cores startup and
 * primary core resume procedures.
 */
ENTRY(v7_invalidate_l1)
	mov	r0, #0
	mcr	p15, 2, r0, c0, c0, 0	@ select L1 data cache in CSSELR
	isb
	mrc	p15, 1, r0, c0, c0, 0	@ read cache geometry from CCSIDR

	movw	r3, #0x3ff
	and	r3, r3, r0, lsr #3	@ 'Associativity' in CCSIDR[12:3]
	clz	r1, r3			@ WayShift
	mov	r2, #1
	mov	r3, r3, lsl r1		@ NumWays-1 shifted into bits [31:...]
	movs	r1, r2, lsl r1		@ #1 shifted left by same amount
	moveq	r1, #1			@ r1 needs value > 0 even if only 1 way

	and	r2, r0, #0x7
	add	r2, r2, #4		@ SetShift

1:	movw	ip, #0x7fff
	and	r0, ip, r0, lsr #13	@ 'NumSets' in CCSIDR[27:13]

2:	mov	ip, r0, lsl r2		@ NumSet << SetShift
	orr	ip, ip, r3		@ Reg = (Temp<<WayShift)|(NumSets<<SetShift)
	mcr	p15, 0, ip, c7, c6, 2
	subs	r0, r0, #1		@ Set--
	bpl	2b
	subs	r3, r3, r1		@ Way--
	bcc	3f
	mrc	p15, 1, r0, c0, c0, 0	@ re-read cache geometry from CCSIDR
	b	1b
3:	dsb	st
	isb
	ret	lr
ENDPROC(v7_invalidate_l1)

/*
 *	v7_flush_icache_all()
 *
 *	Flush the whole I-cache.
 *
 *	Registers:
 *	r0 - set to 0
 */
// --------------------------------------------------------------------------
// v7_flush_icache_all: 刷新整个指令缓存
// 在 SMP 系统中使用 ICIALLUIS 使所有核心的 I-cache 失效，
// 在 UP 系统中使用 ICIALLU 同时使 BTB 失效。
// 由 Alt-SMP/Alt-UP 宏在编译时适配不同的缓存架构。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7_flush_icache_all)
	mov	r0, #0
	ALT_SMP(mcr	p15, 0, r0, c7, c1, 0)		@ invalidate I-cache inner shareable
	ALT_UP(mcr	p15, 0, r0, c7, c5, 0)		@ I+BTB cache invalidate
	ret	lr
SYM_FUNC_END(v7_flush_icache_all)

 /*
 *     v7_flush_dcache_louis()
 *
 *     Flush the D-cache up to the Level of Unification Inner Shareable
 *
 *     Corrupted registers: r0-r6, r9-r10
 */
// --------------------------------------------------------------------------
// v7_flush_dcache_louis: 刷新数据缓存至内部共享统一层次(LoUIS)
// 读取 CLIDR 寄存器的 LoUIS(Level of Unification Inner Shareable)字段，
// 确定需要刷新的最大缓存层次。SMP 系统使用 LoUIS(bit[23:21])，
// UP 系统使用 LoUU(bit[29:27])。
// 包含 ARM Errata 643719 的修复(Cortex-A9 r0p? 的 LoUIS 值错误)。
// --------------------------------------------------------------------------

ENTRY(v7_flush_dcache_louis)
	dmb					@ ensure ordering with previous memory accesses
	mrc	p15, 1, r0, c0, c0, 1		@ read clidr, r0 = clidr
ALT_SMP(mov	r3, r0, lsr #20)		@ move LoUIS into position
ALT_UP(	mov	r3, r0, lsr #26)		@ move LoUU into position
	ands	r3, r3, #7 << 1 		@ extract LoU*2 field from clidr
	bne	start_flush_levels		@ LoU != 0, start flushing
#ifdef CONFIG_ARM_ERRATA_643719
ALT_SMP(mrc	p15, 0, r2, c0, c0, 0)		@ read main ID register
ALT_UP(	ret	lr)				@ LoUU is zero, so nothing to do
	movw	r1, #:lower16:(0x410fc090 >> 4)	@ ID of ARM Cortex A9 r0p?
	movt	r1, #:upper16:(0x410fc090 >> 4)
	teq	r1, r2, lsr #4			@ test for errata affected core and if so...
	moveq	r3, #1 << 1			@   fix LoUIS value
	beq	start_flush_levels		@   start flushing cache levels
#endif
	ret	lr
ENDPROC(v7_flush_dcache_louis)

/*
 *	v7_flush_dcache_all()
 *
 *	Flush the whole D-cache.
 *
 *	Corrupted registers: r0-r6, r9-r10
 *
 *	- mm    - mm_struct describing address space
 */
// --------------------------------------------------------------------------
// v7_flush_dcache_all: 刷新整个数据缓存
// 读取 CLIDR 获取 LoC(Level of Coherency)，确定需要操作的缓存层次。
// 核心算法: 遍历所有缓存层次(L1, L2, L3...)。
//   对每一层: 通过 CSSELR/CCSIDR 读取几何信息(组数、路数、缓存行大小)。
//   按组/路(Set/Way)方式逐行进行 clean+invalidate。
// 最终将 CSSELR 切回 L1 缓存, 执行 DSB/ISB 保证操作完成。
// --------------------------------------------------------------------------
ENTRY(v7_flush_dcache_all)
	dmb					@ ensure ordering with previous memory accesses
	mrc	p15, 1, r0, c0, c0, 1		@ read clidr
	mov	r3, r0, lsr #23			@ move LoC into position
	ands	r3, r3, #7 << 1			@ extract LoC*2 from clidr
	beq	finished			@ if loc is 0, then no need to clean
start_flush_levels:
		// r10: 当前缓存层次(0=L1,2=L2,4=L3...)
		// r3: 最大层次(LoC*2)
		// 通过两层循环逐级遍历: 外层选缓存层次，内层遍历所有组/路
	mov	r10, #0				@ start clean at cache level 0
flush_levels:
	add	r2, r10, r10, lsr #1		@ work out 3x current cache level
	mov	r1, r0, lsr r2			@ extract cache type bits from clidr
	and	r1, r1, #7			@ mask of the bits for current cache only
	cmp	r1, #2				@ see what cache we have at this level
	blt	skip				@ skip if no cache, or just i-cache
#ifdef CONFIG_PREEMPTION
	save_and_disable_irqs_notrace r9	@ make cssr&csidr read atomic
#endif
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	isb					@ isb to sych the new cssr&csidr
	mrc	p15, 1, r1, c0, c0, 0		@ read the new csidr
#ifdef CONFIG_PREEMPTION
	restore_irqs_notrace r9
#endif
	and	r2, r1, #7			@ extract the length of the cache lines
	add	r2, r2, #4			@ add 4 (line length offset)
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3		@ find maximum number on the way size
	clz	r5, r4				@ find bit position of way size increment
	movw	r6, #0x7fff
	and	r1, r6, r1, lsr #13		@ extract max number of the index size
	mov	r6, #1
	movne	r4, r4, lsl r5			@ # of ways shifted into bits [31:...]
	movne	r6, r6, lsl r5			@ 1 shifted left by same amount
loop1:
		// 外层循环: 遍历所有组(Set)
		// r9: 当前组索引(从最高组向下递减)
		// 每次迭代: 读组号, 或入路号和缓存层次, 执行 clean+invalidate
	mov	r9, r1				@ create working copy of max index
loop2:
		// 内层循环: 遍历所有路(Way)
		// r5 = (组号 << SetShift) | (路号 << WayShift) | 缓存层次
		// 使用 MCR 指令通过组/路方式执行 clean+invalidate
	mov	r5, r9, lsl r2			@ factor set number into r5
	orr	r5, r5, r4			@ factor way number into r5
	orr	r5, r5, r10			@ factor cache level into r5
	mcr	p15, 0, r5, c7, c14, 2		@ clean & invalidate by set/way
	subs	r9, r9, #1			@ decrement the index
	bge	loop2
	subs	r4, r4, r6			@ decrement the way
	bcs	loop1
skip:
	add	r10, r10, #2			@ increment cache number
	cmp	r3, r10
#ifdef CONFIG_ARM_ERRATA_814220
	dsb
#endif
	bgt	flush_levels
finished:
		// 所有缓存层次刷新完毕，将 CSSELR 切回 L1
		// DSB + ISB 保证维护指令完成并同步流水线
	mov	r10, #0				@ switch back to cache level 0
	mcr	p15, 2, r10, c0, c0, 0		@ select current cache level in cssr
	dsb	st
	isb
	ret	lr
ENDPROC(v7_flush_dcache_all)

/*
 *	v7_flush_cache_all()
 *
 *	Flush the entire cache system.
 *  The data cache flush is now achieved using atomic clean / invalidates
 *  working outwards from L1 cache. This is done using Set/Way based cache
 *  maintenance instructions.
 *  The instruction cache can still be invalidated back to the point of
 *  unification in a single instruction.
 *
 */
// --------------------------------------------------------------------------
// v7_flush_kern_cache_all: 刷新所有内核缓存(D-cache + I-cache)
// 先调用 v7_flush_dcache_all 刷新全部数据缓存(按组/路逐层遍历)，
// 再通过 ICIALLUIS/ICIALLU 使所有指令缓存失效。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7_flush_kern_cache_all)
	stmfd	sp!, {r4-r6, r9-r10, lr}
	bl	v7_flush_dcache_all
	mov	r0, #0
	ALT_SMP(mcr	p15, 0, r0, c7, c1, 0)	@ invalidate I-cache inner shareable
	ALT_UP(mcr	p15, 0, r0, c7, c5, 0)	@ I+BTB cache invalidate
	ldmfd	sp!, {r4-r6, r9-r10, lr}
	ret	lr
SYM_FUNC_END(v7_flush_kern_cache_all)

 /*
 *     v7_flush_kern_cache_louis(void)
 *
 *     Flush the data cache up to Level of Unification Inner Shareable.
 *     Invalidate the I-cache to the point of unification.
 */
// --------------------------------------------------------------------------
// v7_flush_kern_cache_louis: 刷新内核缓存至 LoUIS 层次
// 比 v7_flush_kern_cache_all 刷新的层次少(仅到 LoUIS 而非 LoC)，
// 在不需要全局一致性的场景中性能更好。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7_flush_kern_cache_louis)
	stmfd	sp!, {r4-r6, r9-r10, lr}
	bl	v7_flush_dcache_louis
	mov	r0, #0
	ALT_SMP(mcr	p15, 0, r0, c7, c1, 0)	@ invalidate I-cache inner shareable
	ALT_UP(mcr	p15, 0, r0, c7, c5, 0)	@ I+BTB cache invalidate
	ldmfd	sp!, {r4-r6, r9-r10, lr}
	ret	lr
SYM_FUNC_END(v7_flush_kern_cache_louis)

/*
 *	v7_flush_cache_all()
 *
 *	Flush all TLB entries in a particular address space
 *
 *	- mm    - mm_struct describing address space
 */
SYM_TYPED_FUNC_START(v7_flush_user_cache_all)
	ret	lr
SYM_FUNC_END(v7_flush_user_cache_all)

/*
 *	v7_flush_cache_range(start, end, flags)
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
SYM_TYPED_FUNC_START(v7_flush_user_cache_range)
	ret	lr
SYM_FUNC_END(v7_flush_user_cache_range)

/*
 *	v7_coherent_kern_range(start,end)
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
SYM_TYPED_FUNC_START(v7_coherent_kern_range)
#ifdef CONFIG_CFI /* Fallthrough if !CFI */
	b	v7_coherent_user_range
#endif
SYM_FUNC_END(v7_coherent_kern_range)

/*
 *	v7_coherent_user_range(start,end)
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
// v7_coherent_user_range: 确保用户空间指定范围的 I/D 缓存一致性
// 算法:
//   1. 按缓存行对齐起始地址
//   2. 逐行 D-cache clean 到统一点(PoU)
//   3. DSB 等待 clean 完成
//   4. 逐行 I-cache invalidate(含 BTB 失效)
//   5. DSB + ISB 保证一致性操作全部可见
// 若虚拟地址未映射, 第二级故障处理返回 -EFAULT
// 可选 Errata 764369/775420 修复
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7_coherent_user_range)
 UNWIND(.fnstart		)
	dcache_line_size r2, r3
	sub	r3, r2, #1
	bic	r12, r0, r3
#ifdef CONFIG_ARM_ERRATA_764369
	ALT_SMP(W(dsb))
	ALT_UP(W(nop))
#endif
1:
 USER(	mcr	p15, 0, r12, c7, c11, 1	)	@ clean D line to the point of unification
	add	r12, r12, r2
	cmp	r12, r1
	blo	1b
	dsb	ishst
#ifdef CONFIG_CPU_ICACHE_MISMATCH_WORKAROUND
	ldr	r3, =icache_size
	ldr	r2, [r3, #0]
#else
	icache_line_size r2, r3
#endif
	sub	r3, r2, #1
	bic	r12, r0, r3
2:
 USER(	mcr	p15, 0, r12, c7, c5, 1	)	@ invalidate I line
	add	r12, r12, r2
	cmp	r12, r1
	blo	2b
	mov	r0, #0
	ALT_SMP(mcr	p15, 0, r0, c7, c1, 6)	@ invalidate BTB Inner Shareable
	ALT_UP(mcr	p15, 0, r0, c7, c5, 6)	@ invalidate BTB
	dsb	ishst
	isb
	ret	lr

/*
 * Fault handling for the cache operation above. If the virtual address in r0
 * isn't mapped, fail with -EFAULT.
 */
// 故障处理: 若缓存操作中虚拟地址未映射(如用户空间非法地址)，返回 -EFAULT
9001:
#ifdef CONFIG_ARM_ERRATA_775420
	dsb
#endif
	mov	r0, #-EFAULT
	ret	lr
 UNWIND(.fnend		)
SYM_FUNC_END(v7_coherent_user_range)

/*
 *	v7_flush_kern_dcache_area(void *addr, size_t size)
 *
 *	Ensure that the data held in the page kaddr is written back
 *	to the page in question.
 *
 *	- addr	- kernel address
 *	- size	- region size
 */
// --------------------------------------------------------------------------
// v7_flush_kern_dcache_area: 刷新内核 D-cache 指定区域
// 对给定虚拟地址范围逐缓存行执行 clean+invalidate。
// 用于代码写入内存后即将执行，或页分配后需要刷新的场景。
// 对齐首地址后，通过 DCCIMVAC(DC clean&invalidate MVA) 指令逐行操作。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7_flush_kern_dcache_area)
	dcache_line_size r2, r3
	add	r1, r0, r1
	sub	r3, r2, #1
	bic	r0, r0, r3
#ifdef CONFIG_ARM_ERRATA_764369
	ALT_SMP(W(dsb))
	ALT_UP(W(nop))
#endif
1:
	mcr	p15, 0, r0, c7, c14, 1		@ clean & invalidate D line / unified line
	add	r0, r0, r2
	cmp	r0, r1
	blo	1b
	dsb	st
	ret	lr
SYM_FUNC_END(v7_flush_kern_dcache_area)

/*
 *	v7_dma_inv_range(start,end)
 *
 *	Invalidate the data cache within the specified region; we will
 *	be performing a DMA operation in this region and we want to
 *	purge old data in the cache.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
// --------------------------------------------------------------------------
// v7_dma_inv_range: DMA 操作前使数据缓存失效
// 边界处理策略("缓存行弹射"):
//   1. 若起始地址未对齐到缓存行: 先 clean+invalidate 该首行(避免破坏邻居数据)
//   2. 若结束地址未对齐: 先 clean+invalidate 该尾行
//   3. 中间完整缓存行: 直接 invalidate(不写回, 避免脏数据覆盖 DMA)
// 最后执行 DSB 保证操作完成。
// --------------------------------------------------------------------------
v7_dma_inv_range:
	dcache_line_size r2, r3
	sub	r3, r2, #1
	tst	r0, r3
	bic	r0, r0, r3
#ifdef CONFIG_ARM_ERRATA_764369
	ALT_SMP(W(dsb))
	ALT_UP(W(nop))
#endif
	mcrne	p15, 0, r0, c7, c14, 1		@ clean & invalidate D / U line
	addne	r0, r0, r2

	tst	r1, r3
	bic	r1, r1, r3
	mcrne	p15, 0, r1, c7, c14, 1		@ clean & invalidate D / U line
	cmp	r0, r1
1:
	mcrlo	p15, 0, r0, c7, c6, 1		@ invalidate D / U line
	addlo	r0, r0, r2
	cmplo	r0, r1
	blo	1b
	dsb	st
	ret	lr
ENDPROC(v7_dma_inv_range)

/*
 *	v7_dma_clean_range(start,end)
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
// --------------------------------------------------------------------------
// v7_dma_clean_range: DMA 操作前回写数据缓存(不失效)
// 通过 DCCMVAC(DC clean MVA by VA to PoC)逐行回写脏数据到内存。
// 用于 DMA_FROM_DEVICE 映射方向，确保 CPU 对内存缓冲区的写入到达内存。
// --------------------------------------------------------------------------
v7_dma_clean_range:
	dcache_line_size r2, r3
	sub	r3, r2, #1
	bic	r0, r0, r3
#ifdef CONFIG_ARM_ERRATA_764369
	ALT_SMP(W(dsb))
	ALT_UP(W(nop))
#endif
1:
	mcr	p15, 0, r0, c7, c10, 1		@ clean D / U line
	add	r0, r0, r2
	cmp	r0, r1
	blo	1b
	dsb	st
	ret	lr
ENDPROC(v7_dma_clean_range)

/*
 *	v7_dma_flush_range(start,end)
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
// --------------------------------------------------------------------------
// v7_dma_flush_range: DMA 前后回写并使D-cache失效
// 使用 DCCIMVAC 逐行 clean+invalidate。
// 当 DMA 方向不确定或双向传输时，缓存行完全同步内存是最安全的选择。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7_dma_flush_range)
	dcache_line_size r2, r3
	sub	r3, r2, #1
	bic	r0, r0, r3
#ifdef CONFIG_ARM_ERRATA_764369
	ALT_SMP(W(dsb))
	ALT_UP(W(nop))
#endif
1:
	mcr	p15, 0, r0, c7, c14, 1		@ clean & invalidate D / U line
	add	r0, r0, r2
	cmp	r0, r1
	blo	1b
	dsb	st
	ret	lr
SYM_FUNC_END(v7_dma_flush_range)

/*
 *	dma_map_area(start, size, dir)
 *	- start	- kernel virtual start address
 *	- size	- size of region
 *	- dir	- DMA direction
 */
// --------------------------------------------------------------------------
// v7_dma_map_area/v7_dma_unmap_area: DMA 映射/取消映射时的缓存操作
// map:   DMA_FROM_DEVICE -> invalidate(丢弃CPU可能读取的陈数据)
//        其他方向 -> clean(确保CPU写入到达内存)
// unmap: DMA_TO_DEVICE -> 不操作(设备已读取最新数据)
//        其他方向 -> invalidate(CPU可能读取设备写入的数据)
// 这种策略最小化了不必要的缓存维护开销。
// --------------------------------------------------------------------------
SYM_TYPED_FUNC_START(v7_dma_map_area)
	add	r1, r1, r0
	teq	r2, #DMA_FROM_DEVICE
	beq	v7_dma_inv_range
	b	v7_dma_clean_range
SYM_FUNC_END(v7_dma_map_area)

/*
 *	dma_unmap_area(start, size, dir)
 *	- start	- kernel virtual start address
 *	- size	- size of region
 *	- dir	- DMA direction
 */
SYM_TYPED_FUNC_START(v7_dma_unmap_area)
	add	r1, r1, r0
	teq	r2, #DMA_TO_DEVICE
	bne	v7_dma_inv_range
	ret	lr
SYM_FUNC_END(v7_dma_unmap_area)
