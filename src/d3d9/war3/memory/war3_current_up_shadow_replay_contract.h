#pragma once

#include <cstdint>

namespace dxvk::war3::memory {

#if defined(WARVK_ENABLE_CURRENT_UP_SHADOW_REPLAY_DEV) && \
    WARVK_ENABLE_CURRENT_UP_SHADOW_REPLAY_DEV
  inline constexpr bool kCurrentUpShadowReplayDevelopmentEnabled = true;
#else
  inline constexpr bool kCurrentUpShadowReplayDevelopmentEnabled = false;
#endif

  enum class War3CurrentUpShadowReplayMode : uint8_t {
    Off = 0u,
    Observe = 1u,
    Consume = 2u,
  };

  constexpr War3CurrentUpShadowReplayMode
  ParseWar3CurrentUpShadowReplayMode(uint32_t value) noexcept {
    if constexpr (!kCurrentUpShadowReplayDevelopmentEnabled)
      return War3CurrentUpShadowReplayMode::Off;
    return value == 1u
        ? War3CurrentUpShadowReplayMode::Observe
        : value == 2u
            ? War3CurrentUpShadowReplayMode::Consume
            : War3CurrentUpShadowReplayMode::Off;
  }

  enum class War3CurrentUpPositionReplayRejectReason : uint8_t {
    None = 0u,
    NotCurrentUpload,
    GpuSkinBacking,
    MissingPinnedAllocation,
    MissingBuffer,
    BufferMismatch,
    EmptyRange,
    RangeOutsideUpload,
  };

  struct War3CurrentUpPositionReplayInput {
    bool currentPositionUpload = false;
    bool gpuSkinBacking = false;
    bool hasPinnedAllocation = false;
    bool hasUploadBuffer = false;
    bool sameBuffer = false;
    uint64_t uploadOffset = 0u;
    uint64_t uploadLength = 0u;
    uint64_t replayOffset = 0u;
    uint64_t replayLength = 0u;
  };

  struct War3CurrentUpPositionReplayDecision {
    War3CurrentUpPositionReplayRejectReason rejectReason =
        War3CurrentUpPositionReplayRejectReason::None;
    uint64_t replayOffset = 0u;
    uint64_t replayLength = 0u;

    constexpr explicit operator bool() const noexcept {
      return rejectReason == War3CurrentUpPositionReplayRejectReason::None &&
          replayLength != 0u;
    }
  };

  constexpr War3CurrentUpPositionReplayDecision
  EvaluateWar3CurrentUpPositionReplay(
      const War3CurrentUpPositionReplayInput& input) noexcept {
    War3CurrentUpPositionReplayDecision result = {};
    if (!input.currentPositionUpload) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::NotCurrentUpload;
      return result;
    }
    if (input.gpuSkinBacking) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::GpuSkinBacking;
      return result;
    }
    if (!input.hasPinnedAllocation) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::MissingPinnedAllocation;
      return result;
    }
    if (!input.hasUploadBuffer) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::MissingBuffer;
      return result;
    }
    if (!input.sameBuffer) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::BufferMismatch;
      return result;
    }
    if (input.uploadLength == 0u || input.replayLength == 0u) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::EmptyRange;
      return result;
    }
    if (input.replayOffset < input.uploadOffset) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::RangeOutsideUpload;
      return result;
    }
    const uint64_t localOffset = input.replayOffset - input.uploadOffset;
    if (localOffset > input.uploadLength ||
        input.replayLength > input.uploadLength - localOffset) {
      result.rejectReason =
          War3CurrentUpPositionReplayRejectReason::RangeOutsideUpload;
      return result;
    }
    result.replayOffset = input.replayOffset;
    result.replayLength = input.replayLength;
    return result;
  }

} // namespace dxvk::war3::memory
