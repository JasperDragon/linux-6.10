/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  V4L2 sub-device support header.
 *
 *  Copyright (C) 2008  Hans Verkuil <hverkuil@kernel.org>
 */

//
// ============================================================================
// V4L2 子设备 (Sub-Device) 架构概述
// ============================================================================
//
// 1. 什么是子设备?
//    V4L2 子设备是连接在主桥接芯片 (bridge) 上的外围器件，通常通过 I2C 或
//    SPI 总线进行通信和控制。常见的子设备类型包括:
//      - 图像传感器 (Sensor): 如 OV5640、IMX219 等 CMOS 摄像头传感器
//      - 视频编码器/解码器 (Encoder/Decoder): 如 ADV7180 模拟视频解码器
//      - 音频编解码器 (Audio Codec): 音频信号的采集/播放芯片
//      - 调谐器 (Tuner): AM/FM 收音机或电视调谐器
//      - ISP (Image Signal Processor): 图像信号处理器，处理 RAW 数据
//      - 视频多路复用器 (Mux): 视频信号的路由切换
//      - 红外控制器: 红外遥控信号的接收和发射
//    操作系统通过 v4l2_device (bridge 设备) 统一管理所有连接的子设备。
//
// 2. Ops 分类设计哲学
//    子设备操作按功能领域划分为独立的 ops 结构体，每种功能类别各有一组
//    函数指针。这种设计的核心优势:
//      - 低耦合: 驱动只需实现设备相关的操作类别，无关类别指针设为 NULL
//      - 高内聚: 相关功能聚合在同一结构体中，便于理解和维护
//      - 可扩展: 新增操作类别不会影响已有驱动
//    各操作类别简述:
//      core:  日志、调试、电源、事件 -- 所有子设备的共同基础
//      tuner: 频率、频段、调制器 -- 仅调谐器设备
//      audio: 时钟、路由、音频流 -- 仅音频设备
//      video: 标准、流控 (s_stream 已废弃) -- 视频设备
//      vbi:   VBI 编解码 -- 视频解码器
//      ir:    红外收发 -- 红外设备
//      sensor: 跳过帧/行 -- 图像传感器
//      pad:   格式、路由、per-stream 流控 -- 推荐所有新驱动实现
//
// 3. 子设备与 media_entity 的关系
//    struct v4l2_subdev 内嵌 struct media_entity，将子设备接入媒体控制器
//    (Media Controller) 框架。通过 media_entity_to_v4l2_subdev() 宏
//    可在两者之间安全转换。子设备的 pads (输入/输出端口) 通过
//    media_entity_pads_init() 初始化，子设备间的连接通过 media_link 描述。
//    Userspace 通过 Media Controller API (MEDIA_IOC_ENUM_ENTITIES/LINKS)
//    枚举和配置整个视频管道的拓扑。
//
// 4. 流控制模型: 从 s_stream 到 enable/disable_streams + routing
//    [传统模型 - s_stream]
//    v4l2_subdev_video_ops.s_stream(enable) 对整个子设备进行全局流启停。
//    简单直接但粒度粗糙，无法独立控制每个 pad 上的流。
//    此接口已废弃，新驱动不应使用。
//
//    [新模型 - Multiplexed Streams]
//    v4l2_subdev_pad_ops 中的 enable_streams/disable_streams 配合路由表
//    (routing table) 实现 per-pad per-stream 粒度的精细流控制。
//    子设备通过设置 V4L2_SUBDEV_FL_STREAMS 标志启用此功能。
//    路由表定义 sink pad/stream 到 source pad/stream 的映射，
//    支持 1:N 扇出、N:1 合并和 N:M 等复杂路由拓扑。
//
//    [兼容性层]
//    v4l2_subdev_s_stream_helper() 将 enable/disable_streams 封装为传统
//    s_stream 接口以兼容旧版应用。v4l2_subdev_enable_streams()/
//    disable_streams() 是推荐的流控入口函数，内部自动处理新老模型适配。
//
// 5. 子设备状态管理 (State Management): ACTIVE vs TRY
//    子设备状态由 struct v4l2_subdev_state 管理，包含 per-pad 的格式
//    (v4l2_mbus_framefmt)、裁剪/合成矩形 (v4l2_rect)、帧间隔和路由表。
//    状态操作支持两种模式:
//
//    V4L2_SUBDEV_FORMAT_ACTIVE:
//      通过 v4l2_subdev_init_finalize() 为子设备分配持久的 active_state。
//      驱动在 pad ops 中收到 ACTIVE 请求时，操作实时写入硬件寄存器，
//      同时更新 state 以保持软硬同步。
//      对 active_state 的访问需要通过 state->lock 互斥锁保护。
//
//    V4L2_SUBDEV_FORMAT_TRY:
//      分配临时 state 用于"试配置"操作 (如 S_FMT 之前验证格式是否支持)。
//      不修改硬件状态，仅在内核空间模拟配置效果。
//      通常由 userspace 的 VIDIOC_SUBDEV_S_FMT ioctl 的 TRY 类型触发，
//      用于在不实际更改硬件的前提下获取可行的格式参数。
//
//    状态访问器函数族 (get_format/crop/compose/interval) 支持两种调用:
//      双参数: state + pad (stream 默认为 0，用于非多流驱动)
//      三参数: state + pad + stream (用于多流驱动)
//    当 state 参数为 const 时，返回 const 指针以保证常量安全性。
//
// 6. V4L2_SUBDEV_FL_* 标志位的作用
//    V4L2_SUBDEV_FL_IS_I2C       - 子设备挂载在 I2C 总线上
//    V4L2_SUBDEV_FL_IS_SPI       - 子设备挂载在 SPI 总线上
//    V4L2_SUBDEV_FL_HAS_DEVNODE  - 拥有 /dev/v4l-subdevX 设备节点
//    V4L2_SUBDEV_FL_HAS_EVENTS   - 能产生并上报事件 (如控制值变化)
//    V4L2_SUBDEV_FL_STREAMS      - 支持多路复用流:
//      - 启用集中式 active state 管理
//      - 不支持传统 pad config (state->pads = NULL)
//      - 支持路由 ioctl (VIDIOC_SUBDEV_S_ROUTING)
//      - 每个 pad 可承载多个独立 stream
//
// 7. 本文件结构导览
//    文件按以下顺序组织:
//      - VBI 解码辅助结构 (v4l2_decode_vbi_line)
//      - IO 引脚配置枚举和结构体
//      - 子设备操作集 (core -> tuner -> audio -> video -> vbi -> sensor -> ir)
//      - Pad 级操作集 (v4l2_subdev_pad_ops)
//      - 顶层操作集聚合 (v4l2_subdev_ops, v4l2_subdev_internal_ops)
//      - 子设备标志位 (V4L2_SUBDEV_FL_*)
//      - 子设备主结构体 (v4l2_subdev) 和访问宏
//      - 文件句柄 (v4l2_subdev_fh) 和数据访问器
//      - 媒体控制器相关函数 (链接验证、状态分配、状态访问器)
//      - 路由表管理和多路流控制
//      - 状态锁管理和调用宏 (v4l2_subdev_call 机制)
//      - 辅助函数 (事件通知、流状态查询)
// ============================================================================
//

#ifndef _V4L2_SUBDEV_H
#define _V4L2_SUBDEV_H

#include <linux/types.h>
#include <linux/v4l2-subdev.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mediabus.h>

/* generic v4l2_device notify callback notification values */
#define V4L2_SUBDEV_IR_RX_NOTIFY		_IOW('v', 0, u32)
#define V4L2_SUBDEV_IR_RX_FIFO_SERVICE_REQ	0x00000001
#define V4L2_SUBDEV_IR_RX_END_OF_RX_DETECTED	0x00000002
#define V4L2_SUBDEV_IR_RX_HW_FIFO_OVERRUN	0x00000004
#define V4L2_SUBDEV_IR_RX_SW_FIFO_OVERRUN	0x00000008

#define V4L2_SUBDEV_IR_TX_NOTIFY		_IOW('v', 1, u32)
#define V4L2_SUBDEV_IR_TX_FIFO_SERVICE_REQ	0x00000001

#define	V4L2_DEVICE_NOTIFY_EVENT		_IOW('v', 2, struct v4l2_event)

struct v4l2_device;
struct v4l2_ctrl_handler;
struct v4l2_event;
struct v4l2_event_subscription;
struct v4l2_fh;
struct v4l2_subdev;
struct v4l2_subdev_fh;
struct v4l2_subdev_stream_config;
struct tuner_setup;
struct v4l2_mbus_frame_desc;
struct led_classdev;

//
// ========== VBI 解码辅助结构 ==========
// v4l2_decode_vbi_line 用于从视频解码器中提取和解析
// VBI (Vertical Blanking Interval) 消隐期数据。
// VBI 数据通常包含隐藏字幕 (Closed Caption)、图文电视 (Teletext)、
// VPS (Video Programming System) 等信息。
// 解码器驱动解析原始 VBI 数据流，填充此结构体中的各字段，
// 上层应用据此获取结构化 VBI 内容。
//

/**
 * struct v4l2_decode_vbi_line - used to decode_vbi_line
 *
 * @is_second_field: Set to 0 for the first (odd) field;
 *	set to 1 for the second (even) field.
 * @p: Pointer to the sliced VBI data from the decoder. On exit, points to
 *	the start of the payload.
 * @line: Line number of the sliced VBI data (1-23)
 * @type: VBI service type (V4L2_SLICED_*). 0 if no service found
 */
struct v4l2_decode_vbi_line {
	u32 is_second_field;
	u8 *p;
	u32 line;
	u32 type;
};

/*
 * Sub-devices are devices that are connected somehow to the main bridge
 * device. These devices are usually audio/video muxers/encoders/decoders or
 * sensors and webcam controllers.
 *
 * Usually these devices are controlled through an i2c bus, but other buses
 * may also be used.
 *
 * The v4l2_subdev struct provides a way of accessing these devices in a
 * generic manner. Most operations that these sub-devices support fall in
 * a few categories: core ops, audio ops, video ops and tuner ops.
 *
 * More categories can be added if needed, although this should remain a
 * limited set (no more than approx. 8 categories).
 *
 * Each category has its own set of ops that subdev drivers can implement.
 *
 * A subdev driver can leave the pointer to the category ops NULL if
 * it does not implement them (e.g. an audio subdev will generally not
 * implement the video category ops). The exception is the core category:
 * this must always be present.
 *
 * These ops are all used internally so it is no problem to change, remove
 * or add ops or move ops from one to another category. Currently these
 * ops are based on the original ioctls, but since ops are not limited to
 * one argument there is room for improvement here once all i2c subdev
 * drivers are converted to use these ops.
 */

/*
 * Core ops: it is highly recommended to implement at least these ops:
 *
 * log_status
 * g_register
 * s_register
 *
 * This provides basic debugging support.
 *
 * The ioctl ops is meant for generic ioctl-like commands. Depending on
 * the use-case it might be better to use subdev-specific ops (currently
 * not yet implemented) since ops provide proper type-checking.
 */

//
// ========== IO 引脚配置 ==========
// 以下枚举和结构体用于配置子设备芯片的外部 IO 引脚。
// 某些芯片将多种内部信号复用到有限的物理引脚上，
// 通过 v4l2_subdev_core_ops.s_io_pin_config 进行配置。
// 配置项包括: 引脚方向 (输入/输出)、输出值、驱动强度、
// 有效电平 (高/低有效) 和内部信号路由选择。
//

/**
 * enum v4l2_subdev_io_pin_bits - Subdevice external IO pin configuration
 *	bits
 *
 * @V4L2_SUBDEV_IO_PIN_DISABLE: disables a pin config. ENABLE assumed.
 * @V4L2_SUBDEV_IO_PIN_OUTPUT: set it if pin is an output.
 * @V4L2_SUBDEV_IO_PIN_INPUT: set it if pin is an input.
 * @V4L2_SUBDEV_IO_PIN_SET_VALUE: to set the output value via
 *				  &struct v4l2_subdev_io_pin_config->value.
 * @V4L2_SUBDEV_IO_PIN_ACTIVE_LOW: pin active is bit 0.
 *				   Otherwise, ACTIVE HIGH is assumed.
 */
enum v4l2_subdev_io_pin_bits {
	V4L2_SUBDEV_IO_PIN_DISABLE	= 0,
	V4L2_SUBDEV_IO_PIN_OUTPUT	= 1,
	V4L2_SUBDEV_IO_PIN_INPUT	= 2,
	V4L2_SUBDEV_IO_PIN_SET_VALUE	= 3,
	V4L2_SUBDEV_IO_PIN_ACTIVE_LOW	= 4,
};

/**
 * struct v4l2_subdev_io_pin_config - Subdevice external IO pin configuration
 *
 * @flags: bitmask with flags for this pin's config, whose bits are defined by
 *	   &enum v4l2_subdev_io_pin_bits.
 * @pin: Chip external IO pin to configure
 * @function: Internal signal pad/function to route to IO pin
 * @value: Initial value for pin - e.g. GPIO output value
 * @strength: Pin drive strength
 */
struct v4l2_subdev_io_pin_config {
	u32 flags;
	u8 pin;
	u8 function;
	u8 value;
	u8 strength;
};

//
// 核心操作集: 所有子设备都应实现的最基础操作。
// log_status: 输出设备当前状态到内核日志，是调试的核心入口。
// s_io_pin_config: 配置芯片 IO 引脚功能复用。
// init / reset / load_fw: 初始化/复位/加载固件 (init/reset 已废弃)。
// s_gpio: GPIO 引脚设置。
// command / ioctl: 通用命令和私有 ioctl 处理。
// g_register / s_register: 寄存器调试读写 (需 CONFIG_VIDEO_ADV_DEBUG)。
// s_power: 电源管理 (已废弃，改用 runtime PM)。
// interrupt_service_routine: 中断服务例程 (在 IRQ 上下文中调用!)。
// subscribe_event / unsubscribe_event: 控制事件订阅管理。
// 注意: 新驱动应尽量使用标准化的子操作 (如 pad_ops) 而非 ioctl/cmd。
//

