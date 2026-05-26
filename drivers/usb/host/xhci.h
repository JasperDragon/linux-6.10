/* SPDX-License-Identifier: GPL-2.0 */

/*
 * xHCI host controller driver
 *
 * Copyright (C) 2008 Intel Corp.
 *
 * Author: Sarah Sharp
 * Some code borrowed from the Linux EHCI driver.
 */

/*
 * =============================================================================
 * xhci.h - XHCI (eXtensible Host Controller Interface) 主控器驱动主头文件
 * =============================================================================
 *
 * 概述:
 * ------
 * XHCI (eXtensible Host Controller Interface) 是 USB 3.x / SuperSpeed 的
 * 标准主机控制器接口规范，同时向后兼容 USB 2.0/1.1。本文件定义了 XHCI 硬件
 * 相关的所有关键数据结构和常量，是 XHCI 驱动最核心的头文件。
 *
 * 一、TRB (Transfer Request Block) — 传输请求块
 *     TRB 是 XHCI 的基本传输单元，固定 16 字节大小，由 4 个 32-bit 字段组成:
 *       - Field 0-1 (64-bit): Data Buffer Pointer, 指向数据传输缓冲区
 *       - Field 2 (32-bit): 状态/传输长度信息
 *       - Field 3 (32-bit): 控制信息，包含 TRB 类型、Cycle 位等
 *     TRB 类型包括:
 *       - Normal TRB: 批量/中断/等时传输的数据 TRB
 *       - Setup Stage TRB: 控制传输的设置阶段
 *       - Data Stage TRB: 控制传输的数据阶段
 *       - Status Stage TRB: 控制传输的状态阶段
 *       - Link TRB: 连接 Transfer Ring 的不同 segment
 *       - Event Data TRB: 携带额外事件数据
 *       - No-Op TRB: 空操作
 *     Completion Code (完成码) 表示 TRB 的执行结果:
 *       - COMP_SUCCESS (1): 成功
 *       - COMP_STALL_ERROR (6): 端点返回 STALL
 *       - COMP_SHORT_PACKET (13): 短包 (实际数据少于期望)
 *       - COMP_USB_TRANSACTION_ERROR (4): USB 事务错误
 *       等等，详见 COMP_* 宏定义。
 *
 * 二、Transfer Ring — 传输环
 *     Transfer Ring 是驱动与 HC 之间通信的循环队列:
 *       - 驱动 (软件/HCD) 作为生产者，写入 TRB 到 Ring 中
 *       - HC (硬件) 作为消费者，读取并执行 Ring 中的 TRB
 *       - 每个 Ring 由多个 segment 组成，通过 Link TRB 连接成逻辑环
 *       - Cycle State (toggle bit) 控制 TRB 的所有权: Cycle=1 表示 HC 拥有，
 *         Cycle=0 表示驱动拥有。每次绕环一周 toggle 一次。
 *
 * 三、Event Ring — 事件环
 *     HC 完成 TRB 处理后，将完成状态写入 Event Ring:
 *       - Transfer Event: 数据传输完成事件
 *       - Command Completion Event: 命令完成事件
 *       - Port Status Change Event: 端口状态变化事件
 *       - 等等
 *     驱动从中读取事件，更新 URB 状态并唤醒等待的进程。
 *
 * 四、Device Context — 设备上下文
 *     每个 USB 设备对应一个 Slot Context (槽上下文)，包含最多 31 个
 *     Endpoint Context (端点上下文):
 *       - Slot Context: 设备路由、速度、地址、状态等信息
 *       - Endpoint Context: 端点类型、最大包长、Ring 基址、流信息等
 *     DCBAA (Device Context Base Address Array) 是 HC 内部数组，
 *     存放所有设备上下文的 DMA 地址。
 *
 * 五、关键寄存器
 *     - Capability Registers (能力寄存器): HC 版本号、结构参数等只读信息
 *     - Operational Registers (操作寄存器): 命令、状态、配置等运行时控制
 *     - Runtime Registers (运行时寄存器): 微帧计数器、中断寄存器组
 *     - Doorbell Array (门铃数组): 驱动通过写 Doorbell 通知 HC 处理新 TRB
 *       每个端点对应一个 Doorbell，写 Doorbell 相当于"敲门"告诉 HC 有活要干。
 *
 * 六、Input/Output Context
 *     - Input Context: 驱动发给 HC 的上下文变更请求
 *     - Output Context: HC 执行命令后返回的上下文结果
 *     通过 Address Device、Configure Endpoint 等命令来变更设备配置。
 *
 * 本文件中的数据结构与 xHCI 规范 (www.intel.com 提供) 第 5-6 章直接对应。
 * =============================================================================
 */

#ifndef __LINUX_XHCI_HCD_H
#define __LINUX_XHCI_HCD_H

#include <linux/bits.h>
#include <linux/usb.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/usb/hcd.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/io-64-nonatomic-hi-lo.h>

/* Code sharing between pci-quirks and xhci hcd */
#include	"xhci-ext-caps.h"
#include "pci-quirks.h"

#include "xhci-port.h"
#include "xhci-caps.h"

/* max buffer size for trace and debug messages */
#define XHCI_MSG_MAX		500

/* xHCI PCI Configuration Registers */
#define XHCI_SBRN_OFFSET	(0x60)

/* Max number of USB devices for any host controller - limit in section 6.1 */
#define MAX_HC_SLOTS		256
/*
 * Max Number of Ports. xHCI specification section 5.3.3
 * Valid values are in the range of 1 to 255.
 */
#define MAX_HC_PORTS		127
/*
 * Max number of Interrupter Register Sets. xHCI specification section 5.3.3
 * Valid values are in the range of 1 to 1024.
 */
#define MAX_HC_INTRS		128

/*
 * xHCI register interface.
 * This corresponds to the eXtensible Host Controller Interface (xHCI)
 * Revision 0.95 specification
 */

/**
 * struct xhci_cap_regs - xHCI Host Controller Capability Registers.
 * @hc_capbase:		length of the capabilities register and HC version number
 * @hcs_params1:	HCSPARAMS1 - Structural Parameters 1
 * @hcs_params2:	HCSPARAMS2 - Structural Parameters 2
 * @hcs_params3:	HCSPARAMS3 - Structural Parameters 3
 * @hcc_params:		HCCPARAMS - Capability Parameters
 * @db_off:		DBOFF - Doorbell array offset
 * @run_regs_off:	RTSOFF - Runtime register space offset
 * @hcc_params2:	HCCPARAMS2 Capability Parameters 2, xhci 1.1 only
 */
struct xhci_cap_regs {
	__le32	hc_capbase;
	__le32	hcs_params1;
	__le32	hcs_params2;
	__le32	hcs_params3;
	__le32	hcc_params;
	__le32	db_off;
	__le32	run_regs_off;
	__le32	hcc_params2; /* xhci 1.1 */
	/* Reserved up to (CAPLENGTH - 0x1C) */
};

/*
 * struct xhci_port_regs - USB 端口寄存器组, xHCI 规范 5.4.8
 *
 * 每个 USB 端口 (包括 USB2 和 USB3) 对应一组端口寄存器:
 *
 * @portsc:      Port Status and Control (端口状态与控制)
 *               包含连接状态、使能/禁用、复位、链路状态 (U0/U1/U2/U3)、
 *               速度、电源控制、唤醒配置等。是所有端口寄存器中最核心的。
 *               USB2 和 USB3 端口的 portsc 布局略有不同:
 *               - USB2: 包含 PEC (端口使能变更) 等位
 *               - USB3: 包含 WRC (预热复位完成) 等位
 * @portpmsc:    Port Power Management Status and Control
 *               (端口电源管理状态与控制) — LPM、U1/U2 超时等
 * @portli:      Port Link Info (端口链路信息)
 *               提供链路错误计数等调试信息
 * @porthlmpc:   Port Hardware LPM Control
 *               (端口硬件 LPM 控制) — 硬件辅助 LPM 配置
 *
 * 端口编号约定:
 *   - hw_portnum: HC 硬件端口号 (从 1 开始)
 *   - hcd_portnum: HCD 驱动端口号 (从 1 开始)
 *   - USB2 和 USB3 端口共享同一编号空间，通过不同的根 Hub 访问
 */
struct xhci_port_regs {
	__le32	portsc;
	__le32	portpmsc;
	__le32	portli;
	__le32	porthlmpc;
};

/**
 * struct xhci_op_regs - xHCI Host Controller Operational Registers.
 * @command:		USBCMD - xHC command register
 * @status:		USBSTS - xHC status register
 * @page_size:		This indicates the page size that the host controller
 * 			supports.  If bit n is set, the HC supports a page size
 * 			of 2^(n+12), up to a 128MB page size.
 * 			4K is the minimum page size.
 * @cmd_ring:		CRP - 64-bit Command Ring Pointer
 * @dcbaa_ptr:		DCBAAP - 64-bit Device Context Base Address Array Pointer
 * @config_reg:		CONFIG - Configure Register
 * @port_regs:		Port Register Sets, from 1 to MaxPorts (defined by HCSPARAMS1).
 */
struct xhci_op_regs {
	__le32	command;
	__le32	status;
	__le32	page_size;
	__le32	reserved1;
	__le32	reserved2;
	__le32	dev_notification;
	__le64	cmd_ring;
	/* rsvd: offset 0x20-2F */
	__le32	reserved3[4];
	__le64	dcbaa_ptr;
	__le32	config_reg;
	/* rsvd: offset 0x3C-3FF */
	__le32	reserved4[241];
	struct xhci_port_regs port_regs[];
};

/* USBCMD - USB command - command bitmasks */
/* start/stop HC execution - do not write unless HC is halted*/
#define CMD_RUN		XHCI_CMD_RUN
/* Reset HC - resets internal HC state machine and all registers (except
 * PCI config regs).  HC does NOT drive a USB reset on the downstream ports.
 * The xHCI driver must reinitialize the xHC after setting this bit.
 */
#define CMD_RESET	BIT(1)
/* Event Interrupt Enable - a '1' allows interrupts from the host controller */
#define CMD_EIE		XHCI_CMD_EIE
/* Host System Error Interrupt Enable - get out-of-band signal for HC errors */
#define CMD_HSEIE	XHCI_CMD_HSEIE
/* bits 4:6 are reserved (and should be preserved on writes). */
/* light reset (port status stays unchanged) - reset completed when this is 0 */
#define CMD_LRESET	BIT(7)
/* host controller save/restore state. */
#define CMD_CSS		BIT(8)
#define CMD_CRS		BIT(9)
/* Enable Wrap Event - '1' means xHC generates an event when MFINDEX wraps. */
#define CMD_EWE		XHCI_CMD_EWE
/* MFINDEX power management - '1' means xHC can stop MFINDEX counter if all root
 * hubs are in U3 (selective suspend), disconnect, disabled, or powered-off.
 * '0' means the xHC can power it off if all ports are in the disconnect,
 * disabled, or powered-off state.
 */
#define CMD_PM_INDEX	BIT(11)
/* bit 14 Extended TBC Enable, changes Isoc TRB fields to support larger TBC */
#define CMD_ETE		BIT(14)
/* bits 15:31 are reserved (and should be preserved on writes). */

#define XHCI_RESET_LONG_USEC		(10 * 1000 * 1000)
#define XHCI_RESET_SHORT_USEC		(250 * 1000)

/* USBSTS - USB status - status bitmasks */
/* HC not running - set to 1 when run/stop bit is cleared. */
#define STS_HALT	XHCI_STS_HALT
/* serious error, e.g. PCI parity error.  The HC will clear the run/stop bit. */
#define STS_FATAL	BIT(2)
/* event interrupt - clear this prior to clearing any IP flags in IR set*/
#define STS_EINT	BIT(3)
/* port change detect */
#define STS_PORT	BIT(4)
/* bits 5:7 reserved and zeroed */
/* save state status - '1' means xHC is saving state */
#define STS_SAVE	BIT(8)
/* restore state status - '1' means xHC is restoring state */
#define STS_RESTORE	BIT(9)
/* true: save or restore error */
#define STS_SRE		BIT(10)
/* true: Controller Not Ready to accept doorbell or op reg writes after reset */
#define STS_CNR		XHCI_STS_CNR
/* true: internal Host Controller Error - SW needs to reset and reinitialize */
#define STS_HCE		BIT(12)
/* bits 13:31 reserved and should be preserved */

/*
 * DNCTRL - Device Notification Control Register - dev_notification bitmasks
 * Generate a device notification event when the HC sees a transaction with a
 * notification type that matches a bit set in this bit field.
 */
#define	DEV_NOTE_MASK		(0xffff)
/* Most of the device notification types should only be used for debug.
 * SW does need to pay attention to function wake notifications.
 */
#define	DEV_NOTE_FWAKE		BIT(1)

/* CRCR - Command Ring Control Register - cmd_ring bitmasks */
/* bit 0 - Cycle bit indicates the ownership of the command ring */
#define CMD_RING_CYCLE		BIT(0)
/* stop ring operation after completion of the currently executing command */
#define CMD_RING_PAUSE		BIT(1)
/* stop ring immediately - abort the currently executing command */
#define CMD_RING_ABORT		BIT(2)
/* true: command ring is running */
#define CMD_RING_RUNNING	BIT(3)
/* bits 63:6 - Command Ring pointer */
#define CMD_RING_PTR_MASK	GENMASK_ULL(63, 6)

/* CONFIG - Configure Register - config_reg bitmasks */
/* bits 0:7 - maximum number of device slots enabled (NumSlotsEn) */
#define MAX_DEVS(p)	((p) & 0xff)
/* bit 8: U3 Entry Enabled, assert PLC when root port enters U3, xhci 1.1 */
#define CONFIG_U3E		BIT(8)
/* bit 9: Configuration Information Enable, xhci 1.1 */
#define CONFIG_CIE		BIT(9)
/* bits 10:31 - reserved and should be preserved */

/* bits 15:0 - HCD page shift bit */
#define XHCI_PAGE_SIZE_MASK     0xffff

/**
 * struct xhci_intr_reg - Interrupt Register Set, v1.2 section 5.5.2.
 * @iman:		IMAN - Interrupt Management Register. Used to enable
 *			interrupts and check for pending interrupts.
 * @imod:		IMOD - Interrupt Moderation Register. Used to throttle interrupts.
 * @erst_size:		ERSTSZ - Number of segments in the Event Ring Segment Table (ERST).
 * @erst_base:		ERSTBA - Event ring segment table base address.
 * @erst_dequeue:	ERDP - Event ring dequeue pointer.
 *
 * Each interrupter (defined by a MSI-X vector) has an event ring and an Event
 * Ring Segment Table (ERST) associated with it.  The event ring is comprised of
 * multiple segments of the same size.  The HC places events on the ring and
 * "updates the Cycle bit in the TRBs to indicate to software the current
 * position of the Enqueue Pointer." The HCD (Linux) processes those events and
 * updates the dequeue pointer.
 */
struct xhci_intr_reg {
	__le32	iman;
	__le32	imod;
	__le32	erst_size;
	__le32	rsvd;
	__le64	erst_base;
	__le64	erst_dequeue;
};

/* iman bitmasks */
/* bit 0 - Interrupt Pending (IP), whether there is an interrupt pending. Write-1-to-clear. */
#define	IMAN_IP			BIT(0)
/* bit 1 - Interrupt Enable (IE), whether the interrupter is capable of generating an interrupt */
#define	IMAN_IE			BIT(1)

/* imod bitmasks */
/*
 * bits 15:0 - Interrupt Moderation Interval, the minimum interval between interrupts
 * (in 250ns intervals). The interval between interrupts will be longer if there are no
 * events on the event ring. Default is 4000 (1 ms).
 */
#define IMODI_MASK		(0xffff)
/* bits 31:16 - Interrupt Moderation Counter, used to count down the time to the next interrupt */
#define IMODC_MASK		(0xffff << 16)

