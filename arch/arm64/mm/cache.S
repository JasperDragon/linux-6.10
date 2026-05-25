/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cache maintenance
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2012 ARM Ltd.
 */

//=============================================================================
// cache.S — ARM64 缓存一致性维护操作
//
// 本文件实现了针对不同一致性域（Point of Coherency/Unification/Persistence）
// 的缓存清理（clean）和失效（invalidate）操作。
//
// 关键概念：
//   PoU（Point of Unification）：统一点，核心内指令和数据缓存一致的点
//   PoC（Point of Coherency）：一致点，系统中所有主设备看到一致内存的点
//   PoP（Point of Persistence）：持久点，写入持久存储的点
//
// 主要函数：
//   指令/数据缓存一致性：
//     - caches_clean_inval_pou:     使 I-cache 和 D-cache 在 PoU 一致
//     - caches_clean_inval_user_pou: 用户空间的 I/D 缓存一致性
//     - icache_inval_pou:           仅 I-cache 在 PoU 失效
//     - dcache_clean_pou:           仅 D-cache 在 PoU 清理
//
//   数据缓存维护（PoC 级）：
//     - __pi_dcache_clean_inval_poc:    清理并失效 D-cache 到 PoC
//     - __pi_dcache_inval_poc:          失效 D-cache 到 PoC（处理部分行）
//     - __pi_dcache_clean_poc:          清理 D-cache 到 PoC
//
//   其它：
//     - __pi_dcache_inval_poc_nosync:   失效 D-cache 到 PoC（无 dsb 同步）
//     - __pi_dcache_clean_poc_nosync:   清理 D-cache 到 PoC（无 dsb 同步）
//     - __pi_dcache_clean_pop:          清理 D-cache 到 PoP（DC CVAP 指令）
//
// 优化：ARM64_HAS_CACHE_IDC 表示 D-cache 自动使 I-cache 一致（无需显式操作）
//       ARM64_HAS_CACHE_DIC 表示 I-cache 自动检测数据更新（无需显式失效）
//=============================================================================

#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/assembler.h>
#include <asm/cpufeature.h>
#include <asm/alternative.h>
#include <asm/asm-uaccess.h>

//=============================================================================
// 指令/数据缓存一致性（I/D Cache Coherency）— PoU 级操作
//
// 当代码写入内存区域并即将执行时，需要确保指令缓存（I-cache）和
// 数据缓存（D-cache）在 PoU 域中一致。
//
// caches_clean_inval_pou_macro:
//   核心宏，执行以下步骤：
//   1. 如果硬件支持 IDC（D-cache 自动使 I-cache 一致），只需 dsb ishst
//   2. 否则对指定区域逐行执行 D-cache 清理（DC CVAU）
//   3. 如果硬件支持 DIC（I-cache 自动检测数据更新），只需 isb
//   4. 否则逐行失效 I-cache
//
// caches_clean_inval_pou:
//   内核空间版本，调用上述宏后返回
//
// caches_clean_inval_user_pou:
//   用户空间版本，通过 uaccess_ttbr0_enable/disable 切换页表
//   访问用户地址空间。如果发生故障，返回 -EFAULT。
//
// icache_inval_pou:
//   仅失效 I-cache（硬件不支持 DIC 时使用）
//
// dcache_clean_pou:
//   仅清理 D-cache 到 PoU（硬件不支持 IDC 时使用）
//=============================================================================
/*
 *	caches_clean_inval_pou_macro(start,end) [fixup]
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 *	- fixup   - optional label to branch to on user fault
 */
.macro	caches_clean_inval_pou_macro, fixup
alternative_if ARM64_HAS_CACHE_IDC
	dsb     ishst
	b       .Ldc_skip_\@
alternative_else_nop_endif
	mov     x2, x0
	mov     x3, x1
	dcache_by_line_op cvau, ish, x2, x3, x4, x5, \fixup
.Ldc_skip_\@:
alternative_if ARM64_HAS_CACHE_DIC
	isb
	b	.Lic_skip_\@
alternative_else_nop_endif
	invalidate_icache_by_line x0, x1, x2, x3, \fixup
.Lic_skip_\@:
.endm

