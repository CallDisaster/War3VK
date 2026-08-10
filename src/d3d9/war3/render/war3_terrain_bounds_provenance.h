#pragma once

#include <cstdint>
#include <type_traits>

namespace dxvk::war3::render {

// D3D9's MinVertexIndex/NumVertices pair is a caller-supplied optimization
// hint. It cannot prove the vertex domain referenced by an indexed draw.
// Directional-shadow culling may use an indexed range only after the current
// index bytes have been scanned successfully. Non-indexed draws already name
// their exact contiguous range.
struct War3TerrainBoundsVertexRange {
  uint32_t firstVertex = 0u;
  uint32_t vertexCount = 0u;
  bool exact = false;
};

constexpr War3TerrainBoundsVertexRange War3ResolveTerrainBoundsVertexRange(
    bool indexed, uint32_t nonIndexedFirstVertex,
    uint32_t nonIndexedVertexCount, bool exactIndexedDomainKnown,
    uint32_t exactIndexedFirstVertex, uint32_t exactIndexedVertexCount) {
  if (!indexed) {
    return {nonIndexedFirstVertex, nonIndexedVertexCount,
            nonIndexedVertexCount != 0u};
  }

  if (!exactIndexedDomainKnown || exactIndexedVertexCount == 0u)
    return {};

  return {exactIndexedFirstVertex, exactIndexedVertexCount, true};
}

static_assert(std::is_standard_layout_v<War3TerrainBoundsVertexRange>);
static_assert(std::is_trivially_copyable_v<War3TerrainBoundsVertexRange>);

} // namespace dxvk::war3::render