/* erst_size bitmasks */
/* bits 15:0 - Event Ring Segment Table Size, number of ERST entries */
#define	ERST_SIZE_MASK		(0xffff)

/* erst_base bitmasks */
/* bits 63:6 - Event Ring Segment Table Base Address Register */
#define ERST_BASE_ADDRESS_MASK	GENMASK_ULL(63, 6)

/* erst_dequeue bitmasks */
/*
 * bits 2:0 - Dequeue ERST Segment Index (DESI), is the segment number (or alias) where the
 * current dequeue pointer lies. This is an optional HW hint.
 */
#define ERST_DESI_MASK		(0x7)
/*
 * bit 3 - Event Handler Busy (EHB), whether the event ring is scheduled to be serviced by
 * a work queue (or delayed service routine)?
 */
#define ERST_EHB		BIT(3)
/* bits 63:4 - Event Ring Dequeue Pointer */
#define ERST_PTR_MASK		GENMASK_ULL(63, 4)

/**
 * struct xhci_run_regs
 * @microframe_index:
 * 		MFINDEX - current microframe number
 *
 * Section 5.5 Host Controller Runtime Registers:
 * "Software should read and write these registers using only Dword (32 bit)
 * or larger accesses"
 */
struct xhci_run_regs {
	__le32			microframe_index;
	__le32			rsvd[7];
	struct xhci_intr_reg	ir_set[1024];
};

/**
 * struct doorbell_array - XHCI Doorbell Array (门铃数组)
 *
 * 门铃机制是 XHCI 的核心通知机制。驱动将一个 TRB 写入 Transfer Ring 后，
 * 必须写对应端点的 Doorbell 寄存器来通知 HC 处理新 TRB。
 *
 * Doorbell 寄存器的位布局:
 * Bits  0 -  7: Endpoint target (端点目标)
 *               0 = Command Ring, 1-31 = EP0-EP30
 * Bits  8 - 15: RsvdZ (保留)
 * Bits 16 - 31: Stream ID (流 ID)
 *
 * Slot 0 的 Doorbell 用于通知 Command Ring，非 0 Slot 的 Doorbell
 * 用于通知对应端点的 Transfer Ring。
 *
 * Section 5.6
 */
struct xhci_doorbell_array {
	__le32	doorbell[256];
};

#define DB_VALUE(ep, stream)	((((ep) + 1) & 0xff) | ((stream) << 16))
#define DB_VALUE_HOST		0x00000000

#define PLT_MASK        (0x03 << 6)
#define PLT_SYM         (0x00 << 6)
#define PLT_ASYM_RX     (0x02 << 6)
#define PLT_ASYM_TX     (0x03 << 6)

/**
 * struct xhci_container_ctx - 设备上下文或输入上下文的容器
 * @type: 上下文类型, 用于计算内部子上下文的偏移
 *        XHCI_CTX_TYPE_DEVICE (0x1) = 输出上下文 (HC 返回给驱动)
 *        XHCI_CTX_TYPE_INPUT  (0x2) = 输入上下文 (驱动发给 HC)
 * @size: 上下文数据的总大小 (字节)
 * @bytes: 指向原始上下文内存的指针，该内存直接暴露给硬件
 * @dma: bytes 的 DMA 地址
 *
 * 每个上下文容器由以下部分按顺序组成:
 *   1. Input Control Context (仅 Input Context 有) — 8 字节
 *      指示哪些端点上下文被添加或删除
 *   2. Slot Context — 32 或 64 字节
 *      设备级别的信息: 路由、速度、地址、状态
 *   3. Endpoint Context 0 .. 30 — 各 32 或 64 字节
 *      每个端点的配置: 类型、Ring 地址、最大包长等
 *
 * 输入上下文 (Input Context) 是驱动发给 HC 的请求，告之想如何变更设备状态。
 * 输出上下文 (Output Context/Device Context) 是 HC 维护的设备当前状态，
 * 驱动可通过命令让 HC 更新该状态。
 */
struct xhci_container_ctx {
	unsigned type;
#define XHCI_CTX_TYPE_DEVICE  0x1
#define XHCI_CTX_TYPE_INPUT   0x2

	int size;

	u8 *bytes;
	dma_addr_t dma;
};

/**
 * struct xhci_slot_ctx - Slot Context (槽上下文), XHCI 规范 6.2.1.1
 *
 * Slot Context 描述一个 USB 设备的全局属性。每个通过 Enable Slot 命令
 * 分配的设备 Slot 都有一个对应的 Slot Context。它是设备上下文的第一个
 * 部分，紧接着是 Endpoint Contexts。
 *
 * 上下文大小: 32 字节 (如果 HC 使用 64 字节上下文模式，则有额外 32 字节保留)
 *
 * @dev_info:   设备信息 — 包含 Route String (路由字符串，描述拓扑路径)、
 *              设备速度、是否为 Hub、以及最后一个有效端点索引
 * @dev_info2:  附加信息 — 最大退出延迟 MEL (Max Exit Latency, 单位 us)、
 *              根 Hub 端口号、以及 Hub 设备的最大端口数
 * @tt_info:    事务翻译器信息 — 用于构造 Split Transaction Token。
 *              当 LS/FS 设备通过 HS Hub 连接时需要此信息。
 *              TT Hub Slot ID、TT 端口号和 Hub 的 Think Time。
 * @dev_state:  设备地址与状态 — 包含 HC 分配的 USB 设备地址 (8-bit)
 *              和 Slot 状态机 (27:31 位)
 *
 * Slot State 状态转换:
 *   DISABLED (0) -> DEFAULT (1) -> ADDRESSED (2) -> CONFIGURED (3)
 *   Address Device 命令将状态从 DEFAULT 推进到 ADDRESSED
 *   Configure Endpoint 命令将状态从 ADDRESSED 推进到 CONFIGURED
 */
struct xhci_slot_ctx {
	__le32	dev_info;
	__le32	dev_info2;
	__le32	tt_info;
	__le32	dev_state;
	/* offset 0x10 to 0x1f reserved for HC internal use */
	__le32	reserved[4];
};

/* dev_info bitmasks */
/* Route String - 0:19 */
#define ROUTE_STRING_MASK	(0xfffff)
/* Device speed - values defined by PORTSC Device Speed field - 20:23 */
#define DEV_SPEED	(0xf << 20)
#define GET_DEV_SPEED(n) (((n) & DEV_SPEED) >> 20)
/* bit 24 reserved */
/* Is this LS/FS device connected through a HS hub? - bit 25 */
#define DEV_MTT		BIT(25)
/* Set if the device is a hub - bit 26 */
#define DEV_HUB		BIT(26)
/* Index of the last valid endpoint context in this device context - 27:31 */
#define LAST_CTX_MASK	(0x1f << 27)
#define LAST_CTX(p)	((p) << 27)
#define LAST_CTX_TO_EP_NUM(p)	(((p) >> 27) - 1)
#define SLOT_FLAG	BIT(0)
#define EP0_FLAG	BIT(1)

/* dev_info2 bitmasks */
/* Max Exit Latency (ms) - worst case time to wake up all links in dev path */
#define MAX_EXIT	(0xffff)
/* Root hub port number that is needed to access the USB device */
#define ROOT_HUB_PORT(p)	(((p) & 0xff) << 16)
#define DEVINFO_TO_ROOT_HUB_PORT(p)	(((p) >> 16) & 0xff)
/* Maximum number of ports under a hub device */
#define XHCI_MAX_PORTS(p)	(((p) & 0xff) << 24)
#define DEVINFO_TO_MAX_PORTS(p)	(((p) & (0xff << 24)) >> 24)

/* tt_info bitmasks */
/*
 * TT Hub Slot ID - for low or full speed devices attached to a high-speed hub
 * The Slot ID of the hub that isolates the high speed signaling from
 * this low or full-speed device.  '0' if attached to root hub port.
 */
#define TT_SLOT		(0xff)
/*
 * The number of the downstream facing port of the high-speed hub
 * '0' if the device is not low or full speed.
 */
#define TT_PORT		(0xff << 8)
#define TT_THINK_TIME(p)	(((p) & 0x3) << 16)
#define GET_TT_THINK_TIME(p)	(((p) & (0x3 << 16)) >> 16)

/* dev_state bitmasks */
/* USB device address - assigned by the HC */
#define DEV_ADDR_MASK	(0xff)
/* bits 8:26 reserved */
/* Slot state */
#define SLOT_STATE	(0x1f << 27)
#define GET_SLOT_STATE(p)	(((p) & (0x1f << 27)) >> 27)

#define SLOT_STATE_DISABLED	0
#define SLOT_STATE_ENABLED	SLOT_STATE_DISABLED
#define SLOT_STATE_DEFAULT	1
#define SLOT_STATE_ADDRESSED	2
#define SLOT_STATE_CONFIGURED	3

/**
 * struct xhci_ep_ctx - Endpoint Context (端点上下文), XHCI 规范 6.2.1.2
 *
 * 每个 USB 端点 (除默认控制端点 EP0 外最多 30 个) 对应一个 Endpoint Context。
 * 它描述端点的所有硬件属性，包括类型、传输配置、Ring 位置等。
 *
 * 上下文大小: 32 字节 (64 字节模式下额外 32 字节保留给 HC 内部使用)
 *
 * 与 USB 端点的关系:
 *   EP Index 0 = EP0 (默认控制端点，双向)
 *   EP Index 奇数 = OUT 端点
 *   EP Index 偶数 = IN 端点
 *   ep_index = (ep_id - 1), ep_id 范围 1-31
 *
 * @ep_info:    端点信息 — 包含端点状态 (0=禁用, 1=运行, 2=暂停, 3=停止, 4=错误)、
 *              Mult/Max Burst 数、Max Primary Streams、Interval (服务间隔)
 * @ep_info2:   附加信息 — 端点类型 (控制/批量/中断/等时+方向)、
 *              Max Packet Size (最大包长)、Max Burst Size (最大突发)、
 *              Error Count (错误重试次数)、Force Event 标志
 * @deq:        64-bit Transfer Ring Dequeue Pointer (TR Dequeue 指针):
 *              如果端点只定义了单个流，此指针直接指向传输 Ring；
 *              如果定义了多个流，它指向 Stream Context Array，
 *              数组中的每个条目包含一个流的 Ring 指针。
 *              低 4 位包含 Cycle State 和 Stream Context Type。
 * @tx_info:    传输信息 — Average TRB Length (平均 TRB 长度, 低 16 位)
 *              和 Max ESIT Payload (每个服务间隔的最大负载量, 高 16 位)
 */
struct xhci_ep_ctx {
	__le32	ep_info;
	__le32	ep_info2;
	__le64	deq;
	__le32	tx_info;
	/* offset 0x14 - 0x1f reserved for HC internal use */
	__le32	reserved[3];
};

/* ep_info bitmasks */
/*
 * Endpoint State - bits 0:2
 * 0 - disabled
 * 1 - running
 * 2 - halted due to halt condition - ok to manipulate endpoint ring
 * 3 - stopped
 * 4 - TRB error
 * 5-7 - reserved
 */
#define EP_STATE_MASK		(0x7)
#define EP_STATE_DISABLED	0
#define EP_STATE_RUNNING	1
#define EP_STATE_HALTED		2
#define EP_STATE_STOPPED	3
#define EP_STATE_ERROR		4
#define GET_EP_CTX_STATE(ctx)	(le32_to_cpu((ctx)->ep_info) & EP_STATE_MASK)

/* Mult - Max number of burtst within an interval, in EP companion desc. */
#define EP_MULT(p)		(((p) & 0x3) << 8)
#define CTX_TO_EP_MULT(p)	(((p) >> 8) & 0x3)
/* bits 10:14 are Max Primary Streams */
/* bit 15 is Linear Stream Array */
/* Interval - period between requests to an endpoint - 125u increments. */
#define EP_INTERVAL(p)			(((p) & 0xff) << 16)
#define EP_INTERVAL_TO_UFRAMES(p)	(1 << (((p) >> 16) & 0xff))
#define CTX_TO_EP_INTERVAL(p)		(((p) >> 16) & 0xff)
#define EP_MAXPSTREAMS_MASK		(0x1f << 10)
#define EP_MAXPSTREAMS(p)		(((p) << 10) & EP_MAXPSTREAMS_MASK)
#define CTX_TO_EP_MAXPSTREAMS(p)	(((p) & EP_MAXPSTREAMS_MASK) >> 10)
/* Endpoint is set up with a Linear Stream Array (vs. Secondary Stream Array) */
#define	EP_HAS_LSA		BIT(15)
/* hosts with LEC=1 use bits 31:24 as ESIT high bits. */
#define CTX_TO_MAX_ESIT_PAYLOAD_HI(p)	(((p) >> 24) & 0xff)

/* ep_info2 bitmasks */
/*
 * Force Event - generate transfer events for all TRBs for this endpoint
 * This will tell the HC to ignore the IOC and ISP flags (for debugging only).
 */
#define	FORCE_EVENT	(0x1)
#define ERROR_COUNT(p)	(((p) & 0x3) << 1)
#define CTX_TO_EP_TYPE(p)	(((p) >> 3) & 0x7)
#define EP_TYPE(p)	((p) << 3)
#define ISOC_OUT_EP	1
#define BULK_OUT_EP	2
#define INT_OUT_EP	3
#define CTRL_EP		4
#define ISOC_IN_EP	5
#define BULK_IN_EP	6
#define INT_IN_EP	7
/* bit 6 reserved */
/* bit 7 is Host Initiate Disable - for disabling stream selection */
#define MAX_BURST(p)	(((p)&0xff) << 8)
#define CTX_TO_MAX_BURST(p)	(((p) >> 8) & 0xff)
#define MAX_PACKET(p)	(((p)&0xffff) << 16)
#define MAX_PACKET_MASK		(0xffff << 16)
#define MAX_PACKET_DECODED(p)	(((p) >> 16) & 0xffff)

/* tx_info bitmasks */
#define EP_AVG_TRB_LENGTH(p)		((p) & 0xffff)
#define EP_MAX_ESIT_PAYLOAD_LO(p)	(((p) & 0xffff) << 16)
#define EP_MAX_ESIT_PAYLOAD_HI(p)	((((p) >> 16) & 0xff) << 24)
#define CTX_TO_MAX_ESIT_PAYLOAD(p)	(((p) >> 16) & 0xffff)

/* deq bitmasks */
#define EP_CTX_CYCLE_MASK		BIT(0)
/* bits 63:4 - TR Dequeue Pointer */
#define TR_DEQ_PTR_MASK			GENMASK_ULL(63, 4)


/**
 * struct xhci_input_control_context
 * Input control context; see section 6.2.5.
 *
 * @drop_context:	set the bit of the endpoint context you want to disable
 * @add_context:	set the bit of the endpoint context you want to enable
 */
struct xhci_input_control_ctx {
	__le32	drop_flags;
	__le32	add_flags;
	__le32	rsvd2[6];
};

#define	EP_IS_ADDED(ctrl_ctx, i) \
	(le32_to_cpu(ctrl_ctx->add_flags) & (1 << (i + 1)))
#define	EP_IS_DROPPED(ctrl_ctx, i)       \
	(le32_to_cpu(ctrl_ctx->drop_flags) & (1 << (i + 1)))

/* Represents everything that is needed to issue a command on the command ring.
 * It's useful to pre-allocate these for commands that cannot fail due to
 * out-of-memory errors, like freeing streams.
 */
struct xhci_command {
	/* Input context for changing device state */
	struct xhci_container_ctx	*in_ctx;
	u32				status;
	u32				comp_param;
	int				slot_id;
	/* If completion is null, no one is waiting on this command
	 * and the structure can be freed after the command completes.
	 */
	struct completion		*completion;
	union xhci_trb			*command_trb;
	struct list_head		cmd_list;
	/* xHCI command response timeout in milliseconds */
	unsigned int			timeout_ms;
};

/* drop context bitmasks */
#define	DROP_EP(x)	(0x1 << x)
/* add context bitmasks */
#define	ADD_EP(x)	(0x1 << x)