/**
 * struct v4l2_subdev_core_ops - Define core ops callbacks for subdevs
 *
 * @log_status: callback for VIDIOC_LOG_STATUS() ioctl handler code.
 *
 * @s_io_pin_config: configure one or more chip I/O pins for chips that
 *	multiplex different internal signal pads out to IO pins.  This function
 *	takes a pointer to an array of 'n' pin configuration entries, one for
 *	each pin being configured.  This function could be called at times
 *	other than just subdevice initialization.
 *
 * @init: initialize the sensor registers to some sort of reasonable default
 *	values. Do not use for new drivers and should be removed in existing
 *	drivers.
 *
 * @load_fw: load firmware.
 *
 * @reset: generic reset command. The argument selects which subsystems to
 *	reset. Passing 0 will always reset the whole chip. Do not use for new
 *	drivers without discussing this first on the linux-media mailinglist.
 *	There should be no reason normally to reset a device.
 *
 * @s_gpio: set GPIO pins. Very simple right now, might need to be extended with
 *	a direction argument if needed.
 *
 * @command: called by in-kernel drivers in order to call functions internal
 *	   to subdev drivers driver that have a separate callback.
 *
 * @ioctl: called at the end of ioctl() syscall handler at the V4L2 core.
 *	   used to provide support for private ioctls used on the driver.
 *
 * @compat_ioctl32: called when a 32 bits application uses a 64 bits Kernel,
 *		    in order to fix data passed from/to userspace.
 *
 * @g_register: callback for VIDIOC_DBG_G_REGISTER() ioctl handler code.
 *
 * @s_register: callback for VIDIOC_DBG_S_REGISTER() ioctl handler code.
 *
 * @s_power: puts subdevice in power saving mode (on == 0) or normal operation
 *	mode (on == 1). DEPRECATED. See
 *	Documentation/driver-api/media/camera-sensor.rst . pre_streamon and
 *	post_streamoff callbacks can be used for e.g. setting the bus to LP-11
 *	mode before s_stream is called.
 *
 * @interrupt_service_routine: Called by the bridge chip's interrupt service
 *	handler, when an interrupt status has be raised due to this subdev,
 *	so that this subdev can handle the details.  It may schedule work to be
 *	performed later.  It must not sleep. **Called from an IRQ context**.
 *
 * @subscribe_event: used by the drivers to request the control framework that
 *		     for it to be warned when the value of a control changes.
 *
 * @unsubscribe_event: remove event subscription from the control framework.
 */
struct v4l2_subdev_core_ops {
	int (*log_status)(struct v4l2_subdev *sd);
	int (*s_io_pin_config)(struct v4l2_subdev *sd, size_t n,
				      struct v4l2_subdev_io_pin_config *pincfg);
	int (*init)(struct v4l2_subdev *sd, u32 val);
	int (*load_fw)(struct v4l2_subdev *sd);
	int (*reset)(struct v4l2_subdev *sd, u32 val);
	int (*s_gpio)(struct v4l2_subdev *sd, u32 val);
	long (*command)(struct v4l2_subdev *sd, unsigned int cmd, void *arg);
	long (*ioctl)(struct v4l2_subdev *sd, unsigned int cmd, void *arg);
#ifdef CONFIG_COMPAT
	long (*compat_ioctl32)(struct v4l2_subdev *sd, unsigned int cmd,
			       unsigned long arg);
#endif
#ifdef CONFIG_VIDEO_ADV_DEBUG
	int (*g_register)(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg);
	int (*s_register)(struct v4l2_subdev *sd, const struct v4l2_dbg_register *reg);
#endif
	int (*s_power)(struct v4l2_subdev *sd, int on);
	int (*interrupt_service_routine)(struct v4l2_subdev *sd,
						u32 status, bool *handled);
	int (*subscribe_event)(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			       struct v4l2_event_subscription *sub);
	int (*unsubscribe_event)(struct v4l2_subdev *sd, struct v4l2_fh *fh,
				 struct v4l2_event_subscription *sub);
};

//
// 调谐器操作集: 用于 TV 调谐器或 AM/FM 收音机子设备。
// standby: 将调谐器置于待机模式，下次使用时自动唤醒。
// s_radio: 切换到收音机模式 (仅对 TV/Radio 复合设备需要)。
// s_frequency / g_frequency: 设置/获取调谐频率。
// enum_freq_bands: 枚举支持的频段 (如 FM、AM、VHF、UHF)。
// g_tuner / s_tuner: 获取/设置调谐器参数 (如立体声/单声道模式)。
// g_modulator / s_modulator: 获取/设置调制器参数。
// s_type_addr: 设置调谐器类型和 I2C 地址。
// s_config: 配置调谐器特定参数 (如 tda9887 的端口配置)。
// 注意: 当设备同时支持 AM/FM 和 TV 时，驱动必须显式调用 s_radio
// 切换模式后再操作调谐器参数。
//

/**
 * struct v4l2_subdev_tuner_ops - Callbacks used when v4l device was opened
 *	in radio mode.
 *
 * @standby: puts the tuner in standby mode. It will be woken up
 *	     automatically the next time it is used.
 *
 * @s_radio: callback that switches the tuner to radio mode.
 *	     drivers should explicitly call it when a tuner ops should
 *	     operate on radio mode, before being able to handle it.
 *	     Used on devices that have both AM/FM radio receiver and TV.
 *
 * @s_frequency: callback for VIDIOC_S_FREQUENCY() ioctl handler code.
 *
 * @g_frequency: callback for VIDIOC_G_FREQUENCY() ioctl handler code.
 *		 freq->type must be filled in. Normally done by video_ioctl2()
 *		 or the bridge driver.
 *
 * @enum_freq_bands: callback for VIDIOC_ENUM_FREQ_BANDS() ioctl handler code.
 *
 * @g_tuner: callback for VIDIOC_G_TUNER() ioctl handler code.
 *
 * @s_tuner: callback for VIDIOC_S_TUNER() ioctl handler code. @vt->type must be
 *	     filled in. Normally done by video_ioctl2 or the
 *	     bridge driver.
 *
 * @g_modulator: callback for VIDIOC_G_MODULATOR() ioctl handler code.
 *
 * @s_modulator: callback for VIDIOC_S_MODULATOR() ioctl handler code.
 *
 * @s_type_addr: sets tuner type and its I2C addr.
 *
 * @s_config: sets tda9887 specific stuff, like port1, port2 and qss
 *
 * .. note::
 *
 *	On devices that have both AM/FM and TV, it is up to the driver
 *	to explicitly call s_radio when the tuner should be switched to
 *	radio mode, before handling other &struct v4l2_subdev_tuner_ops
 *	that would require it. An example of such usage is::
 *
 *	  static void s_frequency(void *priv, const struct v4l2_frequency *f)
 *	  {
 *		...
 *		if (f.type == V4L2_TUNER_RADIO)
 *			v4l2_device_call_all(v4l2_dev, 0, tuner, s_radio);
 *		...
 *		v4l2_device_call_all(v4l2_dev, 0, tuner, s_frequency);
 *	  }
 */
struct v4l2_subdev_tuner_ops {
	int (*standby)(struct v4l2_subdev *sd);
	int (*s_radio)(struct v4l2_subdev *sd);
	int (*s_frequency)(struct v4l2_subdev *sd, const struct v4l2_frequency *freq);
	int (*g_frequency)(struct v4l2_subdev *sd, struct v4l2_frequency *freq);
	int (*enum_freq_bands)(struct v4l2_subdev *sd, struct v4l2_frequency_band *band);
	int (*g_tuner)(struct v4l2_subdev *sd, struct v4l2_tuner *vt);
	int (*s_tuner)(struct v4l2_subdev *sd, const struct v4l2_tuner *vt);
	int (*g_modulator)(struct v4l2_subdev *sd, struct v4l2_modulator *vm);
	int (*s_modulator)(struct v4l2_subdev *sd, const struct v4l2_modulator *vm);
	int (*s_type_addr)(struct v4l2_subdev *sd, struct tuner_setup *type);
	int (*s_config)(struct v4l2_subdev *sd, const struct v4l2_priv_tun_config *config);
};

//
// 音频操作集: 音频编解码器和具有音频功能的视频解码器使用。
// s_clock_freq: 设置音频主时钟频率 (通常为 48000/44100/32000 Hz)，
//   用于使音频处理器与视频解码器同步。
// s_i2s_clock_freq: 设置 I2S 总线时钟频率 (bps)。
// s_routing: 配置音频输入/输出引脚路由。
// s_stream: 通知音频码流开始/停止传输。
// 通常由音频 Codec 或集成音频功能的视频解码器实现。
//

/**
 * struct v4l2_subdev_audio_ops - Callbacks used for audio-related settings
 *
 * @s_clock_freq: set the frequency (in Hz) of the audio clock output.
 *	Used to slave an audio processor to the video decoder, ensuring that
 *	audio and video remain synchronized. Usual values for the frequency
 *	are 48000, 44100 or 32000 Hz. If the frequency is not supported, then
 *	-EINVAL is returned.
 *
 * @s_i2s_clock_freq: sets I2S speed in bps. This is used to provide a standard
 *	way to select I2S clock used by driving digital audio streams at some
 *	board designs. Usual values for the frequency are 1024000 and 2048000.
 *	If the frequency is not supported, then %-EINVAL is returned.
 *
 * @s_routing: used to define the input and/or output pins of an audio chip,
 *	and any additional configuration data.
 *	Never attempt to use user-level input IDs (e.g. Composite, S-Video,
 *	Tuner) at this level. An i2c device shouldn't know about whether an
 *	input pin is connected to a Composite connector, become on another
 *	board or platform it might be connected to something else entirely.
 *	The calling driver is responsible for mapping a user-level input to
 *	the right pins on the i2c device.
 *
 * @s_stream: used to notify the audio code that stream will start or has
 *	stopped.
 */
struct v4l2_subdev_audio_ops {
	int (*s_clock_freq)(struct v4l2_subdev *sd, u32 freq);
	int (*s_i2s_clock_freq)(struct v4l2_subdev *sd, u32 freq);
	int (*s_routing)(struct v4l2_subdev *sd, u32 input, u32 output, u32 config);
	int (*s_stream)(struct v4l2_subdev *sd, int enable);
};

/**
 * struct v4l2_mbus_frame_desc_entry_csi2
 *
 * @vc: CSI-2 virtual channel
 * @dt: CSI-2 data type ID
 */
struct v4l2_mbus_frame_desc_entry_csi2 {
	u8 vc;
	u8 dt;
};

/**
 * enum v4l2_mbus_frame_desc_flags - media bus frame description flags
 *
 * @V4L2_MBUS_FRAME_DESC_FL_LEN_MAX:
 *	Indicates that &struct v4l2_mbus_frame_desc_entry->length field
 *	specifies maximum data length.
 * @V4L2_MBUS_FRAME_DESC_FL_BLOB:
 *	Indicates that the format does not have line offsets, i.e.
 *	the receiver should use 1D DMA.
 */
enum v4l2_mbus_frame_desc_flags {
	V4L2_MBUS_FRAME_DESC_FL_LEN_MAX	= BIT(0),
	V4L2_MBUS_FRAME_DESC_FL_BLOB	= BIT(1),
};

/**
 * struct v4l2_mbus_frame_desc_entry - media bus frame description structure
 *
 * @flags:	bitmask flags, as defined by &enum v4l2_mbus_frame_desc_flags.
 * @stream:	stream in routing configuration
 * @pixelcode:	media bus pixel code, valid if @flags
 *		%FRAME_DESC_FL_BLOB is not set.
 * @length:	number of octets per frame, valid if @flags
 *		%V4L2_MBUS_FRAME_DESC_FL_LEN_MAX is set.
 * @bus:	Bus-specific frame descriptor parameters
 * @bus.csi2:	CSI-2-specific bus configuration
 */
struct v4l2_mbus_frame_desc_entry {
	enum v4l2_mbus_frame_desc_flags flags;
	u32 stream;
	u32 pixelcode;
	u32 length;
	union {
		struct v4l2_mbus_frame_desc_entry_csi2 csi2;
	} bus;
};

 /*
  * If this number is too small, it should be dropped altogether and the
  * API switched to a dynamic number of frame descriptor entries.
  */
#define V4L2_FRAME_DESC_ENTRY_MAX	8

/**
 * enum v4l2_mbus_frame_desc_type - media bus frame description type
 *
 * @V4L2_MBUS_FRAME_DESC_TYPE_UNDEFINED:
 *	Undefined frame desc type. Drivers should not use this, it is
 *	for backwards compatibility.
 * @V4L2_MBUS_FRAME_DESC_TYPE_PARALLEL:
 *	Parallel media bus.
 * @V4L2_MBUS_FRAME_DESC_TYPE_CSI2:
 *	CSI-2 media bus. Frame desc parameters must be set in
 *	&struct v4l2_mbus_frame_desc_entry->csi2.
 */
enum v4l2_mbus_frame_desc_type {
	V4L2_MBUS_FRAME_DESC_TYPE_UNDEFINED = 0,
	V4L2_MBUS_FRAME_DESC_TYPE_PARALLEL,
	V4L2_MBUS_FRAME_DESC_TYPE_CSI2,
};

/**
 * struct v4l2_mbus_frame_desc - media bus data frame description
 * @type: type of the bus (enum v4l2_mbus_frame_desc_type)
 * @entry: frame descriptors array
 * @num_entries: number of entries in @entry array
 */
struct v4l2_mbus_frame_desc {
	enum v4l2_mbus_frame_desc_type type;
	struct v4l2_mbus_frame_desc_entry entry[V4L2_FRAME_DESC_ENTRY_MAX];
	unsigned short num_entries;
};

/**
 * enum v4l2_subdev_pre_streamon_flags - Flags for pre_streamon subdev core op
 *
 * @V4L2_SUBDEV_PRE_STREAMON_FL_MANUAL_LP: Set the transmitter to either LP-11
 *	or LP-111 mode before call to s_stream().
 */
enum v4l2_subdev_pre_streamon_flags {
	V4L2_SUBDEV_PRE_STREAMON_FL_MANUAL_LP = BIT(0),
};

//
// 视频操作集: 传统视频相关的操作集合。
// s_routing: 视频输入/输出路由选择 (如选择 CVBS 或 S-Video 输入)。
// s_std / g_std / querystd: 模拟视频标准选择/查询 (PAL/NTSC/SECAM)。
// s_stream: 启动/停止数据流传输。注意: 此操作已废弃!
//   新驱动应使用 pad_ops 的 enable_streams/disable_streams 来实现
//   per-pad per-stream 粒度的流控制。
//   s_stream 保留仅用于兼容旧版驱动和 userspace 应用。
// pre_streamon / post_streamoff: 流启动前/停止后的总线配置钩子，
//   用于 CSI-2 发送端设置 LP-11/LP-111 模式等预处理操作。
// s_rx_buffer: 设置由主机分配的接收缓冲区。
// 注意: 对于新驱动，建议尽可能使用 pad_ops，必要时通过
//   v4l2_subdev_s_stream_helper() 提供 s_stream 兼容接口。
//

