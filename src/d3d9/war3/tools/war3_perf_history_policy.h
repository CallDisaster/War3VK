#pragma once

#include <algorithm>
#include <cstddef>

namespace dxvk::war3 {

// FrameSnapshot owns per-frame section and timestamp vectors in addition to
// its fixed counters.  War3 is a 32-bit process, so an unbounded environment
// override can exhaust virtual address space long before physical memory is
// exhausted.  Four thousand frames covers roughly one minute at 60 FPS and
// has passed the integrated high-pressure and volumetric gates.
inline constexpr size_t kWar3PerfHistoryFrameLimit32 = 4000u;

constexpr size_t ClampWar3PerfHistoryFrames(size_t requested) noexcept {
  return std::min(requested, kWar3PerfHistoryFrameLimit32);
}

} // namespace dxvk::war3
