#pragma once
#include <cstdint>

namespace dxvk::war3::memory {

// UI-only approximate telemetry. Each member is an atomic sample, NOT a
// transaction-wide allocation/retirement proof. No frame-page traversal.
struct ShadowArenaMemoryStats {
  uint64_t usedBytes = 0, residentBytes = 0, residentLimitBytes = 0;
  uint64_t generation = 0, submittedSerial = 0, completedSerial = 0;
  uint64_t overflowCount = 0, admissionRejectedCount = 0;
  uint64_t busyReuseRejectCount = 0, quarantineCount = 0;
  uint64_t budgetBytes = 0, allocatedBytes = 0, availableBytes = 0;
  uint64_t budgetFrameSerial = 0;
  uint32_t activeGenerationCount = 0, frameIncomplete = 0;
  uint32_t budgetSupported = 0, budgetTrusted = 0;
};

ShadowArenaMemoryStats ShadowArena_QueryMemoryStats() noexcept;
}
