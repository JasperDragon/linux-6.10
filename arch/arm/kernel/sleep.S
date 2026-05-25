/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sleep.S -- ARM32 CPU休眠/唤醒支持代码
 *
 * 概述: 本文件实现ARM32 Linux内核的CPU suspend/resume机制。
 * 当系统进入休眠状态时,__cpu_suspend保存当前CPU的通用寄存器
 * 和架构相关状态到内核栈上;唤醒时cpu_resume从保存的状态恢复,
 * 重新启用MMU并返回到正常执行路径。
 * 另外还实现了MPIDR哈希计算宏,用于在SMP系统中根据MPIDR
 * 确定CPU在睡眠保存数组中的索引位置。
 *
 * suspend流程:
 *   1. __cpu_suspend保存寄存器并调用平台休眠函数
 *   2. 平台休眠函数将系统置于低功耗状态
 *   3. 唤醒时从cpu_resume开始执行
 *   4. cpu_resume_mmu重新启用MMU
 *   5. cpu_resume_after_mmu恢复CPU状态
 *   6. 返回到休眠前的执行路径
 */
#include <linux/linkage.h>
#include <linux/threads.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/glue-cache.h>
#include <asm/glue-proc.h>
		.text

/*
 * compute_mpidr_hash: MPIDR哈希计算宏
 * 通过移位和按位或操作计算CPU在睡眠保存数组中的索引。
 * 用于SMP系统中根据CPU的MPIDR(多处理器ID寄存器)值
 * 确定其在sleep_save_sp数组中的位置。
 *
 * 伪C代码:
 *   u32 mpidr_masked = mpidr & mask;
 *   aff0 = mpidr_masked & 0xff;
 *   aff1 = mpidr_masked & 0xff00;
 *   aff2 = mpidr_masked & 0xff0000;
 *   dst = (aff0 >> rs0 | aff1 >> rs1 | aff2 >> rs2);
 *
 * 输入: rs0, rs1, rs2 = 各affinity level的移位值
 *       mpidr = MPIDR寄存器值
 *       mask = MPIDR掩码
 * 输出: dst = 哈希结果(数组索引)
 * 注意: 输入和输出寄存器必须互不重叠
 */
	.macro compute_mpidr_hash dst, rs0, rs1, rs2, mpidr, mask
	and	\mpidr, \mpidr, \mask			@ 用掩码提取MPIDR相关位
	and	\dst, \mpidr, #0xff			@ 提取affinity level 0 (低8位)
 ARM(	mov	\dst, \dst, lsr \rs0		)	@ aff0右移rs0位
 THUMB(	lsr	\dst, \dst, \rs0		)
	and	\mask, \mpidr, #0xff00			@ 提取affinity level 1 (8-15位)
 ARM(	orr	\dst, \dst, \mask, lsr \rs1	)	@ aff1右移rs1位后并入dst
 THUMB(	lsr	\mask, \mask, \rs1		)
 THUMB(	orr	\dst, \dst, \mask		)
	and	\mask, \mpidr, #0xff0000		@ 提取affinity level 2 (16-23位)
 ARM(	orr	\dst, \dst, \mask, lsr \rs2	)	@ aff2右移rs2位后并入dst
 THUMB(	lsr	\mask, \mask, \rs2		)
 THUMB(	orr	\dst, \dst, \mask		)
	.endm

/*
 * __cpu_suspend: 保存CPU状态并进入休眠
 * 保存当前CPU的通用寄存器(r4-r11, lr)到内核栈,
 * 分配空间保存CPU特定寄存器(MMU页表基址、内核栈指针、物理恢复函数地址)。
 * 使用overflow栈(启用VMAP_STACK时)以避免物理地址转换问题。
 * 在SMP系统中,根据MPIDR计算CPU在sleep_save_sp数组中的索引。
 *
 * 输入:
 *   r0 = suspend function arg0 (休眠函数参数)
 *   r1 = suspend function (休眠函数指针)
 *   r2 = MPIDR value for resuming CPU (唤醒CPU的MPIDR值)
 */
ENTRY(__cpu_suspend)
	stmfd	sp!, {r4 - r11, lr}		@ 保存通用寄存器(被调用者保存)
