#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace dxvk::war3::render {

template <size_t FailureCount>
inline uint64_t DeriveClassifiedSuccessCount(
    uint64_t attemptCount,
    const std::array<uint64_t, FailureCount>& failureCounts,
    uint64_t additionalFailureCount = 0u) noexcept {
  uint64_t totalFailures = additionalFailureCount;
  for (const uint64_t failureCount : failureCounts) {
    if (failureCount > (std::numeric_limits<uint64_t>::max)() - totalFailures)
      return 0u;
    totalFailures += failureCount;
  }
  return attemptCount >= totalFailures ? attemptCount - totalFailures : 0u;
}

} // namespace dxvk::war3::render