struct xhci_stream_ctx {
	/* 64-bit stream ring address, cycle state, and stream type */
	__le64	stream_ring;
	/* offset 0x14 - 0x1f reserved for HC internal use */
	__le32	reserved[2];
};

/* Stream Context Types (section 6.4.1) - bits 3:1 of stream ctx deq ptr */
#define	SCT_FOR_CTX(p)		(((p) & 0x7) << 1)
#define	CTX_TO_SCT(p)		(((p) >> 1) & 0x7)
/* Secondary stream array type, dequeue pointer is to a transfer ring */
#define	SCT_SEC_TR		0
/* Primary stream array type, dequeue pointer is to a transfer ring */
#define	SCT_PRI_TR		1
/* Dequeue pointer is for a secondary stream array (SSA) with 8 entries */
#define SCT_SSA_8		2
#define SCT_SSA_16		3
#define SCT_SSA_32		4
#define SCT_SSA_64		5
#define SCT_SSA_128		6
#define SCT_SSA_256		7

/* Assume no secondary streams for now */
struct xhci_stream_info {
	struct xhci_ring		**stream_rings;
	/* Number of streams, including stream 0 (which drivers can't use) */
	unsigned int			num_streams;
	/* The stream context array may be bigger than
	 * the number of streams the driver asked for
	 */
	struct xhci_stream_ctx		*stream_ctx_array;
	unsigned int			num_stream_ctxs;
	dma_addr_t			ctx_array_dma;
	/* For mapping physical TRB addresses to segments in stream rings */
	struct radix_tree_root		trb_address_map;
	struct xhci_command		*free_streams_command;
};

#define	SMALL_STREAM_ARRAY_SIZE		256
#define	MEDIUM_STREAM_ARRAY_SIZE	1024
#define	GET_PORT_BW_ARRAY_SIZE		256

/* Some Intel xHCI host controllers need software to keep track of the bus
 * bandwidth.  Keep track of endpoint info here.  Each root port is allocated
 * the full bus bandwidth.  We must also treat TTs (including each port under a
 * multi-TT hub) as a separate bandwidth domain.  The direct memory interface
 * (DMI) also limits the total bandwidth (across all domains) that can be used.
 */
struct xhci_bw_info {
	/* ep_interval is zero-based */
	unsigned int		ep_interval;
	/* mult and num_packets are one-based */
	unsigned int		mult;
	unsigned int		num_packets;
	unsigned int		max_packet_size;
	unsigned int		max_esit_payload;
	unsigned int		type;
};

/* "Block" sizes in bytes the hardware uses for different device speeds.
 * The logic in this part of the hardware limits the number of bits the hardware
 * can use, so must represent bandwidth in a less precise manner to mimic what
 * the scheduler hardware computes.
 */
#define	FS_BLOCK	1
#define	HS_BLOCK	4
#define	SS_BLOCK	16
#define	DMI_BLOCK	32

/* Each device speed has a protocol overhead (CRC, bit stuffing, etc) associated
 * with each byte transferred.  SuperSpeed devices have an initial overhead to
 * set up bursts.  These are in blocks, see above.  LS overhead has already been
 * translated into FS blocks.
 */
#define DMI_OVERHEAD 8
#define DMI_OVERHEAD_BURST 4
#define SS_OVERHEAD 8
#define SS_OVERHEAD_BURST 32
#define HS_OVERHEAD 26
#define FS_OVERHEAD 20
#define LS_OVERHEAD 128
/* The TTs need to claim roughly twice as much bandwidth (94 bytes per
 * microframe ~= 24Mbps) of the HS bus as the devices can actually use because
 * of overhead associated with split transfers crossing microframe boundaries.
 * 31 blocks is pure protocol overhead.
 */
#define TT_HS_OVERHEAD (31 + 94)
#define TT_DMI_OVERHEAD (25 + 12)

/* Bandwidth limits in blocks */
#define FS_BW_LIMIT		1285
#define TT_BW_LIMIT		1320
#define HS_BW_LIMIT		1607
#define SS_BW_LIMIT_IN		3906
#define DMI_BW_LIMIT_IN		3906
#define SS_BW_LIMIT_OUT		3906
#define DMI_BW_LIMIT_OUT	3906

/* Percentage of bus bandwidth reserved for non-periodic transfers */
#define FS_BW_RESERVED		10
#define HS_BW_RESERVED		20
#define SS_BW_RESERVED		10

struct xhci_virt_ep {
	struct xhci_virt_device		*vdev;	/* parent */
	unsigned int			ep_index;
	struct xhci_ring		*ring;
	/* Related to endpoints that are configured to use stream IDs only */
	struct xhci_stream_info		*stream_info;
	/* Temporary storage in case the configure endpoint command fails and we
	 * have to restore the device state to the previous state
	 */
	struct xhci_ring		*new_ring;
	unsigned int			err_count;
	unsigned int			ep_state;
#define SET_DEQ_PENDING		BIT(0)
#define EP_HALTED		BIT(1)	/* For stall handling */
#define EP_STOP_CMD_PENDING	BIT(2)	/* For URB cancellation */
/* Transitioning the endpoint to using streams, don't enqueue URBs */
#define EP_GETTING_STREAMS	BIT(3)
#define EP_HAS_STREAMS		BIT(4)
/* Transitioning the endpoint to not using streams, don't enqueue URBs */
#define EP_GETTING_NO_STREAMS	BIT(5)
#define EP_HARD_CLEAR_TOGGLE	BIT(6)
#define EP_SOFT_CLEAR_TOGGLE	BIT(7)
/* usb_hub_clear_tt_buffer is in progress */
#define EP_CLEARING_TT		BIT(8)
	/* ----  Related to URB cancellation ---- */
	struct list_head	cancelled_td_list;
	struct xhci_hcd		*xhci;
	/* Dequeue pointer and dequeue segment for a submitted Set TR Dequeue
	 * command.  We'll need to update the ring's dequeue segment and dequeue
	 * pointer after the command completes.
	 */
	struct xhci_segment	*queued_deq_seg;
	union xhci_trb		*queued_deq_ptr;
	/*
	 * Sometimes the xHC can not process isochronous endpoint ring quickly
	 * enough, and it will miss some isoc tds on the ring and generate
	 * a Missed Service Error Event.
	 * Set skip flag when receive a Missed Service Error Event and
	 * process the missed tds on the endpoint ring.
	 */
	bool			skip;
	/* Bandwidth checking storage */
	struct xhci_bw_info	bw_info;
	struct list_head	bw_endpoint_list;
	unsigned long		stop_time;
	/* Isoch Frame ID checking storage */
	int			next_frame_id;
	/* Use new Isoch TRB layout needed for extended TBC support */
	bool			use_extended_tbc;
	/* set if this endpoint is controlled via sideband access*/
	struct xhci_sideband	*sideband;
};

enum xhci_overhead_type {
	LS_OVERHEAD_TYPE = 0,
	FS_OVERHEAD_TYPE,
	HS_OVERHEAD_TYPE,
};

struct xhci_interval_bw {
	unsigned int		num_packets;
	/* Sorted by max packet size.
	 * Head of the list is the greatest max packet size.
	 */
	struct list_head	endpoints;
	/* How many endpoints of each speed are present. */
	unsigned int		overhead[3];
};

#define	XHCI_MAX_INTERVAL	16

struct xhci_interval_bw_table {
	unsigned int		interval0_esit_payload;
	struct xhci_interval_bw	interval_bw[XHCI_MAX_INTERVAL];
	/* Includes reserved bandwidth for async endpoints */
	unsigned int		bw_used;
	unsigned int		ss_bw_in;
	unsigned int		ss_bw_out;
};

#define EP_CTX_PER_DEV		31

/*
 * struct xhci_virt_device - XHCI 虚拟设备 (软件对 USB 设备的抽象)
 *
 * 每个通过 Enable Slot 命令分配的 USB 设备 Slot 对应一个虚拟设备。
 * 它是驱动管理中最关键的软件结构之一，封装了一个 USB 设备的全部状态。
 *
 * @slot_id:    HC 分配的 Slot ID (1-255)，是设备的硬件标识
 * @udev:       Linux USB 核心的 usb_device 结构
 * @out_ctx:    输出上下文 (Device Context) — HC 维护的设备当前状态。
 *              驱动通过 Address Device、Configure Endpoint 等命令
 *              让 HC 更新此上下文。该内存由驱动分配，但内容由 HC 写入。
 * @in_ctx:     输入上下文 (Input Context) — 驱动发给 HC 的变更请求。
 *              包含 Input Control Context + 新的 Slot/EP Context。
 *              如果命令失败，驱动可以保留旧的 out_ctx 来恢复状态。
 * @eps[31]:    该设备的所有端点 (EP0 + 30 个通用端点)
 * @rhub_port:  设备连接的根 Hub 端口
 * @bw_table:   带宽信息表 (用于 SW 带宽检查)
 * @tt_info:    事务翻译器信息 (LS/FS 设备通过 HS Hub 连接时使用)
 * @flags:      设备状态标志
 * @current_mel:当前使能的 USB3 链路状态最大退出延迟
 * @debugfs_private: debugfs 接口的私有数据
 * @sideband:   边带访问控制
 */
