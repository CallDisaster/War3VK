#include "war3_persistent_gpu_package_immutable.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace dxvk::war3::gpu_skin {

uint64_t ContinuePersistentGpuPackageContentHash(
    uint64_t seed, const void* data, size_t size) noexcept {
  if (data == nullptr && size != 0u)
    return 0u;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0u; i < size; ++i)
    seed = (seed ^ bytes[i]) * 0x100000001b3ull;
  return seed;
}

uint64_t HashPersistentGpuPackageContent(
    const void* data, size_t size) noexcept {
  return ContinuePersistentGpuPackageContentHash(
      kPersistentGpuPackageContentHashSeed, data, size);
}

uint64_t HashPersistentGpuPackageStridedFloat3(
    const void* data, uint64_t byteCount, uint32_t vertexCount,
    uint32_t stride, uint32_t positionOffset) noexcept {
  constexpr uint64_t kPositionBytes = sizeof(float) * 3u;
  if (data == nullptr || byteCount == 0u || vertexCount == 0u ||
      stride < kPositionBytes || positionOffset > stride - kPositionBytes)
    return 0u;
  const uint64_t lastVertex = uint64_t(vertexCount - 1u);
  if (lastVertex >
          ((std::numeric_limits<uint64_t>::max)() - positionOffset) /
              stride)
    return 0u;
  const uint64_t lastOffset = lastVertex * stride + positionOffset;
  if (lastOffset > byteCount || kPositionBytes > byteCount - lastOffset ||
      lastOffset > (std::numeric_limits<size_t>::max)())
    return 0u;

  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = kPersistentGpuPackageContentHashSeed;
  for (uint32_t vertex = 0u; vertex < vertexCount; ++vertex) {
    hash = ContinuePersistentGpuPackageContentHash(
        hash, bytes + uint64_t(vertex) * stride + positionOffset,
        size_t(kPositionBytes));
  }
  return hash;
}

PersistentGpuPackageCurrentDrawMatchDisposition
EvaluatePersistentGpuPackageCurrentDrawEquivalence(
    const PersistentGpuPackageCurrentDrawProof& draw,
    const PersistentGpuPackageCurrentDrawPackageProof& package) noexcept {
  using Disposition = PersistentGpuPackageCurrentDrawMatchDisposition;
  if (!draw.requested)
    return Disposition::NotRequested;
  if (!draw.sealed || draw.frameSerial == 0u || draw.mapEpoch == 0u ||
      draw.deviceEpoch == 0u || draw.exactGeometryKeyHash == 0u ||
      draw.instanceIdentity == 0u || draw.meshPayloadIdentity == 0u ||
      draw.renderablePartIdentity == 0u || draw.vertexCount == 0u ||
      draw.indexCount == 0u)
    return Disposition::InvalidCurrentDraw;
  if (!draw.rigidStatic)
    return Disposition::NotRigidStatic;
  if (!draw.opaqueMaterial)
    return Disposition::MaterialRejected;
  if (draw.gpuSkinBacked || draw.vertexBlendEnabled)
    return Disposition::SkinningRouteRejected;
  if (!draw.indexed || !draw.triangleList || !draw.uint16Indices ||
      !draw.exactIndexDomainKnown || draw.fullVertexDomainFallback ||
      !draw.zeroBasedVertexRange || !draw.positionFloat3 ||
      draw.sourceFirstIndex != 0u || draw.actualIndexMin != 0u ||
      draw.actualIndexMax >= draw.vertexCount)
    return Disposition::GeometryContractRejected;
  if (!draw.positionHostCached || !draw.indexHostCached)
    return Disposition::CpuSourceUnavailable;
  if (draw.positionOwnerIdentity == 0u ||
      draw.positionIdentityGeneration == 0u ||
      draw.positionAllocationGeneration == 0u ||
      draw.positionContentGeneration == 0u ||
      draw.indexOwnerIdentity == 0u ||
      draw.indexIdentityGeneration == 0u ||
      draw.indexAllocationGeneration == 0u ||
      draw.indexContentGeneration == 0u ||
      draw.positionContentHash == 0u || draw.indexContentHash == 0u)
    return Disposition::SourceGenerationMissing;
  if (!package.ready)
    return Disposition::PackageNotReady;
  if (!package.frozenPayloadValid || package.packageGeneration == 0u)
    return Disposition::PackageInvalid;
  if (!package.snapshotIdentityExact ||
      package.mapEpoch != draw.mapEpoch ||
      package.deviceEpoch != draw.deviceEpoch ||
      package.geosetDataIdentity != draw.meshPayloadIdentity)
    return Disposition::SnapshotMismatch;
  if (package.primitiveCount != 1u)
    return Disposition::MultiPrimitiveRejected;
  if (package.vertexCount != draw.vertexCount ||
      package.indexCount != draw.indexCount)
    return Disposition::PackageLayoutMismatch;
  if (package.positionContentHash != draw.positionContentHash)
    return Disposition::PositionContentMismatch;
  if (package.indexContentHash != draw.indexContentHash)
    return Disposition::IndexContentMismatch;
  if (package.primitiveOrdinal != 0u ||
      package.primitiveFirstIndex != draw.sourceFirstIndex ||
      package.primitiveIndexCount != draw.indexCount ||
      package.primitiveMinVertex != draw.actualIndexMin ||
      package.primitiveMaxVertex != draw.actualIndexMax)
    return Disposition::PrimitiveMismatch;
  return Disposition::ExactMatch;
}

