#include "../war3_current_draw_palette_hash.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using dxvk::Matrix4;
using dxvk::Vector4;
using dxvk::war3::render::CurrentDrawPaletteHashSummary;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_current_draw_palette_hash_test: " << message << '\n';
  return condition;
}

bool testStreamingHashMatchesByteOracle() {
  const std::array<Matrix4, 3> matrices = {
      Matrix4(),
      Matrix4(Vector4(1.25f, -2.0f, 3.5f, 0.0f),
              Vector4(4.0f, 5.25f, -6.5f, 0.0f),
              Vector4(7.75f, 8.0f, 9.5f, 0.0f),
              Vector4(-10.0f, 11.25f, 12.5f, 1.0f)),
      Matrix4(0.0f),
  };

  CurrentDrawPaletteHashSummary summary = {};
  for (const auto& matrix : matrices)
    summary.include(matrix);

  const uint64_t expected = dxvk::bit::fnv1a_hash(
      reinterpret_cast<const unsigned char*>(matrices.data()),
      matrices.size() * sizeof(Matrix4));
  if (!require(summary.finish() == expected,
               "streaming hash differs from the historical byte oracle"))
    return false;

  CurrentDrawPaletteHashSummary empty = {};
  return require(
      empty.finish() == dxvk::bit::fnv1a_hash(
                            static_cast<const unsigned char*>(nullptr), 0u),
      "empty streaming hash differs from the historical byte oracle");
}

} // namespace

int main() {
  return testStreamingHashMatchesByteOracle() ? 0 : 1;
}