/**
 * struct v4l2_subdev_video_ops - Callbacks used when v4l device was opened
 *				  in video mode.
 *
 * @s_routing: see s_routing in audio_ops, except this version is for video
 *	devices.
 *
 * @s_crystal_freq: sets the frequency of the crystal used to generate the
 *	clocks in Hz. An extra flags field allows device specific configuration
 *	regarding clock frequency dividers, etc. If not used, then set flags
 *	to 0. If the frequency is not supported, then -EINVAL is returned.
 *
 * @g_std: callback for VIDIOC_G_STD() ioctl handler code.
 *
 * @s_std: callback for VIDIOC_S_STD() ioctl handler code.
 *
 * @s_std_output: set v4l2_std_id for video OUTPUT devices. This is ignored by
 *	video input devices.
 *
 * @g_std_output: get current standard for video OUTPUT devices. This is ignored
 *	by video input devices.
 *
 * @querystd: callback for VIDIOC_QUERYSTD() ioctl handler code.
 *
 * @g_tvnorms: get &v4l2_std_id with all standards supported by the video
 *	CAPTURE device. This is ignored by video output devices.
 *
 * @g_tvnorms_output: get v4l2_std_id with all standards supported by the video
 *	OUTPUT device. This is ignored by video capture devices.
 *
 * @g_input_status: get input status. Same as the status field in the
 *	&struct v4l2_input
 *
 * @s_stream: start (enabled == 1) or stop (enabled == 0) streaming on the
 *	sub-device. Failure on stop will remove any resources acquired in
 *	streaming start, while the error code is still returned by the driver.
 *	The caller shall track the subdev state, and shall not start or stop an
 *	already started or stopped subdev. Also see call_s_stream wrapper in
 *	v4l2-subdev.c.
 *
 *	This callback is DEPRECATED. New drivers should instead implement
 *	&v4l2_subdev_pad_ops.enable_streams and
 *	&v4l2_subdev_pad_ops.disable_streams operations, and use
 *	v4l2_subdev_s_stream_helper for the &v4l2_subdev_video_ops.s_stream
 *	operation to support legacy users.
 *
 *	Drivers should also not call the .s_stream() subdev operation directly,
 *	but use the v4l2_subdev_enable_streams() and
 *	v4l2_subdev_disable_streams() helpers.
 *
 * @s_rx_buffer: set a host allocated memory buffer for the subdev. The subdev
 *	can adjust @size to a lower value and must not write more data to the
 *	buffer starting at @data than the original value of @size.
 *
 * @pre_streamon: May be called before streaming is actually started, to help
 *	initialising the bus. Current usage is to set a CSI-2 transmitter to
 *	LP-11 or LP-111 mode before streaming. See &enum
 *	v4l2_subdev_pre_streamon_flags.
 *
 *	pre_streamon shall return error if it cannot perform the operation as
 *	indicated by the flags argument. In particular, -EACCES indicates lack
 *	of support for the operation. The caller shall call post_streamoff for
 *	each successful call of pre_streamon.
 *
 * @post_streamoff: Called after streaming is stopped, but if and only if
 *	pre_streamon was called earlier.
 */
struct v4l2_subdev_video_ops {
	int (*s_routing)(struct v4l2_subdev *sd, u32 input, u32 output, u32 config);
	int (*s_crystal_freq)(struct v4l2_subdev *sd, u32 freq, u32 flags);
	int (*g_std)(struct v4l2_subdev *sd, v4l2_std_id *norm);
	int (*s_std)(struct v4l2_subdev *sd, v4l2_std_id norm);
	int (*s_std_output)(struct v4l2_subdev *sd, v4l2_std_id std);
	int (*g_std_output)(struct v4l2_subdev *sd, v4l2_std_id *std);
	int (*querystd)(struct v4l2_subdev *sd, v4l2_std_id *std);
	int (*g_tvnorms)(struct v4l2_subdev *sd, v4l2_std_id *std);
	int (*g_tvnorms_output)(struct v4l2_subdev *sd, v4l2_std_id *std);
	int (*g_input_status)(struct v4l2_subdev *sd, u32 *status);
	int (*s_stream)(struct v4l2_subdev *sd, int enable);
	int (*s_rx_buffer)(struct v4l2_subdev *sd, void *buf,
			   unsigned int *size);
	int (*pre_streamon)(struct v4l2_subdev *sd, u32 flags);
	int (*post_streamoff)(struct v4l2_subdev *sd);
};

//
// VBI 操作集: 处理视频消隐期 (Vertical Blanking Interval) 数据的提取和注入。
// VBI 是模拟视频信号中场消隐期间传输的数据区域，通常包含:
//   - 隐藏字幕 (Closed Caption, CC)
//   - 图文电视 (Teletext)
//   - 视频节目系统 (VPS)
//   - 宽屏信令 (WSS)
// decode_vbi_line: 从原始 VBI 数据中解析出切片格式的 VBI 数据包。
// s_vbi_data / g_vbi_data: 设置/获取 VBI 数据。
// g_sliced_vbi_cap: 查询设备支持的切片 VBI 服务类型。
// s_sliced_fmt / g_sliced_fmt: 配置/查询切片 VBI 格式。
// 通常由模拟视频解码器驱动实现。
//

/**
 * struct v4l2_subdev_vbi_ops - Callbacks used when v4l device was opened
 *				  in video mode via the vbi device node.
 *
 *  @decode_vbi_line: video decoders that support sliced VBI need to implement
 *	this ioctl. Field p of the &struct v4l2_decode_vbi_line is set to the
 *	start of the VBI data that was generated by the decoder. The driver
 *	then parses the sliced VBI data and sets the other fields in the
 *	struct accordingly. The pointer p is updated to point to the start of
 *	the payload which can be copied verbatim into the data field of the
 *	&struct v4l2_sliced_vbi_data. If no valid VBI data was found, then the
 *	type field is set to 0 on return.
 *
 * @s_vbi_data: used to generate VBI signals on a video signal.
 *	&struct v4l2_sliced_vbi_data is filled with the data packets that
 *	should be output. Note that if you set the line field to 0, then that
 *	VBI signal is disabled. If no valid VBI data was found, then the type
 *	field is set to 0 on return.
 *
 * @g_vbi_data: used to obtain the sliced VBI packet from a readback register.
 *	Not all video decoders support this. If no data is available because
 *	the readback register contains invalid or erroneous data %-EIO is
 *	returned. Note that you must fill in the 'id' member and the 'field'
 *	member (to determine whether CC data from the first or second field
 *	should be obtained).
 *
 * @g_sliced_vbi_cap: callback for VIDIOC_G_SLICED_VBI_CAP() ioctl handler
 *		      code.
 *
 * @s_raw_fmt: setup the video encoder/decoder for raw VBI.
 *
 * @g_sliced_fmt: retrieve the current sliced VBI settings.
 *
 * @s_sliced_fmt: setup the sliced VBI settings.
 */
struct v4l2_subdev_vbi_ops {
	int (*decode_vbi_line)(struct v4l2_subdev *sd, struct v4l2_decode_vbi_line *vbi_line);
	int (*s_vbi_data)(struct v4l2_subdev *sd, const struct v4l2_sliced_vbi_data *vbi_data);
	int (*g_vbi_data)(struct v4l2_subdev *sd, struct v4l2_sliced_vbi_data *vbi_data);
	int (*g_sliced_vbi_cap)(struct v4l2_subdev *sd, struct v4l2_sliced_vbi_cap *cap);
	int (*s_raw_fmt)(struct v4l2_subdev *sd, struct v4l2_vbi_format *fmt);
	int (*g_sliced_fmt)(struct v4l2_subdev *sd, struct v4l2_sliced_vbi_format *fmt);
	int (*s_sliced_fmt)(struct v4l2_subdev *sd, struct v4l2_sliced_vbi_format *fmt);
};

//
// 传感器操作集: 图像传感器特有的辅助查询操作。
// g_skip_top_lines: 返回图像顶部需要跳过的行数。某些传感器在输出图像
//   的顶部几行会包含损坏的数据或元数据，应用层需跳过这些行。
// g_skip_frames: 返回流启动时需要跳过的帧数。某些传感器在上电或
//   切换格式后的最初几帧可能不稳定或有缺陷，驱动需丢弃这些帧。
// 这两个操作帮助 bridge 驱动补偿传感器硬件的不完美行为。
//

/**
 * struct v4l2_subdev_sensor_ops - v4l2-subdev sensor operations
 * @g_skip_top_lines: number of lines at the top of the image to be skipped.
 *		      This is needed for some sensors, which always corrupt
 *		      several top lines of the output image, or which send their
 *		      metadata in them.
 * @g_skip_frames: number of frames to skip at stream start. This is needed for
 *		   buggy sensors that generate faulty frames when they are
 *		   turned on.
 */
struct v4l2_subdev_sensor_ops {
	int (*g_skip_top_lines)(struct v4l2_subdev *sd, u32 *lines);
	int (*g_skip_frames)(struct v4l2_subdev *sd, u32 *frames);
};

/**
 * enum v4l2_subdev_ir_mode- describes the type of IR supported
 *
 * @V4L2_SUBDEV_IR_MODE_PULSE_WIDTH: IR uses struct ir_raw_event records
 */
enum v4l2_subdev_ir_mode {
	V4L2_SUBDEV_IR_MODE_PULSE_WIDTH,
};

/**
 * struct v4l2_subdev_ir_parameters - Parameters for IR TX or TX
 *
 * @bytes_per_data_element: bytes per data element of data in read or
 *	write call.
 * @mode: IR mode as defined by &enum v4l2_subdev_ir_mode.
 * @enable: device is active if true
 * @interrupt_enable: IR interrupts are enabled if true
 * @shutdown: if true: set hardware to low/no power, false: normal mode
 *
 * @modulation: if true, it uses carrier, if false: baseband
 * @max_pulse_width:  maximum pulse width in ns, valid only for baseband signal
 * @carrier_freq: carrier frequency in Hz, valid only for modulated signal
 * @duty_cycle: duty cycle percentage, valid only for modulated signal
 * @invert_level: invert signal level
 *
 * @invert_carrier_sense: Send 0/space as a carrier burst. used only in TX.
 *
 * @noise_filter_min_width: min time of a valid pulse, in ns. Used only for RX.
 * @carrier_range_lower: Lower carrier range, in Hz, valid only for modulated
 *	signal. Used only for RX.
 * @carrier_range_upper: Upper carrier range, in Hz, valid only for modulated
 *	signal. Used only for RX.
 * @resolution: The receive resolution, in ns . Used only for RX.
 */
struct v4l2_subdev_ir_parameters {
	unsigned int bytes_per_data_element;
	enum v4l2_subdev_ir_mode mode;

	bool enable;
	bool interrupt_enable;
	bool shutdown;

	bool modulation;
	u32 max_pulse_width;
	unsigned int carrier_freq;
	unsigned int duty_cycle;
	bool invert_level;

	/* Tx only */
	bool invert_carrier_sense;

	/* Rx only */
	u32 noise_filter_min_width;
	unsigned int carrier_range_lower;
	unsigned int carrier_range_upper;
	u32 resolution;
};

//
// 红外操作集: 红外遥控信号的接收和发射。包括脉冲宽度编码数据的读写、
// 接收/发射参数配置 (载波频率、占空比、噪声滤波器带宽、分辨率等)。
// 通常由红外接收器/发射器子设备驱动实现。
// 接收和发射分别有独立的参数集合和读写接口。
//

/**
 * struct v4l2_subdev_ir_ops - operations for IR subdevices
 *
 * @rx_read: Reads received codes or pulse width data.
 *	The semantics are similar to a non-blocking read() call.
 * @rx_g_parameters: Get the current operating parameters and state of
 *	the IR receiver.
 * @rx_s_parameters: Set the current operating parameters and state of
 *	the IR receiver.  It is recommended to call
 *	[rt]x_g_parameters first to fill out the current state, and only change
 *	the fields that need to be changed.  Upon return, the actual device
 *	operating parameters and state will be returned.  Note that hardware
 *	limitations may prevent the actual settings from matching the requested
 *	settings - e.g. an actual carrier setting of 35,904 Hz when 36,000 Hz
 *	was requested.  An exception is when the shutdown parameter is true.
 *	The last used operational parameters will be returned, but the actual
 *	state of the hardware be different to minimize power consumption and
 *	processing when shutdown is true.
 *
 * @tx_write: Writes codes or pulse width data for transmission.
 *	The semantics are similar to a non-blocking write() call.
 * @tx_g_parameters: Get the current operating parameters and state of
 *	the IR transmitter.
 * @tx_s_parameters: Set the current operating parameters and state of
 *	the IR transmitter.  It is recommended to call
 *	[rt]x_g_parameters first to fill out the current state, and only change
 *	the fields that need to be changed.  Upon return, the actual device
 *	operating parameters and state will be returned.  Note that hardware
 *	limitations may prevent the actual settings from matching the requested
 *	settings - e.g. an actual carrier setting of 35,904 Hz when 36,000 Hz
 *	was requested.  An exception is when the shutdown parameter is true.
 *	The last used operational parameters will be returned, but the actual
 *	state of the hardware be different to minimize power consumption and
 *	processing when shutdown is true.
 */
struct v4l2_subdev_ir_ops {
	/* Receiver */
	int (*rx_read)(struct v4l2_subdev *sd, u8 *buf, size_t count,
				ssize_t *num);

	int (*rx_g_parameters)(struct v4l2_subdev *sd,
				struct v4l2_subdev_ir_parameters *params);
	int (*rx_s_parameters)(struct v4l2_subdev *sd,
				struct v4l2_subdev_ir_parameters *params);

	/* Transmitter */
	int (*tx_write)(struct v4l2_subdev *sd, u8 *buf, size_t count,
				ssize_t *num);

	int (*tx_g_parameters)(struct v4l2_subdev *sd,
				struct v4l2_subdev_ir_parameters *params);
	int (*tx_s_parameters)(struct v4l2_subdev *sd,
				struct v4l2_subdev_ir_parameters *params);
};

//
// ========== 子设备状态相关数据结构 ==========
// 以下四个结构体构成子设备状态管理的层次化数据模型:
//
// v4l2_subdev_pad_config:
//   单 pad 的配置数据，包含格式 (v4l2_mbus_framefmt)、裁剪矩形 (crop)、
//   合成矩形 (compose) 和帧间隔 (fract)。state->pads[] 是该结构体的数组。
//
// v4l2_subdev_stream_configs:
//   流配置集合 (仅 V4L2_SUBDEV_FL_STREAMS 启用时使用)。
//   包含 num_configs 个 v4l2_subdev_stream_config 条目。
//
// v4l2_subdev_krouting:
//   路由表，定义 sink pad/stream 到 source pad/stream 的映射。
//   包含 routes 数组、num_routes (有效条目数) 和 len_routes (容量)。
//
// v4l2_subdev_state:
//   子设备的完整运行时状态，整合了 pads 配置数组、路由表和流配置。
//   所有 pad ops 通过 state 参数访问当前配置。
//   ACTIVE 模式: state 指向 sd->active_state，操作实时写硬件。
//   TRY 模式: state 指向临时分配的 state，仅用于验证。
//