namespace {

void HashBytesInto(uint64_t& hash, const void* data, size_t size) noexcept {
  hash = ContinuePersistentGpuPackageContentHash(hash, data, size);
}

uint64_t HashBytes(const void* data, size_t size) noexcept {
  return HashPersistentGpuPackageContent(data, size);
}

template <typename T>
void HashValueInto(uint64_t& hash, const T& value) noexcept {
  HashBytesInto(hash, &value, sizeof(value));
}

bool CheckedBytes(uint64_t count, uint64_t stride, uint32_t& result) noexcept {
  if (stride != 0u && count > uint64_t(std::numeric_limits<uint32_t>::max()) /
          stride)
    return false;
  result = uint32_t(count * stride);
  return true;
}

}  // namespace

bool SamePersistentGpuPackagePrimitiveProof(
    const PersistentGpuPackagePrimitiveProof& lhs,
    const PersistentGpuPackagePrimitiveProof& rhs) noexcept {
  return lhs.indexContentHash == rhs.indexContentHash &&
      lhs.ordinal == rhs.ordinal &&
      lhs.primitiveTypeOrMaterialSlot ==
          rhs.primitiveTypeOrMaterialSlot &&
      lhs.firstIndex == rhs.firstIndex &&
      lhs.indexCount == rhs.indexCount &&
      lhs.minVertex == rhs.minVertex &&
      lhs.maxVertex == rhs.maxVertex;
}

bool SamePersistentGpuPackageImmutableProof(
    const PersistentGpuPackageImmutableProof& lhs,
    const PersistentGpuPackageImmutableProof& rhs) noexcept {
  return lhs.immutableModelGeneration == rhs.immutableModelGeneration &&
      lhs.contentHash == rhs.contentHash &&
      lhs.positionContentHash == rhs.positionContentHash &&
      lhs.normalContentHash == rhs.normalContentHash &&
      lhs.vertexGroupContentHash == rhs.vertexGroupContentHash &&
      lhs.uv0ContentHash == rhs.uv0ContentHash &&
      lhs.uv1ContentHash == rhs.uv1ContentHash &&
      lhs.indexContentHash == rhs.indexContentHash &&
      lhs.matrixGroupContentHash == rhs.matrixGroupContentHash &&
      lhs.matrixIndexContentHash == rhs.matrixIndexContentHash &&
      lhs.primitiveProofHash == rhs.primitiveProofHash &&
      lhs.localBoundsHash == rhs.localBoundsHash &&
      lhs.layoutGeneration == rhs.layoutGeneration &&
      lhs.vertexCount == rhs.vertexCount &&
      lhs.indexCount == rhs.indexCount &&
      lhs.uvLayerCount == rhs.uvLayerCount &&
      lhs.primitiveProofCount == rhs.primitiveProofCount &&
      lhs.matrixGroupCount == rhs.matrixGroupCount &&
      lhs.matrixIndexCount == rhs.matrixIndexCount &&
      lhs.maxVertexGroupSlot == rhs.maxVertexGroupSlot &&
      lhs.positionOffset == rhs.positionOffset &&
      lhs.normalOffset == rhs.normalOffset &&
      lhs.groupSlotOffset == rhs.groupSlotOffset &&
      lhs.texcoord0Offset == rhs.texcoord0Offset &&
      lhs.texcoord1Offset == rhs.texcoord1Offset &&
      lhs.staticByteSize == rhs.staticByteSize &&
      lhs.localMinX == rhs.localMinX &&
      lhs.localMinY == rhs.localMinY &&
      lhs.localMinZ == rhs.localMinZ &&
      lhs.localMaxX == rhs.localMaxX &&
      lhs.localMaxY == rhs.localMaxY &&
      lhs.localMaxZ == rhs.localMaxZ;
}

