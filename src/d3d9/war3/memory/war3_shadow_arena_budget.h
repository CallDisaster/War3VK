#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include "war3_adaptive_memory_budget.h"

namespace dxvk::war3::memory {

constexpr uint64_t kShadowArenaFixedResidentLimitBytes =
    1152ull * 1024ull * 1024ull;

struct ShadowArenaMemoryBudgetPolicy {
  bool supported = false;
  bool trusted = false;
  uint64_t heapSizeBytes = 0u;
  uint64_t heapBudgetBytes = 0u;
  uint64_t heapAllocatedBytes = 0u;
  uint64_t availableBytes = 0u;
  uint64_t proportionalLimitBytes = 0u;
  uint64_t reserveLimitBytes = 0u;
  uint64_t fixedResidentLimitBytes = 0u;
  uint64_t effectiveResidentLimitBytes = 0u;
};

// Compatibility-shaped diagnostics, derived from the SAME policy used by
// snapshots. No second allocation algorithm or available-minus-resident rule.
inline ShadowArenaMemoryBudgetPolicy ResolveShadowArenaMemoryBudget(
    const AdaptiveBudgetInput& input) noexcept {
  ShadowArenaMemoryBudgetPolicy result = {};
  const auto adaptive = DecideAdaptiveMemoryBudget(input);
  result.supported = input.supported;
  result.trusted = adaptive.trusted;
  result.heapSizeBytes = input.heapSize;
  result.heapBudgetBytes = input.budget;
  result.heapAllocatedBytes = input.committed;
  result.fixedResidentLimitBytes = input.hardCap;
  result.availableBytes = input.budget > input.committed ? input.budget - input.committed : 0;
  result.proportionalLimitBytes = adaptive.trusted ? (input.budget / 8) * input.quotaEighths : 0;
  result.reserveLimitBytes = adaptive.headroom;
  // Saturating addition: physical headroom already excludes current backing.
  const uint64_t growthLimit = input.resident > UINT64_MAX - adaptive.headroom
      ? UINT64_MAX : input.resident + adaptive.headroom;
  result.effectiveResidentLimitBytes = std::min(adaptive.target, growthLimit);
  return result;
}

inline bool ShadowArenaCanGrowResident(
    uint64_t residentBytes, uint64_t growthBytes,
    uint64_t effectiveResidentLimitBytes) noexcept {
  return growthBytes <= effectiveResidentLimitBytes &&
      residentBytes <= effectiveResidentLimitBytes - growthBytes;
}

inline uint64_t ShadowArenaGrowthHeadroom(
    uint64_t residentBytes, uint64_t effectiveResidentLimitBytes) noexcept {
  return residentBytes < effectiveResidentLimitBytes
      ? effectiveResidentLimitBytes - residentBytes
      : 0u;
}

} // namespace dxvk::war3::memory
