#include "war3_model_resource_cache.h"

#include "../../d3d9_war3_debug.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_memory.h"
#include "../tools/war3_resource_residency_census.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace dxvk::war3::model {

namespace {

constexpr uint32_t kMaxGeosetsPerModel = 512u;
constexpr uint32_t kMaxVertexCount = 1u << 20;
constexpr uint32_t kMaxIndexCount = 1u << 22;
constexpr uint32_t kMaxUvLayers = 8u;
constexpr uint32_t kMaxMatrixGroupCount = 2048u;
constexpr uint32_t kMaxMatrixIndexCount = 1u << 15;

std::atomic<uint32_t> g_modelBindFailLogCount{0};
std::atomic<uint32_t> g_modelBindOkLogCount{0};

uint64_t Fnv1a64(const void *data, size_t size,
                 uint64_t seed = 1469598103934665603ull) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  uint64_t hash = seed;
  for (size_t i = 0; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t CombineHash(uint64_t a, uint64_t b) {
  const uint64_t x = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
  return x ? x : 0x9E3779B97F4A7C15ull;
}

void NoteRuntimeOwnerIndex(std::unordered_map<void*, void*>& index,
                           void* key,
                           void* runtimeModelPtr) {
  if (key == nullptr || runtimeModelPtr == nullptr)
    return;

  const auto inserted = index.emplace(key, runtimeModelPtr);
  if (!inserted.second && inserted.first->second != runtimeModelPtr) {
    // 空值表示该 key 存在歧义；必要时慢速评分路径仍可继续判断，但热路径不会
    // 信任这个直接索引。
    inserted.first->second = nullptr;
  }
}

template <typename T>
bool CopyPodArray(const T *src, uint32_t count, std::vector<T> &out) {
  out.clear();
  if (src == nullptr || count == 0)
    return false;

  const size_t bytes = size_t(count) * sizeof(T);
  if (!dxvk::war3::IsReadableRange(src, bytes))
    return false;

  out.resize(count);
  std::memcpy(out.data(), src, bytes);
  return true;
}

bool CopyFlatFloatArray(const float *src, uint32_t count, uint32_t arity,
                        std::vector<float> &out) {
  out.clear();
  if (src == nullptr || count == 0 || arity == 0)
    return false;

  const size_t floatCount = size_t(count) * size_t(arity);
  const size_t bytes = floatCount * sizeof(float);
  if (!dxvk::war3::IsReadableRange(src, bytes))
    return false;

  out.resize(floatCount);
  std::memcpy(out.data(), src, bytes);
  return true;
}

bool TryReadPtrFast(const void *base, size_t offset, void *&out) {
  out = nullptr;
  return base != nullptr && dxvk::war3::SafeReadPtrFast(base, offset, out) &&
         out != nullptr;
}

bool TryReadU32Fast(const void *base, size_t offset, uint32_t &out) {
  out = 0;
  return base != nullptr && dxvk::war3::SafeReadU32Fast(base, offset, out);
}

bool TryReadGeosetHandleArray(const void* ownerPtr, size_t countOffset,
                              size_t arrayOffset, std::vector<void*>& out) {
  out.clear();

  uint32_t geosetCount = 0;
  void* geosetsPtr = nullptr;
  if (!TryReadU32Fast(ownerPtr, countOffset, geosetCount) ||
      !TryReadPtrFast(ownerPtr, arrayOffset, geosetsPtr)) {
    return false;
  }

  geosetCount = std::min(geosetCount, kMaxGeosetsPerModel);
  if (geosetCount == 0 ||
      !dxvk::war3::IsReadableRange(geosetsPtr,
                                   size_t(geosetCount) * sizeof(void*))) {
    return false;
  }

  out.resize(geosetCount, nullptr);
  std::memcpy(out.data(), geosetsPtr, size_t(geosetCount) * sizeof(void*));
  return true;
}

bool LooksLikeRuntimeModelPtr(const void* candidate) {
  if (candidate == nullptr)
    return false;

  const uintptr_t candidateValue = reinterpret_cast<uintptr_t>(candidate);
  if (candidateValue < 0x10000u)
    return false;

  void* ownedHandlePtr = nullptr;
  uint32_t runtimeGeosetCount = 0u;
  void* runtimeGeosets = nullptr;
  uint32_t finalPoseMatrixCount = 0u;
  void* finalPoseMatrixArray = nullptr;
  const bool hasOwnedHandle =
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
                      ownedHandlePtr) &&
      ownedHandlePtr != nullptr &&
      ShadowModelResourceCache::instance().resolveDirectModelResourcePtr(
          ownedHandlePtr) != nullptr;
  const bool hasRuntimeGeosets =
      SafeReadU32Fast(candidate, dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                      runtimeGeosetCount) &&
      runtimeGeosetCount > 0u &&
      runtimeGeosetCount < 4096u &&
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::RuntimeGeosets,
                      runtimeGeosets) &&
      runtimeGeosets != nullptr &&
      dxvk::war3::IsReadableRange(
          runtimeGeosets,
          size_t(runtimeGeosetCount > 4u ? 4u : runtimeGeosetCount) *
              sizeof(void*));
  const bool hasFinalPoseArray =
      SafeReadU32Fast(candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
                      finalPoseMatrixCount) &&
      finalPoseMatrixCount > 0u &&
      finalPoseMatrixCount <= 256u &&
      SafeReadPtrFast(candidate,
                      dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
                      finalPoseMatrixArray) &&
      finalPoseMatrixArray != nullptr &&
      dxvk::war3::IsReadableRange(finalPoseMatrixArray,
                                  size_t(sizeof(float)) * 16u);
  return hasRuntimeGeosets || (hasOwnedHandle && hasFinalPoseArray);
}

bool LooksLikeDirectModelResourcePtr(const void* modelResourcePtr) {
  if (modelResourcePtr == nullptr)
    return false;

  uint32_t geosetCount = 0;
  void* geosetsPtr = nullptr;
  if (!TryReadU32Fast(modelResourcePtr, dxvk::war3::CModelDataOffsets::GeosetCount,
                      geosetCount) ||
      !TryReadPtrFast(modelResourcePtr, dxvk::war3::CModelDataOffsets::Geosets,
                      geosetsPtr)) {
    return false;
  }

  if (geosetCount == 0 || geosetCount > kMaxGeosetsPerModel)
    return false;

  return dxvk::war3::IsReadableRange(geosetsPtr,
                                     size_t(geosetCount) * sizeof(void*));
}

void* TryResolveDirectModelResourcePtr(const void* modelResourceOrHandlePtr) {
  if (LooksLikeDirectModelResourcePtr(modelResourceOrHandlePtr))
    return const_cast<void*>(modelResourceOrHandlePtr);

  if (modelResourceOrHandlePtr == nullptr)
    return nullptr;

  constexpr size_t kCandidateOffsets[] = {
      0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u, 0x28u,
      0x2Cu, 0x30u, 0x34u, 0x38u, 0x3Cu, 0x40u, 0x44u, 0x48u, 0x4Cu,
      0x50u, 0x54u, 0x58u, 0x5Cu, 0x60u, 0x64u, 0x68u, 0x6Cu, 0x70u,
      0x74u, 0x78u, 0x7Cu};
  for (size_t offset : kCandidateOffsets) {
    void* candidate = nullptr;
    if (!TryReadPtrFast(modelResourceOrHandlePtr, offset, candidate) ||
        candidate == nullptr) {
      continue;
    }
    if (LooksLikeDirectModelResourcePtr(candidate))
      return candidate;

    for (size_t nestedOffset : kCandidateOffsets) {
      void* nestedCandidate = nullptr;
      if (!TryReadPtrFast(candidate, nestedOffset, nestedCandidate) ||
          nestedCandidate == nullptr) {
        continue;
      }
      if (LooksLikeDirectModelResourcePtr(nestedCandidate))
        return nestedCandidate;
    }
  }

  return nullptr;
}

