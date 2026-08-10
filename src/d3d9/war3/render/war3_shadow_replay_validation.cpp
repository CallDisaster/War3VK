#include "war3_shadow_replay_validation.h"

#include <algorithm>
#include <limits>

namespace dxvk::war3::render {
namespace {

bool CheckedAdd(uint64_t a, uint64_t b, uint64_t& out) noexcept {
  if (b > std::numeric_limits<uint64_t>::max() - a)
    return false;
  out = a + b;
  return true;
}

bool CheckedMul(uint64_t a, uint64_t b, uint64_t& out) noexcept {
  if (a != 0u && b > std::numeric_limits<uint64_t>::max() / a)
    return false;
  out = a * b;
  return true;
}

War3ShadowReplayValidationResult Reject(
    War3ShadowReplayRejectReason reason, uint64_t required = 0u,
    uint64_t available = 0u, int64_t minimumVertex = 0,
    int64_t maximumVertex = 0) noexcept {
  War3ShadowReplayValidationResult result = {};
  result.reason = reason;
  result.requiredEnd = required;
  result.availableSize = available;
  result.minimumVertex = minimumVertex;
  result.maximumVertex = maximumVertex;
  return result;
}

War3ShadowReplayValidationResult ValidateVertexAccess(
    const War3ShadowReplayBufferAccess& access, int64_t minimumVertex,
    int64_t maximumVertex, War3ShadowReplayRejectReason missing,
    War3ShadowReplayRejectReason invalidLayout,
    War3ShadowReplayRejectReason outOfBounds) noexcept {
  if (!access.present || access.size == 0u)
    return Reject(missing, 0u, access.size, minimumVertex, maximumVertex);
  if (access.stride == 0u || access.attributeBytes == 0u ||
      access.attributeOffset >= access.stride ||
      access.attributeBytes > access.stride - access.attributeOffset) {
    return Reject(invalidLayout, 0u, access.size, minimumVertex,
                  maximumVertex);
  }
  if (minimumVertex < 0 || maximumVertex < minimumVertex)
    return Reject(War3ShadowReplayRejectReason::NegativeVertexDomain, 0u,
                  access.size, minimumVertex, maximumVertex);

  uint64_t vertexByte = 0u;
  uint64_t attributeEnd = 0u;
  if (!CheckedMul(uint64_t(maximumVertex), uint64_t(access.stride),
                  vertexByte) ||
      !CheckedAdd(vertexByte, uint64_t(access.attributeOffset),
                  attributeEnd) ||
      !CheckedAdd(attributeEnd, uint64_t(access.attributeBytes),
                  attributeEnd)) {
    return Reject(War3ShadowReplayRejectReason::VertexDomainOverflow, 0u,
                  access.size, minimumVertex, maximumVertex);
  }
  if (attributeEnd > access.size)
    return Reject(outOfBounds, attributeEnd, access.size, minimumVertex,
                  maximumVertex);
  return {};
}

bool RangeFits(uint64_t total, uint64_t offset, uint64_t length,
               uint64_t& end) noexcept {
  return CheckedAdd(offset, length, end) && end <= total;
}

} // namespace

War3ShadowReplayValidationResult ValidateWar3ShadowReplayDraw(
    const War3ShadowReplayValidationInput& input) noexcept {
  if (input.drawMapEpoch == 0u || input.expectedMapEpoch == 0u)
    return Reject(War3ShadowReplayRejectReason::MissingMapEpoch);
  if (input.drawMapEpoch != input.expectedMapEpoch)
    return Reject(War3ShadowReplayRejectReason::StaleMapEpoch);
  if (input.drawDeviceEpoch == 0u || input.expectedDeviceEpoch == 0u)
    return Reject(War3ShadowReplayRejectReason::MissingDeviceEpoch);
  if (input.drawDeviceEpoch != input.expectedDeviceEpoch)
    return Reject(War3ShadowReplayRejectReason::StaleDeviceEpoch);
  if (!input.worldMatrixFinite)
    return Reject(War3ShadowReplayRejectReason::NonFiniteWorldMatrix);

  int64_t minimumVertex = 0;
  int64_t maximumVertex = -1;
  if (input.indexed) {
    if (!input.indexBufferPresent || input.indexBufferSize == 0u)
      return Reject(War3ShadowReplayRejectReason::IndexRangeOutOfBounds);
    if (input.indexTypeBytes != 2u && input.indexTypeBytes != 4u)
      return Reject(War3ShadowReplayRejectReason::InvalidIndexType);
    if (input.indexCount == 0u)
      return Reject(War3ShadowReplayRejectReason::EmptyDraw);

    uint64_t indexEnd = 0u;
    uint64_t firstByte = 0u;
    uint64_t countBytes = 0u;
    if (!CheckedMul(input.firstIndex, input.indexTypeBytes, firstByte) ||
        !CheckedMul(input.indexCount, input.indexTypeBytes, countBytes) ||
        !CheckedAdd(firstByte, countBytes, indexEnd)) {
      return Reject(War3ShadowReplayRejectReason::IndexRangeOverflow);
    }
    if (indexEnd > input.indexBufferSize) {
      return Reject(War3ShadowReplayRejectReason::IndexRangeOutOfBounds,
                    indexEnd, input.indexBufferSize);
    }

    uint64_t sourceMinimum = input.minVertexIndex;
    uint64_t sourceMaximum = 0u;
    if (input.actualIndexDomainKnown) {
      if (input.actualIndexMax < input.actualIndexMin)
        return Reject(
            War3ShadowReplayRejectReason::InvalidActualIndexDomain);
      sourceMinimum = input.actualIndexMin;
      sourceMaximum = input.actualIndexMax;
    } else if (input.fullVertexDomainFallback) {
      // The exact current IB was frozen together with the complete bounded VB,
      // but its domain was CPU-opaque. Applying BaseVertexIndex to the entire
      // [0, capacity) backing falsely rejects every positive base vertex.
      // Validate complete attribute coverage; index byte bounds are already
      // proven above and replay uses the same current-draw addressing tuple.
      const uint64_t count = input.numVertices != 0u
          ? input.numVertices
          : input.vertexCount;
      if (count == 0u)
        return Reject(War3ShadowReplayRejectReason::EmptyDraw);
      sourceMinimum = 0u;
      sourceMaximum = count - 1u;
    } else {
      const uint64_t count = input.numVertices != 0u
          ? input.numVertices
          : input.vertexCount;
      if (count == 0u)
        return Reject(War3ShadowReplayRejectReason::EmptyDraw);
      if (!CheckedAdd(sourceMinimum, count - 1u, sourceMaximum))
        return Reject(War3ShadowReplayRejectReason::VertexDomainOverflow);
    }
    const int64_t appliedVertexOffset =
        input.fullVertexDomainFallback && !input.actualIndexDomainKnown
            ? 0
            : int64_t(input.vertexOffset);
    minimumVertex = int64_t(sourceMinimum) + appliedVertexOffset;
    maximumVertex = int64_t(sourceMaximum) + appliedVertexOffset;
    if (minimumVertex < 0 || maximumVertex < minimumVertex)
      return Reject(War3ShadowReplayRejectReason::NegativeVertexDomain, 0u,
                    input.position.size, minimumVertex, maximumVertex);
  } else {
    if (input.vertexCount == 0u)
      return Reject(War3ShadowReplayRejectReason::EmptyDraw);
    minimumVertex = input.firstVertex;
    const uint64_t maximum =
        uint64_t(input.firstVertex) + uint64_t(input.vertexCount) - 1u;
    if (maximum > uint64_t(std::numeric_limits<int64_t>::max()))
      return Reject(War3ShadowReplayRejectReason::VertexDomainOverflow);
    maximumVertex = int64_t(maximum);
  }

  auto position = ValidateVertexAccess(
      input.position, minimumVertex, maximumVertex,
      War3ShadowReplayRejectReason::MissingPositionBuffer,
      War3ShadowReplayRejectReason::InvalidPositionLayout,
      War3ShadowReplayRejectReason::PositionRangeOutOfBounds);
  if (!position)
    return position;

  if (input.blendRequired) {
    auto blend = ValidateVertexAccess(
        input.blend, minimumVertex, maximumVertex,
        War3ShadowReplayRejectReason::MissingBlendBuffer,
        War3ShadowReplayRejectReason::InvalidBlendLayout,
        War3ShadowReplayRejectReason::BlendRangeOutOfBounds);
    if (!blend)
      return blend;
  }
  if (input.uvRequired) {
    auto uv = ValidateVertexAccess(
        input.uv, minimumVertex, maximumVertex,
        War3ShadowReplayRejectReason::MissingUvBuffer,
        War3ShadowReplayRejectReason::InvalidUvLayout,
        War3ShadowReplayRejectReason::UvRangeOutOfBounds);
    if (!uv)
      return uv;
  }

  if (input.paletteRequired) {
    if (input.paletteCount == 0u ||
        input.paletteIndex >= input.paletteCount ||
        input.paletteMatricesPerEntry == 0u) {
      return Reject(War3ShadowReplayRejectReason::InvalidPaletteIndex,
                    input.paletteIndex, input.paletteCount);
    }

    uint64_t paletteOffset = 0u;
    uint64_t paletteEnd = 0u;
    uint64_t totalMatrices = 0u;
    if (!CheckedMul(uint64_t(input.paletteIndex),
                    uint64_t(input.paletteMatricesPerEntry), paletteOffset) ||
        !CheckedAdd(paletteOffset,
                    uint64_t(input.paletteMatricesPerEntry), paletteEnd) ||
        !CheckedMul(uint64_t(input.paletteCount),
                    uint64_t(input.paletteMatricesPerEntry), totalMatrices)) {
      return Reject(War3ShadowReplayRejectReason::PaletteRangeOverflow);
    }
    if (paletteEnd > totalMatrices) {
      return Reject(War3ShadowReplayRejectReason::InvalidPaletteIndex,
                    paletteEnd, totalMatrices);
    }
  }

  if (input.gpuSkinRequired) {
    if (!input.gpuSkinLeaseValid)
      return Reject(War3ShadowReplayRejectReason::InvalidGpuSkinLease);
    if (input.gpuSkinMapEpoch != input.expectedMapEpoch)
      return Reject(War3ShadowReplayRejectReason::StaleGpuSkinMapEpoch);
    if (input.gpuSkinDeviceEpoch != input.expectedDeviceEpoch)
      return Reject(War3ShadowReplayRejectReason::StaleGpuSkinDeviceEpoch);
    uint64_t end = 0u;
    if (!RangeFits(input.gpuSkinSourceSize, input.gpuSkinSourceOffset,
                   input.gpuSkinSourceLength, end)) {
      return Reject(
          War3ShadowReplayRejectReason::GpuSkinSourceRangeOutOfBounds, end,
          input.gpuSkinSourceSize);
    }
    if (!RangeFits(input.gpuSkinPaletteSize, input.gpuSkinPaletteOffset,
                   input.gpuSkinPaletteLength, end)) {
      return Reject(
          War3ShadowReplayRejectReason::GpuSkinPaletteRangeOutOfBounds, end,
          input.gpuSkinPaletteSize);
    }
  }
  return {};
}

War3ShadowReplayBatchValidationResult ValidateWar3ShadowReplayBatch(
    const War3ShadowReplayValidationInput* inputs,
    std::size_t count) noexcept {
  War3ShadowReplayBatchValidationResult batch = {};
  if (count != 0u && inputs == nullptr) {
    batch.valid = false;
    batch.failure.reason =
        War3ShadowReplayRejectReason::MissingPositionBuffer;
    return batch;
  }

  for (std::size_t i = 0u; i < count; ++i) {
    const War3ShadowReplayValidationResult result =
        ValidateWar3ShadowReplayDraw(inputs[i]);
    if (!result) {
      batch.valid = false;
      batch.failureIndex = i;
      batch.failure = result;
      return batch;
    }
    ++batch.validatedCount;
  }
  return batch;
}

const char* War3ShadowReplayRejectReasonName(
    War3ShadowReplayRejectReason reason) noexcept {
  static constexpr const char* kNames[] = {
      "none", "missing-map-epoch", "stale-map-epoch",
      "missing-device-epoch", "stale-device-epoch", "nonfinite-world",
      "missing-position-buffer", "invalid-position-layout",
      "position-range-overflow", "position-range-out-of-bounds",
      "invalid-index-type", "index-range-overflow",
      "index-range-out-of-bounds", "empty-draw", "negative-vertex-domain",
      "vertex-domain-overflow", "invalid-actual-index-domain",
      "missing-blend-buffer", "invalid-blend-layout",
      "blend-range-out-of-bounds", "missing-uv-buffer", "invalid-uv-layout",
      "uv-range-out-of-bounds", "invalid-palette-index",
      "palette-range-overflow", "invalid-gpu-skin-lease",
      "stale-gpu-skin-map-epoch", "stale-gpu-skin-device-epoch",
      "gpu-skin-source-range-out-of-bounds",
      "gpu-skin-palette-range-out-of-bounds",
      "incomplete-replay-plan", "producer-incomplete",
      "producer-stamp-mismatch"};
  const uint32_t index = static_cast<uint32_t>(reason);
  return index < static_cast<uint32_t>(War3ShadowReplayRejectReason::Count)
      ? kNames[index]
      : "unknown";
}

} // namespace dxvk::war3::render