/**
 * struct v4l2_subdev_pad_config - Used for storing subdev pad information.
 *
 * @format: &struct v4l2_mbus_framefmt
 * @crop: &struct v4l2_rect to be used for crop
 * @compose: &struct v4l2_rect to be used for compose
 * @interval: frame interval
 */
struct v4l2_subdev_pad_config {
	struct v4l2_mbus_framefmt format;   // 此 pad 上的媒体总线帧格式 (分辨率、像素编码、色彩空间)
	struct v4l2_rect crop;              // 裁剪矩形：从传感器全幅中选取感兴趣区域 (ROI)
	struct v4l2_rect compose;           // 组合矩形：裁剪后缩放/平移输出到目标区域
	struct v4l2_fract interval;         // 帧间隔：两帧之间的时间 (numerator/denominator)
};

/**
 * struct v4l2_subdev_stream_configs - A collection of stream configs.
 *
 * @num_configs: number of entries in @config.
 * @configs: an array of &struct v4l2_subdev_stream_configs.
 */
struct v4l2_subdev_stream_configs {
	u32 num_configs;
	struct v4l2_subdev_stream_config *configs;
};

/**
 * struct v4l2_subdev_krouting - subdev routing table
 *
 * @len_routes: length of routes array, in routes
 * @num_routes: number of routes
 * @routes: &struct v4l2_subdev_route
 *
 * This structure contains the routing table for a subdev.
 */
struct v4l2_subdev_krouting {
	unsigned int len_routes;           // routes 数组容量 (分配的元素数量)
	unsigned int num_routes;           // 实际有效的路由条目数 (≤ len_routes)
	struct v4l2_subdev_route *routes;  // 路由表数组，每条路由定义 pad/stream 对的连接
};

/**
 * struct v4l2_subdev_state - Used for storing subdev state information.
 *
 * @_lock: default for 'lock'
 * @lock: mutex for the state. May be replaced by the user.
 * @sd: the sub-device which the state is related to
 * @pads: &struct v4l2_subdev_pad_config array
 * @routing: routing table for the subdev
 * @stream_configs: stream configurations (only for V4L2_SUBDEV_FL_STREAMS)
 *
 * This structure only needs to be passed to the pad op if the 'which' field
 * of the main argument is set to %V4L2_SUBDEV_FORMAT_TRY. For
 * %V4L2_SUBDEV_FORMAT_ACTIVE it is safe to pass %NULL.
 */
struct v4l2_subdev_state {
	/* lock for the struct v4l2_subdev_state fields */
	struct mutex _lock;                             // 默认锁实例 (当 lock 指向别处时作为后备)
	struct mutex *lock;                             // 当前使用的锁指针 (可通过 v4l2_subdev_state_lock 替换)
	struct v4l2_subdev *sd;                         // 拥有此 state 的子设备
	struct v4l2_subdev_pad_config *pads;            // 传统 per-pad 配置数组 (streams 模式下为 NULL)
	struct v4l2_subdev_krouting routing;            // 路由表：定义子设备内部 pad/stream 的连接关系
	struct v4l2_subdev_stream_configs stream_configs; // 多流配置 (仅 V4L2_SUBDEV_FL_STREAMS 启用)
};

//
// ========== Pad 级操作集 (v4l2_subdev_pad_ops) ==========
// 这是 V4L2 子设备框架中最核心、最复杂的操作集合，每个操作都关联到
// 特定的 pad (输入/输出端口)，支持 per-pad 的精细配置。主要包括:
//
// [格式查询与配置]
//   enum_mbus_code: 枚举 pad 支持的媒体总线像素格式
//   enum_frame_size: 枚举 pad 支持的帧尺寸
//   get_fmt / set_fmt: 获取/设置 pad 上某 stream 的格式
//
// [裁剪与合成]
//   get_selection / set_selection: 获取/设置裁剪 (crop) 和合成 (compose) 矩形
//   crop 定义从传感器读取的区域，compose 定义输出到总线的区域
//
// [显示时序]
//   s_dv_timings / g_dv_timings: 设置/获取数字视频时序 (如 HDMI)
//   query_dv_timings: 检测当前输入信号的时序
//   dv_timings_cap / enum_dv_timings: 查询支持的时序能力
//
// [EDID 管理]
//   get_edid / set_edid: 获取/设置 EDID 数据 (显示识别数据)
//
// [总线配置]
//   get_mbus_config: 获取远程子设备的媒体总线配置 (如 CSI-2 通道数)
//   get_frame_desc / set_frame_desc: 获取/设置底层帧描述 (CSI-2 VC/DT)
//
// [链接验证]
//   link_validate: 验证 media link 两端的格式是否匹配
//
// ========== 路由和多路流 API 详解 ==========
// 以下操作仅当子设备设置了 V4L2_SUBDEV_FL_STREAMS 标志时启用:
//
// set_routing:
//   设置子设备内部路由表，定义 sink pad/stream 到 source pad/stream
//   的映射关系。支持 1:N (扇出)、N:1 (合并)、N:M 等多种拓扑。
//   路由表包含多个 v4l2_subdev_route 条目，每个条目描述一条
//   单向数据路径。通过 v4l2_subdev_set_routing() 写入 state。
//
// enable_streams / disable_streams:
//   替代已废弃的 s_stream()，实现 per-pad per-stream 粒度的流控制。
//   streams_mask 是 u64 位掩码，每个 bit 代表一个 stream ID。
//   调用流程:
//     1. v4l2_subdev_enable_streams() (核心包装函数)
//     2. -> 遍历 streams_mask 中的每个 stream
//     3. -> 调用驱动实现的 enable_streams op
//     4. -> 更新 enabled_pads 状态跟踪
//   disable_streams 是逆操作，流程类似。
//
// 流翻译与验证:
//   v4l2_subdev_state_xlate_streams():
//     根据路由表将一端 pad 的 stream 位掩码翻译到另一端。
//     对 pipeline 中的流传播至关重要。
//   v4l2_subdev_routing_validate():
//     使用 routing_restriction 枚举验证路由表是否满足驱动约束。
//     约束包括: 禁止 1:N 扇出、禁止 N:1 合并、禁止流混合等。
//
// 传统兼容性:
//   v4l2_subdev_s_stream_helper() 将 enable/disable_streams 封装为
//   传统 s_stream 接口，使新版驱动仍可与旧版 userspace 应用兼容。
//   此辅助函数仅适用于具有单个 source pad 的子设备。
//
// 帧描述符透传:
//   v4l2_subdev_get_frame_desc_passthrough() 为简单透传子设备提供
//   get_frame_desc 的默认实现，自动遍历路由表收集上游帧描述信息。
//

/**
 * struct v4l2_subdev_pad_ops - v4l2-subdev pad level operations
 *
 * @enum_mbus_code: callback for VIDIOC_SUBDEV_ENUM_MBUS_CODE() ioctl handler
 *		    code.
 * @enum_frame_size: callback for VIDIOC_SUBDEV_ENUM_FRAME_SIZE() ioctl handler
 *		     code.
 *
 * @enum_frame_interval: callback for VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL() ioctl
 *			 handler code.
 *
 * @get_fmt: callback for VIDIOC_SUBDEV_G_FMT() ioctl handler code.
 *
 * @set_fmt: callback for VIDIOC_SUBDEV_S_FMT() ioctl handler code.
 *
 * @get_selection: callback for VIDIOC_SUBDEV_G_SELECTION() ioctl handler code.
 *
 * @set_selection: callback for VIDIOC_SUBDEV_S_SELECTION() ioctl handler code.
 *
 * @get_frame_interval: callback for VIDIOC_SUBDEV_G_FRAME_INTERVAL()
 *			ioctl handler code.
 *
 * @set_frame_interval: callback for VIDIOC_SUBDEV_S_FRAME_INTERVAL()
 *			ioctl handler code.
 *
 * @get_edid: callback for VIDIOC_SUBDEV_G_EDID() ioctl handler code.
 *
 * @set_edid: callback for VIDIOC_SUBDEV_S_EDID() ioctl handler code.
 *
 * @s_dv_timings: Set custom dv timings in the sub device. This is used
 *	when sub device is capable of setting detailed timing information
 *	in the hardware to generate/detect the video signal.
 *
 * @g_dv_timings: Get custom dv timings in the sub device.
 *
 * @query_dv_timings: callback for VIDIOC_QUERY_DV_TIMINGS() ioctl handler code.
 *
 * @dv_timings_cap: callback for VIDIOC_SUBDEV_DV_TIMINGS_CAP() ioctl handler
 *		    code.
 *
 * @enum_dv_timings: callback for VIDIOC_SUBDEV_ENUM_DV_TIMINGS() ioctl handler
 *		     code.
 *
 * @link_validate: used by the media controller code to check if the links
 *		   that belongs to a pipeline can be used for stream.
 *
 * @get_frame_desc: get the current low level media bus frame parameters.
 *
 * @set_frame_desc: set the low level media bus frame parameters, @fd array
 *                  may be adjusted by the subdev driver to device capabilities.
 *
 * @get_mbus_config: get the media bus configuration of a remote sub-device.
 *		     The media bus configuration is usually retrieved from the
 *		     firmware interface at sub-device probe time, immediately
 *		     applied to the hardware and eventually adjusted by the
 *		     driver. Remote sub-devices (usually video receivers) shall
 *		     use this operation to query the transmitting end bus
 *		     configuration in order to adjust their own one accordingly.
 *		     Callers should make sure they get the most up-to-date as
 *		     possible configuration from the remote end, likely calling
 *		     this operation as close as possible to stream on time. The
 *		     operation shall fail if the pad index it has been called on
 *		     is not valid or in case of unrecoverable failures. The
 *		     config argument has been memset to 0 just before calling
 *		     the op.
 *
 * @set_routing: Enable or disable data connection routes described in the
 *		 subdevice routing table. Subdevs that implement this operation
 *		 must set the V4L2_SUBDEV_FL_STREAMS flag.
 *
 * @enable_streams: Enable the streams defined in streams_mask on the given
 *	source pad. Subdevs that implement this operation must use the active
 *	state management provided by the subdev core (enabled through a call to
 *	v4l2_subdev_init_finalize() at initialization time). Do not call
 *	directly, use v4l2_subdev_enable_streams() instead.
 *
 *	Drivers that support only a single stream without setting the
 *	V4L2_SUBDEV_CAP_STREAMS sub-device capability flag can ignore the mask
 *	argument.
 *
 * @disable_streams: Disable the streams defined in streams_mask on the given
 *	source pad. Subdevs that implement this operation must use the active
 *	state management provided by the subdev core (enabled through a call to
 *	v4l2_subdev_init_finalize() at initialization time). Do not call
 *	directly, use v4l2_subdev_disable_streams() instead.
 *
 *	Drivers that support only a single stream without setting the
 *	V4L2_SUBDEV_CAP_STREAMS sub-device capability flag can ignore the mask
 *	argument.
 */
struct v4l2_subdev_pad_ops {
	int (*enum_mbus_code)(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_mbus_code_enum *code);
	int (*enum_frame_size)(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_frame_size_enum *fse);
	int (*enum_frame_interval)(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_interval_enum *fie);
	int (*get_fmt)(struct v4l2_subdev *sd,
		       struct v4l2_subdev_state *state,
		       struct v4l2_subdev_format *format);
	int (*set_fmt)(struct v4l2_subdev *sd,
		       struct v4l2_subdev_state *state,
		       struct v4l2_subdev_format *format);
	int (*get_selection)(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_selection *sel);
	int (*set_selection)(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_selection *sel);
	int (*get_frame_interval)(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_interval *interval);
	int (*set_frame_interval)(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_interval *interval);
	int (*get_edid)(struct v4l2_subdev *sd, struct v4l2_edid *edid);
	int (*set_edid)(struct v4l2_subdev *sd, struct v4l2_edid *edid);
	int (*s_dv_timings)(struct v4l2_subdev *sd, unsigned int pad,
			    struct v4l2_dv_timings *timings);
	int (*g_dv_timings)(struct v4l2_subdev *sd, unsigned int pad,
			    struct v4l2_dv_timings *timings);
	int (*query_dv_timings)(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_dv_timings *timings);
	int (*dv_timings_cap)(struct v4l2_subdev *sd,
			      struct v4l2_dv_timings_cap *cap);
	int (*enum_dv_timings)(struct v4l2_subdev *sd,
			       struct v4l2_enum_dv_timings *timings);
#ifdef CONFIG_MEDIA_CONTROLLER
	int (*link_validate)(struct v4l2_subdev *sd, struct media_link *link,
			     struct v4l2_subdev_format *source_fmt,
			     struct v4l2_subdev_format *sink_fmt);
#endif /* CONFIG_MEDIA_CONTROLLER */
	int (*get_frame_desc)(struct v4l2_subdev *sd, unsigned int pad,
			      struct v4l2_mbus_frame_desc *fd);
	int (*set_frame_desc)(struct v4l2_subdev *sd, unsigned int pad,
			      struct v4l2_mbus_frame_desc *fd);
	int (*get_mbus_config)(struct v4l2_subdev *sd, unsigned int pad,
			       struct v4l2_mbus_config *config);
	int (*set_routing)(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   enum v4l2_subdev_format_whence which,
			   struct v4l2_subdev_krouting *route);
	int (*enable_streams)(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state, u32 pad,
			      u64 streams_mask);
	int (*disable_streams)(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state, u32 pad,
			       u64 streams_mask);
};

//
// ========== 子设备操作集聚合 (v4l2_subdev_ops) ==========
// v4l2_subdev_ops 是顶层操作集，聚合所有功能类别的操作指针。
// 每个子设备驱动只需实现其设备相关的操作类别:
//   core  - 必选 (日志、调试、电源管理、事件订阅)
//   tuner - 仅调谐器设备
//   audio - 仅音频编解码器
//   video - 传统视频流控 (s_stream 已废弃，建议用 pad ops)
//   vbi   - 仅视频解码器需要 VBI 数据
//   ir    - 仅红外遥控设备
//   sensor- 仅图像传感器
//   pad   - 推荐实现，提供 per-pad 精细控制
//
// 未实现的类别指针设为 NULL 即可。核心通过 v4l2_subdev_call 宏
// 安全地检查 NULL 指针，返回 -ENOIOCTLCMD。
//