void MergeGeosetRecord(ShadowGeosetResourceRecord &dst,
                       const ShadowGeosetResourceRecord &src) {
  if (src.geosetPtr)
    dst.geosetPtr = src.geosetPtr;
  if (src.geosetDataPtr)
    dst.geosetDataPtr = src.geosetDataPtr;
  if (src.modelResourcePtr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (src.modelKey != 0)
    dst.modelKey = src.modelKey;
  if (src.prefersRuntimeContract)
    dst.prefersRuntimeContract = true;
  if (src.geosetIndex != kInvalidShadowGeosetIndex)
    dst.geosetIndex = src.geosetIndex;
  if (src.materialOrLayoutSlot != 0)
    dst.materialOrLayoutSlot = src.materialOrLayoutSlot;
  if (src.layoutOrMaterialSlot != 0)
    dst.layoutOrMaterialSlot = src.layoutOrMaterialSlot;
  if (src.mergedGeosetSlotOrBindingIndex != 0)
    dst.mergedGeosetSlotOrBindingIndex = src.mergedGeosetSlotOrBindingIndex;
  if (src.vertexCount != 0)
    dst.vertexCount = src.vertexCount;
  if (!src.positions.empty())
    dst.positions = src.positions;
  if (src.normalCount != 0)
    dst.normalCount = src.normalCount;
  if (!src.normals.empty())
    dst.normals = src.normals;
  if (src.vertexGroupCount != 0)
    dst.vertexGroupCount = src.vertexGroupCount;
  if (!src.vertexGroupIndices.empty())
    dst.vertexGroupIndices = src.vertexGroupIndices;
  if (src.uvLayerCount != 0)
    dst.uvLayerCount = src.uvLayerCount;
  if (!src.uvLayers.empty())
    dst.uvLayers = src.uvLayers;
  if (src.primitiveCount != 0)
    dst.primitiveCount = src.primitiveCount;
  if (!src.primitiveRecords.empty())
    dst.primitiveRecords = src.primitiveRecords;
  if (src.indexCount != 0)
    dst.indexCount = src.indexCount;
  if (!src.indices.empty())
    dst.indices = src.indices;
  if (src.matrixGroupCount != 0)
    dst.matrixGroupCount = src.matrixGroupCount;
  if (!src.matrixGroupSizes.empty())
    dst.matrixGroupSizes = src.matrixGroupSizes;
  if (src.matrixIndexCount != 0)
    dst.matrixIndexCount = src.matrixIndexCount;
  if (!src.matrixIndices.empty())
    dst.matrixIndices = src.matrixIndices;
  if (src.contentHash != 0)
    dst.contentHash = src.contentHash;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
  if (src.lastRuntimeRefreshFrame > dst.lastRuntimeRefreshFrame)
    dst.lastRuntimeRefreshFrame = src.lastRuntimeRefreshFrame;
}

void MergeModelRecord(ShadowModelResourceRecord &dst,
                      const ShadowModelResourceRecord &src) {
  if (src.runtimeModelPtr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (src.modelResourcePtr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (src.modelKey != 0)
    dst.modelKey = src.modelKey;
  if (src.geosetCount != 0)
    dst.geosetCount = src.geosetCount;
  if (!src.geosetPtrs.empty())
    dst.geosetPtrs = src.geosetPtrs;
  if (!src.geosetDataPtrs.empty())
    dst.geosetDataPtrs = src.geosetDataPtrs;
  if (src.geosetCount != 0u || !src.geosetPtrs.empty() ||
      !src.geosetDataPtrs.empty() || src.readyGeosetCount != 0u)
    dst.readyGeosetCount = src.readyGeosetCount;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
}

bool HasSameGeosetConsumerContent(const ShadowGeosetResourceRecord& a,
                                  const ShadowGeosetResourceRecord& b) {
  const auto sameUvLayers = [&]() {
    if (a.uvLayers.size() != b.uvLayers.size())
      return false;
    for (size_t i = 0; i < a.uvLayers.size(); ++i) {
      if (a.uvLayers[i].uvCount != b.uvLayers[i].uvCount ||
          !SameShadowGeosetFloatPayloadBytes(
              a.uvLayers[i].uvPairs, b.uvLayers[i].uvPairs)) {
        return false;
      }
    }
    return true;
  };
  const auto samePrimitiveRecords = [&]() {
    if (a.primitiveRecords.size() != b.primitiveRecords.size())
      return false;
    for (size_t i = 0; i < a.primitiveRecords.size(); ++i) {
      if (a.primitiveRecords[i].primitiveTypeOrMaterialSlot !=
              b.primitiveRecords[i].primitiveTypeOrMaterialSlot ||
          a.primitiveRecords[i].indexCount !=
              b.primitiveRecords[i].indexCount) {
        return false;
      }
    }
    return true;
  };
  return a.geosetPtr == b.geosetPtr &&
         a.geosetDataPtr == b.geosetDataPtr &&
         a.modelResourcePtr == b.modelResourcePtr &&
         a.modelKey == b.modelKey &&
         a.prefersRuntimeContract == b.prefersRuntimeContract &&
         a.geosetIndex == b.geosetIndex &&
         a.materialOrLayoutSlot == b.materialOrLayoutSlot &&
         a.layoutOrMaterialSlot == b.layoutOrMaterialSlot &&
         a.mergedGeosetSlotOrBindingIndex ==
             b.mergedGeosetSlotOrBindingIndex &&
         a.vertexCount == b.vertexCount &&
         SameShadowGeosetFloatPayloadBytes(a.positions, b.positions) &&
         a.normalCount == b.normalCount &&
         SameShadowGeosetFloatPayloadBytes(a.normals, b.normals) &&
         a.vertexGroupCount == b.vertexGroupCount &&
         a.vertexGroupIndices == b.vertexGroupIndices &&
         a.uvLayerCount == b.uvLayerCount &&
         sameUvLayers() &&
         a.primitiveCount == b.primitiveCount &&
         samePrimitiveRecords() &&
         a.indexCount == b.indexCount &&
         a.indices == b.indices &&
         a.matrixGroupCount == b.matrixGroupCount &&
         a.matrixGroupSizes == b.matrixGroupSizes &&
         a.matrixIndexCount == b.matrixIndexCount &&
         a.matrixIndices == b.matrixIndices &&
         a.contentHash == b.contentHash;
}

bool HasSameModelConsumerContent(const ShadowModelResourceRecord& a,
                                 const ShadowModelResourceRecord& b) {
  return a.runtimeModelPtr == b.runtimeModelPtr &&
         a.modelResourcePtr == b.modelResourcePtr &&
         a.modelKey == b.modelKey &&
         a.geosetCount == b.geosetCount &&
         a.geosetPtrs == b.geosetPtrs &&
         a.geosetDataPtrs == b.geosetDataPtrs &&
         a.readyGeosetCount == b.readyGeosetCount;
}

uint64_t ComputeGeosetContentHash(const ShadowGeosetResourceRecord &record) {
  uint64_t hash = Fnv1a64(&record.vertexCount, sizeof(record.vertexCount));
  hash = CombineHash(hash,
                     Fnv1a64(&record.normalCount, sizeof(record.normalCount)));
  hash = CombineHash(
      hash, Fnv1a64(&record.vertexGroupCount,
                    sizeof(record.vertexGroupCount)));
  hash = CombineHash(hash,
                     Fnv1a64(&record.uvLayerCount,
                             sizeof(record.uvLayerCount)));
  hash = CombineHash(hash,
                     Fnv1a64(&record.primitiveCount,
                             sizeof(record.primitiveCount)));
  hash = CombineHash(hash, Fnv1a64(&record.indexCount, sizeof(record.indexCount)));
  hash = CombineHash(hash,
                     Fnv1a64(&record.matrixGroupCount,
                             sizeof(record.matrixGroupCount)));
  hash = CombineHash(hash,
                     Fnv1a64(&record.matrixIndexCount,
                             sizeof(record.matrixIndexCount)));
  if (!record.positions.empty()) {
    hash = CombineHash(hash,
                       Fnv1a64(record.positions.data(),
                               record.positions.size() * sizeof(float)));
  }
  if (!record.normals.empty()) {
    hash = CombineHash(hash,
                       Fnv1a64(record.normals.data(),
                               record.normals.size() * sizeof(float)));
  }
  for (const ShadowGeosetUvLayerRecord &uv : record.uvLayers) {
    hash = CombineHash(hash, Fnv1a64(&uv.uvCount, sizeof(uv.uvCount)));
    if (!uv.uvPairs.empty()) {
      hash = CombineHash(hash,
                         Fnv1a64(uv.uvPairs.data(),
                                 uv.uvPairs.size() * sizeof(float)));
    }
  }
  if (!record.primitiveRecords.empty()) {
    hash = CombineHash(
        hash, Fnv1a64(record.primitiveRecords.data(),
                      record.primitiveRecords.size() *
                          sizeof(ShadowGeosetPrimitiveRecord)));
  }
  if (!record.indices.empty()) {
    hash = CombineHash(hash, Fnv1a64(record.indices.data(),
                                     record.indices.size() * sizeof(uint16_t)));
  }
  if (!record.vertexGroupIndices.empty()) {
    hash = CombineHash(
        hash, Fnv1a64(record.vertexGroupIndices.data(),
                      record.vertexGroupIndices.size() * sizeof(uint8_t)));
  }
  if (!record.matrixGroupSizes.empty()) {
    hash = CombineHash(hash, Fnv1a64(record.matrixGroupSizes.data(),
                                     record.matrixGroupSizes.size() *
                                         sizeof(uint32_t)));
  }
  if (!record.matrixIndices.empty()) {
    hash = CombineHash(hash, Fnv1a64(record.matrixIndices.data(),
                                     record.matrixIndices.size() *
                                         sizeof(uint32_t)));
  }
  return hash;
}

bool CaptureGeosetRecord(void *geosetPtr, ShadowGeosetResourceRecord &record) {
  record = {};
  if (geosetPtr == nullptr)
    return false;

  void *geosetDataPtr = nullptr;
  if (!TryReadPtrFast(geosetPtr, dxvk::war3::CGeosetOffsets::GeosetData,
                      geosetDataPtr)) {
    return false;
  }

  record.geosetPtr = geosetPtr;
  record.geosetDataPtr = geosetDataPtr;
  TryReadU32Fast(geosetPtr, dxvk::war3::CGeosetOffsets::MaterialOrLayoutSlot,
                 record.materialOrLayoutSlot);
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::LayoutOrMaterialSlot,
                 record.layoutOrMaterialSlot);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MergedGeosetBindingIndex,
                 record.mergedGeosetSlotOrBindingIndex);

  uint32_t vertexCount = 0;
  uint32_t normalCount = 0;
  uint32_t vertexGroupCount = 0;
  uint32_t uvLayerCount = 0;
  uint32_t primitiveCount = 0;
  uint32_t indexCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t matrixIndexCount = 0;
  void *positionsPtr = nullptr;
  void *normalsPtr = nullptr;
  void *vertexGroupsPtr = nullptr;
  void *uvLayersPtr = nullptr;
  void *primitiveRecordsPtr = nullptr;
  void *indicesPtr = nullptr;
  void *matrixGroupSizesPtr = nullptr;
  void *matrixIndicesPtr = nullptr;

  const bool vertexCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexCount,
      vertexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexPositions, positionsPtr);
  const bool normalCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::NormalCount,
      normalCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::NormalVectors, normalsPtr);
  const bool vertexGroupCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexGroupCount,
      vertexGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexGroupIndices,
                 vertexGroupsPtr);
  const bool uvLayerCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayerCount,
      uvLayerCount);
  TryReadPtrFast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayers,
                 uvLayersPtr);
  const bool primitiveCountRead = TryReadU32Fast(
      geosetDataPtr,
      dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
      primitiveCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
                 primitiveRecordsPtr);
  const bool indexCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::IndexCount,
      indexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::IndexBufferU16, indicesPtr);
  const bool matrixGroupCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
      matrixGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
                 matrixGroupSizesPtr);
  const bool matrixIndexCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
      matrixIndexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixIndices,
                 matrixIndicesPtr);

  record.vertexCount = std::min(vertexCount, kMaxVertexCount);
  record.normalCount = std::min(normalCount, kMaxVertexCount);
  record.vertexGroupCount = std::min(vertexGroupCount, kMaxVertexCount);
  record.uvLayerCount = std::min(uvLayerCount, kMaxUvLayers);
  record.primitiveCount = std::min(primitiveCount, kMaxIndexCount);
  record.indexCount = std::min(indexCount, kMaxIndexCount);
  record.matrixGroupCount = std::min(matrixGroupCount, kMaxMatrixGroupCount);
  record.matrixIndexCount = std::min(matrixIndexCount, kMaxMatrixIndexCount);

  const bool rawCountsReadable = vertexCountRead && normalCountRead &&
      vertexGroupCountRead && uvLayerCountRead && primitiveCountRead &&
      indexCountRead && matrixGroupCountRead && matrixIndexCountRead;
  const bool rawCountsWithinLimits = rawCountsReadable && vertexCount != 0u &&
      vertexCount <= kMaxVertexCount && normalCount <= kMaxVertexCount &&
      vertexGroupCount <= kMaxVertexCount && uvLayerCount <= kMaxUvLayers &&
      primitiveCount <= kMaxIndexCount && indexCount <= kMaxIndexCount &&
      matrixGroupCount <= kMaxMatrixGroupCount &&
      matrixIndexCount <= kMaxMatrixIndexCount;
  const bool positionsCopied = CopyFlatFloatArray(
      static_cast<const float *>(positionsPtr), record.vertexCount,
      3u, record.positions);
  const bool normalsCopied = record.normalCount == 0u || CopyFlatFloatArray(
      static_cast<const float *>(normalsPtr), record.normalCount,
      3u, record.normals);
  const bool groupsCopied = record.vertexGroupCount == 0u || CopyPodArray(
      static_cast<const uint8_t *>(vertexGroupsPtr),
      record.vertexGroupCount, record.vertexGroupIndices);
  const bool indicesCopied = record.indexCount == 0u || CopyPodArray(
      static_cast<const uint16_t *>(indicesPtr), record.indexCount,
      record.indices);
  const bool matrixGroupsCopied = record.matrixGroupCount == 0u || CopyPodArray(
      static_cast<const uint32_t *>(matrixGroupSizesPtr),
      record.matrixGroupCount, record.matrixGroupSizes);
  const bool matrixIndicesCopied = record.matrixIndexCount == 0u || CopyPodArray(
      static_cast<const uint32_t *>(matrixIndicesPtr),
      record.matrixIndexCount, record.matrixIndices);

  std::vector<dxvk::war3::GeosetPrimitiveRecord> primitivesRaw;
  const bool primitivesCopied = record.primitiveCount == 0u || CopyPodArray(
      static_cast<const dxvk::war3::GeosetPrimitiveRecord *>(
                       primitiveRecordsPtr),
                   record.primitiveCount, primitivesRaw);
  if (primitivesCopied) {
    record.primitiveRecords.reserve(primitivesRaw.size());
    for (const auto &primitive : primitivesRaw) {
      record.primitiveRecords.push_back(ShadowGeosetPrimitiveRecord{
          primitive.primitive_type_or_material_slot, primitive.index_count});
    }
  }

  bool uvLayersCopied = record.uvLayerCount == 0u;
  if (uvLayersPtr != nullptr && record.uvLayerCount != 0 &&
      dxvk::war3::IsReadableRange(
          uvLayersPtr, size_t(record.uvLayerCount) *
                          sizeof(dxvk::war3::GeosetUvLayerVec2ArrayRecord))) {
    auto *layers = static_cast<const dxvk::war3::GeosetUvLayerVec2ArrayRecord *>(
        uvLayersPtr);
    record.uvLayers.reserve(record.uvLayerCount);
    uvLayersCopied = true;
    for (uint32_t i = 0; i < record.uvLayerCount; ++i) {
      ShadowGeosetUvLayerRecord uvRecord = {};
      uvRecord.uvCount = std::min(layers[i].count, record.vertexCount);
      const float *uvData =
          layers[i].data != nullptr ? layers[i].data : layers[i].inline_storage;
      const bool exactUvCount = layers[i].count == record.vertexCount;
      const bool copied = exactUvCount && CopyFlatFloatArray(
          uvData, uvRecord.uvCount, 2u, uvRecord.uvPairs);
      uvLayersCopied = uvLayersCopied && copied;
      record.uvLayers.emplace_back(std::move(uvRecord));
    }
  }

  record.immutableCaptureStatus =
      rawCountsWithinLimits && positionsCopied && normalsCopied &&
          groupsCopied && indicesCopied && matrixGroupsCopied &&
          matrixIndicesCopied && primitivesCopied && uvLayersCopied
      ? ShadowGeosetImmutableCaptureStatus::Complete
      : ShadowGeosetImmutableCaptureStatus::AttemptedFailed;
  record.contentHash = ComputeGeosetContentHash(record);
  return true;
}

bool CaptureGeosetHeaderRecord(void *geosetPtr,
                               ShadowGeosetResourceRecord &record) {
  record = {};
  if (geosetPtr == nullptr)
    return false;

  void *geosetDataPtr = nullptr;
  if (!TryReadPtrFast(geosetPtr, dxvk::war3::CGeosetOffsets::GeosetData,
                      geosetDataPtr)) {
    return false;
  }

  record.geosetPtr = geosetPtr;
  record.geosetDataPtr = geosetDataPtr;
  TryReadU32Fast(geosetPtr, dxvk::war3::CGeosetOffsets::MaterialOrLayoutSlot,
                 record.materialOrLayoutSlot);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::LayoutOrMaterialSlot,
                 record.layoutOrMaterialSlot);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MergedGeosetBindingIndex,
                 record.mergedGeosetSlotOrBindingIndex);
  return true;
}

