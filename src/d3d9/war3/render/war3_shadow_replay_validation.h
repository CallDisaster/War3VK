#pragma once

#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

enum class War3ShadowReplayRejectReason : uint32_t {
  None = 0u,
  MissingMapEpoch,
  StaleMapEpoch,
  MissingDeviceEpoch,
  StaleDeviceEpoch,
  NonFiniteWorldMatrix,
  MissingPositionBuffer,
  InvalidPositionLayout,
  PositionRangeOverflow,
  PositionRangeOutOfBounds,
  InvalidIndexType,
  IndexRangeOverflow,
  IndexRangeOutOfBounds,
  EmptyDraw,
  NegativeVertexDomain,
  VertexDomainOverflow,
  InvalidActualIndexDomain,
  MissingBlendBuffer,
  InvalidBlendLayout,
  BlendRangeOutOfBounds,
  MissingUvBuffer,
  InvalidUvLayout,
  UvRangeOutOfBounds,
  InvalidGpuSkinLease,
  StaleGpuSkinMapEpoch,
  StaleGpuSkinDeviceEpoch,
  GpuSkinSourceRangeOutOfBounds,
  GpuSkinPaletteRangeOutOfBounds,
  IncompleteReplayPlan,
  Count,
};

struct War3ShadowReplayBufferAccess {
  bool present = false;
  uint64_t size = 0u;
  uint32_t stride = 0u;
  uint32_t attributeOffset = 0u;
  uint32_t attributeBytes = 0u;
};

struct War3ShadowReplayValidationInput {
  uint64_t expectedMapEpoch = 0u;
  uint64_t expectedDeviceEpoch = 0u;
  uint64_t drawMapEpoch = 0u;
  uint64_t drawDeviceEpoch = 0u;
  bool worldMatrixFinite = false;

  War3ShadowReplayBufferAccess position = {};
  bool indexed = false;
  bool indexBufferPresent = false;
  uint64_t indexBufferSize = 0u;
  uint32_t indexTypeBytes = 0u;
  uint32_t firstIndex = 0u;
  uint32_t indexCount = 0u;
  int32_t vertexOffset = 0;
  uint32_t minVertexIndex = 0u;
  uint32_t numVertices = 0u;
  uint32_t firstVertex = 0u;
  uint32_t vertexCount = 0u;
  bool actualIndexDomainKnown = false;
  // The producer froze the complete current VB because the exact IB domain
  // was CPU-opaque. numVertices then describes the frozen backing rather than
  // a D3D draw range that should be shifted by vertexOffset again.
  bool fullVertexDomainFallback = false;
  uint32_t actualIndexMin = 0u;
  uint32_t actualIndexMax = 0u;

  bool blendRequired = false;
  War3ShadowReplayBufferAccess blend = {};
  bool uvRequired = false;
  War3ShadowReplayBufferAccess uv = {};

  bool gpuSkinRequired = false;
  bool gpuSkinLeaseValid = false;
  uint64_t gpuSkinMapEpoch = 0u;
  uint64_t gpuSkinDeviceEpoch = 0u;
  uint64_t gpuSkinSourceSize = 0u;
  uint32_t gpuSkinSourceOffset = 0u;
  uint32_t gpuSkinSourceLength = 0u;
  uint64_t gpuSkinPaletteSize = 0u;
  uint32_t gpuSkinPaletteOffset = 0u;
  uint32_t gpuSkinPaletteLength = 0u;
};

struct War3ShadowReplayValidationResult {
  War3ShadowReplayRejectReason reason =
      War3ShadowReplayRejectReason::None;
  uint64_t requiredEnd = 0u;
  uint64_t availableSize = 0u;
  int64_t minimumVertex = 0;
  int64_t maximumVertex = 0;

  explicit operator bool() const noexcept {
    return reason == War3ShadowReplayRejectReason::None;
  }
};

/**
 * Result of validating one immutable replay batch before command recording.
 * A consumer must reject the entire batch when valid is false; failureIndex
 * names the first malformed draw and no earlier draw is permission to submit.
 */
struct War3ShadowReplayBatchValidationResult {
  bool valid = true;
  std::size_t failureIndex = 0u;
  std::size_t validatedCount = 0u;
  War3ShadowReplayValidationResult failure = {};

  explicit operator bool() const noexcept {
    return valid;
  }
};

War3ShadowReplayValidationResult ValidateWar3ShadowReplayDraw(
    const War3ShadowReplayValidationInput& input) noexcept;

War3ShadowReplayBatchValidationResult ValidateWar3ShadowReplayBatch(
    const War3ShadowReplayValidationInput* inputs,
    std::size_t count) noexcept;

const char* War3ShadowReplayRejectReasonName(
    War3ShadowReplayRejectReason reason) noexcept;

} // namespace dxvk::war3::render
