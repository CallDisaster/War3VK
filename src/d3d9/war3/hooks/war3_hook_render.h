#pragma once


#include <cstdint>
#include <windows.h>

namespace dxvk {
namespace war3::hooks {
/**
 * @brief 渲染域 Hook 安装与查询接口。
 *
 * 该模块承载 RenderDispatcher/WorldDispatch/DispatchCommon 等
 * 渲染关键链路 Hook 的安装入口与 trampoline 查询接口。
 */
class War3HookRender {
public:
  /**
   * @brief 安装渲染域 Hook。
   * @param gameBase Game.dll 基址。
   */
  static void Install(uintptr_t gameBase);

  /**
   * @brief 获取 DispatchSpecial trampoline。
   * @return 原函数 trampoline 地址。
   */
  static void *GetTrampolineDispatchSpecial();

  /**
   * @brief 获取 DispatchCommon trampoline。
   * @return 原函数 trampoline 地址。
   */
  static void *GetTrampolineDispatchCommon();

  /**
   * @brief 获取 WorldObjects_RenderGroup trampoline。
   * @return 原函数 trampoline 地址。
   */
  static void *GetTrampolineWorldObjectsRenderGroup();

  /**
   * @brief 重置 Dispatch 热路径缓存与局部合并上下文（帧尾收口）。
   *
   * 当启用 Dispatch 局部上下文复用时，最后一个批次可能在帧尾仍保持活跃，
   * 该接口用于在 FlushAndReset 收口阶段显式结束上下文并清空短期缓存，
   * 避免跨帧污染。
   */
  static void ResetDispatchMergeContext();
};
} // namespace war3::hooks
} // namespace dxvk
