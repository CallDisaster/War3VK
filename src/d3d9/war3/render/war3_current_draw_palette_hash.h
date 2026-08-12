#pragma once

#include "../../../util/util_bit.h"
#include "../../../util/util_matrix.h"

#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

// Streaming form of bit::fnv1a_hash for decoded Matrix4 palettes. CurrentDraw
// already writes every decoded matrix once, so deriving the content identity
// in that pass avoids walking the complete palette again in BuildEligible.
// The byte packing and final size mix deliberately match util_bit.h exactly.
struct CurrentDrawPaletteHashSummary {
  uint64_t hash = bit::fnv1a_init();
  size_t byteCount = 0u;

  void include(const Matrix4& matrix) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&matrix);
    static_assert(sizeof(Matrix4) % sizeof(uint64_t) == 0u);
    for (size_t offset = 0u; offset < sizeof(Matrix4);
         offset += sizeof(uint64_t)) {
      uint64_t value = 0u;
      for (size_t byte = 0u; byte < sizeof(uint64_t); ++byte)
        value |= uint64_t(bytes[offset + byte]) << (8u * byte);
      hash = bit::fnv1a_iter(hash, value);
    }
    byteCount += sizeof(Matrix4);
  }

  uint64_t finish() const noexcept {
    return bit::fnv1a_iter(hash, byteCount);
  }
};

} // namespace dxvk::war3::render
