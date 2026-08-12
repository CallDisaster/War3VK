#include "../war3_palette_pack.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_palette_pack_test: " << message << '\n';
  return condition;
}

bool testHistoricalByteLayout() {
  const float rows[4][4] = {
      {1.0f, 2.0f, 3.0f, 101.0f},
      {4.0f, 5.0f, 6.0f, 102.0f},
      {7.0f, 8.0f, 9.0f, 103.0f},
      {10.0f, 11.0f, 12.0f, 104.0f},
  };
  const dxvk::Matrix4 matrix(rows);
  std::array<uint8_t,
             dxvk::war3::model::kWar3PackedPaletteMatrixBytes> packed = {};
  dxvk::war3::model::PackWar3PaletteMatrix3x4(matrix, packed.data());

  const std::array<float, 12> expected = {
      1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f,
      7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f,
  };
  return require(std::memcmp(packed.data(), expected.data(), packed.size()) == 0,
                 "packed 3x4 bytes changed from the historical layout");
}

} // namespace

int main() {
  return testHistoricalByteLayout() ? 0 : 1;
}