struct xhci_virt_device {

/*
 * For each roothub, keep track of the bandwidth information for each periodic
 * interval.
 *
 * If a high speed hub is attached to the roothub, each TT associated with that
 * hub is a separate bandwidth domain.  The interval information for the
 * endpoints on the devices under that TT will appear in the TT structure.
 */
struct xhci_root_port_bw_info {
	struct list_head		tts;
	unsigned int			num_active_tts;
	struct xhci_interval_bw_table	bw_table;
};

struct xhci_tt_bw_info {
	struct list_head		tt_list;
	int				slot_id;
	int				ttport;
	struct xhci_interval_bw_table	bw_table;
	int				active_eps;
};


/**
 * struct xhci_device_context_array
 * @dev_context_ptr	array of 64-bit DMA addresses for device contexts
 */
struct xhci_device_context_array {
	/* 64-bit device addresses; we only write 32-bit addresses */
	__le64			dev_context_ptrs[MAX_HC_SLOTS];
	/* private xHCD pointers */
	dma_addr_t	dma;
};
/*
 * TODO: change this to be dynamically sized at HC mem init time since the HC
 * might not be able to handle the maximum number of devices possible.
 */


/*
 * struct xhci_transfer_event - XHCI Transfer Event (传输完成事件)
 *
 * 当 HC 完成一个 TRB 的处理后，会写一个 Transfer Event 到 Event Ring。
 * 驱动从中读取完成状态，并据此更新 URB 的状态。
 *
 * 事件字段:
 * @buffer:      64-bit 缓冲区地址，或立即数据 (Immediate Data)
 * @transfer_len:传输剩余长度/完成码 — 低 24 位是剩余字节数，
 *               高 8 位是 Completion Code (完成码)
 * @flags:       标志位 — 包含 Slot ID (24:31)、Endpoint ID (16:20) 和
 *               Cycle bit (0) 等。根据 TRB 类型不同，部分字段含义不同。
 *
 * 常见的 Completion Code (完成码) 含义:
 *   COMP_SUCCESS (1):       传输成功完成
 *   COMP_SHORT_PACKET (13): 短包 (收到的数据少于期望值)
 *   COMP_STALL_ERROR (6):   端点返回 STALL 握手
 *   COMP_USB_TRANSACTION_ERROR (4): USB 事务错误 (CRC 错误等)
 *   COMP_TRB_ERROR (5):     TRB 编程错误 (无效参数)
 *   COMP_BABBLE_DETECTED_ERROR (3): 设备发言超时
 *   COMP_RING_UNDERRUN (14): 等时 OUT 端点没有准备好数据
 *   COMP_RING_OVERRUN (15):  等时 IN 端点来不及接收数据
 *   COMP_MISSED_SERVICE_ERROR (23): 等时服务错过
 *   COMP_CONTEXT_STATE_ERROR (19): 上下文状态错误
 */
/*
 * struct xhci_transfer_event - XHCI Transfer Event (传输完成事件)
 *
 * 当 HC 完成一个 TRB 的处理后，会写一个 Transfer Event 到 Event Ring。
 * 驱动从中读取完成状态，并据此更新 URB 的状态。
 *
 * 事件字段:
 * @buffer:      64-bit 缓冲区地址，或立即数据 (Immediate Data)
 * @transfer_len:传输剩余长度/完成码 — 低 24 位是剩余字节数，
 *               高 8 位是 Completion Code (完成码)
 * @flags:       标志位 — 包含 Slot ID (24:31)、Endpoint ID (16:20) 和
 *               Cycle bit (0) 等。根据 TRB 类型不同，部分字段含义不同。
 *
 * 常见的 Completion Code (完成码) 含义:
 *   COMP_SUCCESS (1):       传输成功完成
 *   COMP_SHORT_PACKET (13): 短包 (收到的数据少于期望值)
 *   COMP_STALL_ERROR (6):   端点返回 STALL 握手
 *   COMP_USB_TRANSACTION_ERROR (4): USB 事务错误 (CRC 错误等)
 *   COMP_TRB_ERROR (5):     TRB 编程错误 (无效参数)
 *   COMP_BABBLE_DETECTED_ERROR (3): 设备发言超时
 *   COMP_RING_UNDERRUN (14): 等时 OUT 端点没有准备好数据
 *   COMP_RING_OVERRUN (15):  等时 IN 端点来不及接收数据
 *   COMP_MISSED_SERVICE_ERROR (23): 等时服务错过
 *   COMP_CONTEXT_STATE_ERROR (19): 上下文状态错误
 */
struct xhci_transfer_event {
	/* 64-bit buffer address, or immediate data */
	__le64	buffer;
	__le32	transfer_len;
	/* This field is interpreted differently based on the type of TRB */
	__le32	flags;
};

/* Transfer event flags bitfield, also for select command completion events */
#define TRB_TO_SLOT_ID(p)	(((p) >> 24) & 0xff)
#define SLOT_ID_FOR_TRB(p)	(((p) & 0xff) << 24)

#define TRB_TO_EP_ID(p)		(((p) >> 16) & 0x1f) /* Endpoint ID 1 - 31 */
#define EP_ID_FOR_TRB(p)	(((p) & 0x1f) << 16)

#define TRB_TO_EP_INDEX(p)	(TRB_TO_EP_ID(p) - 1) /* Endpoint index 0 - 30 */
#define EP_INDEX_FOR_TRB(p)	((((p) + 1) & 0x1f) << 16)

/* Transfer event TRB length bit mask */
#define	EVENT_TRB_LEN(p)		((p) & 0xffffff)

/* Completion Code - only applicable for some types of TRBs */
#define	COMP_CODE_MASK		(0xff << 24)
#define GET_COMP_CODE(p)	(((p) & COMP_CODE_MASK) >> 24)
#define COMP_INVALID				0
#define COMP_SUCCESS				1
#define COMP_DATA_BUFFER_ERROR			2
#define COMP_BABBLE_DETECTED_ERROR		3
#define COMP_USB_TRANSACTION_ERROR		4
#define COMP_TRB_ERROR				5
#define COMP_STALL_ERROR			6
#define COMP_RESOURCE_ERROR			7
#define COMP_BANDWIDTH_ERROR			8
#define COMP_NO_SLOTS_AVAILABLE_ERROR		9
#define COMP_INVALID_STREAM_TYPE_ERROR		10
#define COMP_SLOT_NOT_ENABLED_ERROR		11
#define COMP_ENDPOINT_NOT_ENABLED_ERROR		12
#define COMP_SHORT_PACKET			13
#define COMP_RING_UNDERRUN			14
#define COMP_RING_OVERRUN			15
#define COMP_VF_EVENT_RING_FULL_ERROR		16
#define COMP_PARAMETER_ERROR			17
#define COMP_BANDWIDTH_OVERRUN_ERROR		18
#define COMP_CONTEXT_STATE_ERROR		19
#define COMP_NO_PING_RESPONSE_ERROR		20
#define COMP_EVENT_RING_FULL_ERROR		21
#define COMP_INCOMPATIBLE_DEVICE_ERROR		22
#define COMP_MISSED_SERVICE_ERROR		23
#define COMP_COMMAND_RING_STOPPED		24
#define COMP_COMMAND_ABORTED			25
#define COMP_STOPPED				26
#define COMP_STOPPED_LENGTH_INVALID		27
#define COMP_STOPPED_SHORT_PACKET		28
#define COMP_MAX_EXIT_LATENCY_TOO_LARGE_ERROR	29
#define COMP_ISOCH_BUFFER_OVERRUN		31
#define COMP_EVENT_LOST_ERROR			32
#define COMP_UNDEFINED_ERROR			33
#define COMP_INVALID_STREAM_ID_ERROR		34
#define COMP_SECONDARY_BANDWIDTH_ERROR		35
#define COMP_SPLIT_TRANSACTION_ERROR		36

static inline const char *xhci_trb_comp_code_string(u8 status)
{
	switch (status) {
	case COMP_INVALID:
		return "Invalid";
	case COMP_SUCCESS:
		return "Success";
	case COMP_DATA_BUFFER_ERROR:
		return "Data Buffer Error";
	case COMP_BABBLE_DETECTED_ERROR:
		return "Babble Detected";
	case COMP_USB_TRANSACTION_ERROR:
		return "USB Transaction Error";
	case COMP_TRB_ERROR:
		return "TRB Error";
	case COMP_STALL_ERROR:
		return "Stall Error";
	case COMP_RESOURCE_ERROR:
		return "Resource Error";
	case COMP_BANDWIDTH_ERROR:
		return "Bandwidth Error";
	case COMP_NO_SLOTS_AVAILABLE_ERROR:
		return "No Slots Available Error";
	case COMP_INVALID_STREAM_TYPE_ERROR:
		return "Invalid Stream Type Error";
	case COMP_SLOT_NOT_ENABLED_ERROR:
		return "Slot Not Enabled Error";
	case COMP_ENDPOINT_NOT_ENABLED_ERROR:
		return "Endpoint Not Enabled Error";
	case COMP_SHORT_PACKET:
		return "Short Packet";
	case COMP_RING_UNDERRUN:
		return "Ring Underrun";
	case COMP_RING_OVERRUN:
		return "Ring Overrun";
	case COMP_VF_EVENT_RING_FULL_ERROR:
		return "VF Event Ring Full Error";
	case COMP_PARAMETER_ERROR:
		return "Parameter Error";
	case COMP_BANDWIDTH_OVERRUN_ERROR:
		return "Bandwidth Overrun Error";
	case COMP_CONTEXT_STATE_ERROR:
		return "Context State Error";
	case COMP_NO_PING_RESPONSE_ERROR:
		return "No Ping Response Error";
	case COMP_EVENT_RING_FULL_ERROR:
		return "Event Ring Full Error";
	case COMP_INCOMPATIBLE_DEVICE_ERROR:
		return "Incompatible Device Error";
	case COMP_MISSED_SERVICE_ERROR:
		return "Missed Service Error";
	case COMP_COMMAND_RING_STOPPED:
		return "Command Ring Stopped";
	case COMP_COMMAND_ABORTED:
		return "Command Aborted";
	case COMP_STOPPED:
		return "Stopped";
	case COMP_STOPPED_LENGTH_INVALID:
		return "Stopped - Length Invalid";
	case COMP_STOPPED_SHORT_PACKET:
		return "Stopped - Short Packet";
	case COMP_MAX_EXIT_LATENCY_TOO_LARGE_ERROR:
		return "Max Exit Latency Too Large Error";
	case COMP_ISOCH_BUFFER_OVERRUN:
		return "Isoch Buffer Overrun";
	case COMP_EVENT_LOST_ERROR:
		return "Event Lost Error";
	case COMP_UNDEFINED_ERROR:
		return "Undefined Error";
	case COMP_INVALID_STREAM_ID_ERROR:
		return "Invalid Stream ID Error";
	case COMP_SECONDARY_BANDWIDTH_ERROR:
		return "Secondary Bandwidth Error";
	case COMP_SPLIT_TRANSACTION_ERROR:
		return "Split Transaction Error";
	default:
		return "Unknown!!";
	}
}

struct xhci_link_trb {
	/* 64-bit segment pointer*/
	__le64 segment_ptr;
	__le32 intr_target;
	__le32 control;
};

/* control bitfields */
#define LINK_TOGGLE	BIT(1)

/* Command completion event TRB */
struct xhci_event_cmd {
	/* Pointer to command TRB, or the value passed by the event data trb */
	__le64 cmd_trb;
	__le32 status;
	__le32 flags;
};

/* status bitmasks */
#define COMP_PARAM(p)	((p) & 0xffffff) /* Command Completion Parameter */

/* Address device - disable SetAddress */
#define TRB_BSR		BIT(9)

/* Configure Endpoint - Deconfigure */
#define TRB_DC		BIT(9)

/* Stop Ring - Transfer State Preserve */
#define TRB_TSP		BIT(9)

enum xhci_ep_reset_type {
	EP_HARD_RESET,
	EP_SOFT_RESET,
};

/* Force Event */
#define TRB_TO_VF_INTR_TARGET(p)	(((p) & (0x3ff << 22)) >> 22)
#define TRB_TO_VF_ID(p)			(((p) & (0xff << 16)) >> 16)

/* Set Latency Tolerance Value */
#define TRB_TO_BELT(p)			(((p) & (0xfff << 16)) >> 16)

/* Get Port Bandwidth */
#define TRB_TO_DEV_SPEED(p)		(((p) & (0xf << 16)) >> 16)

/* Force Header */
#define TRB_TO_PACKET_TYPE(p)		((p) & 0x1f)
#define TRB_TO_ROOTHUB_PORT(p)		(((p) & (0xff << 24)) >> 24)

enum xhci_setup_dev {
	SETUP_CONTEXT_ONLY,
	SETUP_CONTEXT_ADDRESS,
};

/* bits 16:23 are the virtual function ID */
/* bits 24:31 are the slot ID */

/* bits 19:16 are the dev speed */
#define DEV_SPEED_FOR_TRB(p)    ((p) << 16)

/* Stop Endpoint TRB - ep_index to endpoint ID for this TRB */
#define SUSPEND_PORT_FOR_TRB(p)		(((p) & 1) << 23)
#define TRB_TO_SUSPEND_PORT(p)		(((p) & (1 << 23)) >> 23)
#define LAST_EP_INDEX			30

/* Set TR Dequeue Pointer command TRB fields, 6.4.3.9 */
#define TRB_TO_STREAM_ID(p)		((((p) & (0xffff << 16)) >> 16))
#define STREAM_ID_FOR_TRB(p)		((((p)) & 0xffff) << 16)
#define SCT_FOR_TRB(p)			(((p) & 0x7) << 1)

/* Link TRB specific fields */
#define TRB_TC			BIT(1)

/* Port Status Change Event TRB fields */
/* Port ID - bits 31:24 */
#define GET_PORT_ID(p)		(((p) & (0xff << 24)) >> 24)

#define EVENT_DATA		BIT(2)

/* Normal TRB fields */
/* transfer_len bitmasks - bits 0:16 */
#define	TRB_LEN(p)		((p) & 0x1ffff)
/* TD Size, packets remaining in this TD, bits 21:17 (5 bits, so max 31) */
#define TRB_TD_SIZE(p)          (min((p), (u32)31) << 17)
#define GET_TD_SIZE(p)		(((p) & 0x3e0000) >> 17)
/* xhci 1.1 uses the TD_SIZE field for TBC if Extended TBC is enabled (ETE) */
#define TRB_TD_SIZE_TBC(p)      (min((p), (u32)31) << 17)
/* Interrupter Target - which MSI-X vector to target the completion event at */
#define TRB_INTR_TARGET(p)	(((p) & 0x3ff) << 22)
#define GET_INTR_TARGET(p)	(((p) >> 22) & 0x3ff)

/* Cycle bit - indicates TRB ownership by HC or HCD */
#define TRB_CYCLE		BIT(0)
/*
 * Force next event data TRB to be evaluated before task switch.
 * Used to pass OS data back after a TD completes.
 */
#define TRB_ENT			BIT(1)
/* Interrupt on short packet */
#define TRB_ISP			BIT(2)
/* Set PCIe no snoop attribute */
#define TRB_NO_SNOOP		BIT(3)
/* Chain multiple TRBs into a TD */
#define TRB_CHAIN		BIT(4)
/* Interrupt on completion */
#define TRB_IOC			BIT(5)
/* The buffer pointer contains immediate data */
#define TRB_IDT			BIT(6)
/* TDs smaller than this might use IDT */
#define TRB_IDT_MAX_SIZE	8

/* Block Event Interrupt */
#define	TRB_BEI			BIT(9)

/* Control transfer TRB specific fields */
#define TRB_DIR_IN		BIT(16)
#define	TRB_TX_TYPE(p)		((p) << 16)
#define	TRB_DATA_OUT		2
#define	TRB_DATA_IN		3

/* Isochronous TRB specific fields */
#define TRB_SIA			BIT(31)
#define TRB_FRAME_ID(p)		(((p) & 0x7ff) << 20)
#define GET_FRAME_ID(p)		(((p) >> 20) & 0x7ff)
/* Total burst count field, Rsvdz on xhci 1.1 with Extended TBC enabled (ETE) */
#define TRB_TBC(p)		(((p) & 0x3) << 7)
#define GET_TBC(p)		(((p) >> 7) & 0x3)
#define TRB_TLBPC(p)		(((p) & 0xf) << 16)
#define GET_TLBPC(p)		(((p) >> 16) & 0xf)

/* TRB cache size for xHC with TRB cache */
#define TRB_CACHE_SIZE_HS	8
#define TRB_CACHE_SIZE_SS	16

/*
 * struct xhci_generic_trb - 通用 TRB 结构
 *
 * TRB (Transfer Request Block) 是 XHCI 中最基本的传输单元，固定 16 字节。
 * 所有类型的 TRB 共享此通用布局，通过 field[3] 的 TRB 类型字段来区分。
 *
 * 16 字节布局:
 *   field[0] (32-bit): 数据缓冲区的低 32 位地址 (Data Buffer Pointer Lo)
 *   field[1] (32-bit): 数据缓冲区的高 32 位地址 (Data Buffer Pointer Hi)
 *   field[2] (32-bit): 状态/传输长度 (Status/Length)
 *   field[3] (32-bit): 控制信息 (Control)
 *                       bit 0:     Cycle bit (所有权位)
 *                       bits 5:3:  TRB 类型
 *                       bits 9:6:  各种控制标志
 *                       bits 15:10: 中断目标
 *
 * Transfer Type (传输类型) 说明:
 * - Control Transfer: 由 Setup + Data + Status 三个阶段 TRB 组成
 * - Bulk Transfer: 使用 Normal TRB，大数据量非周期性传输
 * - Interrupt Transfer: 使用 Normal TRB，周期性小数据量传输
 * - Isochronous Transfer: 使用 Isoch TRB，周期性实时传输
 */
struct xhci_generic_trb {
	__le32 field[4];
};

/*
 * union xhci_trb - TRB 联合体，根据类型选择不同的解读方式
 *
 * 同一个 16 字节内存块可以根据 TRB 类型被解读为不同结构:
 *   - 传输 TRB 可分为 Normal/Setup/Data/Status/Isoch/Link
 *   - 事件 TRB 分为 Transfer Event / Command Completion Event / Port Status Change
 *
 * @link:       作为 Link TRB 时使用，链接不同 ring segment
 * @trans_event:作为 Transfer Event (完成事件) 时使用
 * @event_cmd:  作为 Command Completion Event 时使用
 * @generic:    通用 4x32-bit 视图
 */
union xhci_trb {
	struct xhci_link_trb		link;
	struct xhci_transfer_event	trans_event;
	struct xhci_event_cmd		event_cmd;
	struct xhci_generic_trb		generic;
};

/* TRB bit mask */
#define	TRB_TYPE_BITMASK	(0xfc00)
#define TRB_TYPE(p)		((p) << 10)
#define TRB_FIELD_TO_TYPE(p)	(((p) & TRB_TYPE_BITMASK) >> 10)
/* TRB type IDs */
/* bulk, interrupt, isoc scatter/gather, and control data stage */
#define TRB_NORMAL		1
/* setup stage for control transfers */
#define TRB_SETUP		2
/* data stage for control transfers */
#define TRB_DATA		3
/* status stage for control transfers */
#define TRB_STATUS		4
/* isoc transfers */
#define TRB_ISOC		5
/* TRB for linking ring segments */
#define TRB_LINK		6
#define TRB_EVENT_DATA		7
/* Transfer Ring No-op (not for the command ring) */
#define TRB_TR_NOOP		8
/* Command TRBs */
/* Enable Slot Command */
#define TRB_ENABLE_SLOT		9
/* Disable Slot Command */
#define TRB_DISABLE_SLOT	10
/* Address Device Command */
#define TRB_ADDR_DEV		11
/* Configure Endpoint Command */
#define TRB_CONFIG_EP		12
/* Evaluate Context Command */
#define TRB_EVAL_CONTEXT	13
/* Reset Endpoint Command */
#define TRB_RESET_EP		14
/* Stop Transfer Ring Command */
#define TRB_STOP_RING		15
/* Set Transfer Ring Dequeue Pointer Command */
#define TRB_SET_DEQ		16
/* Reset Device Command */
#define TRB_RESET_DEV		17
/* Force Event Command (opt) */
#define TRB_FORCE_EVENT		18
/* Negotiate Bandwidth Command (opt) */
#define TRB_NEG_BANDWIDTH	19
/* Set Latency Tolerance Value Command (opt) */
#define TRB_SET_LT		20
/* Get port bandwidth Command */
#define TRB_GET_BW		21
/* Force Header Command - generate a transaction or link management packet */
#define TRB_FORCE_HEADER	22
/* No-op Command - not for transfer rings */
#define TRB_CMD_NOOP		23
/* TRB IDs 24-31 reserved */
/* Event TRBS */
/* Transfer Event */
#define TRB_TRANSFER		32
/* Command Completion Event */
#define TRB_COMPLETION		33
/* Port Status Change Event */
#define TRB_PORT_STATUS		34
/* Bandwidth Request Event (opt) */
#define TRB_BANDWIDTH_EVENT	35
/* Doorbell Event (opt) */
#define TRB_DOORBELL		36
/* Host Controller Event */
#define TRB_HC_EVENT		37
/* Device Notification Event - device sent function wake notification */
#define TRB_DEV_NOTE		38
/* MFINDEX Wrap Event - microframe counter wrapped */
#define TRB_MFINDEX_WRAP	39
/* TRB IDs 40-47 reserved, 48-63 is vendor-defined */
#define TRB_VENDOR_DEFINED_LOW	48
/* Nec vendor-specific command completion event. */
#define	TRB_NEC_CMD_COMP	48
/* Get NEC firmware revision. */
#define	TRB_NEC_GET_FW		49

static inline const char *xhci_trb_type_string(u8 type)
{
	switch (type) {
	case TRB_NORMAL:
		return "Normal";
	case TRB_SETUP:
		return "Setup Stage";
	case TRB_DATA:
		return "Data Stage";
	case TRB_STATUS:
		return "Status Stage";
	case TRB_ISOC:
		return "Isoch";
	case TRB_LINK:
		return "Link";
	case TRB_EVENT_DATA:
		return "Event Data";
	case TRB_TR_NOOP:
		return "No-Op";
	case TRB_ENABLE_SLOT:
		return "Enable Slot Command";
	case TRB_DISABLE_SLOT:
		return "Disable Slot Command";
	case TRB_ADDR_DEV:
		return "Address Device Command";
	case TRB_CONFIG_EP:
		return "Configure Endpoint Command";
	case TRB_EVAL_CONTEXT:
		return "Evaluate Context Command";
	case TRB_RESET_EP:
		return "Reset Endpoint Command";
	case TRB_STOP_RING:
		return "Stop Ring Command";
	case TRB_SET_DEQ:
		return "Set TR Dequeue Pointer Command";
	case TRB_RESET_DEV:
		return "Reset Device Command";
	case TRB_FORCE_EVENT:
		return "Force Event Command";
	case TRB_NEG_BANDWIDTH:
		return "Negotiate Bandwidth Command";
	case TRB_SET_LT:
		return "Set Latency Tolerance Value Command";
	case TRB_GET_BW:
		return "Get Port Bandwidth Command";
	case TRB_FORCE_HEADER:
		return "Force Header Command";
	case TRB_CMD_NOOP:
		return "No-Op Command";
	case TRB_TRANSFER:
		return "Transfer Event";
	case TRB_COMPLETION:
		return "Command Completion Event";
	case TRB_PORT_STATUS:
		return "Port Status Change Event";
	case TRB_BANDWIDTH_EVENT:
		return "Bandwidth Request Event";
	case TRB_DOORBELL:
		return "Doorbell Event";
	case TRB_HC_EVENT:
		return "Host Controller Event";
	case TRB_DEV_NOTE:
		return "Device Notification Event";
	case TRB_MFINDEX_WRAP:
		return "MFINDEX Wrap Event";
	case TRB_NEC_CMD_COMP:
		return "NEC Command Completion Event";
	case TRB_NEC_GET_FW:
		return "NET Get Firmware Revision Command";
	default:
		return "UNKNOWN";
	}
}

#define TRB_TYPE_LINK(x)	(((x) & TRB_TYPE_BITMASK) == TRB_TYPE(TRB_LINK))
/* Above, but for __le32 types -- can avoid work by swapping constants: */
#define TRB_TYPE_LINK_LE32(x)	(((x) & cpu_to_le32(TRB_TYPE_BITMASK)) == \
				 cpu_to_le32(TRB_TYPE(TRB_LINK)))
