#pragma once

#include <cstdint>

namespace dxvk::war3::platform {

/**
 * @brief 初始化 War3 运行时核心模块。
 *
 * 包含：
 * - `dxvk::war3::Initialize` 初始化；
 * - ShaderManager 热重载；
 * - NetEventHook 初始化。
 *
 * @param gameBase `Game.dll` 基址（为 0 时使用自动探测路径）。
 */
void InitializeRuntimeCore(uintptr_t gameBase);

/**
 * @brief 重置 War3 运行时核心模块。
 *
 * 包含：
 * - 渲染追踪器与缓存清理；
 * - 渲染器帧状态复位；
 * - 运行时状态机复位；
 * - NetEventHook 清理。
 */
void ResetRuntimeCore();

} // namespace dxvk::war3::platform

