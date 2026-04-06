#pragma once


#include <cstdint>
#include <windows.h>


namespace dxvk::war3::hooks {
/**
 * @brief 生命周期域 Hook 安装入口。
 *
 * 负责 MainRunner/FlushAndReset 等生命周期相关链路的 Hook 安装。
 */
class War3HookLifecycle {
public:
  /**
   * @brief 安装生命周期域 Hook。
   * @param gameBase Game.dll 基址。
   */
  static void Install(uintptr_t gameBase);

  /**
   * @brief 获取 FlushAndReset trampoline。
   * @return 原始 `FlushAndReset` 的 trampoline 地址；不可用时返回 `nullptr`。
   */
  static void *GetTrampolineFlushAndReset();
};

/**
 * @brief 尝试同步 War3 内部 windowed 逻辑分辨率。
 * @param previousWidth  resize 前的逻辑宽度。
 * @param previousHeight resize 前的逻辑高度。
 * @param width          当前 client width。
 * @param height         当前 client height。
 * @return 命中并写入至少一组内部字段时返回 true。
 */
bool TryOverrideWindowedClientSize(UINT previousWidth, UINT previousHeight,
                                   UINT width, UINT height);

/**
 * @brief 触发 War3 原生 UI 的 windowed resize 收尾逻辑。
 * @param window 当前游戏窗口句柄。
 * @param width  当前 client width。
 * @param height 当前 client height。
 * @return 成功命中并调用原生 resize 回调时返回 true。
 */
bool TryNotifyWindowedUiSizeChanged(HWND window, UINT width, UINT height);

/**
 * @brief 获取已识别的主循环线程 ID。
 * @return 主循环线程 ID；若尚未识别则返回 0。
 */
DWORD GetMainLoopThreadId();
} // namespace dxvk::war3::hooks
