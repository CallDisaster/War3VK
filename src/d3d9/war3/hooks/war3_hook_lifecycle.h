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

/**
 * Temporarily allow a PauseGame request on the current thread even when the
 * unattended-test build blocks ordinary pause/menu requests.
 *
 * Returns the previous state so a tightly scoped diagnostic caller can
 * restore it immediately after the stock native returns. This never changes
 * the process-wide AutoTest pause policy.
 */
bool SetInternalTestGamePauseBypassForCurrentThread(bool enabled);

/**
 * @brief 以 WorldFrame 的准备/渲染边界校正主循环线程身份。
 *
 * 生命周期域的若干引擎 Hook 也可能在 worker 上命中，不能让它们的首次
 * CAS 永久冒充主线程。WorldFrame 边界是渲染主线程的权威证据。录制开启时，
 * 前两次调用还会分别闭合 Present→首个 WorldFrame 边界与首个→第二个
 * WorldFrame 边界的 detached phase-wall 窗口。
 */
void MarkWorldFrameThread();

/**
 * @brief 在成功 Present 完成并开启新 perf frame 后，开始 detached 帧阶段墙钟。
 *
 * PERF_LEVEL>=1 且 monitor 正在录制时，QPC 从 Swapchain beginFrame() 后开始。
 * 随后的两个 WorldFrame 入口与下一次 Present 入口把它闭合为三个相邻阶段墙钟。
 * 它不占用 PerfMonitor TLS scope 栈，因此不会与包住 Present 的
 * EventMainCallback/NativeOriginal RAII scope 发生跨函数 LIFO 冲突。
 */
void BeginPreWorldLogicPerfPhase();

/**
 * @brief 在下一次 Present 收口前结束 detached 帧阶段墙钟。
 *
 * 正常帧闭合第二个 WorldFrame 边界→Present；只有一个边界时闭合
 * 首边界→Present；loading/device transition 等没有 WorldFrame 的帧则闭合整段
 * Present→Present。每次调用都会 exchange-reset 全部状态，禁止 QPC 起点跨 epoch。
 * 所有结果都是 non-additive phase-wall overlay，不是 CPU scope，也不伪造同期
 * Hook 节点为它的调用树子节点。
 */
void EndPreWorldLogicPerfPhase();
} // namespace dxvk::war3::hooks
