#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxvk::war3::tools::screenshot {

constexpr uint32_t SlotCount = 3;
constexpr uint64_t MaxBytesPerSlot = 32ull * 1024 * 1024;
enum class State : uint32_t { Retired, Free, Requested, Preparing, Submitted, Quarantined };

inline bool Claim(std::atomic<State>& state, State from, State to) {
  return state.compare_exchange_strong(from, to, std::memory_order_acq_rel);
}

inline bool Layout(uint32_t width, uint32_t height, uint64_t& bytes) {
  bytes = 0;
  if (!width || !height || width > 8192 || height > 8192) return false;
  bytes = uint64_t(width) * height * 4;
  return bytes <= MaxBytesPerSlot;
}

// No frame-delay heuristic: a valid completion query AND this slot's exact
// submitted value are mandatory. A query error never authorizes a mapped read.
inline bool Readable(bool queryOk, bool deviceOk, uint64_t completed, uint64_t target) {
  return queryOk && deviceOk && target && completed >= target;
}

inline std::array<uint8_t, 18> TgaHeader(uint32_t width, uint32_t height) {
  std::array<uint8_t, 18> h{};
  h[2] = 2; // Uncompressed true-color, BGRA byte order.
  h[12] = uint8_t(width); h[13] = uint8_t(width >> 8);
  h[14] = uint8_t(height); h[15] = uint8_t(height >> 8);
  h[16] = 32; h[17] = 0x28; // Top-left origin, eight alpha bits.
  return h;
}

inline void OpaqueRow(uint8_t* dst, const uint8_t* src, uint32_t width) {
  std::memcpy(dst, src, size_t(width) * 4);
  // Warcraft's native readback also forces opaque alpha; X8 alpha is undefined.
  for (uint32_t x = 0; x < width; ++x) dst[size_t(x) * 4 + 3] = 255;
}

// Shared by the real worker and CPU tests (writer/cancellation fault injection).
// Caller must prove GPU completion before supplying pixels. No full-frame copy.
template<typename Writer, typename Cancelled>
bool EncodeTga(uint32_t width, uint32_t height, const uint8_t* pixels,
    size_t pitch, Writer&& write, Cancelled&& cancelled) {
  uint64_t bytes = 0;
  if (!Layout(width, height, bytes) || !pixels || pitch < size_t(width) * 4 ||
      pitch > SIZE_MAX / height || cancelled()) return false;
  const auto header = TgaHeader(width, height);
  if (!write(header.data(), header.size())) return false;
  std::array<uint8_t, 8192 * 4> row;
  for (uint32_t y = 0; y < height; ++y) {
    if (cancelled()) return false;
    OpaqueRow(row.data(), pixels + size_t(y) * pitch, width);
    if (!write(row.data(), size_t(width) * 4)) return false;
  }
  return !cancelled();
}

} // namespace dxvk::war3::tools::screenshot
