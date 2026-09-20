#pragma once
#include <cstdint>

namespace dxvk::war3::tools::evidence::inputs {
constexpr uint32_t Slots = 576; // >= 256 pre + 16 post frames, two sun passes
constexpr uint32_t BytesPerSlot = 256 * 1024;
constexpr uint32_t DrawsPerSlot = 128;
// Admission accounts for the complete lazily populated input pool, not just
// the first 256-KiB buffer. Production asserts the metadata upper bound.
constexpr uint32_t MaxDrawMetadataBytes = 1088; // Win32 Draw is 1040; round up to 64-byte units
inline constexpr uint64_t HostPayloadBudgetForSlots(uint32_t slots) noexcept {
  return uint64_t(slots) *
    (BytesPerSlot + uint64_t(DrawsPerSlot) * MaxDrawMetadataBytes);
}
constexpr uint64_t HostPayloadBudget = HostPayloadBudgetForSlots(Slots);
enum Status : uint32_t { Absent, Copied, MissingOwner, Usage, Range,
  Capacity, Unsupported, MatrixRange };
inline bool contains(uint64_t base, uint64_t size, uint64_t offset, uint64_t bytes) {
  return bytes && offset >= base && offset-base <= size && bytes <= size-(offset-base);
}
inline bool reserve(uint32_t& used, uint64_t bytes, uint32_t& offset) {
  const uint64_t aligned=(uint64_t(used)+3)&~uint64_t(3);
  if (!bytes || aligned>BytesPerSlot || bytes>BytesPerSlot-aligned) return false;
  offset=uint32_t(aligned);used=uint32_t(aligned+bytes);return true;
}
inline bool focus(uint32_t stage, uint32_t vertices, uint32_t indices) {
  // The native snapshot may bind a full 16384-vertex backing for a tiny draw.
  // Selection by index-count is only prioritization, never identity proof.
  return stage==11 && (indices==105||indices==1587);
}
} // namespace dxvk::war3::tools::evidence::inputs
