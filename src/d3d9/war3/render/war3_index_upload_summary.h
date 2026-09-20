#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace dxvk::war3::render {
// Candidate only. It changes no index bytes, GPU copies, barriers or lifetimes.
inline bool Stage11UploadRangeEnabled() noexcept {
  static const bool enabled = [] {
    const char* v = std::getenv("DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE");
    return v && v[0] == '1' && v[1] == '\0';
  }();
  return enabled;
}
struct IndexUploadBudget {
  uint64_t frame = UINT64_MAX, serial = 0;
  uint32_t bytes = 0;
  uint64_t copiedWithProof = 0, bypassed = 0;
  uint64_t next(uint64_t currentFrame) {
    if (frame != currentFrame) { frame = currentFrame; bytes = 0; }
    if (serial == UINT64_MAX) return 0; // permanently unprovable on wrap
    return ++serial;
  }
};
class IndexUploadSummary {
public:
  struct Range { uint32_t min = 0, max = 0; uint64_t hash = 0; bool valid = false; };
  // Same preconditions as the memcpy this replaces: source/destination cover
  // bytes, do not overlap, and source is the input actually used by the upload.
  static IndexUploadSummary Copy(void* dst, const void* src, uint32_t bytes,
      uint32_t width, uintptr_t allocation, uint64_t map, uint64_t serial,
      IndexUploadBudget& budget, bool enabled) {
    IndexUploadSummary out;
    constexpr uint32_t PerDraw = 16u * 1024u, PerFrame = 256u * 1024u;
    if (!enabled || !allocation || !serial || !bytes || (width != 2 && width != 4) ||
        bytes % width || bytes > PerDraw || budget.bytes > PerFrame || bytes > PerFrame - budget.bytes) {
      std::memcpy(dst, src, bytes);
      if (enabled) ++budget.bypassed;
      return out;
    }
    // Never scan write-combined UP output. Read the original source exactly
    // once into small cached scratch; BOTH uploaded bytes and summary use it.
    std::array<uint8_t, 1024> scratch;
    Range range{UINT32_MAX, 0, 0xcbf29ce484222325ull, true};
    for (uint32_t offset = 0; offset < bytes;) {
      const uint32_t n = std::min(uint32_t(scratch.size()), bytes - offset);
      std::memcpy(scratch.data(), static_cast<const uint8_t*>(src) + offset, n);
      std::memcpy(static_cast<uint8_t*>(dst) + offset, scratch.data(), n);
      for (uint32_t i = 0; i < n; i += width) {
        uint32_t value = 0;
        if (width == 2) { uint16_t v; std::memcpy(&v, scratch.data() + i, 2); value = v; }
        else std::memcpy(&value, scratch.data() + i, 4);
        range.min = std::min(range.min, value); range.max = std::max(range.max, value);
        range.hash = (range.hash ^ value) * 0x100000001b3ull;
      }
      offset += n;
    }
    budget.bytes += bytes; ++budget.copiedWithProof;
    out.m_range = range; out.m_destination = dst; out.m_bytes = bytes; out.m_width = width;
    out.m_allocation = allocation; out.m_map = map; out.m_serial = serial;
    return out;
  }
  Range query(const void* destination, uint64_t bytes, uint32_t width,
      uintptr_t allocation, uint64_t map, uint64_t serial) const {
    if (!m_range.valid || !serial || destination != m_destination || bytes != m_bytes ||
        width != m_width || allocation != m_allocation || map != m_map || serial != m_serial)
      return {};
    return m_range;
  }
private:
  Range m_range{};
  const void* m_destination = nullptr; // identity only, never dereferenced
  uint64_t m_map = 0, m_serial = 0;
  uintptr_t m_allocation = 0;
  uint32_t m_bytes = 0, m_width = 0;
};
} // namespace dxvk::war3::render
