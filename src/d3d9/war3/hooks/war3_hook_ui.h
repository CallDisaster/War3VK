#pragma once

#include <cstdint>

namespace dxvk::war3::hooks {
/**
 * @brief 安装 UI 域 Hook。
 * @param gameBase Game.dll 基址。
 *
 * 该入口负责安装 UI 分发链与 UI 可渲染节点链路 Hook，
 * 同时接入 FPS 解锁逻辑所需的运行时辅助路径。
 */
void InstallUiHooks(uintptr_t gameBase);

/**
 * @brief 尝试覆盖游戏帧率上限。
 *
 * 该函数在 UI/渲染热路径按低频重试策略执行，
 * 用于在运行时阶段完成 FPS 上限和刷新率配置。
 */
void War3TryOverrideMaxFps();
} // namespace dxvk::war3::hooks

/**
 * @brief 兼容性导出命名空间。
 *
 * 保留旧调用点可见的符号路径，避免迁移期间链接失败。
 */
namespace dxvk {
using war3::hooks::War3TryOverrideMaxFps;
}