#define TRB_TYPE_NOOP_LE32(x)	(((x) & cpu_to_le32(TRB_TYPE_BITMASK)) == \
				 cpu_to_le32(TRB_TYPE(TRB_TR_NOOP)))

#define NEC_FW_MINOR(p)		(((p) >> 0) & 0xff)
#define NEC_FW_MAJOR(p)		(((p) >> 8) & 0xff)

/*
 * TRBS_PER_SEGMENT must be a multiple of 4,
 * since the command ring is 64-byte aligned.
 * It must also be greater than 16.
 */
#define TRBS_PER_SEGMENT	256
/* Allow two commands + a link TRB, along with any reserved command TRBs */
#define MAX_RSVD_CMD_TRBS	(TRBS_PER_SEGMENT - 3)
#define TRB_SEGMENT_SIZE	(TRBS_PER_SEGMENT*16)
#define TRB_SEGMENT_SHIFT	(ilog2(TRB_SEGMENT_SIZE))
/* TRB buffer pointers can't cross 64KB boundaries */
#define TRB_MAX_BUFF_SHIFT		16
#define TRB_MAX_BUFF_SIZE	(1 << TRB_MAX_BUFF_SHIFT)
/* How much data is left before the 64KB boundary? */
#define TRB_BUFF_LEN_UP_TO_BOUNDARY(addr)	(TRB_MAX_BUFF_SIZE - \
					(addr & (TRB_MAX_BUFF_SIZE - 1)))
#define MAX_SOFT_RETRY		3
/*
 * Limits of consecutive isoc trbs that can Block Event Interrupt (BEI) if
 * XHCI_AVOID_BEI quirk is in use.
 */
#define AVOID_BEI_INTERVAL_MIN	8
#define AVOID_BEI_INTERVAL_MAX	32

#define xhci_for_each_ring_seg(head, seg) \
	for (seg = head; seg != NULL; seg = (seg->next != head ? seg->next : NULL))

/*
 * struct xhci_segment - XHCI Ring Segment (环段)
 *
 * Transfer Ring 由多个 segment (段) 通过 Link TRB 连接成逻辑循环链表。
 * 每个 segment 包含固定数量的 TRB (通常为 256 个，共 4KB)。
 *
 * 链式结构:
 *   first_seg -> seg1 -> seg2 -> ... -> segN -> first_seg (循环)
 *   最后一个 segment 的 Link TRB 指向第一个 segment，形成环。
 *
 * @trbs:       TRB 数组基址，每个 segment 包含 TRBS_PER_SEGMENT 个 TRB
 * @next:       指向下一个 segment，最后一个 segment 的 next 指向 first_seg
 * @num:        segment 在 ring 中的序号 (用于调试和追踪)
 * @dma:        segment 的 DMA 地址 (硬件可见)
 * @bounce_dma: 用于非对齐 TD 片段的最大包长弹跳缓冲区 DMA 地址
 * @bounce_buf: 弹跳缓冲区的虚拟地址
 * @bounce_offs:弹跳偏移量
 * @bounce_len: 弹跳长度
 */
struct xhci_segment {
	union xhci_trb		*trbs;
	/* private to HCD */
	struct xhci_segment	*next;
	unsigned int		num;
	dma_addr_t		dma;
	/* Max packet sized bounce buffer for td-fragmant alignment */
	dma_addr_t		bounce_dma;
	void			*bounce_buf;
	unsigned int		bounce_offs;
	unsigned int		bounce_len;
};

enum xhci_cancelled_td_status {
	TD_DIRTY = 0,
	TD_HALTED,
	TD_CLEARING_CACHE,
	TD_CLEARING_CACHE_DEFERRED,
	TD_CLEARED,
};

struct xhci_td {
	struct list_head	td_list;
	struct list_head	cancelled_td_list;
	int			status;
	enum xhci_cancelled_td_status	cancel_status;
	struct urb		*urb;
	struct xhci_segment	*start_seg;
	union xhci_trb		*start_trb;
	struct xhci_segment	*end_seg;
	union xhci_trb		*end_trb;
	struct xhci_segment	*bounce_seg;
	/* actual_length of the URB has already been set */
	bool			urb_length_set;
	bool			error_mid_td;
};

/*
 * xHCI command default timeout value in milliseconds.
 * USB 3.2 spec, section 9.2.6.1
 */
#define XHCI_CMD_DEFAULT_TIMEOUT	5000

/* command descriptor */
struct xhci_cd {
	struct xhci_command	*command;
	union xhci_trb		*cmd_trb;
};

enum xhci_ring_type {
	TYPE_CTRL = 0,
	TYPE_ISOC,
	TYPE_BULK,
	TYPE_INTR,
	TYPE_STREAM,
	TYPE_COMMAND,
	TYPE_EVENT,
};

static inline const char *xhci_ring_type_string(enum xhci_ring_type type)
{
	switch (type) {
	case TYPE_CTRL:
		return "CTRL";
	case TYPE_ISOC:
		return "ISOC";
	case TYPE_BULK:
		return "BULK";
	case TYPE_INTR:
		return "INTR";
	case TYPE_STREAM:
		return "STREAM";
	case TYPE_COMMAND:
		return "CMD";
	case TYPE_EVENT:
		return "EVENT";
	}

	return "UNKNOWN";
}

/*
 * struct xhci_ring - XHCI Transfer Ring (传输环)
 *
 * Transfer Ring 是驱动 (软件) 和 HC (硬件) 之间传递传输请求的循环队列。
 * 这是 XHCI 架构中最核心的数据结构之一。
 *
 * 工作原理:
 *   1. 驱动将 TRB (传输请求块) 写入 Ring 的 enqueue 位置
 *   2. 驱动写对应端点的 Doorbell 通知 HC
 *   3. HC 从 dequeue 位置读取 TRB 并执行
 *   4. HC 完成后写 Event Ring (事件环) 通知驱动
 *
 * Cycle State (循环状态位):
 *   - 每个 TRB 的 bit 0 是 Cycle 位
 *   - 驱动写入 TRB 时设置 cycle_state，HC 消费后翻转它
 *   - 每次 Ring 绕完一圈 (通过 Link TRB 回到起点) 时，
 *     cycle_state 翻转，从而区分新旧 TRB
 *
 * @first_seg:   Ring 的第一个 segment
 * @last_seg:    Ring 的最后一个 segment (用于追加新 segment)
 * @enqueue:     下一个空闲 TRB 的位置 (驱动写入位置)
 * @enq_seg:     enqueue 所属的 segment
 * @dequeue:     下一个待处理 TRB 的位置 (HC 读取位置 / 驱动已处理位置)
 * @deq_seg:     dequeue 所属的 segment
 * @td_list:     属于该 Ring 的所有 TD (Transfer Descriptor) 链表
 * @cycle_state: 当前的 Cycle 状态 (0 或 1)，写入 TRB 时使用
 * @stream_id:   该 Ring 关联的流 ID (0 = 非流)
 * @num_segs:    segment 数量
 * @num_trbs_free: 空闲 TRB 数量 (仅用于 DbC)
 * @bounce_buf_len: 弹跳缓冲区长度
 * @type:        Ring 类型 (CTRL/ISOC/BULK/INTR/STREAM/COMMAND/EVENT)
 * @old_trb_comp_code: 旧的 TRB 完成码 (用于重试逻辑)
 * @trb_address_map:  物理 TRB 地址到 segment 的映射 (用于流)
 */
struct xhci_ring {
	struct xhci_segment	*first_seg;
	struct xhci_segment	*last_seg;
	union  xhci_trb		*enqueue;
	struct xhci_segment	*enq_seg;
	union  xhci_trb		*dequeue;
	struct xhci_segment	*deq_seg;
	struct list_head	td_list;
	/*
	 * Write the cycle state into the TRB cycle field to give ownership of
	 * the TRB to the host controller (if we are the producer), or to check
	 * if we own the TRB (if we are the consumer).  See section 4.9.1.
	 */
	u32			cycle_state;
	unsigned int		stream_id;
	unsigned int		num_segs;
	unsigned int		num_trbs_free; /* used only by xhci DbC */
	unsigned int		bounce_buf_len;
	enum xhci_ring_type	type;
	u32			old_trb_comp_code;
	struct radix_tree_root	*trb_address_map;
};

struct xhci_erst_entry {
	/* 64-bit event ring segment address */
	__le64	seg_addr;
	__le32	seg_size;
	/* Set to zero */
	__le32	rsvd;
};

struct xhci_erst {
	struct xhci_erst_entry	*entries;
	unsigned int		num_entries;
	/* xhci->event_ring keeps track of segment dma addresses */
	dma_addr_t		erst_dma_addr;
};

struct xhci_scratchpad {
	u64 *sp_array;
	dma_addr_t sp_dma;
	void **sp_buffers;
};

struct urb_priv {
	int	num_tds;
	int	num_tds_done;
	struct	xhci_td	td[] __counted_by(num_tds);
};

/* Number of Event Ring segments to allocate, when amount is not specified. (spec allows 32k) */
#define	ERST_DEFAULT_SEGS	2
/* Poll every 60 seconds */
#define	POLL_TIMEOUT	60
/* Stop endpoint command timeout (secs) for URB cancellation watchdog timer */
#define XHCI_STOP_EP_CMD_TIMEOUT	5
/* XXX: Make these module parameters */

struct s3_save {
	u32	command;
	u32	dev_nt;
	u64	dcbaa_ptr;
	u32	config_reg;
};

/* Use for lpm */
struct dev_info {
	u32			dev_id;
	struct	list_head	list;
};

struct xhci_bus_state {
	unsigned long		bus_suspended;
	unsigned long		next_statechange;

	/* Port suspend arrays are indexed by the portnum of the fake roothub */
	/* ports suspend status arrays - max 31 ports for USB2, 15 for USB3 */
	u32			port_c_suspend;
	u32			suspended_ports;
	u32			port_remote_wakeup;
	/* which ports have started to resume */
	unsigned long		resuming_ports;
};

struct xhci_interrupter {
	struct xhci_ring	*event_ring;
	struct xhci_erst	erst;
	struct xhci_intr_reg __iomem *ir_set;
	unsigned int		intr_num;
	bool			ip_autoclear;
	u32			isoc_bei_interval;
	/* For interrupter registers save and restore over suspend/resume */
	u32	s3_iman;
	u32	s3_imod;
	u32	s3_erst_size;
	u64	s3_erst_base;
	u64	s3_erst_dequeue;
};
/*
 * It can take up to 20 ms to transition from RExit to U0 on the
 * Intel Lynx Point LP xHCI host.
 */
#define	XHCI_MAX_REXIT_TIMEOUT_MS	20
struct xhci_port_cap {
	u32			*psi;	/* array of protocol speed ID entries */
	u8			psi_count;
	u8			psi_uid_count;
	u8			maj_rev;
	u8			min_rev;
	u32			protocol_caps;
};

struct xhci_port {
	struct xhci_port_regs __iomem	*port_reg;
	int			hw_portnum;
	int			hcd_portnum;
	struct xhci_hub		*rhub;
	struct xhci_port_cap	*port_cap;
	unsigned int		lpm_incapable:1;
	unsigned long		resume_timestamp;
	bool			rexit_active;
	/* Slot ID is the index of the device directly connected to the port */
	int			slot_id;
	struct completion	rexit_done;
	struct completion	u3exit_done;
};

struct xhci_hub {
	struct xhci_port	**ports;
	unsigned int		num_ports;
	struct usb_hcd		*hcd;
	/* keep track of bus suspend info */
	struct xhci_bus_state   bus_state;
	/* supported prococol extended capabiliy values */
	u8			maj_rev;
	u8			min_rev;
};

/*
 * struct xhci_hcd - XHCI 主控制器结构体 (每个控制器一个实例)
 *
 * 这是 XHCI 驱动最核心的结构体，管理一个 xHC 硬件的全部状态。
 * 包含寄存器映射、数据传输结构、设备管理、和中断处理等所有子系统。
 *
 * 内部架构概览:
 *
 * 一、寄存器映射 (Register Mappings):
 *   @cap_regs:   Capability Registers (能力寄存器) — HC 版本、结构参数等只读信息
 *   @op_regs:    Operational Registers (操作寄存器) — 命令/状态/配置/端口
 *   @run_regs:   Runtime Registers (运行时寄存器) — 微帧计数器、中断寄存器组
 *   @dba:        Doorbell Array (门铃数组) — 写门铃通知 HC 处理 TRB
 *
 * 二、数据传输结构 (Data Transfer):
 *   @cmd_ring:   Command Ring — 驱动向 HC 发送命令的传输环
 *                (如 Enable Slot, Address Device, Configure Endpoint 等)
 *   @interrupters[]: 中断寄存器组 — 每个 MSI-X 向量对应一个 Event Ring
 *   @dcbaa:      Device Context Base Address Array — HC 内部使用的设备
 *                上下文 DMA 地址数组，每个 Slot 对应一个 64-bit 指针
 *
 * 三、设备管理 (Device Management):
 *   @devs[]:     虚拟设备数组 — 软件对 USB 设备的抽象，每个 Slot 一个
 *   @scratchpad: 暂存缓冲区 — HC 初始化时分配的 DMA 缓冲区，供 HC 内部使用
 *   @hw_ports:   硬件端口数组 — 所有 USB2/USB3 端口的抽象
 *   @usb2_rhub / @usb3_rhub: USB2 和 USB3 的根 Hub 信息
 *
 * 四、初始化流程:
 *   1. 读取 cap_regs 获取 HC 能力 (hci_version, max_slots, max_ports)
 *   2. 分配 DCBAA、Command Ring、Event Ring、Scratchpad
 *   3. 写 op_regs 的 USBCMD 启动 HC
 *   4. 中断使能后，HC 开始处理 Doorbell 和事件
 *
 * 五、quirk 机制:
 *   @quirks: 64-bit 标志位，用于处理不同厂商 xHC 的硬件缺陷和行为差异。
 *            Intel/NEC/AMD/MediaTek/Zhaoxin 等厂商都有对应的 quirk。
 */
 /* There is one xhci_hcd structure per controller */
