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

  /**
   * @brief 仅安装 WarVK JASS 命令桥 carrier。
   *
   * 用于 `.ai`/LoadLibrary 中途载入路径：此时 InitJassNatives 通常已经
   * 结束，不应再晚装完整 JASS/生命周期 Hook；只需要把
   * 三个字符串 carrier 指向 WarVK 控制面；可选 Hashtable Save/Load
   * carrier 指向强类型数值数据面。
   *
   * @param gameBase Game.dll 基址。
   * @param reason 诊断来源。
   * @return true 表示 carrier 已安装完成。
   */
  static bool InstallCommandBridgeOnly(uintptr_t gameBase,
                                       const char *reason);
};
} // namespace dxvk::war3::hooks
