#pragma once

#include "../../../util/util_matrix.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace dxvk::war3::render {

constexpr uint32_t kWar3ShadowPaletteMatrixCapacity = 256u;

inline uint32_t War3BoundShadowPaletteMatrixCount(size_t count) noexcept {
  return static_cast<uint32_t>((std::min)(
      count, size_t(kWar3ShadowPaletteMatrixCapacity)));
}

// The shader ABI keeps a fixed 256-matrix stride. CPU scene storage owns only
// the live prefix; expansion restores the historical identity-filled tail at
// the upload boundary so no uninitialized matrix can become GPU-visible.
inline void War3ExpandShadowPaletteForUpload(
    Matrix4* destination, const Matrix4* source,
    uint32_t sourceCount) noexcept {
  if (destination == nullptr)
    return;

  static const std::array<Matrix4, kWar3ShadowPaletteMatrixCapacity>
      kIdentityTail = {};
  std::memcpy(destination, kIdentityTail.data(), sizeof(kIdentityTail));

  const uint32_t boundedCount = source != nullptr
      ? (std::min)(sourceCount, kWar3ShadowPaletteMatrixCapacity)
      : 0u;
  if (boundedCount != 0u) {
    std::memcpy(destination, source,
                sizeof(Matrix4) * size_t(boundedCount));
  }
}

} // namespace dxvk::war3::render
