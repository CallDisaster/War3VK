#include "war3_shadow_alpha_test_payload.h"

namespace dxvk {
namespace war3 {
namespace shadow {

War3ShadowAlphaTestPayloadCounters g_war3ShadowAlphaTestPayloadCounters = {};

/// @brief 快照全局计数器；调用方不持有锁。
War3ShadowAlphaTestPayloadCountersSnapshot
ReadWar3ShadowAlphaTestPayloadCountersSnapshot() {
  const auto& src = g_war3ShadowAlphaTestPayloadCounters;
  War3ShadowAlphaTestPayloadCountersSnapshot out = {};
  out.attemptCount = src.attemptCount.load(std::memory_order_relaxed);
  out.hitCount = src.hitCount.load(std::memory_order_relaxed);
  out.missNoUvCount = src.missNoUvCount.load(std::memory_order_relaxed);
  out.missNoDiffuseCount =
      src.missNoDiffuseCount.load(std::memory_order_relaxed);
  out.missStageInvalidCount =
      src.missStageInvalidCount.load(std::memory_order_relaxed);
  out.appliedCount = src.appliedCount.load(std::memory_order_relaxed);
  out.fallbackRejectCount =
      src.fallbackRejectCount.load(std::memory_order_relaxed);
  out.stashCapturedCount =
      src.stashCapturedCount.load(std::memory_order_relaxed);
  out.stashSkipNoSemanticKeyCount =
      src.stashSkipNoSemanticKeyCount.load(std::memory_order_relaxed);
  out.stashSkipNoUvCount =
      src.stashSkipNoUvCount.load(std::memory_order_relaxed);
  out.stashSkipNoDiffuseCount =
      src.stashSkipNoDiffuseCount.load(std::memory_order_relaxed);
  out.stashSkipNoUploadCount =
      src.stashSkipNoUploadCount.load(std::memory_order_relaxed);
  out.cacheEvictedCount =
      src.cacheEvictedCount.load(std::memory_order_relaxed);
  out.cacheSizeGauge = src.cacheSizeGauge.load(std::memory_order_relaxed);
  return out;
}

} // namespace shadow
} // namespace war3
} // namespace dxvk
