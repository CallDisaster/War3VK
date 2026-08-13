#pragma once

#include <cstdint>
#include <limits>

namespace dxvk::war3::memory {

enum class War3CoherentUpIndexTrimMode : uint8_t {
  Off = 0u,
  Observe = 1u,
  Consume = 2u,
};

#if defined(WARVK_ENABLE_COHERENT_UP_INDEX_TRIM_DEV) && \
    WARVK_ENABLE_COHERENT_UP_INDEX_TRIM_DEV
inline constexpr bool kCoherentUpIndexTrimDevelopmentEnabled = true;
#else
inline constexpr bool kCoherentUpIndexTrimDevelopmentEnabled = false;
#endif

constexpr War3CoherentUpIndexTrimMode ParseWar3CoherentUpIndexTrimMode(
    uint32_t configuredMode) noexcept {
  if (!kCoherentUpIndexTrimDevelopmentEnabled)
    return War3CoherentUpIndexTrimMode::Off;
  return configuredMode == 1u
      ? War3CoherentUpIndexTrimMode::Observe
      : configuredMode == 2u
          ? War3CoherentUpIndexTrimMode::Consume
          : War3CoherentUpIndexTrimMode::Off;
}

enum class War3CoherentUpIndexTrimRejectReason : uint8_t {
  None = 0u,
  NotIndexed,
  LegacyGpuSkin,
  NotCurrentUpPair,
  DifferentAllocation,
  MissingBytes,
  InvalidPositionStride,
  InvalidIndexType,
  EmptyIndexRange,
  NonZeroUploadedFirstIndex,
  IndexRangeOverflow,
  IndexRangeOutsideUpload,
  PositionRangeOutsideUpload,
};

struct War3CoherentUpIndexTrimInput {
  bool indexed = false;
  bool legacyGpuSkin = false;
  bool currentPositionUpload = false;
  bool currentIndexUpload = false;
  bool samePinnedAllocation = false;
  bool hasPositionBytes = false;
  bool hasIndexBytes = false;
  uint64_t positionUploadBytes = 0u;
  uint64_t positionSliceBytes = 0u;
  uint32_t positionStride = 0u;
  uint64_t indexUploadBytes = 0u;
  uint64_t indexSliceBytes = 0u;
  uint32_t indexElementBytes = 0u;
  uint32_t indexCount = 0u;
  uint32_t firstIndex = 0u;
};

struct War3CoherentUpIndexTrimDecision {
  uint64_t indexBytes = 0u;
  uint32_t positionCapacity = 0u;
  War3CoherentUpIndexTrimRejectReason rejectReason =
      War3CoherentUpIndexTrimRejectReason::NotIndexed;

  explicit constexpr operator bool() const noexcept {
    return rejectReason == War3CoherentUpIndexTrimRejectReason::None &&
        indexBytes != 0u && positionCapacity != 0u;
  }
};

constexpr War3CoherentUpIndexTrimDecision
EvaluateWar3CoherentUpIndexTrim(
    const War3CoherentUpIndexTrimInput& input) noexcept {
  War3CoherentUpIndexTrimDecision result = {};
  if (!input.indexed)
    return result;
  if (input.legacyGpuSkin) {
    result.rejectReason = War3CoherentUpIndexTrimRejectReason::LegacyGpuSkin;
    return result;
  }
  if (!input.currentPositionUpload || !input.currentIndexUpload) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::NotCurrentUpPair;
    return result;
  }
  if (!input.samePinnedAllocation) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::DifferentAllocation;
    return result;
  }
  if (!input.hasPositionBytes || !input.hasIndexBytes) {
    result.rejectReason = War3CoherentUpIndexTrimRejectReason::MissingBytes;
    return result;
  }
  if (input.positionStride == 0u) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::InvalidPositionStride;
    return result;
  }
  if (input.indexElementBytes != 2u && input.indexElementBytes != 4u) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::InvalidIndexType;
    return result;
  }
  if (input.indexCount == 0u) {
    result.rejectReason = War3CoherentUpIndexTrimRejectReason::EmptyIndexRange;
    return result;
  }
  if (input.firstIndex != 0u) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::NonZeroUploadedFirstIndex;
    return result;
  }
  if (uint64_t(input.indexCount) >
      std::numeric_limits<uint64_t>::max() /
          uint64_t(input.indexElementBytes)) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::IndexRangeOverflow;
    return result;
  }
  result.indexBytes =
      uint64_t(input.indexCount) * uint64_t(input.indexElementBytes);
  if (result.indexBytes > input.indexUploadBytes ||
      result.indexBytes > input.indexSliceBytes) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::IndexRangeOutsideUpload;
    return result;
  }
  const uint64_t positionBytes =
      input.positionUploadBytes < input.positionSliceBytes
      ? input.positionUploadBytes : input.positionSliceBytes;
  const uint64_t positionCapacity =
      positionBytes / uint64_t(input.positionStride);
  if (positionCapacity == 0u ||
      positionCapacity > uint64_t(std::numeric_limits<uint32_t>::max())) {
    result.rejectReason =
        War3CoherentUpIndexTrimRejectReason::PositionRangeOutsideUpload;
    return result;
  }
  result.positionCapacity = uint32_t(positionCapacity);
  result.rejectReason = War3CoherentUpIndexTrimRejectReason::None;
  return result;
}

}  // namespace dxvk::war3::memory
