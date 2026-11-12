#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Resolution of the directional shadow guide that sharpens volumetric shaft
// edges. The guide is integrated per pixel through `depth` froxel slices, so
// its cost is bounded by width/d x height/d x depth cells.
//
// The guide used to be all-or-nothing: half resolution when that fitted the
// cell budget, otherwise the effect resolution itself. At 2560x1440 High the
// half-resolution guide is 118M cells against a 100M budget, so the guide
// fell from 1/2 straight to 1/8 and shafts lost their edge refinement
// entirely. Picking the finest divisor that fits keeps the same budget and
// gives 1/3 at 1440p and 1/4 at 4K.
//
// Returns `effectDivisor` (no refinement) when nothing finer fits.
constexpr uint32_t War3SelectDirectionalGuideDivisor(
    uint32_t width, uint32_t height, uint32_t depth, uint32_t targetDivisor,
    uint32_t effectDivisor, uint64_t cellBudget) noexcept {
  if (targetDivisor == 0u || depth == 0u)
    return effectDivisor;
  for (uint32_t divisor = targetDivisor; divisor < effectDivisor; ++divisor) {
    const uint64_t guideWidth = (uint64_t(width) + divisor - 1u) / divisor;
    const uint64_t guideHeight = (uint64_t(height) + divisor - 1u) / divisor;
    // width, height < 2^32, so the product fits; divide the budget instead
    // of multiplying by depth to stay overflow-free.
    if (guideWidth * guideHeight <= cellBudget / uint64_t(depth))
      return divisor;
  }
  return effectDivisor;
}

} // namespace dxvk::war3::render
