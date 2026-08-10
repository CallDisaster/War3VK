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
              "empty indexed domain incorrectly authorized bounds")
      ? 0
      : 1;
}
