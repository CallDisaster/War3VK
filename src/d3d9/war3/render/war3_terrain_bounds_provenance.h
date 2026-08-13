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

enum class War3TerrainIndexedHintRelation : uint8_t {
  Invalid = 0u,
  Exact,
  ConservativeSuperset,
  UnderCoversExactDomain,
};

struct War3TerrainIndexedHintEvidence {
  int32_t baseVertex = 0;
  uint32_t minVertexIndex = 0u;
  uint32_t numVertices = 0u;
  uint32_t vertexCapacity = 0u;
  uint32_t exactFirstVertex = 0u;
  uint32_t exactVertexCount = 0u;
  bool exactDomainKnown = false;
};

struct War3TerrainIndexedHintDecision {
  War3TerrainIndexedHintRelation relation =
      War3TerrainIndexedHintRelation::Invalid;
  uint32_t firstVertex = 0u;
  uint32_t vertexCount = 0u;
};

// DrawIndexedPrimitive declares that every raw index addressed by the draw is
// in [MinVertexIndex, MinVertexIndex + NumVertices). This value contract turns
// that declared access domain into the compact position slice used by the
// development coherent-REAL replay candidate. It deliberately does not prove
// the tighter, actual min/max domain required by general-purpose culling.
struct War3TerrainIndexedDeclaredDomain {
  uint32_t firstVertex = 0u;
  uint32_t vertexCount = 0u;
  uint32_t minIndex = 0u;
  uint32_t maxIndex = 0u;
  bool valid = false;
};

inline constexpr uint32_t kWar3TerrainIndexedHintAuditPeriod = 64u;

constexpr War3TerrainBoundsVertexRange
War3ResolveConservativeTerrainIndexedHintRange(
    int32_t baseVertex, uint32_t minVertexIndex, uint32_t numVertices,
    uint32_t vertexCapacity) {
  if (numVertices == 0u || vertexCapacity == 0u)
    return {};
  const int64_t first = int64_t(baseVertex) + int64_t(minVertexIndex);
  const int64_t end = first + int64_t(numVertices);
  if (first < 0 || end <= first || end > int64_t(vertexCapacity))
    return {};
  return {uint32_t(first), numVertices, true};
}

constexpr War3TerrainIndexedDeclaredDomain
War3ResolveTerrainIndexedDeclaredDomain(
    int32_t baseVertex, uint32_t minVertexIndex, uint32_t numVertices,
    uint32_t vertexCapacity, uint32_t indexElementBytes) {
  if (numVertices == 0u || vertexCapacity == 0u ||
      (indexElementBytes != 2u && indexElementBytes != 4u))
    return {};

  const uint64_t rawEnd = uint64_t(minVertexIndex) + uint64_t(numVertices);
  const uint64_t rawDomainLimit = indexElementBytes == 2u
      ? (uint64_t(1u) << 16u)
      : (uint64_t(1u) << 32u);
  if (rawEnd <= uint64_t(minVertexIndex) || rawEnd > rawDomainLimit)
    return {};

  const int64_t first = int64_t(baseVertex) + int64_t(minVertexIndex);
  const int64_t end = first + int64_t(numVertices);
  if (first < 0 || end <= first || end > int64_t(vertexCapacity))
    return {};

  return {uint32_t(first), numVertices, minVertexIndex,
          uint32_t(rawEnd - 1u), true};
}

constexpr bool War3ShouldAuditTerrainIndexedHint(
    uint64_t frameSerial, uintptr_t ownerIdentity, uint32_t startIndex,
    uint32_t indexCount) {
  if (frameSerial == 0u || ownerIdentity == 0u || indexCount == 0u)
    return false;
  uint64_t value = frameSerial;
  value ^= uint64_t(ownerIdentity) + 0x9e3779b97f4a7c15ull +
      (value << 6u) + (value >> 2u);
  value ^= uint64_t(startIndex) | (uint64_t(indexCount) << 32u);
  value ^= value >> 33u;
  value *= 0xff51afd7ed558ccdull;
  value ^= value >> 33u;
  return (value & uint64_t(kWar3TerrainIndexedHintAuditPeriod - 1u)) == 0u;
}

// D3D9 documents MinVertexIndex/NumVertices as the vertex range used by the
// indexed draw. This observer compares that caller-supplied range against a
// current-generation exact IB scan before any future use as culling proof.
// A larger containing range is conservative; any under-coverage is unsafe.
constexpr War3TerrainIndexedHintDecision
War3EvaluateTerrainIndexedHintAgainstExactDomain(
    const War3TerrainIndexedHintEvidence& evidence) {
  if (!evidence.exactDomainKnown || evidence.exactVertexCount == 0u ||
      evidence.numVertices == 0u || evidence.vertexCapacity == 0u)
    return {};

  const int64_t hintFirst = int64_t(evidence.baseVertex) +
      int64_t(evidence.minVertexIndex);
  const int64_t hintEnd = hintFirst + int64_t(evidence.numVertices);
  const uint64_t exactFirst = uint64_t(evidence.exactFirstVertex);
  const uint64_t exactEnd = exactFirst + uint64_t(evidence.exactVertexCount);
  if (hintFirst < 0 || hintEnd <= hintFirst ||
      hintEnd > int64_t(evidence.vertexCapacity) ||
      exactEnd > uint64_t(evidence.vertexCapacity))
    return {};

  War3TerrainIndexedHintDecision decision = {};
  decision.firstVertex = uint32_t(hintFirst);
  decision.vertexCount = evidence.numVertices;
  if (uint64_t(hintFirst) == exactFirst &&
      uint64_t(hintEnd) == exactEnd) {
    decision.relation = War3TerrainIndexedHintRelation::Exact;
  } else if (uint64_t(hintFirst) <= exactFirst &&
             uint64_t(hintEnd) >= exactEnd) {
    decision.relation =
        War3TerrainIndexedHintRelation::ConservativeSuperset;
  } else {
    decision.relation =
        War3TerrainIndexedHintRelation::UnderCoversExactDomain;
  }
  return decision;
}

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
static_assert(std::is_standard_layout_v<War3TerrainIndexedHintEvidence>);
static_assert(std::is_trivially_copyable_v<War3TerrainIndexedHintEvidence>);
static_assert(std::is_standard_layout_v<War3TerrainIndexedHintDecision>);
static_assert(std::is_trivially_copyable_v<War3TerrainIndexedHintDecision>);
static_assert(std::is_standard_layout_v<War3TerrainIndexedDeclaredDomain>);
static_assert(std::is_trivially_copyable_v<War3TerrainIndexedDeclaredDomain>);
static_assert((kWar3TerrainIndexedHintAuditPeriod &
               (kWar3TerrainIndexedHintAuditPeriod - 1u)) == 0u);

} // namespace dxvk::war3::render
