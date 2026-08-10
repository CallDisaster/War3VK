#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// The GC runs before producer/Present handoff can finish observing every
// Stage11 entry.  Protect a deliberately short recent window instead of
// treating the inactive 64 MiB target as permission to evict live geometry.
constexpr uint64_t kWar3ShadowDrawTimeStaticRecentProtectFrames = 2u;

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