/*
 *	caches_clean_inval_pou(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(caches_clean_inval_pou)
	caches_clean_inval_pou_macro
	ret
SYM_FUNC_END(caches_clean_inval_pou)
SYM_FUNC_ALIAS(__pi_caches_clean_inval_pou, caches_clean_inval_pou)

/*
 *	caches_clean_inval_user_pou(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(caches_clean_inval_user_pou)
	uaccess_ttbr0_enable x2, x3, x4

	caches_clean_inval_pou_macro 2f
	mov	x0, xzr
1:
	uaccess_ttbr0_disable x1, x2
	ret
2:
	mov	x0, #-EFAULT
	b	1b
SYM_FUNC_END(caches_clean_inval_user_pou)

/*
 *	icache_inval_pou(start,end)
 *
 *	Ensure that the I cache is invalid within specified region.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(icache_inval_pou)
alternative_if ARM64_HAS_CACHE_DIC
	isb
	ret
alternative_else_nop_endif

	invalidate_icache_by_line x0, x1, x2, x3
	ret
SYM_FUNC_END(icache_inval_pou)

//=============================================================================
// 数据缓存维护 — PoC（Point of Coherency）级操作
//
// PoC 操作确保所有主设备（CPU、DMA、GPU）看到一致的内存视图。
// 用于 DMA 缓冲区准备/完成、设备内存同步等场景。
//
// __pi_dcache_clean_inval_poc: DC CIVAC + dsb sy
// __pi_dcache_clean_poc:      DC CVAC + dsb sy
// __pi_dcache_clean_poc_nosync: DC CVAC，无 dsb（调用者需自行同步）
// __pi_dcache_inval_poc:      区域两端处理部分行，DC CIVAC+IVAC + dsb sy
// __pi_dcache_inval_poc_nosync:同上，无 dsb
//=============================================================================
/*
 *	dcache_clean_inval_poc(start, end)
 *
 *	Ensure that any D-cache lines for the interval [start, end)
 *	are cleaned and invalidated to the PoC.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(__pi_dcache_clean_inval_poc)
	dcache_by_line_op civac, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_inval_poc)
SYM_FUNC_ALIAS(dcache_clean_inval_poc, __pi_dcache_clean_inval_poc)

/*
 *	dcache_clean_pou(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoU.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(dcache_clean_pou)
alternative_if ARM64_HAS_CACHE_IDC
	dsb	ishst
	ret
alternative_else_nop_endif
	dcache_by_line_op cvau, ish, x0, x1, x2, x3
	ret
SYM_FUNC_END(dcache_clean_pou)

//=============================================================================
// __dcache_inval_poc_nosync 宏 — 逐行失效 D-cache 到 PoC（无同步屏障）
//
// 对区域两端不完全缓存行执行"清理+失效"（CIVAC），防止相邻数据丢失。
// 中间对齐的行直接执行 DC IVAC。不包含 dsb，调用者需自行添加屏障。
//=============================================================================

.macro __dcache_inval_poc_nosync
	dcache_line_size x2, x3
	sub	x3, x2, #1
	tst	x1, x3				// end cache line aligned?
	bic	x1, x1, x3
	b.eq	1f
	dc	civac, x1			// clean & invalidate D / U line
1:	tst	x0, x3				// start cache line aligned?
	bic	x0, x0, x3
	b.eq	2f
	dc	civac, x0			// clean & invalidate D / U line
	b	3f
2:	dc	ivac, x0			// invalidate D / U line
3:	add	x0, x0, x2
	cmp	x0, x1
	b.lo	2b
.endm

/*
 *	dcache_inval_poc(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are invalidated. Any partial lines at the ends of the interval are
 *	also cleaned to PoC to prevent data loss.
 *
 *	- start   - kernel start address of region
 *	- end     - kernel end address of region
 */
SYM_FUNC_START(__pi_dcache_inval_poc)
	__dcache_inval_poc_nosync
	dsb	sy
	ret
SYM_FUNC_END(__pi_dcache_inval_poc)
SYM_FUNC_ALIAS(dcache_inval_poc, __pi_dcache_inval_poc)

/*
 *	dcache_inval_poc_nosync(start, end)
 *
 * 	Issue the instructions of D-cache lines for the interval [start, end)
 * 	for invalidation. Not necessarily cleaned to PoC till an explicit dsb
 *	sy is issued later
 *
 *	- start   - kernel start address of region
 *	- end     - kernel end address of region
 */
SYM_FUNC_START(__pi_dcache_inval_poc_nosync)
	__dcache_inval_poc_nosync
	ret
SYM_FUNC_END(__pi_dcache_inval_poc_nosync)
SYM_FUNC_ALIAS(dcache_inval_poc_nosync, __pi_dcache_inval_poc_nosync)

/*
 *	dcache_clean_poc(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoC.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(__pi_dcache_clean_poc)
	dcache_by_line_op cvac, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_poc)
SYM_FUNC_ALIAS(dcache_clean_poc, __pi_dcache_clean_poc)

/*
 *	dcache_clean_poc_nosync(start, end)
 *
 * 	Issue the instructions of D-cache lines for the interval [start, end).
 * 	not necessarily cleaned to the PoC till an explicit dsb sy afterward.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(__pi_dcache_clean_poc_nosync)
	dcache_by_line_op_nosync cvac, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_poc_nosync)
SYM_FUNC_ALIAS(dcache_clean_poc_nosync, __pi_dcache_clean_poc_nosync)

//=============================================================================
// dcache_clean_pop — 清理 D-cache 到 PoP（Point of Persistence）
//
// 使用 DC CVAP 指令将缓存行清理到持久存储点（如 NVDIMM 或持久内存）。
// 仅当 ARM64_HAS_DCPOP 特性被检测到时可用。
// 如果不支持，回退到 dcache_clean_poc（PoC 级清理）。
//=============================================================================
/*
 *	dcache_clean_pop(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoP.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(__pi_dcache_clean_pop)
	alternative_if_not ARM64_HAS_DCPOP
	b	dcache_clean_poc
	alternative_else_nop_endif
	dcache_by_line_op cvap, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END(__pi_dcache_clean_pop)
SYM_FUNC_ALIAS(dcache_clean_pop, __pi_dcache_clean_pop)
