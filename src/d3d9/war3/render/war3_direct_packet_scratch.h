#pragma once

#include "war3_current_draw_contract.h"
#include "../shadow/war3_shadow_renderer_core.h"

#include <utility>
#include <vector>

namespace dxvk::war3::render {

constexpr size_t kDirectPacketScratchMaxPaletteMatrices = 256u;
constexpr size_t kDirectPacketScratchMaxVertexEntries = 200000u;

template <typename T>
inline void ClearBoundedDirectPacketScratch(std::vector<T>& values,
                                            size_t maxCapacity) noexcept {
  values.clear();
  if (values.capacity() > maxCapacity)
    std::vector<T>().swap(values);
}

/**
 * Clears a DirectGrouped packet while retaining only caller-owned vector
 * capacity. No geometry bytes, pointer aliases, shared owners, identities or
 * generations survive the reset; the next build must prove and bind all of
 * them again from the current frame.
 */
inline void ResetShadowDrawPacketPreserveScratch(
    shadow::ShadowDrawPacket& packet) noexcept {
  auto ownedVertexGroupIndices =
      std::move(packet.resource.ownedVertexGroupIndices);
  auto ownedVertexBlendWeights =
      std::move(packet.resource.ownedVertexBlendWeights);
  auto ownedVertexBlendIndices =
      std::move(packet.resource.ownedVertexBlendIndices);
  auto posePalette = std::move(packet.pose.matrixPalette);
  auto runtimeGroupPalette = std::move(packet.runtimeGroupPalette);

  packet = {};

  ClearBoundedDirectPacketScratch(
      ownedVertexGroupIndices, kDirectPacketScratchMaxVertexEntries);
  ClearBoundedDirectPacketScratch(
      ownedVertexBlendWeights, kDirectPacketScratchMaxVertexEntries);
  ClearBoundedDirectPacketScratch(
      ownedVertexBlendIndices, kDirectPacketScratchMaxVertexEntries);
  ClearBoundedDirectPacketScratch(
      posePalette, kDirectPacketScratchMaxPaletteMatrices);
  ClearBoundedDirectPacketScratch(
      runtimeGroupPalette, kDirectPacketScratchMaxPaletteMatrices);

  packet.resource.ownedVertexGroupIndices =
      std::move(ownedVertexGroupIndices);
  packet.resource.ownedVertexBlendWeights =
      std::move(ownedVertexBlendWeights);
  packet.resource.ownedVertexBlendIndices =
      std::move(ownedVertexBlendIndices);
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