struct xhci_hcd {
	struct usb_hcd *main_hcd;
	struct usb_hcd *shared_hcd;
	/* glue to PCI and HCD framework */
	struct xhci_cap_regs __iomem *cap_regs;
	struct xhci_op_regs __iomem *op_regs;
	struct xhci_run_regs __iomem *run_regs;
	struct xhci_doorbell_array __iomem *dba;

	/* Cached register copies of read-only HC data */
	__u32		hcs_params2;
	__u32		hcs_params3;
	__u32		hcc_params;
	__u32		hcc_params2;

	spinlock_t	lock;

	/* packed release number */
	u16		hci_version;
	u16		max_interrupters;
	u8		max_slots;
	u8		max_ports;
	/* imod_interval in ns (I * 250ns) */
	u32		imod_interval;
	u32		page_size;
	/* MSI-X/MSI vectors */
	int		nvecs;
	/* optional clocks */
	struct clk		*clk;
	struct clk		*reg_clk;
	/* optional reset controller */
	struct reset_control *reset;
	/* data structures */
	struct xhci_device_context_array *dcbaa;
	struct xhci_interrupter **interrupters;
	struct xhci_ring	*cmd_ring;
	unsigned int            cmd_ring_state;
#define CMD_RING_STATE_RUNNING         BIT(0)
#define CMD_RING_STATE_ABORTED         BIT(1)
#define CMD_RING_STATE_STOPPED         BIT(2)
	struct list_head        cmd_list;
	unsigned int		cmd_ring_reserved_trbs;
	struct delayed_work	cmd_timer;
	struct completion	cmd_ring_stop_completion;
	struct xhci_command	*current_cmd;

	/* Scratchpad */
	struct xhci_scratchpad  *scratchpad;

	/* slot enabling and address device helpers */
	/* these are not thread safe so use mutex */
	struct mutex mutex;
	/* Internal mirror of the HW's dcbaa */
	struct xhci_virt_device	*devs[MAX_HC_SLOTS];
	/* For keeping track of bandwidth domains per roothub. */
	struct xhci_root_port_bw_info	*rh_bw;

	/* DMA pools */
	struct dma_pool	*device_pool;
	struct dma_pool	*segment_pool;
	struct dma_pool	*small_streams_pool;
	struct dma_pool	*port_bw_pool;
	struct dma_pool	*medium_streams_pool;

	/* Host controller watchdog timer structures */
	unsigned int		xhc_state;
	unsigned long		run_graceperiod;
	struct s3_save		s3;
/* Host controller is dying - not responding to commands. "I'm not dead yet!"
 *
 * xHC interrupts have been disabled and a watchdog timer will (or has already)
 * halt the xHCI host, and complete all URBs with an -ESHUTDOWN code.  Any code
 * that sees this status (other than the timer that set it) should stop touching
 * hardware immediately.  Interrupt handlers should return immediately when
 * they see this status (any time they drop and re-acquire xhci->lock).
 * xhci_urb_dequeue() should call usb_hcd_check_unlink_urb() and return without
 * putting the TD on the canceled list, etc.
 *
 * There are no reports of xHCI host controllers that display this issue.
 */
#define XHCI_STATE_DYING	BIT(0)
#define XHCI_STATE_HALTED	BIT(1)
#define XHCI_STATE_REMOVING	BIT(2)
	unsigned long long	quirks;
#define	XHCI_LINK_TRB_QUIRK	BIT_ULL(0)
#define XHCI_RESET_EP_QUIRK	BIT_ULL(1) /* Deprecated */
#define XHCI_NEC_HOST		BIT_ULL(2)
#define XHCI_AMD_PLL_FIX	BIT_ULL(3)
#define XHCI_SPURIOUS_SUCCESS	BIT_ULL(4)
/*
 * Certain Intel host controllers have a limit to the number of endpoint
 * contexts they can handle.  Ideally, they would signal that they can't handle
 * anymore endpoint contexts by returning a Resource Error for the Configure
 * Endpoint command, but they don't.  Instead they expect software to keep track
 * of the number of active endpoints for them, across configure endpoint
 * commands, reset device commands, disable slot commands, and address device
 * commands.
 */
#define XHCI_EP_LIMIT_QUIRK	BIT_ULL(5)
#define XHCI_BROKEN_MSI		BIT_ULL(6)
#define XHCI_RESET_ON_RESUME	BIT_ULL(7)
#define	XHCI_SW_BW_CHECKING	BIT_ULL(8)
#define XHCI_AMD_0x96_HOST	BIT_ULL(9)
#define XHCI_TRUST_TX_LENGTH	BIT_ULL(10) /* Deprecated */
#define XHCI_LPM_SUPPORT	BIT_ULL(11)
#define XHCI_INTEL_HOST		BIT_ULL(12)
#define XHCI_SPURIOUS_REBOOT	BIT_ULL(13)
#define XHCI_COMP_MODE_QUIRK	BIT_ULL(14)
#define XHCI_AVOID_BEI		BIT_ULL(15)
#define XHCI_PLAT		BIT_ULL(16) /* Deprecated */
#define XHCI_SLOW_SUSPEND	BIT_ULL(17)
#define XHCI_SPURIOUS_WAKEUP	BIT_ULL(18)
/* For controllers with a broken beyond repair streams implementation */
#define XHCI_BROKEN_STREAMS	BIT_ULL(19)
#define XHCI_PME_STUCK_QUIRK	BIT_ULL(20)
#define XHCI_MTK_HOST		BIT_ULL(21)
#define XHCI_SSIC_PORT_UNUSED	BIT_ULL(22)
#define XHCI_NO_64BIT_SUPPORT	BIT_ULL(23)
#define XHCI_MISSING_CAS	BIT_ULL(24)
/* For controller with a broken Port Disable implementation */
#define XHCI_BROKEN_PORT_PED	BIT_ULL(25)
#define XHCI_LIMIT_ENDPOINT_INTERVAL_7	BIT_ULL(26)
#define XHCI_U2_DISABLE_WAKE	BIT_ULL(27)
#define XHCI_ASMEDIA_MODIFY_FLOWCONTROL	BIT_ULL(28)
#define XHCI_HW_LPM_DISABLE	BIT_ULL(29)
#define XHCI_SUSPEND_DELAY	BIT_ULL(30)
#define XHCI_INTEL_USB_ROLE_SW	BIT_ULL(31)
#define XHCI_ZERO_64B_REGS	BIT_ULL(32)
#define XHCI_DEFAULT_PM_RUNTIME_ALLOW	BIT_ULL(33)
#define XHCI_RESET_PLL_ON_DISCONNECT	BIT_ULL(34)
#define XHCI_SNPS_BROKEN_SUSPEND    BIT_ULL(35)
/* Reserved. It was XHCI_RENESAS_FW_QUIRK */
#define XHCI_SKIP_PHY_INIT	BIT_ULL(37)
#define XHCI_DISABLE_SPARSE	BIT_ULL(38)
#define XHCI_SG_TRB_CACHE_SIZE_QUIRK	BIT_ULL(39)
#define XHCI_NO_SOFT_RETRY	BIT_ULL(40)
#define XHCI_BROKEN_D3COLD_S2I	BIT_ULL(41)
#define XHCI_EP_CTX_BROKEN_DCS	BIT_ULL(42)
#define XHCI_SUSPEND_RESUME_CLKS	BIT_ULL(43)
#define XHCI_RESET_TO_DEFAULT	BIT_ULL(44)
#define XHCI_TRB_OVERFETCH	BIT_ULL(45)
#define XHCI_ZHAOXIN_HOST	BIT_ULL(46)
#define XHCI_WRITE_64_HI_LO	BIT_ULL(47)
#define XHCI_CDNS_SCTX_QUIRK	BIT_ULL(48)
#define XHCI_ETRON_HOST	BIT_ULL(49)
#define XHCI_LIMIT_ENDPOINT_INTERVAL_9 BIT_ULL(50)

	unsigned int		num_active_eps;
	unsigned int		limit_active_eps;
	struct xhci_port	*hw_ports;
	struct xhci_hub		usb2_rhub;
	struct xhci_hub		usb3_rhub;
	/* support xHCI 1.0 spec USB2 hardware LPM */
	unsigned		hw_lpm_support:1;
	/* Broken Suspend flag for SNPS Suspend resume issue */
	unsigned		broken_suspend:1;
	/* Indicates that omitting hcd is supported if root hub has no ports */
	unsigned		allow_single_roothub:1;
	/* cached extended protocol port capabilities */
	struct xhci_port_cap	*port_caps;
	unsigned int		num_port_caps;
	/* Compliance Mode Recovery Data */
	struct timer_list	comp_mode_recovery_timer;
	u32			port_status_u0;
	u16			test_mode;
/* Compliance Mode Timer Triggered every 2 seconds */
#define COMP_MODE_RCVRY_MSECS 2000

	struct dentry		*debugfs_root;
	struct dentry		*debugfs_slots;
	struct list_head	regset_list;

	void			*dbc;
	/* platform-specific data -- must come last */
	unsigned long		priv[] __aligned(sizeof(s64));
};

/* Platform specific overrides to generic XHCI hc_driver ops */
struct xhci_driver_overrides {
	size_t extra_priv_size;
	int (*reset)(struct usb_hcd *hcd);
	int (*start)(struct usb_hcd *hcd);
	int (*add_endpoint)(struct usb_hcd *hcd, struct usb_device *udev,
			    struct usb_host_endpoint *ep);
	int (*drop_endpoint)(struct usb_hcd *hcd, struct usb_device *udev,
			     struct usb_host_endpoint *ep);
	int (*check_bandwidth)(struct usb_hcd *, struct usb_device *);
	void (*reset_bandwidth)(struct usb_hcd *, struct usb_device *);
	int (*update_hub_device)(struct usb_hcd *hcd, struct usb_device *hdev,
			    struct usb_tt *tt, gfp_t mem_flags);
	int (*hub_control)(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
			   u16 wIndex, char *buf, u16 wLength);
};

#define	XHCI_CFC_DELAY		10

/* convert between an HCD pointer and the corresponding EHCI_HCD */
static inline struct xhci_hcd *hcd_to_xhci(struct usb_hcd *hcd)
{
	struct usb_hcd *primary_hcd;

	if (usb_hcd_is_primary_hcd(hcd))
		primary_hcd = hcd;
	else
		primary_hcd = hcd->primary_hcd;

	return (struct xhci_hcd *) (primary_hcd->hcd_priv);
}

static inline struct usb_hcd *xhci_to_hcd(struct xhci_hcd *xhci)
{
	return xhci->main_hcd;
}

static inline struct usb_hcd *xhci_get_usb3_hcd(struct xhci_hcd *xhci)
{
	if (xhci->shared_hcd)
		return xhci->shared_hcd;

	if (!xhci->usb2_rhub.num_ports)
		return xhci->main_hcd;

	return NULL;
}

static inline bool xhci_hcd_is_usb3(struct usb_hcd *hcd)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	return hcd == xhci_get_usb3_hcd(xhci);
}

static inline bool xhci_has_one_roothub(struct xhci_hcd *xhci)
{
	return xhci->allow_single_roothub &&
	       (!xhci->usb2_rhub.num_ports || !xhci->usb3_rhub.num_ports);
}

