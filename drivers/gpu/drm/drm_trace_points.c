/*
 * 文件名: drm_trace_points.c
 *
 * 中文描述: DRM 跟踪点定义
 *
 * 本文件定义了 DRM 子系统的 ftrace 跟踪点。跟踪点是 Linux 内核 ftrace
 * 框架的核心概念，允许在代码的关键路径上插入观察点，用于调试、性能分析和
 * 行为追踪，而不会在未启用时引入额外开销。
 *
 * 通过 CREATE_TRACE_POINTS 宏实例化 drm_trace.h 头文件中声明的所有
 * 跟踪点。驱动程序或核心 DRM 代码可以在关键操作处调用这些跟踪点，
 * 使开发者能够通过 tracefs 或 perf 工具观察 DRM 子系统的行为。
 */

#include <drm/drm_file.h>

#define CREATE_TRACE_POINTS
#include "drm_trace.h"
