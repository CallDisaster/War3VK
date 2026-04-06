#pragma once

#include <cstdint>
#include <windows.h>

namespace dxvk::war3::hooks {
/**
 * @brief JASS 域 Hook 安装入口。
 *
 * 负责 JASS Native 注册链与执行入口 Hook 的安装。
 */
class War3HookJass {
public:
  /**
   * @brief 安装 JASS 域 Hook。
   * @param gameBase Game.dll 基址。
   */
  static void Install(uintptr_t gameBase);
};
} // namespace dxvk::war3::hooks
