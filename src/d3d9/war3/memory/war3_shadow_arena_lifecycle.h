#pragma once

#include <cstdint>

namespace dxvk::war3::memory {

struct ShadowArenaGenerationLease {
  uint64_t frameSerial = 0u;
  uint64_t retireSerial = 0u;
  bool allocatable = false;
};

inline bool ShadowArenaGenerationCanBeReused(
    uint64_t retireSerial, uint64_t completedSerial) noexcept {
  return retireSerial <= completedSerial;
}

inline bool ShadowArenaQuarantineGeneration(
    ShadowArenaGenerationLease& lease, uint64_t retireSerial) noexcept {
  if (!lease.allocatable || lease.frameSerial == 0u || retireSerial == 0u)
    return false;
  lease.retireSerial = retireSerial;
  lease.allocatable = false;
  return true;
}

inline bool ShadowArenaTryAcquireGeneration(
    ShadowArenaGenerationLease& lease, uint64_t frameSerial,
    uint64_t completedSerial) noexcept {
  if (frameSerial == 0u || lease.allocatable ||
      !ShadowArenaGenerationCanBeReused(
          lease.retireSerial, completedSerial)) {
    return false;
  }
  lease.frameSerial = frameSerial;
  lease.retireSerial = 0u;
  lease.allocatable = true;
  return true;
}

} // namespace dxvk::war3::memory
