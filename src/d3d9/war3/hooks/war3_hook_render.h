#pragma once


#include <array>
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace dxvk {
namespace war3::hooks {

enum PublishVisibleSafeCopyMismatch : uint32_t {
  PublishVisibleSafeCopyMismatchNone = 0u,
  PublishVisibleSafeCopyMismatchMeshData = 1u << 0,
  PublishVisibleSafeCopyMismatchSceneNode = 1u << 1,
  PublishVisibleSafeCopyMismatchEarlyReturn = 1u << 2,
};

constexpr size_t kPublishVisibleSafeCopyMismatchMaskCount = 8u;

/**
 * @brief PublishVisible SafeCopy 与历史 SafeReadPtrFast 的抽样等价性快照。
 *
 * 计数为进程生命周期口径。默认关闭的 verifier 使用 TLS 批量累计；命中
 * mismatch 时立即发布，正常样本则按批次发布。报告导出会刷新当前线程残量。
 */
struct PublishVisibleSafeCopyVerifierStats {
  bool productionEnabled = false;
  bool enabled = false;
  bool assertOnMismatch = false;
  uint32_t samplePeriod = 256u;
  uint64_t attempts = 0u;
  uint64_t scenePreset = 0u;
  uint64_t sceneNullInput = 0u;
  uint64_t copySuccess = 0u;
  uint64_t copyFailure = 0u;
  uint64_t candidateEarlyReturn = 0u;
  uint64_t legacyEarlyReturn = 0u;
  uint64_t rereadCount = 0u;
  uint64_t initialMismatchCount = 0u;
  uint64_t stableMismatchCount = 0u;
  uint64_t unstableCount = 0u;
  uint32_t initialMismatchMaskOr = PublishVisibleSafeCopyMismatchNone;
  uint32_t stableMismatchMaskOr = PublishVisibleSafeCopyMismatchNone;
  std::array<uint64_t, kPublishVisibleSafeCopyMismatchMaskCount>
      initialMismatchMaskCounts = {};
  std::array<uint64_t, kPublishVisibleSafeCopyMismatchMaskCount>
      stableMismatchMaskCounts = {};
};

PublishVisibleSafeCopyVerifierStats
QueryPublishVisibleSafeCopyVerifierStats() noexcept;

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