/**
 * struct v4l2_subdev_ops - Subdev operations
 *
 * @core: pointer to &struct v4l2_subdev_core_ops. Can be %NULL
 * @tuner: pointer to &struct v4l2_subdev_tuner_ops. Can be %NULL
 * @audio: pointer to &struct v4l2_subdev_audio_ops. Can be %NULL
 * @video: pointer to &struct v4l2_subdev_video_ops. Can be %NULL
 * @vbi: pointer to &struct v4l2_subdev_vbi_ops. Can be %NULL
 * @ir: pointer to &struct v4l2_subdev_ir_ops. Can be %NULL
 * @sensor: pointer to &struct v4l2_subdev_sensor_ops. Can be %NULL
 * @pad: pointer to &struct v4l2_subdev_pad_ops. Can be %NULL
 */
struct v4l2_subdev_ops {
	const struct v4l2_subdev_core_ops	*core;
	const struct v4l2_subdev_tuner_ops	*tuner;
	const struct v4l2_subdev_audio_ops	*audio;
	const struct v4l2_subdev_video_ops	*video;
	const struct v4l2_subdev_vbi_ops	*vbi;
	const struct v4l2_subdev_ir_ops		*ir;
	const struct v4l2_subdev_sensor_ops	*sensor;
	const struct v4l2_subdev_pad_ops	*pad;
};

//
// ========== 子设备内部操作集 (v4l2_subdev_internal_ops) ==========
// 内部操作由 V4L2 框架自身调用，驱动开发人员不应直接调用:
//   init_state:   初始化子设备状态为默认值
//   registered:   子设备注册到 v4l2_device 后调用
//   unregistered: 子设备从 v4l2_device 注销前调用
//   open:         /dev/v4l-subdevX 设备节点被打开时调用
//   close:        设备节点关闭时调用 (可能在 unregistered 之后!)
//   release:      最后一个引用释放后调用，通常用于释放子设备内存
//                 若设置了 V4L2_SUBDEV_FL_HAS_DEVNODE 标志，
//                 则几乎必须实现此回调。
//
// 警告: 这些 ops 仅框架可调用，驱动永远不要直接调用它们!
//

/**
 * struct v4l2_subdev_internal_ops - V4L2 subdev internal ops
 *
 * @init_state: initialize the subdev state to default values
 *
 * @registered: called when this subdev is registered. When called the v4l2_dev
 *	field is set to the correct v4l2_device.
 *
 * @unregistered: called when this subdev is unregistered. When called the
 *	v4l2_dev field is still set to the correct v4l2_device.
 *
 * @open: called when the subdev device node is opened by an application.
 *
 * @close: called when the subdev device node is closed. Please note that
 *	it is possible for @close to be called after @unregistered!
 *
 * @release: called when the last user of the subdev device is gone. This
 *	happens after the @unregistered callback and when the last open
 *	filehandle to the v4l-subdevX device node was closed. If no device
 *	node was created for this sub-device, then the @release callback
 *	is called right after the @unregistered callback.
 *	The @release callback is typically used to free the memory containing
 *	the v4l2_subdev structure. It is almost certainly required for any
 *	sub-device that sets the V4L2_SUBDEV_FL_HAS_DEVNODE flag.
 *
 * .. note::
 *	Never call this from drivers, only the v4l2 framework can call
 *	these ops.
 */
struct v4l2_subdev_internal_ops {
	int (*init_state)(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state);
	int (*registered)(struct v4l2_subdev *sd);
	void (*unregistered)(struct v4l2_subdev *sd);
	int (*open)(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh);
	int (*close)(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh);
	void (*release)(struct v4l2_subdev *sd);
};

//
// ========== V4L2_SUBDEV_FL_* 子设备标志位 ==========
// 每个子设备通过 flags 字段声明自身能力和特性:
//   V4L2_SUBDEV_FL_IS_I2C      - 挂载在 I2C 总线上
//   V4L2_SUBDEV_FL_IS_SPI      - 挂载在 SPI 总线上
//   V4L2_SUBDEV_FL_HAS_DEVNODE - 拥有独立的 /dev/v4l-subdevX 设备节点
//   V4L2_SUBDEV_FL_HAS_EVENTS  - 能产生并上报事件 (如控制值变化)
//   V4L2_SUBDEV_FL_STREAMS     - 支持多路复用流:
//                                - 启用集中式 active state 管理
//                                - 不支持传统 pad config (state->pads = NULL)
//                                - 支持路由 ioctl (set_routing)
//                                - 每个 pad 可承载多个独立流
//                                启用此标志后，驱动必须实现 set_routing、
//                                enable_streams、disable_streams 等操作。
//

/* Set this flag if this subdev is a i2c device. */
#define V4L2_SUBDEV_FL_IS_I2C			(1U << 0)
/* Set this flag if this subdev is a spi device. */
#define V4L2_SUBDEV_FL_IS_SPI			(1U << 1)
/* Set this flag if this subdev needs a device node. */
#define V4L2_SUBDEV_FL_HAS_DEVNODE		(1U << 2)
/*
 * Set this flag if this subdev generates events.
 * Note controls can send events, thus drivers exposing controls
 * should set this flag.
 */
#define V4L2_SUBDEV_FL_HAS_EVENTS		(1U << 3)
/*
 * Set this flag if this subdev supports multiplexed streams. This means
 * that the driver supports routing and handles the stream parameter in its
 * v4l2_subdev_pad_ops handlers. More specifically, this means:
 *
 * - Centrally managed subdev active state is enabled
 * - Legacy pad config is _not_ supported (state->pads is NULL)
 * - Routing ioctls are available
 * - Multiple streams per pad are supported
 */
#define V4L2_SUBDEV_FL_STREAMS			(1U << 4)

struct regulator_bulk_data;

/**
 * struct v4l2_subdev_platform_data - regulators config struct
 *
 * @regulators: Optional regulators used to power on/off the subdevice
 * @num_regulators: Number of regululators
 * @host_priv: Per-subdevice data, specific for a certain video host device
 */
struct v4l2_subdev_platform_data {
	struct regulator_bulk_data *regulators;
	int num_regulators;

	void *host_priv;
};

//
// ========== 子设备主结构体 (struct v4l2_subdev) ==========
// 这是 V4L2 子设备框架的核心数据结构，每个子设备驱动创建一个实例。
// 关键字段说明:
//   entity:      内嵌的 media_entity，将子设备接入 media controller 框架，
//                通过 media_entity_to_v4l2_subdev() 可双向转换
//   ops:         指向 v4l2_subdev_ops，定义所有可调用的子设备操作
//   internal_ops:框架内部操作 (注册/注销/打开/关闭回调)
//   ctrl_handler:控制处理器，管理子设备的 V4L2 控制 (如曝光、增益)
//   active_state:集中管理的运行时 state (格式、裁剪、路由表等)
//                通过 v4l2_subdev_init_finalize() 初始化
//   fwnode:      固件节点句柄 (DT/ACPI)，用于设备树匹配和 async 绑定
//   enabled_pads:跟踪已启用流的 pad，供 enable/disable_streams 使用
//
// 初始化流程:
//   1. 分配 v4l2_subdev 实例 (独立或嵌入私有的设备结构体)
//   2. 调用 v4l2_subdev_init() 或 v4l2_i2c_subdev_init() 初始化基本字段
//   3. 调用 media_entity_pads_init() 初始化 pads
//   4. (可选) 调用 v4l2_subdev_init_finalize() 分配 active_state
//   5. 调用 v4l2_device_register_subdev() 注册到 bridge 设备
//

/**
 * struct v4l2_subdev - describes a V4L2 sub-device
 *
 * @entity: pointer to &struct media_entity
 * @list: List of sub-devices
 * @owner: The owner is the same as the driver's &struct device owner.
 * @owner_v4l2_dev: true if the &sd->owner matches the owner of @v4l2_dev->dev
 *	owner. Initialized by v4l2_device_register_subdev().
 * @flags: subdev flags. Can be:
 *   %V4L2_SUBDEV_FL_IS_I2C - Set this flag if this subdev is a i2c device;
 *   %V4L2_SUBDEV_FL_IS_SPI - Set this flag if this subdev is a spi device;
 *   %V4L2_SUBDEV_FL_HAS_DEVNODE - Set this flag if this subdev needs a
 *   device node;
 *   %V4L2_SUBDEV_FL_HAS_EVENTS -  Set this flag if this subdev generates
 *   events.
 *
 * @v4l2_dev: pointer to struct &v4l2_device
 * @ops: pointer to struct &v4l2_subdev_ops
 * @internal_ops: pointer to struct &v4l2_subdev_internal_ops.
 *	Never call these internal ops from within a driver!
 * @ctrl_handler: The control handler of this subdev. May be NULL.
 * @name: Name of the sub-device. Please notice that the name must be unique.
 * @grp_id: can be used to group similar subdevs. Value is driver-specific
 * @dev_priv: pointer to private data
 * @host_priv: pointer to private data used by the device where the subdev
 *	is attached.
 * @devnode: subdev device node
 * @dev: pointer to the physical device, if any
 * @fwnode: The fwnode_handle of the subdev, usually the same as
 *	    either dev->of_node->fwnode or dev->fwnode (whichever is non-NULL).
 * @async_list: Links this subdev to a global subdev_list or
 *		@notifier->done_list list.
 * @async_subdev_endpoint_list: List entry in async_subdev_endpoint_entry of
 *				&struct v4l2_async_subdev_endpoint.
 * @subdev_notifier: A sub-device notifier implicitly registered for the sub-
 *		     device using v4l2_async_register_subdev_sensor().
 * @asc_list: Async connection list, of &struct
 *	      v4l2_async_connection.subdev_entry.
 * @pdata: common part of subdevice platform data
 * @state_lock: A pointer to a lock used for all the subdev's states, set by the
 *		driver. This is	optional. If NULL, each state instance will get
 *		a lock of its own.
 * @privacy_led: Optional pointer to a LED classdev for the privacy LED for sensors.
 * @active_state: Active state for the subdev (NULL for subdevs tracking the
 *		  state internally). Initialized by calling
 *		  v4l2_subdev_init_finalize().
 * @enabled_pads: Bitmask of enabled pads used by v4l2_subdev_enable_streams()
 *		  and v4l2_subdev_disable_streams() helper functions for
 *		  fallback cases.
 * @s_stream_enabled: Tracks whether streaming has been enabled with s_stream.
 *                    This is only for call_s_stream() internal use.
 *
 * Each instance of a subdev driver should create this struct, either
 * stand-alone or embedded in a larger struct.
 *
 * This structure should be initialized by v4l2_subdev_init() or one of
 * its variants: v4l2_spi_subdev_init(), v4l2_i2c_subdev_init().
 */
struct v4l2_subdev {
#if defined(CONFIG_MEDIA_CONTROLLER)
	struct media_entity entity; // 内嵌媒体实体：将子设备挂入 Media Controller 拓扑图
#endif
	struct list_head list;       // v4l2_device->subdevs 链表节点
	struct module *owner;        // 驱动模块 owner，防止子设备使用期间模块被卸载
	bool owner_v4l2_dev;         // owner 是否与 v4l2_device->dev->driver->owner 相同
	u32 flags;                   // V4L2_SUBDEV_FL_* 标志位：IS_I2C, HAS_DEVNODE, STREAMS ...
	struct v4l2_device *v4l2_dev;// 父 V4L2 设备指针，注册后设置，注销后置 NULL
	const struct v4l2_subdev_ops *ops;         // 功能操作集：core/video/audio/tuner/pad ...
	const struct v4l2_subdev_internal_ops *internal_ops; // 框架内部操作：registered/unregistered/release
	struct v4l2_ctrl_handler *ctrl_handler;    // 控制处理器：管理此子设备的 V4L2 控制项
	char name[52];               // 全局唯一名称，用于 debug 和 sysfs
	u32 grp_id;                  // 分组 ID：bridge 驱动用于批量操作同类子设备
	void *dev_priv;              // 驱动私有数据 (通过 v4l2_set/get_subdevdata 存取)
	void *host_priv;             // 主机私有数据：bridge 驱动通过 pdata->host_priv 传入
	struct video_device *devnode;// 关联的 /dev/v4l-subdevX 设备节点 (如有 HAS_DEVNODE 标志)
	struct device *dev;          // 物理设备指针 (如 &i2c_client->dev)
	struct fwnode_handle *fwnode;// 固件节点：DT/ACPI 中的 endpoint 节点，用于 async 匹配
	struct list_head async_list;              // async 全局子设备列表或 notifier->done 列表节点
	struct list_head async_subdev_endpoint_list; // v4l2_async_subdev_endpoint 链表节点
	struct v4l2_async_notifier *subdev_notifier; // 隐式 notifier (sensor 驱动自动创建)
	struct list_head asc_list;               // Async connection 链表 (v4l2_async_connection.subdev_entry)
	struct v4l2_subdev_platform_data *pdata; // 平台数据：regulators 和 host_priv
	struct mutex *state_lock;                // state 互斥锁指针 (可选，NULL 时各 state 自有锁)

	/*
	 * The fields below are private, and should only be accessed via
	 * appropriate functions.
	 */

	struct led_classdev *privacy_led; // 隐私 LED：sensor 激活时自动点亮 LED

	/*
	 * TODO: active_state should most likely be changed from a pointer to an
	 * embedded field. For the time being it's kept as a pointer to more
	 * easily catch uses of active_state in the cases where the driver
	 * doesn't support it.
	 */
	struct v4l2_subdev_state *active_state; // 集中管理的活跃 state (format/crop/routes)
	                                         // 由 v4l2_subdev_init_finalize() 分配
	u64 enabled_pads;        // 已启用流的 pad 位掩码：enable/disable_streams 回退逻辑使用
	bool s_stream_enabled;   // s_stream() 旧 API 的流状态跟踪，仅 call_s_stream 内部使用
};


/**
 * media_entity_to_v4l2_subdev - Returns a &struct v4l2_subdev from
 *    the &struct media_entity embedded in it.
 *
 * @ent: pointer to &struct media_entity.
 */
#define media_entity_to_v4l2_subdev(ent)				\
({									\
	typeof(ent) __me_sd_ent = (ent);				\
									\
	__me_sd_ent ?							\
		container_of_const(__me_sd_ent, struct v4l2_subdev, entity) : \
		NULL;							\
})

/**
 * vdev_to_v4l2_subdev - Returns a &struct v4l2_subdev from
 *	the &struct video_device embedded on it.
 *
 * @vdev: pointer to &struct video_device
 */
#define vdev_to_v4l2_subdev(vdev) \
	((struct v4l2_subdev *)video_get_drvdata(vdev))

//
// ========== 子设备文件句柄 (struct v4l2_subdev_fh) ==========
// 每个打开 /dev/v4l-subdevX 的文件描述符关联一个 v4l2_subdev_fh 实例，
// 其中包含:
//   vfh:        V4L2 文件句柄基类
//   state:      关联的子设备 TRY state (用于 VIDIOC_SUBDEV_S_FMT 等 TRY 操作)
//   owner:      打开此句柄的内核模块指针
//   client_caps:客户端能力标志 (如 V4L2_SUBDEV_CLIENT_CAP_STREAMS)
//
// 通过 to_v4l2_subdev_fh(fh) 宏从 v4l2_fh 转换得到。
// v4l2_subdev_fops 提供了标准的 subdev 文件操作函数表。
//

