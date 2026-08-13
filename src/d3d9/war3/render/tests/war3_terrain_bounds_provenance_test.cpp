#include "../war3_terrain_bounds_provenance.h"

#include <iostream>

namespace {

namespace policy = dxvk::war3::render;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_terrain_bounds_provenance_test: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  const auto nonIndexed = policy::War3ResolveTerrainBoundsVertexRange(
      false, 17u, 23u, false, 999u, 999u);
  const auto indexedWithoutProof =
      policy::War3ResolveTerrainBoundsVertexRange(
          true, 17u, 23u, false, 300u, 40u);
  const auto indexedExact = policy::War3ResolveTerrainBoundsVertexRange(
      true, 17u, 23u, true, 300u, 40u);
  const auto indexedEmpty = policy::War3ResolveTerrainBoundsVertexRange(
      true, 17u, 23u, true, 300u, 0u);
  const auto exactHint =
      policy::War3EvaluateTerrainIndexedHintAgainstExactDomain(
          {-4, 4u, 6u, 32u, 0u, 6u, true});
  const auto supersetHint =
      policy::War3EvaluateTerrainIndexedHintAgainstExactDomain(
          {-4, 4u, 10u, 32u, 2u, 6u, true});
  const auto underHint =
      policy::War3EvaluateTerrainIndexedHintAgainstExactDomain(
          {-4, 6u, 4u, 32u, 0u, 6u, true});
  const auto negativeHint =
      policy::War3EvaluateTerrainIndexedHintAgainstExactDomain(
          {-8, 4u, 8u, 32u, 0u, 6u, true});
  const auto overflowHint =
      policy::War3EvaluateTerrainIndexedHintAgainstExactDomain(
          {0, 30u, 8u, 32u, 30u, 2u, true});
  const auto unknownExact =
      policy::War3EvaluateTerrainIndexedHintAgainstExactDomain(
          {0, 0u, 8u, 32u, 0u, 6u, false});
  const auto conservativeRange =
      policy::War3ResolveConservativeTerrainIndexedHintRange(
          -4, 6u, 8u, 32u);
  const auto invalidConservativeRange =
      policy::War3ResolveConservativeTerrainIndexedHintRange(
          -8, 4u, 8u, 32u);
  const auto declaredExact =
      policy::War3ResolveTerrainIndexedDeclaredDomain(
          -4, 4u, 6u, 32u, 2u);
  const auto declaredSuperset =
      policy::War3ResolveTerrainIndexedDeclaredDomain(
          -4, 4u, 10u, 32u, 2u);
  const auto declaredNegative =
      policy::War3ResolveTerrainIndexedDeclaredDomain(
          -8, 4u, 8u, 32u, 2u);
  const auto declaredPositionOverflow =
      policy::War3ResolveTerrainIndexedDeclaredDomain(
          0, 30u, 8u, 32u, 2u);
  const auto declaredUint16Overflow =
      policy::War3ResolveTerrainIndexedDeclaredDomain(
          -65535, 65535u, 2u, 32u, 2u);
  const auto declaredUint32Boundary =
      policy::War3ResolveTerrainIndexedDeclaredDomain(
          -2147483647, 0x7fffffffu, 0x80000001u,
          0x80000001u, 4u);
  const auto declaredEmpty =
      policy::War3ResolveTerrainIndexedDeclaredDomain(
          0, 0u, 0u, 32u, 2u);
  uint32_t auditCount = 0u;
  for (uint64_t frame = 1u; frame <= 4096u; ++frame) {
    auditCount += policy::War3ShouldAuditTerrainIndexedHint(
        frame, 0x1234u, 12u, 48u) ? 1u : 0u;
  }

  return require(nonIndexed.exact && nonIndexed.firstVertex == 17u &&
                     nonIndexed.vertexCount == 23u,
                 "non-indexed exact draw range was not preserved") &&
      require(!indexedWithoutProof.exact &&
                  indexedWithoutProof.vertexCount == 0u,
              "indexed D3D9 hints incorrectly authorized bounds") &&
      require(indexedExact.exact && indexedExact.firstVertex == 300u &&
                  indexedExact.vertexCount == 40u,
              "scanned indexed domain was not selected") &&
      require(!indexedEmpty.exact,
              "empty indexed domain incorrectly authorized bounds") &&
      require(exactHint.relation ==
                  policy::War3TerrainIndexedHintRelation::Exact &&
                  exactHint.firstVertex == 0u &&
                  exactHint.vertexCount == 6u,
              "exact D3D9 hint was not recognized") &&
      require(supersetHint.relation ==
                  policy::War3TerrainIndexedHintRelation::ConservativeSuperset,
              "conservative D3D9 hint was not recognized") &&
      require(underHint.relation ==
                  policy::War3TerrainIndexedHintRelation::UnderCoversExactDomain,
              "under-covering D3D9 hint was not rejected") &&
      require(negativeHint.relation ==
                  policy::War3TerrainIndexedHintRelation::Invalid &&
                  overflowHint.relation ==
                      policy::War3TerrainIndexedHintRelation::Invalid &&
                  unknownExact.relation ==
                      policy::War3TerrainIndexedHintRelation::Invalid,
              "invalid D3D9 hint domain was accepted") &&
      require(conservativeRange.exact &&
                  conservativeRange.firstVertex == 2u &&
                  conservativeRange.vertexCount == 8u &&
                  !invalidConservativeRange.exact,
              "conservative D3D9 range validation failed") &&
      require(declaredExact.valid && declaredExact.firstVertex == 0u &&
                  declaredExact.vertexCount == 6u &&
                  declaredExact.minIndex == 4u &&
                  declaredExact.maxIndex == 9u,
              "exact declared D3D9 access domain was not resolved") &&
      require(declaredSuperset.valid &&
                  declaredSuperset.firstVertex == 0u &&
                  declaredSuperset.vertexCount == 10u &&
                  declaredSuperset.minIndex == 4u &&
                  declaredSuperset.maxIndex == 13u,
              "conservative declared D3D9 access domain was not resolved") &&
      require(!declaredNegative.valid &&
                  !declaredPositionOverflow.valid &&
                  !declaredUint16Overflow.valid &&
                  !declaredEmpty.valid,
              "invalid declared D3D9 access domain was accepted") &&
      require(declaredUint32Boundary.valid &&
                  declaredUint32Boundary.firstVertex == 0u &&
                  declaredUint32Boundary.maxIndex == 0xffffffffu,
              "uint32 declared domain boundary was rejected") &&
      require(auditCount > 32u && auditCount < 96u,
              "deterministic 1/64 audit cadence drifted")
      ? 0
      : 1;
}
