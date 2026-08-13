#pragma once

#include "../../../util/util_matrix.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxvk::war3::model {

constexpr size_t kWar3PackedPaletteMatrixBytes = 48u;

// War3's blended palette exposes four rows with three stored components per
// row. Copy by row so the byte destination does not need float alignment.
inline void PackWar3PaletteMatrix3x4(const Matrix4& matrix,
                                     uint8_t* destination) noexcept {
  for (size_t row = 0u; row < 4u; ++row) {
    std::memcpy(destination + row * 3u * sizeof(float),
                &matrix[row][0], 3u * sizeof(float));
  }
}

} // namespace dxvk::war3::model
