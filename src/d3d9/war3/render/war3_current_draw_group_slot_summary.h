#pragma once

#include "../../../util/util_bit.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxvk::war3::render {

// Pure accumulator for the current-draw group-slot stream.  The producer
// already visits every slot while validating it, so stable identity and the
// palette upper bound must be derived from that same pass rather than by
// rescanning the decoded vector later in BuildEligible.
struct CurrentDrawGroupSlotSummary {
  uint64_t slotHash = bit::fnv1a_init();
  uint32_t maxGroupSlot = 0u;

  void include(uint32_t groupSlot) noexcept {
    slotHash = bit::fnv1a_iter(slotHash, groupSlot);
    maxGroupSlot = std::max(maxGroupSlot, groupSlot);
  }

  uint64_t diagnosticHash(uintptr_t streamAddress,
                          uint32_t streamStep,
                          uint32_t payloadWord48,
                          uint32_t payloadWord108,
                          uint32_t payloadWord11C,
                          uint32_t layerIndex) const noexcept {
    uint64_t hash = slotHash;
    hash = bit::fnv1a_iter(hash, streamAddress);
    hash = bit::fnv1a_iter(hash, streamStep);
    hash = bit::fnv1a_iter(hash, payloadWord48);
    hash = bit::fnv1a_iter(hash, payloadWord108);
    hash = bit::fnv1a_iter(hash, payloadWord11C);
    hash = bit::fnv1a_iter(hash, layerIndex);
    return hash;
  }

  uint64_t stableHash(uint32_t streamStep,
                      uint32_t payloadWord48,
                      uint32_t payloadWord108,
                      uint32_t payloadWord11C,
                      uint32_t layerIndex) const noexcept {
    uint64_t hash = slotHash;
    hash = bit::fnv1a_iter(hash, streamStep);
    hash = bit::fnv1a_iter(hash, payloadWord48);
    hash = bit::fnv1a_iter(hash, payloadWord108);
    hash = bit::fnv1a_iter(hash, payloadWord11C);
    hash = bit::fnv1a_iter(hash, layerIndex);
    return hash;
  }
};

// Optional proof supplied by an immutable, generation-backed geoset.  A raw
// stream address is never sufficient: the current draw bytes must still match
// the complete immutable slot array before its precomputed summary can replace
// the per-vertex hash/max pass.  The caller retains ownership of bytes for the
// duration of the query; this value grants no cross-frame lifetime.
struct CurrentDrawImmutableGroupSlotHint {
  const uint8_t* bytes = nullptr;
  size_t count = 0u;
  uint64_t slotHash = 0u;
  uint32_t maxGroupSlot = 0u;
  uint64_t immutableGeneration = 0u;

  bool matches(const uint8_t* currentBytes,
               size_t currentCount,
               uint32_t paletteCount) const noexcept {
    return immutableGeneration != 0u && bytes != nullptr &&
        currentBytes != nullptr && count == currentCount && count != 0u &&
        maxGroupSlot < paletteCount &&
        std::memcmp(bytes, currentBytes, count) == 0;
  }

  CurrentDrawGroupSlotSummary summary() const noexcept {
    CurrentDrawGroupSlotSummary result = {};
    result.slotHash = slotHash;
    result.maxGroupSlot = maxGroupSlot;
    return result;
  }
};

} // namespace dxvk::war3::render