bool BuildPersistentGpuPackageImmutableProof(
    const model::ShadowGeosetResourceRecord& record,
    PersistentGpuPackageImmutableProof& outputProof,
    std::vector<PersistentGpuPackagePrimitiveProof>& outputPrimitiveProofs)
    noexcept {
  outputProof = {};
  outputPrimitiveProofs.clear();
  PersistentGpuPackageImmutableProof proof = {};
  std::vector<PersistentGpuPackagePrimitiveProof> primitiveProofs;

  try {
    const uint64_t vertexCount = record.vertexCount;
    if (record.geosetDataPtr == nullptr || vertexCount == 0u ||
        record.immutableModelGeneration == 0u ||
        record.immutableCaptureStatus !=
            model::ShadowGeosetImmutableCaptureStatus::Complete ||
        record.contentHash == 0u ||
        record.normalCount != vertexCount ||
        record.vertexGroupCount != vertexCount ||
        record.uvLayerCount > 2u ||
        record.uvLayers.size() != record.uvLayerCount ||
        record.positions.size() != vertexCount * 3u ||
        record.normals.size() != vertexCount * 3u ||
        record.vertexGroupIndices.size() != vertexCount ||
        record.primitiveCount == 0u ||
        record.primitiveRecords.size() != record.primitiveCount ||
        record.indexCount == 0u ||
        record.indices.size() != record.indexCount ||
        record.matrixGroupCount == 0u ||
        record.matrixGroupSizes.size() != record.matrixGroupCount ||
        record.matrixIndices.size() != record.matrixIndexCount) {
      return false;
    }

    for (const auto& uv : record.uvLayers) {
      if (uv.uvCount != vertexCount ||
          uv.uvPairs.size() != vertexCount * 2u)
        return false;
    }

    uint64_t matrixIndexSum = 0u;
    for (uint32_t groupSize : record.matrixGroupSizes) {
      if (groupSize == 0u ||
          matrixIndexSum > std::numeric_limits<uint64_t>::max() - groupSize)
        return false;
      matrixIndexSum += groupSize;
    }
    if (matrixIndexSum != record.matrixIndexCount)
      return false;

    uint32_t positionBytes = 0u;
    uint32_t normalBytes = 0u;
    uint32_t groupBytes = 0u;
    uint32_t uvBytes = 0u;
    if (!CheckedBytes(vertexCount, 12u, positionBytes) ||
        !CheckedBytes(vertexCount, 12u, normalBytes) ||
        !CheckedBytes(vertexCount, 1u, groupBytes) ||
        !CheckedBytes(vertexCount, 8u, uvBytes)) {
      return false;
    }
    const uint64_t groupEnd = uint64_t(positionBytes) + normalBytes + groupBytes;
    const uint64_t texcoord0Offset = (groupEnd + 3u) & ~uint64_t(3u);
    const uint64_t staticByteSize = texcoord0Offset +
        uint64_t(uvBytes) * record.uvLayerCount;
    if (texcoord0Offset > std::numeric_limits<uint32_t>::max() ||
        texcoord0Offset + uvBytes >
            std::numeric_limits<uint32_t>::max() ||
        staticByteSize > std::numeric_limits<uint32_t>::max()) {
      return false;
    }

    for (float value : record.normals) {
      if (!std::isfinite(value))
        return false;
    }
    for (const auto& uv : record.uvLayers) {
      for (float value : uv.uvPairs) {
        if (!std::isfinite(value))
          return false;
      }
    }
    const auto maxGroup = std::max_element(
        record.vertexGroupIndices.begin(), record.vertexGroupIndices.end());
    if (maxGroup == record.vertexGroupIndices.end() ||
        *maxGroup >= record.matrixGroupCount)
      return false;

    proof.immutableModelGeneration = record.immutableModelGeneration;
    proof.contentHash = record.contentHash;
    proof.positionContentHash = HashBytes(
        record.positions.data(), record.positions.size() * sizeof(float));
    proof.normalContentHash = HashBytes(
        record.normals.data(), record.normals.size() * sizeof(float));
    proof.vertexGroupContentHash = HashBytes(
        record.vertexGroupIndices.data(), record.vertexGroupIndices.size());
    if (record.uvLayerCount >= 1u) {
      proof.uv0ContentHash = HashBytes(
          record.uvLayers[0].uvPairs.data(),
          record.uvLayers[0].uvPairs.size() * sizeof(float));
    }
    if (record.uvLayerCount >= 2u) {
      proof.uv1ContentHash = HashBytes(
          record.uvLayers[1].uvPairs.data(),
          record.uvLayers[1].uvPairs.size() * sizeof(float));
    }
    proof.indexContentHash = HashBytes(
        record.indices.data(), record.indices.size() * sizeof(uint16_t));
    if (!record.matrixGroupSizes.empty()) {
      proof.matrixGroupContentHash = HashBytes(
          record.matrixGroupSizes.data(),
          record.matrixGroupSizes.size() * sizeof(uint32_t));
    }
    if (!record.matrixIndices.empty()) {
      proof.matrixIndexContentHash = HashBytes(
          record.matrixIndices.data(),
          record.matrixIndices.size() * sizeof(uint32_t));
    }
    if (proof.positionContentHash == 0u ||
        proof.normalContentHash == 0u ||
        proof.vertexGroupContentHash == 0u ||
        proof.indexContentHash == 0u) {
      return false;
    }

    primitiveProofs.reserve(record.primitiveCount);
    uint64_t primitiveAggregate = 0xcbf29ce484222325ull;
    uint64_t firstIndex = 0u;
    for (uint32_t ordinal = 0u; ordinal < record.primitiveCount; ++ordinal) {
      const auto& primitive = record.primitiveRecords[ordinal];
      const uint64_t indexCount = primitive.indexCount;
      if (indexCount == 0u || firstIndex > record.indexCount ||
          indexCount > uint64_t(record.indexCount) - firstIndex)
        return false;

      const auto begin = record.indices.begin() + size_t(firstIndex);
      const auto end = begin + size_t(indexCount);
      const auto minmax = std::minmax_element(begin, end);
      if (minmax.first == end || *minmax.second >= record.vertexCount)
        return false;

      PersistentGpuPackagePrimitiveProof primitiveProof = {};
      primitiveProof.indexContentHash = HashBytes(
          &record.indices[size_t(firstIndex)],
          size_t(indexCount) * sizeof(uint16_t));
      primitiveProof.ordinal = ordinal;
      primitiveProof.primitiveTypeOrMaterialSlot =
          primitive.primitiveTypeOrMaterialSlot;
      primitiveProof.firstIndex = uint32_t(firstIndex);
      primitiveProof.indexCount = uint32_t(indexCount);
      primitiveProof.minVertex = *minmax.first;
      primitiveProof.maxVertex = *minmax.second;
      if (primitiveProof.indexContentHash == 0u)
        return false;

      HashValueInto(primitiveAggregate, primitiveProof.indexContentHash);
      HashValueInto(primitiveAggregate, primitiveProof.ordinal);
      HashValueInto(
          primitiveAggregate, primitiveProof.primitiveTypeOrMaterialSlot);
      HashValueInto(primitiveAggregate, primitiveProof.firstIndex);
      HashValueInto(primitiveAggregate, primitiveProof.indexCount);
      HashValueInto(primitiveAggregate, primitiveProof.minVertex);
      HashValueInto(primitiveAggregate, primitiveProof.maxVertex);
      primitiveProofs.push_back(primitiveProof);
      firstIndex += indexCount;
    }
    if (firstIndex != record.indexCount || primitiveAggregate == 0u)
      return false;

    float bounds[6] = {
        record.positions[0], record.positions[1], record.positions[2],
        record.positions[0], record.positions[1], record.positions[2]};
    for (uint32_t vertex = 0u; vertex < record.vertexCount; ++vertex) {
      const float x = record.positions[size_t(vertex) * 3u + 0u];
      const float y = record.positions[size_t(vertex) * 3u + 1u];
      const float z = record.positions[size_t(vertex) * 3u + 2u];
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
      bounds[0] = std::min(bounds[0], x);
      bounds[1] = std::min(bounds[1], y);
      bounds[2] = std::min(bounds[2], z);
      bounds[3] = std::max(bounds[3], x);
      bounds[4] = std::max(bounds[4], y);
      bounds[5] = std::max(bounds[5], z);
    }

    proof.primitiveProofHash = primitiveAggregate;
    proof.localBoundsHash = HashBytes(bounds, sizeof(bounds));
    proof.layoutGeneration = kPersistentGpuPackageStaticLayoutGeneration;
    proof.vertexCount = record.vertexCount;
    proof.indexCount = record.indexCount;
    proof.uvLayerCount = record.uvLayerCount;
    proof.primitiveProofCount = uint32_t(primitiveProofs.size());
    proof.matrixGroupCount = record.matrixGroupCount;
    proof.matrixIndexCount = record.matrixIndexCount;
    proof.maxVertexGroupSlot = *maxGroup;
    proof.positionOffset = 0u;
    proof.normalOffset = positionBytes;
    proof.groupSlotOffset = positionBytes + normalBytes;
    proof.texcoord0Offset = uint32_t(texcoord0Offset);
    proof.texcoord1Offset = uint32_t(texcoord0Offset) + uvBytes;
    proof.staticByteSize = uint32_t(staticByteSize);
    proof.localMinX = bounds[0];
    proof.localMinY = bounds[1];
    proof.localMinZ = bounds[2];
    proof.localMaxX = bounds[3];
    proof.localMaxY = bounds[4];
    proof.localMaxZ = bounds[5];
    if (proof.localBoundsHash == 0u || proof.primitiveProofCount == 0u)
      return false;
    outputProof = proof;
    outputPrimitiveProofs = std::move(primitiveProofs);
    return true;
  } catch (...) {
    outputProof = {};
    outputPrimitiveProofs.clear();
    return false;
  }
}