#ifdef MULTI_CPU
	ldr	r10, =processor
	ldr	r4, [r10, #CPU_SLEEP_SIZE]	@ 多CPU: 获取CPU睡眠状态大小
#else
	ldr	r4, =cpu_suspend_size		@ 单CPU: 获取睡眠状态大小
#endif
	mov	r5, sp				@ 保存当前SP(虚拟地址)
#ifdef CONFIG_VMAP_STACK
	@ 从overflow栈执行suspend代码,避免在arch suspend代码中使用
	@ vmalloc到物理地址的转换。
	@ 原始SP值保存在R5中,返回时恢复。
	ldr_this_cpu sp, overflow_stack_ptr, r6, r7	@ 切换到overflow栈
#endif
	add	r4, r4, #12			@ 额外空间: pgd, virt sp, phys resume fn
	sub	sp, sp, r4			@ 在内核栈上分配睡眠状态空间
	ldr	r3, =sleep_save_sp		@ 加载睡眠保存数组基地址
	stmfd	sp!, {r0, r1}			@ 保存休眠函数参数和指针
	ldr	r3, [r3, #SLEEP_SAVE_SP_VIRT]	@ 加载虚拟地址基址
	ALT_SMP(W(nop))				@ SMP: 空操作(不使用adr_l)
	ALT_UP_B(1f)				@ UP: 跳过MPIDR哈希计算
	adr_l	r0, mpidr_hash			@ 加载mpidr_hash结构地址
	/* 此ldmia依赖于mpidr_hash结构的内存布局 */
	ldmia	r0, {r1, r6-r8}			@ r1=MPIDR掩码 (r6,r7,r8)=l[0,1,2]移位值
	compute_mpidr_hash	r0, r6, r7, r8, r2, r1	@ 计算CPU在数组中的索引
	add	r3, r3, r0, lsl #2		@ 加上索引偏移
1:	mov	r2, r5				@ 内核栈虚拟地址
	mov	r1, r4				@ 保存块大小
	add	r0, sp, #8			@ 保存块指针(跳过r0,r1)
	bl	__cpu_suspend_save		@ 调用C函数保存CPU状态
	badr	lr, cpu_suspend_abort		@ 设置异常返回地址
	ldmfd	sp!, {r0, pc}			@ 加载并调用休眠函数
ENDPROC(__cpu_suspend)
	.ltorg

/*
 * cpu_suspend_abort: 休眠中止恢复
 * 当休眠函数返回非零值时,从中止路径恢复执行。
 * 从栈中弹出保存的页表基址、内核栈和恢复函数参数,
 * 恢复原始SP和通用寄存器。
 */
cpu_suspend_abort:
	ldmia	sp!, {r1 - r3}			@ 弹出phys pgd, virt SP, phys resume fn
	teq	r0, #0				@ 检查返回值?
	moveq	r0, #1				@ 为零则强制设为非零(表示失败)
	mov	sp, r2				@ 恢复原始内核栈指针
	ldmfd	sp!, {r4 - r11, pc}		@ 恢复通用寄存器并返回
ENDPROC(cpu_suspend_abort)

/*
 * cpu_resume_mmu: 唤醒时重新启用MMU
 * 在标识映射区域(.idmap.text)中执行,确保MMU切换期间不会发生地址错乱。
 * 设置SCTLR(System Control Register)启用MMU和缓存,
 * 然后跳转到虚拟地址空间的cpu_resume_after_mmu。
 *
 * 输入: r0 = control register value (SCTLR值)
 */
	.align	5
	.pushsection	.idmap.text,"ax"
ENTRY(cpu_resume_mmu)
	ldr	r3, =cpu_resume_after_mmu	@ 加载虚拟地址空间中的目标地址
	instr_sync				@ 指令同步屏障
	mcr	p15, 0, r0, c1, c0, 0		@ 写入SCTLR: 开启MMU、I-cache等
	mrc	p15, 0, r0, c0, c0, 0		@ 读取MIDR(处理器ID)
	instr_sync				@ 指令同步屏障
	mov	r0, r0				@ 空操作(用于MMU启用后的流水线刷新)
	mov	r0, r0
	ret	r3				@ 跳转到虚拟地址(MMU已启用)
ENDPROC(cpu_resume_mmu)
	.popsection

/*
 * cpu_resume_after_mmu: MMU启用后的CPU恢复
 * 恢复异常模式寄存器(und/abt/irq banked regs),
 * 在VMAP_STACK配置下,切换页表基址使vmalloc区域可用。
 * 返回0表示恢复成功。
 */
cpu_resume_after_mmu:
#if defined(CONFIG_VMAP_STACK) && !defined(CONFIG_ARM_LPAE)
	@ 使用vmap栈前必须切换到swapper_pg_dir,
	@ 因为标识映射不覆盖vmalloc区域。
	mrc	p15, 0, ip, c2, c0, 1		@ 读取TTBR1(内核页表基址)
	mcr	p15, 0, ip, c2, c0, 0		@ 写入TTBR0(切换为内核页表)
	instr_sync				@ 指令同步屏障
#endif
	bl	cpu_init			@ 恢复und/abt/irq模式下的banked寄存器
#if defined(CONFIG_KASAN) && defined(CONFIG_KASAN_STACK)
	mov	r0, sp
	bl	kasan_unpoison_task_stack_below	@ KASAN: 取消毒化栈内存
#endif
	mov	r0, #0				@ 返回0表示成功
	ldmfd	sp!, {r4 - r11, pc}		@ 恢复通用寄存器并返回
ENDPROC(cpu_resume_after_mmu)

	.text
	.align

#ifdef CONFIG_MCPM
/*
 * cpu_resume_no_hyp: 非Hyp模式唤醒入口(MCPM)
 * 多簇功耗管理(MCPM)使用的唤醒入口,跳过Hyp模式初始化。
 */
	.arm
THUMB(	.thumb			)
ENTRY(cpu_resume_no_hyp)
ARM_BE8(setend be)			@ 确保在大端模式运行
	b	no_hyp				@ 跳转到通用恢复路径
#endif

#ifdef CONFIG_MMU
/*
 * cpu_resume_arm: ARM模式唤醒入口
 * 处理Thumb-2内核从ARM模式的启动:
 * 如果内核是Thumb-2,从ARM模式切换到Thumb模式。
 */
	.arm
ENTRY(cpu_resume_arm)
 THUMB(	badr	r9, 1f		)	@ ARM模式下获取Thumb标签地址
 THUMB(	bx	r9		)	@ 切换到Thumb模式
 THUMB(	.thumb			)	@ 后续代码为Thumb指令
 THUMB(1:			)
#endif

/*
 * cpu_resume: CPU唤醒主入口
 * 从休眠状态恢复CPU执行。执行以下步骤:
 *   1. 安装Hyp存根(ARM虚拟化扩展)
 *   2. 设置SVC模式并禁用中断
 *   3. 根据MPIDR计算当前CPU在睡眠保存数组中的索引
 *   4. 从sleep_save_sp加载保存的页表基址、内核栈和恢复函数地址
 *   5. 跳转到cpu_resume_mmu重新启用MMU
 */
ENTRY(cpu_resume)
ARM_BE8(setend be)			@ 确保在大端模式运行
#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install_secondary	@ 安装Hyp存根(二级CPU)
#endif
	safe_svcmode_maskall r1		@ 切换到SVC模式并屏蔽所有中断
no_hyp:
	mov	r1, #0				@ 清零r1(UP:表示CPU 0)
	ALT_SMP(mrc p15, 0, r0, c0, c0, 5)	@ SMP:读取MPIDR
	ALT_UP_B(1f)				@ UP:跳转到1(使用CPU 0的槽位)
	adr_l	r2, mpidr_hash			@ r2 = mpidr_hash结构物理地址

	/*
	 * 此ldmia依赖于mpidr_hash结构的内存布局。
	 */
	ldmia	r2, { r3-r6 }			@ r3=MPIDR掩码 (r4,r5,r6)=l[0,1,2]移位值
	compute_mpidr_hash	r1, r4, r5, r6, r0, r3	@ 计算CPU在数组中的索引
1:
	ldr_l	r0, sleep_save_sp + SLEEP_SAVE_SP_PHYS	@ 加载睡眠保存数组物理基址
	ldr	r0, [r0, r1, lsl #2]		@ 根据索引获取本CPU的保存块

	@ 加载保存的页表基址、内核栈和恢复函数地址
  ARM(	ldmia	r0!, {r1, sp, pc}	)	@ r1=phys pgd, sp=kernel stack, pc=resume fn
THUMB(	ldmia	r0!, {r1, r2, r3}	)	@ Thumb: 分步加载
THUMB(	mov	sp, r2			)
THUMB(	bx	r3			)
ENDPROC(cpu_resume)

#ifdef CONFIG_MMU
ENDPROC(cpu_resume_arm)
#endif
#ifdef CONFIG_MCPM
ENDPROC(cpu_resume_no_hyp)
#endif

/*
 * sleep_save_sp: 睡眠状态保存数组
 * 每个CPU在数组中占用一个指针槽位,指向保存的CPU状态块。
 * 数组大小在SLEEP_SAVE_SP_SZ中定义,编译时确定。
 */
	.data
	.align	2
	.type	sleep_save_sp, #object
ENTRY(sleep_save_sp)
	.space	SLEEP_SAVE_SP_SZ		@ struct sleep_save_sp数组空间
