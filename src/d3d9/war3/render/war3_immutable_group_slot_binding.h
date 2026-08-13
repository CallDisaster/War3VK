#pragma once

#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

// Pure, allocation-free proof for a packet alias into an immutable geoset.
// Pointer equality alone is insufficient: the retained owner, map/model
// generations and the complete vertex range must all describe the same data.
struct ImmutableGroupSlotBindingProof {
  bool declared = false;
  bool ownerReady = false;
  const void* keepAliveOwner = nullptr;
  const void* resolvedOwner = nullptr;
  const void* packetSlots = nullptr;
  const void* ownerSlots = nullptr;
  uint64_t packetMapEpoch = 0u;
  uint64_t ownerMapEpoch = 0u;
  uint64_t packetImmutableGeneration = 0u;
  uint64_t ownerImmutableGeneration = 0u;
  uint32_t packetGeosetIndex = 0u;
  uint32_t ownerGeosetIndex = 0u;
  size_t requiredVertexCount = 0u;
  size_t ownerSlotCount = 0u;
};

inline bool ValidateImmutableGroupSlotBinding(
    const ImmutableGroupSlotBindingProof& proof) noexcept {
  return proof.declared && proof.ownerReady &&
      proof.keepAliveOwner != nullptr &&
      proof.keepAliveOwner == proof.resolvedOwner &&
      proof.packetSlots != nullptr && proof.packetSlots == proof.ownerSlots &&
      proof.packetMapEpoch != 0u &&
      proof.packetMapEpoch == proof.ownerMapEpoch &&
      proof.packetImmutableGeneration != 0u &&
      proof.packetImmutableGeneration == proof.ownerImmutableGeneration &&
      proof.packetGeosetIndex == proof.ownerGeosetIndex &&
      proof.requiredVertexCount != 0u &&
      proof.ownerSlotCount >= proof.requiredVertexCount;
}

} // namespace dxvk::war3::render