bool ValidatePersistentGpuPackagePackedBytes(
    const void* packageBytes,
    uint64_t packageByteCount,
    uint64_t indexRelativeOffset,
    const PersistentGpuPackageImmutableProof& proof,
    const std::vector<PersistentGpuPackagePrimitiveProof>&
        primitiveProofs) noexcept {
  if (packageBytes == nullptr || packageByteCount == 0u ||
      proof.layoutGeneration !=
          kPersistentGpuPackageStaticLayoutGeneration ||
      proof.vertexCount == 0u || proof.indexCount == 0u ||
      primitiveProofs.size() != proof.primitiveProofCount ||
      indexRelativeOffset < proof.staticByteSize ||
      (indexRelativeOffset % alignof(uint16_t)) != 0u)
    return false;

  const uint64_t positionBytes = uint64_t(proof.vertexCount) * 12u;
  const uint64_t normalBytes = uint64_t(proof.vertexCount) * 12u;
  const uint64_t groupBytes = proof.vertexCount;
  const uint64_t uvBytes = uint64_t(proof.vertexCount) * 8u;
  const uint64_t indexBytes = uint64_t(proof.indexCount) * sizeof(uint16_t);
  if (proof.positionOffset != 0u ||
      proof.normalOffset != positionBytes ||
      proof.groupSlotOffset != positionBytes + normalBytes ||
      proof.texcoord0Offset < proof.groupSlotOffset + groupBytes ||
      proof.texcoord1Offset != proof.texcoord0Offset + uvBytes ||
      proof.staticByteSize !=
          proof.texcoord0Offset + uvBytes * proof.uvLayerCount ||
      indexRelativeOffset > packageByteCount ||
      indexBytes > packageByteCount - indexRelativeOffset ||
      indexRelativeOffset + indexBytes != packageByteCount)
    return false;

  const auto* bytes = static_cast<const uint8_t*>(packageBytes);
  if (HashBytes(bytes + proof.positionOffset, size_t(positionBytes)) !=
          proof.positionContentHash ||
      HashBytes(bytes + proof.normalOffset, size_t(normalBytes)) !=
          proof.normalContentHash ||
      HashBytes(bytes + proof.groupSlotOffset, size_t(groupBytes)) !=
          proof.vertexGroupContentHash ||
      HashBytes(bytes + indexRelativeOffset, size_t(indexBytes)) !=
          proof.indexContentHash)
    return false;
  if (proof.uvLayerCount >= 1u &&
      HashBytes(bytes + proof.texcoord0Offset, size_t(uvBytes)) !=
          proof.uv0ContentHash)
    return false;
  if (proof.uvLayerCount >= 2u &&
      HashBytes(bytes + proof.texcoord1Offset, size_t(uvBytes)) !=
          proof.uv1ContentHash)
    return false;

  const uint64_t groupEnd = proof.groupSlotOffset + groupBytes;
  for (uint64_t offset = groupEnd; offset < proof.texcoord0Offset; ++offset) {
    if (bytes[offset] != 0u)
      return false;
  }
  for (uint64_t offset = proof.staticByteSize;
       offset < indexRelativeOffset; ++offset) {
    if (bytes[offset] != 0u)
      return false;
  }

  float bounds[6] = {};
  const auto* positions = reinterpret_cast<const float*>(
      bytes + proof.positionOffset);
  bounds[0] = bounds[3] = positions[0];
  bounds[1] = bounds[4] = positions[1];
  bounds[2] = bounds[5] = positions[2];
  for (uint32_t vertex = 0u; vertex < proof.vertexCount; ++vertex) {
    const float x = positions[size_t(vertex) * 3u + 0u];
    const float y = positions[size_t(vertex) * 3u + 1u];
    const float z = positions[size_t(vertex) * 3u + 2u];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
      return false;
    bounds[0] = std::min(bounds[0], x);
    bounds[1] = std::min(bounds[1], y);
    bounds[2] = std::min(bounds[2], z);
    bounds[3] = std::max(bounds[3], x);
    bounds[4] = std::max(bounds[4], y);
    bounds[5] = std::max(bounds[5], z);
  }
  if (HashBytes(bounds, sizeof(bounds)) != proof.localBoundsHash ||
      bounds[0] != proof.localMinX || bounds[1] != proof.localMinY ||
      bounds[2] != proof.localMinZ || bounds[3] != proof.localMaxX ||
      bounds[4] != proof.localMaxY || bounds[5] != proof.localMaxZ)
    return false;

  const auto* indices = reinterpret_cast<const uint16_t*>(
      bytes + indexRelativeOffset);
  uint64_t expectedFirstIndex = 0u;
  uint64_t primitiveAggregate = 0xcbf29ce484222325ull;
  for (const auto& primitive : primitiveProofs) {
    if (primitive.firstIndex != expectedFirstIndex ||
        primitive.indexCount == 0u ||
        primitive.indexCount > proof.indexCount - expectedFirstIndex)
      return false;
    const uint16_t* begin = indices + primitive.firstIndex;
    const uint16_t* end = begin + primitive.indexCount;
    const auto minmax = std::minmax_element(begin, end);
    if (minmax.first == end || *minmax.first != primitive.minVertex ||
        *minmax.second != primitive.maxVertex ||
        *minmax.second >= proof.vertexCount ||
        HashBytes(begin, size_t(primitive.indexCount) * sizeof(uint16_t)) !=
            primitive.indexContentHash)
      return false;
    HashValueInto(primitiveAggregate, primitive.indexContentHash);
    HashValueInto(primitiveAggregate, primitive.ordinal);
    HashValueInto(
        primitiveAggregate, primitive.primitiveTypeOrMaterialSlot);
    HashValueInto(primitiveAggregate, primitive.firstIndex);
    HashValueInto(primitiveAggregate, primitive.indexCount);
    HashValueInto(primitiveAggregate, primitive.minVertex);
    HashValueInto(primitiveAggregate, primitive.maxVertex);
    expectedFirstIndex += primitive.indexCount;
  }
  return expectedFirstIndex == proof.indexCount &&
      primitiveAggregate == proof.primitiveProofHash;
}

}  // namespace dxvk::war3::gpu_skin
