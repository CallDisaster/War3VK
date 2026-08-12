#pragma once

#include "war3_current_draw_contract.h"
#include "../shadow/war3_shadow_renderer_core.h"

#include <utility>
#include <vector>

namespace dxvk::war3::render {

/**
 * Clears a DirectGrouped packet while retaining only caller-owned vector
 * capacity. No geometry bytes, pointer aliases, shared owners, identities or
 * generations survive the reset; the next build must prove and bind all of
 * them again from the current frame.
 */
inline void ResetShadowDrawPacketPreserveScratch(
    shadow::ShadowDrawPacket& packet) noexcept {
  auto ownedPositions = std::move(packet.resource.ownedPositions);
  auto ownedVertexGroupIndices =
      std::move(packet.resource.ownedVertexGroupIndices);
  auto ownedVertexBlendWeights =
      std::move(packet.resource.ownedVertexBlendWeights);
  auto ownedVertexBlendIndices =
      std::move(packet.resource.ownedVertexBlendIndices);
  auto ownedIndices = std::move(packet.resource.ownedIndices);
  auto ownedMatrixGroupSizes =
      std::move(packet.resource.ownedMatrixGroupSizes);
  auto ownedMatrixIndices = std::move(packet.resource.ownedMatrixIndices);
  auto posePalette = std::move(packet.pose.matrixPalette);
  auto runtimeGroupPalette = std::move(packet.runtimeGroupPalette);

  packet = {};

  ownedPositions.clear();
  ownedVertexGroupIndices.clear();
  ownedVertexBlendWeights.clear();
  ownedVertexBlendIndices.clear();
  ownedIndices.clear();
  ownedMatrixGroupSizes.clear();
  ownedMatrixIndices.clear();
  posePalette.clear();
  runtimeGroupPalette.clear();

  packet.resource.ownedPositions = std::move(ownedPositions);
  packet.resource.ownedVertexGroupIndices =
      std::move(ownedVertexGroupIndices);
  packet.resource.ownedVertexBlendWeights =
      std::move(ownedVertexBlendWeights);
  packet.resource.ownedVertexBlendIndices =
      std::move(ownedVertexBlendIndices);
  packet.resource.ownedIndices = std::move(ownedIndices);
  packet.resource.ownedMatrixGroupSizes =
      std::move(ownedMatrixGroupSizes);
  packet.resource.ownedMatrixIndices = std::move(ownedMatrixIndices);
  packet.pose.matrixPalette = std::move(posePalette);
  packet.runtimeGroupPalette = std::move(runtimeGroupPalette);
}

// Swap last frame's elements into a bounded recycler. After the first warm-up
// exchange both outer allocations remain available, and no O(N) element move
// is charged at frame start. Each acquired element must be reset before use.
template <typename T>
inline void RecycleScratchElements(std::vector<T>& live,
                                   std::vector<T>& recycled) {
  recycled.clear();
  live.swap(recycled);
}

template <typename T>
inline T AcquireScratchElement(std::vector<T>& recycled) {
  if (recycled.empty())
    return {};
  T value = std::move(recycled.back());
  recycled.pop_back();
  return value;
}

} // namespace dxvk::war3::render
