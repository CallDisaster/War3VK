#include "../war3_shadow_palette_storage.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using dxvk::Matrix4;
using dxvk::Vector4;

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_shadow_palette_storage_test: " << message << '\n';
  return condition;
}

bool matrixEqual(const Matrix4& a, const Matrix4& b) {
  for (uint32_t row = 0u; row < 4u; ++row) {
    for (uint32_t column = 0u; column < 4u; ++column) {
      if (a[row][column] != b[row][column])
        return false;
    }
  }
  return true;
}

bool testPrefixAndIdentityTail() {
  std::array<Matrix4, 3u> source = {
      Matrix4(2.0f),
      Matrix4(Vector4(1.0f, 2.0f, 3.0f, 4.0f),
              Vector4(5.0f, 6.0f, 7.0f, 8.0f),
              Vector4(9.0f, 10.0f, 11.0f, 12.0f),
              Vector4(13.0f, 14.0f, 15.0f, 16.0f)),
      Matrix4(0.0f),
  };
  std::array<Matrix4,
             dxvk::war3::render::kWar3ShadowPaletteMatrixCapacity>
      destination;
  for (auto& matrix : destination)
    matrix = Matrix4(-7.0f);

  dxvk::war3::render::War3ExpandShadowPaletteForUpload(
      destination.data(), source.data(), uint32_t(source.size()));
  for (size_t i = 0u; i < source.size(); ++i) {
    if (!require(matrixEqual(destination[i], source[i]),
                 "live prefix changed during expansion"))
      return false;
  }
  const Matrix4 identity;
  for (size_t i = source.size(); i < destination.size(); ++i) {
    if (!require(matrixEqual(destination[i], identity),
                 "unused tail was not restored to identity"))
      return false;
  }
  return true;
}

bool testBounds() {
  using dxvk::war3::render::War3BoundShadowPaletteMatrixCount;
  using dxvk::war3::render::kWar3ShadowPaletteMatrixCapacity;
  return require(War3BoundShadowPaletteMatrixCount(0u) == 0u,
                 "zero count changed") &&
      require(War3BoundShadowPaletteMatrixCount(17u) == 17u,
              "live count changed") &&
      require(War3BoundShadowPaletteMatrixCount(999u) ==
                  kWar3ShadowPaletteMatrixCapacity,
              "oversized count was not clamped");
}

} // namespace

int main() {
  return testPrefixAndIdentityTail() && testBounds() ? 0 : 1;
}
