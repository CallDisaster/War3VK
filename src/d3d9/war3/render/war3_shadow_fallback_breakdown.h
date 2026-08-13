#pragma once

#include "war3_render_objects.h"
#include "war3_render_state.h"

#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

// Live fallback counters describe the current scene vector, whereas
// fallbackSnapshotCount is an independent capture-total statistic. Keep the
// current-vector bookkeeping O(1) on append and reserve a full scan for the
// only operation that can remove entries.
template <typename Stats>
inline void War3ResetShadowFallbackBreakdown(
    Stats& stats, size_t liveFallbackCount) noexcept {
  stats.fallbackDrawCount = static_cast<uint32_t>(liveFallbackCount);
  stats.fallbackDrawCountTerrain = 0u;
  stats.fallbackDrawCountWorldObject = 0u;
  stats.fallbackDrawCountUnitObject = 0u;
}

template <typename Stats>
inline void War3AccumulateShadowFallbackClassification(
    Stats& stats, War3RenderState::StageCategory category,
    uint8_t objectKind) noexcept {
  if (category == War3RenderState::StageCategory::Terrain)
    ++stats.fallbackDrawCountTerrain;
  if (category == War3RenderState::StageCategory::WorldObject ||
      category == War3RenderState::StageCategory::Effect) {
    ++stats.fallbackDrawCountWorldObject;
  }
  // Object kind and render category are independent evidence. A world-object
  // unit intentionally contributes to both counters, matching the historical
  // full-scan diagnostics.
  if (objectKind == static_cast<uint8_t>(ObjectKind::Unit))
    ++stats.fallbackDrawCountUnitObject;
}

template <typename Stats>
inline void War3NoteShadowFallbackAppended(
    Stats& stats, size_t liveFallbackCount,
    War3RenderState::StageCategory category, uint8_t objectKind) noexcept {
  stats.fallbackDrawCount = static_cast<uint32_t>(liveFallbackCount);
  War3AccumulateShadowFallbackClassification(stats, category, objectKind);
}

} // namespace dxvk::war3::render
