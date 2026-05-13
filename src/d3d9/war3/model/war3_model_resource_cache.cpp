#include "war3_model_resource_cache.h"

#include "../../d3d9_war3_debug.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_memory.h"

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
    // Null marks an ambiguous key. The slow scoring path can still decide
    // later if needed, but the hot path will not trust this direct index.
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
  if (src.readyGeosetCount != 0)
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
          a.uvLayers[i].uvPairs != b.uvLayers[i].uvPairs) {
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
         a.positions == b.positions &&
         a.normalCount == b.normalCount &&
         a.normals == b.normals &&
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
  hash = CombineHash(hash, Fnv1a64(&record.indexCount, sizeof(record.indexCount)));
  if (!record.positions.empty()) {
    hash = CombineHash(hash,
                       Fnv1a64(record.positions.data(),
                               record.positions.size() * sizeof(float)));
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

  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexCount,
                 vertexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexPositions, positionsPtr);
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::NormalCount,
                 normalCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::NormalVectors, normalsPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexGroupCount,
                 vertexGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexGroupIndices,
                 vertexGroupsPtr);
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayerCount,
                 uvLayerCount);
  TryReadPtrFast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayers,
                 uvLayersPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
                 primitiveCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
                 primitiveRecordsPtr);
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::IndexCount,
                 indexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::IndexBufferU16, indicesPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
                 matrixGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
                 matrixGroupSizesPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
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

  CopyFlatFloatArray(static_cast<const float *>(positionsPtr), record.vertexCount,
                     3u, record.positions);
  CopyFlatFloatArray(static_cast<const float *>(normalsPtr), record.normalCount,
                     3u, record.normals);
  CopyPodArray(static_cast<const uint8_t *>(vertexGroupsPtr),
               record.vertexGroupCount, record.vertexGroupIndices);
  CopyPodArray(static_cast<const uint16_t *>(indicesPtr), record.indexCount,
               record.indices);
  CopyPodArray(static_cast<const uint32_t *>(matrixGroupSizesPtr),
               record.matrixGroupCount, record.matrixGroupSizes);
  CopyPodArray(static_cast<const uint32_t *>(matrixIndicesPtr),
               record.matrixIndexCount, record.matrixIndices);

  std::vector<dxvk::war3::GeosetPrimitiveRecord> primitivesRaw;
  if (CopyPodArray(static_cast<const dxvk::war3::GeosetPrimitiveRecord *>(
                       primitiveRecordsPtr),
                   record.primitiveCount, primitivesRaw)) {
    record.primitiveRecords.reserve(primitivesRaw.size());
    for (const auto &primitive : primitivesRaw) {
      record.primitiveRecords.push_back(ShadowGeosetPrimitiveRecord{
          primitive.primitive_type_or_material_slot, primitive.index_count});
    }
  }

  if (uvLayersPtr != nullptr && record.uvLayerCount != 0 &&
      dxvk::war3::IsReadableRange(
          uvLayersPtr, size_t(record.uvLayerCount) *
                          sizeof(dxvk::war3::GeosetUvLayerVec2ArrayRecord))) {
    auto *layers = static_cast<const dxvk::war3::GeosetUvLayerVec2ArrayRecord *>(
        uvLayersPtr);
    record.uvLayers.reserve(record.uvLayerCount);
    for (uint32_t i = 0; i < record.uvLayerCount; ++i) {
      ShadowGeosetUvLayerRecord uvRecord = {};
      uvRecord.uvCount = std::min(layers[i].count, record.vertexCount);
      const float *uvData =
          layers[i].data != nullptr ? layers[i].data : layers[i].inline_storage;
      CopyFlatFloatArray(uvData, uvRecord.uvCount, 2u, uvRecord.uvPairs);
      record.uvLayers.emplace_back(std::move(uvRecord));
    }
  }

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
    if (geosetDataPtr != nullptr)
      record.geosetDataPtr = geosetDataPtr;
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

  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::VertexCount,
                 vertexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexPositions, positionsPtr);
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::NormalCount,
                 normalCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::NormalVectors, normalsPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexGroupCount,
                 vertexGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::VertexGroupIndices,
                 vertexGroupsPtr);
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayerCount,
                 uvLayerCount);
  TryReadPtrFast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::UvLayers,
                 uvLayersPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
                 primitiveCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
                 primitiveRecordsPtr);
  TryReadU32Fast(geosetDataPtr, dxvk::war3::CGeosetDataOffsets::IndexCount,
                 indexCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::IndexBufferU16, indicesPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
                 matrixGroupCount);
  TryReadPtrFast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
                 matrixGroupSizesPtr);
  TryReadU32Fast(geosetDataPtr,
                 dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
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

  CopyFlatFloatArray(static_cast<const float*>(positionsPtr), record.vertexCount,
                     3u, record.positions);
  CopyFlatFloatArray(static_cast<const float*>(normalsPtr), record.normalCount,
                     3u, record.normals);
  CopyPodArray(static_cast<const uint8_t*>(vertexGroupsPtr),
               record.vertexGroupCount, record.vertexGroupIndices);
  CopyPodArray(static_cast<const uint16_t*>(indicesPtr), record.indexCount,
               record.indices);
  CopyPodArray(static_cast<const uint32_t*>(matrixGroupSizesPtr),
               record.matrixGroupCount, record.matrixGroupSizes);
  CopyPodArray(static_cast<const uint32_t*>(matrixIndicesPtr),
               record.matrixIndexCount, record.matrixIndices);

  std::vector<dxvk::war3::GeosetPrimitiveRecord> primitivesRaw;
  if (CopyPodArray(static_cast<const dxvk::war3::GeosetPrimitiveRecord*>(
                       primitiveRecordsPtr),
                   record.primitiveCount, primitivesRaw)) {
    record.primitiveRecords.reserve(primitivesRaw.size());
    for (const auto& primitive : primitivesRaw) {
      record.primitiveRecords.push_back(ShadowGeosetPrimitiveRecord{
          primitive.primitive_type_or_material_slot, primitive.index_count});
    }
  }

  if (uvLayersPtr != nullptr && record.uvLayerCount != 0 &&
      dxvk::war3::IsReadableRange(
          uvLayersPtr, size_t(record.uvLayerCount) *
                          sizeof(dxvk::war3::GeosetUvLayerVec2ArrayRecord))) {
    auto* layers = static_cast<const dxvk::war3::GeosetUvLayerVec2ArrayRecord*>(
        uvLayersPtr);
    record.uvLayers.reserve(record.uvLayerCount);
    for (uint32_t i = 0; i < record.uvLayerCount; ++i) {
      ShadowGeosetUvLayerRecord uvRecord = {};
      uvRecord.uvCount = std::min(layers[i].count, record.vertexCount);
      const float* uvData =
          layers[i].data != nullptr ? layers[i].data : layers[i].inline_storage;
      CopyFlatFloatArray(uvData, uvRecord.uvCount, 2u, uvRecord.uvPairs);
      record.uvLayers.emplace_back(std::move(uvRecord));
    }
  }

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
                           const std::unordered_map<void*, ShadowGeosetResourceRecord>& byGeosetData) {
  uint32_t readyCount = 0u;
  for (size_t i = 0; i < record.geosetPtrs.size(); ++i) {
    const void* geosetPtr = record.geosetPtrs[i];
    if (geosetPtr != nullptr) {
      const auto itGeoset = byGeoset.find(const_cast<void*>(geosetPtr));
      if (itGeoset != byGeoset.end() && itGeoset->second.readyForShadowConsumer()) {
        ++readyCount;
        continue;
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
        itGeosetData->second.readyForShadowConsumer()) {
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

void ShadowModelResourceCache::beginFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_frameNumber;
}

void ShadowModelResourceCache::endFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
}

void ShadowModelResourceCache::storeGeosetRecord(
    const ShadowGeosetResourceRecord &record) {
  ShadowGeosetResourceRecord merged = {};
  bool hadExisting = false;
  ShadowGeosetResourceRecord existing = {};
  if (record.geosetPtr != nullptr) {
    auto it = m_byGeoset.find(record.geosetPtr);
    if (it != m_byGeoset.end()) {
      MergeGeosetRecord(merged, it->second);
      if (!hadExisting) {
        existing = it->second;
        hadExisting = true;
      }
    }
  }
  if (record.geosetDataPtr != nullptr) {
    auto it = m_byGeosetData.find(record.geosetDataPtr);
    if (it != m_byGeosetData.end()) {
      MergeGeosetRecord(merged, it->second);
      if (!hadExisting) {
        existing = it->second;
        hadExisting = true;
      }
    }
  }
  MergeGeosetRecord(merged, record);

  bool changed = !hadExisting || !HasSameGeosetConsumerContent(merged, existing);
  if (merged.geosetPtr != nullptr)
    m_byGeoset[merged.geosetPtr] = merged;
  if (merged.geosetDataPtr != nullptr)
    m_byGeosetData[merged.geosetDataPtr] = merged;
  if (changed)
    ++m_revision;
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
    ++m_revision;
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
    ++m_revision;
}

void ShadowModelResourceCache::recordGeosetCreate(void *geosetPtr) {
  ShadowGeosetResourceRecord record = {};
  if (!CaptureGeosetHeaderRecord(geosetPtr, record))
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (record.geosetPtr != nullptr) {
    const auto it = m_byGeoset.find(record.geosetPtr);
    if (it != m_byGeoset.end() && it->second.readyForShadowConsumer()) {
      it->second.lastSeenFrame = m_frameNumber;
      return;
    }
  }
  if (record.geosetDataPtr != nullptr) {
    const auto it = m_byGeosetData.find(record.geosetDataPtr);
    if (it != m_byGeosetData.end() && it->second.readyForShadowConsumer()) {
      it->second.lastSeenFrame = m_frameNumber;
      return;
    }
  }
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
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
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_byModelResource.find(modelResourcePtr);
    if (it != m_byModelResource.end() &&
        it->second.readyForShadowConsumer() &&
        (modelKey == 0 || it->second.modelKey == 0 ||
         it->second.modelKey == modelKey)) {
      it->second.lastSeenFrame = m_frameNumber;
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

  std::lock_guard<std::mutex> lock(m_mutex);
  if (modelRecord.firstSeenFrame == 0)
    modelRecord.firstSeenFrame = m_frameNumber;
  modelRecord.lastSeenFrame = m_frameNumber;

  for (uint32_t i = 0; i < modelRecord.geosetCount; ++i) {
    ShadowGeosetResourceRecord geosetRecord = {};
    auto itGeoset = m_byGeoset.find(geosetPtrs[i]);
    if (itGeoset != m_byGeoset.end() &&
        itGeoset->second.readyForShadowConsumer()) {
      geosetRecord = itGeoset->second;
    } else if (CaptureGeosetRecord(geosetPtrs[i], geosetRecord)) {
      if (geosetRecord.firstSeenFrame == 0)
        geosetRecord.firstSeenFrame = m_frameNumber;
    } else if (itGeoset != m_byGeoset.end()) {
      geosetRecord = itGeoset->second;
    } else {
      continue;
    }

    geosetRecord.modelResourcePtr = modelResourcePtr;
    if (modelKey != 0)
      geosetRecord.modelKey = modelKey;
    geosetRecord.geosetIndex = i;
    geosetRecord.lastSeenFrame = m_frameNumber;
    storeGeosetRecord(geosetRecord);

    modelRecord.geosetDataPtrs[i] = geosetRecord.geosetDataPtr;
    if (geosetRecord.readyForShadowConsumer())
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
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_byRuntimeModel.find(runtimeModelPtr);
    if (it != m_byRuntimeModel.end() &&
        it->second.readyForShadowConsumer() &&
        (modelResourcePtr == nullptr ||
         it->second.modelResourcePtr == nullptr ||
         it->second.modelResourcePtr == modelResourcePtr) &&
        (modelKey == 0 || it->second.modelKey == 0 ||
         it->second.modelKey == modelKey)) {
      it->second.lastSeenFrame = m_frameNumber;
      if (it->second.modelResourcePtr == nullptr && modelResourcePtr != nullptr)
        it->second.modelResourcePtr = modelResourcePtr;
      if (it->second.modelKey == 0 && modelKey != 0)
        it->second.modelKey = modelKey;
      return;
    }

    if (modelResourcePtr != nullptr) {
      auto modelIt = m_byModelResource.find(modelResourcePtr);
      if (modelIt != m_byModelResource.end() &&
          modelIt->second.readyForShadowConsumer()) {
        ShadowModelResourceRecord runtimeAlias = modelIt->second;
        runtimeAlias.runtimeModelPtr = runtimeModelPtr;
        runtimeAlias.modelResourcePtr = modelResourcePtr;
        if (modelKey != 0)
          runtimeAlias.modelKey = modelKey;
        if (runtimeAlias.firstSeenFrame == 0)
          runtimeAlias.firstSeenFrame = m_frameNumber;
        runtimeAlias.lastSeenFrame = m_frameNumber;
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

  std::lock_guard<std::mutex> lock(m_mutex);
  if (runtimeRecord.firstSeenFrame == 0)
    runtimeRecord.firstSeenFrame = m_frameNumber;
  runtimeRecord.lastSeenFrame = m_frameNumber;

  if (!hasRuntimeGeosets) {
    // 有些 runtimeModel 运行态已经可稳定识别，但并不总能安全读到 geoset 数组。
    // 这里仍保留最小 runtime record，避免 runtimeModel lineage 完全丢失；
    // 后续若通过 runtimeGeoset/runtimeGeosetData 进一步补齐，会在同一 record 上增量合并。
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
      geosetRecord = itGeoset->second;
    } else {
      geosetRecord.geosetPtr = geosetPtrs[i];
      TryReadGeosetDataPtr(geosetPtrs[i], geosetRecord.geosetDataPtr);
      geosetRecord.firstSeenFrame = m_frameNumber;
    }

    geosetRecord.geosetPtr = geosetPtrs[i];
    geosetRecord.geosetIndex = i;
    if (geosetRecord.firstSeenFrame == 0)
      geosetRecord.firstSeenFrame = m_frameNumber;
    if (modelResourcePtr != nullptr)
      geosetRecord.modelResourcePtr = modelResourcePtr;
    if (modelKey != 0)
      geosetRecord.modelKey = modelKey;
    geosetRecord.lastSeenFrame = m_frameNumber;
    storeGeosetRecord(geosetRecord);

    runtimeRecord.geosetDataPtrs[i] = geosetRecord.geosetDataPtr;
    if (geosetRecord.readyForShadowConsumer())
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (runtimeGeosetPtr != nullptr) {
      const auto itGeoset = m_byGeoset.find(runtimeGeosetPtr);
      if (itGeoset != m_byGeoset.end()) {
        geosetRecord = itGeoset->second;
        alreadyRefreshedThisFrame =
            geosetRecord.prefersRuntimeContract &&
            geosetRecord.lastRuntimeRefreshFrame == m_frameNumber;
      }
    }
    if (geosetRecord.geosetPtr == nullptr && runtimeGeosetDataPtr != nullptr) {
      const auto itGeosetData = m_byGeosetData.find(runtimeGeosetDataPtr);
      if (itGeosetData != m_byGeosetData.end()) {
        geosetRecord = itGeosetData->second;
        alreadyRefreshedThisFrame =
            geosetRecord.prefersRuntimeContract &&
            geosetRecord.lastRuntimeRefreshFrame == m_frameNumber;
      }
    }
  }

  if (!alreadyRefreshedThisFrame) {
    ShadowGeosetResourceRecord liveRuntimeRecord = {};
    if (CaptureGeosetRecordFromKnownPtrs(runtimeGeosetPtr, runtimeGeosetDataPtr,
                                         liveRuntimeRecord)) {
      if (geosetRecord.geosetPtr != nullptr || geosetRecord.geosetDataPtr != nullptr) {
        ShadowGeosetResourceRecord mergedRecord = geosetRecord;
        MergeGeosetRecord(mergedRecord, liveRuntimeRecord);
        geosetRecord = std::move(mergedRecord);
      } else {
        geosetRecord = std::move(liveRuntimeRecord);
      }
    }
  }

  if (geosetRecord.geosetPtr == nullptr && geosetRecord.geosetDataPtr == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
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
    geosetRecord.firstSeenFrame = m_frameNumber;
  geosetRecord.lastSeenFrame = m_frameNumber;
  geosetRecord.prefersRuntimeContract = true;
  geosetRecord.lastRuntimeRefreshFrame = m_frameNumber;
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
  storeGeosetRecord(geosetRecord);

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
      runtimeRecord.firstSeenFrame = m_frameNumber;
    runtimeRecord.lastSeenFrame = m_frameNumber;
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
      modelAlias.firstSeenFrame = m_frameNumber;
    modelAlias.lastSeenFrame = m_frameNumber;
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

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_byGeoset.find(geosetPtr);
  if (it == m_byGeoset.end())
    return false;
  out = it->second;
  return true;
}

const ShadowGeosetResourceRecord* ShadowModelResourceCache::findGeosetByPtrRef(
    void* geosetPtr) const {
  if (geosetPtr == nullptr)
    return nullptr;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_byGeoset.find(geosetPtr);
  if (it == m_byGeoset.end())
    return nullptr;
  return &it->second;
}

bool ShadowModelResourceCache::findGeosetByData(
    void *geosetDataPtr, ShadowGeosetResourceRecord &out) const {
  out = {};
  if (geosetDataPtr == nullptr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_byGeosetData.find(geosetDataPtr);
  if (it == m_byGeosetData.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowModelResourceCache::hydrateGeosetByKnownPtrs(
    void* geosetPtr, void* geosetDataPtr, ShadowGeosetResourceRecord& out) {
  out = {};
  if (geosetPtr == nullptr && geosetDataPtr == nullptr)
    return false;

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (geosetPtr != nullptr) {
      const auto it = m_byGeoset.find(geosetPtr);
      if (it != m_byGeoset.end() && it->second.readyForShadowConsumer()) {
        out = it->second;
        return true;
      }
    }
    if (geosetDataPtr != nullptr) {
      const auto it = m_byGeosetData.find(geosetDataPtr);
      if (it != m_byGeosetData.end() && it->second.readyForShadowConsumer()) {
        out = it->second;
        return true;
      }
    }
  }

  ShadowGeosetResourceRecord hydrated = {};
  if (!CaptureGeosetRecordFromKnownPtrs(geosetPtr, geosetDataPtr, hydrated))
    return false;
  if (!hydrated.readyForShadowConsumer())
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (hydrated.firstSeenFrame == 0)
    hydrated.firstSeenFrame = m_frameNumber;
  hydrated.lastSeenFrame = m_frameNumber;
  storeGeosetRecord(hydrated);
  out = hydrated;
  return true;
}

const ShadowGeosetResourceRecord* ShadowModelResourceCache::findGeosetByDataRef(
    void* geosetDataPtr) const {
  if (geosetDataPtr == nullptr)
    return nullptr;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_byGeosetData.find(geosetDataPtr);
  if (it == m_byGeosetData.end())
    return nullptr;
  return &it->second;
}

bool ShadowModelResourceCache::findModelGeoset(
    void *modelResourcePtr, uint32_t geosetIndex,
    ShadowGeosetResourceRecord &out) const {
  out = {};
  if (modelResourcePtr == nullptr || geosetIndex == kInvalidShadowGeosetIndex)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
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
      out = itGeoset->second;
      return true;
    }
  }

  if (geosetIndex >= modelRecord.geosetDataPtrs.size())
    return false;

  const void *geosetDataPtr = modelRecord.geosetDataPtrs[geosetIndex];
  if (geosetDataPtr == nullptr)
    return false;

  const auto itGeosetData = m_byGeosetData.find(const_cast<void *>(geosetDataPtr));
  if (itGeosetData == m_byGeosetData.end())
    return false;

  out = itGeosetData->second;
  return true;
}

bool ShadowModelResourceCache::findRuntimeModelGeoset(
    void* runtimeModelPtr, uint32_t geosetIndex,
    ShadowGeosetResourceRecord& out) const {
  out = {};
  if (runtimeModelPtr == nullptr || geosetIndex == kInvalidShadowGeosetIndex)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
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
      out = itGeoset->second;
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
  if (itGeosetData == m_byGeosetData.end())
    return false;

  out = itGeosetData->second;
  return true;
}

bool ShadowModelResourceCache::findRuntimeModelOwner(
    void* runtimeGeosetPtr, void* runtimeGeosetDataPtr, uint32_t geosetIndex,
    void* modelResourcePtr, ShadowModelResourceRecord& out) const {
  out = {};
  if (runtimeGeosetPtr == nullptr && runtimeGeosetDataPtr == nullptr)
    return false;

  modelResourcePtr = TryResolveDirectModelResourcePtr(modelResourcePtr);

  std::lock_guard<std::mutex> lock(m_mutex);
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

  std::lock_guard<std::mutex> lock(m_mutex);
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

bool ShadowModelResourceCache::findModelResource(
    void *modelResourcePtr, ShadowModelResourceRecord &out) const {
  out = {};
  if (modelResourcePtr == nullptr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_byModelResource.find(modelResourcePtr);
  if (it == m_byModelResource.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowModelResourceCache::findRuntimeModelResource(
    void* runtimeModelPtr, ShadowModelResourceRecord& out) const {
  out = {};
  if (runtimeModelPtr == nullptr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
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
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<ShadowGeosetResourceRecord> out;
  out.reserve(m_byGeoset.size() + m_byGeosetData.size());
  for (const auto &it : m_byGeoset)
    out.push_back(it.second);
  for (const auto& it : m_byGeosetData) {
    const auto& record = it.second;
    if (record.geosetPtr != nullptr &&
        m_byGeoset.find(record.geosetPtr) != m_byGeoset.end()) {
      continue;
    }
    out.push_back(record);
  }
  return out;
}

std::vector<ShadowModelResourceRecord> ShadowModelResourceCache::snapshotModels() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<ShadowModelResourceRecord> out;
  out.reserve(m_byModelResource.size());
  for (const auto &it : m_byModelResource)
    out.push_back(it.second);
  return out;
}

std::vector<ShadowModelResourceRecord>
ShadowModelResourceCache::snapshotRuntimeModels() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<ShadowModelResourceRecord> out;
  out.reserve(m_byRuntimeModel.size());
  for (const auto& it : m_byRuntimeModel)
    out.push_back(it.second);
  return out;
}

size_t ShadowModelResourceCache::geosetRecordCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = m_byGeoset.size();
  for (const auto& it : m_byGeosetData) {
    if (it.second.geosetPtr != nullptr &&
        m_byGeoset.find(it.second.geosetPtr) != m_byGeoset.end()) {
      continue;
    }
    ++count;
  }
  return count;
}

size_t ShadowModelResourceCache::readyGeosetCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_byGeoset) {
    if (it.second.readyForShadowConsumer())
      ++count;
  }
  for (const auto& it : m_byGeosetData) {
    if (it.second.geosetPtr != nullptr &&
        m_byGeoset.find(it.second.geosetPtr) != m_byGeoset.end()) {
      continue;
    }
    if (it.second.readyForShadowConsumer())
      ++count;
  }
  return count;
}

size_t ShadowModelResourceCache::modelResourceCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_byModelResource.size();
}

size_t ShadowModelResourceCache::runtimeModelRecordCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

uint64_t ShadowModelResourceCache::frameNumber() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_frameNumber;
}

uint64_t ShadowModelResourceCache::revision() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_revision;
}

} // namespace dxvk::war3::model
