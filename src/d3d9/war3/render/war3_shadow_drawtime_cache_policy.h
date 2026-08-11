#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Draw-time cache GC runs on a fixed cadence.  Warcraft can skip or defer a
// native world-producer batch across a Present boundary, so a protected
// static working set must survive at least one complete GC interval.  Two
// intervals cover one missed producer batch without turning the 64 MiB
// inactive-residency target into an unbounded cache.
constexpr uint64_t kWar3ShadowDrawTimeVBCacheGcIntervalFrames = 60u;
constexpr uint64_t kWar3ShadowDrawTimeStaticRecentProtectFrames =
    2u * kWar3ShadowDrawTimeVBCacheGcIntervalFrames;

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