bool CaptureGeosetRecordFromKnownPtrs(void* geosetPtr, void* geosetDataPtr,
                                      ShadowGeosetResourceRecord& record) {
  record = {};
  if (geosetPtr == nullptr && geosetDataPtr == nullptr)
    return false;

  if (geosetPtr != nullptr && CaptureGeosetRecord(geosetPtr, record)) {
    // Never overwrite the data identity actually captured through geosetPtr
    // with a caller hint.  A mismatch means the hint is stale and allowing it
    // would merge payload bytes across two source identities.
    if (geosetDataPtr != nullptr &&
        record.geosetDataPtr != geosetDataPtr)
      return false;
    return true;
  }

  if (geosetDataPtr == nullptr)
    return false;

  record.geosetPtr = geosetPtr;
  record.geosetDataPtr = geosetDataPtr;
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::LayoutOrMaterialSlot,
                 record.layoutOrMaterialSlot);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MergedGeosetBindingIndex,
                 record.mergedGeosetSlotOrBindingIndex);

  uint32_t vertexCount = 0;
  uint32_t normalCount = 0;
  uint32_t vertexGroupCount = 0;
  uint32_t uvLayerCount = 0;
  uint32_t primitiveCount = 0;
  uint32_t indexCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t matrixIndexCount = 0;
  void* positionsPtr = nullptr;
  void* normalsPtr = nullptr;
  void* vertexGroupsPtr = nullptr;
  void* uvLayersPtr = nullptr;
  void* primitiveRecordsPtr = nullptr;
  void* indicesPtr = nullptr;
  void* matrixGroupSizesPtr = nullptr;
  void* matrixIndicesPtr = nullptr;

  const bool vertexCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexCount,
      vertexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexPositions, positionsPtr);
  const bool normalCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::NormalCount,
      normalCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::NormalVectors, normalsPtr);
  const bool vertexGroupCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexGroupCount,
      vertexGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexGroupIndices,
                 vertexGroupsPtr);
  const bool uvLayerCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayerCount,
      uvLayerCount);
  TryReadPtrFast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayers,
                 uvLayersPtr);
  const bool primitiveCountRead = TryReadU32Fast(
      geosetDataPtr,
      dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
      primitiveCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
                 primitiveRecordsPtr);
  const bool indexCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::IndexCount,
      indexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::IndexBufferU16, indicesPtr);
  const bool matrixGroupCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
      matrixGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
                 matrixGroupSizesPtr);
  const bool matrixIndexCountRead = TryReadU32Fast(
      geosetDataPtr, dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
      matrixIndexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixIndices,
                 matrixIndicesPtr);

  record.vertexCount = std::min(vertexCount, kMaxVertexCount);
  record.normalCount = std::min(normalCount, kMaxVertexCount);
  record.vertexGroupCount = std::min(vertexGroupCount, kMaxVertexCount);
  record.uvLayerCount = std::min(uvLayerCount, kMaxUvLayers);
  record.primitiveCount = std::min(primitiveCount, kMaxIndexCount);
  record.indexCount = std::min(indexCount, kMaxIndexCount);
  record.matrixGroupCount = std::min(matrixGroupCount, kMaxMatrixGroupCount);
  record.matrixIndexCount = std::min(matrixIndexCount, kMaxMatrixIndexCount);

  const bool rawCountsReadable = vertexCountRead && normalCountRead &&
      vertexGroupCountRead && uvLayerCountRead && primitiveCountRead &&
      indexCountRead && matrixGroupCountRead && matrixIndexCountRead;
  const bool rawCountsWithinLimits = rawCountsReadable && vertexCount != 0u &&
      vertexCount <= kMaxVertexCount && normalCount <= kMaxVertexCount &&
      vertexGroupCount <= kMaxVertexCount && uvLayerCount <= kMaxUvLayers &&
      primitiveCount <= kMaxIndexCount && indexCount <= kMaxIndexCount &&
      matrixGroupCount <= kMaxMatrixGroupCount &&
      matrixIndexCount <= kMaxMatrixIndexCount;
  const bool positionsCopied = CopyFlatFloatArray(
      static_cast<const float*>(positionsPtr), record.vertexCount,
      3u, record.positions);
  const bool normalsCopied = record.normalCount == 0u || CopyFlatFloatArray(
      static_cast<const float*>(normalsPtr), record.normalCount,
      3u, record.normals);
  const bool groupsCopied = record.vertexGroupCount == 0u || CopyPodArray(
      static_cast<const uint8_t*>(vertexGroupsPtr),
      record.vertexGroupCount, record.vertexGroupIndices);
  const bool indicesCopied = record.indexCount == 0u || CopyPodArray(
      static_cast<const uint16_t*>(indicesPtr), record.indexCount,
      record.indices);
  const bool matrixGroupsCopied = record.matrixGroupCount == 0u || CopyPodArray(
      static_cast<const uint32_t*>(matrixGroupSizesPtr),
      record.matrixGroupCount, record.matrixGroupSizes);
  const bool matrixIndicesCopied = record.matrixIndexCount == 0u || CopyPodArray(
      static_cast<const uint32_t*>(matrixIndicesPtr),
      record.matrixIndexCount, record.matrixIndices);

  std::vector<dxvk::war3::GeosetPrimitiveRecord> primitivesRaw;
  const bool primitivesCopied = record.primitiveCount == 0u || CopyPodArray(
      static_cast<const dxvk::war3::GeosetPrimitiveRecord*>(
                       primitiveRecordsPtr),
                   record.primitiveCount, primitivesRaw);
  if (primitivesCopied) {
    record.primitiveRecords.reserve(primitivesRaw.size());
    for (const auto& primitive : primitivesRaw) {
      record.primitiveRecords.push_back(ShadowGeosetPrimitiveRecord{
          primitive.primitive_type_or_material_slot, primitive.index_count});
    }
  }

  bool uvLayersCopied = record.uvLayerCount == 0u;
  if (uvLayersPtr != nullptr && record.uvLayerCount != 0 &&
      dxvk::war3::IsReadableRange(
          uvLayersPtr, size_t(record.uvLayerCount) *
                          sizeof(dxvk::war3::GeosetUvLayerVec2ArrayRecord))) {
    auto* layers = static_cast<const dxvk::war3::GeosetUvLayerVec2ArrayRecord*>(
        uvLayersPtr);
    record.uvLayers.reserve(record.uvLayerCount);
    uvLayersCopied = true;
    for (uint32_t i = 0; i < record.uvLayerCount; ++i) {
      ShadowGeosetUvLayerRecord uvRecord = {};
      uvRecord.uvCount = std::min(layers[i].count, record.vertexCount);
      const float* uvData =
          layers[i].data != nullptr ? layers[i].data : layers[i].inline_storage;
      const bool exactUvCount = layers[i].count == record.vertexCount;
      const bool copied = exactUvCount && CopyFlatFloatArray(
          uvData, uvRecord.uvCount, 2u, uvRecord.uvPairs);
      uvLayersCopied = uvLayersCopied && copied;
      record.uvLayers.emplace_back(std::move(uvRecord));
    }
  }

  record.immutableCaptureStatus =
      rawCountsWithinLimits && positionsCopied && normalsCopied &&
          groupsCopied && indicesCopied && matrixGroupsCopied &&
          matrixIndicesCopied && primitivesCopied && uvLayersCopied
      ? ShadowGeosetImmutableCaptureStatus::Complete
      : ShadowGeosetImmutableCaptureStatus::AttemptedFailed;
  record.contentHash = ComputeGeosetContentHash(record);
  return true;
}

bool TryReadGeosetDataPtr(void* geosetPtr, void*& outGeosetDataPtr) {
  outGeosetDataPtr = nullptr;
  return geosetPtr != nullptr &&
         TryReadPtrFast(geosetPtr, dxvk::war3::CGeosetOffsets::GeosetData,
                        outGeosetDataPtr);
}

void EnsureModelRecordCapacity(ShadowModelResourceRecord& record,
                               uint32_t geosetIndex) {
  const size_t requiredSize = size_t(geosetIndex) + 1u;
  if (record.geosetPtrs.size() < requiredSize)
    record.geosetPtrs.resize(requiredSize, nullptr);
  if (record.geosetDataPtrs.size() < requiredSize)
    record.geosetDataPtrs.resize(requiredSize, nullptr);
  if (record.geosetCount < requiredSize)
    record.geosetCount = uint32_t(requiredSize);
}

uint32_t CountReadyGeosets(const ShadowModelResourceRecord& record,
                           const std::unordered_map<void*, ShadowGeosetResourceRecord>& byGeoset,
                           const std::unordered_map<
                               void*, ShadowGeosetResourceSnapshot>&
                               byGeosetData) {
  uint32_t readyCount = 0u;
  for (size_t i = 0; i < record.geosetPtrs.size(); ++i) {
    const void* geosetPtr = record.geosetPtrs[i];
    if (geosetPtr != nullptr) {
      const auto itGeoset = byGeoset.find(const_cast<void*>(geosetPtr));
      if (itGeoset != byGeoset.end()) {
        const auto& alias = itGeoset->second;
        if (!alias.readyForShadowConsumer())
          continue;
        if (alias.geosetDataPtr != nullptr) {
          const auto canonical = byGeosetData.find(alias.geosetDataPtr);
          if (canonical != byGeosetData.end()) {
            if (canonical->second != nullptr &&
                canonical->second->readyForShadowConsumer()) {
              ++readyCount;
            }
            // Canonical presence is authoritative even when it is a failure
            // tombstone; never fall through to a stale model data pointer.
            continue;
          }
        }
        if (alias.geosetDataPtr == nullptr) {
          ++readyCount;
          continue;
        }
      }
    }

    if (i >= record.geosetDataPtrs.size())
      continue;

    const void* geosetDataPtr = record.geosetDataPtrs[i];
    if (geosetDataPtr == nullptr)
      continue;

    const auto itGeosetData =
        byGeosetData.find(const_cast<void*>(geosetDataPtr));
    if (itGeosetData != byGeosetData.end() &&
        itGeosetData->second != nullptr &&
        itGeosetData->second->readyForShadowConsumer()) {
      ++readyCount;
    }
  }
  return readyCount;
}

int ScoreRuntimeOwnerCandidate(const ShadowModelResourceRecord& record,
                               void* runtimeGeosetPtr,
                               void* runtimeGeosetDataPtr,
                               uint32_t geosetIndex,
                               void* modelResourcePtr) {
  if (record.runtimeModelPtr == nullptr ||
      !LooksLikeRuntimeModelPtr(record.runtimeModelPtr))
    return -1;

  const size_t geosetCount =
      (std::max)(record.geosetPtrs.size(), record.geosetDataPtrs.size());
  if (geosetCount == 0u)
    return -1;

  int bestScore = -1;
  for (size_t i = 0; i < geosetCount; ++i) {
    const void* candidateGeosetPtr =
        i < record.geosetPtrs.size() ? record.geosetPtrs[i] : nullptr;
    const void* candidateGeosetDataPtr =
        i < record.geosetDataPtrs.size() ? record.geosetDataPtrs[i] : nullptr;
    const bool ptrMatch =
        runtimeGeosetPtr != nullptr && candidateGeosetPtr == runtimeGeosetPtr;
    const bool dataMatch = runtimeGeosetDataPtr != nullptr &&
                           candidateGeosetDataPtr == runtimeGeosetDataPtr;
    if (!ptrMatch && !dataMatch)
      continue;

    int score = ptrMatch ? 100 : 60;
    if (geosetIndex != kInvalidShadowGeosetIndex &&
        geosetIndex == uint32_t(i)) {
      score += 10;
    }
    if (modelResourcePtr != nullptr &&
        record.modelResourcePtr == modelResourcePtr) {
      score += 6;
    }
    score += int((std::min)(record.readyGeosetCount, 8u));
    bestScore = (std::max)(bestScore, score);
  }

  return bestScore;
}

} // namespace

ShadowModelResourceCache &ShadowModelResourceCache::instance() {
  static ShadowModelResourceCache s_instance;
  return s_instance;
}

bool ShadowModelResourceCache::resetMapEpoch(uint64_t nextMapEpoch) {
  if (nextMapEpoch == 0u)
    return false;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_byGeoset.clear();
  m_byGeosetData.clear();
  m_geosetDataObservation.clear();
  m_byModelResource.clear();
  m_byRuntimeModel.clear();
  m_runtimeOwnerByGeoset.clear();
  m_runtimeOwnerByGeosetData.clear();
  m_mapEpoch.store(nextMapEpoch, std::memory_order_release);
  m_revision.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

void ShadowModelResourceCache::beginFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_frameNumber.fetch_add(1u, std::memory_order_relaxed);
}