/**
 * struct v4l2_subdev_fh - Used for storing subdev information per file handle
 *
 * @vfh: pointer to &struct v4l2_fh
 * @state: pointer to &struct v4l2_subdev_state
 * @owner: module pointer to the owner of this file handle
 * @client_caps: bitmask of ``V4L2_SUBDEV_CLIENT_CAP_*``
 */
struct v4l2_subdev_fh {
	struct v4l2_fh vfh;
	struct module *owner;
#if defined(CONFIG_VIDEO_V4L2_SUBDEV_API)
	struct v4l2_subdev_state *state;
	u64 client_caps;
#endif
};

/**
 * to_v4l2_subdev_fh - Returns a &struct v4l2_subdev_fh from
 *	the &struct v4l2_fh embedded on it.
 *
 * @fh: pointer to &struct v4l2_fh
 */
#define to_v4l2_subdev_fh(fh)	\
	container_of(fh, struct v4l2_subdev_fh, vfh)

extern const struct v4l2_file_operations v4l2_subdev_fops;

/**
 * v4l2_set_subdevdata - Sets V4L2 dev private device data
 *
 * @sd: pointer to &struct v4l2_subdev
 * @p: pointer to the private device data to be stored.
 */
static inline void v4l2_set_subdevdata(struct v4l2_subdev *sd, void *p)
{
	sd->dev_priv = p;
}

/**
 * v4l2_get_subdevdata - Gets V4L2 dev private device data
 *
 * @sd: pointer to &struct v4l2_subdev
 *
 * Returns the pointer to the private device data to be stored.
 */
static inline void *v4l2_get_subdevdata(const struct v4l2_subdev *sd)
{
	return sd->dev_priv;
}

/**
 * v4l2_set_subdev_hostdata - Sets V4L2 dev private host data
 *
 * @sd: pointer to &struct v4l2_subdev
 * @p: pointer to the private data to be stored.
 */
static inline void v4l2_set_subdev_hostdata(struct v4l2_subdev *sd, void *p)
{
	sd->host_priv = p;
}

/**
 * v4l2_get_subdev_hostdata - Gets V4L2 dev private data
 *
 * @sd: pointer to &struct v4l2_subdev
 *
 * Returns the pointer to the private host data to be stored.
 */
static inline void *v4l2_get_subdev_hostdata(const struct v4l2_subdev *sd)
{
	return sd->host_priv;
}

#ifdef CONFIG_MEDIA_CONTROLLER

/**
 * v4l2_subdev_get_fwnode_pad_1_to_1 - Get pad number from a subdev fwnode
 *                                     endpoint, assuming 1:1 port:pad
 *
 * @entity: Pointer to the subdev entity
 * @endpoint: Pointer to a parsed fwnode endpoint
 *
 * This function can be used as the .get_fwnode_pad operation for
 * subdevices that map port numbers and pad indexes 1:1. If the endpoint
 * is owned by the subdevice, the function returns the endpoint port
 * number.
 *
 * Returns the endpoint port number on success or a negative error code.
 */
int v4l2_subdev_get_fwnode_pad_1_to_1(struct media_entity *entity,
				      struct fwnode_endpoint *endpoint);

/**
 * v4l2_subdev_link_validate_default - validates a media link
 *
 * @sd: pointer to &struct v4l2_subdev
 * @link: pointer to &struct media_link
 * @source_fmt: pointer to &struct v4l2_subdev_format
 * @sink_fmt: pointer to &struct v4l2_subdev_format
 *
 * This function ensures that width, height and the media bus pixel
 * code are equal on both source and sink of the link.
 */
int v4l2_subdev_link_validate_default(struct v4l2_subdev *sd,
				      struct media_link *link,
				      struct v4l2_subdev_format *source_fmt,
				      struct v4l2_subdev_format *sink_fmt);

/**
 * v4l2_subdev_link_validate - validates a media link
 *
 * @link: pointer to &struct media_link
 *
 * This function calls the subdev's link_validate ops to validate
 * if a media link is valid for streaming. It also internally
 * calls v4l2_subdev_link_validate_default() to ensure that
 * width, height and the media bus pixel code are equal on both
 * source and sink of the link.
 *
 * The function can be used as a drop-in &media_entity_ops.link_validate
 * implementation for v4l2_subdev instances. It supports all links between
 * subdevs, as well as links between subdevs and video devices, provided that
 * the video devices also implement their &media_entity_ops.link_validate
 * operation.
 */
int v4l2_subdev_link_validate(struct media_link *link);

/**
 * v4l2_subdev_has_pad_interdep - MC has_pad_interdep implementation for subdevs
 *
 * @entity: pointer to &struct media_entity
 * @pad0: pad number for the first pad
 * @pad1: pad number for the second pad
 *
 * This function is an implementation of the
 * media_entity_operations.has_pad_interdep operation for subdevs that
 * implement the multiplexed streams API (as indicated by the
 * V4L2_SUBDEV_FL_STREAMS subdev flag).
 *
 * It considers two pads interdependent if there is an active route between pad0
 * and pad1.
 */
bool v4l2_subdev_has_pad_interdep(struct media_entity *entity,
				  unsigned int pad0, unsigned int pad1);

//
// ========== 子设备状态分配与初始化 ==========
// 以下函数管理子设备状态 (v4l2_subdev_state) 的生命周期:
//   __v4l2_subdev_state_alloc: 分配新的 state，包含 pads 数组和路由表
//   __v4l2_subdev_state_free: 释放 state 及其动态分配的内部数据
//   v4l2_subdev_init_finalize: 驱动初始化完成后调用，分配 active_state
//     此宏内建 lock_class_key，确保锁验证 (lockdep) 正常工作
//   v4l2_subdev_cleanup: 释放与子设备关联的所有资源 (async 连接、state 等)
//
// 注意: __v4l2_subdev_state_alloc/free 是内部函数，驱动不应直接调用。
// 驱动应使用 v4l2_subdev_init_finalize() 和 v4l2_subdev_cleanup()。
//

/**
 * __v4l2_subdev_state_alloc - allocate v4l2_subdev_state
 *
 * @sd: pointer to &struct v4l2_subdev for which the state is being allocated.
 * @lock_name: name of the state lock
 * @key: lock_class_key for the lock
 *
 * Must call __v4l2_subdev_state_free() when state is no longer needed.
 *
 * Not to be called directly by the drivers.
 */
struct v4l2_subdev_state *__v4l2_subdev_state_alloc(struct v4l2_subdev *sd,
						    const char *lock_name,
						    struct lock_class_key *key);

/**
 * __v4l2_subdev_state_free - free a v4l2_subdev_state
 *
 * @state: v4l2_subdev_state to be freed.
 *
 * Not to be called directly by the drivers.
 */
void __v4l2_subdev_state_free(struct v4l2_subdev_state *state);

/**
 * v4l2_subdev_init_finalize() - Finalizes the initialization of the subdevice
 * @sd: The subdev
 *
 * This function finalizes the initialization of the subdev, including
 * allocation of the active state for the subdev.
 *
 * This function must be called by the subdev drivers that use the centralized
 * active state, after the subdev struct has been initialized and
 * media_entity_pads_init() has been called, but before registering the
 * subdev.
 *
 * The user must call v4l2_subdev_cleanup() when the subdev is being removed.
 */
#define v4l2_subdev_init_finalize(sd)                                          \
	({                                                                     \
		static struct lock_class_key __key;                            \
		const char *name = KBUILD_BASENAME                             \
			":" __stringify(__LINE__) ":sd->active_state->lock";   \
		__v4l2_subdev_init_finalize(sd, name, &__key);                 \
	})

int __v4l2_subdev_init_finalize(struct v4l2_subdev *sd, const char *name,
				struct lock_class_key *key);

/**
 * v4l2_subdev_cleanup() - Releases the resources allocated by the subdevice
 * @sd: The subdevice
 *
 * Clean up a V4L2 async sub-device. Must be called for a sub-device as part of
 * its release if resources have been associated with it using
 * v4l2_async_subdev_endpoint_add() or v4l2_subdev_init_finalize().
 */
void v4l2_subdev_cleanup(struct v4l2_subdev *sd);

/*
 * A macro to generate the macro or function name for sub-devices state access
 * wrapper macros below.
 */
#define __v4l2_subdev_state_gen_call(NAME, _1, ARG, ...)	\
	__v4l2_subdev_state_get_ ## NAME ## ARG

/*
 * A macro to constify the return value of the state accessors when the state
 * parameter is const.
 */
#define __v4l2_subdev_state_constify_ret(state, value)				\
	_Generic(state,								\
		const struct v4l2_subdev_state *: (const typeof(*(value)) *)(value), \
		struct v4l2_subdev_state *: (value)				\
	)

/**
//
// ========== 子设备状态访问器 ==========
// 以下宏和函数提供对子设备 state 中 per-pad per-stream 配置的访问:
//   v4l2_subdev_state_get_format:  获取 pad/stream 的媒体总线格式
//   v4l2_subdev_state_get_crop:    获取裁剪矩形
//   v4l2_subdev_state_get_compose: 获取合成矩形
//   v4l2_subdev_state_get_interval:获取帧间隔
//
// 每个访问器支持两种调用方式:
//   - 双参数: state + pad, stream 默认为 0 (用于非多流驱动)
//   - 三参数: state + pad + stream (用于多流驱动)
// 这是通过 __v4l2_subdev_state_gen_call() 宏重载实现的。
// 当 state 参数为 const 时，返回 const 指针，保证常量安全性。
//
// 对于不支持多路流的传统驱动，stream 参数始终为 0。
// 若指定 pad 不存在，所有访问器返回 NULL。
//

/**
 * v4l2_subdev_state_get_format() - Get pointer to a stream format
 * @state: subdevice state
 * @pad: pad id
 * @...: stream id (optional argument)
 *
 * This returns a pointer to &struct v4l2_mbus_framefmt for the given pad +
 * stream in the subdev state.
 *
 * For stream-unaware drivers the format for the corresponding pad is returned.
 * If the pad does not exist, NULL is returned.
 */
/*
 * Wrap v4l2_subdev_state_get_format(), allowing the function to be called with
 * two or three arguments. The purpose of the __v4l2_subdev_state_gen_call()
 * macro is to come up with the name of the function or macro to call, using
 * the last two arguments (_stream and _pad). The selected function or macro is
 * then called using the arguments specified by the caller. The
 * __v4l2_subdev_state_constify_ret() macro constifies the returned pointer
 * when the state is const, allowing the state accessors to guarantee
 * const-correctness in all cases.
 *
 * A similar arrangement is used for v4l2_subdev_state_crop(),
 * v4l2_subdev_state_compose() and v4l2_subdev_state_get_interval() below.
 */
