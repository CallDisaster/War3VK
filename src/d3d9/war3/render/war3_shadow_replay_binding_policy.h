#pragma once

#include <cstdint>
#include <limits>

namespace dxvk::war3::render {

enum class War3ShadowReplayBindingRejectReason : uint32_t {
  None = 0u,
  MissingCapture,
  EpochMismatch,
  SourceGenerationMissing,
  SourceGenerationMismatch,
  OwnerMismatch,
  BackingMismatchAtCapture,
  RangeOutOfBounds,
};

struct War3ShadowReplayLogicalRange {
  uint64_t offset = 0u;
  uint64_t length = 0u;
  bool valid = false;
};

inline War3ShadowReplayLogicalRange MakeWar3ShadowReplayLogicalRange(
    uint64_t backingOffset, uint64_t backingSize,
    uint64_t capturedOffset, uint64_t capturedLength) noexcept {
  War3ShadowReplayLogicalRange result = {};
  if (capturedLength == 0u || capturedOffset < backingOffset)
    return result;

  const uint64_t localOffset = capturedOffset - backingOffset;
  if (localOffset > backingSize || capturedLength > backingSize - localOffset)
    return result;

  result.offset = localOffset;
  result.length = capturedLength;
  result.valid = true;
  return result;
}

inline bool ResolveWar3ShadowReplayLogicalRange(
    uint64_t backingOffset, uint64_t backingSize,
    const War3ShadowReplayLogicalRange& logical,
    uint64_t& resolvedOffset, uint64_t& resolvedLength) noexcept {
  resolvedOffset = 0u;
  resolvedLength = 0u;
  if (!logical.valid || logical.length == 0u ||
      logical.offset > backingSize ||
      logical.length > backingSize - logical.offset ||
      backingOffset > std::numeric_limits<uint64_t>::max() - logical.offset) {
    return false;
  }

  resolvedOffset = backingOffset + logical.offset;
  resolvedLength = logical.length;
  return true;
}

} // namespace dxvk::war3::render