void MergeGeosetMetadata(ShadowGeosetResourceRecord& dst,
                         const ShadowGeosetResourceRecord& src) {
  if (src.geosetPtr)
    dst.geosetPtr = src.geosetPtr;
  if (src.geosetDataPtr)
    dst.geosetDataPtr = src.geosetDataPtr;
  if (src.modelResourcePtr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (src.modelKey != 0u)
    dst.modelKey = src.modelKey;
  if (src.prefersRuntimeContract)
    dst.prefersRuntimeContract = true;
  if (src.geosetIndex != kInvalidShadowGeosetIndex)
    dst.geosetIndex = src.geosetIndex;
  if (src.materialOrLayoutSlot != 0u)
    dst.materialOrLayoutSlot = src.materialOrLayoutSlot;
  if (src.layoutOrMaterialSlot != 0u)
    dst.layoutOrMaterialSlot = src.layoutOrMaterialSlot;
  if (src.mergedGeosetSlotOrBindingIndex != 0u)
    dst.mergedGeosetSlotOrBindingIndex =
        src.mergedGeosetSlotOrBindingIndex;
  if (src.mapEpoch != 0u)
    dst.mapEpoch = src.mapEpoch;
  if (dst.firstSeenFrame == 0u ||
      (src.firstSeenFrame != 0u &&
       src.firstSeenFrame < dst.firstSeenFrame))
    dst.firstSeenFrame = src.firstSeenFrame;
  dst.lastSeenFrame = (std::max)(dst.lastSeenFrame, src.lastSeenFrame);
  dst.lastRuntimeRefreshFrame = (std::max)(
      dst.lastRuntimeRefreshFrame, src.lastRuntimeRefreshFrame);
}

void ReplaceGeosetImmutablePayload(ShadowGeosetResourceRecord& dst,
                                   const ShadowGeosetResourceRecord& src) {
  dst.vertexCount = src.vertexCount;
  dst.positions = src.positions;
  dst.normalCount = src.normalCount;
  dst.normals = src.normals;
  dst.vertexGroupCount = src.vertexGroupCount;
  dst.vertexGroupIndices = src.vertexGroupIndices;
  dst.maxVertexGroupSlot = src.maxVertexGroupSlot;
  dst.uvLayerCount = src.uvLayerCount;
  dst.uvLayers = src.uvLayers;
  dst.primitiveCount = src.primitiveCount;
  dst.primitiveRecords = src.primitiveRecords;
  dst.indexCount = src.indexCount;
  dst.indices = src.indices;
  dst.matrixGroupCount = src.matrixGroupCount;
  dst.matrixGroupSizes = src.matrixGroupSizes;
  dst.maxMatrixGroupSize = src.maxMatrixGroupSize;
  dst.matrixIndexCount = src.matrixIndexCount;
  dst.matrixIndices = src.matrixIndices;
  dst.contentHash = src.contentHash;
  dst.immutableModelGeneration = 0u;
  dst.localBounds = {};
  dst.immutableCaptureStatus = src.immutableCaptureStatus;
}

void RefreshGeosetImmutableDerivedValues(
    ShadowGeosetResourceRecord& record) noexcept {
  record.maxVertexGroupSlot = 0u;
  for (uint8_t slot : record.vertexGroupIndices)
    record.maxVertexGroupSlot = (std::max)(record.maxVertexGroupSlot,
                                           uint32_t(slot));

  record.maxMatrixGroupSize = 0u;
  for (uint32_t groupSize : record.matrixGroupSizes)
    record.maxMatrixGroupSize = (std::max)(record.maxMatrixGroupSize,
                                           groupSize);
}

void ShadowModelResourceCache::endFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
}

void ShadowModelResourceCache::noteGeosetDataMetadataLocked(
    void* geosetDataPtr, const ShadowGeosetResourceRecord& record) {
  if (geosetDataPtr == nullptr)
    return;

  GeosetObservationMetadata& metadata =
      m_geosetDataObservation[geosetDataPtr];
  if (metadata.firstSeenFrame == 0u ||
      (record.firstSeenFrame != 0u &&
       record.firstSeenFrame < metadata.firstSeenFrame)) {
    metadata.firstSeenFrame = record.firstSeenFrame;
  }
  metadata.lastSeenFrame =
      (std::max)(metadata.lastSeenFrame, record.lastSeenFrame);
  metadata.lastRuntimeRefreshFrame =
      (std::max)(metadata.lastRuntimeRefreshFrame,
                 record.lastRuntimeRefreshFrame);
}

ShadowGeosetResourceRecord
ShadowModelResourceCache::materializeGeosetDataRecordLocked(
    void* geosetDataPtr,
    const ShadowGeosetResourceSnapshot& snapshot) const {
  ShadowGeosetResourceRecord result =
      snapshot != nullptr ? *snapshot : ShadowGeosetResourceRecord{};
  const auto metadata = m_geosetDataObservation.find(geosetDataPtr);
  if (metadata == m_geosetDataObservation.end())
    return result;

  const GeosetObservationMetadata& observed = metadata->second;
  if (result.firstSeenFrame == 0u ||
      (observed.firstSeenFrame != 0u &&
       observed.firstSeenFrame < result.firstSeenFrame)) {
    result.firstSeenFrame = observed.firstSeenFrame;
  }
  result.lastSeenFrame =
      (std::max)(result.lastSeenFrame, observed.lastSeenFrame);
  result.lastRuntimeRefreshFrame =
      (std::max)(result.lastRuntimeRefreshFrame,
                 observed.lastRuntimeRefreshFrame);
  return result;
}

ShadowGeosetResourceRecord
ShadowModelResourceCache::materializeGeosetAliasRecordLocked(
    const ShadowGeosetResourceRecord& alias) const {
  // Header/failed transition records are explicit unresolved tombstones. An
  // independently Ready canonical target must not wash them back to Ready.
  if (!alias.readyForShadowConsumer() || alias.geosetDataPtr == nullptr)
    return alias;

  const auto canonical = m_byGeosetData.find(alias.geosetDataPtr);
  if (canonical != m_byGeosetData.end() && canonical->second != nullptr) {
    ShadowGeosetResourceRecord result = materializeGeosetDataRecordLocked(
        alias.geosetDataPtr, canonical->second);
    // Canonical owns every immutable byte and its generation. The by-geoset
    // value contributes only alias/owner observation metadata.
    MergeGeosetMetadata(result, alias);
    return result;
  }

  // A generated alias without its canonical publication violates the cache
  // ownership invariant. Never leak its old payload/generation as ready.
  ShadowGeosetResourceRecord unresolved = {};
  MergeGeosetMetadata(unresolved, alias);
  unresolved.immutableCaptureStatus =
      ShadowGeosetImmutableCaptureStatus::NotAttempted;
  unresolved.immutableModelGeneration = 0u;
  unresolved.contentHash = ComputeGeosetContentHash(unresolved);
  return unresolved;
}

ShadowGeosetResourceSnapshot ShadowModelResourceCache::storeGeosetRecord(
    const ShadowGeosetResourceRecord &incomingRecord) {
  ShadowGeosetResourceRecord record = incomingRecord;
  record.mapEpoch = m_mapEpoch.load(std::memory_order_acquire);
  const ShadowGeosetResourceRecord* existingByGeoset = nullptr;
  ShadowGeosetResourceSnapshot existingByData;
  if (record.geosetPtr != nullptr) {
    const auto found = m_byGeoset.find(record.geosetPtr);
    if (found != m_byGeoset.end())
      existingByGeoset = &found->second;
  }
  if (record.geosetDataPtr != nullptr) {
    const auto found = m_byGeosetData.find(record.geosetDataPtr);
    if (found != m_byGeosetData.end())
      existingByData = found->second;
  }

  const bool sourceIdentityTransition =
      existingByGeoset != nullptr &&
      existingByGeoset->geosetDataPtr != nullptr &&
      record.geosetDataPtr != nullptr &&
      existingByGeoset->geosetDataPtr != record.geosetDataPtr;
  const bool pendingAliasResolution = !sourceIdentityTransition &&
      existingByGeoset != nullptr && existingByData != nullptr &&
      existingByGeoset->geosetDataPtr == record.geosetDataPtr &&
      existingByGeoset->immutableModelGeneration == 0u &&
      existingByGeoset->immutableCaptureStatus !=
          ShadowGeosetImmutableCaptureStatus::Complete &&
      existingByData->immutableModelGeneration != 0u &&
      existingByData->readyForShadowConsumer();
  const bool forceFreshGeneration = sourceIdentityTransition ||
      pendingAliasResolution ||
      (existingByGeoset != nullptr &&
       existingByGeoset->geosetDataPtr == record.geosetDataPtr &&
       existingByGeoset->immutableModelGeneration == 0u &&
       existingByGeoset->immutableCaptureStatus !=
           ShadowGeosetImmutableCaptureStatus::Complete);

  // A header-only or failed observation of alias P moving to canonical data B
  // must neither inherit A's vectors nor tombstone B's independently complete
  // snapshot. Publish only an unresolved per-geoset record until P itself has
  // a complete capture.
  if ((sourceIdentityTransition || pendingAliasResolution) &&
      record.immutableCaptureStatus !=
          ShadowGeosetImmutableCaptureStatus::Complete) {
    ShadowGeosetResourceRecord unresolved = {};
    MergeGeosetMetadata(unresolved, *existingByGeoset);
    MergeGeosetMetadata(unresolved, record);
    unresolved.immutableCaptureStatus = record.immutableCaptureStatus;
    unresolved.immutableModelGeneration = 0u;
    unresolved.localBounds = {};
    unresolved.contentHash = ComputeGeosetContentHash(unresolved);
    const bool changed = existingByGeoset->immutableModelGeneration != 0u ||
        existingByGeoset->immutableCaptureStatus !=
            unresolved.immutableCaptureStatus ||
        !HasSameGeosetConsumerContent(*existingByGeoset, unresolved);
    m_byGeoset[unresolved.geosetPtr] = unresolved;
    if (changed)
      m_revision.fetch_add(1u, std::memory_order_relaxed);
    return std::make_shared<ShadowGeosetResourceRecord>(unresolved);
  }

  ShadowGeosetResourceRecord merged = {};
  if (existingByGeoset != nullptr)
    MergeGeosetMetadata(merged, *existingByGeoset);
  if (existingByData != nullptr)
    MergeGeosetMetadata(merged, *existingByData);
  MergeGeosetMetadata(merged, record);

  if (record.immutableCaptureStatus ==
      ShadowGeosetImmutableCaptureStatus::Complete) {
    // A successful capture is a full replacement, never a partial vector
    // merge. This allows legitimate stream removal and prevents old payload
    // inheritance across aliases.
    ReplaceGeosetImmutablePayload(merged, record);
  } else if (record.immutableCaptureStatus ==
      ShadowGeosetImmutableCaptureStatus::AttemptedFailed) {
    // Publish a non-ready tombstone for the current data identity. Clear all
    // old payload so ready-pointer early-outs cannot make the failure sticky.
    merged.immutableCaptureStatus =
        ShadowGeosetImmutableCaptureStatus::AttemptedFailed;
    merged.immutableModelGeneration = 0u;
    merged.localBounds = {};
    merged.contentHash = ComputeGeosetContentHash(merged);
  } else {
    // Header-only metadata may reuse immutable bytes only from the same
    // canonical data identity.
    if (existingByData != nullptr)
      ReplaceGeosetImmutablePayload(merged, *existingByData);
    else if (existingByGeoset != nullptr && !sourceIdentityTransition)
      ReplaceGeosetImmutablePayload(merged, *existingByGeoset);
  }

  if (merged.contentHash == 0u)
    merged.contentHash = ComputeGeosetContentHash(merged);
  uint64_t generation = 0u;
  const bool completeConsumerPayload =
      merged.hasCompleteImmutableConsumerPayload();
  if (completeConsumerPayload) {
    // A geoset alias moving A -> B -> A is a new source-lifetime
    // publication even when canonical A still has identical bytes. Never
    // resurrect A's earlier generation across that identity transition.
    if (!forceFreshGeneration && existingByData != nullptr &&
        existingByData->immutableModelGeneration != 0u &&
        existingByData->immutableCaptureStatus ==
            ShadowGeosetImmutableCaptureStatus::Complete &&
        SameShadowGeosetImmutableConsumerPayload(
            *existingByData, merged)) {
      generation = existingByData->immutableModelGeneration;
    } else if (!forceFreshGeneration && existingByData == nullptr &&
        existingByGeoset != nullptr &&
        existingByGeoset->geosetDataPtr == merged.geosetDataPtr &&
        existingByGeoset->immutableModelGeneration != 0u &&
        existingByGeoset->immutableCaptureStatus ==
            ShadowGeosetImmutableCaptureStatus::Complete &&
        SameShadowGeosetImmutableConsumerPayload(
            *existingByGeoset, merged)) {
      generation = existingByGeoset->immutableModelGeneration;
    } else {
      generation = m_immutableModelGenerations.issue();
    }
  }
  merged.immutableModelGeneration = generation;
  RefreshGeosetImmutableDerivedValues(merged);
  merged.localBounds = generation != 0u
      ? ComputeShadowGeosetLocalBounds(merged.positions, merged.vertexCount)
      : ShadowGeosetLocalBounds{};

  const ShadowGeosetResourceRecord* comparison = existingByData != nullptr
      ? existingByData.get() : existingByGeoset;
  const bool changed = comparison == nullptr ||
      comparison->immutableModelGeneration != merged.immutableModelGeneration ||
      comparison->immutableCaptureStatus != merged.immutableCaptureStatus ||
      !HasSameGeosetConsumerContent(*comparison, merged);
  if (merged.geosetPtr != nullptr)
    m_byGeoset[merged.geosetPtr] = merged;
  if (merged.geosetDataPtr != nullptr) {
    const auto current = m_byGeosetData.find(merged.geosetDataPtr);
    if (current == m_byGeosetData.end() || current->second == nullptr ||
        current->second->immutableModelGeneration !=
            merged.immutableModelGeneration ||
        current->second->immutableCaptureStatus !=
            merged.immutableCaptureStatus ||
        !HasSameGeosetConsumerContent(*current->second, merged)) {
      m_byGeosetData[merged.geosetDataPtr] =
          std::make_shared<ShadowGeosetResourceRecord>(merged);
    }
    noteGeosetDataMetadataLocked(merged.geosetDataPtr, merged);
  }
  if (changed)
    m_revision.fetch_add(1u, std::memory_order_relaxed);
  if (merged.geosetDataPtr != nullptr) {
    const auto published = m_byGeosetData.find(merged.geosetDataPtr);
    if (published != m_byGeosetData.end())
      return published->second;
  }
  return std::make_shared<ShadowGeosetResourceRecord>(merged);
}

