#pragma once

#include "../../../util/util_bit.h"

#include <algorithm>
#include <cstdint>

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

} // namespace dxvk::war3::render
