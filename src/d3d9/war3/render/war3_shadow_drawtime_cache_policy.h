#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Warcraft can submit several Present calls between two world-producer
// batches.  Keep the recent static window aligned with the existing 16-frame
// dynamic-cache safety horizon so a still-visible Stage11 working set cannot
// become inactive merely because GC ran between two native world updates.
// This remains deliberately short: the 64 MiB inactive target still governs
// geometry that has actually left the active producer window.
constexpr uint64_t kWar3ShadowDrawTimeStaticRecentProtectFrames = 16u;

constexpr bool IsWar3ShadowDrawTimeStaticWorkingSetProtected(
    uint64_t lastAccessFrame, uint64_t currentFrame) noexcept {
  return lastAccessFrame != 0u && lastAccessFrame <= currentFrame &&
      currentFrame - lastAccessFrame <=
          kWar3ShadowDrawTimeStaticRecentProtectFrames;
}

constexpr uint64_t War3ShadowDrawTimeStaticOverCapBytes(
    uint64_t protectedBytes, uint64_t inactiveTargetBytes) noexcept {
  return protectedBytes > inactiveTargetBytes
      ? protectedBytes - inactiveTargetBytes
      : 0u;
}

} // namespace dxvk::war3::render