void ShadowModelResourceCache::storeModelRecord(
    const ShadowModelResourceRecord &record) {
  if (record.modelResourcePtr == nullptr)
    return;

  ShadowModelResourceRecord merged = {};
  bool hadExisting = false;
  ShadowModelResourceRecord existing = {};
  auto it = m_byModelResource.find(record.modelResourcePtr);
  if (it != m_byModelResource.end()) {
    MergeModelRecord(merged, it->second);
    existing = it->second;
    hadExisting = true;
  }
  MergeModelRecord(merged, record);
  m_byModelResource[record.modelResourcePtr] = std::move(merged);
  if (!hadExisting ||
      !HasSameModelConsumerContent(m_byModelResource[record.modelResourcePtr],
                                   existing)) {
    m_revision.fetch_add(1u, std::memory_order_relaxed);
  }
}

void ShadowModelResourceCache::storeRuntimeModelRecord(
    const ShadowModelResourceRecord& record) {
  if (record.runtimeModelPtr == nullptr)
    return;

  ShadowModelResourceRecord merged = {};
  bool hadExisting = false;
  ShadowModelResourceRecord existing = {};
  auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
  if (it != m_byRuntimeModel.end()) {
    MergeModelRecord(merged, it->second);
    existing = it->second;
    hadExisting = true;
  }
  MergeModelRecord(merged, record);
  m_byRuntimeModel[record.runtimeModelPtr] = std::move(merged);
  const ShadowModelResourceRecord& stored = m_byRuntimeModel[record.runtimeModelPtr];
  for (void* geosetPtr : stored.geosetPtrs)
    NoteRuntimeOwnerIndex(m_runtimeOwnerByGeoset, geosetPtr,
                          stored.runtimeModelPtr);
  for (void* geosetDataPtr : stored.geosetDataPtrs)
    NoteRuntimeOwnerIndex(m_runtimeOwnerByGeosetData, geosetDataPtr,
                          stored.runtimeModelPtr);
  if (!hadExisting || !HasSameModelConsumerContent(stored, existing))
    m_revision.fetch_add(1u, std::memory_order_relaxed);
}

void ShadowModelResourceCache::recordGeosetCreate(void *geosetPtr) {
  ShadowGeosetResourceRecord record = {};
  if (!CaptureGeosetHeaderRecord(geosetPtr, record))
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  bool aliasIdentityTransition = false;
  if (record.geosetPtr != nullptr) {
    const auto it = m_byGeoset.find(record.geosetPtr);
    const ShadowGeosetResourceRecord current = it != m_byGeoset.end()
        ? materializeGeosetAliasRecordLocked(it->second)
        : ShadowGeosetResourceRecord{};
    aliasIdentityTransition = it != m_byGeoset.end() &&
        current.geosetDataPtr != nullptr && record.geosetDataPtr != nullptr &&
        current.geosetDataPtr != record.geosetDataPtr;
    if (!aliasIdentityTransition && it != m_byGeoset.end() &&
        current.readyForShadowConsumer()) {
      it->second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
      noteGeosetDataMetadataLocked(current.geosetDataPtr, current);
      return;
    }
  }
  if (!aliasIdentityTransition && record.geosetDataPtr != nullptr) {
    const auto it = m_byGeosetData.find(record.geosetDataPtr);
    if (it != m_byGeosetData.end() && it->second != nullptr &&
        it->second->readyForShadowConsumer()) {
      record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
      noteGeosetDataMetadataLocked(record.geosetDataPtr, record);
      return;
    }
  }
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeGeosetRecord(record);
}

void ShadowModelResourceCache::noteModelResourceBinding(void *modelResourcePtr,
                                                        uint64_t modelKey) {
  void* rawModelResourcePtr = modelResourcePtr;
  modelResourcePtr = TryResolveDirectModelResourcePtr(modelResourcePtr);
  if (modelResourcePtr == nullptr)
  {
    if (rawModelResourcePtr != nullptr) {
      uint32_t headWords[8] = {};
      for (size_t i = 0; i < 8u; ++i)
        TryReadU32Fast(rawModelResourcePtr, i * sizeof(uint32_t), headWords[i]);
      const uint32_t failCount =
          g_modelBindFailLogCount.fetch_add(1, std::memory_order_relaxed);
      if (failCount < 24u) {
        dxvk::war3dbg::Print(
            "DXVK ModelCache: model bind rejected raw=%p key=0x%llX "
            "head=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X\n",
            rawModelResourcePtr, static_cast<unsigned long long>(modelKey),
            headWords[0], headWords[1], headWords[2], headWords[3],
            headWords[4], headWords[5], headWords[6], headWords[7]);
      }
    }
    return;
  }

  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_byModelResource.find(modelResourcePtr);
    if (it != m_byModelResource.end())
      it->second.readyGeosetCount = CountReadyGeosets(
          it->second, m_byGeoset, m_byGeosetData);
    if (it != m_byModelResource.end() &&
        it->second.readyForShadowConsumer() &&
        (modelKey == 0 || it->second.modelKey == 0 ||
         it->second.modelKey == modelKey)) {
      it->second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
      if (it->second.modelKey == 0 && modelKey != 0)
        it->second.modelKey = modelKey;
      return;
    }
  }

  std::vector<void*> geosetPtrs;
  if (!TryReadGeosetHandleArray(modelResourcePtr,
                                dxvk::war3::CModelDataOffsets::GeosetCount,
                                dxvk::war3::CModelDataOffsets::Geosets,
                                geosetPtrs)) {
    uint32_t geosetCount = 0;
    void* geosetsPtr = nullptr;
    TryReadU32Fast(modelResourcePtr, dxvk::war3::CModelDataOffsets::GeosetCount,
                   geosetCount);
    TryReadPtrFast(modelResourcePtr, dxvk::war3::CModelDataOffsets::Geosets,
                   geosetsPtr);
    const uint32_t failCount =
        g_modelBindFailLogCount.fetch_add(1, std::memory_order_relaxed);
    if (failCount < 24u) {
      dxvk::war3dbg::Print(
          "DXVK ModelCache: model bind rejected ptr=%p key=0x%llX "
          "geosetCount=%u geosets=%p\n",
          modelResourcePtr, static_cast<unsigned long long>(modelKey),
          geosetCount, geosetsPtr);
    }
    return;
  }

  ShadowModelResourceRecord modelRecord = {};
  modelRecord.modelResourcePtr = modelResourcePtr;
  modelRecord.modelKey = modelKey;
  modelRecord.geosetCount = uint32_t(geosetPtrs.size());
  modelRecord.geosetPtrs = geosetPtrs;
  modelRecord.geosetDataPtrs.resize(geosetPtrs.size(), nullptr);

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if (modelRecord.firstSeenFrame == 0)
    modelRecord.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  modelRecord.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);

  for (uint32_t i = 0; i < modelRecord.geosetCount; ++i) {
    ShadowGeosetResourceRecord geosetRecord = {};
    auto itGeoset = m_byGeoset.find(geosetPtrs[i]);
    if (itGeoset != m_byGeoset.end())
      geosetRecord = materializeGeosetAliasRecordLocked(itGeoset->second);
    if (!geosetRecord.readyForShadowConsumer()) {
      ShadowGeosetResourceRecord captured = {};
      if (CaptureGeosetRecord(geosetPtrs[i], captured)) {
        MergeGeosetMetadata(captured, geosetRecord);
        geosetRecord = std::move(captured);
        if (geosetRecord.firstSeenFrame == 0) {
          geosetRecord.firstSeenFrame =
              m_frameNumber.load(std::memory_order_relaxed);
        }
      } else if (itGeoset == m_byGeoset.end()) {
        continue;
      }
    }

    geosetRecord.modelResourcePtr = modelResourcePtr;
    if (modelKey != 0)
      geosetRecord.modelKey = modelKey;
    geosetRecord.geosetIndex = i;
    geosetRecord.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    const ShadowGeosetResourceSnapshot published =
        storeGeosetRecord(geosetRecord);

    modelRecord.geosetDataPtrs[i] = published != nullptr
        ? published->geosetDataPtr : nullptr;
    if (published != nullptr && published->readyForShadowConsumer())
      ++modelRecord.readyGeosetCount;
  }

  storeModelRecord(modelRecord);
  const uint32_t okCount =
      g_modelBindOkLogCount.fetch_add(1, std::memory_order_relaxed);
  if (okCount < 16u || (okCount % 256u) == 0u) {
    dxvk::war3dbg::Print(
        "DXVK ModelCache: model bound ptr=%p key=0x%llX geosets=%u ready=%u\n",
        modelResourcePtr, static_cast<unsigned long long>(modelKey),
        modelRecord.geosetCount, modelRecord.readyGeosetCount);
  }
}

void ShadowModelResourceCache::noteRuntimeModelBinding(void* runtimeModelPtr,
                                                       void* modelResourcePtr,
                                                       uint64_t modelKey) {
  if (runtimeModelPtr == nullptr)
    return;

  modelResourcePtr = TryResolveDirectModelResourcePtr(modelResourcePtr);

  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_byRuntimeModel.find(runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      it->second.readyGeosetCount = CountReadyGeosets(
          it->second, m_byGeoset, m_byGeosetData);
    if (it != m_byRuntimeModel.end() &&
        it->second.readyForShadowConsumer() &&
        (modelResourcePtr == nullptr ||
         it->second.modelResourcePtr == nullptr ||
         it->second.modelResourcePtr == modelResourcePtr) &&
        (modelKey == 0 || it->second.modelKey == 0 ||
         it->second.modelKey == modelKey)) {
      it->second.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
      if (it->second.modelResourcePtr == nullptr && modelResourcePtr != nullptr)
        it->second.modelResourcePtr = modelResourcePtr;
      if (it->second.modelKey == 0 && modelKey != 0)
        it->second.modelKey = modelKey;
      return;
    }

    if (modelResourcePtr != nullptr) {
      auto modelIt = m_byModelResource.find(modelResourcePtr);
      if (modelIt != m_byModelResource.end())
        modelIt->second.readyGeosetCount = CountReadyGeosets(
            modelIt->second, m_byGeoset, m_byGeosetData);
      if (modelIt != m_byModelResource.end() &&
          modelIt->second.readyForShadowConsumer()) {
        ShadowModelResourceRecord runtimeAlias = modelIt->second;
        runtimeAlias.runtimeModelPtr = runtimeModelPtr;
        runtimeAlias.modelResourcePtr = modelResourcePtr;
        if (modelKey != 0)
          runtimeAlias.modelKey = modelKey;
        if (runtimeAlias.firstSeenFrame == 0)
          runtimeAlias.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
        runtimeAlias.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
        storeRuntimeModelRecord(runtimeAlias);
        return;
      }
    }
  }

  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;

  std::vector<void*> geosetPtrs;
  const bool hasRuntimeGeosets =
      TryReadGeosetHandleArray(runtimeModelPtr,
                               dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                               dxvk::war3::CModelOffsets::RuntimeGeosets,
                               geosetPtrs);

  ShadowModelResourceRecord runtimeRecord = {};
  runtimeRecord.runtimeModelPtr = runtimeModelPtr;
  runtimeRecord.modelResourcePtr = modelResourcePtr;
  runtimeRecord.modelKey = modelKey;
  runtimeRecord.geosetCount = uint32_t(geosetPtrs.size());
  runtimeRecord.geosetPtrs = geosetPtrs;
  runtimeRecord.geosetDataPtrs.resize(geosetPtrs.size(), nullptr);

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if (runtimeRecord.firstSeenFrame == 0)
    runtimeRecord.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  runtimeRecord.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);

  if (!hasRuntimeGeosets) {
    // 閺堝绨?runtimeModel 鏉╂劘顢戦幀浣稿嚒缂佸繐褰茬粙鍐茬暰鐠囧棗鍩嗛敍灞肩稻楠炴湹绗夐幀鏄忓厴鐎瑰鍙忕拠璇插煂 geoset 閺佹壆绮嶉妴?
    // 鏉╂瑩鍣锋禒宥勭箽閻ｆ瑦娓剁亸?runtime record閿涘矂浼╅崗?runtimeModel lineage 鐎瑰苯鍙忔稉銏犮亼閿?
    // 閸氬海鐢婚懟銉┾偓姘崇箖 runtimeGeoset/runtimeGeosetData 鏉╂稐绔村銉ㄋ夋鎰剁礉娴兼艾婀崥灞肩 record 娑撳﹤顤冮柌蹇撴値楠炶翰鈧?
    storeRuntimeModelRecord(runtimeRecord);
    if (modelResourcePtr != nullptr) {
      ShadowModelResourceRecord modelAlias = runtimeRecord;
      modelAlias.modelResourcePtr = modelResourcePtr;
      storeModelRecord(modelAlias);
    }
    return;
  }

  for (uint32_t i = 0; i < runtimeRecord.geosetCount; ++i) {
    ShadowGeosetResourceRecord geosetRecord = {};
    auto itGeoset = m_byGeoset.find(geosetPtrs[i]);
    if (itGeoset != m_byGeoset.end()) {
      geosetRecord = materializeGeosetAliasRecordLocked(itGeoset->second);
    }
    if (!geosetRecord.readyForShadowConsumer()) {
      ShadowGeosetResourceRecord captured = {};
      if (CaptureGeosetRecord(geosetPtrs[i], captured)) {
        MergeGeosetMetadata(captured, geosetRecord);
        geosetRecord = std::move(captured);
      } else if (itGeoset == m_byGeoset.end()) {
        geosetRecord.geosetPtr = geosetPtrs[i];
        TryReadGeosetDataPtr(geosetPtrs[i], geosetRecord.geosetDataPtr);
        geosetRecord.firstSeenFrame =
            m_frameNumber.load(std::memory_order_relaxed);
      }
    }

    geosetRecord.geosetPtr = geosetPtrs[i];
    geosetRecord.geosetIndex = i;
    if (geosetRecord.firstSeenFrame == 0)
      geosetRecord.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    if (modelResourcePtr != nullptr)
      geosetRecord.modelResourcePtr = modelResourcePtr;
    if (modelKey != 0)
      geosetRecord.modelKey = modelKey;
    geosetRecord.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    const ShadowGeosetResourceSnapshot published =
        storeGeosetRecord(geosetRecord);

    runtimeRecord.geosetDataPtrs[i] = published != nullptr
        ? published->geosetDataPtr : nullptr;
    if (published != nullptr && published->readyForShadowConsumer())
      ++runtimeRecord.readyGeosetCount;
  }

  storeRuntimeModelRecord(runtimeRecord);

  if (modelResourcePtr != nullptr) {
    ShadowModelResourceRecord modelAlias = runtimeRecord;
    modelAlias.modelResourcePtr = modelResourcePtr;
    storeModelRecord(modelAlias);
  }
}

