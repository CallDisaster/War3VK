#pragma once

#include <cstddef>
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
    return valid() && other.valid() && *this == other;
  }

  constexpr bool operator==(
      const War3ShadowGenerationBackedStreamProof& other) const noexcept {
    return ownerIdentity == other.ownerIdentity &&
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

// Hashing is used only after valid() has established the exact owner,
// allocation/content generations, map/device epochs and byte range. A hash
// match never authorizes reuse by itself: unordered_map still applies the
// complete value equality above.
struct War3ShadowGenerationBackedStreamProofHash {
  size_t operator()(
      const War3ShadowGenerationBackedStreamProof& proof) const noexcept {
    uint64_t hash = uint64_t(proof.ownerIdentity);
    const auto mix = [&](uint64_t value) {
      hash ^= value + UINT64_C(0x9e3779b97f4a7c15) +
          (hash << 6u) + (hash >> 2u);
    };
    mix(proof.identityGeneration);
    mix(proof.allocationGeneration);
    mix(proof.contentGeneration);
    mix(proof.sourceOffset);
    mix(proof.sourceLength);
    mix(proof.elementStride);
    mix(proof.elementSize);
    mix(proof.mapEpoch);
    mix(proof.deviceEpoch);
    mix(uint8_t(proof.streamKind));
    return size_t(hash);
  }
};

struct War3ShadowGenerationBackedGeometryProof {
  War3ShadowGenerationBackedStreamProof position = {};
  War3ShadowGenerationBackedStreamProof index = {};
  bool indexed = false;

  constexpr bool valid() const noexcept {
    return position.valid() &&
        position.streamKind == War3ShadowStreamKind::Position &&
        (!indexed || (index.valid() &&
            index.streamKind == War3ShadowStreamKind::Index));
  }

  constexpr bool matches(
      const War3ShadowGenerationBackedGeometryProof& other) const noexcept {
    return valid() && other.valid() && indexed == other.indexed &&
        position.matches(other.position) &&
        (!indexed || index.matches(other.index));
  }
};

struct War3ShadowGenerationStabilityState {
  War3ShadowGenerationBackedGeometryProof proof = {};
  uint64_t lastObservedFrame = 0u;
  uint32_t distinctStableFrames = 0u;
};

// Presents are not Warcraft world-production frames: the native renderer can
// present the same world several times before Stage11 runs again.  Compress
// the sparse Present serials into a monotonic observation clock so "adjacent"
// below means adjacent observed producer batches, not adjacent swapchain
// presents. Multiple S1 draws from one producer batch share one serial.
struct War3ShadowGenerationObservationClock {
  uint64_t sourceFrameSerial = 0u;
  uint64_t observationFrameSerial = 0u;
};

constexpr uint64_t AdvanceWar3ShadowGenerationObservationClock(
    War3ShadowGenerationObservationClock& clock,
    uint64_t sourceFrameSerial) noexcept {
  if (sourceFrameSerial == 0u)
    return 0u;
  if (clock.sourceFrameSerial == sourceFrameSerial)
    return clock.observationFrameSerial;
  clock.sourceFrameSerial = sourceFrameSerial;
  if (clock.observationFrameSerial != UINT64_MAX)
    ++clock.observationFrameSerial;
  return clock.observationFrameSerial;
}

enum class War3ShadowGenerationObservation : uint8_t {
  Invalid = 0u,
  First,
  SameFrame,
  Advanced,
  Changed,
  StaleRestart,
};

struct War3ShadowGenerationObservationResult {
  War3ShadowGenerationObservation observation =
      War3ShadowGenerationObservation::Invalid;
  bool promotionReady = false;
};

// A source is eligible for persistent promotion only after the exact same
// generation-backed proof was observed on distinct, adjacent render frames.
// Multiple terrain draws in one frame cannot manufacture stability, and a
// source-generation change restarts probation instead of reusing old bytes.
constexpr War3ShadowGenerationObservationResult
ObserveWar3ShadowGenerationStability(
    War3ShadowGenerationStabilityState& state,
    const War3ShadowGenerationBackedGeometryProof& proof,
    uint64_t frameSerial,
    uint32_t requiredDistinctFrames = 2u) noexcept {
  War3ShadowGenerationObservationResult result = {};
  if (!proof.valid() || frameSerial == 0u || requiredDistinctFrames == 0u)
    return result;

  const auto restart = [&](War3ShadowGenerationObservation observation) {
    state.proof = proof;
    state.lastObservedFrame = frameSerial;
    state.distinctStableFrames = 1u;
    result.observation = observation;
    result.promotionReady = requiredDistinctFrames <= 1u;
  };

  if (!state.proof.valid() || state.lastObservedFrame == 0u) {
    restart(War3ShadowGenerationObservation::First);
    return result;
  }

  if (!state.proof.matches(proof)) {
    restart(War3ShadowGenerationObservation::Changed);
    return result;
  }

  if (frameSerial == state.lastObservedFrame) {
    result.observation = War3ShadowGenerationObservation::SameFrame;
    result.promotionReady =
        state.distinctStableFrames >= requiredDistinctFrames;
    return result;
  }

  if (frameSerial != state.lastObservedFrame + 1u) {
    restart(War3ShadowGenerationObservation::StaleRestart);
    return result;
  }

  state.lastObservedFrame = frameSerial;
  if (state.distinctStableFrames != UINT32_MAX)
    ++state.distinctStableFrames;
  result.observation = War3ShadowGenerationObservation::Advanced;
  result.promotionReady =
      state.distinctStableFrames >= requiredDistinctFrames;
  return result;
}

} // namespace dxvk::war3::render
