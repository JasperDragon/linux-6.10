/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vlock.S - simple voting lock implementation for ARM
 *
 * Created by:	Dave Martin, 2012-08-16
 * Copyright:	(C) 2012-2013  Linaro Limited
 *
 * This algorithm is described in more detail in
 * Documentation/arch/arm/vlocks.rst.
 */

//
// 文件: arch/arm/common/vlock.S
// 描述: ARM 投票锁 (Voting Lock) 的汇编实现
// 功能: 用于多核处理器中"首位协调" (first-man coordination) 的简单自旋锁，
//       通过投票机制决定哪个 CPU 获得锁
// 算法: 参见 Documentation/arch/arm/vlocks.rst
// 特点: 适用于 Strongly-Ordered 或 Device 类型内存，最小化了内存屏障
//
#include <linux/linkage.h>
#include "vlock.h"

.arch armv7-a

// 根据 VLOCK_VOTING_SIZE 选择代码路径
// 如果投票标志可放入一个 word (<=4 字节)，使用 FEW 路径 (单次加载)
// 否则使用 MANY 路径 (循环加载多个 word)
#if VLOCK_VOTING_SIZE > 4
#define FEW(x...)
#define MANY(x...) x
#else
#define FEW(x...) x
#define MANY(x...)
#endif

// voting_begin: 开始投票 — 设置当前 CPU 的投票标志为 1
// 使用 DMB 确保投票对其他 CPU 可见
@ voting lock for first-man coordination

.macro voting_begin rbase:req, rcpu:req, rscratch:req
	mov	\rscratch, #1
	strb	\rscratch, [\rbase, \rcpu]
	dmb
.endm

// voting_end: 结束投票 — 清除当前 CPU 的投票标志
// 使用 DSB + SEV 确保清除操作完成并唤醒等待的 CPU
.macro voting_end rbase:req, rcpu:req, rscratch:req
	dmb
	mov	\rscratch, #0
	strb	\rscratch, [\rbase, \rcpu]
	dsb	st
	sev
.endm

/*
 * The vlock structure must reside in Strongly-Ordered or Device memory.
 * This implementation deliberately eliminates most of the barriers which
 * would be required for other memory types, and assumes that independent
 * writes to neighbouring locations within a cacheline do not interfere
 * with one another.
 */

// vlock_trylock: 尝试获取投票锁
// 参数: r0 = 锁结构基址, r1 = CPU ID (簇内 0 基索引)
// 返回: r0 = 0 (获取成功) 或非零 (获取失败)
// 算法: 投票 -> 检查锁是否空闲 -> 提交自己的票 -> 等待投票结束 -> 判断胜者
	@ r0: lock structure base
	@ r1: CPU ID (0-based index within cluster)
ENTRY(vlock_trylock)
	add	r1, r1, #VLOCK_VOTING_OFFSET

	voting_begin	r0, r1, r2

	ldrb	r2, [r0, #VLOCK_OWNER_OFFSET]	@ check whether lock is held
	cmp	r2, #VLOCK_OWNER_NONE
	bne	trylock_fail			@ fail if so

	@ Control dependency implies strb not observable before previous ldrb.

	strb	r1, [r0, #VLOCK_OWNER_OFFSET]	@ submit my vote

	voting_end	r0, r1, r2		@ implies DMB

	// 等待本轮所有 CPU 投票结束
	@ Wait for the current round of voting to finish:

 MANY(	mov	r3, #VLOCK_VOTING_OFFSET			)
0:
 MANY(	ldr	r2, [r0, r3]					)
 FEW(	ldr	r2, [r0, #VLOCK_VOTING_OFFSET]			)
	cmp	r2, #0
	wfene
	bne	0b
 MANY(	add	r3, r3, #4					)
 MANY(	cmp	r3, #VLOCK_VOTING_OFFSET + VLOCK_VOTING_SIZE	)
 MANY(	bne	0b						)

	// 检查谁获得了锁 — 比较自己的票和最终的锁拥有者
	// eor 结果为 0 表示自己获胜，否则失败
	@ Check who won:

	dmb
	ldrb	r2, [r0, #VLOCK_OWNER_OFFSET]
	eor	r0, r1, r2			@ zero if I won, else nonzero
	bx	lr

trylock_fail:
// 获取锁失败 — 结束投票并返回非零值
	voting_end	r0, r1, r2
	mov	r0, #1				@ nonzero indicates that I lost
	bx	lr
ENDPROC(vlock_trylock)

// vlock_unlock: 释放投票锁
// 参数: r0 = 锁结构基址
// 将锁拥有者设为 VLOCK_OWNER_NONE，使用 DSB + SEV 确保可见性
	@ r0: lock structure base
ENTRY(vlock_unlock)
	dmb
	mov	r1, #VLOCK_OWNER_NONE
	strb	r1, [r0, #VLOCK_OWNER_OFFSET]
	dsb	st
	sev
	bx	lr
ENDPROC(vlock_unlock)