void ShadowModelResourceCache::noteRuntimeGeosetBinding(
    void* runtimeModelPtr, uint32_t geosetIndex, void* runtimeGeosetPtr,
    void* runtimeGeosetDataPtr, void* modelResourcePtr, uint64_t modelKey) {
  if (runtimeGeosetPtr == nullptr && runtimeGeosetDataPtr == nullptr) {
    return;
  }

  if (runtimeModelPtr != nullptr && !LooksLikeRuntimeModelPtr(runtimeModelPtr))
    runtimeModelPtr = nullptr;

  modelResourcePtr = TryResolveDirectModelResourcePtr(modelResourcePtr);
  if (modelResourcePtr == nullptr && runtimeModelPtr != nullptr) {
    void* ownedModelDataHandle = nullptr;
    if (TryReadPtrFast(runtimeModelPtr,
                       dxvk::war3::CModelOffsets::OwnedModelDataHandle,
                       ownedModelDataHandle) &&
        ownedModelDataHandle != nullptr) {
      modelResourcePtr = TryResolveDirectModelResourcePtr(ownedModelDataHandle);
    }
  }

  ShadowGeosetResourceRecord geosetRecord = {};
  bool alreadyRefreshedThisFrame = false;
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (runtimeGeosetPtr != nullptr) {
      const auto itGeoset = m_byGeoset.find(runtimeGeosetPtr);
      if (itGeoset != m_byGeoset.end()) {
        geosetRecord = materializeGeosetAliasRecordLocked(itGeoset->second);
        alreadyRefreshedThisFrame =
            geosetRecord.prefersRuntimeContract &&
            geosetRecord.lastRuntimeRefreshFrame ==
                m_frameNumber.load(std::memory_order_relaxed) &&
            (runtimeGeosetDataPtr == nullptr ||
             geosetRecord.geosetDataPtr == runtimeGeosetDataPtr);
      }
    }
    if (geosetRecord.geosetPtr == nullptr && runtimeGeosetDataPtr != nullptr) {
      const auto itGeosetData = m_byGeosetData.find(runtimeGeosetDataPtr);
      if (itGeosetData != m_byGeosetData.end() &&
          itGeosetData->second != nullptr) {
        geosetRecord = materializeGeosetDataRecordLocked(
            runtimeGeosetDataPtr, itGeosetData->second);
        alreadyRefreshedThisFrame =
            geosetRecord.prefersRuntimeContract &&
            geosetRecord.lastRuntimeRefreshFrame == m_frameNumber.load(std::memory_order_relaxed);
      }
    }
  }

  if (!alreadyRefreshedThisFrame) {
    ShadowGeosetResourceRecord liveRuntimeRecord = {};
    if (CaptureGeosetRecordFromKnownPtrs(runtimeGeosetPtr, runtimeGeosetDataPtr,
                                         liveRuntimeRecord)) {
      if (geosetRecord.geosetPtr != nullptr || geosetRecord.geosetDataPtr != nullptr) {
        ShadowGeosetResourceRecord mergedRecord = {};
        MergeGeosetMetadata(mergedRecord, geosetRecord);
        MergeGeosetMetadata(mergedRecord, liveRuntimeRecord);
        ReplaceGeosetImmutablePayload(mergedRecord, liveRuntimeRecord);
        geosetRecord = std::move(mergedRecord);
      } else {
        geosetRecord = std::move(liveRuntimeRecord);
      }
    } else {
      return;
    }
  }

  if (geosetRecord.geosetPtr == nullptr && geosetRecord.geosetDataPtr == nullptr) {
    return;
  }

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ShadowModelResourceRecord existingRuntimeRecord = {};
  bool hasExistingRuntimeRecord = false;
  if (runtimeModelPtr != nullptr) {
    const auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
    if (itRuntime != m_byRuntimeModel.end()) {
      existingRuntimeRecord = itRuntime->second;
      hasExistingRuntimeRecord = true;
    }
  }
  if (modelResourcePtr == nullptr && hasExistingRuntimeRecord &&
      existingRuntimeRecord.modelResourcePtr != nullptr) {
    modelResourcePtr = existingRuntimeRecord.modelResourcePtr;
  }
  if (modelKey == 0u && hasExistingRuntimeRecord &&
      existingRuntimeRecord.modelKey != 0u) {
    modelKey = existingRuntimeRecord.modelKey;
  }
  if (geosetRecord.firstSeenFrame == 0)
    geosetRecord.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  geosetRecord.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  geosetRecord.prefersRuntimeContract = true;
  geosetRecord.lastRuntimeRefreshFrame = m_frameNumber.load(std::memory_order_relaxed);
  if (runtimeGeosetPtr != nullptr)
    geosetRecord.geosetPtr = runtimeGeosetPtr;
  if (runtimeGeosetDataPtr != nullptr)
    geosetRecord.geosetDataPtr = runtimeGeosetDataPtr;
  if (geosetIndex != kInvalidShadowGeosetIndex)
    geosetRecord.geosetIndex = geosetIndex;
  if (modelResourcePtr != nullptr)
    geosetRecord.modelResourcePtr = modelResourcePtr;
  if (modelKey != 0)
    geosetRecord.modelKey = modelKey;
  const ShadowGeosetResourceSnapshot published =
      storeGeosetRecord(geosetRecord);
  if (published != nullptr)
    geosetRecord = *published;

  if (runtimeModelPtr != nullptr && geosetIndex != kInvalidShadowGeosetIndex) {
    ShadowModelResourceRecord runtimeRecord = {};
    if (hasExistingRuntimeRecord) {
      runtimeRecord = existingRuntimeRecord;
    } else {
      const auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
      if (itRuntime != m_byRuntimeModel.end())
        runtimeRecord = itRuntime->second;
    }
    runtimeRecord.runtimeModelPtr = runtimeModelPtr;
    if (modelResourcePtr != nullptr)
      runtimeRecord.modelResourcePtr = modelResourcePtr;
    if (modelKey != 0)
      runtimeRecord.modelKey = modelKey;
    if (runtimeRecord.firstSeenFrame == 0)
      runtimeRecord.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    runtimeRecord.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    EnsureModelRecordCapacity(runtimeRecord, geosetIndex);
    runtimeRecord.geosetPtrs[geosetIndex] = geosetRecord.geosetPtr;
    runtimeRecord.geosetDataPtrs[geosetIndex] = geosetRecord.geosetDataPtr;
    runtimeRecord.readyGeosetCount =
        CountReadyGeosets(runtimeRecord, m_byGeoset, m_byGeosetData);
    storeRuntimeModelRecord(runtimeRecord);

    if (runtimeRecord.modelResourcePtr != nullptr) {
      ShadowModelResourceRecord modelAlias = runtimeRecord;
      modelAlias.modelResourcePtr = runtimeRecord.modelResourcePtr;
      modelAlias.readyGeosetCount =
          CountReadyGeosets(modelAlias, m_byGeoset, m_byGeosetData);
      storeModelRecord(modelAlias);
    }
  } else if (modelResourcePtr != nullptr &&
             geosetIndex != kInvalidShadowGeosetIndex) {
    ShadowModelResourceRecord modelAlias = {};
    const auto itModel = m_byModelResource.find(modelResourcePtr);
    if (itModel != m_byModelResource.end())
      modelAlias = itModel->second;
    modelAlias.modelResourcePtr = modelResourcePtr;
    if (modelKey != 0)
      modelAlias.modelKey = modelKey;
    if (modelAlias.firstSeenFrame == 0)
      modelAlias.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    modelAlias.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
    EnsureModelRecordCapacity(modelAlias, geosetIndex);
    modelAlias.geosetPtrs[geosetIndex] = geosetRecord.geosetPtr;
    modelAlias.geosetDataPtrs[geosetIndex] = geosetRecord.geosetDataPtr;
    modelAlias.readyGeosetCount =
        CountReadyGeosets(modelAlias, m_byGeoset, m_byGeosetData);
    storeModelRecord(modelAlias);
  }
}

