#pragma once

#include <cstdint>

namespace dxvk::war3::render {

enum class War3ShadowStreamKind : uint8_t {
  Position = 0u,
  Uv,
  Index,
};

// Immutable value proof for one exact source range.  This deliberately does
// not use buffer addresses or content fingerprints as cross-frame authority:
// the D3D9 owner, allocation and content generations must all remain equal.
struct War3ShadowGenerationBackedStreamProof {
  uintptr_t ownerIdentity = 0u;
  uint64_t identityGeneration = 0u;
  uint64_t allocationGeneration = 0u;
  uint64_t contentGeneration = 0u;
  uint64_t sourceOffset = 0u;
  uint64_t sourceLength = 0u;
  uint32_t elementStride = 0u;
  uint32_t elementSize = 0u;
  uint64_t mapEpoch = 0u;
  uint64_t deviceEpoch = 0u;
  War3ShadowStreamKind streamKind = War3ShadowStreamKind::Position;

  constexpr bool valid() const noexcept {
    return ownerIdentity != 0u && identityGeneration != 0u &&
        allocationGeneration != 0u && contentGeneration != 0u &&
        sourceLength != 0u && elementStride != 0u && elementSize != 0u &&
        elementSize <= elementStride && mapEpoch != 0u && deviceEpoch != 0u;
  }

  constexpr bool matches(
      const War3ShadowGenerationBackedStreamProof& other) const noexcept {
    return valid() && other.valid() &&
        ownerIdentity == other.ownerIdentity &&
        identityGeneration == other.identityGeneration &&
        allocationGeneration == other.allocationGeneration &&
        contentGeneration == other.contentGeneration &&
        sourceOffset == other.sourceOffset &&
        sourceLength == other.sourceLength &&
        elementStride == other.elementStride &&
        elementSize == other.elementSize &&
        mapEpoch == other.mapEpoch && deviceEpoch == other.deviceEpoch &&
        streamKind == other.streamKind;
  }
};

} // namespace dxvk::war3::render