#define v4l2_subdev_state_get_format(state, pad, ...)				\
	__v4l2_subdev_state_constify_ret(state,					\
		__v4l2_subdev_state_gen_call(format, ##__VA_ARGS__, , _pad)	\
			((struct v4l2_subdev_state *)state, pad, ##__VA_ARGS__))
#define __v4l2_subdev_state_get_format_pad(state, pad)	\
	__v4l2_subdev_state_get_format(state, pad, 0)
struct v4l2_mbus_framefmt *
__v4l2_subdev_state_get_format(struct v4l2_subdev_state *state,
			       unsigned int pad, u32 stream);

/**
 * v4l2_subdev_state_get_crop() - Get pointer to a stream crop rectangle
 * @state: subdevice state
 * @pad: pad id
 * @...: stream id (optional argument)
 *
 * This returns a pointer to crop rectangle for the given pad + stream in the
 * subdev state.
 *
 * For stream-unaware drivers the crop rectangle for the corresponding pad is
 * returned. If the pad does not exist, NULL is returned.
 */
#define v4l2_subdev_state_get_crop(state, pad, ...)				\
	__v4l2_subdev_state_constify_ret(state,					\
		__v4l2_subdev_state_gen_call(crop, ##__VA_ARGS__, , _pad)	\
			((struct v4l2_subdev_state *)state, pad, ##__VA_ARGS__))
#define __v4l2_subdev_state_get_crop_pad(state, pad)	\
	__v4l2_subdev_state_get_crop(state, pad, 0)
struct v4l2_rect *
__v4l2_subdev_state_get_crop(struct v4l2_subdev_state *state, unsigned int pad,
			     u32 stream);

/**
 * v4l2_subdev_state_get_compose() - Get pointer to a stream compose rectangle
 * @state: subdevice state
 * @pad: pad id
 * @...: stream id (optional argument)
 *
 * This returns a pointer to compose rectangle for the given pad + stream in the
 * subdev state.
 *
 * For stream-unaware drivers the compose rectangle for the corresponding pad is
 * returned. If the pad does not exist, NULL is returned.
 */
#define v4l2_subdev_state_get_compose(state, pad, ...)				\
	__v4l2_subdev_state_constify_ret(state,					\
		__v4l2_subdev_state_gen_call(compose, ##__VA_ARGS__, , _pad)	\
			((struct v4l2_subdev_state *)state, pad, ##__VA_ARGS__))
#define __v4l2_subdev_state_get_compose_pad(state, pad)	\
	__v4l2_subdev_state_get_compose(state, pad, 0)
struct v4l2_rect *
__v4l2_subdev_state_get_compose(struct v4l2_subdev_state *state,
				unsigned int pad, u32 stream);

/**
 * v4l2_subdev_state_get_interval() - Get pointer to a stream frame interval
 * @state: subdevice state
 * @pad: pad id
 * @...: stream id (optional argument)
 *
 * This returns a pointer to the frame interval for the given pad + stream in
 * the subdev state.
 *
 * For stream-unaware drivers the frame interval for the corresponding pad is
 * returned. If the pad does not exist, NULL is returned.
 */
#define v4l2_subdev_state_get_interval(state, pad, ...)				\
	__v4l2_subdev_state_constify_ret(state,					\
		__v4l2_subdev_state_gen_call(interval, ##__VA_ARGS__, , _pad)	\
			((struct v4l2_subdev_state *)state, pad, ##__VA_ARGS__))
#define __v4l2_subdev_state_get_interval_pad(state, pad)	\
	__v4l2_subdev_state_get_interval(state, pad, 0)
struct v4l2_fract *
__v4l2_subdev_state_get_interval(struct v4l2_subdev_state *state,
				 unsigned int pad, u32 stream);

#if defined(CONFIG_VIDEO_V4L2_SUBDEV_API)

/**
 * v4l2_subdev_get_fmt() - Fill format based on state
 * @sd: subdevice
 * @state: subdevice state
 * @format: pointer to &struct v4l2_subdev_format
 *
 * Fill @format->format field based on the information in the @format struct.
 *
 * This function can be used by the subdev drivers which support active state to
 * implement v4l2_subdev_pad_ops.get_fmt if the subdev driver does not need to
 * do anything special in their get_fmt op.
 *
 * Returns 0 on success, error value otherwise.
 */
int v4l2_subdev_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			struct v4l2_subdev_format *format);

/**
 * v4l2_subdev_get_frame_interval() - Fill frame interval based on state
 * @sd: subdevice
 * @state: subdevice state
 * @fi: pointer to &struct v4l2_subdev_frame_interval
 *
 * Fill @fi->interval field based on the information in the @fi struct.
 *
 * This function can be used by the subdev drivers which support active state to
 * implement v4l2_subdev_pad_ops.get_frame_interval if the subdev driver does
 * not need to do anything special in their get_frame_interval op.
 *
 * Returns 0 on success, error value otherwise.
 */
int v4l2_subdev_get_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_interval *fi);

//
// ========== 子设备路由表管理 ==========
// 路由表 (routing table) 是多路流 (Multiplexed Streams) 的核心数据结构，
// 定义 sink pad/stream 到 source pad/stream 的映射关系。
// 仅在子设备设置了 V4L2_SUBDEV_FL_STREAMS 标志时启用。
//
// 路由 API 主要函数:
//   v4l2_subdev_set_routing: 将路由表写入 state，自动释放旧表
//   v4l2_subdev_set_routing_with_fmt: 设置路由的同时用指定格式初始化所有流
//   for_each_active_route: 遍历路由表中所有激活 (enabled) 的 route
//   v4l2_subdev_routing_find_opposite_end: 查找 route 另一端的 pad/stream
//   v4l2_subdev_state_get_opposite_stream_format: 获取对端流格式
//   v4l2_subdev_state_xlate_streams: 将一端 stream 位掩码翻译到另一端
//   v4l2_subdev_routing_validate: 验证路由表是否满足驱动约束
//
// 路由约束 (routing_restriction):
//   驱动通过 v4l2_subdev_routing_validate() 和 routing_restriction 枚举
//   限制允许的路由拓扑，如 1:N 扇出限制、N:1 合并限制、流混合限制等。
//

/**
 * v4l2_subdev_set_routing() - Set given routing to subdev state
 * @sd: The subdevice
 * @state: The subdevice state
 * @routing: Routing that will be copied to subdev state
 *
 * This will release old routing table (if any) from the state, allocate
 * enough space for the given routing, and copy the routing.
 *
 * This can be used from the subdev driver's set_routing op, after validating
 * the routing.
 */
int v4l2_subdev_set_routing(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    const struct v4l2_subdev_krouting *routing);

struct v4l2_subdev_route *
__v4l2_subdev_next_active_route(const struct v4l2_subdev_krouting *routing,
				struct v4l2_subdev_route *route);

/**
 * for_each_active_route - iterate on all active routes of a routing table
 * @routing: The routing table
 * @route: The route iterator
 */
#define for_each_active_route(routing, route) \
	for ((route) = NULL;                  \
	     ((route) = __v4l2_subdev_next_active_route((routing), (route)));)

/**
 * v4l2_subdev_set_routing_with_fmt() - Set given routing and format to subdev
 *					state
 * @sd: The subdevice
 * @state: The subdevice state
 * @routing: Routing that will be copied to subdev state
 * @fmt: Format used to initialize all the streams
 *
 * This is the same as v4l2_subdev_set_routing, but additionally initializes
 * all the streams using the given format.
 */
int v4l2_subdev_set_routing_with_fmt(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     const struct v4l2_subdev_krouting *routing,
				     const struct v4l2_mbus_framefmt *fmt);

/**
 * v4l2_subdev_routing_find_opposite_end() - Find the opposite stream
 * @routing: routing used to find the opposite side
 * @pad: pad id
 * @stream: stream id
 * @other_pad: pointer used to return the opposite pad
 * @other_stream: pointer used to return the opposite stream
 *
 * This function uses the routing table to find the pad + stream which is
 * opposite the given pad + stream.
 *
 * @other_pad and/or @other_stream can be NULL if the caller does not need the
 * value.
 *
 * Returns 0 on success, or -EINVAL if no matching route is found.
 */
int v4l2_subdev_routing_find_opposite_end(const struct v4l2_subdev_krouting *routing,
					  u32 pad, u32 stream, u32 *other_pad,
					  u32 *other_stream);

/**
 * v4l2_subdev_state_get_opposite_stream_format() - Get pointer to opposite
 *                                                  stream format
 * @state: subdevice state
 * @pad: pad id
 * @stream: stream id
 *
 * This returns a pointer to &struct v4l2_mbus_framefmt for the pad + stream
 * that is opposite the given pad + stream in the subdev state.
 *
 * If the state does not contain the given pad + stream, NULL is returned.
 */
struct v4l2_mbus_framefmt *
v4l2_subdev_state_get_opposite_stream_format(struct v4l2_subdev_state *state,
					     u32 pad, u32 stream);

/**
 * v4l2_subdev_state_xlate_streams() - Translate streams from one pad to another
 *
 * @state: Subdevice state
 * @pad0: The first pad
 * @pad1: The second pad
 * @streams: Streams bitmask on the first pad
 *
 * Streams on sink pads of a subdev are routed to source pads as expressed in
 * the subdev state routing table. Stream numbers don't necessarily match on
 * the sink and source side of a route. This function translates stream numbers
 * on @pad0, expressed as a bitmask in @streams, to the corresponding streams
 * on @pad1 using the routing table from the @state. It returns the stream mask
 * on @pad1, and updates @streams with the streams that have been found in the
 * routing table.
 *
 * @pad0 and @pad1 must be a sink and a source, in any order.
 *
 * Return: The bitmask of streams of @pad1 that are routed to @streams on @pad0.
 */
u64 v4l2_subdev_state_xlate_streams(const struct v4l2_subdev_state *state,
				    u32 pad0, u32 pad1, u64 *streams);

/**
 * enum v4l2_subdev_routing_restriction - Subdevice internal routing restrictions
 *
 * @V4L2_SUBDEV_ROUTING_NO_1_TO_N:
 *	an input stream shall not be routed to multiple output streams (stream
 *	duplication)
 * @V4L2_SUBDEV_ROUTING_NO_N_TO_1:
 *	multiple input streams shall not be routed to the same output stream
 *	(stream merging)
 * @V4L2_SUBDEV_ROUTING_NO_SINK_STREAM_MIX:
 *	all streams from a sink pad must be routed to a single source pad
 * @V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX:
 *	all streams on a source pad must originate from a single sink pad
 * @V4L2_SUBDEV_ROUTING_NO_SOURCE_MULTIPLEXING:
 *	source pads shall not contain multiplexed streams
 * @V4L2_SUBDEV_ROUTING_NO_SINK_MULTIPLEXING:
 *	sink pads shall not contain multiplexed streams
 * @V4L2_SUBDEV_ROUTING_ONLY_1_TO_1:
 *	only non-overlapping 1-to-1 stream routing is allowed (a combination of
 *	@V4L2_SUBDEV_ROUTING_NO_1_TO_N and @V4L2_SUBDEV_ROUTING_NO_N_TO_1)
 * @V4L2_SUBDEV_ROUTING_NO_STREAM_MIX:
 *	all streams from a sink pad must be routed to a single source pad, and
 *	that source pad shall not get routes from any other sink pad
 *	(a combination of @V4L2_SUBDEV_ROUTING_NO_SINK_STREAM_MIX and
 *	@V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX)
 * @V4L2_SUBDEV_ROUTING_NO_MULTIPLEXING:
 *	no multiplexed streams allowed on either source or sink sides.
 */
enum v4l2_subdev_routing_restriction {
	V4L2_SUBDEV_ROUTING_NO_1_TO_N = BIT(0),
	V4L2_SUBDEV_ROUTING_NO_N_TO_1 = BIT(1),
	V4L2_SUBDEV_ROUTING_NO_SINK_STREAM_MIX = BIT(2),
	V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX = BIT(3),
	V4L2_SUBDEV_ROUTING_NO_SINK_MULTIPLEXING = BIT(4),
	V4L2_SUBDEV_ROUTING_NO_SOURCE_MULTIPLEXING = BIT(5),
	V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 =
		V4L2_SUBDEV_ROUTING_NO_1_TO_N |
		V4L2_SUBDEV_ROUTING_NO_N_TO_1,
	V4L2_SUBDEV_ROUTING_NO_STREAM_MIX =
		V4L2_SUBDEV_ROUTING_NO_SINK_STREAM_MIX |
		V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX,
	V4L2_SUBDEV_ROUTING_NO_MULTIPLEXING =
		V4L2_SUBDEV_ROUTING_NO_SINK_MULTIPLEXING |
		V4L2_SUBDEV_ROUTING_NO_SOURCE_MULTIPLEXING,
};

/**
 * v4l2_subdev_routing_validate() - Verify that routes comply with driver
 *				    constraints
 * @sd: The subdevice
 * @routing: Routing to verify
 * @disallow: Restrictions on routes
 *
 * This verifies that the given routing complies with the @disallow constraints.
 *
 * Returns 0 on success, error value otherwise.
 */
int v4l2_subdev_routing_validate(struct v4l2_subdev *sd,
				 const struct v4l2_subdev_krouting *routing,
				 enum v4l2_subdev_routing_restriction disallow);

//
// ========== 流控制 (Per-Pad Per-Stream) ==========
// enable/disable_streams 是多路流 API 的流控制核心操作，
// 取代了传统的 v4l2_subdev_video_ops.s_stream()。
//
//   v4l2_subdev_enable_streams(sd, pad, streams_mask):
//     在指定 source pad 上启用 streams_mask 中标记的流。
//     可同时启用多个流 (如 BIT_ULL(0) | BIT_ULL(1))。
//
//   v4l2_subdev_disable_streams(sd, pad, streams_mask):
//     禁用指定流的反向操作。
//
//   v4l2_subdev_s_stream_helper(sd, enable):
//     将 enable/disable_streams 封装为传统 s_stream 接口，
//     用于兼容旧版 userspace。仅适用于单 source pad 子设备。
//
//   __v4l2_subdev_get_frame_desc_passthrough / ...passthrough:
//     为透传型子设备提供 get_frame_desc 的默认实现。
//     自动遍历路由表，从上游收集帧描述信息 (如 CSI-2 VC/DT)。
//
// 注意: enable/disable 不允许重复操作已启用/禁用的流，
// 重复操作将返回 -EALREADY。调用者应通过 v4l2_subdev_is_streaming()
// 检查当前流状态。
//

/**
 * v4l2_subdev_enable_streams() - Enable streams on a pad
 * @sd: The subdevice
 * @pad: The pad
 * @streams_mask: Bitmask of streams to enable
 *
 * This function enables streams on a source @pad of a subdevice. The pad is
 * identified by its index, while the streams are identified by the
 * @streams_mask bitmask. This allows enabling multiple streams on a pad at
 * once.
 *
 * Enabling a stream that is already enabled isn't allowed. If @streams_mask
 * contains an already enabled stream, this function returns -EALREADY without
 * performing any operation.
 *
 * Per-stream enable is only available for subdevs that implement the
 * .enable_streams() and .disable_streams() operations. For other subdevs, this
 * function implements a best-effort compatibility by calling the .s_stream()
 * operation, limited to subdevs that have a single source pad.
 *
 * Drivers that are not stream-aware shall set @streams_mask to BIT_ULL(0).
 *
 * Return:
 * * 0: Success
 * * -EALREADY: One of the streams in streams_mask is already enabled
 * * -EINVAL: The pad index is invalid, or doesn't correspond to a source pad
 * * -EOPNOTSUPP: Falling back to the legacy .s_stream() operation is
 *   impossible because the subdev has multiple source pads
 */
int v4l2_subdev_enable_streams(struct v4l2_subdev *sd, u32 pad,
			       u64 streams_mask);

/**
 * v4l2_subdev_disable_streams() - Disable streams on a pad
 * @sd: The subdevice
 * @pad: The pad
 * @streams_mask: Bitmask of streams to disable
 *
 * This function disables streams on a source @pad of a subdevice. The pad is
 * identified by its index, while the streams are identified by the
 * @streams_mask bitmask. This allows disabling multiple streams on a pad at
 * once.
 *
 * Disabling a streams that is not enabled isn't allowed. If @streams_mask
 * contains a disabled stream, this function returns -EALREADY without
 * performing any operation.
 *
 * Per-stream disable is only available for subdevs that implement the
 * .enable_streams() and .disable_streams() operations. For other subdevs, this
 * function implements a best-effort compatibility by calling the .s_stream()
 * operation, limited to subdevs that have a single source pad.
 *
 * Drivers that are not stream-aware shall set @streams_mask to BIT_ULL(0).
 *
 * Return:
 * * 0: Success
 * * -EALREADY: One of the streams in streams_mask is not enabled
 * * -EINVAL: The pad index is invalid, or doesn't correspond to a source pad
 * * -EOPNOTSUPP: Falling back to the legacy .s_stream() operation is
 *   impossible because the subdev has multiple source pads
 */
int v4l2_subdev_disable_streams(struct v4l2_subdev *sd, u32 pad,
				u64 streams_mask);

/**
 * v4l2_subdev_s_stream_helper() - Helper to implement the subdev s_stream
 *	operation using enable_streams and disable_streams
 * @sd: The subdevice
 * @enable: Enable or disable streaming
 *
 * Subdevice drivers that implement the streams-aware
 * &v4l2_subdev_pad_ops.enable_streams and &v4l2_subdev_pad_ops.disable_streams
 * operations can use this helper to implement the legacy
 * &v4l2_subdev_video_ops.s_stream operation.
 *
 * This helper can only be used by subdevs that have a single source pad.
 *
 * Return: 0 on success, or a negative error code otherwise.
 */
int v4l2_subdev_s_stream_helper(struct v4l2_subdev *sd, int enable);

/**
 * __v4l2_subdev_get_frame_desc_passthrough - Helper to implement the
 *	subdev get_frame_desc operation in simple passthrough cases
 * @sd: The subdevice
 * @state: The locked subdevice active state
 * @pad: The source pad index
 * @fd: The mbus frame desc
 *
 * This helper implements the get_frame_desc operation for subdevices that pass
 * streams through without modification.
 *
 * The helper iterates over the subdevice's sink pads, calls get_frame_desc on
 * the remote subdevice connected to each sink pad, and collects the frame desc
 * entries for streams that are routed to the given source pad according to the
 * subdevice's routing table. Each entry is copied as-is from the upstream
 * source, with the exception of the 'stream' field which is remapped to the
 * source stream ID from the routing table.
 *
 * The frame desc type is taken from the first upstream source. If multiple
 * sink pads are involved and the upstream sources report different frame desc
 * types, -EPIPE is returned.
 *
 * The caller must hold the subdevice's active state lock. This variant is
 * intended for drivers that need to perform additional work around the
 * passthrough frame descriptor collection. Drivers that do not need any
 * customization should use v4l2_subdev_get_frame_desc_passthrough() instead.
 *
 * Return: 0 on success, or a negative error code otherwise.
 */
int __v4l2_subdev_get_frame_desc_passthrough(struct v4l2_subdev *sd,
					     struct v4l2_subdev_state *state,
					     unsigned int pad,
					     struct v4l2_mbus_frame_desc *fd);

/**
 * v4l2_subdev_get_frame_desc_passthrough() - Helper to implement the subdev
 *	get_frame_desc operation in simple passthrough cases
 * @sd: The subdevice
 * @pad: The source pad index
 * @fd: The mbus frame desc
 *
 * This function locks the subdevice's active state, calls
 * __v4l2_subdev_get_frame_desc_passthrough(), and unlocks the state.
 *
 * This function can be assigned directly as the .get_frame_desc callback in
 * &v4l2_subdev_pad_ops for subdevices that pass streams through without
 * modification. Drivers that need to perform additional work should use
 * __v4l2_subdev_get_frame_desc_passthrough() in their custom
 * .get_frame_desc implementation instead.
 *
 * Return: 0 on success, or a negative error code otherwise.
 */
int v4l2_subdev_get_frame_desc_passthrough(struct v4l2_subdev *sd,
					   unsigned int pad,
					   struct v4l2_mbus_frame_desc *fd);

#endif /* CONFIG_VIDEO_V4L2_SUBDEV_API */

#endif /* CONFIG_MEDIA_CONTROLLER */

//
// ========== 子设备状态锁管理 ==========
// 以下函数管理子设备 active_state 的并发访问:
//   v4l2_subdev_lock_state / unlock_state: 加/解锁单个 state
//   v4l2_subdev_lock_states / unlock_states: 同时操作两个 state，
//     当两 state 共享同一把锁时只锁一次以避免递归死锁
//   v4l2_subdev_get_unlocked_active_state: 获取 state 并断言锁未持有
//   v4l2_subdev_get_locked_active_state: 获取 state 并断言锁已持有
//   v4l2_subdev_lock_and_get_active_state: "加锁 + 获取" 组合操作
//
// 使用规范: 所有对 active_state 字段的读写都必须持有 state->lock。
//

/**
 * v4l2_subdev_lock_state() - Locks the subdev state
 * @state: The subdevice state
 *
 * Locks the given subdev state.
 *
 * The state must be unlocked with v4l2_subdev_unlock_state() after use.
 */
static inline void v4l2_subdev_lock_state(struct v4l2_subdev_state *state)
{
	mutex_lock(state->lock);
}

/**
 * v4l2_subdev_unlock_state() - Unlocks the subdev state
 * @state: The subdevice state
 *
 * Unlocks the given subdev state.
 */
static inline void v4l2_subdev_unlock_state(struct v4l2_subdev_state *state)
{
	mutex_unlock(state->lock);
}

/**
 * v4l2_subdev_lock_states - Lock two sub-device states
 * @state1: One subdevice state
 * @state2: The other subdevice state
 *
 * Locks the state of two sub-devices.
 *
 * The states must be unlocked with v4l2_subdev_unlock_states() after use.
 *
 * This differs from calling v4l2_subdev_lock_state() on both states so that if
 * the states share the same lock, the lock is acquired only once (so no
 * deadlock occurs). The caller is responsible for ensuring the locks will
 * always be acquired in the same order.
 */
static inline void v4l2_subdev_lock_states(struct v4l2_subdev_state *state1,
					   struct v4l2_subdev_state *state2)
{
	mutex_lock(state1->lock);
	if (state1->lock != state2->lock)
		mutex_lock(state2->lock);
}

/**
 * v4l2_subdev_unlock_states() - Unlock two sub-device states
 * @state1: One subdevice state
 * @state2: The other subdevice state
 *
 * Unlocks the state of two sub-devices.
 *
 * This differs from calling v4l2_subdev_unlock_state() on both states so that
 * if the states share the same lock, the lock is released only once.
 */
static inline void v4l2_subdev_unlock_states(struct v4l2_subdev_state *state1,
					     struct v4l2_subdev_state *state2)
{
	mutex_unlock(state1->lock);
	if (state1->lock != state2->lock)
		mutex_unlock(state2->lock);
}

/**
 * v4l2_subdev_get_unlocked_active_state() - Checks that the active subdev state
 *					     is unlocked and returns it
 * @sd: The subdevice
 *
 * Returns the active state for the subdevice, or NULL if the subdev does not
 * support active state. If the state is not NULL, calls
 * lockdep_assert_not_held() to issue a warning if the state is locked.
 *
 * This function is to be used e.g. when getting the active state for the sole
 * purpose of passing it forward, without accessing the state fields.
 */
static inline struct v4l2_subdev_state *
v4l2_subdev_get_unlocked_active_state(struct v4l2_subdev *sd)
{
	if (sd->active_state)
		lockdep_assert_not_held(sd->active_state->lock);
	return sd->active_state;
}

/**
 * v4l2_subdev_get_locked_active_state() - Checks that the active subdev state
 *					   is locked and returns it
 *
 * @sd: The subdevice
 *
 * Returns the active state for the subdevice, or NULL if the subdev does not
 * support active state. If the state is not NULL, calls lockdep_assert_held()
 * to issue a warning if the state is not locked.
 *
 * This function is to be used when the caller knows that the active state is
 * already locked.
 */
static inline struct v4l2_subdev_state *
v4l2_subdev_get_locked_active_state(struct v4l2_subdev *sd)
{
	if (sd->active_state)
		lockdep_assert_held(sd->active_state->lock);
	return sd->active_state;
}

/**
 * v4l2_subdev_lock_and_get_active_state() - Locks and returns the active subdev
 *					     state for the subdevice
 * @sd: The subdevice
 *
 * Returns the locked active state for the subdevice, or NULL if the subdev
 * does not support active state.
 *
 * The state must be unlocked with v4l2_subdev_unlock_state() after use.
 */
static inline struct v4l2_subdev_state *
v4l2_subdev_lock_and_get_active_state(struct v4l2_subdev *sd)
{
	if (sd->active_state)
		v4l2_subdev_lock_state(sd->active_state);
	return sd->active_state;
}

//
// ========== 子设备初始化函数 ==========
// v4l2_subdev_init() 是基础的子设备初始化函数，设置 sd->ops 和基本字段。
// 对于 I2C/SPI 设备应使用 v4l2_i2c_subdev_init()/v4l2_spi_subdev_init()。
// 如需集中式 active state 管理，在 media_entity_pads_init() 之后调用
// v4l2_subdev_init_finalize() 分配 active_state。
//

/**
 * v4l2_subdev_init - initializes the sub-device struct
 *
 * @sd: pointer to the &struct v4l2_subdev to be initialized
 * @ops: pointer to &struct v4l2_subdev_ops.
 */
void v4l2_subdev_init(struct v4l2_subdev *sd,
		      const struct v4l2_subdev_ops *ops);

//
// ========== v4l2_subdev_call 宏：子设备操作调用分发机制 ==========
// v4l2_subdev_call(sd, o, f, args...) 是调用子设备操作的核心宏:
//   1. 空指针检查: sd == NULL 时返回 -ENODEV
//   2. 存在性检查: ops->o 或 ops->o->f 为 NULL 时返回 -ENOIOCTLCMD
//   3. 包装器检查: 若 v4l2_subdev_call_wrappers.o->f 存在则调用之。
//      这种全局包装器机制允许 V4L2 核心框架在驱动操作前后注入额外行为
//      (如参数验证、锁管理)，无需修改驱动代码。
//   4. 直接调用: 否则执行 __sd->ops->o->f(__sd, args...)
//   5. 返回 int 类型的 __result
//
// v4l2_subdev_call_state_active(sd, o, f, args...):
//   pad ops 专用变体。自动获取 active_state，加锁后作为 state 参数
//   传给操作函数，完成后解锁。省去手动锁管理。
//
// v4l2_subdev_call_state_try(sd, o, f, args...):
//   为 TRY 模式分配临时 v4l2_subdev_state，用于格式验证。
//   仅旧式非 MC 驱动需要此宏。
//
// v4l2_subdev_has_op(sd, o, f):
//   检查子设备是否实现了特定操作 ops->o->f。
//   展开为: (sd)->ops->o && (sd)->ops->o->f
//   用于在调用前确认操作可用。
//

extern const struct v4l2_subdev_ops v4l2_subdev_call_wrappers;

/**
 * v4l2_subdev_call - call an operation of a v4l2_subdev.
 *
 * @sd: pointer to the &struct v4l2_subdev
 * @o: name of the element at &struct v4l2_subdev_ops that contains @f.
 *     Each element there groups a set of callbacks functions.
 * @f: callback function to be called.
 *     The callback functions are defined in groups, according to
 *     each element at &struct v4l2_subdev_ops.
 * @args: arguments for @f.
 *
 * Example: err = v4l2_subdev_call(sd, video, s_std, norm);
 */
#define v4l2_subdev_call(sd, o, f, args...)				\
	({								\
		struct v4l2_subdev *__sd = (sd);			\
		int __result;						\
		if (!__sd)						\
			__result = -ENODEV;				\
		else if (!(__sd->ops->o && __sd->ops->o->f))		\
			__result = -ENOIOCTLCMD;			\
		else if (v4l2_subdev_call_wrappers.o &&			\
			 v4l2_subdev_call_wrappers.o->f)		\
			__result = v4l2_subdev_call_wrappers.o->f(	\
							__sd, ##args);	\
		else							\
			__result = __sd->ops->o->f(__sd, ##args);	\
		__result;						\
	})

/**
 * v4l2_subdev_call_state_active - call an operation of a v4l2_subdev which
 *				   takes state as a parameter, passing the
 *				   subdev its active state.
 *
 * @sd: pointer to the &struct v4l2_subdev
 * @o: name of the element at &struct v4l2_subdev_ops that contains @f.
 *     Each element there groups a set of callbacks functions.
 * @f: callback function to be called.
 *     The callback functions are defined in groups, according to
 *     each element at &struct v4l2_subdev_ops.
 * @args: arguments for @f.
 *
 * This is similar to v4l2_subdev_call(), except that this version can only be
 * used for ops that take a subdev state as a parameter. The macro will get the
 * active state, lock it before calling the op and unlock it after the call.
 */
#define v4l2_subdev_call_state_active(sd, o, f, args...)		\
	({								\
		int __result;						\
		struct v4l2_subdev_state *state;			\
		state = v4l2_subdev_get_unlocked_active_state(sd);	\
		if (state)						\
			v4l2_subdev_lock_state(state);			\
		__result = v4l2_subdev_call(sd, o, f, state, ##args);	\
		if (state)						\
			v4l2_subdev_unlock_state(state);		\
		__result;						\
	})

/**
 * v4l2_subdev_call_state_try - call an operation of a v4l2_subdev which
 *				takes state as a parameter, passing the
 *				subdev a newly allocated try state.
 *
 * @sd: pointer to the &struct v4l2_subdev
 * @o: name of the element at &struct v4l2_subdev_ops that contains @f.
 *     Each element there groups a set of callbacks functions.
 * @f: callback function to be called.
 *     The callback functions are defined in groups, according to
 *     each element at &struct v4l2_subdev_ops.
 * @args: arguments for @f.
 *
 * This is similar to v4l2_subdev_call_state_active(), except that as this
 * version allocates a new state, this is only usable for
 * V4L2_SUBDEV_FORMAT_TRY use cases.
 *
 * Note: only legacy non-MC drivers may need this macro.
 */
#define v4l2_subdev_call_state_try(sd, o, f, args...)                         \
	({                                                                    \
		int __result;                                                 \
		static struct lock_class_key __key;                           \
		const char *name = KBUILD_BASENAME                            \
			":" __stringify(__LINE__) ":state->lock";             \
		struct v4l2_subdev_state *state =                             \
			__v4l2_subdev_state_alloc(sd, name, &__key);          \
		if (IS_ERR(state)) {                                          \
			__result = PTR_ERR(state);                            \
		} else {                                                      \
			v4l2_subdev_lock_state(state);                        \
			__result = v4l2_subdev_call(sd, o, f, state, ##args); \
			v4l2_subdev_unlock_state(state);                      \
			__v4l2_subdev_state_free(state);                      \
		}                                                             \
		__result;                                                     \
	})

/**
 * v4l2_subdev_has_op - Checks if a subdev defines a certain operation.
 *
 * @sd: pointer to the &struct v4l2_subdev
 * @o: The group of callback functions in &struct v4l2_subdev_ops
 * which @f is a part of.
 * @f: callback function to be checked for its existence.
 */
#define v4l2_subdev_has_op(sd, o, f) \
	((sd)->ops->o && (sd)->ops->o->f)

//
// ========== 子设备辅助函数 ==========
// v4l2_subdev_notify_event: 向所有订阅了子设备事件的 userspace 监听器
//     以及 bridge 驱动发送事件通知。通知类型为 V4L2_DEVICE_NOTIFY_EVENT。
// v4l2_subdev_is_streaming: 查询子设备当前是否处于流传输状态。
//     当 s_stream() 或 enable_streams() 成功调用且尚未停止时返回 true。
//     若子设备实现了 enable_streams()，调用此函数前需持有 active state 锁。
//

/**
 * v4l2_subdev_notify_event() - Delivers event notification for subdevice
 * @sd: The subdev for which to deliver the event
 * @ev: The event to deliver
 *
 * Will deliver the specified event to all userspace event listeners which are
 * subscribed to the v42l subdev event queue as well as to the bridge driver
 * using the notify callback. The notification type for the notify callback
 * will be %V4L2_DEVICE_NOTIFY_EVENT.
 */
void v4l2_subdev_notify_event(struct v4l2_subdev *sd,
			      const struct v4l2_event *ev);

/**
 * v4l2_subdev_is_streaming() - Returns if the subdevice is streaming
 * @sd: The subdevice
 *
 * v4l2_subdev_is_streaming() tells if the subdevice is currently streaming.
 * "Streaming" here means whether .s_stream() or .enable_streams() has been
 * successfully called, and the streaming has not yet been disabled.
 *
 * If the subdevice implements .enable_streams() this function must be called
 * while holding the active state lock.
 */
bool v4l2_subdev_is_streaming(struct v4l2_subdev *sd);

#endif /* _V4L2_SUBDEV_H */
