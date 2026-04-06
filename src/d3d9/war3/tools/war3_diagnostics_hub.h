#pragma once

#include <cstdint>

namespace dxvk::war3::tools {

/**
 * @brief 记录运行时初始化总览（仅首次打印）。
 *
 * 输出内容包含：
 * - 模块系统统计；
 * - PerfMonitor 开关状态。
 *
 * @param source 触发来源（例如 `ActivateWar3Runtime`）。
 */
void LogRuntimeSummaryOnce(const char* source);

/**
 * @brief 低频记录运行时健康状态。
 *
 * @param frameIndex 当前帧号。
 * @param interval 每隔多少帧输出一次。
 */
void LogRuntimeHealthPeriodic(uint64_t frameIndex, uint32_t interval = 1200);

/**
 * @brief 立即写出 runtime 状态快照到 `WarVK/Temp/runtime_status.json`。
 *
 * 用于外部自动化（MCP）按文件轮询“运行时就绪/进图状态”。
 *
 * @param source 来源标签（例如 `War3Events/OnGameStart`）。
 * @param frameIndex 当前帧号（未知时传 0）。
 */
void ExportRuntimeStatusSnapshot(const char* source, uint64_t frameIndex = 0);

/**
 * @brief 标记“已看到正式进游戏渲染信号”。
 *
 * 用于修正仅靠 NetEvent runtimeReady 不足以覆盖的场景。
 */
void MarkInGameRenderReady(const char* source, uint64_t frameIndex = 0);

/**
 * @brief 重置运行态就绪补充信号。
 */
void ResetRuntimeReadySignals();

} // namespace dxvk::war3::tools