bool ShadowModelResourceCache::findGeosetByPtr(
    void *geosetPtr, ShadowGeosetResourceRecord &out) const {
  out = {};
  if (geosetPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byGeoset.find(geosetPtr);
  if (it == m_byGeoset.end())
    return false;
  out = materializeGeosetAliasRecordLocked(it->second);
  return true;
}

bool ShadowModelResourceCache::findGeosetByData(
    void *geosetDataPtr, ShadowGeosetResourceRecord &out) const {
  out = {};
  if (geosetDataPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byGeosetData.find(geosetDataPtr);
  if (it == m_byGeosetData.end() || it->second == nullptr)
    return false;
  out = materializeGeosetDataRecordLocked(geosetDataPtr, it->second);
  return true;
}

namespace {

void CopyReadyGeosetBinding(const ShadowGeosetResourceRecord& record,
                            ShadowReadyGeosetBinding& out) {
  out.geosetPtr = record.geosetPtr;
  out.geosetDataPtr = record.geosetDataPtr;
  out.modelResourcePtr = record.modelResourcePtr;
  out.modelKey = record.modelKey;
  out.geosetIndex = record.geosetIndex;
}

void MergeReadyGeosetAlias(const ShadowGeosetResourceRecord& alias,
                           ShadowReadyGeosetBinding& out) {
  if (alias.geosetPtr != nullptr)
    out.geosetPtr = alias.geosetPtr;
  if (alias.geosetDataPtr != nullptr)
    out.geosetDataPtr = alias.geosetDataPtr;
  if (alias.modelResourcePtr != nullptr)
    out.modelResourcePtr = alias.modelResourcePtr;
  if (alias.modelKey != 0u)
    out.modelKey = alias.modelKey;
  if (alias.geosetIndex != kInvalidShadowGeosetIndex)
    out.geosetIndex = alias.geosetIndex;
}

} // namespace

bool ShadowModelResourceCache::findReadyGeosetBindingByPtr(
    void* geosetPtr, ShadowReadyGeosetBinding& out) const {
  out = {};
  if (geosetPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto aliasIt = m_byGeoset.find(geosetPtr);
  if (aliasIt == m_byGeoset.end() ||
      !aliasIt->second.readyForShadowConsumer() ||
      aliasIt->second.geosetDataPtr == nullptr) {
    return false;
  }

  const auto canonicalIt = m_byGeosetData.find(
      aliasIt->second.geosetDataPtr);
  if (canonicalIt == m_byGeosetData.end() ||
      canonicalIt->second == nullptr ||
      !canonicalIt->second->readyForShadowConsumer()) {
    return false;
  }

  CopyReadyGeosetBinding(*canonicalIt->second, out);
  MergeReadyGeosetAlias(aliasIt->second, out);
  return true;
}

bool ShadowModelResourceCache::findReadyGeosetBindingByData(
    void* geosetDataPtr, ShadowReadyGeosetBinding& out) const {
  out = {};
  if (geosetDataPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto canonicalIt = m_byGeosetData.find(geosetDataPtr);
  if (canonicalIt == m_byGeosetData.end() ||
      canonicalIt->second == nullptr ||
      !canonicalIt->second->readyForShadowConsumer()) {
    return false;
  }

  CopyReadyGeosetBinding(*canonicalIt->second, out);
  return true;
}

ShadowGeosetResourceSnapshot
ShadowModelResourceCache::findGeosetSnapshotByData(
    void* geosetDataPtr) const {
  if (geosetDataPtr == nullptr)
    return {};

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byGeosetData.find(geosetDataPtr);
  const uint64_t currentMapEpoch =
      m_mapEpoch.load(std::memory_order_acquire);
  return it != m_byGeosetData.end() && it->second != nullptr &&
          it->second->mapEpoch == currentMapEpoch
      ? it->second : ShadowGeosetResourceSnapshot{};
}

struct GeosetCapacityBreakdown {
  uint64_t total = 0;
  uint64_t positions = 0;
  uint64_t normals = 0;
  uint64_t groupSlots = 0;
  uint64_t uv = 0;
  uint64_t primitives = 0;
  uint64_t indices = 0;
  uint64_t matrixGroups = 0;
  uint64_t matrixIndices = 0;
};

GeosetCapacityBreakdown MeasureGeosetCapacity(
    const ShadowGeosetResourceRecord& record) {
  GeosetCapacityBreakdown result;
  result.positions = record.positions.capacity() * sizeof(float);
  result.normals = record.normals.capacity() * sizeof(float);
  result.groupSlots = record.vertexGroupIndices.capacity() * sizeof(uint8_t);
  for (const auto& uv : record.uvLayers)
    result.uv += uv.uvPairs.capacity() * sizeof(float);
  result.uv += record.uvLayers.capacity() *
      sizeof(ShadowGeosetUvLayerRecord);
  result.primitives = record.primitiveRecords.capacity() *
      sizeof(ShadowGeosetPrimitiveRecord);
  result.indices = record.indices.capacity() * sizeof(uint16_t);
  result.matrixGroups = record.matrixGroupSizes.capacity() * sizeof(uint32_t);
  result.matrixIndices = record.matrixIndices.capacity() * sizeof(uint32_t);
  result.total = result.positions + result.normals + result.groupSlots +
      result.uv + result.primitives + result.indices + result.matrixGroups +
      result.matrixIndices;
  return result;
}

bool ShadowModelResourceCache::findGeosetStampByData(
    void* geosetDataPtr, ShadowGeosetResourceStamp& out) const {
  return findGeosetStampByDataForEpoch(
      geosetDataPtr, m_mapEpoch.load(std::memory_order_acquire), out);
}

bool ShadowModelResourceCache::findGeosetStampByDataForEpoch(
    void* geosetDataPtr, uint64_t expectedMapEpoch,
    ShadowGeosetResourceStamp& out) const {
  out = {};
  if (geosetDataPtr == nullptr || expectedMapEpoch == 0u)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  if (m_mapEpoch.load(std::memory_order_acquire) != expectedMapEpoch)
    return false;
  const auto it = m_byGeosetData.find(geosetDataPtr);
  if (it == m_byGeosetData.end() || it->second == nullptr ||
      it->second->mapEpoch != expectedMapEpoch)
    return false;
  out.geosetDataPtr = it->second->geosetDataPtr;
  out.contentHash = it->second->contentHash;
  out.mapEpoch = it->second->mapEpoch;
  out.immutableModelGeneration =
      it->second->immutableModelGeneration;
  out.vertexCount = it->second->vertexCount;
  out.immutableCaptureStatus =
      it->second->immutableCaptureStatus;
  if (out.contentHash == 0u || out.mapEpoch != expectedMapEpoch ||
      out.immutableModelGeneration == 0u ||
      out.vertexCount == 0u || out.immutableCaptureStatus !=
          ShadowGeosetImmutableCaptureStatus::Complete) {
    out = {};
    return false;
  }
  return true;
}

bool ShadowModelResourceCache::hydrateGeosetByKnownPtrs(
    void* geosetPtr, void* geosetDataPtr, ShadowGeosetResourceRecord& out) {
  out = {};
  if (geosetPtr == nullptr && geosetDataPtr == nullptr)
    return false;

  {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    bool aliasIdentityTransition = false;
    bool aliasRequiresCapture = false;
    if (geosetPtr != nullptr) {
      const auto it = m_byGeoset.find(geosetPtr);
      if (it != m_byGeoset.end()) {
        ShadowGeosetResourceRecord current =
            materializeGeosetAliasRecordLocked(it->second);
        aliasIdentityTransition = current.geosetDataPtr != nullptr &&
            geosetDataPtr != nullptr &&
            current.geosetDataPtr != geosetDataPtr;
        aliasRequiresCapture = !current.readyForShadowConsumer();
        if (!aliasIdentityTransition && current.readyForShadowConsumer()) {
          out = std::move(current);
          return true;
        }
      }
    }
    if (!aliasIdentityTransition && !aliasRequiresCapture &&
        geosetDataPtr != nullptr) {
      const auto it = m_byGeosetData.find(geosetDataPtr);
      if (it != m_byGeosetData.end() && it->second != nullptr &&
          it->second->readyForShadowConsumer()) {
        out = materializeGeosetDataRecordLocked(geosetDataPtr, it->second);
        return true;
      }
    }
  }

  ShadowGeosetResourceRecord hydrated = {};
  if (!CaptureGeosetRecordFromKnownPtrs(geosetPtr, geosetDataPtr, hydrated))
    return false;
  if (!hydrated.hasCompleteImmutableConsumerPayload())
    return false;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if (hydrated.firstSeenFrame == 0)
    hydrated.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  hydrated.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  const ShadowGeosetResourceSnapshot published = storeGeosetRecord(hydrated);
  if (published == nullptr)
    return false;
  out = published->geosetDataPtr != nullptr
      ? materializeGeosetDataRecordLocked(
            published->geosetDataPtr, published)
      : *published;
  return out.immutableModelGeneration != 0u &&
      out.readyForShadowConsumer();
}

bool ShadowModelResourceCache::findModelGeoset(
    void *modelResourcePtr, uint32_t geosetIndex,
    ShadowGeosetResourceRecord &out) const {
  out = {};
  if (modelResourcePtr == nullptr || geosetIndex == kInvalidShadowGeosetIndex)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto itModel = m_byModelResource.find(modelResourcePtr);
  if (itModel == m_byModelResource.end())
    return false;

  const auto &modelRecord = itModel->second;
  if (geosetIndex >= modelRecord.geosetPtrs.size())
    return false;

  const void *geosetPtr = modelRecord.geosetPtrs[geosetIndex];
  if (geosetPtr != nullptr) {
    const auto itGeoset = m_byGeoset.find(const_cast<void *>(geosetPtr));
    if (itGeoset != m_byGeoset.end()) {
      out = materializeGeosetAliasRecordLocked(itGeoset->second);
      return true;
    }
  }

  if (geosetIndex >= modelRecord.geosetDataPtrs.size())
    return false;

  const void *geosetDataPtr = modelRecord.geosetDataPtrs[geosetIndex];
  if (geosetDataPtr == nullptr)
    return false;

  const auto itGeosetData = m_byGeosetData.find(const_cast<void *>(geosetDataPtr));
  if (itGeosetData == m_byGeosetData.end() ||
      itGeosetData->second == nullptr)
    return false;

  out = materializeGeosetDataRecordLocked(
      const_cast<void*>(geosetDataPtr), itGeosetData->second);
  return true;
}

bool ShadowModelResourceCache::findRuntimeModelGeoset(
    void* runtimeModelPtr, uint32_t geosetIndex,
    ShadowGeosetResourceRecord& out) const {
  out = {};
  if (runtimeModelPtr == nullptr || geosetIndex == kInvalidShadowGeosetIndex)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime == m_byRuntimeModel.end())
    return false;

  const auto& runtimeRecord = itRuntime->second;
  if (geosetIndex >= runtimeRecord.geosetPtrs.size())
    return false;

  const void* geosetPtr = runtimeRecord.geosetPtrs[geosetIndex];
  if (geosetPtr != nullptr) {
    const auto itGeoset = m_byGeoset.find(const_cast<void*>(geosetPtr));
    if (itGeoset != m_byGeoset.end()) {
      out = materializeGeosetAliasRecordLocked(itGeoset->second);
      return true;
    }
  }

  if (geosetIndex >= runtimeRecord.geosetDataPtrs.size())
    return false;

  const void* geosetDataPtr = runtimeRecord.geosetDataPtrs[geosetIndex];
  if (geosetDataPtr == nullptr)
    return false;

  const auto itGeosetData =
      m_byGeosetData.find(const_cast<void*>(geosetDataPtr));
  if (itGeosetData == m_byGeosetData.end() ||
      itGeosetData->second == nullptr)
    return false;

  out = materializeGeosetDataRecordLocked(
      const_cast<void*>(geosetDataPtr), itGeosetData->second);
  return true;
}

bool ShadowModelResourceCache::findRuntimeModelOwner(
    void* runtimeGeosetPtr, void* runtimeGeosetDataPtr, uint32_t geosetIndex,
    void* modelResourcePtr, ShadowModelResourceRecord& out) const {
  out = {};
  if (runtimeGeosetPtr == nullptr && runtimeGeosetDataPtr == nullptr)
    return false;

  modelResourcePtr = TryResolveDirectModelResourcePtr(modelResourcePtr);

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto tryIndexedOwner = [&](const std::unordered_map<void*, void*>& index,
                             void* key) -> bool {
    if (key == nullptr)
      return false;
    const auto itOwner = index.find(key);
    if (itOwner == index.end())
      return false;
    if (itOwner->second == nullptr) {
      out = {};
      return true;
    }
    const auto itRuntime = m_byRuntimeModel.find(itOwner->second);
    if (itRuntime == m_byRuntimeModel.end()) {
      out = {};
      return true;
    }
    out = itRuntime->second;
    return true;
  };

  if (tryIndexedOwner(m_runtimeOwnerByGeoset, runtimeGeosetPtr))
    return out.runtimeModelPtr != nullptr;
  if (tryIndexedOwner(m_runtimeOwnerByGeosetData, runtimeGeosetDataPtr))
    return out.runtimeModelPtr != nullptr;

  int bestScore = -1;
  bool ambiguous = false;
  for (const auto& itRuntime : m_byRuntimeModel) {
    const ShadowModelResourceRecord& runtimeRecord = itRuntime.second;
    const int score = ScoreRuntimeOwnerCandidate(
        runtimeRecord, runtimeGeosetPtr, runtimeGeosetDataPtr, geosetIndex,
        modelResourcePtr);
    if (score < 0)
      continue;

    if (score > bestScore) {
      out = runtimeRecord;
      bestScore = score;
      ambiguous = false;
      continue;
    }

    if (score == bestScore && out.runtimeModelPtr != runtimeRecord.runtimeModelPtr)
      ambiguous = true;
  }

  if (bestScore < 0 || ambiguous) {
    out = {};
    return false;
  }

  return true;
}

bool ShadowModelResourceCache::findRuntimeModelOwnerIndexed(
    void* runtimeGeosetPtr, void* runtimeGeosetDataPtr,
    ShadowModelResourceRecord& out) const {
  out = {};
  if (runtimeGeosetPtr == nullptr && runtimeGeosetDataPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto tryIndexedOwner = [&](const std::unordered_map<void*, void*>& index,
                             void* key) -> bool {
    if (key == nullptr)
      return false;
    const auto itOwner = index.find(key);
    if (itOwner == index.end() || itOwner->second == nullptr)
      return false;
    const auto itRuntime = m_byRuntimeModel.find(itOwner->second);
    if (itRuntime == m_byRuntimeModel.end())
      return false;
    out = itRuntime->second;
    return out.runtimeModelPtr != nullptr;
  };

  if (tryIndexedOwner(m_runtimeOwnerByGeoset, runtimeGeosetPtr))
    return true;
  if (tryIndexedOwner(m_runtimeOwnerByGeosetData, runtimeGeosetDataPtr))
    return true;
  return false;
}

bool ShadowModelResourceCache::findRuntimeModelOwnerBinding(
    void* runtimeGeosetPtr, void* runtimeGeosetDataPtr, uint32_t geosetIndex,
    void* modelResourcePtr, ShadowRuntimeModelOwnerBinding& out) const {
  out = {};
  if (runtimeGeosetPtr == nullptr && runtimeGeosetDataPtr == nullptr)
    return false;

  modelResourcePtr = TryResolveDirectModelResourcePtr(modelResourcePtr);

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto project = [&](const ShadowModelResourceRecord& record) {
    out.runtimeModelPtr = record.runtimeModelPtr;
    out.modelResourcePtr = record.modelResourcePtr;
    out.modelKey = record.modelKey;
    out.geosetCount = record.geosetCount;
  };
  const auto tryIndexedOwner = [&](
      const std::unordered_map<void*, void*>& index, void* key) -> bool {
    if (key == nullptr)
      return false;
    const auto itOwner = index.find(key);
    if (itOwner == index.end())
      return false;
    if (itOwner->second == nullptr)
      return true;
    const auto itRuntime = m_byRuntimeModel.find(itOwner->second);
    if (itRuntime == m_byRuntimeModel.end())
      return true;
    project(itRuntime->second);
    return true;
  };

  if (tryIndexedOwner(m_runtimeOwnerByGeoset, runtimeGeosetPtr))
    return out.runtimeModelPtr != nullptr;
  if (tryIndexedOwner(m_runtimeOwnerByGeosetData, runtimeGeosetDataPtr))
    return out.runtimeModelPtr != nullptr;

  const ShadowModelResourceRecord* best = nullptr;
  int bestScore = -1;
  bool ambiguous = false;
  for (const auto& itRuntime : m_byRuntimeModel) {
    const ShadowModelResourceRecord& runtimeRecord = itRuntime.second;
    const int score = ScoreRuntimeOwnerCandidate(
        runtimeRecord, runtimeGeosetPtr, runtimeGeosetDataPtr, geosetIndex,
        modelResourcePtr);
    if (score < 0)
      continue;
    if (score > bestScore) {
      best = &runtimeRecord;
      bestScore = score;
      ambiguous = false;
    } else if (score == bestScore && best != nullptr &&
               best->runtimeModelPtr != runtimeRecord.runtimeModelPtr) {
      ambiguous = true;
    }
  }

  if (best == nullptr || ambiguous) {
    out = {};
    return false;
  }
  project(*best);
  return out.runtimeModelPtr != nullptr;
}

bool ShadowModelResourceCache::findRuntimeModelOwnerBindingIndexed(
    void* runtimeGeosetPtr, void* runtimeGeosetDataPtr,
    ShadowRuntimeModelOwnerBinding& out) const {
  out = {};
  if (runtimeGeosetPtr == nullptr && runtimeGeosetDataPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto tryIndexedOwner = [&](
      const std::unordered_map<void*, void*>& index, void* key) -> bool {
    if (key == nullptr)
      return false;
    const auto itOwner = index.find(key);
    if (itOwner == index.end() || itOwner->second == nullptr)
      return false;
    const auto itRuntime = m_byRuntimeModel.find(itOwner->second);
    if (itRuntime == m_byRuntimeModel.end())
      return false;
    const ShadowModelResourceRecord& record = itRuntime->second;
    out.runtimeModelPtr = record.runtimeModelPtr;
    out.modelResourcePtr = record.modelResourcePtr;
    out.modelKey = record.modelKey;
    out.geosetCount = record.geosetCount;
    return out.runtimeModelPtr != nullptr;
  };

  return tryIndexedOwner(m_runtimeOwnerByGeoset, runtimeGeosetPtr) ||
      tryIndexedOwner(m_runtimeOwnerByGeosetData, runtimeGeosetDataPtr);
}

bool ShadowModelResourceCache::findModelBinding(
    void* modelResourcePtr, ShadowRuntimeModelOwnerBinding& out) const {
  out = {};
  if (modelResourcePtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byModelResource.find(modelResourcePtr);
  if (it == m_byModelResource.end())
    return false;
  const ShadowModelResourceRecord& record = it->second;
  out.runtimeModelPtr = record.runtimeModelPtr;
  out.modelResourcePtr = record.modelResourcePtr;
  out.modelKey = record.modelKey;
  out.geosetCount = record.geosetCount;
  return true;
}

bool ShadowModelResourceCache::findRuntimeModelBinding(
    void* runtimeModelPtr, ShadowRuntimeModelOwnerBinding& out) const {
  out = {};
  if (runtimeModelPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  const ShadowModelResourceRecord& record = it->second;
  out.runtimeModelPtr = record.runtimeModelPtr;
  out.modelResourcePtr = record.modelResourcePtr;
  out.modelKey = record.modelKey;
  out.geosetCount = record.geosetCount;
  return true;
}

bool ShadowModelResourceCache::findModelResource(
    void *modelResourcePtr, ShadowModelResourceRecord &out) const {
  out = {};
  if (modelResourcePtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byModelResource.find(modelResourcePtr);
  if (it == m_byModelResource.end())
    return false;
  out = it->second;
  out.readyGeosetCount = CountReadyGeosets(
      out, m_byGeoset, m_byGeosetData);
  return true;
}

bool ShadowModelResourceCache::findRuntimeModelResource(
    void* runtimeModelPtr, ShadowModelResourceRecord& out) const {
  out = {};
  if (runtimeModelPtr == nullptr)
    return false;

  std::shared_lock<std::shared_mutex> lock(m_mutex);
  const auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  out.readyGeosetCount = CountReadyGeosets(
      out, m_byGeoset, m_byGeosetData);
  return true;
}

bool ShadowModelResourceCache::isDirectModelResourcePtr(
    void* modelResourcePtr) const {
  return LooksLikeDirectModelResourcePtr(modelResourcePtr);
}

void* ShadowModelResourceCache::resolveDirectModelResourcePtr(
    void* modelResourceOrHandlePtr) const {
  return TryResolveDirectModelResourcePtr(modelResourceOrHandlePtr);
}

std::vector<ShadowGeosetResourceRecord>
ShadowModelResourceCache::snapshotGeosets() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ShadowGeosetResourceRecord> out;
  out.reserve(m_byGeoset.size() + m_byGeosetData.size());
  for (const auto &it : m_byGeoset)
    out.push_back(materializeGeosetAliasRecordLocked(it.second));
  for (const auto& it : m_byGeosetData) {
    if (it.second == nullptr)
      continue;
    const auto& record = *it.second;
    if (record.geosetPtr != nullptr &&
        m_byGeoset.find(record.geosetPtr) != m_byGeoset.end()) {
      continue;
    }
    out.push_back(materializeGeosetDataRecordLocked(it.first, it.second));
  }
  return out;
}

std::vector<ShadowModelResourceRecord> ShadowModelResourceCache::snapshotModels() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ShadowModelResourceRecord> out;
  out.reserve(m_byModelResource.size());
  for (const auto &it : m_byModelResource)
    out.push_back(it.second);
  return out;
}

std::vector<ShadowModelResourceRecord>
ShadowModelResourceCache::snapshotRuntimeModels() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ShadowModelResourceRecord> out;
  out.reserve(m_byRuntimeModel.size());
  for (const auto& it : m_byRuntimeModel)
    out.push_back(it.second);
  return out;
}

std::vector<ShadowModelAliasSnapshotRecord>
ShadowModelResourceCache::snapshotModelAliases() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ShadowModelAliasSnapshotRecord> out;
  out.reserve(m_byModelResource.size());
  for (const auto& entry : m_byModelResource) {
    const auto& record = entry.second;
    out.push_back({record.runtimeModelPtr, record.modelResourcePtr,
                   record.geosetCount});
  }
  return out;
}

std::vector<ShadowModelAliasSnapshotRecord>
ShadowModelResourceCache::snapshotRuntimeModelAliases() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ShadowModelAliasSnapshotRecord> out;
  out.reserve(m_byRuntimeModel.size());
  for (const auto& entry : m_byRuntimeModel) {
    const auto& record = entry.second;
    out.push_back({record.runtimeModelPtr, record.modelResourcePtr,
                   record.geosetCount});
  }
  return out;
}

size_t ShadowModelResourceCache::geosetRecordCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = m_byGeoset.size();
  for (const auto& it : m_byGeosetData) {
    if (it.second == nullptr)
      continue;
    if (it.second->geosetPtr != nullptr &&
        m_byGeoset.find(it.second->geosetPtr) != m_byGeoset.end()) {
      continue;
    }
    ++count;
  }
  return count;
}

size_t ShadowModelResourceCache::readyGeosetCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_byGeoset) {
    if (materializeGeosetAliasRecordLocked(
            it.second).readyForShadowConsumer())
      ++count;
  }
  for (const auto& it : m_byGeosetData) {
    if (it.second == nullptr)
      continue;
    if (it.second->geosetPtr != nullptr &&
        m_byGeoset.find(it.second->geosetPtr) != m_byGeoset.end()) {
      continue;
    }
    if (it.second->readyForShadowConsumer())
      ++count;
  }
  return count;
}