#define xhci_dbg(xhci, fmt, args...) \
	dev_dbg(xhci_to_hcd(xhci)->self.controller , fmt , ## args)
#define xhci_err(xhci, fmt, args...) \
	dev_err(xhci_to_hcd(xhci)->self.controller , fmt , ## args)
#define xhci_warn(xhci, fmt, args...) \
	dev_warn(xhci_to_hcd(xhci)->self.controller , fmt , ## args)
#define xhci_info(xhci, fmt, args...) \
	dev_info(xhci_to_hcd(xhci)->self.controller , fmt , ## args)

/*
 * Registers should always be accessed with double word or quad word accesses.
 *
 * Some xHCI implementations may support 64-bit address pointers.  Registers
 * with 64-bit address pointers should be written to with dword accesses by
 * writing the low dword first (ptr[0]), then the high dword (ptr[1]) second.
 * xHCI implementations that do not support 64-bit address pointers will ignore
 * the high dword, and write order is irrelevant.
 */
static inline u64 xhci_read_64(const struct xhci_hcd *xhci,
		__le64 __iomem *regs)
{
	return lo_hi_readq(regs);
}
static inline void xhci_write_64(struct xhci_hcd *xhci,
				 const u64 val, __le64 __iomem *regs)
{
	lo_hi_writeq(val, regs);
}


/*
 * Reportedly, some chapters of v0.95 spec said that Link TRB always has its chain bit set.
 * Other chapters and later specs say that it should only be set if the link is inside a TD
 * which continues from the end of one segment to the next segment.
 *
 * Some 0.95 hardware was found to misbehave if any link TRB doesn't have the chain bit set.
 *
 * 0.96 hardware from AMD and NEC was found to ignore unchained isochronous link TRBs when
 * "resynchronizing the pipe" after a Missed Service Error.
 */
static inline bool xhci_link_chain_quirk(struct xhci_hcd *xhci, enum xhci_ring_type type)
{
	return (xhci->quirks & XHCI_LINK_TRB_QUIRK) ||
	       (type == TYPE_ISOC && (xhci->quirks & (XHCI_AMD_0x96_HOST | XHCI_NEC_HOST)));
}

/* xHCI debugging */
char *xhci_get_slot_state(struct xhci_hcd *xhci,
		struct xhci_container_ctx *ctx);
void xhci_dbg_trace(struct xhci_hcd *xhci, void (*trace)(struct va_format *),
			const char *fmt, ...);

/* xHCI memory management */
void xhci_mem_cleanup(struct xhci_hcd *xhci);
int xhci_mem_init(struct xhci_hcd *xhci, gfp_t flags);
void xhci_free_virt_device(struct xhci_hcd *xhci, struct xhci_virt_device *dev, int slot_id);
void xhci_free_virt_devices_depth_first(struct xhci_hcd *xhci, int slot_id);
int xhci_alloc_virt_device(struct xhci_hcd *xhci, int slot_id, struct usb_device *udev, gfp_t flags);
int xhci_setup_addressable_virt_dev(struct xhci_hcd *xhci, struct usb_device *udev);
void xhci_copy_ep0_dequeue_into_input_ctx(struct xhci_hcd *xhci,
		struct usb_device *udev);
unsigned int xhci_get_endpoint_index(struct usb_endpoint_descriptor *desc);
unsigned int xhci_last_valid_endpoint(u32 added_ctxs);
void xhci_endpoint_zero(struct xhci_hcd *xhci, struct xhci_virt_device *virt_dev, struct usb_host_endpoint *ep);
void xhci_update_tt_active_eps(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		int old_active_eps);
void xhci_clear_endpoint_bw_info(struct xhci_bw_info *bw_info);
void xhci_rh_bw_cleanup(struct xhci_hcd *xhci);
void xhci_update_bw_info(struct xhci_hcd *xhci,
		struct xhci_container_ctx *in_ctx,
		struct xhci_input_control_ctx *ctrl_ctx,
		struct xhci_virt_device *virt_dev);
void xhci_endpoint_copy(struct xhci_hcd *xhci,
		struct xhci_container_ctx *in_ctx,
		struct xhci_container_ctx *out_ctx,
		unsigned int ep_index);
void xhci_slot_copy(struct xhci_hcd *xhci,
		struct xhci_container_ctx *in_ctx,
		struct xhci_container_ctx *out_ctx);
int xhci_endpoint_init(struct xhci_hcd *xhci, struct xhci_virt_device *virt_dev,
		struct usb_device *udev, struct usb_host_endpoint *ep,
		gfp_t mem_flags);
struct xhci_ring *xhci_ring_alloc(struct xhci_hcd *xhci, unsigned int num_segs,
		enum xhci_ring_type type, unsigned int max_packet, gfp_t flags);
void xhci_ring_free(struct xhci_hcd *xhci, struct xhci_ring *ring);
int xhci_ring_expansion(struct xhci_hcd *xhci, struct xhci_ring *ring,
		unsigned int num_trbs, gfp_t flags);
void xhci_initialize_ring_info(struct xhci_ring *ring);
void xhci_ring_init(struct xhci_hcd *xhci, struct xhci_ring *ring);
void xhci_free_endpoint_ring(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		unsigned int ep_index);
struct xhci_stream_info *xhci_alloc_stream_info(struct xhci_hcd *xhci,
		unsigned int num_stream_ctxs,
		unsigned int num_streams,
		unsigned int max_packet, gfp_t flags);
void xhci_free_stream_info(struct xhci_hcd *xhci,
		struct xhci_stream_info *stream_info);
void xhci_setup_streams_ep_input_ctx(struct xhci_hcd *xhci,
		struct xhci_ep_ctx *ep_ctx,
		struct xhci_stream_info *stream_info);
void xhci_setup_no_streams_ep_input_ctx(struct xhci_ep_ctx *ep_ctx,
		struct xhci_virt_ep *ep);
void xhci_free_device_endpoint_resources(struct xhci_hcd *xhci,
	struct xhci_virt_device *virt_dev, bool drop_control_ep);
struct xhci_ring *xhci_dma_to_transfer_ring(
		struct xhci_virt_ep *ep,
		u64 address);
struct xhci_command *xhci_alloc_command(struct xhci_hcd *xhci,
		bool allocate_completion, gfp_t mem_flags);
struct xhci_command *xhci_alloc_command_with_ctx(struct xhci_hcd *xhci,
		bool allocate_completion, gfp_t mem_flags);
void xhci_urb_free_priv(struct urb_priv *urb_priv);
void xhci_free_command(struct xhci_hcd *xhci,
		struct xhci_command *command);
struct xhci_container_ctx *xhci_alloc_container_ctx(struct xhci_hcd *xhci,
		int type, gfp_t flags);
void xhci_free_container_ctx(struct xhci_hcd *xhci,
		struct xhci_container_ctx *ctx);
struct xhci_container_ctx *xhci_alloc_port_bw_ctx(struct xhci_hcd *xhci,
		gfp_t flags);
void xhci_free_port_bw_ctx(struct xhci_hcd *xhci,
		struct xhci_container_ctx *ctx);
struct xhci_interrupter *
xhci_create_secondary_interrupter(struct usb_hcd *hcd, unsigned int segs,
				  u32 imod_interval, unsigned int intr_num);
void xhci_remove_secondary_interrupter(struct usb_hcd
				       *hcd, struct xhci_interrupter *ir);
void xhci_skip_sec_intr_events(struct xhci_hcd *xhci,
			       struct xhci_ring *ring,
			       struct xhci_interrupter *ir);

/* xHCI host controller glue */
typedef void (*xhci_get_quirks_t)(struct device *, struct xhci_hcd *);
int xhci_handshake(void __iomem *ptr, u32 mask, u32 done, u64 timeout_us);
void xhci_quiesce(struct xhci_hcd *xhci);
int xhci_halt(struct xhci_hcd *xhci);
int xhci_start(struct xhci_hcd *xhci);
int xhci_reset(struct xhci_hcd *xhci, u64 timeout_us);
int xhci_run(struct usb_hcd *hcd);
int xhci_gen_setup(struct usb_hcd *hcd, xhci_get_quirks_t get_quirks);
void xhci_shutdown(struct usb_hcd *hcd);
void xhci_stop(struct usb_hcd *hcd);
void xhci_init_driver(struct hc_driver *drv,
		      const struct xhci_driver_overrides *over);
int xhci_add_endpoint(struct usb_hcd *hcd, struct usb_device *udev,
		      struct usb_host_endpoint *ep);
int xhci_drop_endpoint(struct usb_hcd *hcd, struct usb_device *udev,
		       struct usb_host_endpoint *ep);
int xhci_check_bandwidth(struct usb_hcd *hcd, struct usb_device *udev);
void xhci_reset_bandwidth(struct usb_hcd *hcd, struct usb_device *udev);
int xhci_update_hub_device(struct usb_hcd *hcd, struct usb_device *hdev,
			   struct usb_tt *tt, gfp_t mem_flags);
int xhci_disable_slot(struct xhci_hcd *xhci, u32 slot_id);
int xhci_disable_and_free_slot(struct xhci_hcd *xhci, u32 slot_id);
int xhci_ext_cap_init(struct xhci_hcd *xhci);

int xhci_suspend(struct xhci_hcd *xhci, bool do_wakeup);
int xhci_resume(struct xhci_hcd *xhci, bool power_lost, bool is_auto_resume);

irqreturn_t xhci_irq(struct usb_hcd *hcd);
irqreturn_t xhci_msi_irq(int irq, void *hcd);
int xhci_alloc_dev(struct usb_hcd *hcd, struct usb_device *udev);
int xhci_alloc_tt_info(struct xhci_hcd *xhci,
		struct xhci_virt_device *virt_dev,
		struct usb_device *hdev,
		struct usb_tt *tt, gfp_t mem_flags);
int xhci_set_interrupter_moderation(struct xhci_interrupter *ir,
				    u32 imod_interval);
int xhci_enable_interrupter(struct xhci_interrupter *ir);
int xhci_disable_interrupter(struct xhci_hcd *xhci, struct xhci_interrupter *ir);

/* xHCI ring, segment, TRB, and TD functions */
dma_addr_t xhci_trb_virt_to_dma(struct xhci_segment *seg, union xhci_trb *trb);
int xhci_is_vendor_info_code(struct xhci_hcd *xhci, unsigned int trb_comp_code);
void xhci_ring_cmd_db(struct xhci_hcd *xhci);
int xhci_queue_slot_control(struct xhci_hcd *xhci, struct xhci_command *cmd,
		u32 trb_type, u32 slot_id);
int xhci_queue_address_device(struct xhci_hcd *xhci, struct xhci_command *cmd,
		dma_addr_t in_ctx_ptr, u32 slot_id, enum xhci_setup_dev);
int xhci_queue_vendor_command(struct xhci_hcd *xhci, struct xhci_command *cmd,
		u32 field1, u32 field2, u32 field3, u32 field4);
int xhci_queue_stop_endpoint(struct xhci_hcd *xhci, struct xhci_command *cmd,
		int slot_id, unsigned int ep_index, int suspend);
int xhci_queue_ctrl_tx(struct xhci_hcd *xhci, gfp_t mem_flags, struct urb *urb,
		int slot_id, unsigned int ep_index);
int xhci_queue_bulk_tx(struct xhci_hcd *xhci, gfp_t mem_flags, struct urb *urb,
		int slot_id, unsigned int ep_index);
int xhci_queue_intr_tx(struct xhci_hcd *xhci, gfp_t mem_flags, struct urb *urb,
		int slot_id, unsigned int ep_index);
int xhci_queue_isoc_tx_prepare(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index);
int xhci_queue_configure_endpoint(struct xhci_hcd *xhci,
		struct xhci_command *cmd, dma_addr_t in_ctx_ptr, u32 slot_id,
		bool command_must_succeed);
int xhci_queue_get_port_bw(struct xhci_hcd *xhci,
		struct xhci_command *cmd, dma_addr_t in_ctx_ptr,
		u8 dev_speed, bool command_must_succeed);
int xhci_get_port_bandwidth(struct xhci_hcd *xhci, struct xhci_container_ctx *ctx,
		u8 dev_speed);
int xhci_queue_evaluate_context(struct xhci_hcd *xhci, struct xhci_command *cmd,
		dma_addr_t in_ctx_ptr, u32 slot_id, bool command_must_succeed);
int xhci_queue_reset_ep(struct xhci_hcd *xhci, struct xhci_command *cmd,
		int slot_id, unsigned int ep_index,
		enum xhci_ep_reset_type reset_type);
int xhci_queue_reset_device(struct xhci_hcd *xhci, struct xhci_command *cmd,
		u32 slot_id);
void xhci_handle_command_timeout(struct work_struct *work);

void xhci_ring_ep_doorbell(struct xhci_hcd *xhci, unsigned int slot_id,
		unsigned int ep_index, unsigned int stream_id);
void xhci_ring_doorbell_for_active_rings(struct xhci_hcd *xhci,
		unsigned int slot_id,
		unsigned int ep_index);
void xhci_cleanup_command_queue(struct xhci_hcd *xhci);
void inc_deq(struct xhci_hcd *xhci, struct xhci_ring *ring);
unsigned int count_trbs(u64 addr, u64 len);
int xhci_stop_endpoint_sync(struct xhci_hcd *xhci, struct xhci_virt_ep *ep,
			    int suspend, gfp_t gfp_flags);
void xhci_process_cancelled_tds(struct xhci_virt_ep *ep);
void xhci_update_erst_dequeue(struct xhci_hcd *xhci,
			      struct xhci_interrupter *ir,
			      bool clear_ehb);
void xhci_add_interrupter(struct xhci_hcd *xhci, unsigned int intr_num);
int xhci_usb_endpoint_maxp(struct usb_device *udev,
			   struct usb_host_endpoint *host_ep);
void xhci_portsc_writel(struct xhci_port *port, u32 val);
u32 xhci_portsc_readl(struct xhci_port *port);

/* xHCI roothub code */
void xhci_set_link_state(struct xhci_hcd *xhci, struct xhci_port *port,
				u32 link_state);
void xhci_test_and_clear_bit(struct xhci_hcd *xhci, struct xhci_port *port,
				u32 port_bit);
int xhci_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue, u16 wIndex,
		char *buf, u16 wLength);
int xhci_hub_status_data(struct usb_hcd *hcd, char *buf);
int xhci_find_raw_port_number(struct usb_hcd *hcd, int port1);
struct xhci_hub *xhci_get_rhub(struct usb_hcd *hcd);
enum usb_link_tunnel_mode xhci_port_is_tunneled(struct xhci_hcd *xhci,
						struct xhci_port *port);
void xhci_hc_died(struct xhci_hcd *xhci);

#ifdef CONFIG_PM
int xhci_bus_suspend(struct usb_hcd *hcd);
int xhci_bus_resume(struct usb_hcd *hcd);
unsigned long xhci_get_resuming_ports(struct usb_hcd *hcd);
#else
#define	xhci_bus_suspend	NULL
#define	xhci_bus_resume		NULL
#define	xhci_get_resuming_ports	NULL
#endif	/* CONFIG_PM */

u32 xhci_port_state_to_neutral(u32 state);
void xhci_ring_device(struct xhci_hcd *xhci, int slot_id);

/* xHCI contexts */
struct xhci_input_control_ctx *xhci_get_input_control_ctx(struct xhci_container_ctx *ctx);
struct xhci_slot_ctx *xhci_get_slot_ctx(struct xhci_hcd *xhci, struct xhci_container_ctx *ctx);
struct xhci_ep_ctx *xhci_get_ep_ctx(struct xhci_hcd *xhci, struct xhci_container_ctx *ctx, unsigned int ep_index);

struct xhci_ring *xhci_triad_to_transfer_ring(struct xhci_hcd *xhci,
		unsigned int slot_id, unsigned int ep_index,
		unsigned int stream_id);

static inline struct xhci_ring *xhci_urb_to_transfer_ring(struct xhci_hcd *xhci,
								struct urb *urb)
{
	return xhci_triad_to_transfer_ring(xhci, urb->dev->slot_id,
					xhci_get_endpoint_index(&urb->ep->desc),
					urb->stream_id);
}

/*
 * TODO: As per spec Isochronous IDT transmissions are supported. We bypass
 * them anyways as we where unable to find a device that matches the
 * constraints.
 */
static inline bool xhci_urb_suitable_for_idt(struct urb *urb)
{
	if (!usb_endpoint_xfer_isoc(&urb->ep->desc) && usb_urb_dir_out(urb) &&
	    usb_endpoint_maxp(&urb->ep->desc) >= TRB_IDT_MAX_SIZE &&
	    urb->transfer_buffer_length <= TRB_IDT_MAX_SIZE &&
	    !(urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP) &&
	    !urb->num_sgs)
		return true;

	return false;
}

static inline char *xhci_slot_state_string(u32 state)
{
	switch (state) {
	case SLOT_STATE_ENABLED:
		return "enabled/disabled";
	case SLOT_STATE_DEFAULT:
		return "default";
	case SLOT_STATE_ADDRESSED:
		return "addressed";
	case SLOT_STATE_CONFIGURED:
		return "configured";
	default:
		return "reserved";
	}
}

static inline const char *xhci_decode_trb(char *str, size_t size,
					  u32 field0, u32 field1, u32 field2, u32 field3)
{
	int type = TRB_FIELD_TO_TYPE(field3);

	switch (type) {
	case TRB_LINK:
		snprintf(str, size,
			"LINK %08x%08x intr %d type '%s' flags %c:%c:%c:%c",
			field1, field0, GET_INTR_TARGET(field2),
			xhci_trb_type_string(type),
			field3 & TRB_IOC ? 'I' : 'i',
			field3 & TRB_CHAIN ? 'C' : 'c',
			field3 & TRB_TC ? 'T' : 't',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_TRANSFER:
	case TRB_COMPLETION:
	case TRB_PORT_STATUS:
	case TRB_BANDWIDTH_EVENT:
	case TRB_DOORBELL:
	case TRB_HC_EVENT:
	case TRB_DEV_NOTE:
	case TRB_MFINDEX_WRAP:
		snprintf(str, size,
			"TRB %08x%08x status '%s' len %d slot %d ep %d type '%s' flags %c:%c",
			field1, field0,
			xhci_trb_comp_code_string(GET_COMP_CODE(field2)),
			EVENT_TRB_LEN(field2), TRB_TO_SLOT_ID(field3),
			TRB_TO_EP_ID(field3),
			xhci_trb_type_string(type),
			field3 & EVENT_DATA ? 'E' : 'e',
			field3 & TRB_CYCLE ? 'C' : 'c');

		break;
	case TRB_SETUP:
		snprintf(str, size,
			"bRequestType %02x bRequest %02x wValue %02x%02x wIndex %02x%02x wLength %d length %d TD size %d intr %d type '%s' flags %c:%c:%c",
				field0 & 0xff,
				(field0 & 0xff00) >> 8,
				(field0 & 0xff000000) >> 24,
				(field0 & 0xff0000) >> 16,
				(field1 & 0xff00) >> 8,
				field1 & 0xff,
				(field1 & 0xff000000) >> 16 |
				(field1 & 0xff0000) >> 16,
				TRB_LEN(field2), GET_TD_SIZE(field2),
				GET_INTR_TARGET(field2),
				xhci_trb_type_string(type),
				field3 & TRB_IDT ? 'I' : 'i',
				field3 & TRB_IOC ? 'I' : 'i',
				field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_DATA:
		snprintf(str, size,
			 "Buffer %08x%08x length %d TD size %d intr %d type '%s' flags %c:%c:%c:%c:%c:%c:%c",
				field1, field0, TRB_LEN(field2), GET_TD_SIZE(field2),
				GET_INTR_TARGET(field2),
				xhci_trb_type_string(type),
				field3 & TRB_IDT ? 'I' : 'i',
				field3 & TRB_IOC ? 'I' : 'i',
				field3 & TRB_CHAIN ? 'C' : 'c',
				field3 & TRB_NO_SNOOP ? 'S' : 's',
				field3 & TRB_ISP ? 'I' : 'i',
				field3 & TRB_ENT ? 'E' : 'e',
				field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_STATUS:
		snprintf(str, size,
			 "Buffer %08x%08x length %d TD size %d intr %d type '%s' flags %c:%c:%c:%c",
				field1, field0, TRB_LEN(field2), GET_TD_SIZE(field2),
				GET_INTR_TARGET(field2),
				xhci_trb_type_string(type),
				field3 & TRB_IOC ? 'I' : 'i',
				field3 & TRB_CHAIN ? 'C' : 'c',
				field3 & TRB_ENT ? 'E' : 'e',
				field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_NORMAL:
	case TRB_EVENT_DATA:
	case TRB_TR_NOOP:
		snprintf(str, size,
			"Buffer %08x%08x length %d TD size %d intr %d type '%s' flags %c:%c:%c:%c:%c:%c:%c:%c",
			field1, field0, TRB_LEN(field2), GET_TD_SIZE(field2),
			GET_INTR_TARGET(field2),
			xhci_trb_type_string(type),
			field3 & TRB_BEI ? 'B' : 'b',
			field3 & TRB_IDT ? 'I' : 'i',
			field3 & TRB_IOC ? 'I' : 'i',
			field3 & TRB_CHAIN ? 'C' : 'c',
			field3 & TRB_NO_SNOOP ? 'S' : 's',
			field3 & TRB_ISP ? 'I' : 'i',
			field3 & TRB_ENT ? 'E' : 'e',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_ISOC:
		snprintf(str, size,
			"Buffer %08x%08x length %d TD size/TBC %d intr %d type '%s' TBC %u TLBPC %u frame_id %u flags %c:%c:%c:%c:%c:%c:%c:%c:%c",
			field1, field0, TRB_LEN(field2), GET_TD_SIZE(field2),
			GET_INTR_TARGET(field2),
			xhci_trb_type_string(type),
			GET_TBC(field3),
			GET_TLBPC(field3),
			GET_FRAME_ID(field3),
			field3 & TRB_SIA ? 'S' : 's',
			field3 & TRB_BEI ? 'B' : 'b',
			field3 & TRB_IDT ? 'I' : 'i',
			field3 & TRB_IOC ? 'I' : 'i',
			field3 & TRB_CHAIN ? 'C' : 'c',
			field3 & TRB_NO_SNOOP ? 'S' : 's',
			field3 & TRB_ISP ? 'I' : 'i',
			field3 & TRB_ENT ? 'E' : 'e',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_CMD_NOOP:
	case TRB_ENABLE_SLOT:
		snprintf(str, size,
			"%s: flags %c",
			xhci_trb_type_string(type),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_DISABLE_SLOT:
	case TRB_NEG_BANDWIDTH:
		snprintf(str, size,
			"%s: slot %d flags %c",
			xhci_trb_type_string(type),
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_ADDR_DEV:
		snprintf(str, size,
			"%s: ctx %08x%08x slot %d flags %c:%c",
			xhci_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_BSR ? 'B' : 'b',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_CONFIG_EP:
		snprintf(str, size,
			"%s: ctx %08x%08x slot %d flags %c:%c",
			xhci_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_DC ? 'D' : 'd',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_EVAL_CONTEXT:
		snprintf(str, size,
			"%s: ctx %08x%08x slot %d flags %c",
			xhci_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_RESET_EP:
		snprintf(str, size,
			"%s: ctx %08x%08x slot %d ep %d flags %c:%c",
			xhci_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			TRB_TO_EP_ID(field3),
			field3 & TRB_TSP ? 'T' : 't',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_STOP_RING:
		snprintf(str, size,
			"%s: slot %d sp %d ep %d flags %c",
			xhci_trb_type_string(type),
			TRB_TO_SLOT_ID(field3),
			TRB_TO_SUSPEND_PORT(field3),
			TRB_TO_EP_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_SET_DEQ:
		snprintf(str, size,
			"%s: deq %08x%08x stream %d slot %d ep %d flags %c",
			xhci_trb_type_string(type),
			field1, field0,
			TRB_TO_STREAM_ID(field2),
			TRB_TO_SLOT_ID(field3),
			TRB_TO_EP_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_RESET_DEV:
		snprintf(str, size,
			"%s: slot %d flags %c",
			xhci_trb_type_string(type),
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_FORCE_EVENT:
		snprintf(str, size,
			"%s: event %08x%08x vf intr %d vf id %d flags %c",
			xhci_trb_type_string(type),
			field1, field0,
			TRB_TO_VF_INTR_TARGET(field2),
			TRB_TO_VF_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_SET_LT:
		snprintf(str, size,
			"%s: belt %d flags %c",
			xhci_trb_type_string(type),
			TRB_TO_BELT(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_GET_BW:
		snprintf(str, size,
			"%s: ctx %08x%08x slot %d speed %d flags %c",
			xhci_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			TRB_TO_DEV_SPEED(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_FORCE_HEADER:
		snprintf(str, size,
			"%s: info %08x%08x%08x pkt type %d roothub port %d flags %c",
			xhci_trb_type_string(type),
			field2, field1, field0 & 0xffffffe0,
			TRB_TO_PACKET_TYPE(field0),
			TRB_TO_ROOTHUB_PORT(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	default:
		snprintf(str, size,
			"type '%s' -> raw %08x %08x %08x %08x",
			xhci_trb_type_string(type),
			field0, field1, field2, field3);
	}

	return str;
}

static inline const char *xhci_decode_ctrl_ctx(char *str,
		unsigned long drop, unsigned long add)
{
	unsigned int	bit;
	int		ret = 0;

	str[0] = '\0';

	if (drop) {
		ret = sprintf(str, "Drop:");
		for_each_set_bit(bit, &drop, 32)
			ret += sprintf(str + ret, " %d%s",
				       bit / 2,
				       bit % 2 ? "in":"out");
		ret += sprintf(str + ret, ", ");
	}

	if (add) {
		ret += sprintf(str + ret, "Add:%s%s",
			       (add & SLOT_FLAG) ? " slot":"",
			       (add & EP0_FLAG) ? " ep0":"");
		add &= ~(SLOT_FLAG | EP0_FLAG);
		for_each_set_bit(bit, &add, 32)
			ret += sprintf(str + ret, " %d%s",
				       bit / 2,
				       bit % 2 ? "in":"out");
	}
	return str;
}

static inline const char *xhci_decode_slot_context(char *str,
		u32 info, u32 info2, u32 tt_info, u32 state)
{
	u32 speed;
	u32 hub;
	u32 mtt;
	int ret = 0;

	speed = info & DEV_SPEED;
	hub = info & DEV_HUB;
	mtt = info & DEV_MTT;

	ret = sprintf(str, "RS %05x %s%s%s Ctx Entries %d MEL %d us Port# %d/%d",
			info & ROUTE_STRING_MASK,
			({ char *s;
			switch (speed) {
			case SLOT_SPEED_FS:
				s = "full-speed";
				break;
			case SLOT_SPEED_LS:
				s = "low-speed";
				break;
			case SLOT_SPEED_HS:
				s = "high-speed";
				break;
			case SLOT_SPEED_SS:
				s = "super-speed";
				break;
			case SLOT_SPEED_SSP:
				s = "super-speed plus";
				break;
			default:
				s = "UNKNOWN speed";
			} s; }),
			mtt ? " multi-TT" : "",
			hub ? " Hub" : "",
			(info & LAST_CTX_MASK) >> 27,
			info2 & MAX_EXIT,
			DEVINFO_TO_ROOT_HUB_PORT(info2),
			DEVINFO_TO_MAX_PORTS(info2));

	ret += sprintf(str + ret, " [TT Slot %d Port# %d TTT %d Intr %d] Addr %d State %s",
			tt_info & TT_SLOT, (tt_info & TT_PORT) >> 8,
			GET_TT_THINK_TIME(tt_info), GET_INTR_TARGET(tt_info),
			state & DEV_ADDR_MASK,
			xhci_slot_state_string(GET_SLOT_STATE(state)));

	return str;
}


static inline const char *xhci_portsc_link_state_string(u32 portsc)
{
	switch (portsc & PORT_PLS_MASK) {
	case XDEV_U0:
		return "U0";
	case XDEV_U1:
		return "U1";
	case XDEV_U2:
		return "U2";
	case XDEV_U3:
		return "U3";
	case XDEV_DISABLED:
		return "Disabled";
	case XDEV_RXDETECT:
		return "RxDetect";
	case XDEV_INACTIVE:
		return "Inactive";
	case XDEV_POLLING:
		return "Polling";
	case XDEV_RECOVERY:
		return "Recovery";
	case XDEV_HOT_RESET:
		return "Hot Reset";
	case XDEV_COMP_MODE:
		return "Compliance mode";
	case XDEV_TEST_MODE:
		return "Test mode";
	case XDEV_RESUME:
		return "Resume";
	default:
		break;
	}
	return "Unknown";
}

static inline const char *xhci_decode_portsc(char *str, u32 portsc)
{
	int ret;

	ret = sprintf(str, "0x%08x ", portsc);

	if (portsc == ~(u32)0)
		return str;

	ret += sprintf(str + ret, "Speed=%d ", DEV_PORT_SPEED(portsc));
	ret += sprintf(str + ret, "Link=%s ", xhci_portsc_link_state_string(portsc));

	/* RO/ROS: Read-only */
	if (portsc & PORT_CONNECT)
		ret += sprintf(str + ret, "CCS ");
	if (portsc & PORT_OC)
		ret += sprintf(str + ret, "OCA "); /* No set for USB2 ports */
	if (portsc & PORT_CAS)
		ret += sprintf(str + ret, "CAS ");
	if (portsc & PORT_DEV_REMOVE)
		ret += sprintf(str + ret, "DR ");

	/* RWS; writing 1 sets the bit, writing 0 clears the bit. */
	if (portsc & PORT_POWER)
		ret += sprintf(str + ret, "PP ");
	if (portsc & PORT_WKCONN_E)
		ret += sprintf(str + ret, "WCE ");
	if (portsc & PORT_WKDISC_E)
		ret += sprintf(str + ret, "WDE ");
	if (portsc & PORT_WKOC_E)
		ret += sprintf(str + ret, "WOE ");

	/* RW; writing 1 sets the bit, writing 0 clears the bit */
	if (portsc & PORT_LINK_STROBE)
		ret += sprintf(str + ret, "LWS "); /* LWS 0 write is ignored */

	/* RW1S; writing 1 sets the bit, writing 0 has no effect */
	if (portsc & PORT_RESET)
		ret += sprintf(str + ret, "PR ");
	if (portsc & PORT_WR)
		ret += sprintf(str + ret, "WPR "); /* RsvdZ for USB2 ports */

	/* RW1CS; writing 1 clears the bit, writing 0 has no effect. */
	if (portsc & PORT_PE)
		ret += sprintf(str + ret, "PED ");
	if (portsc & PORT_CSC)
		ret += sprintf(str + ret, "CSC ");
	if (portsc & PORT_PEC)
		ret += sprintf(str + ret, "PEC "); /* No set for USB3 ports */
	if (portsc & PORT_WRC)
		ret += sprintf(str + ret, "WRC "); /* RsvdZ for USB2 ports */
	if (portsc & PORT_OCC)
		ret += sprintf(str + ret, "OCC ");
	if (portsc & PORT_RC)
		ret += sprintf(str + ret, "PRC ");
	if (portsc & PORT_PLC)
		ret += sprintf(str + ret, "PLC ");
	if (portsc & PORT_CEC)
		ret += sprintf(str + ret, "CEC "); /* RsvdZ for USB2 ports */

	return str;
}

static inline const char *xhci_decode_usbsts(char *str, u32 usbsts)
{
	int ret = 0;

	ret = sprintf(str, " 0x%08x", usbsts);

	if (usbsts == ~(u32)0)
		return str;

	if (usbsts & STS_HALT)
		ret += sprintf(str + ret, " HCHalted");
	if (usbsts & STS_FATAL)
		ret += sprintf(str + ret, " HSE");
	if (usbsts & STS_EINT)
		ret += sprintf(str + ret, " EINT");
	if (usbsts & STS_PORT)
		ret += sprintf(str + ret, " PCD");
	if (usbsts & STS_SAVE)
		ret += sprintf(str + ret, " SSS");
	if (usbsts & STS_RESTORE)
		ret += sprintf(str + ret, " RSS");
	if (usbsts & STS_SRE)
		ret += sprintf(str + ret, " SRE");
	if (usbsts & STS_CNR)
		ret += sprintf(str + ret, " CNR");
	if (usbsts & STS_HCE)
		ret += sprintf(str + ret, " HCE");

	return str;
}

static inline const char *xhci_decode_doorbell(char *str, u32 slot, u32 doorbell)
{
	u8 ep;
	u16 stream;
	int ret;

	ep = (doorbell & 0xff);
	stream = doorbell >> 16;

	if (slot == 0) {
		sprintf(str, "Command Ring %d", doorbell);
		return str;
	}
	ret = sprintf(str, "Slot %d ", slot);
	if (ep > 0 && ep < 32)
		ret = sprintf(str + ret, "ep%d%s",
			      ep / 2,
			      ep % 2 ? "in" : "out");
	else if (ep == 0 || ep < 248)
		ret = sprintf(str + ret, "Reserved %d", ep);
	else
		ret = sprintf(str + ret, "Vendor Defined %d", ep);
	if (stream)
		ret = sprintf(str + ret, " Stream %d", stream);

	return str;
}

static inline const char *xhci_ep_state_string(u8 state)
{
	switch (state) {
	case EP_STATE_DISABLED:
		return "disabled";
	case EP_STATE_RUNNING:
		return "running";
	case EP_STATE_HALTED:
		return "halted";
	case EP_STATE_STOPPED:
		return "stopped";
	case EP_STATE_ERROR:
		return "error";
	default:
		return "INVALID";
	}
}

static inline const char *xhci_ep_type_string(u8 type)
{
	switch (type) {
	case ISOC_OUT_EP:
		return "Isoc OUT";
	case BULK_OUT_EP:
		return "Bulk OUT";
	case INT_OUT_EP:
		return "Int OUT";
	case CTRL_EP:
		return "Ctrl";
	case ISOC_IN_EP:
		return "Isoc IN";
	case BULK_IN_EP:
		return "Bulk IN";
	case INT_IN_EP:
		return "Int IN";
	default:
		return "INVALID";
	}
}

static inline const char *xhci_decode_ep_context(char *str, u32 info,
		u32 info2, u64 deq, u32 tx_info)
{
	int ret;

	u32 esit;
	u16 maxp;
	u16 avg;

	u8 max_pstr;
	u8 ep_state;
	u8 interval;
	u8 ep_type;
	u8 burst;
	u8 cerr;
	u8 mult;

	bool lsa;
	bool hid;

	esit = CTX_TO_MAX_ESIT_PAYLOAD_HI(info) << 16 |
		CTX_TO_MAX_ESIT_PAYLOAD(tx_info);

	ep_state = info & EP_STATE_MASK;
	max_pstr = CTX_TO_EP_MAXPSTREAMS(info);
	interval = CTX_TO_EP_INTERVAL(info);
	mult = CTX_TO_EP_MULT(info) + 1;
	lsa = !!(info & EP_HAS_LSA);

	cerr = (info2 & (3 << 1)) >> 1;
	ep_type = CTX_TO_EP_TYPE(info2);
	hid = !!(info2 & (1 << 7));
	burst = CTX_TO_MAX_BURST(info2);
	maxp = MAX_PACKET_DECODED(info2);

	avg = EP_AVG_TRB_LENGTH(tx_info);

	ret = sprintf(str, "State %s mult %d max P. Streams %d %s",
			xhci_ep_state_string(ep_state), mult,
			max_pstr, lsa ? "LSA " : "");

	ret += sprintf(str + ret, "interval %d us max ESIT payload %d CErr %d ",
			(1 << interval) * 125, esit, cerr);

	ret += sprintf(str + ret, "Type %s %sburst %d maxp %d deq %016llx ",
			xhci_ep_type_string(ep_type), hid ? "HID" : "",
			burst, maxp, deq);

	ret += sprintf(str + ret, "avg trb len %d", avg);

	return str;
}

#endif /* __LINUX_XHCI_HCD_H */
