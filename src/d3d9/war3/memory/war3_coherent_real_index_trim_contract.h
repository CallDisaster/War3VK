#pragma once

#include <cstdint>
#include <limits>

namespace dxvk::war3::memory {

enum class War3CoherentRealIndexTrimMode : uint8_t {
  Off = 0u,
  Observe = 1u,
  Consume = 2u,
};

#if defined(WARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV) && \
    WARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV
inline constexpr bool kCoherentRealIndexTrimDevelopmentEnabled = true;
#else
inline constexpr bool kCoherentRealIndexTrimDevelopmentEnabled = false;
#endif

constexpr War3CoherentRealIndexTrimMode ParseWar3CoherentRealIndexTrimMode(
    uint32_t configuredMode) noexcept {
  if (!kCoherentRealIndexTrimDevelopmentEnabled)
    return War3CoherentRealIndexTrimMode::Off;
  return configuredMode == 1u
      ? War3CoherentRealIndexTrimMode::Observe
      : configuredMode == 2u
          ? War3CoherentRealIndexTrimMode::Consume
          : War3CoherentRealIndexTrimMode::Off;
}

enum class War3CoherentRealIndexTrimRejectReason : uint8_t {
  None = 0u,
  NotIndexedTerrain,
  LegacyGpuSkin,
  NotRigidOpaque,
  NotDynamicRealPosition,
  MissingCurrentPositionSpan,
  MissingCurrentIndexSpan,
  InvalidPositionStride,
  InvalidIndexType,
  EmptyIndexRange,
  IndexRangeOverflow,
  IndexRangeOutsideSpan,
  PositionRangeOutsideSpan,
};

struct War3CoherentRealIndexTrimInput {
  bool indexedTerrain = false;
  bool legacyGpuSkin = false;
  bool vertexBlendEnabled = false;
  bool alphaTestEnabled = false;
  bool alphaBlendEnabled = false;
  bool dynamicRealPosition = false;
  bool currentPositionSpan = false;
  bool currentIndexSpan = false;
  uint64_t positionSpanBytes = 0u;
  uint32_t positionStride = 0u;
  uint64_t indexSpanBytes = 0u;
  uint32_t indexElementBytes = 0u;
  uint32_t indexCount = 0u;
};

struct War3CoherentRealIndexTrimDecision {
  uint64_t indexBytes = 0u;
  uint32_t positionCapacity = 0u;
  War3CoherentRealIndexTrimRejectReason rejectReason =
      War3CoherentRealIndexTrimRejectReason::NotIndexedTerrain;

  explicit constexpr operator bool() const noexcept {
    return rejectReason == War3CoherentRealIndexTrimRejectReason::None &&
        indexBytes != 0u && positionCapacity != 0u;
  }
};

// This contract only admits the ordinary D3D9 REAL-buffer terrain lane. The
// caller must still prove the exact index domain and immediately copy the
// compact position/index bytes before returning to Warcraft. It never grants
// cross-frame reuse or permission to retain a mutable mapped pointer.
constexpr War3CoherentRealIndexTrimDecision
EvaluateWar3CoherentRealIndexTrim(
    const War3CoherentRealIndexTrimInput& input) noexcept {
  War3CoherentRealIndexTrimDecision result = {};
  if (!input.indexedTerrain)
    return result;
  if (input.legacyGpuSkin) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::LegacyGpuSkin;
    return result;
  }
  if (input.vertexBlendEnabled || input.alphaTestEnabled ||
      input.alphaBlendEnabled) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::NotRigidOpaque;
    return result;
  }
  if (!input.dynamicRealPosition) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::NotDynamicRealPosition;
    return result;
  }
  if (!input.currentPositionSpan) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::MissingCurrentPositionSpan;
    return result;
  }
  if (!input.currentIndexSpan) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::MissingCurrentIndexSpan;
    return result;
  }
  if (input.positionStride == 0u) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::InvalidPositionStride;
    return result;
  }
  if (input.indexElementBytes != 2u && input.indexElementBytes != 4u) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::InvalidIndexType;
    return result;
  }
  if (input.indexCount == 0u) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::EmptyIndexRange;
    return result;
  }
  if (uint64_t(input.indexCount) >
      std::numeric_limits<uint64_t>::max() /
          uint64_t(input.indexElementBytes)) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::IndexRangeOverflow;
    return result;
  }
  result.indexBytes =
      uint64_t(input.indexCount) * uint64_t(input.indexElementBytes);
  if (result.indexBytes > input.indexSpanBytes) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::IndexRangeOutsideSpan;
    return result;
  }
  const uint64_t positionCapacity =
      input.positionSpanBytes / uint64_t(input.positionStride);
  if (positionCapacity == 0u ||
      positionCapacity > uint64_t(std::numeric_limits<uint32_t>::max())) {
    result.rejectReason =
        War3CoherentRealIndexTrimRejectReason::PositionRangeOutsideSpan;
    return result;
  }
  result.positionCapacity = uint32_t(positionCapacity);
  result.rejectReason = War3CoherentRealIndexTrimRejectReason::None;
  return result;
}

}  // namespace dxvk::war3::memory