size_t ShadowModelResourceCache::modelResourceCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_byModelResource.size();
}

size_t ShadowModelResourceCache::runtimeModelRecordCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

ShadowResourceStoreCapacityHint
ShadowModelResourceCache::resourceStoreCapacityHint() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  ShadowResourceStoreCapacityHint result = {};
  result.geosetRecordUpperBound = m_byGeoset.size() + m_byGeosetData.size();

  const auto addAliasCapacity = [&result](
      const std::unordered_map<void*, ShadowModelResourceRecord>& records) {
    for (const auto& entry : records) {
      if (entry.second.runtimeModelPtr == nullptr)
        continue;
      const size_t remaining = (std::numeric_limits<size_t>::max)() -
          result.runtimeAliasUpperBound;
      result.runtimeAliasUpperBound +=
          (std::min)(remaining, size_t(entry.second.geosetCount));
    }
  };
  addAliasCapacity(m_byModelResource);
  addAliasCapacity(m_byRuntimeModel);
  return result;
}

ShadowModelResourceMemorySnapshot
ShadowModelResourceCache::memorySnapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  ShadowModelResourceMemorySnapshot result;

  const auto accumulateResident = [&](
      const ShadowGeosetResourceRecord& record) {
    const auto bytes = MeasureGeosetCapacity(record);
    ++result.residentGeosetRecords;
    result.residentGeosetCapacityBytes += bytes.total;
    result.positionsCapacityBytes += bytes.positions;
    result.normalsCapacityBytes += bytes.normals;
    result.groupSlotsCapacityBytes += bytes.groupSlots;
    result.uvCapacityBytes += bytes.uv;
    result.primitiveCapacityBytes += bytes.primitives;
    result.indicesCapacityBytes += bytes.indices;
    result.matrixGroupsCapacityBytes += bytes.matrixGroups;
    result.matrixIndicesCapacityBytes += bytes.matrixIndices;
  };
  const auto accumulateUnique = [&](
      const ShadowGeosetResourceRecord& record) {
    ++result.uniqueGeosetRecords;
    result.uniqueGeosetCapacityBytes += MeasureGeosetCapacity(record).total;
  };

  for (const auto& entry : m_byGeoset) {
    accumulateResident(entry.second);
    accumulateUnique(entry.second);
  }
  for (const auto& entry : m_byGeosetData) {
    if (entry.second == nullptr)
      continue;
    accumulateResident(*entry.second);
    if (entry.second->geosetPtr == nullptr ||
        m_byGeoset.find(entry.second->geosetPtr) == m_byGeoset.end()) {
      accumulateUnique(*entry.second);
    }
  }

  result.aliasDuplicateCapacityBytes =
      result.residentGeosetCapacityBytes >= result.uniqueGeosetCapacityBytes
      ? result.residentGeosetCapacityBytes -
            result.uniqueGeosetCapacityBytes
      : 0u;
  const auto accumulateModelPointers = [&](
      const auto& records) {
    for (const auto& entry : records) {
      result.modelPointerCapacityBytes +=
          entry.second.geosetPtrs.capacity() * sizeof(void*) +
          entry.second.geosetDataPtrs.capacity() * sizeof(void*);
    }
  };
  accumulateModelPointers(m_byModelResource);
  accumulateModelPointers(m_byRuntimeModel);
  return result;
}

uint64_t ShadowModelResourceCache::frameNumber() const {
  // Phase 7.83锛歛tomic 鐩存帴 load锛屾棤闇€閿併€?
  return m_frameNumber.load(std::memory_order_relaxed);
}

uint64_t ShadowModelResourceCache::revision() const {
  // Phase 7.83锛歛tomic 鐩存帴 load锛屾棤闇€閿併€?
  return m_revision.load(std::memory_order_relaxed);
}

uint64_t ShadowModelResourceCache::mapEpoch() const {
  return m_mapEpoch.load(std::memory_order_acquire);
}

} // namespace dxvk::war3::model
