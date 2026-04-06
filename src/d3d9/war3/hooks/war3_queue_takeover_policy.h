#pragma once

#include <cstdint>

namespace dxvk::war3::hooks {

/**
 * @brief 队列接管模式。
 */
enum class War3QueueTakeoverMode : std::uint8_t {
  Fallback = 0,
  Full,
  Conservative,
};

/**
 * @brief 队列接管决策原因码。
 */
enum class War3QueueTakeoverReason : std::uint8_t {
  Unknown = 0,
  Disabled,
  FullAccepted,
  ConservativeAccepted,
  OpaqueBelowFullMin,
  TransparentAboveFullRange,
  TransparentPrereqMissing,
  NativeTransparentFlushMissing,
  ConservativeRejected,
};

/**
 * @brief 队列接管上下文。
 *
 * 全部为“只读视图”，不持有所有权。
 */
struct War3QueueTakeoverContext {
  const std::uint32_t *opaqueCountPtr = nullptr;
  const std::uint32_t *transparentCountPtr = nullptr;
  const void *transparentArrayBasePtr = nullptr;
  const void *transparentSortedPtrs = nullptr;
  const void *transparentDispatchType0 = nullptr;
  const void *transparentDispatchType1 = nullptr;
  const void *transparentDispatchType2 = nullptr;
  const void *transparentDispatchType3 = nullptr;
  const void *transparentDispatchType4 = nullptr;
  bool hasOriginalFlushTransparent = false;
};

/**
 * @brief 队列接管决策结果。
 */
struct War3QueueTakeoverDecision {
  War3QueueTakeoverMode mode = War3QueueTakeoverMode::Fallback;
  War3QueueTakeoverReason reason = War3QueueTakeoverReason::Unknown;
  std::uint32_t opaqueCount = 0;
  std::uint32_t transparentCount = 0;
};

bool HasTransparentTakeoverPrerequisites(
    const War3QueueTakeoverContext &ctx);

bool ShouldUseConservativeQueueTakeover(
    const War3QueueTakeoverContext &ctx, std::uint32_t *outOpaqueCount,
    std::uint32_t *outTransparentCount);

War3QueueTakeoverDecision
EvaluateQueueTakeoverDecision(const War3QueueTakeoverContext &ctx);

} // namespace dxvk::war3::hooks

