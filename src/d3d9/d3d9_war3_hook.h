#pragma once

#include <atomic>
#include <cstdint>
#include <d3d9.h>

#include "war3/render/war3_render_state.h"

namespace dxvk {

/**
 * @brief War3 渲染 Hook 总入口。
 *
 * 该类负责管理 War3 运行时相关 Hook 的安装与状态查询，并提供给设备层
 * （`D3D9DeviceEx`）的最小访问接口。
 *
 * @note 该类是静态工具类，不提供实例化对象。
 */
class War3Hook {
public:
  using RenderLayer = War3RenderLayer;

  /**
   * @brief 安装引导 Hook（Bootstrap）。
   * @param device 当前 D3D9 设备指针。
   *
   * 在设备创建后调用，仅安装轻量入口（如 JASS 执行入口），
   * 完整渲染 Hook 由运行时激活阶段延后安装。
   */
  static void InstallHooks(IDirect3DDevice9 *device);

  /**
   * @brief 安装完整 Game.dll Hook 集合。
   * @param gameBase Game.dll 模块基址。
   *
   * @warning 仅在版本校验与地址探测通过后执行。
   */
  static void InstallGameHooks(uintptr_t gameBase);

  /**
   * @brief 设置当前渲染阶段。
   * @param stage 当前阶段编号。
   *
   * 由渲染分发 Hook 调用，用于后续 Draw 路径读取阶段上下文。
   */
  static void SetCurrentStage(int stage);

  /**
   * @brief 获取当前渲染阶段。
   * @return 当前阶段编号；未命中时一般为负值。
   */
  static int GetCurrentStage();

  /**
   * @brief 查询当前是否处于 UI 渲染阶段。
   * @return `true` 表示当前为 UI 渲染。
   */
  static bool IsUiRendering();

  /**
   * @brief 查询是否应跳过 UI 绘制。
   * @return `true` 表示当前配置要求跳过 UI Draw。
   */
  static bool ShouldSkipUiDraw();

  /**
   * @brief 查询完整 Hook 是否已安装完成。
   * @return `true` 表示已完成安装流程。
   */
  static bool IsHooksInstalled();

  /**
   * @brief 标记 Hook 已安装完成。
   * @note 由安装流程尾部调用，避免重复安装。
   */
  static void MarkHooksInstalled();

  /**
   * @brief 获取当前阶段对应的调试颜色。
   * @return ARGB 颜色值；返回 `0` 表示不覆盖颜色。
   */
  static uint32_t GetDebugColorForCurrentStage();

  /**
   * @brief 查询是否启用调试着色。
   * @return `true` 表示允许输出调试着色。
   */
  static bool IsDebugColoringEnabled();

  /**
   * @brief 写入 UI 层标记。
   * @note 主要用于兼容旧入口，具体状态由 `War3RenderState` 管理。
   */
  static void PushUiLayer();

  /**
   * @brief 恢复上一层渲染层级。
   * @param prev 进入当前层前的层级值。
   */
  static void PopLayer(War3RenderLayer prev);

  /**
   * @brief 触发阴影重放。
   * @note 在阴影 Pass 中调用，重放关键 world group 以生成 caster 数据。
   */
  static void TriggerShadowReplay();

  /**
   * @brief 获取缓存的 World 指针。
   * @return World 对象指针；不可用时返回 `nullptr`。
   */
  static void *GetCachedWorldPtr();

  /**
   * @brief 设置当前线程阴影 Pass 标记。
   * @param active 是否处于阴影 Pass。
   *
   * @thread_safety 使用线程局部变量，不跨线程共享状态。
   */
  static void SetShadowPass(bool active);

  /**
   * @brief 查询当前线程是否处于阴影 Pass。
   * @return `true` 表示当前线程正在执行阴影 Pass。
   */
  static bool IsInShadowPass();

  /**
   * @brief 在运行时真正热起来后，惰性安装 native renderer takeover。
   * @param reason 触发原因（用于诊断日志）。
   *
   * 该入口用于避免在 `ActivateWar3Runtime` 或伪 in-game 时机过早改写
   * `CWorldFrameWar3::RenderScene`，导致地图进入链被冻结。
   */
  static void MaybeInstallNativeRendererTakeover(const char *reason = nullptr);

private:
  static std::atomic<int> s_currentStage;
  static bool s_hooksInstalled;
  static thread_local bool s_inShadowPass;
  static IDirect3DDevice9 *s_device;

public:
  /**
   * @brief 获取当前设备指针。
   * @return 当前绑定的 `IDirect3DDevice9*`。
   */
  static IDirect3DDevice9 *GetDevice();
};

/**
 * @brief 激活 War3 运行时并安装完整 Hook 集。
 * @param gameBase `Game.dll` 基址。
 * @param source 触发来源标识（用于诊断日志）。
 *
 * @note 该入口由生命周期域在 `MainRunner` 返回后调用，
 * 保持与原生初始化顺序一致。
 */
void ActivateWar3Runtime(uintptr_t gameBase, const char *source);

/**
 * @brief 在主循环入口前尝试前置安装早期阴影/内存域能力。
 * @param gameBase `Game.dll` 基址。
 * @param source 触发来源标识（用于诊断日志）。
 *
 * @performance 该入口用于“首轮写入捕获 + StormBreaker 尽早装载”场景，
 * 失败后会由常规运行时初始化路径兜底，不应在热路径反复做重探测。
 */
void TryInstallShadowHooksEarly(uintptr_t gameBase, const char *source);

// Phase 7.89：shadow 数据层 producer 在退出地图后的门控标志。
// 定义在 d3d9_war3_hook.cpp（namespace dxvk 内）。退出地图时置 false，进入地图时置 true。
extern std::atomic<bool> g_war3_runtime_activated;

} // namespace dxvk
