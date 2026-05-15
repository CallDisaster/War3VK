#include "war3_shadow_renderer_core.h"

#include "war3_shadow_backend_dxvk.h"
#include "../render/war3_render_identity_bridge.h"
#include "../render/war3_upper_layer_shadow.h"
#include "../render/war3_shadow_object_registry.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../game/war3_unit.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_memory.h"
#include "../../d3d9_war3_debug.h"
#include "../../util/util_env.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace dxvk::war3::shadow {

namespace {

bool SemanticCoreTraceEnabled() {
  static const bool enabled =
      dxvk::env::getEnvVar("DXVK_WAR3_SEMANTIC_SHADOW_TRACE") == "1" ||
      dxvk::env::getEnvVar("DXVK_WAR3_SEMANTIC_SHADOW_TRACE") == "true" ||
      dxvk::env::getEnvVar("DXVK_WAR3_SEMANTIC_SHADOW_TRACE") == "TRUE";
  return enabled;
}

bool SemanticCoreSliceTraceEnabled() {
  static const bool enabled =
      dxvk::env::getEnvVar("DXVK_WAR3_SEMANTIC_SLICE_TRACE") == "1" ||
      dxvk::env::getEnvVar("DXVK_WAR3_SEMANTIC_SLICE_TRACE") == "true" ||
      dxvk::env::getEnvVar("DXVK_WAR3_SEMANTIC_SLICE_TRACE") == "TRUE";
  return enabled;
}

bool LooksLikeGeosetDataPtr(void* candidate);

bool RenderableUsesDirectGeosetData(
    const ShadowRenderableRecord& renderable) {
  return renderable.meshData != nullptr &&
         renderable.runtimeGeosetDataPtr != nullptr &&
         renderable.meshData == renderable.runtimeGeosetDataPtr;
}

bool IsCanonicalDirectGeosetWholeSlice(
    const ShadowRenderableRecord& renderable,
    const ShadowModelResourceRecord& resource,
    uint32_t primitiveBaseIndex,
    uint32_t indexCount) {
  if (!RenderableUsesDirectGeosetData(renderable) ||
      renderable.queueKind != render::VisibleRenderableQueueKind::MainQueue ||
      renderable.groupIdx != 0 ||
      renderable.unitPtr == nullptr ||
      renderable.rawcode == 0u ||
      renderable.objectKind == render::ObjectKind::Building ||
      (renderable.unitFlags5C & UnitFlags5C::Building) != 0u) {
    return false;
  }

  return primitiveBaseIndex == 0u &&
         indexCount != 0u &&
         indexCount == uint32_t(resource.indices.size()) &&
         resource.primitiveRecords.size() == 1u;
}

bool ShouldSkipLegacyMeshDataDecode(
    const ShadowRenderableRecord& renderable) {
  return RenderableUsesDirectGeosetData(renderable);
}

ShadowRenderableRecord ConvertVisibleRecord(
    const render::VisibleRenderableRecord& src, uint64_t frameSerial) {
  ShadowRenderableRecord dst = {};
  dst.worldObjectEntry = src.identity.worldObjectEntry;
  dst.sceneNode =
      src.identity.sceneNode != nullptr ? src.identity.sceneNode : src.sceneNode;
  dst.unitPtr = src.identity.unitPtr;
  dst.renderablePart = src.renderablePart;
  dst.payload = src.payload;
  dst.meshData = src.meshData;
  dst.layerState = src.layerState;
  dst.runtimeModelPtr = src.runtimeModelPtr;
  dst.modelResourcePtr = src.modelResourcePtr;
  dst.runtimeGeosetPtr = src.runtimeGeosetPtr;
  dst.runtimeGeosetDataPtr = src.runtimeGeosetDataPtr;
  dst.modelKey = src.modelKey;
  dst.flags = src.flags;
  dst.jHandle = src.identity.jHandle;
  dst.rawcode = src.identity.rawcode;
  dst.unitFlags5C = src.identity.flags5C;
  dst.layerIndex = src.layerIndex;
  dst.subIndex = src.subIndex;
  dst.transparentType = src.transparentType;
  dst.transparentSortKey = src.transparentSortKey;
  dst.meshIndex = src.meshIndex;
  dst.geosetIndex = src.geosetIndex;
  dst.objectKind = src.identity.kind;
  dst.queueKind = src.queueKind;
  dst.groupIdx = src.identity.groupIdx;
  dst.frameSerial = frameSerial;
  return dst;
}

ShadowModelResourceRecord ConvertGeosetRecord(
    const model::ShadowGeosetResourceRecord& src, uint64_t frameSerial) {
  ShadowModelResourceRecord dst = {};
  dst.runtimeGeosetPtr = src.geosetPtr;
  dst.runtimeGeosetDataPtr = src.geosetDataPtr;
  dst.modelResourcePtr = src.modelResourcePtr;
  dst.modelKey = src.modelKey;
  dst.prefersRuntimeContract = src.prefersRuntimeContract;
  dst.geosetIndex = src.geosetIndex;
  dst.vertexCount = src.vertexCount;
  dst.positions = src.positions;
  dst.normals = src.normals;
  dst.vertexGroupIndices = src.vertexGroupIndices;
  dst.primitiveRecords.reserve(src.primitiveRecords.size());
  for (const auto& primitive : src.primitiveRecords) {
    dst.primitiveRecords.push_back(ShadowPrimitiveRecord{
        primitive.primitiveTypeOrMaterialSlot, primitive.indexCount});
  }
  dst.matrixGroupSizes = src.matrixGroupSizes;
  dst.matrixIndices = src.matrixIndices;
  dst.indices = src.indices;
  dst.uvLayers.reserve(src.uvLayers.size());
  for (const auto& uvLayer : src.uvLayers)
    dst.uvLayers.push_back(uvLayer.uvPairs);
  dst.contentHash = src.contentHash;
  dst.frameSerial = frameSerial;
  return dst;
}

uint32_t InferCacheOwnerGeosetIndex(
    const ShadowRenderableRecord& record,
    const model::ShadowModelResourceRecord& ownerResource) {
  if (record.geosetIndex != kInvalidShadowContractGeosetIndex)
    return record.geosetIndex;

  for (uint32_t i = 0u; i < ownerResource.geosetPtrs.size(); ++i) {
    if (record.runtimeGeosetPtr != nullptr &&
        ownerResource.geosetPtrs[i] == record.runtimeGeosetPtr) {
      return i;
    }
  }
  for (uint32_t i = 0u; i < ownerResource.geosetDataPtrs.size(); ++i) {
    if (record.runtimeGeosetDataPtr != nullptr &&
        ownerResource.geosetDataPtrs[i] == record.runtimeGeosetDataPtr) {
      return i;
    }
  }
  return ownerResource.geosetCount == 1u ? 0u
                                         : kInvalidShadowContractGeosetIndex;
}

bool TryFindRenderableResourceFromCache(
    const ShadowRenderableRecord& record,
    uint64_t frameSerial,
    ShadowModelResourceRecord& out,
    model::ShadowModelResourceRecord* outOwnerResource = nullptr) {
  out = {};
  if (outOwnerResource != nullptr)
    *outOwnerResource = {};

  auto acceptGeoset = [&](const model::ShadowGeosetResourceRecord& geoset) {
    if (!geoset.readyForShadowConsumer())
      return false;
    out = ConvertGeosetRecord(geoset, frameSerial);
    return out.readyForConsumer();
  };

  auto& cache = model::ShadowModelResourceCache::instance();
  model::ShadowGeosetResourceRecord geoset = {};
  if (record.runtimeGeosetPtr != nullptr &&
      cache.findGeosetByPtr(record.runtimeGeosetPtr, geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }
  if (record.runtimeGeosetDataPtr != nullptr &&
      cache.findGeosetByData(record.runtimeGeosetDataPtr, geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }
  if (record.runtimeModelPtr != nullptr &&
      record.geosetIndex != kInvalidShadowContractGeosetIndex &&
      cache.findRuntimeModelGeoset(record.runtimeModelPtr, record.geosetIndex,
                                   geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }
  if (record.modelResourcePtr != nullptr &&
      record.geosetIndex != kInvalidShadowContractGeosetIndex &&
      cache.findModelGeoset(record.modelResourcePtr, record.geosetIndex,
                            geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }

  if (record.runtimeGeosetPtr == nullptr &&
      record.runtimeGeosetDataPtr == nullptr) {
    return false;
  }

  // Scene submission runs on a hot path and must not fall back to the cache's
  // owner scan when the contract/resource store failed direct indexing. The
  // scan is useful for offline diagnostics, but a single miss can walk the
  // runtime model table and stall the frame for hundreds of milliseconds.
  if (dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled()) {
    return false;
  }

  model::ShadowModelResourceRecord ownerResource = {};
  if (!cache.findRuntimeModelOwner(record.runtimeGeosetPtr,
                                   record.runtimeGeosetDataPtr,
                                   record.geosetIndex,
                                   record.modelResourcePtr, ownerResource)) {
    return false;
  }
  if (outOwnerResource != nullptr)
    *outOwnerResource = ownerResource;

  const uint32_t ownerGeosetIndex =
      InferCacheOwnerGeosetIndex(record, ownerResource);
  if (ownerGeosetIndex == kInvalidShadowContractGeosetIndex)
    return false;
  if (ownerGeosetIndex < ownerResource.geosetPtrs.size() &&
      ownerResource.geosetPtrs[ownerGeosetIndex] != nullptr &&
      cache.findGeosetByPtr(ownerResource.geosetPtrs[ownerGeosetIndex],
                            geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }
  if (ownerGeosetIndex < ownerResource.geosetDataPtrs.size() &&
      ownerResource.geosetDataPtrs[ownerGeosetIndex] != nullptr &&
      cache.findGeosetByData(ownerResource.geosetDataPtrs[ownerGeosetIndex],
                             geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }
  if (ownerResource.runtimeModelPtr != nullptr &&
      cache.findRuntimeModelGeoset(ownerResource.runtimeModelPtr,
                                   ownerGeosetIndex, geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }
  if (ownerResource.modelResourcePtr != nullptr &&
      cache.findModelGeoset(ownerResource.modelResourcePtr, ownerGeosetIndex,
                            geoset) &&
      acceptGeoset(geoset)) {
    return true;
  }

  return false;
}

bool IsStaticWorldRenderable(const ShadowRenderableRecord& record) {
  return record.objectKind == render::ObjectKind::Building ||
         record.objectKind == render::ObjectKind::Destructible;
}

bool IsResourceSaneForSceneSubmission(
    const ShadowModelResourceRecord& resource) {
  const uint32_t vertexCount =
      resource.vertexCount != 0u
          ? resource.vertexCount
          : uint32_t(resource.positions.size() / 3u);
  if (vertexCount == 0u || vertexCount > 20000u)
    return false;
  if (resource.positions.size() < size_t(vertexCount) * 3u)
    return false;
  if (!resource.indices.empty() && resource.indices.size() > 120000u)
    return false;
  if (!resource.vertexGroupIndices.empty() &&
      resource.vertexGroupIndices.size() < vertexCount) {
    return false;
  }
  if (resource.matrixGroupSizes.size() > 2048u ||
      resource.matrixIndices.size() > 32768u) {
    return false;
  }
  return true;
}

bool TryReadMeshSubPrimitiveRecords(
    const ShadowRenderableRecord& renderable,
    std::vector<ShadowPrimitiveRecord>& outPrimitiveRecords);

bool TryFindUniquePrimitiveSequenceInResource(
    const ShadowModelResourceRecord& resource,
    const std::vector<ShadowPrimitiveRecord>& targetRecords,
    bool requireTypeMatch, uint32_t& outPrimitiveBaseIndex,
    uint32_t& outIndexCount, ShadowPrimitiveTopology& outTopology);

bool IsStaticMeshDataPreviewCandidate(const ShadowRenderableRecord& record) {
  return IsStaticWorldRenderable(record) && record.meshData != nullptr &&
         record.sceneNode != nullptr;
}

struct StaticMeshDataResourceCacheEntry {
  size_t resourceRecordCount = 0u;
  const void* primaryStreamPtr = nullptr;
  uint32_t primaryStride = 0u;
  uint32_t meshIndex = kInvalidShadowContractGeosetIndex;
  bool hasResult = false;
  ShadowModelResourceRecord resource;
};

// Phase 7.82：Static mesh resource cache 改 shared_mutex。
// 主路径在 PrepareStaticGeometryFromMeshData 里 read-only 查询；只有 cache miss
// 才需要写。
std::shared_mutex& StaticMeshDataResourceCacheMutex() {
  static std::shared_mutex mutex;
  return mutex;
}

std::unordered_map<void*, StaticMeshDataResourceCacheEntry>&
StaticMeshDataResourceCache() {
  static std::unordered_map<void*, StaticMeshDataResourceCacheEntry> cache;
  return cache;
}

void ApplyStaticMeshDataResource(ShadowRenderableRecord& record,
                                 const ShadowModelResourceRecord& resource) {
  record.modelResourcePtr = resource.modelResourcePtr;
  if (record.modelKey == 0u)
    record.modelKey = resource.modelKey;
  record.runtimeGeosetPtr = resource.runtimeGeosetPtr;
  record.runtimeGeosetDataPtr = resource.runtimeGeosetDataPtr;
  record.geosetIndex = resource.geosetIndex;
  record.meshIndex = resource.geosetIndex;
}

bool TryReadSceneNodeWorldPose(const ShadowRenderableRecord& record,
                               uint64_t frameSerial,
                               ShadowPoseRecord& outPose) {
  outPose = {};
  if (record.sceneNode == nullptr)
    return false;

  const auto* matrixBase = reinterpret_cast<const uint8_t*>(record.sceneNode) +
                           dxvk::war3::SceneNodeOffsets::WorldMatrix;
  float raw[12] = {};
  if (!dxvk::war3::IsReadableRange(matrixBase, sizeof(raw)))
    return false;

  std::memcpy(raw, matrixBase, sizeof(raw));
  outPose.sceneNode = record.sceneNode;
  outPose.unitPtr = record.unitPtr;
  outPose.runtimeModelPtr = record.runtimeModelPtr;
  outPose.hasWorldTransform = true;
  outPose.worldTransform =
      Matrix4(Vector4(raw[0], raw[1], raw[2], 0.0f),
              Vector4(raw[3], raw[4], raw[5], 0.0f),
              Vector4(raw[6], raw[7], raw[8], 0.0f),
              Vector4(raw[9], raw[10], raw[11], 1.0f));
  outPose.frameSerial = frameSerial;
  return true;
}

bool TryReadMeshDataPrimaryStream(void* meshData,
                                  const void*& outStreamPtr,
                                  uint32_t& outStride) {
  outStreamPtr = nullptr;
  outStride = 0u;
  if (meshData == nullptr)
    return false;

  void* primaryStreamPtr = nullptr;
  uint32_t primaryStride = 0u;
  uint32_t primaryArg0 = 0u;
  if (!dxvk::war3::SafeReadPtrFast(
          meshData, dxvk::war3::MeshDataOffsets::PrimaryStreamPtr,
          primaryStreamPtr) ||
      primaryStreamPtr == nullptr) {
    return false;
  }
  dxvk::war3::SafeReadU32Fast(
      meshData, dxvk::war3::MeshDataOffsets::PrimaryStreamStride,
      primaryStride);
  dxvk::war3::SafeReadU32Fast(
      meshData, dxvk::war3::MeshDataOffsets::PrimaryStreamArg0,
      primaryArg0);

  const std::array<uint32_t, 6> strideCandidates = {
      primaryStride, primaryArg0, 12u, 16u, 20u, 24u};
  for (uint32_t stride : strideCandidates) {
    if (stride < 12u || stride > 128u)
      continue;
    if (!dxvk::war3::IsReadableRange(primaryStreamPtr, 12u))
      continue;
    outStreamPtr = primaryStreamPtr;
    outStride = stride;
    return true;
  }

  return false;
}

bool MeshPrimaryStreamMatchesResource(const void* streamPtr, uint32_t stride,
                                      const ShadowModelResourceRecord& resource) {
  if (streamPtr == nullptr || stride < 12u || resource.positions.empty())
    return false;

  const uint32_t vertexCount =
      resource.vertexCount != 0u
          ? resource.vertexCount
          : uint32_t(resource.positions.size() / 3u);
  if (vertexCount == 0u || vertexCount > 200000u ||
      resource.positions.size() < size_t(vertexCount) * 3u) {
    return false;
  }

  const auto* base = reinterpret_cast<const uint8_t*>(streamPtr);
  const auto closeEnough = [](float a, float b) {
    const float diff = std::fabs(a - b);
    const float scale = (std::max)(1.0f, (std::max)(std::fabs(a), std::fabs(b)));
    return diff <= 0.01f * scale;
  };

  std::array<uint32_t, 8> sampleIndices = {
      0u,
      vertexCount > 1u ? 1u : 0u,
      vertexCount > 2u ? 2u : 0u,
      vertexCount / 4u,
      vertexCount / 2u,
      (vertexCount * 3u) / 4u,
      vertexCount > 2u ? vertexCount - 2u : vertexCount - 1u,
      vertexCount - 1u};
  for (uint32_t index : sampleIndices) {
    if (index >= vertexCount)
      index = vertexCount - 1u;

    const size_t byteOffset = size_t(index) * size_t(stride);
    if (uint64_t(byteOffset) + sizeof(float) * 3ull >
            64ull * 1024ull * 1024ull ||
        !dxvk::war3::IsReadableRange(base + byteOffset,
                                     sizeof(float) * 3u)) {
      return false;
    }

    float xyz[3] = {};
    std::memcpy(xyz, base + byteOffset, sizeof(xyz));
    if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) ||
        !std::isfinite(xyz[2])) {
      return false;
    }

    const size_t posBase = size_t(index) * 3u;
    if (!closeEnough(xyz[0], resource.positions[posBase + 0u]) ||
        !closeEnough(xyz[1], resource.positions[posBase + 1u]) ||
        !closeEnough(xyz[2], resource.positions[posBase + 2u])) {
      return false;
    }
  }

  return true;
}

const ShadowModelResourceRecord* TryFindStaticMeshDataResource(
    ShadowRenderableRecord& record,
    const ShadowModelResourceStore& resources,
    ShadowModelResourceRecord& outResource) {
  outResource = {};
  if (!IsStaticWorldRenderable(record) || record.meshData == nullptr)
    return nullptr;

  const void* primaryStreamPtr = nullptr;
  uint32_t primaryStride = 0u;
  const bool hasPrimaryStream =
      TryReadMeshDataPrimaryStream(record.meshData, primaryStreamPtr,
                                   primaryStride);

  uint32_t meshIndex = record.meshIndex;
  if (meshIndex == kInvalidShadowContractGeosetIndex) {
    dxvk::war3::SafeReadU32Fast(record.meshData,
                                dxvk::war3::MeshDataOffsets::MeshIndex,
                                meshIndex);
  }

  const size_t resourceRecordCount = resources.records().size();
  {
    std::shared_lock<std::shared_mutex> lock(
        StaticMeshDataResourceCacheMutex());
    auto& cache = StaticMeshDataResourceCache();
    const auto it = cache.find(record.meshData);
    if (it != cache.end() &&
        it->second.resourceRecordCount == resourceRecordCount &&
        it->second.primaryStreamPtr == primaryStreamPtr &&
        it->second.primaryStride == primaryStride &&
        it->second.meshIndex == meshIndex) {
      if (!it->second.hasResult)
        return nullptr;
      outResource = it->second.resource;
      ApplyStaticMeshDataResource(record, outResource);
      return &outResource;
    }
  }

  std::vector<ShadowPrimitiveRecord> meshPrimitiveRecords;
  const ShadowModelResourceRecord* fallbackMatch = nullptr;
  if (TryReadMeshSubPrimitiveRecords(record, meshPrimitiveRecords) &&
      !meshPrimitiveRecords.empty()) {
    const auto tryPrimitiveSequenceMatch =
        [&](bool enforceMeshIndex) -> const ShadowModelResourceRecord* {
      const ShadowModelResourceRecord* match = nullptr;
      for (const auto& candidate : resources.records()) {
        if (!candidate.readyForConsumer() || candidate.primitiveRecords.empty())
          continue;
        if (enforceMeshIndex &&
            meshIndex != kInvalidShadowContractGeosetIndex &&
            candidate.geosetIndex != kInvalidShadowContractGeosetIndex &&
            candidate.geosetIndex != meshIndex) {
          continue;
        }

        uint32_t primitiveBaseIndex = 0u;
        uint32_t indexCount = 0u;
        ShadowPrimitiveTopology topology = ShadowPrimitiveTopology::TriangleList;
        if (!TryFindUniquePrimitiveSequenceInResource(
                candidate, meshPrimitiveRecords, true, primitiveBaseIndex,
                indexCount, topology)) {
          continue;
        }

        if (match != nullptr)
          return nullptr;
        match = &candidate;
      }
      return match;
    };

    fallbackMatch = tryPrimitiveSequenceMatch(true);
    if (fallbackMatch == nullptr &&
        meshIndex != kInvalidShadowContractGeosetIndex) {
      fallbackMatch = tryPrimitiveSequenceMatch(false);
    }
  }

  if (fallbackMatch == nullptr && hasPrimaryStream) {
    for (const auto& candidate : resources.records()) {
      if (!candidate.readyForConsumer())
        continue;
      if (meshIndex != kInvalidShadowContractGeosetIndex &&
          candidate.geosetIndex != kInvalidShadowContractGeosetIndex &&
          candidate.geosetIndex != meshIndex) {
        continue;
      }
      if (!MeshPrimaryStreamMatchesResource(primaryStreamPtr, primaryStride,
                                            candidate)) {
        continue;
      }

      if (fallbackMatch != nullptr)
        return nullptr;
      fallbackMatch = &candidate;
    }
  }

  if (fallbackMatch == nullptr &&
      meshIndex != kInvalidShadowContractGeosetIndex && hasPrimaryStream) {
    for (const auto& candidate : resources.records()) {
      if (!candidate.readyForConsumer())
        continue;
      if (!MeshPrimaryStreamMatchesResource(primaryStreamPtr, primaryStride,
                                            candidate)) {
        continue;
      }
      if (fallbackMatch != nullptr)
        return nullptr;
      fallbackMatch = &candidate;
    }
  }

  if (fallbackMatch == nullptr)
  {
    StaticMeshDataResourceCacheEntry entry = {};
    entry.resourceRecordCount = resourceRecordCount;
    entry.primaryStreamPtr = primaryStreamPtr;
    entry.primaryStride = primaryStride;
    entry.meshIndex = meshIndex;
    entry.hasResult = false;
    std::unique_lock<std::shared_mutex> lock(
        StaticMeshDataResourceCacheMutex());
    StaticMeshDataResourceCache()[record.meshData] = std::move(entry);
    return nullptr;
  }

  outResource = *fallbackMatch;
  ApplyStaticMeshDataResource(record, outResource);
  StaticMeshDataResourceCacheEntry entry = {};
  entry.resourceRecordCount = resourceRecordCount;
  entry.primaryStreamPtr = primaryStreamPtr;
  entry.primaryStride = primaryStride;
  entry.meshIndex = meshIndex;
  entry.hasResult = true;
  entry.resource = outResource;
  {
    std::unique_lock<std::shared_mutex> lock(
        StaticMeshDataResourceCacheMutex());
    StaticMeshDataResourceCache()[record.meshData] = std::move(entry);
  }
  return &outResource;
}

ShadowPoseRecord ConvertPoseRecord(const model::PoseRecord& src,
                                   uint64_t frameSerial) {
  ShadowPoseRecord dst = {};
  dst.runtimeModelPtr = src.runtimeModelPtr;
  dst.sceneNode = src.sceneNode;
  dst.unitPtr = src.unitPtr;
  dst.matrixCount = src.matrixCount;
  dst.matrixHash = src.matrixHash;
  dst.matrixPalette = src.matrixPalette;
  dst.hasWorldTransform = src.hasWorldTransform || src.hasSpriteFrameTransform;
  dst.worldTransform =
      src.hasSpriteFrameTransform ? src.spriteFrameTransform : src.worldTransform;
  dst.frameSerial = frameSerial;
  return dst;
}

ShadowPoseRecord ConvertShadowObjectPoseRecord(
    const render::ShadowObjectRecord& src, uint64_t frameSerial) {
  ShadowPoseRecord dst = {};
  dst.runtimeModelPtr = src.runtimeModelPtr;
  dst.sceneNode = src.sceneNode;
  dst.unitPtr = src.unitPtr;
  dst.matrixCount = src.matrixCount;
  dst.matrixHash = src.matrixHash;
  dst.hasWorldTransform = src.hasWorldTransform || src.hasSpriteFrameTransform;
  dst.worldTransform =
      src.hasSpriteFrameTransform ? src.spriteFrameTransform : src.worldTransform;
  dst.frameSerial = frameSerial;
  return dst;
}

void MergeAttachmentRootPoseCandidate(const ShadowPoseRecord& candidate,
                                      ShadowPoseRecord& ioPose) {
  if (candidate.runtimeModelPtr != nullptr && ioPose.runtimeModelPtr == nullptr)
    ioPose.runtimeModelPtr = candidate.runtimeModelPtr;
  if (candidate.sceneNode != nullptr && ioPose.sceneNode == nullptr)
    ioPose.sceneNode = candidate.sceneNode;
  if (candidate.unitPtr != nullptr && ioPose.unitPtr == nullptr)
    ioPose.unitPtr = candidate.unitPtr;
  if (!ioPose.hasWorldTransform && candidate.hasWorldTransform) {
    ioPose.hasWorldTransform = true;
    ioPose.worldTransform = candidate.worldTransform;
  }
  if (ioPose.matrixCount == 0u && candidate.matrixCount != 0u &&
      !candidate.matrixPalette.empty()) {
    ioPose.matrixCount = candidate.matrixCount;
    ioPose.matrixHash = candidate.matrixHash;
    ioPose.matrixPalette = candidate.matrixPalette;
  }
  if (ioPose.frameSerial == 0u && candidate.frameSerial != 0u)
    ioPose.frameSerial = candidate.frameSerial;
}

constexpr size_t kRuntimeMatrixCountOffset = 0x5Cu;
constexpr size_t kRuntimeMatrixArrayOffset = 0x60u;

// 全局调色板缓冲区基址的 RVA
// dword_6FBC6BD0 = Game.dll + 0xBC6BD0
// 这是引擎在 CModel_AllocAndFillGroupPalette 中使用的全局缓冲区
constexpr uintptr_t kGlobalPaletteBufferRva = 0xBC6BD0;

// 调色板槽位索引缓存：记录每个 RenderablePart 最近成功读取的调色板槽位索引
// 用于解决 RenderablePart + 0x08 在某些帧没有被更新的问题
struct PaletteSlotCacheEntry {
  void* renderablePart = nullptr;
  uint32_t paletteSlotIndex = 0xFFFFFFFF;
  uint64_t lastUpdateFrame = 0;
};
static constexpr size_t kMaxPaletteSlotCacheEntries = 4096;
static thread_local PaletteSlotCacheEntry s_paletteSlotCache[kMaxPaletteSlotCacheEntries];
static thread_local size_t s_paletteSlotCacheIndex = 0;
static std::atomic<uint64_t> g_paletteSlotCacheHitCount{0};
static std::atomic<uint64_t> g_paletteSlotCacheMissCount{0};

// 查找或更新调色板槽位索引缓存
static uint32_t FindOrUpdatePaletteSlotCache(void* renderablePart, uint32_t currentSlotIndex) {
  if (renderablePart == nullptr)
    return 0xFFFFFFFF;

  // 查找缓存
  for (size_t i = 0; i < kMaxPaletteSlotCacheEntries; ++i) {
    if (s_paletteSlotCache[i].renderablePart == renderablePart) {
      // 找到缓存条目
      if (currentSlotIndex != 0xFFFFFFFF && currentSlotIndex < 0x3A98) {
        // 更新缓存
        s_paletteSlotCache[i].paletteSlotIndex = currentSlotIndex;
        s_paletteSlotCache[i].lastUpdateFrame = g_paletteSlotCacheHitCount.load(std::memory_order_relaxed);
        g_paletteSlotCacheHitCount.fetch_add(1u, std::memory_order_relaxed);
        return currentSlotIndex;
      } else {
        // 使用缓存的值
        g_paletteSlotCacheHitCount.fetch_add(1u, std::memory_order_relaxed);
        return s_paletteSlotCache[i].paletteSlotIndex;
      }
    }
  }

  // 没有找到缓存条目，添加新的
  if (currentSlotIndex != 0xFFFFFFFF && currentSlotIndex < 0x3A98) {
    const size_t cacheSlot = s_paletteSlotCacheIndex % kMaxPaletteSlotCacheEntries;
    s_paletteSlotCache[cacheSlot].renderablePart = renderablePart;
    s_paletteSlotCache[cacheSlot].paletteSlotIndex = currentSlotIndex;
    s_paletteSlotCache[cacheSlot].lastUpdateFrame = g_paletteSlotCacheHitCount.load(std::memory_order_relaxed);
    s_paletteSlotCacheIndex++;
    g_paletteSlotCacheMissCount.fetch_add(1u, std::memory_order_relaxed);
    return currentSlotIndex;
  }

  g_paletteSlotCacheMissCount.fetch_add(1u, std::memory_order_relaxed);
  return 0xFFFFFFFF;
}

uint64_t HashMatrixPalette(const std::vector<Matrix4>& matrices) {
  uint64_t hash = 1469598103934665603ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(matrices.data());
  const size_t size = matrices.size() * sizeof(Matrix4);
  for (size_t i = 0; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

Matrix4 DecodeRuntimePoseMatrix48(const uint8_t* poseBytes) {
  if (poseBytes == nullptr)
    return Matrix4();

  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseBytes, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

ShadowPacketResource MakePacketResourceRef(
    const ShadowModelResourceRecord& src) {
  ShadowPacketResource dst = {};
  dst.modelResourcePtr = src.modelResourcePtr;
  dst.modelKey = src.modelKey;
  dst.geosetIndex = src.geosetIndex;
  dst.vertexCount = src.vertexCount;
  dst.primitiveRecordCount = uint32_t(src.primitiveRecords.size());
  dst.contentHash = src.contentHash;
  dst.topology = ShadowPrimitiveTopology::TriangleList;
  dst.positions = &src.positions;
  dst.vertexGroupIndices = &src.vertexGroupIndices;
  dst.indices = &src.indices;
  dst.matrixGroupSizes = &src.matrixGroupSizes;
  dst.matrixIndices = &src.matrixIndices;
  return dst;
}

bool TryOwnDynamicIndexSlice(ShadowPacketResource& dst,
                             const uint16_t* indexStream,
                             uint32_t indexCount) {
  if (indexStream == nullptr || indexCount == 0u)
    return false;

  constexpr uint32_t kMaxOwnedDynamicIndexCount = 2u * 1024u * 1024u;
  if (indexCount > kMaxOwnedDynamicIndexCount)
    return false;

  const size_t byteCount = size_t(indexCount) * sizeof(uint16_t);
  if (!dxvk::war3::IsReadableRange(indexStream, byteCount))
    return false;

  auto owned = std::make_shared<std::vector<uint16_t>>(
      indexStream, indexStream + indexCount);
  if (owned->empty())
    return false;

  dst.ownedDynamicIndices = std::move(owned);
  dst.dynamicIndexStream = dst.ownedDynamicIndices->data();
  dst.dynamicIndexCount = uint32_t(dst.ownedDynamicIndices->size());
  return true;
}

bool LooksLikeRuntimeModelPtr(void* candidate);
bool LooksLikeGeosetDataPtr(void* candidate);

void* TryResolveSpritePtrFromUnit(void* unitPtr) {
  if (unitPtr == nullptr)
    return nullptr;
  game::UnitWrapper unit(unitPtr);
  if (!unit.IsValid())
    return nullptr;
  return unit.GetSprite();
}

void* TryReadRuntimeModelFromSprite(void* spritePtr) {
  void* runtimeModelPtr = nullptr;
  if (spritePtr == nullptr)
    return nullptr;
  if (!dxvk::war3::SafeReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::Model,
                                   runtimeModelPtr) ||
      !LooksLikeRuntimeModelPtr(runtimeModelPtr)) {
    return nullptr;
  }
  return runtimeModelPtr;
}

void* TryResolveSpritePtrFromRuntimeModel(
    void* runtimeModelPtr, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry) {
  if (runtimeModelPtr == nullptr)
    return nullptr;

  model::ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord) &&
      instanceRecord.spritePtr != nullptr) {
    return instanceRecord.spritePtr;
  }
  if (instanceRegistry.findOwnerByRuntimeModel(runtimeModelPtr, instanceRecord) &&
      instanceRecord.spritePtr != nullptr) {
    return instanceRecord.spritePtr;
  }

  render::ShadowObjectRecord shadowRecord = {};
  if (shadowRegistry.findByRuntimeModel(runtimeModelPtr, shadowRecord) &&
      shadowRecord.spritePtr != nullptr) {
    return shadowRecord.spritePtr;
  }

  model::ModelResourceRecord modelRecord = {};
  if (model::ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                          modelRecord) &&
      modelRecord.spritePtr != nullptr) {
    return modelRecord.spritePtr;
  }

  return nullptr;
}

bool TryReadRuntimeMatrixPaletteLive(void* runtimeModelPtr,
                                     ShadowPoseRecord& outPose) {
  outPose = {};
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return false;

  uint32_t matrixCount = 0u;
  void* matrixBase = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(runtimeModelPtr, kRuntimeMatrixCountOffset,
                                   matrixCount) ||
      !dxvk::war3::SafeReadPtrFast(runtimeModelPtr, kRuntimeMatrixArrayOffset,
                                   matrixBase) ||
      matrixBase == nullptr || matrixCount == 0u) {
    return false;
  }

  matrixCount = (std::min)(matrixCount, 256u);
  const size_t bytes = size_t(matrixCount) * 48u;
  if (!dxvk::war3::IsReadableRange(matrixBase, bytes))
    return false;

  outPose.runtimeModelPtr = runtimeModelPtr;
  outPose.matrixCount = matrixCount;
  outPose.matrixPalette.resize(matrixCount);
  const auto* raw = reinterpret_cast<const uint8_t*>(matrixBase);
  for (uint32_t i = 0u; i < matrixCount; ++i)
    outPose.matrixPalette[i] = DecodeRuntimePoseMatrix48(raw + size_t(i) * 48u);
  outPose.matrixHash =
      outPose.matrixPalette.empty() ? 0ull : HashMatrixPalette(outPose.matrixPalette);
  return outPose.matrixCount != 0u && !outPose.matrixPalette.empty();
}

bool TryResolvePoseByRuntimeModelSnapshotOnly(const ShadowPoseStore& poses,
                                              void* runtimeModelPtr,
                                              ShadowPoseRecord& outPose) {
  outPose = {};
  bool hit = false;

  auto usableMatrixCount = [](const ShadowPoseRecord& pose) {
    if (pose.matrixCount == 0u || pose.matrixPalette.empty())
      return 0u;
    return (std::min)(pose.matrixCount, uint32_t(pose.matrixPalette.size()));
  };

  auto consider = [&](const ShadowPoseRecord& candidate) {
    const uint32_t candidateCount = usableMatrixCount(candidate);
    const uint32_t currentCount = usableMatrixCount(outPose);
    if (candidateCount == 0u)
      return;
    if (currentCount == 0u || candidateCount > currentCount ||
        (candidateCount == currentCount && !outPose.hasWorldTransform &&
         candidate.hasWorldTransform)) {
      outPose = candidate;
      hit = true;
    }
  };

  auto trySnapshotCandidate = [&](void* candidateRuntimeModelPtr) {
    if (candidateRuntimeModelPtr == nullptr)
      return;
    if (const auto* candidate =
            poses.findByRuntimeModelPtr(candidateRuntimeModelPtr)) {
      consider(*candidate);
    }
  };

  // The consumer hot path uses the frame contract and known +0xA0/-0xA0
  // aliases only. Live CModel reads stay in the explicit diagnostic fallback.
  trySnapshotCandidate(runtimeModelPtr);
  constexpr intptr_t kCModelComplexExtensionOffset = 0xA0;
  const auto base = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (runtimeModelPtr != nullptr) {
    const uintptr_t plusAlias = base + uintptr_t(kCModelComplexExtensionOffset);
    if (plusAlias >= 0x10000u && plusAlias != base)
      trySnapshotCandidate(reinterpret_cast<void*>(plusAlias));
    if (base > uintptr_t(kCModelComplexExtensionOffset)) {
      const uintptr_t minusAlias =
          base - uintptr_t(kCModelComplexExtensionOffset);
      if (minusAlias >= 0x10000u && minusAlias != base)
        trySnapshotCandidate(reinterpret_cast<void*>(minusAlias));
    }
  }
  if (hit)
    return true;

  return false;
}

bool TryResolvePoseByRuntimeModelSnapshotOrLive(const ShadowPoseStore& poses,
                                                void* runtimeModelPtr,
                                                ShadowPoseRecord& outPose) {
  if (TryResolvePoseByRuntimeModelSnapshotOnly(poses, runtimeModelPtr, outPose))
    return true;

  outPose = {};
  bool hit = false;

  auto usableMatrixCount = [](const ShadowPoseRecord& pose) {
    if (pose.matrixCount == 0u || pose.matrixPalette.empty())
      return 0u;
    return (std::min)(pose.matrixCount, uint32_t(pose.matrixPalette.size()));
  };

  auto consider = [&](const ShadowPoseRecord& candidate) {
    const uint32_t candidateCount = usableMatrixCount(candidate);
    const uint32_t currentCount = usableMatrixCount(outPose);
    if (candidateCount == 0u)
      return;
    if (currentCount == 0u || candidateCount > currentCount ||
        (candidateCount == currentCount && !outPose.hasWorldTransform &&
         candidate.hasWorldTransform)) {
      outPose = candidate;
      hit = true;
    }
  };

  auto tryRuntimeCandidate = [&](void* candidateRuntimeModelPtr) {
    if (candidateRuntimeModelPtr == nullptr)
      return;
    ShadowPoseRecord candidate = {};
    if (TryReadRuntimeMatrixPaletteLive(candidateRuntimeModelPtr, candidate))
      consider(candidate);
  };

  tryRuntimeCandidate(runtimeModelPtr);

  // CModelComplex stores/addresses some hot sprite-frame state at the +0xA0
  // extension while resource ownership may still be keyed by the CModel base.
  // Try both aliases so semantic rendering can join resource-owner and pose
  // observations without promoting either representation to a new contract key.
  constexpr intptr_t kCModelComplexExtensionOffset = 0xA0;
  const auto base = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (runtimeModelPtr != nullptr) {
    const uintptr_t plusAlias = base + uintptr_t(kCModelComplexExtensionOffset);
    if (plusAlias >= 0x10000u && plusAlias != base)
      tryRuntimeCandidate(reinterpret_cast<void*>(plusAlias));
    if (base > uintptr_t(kCModelComplexExtensionOffset)) {
      const uintptr_t minusAlias =
          base - uintptr_t(kCModelComplexExtensionOffset);
      if (minusAlias >= 0x10000u && minusAlias != base)
        tryRuntimeCandidate(reinterpret_cast<void*>(minusAlias));
    }
  }

  return hit;
}

bool TryResolveWorldPoseByRuntimeModelSnapshot(const ShadowPoseStore& poses,
                                               void* runtimeModelPtr,
                                               ShadowPoseRecord& outPose) {
  outPose = {};
  if (runtimeModelPtr == nullptr)
    return false;

  auto tryCandidate = [&](void* candidateRuntimeModelPtr) {
    if (candidateRuntimeModelPtr == nullptr)
      return false;
    const auto* candidate = poses.findByRuntimeModelPtr(candidateRuntimeModelPtr);
    if (candidate == nullptr || !candidate->hasWorldTransform) {
      return false;
    }
    outPose = *candidate;
    return true;
  };

  if (tryCandidate(runtimeModelPtr))
    return true;

  constexpr intptr_t kCModelComplexExtensionOffset = 0xA0;
  const auto base = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (base >= 0x10000u) {
    if (tryCandidate(reinterpret_cast<void*>(
            base + uintptr_t(kCModelComplexExtensionOffset)))) {
      return true;
    }
    if (base > uintptr_t(kCModelComplexExtensionOffset) &&
        tryCandidate(reinterpret_cast<void*>(
            base - uintptr_t(kCModelComplexExtensionOffset)))) {
      return true;
    }
  }

  return false;
}

bool TryResolveResourceOwnerWorldPose(const ShadowRenderableRecord& renderable,
                                      const ShadowPoseStore& poses,
                                      void*& outRuntimeModelPtr,
                                      ShadowPoseRecord& outPose) {
  outRuntimeModelPtr = nullptr;
  outPose = {};

  model::ShadowModelResourceRecord runtimeOwner = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (resourceCache.findRuntimeModelOwner(
          renderable.runtimeGeosetPtr, renderable.runtimeGeosetDataPtr,
          renderable.geosetIndex, renderable.modelResourcePtr, runtimeOwner) &&
      runtimeOwner.runtimeModelPtr != nullptr &&
      TryResolveWorldPoseByRuntimeModelSnapshot(poses, runtimeOwner.runtimeModelPtr,
                                                outPose)) {
    outRuntimeModelPtr = runtimeOwner.runtimeModelPtr;
    return true;
  }

  if (renderable.runtimeModelPtr != nullptr &&
      TryResolveWorldPoseByRuntimeModelSnapshot(poses, renderable.runtimeModelPtr,
                                                outPose)) {
    outRuntimeModelPtr = renderable.runtimeModelPtr;
    return true;
  }

  return false;
}

void* TryResolveDirectModelResourceFromRuntimeModel(void* runtimeModelPtr) {
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return nullptr;

  auto& resourceCache = model::ShadowModelResourceCache::instance();

  model::ShadowModelResourceRecord runtimeResource = {};
  if (resourceCache.findRuntimeModelResource(runtimeModelPtr, runtimeResource) &&
      runtimeResource.modelResourcePtr != nullptr) {
    if (void* directModelResourcePtr =
            resourceCache.resolveDirectModelResourcePtr(
                runtimeResource.modelResourcePtr)) {
      return directModelResourcePtr;
    }
  }

  model::ModelResourceRecord modelRecord = {};
  if (model::ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                          modelRecord) &&
      modelRecord.modelResourcePtr != nullptr) {
    if (void* directModelResourcePtr =
            resourceCache.resolveDirectModelResourcePtr(
                modelRecord.modelResourcePtr)) {
      return directModelResourcePtr;
    }
  }

  void* ownedModelResourcePtr = nullptr;
  dxvk::war3::SafeReadPtrFast(
      runtimeModelPtr, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
      ownedModelResourcePtr);
  return resourceCache.resolveDirectModelResourcePtr(ownedModelResourcePtr);
}

bool LooksLikeRuntimeModelPtr(void* candidate) {
  if (candidate == nullptr)
    return false;

  const uintptr_t candidateValue = reinterpret_cast<uintptr_t>(candidate);
  if (candidateValue < 0x10000u)
    return false;

  void* ownedHandlePtr = nullptr;
  const bool hasOwnedHandle =
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          ownedHandlePtr) &&
      ownedHandlePtr != nullptr &&
      model::ShadowModelResourceCache::instance().resolveDirectModelResourcePtr(
          ownedHandlePtr) != nullptr;

  uint32_t runtimeGeosetCount = 0u;
  void* runtimeGeosets = nullptr;
  const bool hasRuntimeGeosets =
      dxvk::war3::SafeReadU32Fast(candidate,
                                  dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                                  runtimeGeosetCount) &&
      runtimeGeosetCount > 0u &&
      runtimeGeosetCount < 4096u &&
      dxvk::war3::SafeReadPtrFast(candidate,
                                  dxvk::war3::CModelOffsets::RuntimeGeosets,
                                  runtimeGeosets) &&
      runtimeGeosets != nullptr &&
      dxvk::war3::IsReadableRange(
          runtimeGeosets,
          size_t(runtimeGeosetCount > 4u ? 4u : runtimeGeosetCount) *
              sizeof(void*));

  uint32_t finalPoseMatrixCount = 0u;
  void* finalPoseMatrixArray = nullptr;
  const bool hasFinalPoseArray =
      dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          finalPoseMatrixCount) &&
      finalPoseMatrixCount > 0u &&
      finalPoseMatrixCount <= 256u &&
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          finalPoseMatrixArray) &&
      finalPoseMatrixArray != nullptr &&
      dxvk::war3::IsReadableRange(finalPoseMatrixArray,
                                  size_t(sizeof(float)) * 16u);

  return hasRuntimeGeosets || (hasOwnedHandle && hasFinalPoseArray);
}

bool LooksLikeGeosetDataPtr(void* candidate) {
  if (candidate == nullptr)
    return false;

  uint32_t vertexCount = 0u;
  uint32_t primitiveCount = 0u;
  uint32_t matrixGroupCount = 0u;
  uint32_t matrixIndexCount = 0u;
  void* positions = nullptr;
  void* primitiveRecords = nullptr;
  void* matrixGroupSizes = nullptr;
  void* matrixIndices = nullptr;

  if (!dxvk::war3::SafeReadU32Fast(candidate,
                                   dxvk::war3::CGeosetDataOffsets::VertexCount,
                                   vertexCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::VertexPositions,
          positions) ||
      !dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
          primitiveCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
          primitiveRecords) ||
      !dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
          matrixGroupCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
          matrixGroupSizes) ||
      !dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
          matrixIndexCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixIndices,
          matrixIndices)) {
    return false;
  }

  if (vertexCount == 0u || vertexCount > (1u << 20) ||
      primitiveCount == 0u || primitiveCount > (1u << 16) ||
      matrixGroupCount == 0u || matrixGroupCount > 4096u ||
      matrixIndexCount == 0u || matrixIndexCount > (1u << 16)) {
    return false;
  }

  return dxvk::war3::IsReadableRange(
             positions,
             (std::min)(size_t(vertexCount) * 3u * sizeof(float), size_t(64u))) &&
         dxvk::war3::IsReadableRange(
             primitiveRecords,
             (std::min)(size_t(primitiveCount) *
                            sizeof(dxvk::war3::GeosetPrimitiveRecord),
                        size_t(64u))) &&
         dxvk::war3::IsReadableRange(
             matrixGroupSizes,
             (std::min)(size_t(matrixGroupCount) * sizeof(uint32_t),
                        size_t(64u))) &&
         dxvk::war3::IsReadableRange(
             matrixIndices,
             (std::min)(size_t(matrixIndexCount) * sizeof(uint32_t),
                        size_t(64u)));
}

constexpr uint32_t PackFourCcEditor(char c0, char c1, char c2, char c3) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(c0)) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c1)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c2)) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(c3));
}

constexpr uint32_t ByteSwapU32(uint32_t v) {
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

uint32_t NormalizeFourCcEditorOrder(uint32_t rawcode) {
  if (rawcode == 0u)
    return 0u;

  const uint32_t direct = rawcode;
  const uint32_t swapped = ByteSwapU32(rawcode);
  auto looksLikeEditorOrder = [](uint32_t v) -> bool {
    const uint32_t c0 = (v >> 24) & 0xFFu;
    const uint32_t c1 = (v >> 16) & 0xFFu;
    return c0 == static_cast<uint32_t>('Y') &&
           (c1 == static_cast<uint32_t>('T') ||
            c1 == static_cast<uint32_t>('t'));
  };

  uint32_t normalized = looksLikeEditorOrder(direct) ? direct : swapped;
  const uint32_t c1 = (normalized >> 16) & 0xFFu;
  if (c1 >= static_cast<uint32_t>('a') &&
      c1 <= static_cast<uint32_t>('z')) {
    normalized = (normalized & 0xFF00FFFFu) | ((c1 - 0x20u) << 16);
  }
  return normalized;
}

bool IsBlockedSemanticFourCc(uint32_t rawcode) {
  switch (NormalizeFourCcEditorOrder(rawcode)) {
  case PackFourCcEditor('Y', 'T', 'a', 'b'):
  case PackFourCcEditor('Y', 'T', 'a', 'c'):
  case PackFourCcEditor('Y', 'T', 'p', 'b'):
  case PackFourCcEditor('Y', 'T', 'p', 'c'):
  case PackFourCcEditor('Y', 'T', 'f', 'b'):
  case PackFourCcEditor('Y', 'T', 'f', 'c'):
  case PackFourCcEditor('Y', 'T', 'l', 'b'):
  case PackFourCcEditor('Y', 'T', 'l', 'c'):
    return true;
  default:
    return false;
  }
}

void AppendDistinctRuntimeModelPtrTrusted(std::vector<void*>& outRuntimeModels,
                                          void* runtimeModelPtr) {
  if (runtimeModelPtr == nullptr)
    return;
  if (std::find(outRuntimeModels.begin(), outRuntimeModels.end(),
                runtimeModelPtr) != outRuntimeModels.end()) {
    return;
  }
  outRuntimeModels.push_back(runtimeModelPtr);
}

void AppendDistinctRuntimeModelPtr(std::vector<void*>& outRuntimeModels,
                                   void* runtimeModelPtr) {
  if (runtimeModelPtr == nullptr || !LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return;
  AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels, runtimeModelPtr);
}

void CollectRenderableRuntimeModelRoots(const ShadowRenderableRecord& renderable,
                                        std::vector<void*>& outRuntimeModels) {
  outRuntimeModels.clear();

  model::ShadowModelResourceRecord runtimeOwner = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (resourceCache.findRuntimeModelOwner(renderable.runtimeGeosetPtr,
                                          renderable.runtimeGeosetDataPtr,
                                          renderable.geosetIndex,
                                          renderable.modelResourcePtr,
                                          runtimeOwner)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         runtimeOwner.runtimeModelPtr);
  }

  AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                       renderable.runtimeModelPtr);

  model::ModelInstanceRecord instanceRecord = {};
  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  if (renderable.worldObjectEntry != nullptr &&
      instanceRegistry.findByWorldObjectEntry(renderable.worldObjectEntry,
                                              instanceRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         instanceRecord.runtimeModelPtr);
  }
  if (renderable.sceneNode != nullptr &&
      instanceRegistry.findBySceneNode(renderable.sceneNode, instanceRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         instanceRecord.runtimeModelPtr);
  }
  if (renderable.unitPtr != nullptr &&
      instanceRegistry.findByUnitPtr(renderable.unitPtr, instanceRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         instanceRecord.runtimeModelPtr);
  }
  if (renderable.jHandle != 0u &&
      instanceRegistry.findByHandle(renderable.jHandle, instanceRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         instanceRecord.runtimeModelPtr);
  }

  render::ShadowObjectRecord shadowRecord = {};
  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  if (renderable.worldObjectEntry != nullptr &&
      shadowRegistry.findByWorldObjectEntry(renderable.worldObjectEntry,
                                            shadowRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         shadowRecord.runtimeModelPtr);
  }
  if (renderable.sceneNode != nullptr &&
      shadowRegistry.findBySceneNode(renderable.sceneNode, shadowRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         shadowRecord.runtimeModelPtr);
  }
  if (renderable.unitPtr != nullptr &&
      shadowRegistry.findByUnitPtr(renderable.unitPtr, shadowRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         shadowRecord.runtimeModelPtr);
  }
  if (renderable.jHandle != 0u &&
      shadowRegistry.findByHandle(renderable.jHandle, shadowRecord)) {
    AppendDistinctRuntimeModelPtrTrusted(outRuntimeModels,
                                         shadowRecord.runtimeModelPtr);
  }

  const render::RenderObjectInfo* renderObject = nullptr;
  auto& renderRegistry = render::RenderObjectRegistry::instance();
  if (renderable.sceneNode != nullptr)
    renderObject = renderRegistry.findBySceneNode(renderable.sceneNode);
  if (renderObject == nullptr && renderable.worldObjectEntry != nullptr)
    renderObject = renderRegistry.findByEntry(renderable.worldObjectEntry);
  if (renderObject == nullptr && renderable.jHandle != 0u)
    renderObject = renderRegistry.findByHandle(renderable.jHandle);

  void* spritePtr = TryResolveSpritePtrFromUnit(renderable.unitPtr);
  if (spritePtr == nullptr && renderObject != nullptr && renderObject->unitPtr != nullptr)
    spritePtr = TryResolveSpritePtrFromUnit(renderObject->unitPtr);
  if (spritePtr == nullptr && instanceRecord.spritePtr != nullptr)
    spritePtr = instanceRecord.spritePtr;
  if (spritePtr == nullptr && shadowRecord.spritePtr != nullptr)
    spritePtr = shadowRecord.spritePtr;

  AppendDistinctRuntimeModelPtr(outRuntimeModels,
                                TryReadRuntimeModelFromSprite(spritePtr));
}

bool IsSupplementalRootUnitRenderable(const ShadowRenderableRecord& renderable) {
  return renderable.objectKind == render::ObjectKind::Unit &&
         renderable.runtimeModelPtr != nullptr &&
         renderable.modelResourcePtr != nullptr &&
         renderable.meshData == nullptr && renderable.renderablePart == nullptr;
}

ShadowPacketResource MakePacketResourceOwned(
    const ShadowModelResourceRecord& src) {
  ShadowPacketResource dst = {};
  dst.modelResourcePtr = src.modelResourcePtr;
  dst.modelKey = src.modelKey;
  dst.geosetIndex = src.geosetIndex;
  dst.vertexCount = src.vertexCount;
  dst.primitiveRecordCount = uint32_t(src.primitiveRecords.size());
  dst.contentHash = src.contentHash;
  dst.topology = ShadowPrimitiveTopology::TriangleList;
  dst.ownedPositions = src.positions;
  dst.ownedVertexGroupIndices = src.vertexGroupIndices;
  dst.ownedIndices = src.indices;
  dst.ownedMatrixGroupSizes = src.matrixGroupSizes;
  dst.ownedMatrixIndices = src.matrixIndices;
  dst.positions = &dst.ownedPositions;
  dst.vertexGroupIndices = &dst.ownedVertexGroupIndices;
  dst.indices = &dst.ownedIndices;
  dst.matrixGroupSizes = &dst.ownedMatrixGroupSizes;
  dst.matrixIndices = &dst.ownedMatrixIndices;
  return dst;
}

struct MeshLayerBindingContract {
  uint32_t meshIndex = kInvalidShadowContractGeosetIndex;
  uint32_t layerIndex = 0u;
  uint32_t layerCount = 0u;
  uint32_t sceneStagePresetBaseIndex = 0u;
  uint32_t stagePresetSpanBaseIndex = 0u;
  uint32_t stagePresetIndex0 = 0u;
  uint32_t stagePresetIndex1 = 0u;
  uint32_t resolvedStagePresetIndex0 = 0u;
  uint32_t resolvedStagePresetIndex1 = 0u;
  const void* layerStateRecordPtr = nullptr;
  const void* layerStateViewPtr = nullptr;
  uint32_t primaryResourceBinding = 0u;
  uint32_t blendOrDrawMode = 0u;
  uint32_t layerStateWord0 = 0u;
  uint32_t layerStateWord18 = 0u;
  uint32_t layerStateWord1C = 0u;
  uint32_t layerStateWord20 = 0u;
  int32_t auxRefIndex0 = -1;
  int32_t auxRefIndex1 = -1;
  uint32_t auxRefEnable0 = 0u;
  uint32_t auxRefEnable1 = 0u;
  uintptr_t auxStreamPtr0 = 0u;
  uintptr_t auxStreamPtr1 = 0u;
  uint32_t auxStreamStride0 = 8u;
  uint32_t auxStreamStride1 = 8u;
  uint32_t auxEntry0Word0 = 0u;
  uint32_t auxEntry0Word8 = 0u;
  uint32_t auxEntry1Word0 = 0u;
  uint32_t auxEntry1Word8 = 0u;
  uint32_t stageMode0 = 0u;
  uint32_t stageMode1 = 0u;
  bool batchLayerStateProvided = false;
  bool usesCanonicalLayerStateRecord = false;
  bool batchLayerStateMatchesRecordBase = false;
  bool batchLayerStateMatchesStateView = false;
  bool hasAuxEntry0Snapshot = false;
  bool hasAuxEntry1Snapshot = false;
  bool hasAuxStream0 = false;
  bool hasAuxStream1 = false;

  bool hasAnyAuxStream() const {
    return (auxRefEnable0 != 0u && hasAuxStream0) ||
           (auxRefEnable1 != 0u && hasAuxStream1);
  }

  bool prefersPrimaryStreamAuxView() const {
    // 最新 IDA 结论已经坐实：
    // - `MeshAuxResourceEntry + 0x08` 是显式送进 `sub_6F0E35B0` 的 aux stream ptr；
    // - `primaryResourceBinding == 3` 才是允许 primary interleaved aux view
    //   的 prepared-primitive profile。
    // 因此不要再因为“看到了 aux gate”就盲扫 primary stream，先尊重显式
    // aux stream；只有 profile3 再把 primary stream 当候选。
    return primaryResourceBinding == 3u;
  }
};

bool TryResolveMeshLayerBindingContract(const ShadowRenderableRecord& renderable,
                                        MeshLayerBindingContract& out);

ShadowAlphaMode ResolveShadowAlphaMode(
    const ShadowRenderableRecord& renderable,
    const MeshLayerBindingContract* layerContract) {
  const uint32_t blendOrDrawMode =
      layerContract != nullptr
          ? layerContract->blendOrDrawMode
          : (renderable.queueKind == render::VisibleRenderableQueueKind::Transparent
                 ? 2u
                 : 0u);
  if (renderable.queueKind == render::VisibleRenderableQueueKind::Transparent ||
      blendOrDrawMode >= 2u) {
    return ShadowAlphaMode::AlphaBlend;
  }
  if (blendOrDrawMode == 1u)
    return ShadowAlphaMode::Cutout;
  return ShadowAlphaMode::Opaque;
}

ShadowMaterialSignature BuildShadowMaterialSignature(
    const ShadowRenderableRecord& renderable,
    const MeshLayerBindingContract* layerContract = nullptr) {
  ShadowMaterialSignature signature = {};
  signature.alphaMode = ResolveShadowAlphaMode(renderable, layerContract);
  signature.alphaCutoutRef =
      signature.alphaMode == ShadowAlphaMode::Cutout ? 0.5f : 0.0f;
  signature.blendOrDrawMode =
      layerContract != nullptr
          ? layerContract->blendOrDrawMode
          : (renderable.queueKind == render::VisibleRenderableQueueKind::Transparent
                 ? 2u
                 : 0u);
  signature.layerIndex = renderable.layerIndex;
  signature.queueKind =
      renderable.queueKind == render::VisibleRenderableQueueKind::Transparent
          ? 1u
          : 0u;
  signature.transparentType = renderable.transparentType;
  signature.layerContractResolved = layerContract != nullptr;
  if (layerContract != nullptr) {
    signature.layerContractMeshIndex = layerContract->meshIndex;
    signature.layerContractLayerCount = layerContract->layerCount;
  }

  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, renderable.flags);
  hash = bit::fnv1a_iter(hash, renderable.layerIndex);
  hash = bit::fnv1a_iter(hash, renderable.transparentType);
  hash = bit::fnv1a_iter(hash, renderable.transparentSortKey);
  hash = bit::fnv1a_iter(hash, signature.blendOrDrawMode);
  hash = bit::fnv1a_iter(hash, signature.queueKind);
  hash = bit::fnv1a_iter(hash, signature.transparentType);
  hash = bit::fnv1a_iter(hash, uint32_t(signature.alphaMode));
  hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(signature.alphaCutoutRef));
  hash = bit::fnv1a_iter(
      hash, uint64_t(reinterpret_cast<uintptr_t>(renderable.layerState)));

  if (layerContract != nullptr) {
    hash = bit::fnv1a_iter(hash, layerContract->primaryResourceBinding);
    hash = bit::fnv1a_iter(hash, layerContract->blendOrDrawMode);
    hash = bit::fnv1a_iter(hash, layerContract->auxRefEnable0);
    hash = bit::fnv1a_iter(hash, layerContract->auxRefEnable1);
    hash = bit::fnv1a_iter(hash, layerContract->resolvedStagePresetIndex0);
    hash = bit::fnv1a_iter(hash, layerContract->resolvedStagePresetIndex1);
    hash = bit::fnv1a_iter(hash, layerContract->stageMode0);
    hash = bit::fnv1a_iter(hash, layerContract->stageMode1);
  } else if (renderable.layerState != nullptr &&
             dxvk::war3::IsReadableRange(renderable.layerState, 20u)) {
    std::array<uint32_t, 5> layerPrefix = {};
    std::memcpy(layerPrefix.data(), renderable.layerState,
                sizeof(uint32_t) * layerPrefix.size());
    for (uint32_t word : layerPrefix)
      hash = bit::fnv1a_iter(hash, word);
  }

  signature.signatureHash = hash != 0u ? hash : 1u;
  return signature;
}

} // namespace

ShadowMaterialSignature BuildShadowMaterialSignatureForRenderable(
    const ShadowRenderableRecord& renderable) {
  MeshLayerBindingContract layerContract = {};
  const bool hasLayerContract =
      TryResolveMeshLayerBindingContract(renderable, layerContract);
  return BuildShadowMaterialSignature(renderable,
                                      hasLayerContract ? &layerContract : nullptr);
}

bool InspectShadowMaterialBindingForRenderable(
    const ShadowRenderableRecord& renderable,
    ShadowMaterialBindingDiagnostics& out) {
  out = {};
  MeshLayerBindingContract layerContract = {};
  if (!TryResolveMeshLayerBindingContract(renderable, layerContract))
    return false;

  out.resolved = true;
  out.meshIndex = layerContract.meshIndex;
  out.layerIndex = layerContract.layerIndex;
  out.layerCount = layerContract.layerCount;
  out.blendOrDrawMode = layerContract.blendOrDrawMode;
  return true;
}

namespace {

constexpr int kDynamicAuxCandidateStream1 = -1;
constexpr int kDynamicAuxCandidatePrimaryStream = -2;
constexpr int kDynamicAuxCandidateLayerState0 = -3;
constexpr int kDynamicAuxCandidateLayerState1 = -4;
constexpr int kDynamicAuxCandidateLayerState0Indirect = -5;
constexpr int kDynamicAuxCandidateLayerState1Indirect = -6;
constexpr int kDynamicAuxCandidateAuxStream0 = 0;
constexpr int kDynamicAuxCandidateAuxStream1 = 1;
constexpr int kDynamicAuxCandidateAuxStream0Indirect = 2;
constexpr int kDynamicAuxCandidateAuxStream1Indirect = 3;
constexpr uint32_t kMaxDynamicFieldOffsetScan = 95u;

struct DynamicAuxStreamCandidate {
  const void* ptr = nullptr;
  std::array<uint32_t, 10> strides = {};
  bool usesPrimaryStream = false;
  int auxEntryIndex = -1;
};

template <size_t N>
std::array<uint32_t, 10> MakeDynamicAuxStrideHints(
    const std::array<uint32_t, N>& seeds) {
  std::array<uint32_t, 10> hints = {4u, 8u, 12u, 16u, 20u,
                                    24u, 28u, 32u, 0u, 0u};
  size_t cursor = 0u;
  auto pushUnique = [&](uint32_t value) {
    if (value == 0u)
      return;
    for (size_t i = 0u; i < cursor; ++i) {
      if (hints[i] == value)
        return;
    }
    if (cursor < hints.size())
      hints[cursor++] = value;
  };

  for (uint32_t value : seeds)
    pushUnique(value);

  const std::array<uint32_t, 8> defaults = {4u, 8u, 12u, 16u,
                                            20u, 24u, 28u, 32u};
  for (uint32_t value : defaults)
    pushUnique(value);
  return hints;
}

void AppendDynamicAuxCandidate(std::vector<DynamicAuxStreamCandidate>& candidates,
                               const void* ptr,
                               const std::array<uint32_t, 10>& strides,
                               bool usesPrimaryStream,
                               int auxEntryIndex) {
  if (ptr == nullptr)
    return;

  for (const auto& candidate : candidates) {
    if (candidate.ptr == ptr)
      return;
  }

  candidates.push_back(
      DynamicAuxStreamCandidate{ptr, strides, usesPrimaryStream, auxEntryIndex});
}

bool TryReadLinearWordSample(const void* ptr, size_t byteOffset,
                             uint32_t& outWord) {
  outWord = 0u;
  if (ptr == nullptr ||
      !dxvk::war3::IsReadableRange(
          reinterpret_cast<const uint8_t*>(ptr) + byteOffset, sizeof(outWord))) {
    return false;
  }

  std::memcpy(&outWord, reinterpret_cast<const uint8_t*>(ptr) + byteOffset,
              sizeof(outWord));
  return true;
}

bool TryReadPointerWordWindow(const void* ptr, uint32_t& outWord0,
                              uint32_t& outWord4, uint32_t& outWord8) {
  outWord0 = 0u;
  outWord4 = 0u;
  outWord8 = 0u;
  if (ptr == nullptr || !dxvk::war3::IsReadableRange(ptr, 12u))
    return false;

  std::memcpy(&outWord0, ptr, sizeof(outWord0));
  std::memcpy(&outWord4, reinterpret_cast<const uint8_t*>(ptr) + 4u,
              sizeof(outWord4));
  std::memcpy(&outWord8, reinterpret_cast<const uint8_t*>(ptr) + 8u,
              sizeof(outWord8));
  return true;
}

bool LooksLikeDynamicAuxPointerCandidate(uint32_t rawValue) {
  const uintptr_t rawPtr = uintptr_t(rawValue);
  return rawPtr >= 0x10000u && (rawPtr & 0x3u) == 0u;
}

bool TryReadAuxStreamHead(uintptr_t rawPtr, uint32_t& outWord0,
                          uint32_t& outWord4) {
  outWord0 = 0u;
  outWord4 = 0u;
  if (rawPtr == 0u)
    return false;

  const auto* ptr = reinterpret_cast<const void*>(rawPtr);
  if (!dxvk::war3::IsReadableRange(ptr, 8u))
    return false;

  std::memcpy(&outWord0, ptr, sizeof(outWord0));
  std::memcpy(&outWord4, reinterpret_cast<const uint8_t*>(ptr) + 4u,
              sizeof(outWord4));
  return true;
}

bool TryReadAuxStreamSample(uintptr_t rawPtr, uint32_t sampleIndex,
                            uint32_t& outWord0, uint32_t& outWord4) {
  outWord0 = 0u;
  outWord4 = 0u;
  if (rawPtr == 0u)
    return false;

  const size_t byteOffset = size_t(sampleIndex) * 8u;
  const auto* ptr =
      reinterpret_cast<const uint8_t*>(reinterpret_cast<const void*>(rawPtr));
  if (!dxvk::war3::IsReadableRange(ptr + byteOffset, 8u))
    return false;

  std::memcpy(&outWord0, ptr + byteOffset, sizeof(outWord0));
  std::memcpy(&outWord4, ptr + byteOffset + 4u, sizeof(outWord4));
  return true;
}

std::vector<DynamicAuxStreamCandidate> CollectDynamicAuxStreamCandidates(
    const ShadowRenderableRecord& renderable,
    const MeshLayerBindingContract* layerContract) {
  std::vector<DynamicAuxStreamCandidate> candidates;
  candidates.reserve(12u);

  if (renderable.meshData == nullptr ||
      ShouldSkipLegacyMeshDataDecode(renderable))
    return candidates;

  void* stream1Ptr = nullptr;
  uint32_t declaredStride = 0u;
  void* primaryStreamPtr = nullptr;
  uint32_t primaryStride = 0u;
  uint32_t primaryArg0 = 0u;
  dxvk::war3::SafeReadPtrFast(renderable.meshData,
                              dxvk::war3::MeshDataOffsets::Stream1Ptr,
                              stream1Ptr);
  dxvk::war3::SafeReadU32Fast(renderable.meshData,
                              dxvk::war3::MeshDataOffsets::Stream1Stride,
                              declaredStride);

  AppendDynamicAuxCandidate(
      candidates, stream1Ptr,
      MakeDynamicAuxStrideHints(std::array<uint32_t, 4>{
          declaredStride, 1u, 4u, 8u}),
      false, kDynamicAuxCandidateStream1);

  if (layerContract != nullptr && layerContract->prefersPrimaryStreamAuxView()) {
    dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                dxvk::war3::MeshDataOffsets::PrimaryStreamPtr,
                                primaryStreamPtr);
    dxvk::war3::SafeReadU32Fast(renderable.meshData,
                                dxvk::war3::MeshDataOffsets::PrimaryStreamStride,
                                primaryStride);
    dxvk::war3::SafeReadU32Fast(renderable.meshData,
                                dxvk::war3::MeshDataOffsets::PrimaryStreamArg0,
                                primaryArg0);
    if (primaryStreamPtr != nullptr && primaryStreamPtr != stream1Ptr) {
      AppendDynamicAuxCandidate(
          candidates, primaryStreamPtr,
          MakeDynamicAuxStrideHints(std::array<uint32_t, 4>{
              primaryStride, primaryArg0, 20u, 24u}),
          true, kDynamicAuxCandidatePrimaryStream);
    }
  }

  if (layerContract == nullptr)
    return candidates;

  auto appendDirectPointerCandidate = [&](uint32_t rawPtrValue,
                                          const std::array<uint32_t, 4>& seeds,
                                          int sourceTag) {
    if (!LooksLikeDynamicAuxPointerCandidate(rawPtrValue))
      return;
    AppendDynamicAuxCandidate(
        candidates, reinterpret_cast<const void*>(uintptr_t(rawPtrValue)),
        MakeDynamicAuxStrideHints(seeds), false, sourceTag);
  };

  auto appendIndirectPointerCandidate =
      [&](uint32_t ownerRawPtr, const std::array<uint32_t, 4>& seeds,
          int sourceTag) {
        if (!LooksLikeDynamicAuxPointerCandidate(ownerRawPtr))
          return;

        auto appendHopWindow = [&](uint32_t baseRawPtr, uint32_t rootRawPtr) {
          uint32_t word0 = 0u;
          uint32_t word4 = 0u;
          uint32_t word8 = 0u;
          if (!TryReadPointerWordWindow(
                  reinterpret_cast<const void*>(uintptr_t(baseRawPtr)), word0,
                  word4, word8)) {
            return;
          }

          const std::array<uint32_t, 3> hopWords = {word0, word4, word8};
          for (uint32_t hopWord : hopWords) {
            if (!LooksLikeDynamicAuxPointerCandidate(hopWord) ||
                hopWord == rootRawPtr || hopWord == baseRawPtr) {
              continue;
            }
            AppendDynamicAuxCandidate(
                candidates, reinterpret_cast<const void*>(uintptr_t(hopWord)),
                MakeDynamicAuxStrideHints(seeds), false, sourceTag);
          }
        };

        appendHopWindow(ownerRawPtr, ownerRawPtr);

        uint32_t firstHop0 = 0u;
        uint32_t firstHop4 = 0u;
        uint32_t firstHop8 = 0u;
        if (!TryReadPointerWordWindow(
                reinterpret_cast<const void*>(uintptr_t(ownerRawPtr)), firstHop0,
                firstHop4, firstHop8)) {
          return;
        }

        const std::array<uint32_t, 3> firstHopWords = {
            firstHop0, firstHop4, firstHop8};
        for (uint32_t firstHopWord : firstHopWords) {
          if (!LooksLikeDynamicAuxPointerCandidate(firstHopWord) ||
              firstHopWord == ownerRawPtr) {
            continue;
          }
          appendHopWindow(firstHopWord, ownerRawPtr);
        }
      };

  const uint32_t aux0CountHint =
      layerContract->hasAuxEntry0Snapshot ? layerContract->auxEntry0Word0 : 0u;
  const uint32_t aux1CountHint =
      layerContract->hasAuxEntry1Snapshot ? layerContract->auxEntry1Word0 : 0u;
  appendDirectPointerCandidate(layerContract->layerStateWord1C,
                               {aux0CountHint, declaredStride, primaryStride, 1u},
                               kDynamicAuxCandidateLayerState0);
  appendDirectPointerCandidate(layerContract->layerStateWord20,
                               {aux1CountHint, declaredStride, primaryStride, 1u},
                               kDynamicAuxCandidateLayerState1);
  appendIndirectPointerCandidate(layerContract->layerStateWord1C,
                                 {declaredStride, 1u, 4u, 8u},
                                 kDynamicAuxCandidateLayerState0Indirect);
  appendIndirectPointerCandidate(layerContract->layerStateWord20,
                                 {declaredStride, 1u, 4u, 8u},
                                 kDynamicAuxCandidateLayerState1Indirect);

  auto appendAuxStreamCandidate = [&](uintptr_t auxStreamPtr, uint32_t auxWord0,
                                      uint32_t auxStride, int directTag,
                                      int indirectTag) {
    if (auxStreamPtr == 0u)
      return;
    AppendDynamicAuxCandidate(
        candidates, reinterpret_cast<const void*>(auxStreamPtr),
        MakeDynamicAuxStrideHints(
            std::array<uint32_t, 4>{auxStride, auxWord0, declaredStride, 1u}),
        false, directTag);
    appendIndirectPointerCandidate(
        uint32_t(auxStreamPtr), {auxStride, declaredStride, 1u, 4u}, indirectTag);
  };
  appendAuxStreamCandidate(layerContract->auxStreamPtr0, layerContract->auxEntry0Word0,
                           layerContract->auxStreamStride0,
                           kDynamicAuxCandidateAuxStream0,
                           kDynamicAuxCandidateAuxStream0Indirect);
  appendAuxStreamCandidate(layerContract->auxStreamPtr1, layerContract->auxEntry1Word0,
                           layerContract->auxStreamStride1,
                           kDynamicAuxCandidateAuxStream1,
                           kDynamicAuxCandidateAuxStream1Indirect);

  return candidates;
}

bool TryResolveMeshLayerBindingContract(const ShadowRenderableRecord& renderable,
                                        MeshLayerBindingContract& out) {
  out = {};

  if (renderable.queueKind != render::VisibleRenderableQueueKind::MainQueue ||
      renderable.meshData == nullptr || renderable.sceneNode == nullptr) {
    return false;
  }

  uint32_t meshIndex = renderable.meshIndex;
  if (meshIndex == kInvalidShadowContractGeosetIndex &&
      !dxvk::war3::SafeReadU32Fast(renderable.meshData,
                                   dxvk::war3::MeshDataOffsets::MeshIndex,
                                   meshIndex)) {
    return false;
  }

  if (meshIndex == kInvalidShadowContractGeosetIndex || meshIndex > 4096u)
    return false;

  if (renderable.renderablePart != nullptr) {
    dxvk::war3::SafeReadU32Fast(
        renderable.renderablePart,
        dxvk::war3::RenderablePartFieldOffsets::StagePresetSpanBaseIndex,
        out.stagePresetSpanBaseIndex);
  }
  dxvk::war3::SafeReadU32Fast(
      renderable.sceneNode, dxvk::war3::SceneNodeOffsets::StagePresetBaseIndex,
      out.sceneStagePresetBaseIndex);

  void* meshInfoTable = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(renderable.sceneNode,
                                   dxvk::war3::SceneNodeOffsets::MeshInfoTable,
                                   meshInfoTable) ||
      meshInfoTable == nullptr ||
      !dxvk::war3::IsReadableRange(meshInfoTable,
                                   (size_t(meshIndex) + 1u) * sizeof(void*))) {
    return false;
  }

  void* meshInfo = nullptr;
  std::memcpy(&meshInfo,
              reinterpret_cast<const uint8_t*>(meshInfoTable) +
                  size_t(meshIndex) * sizeof(void*),
              sizeof(meshInfo));
  if (meshInfo == nullptr)
    return false;

  uint32_t layerCount = 0u;
  void* layerInfo = nullptr;
  const auto* batchLayerStatePtr =
      reinterpret_cast<const uint8_t*>(renderable.layerState);
  if (!dxvk::war3::SafeReadU32Fast(meshInfo, dxvk::war3::MeshInfoOffsets::LayerCount,
                                   layerCount) ||
      layerCount == 0u || renderable.layerIndex >= layerCount ||
      !dxvk::war3::SafeReadPtrFast(meshInfo, dxvk::war3::MeshInfoOffsets::LayerInfo,
                                   layerInfo) ||
      layerInfo == nullptr) {
    return false;
  }

  void* layerRecords = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(layerInfo, dxvk::war3::LayerInfoOffsets::LayerRecords,
                                   layerRecords) ||
      layerRecords == nullptr) {
    return false;
  }

  const uint8_t* canonicalLayerStateRecordPtr = nullptr;
  void* layerStates = nullptr;
  if (dxvk::war3::SafeReadPtrFast(meshInfo, dxvk::war3::MeshInfoOffsets::LayerStates,
                                  layerStates) &&
      layerStates != nullptr) {
    canonicalLayerStateRecordPtr =
        reinterpret_cast<const uint8_t*>(layerStates) +
        size_t(renderable.layerIndex) * 0x24u;
    if (!dxvk::war3::IsReadableRange(canonicalLayerStateRecordPtr, 0x24u))
      canonicalLayerStateRecordPtr = nullptr;
  }

  // 最新 reimpl/native 对照已经收敛：batch->layerStatePtr 更像
  // `MeshLayerStateRecord + 4` 的 state-view，而 authoritative record base
  // 应当从 meshInfo->layerStates 取。这里不再用 batch ptr / batch-4 充当
  // record base，避免把 state-view 错读成 layer contract。
  const uint8_t* layerStateRecordPtr = canonicalLayerStateRecordPtr;
  if (layerStateRecordPtr == nullptr)
    return false;

  const auto* layerStateViewPtr = layerStateRecordPtr + 4u;
  const auto* dispatchPtr =
      reinterpret_cast<const uint8_t*>(layerRecords) +
      size_t(renderable.layerIndex) * 0x2Cu;
  if (!dxvk::war3::IsReadableRange(layerStateRecordPtr, 0x24u) ||
      !dxvk::war3::IsReadableRange(dispatchPtr, 0x2Cu)) {
    return false;
  }

  out.meshIndex = meshIndex;
  out.layerIndex = renderable.layerIndex;
  out.layerCount = layerCount;
  out.layerStateRecordPtr = layerStateRecordPtr;
  out.layerStateViewPtr = layerStateViewPtr;
  out.batchLayerStateProvided = batchLayerStatePtr != nullptr;
  out.usesCanonicalLayerStateRecord = true;
  out.batchLayerStateMatchesRecordBase =
      batchLayerStatePtr != nullptr && batchLayerStatePtr == layerStateRecordPtr;
  out.batchLayerStateMatchesStateView =
      batchLayerStatePtr != nullptr && batchLayerStatePtr == layerStateViewPtr;
  dxvk::war3::SafeReadU32Fast(
      layerStateRecordPtr,
      dxvk::war3::MeshLayerStateRecordOffsets::PrimaryResourceBinding,
      out.primaryResourceBinding);
  dxvk::war3::SafeReadU32Fast(
      layerStateRecordPtr, dxvk::war3::MeshLayerStateRecordOffsets::BlendOrDrawMode,
      out.blendOrDrawMode);
  dxvk::war3::SafeReadU32Fast(
      layerStateRecordPtr, dxvk::war3::MeshLayerStateRecordOffsets::AuxRefEnable0,
      out.auxRefEnable0);
  dxvk::war3::SafeReadU32Fast(
      layerStateRecordPtr, dxvk::war3::MeshLayerStateRecordOffsets::AuxRefEnable1,
      out.auxRefEnable1);
  out.layerStateWord0 = out.primaryResourceBinding;
  out.layerStateWord18 = out.blendOrDrawMode;
  out.layerStateWord1C = out.auxRefEnable0;
  out.layerStateWord20 = out.auxRefEnable1;
  dxvk::war3::SafeReadFast(dispatchPtr,
                           dxvk::war3::MeshLayerDispatchRecordOffsets::AuxRefIndex0,
                           out.auxRefIndex0);
  dxvk::war3::SafeReadFast(dispatchPtr,
                           dxvk::war3::MeshLayerDispatchRecordOffsets::AuxRefIndex1,
                           out.auxRefIndex1);
  dxvk::war3::SafeReadU32Fast(
      dispatchPtr, dxvk::war3::MeshLayerDispatchRecordOffsets::StagePresetIndex0,
      out.stagePresetIndex0);
  dxvk::war3::SafeReadU32Fast(
      dispatchPtr, dxvk::war3::MeshLayerDispatchRecordOffsets::StagePresetIndex1,
      out.stagePresetIndex1);
  if (out.stagePresetIndex0 < 0x80000000u) {
    out.resolvedStagePresetIndex0 = out.sceneStagePresetBaseIndex +
                                    out.stagePresetSpanBaseIndex +
                                    out.stagePresetIndex0;
  }
  if (out.stagePresetIndex1 < 0x80000000u) {
    out.resolvedStagePresetIndex1 = out.sceneStagePresetBaseIndex +
                                    out.stagePresetSpanBaseIndex +
                                    out.stagePresetIndex1;
  }
  dxvk::war3::SafeReadU32Fast(dispatchPtr,
                              dxvk::war3::MeshLayerDispatchRecordOffsets::StageMode0,
                              out.stageMode0);
  dxvk::war3::SafeReadU32Fast(dispatchPtr,
                              dxvk::war3::MeshLayerDispatchRecordOffsets::StageMode1,
                              out.stageMode1);

  void* auxTable = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                   dxvk::war3::MeshDataOffsets::AuxLayerResourceTable,
                                   auxTable) ||
      auxTable == nullptr) {
    return true;
  }

  auto readAuxEntry = [&](int32_t auxIndex, uintptr_t& outStreamPtr,
                          bool& outHasStream, uint32_t& outWord0,
                            uint32_t& outWord8, bool& outHasSnapshot) {
    outStreamPtr = 0u;
    outHasStream = false;
    outWord0 = 0u;
    outWord8 = 0u;
    outHasSnapshot = false;
    if (auxIndex < 0 || auxIndex > 2048)
      return;

    const auto* entryPtr =
        reinterpret_cast<const uint8_t*>(auxTable) + size_t(auxIndex) * 0x2Cu;
    if (!dxvk::war3::IsReadableRange(entryPtr, 0x2Cu))
      return;

    outHasSnapshot = dxvk::war3::SafeReadU32Fast(entryPtr, 0x00u, outWord0);
    dxvk::war3::SafeReadU32Fast(entryPtr,
                                dxvk::war3::MeshAuxResourceEntryOffsets::ResourceBinding,
                                outWord8);
    if (LooksLikeDynamicAuxPointerCandidate(outWord8)) {
      outStreamPtr = uintptr_t(outWord8);
      outHasStream = true;
    }
  };

  if (out.auxRefEnable0 != 0u)
    readAuxEntry(out.auxRefIndex0, out.auxStreamPtr0, out.hasAuxStream0,
                 out.auxEntry0Word0, out.auxEntry0Word8,
                 out.hasAuxEntry0Snapshot);
  if (out.auxRefEnable1 != 0u)
    readAuxEntry(out.auxRefIndex1, out.auxStreamPtr1, out.hasAuxStream1,
                 out.auxEntry1Word0, out.auxEntry1Word8,
                 out.hasAuxEntry1Snapshot);
  return true;
}

std::vector<void*> CollectRuntimeModelTree(
    void* rootRuntimeModelPtr,
    size_t maxRuntimeModels = 256u,
    size_t maxLinkNodes = 1024u) {
  if (rootRuntimeModelPtr == nullptr)
    return {};

  std::vector<void*> out;
  out.reserve(32u);
  std::vector<void*> pending;
  pending.reserve(32u);
  pending.push_back(rootRuntimeModelPtr);

  std::unordered_set<void*> visitedRuntimeModels;
  visitedRuntimeModels.reserve(64u);
  std::unordered_set<void*> visitedLinkNodes;
  visitedLinkNodes.reserve(128u);

  size_t cursor = 0u;
  while (cursor < pending.size() && pending.size() <= maxRuntimeModels) {
    void* currentRuntimeModel = pending[cursor++];
    if (currentRuntimeModel == nullptr ||
        !visitedRuntimeModels.insert(currentRuntimeModel).second) {
      continue;
    }
    out.push_back(currentRuntimeModel);

    uint32_t childGroupCount = 0u;
    void* childGroupArray = nullptr;
    if (!dxvk::war3::SafeReadU32Fast(
            currentRuntimeModel, dxvk::war3::CModelOffsets::ChildBucketCount,
            childGroupCount) ||
        !dxvk::war3::SafeReadPtrFast(
            currentRuntimeModel, dxvk::war3::CModelOffsets::ChildBucketArray,
            childGroupArray) ||
        childGroupCount == 0u || childGroupArray == nullptr ||
        !dxvk::war3::IsReadableRange(childGroupArray, size_t(childGroupCount) * 12u)) {
      continue;
    }

    const auto* childGroups =
        reinterpret_cast<const uint8_t*>(childGroupArray);
    for (uint32_t i = 0u; i < childGroupCount; ++i) {
      void* linkNode = nullptr;
      dxvk::war3::SafeReadPtrFast(childGroups + size_t(i) * 12u, 8u, linkNode);
      size_t traversed = 0u;
      while (linkNode != nullptr && traversed < maxLinkNodes &&
             visitedLinkNodes.insert(linkNode).second) {
        ++traversed;
        void* childRuntimeModel = nullptr;
        void* nextLinkNode = nullptr;
        dxvk::war3::SafeReadPtrFast(linkNode, 8u, childRuntimeModel);
        dxvk::war3::SafeReadPtrFast(linkNode, 4u, nextLinkNode);
        if (childRuntimeModel != nullptr && pending.size() < maxRuntimeModels)
          pending.push_back(childRuntimeModel);
        linkNode = nextLinkNode;
      }
    }
  }

  return out;
}

bool TryResolveChildRuntimeModelByModelResource(void* rootRuntimeModelPtr,
                                                void* targetModelResourcePtr,
                                                void*& outRuntimeModelPtr,
                                                size_t maxRuntimeModels = 256u,
                                                size_t maxLinkNodes = 1024u) {
  outRuntimeModelPtr = nullptr;
  if (rootRuntimeModelPtr == nullptr || targetModelResourcePtr == nullptr)
    return false;

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  targetModelResourcePtr =
      resourceCache.resolveDirectModelResourcePtr(targetModelResourcePtr);
  if (targetModelResourcePtr == nullptr)
    return false;

  const auto runtimeTree =
      CollectRuntimeModelTree(rootRuntimeModelPtr, maxRuntimeModels, maxLinkNodes);
  for (void* currentRuntimeModel : runtimeTree) {
    void* ownedModelResourcePtr =
        TryResolveDirectModelResourceFromRuntimeModel(currentRuntimeModel);
    if (ownedModelResourcePtr != nullptr &&
        ownedModelResourcePtr == targetModelResourcePtr) {
      outRuntimeModelPtr = currentRuntimeModel;
      break;
    }
  }
  return outRuntimeModelPtr != nullptr;
}

bool TryResolveMeshPoseContext(const ShadowRenderableRecord& renderable,
                               const ShadowPoseStore& poses,
                               void*& outRuntimeModelPtr,
                               ShadowPoseRecord& outPose) {
  outRuntimeModelPtr = nullptr;
  outPose = {};

  if (renderable.meshData == nullptr) {
    return false;
  }
  if (renderable.meshData == renderable.runtimeGeosetPtr ||
      renderable.meshData == renderable.runtimeGeosetDataPtr ||
      LooksLikeGeosetDataPtr(renderable.meshData)) {
    return false;
  }

  if (!dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                   dxvk::war3::MeshDataOffsets::TransformOrPoseCtx,
                                   outRuntimeModelPtr) ||
      outRuntimeModelPtr == nullptr) {
    return false;
  }

  // PoseStore is the authoritative published contract here. Avoid re-probing
  // the candidate CModel shape on every renderable; that structure check does
  // several SafeRead/IsReadableRange calls and was showing up as the low-pressure
  // consumer hot path.
  if (!TryResolvePoseByRuntimeModelSnapshotOnly(poses, outRuntimeModelPtr,
                                                outPose)) {
    outRuntimeModelPtr = nullptr;
    outPose = {};
    return false;
  }

  return true;
}

bool TryAugmentRenderableSemanticRecovery(ShadowRenderableRecord& renderable) {
  const bool missingIdentityContext =
      renderable.worldObjectEntry == nullptr ||
      renderable.sceneNode == nullptr ||
      renderable.unitPtr == nullptr;
  const bool needsAugment =
      missingIdentityContext ||
      renderable.runtimeModelPtr == nullptr ||
      renderable.modelResourcePtr == nullptr || renderable.modelKey == 0u;
  if (!needsAugment)
    return false;

  const render::RenderObjectInfo* currentObj = nullptr;
  auto& renderRegistry = render::RenderObjectRegistry::instance();
  if (renderable.sceneNode != nullptr)
    currentObj = renderRegistry.findBySceneNode(renderable.sceneNode);
  if (currentObj == nullptr && renderable.worldObjectEntry != nullptr)
    currentObj = renderRegistry.findByEntry(renderable.worldObjectEntry);
  if (currentObj == nullptr && renderable.jHandle != 0u)
    currentObj = renderRegistry.findByHandle(renderable.jHandle);

  render::RenderObjectInfo hintedObject = {};
  if (currentObj == nullptr && renderable.unitPtr != nullptr) {
    hintedObject.worldObjectEntry = renderable.worldObjectEntry;
    hintedObject.sceneNode = renderable.sceneNode;
    hintedObject.unitPtr = renderable.unitPtr;
    hintedObject.jHandle = renderable.jHandle;
    hintedObject.rawcode = renderable.rawcode;
    hintedObject.kind = renderable.objectKind;
    currentObj = &hintedObject;
  }

  dxvk::War3ShadowSemanticContext semantic = {};
  semantic.renderablePart = renderable.renderablePart;
  semantic.sceneNode = renderable.sceneNode;
  semantic.worldObjectEntry = renderable.worldObjectEntry;
  semantic.runtimeModelPtr = renderable.runtimeModelPtr;
  semantic.modelResourcePtr = renderable.modelResourcePtr;
  semantic.object = currentObj;
  semantic.jHandle = renderable.jHandle;
  semantic.rawcode = renderable.rawcode;
  semantic.modelKey = renderable.modelKey;
  semantic.objectKind = renderable.objectKind;

  const bool bridgeReady =
      render::AugmentShadowSemanticContext(semantic, currentObj);
  bool changed = false;
  model::ModelInstanceRecord worldInstance = {};
  bool worldInstanceHit = false;
  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  void* spritePtr = TryResolveSpritePtrFromUnit(renderable.unitPtr);
  if (renderable.worldObjectEntry != nullptr)
    worldInstanceHit = instanceRegistry.findByWorldObjectEntry(
        renderable.worldObjectEntry, worldInstance);
  if (!worldInstanceHit && renderable.runtimeModelPtr != nullptr)
    worldInstanceHit =
        instanceRegistry.findByRuntimeModel(renderable.runtimeModelPtr,
                                            worldInstance);
  if (!worldInstanceHit && semantic.runtimeModelPtr != nullptr)
    worldInstanceHit =
        instanceRegistry.findByRuntimeModel(semantic.runtimeModelPtr,
                                            worldInstance);
  if (!worldInstanceHit && renderable.sceneNode != nullptr)
    worldInstanceHit =
        instanceRegistry.findBySceneNode(renderable.sceneNode, worldInstance);
  if (!worldInstanceHit && renderable.jHandle != 0u)
    worldInstanceHit =
        instanceRegistry.findByHandle(renderable.jHandle, worldInstance);
  if (!worldInstanceHit && spritePtr != nullptr)
    worldInstanceHit =
        instanceRegistry.findBySpritePtr(spritePtr, worldInstance);
  if (!worldInstanceHit && semantic.object != nullptr &&
      semantic.object->unitPtr != nullptr) {
    spritePtr = spritePtr != nullptr
                    ? spritePtr
                    : TryResolveSpritePtrFromUnit(semantic.object->unitPtr);
    if (spritePtr != nullptr)
      worldInstanceHit =
          instanceRegistry.findBySpritePtr(spritePtr, worldInstance);
  }

  if (renderable.sceneNode == nullptr && semantic.sceneNode != nullptr) {
    renderable.sceneNode = semantic.sceneNode;
    changed = true;
  }
  if (renderable.worldObjectEntry == nullptr &&
      semantic.worldObjectEntry != nullptr) {
    renderable.worldObjectEntry = semantic.worldObjectEntry;
    changed = true;
  }
  if (renderable.unitPtr == nullptr && semantic.object != nullptr &&
      semantic.object->unitPtr != nullptr) {
    renderable.unitPtr = semantic.object->unitPtr;
    changed = true;
  }
  if (renderable.unitPtr == nullptr &&
      worldInstanceHit && worldInstance.unitPtr != nullptr) {
    renderable.unitPtr = worldInstance.unitPtr;
    changed = true;
  }
  if (spritePtr == nullptr && worldInstanceHit && worldInstance.spritePtr != nullptr)
    spritePtr = worldInstance.spritePtr;
  if (renderable.runtimeModelPtr == nullptr &&
      semantic.runtimeModelPtr != nullptr) {
    renderable.runtimeModelPtr = semantic.runtimeModelPtr;
    changed = true;
  }
  if (renderable.runtimeModelPtr == nullptr &&
      worldInstanceHit && worldInstance.runtimeModelPtr != nullptr) {
    renderable.runtimeModelPtr = worldInstance.runtimeModelPtr;
    changed = true;
  }
  if (renderable.runtimeModelPtr == nullptr && spritePtr != nullptr) {
    void* runtimeModelPtr = TryReadRuntimeModelFromSprite(spritePtr);
    if (runtimeModelPtr != nullptr) {
      renderable.runtimeModelPtr = runtimeModelPtr;
      changed = true;
    }
  }
  if (spritePtr != nullptr &&
      (renderable.modelResourcePtr == nullptr || renderable.modelKey == 0u)) {
    model::ModelResourceRecord modelRecord = {};
    auto& modelRegistry = model::ModelRegistry::instance();
    if (modelRegistry.findBySprite(spritePtr, modelRecord) ||
        (renderable.runtimeModelPtr != nullptr &&
         modelRegistry.findByRuntimeModel(renderable.runtimeModelPtr,
                                          modelRecord))) {
      if (renderable.modelResourcePtr == nullptr &&
          modelRecord.modelResourcePtr != nullptr) {
        renderable.modelResourcePtr = modelRecord.modelResourcePtr;
        changed = true;
      }
      if (renderable.modelKey == 0u && modelRecord.modelKey != 0u) {
        renderable.modelKey = modelRecord.modelKey;
        changed = true;
      }
    }
  }
  if (renderable.modelResourcePtr == nullptr &&
      semantic.modelResourcePtr != nullptr) {
    renderable.modelResourcePtr = semantic.modelResourcePtr;
    changed = true;
  }
  if (renderable.modelResourcePtr == nullptr &&
      worldInstanceHit && worldInstance.modelResourcePtr != nullptr) {
    renderable.modelResourcePtr = worldInstance.modelResourcePtr;
    changed = true;
  }
  if (renderable.modelKey == 0u && semantic.modelKey != 0u) {
    renderable.modelKey = semantic.modelKey;
    changed = true;
  }
  if (renderable.modelKey == 0u &&
      worldInstanceHit && worldInstance.modelKey != 0u) {
    renderable.modelKey = worldInstance.modelKey;
    changed = true;
  }

  model::ShadowModelResourceRecord runtimeOwnerResource = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  const bool needsRuntimeOwnerResource =
      (renderable.runtimeModelPtr == nullptr ||
       renderable.modelResourcePtr == nullptr || renderable.modelKey == 0u) &&
      (renderable.runtimeGeosetPtr != nullptr ||
       renderable.runtimeGeosetDataPtr != nullptr);
  if (needsRuntimeOwnerResource &&
      resourceCache.findRuntimeModelOwner(
          renderable.runtimeGeosetPtr, renderable.runtimeGeosetDataPtr,
          renderable.geosetIndex, renderable.modelResourcePtr,
          runtimeOwnerResource) &&
      runtimeOwnerResource.runtimeModelPtr != nullptr) {
    if (renderable.runtimeModelPtr == nullptr) {
      renderable.runtimeModelPtr = runtimeOwnerResource.runtimeModelPtr;
      changed = true;
    }
    if (renderable.modelResourcePtr == nullptr &&
        runtimeOwnerResource.modelResourcePtr != nullptr) {
      renderable.modelResourcePtr = runtimeOwnerResource.modelResourcePtr;
      changed = true;
    }
    if (renderable.modelKey == 0u && runtimeOwnerResource.modelKey != 0u) {
      renderable.modelKey = runtimeOwnerResource.modelKey;
      changed = true;
    }

    model::ModelInstanceRecord runtimeOwnerInstance = {};
    if (instanceRegistry.findByRuntimeModel(runtimeOwnerResource.runtimeModelPtr,
                                            runtimeOwnerInstance) ||
        instanceRegistry.findOwnerByRuntimeModel(
            runtimeOwnerResource.runtimeModelPtr, runtimeOwnerInstance)) {
      if (spritePtr == nullptr && runtimeOwnerInstance.spritePtr != nullptr)
        spritePtr = runtimeOwnerInstance.spritePtr;
      if (renderable.worldObjectEntry == nullptr &&
          runtimeOwnerInstance.worldObjectEntry != nullptr) {
        renderable.worldObjectEntry = runtimeOwnerInstance.worldObjectEntry;
        changed = true;
      }
      if (renderable.sceneNode == nullptr &&
          runtimeOwnerInstance.sceneNode != nullptr) {
        renderable.sceneNode = runtimeOwnerInstance.sceneNode;
        changed = true;
      }
      if (renderable.unitPtr == nullptr &&
          runtimeOwnerInstance.unitPtr != nullptr) {
        renderable.unitPtr = runtimeOwnerInstance.unitPtr;
        changed = true;
      }
      if (renderable.jHandle == 0u && runtimeOwnerInstance.jHandle != 0u) {
        renderable.jHandle = runtimeOwnerInstance.jHandle;
        changed = true;
      }
      if (renderable.rawcode == 0u && runtimeOwnerInstance.rawcode != 0u) {
        renderable.rawcode = runtimeOwnerInstance.rawcode;
        changed = true;
      }
    }
  }

  if (renderable.jHandle == 0u && semantic.jHandle != 0u) {
    renderable.jHandle = semantic.jHandle;
    changed = true;
  }
  if (renderable.jHandle == 0u &&
      worldInstanceHit && worldInstance.jHandle != 0u) {
    renderable.jHandle = worldInstance.jHandle;
    changed = true;
  }
  if (renderable.rawcode == 0u && semantic.rawcode != 0u) {
    renderable.rawcode = semantic.rawcode;
    changed = true;
  }
  if (renderable.rawcode == 0u &&
      worldInstanceHit && worldInstance.rawcode != 0u) {
    renderable.rawcode = worldInstance.rawcode;
    changed = true;
  }
  if (renderable.objectKind == render::ObjectKind::Unknown &&
      semantic.objectKind != render::ObjectKind::Unknown) {
    renderable.objectKind = semantic.objectKind;
    changed = true;
  }

  render::ShadowObjectRecord shadowRecord = {};
  bool shadowRecordHit = false;
  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  const bool needsStructuralShadowRecord =
      renderable.worldObjectEntry == nullptr ||
      renderable.sceneNode == nullptr || renderable.unitPtr == nullptr ||
      renderable.runtimeModelPtr == nullptr ||
      renderable.modelResourcePtr == nullptr || renderable.modelKey == 0u;
  if (needsStructuralShadowRecord) {
    if (!shadowRecordHit && renderable.worldObjectEntry != nullptr)
      shadowRecordHit =
          shadowRegistry.findByWorldObjectEntry(renderable.worldObjectEntry,
                                                shadowRecord);
    if (!shadowRecordHit && renderable.sceneNode != nullptr)
      shadowRecordHit =
          shadowRegistry.findBySceneNode(renderable.sceneNode, shadowRecord);
    if (!shadowRecordHit && renderable.unitPtr != nullptr)
      shadowRecordHit =
          shadowRegistry.findByUnitPtr(renderable.unitPtr, shadowRecord);
    if (!shadowRecordHit && renderable.jHandle != 0u)
      shadowRecordHit =
          shadowRegistry.findByHandle(renderable.jHandle, shadowRecord);
    if (!shadowRecordHit && renderable.runtimeModelPtr != nullptr)
      shadowRecordHit =
          shadowRegistry.findByRuntimeModel(renderable.runtimeModelPtr, shadowRecord);
    if (!shadowRecordHit && spritePtr != nullptr)
      shadowRecordHit = shadowRegistry.findBySpritePtr(spritePtr, shadowRecord);
  }

  if (shadowRecordHit) {
    if (spritePtr == nullptr && shadowRecord.spritePtr != nullptr)
      spritePtr = shadowRecord.spritePtr;
    if (renderable.worldObjectEntry == nullptr &&
        shadowRecord.worldObjectEntry != nullptr) {
      renderable.worldObjectEntry = shadowRecord.worldObjectEntry;
      changed = true;
    }
    if (renderable.sceneNode == nullptr && shadowRecord.sceneNode != nullptr) {
      renderable.sceneNode = shadowRecord.sceneNode;
      changed = true;
    }
    if (renderable.unitPtr == nullptr && shadowRecord.unitPtr != nullptr) {
      renderable.unitPtr = shadowRecord.unitPtr;
      changed = true;
    }
    if (renderable.runtimeModelPtr == nullptr &&
        shadowRecord.runtimeModelPtr != nullptr) {
      renderable.runtimeModelPtr = shadowRecord.runtimeModelPtr;
      changed = true;
    }
    if (renderable.modelResourcePtr == nullptr &&
        shadowRecord.modelResourcePtr != nullptr) {
      renderable.modelResourcePtr = shadowRecord.modelResourcePtr;
      changed = true;
    }
    if (renderable.modelKey == 0u && shadowRecord.modelKey != 0u) {
      renderable.modelKey = shadowRecord.modelKey;
      changed = true;
    }
    if (renderable.jHandle == 0u && shadowRecord.jHandle != 0u) {
      renderable.jHandle = shadowRecord.jHandle;
      changed = true;
    }
    if (renderable.rawcode == 0u && shadowRecord.rawcode != 0u) {
      renderable.rawcode = shadowRecord.rawcode;
      changed = true;
    }
    if (renderable.objectKind == render::ObjectKind::Unknown &&
        shadowRecord.kind != render::ObjectKind::Unknown) {
      renderable.objectKind = shadowRecord.kind;
      changed = true;
    }
  }

  return bridgeReady || changed;
}

uint32_t GetUsablePoseMatrixCount(const ShadowPoseRecord& pose) {
  if (pose.matrixCount == 0u || pose.matrixPalette.empty())
    return 0u;
  return (std::min)(pose.matrixCount, uint32_t(pose.matrixPalette.size()));
}

bool ShouldPreferPoseCandidate(const ShadowPoseRecord& candidate,
                               const ShadowPoseRecord& current) {
  const uint32_t candidateCount = GetUsablePoseMatrixCount(candidate);
  const uint32_t currentCount = GetUsablePoseMatrixCount(current);
  if (candidateCount == 0u)
    return false;
  if (currentCount == 0u)
    return true;
  if (candidateCount != currentCount)
    return candidateCount > currentCount;
  if (current.runtimeModelPtr == nullptr && candidate.runtimeModelPtr != nullptr)
    return true;
  if (!current.hasWorldTransform && candidate.hasWorldTransform)
    return true;
  return false;
}

bool IsCompletePoseCandidate(const ShadowPoseRecord& pose) {
  return GetUsablePoseMatrixCount(pose) != 0u && pose.hasWorldTransform;
}

enum class PoseResolveMissReason : uint32_t {
  None = 0,
  NoContext,
  AnonymousSubpart,
  LookupMiss,
};

enum class RuntimeGroupPaletteMissReason : uint32_t {
  None = 0,
  NoSkinningData,
  NoPosePalette,
  NoVertexGroups,
  InvalidGroupTable,
  MatrixIndexOutOfRange,
  VertexGroupOutOfRange,
  FallbacksFailed,
};

struct RuntimeGroupPaletteMissDetail {
  RuntimeGroupPaletteMissReason reason = RuntimeGroupPaletteMissReason::None;
  uint32_t group = UINT32_MAX;
  uint32_t matrixIndex = UINT32_MAX;
  uint32_t poseCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t maxVertexGroupSlot = 0u;
  uint32_t matrixIndexCount = 0u;
};

void NoteRuntimeGroupPaletteMiss(RuntimeGroupPaletteMissDetail* outDetail,
                                 RuntimeGroupPaletteMissReason reason,
                                 uint32_t poseCount,
                                 uint32_t groupCount,
                                 uint32_t maxVertexGroupSlot,
                                 uint32_t matrixIndexCount,
                                 uint32_t group = UINT32_MAX,
                                 uint32_t matrixIndex = UINT32_MAX) {
  if (outDetail == nullptr)
    return;
  outDetail->reason = reason;
  outDetail->group = group;
  outDetail->matrixIndex = matrixIndex;
  outDetail->poseCount = poseCount;
  outDetail->groupCount = groupCount;
  outDetail->maxVertexGroupSlot = maxVertexGroupSlot;
  outDetail->matrixIndexCount = matrixIndexCount;
}

void AccumulateRuntimeGroupPaletteMissStats(
    ShadowResolveStats& stats,
    const RuntimeGroupPaletteMissDetail& detail) {
  switch (detail.reason) {
  case RuntimeGroupPaletteMissReason::NoSkinningData:
    stats.runtimeGroupPaletteMissNoSkinningData++;
    break;
  case RuntimeGroupPaletteMissReason::NoPosePalette:
    stats.runtimeGroupPaletteMissNoPosePalette++;
    break;
  case RuntimeGroupPaletteMissReason::NoVertexGroups:
    stats.runtimeGroupPaletteMissNoVertexGroups++;
    break;
  case RuntimeGroupPaletteMissReason::InvalidGroupTable:
    stats.runtimeGroupPaletteMissInvalidGroupTable++;
    break;
  case RuntimeGroupPaletteMissReason::MatrixIndexOutOfRange:
    stats.runtimeGroupPaletteMissMatrixIndexOutOfRange++;
    break;
  case RuntimeGroupPaletteMissReason::VertexGroupOutOfRange:
    stats.runtimeGroupPaletteMissVertexGroupOutOfRange++;
    break;
  case RuntimeGroupPaletteMissReason::FallbacksFailed:
    stats.runtimeGroupPaletteMissFallbacksFailed++;
    break;
  case RuntimeGroupPaletteMissReason::None:
  default:
    break;
  }

  stats.runtimeGroupPaletteMissLastPoseCount = detail.poseCount;
  stats.runtimeGroupPaletteMissLastGroupCount = detail.groupCount;
  stats.runtimeGroupPaletteMissLastMaxVertexGroupSlot = detail.maxVertexGroupSlot;
  stats.runtimeGroupPaletteMissLastMatrixIndexCount = detail.matrixIndexCount;
  stats.runtimeGroupPaletteMissLastMatrixIndex =
      detail.matrixIndex != UINT32_MAX ? detail.matrixIndex : 0u;
}

struct ScopedMaxUs {
  uint64_t* targetUs = nullptr;
  std::chrono::steady_clock::time_point startedAt =
      std::chrono::steady_clock::now();

  explicit ScopedMaxUs(uint64_t* target) : targetUs(target) {}

  ~ScopedMaxUs() {
    if (targetUs == nullptr)
      return;
    const uint64_t elapsedUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt)
            .count());
    *targetUs = (std::max)(*targetUs, elapsedUs);
  }
};

bool TryResolveBestPoseForRenderable(const ShadowRenderableRecord& renderable,
                                     const ShadowPoseStore& poses,
                                     ShadowPoseRecord& outPose,
                                     PoseResolveMissReason* outMissReason =
                                         nullptr,
                                     ShadowResolveStats* ioStats = nullptr) {
  outPose = {};
  if (outMissReason != nullptr)
    *outMissReason = PoseResolveMissReason::None;
  bool hit = false;

  auto consider = [&](const ShadowPoseRecord& candidate) {
    if (!ShouldPreferPoseCandidate(candidate, outPose))
      return;
    outPose = candidate;
    hit = true;
  };

  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseDirectLookupUs
                          : nullptr};
    ShadowPoseRecord candidate = {};
    if (renderable.runtimeModelPtr != nullptr &&
        TryResolvePoseByRuntimeModelSnapshotOnly(
            poses, renderable.runtimeModelPtr, candidate)) {
      consider(candidate);
    }

    if (renderable.unitPtr != nullptr) {
      if (const auto* unitPose = poses.findByUnitPtrPtr(renderable.unitPtr))
        consider(*unitPose);
    }

    if (renderable.sceneNode != nullptr) {
      if (const auto* scenePose = poses.findBySceneNodePtr(renderable.sceneNode))
        consider(*scenePose);
    }
  }
  if (hit && IsCompletePoseCandidate(outPose))
    return true;

  if (dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled()) {
    // Scene-submission builds run on the frame hot path.  If the contract
    // pose store cannot answer by direct runtime/unit/scene keys, do not fall
    // through into owner/root registry scans here; those scans are diagnostic
    // rescue paths and can cost hundreds of milliseconds on low-pressure maps.
    if (hit)
      return true;
    if (outMissReason != nullptr) {
      const bool hasContext =
          renderable.runtimeModelPtr != nullptr || renderable.unitPtr != nullptr ||
          renderable.sceneNode != nullptr || renderable.worldObjectEntry != nullptr ||
          renderable.jHandle != 0u || renderable.rawcode != 0u;
      *outMissReason = hasContext ? PoseResolveMissReason::LookupMiss
                                  : PoseResolveMissReason::NoContext;
    }
    return false;
  }

  ShadowPoseRecord candidate = {};
  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseOwnerLookupUs
                          : nullptr};
    if (!hit &&
        (renderable.runtimeGeosetPtr != nullptr ||
         renderable.runtimeGeosetDataPtr != nullptr)) {
      model::ShadowModelResourceRecord runtimeOwner = {};
      auto& resourceCache = model::ShadowModelResourceCache::instance();
      if (resourceCache.findRuntimeModelOwner(renderable.runtimeGeosetPtr,
                                              renderable.runtimeGeosetDataPtr,
                                              renderable.geosetIndex,
                                              renderable.modelResourcePtr,
                                              runtimeOwner) &&
          runtimeOwner.runtimeModelPtr != nullptr &&
          TryResolvePoseByRuntimeModelSnapshotOnly(
              poses, runtimeOwner.runtimeModelPtr, candidate)) {
        consider(candidate);
      }
    }
  }
  if (hit && IsCompletePoseCandidate(outPose))
    return true;

  void* spritePtr = nullptr;
  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseSpriteLookupUs
                          : nullptr};
    spritePtr = TryResolveSpritePtrFromUnit(renderable.unitPtr);
    if (candidate.matrixCount == 0u && spritePtr != nullptr) {
      void* spriteRuntimeModelPtr = TryReadRuntimeModelFromSprite(spritePtr);
      if (spriteRuntimeModelPtr != nullptr &&
          TryResolvePoseByRuntimeModelSnapshotOnly(
              poses, spriteRuntimeModelPtr, candidate)) {
        consider(candidate);
      }
    }
  }
  if (hit && IsCompletePoseCandidate(outPose))
    return true;

  model::ModelInstanceRecord instanceRecord = {};
  bool instanceRecordHit = false;
  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseInstanceRegistryUs
                          : nullptr};
    auto& instanceRegistry = model::ModelInstanceRegistry::instance();
    if (!instanceRecordHit && renderable.worldObjectEntry != nullptr)
      instanceRecordHit = instanceRegistry.findByWorldObjectEntry(
          renderable.worldObjectEntry, instanceRecord);
    if (!instanceRecordHit && renderable.sceneNode != nullptr)
      instanceRecordHit =
          instanceRegistry.findBySceneNode(renderable.sceneNode, instanceRecord);
    if (!instanceRecordHit && renderable.unitPtr != nullptr)
      instanceRecordHit =
          instanceRegistry.findByUnitPtr(renderable.unitPtr, instanceRecord);
    if (!instanceRecordHit && renderable.jHandle != 0u)
      instanceRecordHit =
          instanceRegistry.findByHandle(renderable.jHandle, instanceRecord);
    if (!instanceRecordHit && renderable.runtimeModelPtr != nullptr)
      instanceRecordHit = instanceRegistry.findByRuntimeModel(
          renderable.runtimeModelPtr, instanceRecord);
    if (!instanceRecordHit && spritePtr != nullptr)
      instanceRecordHit =
          instanceRegistry.findBySpritePtr(spritePtr, instanceRecord);

    if (instanceRecordHit) {
      if (spritePtr == nullptr && instanceRecord.spritePtr != nullptr)
        spritePtr = instanceRecord.spritePtr;
      candidate = {};
      if (instanceRecord.runtimeModelPtr != nullptr &&
          TryResolvePoseByRuntimeModelSnapshotOnly(
              poses, instanceRecord.runtimeModelPtr, candidate)) {
        consider(candidate);
      }
      candidate = {};
      if (instanceRecord.unitPtr != nullptr &&
          poses.findByUnitPtr(instanceRecord.unitPtr, candidate)) {
        consider(candidate);
      }
      candidate = {};
      if (instanceRecord.sceneNode != nullptr &&
          poses.findBySceneNode(instanceRecord.sceneNode, candidate)) {
        consider(candidate);
      }
    }
  }
  if (hit && IsCompletePoseCandidate(outPose))
    return true;

  render::ShadowObjectRecord shadowRecord = {};
  bool shadowRecordHit = false;
  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseShadowRegistryUs
                          : nullptr};
    auto& shadowRegistry = render::ShadowObjectRegistry::instance();
    if (!shadowRecordHit && renderable.worldObjectEntry != nullptr)
      shadowRecordHit = shadowRegistry.findByWorldObjectEntry(
          renderable.worldObjectEntry, shadowRecord);
    if (!shadowRecordHit && renderable.sceneNode != nullptr)
      shadowRecordHit =
          shadowRegistry.findBySceneNode(renderable.sceneNode, shadowRecord);
    if (!shadowRecordHit && renderable.unitPtr != nullptr)
      shadowRecordHit =
          shadowRegistry.findByUnitPtr(renderable.unitPtr, shadowRecord);
    if (!shadowRecordHit && renderable.jHandle != 0u)
      shadowRecordHit =
          shadowRegistry.findByHandle(renderable.jHandle, shadowRecord);
    if (!shadowRecordHit && renderable.runtimeModelPtr != nullptr)
      shadowRecordHit = shadowRegistry.findByRuntimeModel(
          renderable.runtimeModelPtr, shadowRecord);
    if (!shadowRecordHit && spritePtr != nullptr)
      shadowRecordHit = shadowRegistry.findBySpritePtr(spritePtr, shadowRecord);

    if (shadowRecordHit) {
      if (spritePtr == nullptr && shadowRecord.spritePtr != nullptr)
        spritePtr = shadowRecord.spritePtr;
      candidate = {};
      if (shadowRecord.runtimeModelPtr != nullptr &&
          TryResolvePoseByRuntimeModelSnapshotOnly(
              poses, shadowRecord.runtimeModelPtr, candidate)) {
        consider(candidate);
      }
      candidate = {};
      if (shadowRecord.unitPtr != nullptr &&
          poses.findByUnitPtr(shadowRecord.unitPtr, candidate)) {
        consider(candidate);
      }
      candidate = {};
      if (shadowRecord.sceneNode != nullptr &&
          poses.findBySceneNode(shadowRecord.sceneNode, candidate)) {
        consider(candidate);
      }
    }
  }
  if (hit && IsCompletePoseCandidate(outPose))
    return true;

  const render::RenderObjectInfo* renderObject = nullptr;
  auto& renderRegistry = render::RenderObjectRegistry::instance();
  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseRenderRegistryUs
                          : nullptr};
    if (renderable.sceneNode != nullptr)
      renderObject = renderRegistry.findBySceneNode(renderable.sceneNode);
    if (renderObject == nullptr && renderable.worldObjectEntry != nullptr)
      renderObject = renderRegistry.findByEntry(renderable.worldObjectEntry);
    if (renderObject == nullptr && renderable.jHandle != 0u)
      renderObject = renderRegistry.findByHandle(renderable.jHandle);
    if (spritePtr == nullptr && renderObject != nullptr &&
        renderObject->unitPtr != nullptr)
      spritePtr = TryResolveSpritePtrFromUnit(renderObject->unitPtr);
    if (renderObject != nullptr && renderObject->unitPtr != nullptr &&
        poses.findByUnitPtr(renderObject->unitPtr, candidate)) {
      consider(candidate);
    }
    candidate = {};
    if (spritePtr != nullptr) {
      void* spriteRuntimeModelPtr = TryReadRuntimeModelFromSprite(spritePtr);
      if (spriteRuntimeModelPtr != nullptr &&
          TryResolvePoseByRuntimeModelSnapshotOnly(
              poses, spriteRuntimeModelPtr, candidate)) {
        consider(candidate);
      }
    }
  }
  if (hit && IsCompletePoseCandidate(outPose))
    return true;

  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseRuntimeRootsUs
                          : nullptr};
    std::vector<void*> runtimeRoots;
    CollectRenderableRuntimeModelRoots(renderable, runtimeRoots);
    for (void* runtimeRoot : runtimeRoots) {
      candidate = {};
      if (runtimeRoot != nullptr &&
          TryResolvePoseByRuntimeModelSnapshotOnly(poses, runtimeRoot,
                                                   candidate)) {
        consider(candidate);
      }
    }
  }

  void* meshPoseRuntimeModelPtr = nullptr;
  ShadowPoseRecord meshPose = {};
  {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseMeshPoseContextUs
                          : nullptr};
    if (TryResolveMeshPoseContext(renderable, poses, meshPoseRuntimeModelPtr,
                                  meshPose) &&
        meshPoseRuntimeModelPtr != nullptr) {
      consider(meshPose);
    }
  }

  if (!hit) {
    ScopedMaxUs timer{ioStats != nullptr
                          ? &ioStats->slowestPoseMissDiagnosticUs
                          : nullptr};
    static std::atomic<uint32_t> s_noPoseDetailLogCount{0u};
    const uint32_t logIndex =
        s_noPoseDetailLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (SemanticCoreTraceEnabled() &&
        (logIndex < 24u || (logIndex % 512u) == 0u)) {
      const render::RenderObjectInfo* detailedObject = renderObject;
      if (detailedObject == nullptr && renderable.sceneNode != nullptr)
        detailedObject = renderRegistry.findBySceneNode(renderable.sceneNode);
      if (detailedObject == nullptr && renderable.worldObjectEntry != nullptr)
        detailedObject = renderRegistry.findByEntry(renderable.worldObjectEntry);
      if (detailedObject == nullptr && renderable.jHandle != 0u)
        detailedObject = renderRegistry.findByHandle(renderable.jHandle);

      model::ModelInstanceRecord instanceByScene = {};
      model::ModelInstanceRecord instanceByHandle = {};
      model::ModelInstanceRecord instanceByUnit = {};
      model::ModelInstanceRecord instanceBySprite = {};
      const bool instanceSceneHit =
          renderable.sceneNode != nullptr &&
          model::ModelInstanceRegistry::instance().findBySceneNode(
              renderable.sceneNode, instanceByScene);
      const bool instanceHandleHit =
          renderable.jHandle != 0u &&
          model::ModelInstanceRegistry::instance().findByHandle(
              renderable.jHandle, instanceByHandle);
      const bool instanceUnitHit =
          detailedObject != nullptr && detailedObject->unitPtr != nullptr &&
          model::ModelInstanceRegistry::instance().findByUnitPtr(
              detailedObject->unitPtr, instanceByUnit);
      const bool instanceSpriteHit =
          spritePtr != nullptr &&
          model::ModelInstanceRegistry::instance().findBySpritePtr(
              spritePtr, instanceBySprite);

      model::PoseRecord livePoseScene = {};
      model::PoseRecord livePoseUnit = {};
      const bool livePoseSceneHit =
          renderable.sceneNode != nullptr &&
          model::PoseRegistry::instance().findBySceneNode(renderable.sceneNode,
                                                         livePoseScene);
      const bool livePoseUnitHit =
          detailedObject != nullptr && detailedObject->unitPtr != nullptr &&
          model::PoseRegistry::instance().findByUnitPtr(detailedObject->unitPtr,
                                                        livePoseUnit);

      ShadowPoseRecord shadowPoseScene = {};
      ShadowPoseRecord shadowPoseUnit = {};
      const bool shadowPoseSceneHit =
          renderable.sceneNode != nullptr &&
          poses.findBySceneNode(renderable.sceneNode, shadowPoseScene);
      const bool shadowPoseUnitHit =
          detailedObject != nullptr && detailedObject->unitPtr != nullptr &&
          poses.findByUnitPtr(detailedObject->unitPtr, shadowPoseUnit);

      const bool hasIdentityContext =
          renderable.sceneNode != nullptr ||
          renderable.worldObjectEntry != nullptr || renderable.jHandle != 0u;
      const bool hasRuntimeCandidate =
          renderable.runtimeModelPtr != nullptr ||
          (instanceSceneHit && instanceByScene.runtimeModelPtr != nullptr) ||
          (instanceHandleHit && instanceByHandle.runtimeModelPtr != nullptr) ||
          (instanceUnitHit && instanceByUnit.runtimeModelPtr != nullptr) ||
          (instanceSpriteHit && instanceBySprite.runtimeModelPtr != nullptr) ||
          (spritePtr != nullptr &&
           TryReadRuntimeModelFromSprite(spritePtr) != nullptr) ||
          meshPoseRuntimeModelPtr != nullptr;
      const bool hasUnitCandidate =
          detailedObject != nullptr && detailedObject->unitPtr != nullptr;
      if (outMissReason != nullptr) {
        if (!hasIdentityContext && !hasRuntimeCandidate && !hasUnitCandidate)
          *outMissReason = PoseResolveMissReason::NoContext;
        else if (!hasRuntimeCandidate && !hasUnitCandidate && hasIdentityContext)
          *outMissReason = PoseResolveMissReason::AnonymousSubpart;
        else
          *outMissReason = PoseResolveMissReason::LookupMiss;
      }

      if (SemanticCoreTraceEnabled()) {
        war3dbg::Print(
            "DXVK SemanticCore: no-pose detail payload=%p part=%p scene=%p "
            "world=%p handle=%u raw=%08X obj=%p unit=%p sprite=%p "
            "inst[scene=%d handle=%d unit=%d sprite=%d rt=%p/%p/%p/%p] "
            "poseLive[scene=%d unit=%d cnt=%u/%u] "
            "poseSnap[scene=%d unit=%d cnt=%u/%u] meshPose=%p\n",
            renderable.payload, renderable.renderablePart, renderable.sceneNode,
            renderable.worldObjectEntry, renderable.jHandle, renderable.rawcode,
            detailedObject,
            detailedObject != nullptr ? detailedObject->unitPtr : nullptr,
            spritePtr,
            instanceSceneHit ? 1 : 0, instanceHandleHit ? 1 : 0,
            instanceUnitHit ? 1 : 0, instanceSpriteHit ? 1 : 0,
            instanceSceneHit ? instanceByScene.runtimeModelPtr : nullptr,
            instanceHandleHit ? instanceByHandle.runtimeModelPtr : nullptr,
            instanceUnitHit ? instanceByUnit.runtimeModelPtr : nullptr,
            instanceSpriteHit ? instanceBySprite.runtimeModelPtr : nullptr,
            livePoseSceneHit ? 1 : 0, livePoseUnitHit ? 1 : 0,
            livePoseSceneHit ? livePoseScene.matrixCount : 0u,
            livePoseUnitHit ? livePoseUnit.matrixCount : 0u,
            shadowPoseSceneHit ? 1 : 0, shadowPoseUnitHit ? 1 : 0,
            shadowPoseSceneHit ? shadowPoseScene.matrixCount : 0u,
            shadowPoseUnitHit ? shadowPoseUnit.matrixCount : 0u,
            meshPoseRuntimeModelPtr);
      }
    }
  }

  return hit;
}

bool TryResolvePoseByResourceRuntimeTree(const ShadowRenderableRecord& renderable,
                                         const ShadowModelResourceRecord& resource,
                                         const ShadowPoseStore& poses,
                                         void*& outRuntimeModelPtr,
                                         ShadowPoseRecord& outPose,
                                         size_t maxRuntimeModels = 256u,
                                         size_t maxLinkNodes = 1024u) {
  outRuntimeModelPtr = nullptr;
  outPose = {};

  void* targetModelResourcePtr =
      resource.modelResourcePtr != nullptr ? resource.modelResourcePtr
                                           : renderable.modelResourcePtr;
  targetModelResourcePtr =
      model::ShadowModelResourceCache::instance().resolveDirectModelResourcePtr(
          targetModelResourcePtr);
  if (targetModelResourcePtr == nullptr)
    return false;

  std::vector<void*> runtimeRoots;
  CollectRenderableRuntimeModelRoots(renderable, runtimeRoots);
  for (void* runtimeRoot : runtimeRoots) {
    void* childRuntimeModelPtr = nullptr;
    if (TryResolveChildRuntimeModelByModelResource(runtimeRoot,
                                                   targetModelResourcePtr,
                                                   childRuntimeModelPtr,
                                                   maxRuntimeModels,
                                                   maxLinkNodes) &&
        childRuntimeModelPtr != nullptr &&
        childRuntimeModelPtr != runtimeRoot) {
      ShadowPoseRecord candidatePose = {};
      if (TryResolvePoseByRuntimeModelSnapshotOrLive(poses,
                                                     childRuntimeModelPtr,
                                                     candidatePose) &&
          ShouldPreferPoseCandidate(candidatePose, outPose)) {
        outRuntimeModelPtr = childRuntimeModelPtr;
        outPose = std::move(candidatePose);
      }
    }

    const auto runtimeTree =
        CollectRuntimeModelTree(runtimeRoot, maxRuntimeModels, maxLinkNodes);
    for (void* candidateRuntimeModelPtr : runtimeTree) {
      if (candidateRuntimeModelPtr == nullptr ||
          candidateRuntimeModelPtr == runtimeRoot) {
        continue;
      }

      void* ownedModelResourcePtr =
          TryResolveDirectModelResourceFromRuntimeModel(
              candidateRuntimeModelPtr);
      if (ownedModelResourcePtr != targetModelResourcePtr)
        continue;

      ShadowPoseRecord candidatePose = {};
      if (!TryResolvePoseByRuntimeModelSnapshotOrLive(poses,
                                                      candidateRuntimeModelPtr,
                                                      candidatePose) ||
          !ShouldPreferPoseCandidate(candidatePose, outPose)) {
        continue;
      }

      outRuntimeModelPtr = candidateRuntimeModelPtr;
      outPose = std::move(candidatePose);
    }
  }

  return outRuntimeModelPtr != nullptr &&
         outPose.matrixCount != 0u &&
         !outPose.matrixPalette.empty();
}

bool TryRecoverAttachmentRootWorldTransform(
    const ShadowAttachmentRigidRecord& attachment, ShadowPoseRecord& ioRootPose,
    ShadowResolveStats* ioStats = nullptr) {
  if (ioRootPose.hasWorldTransform)
    return true;

  auto tryPoseRecord = [&](const model::PoseRecord& poseRecord) {
    ShadowPoseRecord candidate = ConvertPoseRecord(
        poseRecord, poseRecord.lastSeenFrame != 0u ? poseRecord.lastSeenFrame : 0u);
    if (!candidate.hasWorldTransform)
      return false;
    MergeAttachmentRootPoseCandidate(candidate, ioRootPose);
    if (ioStats != nullptr)
      ioStats->attachmentRigidPoseRecoveredWorldTransformFromLivePose++;
    return ioRootPose.hasWorldTransform;
  };

  auto& poseRegistry = model::PoseRegistry::instance();
  model::PoseRecord poseRecord = {};
  if (attachment.ownerRuntimeModelPtr != nullptr &&
      poseRegistry.findByRuntimeModel(attachment.ownerRuntimeModelPtr, poseRecord) &&
      tryPoseRecord(poseRecord)) {
    return true;
  }
  if (attachment.rootRuntimeModelPtr != nullptr &&
      poseRegistry.findByRuntimeModel(attachment.rootRuntimeModelPtr, poseRecord) &&
      tryPoseRecord(poseRecord)) {
    return true;
  }
  if (attachment.sceneNode != nullptr &&
      poseRegistry.findBySceneNode(attachment.sceneNode, poseRecord) &&
      tryPoseRecord(poseRecord)) {
    return true;
  }
  if (attachment.unitPtr != nullptr &&
      poseRegistry.findByUnitPtr(attachment.unitPtr, poseRecord) &&
      tryPoseRecord(poseRecord)) {
    return true;
  }

  auto tryShadowRecord = [&](const render::ShadowObjectRecord& shadowRecord) {
    ShadowPoseRecord candidate = ConvertShadowObjectPoseRecord(
        shadowRecord,
        shadowRecord.lastSeenFrame != 0u ? shadowRecord.lastSeenFrame : 0u);
    if (!candidate.hasWorldTransform)
      return false;
    MergeAttachmentRootPoseCandidate(candidate, ioRootPose);
    if (ioStats != nullptr)
      ioStats->attachmentRigidPoseRecoveredWorldTransformFromShadowRegistry++;
    return ioRootPose.hasWorldTransform;
  };

  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  render::ShadowObjectRecord shadowRecord = {};
  if (attachment.ownerRuntimeModelPtr != nullptr &&
      shadowRegistry.findByRuntimeModel(attachment.ownerRuntimeModelPtr,
                                        shadowRecord) &&
      tryShadowRecord(shadowRecord)) {
    return true;
  }
  if (attachment.rootRuntimeModelPtr != nullptr &&
      shadowRegistry.findByRuntimeModel(attachment.rootRuntimeModelPtr,
                                        shadowRecord) &&
      tryShadowRecord(shadowRecord)) {
    return true;
  }
  if (attachment.sceneNode != nullptr &&
      shadowRegistry.findBySceneNode(attachment.sceneNode, shadowRecord) &&
      tryShadowRecord(shadowRecord)) {
    return true;
  }
  if (attachment.unitPtr != nullptr &&
      shadowRegistry.findByUnitPtr(attachment.unitPtr, shadowRecord) &&
      tryShadowRecord(shadowRecord)) {
    return true;
  }

  return ioRootPose.hasWorldTransform;
}

bool TryResolveAttachmentFromRuntimeOwnerHint(
    void* runtimeOwnerHintPtr, const ShadowAttachmentRigidStore& attachments,
    ShadowAttachmentRigidRecord& outRecord) {
  outRecord = {};
  if (runtimeOwnerHintPtr == nullptr)
    return false;

  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  auto tryChildSpriteBridge = [&](void* spritePtr) {
    if (spritePtr == nullptr)
      return false;
    return attachments.findByChildSpritePtr(spritePtr, outRecord);
  };

  if (tryChildSpriteBridge(runtimeOwnerHintPtr))
    return true;

  if (attachments.findByAnyRuntimeModel(runtimeOwnerHintPtr, outRecord))
    return true;

  if (tryChildSpriteBridge(TryResolveSpritePtrFromRuntimeModel(
          runtimeOwnerHintPtr, instanceRegistry, shadowRegistry))) {
    return true;
  }

  auto trySourceObjectBridge = [&](void* candidatePtr) {
    if (candidatePtr == nullptr)
      return false;

    model::ModelInstanceRecord instanceRecord = {};
    const bool hasInstanceRecord =
        instanceRegistry.findByRuntimeModel(candidatePtr, instanceRecord) ||
        instanceRegistry.findOwnerByRuntimeModel(candidatePtr, instanceRecord) ||
        instanceRegistry.findBySpritePtr(candidatePtr, instanceRecord);
    if (!hasInstanceRecord)
      return false;

    if (instanceRecord.sourceObjectPtr != nullptr &&
        attachments.findBySourceObject(instanceRecord.sourceObjectPtr,
                                       outRecord)) {
      return true;
    }
    if (instanceRecord.sourceSpriteObjectPtr != nullptr &&
        attachments.findBySourceSpriteObject(
            instanceRecord.sourceSpriteObjectPtr, outRecord)) {
      return true;
    }
    return false;
  };

  if (trySourceObjectBridge(runtimeOwnerHintPtr))
    return true;

  auto tryIdentityBridge = [&](void* identityKeyPtr) {
    if (identityKeyPtr == nullptr)
      return false;

    auto tryInstanceAttachmentBridge =
        [&](const model::ModelInstanceRecord& instanceRecord) {
          if (instanceRecord.runtimeModelPtr != nullptr &&
              attachments.findByAnyRuntimeModel(instanceRecord.runtimeModelPtr,
                                                outRecord)) {
            return true;
          }
          if (instanceRecord.spritePtr != nullptr &&
              attachments.findByChildSpritePtr(instanceRecord.spritePtr,
                                               outRecord)) {
            return true;
          }
          if (instanceRecord.sourceObjectPtr != nullptr &&
              attachments.findBySourceObject(instanceRecord.sourceObjectPtr,
                                             outRecord)) {
            return true;
          }
          if (instanceRecord.sourceSpriteObjectPtr != nullptr &&
              attachments.findBySourceSpriteObject(
                  instanceRecord.sourceSpriteObjectPtr, outRecord)) {
            return true;
          }
          if (instanceRecord.worldObjectEntry != nullptr &&
              attachments.findByWorldObjectEntry(instanceRecord.worldObjectEntry,
                                                 outRecord)) {
            return true;
          }
          if (instanceRecord.sceneNode != nullptr &&
              attachments.findBySceneNode(instanceRecord.sceneNode,
                                          outRecord)) {
            return true;
          }
          if (instanceRecord.unitPtr != nullptr &&
              attachments.findByUnitPtr(instanceRecord.unitPtr, outRecord)) {
            return true;
          }
          return false;
        };

    auto tryShadowAttachmentBridge =
        [&](const render::ShadowObjectRecord& shadowRecord) {
          if (shadowRecord.runtimeModelPtr != nullptr &&
              attachments.findByAnyRuntimeModel(shadowRecord.runtimeModelPtr,
                                                outRecord)) {
            return true;
          }
          if (shadowRecord.spritePtr != nullptr &&
              attachments.findByChildSpritePtr(shadowRecord.spritePtr,
                                               outRecord)) {
            return true;
          }
          if (shadowRecord.worldObjectEntry != nullptr &&
              attachments.findByWorldObjectEntry(shadowRecord.worldObjectEntry,
                                                 outRecord)) {
            return true;
          }
          if (shadowRecord.sceneNode != nullptr &&
              attachments.findBySceneNode(shadowRecord.sceneNode,
                                          outRecord)) {
            return true;
          }
          if (shadowRecord.unitPtr != nullptr &&
              attachments.findByUnitPtr(shadowRecord.unitPtr, outRecord)) {
            return true;
          }
          return false;
        };

    render::RenderObjectIdentitySnapshot renderIdentity = {};
    if (render::TryResolveRenderObjectIdentity(nullptr, identityKeyPtr,
                                               renderIdentity) &&
        renderIdentity.HasStableIdentity()) {
      model::ModelInstanceRecord instanceRecord = {};
      if (renderIdentity.worldObjectEntry != nullptr &&
          model::ModelInstanceRegistry::instance().findByWorldObjectEntry(
              renderIdentity.worldObjectEntry, instanceRecord) &&
          tryInstanceAttachmentBridge(instanceRecord)) {
        return true;
      }
      instanceRecord = {};
      if (renderIdentity.sceneNode != nullptr &&
          model::ModelInstanceRegistry::instance().findBySceneNode(
              renderIdentity.sceneNode, instanceRecord) &&
          tryInstanceAttachmentBridge(instanceRecord)) {
        return true;
      }

      render::ShadowObjectRecord bridgedShadowRecord = {};
      if (renderIdentity.worldObjectEntry != nullptr &&
          render::ShadowObjectRegistry::instance().findByWorldObjectEntry(
              renderIdentity.worldObjectEntry, bridgedShadowRecord) &&
          tryShadowAttachmentBridge(bridgedShadowRecord)) {
        return true;
      }
      bridgedShadowRecord = {};
      if (renderIdentity.sceneNode != nullptr &&
          render::ShadowObjectRegistry::instance().findBySceneNode(
              renderIdentity.sceneNode, bridgedShadowRecord) &&
          tryShadowAttachmentBridge(bridgedShadowRecord)) {
        return true;
      }

      if (renderIdentity.worldObjectEntry != nullptr &&
          attachments.findByWorldObjectEntry(renderIdentity.worldObjectEntry,
                                             outRecord)) {
        return true;
      }
      if (renderIdentity.sceneNode != nullptr &&
          attachments.findBySceneNode(renderIdentity.sceneNode, outRecord)) {
        return true;
      }
      if (renderIdentity.unitPtr != nullptr &&
          attachments.findByUnitPtr(renderIdentity.unitPtr, outRecord)) {
        return true;
      }
    }

    model::ModelInstanceRecord instanceRecord = {};
    if (model::ModelInstanceRegistry::instance().findBySceneNode(identityKeyPtr,
                                                                 instanceRecord) &&
        tryInstanceAttachmentBridge(instanceRecord)) {
      return true;
    }

    render::ShadowObjectRecord shadowRecord = {};
    if (render::ShadowObjectRegistry::instance().findBySceneNode(identityKeyPtr,
                                                                 shadowRecord) &&
        tryShadowAttachmentBridge(shadowRecord)) {
      return true;
    }

    return false;
  };

  if (tryIdentityBridge(runtimeOwnerHintPtr))
    return true;

  auto tryRuntimeModelAliasBridge = [&](void* runtimeModelPtr) {
    if (runtimeModelPtr == nullptr || runtimeModelPtr == runtimeOwnerHintPtr)
      return false;

    if (attachments.findByAnyRuntimeModel(runtimeModelPtr, outRecord))
      return true;
    if (tryChildSpriteBridge(TryResolveSpritePtrFromRuntimeModel(
            runtimeModelPtr, instanceRegistry, shadowRegistry))) {
      return true;
    }
    if (trySourceObjectBridge(runtimeModelPtr))
      return true;
    if (tryIdentityBridge(runtimeModelPtr))
      return true;

    return false;
  };

  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const uintptr_t runtimeOwnerValue =
      reinterpret_cast<uintptr_t>(runtimeOwnerHintPtr);
  if (runtimeOwnerValue >= 0x10000u) {
    if (tryRuntimeModelAliasBridge(reinterpret_cast<void*>(
            runtimeOwnerValue + kCModelComplexExtensionOffset))) {
      return true;
    }
    if (runtimeOwnerValue > kCModelComplexExtensionOffset &&
        tryRuntimeModelAliasBridge(reinterpret_cast<void*>(
            runtimeOwnerValue - kCModelComplexExtensionOffset))) {
      return true;
    }
  }

  void* spriteRuntimeModelPtr = TryReadRuntimeModelFromSprite(runtimeOwnerHintPtr);
  if (tryChildSpriteBridge(runtimeOwnerHintPtr))
    return true;
  if (spriteRuntimeModelPtr != nullptr &&
      attachments.findByAnyRuntimeModel(spriteRuntimeModelPtr, outRecord)) {
    return true;
  }
  if (tryChildSpriteBridge(TryResolveSpritePtrFromRuntimeModel(
          spriteRuntimeModelPtr, instanceRegistry, shadowRegistry))) {
    return true;
  }
  if (trySourceObjectBridge(spriteRuntimeModelPtr))
    return true;

  constexpr size_t kWrapperScanLimit = 0x80u;
  constexpr size_t kPointerStride = sizeof(void*);
  std::unordered_set<void*> visitedCandidates;
  visitedCandidates.reserve(32u);
  for (size_t offset = 0u; offset <= kWrapperScanLimit; offset += kPointerStride) {
    void* candidatePtr = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(runtimeOwnerHintPtr, offset, candidatePtr) ||
        candidatePtr == nullptr ||
        !visitedCandidates.insert(candidatePtr).second) {
      continue;
    }

    if (LooksLikeRuntimeModelPtr(candidatePtr) &&
        attachments.findByAnyRuntimeModel(candidatePtr, outRecord)) {
      return true;
    }

    if (tryChildSpriteBridge(candidatePtr))
      return true;
    if (tryChildSpriteBridge(TryResolveSpritePtrFromRuntimeModel(
            candidatePtr, instanceRegistry, shadowRegistry))) {
      return true;
    }

    if (trySourceObjectBridge(candidatePtr))
      return true;

    if (tryIdentityBridge(candidatePtr))
      return true;

    spriteRuntimeModelPtr = TryReadRuntimeModelFromSprite(candidatePtr);
    if (spriteRuntimeModelPtr != nullptr &&
        attachments.findByAnyRuntimeModel(spriteRuntimeModelPtr, outRecord)) {
      return true;
    }
    if (trySourceObjectBridge(spriteRuntimeModelPtr))
      return true;
  }

  return false;
}

bool RuntimeModelOwnsRenderableGeoset(const ShadowRenderableRecord& renderable,
                                      void* runtimeModelPtr) {
  if (!LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return false;
  if (renderable.runtimeGeosetPtr == nullptr &&
      renderable.runtimeGeosetDataPtr == nullptr) {
    return false;
  }

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (renderable.geosetIndex != kInvalidShadowContractGeosetIndex) {
    model::ShadowGeosetResourceRecord runtimeGeosetRecord = {};
    if (resourceCache.findRuntimeModelGeoset(runtimeModelPtr,
                                             renderable.geosetIndex,
                                             runtimeGeosetRecord)) {
      if ((renderable.runtimeGeosetPtr != nullptr &&
           runtimeGeosetRecord.geosetPtr == renderable.runtimeGeosetPtr) ||
          (renderable.runtimeGeosetDataPtr != nullptr &&
           runtimeGeosetRecord.geosetDataPtr ==
               renderable.runtimeGeosetDataPtr)) {
        return true;
      }
    }
  }

  uint32_t runtimeGeosetCount = 0u;
  void* runtimeGeosets = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(runtimeModelPtr,
                                   dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                                   runtimeGeosetCount) ||
      runtimeGeosetCount == 0u || runtimeGeosetCount > 4096u ||
      !dxvk::war3::SafeReadPtrFast(runtimeModelPtr,
                                   dxvk::war3::CModelOffsets::RuntimeGeosets,
                                   runtimeGeosets) ||
      runtimeGeosets == nullptr ||
      !dxvk::war3::IsReadableRange(runtimeGeosets,
                                   size_t(runtimeGeosetCount) *
                                       sizeof(void*))) {
    return false;
  }

  const auto* geosetArray = reinterpret_cast<void* const*>(runtimeGeosets);
  auto geosetMatches = [&](void* runtimeGeosetPtr) {
    if (runtimeGeosetPtr == nullptr)
      return false;
    if (renderable.runtimeGeosetPtr != nullptr &&
        runtimeGeosetPtr == renderable.runtimeGeosetPtr) {
      return true;
    }

    void* runtimeGeosetDataPtr = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(runtimeGeosetPtr,
                                     dxvk::war3::CGeosetOffsets::GeosetData,
                                     runtimeGeosetDataPtr) ||
        runtimeGeosetDataPtr == nullptr) {
      return false;
    }

    return renderable.runtimeGeosetDataPtr != nullptr &&
           runtimeGeosetDataPtr == renderable.runtimeGeosetDataPtr;
  };

  if (renderable.geosetIndex != kInvalidShadowContractGeosetIndex &&
      renderable.geosetIndex < runtimeGeosetCount &&
      geosetMatches(geosetArray[renderable.geosetIndex])) {
    return true;
  }

  for (uint32_t i = 0u; i < runtimeGeosetCount; ++i) {
    if (geosetMatches(geosetArray[i]))
      return true;
  }

  return false;
}

bool TryResolveAttachmentByChildRuntimeGeoset(
    const ShadowRenderableRecord& renderable,
    const ShadowAttachmentRigidStore& attachments,
    ShadowAttachmentRigidRecord& outRecord,
    bool& outMatchedChildSpriteRuntime) {
  outRecord = {};
  outMatchedChildSpriteRuntime = false;
  if (renderable.runtimeGeosetPtr == nullptr &&
      renderable.runtimeGeosetDataPtr == nullptr) {
    return false;
  }

  const ShadowAttachmentRigidRecord* uniqueMatch = nullptr;
  bool uniqueMatchedChildSpriteRuntime = false;
  for (const auto& attachment : attachments.records()) {
    bool matchedChildSpriteRuntime = false;
    bool matched = RuntimeModelOwnsRenderableGeoset(
        renderable, attachment.childRuntimeModelPtr);
    if (!matched) {
      void* childSpriteRuntimeModelPtr =
          TryReadRuntimeModelFromSprite(attachment.childSpritePtr);
      if (childSpriteRuntimeModelPtr != nullptr &&
          childSpriteRuntimeModelPtr != attachment.childRuntimeModelPtr) {
        matched = RuntimeModelOwnsRenderableGeoset(
            renderable, childSpriteRuntimeModelPtr);
        matchedChildSpriteRuntime = matched;
      }
    }
    if (!matched) {
      continue;
    }

    if (uniqueMatch != nullptr)
      return false;
    uniqueMatch = &attachment;
    uniqueMatchedChildSpriteRuntime = matchedChildSpriteRuntime;
  }

  if (uniqueMatch == nullptr)
    return false;

  outRecord = *uniqueMatch;
  outMatchedChildSpriteRuntime = uniqueMatchedChildSpriteRuntime;
  return true;
}

bool IsAnonymousAttachmentRenderable(const ShadowRenderableRecord& renderable) {
  return renderable.runtimeGeosetDataPtr != nullptr &&
         renderable.worldObjectEntry == nullptr &&
         renderable.sceneNode == nullptr && renderable.unitPtr == nullptr &&
         renderable.runtimeModelPtr == nullptr && renderable.jHandle == 0u &&
         renderable.rawcode == 0u;
}

bool TryResolveAttachmentByParentRuntimeGeoset(
    const ShadowRenderableRecord& renderable,
    const ShadowAttachmentRigidStore& attachments, bool useOwnerRuntime,
    ShadowAttachmentRigidRecord& outRecord) {
  outRecord = {};
  if (renderable.runtimeGeosetPtr == nullptr &&
      renderable.runtimeGeosetDataPtr == nullptr) {
    return false;
  }

  const ShadowAttachmentRigidRecord* uniqueMatch = nullptr;
  for (const auto& attachment : attachments.records()) {
    void* candidateRuntimeModelPtr =
        useOwnerRuntime ? attachment.ownerRuntimeModelPtr
                        : attachment.rootRuntimeModelPtr;
    if (!RuntimeModelOwnsRenderableGeoset(renderable, candidateRuntimeModelPtr))
      continue;

    if (uniqueMatch != nullptr &&
        (uniqueMatch->childRuntimeModelPtr != attachment.childRuntimeModelPtr ||
         uniqueMatch->ownerRuntimeModelPtr != attachment.ownerRuntimeModelPtr ||
         uniqueMatch->rootRuntimeModelPtr != attachment.rootRuntimeModelPtr)) {
      return false;
    }

    uniqueMatch = &attachment;
  }

  if (uniqueMatch == nullptr)
    return false;

  outRecord = *uniqueMatch;
  return true;
}

bool TryResolveAttachmentRigidRecord(
    const ShadowRenderableRecord& renderable,
    const ShadowAttachmentRigidStore& attachments,
    ShadowAttachmentRigidRecord& outRecord,
    ShadowResolveStats* ioStats = nullptr) {
  outRecord = {};
  if (attachments.records().empty())
    return false;
  const bool anonymousGeosetSubpart =
      IsAnonymousAttachmentRenderable(renderable);
  if (renderable.runtimeModelPtr != nullptr &&
      attachments.findByChildRuntimeModel(renderable.runtimeModelPtr,
                                          outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByChildRuntimeModel++;
    return true;
  }
  {
    auto& instanceRegistry = model::ModelInstanceRegistry::instance();
    auto& shadowRegistry = render::ShadowObjectRegistry::instance();
    void* renderableSpritePtr = TryResolveSpritePtrFromRuntimeModel(
        renderable.runtimeModelPtr, instanceRegistry, shadowRegistry);
  if (renderableSpritePtr != nullptr &&
        attachments.findByChildSpritePtr(renderableSpritePtr, outRecord)) {
      if (ioStats != nullptr)
        ioStats->attachmentRigidMatchByChildSprite++;
      return true;
    }
  }
  bool matchedByChildSpriteRuntimeGeoset = false;
  const bool needsAttachmentGeosetScan =
      anonymousGeosetSubpart || renderable.runtimeModelPtr == nullptr;
  if (needsAttachmentGeosetScan &&
      TryResolveAttachmentByChildRuntimeGeoset(
          renderable, attachments, outRecord,
          matchedByChildSpriteRuntimeGeoset)) {
    if (ioStats != nullptr)
      matchedByChildSpriteRuntimeGeoset
          ? ioStats->attachmentRigidMatchByChildSpriteRuntimeGeoset++
          : ioStats->attachmentRigidMatchByChildRuntimeGeoset++;
    return true;
  }
  if (anonymousGeosetSubpart &&
      TryResolveAttachmentByParentRuntimeGeoset(renderable, attachments, true,
                                                outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByOwnerRuntimeGeoset++;
    return true;
  }
  if (anonymousGeosetSubpart &&
      TryResolveAttachmentByParentRuntimeGeoset(renderable, attachments, false,
                                                outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByRootRuntimeGeoset++;
    return true;
  }
  model::ShadowModelResourceRecord runtimeOwner = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (resourceCache.findRuntimeModelOwner(renderable.runtimeGeosetPtr,
                                          renderable.runtimeGeosetDataPtr,
                                          renderable.geosetIndex,
                                          renderable.modelResourcePtr,
                                          runtimeOwner) &&
      runtimeOwner.runtimeModelPtr != nullptr &&
      TryResolveAttachmentFromRuntimeOwnerHint(runtimeOwner.runtimeModelPtr,
                                               attachments, outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByResourceRuntimeOwner++;
    return true;
  }
  if (needsAttachmentGeosetScan) {
    std::vector<void*> runtimeRoots;
    CollectRenderableRuntimeModelRoots(renderable, runtimeRoots);
    for (void* runtimeRoot : runtimeRoots) {
      if (runtimeRoot == nullptr ||
          runtimeRoot == renderable.runtimeModelPtr ||
          runtimeRoot == runtimeOwner.runtimeModelPtr) {
        continue;
      }
      if (TryResolveAttachmentFromRuntimeOwnerHint(runtimeRoot, attachments,
                                                   outRecord)) {
        if (ioStats != nullptr)
          ioStats->attachmentRigidMatchByRenderableRuntimeRoot++;
        return true;
      }
    }
  }
  if (runtimeOwner.runtimeModelPtr != nullptr) {
    auto& renderRegistry = render::RenderObjectRegistry::instance();
    const render::RenderObjectInfo* bridgedObject =
        renderRegistry.findBySceneNode(runtimeOwner.runtimeModelPtr);
    if (bridgedObject == nullptr)
      bridgedObject = renderRegistry.findByEntry(runtimeOwner.runtimeModelPtr);
    if (bridgedObject != nullptr) {
      if (bridgedObject->worldObjectEntry != nullptr &&
          attachments.findByWorldObjectEntry(bridgedObject->worldObjectEntry,
                                             outRecord)) {
        if (ioStats != nullptr)
          ioStats->attachmentRigidMatchByWorldObjectEntry++;
        return true;
      }
      if (bridgedObject->sceneNode != nullptr &&
          attachments.findBySceneNode(bridgedObject->sceneNode, outRecord)) {
        if (ioStats != nullptr)
          ioStats->attachmentRigidMatchBySceneNode++;
        return true;
      }
      if (bridgedObject->unitPtr != nullptr &&
          attachments.findByUnitPtr(bridgedObject->unitPtr, outRecord)) {
        if (ioStats != nullptr)
          ioStats->attachmentRigidMatchByUnitPtr++;
        return true;
      }
    }
  }
  if (renderable.worldObjectEntry != nullptr &&
      attachments.findByWorldObjectEntry(renderable.worldObjectEntry,
                                         outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByWorldObjectEntry++;
    return true;
  }
  if (renderable.sceneNode != nullptr &&
      attachments.findBySceneNode(renderable.sceneNode, outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchBySceneNode++;
    return true;
  }
  if (renderable.unitPtr != nullptr &&
      attachments.findByUnitPtr(renderable.unitPtr, outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByUnitPtr++;
    return true;
  }
  if (renderable.jHandle != 0u &&
      attachments.findByHandle(renderable.jHandle, outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByHandle++;
    return true;
  }
  if (anonymousGeosetSubpart &&
      (renderable.modelResourcePtr != nullptr || renderable.modelKey != 0u) &&
      attachments.findUniqueByChildModelResource(renderable.modelResourcePtr,
                                                 renderable.modelKey,
                                                 outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByChildModelResource++;
    return true;
  }
  if (anonymousGeosetSubpart &&
      attachments.findUniqueWithAnyIdentity(outRecord)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidMatchByUniqueIdentity++;
    return true;
  }
  if (ioStats != nullptr) {
    ioStats->attachmentRigidMatchMiss++;
    ioStats->lastAttachmentRigidMatchMissRuntimeModelPtr =
        reinterpret_cast<uint64_t>(renderable.runtimeModelPtr);
    ioStats->lastAttachmentRigidMatchMissModelResourcePtr =
        reinterpret_cast<uint64_t>(renderable.modelResourcePtr);
    ioStats->lastAttachmentRigidMatchMissRuntimeGeosetPtr =
        reinterpret_cast<uint64_t>(renderable.runtimeGeosetPtr);
    ioStats->lastAttachmentRigidMatchMissRuntimeGeosetDataPtr =
        reinterpret_cast<uint64_t>(renderable.runtimeGeosetDataPtr);
    ioStats->lastAttachmentRigidMatchMissGeosetIndex =
        renderable.geosetIndex == kInvalidShadowContractGeosetIndex
            ? 0u
            : uint64_t(renderable.geosetIndex);
    ioStats->lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr =
        reinterpret_cast<uint64_t>(runtimeOwner.runtimeModelPtr);
  }
  return false;
}

bool MightResolveAttachmentRigidRecord(
    const ShadowRenderableRecord& renderable,
    const ShadowAttachmentRigidStore& attachments) {
  if (attachments.records().empty())
    return false;
  if (IsAnonymousAttachmentRenderable(renderable))
    return true;

  ShadowAttachmentRigidRecord probe = {};
  if (renderable.runtimeModelPtr != nullptr &&
      attachments.findByChildRuntimeModel(renderable.runtimeModelPtr, probe))
    return true;
  if (renderable.worldObjectEntry != nullptr &&
      attachments.findByWorldObjectEntry(renderable.worldObjectEntry, probe))
    return true;
  if (renderable.sceneNode != nullptr &&
      attachments.findBySceneNode(renderable.sceneNode, probe))
    return true;
  if (renderable.unitPtr != nullptr &&
      attachments.findByUnitPtr(renderable.unitPtr, probe))
    return true;
  if (renderable.jHandle != 0u &&
      attachments.findByHandle(renderable.jHandle, probe))
    return true;

  model::ShadowModelResourceRecord runtimeOwner = {};
  if (model::ShadowModelResourceCache::instance().findRuntimeModelOwner(
          renderable.runtimeGeosetPtr, renderable.runtimeGeosetDataPtr,
          renderable.geosetIndex, renderable.modelResourcePtr, runtimeOwner) &&
      runtimeOwner.runtimeModelPtr != nullptr &&
      attachments.findByAnyRuntimeModel(runtimeOwner.runtimeModelPtr, probe)) {
    return true;
  }

  return false;
}

bool TryBuildAttachmentRigidPose(const ShadowRenderableRecord& renderable,
                                 const ShadowAttachmentRigidStore& attachments,
                                 const ShadowPoseStore& poses,
                                 ShadowRenderableRecord& ioRenderable,
                                 ShadowAttachmentRigidRecord& outAttachment,
                                 ShadowPoseRecord& outPose,
                                 ShadowResolveStats* ioStats = nullptr) {
  outAttachment = {};
  outPose = {};
  if (!TryResolveAttachmentRigidRecord(renderable, attachments, outAttachment,
                                       ioStats)) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidPoseMissNoRecord++;
    return false;
  }
  if (outAttachment.rootRuntimeModelPtr == nullptr ||
      outAttachment.childRuntimeModelPtr == nullptr) {
    if (ioStats != nullptr)
      ioStats->attachmentRigidPoseMissMissingRuntimes++;
    return false;
  }

  ShadowPoseRecord rootPose = {};
  bool rootPoseStoreHit = false;
  if (outAttachment.ownerRuntimeModelPtr != nullptr &&
      TryResolveWorldPoseByRuntimeModelSnapshot(
          poses, outAttachment.ownerRuntimeModelPtr, rootPose)) {
    rootPoseStoreHit = true;
  } else if (outAttachment.rootRuntimeModelPtr != nullptr &&
             TryResolveWorldPoseByRuntimeModelSnapshot(
                 poses, outAttachment.rootRuntimeModelPtr, rootPose)) {
    rootPoseStoreHit = true;
  } else {
    if (outAttachment.sceneNode != nullptr &&
        poses.findBySceneNode(outAttachment.sceneNode, rootPose)) {
      rootPoseStoreHit = true;
    } else if (outAttachment.unitPtr != nullptr &&
               poses.findByUnitPtr(outAttachment.unitPtr, rootPose)) {
      rootPoseStoreHit = true;
    }
  }

  TryRecoverAttachmentRootWorldTransform(outAttachment, rootPose, ioStats);
  if (!rootPoseStoreHit && rootPose.runtimeModelPtr == nullptr &&
      rootPose.sceneNode == nullptr && rootPose.unitPtr == nullptr &&
      ioStats != nullptr) {
    ioStats->attachmentRigidPoseMissNoRootPose++;
  }
  if (!rootPose.hasWorldTransform)
  {
    if (ioStats != nullptr)
      ioStats->attachmentRigidPoseMissNoRootWorldTransform++;
    return false;
  }

  const Vector4 translatedPoint =
      rootPose.worldTransform *
      Vector4(outAttachment.localPointX, outAttachment.localPointY,
              outAttachment.localPointZ, 1.0f);

  outPose.runtimeModelPtr = outAttachment.childRuntimeModelPtr;
  outPose.sceneNode = outAttachment.sceneNode;
  outPose.unitPtr = outAttachment.unitPtr;
  outPose.hasWorldTransform = true;
  outPose.worldTransform = rootPose.worldTransform;
  outPose.worldTransform[3] =
      Vector4(translatedPoint.x, translatedPoint.y, translatedPoint.z, 1.0f);
  outPose.frameSerial = outAttachment.frameSerial;

  if (ioRenderable.runtimeModelPtr == nullptr)
    ioRenderable.runtimeModelPtr = outAttachment.childRuntimeModelPtr;
  if (ioRenderable.worldObjectEntry == nullptr)
    ioRenderable.worldObjectEntry = outAttachment.worldObjectEntry;
  if (ioRenderable.sceneNode == nullptr)
    ioRenderable.sceneNode = outAttachment.sceneNode;
  if (ioRenderable.unitPtr == nullptr)
    ioRenderable.unitPtr = outAttachment.unitPtr;
  if (ioRenderable.jHandle == 0u)
    ioRenderable.jHandle = outAttachment.jHandle;
  if (ioRenderable.rawcode == 0u)
    ioRenderable.rawcode = outAttachment.rawcode;

  return true;
}

ShadowPrimitiveTopology MapPrimitiveTypeToTopology(uint32_t primitiveType) {
  switch (primitiveType) {
  case 4u:
    return ShadowPrimitiveTopology::TriangleStrip;
  case 5u:
    return ShadowPrimitiveTopology::TriangleFan;
  case 1u:
    return ShadowPrimitiveTopology::LineList;
  case 2u:
    return ShadowPrimitiveTopology::LineStrip;
  case 3u:
  default:
    return ShadowPrimitiveTopology::TriangleList;
  }
}

bool TryInferDynamicTopologyFromPrimitiveRecords(
    const ShadowModelResourceRecord& resource, uint32_t primitiveBaseIndex,
    uint32_t indexCount, ShadowPrimitiveTopology& outTopology) {
  outTopology = ShadowPrimitiveTopology::TriangleList;

  if (resource.primitiveRecords.empty() || indexCount == 0u)
    return false;

  uint32_t runningIndex = 0u;
  uint32_t consumed = 0u;
  uint32_t selectedPrimitiveType = 0u;
  bool foundStart = false;

  for (const auto& primitive : resource.primitiveRecords) {
    if (!foundStart) {
      if (runningIndex != primitiveBaseIndex) {
        runningIndex += primitive.indexCount;
        continue;
      }
      foundStart = true;
      selectedPrimitiveType = primitive.primitiveTypeOrMaterialSlot;
    } else if (primitive.primitiveTypeOrMaterialSlot != selectedPrimitiveType) {
      return false;
    }

    consumed += primitive.indexCount;
    runningIndex += primitive.indexCount;
    if (consumed == indexCount) {
      outTopology = MapPrimitiveTypeToTopology(selectedPrimitiveType);
      return true;
    }
    if (consumed > indexCount)
      return false;
  }

  return false;
}

bool LooksLikePrimitiveTypeSequence(
    const std::vector<ShadowPrimitiveRecord>& primitiveRecords) {
  if (primitiveRecords.empty())
    return false;

  for (const auto& primitive : primitiveRecords) {
    if (primitive.indexCount == 0u)
      return false;
    if (primitive.primitiveTypeOrMaterialSlot > 5u)
      return false;
  }
  return true;
}

bool TryReadMeshSubPrimitiveRecords(
    const ShadowRenderableRecord& renderable,
    std::vector<ShadowPrimitiveRecord>& outPrimitiveRecords) {
  outPrimitiveRecords.clear();

  if (renderable.meshData == nullptr ||
      ShouldSkipLegacyMeshDataDecode(renderable))
    return false;

  uint32_t subPrimitiveCount = 0u;
  void* subPrimitivePairs = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(renderable.meshData,
                                   dxvk::war3::MeshDataOffsets::SubPrimitiveCount,
                                   subPrimitiveCount) ||
      !dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                   dxvk::war3::MeshDataOffsets::SubPrimitivePairs,
                                   subPrimitivePairs) ||
      subPrimitiveCount == 0u || subPrimitivePairs == nullptr ||
      subPrimitiveCount > 128u ||
      !dxvk::war3::IsReadableRange(
          subPrimitivePairs,
          size_t(subPrimitiveCount) * sizeof(dxvk::war3::GeosetPrimitiveRecord))) {
    return false;
  }

  auto* rawPairs =
      reinterpret_cast<const dxvk::war3::GeosetPrimitiveRecord*>(subPrimitivePairs);
  outPrimitiveRecords.reserve(subPrimitiveCount);
  for (uint32_t i = 0u; i < subPrimitiveCount; ++i) {
    const auto& raw = rawPairs[i];
    if (raw.index_count == 0u || raw.index_count > (1u << 20))
      return false;
    outPrimitiveRecords.push_back(ShadowPrimitiveRecord{
        raw.primitive_type_or_material_slot, raw.index_count});
  }

  return !outPrimitiveRecords.empty();
}

void TraceDirectGeosetSliceResolution(
    const char* reason,
    const ShadowRenderableRecord& renderable,
    const ShadowModelResourceRecord& resource,
    uint32_t resolvedBaseIndex = 0u,
    uint32_t resolvedIndexCount = 0u) {
  if (!SemanticCoreSliceTraceEnabled())
    return;

  static std::atomic<uint32_t> s_traceCount{0u};
  const uint32_t traceIndex =
      s_traceCount.fetch_add(1u, std::memory_order_relaxed);
  if (!(traceIndex < 96u || (traceIndex % 4096u) == 0u))
    return;

  uint32_t partStageSpan = 0u;
  uint32_t partMeshWord = 0u;
  uint32_t stateWord0 = 0u;
  uint32_t stateWord18 = 0u;
  uint32_t stateWord1C = 0u;
  uint32_t stateWord20 = 0u;
  uint32_t scenePresetBase = 0u;
  dxvk::war3::SafeReadU32Fast(
      renderable.renderablePart,
      dxvk::war3::RenderablePartFieldOffsets::StagePresetSpanBaseIndex,
      partStageSpan);
  dxvk::war3::SafeReadU32Fast(
      renderable.renderablePart,
      dxvk::war3::RenderablePartFieldOffsets::MeshData,
      partMeshWord);
  dxvk::war3::SafeReadU32Fast(
      renderable.layerState,
      dxvk::war3::MeshLayerStateRecordOffsets::PrimaryResourceBinding,
      stateWord0);
  dxvk::war3::SafeReadU32Fast(
      renderable.layerState,
      dxvk::war3::MeshLayerStateRecordOffsets::BlendOrDrawMode,
      stateWord18);
  dxvk::war3::SafeReadU32Fast(
      renderable.layerState,
      dxvk::war3::MeshLayerStateRecordOffsets::AuxRefEnable0,
      stateWord1C);
  dxvk::war3::SafeReadU32Fast(
      renderable.layerState,
      dxvk::war3::MeshLayerStateRecordOffsets::AuxRefEnable1,
      stateWord20);
  dxvk::war3::SafeReadU32Fast(
      renderable.sceneNode,
      dxvk::war3::SceneNodeOffsets::StagePresetBaseIndex,
      scenePresetBase);

  uint32_t layerCount = 0u;
  uint32_t stagePreset0 = 0u;
  uint32_t stagePreset1 = 0u;
  uint32_t auxRef0 = 0u;
  uint32_t auxRef1 = 0u;
  uint32_t visibilityOffset = 0u;
  uint32_t alphaFlags = 0u;
  uint32_t stageMode0 = 0u;
  uint32_t stageMode1 = 0u;
  void* meshInfoTable = nullptr;
  void* meshInfo = nullptr;
  void* layerInfo = nullptr;
  void* layerRecords = nullptr;
  if (renderable.sceneNode != nullptr &&
      renderable.meshIndex != kInvalidShadowContractGeosetIndex &&
      renderable.meshIndex < 4096u &&
      dxvk::war3::SafeReadPtrFast(
          renderable.sceneNode, dxvk::war3::SceneNodeOffsets::MeshInfoTable,
          meshInfoTable) &&
      meshInfoTable != nullptr &&
      dxvk::war3::IsReadableRange(
          meshInfoTable, (size_t(renderable.meshIndex) + 1u) * sizeof(void*))) {
    std::memcpy(&meshInfo,
                reinterpret_cast<const uint8_t*>(meshInfoTable) +
                    size_t(renderable.meshIndex) * sizeof(void*),
                sizeof(meshInfo));
  }
  if (meshInfo != nullptr &&
      dxvk::war3::SafeReadU32Fast(meshInfo,
                                  dxvk::war3::MeshInfoOffsets::LayerCount,
                                  layerCount) &&
      dxvk::war3::SafeReadPtrFast(meshInfo,
                                  dxvk::war3::MeshInfoOffsets::LayerInfo,
                                  layerInfo) &&
      layerInfo != nullptr &&
      dxvk::war3::SafeReadPtrFast(layerInfo,
                                  dxvk::war3::LayerInfoOffsets::LayerRecords,
                                  layerRecords) &&
      layerRecords != nullptr &&
      renderable.layerIndex < layerCount) {
    const auto* dispatchPtr =
        reinterpret_cast<const uint8_t*>(layerRecords) +
        size_t(renderable.layerIndex) * 0x2Cu;
    if (dxvk::war3::IsReadableRange(dispatchPtr, 0x2Cu)) {
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr,
          dxvk::war3::MeshLayerDispatchRecordOffsets::StagePresetIndex0,
          stagePreset0);
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr,
          dxvk::war3::MeshLayerDispatchRecordOffsets::StagePresetIndex1,
          stagePreset1);
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr,
          dxvk::war3::MeshLayerDispatchRecordOffsets::AuxRefIndex0,
          auxRef0);
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr,
          dxvk::war3::MeshLayerDispatchRecordOffsets::AuxRefIndex1,
          auxRef1);
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr,
          dxvk::war3::MeshLayerDispatchRecordOffsets::VisibilityOffset,
          visibilityOffset);
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr, dxvk::war3::MeshLayerDispatchRecordOffsets::AlphaFlags,
          alphaFlags);
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr, dxvk::war3::MeshLayerDispatchRecordOffsets::StageMode0,
          stageMode0);
      dxvk::war3::SafeReadU32Fast(
          dispatchPtr, dxvk::war3::MeshLayerDispatchRecordOffsets::StageMode1,
          stageMode1);
    }
  }

  const uint32_t primCount =
      static_cast<uint32_t>(std::min<size_t>(
          resource.primitiveRecords.size(), size_t(4u)));
  std::array<uint32_t, 4> primTypes = {};
  std::array<uint32_t, 4> primIndexCounts = {};
  for (uint32_t i = 0u; i < primCount; ++i) {
    primTypes[i] = resource.primitiveRecords[i].primitiveTypeOrMaterialSlot;
    primIndexCounts[i] = resource.primitiveRecords[i].indexCount;
  }

  dxvk::war3dbg::Print(
      "DXVK SemanticSliceTrace: reason=%s part=%p payload=%p scene=%p "
      "mesh=%p geosetData=%p runtime=%p model=%p layer=%u sub=%u flags=0x%08X "
      "meshIdx=%u geoIdx=%u prims=%u idx=%u resolvedBase=%u resolvedCount=%u "
      "prim0=%u/%u prim1=%u/%u prim2=%u/%u prim3=%u/%u "
      "partSpan=%u partMeshWord=0x%08X state=%p sw=%u/%u/%u/%u "
      "sceneBase=%u layerCount=%u stage=%u/%u aux=%u/%u vis=%u alpha=0x%X "
      "mode=%u/%u handle=0x%08X raw=0x%08X obj=%u group=%d\n",
      reason, renderable.renderablePart, renderable.payload,
      renderable.sceneNode, renderable.meshData,
      renderable.runtimeGeosetDataPtr, renderable.runtimeModelPtr,
      renderable.modelResourcePtr, renderable.layerIndex, renderable.subIndex,
      renderable.flags, renderable.meshIndex, renderable.geosetIndex,
      uint32_t(resource.primitiveRecords.size()),
      uint32_t(resource.indices.size()), resolvedBaseIndex, resolvedIndexCount,
      primTypes[0], primIndexCounts[0], primTypes[1], primIndexCounts[1],
      primTypes[2], primIndexCounts[2], primTypes[3], primIndexCounts[3],
      partStageSpan, partMeshWord, renderable.layerState, stateWord0,
      stateWord18, stateWord1C, stateWord20, scenePresetBase, layerCount,
      stagePreset0, stagePreset1, auxRef0, auxRef1, visibilityOffset,
      alphaFlags, stageMode0, stageMode1, renderable.jHandle,
      renderable.rawcode, uint32_t(renderable.objectKind),
      int(renderable.groupIdx));
}

bool TryResolveDirectGeosetVisibleIndexStream(
    const ShadowRenderableRecord& renderable,
    const ShadowModelResourceRecord& resource,
    const uint16_t*& outIndexStream,
    uint32_t& outIndexCount,
    uint32_t& outPrimitiveBaseIndex,
    uint64_t& outIndexHash,
    ShadowPrimitiveTopology& outTopology) {
  outIndexStream = nullptr;
  outIndexCount = 0u;
  outPrimitiveBaseIndex = 0u;
  outIndexHash = 0u;
  outTopology = ShadowPrimitiveTopology::TriangleList;

  if (!RenderableUsesDirectGeosetData(renderable) ||
      resource.indices.empty() || resource.primitiveRecords.empty()) {
    return false;
  }

  auto tryPrimitiveIndex = [&](uint32_t primitiveIndex) -> bool {
    if (primitiveIndex >= resource.primitiveRecords.size())
      return false;

    uint32_t baseIndex = 0u;
    for (uint32_t i = 0u; i < primitiveIndex; ++i) {
      const uint32_t count = resource.primitiveRecords[i].indexCount;
      if (count == 0u || count > resource.indices.size() ||
          baseIndex > resource.indices.size() - count) {
        return false;
      }
      baseIndex += count;
    }

    const auto& primitive = resource.primitiveRecords[primitiveIndex];
    const uint32_t count = primitive.indexCount;
    if (count == 0u || count > resource.indices.size() ||
        baseIndex > resource.indices.size() - count) {
      return false;
    }

    outPrimitiveBaseIndex = baseIndex;
    outIndexCount = count;
    outIndexStream = resource.indices.data() + baseIndex;
    outTopology = MapPrimitiveTypeToTopology(
        primitive.primitiveTypeOrMaterialSlot);

    uint64_t hash = bit::fnv1a_init();
    hash = bit::fnv1a_iter(hash, reinterpret_cast<uintptr_t>(renderable.meshData));
    hash = bit::fnv1a_iter(hash, renderable.layerIndex);
    hash = bit::fnv1a_iter(hash, renderable.subIndex);
    hash = bit::fnv1a_iter(hash, primitiveIndex);
    hash = bit::fnv1a_iter(hash, outPrimitiveBaseIndex);
    hash = bit::fnv1a_iter(hash, outIndexCount);
    hash = bit::fnv1a_iter(hash, uint32_t(outTopology));
    hash = bit::fnv1a_iter(hash,
                           reinterpret_cast<uintptr_t>(outIndexStream));
    const uint32_t sampleCount = (std::min)(outIndexCount, 32u);
    for (uint32_t i = 0u; i < sampleCount; ++i)
      hash = bit::fnv1a_iter(hash, uint32_t(outIndexStream[i]));
    if (outIndexCount > sampleCount) {
      const uint32_t tailStart = outIndexCount - sampleCount;
      for (uint32_t i = tailStart; i < outIndexCount; ++i)
        hash = bit::fnv1a_iter(hash, uint32_t(outIndexStream[i]));
    }
    outIndexHash = hash;
    return true;
  };

  // Direct CGeosetData batches are already emitted one visible render layer at
  // a time by the render queue. Treat layerIndex as the canonical primitive
  // selector, and fall back to subIndex only when the original layer id is out
  // of range. This avoids the old "full geoset" path that submitted hidden
  // build/scaffold/material variants into the shadow map.
  if (tryPrimitiveIndex(renderable.layerIndex))
    return true;
  if (renderable.subIndex != renderable.layerIndex &&
      tryPrimitiveIndex(renderable.subIndex))
    return true;

  TraceDirectGeosetSliceResolution("direct-layer-miss", renderable, resource);
  return false;
}

bool TryMatchPrimitiveSequenceAtResourceStart(
    const ShadowModelResourceRecord& resource, size_t startPrimitive,
    const std::vector<ShadowPrimitiveRecord>& targetRecords,
    bool requireTypeMatch, uint32_t& outIndexCount,
    ShadowPrimitiveTopology& outTopology) {
  outIndexCount = 0u;
  outTopology = ShadowPrimitiveTopology::TriangleList;

  if (targetRecords.empty() ||
      (startPrimitive + targetRecords.size()) > resource.primitiveRecords.size()) {
    return false;
  }

  uint32_t firstPrimitiveType = 0u;
  bool firstPrimitiveTypeSet = false;
  bool topologyConsistent = true;
  for (size_t i = 0u; i < targetRecords.size(); ++i) {
    const auto& resourcePrimitive = resource.primitiveRecords[startPrimitive + i];
    const auto& targetPrimitive = targetRecords[i];
    if (resourcePrimitive.indexCount != targetPrimitive.indexCount)
      return false;
    if (requireTypeMatch &&
        resourcePrimitive.primitiveTypeOrMaterialSlot !=
            targetPrimitive.primitiveTypeOrMaterialSlot) {
      return false;
    }

    if (!firstPrimitiveTypeSet) {
      firstPrimitiveType = resourcePrimitive.primitiveTypeOrMaterialSlot;
      firstPrimitiveTypeSet = true;
    } else if (resourcePrimitive.primitiveTypeOrMaterialSlot != firstPrimitiveType) {
      topologyConsistent = false;
    }
    outIndexCount += resourcePrimitive.indexCount;
  }

  if (topologyConsistent && firstPrimitiveTypeSet)
    outTopology = MapPrimitiveTypeToTopology(firstPrimitiveType);
  return true;
}

bool TryResolvePrimitiveSequenceAtBaseIndex(
    const ShadowModelResourceRecord& resource, uint32_t primitiveBaseIndex,
    const std::vector<ShadowPrimitiveRecord>& targetRecords,
    bool requireTypeMatch, uint32_t& outIndexCount,
    ShadowPrimitiveTopology& outTopology) {
  outIndexCount = 0u;
  outTopology = ShadowPrimitiveTopology::TriangleList;

  uint32_t runningIndex = 0u;
  for (size_t i = 0u; i < resource.primitiveRecords.size(); ++i) {
    if (runningIndex == primitiveBaseIndex) {
      return TryMatchPrimitiveSequenceAtResourceStart(
          resource, i, targetRecords, requireTypeMatch, outIndexCount,
          outTopology);
    }

    runningIndex += resource.primitiveRecords[i].indexCount;
    if (runningIndex > primitiveBaseIndex)
      return false;
  }

  return false;
}

bool TryFindUniquePrimitiveSequenceInResource(
    const ShadowModelResourceRecord& resource,
    const std::vector<ShadowPrimitiveRecord>& targetRecords,
    bool requireTypeMatch, uint32_t& outPrimitiveBaseIndex,
    uint32_t& outIndexCount, ShadowPrimitiveTopology& outTopology) {
  outPrimitiveBaseIndex = 0u;
  outIndexCount = 0u;
  outTopology = ShadowPrimitiveTopology::TriangleList;

  if (targetRecords.empty() ||
      targetRecords.size() > resource.primitiveRecords.size()) {
    return false;
  }

  uint32_t runningIndex = 0u;
  uint32_t matchCount = 0u;
  for (size_t start = 0u; start < resource.primitiveRecords.size(); ++start) {
    uint32_t matchedIndexCount = 0u;
    ShadowPrimitiveTopology matchedTopology =
        ShadowPrimitiveTopology::TriangleList;
    if (TryMatchPrimitiveSequenceAtResourceStart(
            resource, start, targetRecords, requireTypeMatch, matchedIndexCount,
            matchedTopology)) {
      ++matchCount;
      if (matchCount > 1u)
        return false;
      outPrimitiveBaseIndex = runningIndex;
      outIndexCount = matchedIndexCount;
      outTopology = matchedTopology;
    }

    runningIndex += resource.primitiveRecords[start].indexCount;
  }

  return matchCount == 1u;
}

bool TryResolveMeshPrimaryDynamicStream(
    const ShadowRenderableRecord& renderable,
    const ShadowModelResourceRecord& resource,
    const void*& outStreamPtr,
    uint64_t& outDynamicHash,
    uint32_t& outStrideUsed) {
  outStreamPtr = nullptr;
  outDynamicHash = 0u;
  outStrideUsed = 0u;

  if (renderable.meshData == nullptr ||
      ShouldSkipLegacyMeshDataDecode(renderable))
    return false;

  const uint32_t vertexCount =
      resource.vertexCount != 0u
          ? resource.vertexCount
          : uint32_t(resource.positions.size() / 3u);
  if (vertexCount == 0u || vertexCount > 200000u)
    return false;

  void* primaryStreamPtr = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                   dxvk::war3::MeshDataOffsets::PrimaryStreamPtr,
                                   primaryStreamPtr) ||
      primaryStreamPtr == nullptr) {
    return false;
  }

  uint32_t declaredStride = 0u;
  uint32_t primaryArg0 = 0u;
  dxvk::war3::SafeReadU32Fast(renderable.meshData,
                              dxvk::war3::MeshDataOffsets::PrimaryStreamStride,
                              declaredStride);
  dxvk::war3::SafeReadU32Fast(renderable.meshData,
                              dxvk::war3::MeshDataOffsets::PrimaryStreamArg0,
                              primaryArg0);

  const std::array<uint32_t, 8> candidateStrides = {
      declaredStride, primaryArg0, 12u, 16u, 20u, 24u, 28u, 32u};
  std::array<bool, 129> seenStride = {};
  for (uint32_t stride : candidateStrides) {
    if (stride < 12u || stride >= seenStride.size())
      continue;
    if (seenStride[stride])
      continue;
    seenStride[stride] = true;

    const uint64_t requiredBytes =
        uint64_t(vertexCount - 1u) * uint64_t(stride) + 12u;
    if (requiredBytes > (64ull * 1024ull * 1024ull))
      continue;
    if (!dxvk::war3::IsReadableRange(primaryStreamPtr, size_t(requiredBytes)))
      continue;

    const auto* base = reinterpret_cast<const uint8_t*>(primaryStreamPtr);
    uint64_t hash = bit::fnv1a_init();
    bool valid = true;
    const uint32_t sampleCount = (std::min)(vertexCount, 32u);
    for (uint32_t i = 0u; i < sampleCount; ++i) {
      float xyz[3] = {};
      std::memcpy(xyz, base + size_t(i) * size_t(stride), sizeof(xyz));
      if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) ||
          !std::isfinite(xyz[2]) || std::fabs(xyz[0]) > 1000000.0f ||
          std::fabs(xyz[1]) > 1000000.0f ||
          std::fabs(xyz[2]) > 1000000.0f) {
        valid = false;
        break;
      }

      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(xyz[0]));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(xyz[1]));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(xyz[2]));
    }

    if (!valid)
      continue;

    hash = bit::fnv1a_iter(hash, reinterpret_cast<uintptr_t>(primaryStreamPtr));
    hash = bit::fnv1a_iter(hash, vertexCount);
    hash = bit::fnv1a_iter(hash, stride);
    outStreamPtr = primaryStreamPtr;
    outDynamicHash = hash;
    outStrideUsed = stride;
    return true;
  }

  return false;
}

bool TryResolveMeshDynamicIndexStream(const ShadowRenderableRecord& renderable,
                                      const ShadowModelResourceRecord& resource,
                                      const uint16_t*& outIndexStream,
                                      uint32_t& outIndexCount,
                                      uint32_t& outPrimitiveBaseIndex,
                                      uint64_t& outIndexHash,
                                      ShadowPrimitiveTopology& outTopology) {
  outIndexStream = nullptr;
  outIndexCount = 0u;
  outPrimitiveBaseIndex = 0u;
  outIndexHash = 0u;
  outTopology = ShadowPrimitiveTopology::TriangleList;

  if (renderable.meshData == nullptr || resource.indices.empty())
    return false;

  if (RenderableUsesDirectGeosetData(renderable)) {
    return TryResolveDirectGeosetVisibleIndexStream(
        renderable, resource, outIndexStream, outIndexCount,
        outPrimitiveBaseIndex, outIndexHash, outTopology);
  }

  if (ShouldSkipLegacyMeshDataDecode(renderable))
    return false;

  // `meshData + 0xE0` 在当前逆向结论里是 primitive base index / prepared
  // backing 的起点，不是 index buffer 指针。之前把它当指针会让单位 path
  // 经常退回“整 geoset index”或直接读错地址，表现为撕裂/方块状阴影。
  uint32_t primitiveBaseIndex = 0u;
  if (!dxvk::war3::SafeReadU32Fast(
          renderable.meshData, dxvk::war3::MeshDataOffsets::PrimitiveBaseIndex,
          primitiveBaseIndex)) {
    return false;
  }
  outPrimitiveBaseIndex = primitiveBaseIndex;

  std::vector<ShadowPrimitiveRecord> subPrimitiveRecords;
  const bool hasSubPrimitiveRecords =
      TryReadMeshSubPrimitiveRecords(renderable, subPrimitiveRecords);

  uint32_t indexCount = 0u;
  ShadowPrimitiveTopology resolvedTopology =
      ShadowPrimitiveTopology::TriangleList;
  bool resolvedByPrimitiveSequence = false;
  if (hasSubPrimitiveRecords && !resource.primitiveRecords.empty()) {
    const bool preferTypeMatch =
        LooksLikePrimitiveTypeSequence(subPrimitiveRecords);
    auto tryResolveBySequence = [&](bool requireTypeMatch) -> bool {
      if (primitiveBaseIndex < resource.indices.size() &&
          TryResolvePrimitiveSequenceAtBaseIndex(
              resource, primitiveBaseIndex, subPrimitiveRecords, requireTypeMatch,
              indexCount, resolvedTopology)) {
        outPrimitiveBaseIndex = primitiveBaseIndex;
        return true;
      }

      uint32_t matchedPrimitiveBaseIndex = 0u;
      uint32_t matchedIndexCount = 0u;
      ShadowPrimitiveTopology matchedTopology =
          ShadowPrimitiveTopology::TriangleList;
      if (TryFindUniquePrimitiveSequenceInResource(
              resource, subPrimitiveRecords, requireTypeMatch,
              matchedPrimitiveBaseIndex, matchedIndexCount, matchedTopology)) {
        outPrimitiveBaseIndex = matchedPrimitiveBaseIndex;
        indexCount = matchedIndexCount;
        resolvedTopology = matchedTopology;
        return true;
      }

      return false;
    };

    resolvedByPrimitiveSequence =
        tryResolveBySequence(preferTypeMatch) ||
        (preferTypeMatch && tryResolveBySequence(false));
  }

  if (!resolvedByPrimitiveSequence) {
    if (!hasSubPrimitiveRecords)
      return false;

    if (hasSubPrimitiveRecords) {
      for (const auto& primitive : subPrimitiveRecords)
        indexCount += primitive.indexCount;
    }

    if (primitiveBaseIndex >= resource.indices.size()) {
      // live unit samples已经反复证明 `meshData + 0xE0` 在部分 prepared
      // primitive path 上会给出垃圾大值；此时若 slice count 本身仍可信，
      // 与其整条动态 index contract 失效，不如回退到 base=0 的保守 slice。
      if (indexCount != 0u && indexCount <= resource.indices.size()) {
        outPrimitiveBaseIndex = 0u;
      } else if (resource.primitiveRecords.size() == 1u &&
                 resource.primitiveRecords[0].indexCount != 0u &&
                 resource.primitiveRecords[0].indexCount <=
                     resource.indices.size()) {
        outPrimitiveBaseIndex = 0u;
        indexCount = resource.primitiveRecords[0].indexCount;
        resolvedTopology = MapPrimitiveTypeToTopology(
            resource.primitiveRecords[0].primitiveTypeOrMaterialSlot);
      } else {
        return false;
      }
    }
  }

  const uint32_t availableIndexCount =
      uint32_t(resource.indices.size() - size_t(outPrimitiveBaseIndex));
  if (indexCount == 0u)
    return false;
  if (indexCount > availableIndexCount) {
    if (resolvedByPrimitiveSequence)
      return false;
    indexCount = availableIndexCount;
  }
  if (indexCount == 0u || indexCount > (1u << 20))
    return false;

  outIndexStream = resource.indices.data() + outPrimitiveBaseIndex;
  outIndexCount = indexCount;
  if (resolvedByPrimitiveSequence) {
    outTopology = resolvedTopology;
  } else {
    TryInferDynamicTopologyFromPrimitiveRecords(resource, outPrimitiveBaseIndex,
                                                indexCount, outTopology);
  }

  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, outPrimitiveBaseIndex);
  hash = bit::fnv1a_iter(hash, indexCount);
  hash = bit::fnv1a_iter(hash, uint32_t(outTopology));
  hash = bit::fnv1a_iter(hash,
                         reinterpret_cast<uintptr_t>(resource.indices.data() +
                                                    outPrimitiveBaseIndex));
  hash = bit::fnv1a_iter(hash, resolvedByPrimitiveSequence ? 1u : 0u);
  if (hasSubPrimitiveRecords) {
    hash = bit::fnv1a_iter(hash, uint32_t(subPrimitiveRecords.size()));
    for (const auto& primitive : subPrimitiveRecords) {
      hash = bit::fnv1a_iter(hash, primitive.primitiveTypeOrMaterialSlot);
      hash = bit::fnv1a_iter(hash, primitive.indexCount);
    }
  }
  const uint32_t sampleCount = (std::min)(indexCount, 32u);
  for (uint32_t i = 0u; i < sampleCount; ++i)
    hash = bit::fnv1a_iter(hash, uint32_t(outIndexStream[i]));
  if (indexCount > sampleCount) {
    const uint32_t tailStart = indexCount - sampleCount;
    for (uint32_t i = tailStart; i < indexCount; ++i)
      hash = bit::fnv1a_iter(hash, uint32_t(outIndexStream[i]));
  }
  outIndexHash = hash;
  return true;
}

bool TryResolveMeshDynamicGroupSlots(const ShadowRenderableRecord& renderable,
                                     uint32_t vertexCount,
                                     uint32_t maxAllowedGroupSlot,
                                     std::vector<uint8_t>& outGroupSlots,
                                     uint32_t& outMaxGroupSlot,
                                     uint64_t& outDynamicHash,
                                     uint32_t& outStrideUsed,
                                     uint32_t& outOffsetUsed,
                                     const MeshLayerBindingContract* layerContract,
                                     bool& outUsedPrimaryStream,
                                     int& outAuxEntryIndexUsed) {
  outGroupSlots.clear();
  outMaxGroupSlot = 0u;
  outDynamicHash = 0u;
  outStrideUsed = 0u;
  outOffsetUsed = 0u;
  outUsedPrimaryStream = false;
  outAuxEntryIndexUsed = -1;

  if (renderable.meshData == nullptr ||
      ShouldSkipLegacyMeshDataDecode(renderable) ||
      vertexCount == 0u || vertexCount > 200000u)
    return false;

  const auto candidates =
      CollectDynamicAuxStreamCandidates(renderable, layerContract);
  if (candidates.empty())
    return false;

  for (const auto& candidate : candidates) {
    if (candidate.ptr == nullptr)
      continue;

    std::array<bool, 257> seenStride = {};
    for (uint32_t stride : candidate.strides) {
      // Current-draw group-slot contracts are not always packed dwords. Some
      // unit paths expose a 1-byte-per-vertex slot stream, so semantic shadow
      // must treat R8 and packed auxiliary layouts as first-class candidates.
      if (stride == 0u || stride >= seenStride.size())
        continue;
      if (seenStride[stride])
        continue;
      seenStride[stride] = true;

      const uint64_t requiredBytes =
          uint64_t(vertexCount - 1u) * uint64_t(stride) + 1u;
      if (requiredBytes > (64ull * 1024ull * 1024ull))
        continue;
      if (!dxvk::war3::IsReadableRange(candidate.ptr, size_t(requiredBytes)))
        continue;

      const auto* base = reinterpret_cast<const uint8_t*>(candidate.ptr);
      bool hasBestOffset = false;
      uint32_t bestOffset = 0u;
      uint32_t bestUniqueCount = 0u;
      uint32_t bestSampleMaxGroupSlot = 0u;
      const uint32_t sampleCount = (std::min)(vertexCount, 64u);

      const uint32_t maxOffset =
          (std::min)(stride - 1u, kMaxDynamicFieldOffsetScan);
      for (uint32_t offset = 0u; offset <= maxOffset; ++offset) {
        bool valid = true;
        uint32_t sampleMaxGroupSlot = 0u;
        uint32_t sampleUniqueCount = 0u;
        std::array<bool, 256> seenGroupSlots = {};
        for (uint32_t i = 0u; i < sampleCount; ++i) {
          const uint32_t groupSlot =
              uint32_t(base[size_t(i) * size_t(stride) + size_t(offset)]);
          if (groupSlot > maxAllowedGroupSlot) {
            valid = false;
            break;
          }
          sampleMaxGroupSlot = (std::max)(sampleMaxGroupSlot, groupSlot);
          if (!seenGroupSlots[groupSlot]) {
            seenGroupSlots[groupSlot] = true;
            sampleUniqueCount++;
          }
        }

        if (!valid)
          continue;

        if (!hasBestOffset || sampleUniqueCount > bestUniqueCount ||
            (sampleUniqueCount == bestUniqueCount &&
             sampleMaxGroupSlot > bestSampleMaxGroupSlot)) {
          hasBestOffset = true;
          bestOffset = offset;
          bestUniqueCount = sampleUniqueCount;
          bestSampleMaxGroupSlot = sampleMaxGroupSlot;
        }
      }

      if (!hasBestOffset ||
          (bestUniqueCount <= 1u && bestSampleMaxGroupSlot == 0u))
        continue;

      std::vector<uint8_t> groupSlots(vertexCount, 0u);
      uint64_t hash = bit::fnv1a_init();
      uint32_t maxGroupSlot = 0u;
      for (uint32_t i = 0u; i < vertexCount; ++i) {
        const uint8_t groupSlot =
            base[size_t(i) * size_t(stride) + size_t(bestOffset)];
        if (groupSlot > maxAllowedGroupSlot) {
          groupSlots.clear();
          maxGroupSlot = 0u;
          hash = 0u;
          break;
        }
        groupSlots[i] = groupSlot;
        maxGroupSlot = (std::max)(maxGroupSlot, uint32_t(groupSlot));
        hash = bit::fnv1a_iter(hash, uint32_t(groupSlot));
      }

      if (groupSlots.empty() || hash == 0u)
        continue;

      hash = bit::fnv1a_iter(hash, reinterpret_cast<uintptr_t>(candidate.ptr));
      hash = bit::fnv1a_iter(hash, vertexCount);
      hash = bit::fnv1a_iter(hash, stride);
      hash = bit::fnv1a_iter(hash, bestOffset);
      hash = bit::fnv1a_iter(hash, candidate.usesPrimaryStream ? 1u : 0u);
      outGroupSlots = std::move(groupSlots);
      outMaxGroupSlot = (std::max)(maxGroupSlot, bestSampleMaxGroupSlot);
      outDynamicHash = hash;
      outStrideUsed = stride;
      outOffsetUsed = bestOffset;
      outUsedPrimaryStream = candidate.usesPrimaryStream;
      outAuxEntryIndexUsed = candidate.auxEntryIndex;
      return true;
    }
  }

  return false;
}

struct DynamicTuplePaletteResult {
  std::vector<uint8_t> groupSlots;
  std::vector<Matrix4> runtimeGroupPalette;
  uint32_t maxGroupSlot = 0u;
  uint64_t dynamicHash = 0u;
  uint32_t strideUsed = 0u;
  uint32_t offsetUsed = 0u;
  uint32_t stagePresetBaseBias = 0u;
  bool usesAveraging = false;
  bool usedPrimaryStream = false;
  int auxEntryIndexUsed = -1;
};

struct ExplicitBlendSkinResult {
  std::vector<std::array<float, 3>> weights;
  std::vector<std::array<uint8_t, 4>> indices;
  std::vector<Matrix4> runtimeGroupPalette;
  uint32_t maxGroupSlot = 0u;
  uint64_t dynamicHash = 0u;
  uint32_t stream1StrideUsed = 0u;
  uint32_t stream1OffsetUsed = 0u;
  uint32_t auxStrideUsed = 0u;
  uint32_t stagePresetBaseBias = 0u;
  uint8_t blendCount = 0u;
  bool usedSpanRemap = false;
};

struct CompactRemapSpanTable {
  const uint8_t* table = nullptr;
  uint32_t span = 0u;
  uint32_t flag = 0u;
  uint32_t ownerWord = 0u;
  uint32_t ownerWordTag = 0u;
};

struct PendingCompactRemapNode {
  uint32_t ptrWord = 0u;
  uint32_t spanHint = 0u;
  uint32_t ownerWord = 0u;
  uint32_t tag = 0u;
  uint32_t depth = 0u;
};

constexpr uint32_t kMaxCompactRemapDepth = 4u;
constexpr uint32_t kMaxCompactRemapCandidates = 32u;
constexpr uint32_t kMaxCompactRemapInlineScan = 0x60u;

bool IsPlausibleCompactRemapSpan(uint32_t span) {
  return span >= 2u && span <= 0x10000u;
}

int ScoreCompactRemapTableForExplicitBlend(
    const CompactRemapSpanTable& remap,
    const MeshLayerBindingContract* layerContract) {
  int score = 0;
  if (layerContract != nullptr) {
    if (remap.ownerWord == layerContract->layerStateWord1C)
      score += 100;
    else if (remap.ownerWord == layerContract->layerStateWord20)
      score += 50;
  }

  switch (remap.ownerWordTag) {
    case 0x02Cu:
      score += 40;
      break;
    case 0x034u:
      score += 36;
      break;
    case 0x130u:
      score += 32;
      break;
    case 0x138u:
      score += 28;
      break;
    case 0x030u:
      score += 24;
      break;
    case 0x134u:
      score += 22;
      break;
    case 0x13Cu:
      score += 18;
      break;
    default:
      break;
  }

  if (remap.flag != 0u)
    score += 4;
  if (remap.span <= 0x100u)
    score += 2;
  return score;
}

std::vector<CompactRemapSpanTable> MakePrioritizedCompactRemapTables(
    const std::vector<CompactRemapSpanTable>& remapTables,
    const MeshLayerBindingContract* layerContract) {
  std::vector<CompactRemapSpanTable> prioritized = remapTables;
  std::stable_sort(
      prioritized.begin(), prioritized.end(),
      [&](const CompactRemapSpanTable& a, const CompactRemapSpanTable& b) {
        return ScoreCompactRemapTableForExplicitBlend(a, layerContract) >
               ScoreCompactRemapTableForExplicitBlend(b, layerContract);
      });
  return prioritized;
}

std::vector<uint32_t> CollectStagePresetBaseBiasCandidates(
    const MeshLayerBindingContract* layerContract, uint32_t posePaletteLimit) {
  std::vector<uint32_t> out;
  if (layerContract == nullptr || posePaletteLimit == 0u)
    return out;

  auto appendUnique = [&](uint32_t baseBias) {
    if (baseBias == 0u || baseBias >= posePaletteLimit)
      return;
    for (uint32_t existing : out) {
      if (existing == baseBias)
        return;
    }
    out.push_back(baseBias);
  };

  const uint64_t spanBase =
      uint64_t(layerContract->sceneStagePresetBaseIndex) +
      uint64_t(layerContract->stagePresetSpanBaseIndex);
  if (spanBase < posePaletteLimit)
    appendUnique(uint32_t(spanBase));

  auto appendResolvedBase = [&](uint32_t resolvedIndex, uint32_t localIndex) {
    if (localIndex >= 0x80000000u || resolvedIndex >= posePaletteLimit ||
        resolvedIndex < localIndex) {
      return;
    }
    appendUnique(resolvedIndex - localIndex);
  };

  appendResolvedBase(layerContract->resolvedStagePresetIndex0,
                     layerContract->stagePresetIndex0);
  appendResolvedBase(layerContract->resolvedStagePresetIndex1,
                     layerContract->stagePresetIndex1);
  return out;
}

void SortCompactSlots(std::array<uint8_t, 4>& slots, uint32_t count) {
  count = (std::min)(count, uint32_t(slots.size()));
  for (uint32_t i = 0u; i + 1u < count; ++i) {
    for (uint32_t j = i + 1u; j < count; ++j) {
      if (slots[j] < slots[i])
        std::swap(slots[i], slots[j]);
    }
  }
}

std::vector<CompactRemapSpanTable> CollectCompactRemapSpanTables(
    const MeshLayerBindingContract* layerContract) {
  std::vector<CompactRemapSpanTable> out;
  out.reserve(16u);
  std::vector<PendingCompactRemapNode> pending;
  pending.reserve(24u);

  auto appendUnique = [&](const CompactRemapSpanTable& candidate) {
    if (candidate.table == nullptr || candidate.span < 2u)
      return;
    for (const auto& existing : out) {
      if (existing.table == candidate.table && existing.span == candidate.span)
        return;
    }
    out.push_back(candidate);
  };

  auto appendTablePtr = [&](uint32_t tableWord, uint32_t span, uint32_t flag,
                            uint32_t ownerWord, uint32_t tableTag) {
    if (!LooksLikeDynamicAuxPointerCandidate(tableWord) ||
        !IsPlausibleCompactRemapSpan(span)) {
      return;
    }

    const auto* tablePtr =
        reinterpret_cast<const uint8_t*>(uintptr_t(tableWord));
    if (!dxvk::war3::IsReadableRange(tablePtr, size_t(span)))
      return;

    appendUnique(
        CompactRemapSpanTable{tablePtr, span, flag, ownerWord, tableTag});
  };

  auto appendInlineTablePtr = [&](const uint8_t* tablePtr, uint32_t span,
                                  uint32_t flag, uint32_t ownerWord,
                                  uint32_t tableTag) {
    if (tablePtr == nullptr || !IsPlausibleCompactRemapSpan(span) ||
        !dxvk::war3::IsReadableRange(tablePtr, size_t(span))) {
      return;
    }

    appendUnique(
        CompactRemapSpanTable{tablePtr, span, flag, ownerWord, tableTag});
  };

  auto enqueueNode = [&](uint32_t ptrWord, uint32_t spanHint, uint32_t ownerWord,
                         uint32_t tag, uint32_t depth) {
    if (!LooksLikeDynamicAuxPointerCandidate(ptrWord) ||
        depth > kMaxCompactRemapDepth ||
        pending.size() >= kMaxCompactRemapCandidates) {
      return;
    }

    for (const auto& existing : pending) {
      if (existing.ptrWord == ptrWord && existing.spanHint == spanHint)
        return;
    }

    pending.push_back(
        PendingCompactRemapNode{ptrWord, spanHint, ownerWord, tag, depth});
  };

  auto processNode = [&](const PendingCompactRemapNode& node) {
    if (!LooksLikeDynamicAuxPointerCandidate(node.ptrWord))
      return;

    const auto* nodePtr =
        reinterpret_cast<const void*>(uintptr_t(node.ptrWord));
    if (IsPlausibleCompactRemapSpan(node.spanHint) &&
        dxvk::war3::IsReadableRange(nodePtr, size_t(node.spanHint))) {
      appendUnique(CompactRemapSpanTable{
          reinterpret_cast<const uint8_t*>(nodePtr), node.spanHint, 0u,
          node.ownerWord, node.tag});
    }

    uint32_t hopWord0 = 0u;
    uint32_t hopWord4 = 0u;
    uint32_t hopWord8 = 0u;
    if (!TryReadPointerWordWindow(nodePtr, hopWord0, hopWord4, hopWord8))
      return;

    uint32_t head10 = 0u;
    uint32_t head14 = 0u;
    uint32_t head18 = 0u;
    uint32_t head1C = 0u;
    const bool hasHead10 =
        TryReadLinearWordSample(nodePtr, 0x10u, head10);
    const bool hasHead14 =
        TryReadLinearWordSample(nodePtr, 0x14u, head14);
    const bool hasHead18 =
        TryReadLinearWordSample(nodePtr, 0x18u, head18);
    const bool hasHead1C =
        TryReadLinearWordSample(nodePtr, 0x1Cu, head1C);

    // `l1cHop` 现在已经被 live 样本证实更像 descriptor/object header，
    // 不是最终 remap byte table。这里要把 descriptor 自身的 inline body
    // 当候选，而不是只沿着头部的 pointer 再往外追。
    if (IsPlausibleCompactRemapSpan(hopWord4)) {
      appendInlineTablePtr(reinterpret_cast<const uint8_t*>(nodePtr) + 0x10u,
                           hopWord4, hopWord8, node.ownerWord,
                           node.tag + 0x010u);
      appendInlineTablePtr(reinterpret_cast<const uint8_t*>(nodePtr) + 0x18u,
                           hopWord4, hopWord8, node.ownerWord,
                           node.tag + 0x018u);
    }

    if (hasHead10 && hasHead14) {
      if (LooksLikeDynamicAuxPointerCandidate(head10) &&
          IsPlausibleCompactRemapSpan(head14)) {
        appendTablePtr(head10, head14, hasHead18 ? head18 : 0u,
                       node.ownerWord, node.tag + 0x110u);
      }
      if (IsPlausibleCompactRemapSpan(head14)) {
        appendInlineTablePtr(reinterpret_cast<const uint8_t*>(nodePtr) + 0x10u,
                             head14, hasHead18 ? head18 : 0u,
                             node.ownerWord, node.tag + 0x114u);
      }
    }

    if (hasHead18 && hasHead1C) {
      if (LooksLikeDynamicAuxPointerCandidate(head18) &&
          IsPlausibleCompactRemapSpan(head1C)) {
        appendTablePtr(head18, head1C, hasHead10 ? head10 : 0u,
                       node.ownerWord, node.tag + 0x118u);
      }
      if (IsPlausibleCompactRemapSpan(head1C)) {
        appendInlineTablePtr(reinterpret_cast<const uint8_t*>(nodePtr) + 0x18u,
                             head1C, hasHead10 ? head10 : 0u,
                             node.ownerWord, node.tag + 0x11Cu);
      }
    }

    const uint32_t span0 =
        IsPlausibleCompactRemapSpan(hopWord4) ? hopWord4 : node.spanHint;
    const uint32_t span4 =
        IsPlausibleCompactRemapSpan(hopWord8) ? hopWord8 : node.spanHint;
    const uint32_t span8 = node.spanHint;

    if (LooksLikeDynamicAuxPointerCandidate(hopWord0)) {
      appendTablePtr(hopWord0, span0, hopWord8, node.ownerWord,
                     node.tag + 0x100u);
      enqueueNode(hopWord0, span0, node.ownerWord, node.tag + 0x100u,
                  node.depth + 1u);
    }
    if (LooksLikeDynamicAuxPointerCandidate(hopWord4)) {
      appendTablePtr(hopWord4, span4, hopWord0, node.ownerWord,
                     node.tag + 0x104u);
      enqueueNode(hopWord4, span4, node.ownerWord, node.tag + 0x104u,
                  node.depth + 1u);
    }
    if (LooksLikeDynamicAuxPointerCandidate(hopWord8)) {
      appendTablePtr(hopWord8, span8, hopWord4, node.ownerWord,
                     node.tag + 0x108u);
      enqueueNode(hopWord8, span8, node.ownerWord, node.tag + 0x108u,
                  node.depth + 1u);
    }
  };

  if (layerContract != nullptr) {
    enqueueNode(layerContract->layerStateWord1C, 0u,
                layerContract->layerStateWord1C, 0x1Cu, 0u);
    enqueueNode(layerContract->layerStateWord20, 0u,
                layerContract->layerStateWord20, 0x20u, 0u);
  }

  for (size_t i = 0u;
       i < pending.size() && out.size() < kMaxCompactRemapCandidates; ++i) {
    processNode(pending[i]);
  }

  return out;
}

bool TryBuildPackedTupleKeyWithSpanRemap(
    const uint8_t raw[4], const CompactRemapSpanTable& remap,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  if (remap.table == nullptr || remap.span == 0u)
    return false;

  auto pushUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (uint32_t(slot) >= remap.span)
      return false;
    const uint8_t mapped = remap.table[slot];
    if (uint32_t(mapped) >= posePaletteLimit)
      return false;
    if (mapped == 0u && !allowZero)
      return true;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == mapped)
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = mapped;
    return true;
  };

  if (!pushUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = remap.table[raw[0]] == 0u;
    if (!pushUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;

  SortCompactSlots(outSlots, outCount);
  return true;
}

bool TryBuildPackedTupleKeyWithSpanRemapAndBaseBias(
    const uint8_t raw[4], const CompactRemapSpanTable& remap, uint32_t baseBias,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  if (baseBias == 0u || remap.table == nullptr || remap.span == 0u)
    return false;

  auto pushUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (uint32_t(slot) >= remap.span)
      return false;
    const uint8_t mappedLocal = remap.table[slot];
    if (mappedLocal == 0u && !allowZero)
      return true;
    const uint32_t mapped = baseBias + uint32_t(mappedLocal);
    if (mapped >= posePaletteLimit)
      return false;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == uint8_t(mapped))
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = uint8_t(mapped);
    return true;
  };

  if (!pushUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = remap.table[raw[0]] == 0u;
    if (!pushUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;

  SortCompactSlots(outSlots, outCount);
  return true;
}

bool TryBuildPackedTupleKeyWithSpanRemapWindow(
    const uint8_t raw[4], const CompactRemapSpanTable& remap,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  if (TryBuildPackedTupleKeyWithSpanRemap(
          raw, remap, posePaletteLimit, maxExpectedGroupSize, outSlots,
          outCount)) {
    return true;
  }

  if (remap.table == nullptr || remap.span == 0u)
    return false;

  for (uint32_t offset = 1u; offset <= kMaxCompactRemapInlineScan; ++offset) {
    const auto* tablePtr = remap.table + offset;
    if (!dxvk::war3::IsReadableRange(tablePtr, size_t(remap.span)))
      break;

    CompactRemapSpanTable inlineRemap = remap;
    inlineRemap.table = tablePtr;
    if (TryBuildPackedTupleKeyWithSpanRemap(
            raw, inlineRemap, posePaletteLimit, maxExpectedGroupSize, outSlots,
            outCount)) {
      return true;
    }
  }

  return false;
}

bool TryBuildPackedTupleKeyWithSpanRemapWindowAndBaseBias(
    const uint8_t raw[4], const CompactRemapSpanTable& remap, uint32_t baseBias,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  if (TryBuildPackedTupleKeyWithSpanRemapAndBaseBias(
          raw, remap, baseBias, posePaletteLimit, maxExpectedGroupSize,
          outSlots, outCount)) {
    return true;
  }

  if (baseBias == 0u || remap.table == nullptr || remap.span == 0u)
    return false;

  for (uint32_t offset = 1u; offset <= kMaxCompactRemapInlineScan; ++offset) {
    const auto* tablePtr = remap.table + offset;
    if (!dxvk::war3::IsReadableRange(tablePtr, size_t(remap.span)))
      break;

    CompactRemapSpanTable inlineRemap = remap;
    inlineRemap.table = tablePtr;
    if (TryBuildPackedTupleKeyWithSpanRemapAndBaseBias(
            raw, inlineRemap, baseBias, posePaletteLimit,
            maxExpectedGroupSize, outSlots, outCount)) {
      return true;
    }
  }

  return false;
}

bool TryBuildOrderedTupleSlotsWithSpanRemap(
    const uint8_t raw[4], const CompactRemapSpanTable& remap,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  if (remap.table == nullptr || remap.span == 0u)
    return false;

  auto pushOrderedUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (uint32_t(slot) >= remap.span)
      return false;
    const uint8_t mapped = remap.table[slot];
    if (uint32_t(mapped) >= posePaletteLimit)
      return false;
    if (mapped == 0u && !allowZero)
      return true;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == mapped)
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = mapped;
    return true;
  };

  if (!pushOrderedUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = remap.table[raw[0]] == 0u;
    if (!pushOrderedUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;
  return true;
}

bool TryBuildOrderedTupleSlotsWithSpanRemapAndBaseBias(
    const uint8_t raw[4], const CompactRemapSpanTable& remap, uint32_t baseBias,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  if (baseBias == 0u || remap.table == nullptr || remap.span == 0u)
    return false;

  auto pushOrderedUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (uint32_t(slot) >= remap.span)
      return false;
    const uint8_t mappedLocal = remap.table[slot];
    if (mappedLocal == 0u && !allowZero)
      return true;
    const uint32_t mapped = baseBias + uint32_t(mappedLocal);
    if (mapped >= posePaletteLimit)
      return false;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == uint8_t(mapped))
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = uint8_t(mapped);
    return true;
  };

  if (!pushOrderedUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = remap.table[raw[0]] == 0u;
    if (!pushOrderedUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;
  return true;
}

bool TryBuildOrderedTupleSlotsWithSpanRemapWindow(
    const uint8_t raw[4], const CompactRemapSpanTable& remap,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  if (TryBuildOrderedTupleSlotsWithSpanRemap(
          raw, remap, posePaletteLimit, maxExpectedGroupSize, outSlots,
          outCount)) {
    return true;
  }

  if (remap.table == nullptr || remap.span == 0u)
    return false;

  for (uint32_t offset = 1u; offset <= kMaxCompactRemapInlineScan; ++offset) {
    const auto* tablePtr = remap.table + offset;
    if (!dxvk::war3::IsReadableRange(tablePtr, size_t(remap.span)))
      break;

    CompactRemapSpanTable inlineRemap = remap;
    inlineRemap.table = tablePtr;
    if (TryBuildOrderedTupleSlotsWithSpanRemap(
            raw, inlineRemap, posePaletteLimit, maxExpectedGroupSize, outSlots,
            outCount)) {
      return true;
    }
  }

  return false;
}

bool TryBuildOrderedTupleSlotsWithSpanRemapWindowAndBaseBias(
    const uint8_t raw[4], const CompactRemapSpanTable& remap, uint32_t baseBias,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  if (TryBuildOrderedTupleSlotsWithSpanRemapAndBaseBias(
          raw, remap, baseBias, posePaletteLimit, maxExpectedGroupSize,
          outSlots, outCount)) {
    return true;
  }

  if (baseBias == 0u || remap.table == nullptr || remap.span == 0u)
    return false;

  for (uint32_t offset = 1u; offset <= kMaxCompactRemapInlineScan; ++offset) {
    const auto* tablePtr = remap.table + offset;
    if (!dxvk::war3::IsReadableRange(tablePtr, size_t(remap.span)))
      break;

    CompactRemapSpanTable inlineRemap = remap;
    inlineRemap.table = tablePtr;
    if (TryBuildOrderedTupleSlotsWithSpanRemapAndBaseBias(
            raw, inlineRemap, baseBias, posePaletteLimit,
            maxExpectedGroupSize, outSlots, outCount)) {
      return true;
    }
  }

  return false;
}

bool TryBuildPackedTupleKeyWithAnySpanRemap(
    const uint8_t raw[4], const std::vector<CompactRemapSpanTable>& remapTables,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  for (const auto& remap : remapTables) {
    if (TryBuildPackedTupleKeyWithSpanRemapWindow(
            raw, remap, posePaletteLimit, maxExpectedGroupSize, outSlots,
            outCount)) {
      return true;
    }
  }
  return false;
}

bool TryBuildPackedTupleKeyWithAnySpanRemapAndBaseBias(
    const uint8_t raw[4], const std::vector<CompactRemapSpanTable>& remapTables,
    uint32_t baseBias, uint32_t posePaletteLimit,
    uint32_t maxExpectedGroupSize, std::array<uint8_t, 4>& outSlots,
    uint32_t& outCount) {
  if (baseBias == 0u)
    return false;
  for (const auto& remap : remapTables) {
    if (TryBuildPackedTupleKeyWithSpanRemapWindowAndBaseBias(
            raw, remap, baseBias, posePaletteLimit, maxExpectedGroupSize,
            outSlots, outCount)) {
      return true;
    }
  }
  return false;
}

bool TryBuildOrderedTupleSlotsWithAnySpanRemap(
    const uint8_t raw[4], const std::vector<CompactRemapSpanTable>& remapTables,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    std::array<uint8_t, 4>& outSlots, uint32_t& outCount) {
  for (const auto& remap : remapTables) {
    if (TryBuildOrderedTupleSlotsWithSpanRemapWindow(
            raw, remap, posePaletteLimit, maxExpectedGroupSize, outSlots,
            outCount)) {
      return true;
    }
  }
  return false;
}

bool TryBuildOrderedTupleSlotsWithAnySpanRemapAndBaseBias(
    const uint8_t raw[4], const std::vector<CompactRemapSpanTable>& remapTables,
    uint32_t baseBias, uint32_t posePaletteLimit,
    uint32_t maxExpectedGroupSize, std::array<uint8_t, 4>& outSlots,
    uint32_t& outCount) {
  if (baseBias == 0u)
    return false;
  for (const auto& remap : remapTables) {
    if (TryBuildOrderedTupleSlotsWithSpanRemapWindowAndBaseBias(
            raw, remap, baseBias, posePaletteLimit, maxExpectedGroupSize,
            outSlots, outCount)) {
      return true;
    }
  }
  return false;
}

bool TryBuildPackedTupleKey(const uint8_t raw[4], uint32_t posePaletteLimit,
                            uint32_t maxExpectedGroupSize,
                            std::array<uint8_t, 4>& outSlots,
                            uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  auto pushUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (uint32_t(slot) >= posePaletteLimit)
      return false;
    if (slot == 0u && !allowZero)
      return true;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == slot)
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = slot;
    return true;
  };

  if (!pushUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = raw[0] == 0u;
    if (!pushUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;

  SortCompactSlots(outSlots, outCount);
  return true;
}

bool TryBuildPackedTupleKeyWithBaseBias(
    const uint8_t raw[4], uint32_t baseBias, uint32_t posePaletteLimit,
    uint32_t maxExpectedGroupSize, std::array<uint8_t, 4>& outSlots,
    uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  if (baseBias == 0u)
    return false;

  auto pushUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (slot == 0u && !allowZero)
      return true;
    const uint32_t mapped = baseBias + uint32_t(slot);
    if (mapped >= posePaletteLimit)
      return false;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == uint8_t(mapped))
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = uint8_t(mapped);
    return true;
  };

  if (!pushUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = raw[0] == 0u;
    if (!pushUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;

  SortCompactSlots(outSlots, outCount);
  return true;
}

bool TryBuildOrderedTupleSlots(const uint8_t raw[4], uint32_t posePaletteLimit,
                               uint32_t maxExpectedGroupSize,
                               std::array<uint8_t, 4>& outSlots,
                               uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  auto pushOrderedUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (uint32_t(slot) >= posePaletteLimit)
      return false;
    if (slot == 0u && !allowZero)
      return true;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == slot)
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = slot;
    return true;
  };

  if (!pushOrderedUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = raw[0] == 0u;
    if (!pushOrderedUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;
  return true;
}

bool TryBuildOrderedTupleSlotsWithBaseBias(
    const uint8_t raw[4], uint32_t baseBias, uint32_t posePaletteLimit,
    uint32_t maxExpectedGroupSize, std::array<uint8_t, 4>& outSlots,
    uint32_t& outCount) {
  outSlots.fill(0u);
  outCount = 0u;

  if (baseBias == 0u)
    return false;

  auto pushOrderedUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (slot == 0u && !allowZero)
      return true;
    const uint32_t mapped = baseBias + uint32_t(slot);
    if (mapped >= posePaletteLimit)
      return false;
    for (uint32_t i = 0u; i < outCount; ++i) {
      if (outSlots[i] == uint8_t(mapped))
        return true;
    }
    if (outCount >= outSlots.size())
      return false;
    outSlots[outCount++] = uint8_t(mapped);
    return true;
  };

  if (!pushOrderedUnique(raw[0], true))
    return false;

  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = raw[0] == 0u;
    if (!pushOrderedUnique(raw[i], allowZero))
      return false;
  }

  if (outCount == 0u)
    return false;
  if (maxExpectedGroupSize != 0u && outCount > maxExpectedGroupSize)
    return false;
  return true;
}

bool TryBuildOrderedTupleSlotsWithLocalRemap(
    const uint8_t raw[4], uint32_t posePaletteLimit,
    uint32_t maxExpectedGroupSize, std::array<uint8_t, 4>& outSlots,
    uint32_t& outCount, bool& outUsedLocalRemap) {
  outUsedLocalRemap = false;
  if (TryBuildOrderedTupleSlots(raw, posePaletteLimit, maxExpectedGroupSize,
                                outSlots, outCount)) {
    return true;
  }

  std::array<uint8_t, 4> rawUnique = {};
  uint32_t rawCount = 0u;
  auto pushRawUnique = [&](uint8_t slot, bool allowZero) -> bool {
    if (slot == 0u && !allowZero)
      return true;
    for (uint32_t i = 0u; i < rawCount; ++i) {
      if (rawUnique[i] == slot)
        return true;
    }
    if (rawCount >= rawUnique.size())
      return false;
    rawUnique[rawCount++] = slot;
    return true;
  };

  if (!pushRawUnique(raw[0], true))
    return false;
  for (uint32_t i = 1u; i < 4u; ++i) {
    const bool allowZero = raw[0] == 0u;
    if (!pushRawUnique(raw[i], allowZero))
      return false;
  }

  if (rawCount < 2u || rawCount > 3u)
    return false;
  if (maxExpectedGroupSize != 0u && rawCount > maxExpectedGroupSize)
    return false;
  if (posePaletteLimit == 0u || rawCount > posePaletteLimit || posePaletteLimit > 4u)
    return false;

  outSlots.fill(0u);
  outCount = rawCount;
  for (uint32_t i = 0u; i < rawCount; ++i)
    outSlots[i] = uint8_t(i);
  outUsedLocalRemap = true;
  return true;
}

uint64_t MakePackedTupleKey(const std::array<uint8_t, 4>& slots,
                            uint32_t count) {
  uint64_t key = uint64_t(count & 0xFFu);
  for (uint32_t i = 0u; i < slots.size(); ++i)
    key = (key << 8) | uint64_t(slots[i]);
  return key;
}

bool TryResolveMeshDynamicPackedRuntimeGroups(
    const ShadowRenderableRecord& renderable, uint32_t vertexCount,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    const ShadowPoseRecord& pose, DynamicTuplePaletteResult& outResult,
    const MeshLayerBindingContract* layerContract) {
  outResult = {};

  if (renderable.meshData == nullptr ||
      ShouldSkipLegacyMeshDataDecode(renderable) ||
      vertexCount == 0u || vertexCount > 200000u || posePaletteLimit == 0u ||
      pose.matrixPalette.empty()) {
    return false;
  }

  const auto candidates =
      CollectDynamicAuxStreamCandidates(renderable, layerContract);
  if (candidates.empty())
    return false;

  const auto remapTables = MakePrioritizedCompactRemapTables(
      CollectCompactRemapSpanTables(layerContract), layerContract);
  const bool hasRemapTable = !remapTables.empty();
  std::vector<uint32_t> stagePresetBaseBiases = {0u};
  for (uint32_t baseBias :
       CollectStagePresetBaseBiasCandidates(layerContract, posePaletteLimit)) {
    stagePresetBaseBiases.push_back(baseBias);
  }

  for (const auto& candidate : candidates) {
    if (candidate.ptr == nullptr)
      continue;

    std::array<bool, 257> seenStride = {};
    for (uint32_t stride : candidate.strides) {
      if (stride < 4u || stride >= seenStride.size())
        continue;
      if (seenStride[stride])
        continue;
      seenStride[stride] = true;

      const uint64_t requiredBytes =
          uint64_t(vertexCount - 1u) * uint64_t(stride) + 4u;
      if (requiredBytes > (64ull * 1024ull * 1024ull))
        continue;
      if (!dxvk::war3::IsReadableRange(candidate.ptr, size_t(requiredBytes)))
        continue;

      const auto* base = reinterpret_cast<const uint8_t*>(candidate.ptr);
      const uint32_t sampleCount = (std::min)(vertexCount, 64u);
      bool hasBestOffset = false;
      uint32_t bestOffset = 0u;
      uint32_t bestBaseBias = 0u;
      uint32_t bestMultiTupleSamples = 0u;
      uint32_t bestUniqueTupleCount = 0u;
      const uint32_t maxOffset =
          (std::min)(stride - 4u, kMaxDynamicFieldOffsetScan);
      for (uint32_t offset = 0u; offset <= maxOffset; ++offset) {
        for (uint32_t baseBias : stagePresetBaseBiases) {
          uint32_t multiTupleSamples = 0u;
          uint32_t uniqueTupleCount = 0u;
          std::array<uint64_t, 64> sampleTupleKeys = {};
          bool valid = true;
          for (uint32_t i = 0u; i < sampleCount; ++i) {
            std::array<uint8_t, 4> tupleSlots = {};
            uint32_t tupleCount = 0u;
            const uint8_t* tuplePtr =
                base + size_t(i) * size_t(stride) + size_t(offset);
            const bool decoded =
                baseBias == 0u
                    ? (TryBuildPackedTupleKey(
                           tuplePtr, posePaletteLimit, maxExpectedGroupSize,
                           tupleSlots, tupleCount) ||
                       (hasRemapTable &&
                        TryBuildPackedTupleKeyWithAnySpanRemap(
                            tuplePtr, remapTables, posePaletteLimit,
                            maxExpectedGroupSize, tupleSlots, tupleCount)))
                    : (TryBuildPackedTupleKeyWithBaseBias(
                           tuplePtr, baseBias, posePaletteLimit,
                           maxExpectedGroupSize, tupleSlots, tupleCount) ||
                       (hasRemapTable &&
                        TryBuildPackedTupleKeyWithAnySpanRemapAndBaseBias(
                            tuplePtr, remapTables, baseBias, posePaletteLimit,
                            maxExpectedGroupSize, tupleSlots, tupleCount)));
            if (!decoded) {
              valid = false;
              break;
            }
            if (tupleCount > 1u)
              multiTupleSamples++;
            const uint64_t tupleKey = MakePackedTupleKey(tupleSlots, tupleCount);
            bool seenTuple = false;
            for (uint32_t keyIndex = 0u; keyIndex < uniqueTupleCount;
                 ++keyIndex) {
              if (sampleTupleKeys[keyIndex] == tupleKey) {
                seenTuple = true;
                break;
              }
            }
            if (!seenTuple && uniqueTupleCount < sampleTupleKeys.size())
              sampleTupleKeys[uniqueTupleCount++] = tupleKey;
          }

          if (!valid || multiTupleSamples == 0u)
            continue;

          if (!hasBestOffset || multiTupleSamples > bestMultiTupleSamples ||
              (multiTupleSamples == bestMultiTupleSamples &&
               uniqueTupleCount > bestUniqueTupleCount) ||
              (multiTupleSamples == bestMultiTupleSamples &&
               uniqueTupleCount == bestUniqueTupleCount &&
               baseBias != 0u && bestBaseBias == 0u)) {
            hasBestOffset = true;
            bestOffset = offset;
            bestBaseBias = baseBias;
            bestMultiTupleSamples = multiTupleSamples;
            bestUniqueTupleCount = uniqueTupleCount;
          }
        }
      }

      if (!hasBestOffset)
        continue;

      std::unordered_map<uint64_t, uint8_t> paletteMap;
      std::vector<Matrix4> runtimeGroupPalette;
      std::vector<uint8_t> groupSlots(vertexCount, 0u);
      uint64_t hash = bit::fnv1a_init();
      bool valid = true;
      bool usesAveraging = false;
      for (uint32_t i = 0u; i < vertexCount; ++i) {
        std::array<uint8_t, 4> tupleSlots = {};
        uint32_t tupleCount = 0u;
        const uint8_t* tuplePtr =
            base + size_t(i) * size_t(stride) + size_t(bestOffset);
        const bool decoded =
            bestBaseBias == 0u
                ? (TryBuildPackedTupleKey(
                       tuplePtr, posePaletteLimit, maxExpectedGroupSize,
                       tupleSlots, tupleCount) ||
                   (hasRemapTable &&
                    TryBuildPackedTupleKeyWithAnySpanRemap(
                        tuplePtr, remapTables, posePaletteLimit,
                        maxExpectedGroupSize, tupleSlots, tupleCount)))
                : (TryBuildPackedTupleKeyWithBaseBias(
                       tuplePtr, bestBaseBias, posePaletteLimit,
                       maxExpectedGroupSize, tupleSlots, tupleCount) ||
                   (hasRemapTable &&
                    TryBuildPackedTupleKeyWithAnySpanRemapAndBaseBias(
                        tuplePtr, remapTables, bestBaseBias, posePaletteLimit,
                        maxExpectedGroupSize, tupleSlots, tupleCount)));
        if (!decoded) {
          valid = false;
          break;
        }

        const uint64_t tupleKey = MakePackedTupleKey(tupleSlots, tupleCount);
        auto entry = paletteMap.find(tupleKey);
        uint8_t paletteSlot = 0u;
        if (entry == paletteMap.end()) {
          if (runtimeGroupPalette.size() >= 256u) {
            valid = false;
            break;
          }

          Matrix4 accum(0.0f);
          for (uint32_t j = 0u; j < tupleCount; ++j) {
            const uint32_t poseIndex = tupleSlots[j];
            if (poseIndex >= pose.matrixPalette.size()) {
              valid = false;
              break;
            }
            accum += pose.matrixPalette[poseIndex];
          }
          if (!valid)
            break;

          if (tupleCount > 1u)
            usesAveraging = true;
          paletteSlot = uint8_t(runtimeGroupPalette.size());
          runtimeGroupPalette.push_back(tupleCount > 1u
                                            ? (accum / float(tupleCount))
                                            : accum);
          paletteMap.emplace(tupleKey, paletteSlot);
        } else {
          paletteSlot = entry->second;
        }

        groupSlots[i] = paletteSlot;
        hash = bit::fnv1a_iter(hash, tupleKey);
        hash = bit::fnv1a_iter(hash, uint32_t(paletteSlot));
      }

      if (!valid || runtimeGroupPalette.empty())
        continue;

      hash = bit::fnv1a_iter(hash, reinterpret_cast<uintptr_t>(candidate.ptr));
      hash = bit::fnv1a_iter(hash, vertexCount);
      hash = bit::fnv1a_iter(hash, stride);
      hash = bit::fnv1a_iter(hash, bestOffset);
      hash = bit::fnv1a_iter(hash, bestBaseBias);
      hash = bit::fnv1a_iter(hash, uint32_t(runtimeGroupPalette.size()));
      hash = bit::fnv1a_iter(hash, candidate.usesPrimaryStream ? 1u : 0u);
      outResult.groupSlots = std::move(groupSlots);
      outResult.runtimeGroupPalette = std::move(runtimeGroupPalette);
      outResult.maxGroupSlot =
          uint32_t(outResult.runtimeGroupPalette.size() - 1u);
      outResult.dynamicHash = hash;
      outResult.strideUsed = stride;
      outResult.offsetUsed = bestOffset;
      outResult.stagePresetBaseBias = bestBaseBias;
      outResult.usesAveraging = usesAveraging;
      outResult.usedPrimaryStream = candidate.usesPrimaryStream;
      outResult.auxEntryIndexUsed = candidate.auxEntryIndex;
      return true;
    }
  }

  return false;
}

bool TryResolveMeshDynamicExplicitBlendSkinning(
    const ShadowRenderableRecord& renderable, uint32_t vertexCount,
    uint32_t posePaletteLimit, uint32_t maxExpectedGroupSize,
    const ShadowPoseRecord& pose, const MeshLayerBindingContract* layerContract,
    ExplicitBlendSkinResult& outResult, ShadowResolveStats* ioStats = nullptr) {
  outResult = {};

  if (ioStats != nullptr)
    ioStats->explicitBlendAttempts++;

  if (renderable.meshData == nullptr ||
      ShouldSkipLegacyMeshDataDecode(renderable) ||
      layerContract == nullptr ||
      vertexCount == 0u || vertexCount > 200000u || posePaletteLimit == 0u ||
      pose.matrixPalette.empty() || layerContract->auxStreamPtr0 == 0u ||
      !layerContract->hasAuxStream0) {
    return false;
  }

  const auto candidates =
      CollectDynamicAuxStreamCandidates(renderable, layerContract);
  if (candidates.empty())
    return false;

  const uint32_t auxStride =
      layerContract->auxStreamStride0 >= 8u ? layerContract->auxStreamStride0 : 8u;
  const auto* auxBase = reinterpret_cast<const uint8_t*>(
      reinterpret_cast<const void*>(layerContract->auxStreamPtr0));
  const uint64_t auxRequiredBytes =
      uint64_t(vertexCount - 1u) * uint64_t(auxStride) + 8u;
  if (!dxvk::war3::IsReadableRange(auxBase, size_t(auxRequiredBytes)))
    return false;

  const auto remapTables = MakePrioritizedCompactRemapTables(
      CollectCompactRemapSpanTables(layerContract), layerContract);
  const bool hasRemapTable = !remapTables.empty();
  if (hasRemapTable && ioStats != nullptr)
    ioStats->explicitBlendAttemptWithSpanRemapTable++;
  std::vector<uint32_t> stagePresetBaseBiases = {0u};
  for (uint32_t baseBias :
       CollectStagePresetBaseBiasCandidates(layerContract, posePaletteLimit)) {
    stagePresetBaseBiases.push_back(baseBias);
  }
  const uint32_t sampleCount = (std::min)(vertexCount, 64u);
  bool hasBestOffset = false;
  const DynamicAuxStreamCandidate* bestCandidate = nullptr;
  uint32_t bestStride = 0u;
  uint32_t bestOffset = 0u;
  uint32_t bestStagePresetBaseBias = 0u;
  uint32_t bestScore = 0u;
  for (const auto& candidate : candidates) {
    if (candidate.ptr == nullptr)
      continue;

    const auto* candidateBase = reinterpret_cast<const uint8_t*>(candidate.ptr);
    std::array<bool, 257> seenStride = {};
    for (uint32_t stride : candidate.strides) {
      if (stride < 4u || stride >= seenStride.size())
        continue;
      if (seenStride[stride])
        continue;
      seenStride[stride] = true;

      const uint64_t requiredBytes =
          uint64_t(vertexCount - 1u) * uint64_t(stride) + 4u;
      if (!dxvk::war3::IsReadableRange(candidateBase, size_t(requiredBytes)))
        continue;

      const uint32_t maxOffset =
          (std::min)(stride - 4u, kMaxDynamicFieldOffsetScan);
      for (uint32_t offset = 0u; offset <= maxOffset; ++offset) {
        for (uint32_t stagePresetBaseBias : stagePresetBaseBiases) {
          uint32_t score = 0u;
          bool valid = true;
          for (uint32_t i = 0u; i < sampleCount; ++i) {
            std::array<uint8_t, 4> tupleSlots = {};
            uint32_t tupleCount = 0u;
            bool usedLocalRemap = false;
            const uint8_t* tuplePtr =
                candidateBase + size_t(i) * size_t(stride) + size_t(offset);
            const bool decoded =
                stagePresetBaseBias == 0u
                    ? (TryBuildOrderedTupleSlots(
                           tuplePtr, posePaletteLimit, maxExpectedGroupSize,
                           tupleSlots, tupleCount) ||
                       (hasRemapTable &&
                        TryBuildOrderedTupleSlotsWithAnySpanRemap(
                            tuplePtr, remapTables, posePaletteLimit,
                            maxExpectedGroupSize, tupleSlots, tupleCount)) ||
                       TryBuildOrderedTupleSlotsWithLocalRemap(
                           tuplePtr, posePaletteLimit, maxExpectedGroupSize,
                           tupleSlots, tupleCount, usedLocalRemap))
                    : (TryBuildOrderedTupleSlotsWithBaseBias(
                           tuplePtr, stagePresetBaseBias, posePaletteLimit,
                           maxExpectedGroupSize, tupleSlots, tupleCount) ||
                       (hasRemapTable &&
                        TryBuildOrderedTupleSlotsWithAnySpanRemapAndBaseBias(
                            tuplePtr, remapTables, stagePresetBaseBias,
                            posePaletteLimit, maxExpectedGroupSize,
                            tupleSlots, tupleCount)));
            if (!decoded) {
              valid = false;
              break;
            }
            if (tupleCount < 2u || tupleCount > 3u)
              continue;

            float w0 = 0.0f;
            float w1 = 0.0f;
            std::memcpy(&w0,
                        auxBase + size_t(i) * size_t(auxStride) + 0u,
                        sizeof(w0));
            std::memcpy(&w1,
                        auxBase + size_t(i) * size_t(auxStride) + 4u,
                        sizeof(w1));
            if (!std::isfinite(w0) || !std::isfinite(w1))
              continue;

            w0 = std::clamp(w0, 0.0f, 1.0f);
            w1 = std::clamp(w1, 0.0f, 1.0f);
            const float weightSum = tupleCount >= 3u ? (w0 + w1) : w0;
            if (weightSum <= 0.001f)
              continue;
            score++;
          }

          if (!valid || score == 0u)
            continue;
          if (!hasBestOffset || score > bestScore ||
              (score == bestScore &&
               stagePresetBaseBias != 0u && bestStagePresetBaseBias == 0u)) {
            hasBestOffset = true;
            bestCandidate = &candidate;
            bestStride = stride;
            bestOffset = offset;
            bestStagePresetBaseBias = stagePresetBaseBias;
            bestScore = score;
          }
        }
      }
    }
  }

  if (!hasBestOffset || bestStride < 4u || bestCandidate == nullptr) {
    if (ioStats != nullptr)
      ioStats->explicitBlendStrideSearchMiss++;
    if (SemanticCoreTraceEnabled()) {
      static std::atomic<uint32_t> s_explicitBlendMissLogCount{0};
      const uint32_t logIndex =
          s_explicitBlendMissLogCount.fetch_add(1u, std::memory_order_relaxed);
      if (logIndex < 16u || (logIndex % 2048u) == 0u) {
      uint8_t remapHead[8] = {};
      uint32_t remapWordPairs[8] = {};
      uint32_t remapTags[4] = {};
      if (hasRemapTable && remapTables.front().table != nullptr &&
          dxvk::war3::IsReadableRange(remapTables.front().table,
                                      sizeof(remapHead))) {
        std::memcpy(remapHead, remapTables.front().table, sizeof(remapHead));
      }
      for (size_t tableIndex = 0u;
           tableIndex < remapTables.size() && tableIndex < 4u; ++tableIndex) {
        remapTags[tableIndex] = remapTables[tableIndex].ownerWordTag;
        uint32_t word0 = 0u;
        uint32_t word4 = 0u;
        uint32_t word8 = 0u;
        if (TryReadPointerWordWindow(remapTables[tableIndex].table, word0,
                                     word4, word8)) {
          remapWordPairs[tableIndex * 2u + 0u] = word0;
          remapWordPairs[tableIndex * 2u + 1u] = word4;
        }
      }
      dxvk::war3dbg::Print(
          "DXVK SemanticCore: explicit-blend miss runtime=%p mesh=%p geoIdx=%u "
          "poseLimit=%u maxGroup=%u candidates=%zu remap=%d remapTables=%zu "
          "span=%u flag=%u tags=%08X/%08X/%08X/%08X "
          "tables=%08X/%08X|%08X/%08X|%08X/%08X|%08X/%08X "
          "head=%02X %02X %02X %02X %02X %02X %02X %02X\n",
          renderable.runtimeModelPtr, renderable.meshData,
          renderable.geosetIndex, posePaletteLimit, maxExpectedGroupSize,
          candidates.size(), hasRemapTable ? 1 : 0, remapTables.size(),
          hasRemapTable ? remapTables.front().span : 0u,
          hasRemapTable ? remapTables.front().flag : 0u,
          remapTags[0], remapTags[1], remapTags[2], remapTags[3],
          remapWordPairs[0], remapWordPairs[1], remapWordPairs[2],
          remapWordPairs[3], remapWordPairs[4], remapWordPairs[5],
          remapWordPairs[6], remapWordPairs[7],
          remapHead[0], remapHead[1], remapHead[2], remapHead[3], remapHead[4],
          remapHead[5], remapHead[6], remapHead[7]);
      }
    }
    return false;
  }

  const auto* bestBase =
      reinterpret_cast<const uint8_t*>(bestCandidate->ptr);
  outResult.weights.resize(vertexCount);
  outResult.indices.resize(vertexCount);
  uint32_t maxGroupSlot = 0u;
  uint8_t explicitBlendCount = 0u;
  uint64_t hash = bit::fnv1a_init();
  bool valid = true;
  bool usedLocalRemap = false;
  bool usedSpanRemap = false;
  bool usedStagePresetBias = false;

  for (uint32_t i = 0u; i < vertexCount; ++i) {
    std::array<uint8_t, 4> tupleSlots = {};
    uint32_t tupleCount = 0u;
    bool tupleUsedLocalRemap = false;
    const uint8_t* tuplePtr =
        bestBase + size_t(i) * size_t(bestStride) + size_t(bestOffset);
    const bool decoded =
        bestStagePresetBaseBias == 0u
            ? (TryBuildOrderedTupleSlots(
                   tuplePtr, posePaletteLimit, maxExpectedGroupSize, tupleSlots,
                   tupleCount) ||
               (hasRemapTable &&
                TryBuildOrderedTupleSlotsWithAnySpanRemap(
                    tuplePtr, remapTables, posePaletteLimit,
                    maxExpectedGroupSize, tupleSlots, tupleCount) &&
                (usedSpanRemap = true, true)) ||
               TryBuildOrderedTupleSlotsWithLocalRemap(
                   tuplePtr, posePaletteLimit, maxExpectedGroupSize, tupleSlots,
                   tupleCount, tupleUsedLocalRemap))
            : (TryBuildOrderedTupleSlotsWithBaseBias(
                   tuplePtr, bestStagePresetBaseBias, posePaletteLimit,
                   maxExpectedGroupSize, tupleSlots, tupleCount) ||
               (hasRemapTable &&
                TryBuildOrderedTupleSlotsWithAnySpanRemapAndBaseBias(
                    tuplePtr, remapTables, bestStagePresetBaseBias,
                    posePaletteLimit, maxExpectedGroupSize, tupleSlots,
                    tupleCount) &&
                (usedSpanRemap = true, true)));
    if (!decoded) {
      valid = false;
      break;
    }
    if (tupleCount < 2u || tupleCount > 3u) {
      valid = false;
      break;
    }
    if (tupleUsedLocalRemap)
      usedLocalRemap = true;
    if (bestStagePresetBaseBias != 0u)
      usedStagePresetBias = true;

    float w0 = 0.0f;
    float w1 = 0.0f;
    std::memcpy(&w0, auxBase + size_t(i) * size_t(auxStride) + 0u, sizeof(w0));
    std::memcpy(&w1, auxBase + size_t(i) * size_t(auxStride) + 4u, sizeof(w1));
    if (!std::isfinite(w0) || !std::isfinite(w1)) {
      valid = false;
      break;
    }

    w0 = std::clamp(w0, 0.0f, 1.0f);
    w1 = std::clamp(w1, 0.0f, 1.0f);

    std::array<float, 3> weights = {0.0f, 0.0f, 0.0f};
    std::array<uint8_t, 4> indices = {tupleSlots[0], tupleSlots[1],
                                      tupleCount >= 3u ? tupleSlots[2]
                                                       : tupleSlots[1],
                                      0u};

    if (tupleCount == 2u) {
      if (!(w0 > 0.001f && w0 < 0.999f)) {
        valid = false;
        break;
      }
      weights[0] = w0;
      explicitBlendCount = (std::max)(explicitBlendCount, uint8_t(1u));
    } else {
      float sum = w0 + w1;
      if (sum <= 0.001f) {
        valid = false;
        break;
      }
      if (sum >= 1.0f) {
        w0 /= sum;
        w1 /= sum;
      }
      weights[0] = w0;
      weights[1] = w1;
      explicitBlendCount = (std::max)(explicitBlendCount, uint8_t(2u));
    }

    for (uint32_t slotIndex = 0u; slotIndex < tupleCount; ++slotIndex) {
      maxGroupSlot = (std::max)(maxGroupSlot, uint32_t(tupleSlots[slotIndex]));
      hash = bit::fnv1a_iter(hash, uint32_t(tupleSlots[slotIndex]));
    }
    uint32_t weightBits0 = 0u;
    uint32_t weightBits1 = 0u;
    std::memcpy(&weightBits0, &weights[0], sizeof(weightBits0));
    std::memcpy(&weightBits1, &weights[1], sizeof(weightBits1));
    hash = bit::fnv1a_iter(hash, weightBits0);
    hash = bit::fnv1a_iter(hash, weightBits1);

    outResult.weights[i] = weights;
    outResult.indices[i] = indices;
  }

  if (!valid || explicitBlendCount == 0u ||
      maxGroupSlot >= pose.matrixPalette.size()) {
    if (ioStats != nullptr)
      ioStats->explicitBlendFinalDecodeMiss++;
    if (SemanticCoreTraceEnabled()) {
      static std::atomic<uint32_t> s_explicitBlendFinalMissLogCount{0};
      const uint32_t logIndex = s_explicitBlendFinalMissLogCount.fetch_add(
          1u, std::memory_order_relaxed);
      if (logIndex < 8u || (logIndex % 2048u) == 0u) {
      dxvk::war3dbg::Print(
          "DXVK SemanticCore: explicit-blend final-miss runtime=%p mesh=%p "
          "geoIdx=%u valid=%d blendCount=%u maxSlot=%u poseCount=%zu stride=%u "
          "offset=%u\n",
          renderable.runtimeModelPtr, renderable.meshData,
          renderable.geosetIndex, valid ? 1 : 0, explicitBlendCount,
          maxGroupSlot, pose.matrixPalette.size(), bestStride, bestOffset);
      }
    }
    return false;
  }

  outResult.runtimeGroupPalette.assign(
      pose.matrixPalette.begin(),
      pose.matrixPalette.begin() + size_t(maxGroupSlot + 1u));
  outResult.maxGroupSlot = maxGroupSlot;
  outResult.dynamicHash = bit::fnv1a_iter(
      bit::fnv1a_iter(
          bit::fnv1a_iter(hash,
                          reinterpret_cast<uintptr_t>(bestCandidate->ptr)),
          reinterpret_cast<uintptr_t>(layerContract->auxStreamPtr0)),
      bestOffset);
  outResult.dynamicHash =
      bit::fnv1a_iter(outResult.dynamicHash, bestStagePresetBaseBias);
  outResult.dynamicHash =
      bit::fnv1a_iter(outResult.dynamicHash, usedLocalRemap ? 1u : 0u);
  outResult.dynamicHash =
      bit::fnv1a_iter(outResult.dynamicHash, usedSpanRemap ? 1u : 0u);
  outResult.dynamicHash =
      bit::fnv1a_iter(outResult.dynamicHash, usedStagePresetBias ? 1u : 0u);
  if (usedSpanRemap) {
    outResult.dynamicHash =
        bit::fnv1a_iter(outResult.dynamicHash,
                        reinterpret_cast<uintptr_t>(remapTables.front().table));
    outResult.dynamicHash =
        bit::fnv1a_iter(outResult.dynamicHash, remapTables.front().span);
  }
  outResult.stream1StrideUsed = bestStride;
  outResult.stream1OffsetUsed = bestOffset;
  outResult.auxStrideUsed = auxStride;
  outResult.stagePresetBaseBias = bestStagePresetBaseBias;
  outResult.blendCount = explicitBlendCount;
  outResult.usedSpanRemap = usedSpanRemap;
  return true;
}

bool TryBuildDirectPosePaletteForDynamicGroups(
    const ShadowPoseRecord& pose, uint32_t maxGroupSlot,
    std::vector<Matrix4>& outPalette) {
  outPalette.clear();

  if (pose.matrixCount == 0u || pose.matrixPalette.empty())
    return false;

  const uint32_t posePaletteCount =
      (std::min)(pose.matrixCount, uint32_t(pose.matrixPalette.size()));
  if (posePaletteCount == 0u || maxGroupSlot >= posePaletteCount)
    return false;

  outPalette.assign(pose.matrixPalette.begin(),
                    pose.matrixPalette.begin() + size_t(maxGroupSlot + 1u));
  return true;
}

size_t RuntimeVertexGroupSlotCount(const ShadowModelResourceRecord& resource) {
  size_t count = resource.vertexGroupIndices.size();
  const uint32_t vertexCount =
      resource.vertexCount != 0u
          ? resource.vertexCount
          : uint32_t(resource.positions.size() / 3u);
  if (vertexCount != 0u)
    count = (std::min)(count, size_t(vertexCount));

  // A Warcraft III geoset should be far below this; this guard prevents a bad
  // upstream sideband slice from turning semantic preview into an unbounded scan.
  constexpr size_t kMaxSemanticVertexGroupSlots = 128u * 1024u;
  return (std::min)(count, kMaxSemanticVertexGroupSlots);
}

bool TryBuildDirectPosePaletteForStaticVertexGroups(
    const ShadowModelResourceRecord& resource, uint32_t vertexCount,
    const ShadowPoseRecord& pose, uint32_t& outMaxGroupSlot,
    std::vector<Matrix4>& outPalette) {
  outMaxGroupSlot = 0u;
  outPalette.clear();

  if (vertexCount == 0u ||
      RuntimeVertexGroupSlotCount(resource) < size_t(vertexCount) ||
      pose.matrixCount == 0u || pose.matrixPalette.empty()) {
    return false;
  }

  uint32_t maxGroupSlot = 0u;
  for (uint32_t i = 0u; i < vertexCount; ++i)
    maxGroupSlot = (std::max)(maxGroupSlot,
                              uint32_t(resource.vertexGroupIndices[i]));

  if (!TryBuildDirectPosePaletteForDynamicGroups(pose, maxGroupSlot,
                                                 outPalette)) {
    return false;
  }

  outMaxGroupSlot = maxGroupSlot;
  return true;
}

uint64_t MakeDrawDedupKey(const ShadowDrawPacket& packet) {
  const void* primaryPtr =
      packet.renderable.renderablePart != nullptr
          ? packet.renderable.renderablePart
          : packet.renderable.runtimeGeosetPtr != nullptr
                ? packet.renderable.runtimeGeosetPtr
                : packet.renderable.meshData != nullptr
                      ? packet.renderable.meshData
                      : packet.renderable.sceneNode;
  const uint64_t h1 = uint64_t(reinterpret_cast<uintptr_t>(primaryPtr));
  const uint64_t h2 =
      uint64_t(reinterpret_cast<uintptr_t>(packet.renderable.runtimeModelPtr));
  const uint64_t h3 = uint64_t(packet.renderable.geosetIndex);
  const uint64_t h4 = packet.material.signatureHash;
  const uint64_t h5 = uint64_t(packet.material.alphaMode);
  return h1 ^ (h2 + 0x9E3779B97F4A7C15ull + (h1 << 6) + (h1 >> 2)) ^
         (h3 + 0x9E3779B97F4A7C15ull + (h2 << 6) + (h2 >> 2)) ^
         (h4 + 0x9E3779B97F4A7C15ull + (h3 << 6) + (h3 >> 2)) ^
         (h5 + 0x9E3779B97F4A7C15ull + (h4 << 6) + (h4 >> 2));
}

bool TryConvertUpperLayerResolvedItem(
    const render::UpperLayerShadowResolvedItem& src, uint64_t frameSerial,
    ShadowDrawPacket& outPacket) {
  outPacket = {};
  if (!src.HasAuthoritativeRigidPath() && !src.HasAuthoritativeSkinnedPath())
    return false;

  outPacket.renderable = ConvertVisibleRecord(src.visible, frameSerial);
  outPacket.resource =
      MakePacketResourceOwned(ConvertGeosetRecord(src.geoset, frameSerial));
  outPacket.pose = ConvertPoseRecord(src.pose, frameSerial);
  MeshLayerBindingContract layerContract = {};
  const bool hasLayerContract =
      TryResolveMeshLayerBindingContract(outPacket.renderable, layerContract);
  outPacket.material = BuildShadowMaterialSignature(
      outPacket.renderable, hasLayerContract ? &layerContract : nullptr);
  outPacket.path =
      src.skinned ? ShadowDrawPath::Skinned : ShadowDrawPath::Rigid;
  outPacket.hasRuntimeGroupPalette = src.hasRuntimeGroupPalette;
  outPacket.matrixGroupsUseAveraging = src.matrixGroupsUseAveraging;
  outPacket.maxVertexGroupSlot = src.maxVertexGroupSlot;
  outPacket.runtimeGroupPaletteSlotIndex = 0xFFFFFFFFu;
  outPacket.runtimeGroupPaletteMinFrameTag = 0u;
  outPacket.runtimeGroupPaletteMaxFrameTag = 0u;
  outPacket.runtimeGroupPalette = src.runtimeGroupPalette;
  outPacket.runtimeGroupPaletteHash =
      outPacket.runtimeGroupPalette.empty()
          ? 0ull
          : HashMatrixPalette(outPacket.runtimeGroupPalette);
  return true;
}

bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource,
                                 const ShadowRenderableRecord& renderable,
                                 const ShadowPoseRecord& pose,
                                 const ShadowPoseStore& poses,
                                 std::vector<Matrix4>& outPalette,
                                 uint32_t& outMaxVertexGroupSlot,
                                 bool& outUsesAveraging,
                                 RuntimeGroupPaletteMissDetail* outMissDetail =
                                     nullptr) {
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outUsesAveraging = false;
  if (outMissDetail != nullptr)
    *outMissDetail = {};

  if (!resource.hasSkinningData())
    return (NoteRuntimeGroupPaletteMiss(
                outMissDetail, RuntimeGroupPaletteMissReason::NoSkinningData,
                pose.matrixCount, uint32_t(resource.matrixGroupSizes.size()), 0u,
                uint32_t(resource.matrixIndices.size())),
            false);
  if (pose.matrixPalette.empty() || pose.matrixCount == 0u)
    return (NoteRuntimeGroupPaletteMiss(
                outMissDetail, RuntimeGroupPaletteMissReason::NoPosePalette,
                pose.matrixCount, uint32_t(resource.matrixGroupSizes.size()), 0u,
                uint32_t(resource.matrixIndices.size())),
            false);
  const size_t vertexGroupSlotCount = RuntimeVertexGroupSlotCount(resource);
  if (vertexGroupSlotCount == 0u)
    return (NoteRuntimeGroupPaletteMiss(
                outMissDetail, RuntimeGroupPaletteMissReason::NoVertexGroups,
                pose.matrixCount, uint32_t(resource.matrixGroupSizes.size()), 0u,
                uint32_t(resource.matrixIndices.size())),
            false);

  for (size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = resource.vertexGroupIndices[i];
    outMaxVertexGroupSlot =
        (std::max)(outMaxVertexGroupSlot, uint32_t(groupSlot));
  }

  // 尝试从引擎的全局调色板缓冲区读取完整骨架
  // 这解决了 CModel + 0x60 只有 2-3 个根骨骼矩阵的问题
  auto tryEngineDirectPosePalette = [&]() -> bool {
    // 尝试从 RenderablePart + 0x08 读取调色板槽位索引
    // 注意：RenderablePartFieldOffsets 中该字段名为 StagePresetSpanBaseIndex
    // 但在 CModel_AllocAndFillGroupPalette 中实际用作 palette slot index
    uint32_t paletteSlotIndex = 0xFFFFFFFF;
    if (renderable.renderablePart != nullptr) {
      dxvk::war3::SafeReadU32Fast(
          renderable.renderablePart,
          dxvk::war3::RenderablePartFieldOffsets::StagePresetSpanBaseIndex,
          paletteSlotIndex);
    }

    // 使用缓存机制：如果当前帧没有更新槽位索引，使用上一帧的值
    paletteSlotIndex = FindOrUpdatePaletteSlotCache(
        renderable.renderablePart, paletteSlotIndex);
    
    if (paletteSlotIndex == 0xFFFFFFFF || paletteSlotIndex >= 0x3A98)
      return false;

    uintptr_t gameDllBase = reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
    if (gameDllBase == 0) return false;

    // dword_6FBC6BD0 是动态分配的调色板缓冲区地址，需要解引用
    // sub_6F139060(slotIndex) 返回 dword_6FBC6BD0 + 48 * slotIndex
    void* globalPaletteBufferPtr = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(
            reinterpret_cast<const void*>(gameDllBase + kGlobalPaletteBufferRva),
            0, globalPaletteBufferPtr) || globalPaletteBufferPtr == nullptr) {
      return false;
    }
    const uintptr_t globalPaletteBufferBase = reinterpret_cast<uintptr_t>(globalPaletteBufferPtr);
    
    const uint32_t requiredCount = outMaxVertexGroupSlot + 1u;
    if (requiredCount == 0 || requiredCount > 256) return false;
    
    // 引擎使用 3x4 矩阵（48字节），使用 DecodeRuntimePoseMatrix48 解析
    const uint8_t* enginePalette = reinterpret_cast<const uint8_t*>(globalPaletteBufferBase + 48u * paletteSlotIndex);
    
    // 检查内存可读性
    if (!dxvk::war3::IsReadableRange(enginePalette, requiredCount * 48u))
      return false;
    
    outPalette.resize(requiredCount);
    for (uint32_t i = 0; i < requiredCount; ++i) {
      outPalette[i] = DecodeRuntimePoseMatrix48(enginePalette + i * 48u);
    }
    
    outUsesAveraging = false; 
    return true;
  };

  // 优先使用引擎的全局调色板缓冲区
  if (tryEngineDirectPosePalette()) {
    NoteRuntimeGroupPaletteMiss(outMissDetail, RuntimeGroupPaletteMissReason::None,
                                pose.matrixCount,
                                uint32_t(resource.matrixGroupSizes.size()),
                                outMaxVertexGroupSlot,
                                uint32_t(resource.matrixIndices.size()));
    return true;
  }

  NoteRuntimeGroupPaletteMiss(outMissDetail, RuntimeGroupPaletteMissReason::None,
                              pose.matrixCount,
                              uint32_t(resource.matrixGroupSizes.size()),
                              outMaxVertexGroupSlot,
                              uint32_t(resource.matrixIndices.size()));

  if (!resource.hasSkinningData())
    return false;

  std::vector<uint32_t> uniqueGroupSlots;
  uniqueGroupSlots.reserve(outMaxVertexGroupSlot + 1u);
  std::array<bool, 256> seenGroupSlots = {};
  for (size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = resource.vertexGroupIndices[i];
    if (!seenGroupSlots[groupSlot]) {
      seenGroupSlots[groupSlot] = true;
      uniqueGroupSlots.push_back(uint32_t(groupSlot));
    }
  }

  auto logFailure = [&]() {
    if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled) {
      return;
    }
    if (!SemanticCoreTraceEnabled())
      return;
    static std::atomic<uint32_t> s_runtimeGroupPaletteMissLogCount{0};
    const uint32_t logIndex = s_runtimeGroupPaletteMissLogCount.fetch_add(
        1, std::memory_order_relaxed);
    if (!(logIndex < 32u || (logIndex % 2048u) == 0u))
      return;

    bool directMatrixOk = !resource.matrixIndices.empty() &&
                          outMaxVertexGroupSlot < resource.matrixIndices.size();
    if (directMatrixOk) {
      for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
        const uint32_t matrixIndex = resource.matrixIndices[group];
        if (matrixIndex >= pose.matrixCount ||
            matrixIndex >= pose.matrixPalette.size()) {
          directMatrixOk = false;
          break;
        }
      }
    }

    bool sparseMatrixOk = !resource.matrixIndices.empty() &&
                          !uniqueGroupSlots.empty() &&
                          uniqueGroupSlots.size() <= resource.matrixIndices.size();
    if (sparseMatrixOk) {
      for (size_t i = 0; i < uniqueGroupSlots.size(); ++i) {
        const uint32_t matrixIndex = resource.matrixIndices[i];
        if (matrixIndex >= pose.matrixCount ||
            matrixIndex >= pose.matrixPalette.size()) {
          sparseMatrixOk = false;
          break;
        }
      }
    }

    const bool directPoseOk =
        outMaxVertexGroupSlot < pose.matrixCount &&
        outMaxVertexGroupSlot < pose.matrixPalette.size();
    const bool sparsePoseOk =
        !uniqueGroupSlots.empty() && uniqueGroupSlots.size() <= pose.matrixCount &&
        uniqueGroupSlots.size() <= pose.matrixPalette.size();

    void* meshPoseCtx = nullptr;
    void* meshPoseCandidate = nullptr;
    void* meshPoseCandidateModelResource = nullptr;
    uint32_t meshPoseCandidateFinalCount = 0u;
    uint32_t meshPoseCandidateStoredCount = 0u;
    void* meshPrimaryStreamPtr = nullptr;
    uint32_t meshPrimaryStreamStride = 0u;
    void* meshStream1Ptr = nullptr;
    uint32_t meshStream1Stride = 0u;
    float meshPrimarySample[3] = {};
    uint32_t meshStream1Sample = 0u;
    bool meshPrimarySampleValid = false;
    bool meshStream1SampleValid = false;
    if (renderable.meshData != nullptr &&
        dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                    dxvk::war3::MeshDataOffsets::TransformOrPoseCtx,
                                    meshPoseCtx) &&
        meshPoseCtx != nullptr) {
      meshPoseCandidate = meshPoseCtx;
      dxvk::war3::SafeReadPtrFast(
          meshPoseCandidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          meshPoseCandidateModelResource);
      dxvk::war3::SafeReadU32Fast(
          meshPoseCandidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          meshPoseCandidateFinalCount);

      ShadowPoseRecord meshPoseRecord = {};
      if (poses.findByRuntimeModel(meshPoseCandidate, meshPoseRecord))
        meshPoseCandidateStoredCount = meshPoseRecord.matrixCount;
    }
    if (renderable.meshData != nullptr) {
      dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                  dxvk::war3::MeshDataOffsets::PrimaryStreamPtr,
                                  meshPrimaryStreamPtr);
      dxvk::war3::SafeReadU32Fast(
          renderable.meshData, dxvk::war3::MeshDataOffsets::PrimaryStreamStride,
          meshPrimaryStreamStride);
      dxvk::war3::SafeReadPtrFast(renderable.meshData,
                                  dxvk::war3::MeshDataOffsets::Stream1Ptr,
                                  meshStream1Ptr);
      dxvk::war3::SafeReadU32Fast(renderable.meshData,
                                  dxvk::war3::MeshDataOffsets::Stream1Stride,
                                  meshStream1Stride);
      if (meshPrimaryStreamPtr != nullptr && meshPrimaryStreamStride >= 12u &&
          dxvk::war3::IsReadableRange(meshPrimaryStreamPtr, 12u)) {
        std::memcpy(meshPrimarySample, meshPrimaryStreamPtr, 12u);
        meshPrimarySampleValid = true;
      }
      if (meshStream1Ptr != nullptr && meshStream1Stride != 0u &&
          dxvk::war3::IsReadableRange(meshStream1Ptr, 4u)) {
        std::memcpy(&meshStream1Sample, meshStream1Ptr, sizeof(meshStream1Sample));
        meshStream1SampleValid = true;
      }
    }

    void* rootOwnedHandle = nullptr;
    uint32_t rootFinalPoseCount = 0u;
    if (renderable.runtimeModelPtr != nullptr) {
      dxvk::war3::SafeReadPtrFast(
          renderable.runtimeModelPtr,
          dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          rootOwnedHandle);
      dxvk::war3::SafeReadU32Fast(
          renderable.runtimeModelPtr,
          dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          rootFinalPoseCount);
    }

    void* bestDescendantRuntimeModel = nullptr;
    uint32_t bestDescendantPoseCount = 0u;
    size_t descendantRuntimeCount = 0u;
    if (renderable.runtimeModelPtr != nullptr) {
      const auto runtimeTree =
          CollectRuntimeModelTree(renderable.runtimeModelPtr);
      descendantRuntimeCount = runtimeTree.size();
      for (void* candidateRuntimeModel : runtimeTree) {
        if (candidateRuntimeModel == nullptr ||
            candidateRuntimeModel == renderable.runtimeModelPtr) {
          continue;
        }
        ShadowPoseRecord candidatePose = {};
        if (!poses.findByRuntimeModel(candidateRuntimeModel, candidatePose))
          continue;
        if (candidatePose.matrixCount > bestDescendantPoseCount) {
          bestDescendantPoseCount = candidatePose.matrixCount;
          bestDescendantRuntimeModel = candidateRuntimeModel;
        }
      }
    }

    MeshLayerBindingContract missLayerContract = {};
    const bool missHasLayerContract =
        TryResolveMeshLayerBindingContract(renderable, missLayerContract);
    uint32_t l1cHead00 = 0u;
    uint32_t l1cHead04 = 0u;
    uint32_t l1cHead08 = 0u;
    uint32_t l1cHead0C = 0u;
    uint32_t l1cHead10 = 0u;
    uint32_t l1cHead14 = 0u;
    uint32_t l1cHead18 = 0u;
    uint32_t l1cHead1C = 0u;
    const bool l1cHeadValid =
        missHasLayerContract &&
        LooksLikeDynamicAuxPointerCandidate(missLayerContract.layerStateWord1C) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x00u, l1cHead00) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x04u, l1cHead04) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x08u, l1cHead08) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x0Cu, l1cHead0C) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x10u, l1cHead10) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x14u, l1cHead14) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x18u, l1cHead18) &&
        TryReadLinearWordSample(
            reinterpret_cast<const void*>(uintptr_t(missLayerContract.layerStateWord1C)),
            0x1Cu, l1cHead1C);
    uint32_t rootOwnedHead00 = 0u;
    uint32_t rootOwnedHead04 = 0u;
    uint32_t rootOwnedHead08 = 0u;
    uint32_t rootOwnedHead0C = 0u;
    uint32_t rootOwnedHead10 = 0u;
    uint32_t rootOwnedHead14 = 0u;
    uint32_t rootOwnedHead18 = 0u;
    uint32_t rootOwnedHead1C = 0u;
    uint32_t rootOwnedHead20 = 0u;
    uint32_t rootOwnedHead24 = 0u;
    uint32_t rootOwnedHead28 = 0u;
    uint32_t rootOwnedHead2C = 0u;
    uint32_t rootOwnedFlags94 = 0u;
    void* rootOwnedProto98 = nullptr;
    void* rootOwnedSelf9C = nullptr;
    const bool rootOwnedHeadValid =
        rootOwnedHandle != nullptr &&
        TryReadLinearWordSample(rootOwnedHandle, 0x00u, rootOwnedHead00) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x04u, rootOwnedHead04) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x08u, rootOwnedHead08) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x0Cu, rootOwnedHead0C) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x10u, rootOwnedHead10) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x14u, rootOwnedHead14) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x18u, rootOwnedHead18) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x1Cu, rootOwnedHead1C) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x20u, rootOwnedHead20) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x24u, rootOwnedHead24) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x28u, rootOwnedHead28) &&
        TryReadLinearWordSample(rootOwnedHandle, 0x2Cu, rootOwnedHead2C);
    if (rootOwnedHandle != nullptr) {
      dxvk::war3::SafeReadU32Fast(
          rootOwnedHandle, dxvk::war3::CModelDataOffsets::Flags,
          rootOwnedFlags94);
      dxvk::war3::SafeReadPtrFast(
          rootOwnedHandle, dxvk::war3::CModelDataOffsets::SharedResourceProto,
          rootOwnedProto98);
      dxvk::war3::SafeReadPtrFast(
          rootOwnedHandle, dxvk::war3::CModelDataOffsets::ModelDataHandle,
          rootOwnedSelf9C);
    }

    dxvk::war3dbg::Print(
        "DXVK SemanticCore: runtime-group miss model=%p runtime=%p geoIdx=%u "
        "vertexGroups=%zu uniqueSlots=%zu maxSlot=%u groupCount=%zu "
        "matrixIndices=%zu poseCount=%u rootOwnedHandle=%p rootFinalCount=%u "
        "meshPoseCtx=%p candidateModel=%p "
        "candidateOwned=%p candidateFinalCount=%u candidateStoredCount=%u "
        "meshPrimary=%p stride0=%u sample0=%d(%.3f,%.3f,%.3f) "
        "meshStream1=%p stride1=%u sample1=%d(0x%08X) "
        "descRuntimeCount=%zu bestDescRuntime=%p bestDescPoseCount=%u "
        "fallback[directMatrix=%d sparseMatrix=%d directPose=%d sparsePose=%d] "
        "preset[sceneBase=%u spanBase=%u idx=%u/%u resolved=%u/%u] "
        "rootHandleFields[flags=%08X proto=%p self=%p] "
        "rootHead=%d:%08X/%08X/%08X/%08X|%08X/%08X/%08X/%08X|%08X/%08X/%08X/%08X "
        "l1cHead=%d:%08X/%08X/%08X/%08X|%08X/%08X/%08X/%08X\n",
        resource.modelResourcePtr, renderable.runtimeModelPtr, resource.geosetIndex,
        resource.vertexGroupIndices.size(), uniqueGroupSlots.size(),
        outMaxVertexGroupSlot, resource.matrixGroupSizes.size(),
        resource.matrixIndices.size(), pose.matrixCount, rootOwnedHandle,
        rootFinalPoseCount, meshPoseCtx,
        meshPoseCandidate, meshPoseCandidateModelResource,
        meshPoseCandidateFinalCount, meshPoseCandidateStoredCount,
        meshPrimaryStreamPtr, meshPrimaryStreamStride,
        meshPrimarySampleValid ? 1 : 0,
        meshPrimarySample[0], meshPrimarySample[1], meshPrimarySample[2],
        meshStream1Ptr, meshStream1Stride,
        meshStream1SampleValid ? 1 : 0, meshStream1Sample,
        descendantRuntimeCount, bestDescendantRuntimeModel,
        bestDescendantPoseCount,
        directMatrixOk ? 1 : 0, sparseMatrixOk ? 1 : 0,
        directPoseOk ? 1 : 0, sparsePoseOk ? 1 : 0,
        missHasLayerContract ? missLayerContract.sceneStagePresetBaseIndex : 0u,
        missHasLayerContract ? missLayerContract.stagePresetSpanBaseIndex : 0u,
        missHasLayerContract ? missLayerContract.stagePresetIndex0 : 0u,
        missHasLayerContract ? missLayerContract.stagePresetIndex1 : 0u,
        missHasLayerContract ? missLayerContract.resolvedStagePresetIndex0 : 0u,
        missHasLayerContract ? missLayerContract.resolvedStagePresetIndex1 : 0u,
        rootOwnedFlags94, rootOwnedProto98, rootOwnedSelf9C,
        rootOwnedHeadValid ? 1 : 0,
        rootOwnedHead00, rootOwnedHead04, rootOwnedHead08, rootOwnedHead0C,
        rootOwnedHead10, rootOwnedHead14, rootOwnedHead18, rootOwnedHead1C,
        rootOwnedHead20, rootOwnedHead24, rootOwnedHead28, rootOwnedHead2C,
        l1cHeadValid ? 1 : 0,
        l1cHead00, l1cHead04, l1cHead08, l1cHead0C,
        l1cHead10, l1cHead14, l1cHead18, l1cHead1C);
  };

  auto buildDirectMatrixRemap = [&]() -> bool {
    if (resource.matrixIndices.empty() ||
        outMaxVertexGroupSlot >= resource.matrixIndices.size()) {
      return false;
    }

    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      const uint32_t matrixIndex = resource.matrixIndices[group];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        return false;
      }
      outPalette[group] = pose.matrixPalette[matrixIndex];
    }
    return true;
  };

  auto buildSparseMatrixRemap = [&]() -> bool {
    if (resource.matrixIndices.empty() || uniqueGroupSlots.empty() ||
        uniqueGroupSlots.size() > resource.matrixIndices.size()) {
      return false;
    }

    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (size_t i = 0; i < uniqueGroupSlots.size(); ++i) {
      const uint32_t matrixIndex = resource.matrixIndices[i];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        return false;
      }
      outPalette[uniqueGroupSlots[i]] = pose.matrixPalette[matrixIndex];
    }
    return true;
  };

  auto buildDirectPosePalette = [&]() -> bool {
    if (outMaxVertexGroupSlot >= pose.matrixCount ||
        outMaxVertexGroupSlot >= pose.matrixPalette.size()) {
      return false;
    }

    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group)
      outPalette[group] = pose.matrixPalette[group];
    return true;
  };

  auto buildSparsePosePalette = [&]() -> bool {
    if (uniqueGroupSlots.empty() || uniqueGroupSlots.size() > pose.matrixCount ||
        uniqueGroupSlots.size() > pose.matrixPalette.size()) {
      return false;
    }

    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (size_t i = 0; i < uniqueGroupSlots.size(); ++i)
      outPalette[uniqueGroupSlots[i]] = pose.matrixPalette[i];
    return true;
  };

  auto buildUniformPosePalette = [&]() -> bool {
    if (pose.matrixCount == 0u || pose.matrixPalette.empty())
      return false;

    const uint32_t paletteCount =
        (std::max)(outMaxVertexGroupSlot + 1u,
                   uint32_t(resource.matrixGroupSizes.size()));
    if (paletteCount == 0u)
      return false;

    // Some runtime models publish only the final root matrix even though the
    // static geoset still carries vertex-group metadata. Broadcasting that root
    // matrix gives us a semantic rigid-palette packet instead of dropping back
    // to the old draw-time capture path.
    outPalette.assign(paletteCount, pose.matrixPalette.front());
    return true;
  };

  auto tryFallbacks = [&]() -> bool {
    const bool ok = buildDirectMatrixRemap() || buildSparseMatrixRemap() ||
                    buildDirectPosePalette() || buildSparsePosePalette() ||
                    buildUniformPosePalette();
    if (!ok) {
      NoteRuntimeGroupPaletteMiss(
          outMissDetail, RuntimeGroupPaletteMissReason::FallbacksFailed,
          pose.matrixCount, uint32_t(resource.matrixGroupSizes.size()),
          outMaxVertexGroupSlot, uint32_t(resource.matrixIndices.size()));
      logFailure();
    }
    return ok;
  };

  const uint32_t groupCount = uint32_t(resource.matrixGroupSizes.size());
  if (groupCount == 0u) {
    return tryFallbacks();
  }

  std::vector<uint32_t> prefix(groupCount, 0u);
  uint32_t running = 0u;
  for (uint32_t i = 0u; i < groupCount; ++i) {
    prefix[i] = running;
    running += resource.matrixGroupSizes[i];
  }
  if (running > resource.matrixIndices.size()) {
    NoteRuntimeGroupPaletteMiss(
        outMissDetail, RuntimeGroupPaletteMissReason::InvalidGroupTable,
        pose.matrixCount, groupCount, outMaxVertexGroupSlot,
        uint32_t(resource.matrixIndices.size()));
    return tryFallbacks();
  }

  outPalette.resize(groupCount);
  for (uint32_t group = 0u; group < groupCount; ++group) {
    const uint32_t groupSize = resource.matrixGroupSizes[group];
    const uint32_t groupBase = prefix[group];
    if (groupSize == 0u || (groupBase + groupSize) > resource.matrixIndices.size()) {
      NoteRuntimeGroupPaletteMiss(
          outMissDetail, RuntimeGroupPaletteMissReason::InvalidGroupTable,
          pose.matrixCount, groupCount, outMaxVertexGroupSlot,
          uint32_t(resource.matrixIndices.size()), group);
      return tryFallbacks();
    }

    Matrix4 accum(0.0f);
    for (uint32_t i = 0u; i < groupSize; ++i) {
      const uint32_t matrixIndex = resource.matrixIndices[groupBase + i];
      if (matrixIndex >= pose.matrixCount ||
          matrixIndex >= pose.matrixPalette.size()) {
        NoteRuntimeGroupPaletteMiss(
            outMissDetail,
            RuntimeGroupPaletteMissReason::MatrixIndexOutOfRange,
            pose.matrixCount, groupCount, outMaxVertexGroupSlot,
            uint32_t(resource.matrixIndices.size()), group, matrixIndex);
        return tryFallbacks();
      }
      accum += pose.matrixPalette[matrixIndex];
    }

    if (groupSize > 1u)
      outUsesAveraging = true;
    outPalette[group] =
        groupSize == 1u ? accum : (accum / float(groupSize));
  }

  for (size_t i = 0u; i < vertexGroupSlotCount; ++i) {
    const uint8_t groupSlot = resource.vertexGroupIndices[i];
    if (uint32_t(groupSlot) >= groupCount) {
      NoteRuntimeGroupPaletteMiss(
          outMissDetail, RuntimeGroupPaletteMissReason::VertexGroupOutOfRange,
          pose.matrixCount, groupCount, outMaxVertexGroupSlot,
          uint32_t(resource.matrixIndices.size()), uint32_t(groupSlot));
      return tryFallbacks();
    }
  }

  return true;
}

bool ShouldBuildAttachmentSupplementalForChunk(uint64_t maxDurationUs) {
  if (maxDurationUs == 0u)
    return true;

  // The supplemental attachment pass is not resumable yet: after the main
  // manifest records finish it may scan a large attachment/resource set in one
  // call. Incremental scene-submission builds are meant to validate the
  // canonical manifest path without blocking the only hot frame, so skip the
  // supplemental pass there until it has its own continuation state.
  if (dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled()) {
    return false;
  }

  return true;
}

} // namespace

bool TryResolveExplicitBlendSkinningForRenderable(
    const ShadowRenderableRecord& renderable,
    uint32_t vertexCount,
    uint32_t posePaletteLimit,
    uint32_t maxExpectedGroupSize,
    const ShadowPoseRecord& pose,
    ShadowExplicitBlendSkinningResult& outResult,
    ShadowResolveStats* ioStats) {
  outResult = {};

  MeshLayerBindingContract layerContract = {};
  if (!TryResolveMeshLayerBindingContract(renderable, layerContract))
    return false;

  ExplicitBlendSkinResult internal = {};
  if (!TryResolveMeshDynamicExplicitBlendSkinning(
          renderable, vertexCount, posePaletteLimit, maxExpectedGroupSize,
          pose, &layerContract, internal, ioStats)) {
    return false;
  }

  outResult.weights = std::move(internal.weights);
  outResult.indices = std::move(internal.indices);
  outResult.runtimeGroupPalette = std::move(internal.runtimeGroupPalette);
  outResult.maxGroupSlot = internal.maxGroupSlot;
  outResult.dynamicHash = internal.dynamicHash;
  outResult.blendCount = internal.blendCount;
  outResult.usedSpanRemap = internal.usedSpanRemap;
  return true;
}

ShadowResolveStats ShadowRendererCore::buildFrame(
    const ShadowFrameManifest& manifest, const ShadowModelResourceStore& resources,
    const ShadowPoseStore& poses, const ShadowAttachmentRigidStore& attachments,
    ShadowSubmissionFrame& outFrame) const {
  outFrame = {};
  outFrame.frameSerial = manifest.frameSerial;

  ShadowResolveStats stats = {};
  stats.frameSerial = manifest.frameSerial;
  outFrame.draws.reserve(manifest.records.size());

  buildFrameChunk(manifest, resources, poses, attachments, 0u,
                  uint32_t(manifest.records.size()), 0u, outFrame, stats);

  return stats;
}

size_t ShadowRendererCore::buildFrameChunk(
    const ShadowFrameManifest& manifest, const ShadowModelResourceStore& resources,
    const ShadowPoseStore& poses, const ShadowAttachmentRigidStore& attachments,
    size_t startIndex, uint32_t maxRecords, uint64_t maxDurationUs,
    ShadowSubmissionFrame& ioFrame,
    ShadowResolveStats& ioStats) const {
  if (ioFrame.frameSerial != manifest.frameSerial) {
    ioFrame = {};
    ioFrame.frameSerial = manifest.frameSerial;
  }
  if (ioStats.frameSerial != manifest.frameSerial)
    ioStats.frameSerial = manifest.frameSerial;
  if (startIndex >= manifest.records.size())
    return manifest.records.size();

  if (ioFrame.draws.capacity() < manifest.records.size())
    ioFrame.draws.reserve(manifest.records.size());

  const auto startedAt = std::chrono::steady_clock::now();
  uint32_t processed = 0u;
  size_t index = startIndex;
  while (index < manifest.records.size()) {
    if (maxRecords != 0u && processed >= maxRecords)
      break;

    ShadowDrawPacket packet = {};
    ioStats.considered++;
    const auto recordStart = std::chrono::steady_clock::now();
    const auto& record = manifest.records[index];
    if (resolveRecord(record, resources, poses, attachments, packet, ioStats))
      ioFrame.draws.emplace_back(std::move(packet));
    const uint64_t recordElapsedUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - recordStart)
            .count());
    if (recordElapsedUs > ioStats.slowestRecordResolveUs) {
      ioStats.slowestRecordResolveUs = recordElapsedUs;
      ioStats.slowestRecordIndex = index;
      ioStats.slowestRecordRuntimeModelPtr =
          reinterpret_cast<uint64_t>(record.runtimeModelPtr);
      ioStats.slowestRecordModelResourcePtr =
          reinterpret_cast<uint64_t>(record.modelResourcePtr);
      ioStats.slowestRecordRuntimeGeosetPtr =
          reinterpret_cast<uint64_t>(record.runtimeGeosetPtr);
      ioStats.slowestRecordRuntimeGeosetDataPtr =
          reinterpret_cast<uint64_t>(record.runtimeGeosetDataPtr);
      ioStats.slowestRecordGeosetIndex = record.geosetIndex;
      ioStats.slowestRecordObjectKind = uint64_t(record.objectKind);
    }

    ++processed;
    ++index;

    if (maxDurationUs != 0u) {
      const uint64_t elapsedUs =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - startedAt)
                                    .count());
      if (elapsedUs >= maxDurationUs)
        break;
    }
  }

  if (index >= manifest.records.size() && !attachments.records().empty() &&
      ShouldBuildAttachmentSupplementalForChunk(maxDurationUs)) {
    const bool manifestHasRootUnitRecords = std::any_of(
        manifest.records.begin(), manifest.records.end(),
        [](const ShadowRenderableRecord& record) {
          return record.objectKind == render::ObjectKind::Unit &&
                 record.runtimeModelPtr != nullptr &&
                 record.modelResourcePtr != nullptr &&
                 record.hasResolvedGeoset();
        });
    std::unordered_map<uint64_t, size_t> dedupIndices;
    dedupIndices.reserve(ioFrame.draws.size() +
                         attachments.records().size() * 4u);
    for (size_t drawIndex = 0u; drawIndex < ioFrame.draws.size(); ++drawIndex)
      dedupIndices.try_emplace(MakeDrawDedupKey(ioFrame.draws[drawIndex]),
                               drawIndex);

    const uint64_t maxSupplementalAttachmentResolvedPerFrame =
        manifestHasRootUnitRecords ? 16u : 128u;
    for (const auto& attachment : attachments.records()) {
      if (ioStats.attachmentRigidSupplementalResolvedCount >=
          maxSupplementalAttachmentResolvedPerFrame) {
        break;
      }
      if (attachment.childRuntimeModelPtr == nullptr ||
          (attachment.childModelResourcePtr == nullptr &&
           attachment.childModelKey == 0u)) {
        continue;
      }
      ioStats.attachmentRigidSupplementalAttachmentCount++;

      std::unordered_set<uint64_t> attachmentResourceKeys;
      attachmentResourceKeys.reserve(16u);
      bool foundResourceCandidate = false;

      auto tryAppendAttachmentResource =
          [&](const ShadowModelResourceRecord& resource) {
        if (ioStats.attachmentRigidSupplementalResolvedCount >=
            maxSupplementalAttachmentResolvedPerFrame) {
          return;
        }
        if (!resource.readyForConsumer())
          return;

        const uint64_t resourceKey =
            uint64_t(reinterpret_cast<uintptr_t>(
                resource.runtimeGeosetPtr != nullptr
                    ? resource.runtimeGeosetPtr
                    : resource.runtimeGeosetDataPtr != nullptr
                          ? resource.runtimeGeosetDataPtr
                          : resource.modelResourcePtr)) ^
            (uint64_t(resource.geosetIndex) + 0x9E3779B97F4A7C15ull);
        if (!attachmentResourceKeys.insert(resourceKey).second)
          return;
        foundResourceCandidate = true;
        ioStats.attachmentRigidSupplementalResourceCandidateCount++;

        ShadowRenderableRecord supplemental = {};
        supplemental.worldObjectEntry = attachment.worldObjectEntry;
        supplemental.sceneNode = attachment.sceneNode;
        supplemental.unitPtr = attachment.unitPtr;
        supplemental.runtimeModelPtr = attachment.childRuntimeModelPtr;
        supplemental.modelResourcePtr =
            attachment.childModelResourcePtr != nullptr
                ? attachment.childModelResourcePtr
                : resource.modelResourcePtr;
        supplemental.runtimeGeosetPtr = resource.runtimeGeosetPtr;
        supplemental.runtimeGeosetDataPtr = resource.runtimeGeosetDataPtr;
        supplemental.modelKey =
            attachment.childModelKey != 0u ? attachment.childModelKey
                                           : resource.modelKey;
        supplemental.jHandle = attachment.jHandle;
        supplemental.rawcode = attachment.rawcode;
        supplemental.geosetIndex = resource.geosetIndex;
        supplemental.frameSerial = manifest.frameSerial;
        supplemental.objectKind =
            attachment.unitPtr != nullptr ? render::ObjectKind::Unit
                                          : render::ObjectKind::Unknown;
        supplemental.queueKind = render::VisibleRenderableQueueKind::MainQueue;

        ShadowDrawPacket packet = {};
        ioStats.considered++;
        if (!resolveRecord(supplemental, resources, poses, attachments, packet,
                           ioStats)) {
          return;
        }

        const uint64_t dedupKey = MakeDrawDedupKey(packet);
        if (auto existing = dedupIndices.find(dedupKey);
            existing != dedupIndices.end()) {
          auto& existingPacket = ioFrame.draws[existing->second];
          if (existingPacket.path != ShadowDrawPath::Skinned &&
              packet.path == ShadowDrawPath::Skinned) {
            existingPacket = std::move(packet);
            ioStats.attachmentRigidSupplementalResolvedCount++;
          }
          return;
        }
        dedupIndices.emplace(dedupKey, ioFrame.draws.size());
        ioFrame.draws.emplace_back(std::move(packet));
        ioStats.attachmentRigidSupplementalResolvedCount++;
      };

      constexpr uint32_t kMaxSupplementalAttachmentGeosets = 128u;
      for (uint32_t geosetIndex = 0u;
           geosetIndex < kMaxSupplementalAttachmentGeosets &&
           ioStats.attachmentRigidSupplementalResolvedCount <
               maxSupplementalAttachmentResolvedPerFrame;
           ++geosetIndex) {
        ShadowModelResourceRecord resource = {};
        if (resources.findByRuntimeModel(attachment.childRuntimeModelPtr,
                                         geosetIndex, resource) ||
            (attachment.childModelResourcePtr != nullptr &&
             resources.findByModelResource(attachment.childModelResourcePtr,
                                           geosetIndex, resource))) {
          tryAppendAttachmentResource(resource);
        }
      }

      for (const auto& resource : resources.records()) {
        if (ioStats.attachmentRigidSupplementalResolvedCount >=
            maxSupplementalAttachmentResolvedPerFrame) {
          break;
        }
        const bool modelResourceMatch =
            attachment.childModelResourcePtr != nullptr &&
            resource.modelResourcePtr == attachment.childModelResourcePtr;
        const bool modelKeyMatch =
            attachment.childModelKey != 0u &&
            resource.modelKey == attachment.childModelKey;
        if (!modelResourceMatch && !modelKeyMatch)
          continue;
        tryAppendAttachmentResource(resource);
      }

      if (!foundResourceCandidate)
        ioStats.attachmentRigidSupplementalResourceMissCount++;
    }
  }

  return index;
}

bool ShadowRendererCore::submitFrame(const ShadowSubmissionFrame& frame,
                                     IShadowRenderBackend& backend) const {
  return submitFrameLimited(frame, backend, 0u);
}

bool ShadowRendererCore::submitFrameLimited(const ShadowSubmissionFrame& frame,
                                            IShadowRenderBackend& backend,
                                            uint32_t maxSubmittedDraws) const {
  backend.beginFrame(frame.frameSerial);
  bool submittedAny = false;
  uint32_t submittedCount = 0u;
  for (const auto& draw : frame.draws) {
    ShadowGeometryHandle geometryHandle = {};
    ShadowPaletteHandle paletteHandle = {};
    ShadowMaterialHandle materialHandle = {};
    if (!backend.ensureGeometry(draw, geometryHandle))
      continue;
    if (!backend.ensurePalette(draw, paletteHandle))
      continue;
    if (!backend.ensureMaterial(draw, materialHandle))
      continue;
    if (!backend.submitDraw(draw, geometryHandle, paletteHandle, materialHandle))
      continue;
    submittedAny = true;
    ++submittedCount;
    if (maxSubmittedDraws != 0u && submittedCount >= maxSubmittedDraws)
      break;
  }
  backend.endFrame();
  return submittedAny;
}

bool ShadowRendererCore::resolveRecord(const ShadowRenderableRecord& record,
                                       const ShadowModelResourceStore& resources,
                                       const ShadowPoseStore& poses,
                                       const ShadowAttachmentRigidStore& attachments,
                                       ShadowDrawPacket& outPacket,
                                       ShadowResolveStats& ioStats) const {
  outPacket = {};
  ShadowRenderableRecord resolvedRecord = record;
  const bool sceneSubmissionRuntime =
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled();
  if (!sceneSubmissionRuntime)
    TryAugmentRenderableSemanticRecovery(resolvedRecord);
  const bool hasGeometryHint =
      resolvedRecord.meshData != nullptr ||
      resolvedRecord.runtimeGeosetPtr != nullptr ||
      resolvedRecord.runtimeGeosetDataPtr != nullptr ||
      resolvedRecord.geosetIndex != kInvalidShadowContractGeosetIndex;
  auto logSkippedRecord = [&](const char* reason) {
    if (!SemanticCoreTraceEnabled())
      return;
    static std::atomic<uint32_t> s_skipLogCount{0u};
    const uint32_t logIndex =
        s_skipLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (logIndex >= 32u && (logIndex % 512u) != 0u)
      return;
    war3dbg::Print(
        "DXVK SemanticCore: skip %s payload=%p part=%p scene=%p mesh=%p "
        "layer=%p runtime=%p modelRes=%p runtimeGeo=%p runtimeGeoData=%p "
        "geoIdx=%u meshIdx=%u handle=%u raw=%08X kind=%u q=%u layerIdx=%u\n",
        reason, resolvedRecord.payload, resolvedRecord.renderablePart,
        resolvedRecord.sceneNode, resolvedRecord.meshData,
        resolvedRecord.layerState, resolvedRecord.runtimeModelPtr,
        resolvedRecord.modelResourcePtr, resolvedRecord.runtimeGeosetPtr,
        resolvedRecord.runtimeGeosetDataPtr, resolvedRecord.geosetIndex,
        resolvedRecord.meshIndex, resolvedRecord.jHandle, resolvedRecord.rawcode,
        uint32_t(resolvedRecord.objectKind),
        uint32_t(resolvedRecord.queueKind), resolvedRecord.layerIndex);
  };
  struct ResolvePhaseTimer {
    uint64_t& targetUs;
    std::chrono::steady_clock::time_point startedAt =
        std::chrono::steady_clock::now();
    ~ResolvePhaseTimer() {
      const uint64_t elapsedUs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - startedAt)
              .count());
      targetUs = (std::max)(targetUs, elapsedUs);
    }
  };

  if (resolvedRecord.rawcode != 0u &&
      IsBlockedSemanticFourCc(resolvedRecord.rawcode)) {
    logSkippedRecord("blocked-fourcc");
    return false;
  }

  if (!hasGeometryHint) {
    logSkippedRecord("no-geometry-hint");
    return false;
  }

  ShadowModelResourceRecord meshDataResource = {};
  const ShadowModelResourceRecord* preResolvedResource = nullptr;
  if (!resolvedRecord.hasResolvedGeoset()) {
    preResolvedResource =
        TryFindStaticMeshDataResource(resolvedRecord, resources, meshDataResource);
  }

  const bool hasStableIdentity = resolvedRecord.hasStableIdentity();
  if (sceneSubmissionRuntime && !hasStableIdentity) {
    ioStats.skippedNoIdentity++;
    logSkippedRecord("anonymous-scene-resource");
    return false;
  }
  if (!resolvedRecord.hasResolvedGeoset()) {
    if (!hasStableIdentity) {
      ioStats.skippedNoIdentity++;
    } else {
      ioStats.skippedNoGeoset++;
      ioStats.skippedNoResolvedGeoset++;
    }
    logSkippedRecord(hasStableIdentity ? "no-resolved-geoset" : "no-identity");
    return false;
  }

  const ShadowModelResourceRecord* resource = nullptr;
  ShadowModelResourceRecord cacheResource = {};
  {
    ResolvePhaseTimer timer{ioStats.slowestResourceLookupUs};
    if (preResolvedResource != nullptr) {
      resource = preResolvedResource;
    } else if (resolvedRecord.runtimeGeosetPtr != nullptr &&
        (resource =
             resources.findByRuntimeGeoset(resolvedRecord.runtimeGeosetPtr)) !=
            nullptr) {
    } else if (resolvedRecord.runtimeGeosetDataPtr != nullptr &&
               (resource = resources.findByRuntimeGeosetData(
                    resolvedRecord.runtimeGeosetDataPtr)) != nullptr) {
    } else if (resolvedRecord.runtimeModelPtr != nullptr &&
               resolvedRecord.geosetIndex !=
                   kInvalidShadowContractGeosetIndex &&
               (resource = resources.findByRuntimeModel(
                    resolvedRecord.runtimeModelPtr,
                    resolvedRecord.geosetIndex)) != nullptr) {
    } else if (resolvedRecord.modelResourcePtr != nullptr &&
               resolvedRecord.geosetIndex !=
                   kInvalidShadowContractGeosetIndex &&
               (resource = resources.findByModelResource(
                    resolvedRecord.modelResourcePtr,
                    resolvedRecord.geosetIndex)) != nullptr) {
    }
    if (resource == nullptr &&
        !dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
        TryFindRenderableResourceFromCache(resolvedRecord,
                                           resolvedRecord.frameSerial,
                                           cacheResource)) {
      resource = &cacheResource;
    }
  }
  if (resource == nullptr) {
    if (!hasStableIdentity) {
      ioStats.skippedNoIdentity++;
    } else {
      ioStats.skippedNoGeoset++;
      ioStats.skippedResourceMiss++;
    }
    logSkippedRecord(hasStableIdentity ? "resource-miss" : "resource-miss-no-id");
    return false;
  }

  if (resource == nullptr || !resource->readyForConsumer()) {
    if (!hasStableIdentity) {
      ioStats.skippedNoIdentity++;
    } else {
      ioStats.skippedNoGeoset++;
      ioStats.skippedResourceNotReady++;
    }
    logSkippedRecord(hasStableIdentity ? "resource-not-ready" :
                                         "resource-not-ready-no-id");
    return false;
  }
  if (sceneSubmissionRuntime && !IsResourceSaneForSceneSubmission(*resource)) {
    ioStats.skippedNoGeoset++;
    ioStats.skippedResourceNotReady++;
    logSkippedRecord("resource-scene-sanity-reject");
    return false;
  }

  if (resolvedRecord.modelResourcePtr == nullptr)
    resolvedRecord.modelResourcePtr = resource->modelResourcePtr;
  if (resolvedRecord.modelKey == 0u)
    resolvedRecord.modelKey = resource->modelKey;

  outPacket.renderable = resolvedRecord;
  outPacket.material = BuildShadowMaterialSignature(resolvedRecord);
  outPacket.resource = MakePacketResourceRef(*resource);
  outPacket.path = ShadowDrawPath::Rigid;
  const bool resourceHasSkinningData = resource->hasSkinningData();
  const bool supplementalRootUnitRenderable =
      IsSupplementalRootUnitRenderable(resolvedRecord);
  const size_t runtimeTreeMaxModels =
      supplementalRootUnitRenderable ? 16u : 32u;
  const size_t runtimeTreeMaxLinks =
      supplementalRootUnitRenderable ? 64u : 128u;
  if (resourceHasSkinningData)
    ioStats.skinnedCandidateCount++;
  else
    ioStats.rigidCandidateCount++;

  if (IsStaticWorldRenderable(resolvedRecord)) {
    ShadowPoseRecord staticPose = {};
    if (!TryReadSceneNodeWorldPose(resolvedRecord, resolvedRecord.frameSerial,
                                   staticPose)) {
      ioStats.skippedNoPose++;
      ioStats.skippedNoPoseNoContext++;
      logSkippedRecord("static-no-scene-pose");
      return false;
    }

    outPacket.renderable = resolvedRecord;
    outPacket.material = BuildShadowMaterialSignature(resolvedRecord);
    outPacket.resource = MakePacketResourceRef(*resource);
    outPacket.pose = std::move(staticPose);
    outPacket.path = ShadowDrawPath::Rigid;
    outPacket.hasRuntimeGroupPalette = false;
    ioStats.rigidResolved++;
    ioStats.resolved++;
    return true;
  }

  ShadowAttachmentRigidRecord attachmentRigid = {};
  ShadowPoseRecord attachmentRigidPose = {};
  auto tryBuildAttachmentRigidPacket = [&](bool fromSkinnedCandidate) {
    if (!MightResolveAttachmentRigidRecord(resolvedRecord, attachments))
      return false;
    ResolvePhaseTimer timer{ioStats.slowestAttachmentRigidResolveUs};
    attachmentRigid = {};
    attachmentRigidPose = {};
    if (!TryBuildAttachmentRigidPose(resolvedRecord, attachments, poses,
                                     resolvedRecord, attachmentRigid,
                                     attachmentRigidPose, &ioStats)) {
      return false;
    }

    outPacket.renderable = resolvedRecord;
    outPacket.material = BuildShadowMaterialSignature(resolvedRecord);
    outPacket.pose = attachmentRigidPose;
    outPacket.path = ShadowDrawPath::Rigid;
    ioStats.attachmentRigidResolved++;
    if (fromSkinnedCandidate)
      ioStats.skinnedCandidateResolvedAsAttachmentRigidCount++;
    ioStats.rigidResolved++;
    ioStats.resolved++;
    return true;
  };
  // Attachment rigid is an authoritative path for child/effect runtimes, but
  // it must not steal regular skinned geosets before the canonical palette path
  // has a chance to build a skinned packet.
  if (!resourceHasSkinningData && tryBuildAttachmentRigidPacket(false))
    return true;

  if (resourceHasSkinningData) {
    const bool anonymousGeosetSubpart =
        !hasStableIdentity &&
        resolvedRecord.unitPtr == nullptr &&
        resolvedRecord.runtimeModelPtr == nullptr &&
        resolvedRecord.modelResourcePtr == nullptr &&
        resolvedRecord.jHandle == 0u &&
        resolvedRecord.rawcode == 0u &&
        resolvedRecord.runtimeGeosetDataPtr != nullptr;
    if (anonymousGeosetSubpart) {
      if (tryBuildAttachmentRigidPacket(true))
        return true;
      ioStats.skippedNoPoseAnonymousSubpart++;
      logSkippedRecord("anonymous-subpart");
      return false;
    }

    const render::RenderObjectInfo* poseContextObject = nullptr;
    auto& poseContextRegistry = render::RenderObjectRegistry::instance();
    if (resolvedRecord.sceneNode != nullptr)
      poseContextObject =
          poseContextRegistry.findBySceneNode(resolvedRecord.sceneNode);
    if (poseContextObject == nullptr && resolvedRecord.worldObjectEntry != nullptr)
      poseContextObject =
          poseContextRegistry.findByEntry(resolvedRecord.worldObjectEntry);
    if (poseContextObject == nullptr && resolvedRecord.jHandle != 0u)
      poseContextObject =
          poseContextRegistry.findByHandle(resolvedRecord.jHandle);
    const bool anonymousSceneWrapper =
        resolvedRecord.unitPtr == nullptr &&
        resolvedRecord.runtimeModelPtr == nullptr &&
        resolvedRecord.modelResourcePtr == nullptr &&
        resolvedRecord.jHandle == 0u &&
        resolvedRecord.rawcode == 0u &&
        resolvedRecord.sceneNode != nullptr &&
        poseContextObject != nullptr &&
        poseContextObject->unitPtr == nullptr;
    if (anonymousSceneWrapper) {
      if (tryBuildAttachmentRigidPacket(true))
        return true;
      ioStats.skippedNoPoseAnonymousSubpart++;
      logSkippedRecord("anonymous-scene-wrapper");
      return false;
    }

    ShadowPoseRecord pose = {};
    PoseResolveMissReason poseMissReason = PoseResolveMissReason::None;
    bool poseResolved = false;
    {
      ResolvePhaseTimer timer{ioStats.slowestPoseResolveUs};
      poseResolved = TryResolveBestPoseForRenderable(resolvedRecord, poses, pose,
                                                     &poseMissReason,
                                                     &ioStats);
    }
    if (!poseResolved) {
      if (sceneSubmissionRuntime) {
        if (tryBuildAttachmentRigidPacket(true))
          return true;
        ioStats.skippedNoPose++;
        switch (poseMissReason) {
        case PoseResolveMissReason::NoContext:
          ioStats.skippedNoPoseNoContext++;
          break;
        case PoseResolveMissReason::AnonymousSubpart:
          ioStats.skippedNoPoseAnonymousSubpart++;
          break;
        case PoseResolveMissReason::LookupMiss:
        default:
          ioStats.skippedNoPoseLookupMiss++;
          break;
        }
        logSkippedRecord("no-direct-pose");
        return false;
      }

      void* resourceTreeRuntimeModelPtr = nullptr;
      ShadowPoseRecord resourceTreePose = {};
      if (TryResolvePoseByResourceRuntimeTree(
              resolvedRecord, *resource, poses, resourceTreeRuntimeModelPtr,
              resourceTreePose, runtimeTreeMaxModels, runtimeTreeMaxLinks)) {
        resolvedRecord.runtimeModelPtr = resourceTreeRuntimeModelPtr;
        if (resolvedRecord.modelResourcePtr == nullptr)
          resolvedRecord.modelResourcePtr = resource->modelResourcePtr;
        pose = std::move(resourceTreePose);
      } else {
        if (resolvedRecord.rawcode != 0u &&
            IsBlockedSemanticFourCc(resolvedRecord.rawcode)) {
          logSkippedRecord("blocked-fourcc-no-pose");
          return false;
        }
        if (tryBuildAttachmentRigidPacket(true))
          return true;
        void* worldPoseRuntimeModelPtr = nullptr;
        ShadowPoseRecord worldOnlyPose = {};
        if (TryResolveResourceOwnerWorldPose(resolvedRecord, poses,
                                             worldPoseRuntimeModelPtr,
                                             worldOnlyPose)) {
          resolvedRecord.runtimeModelPtr = worldPoseRuntimeModelPtr;
          outPacket.renderable = resolvedRecord;
          outPacket.material = BuildShadowMaterialSignature(resolvedRecord);
          outPacket.pose = std::move(worldOnlyPose);
          outPacket.path = ShadowDrawPath::Rigid;
          outPacket.hasRuntimeGroupPalette = false;
          ioStats.explicitResourceOwnerRigidResolved++;
          if (outPacket.pose.hasWorldTransform)
            ioStats.explicitResourceOwnerRigidWorldTransformResolved++;
          if (outPacket.pose.matrixCount == 0u ||
              outPacket.pose.matrixPalette.empty())
            ioStats.explicitResourceOwnerRigidNoMatrixPalette++;
          ioStats.rigidResolved++;
          ioStats.resolved++;
          return true;
        }
        if (tryBuildAttachmentRigidPacket(true))
          return true;
        ioStats.skippedNoPose++;
        switch (poseMissReason) {
        case PoseResolveMissReason::NoContext:
          ioStats.skippedNoPoseNoContext++;
          break;
        case PoseResolveMissReason::AnonymousSubpart:
          ioStats.skippedNoPoseAnonymousSubpart++;
          break;
        case PoseResolveMissReason::LookupMiss:
        default:
          ioStats.skippedNoPoseLookupMiss++;
          break;
        }
        logSkippedRecord("no-pose");
        return false;
      }
    }
    ioStats.skinnedCandidatePoseReadyCount++;

    MeshLayerBindingContract layerContract = {};
    bool hasLayerContract = false;
    {
      ResolvePhaseTimer timer{ioStats.slowestLayerContractUs};
      hasLayerContract =
          TryResolveMeshLayerBindingContract(resolvedRecord, layerContract);
    }
    auto finalizeResolvedPacket =
        [&](const ShadowRenderableRecord& finalRenderable) {
          outPacket.renderable = finalRenderable;
          outPacket.material = BuildShadowMaterialSignature(
              finalRenderable, hasLayerContract ? &layerContract : nullptr);
        };
    auto attachDynamicIndexSlice =
        [&](const ShadowRenderableRecord& finalRenderable) {
          const uint16_t* dynamicIndexStream = nullptr;
          uint32_t dynamicIndexCount = 0u;
          uint32_t dynamicPrimitiveBaseIndex = 0u;
          uint64_t dynamicIndexHash = 0u;
          ShadowPrimitiveTopology dynamicTopology =
              ShadowPrimitiveTopology::TriangleList;
          if (!TryResolveMeshDynamicIndexStream(
                  finalRenderable, *resource, dynamicIndexStream,
                  dynamicIndexCount, dynamicPrimitiveBaseIndex,
                  dynamicIndexHash, dynamicTopology)) {
            return;
          }

          const uint32_t fullIndexCount = uint32_t(resource->indices.size());
          const bool canonicalDirectWholeSlice =
              dynamicIndexStream != nullptr && dynamicIndexCount != 0u &&
              IsCanonicalDirectGeosetWholeSlice(finalRenderable, *resource,
                                                dynamicPrimitiveBaseIndex,
                                                dynamicIndexCount);
          if (dynamicIndexStream == nullptr || dynamicIndexCount == 0u ||
              (!canonicalDirectWholeSlice &&
               dynamicPrimitiveBaseIndex == 0u &&
               dynamicIndexCount == fullIndexCount &&
               resource->primitiveRecords.size() <= 1u)) {
            if (dynamicIndexStream != nullptr && dynamicIndexCount != 0u) {
              TraceDirectGeosetSliceResolution(
                  "direct-full-single-primitive", finalRenderable, *resource,
                  dynamicPrimitiveBaseIndex, dynamicIndexCount);
            }
            return;
          }

          // The semantic path uses stable model/geoset vertices, but the live
          // renderable often draws only the current primitive subset. Carry that
          // index slice forward so hidden build/scaffold submeshes do not cast.
          if (!TryOwnDynamicIndexSlice(outPacket.resource, dynamicIndexStream,
                                       dynamicIndexCount)) {
            return;
          }
          outPacket.resource.dynamicIndexHash = dynamicIndexHash;
          outPacket.resource.dynamicPrimitiveBaseIndex =
              dynamicPrimitiveBaseIndex;
          outPacket.resource.topology = dynamicTopology;
          outPacket.resource.contentHash =
              bit::fnv1a_iter(outPacket.resource.contentHash, dynamicIndexHash);
        };
    const uint32_t resolvedVertexCountEarly =
        outPacket.resource.vertexCount != 0u
            ? outPacket.resource.vertexCount
            : resource->vertexCount != 0u
                  ? resource->vertexCount
                  : uint32_t(resource->positions.size() / 3u);
    const uint32_t maxExpectedGroupSize = [&]() {
      uint32_t value = 0u;
      for (uint32_t groupSize : resource->matrixGroupSizes)
        value = (std::max)(value, groupSize);
      return value;
    }();

    auto tryDynamicMeshRescue = [&](const ShadowPoseRecord& rescuePose) {
      const void* dynamicStreamPtr = nullptr;
      const uint16_t* dynamicIndexStream = nullptr;
      uint64_t dynamicHash = 0u;
      uint64_t dynamicIndexHash = 0u;
      uint64_t dynamicGroupHash = 0u;
      uint32_t dynamicStride = 0u;
      uint32_t dynamicGroupStride = 0u;
      uint32_t dynamicGroupOffset = 0u;
      uint32_t dynamicPrimitiveBaseIndex = 0u;
      bool dynamicGroupsUsePrimaryStream = false;
      int dynamicGroupCandidateSource = kDynamicAuxCandidateStream1;
      uint32_t dynamicVertexCount =
          resource->vertexCount != 0u
              ? resource->vertexCount
              : uint32_t(resource->positions.size() / 3u);
      uint32_t dynamicIndexCount = 0u;
      uint32_t dynamicMaxGroupSlot = 0u;
      ShadowPrimitiveTopology dynamicTopology =
          ShadowPrimitiveTopology::TriangleList;
      std::vector<uint8_t> dynamicGroupSlots;
      std::vector<Matrix4> dynamicRuntimeGroupPalette;
      DynamicTuplePaletteResult dynamicTupleGroups = {};
      ShadowPoseRecord dynamicPose = rescuePose;
      void* meshPoseRuntimeModelPtr = nullptr;
      ShadowPoseRecord meshPose = {};
      const bool meshPoseHit =
          TryResolveMeshPoseContext(resolvedRecord, poses, meshPoseRuntimeModelPtr,
                                    meshPose) &&
          meshPoseRuntimeModelPtr != nullptr;
      if (meshPoseHit && meshPose.matrixCount > dynamicPose.matrixCount)
        dynamicPose = meshPose;
      const bool hasDynamicPrimaryStream =
          TryResolveMeshPrimaryDynamicStream(resolvedRecord, *resource,
                                             dynamicStreamPtr, dynamicHash,
                                             dynamicStride);
      if (dynamicVertexCount == 0u || dynamicVertexCount > 200000u)
        dynamicVertexCount = resource->vertexCount;
      TryResolveMeshDynamicIndexStream(
          resolvedRecord, *resource, dynamicIndexStream, dynamicIndexCount,
          dynamicPrimitiveBaseIndex, dynamicIndexHash, dynamicTopology);
      const uint32_t dynamicPosePaletteLimit =
          dynamicPose.matrixCount != 0u && !dynamicPose.matrixPalette.empty()
              ? (std::min)((std::min)(dynamicPose.matrixCount,
                                      uint32_t(dynamicPose.matrixPalette.size())),
                           256u)
              : 0u;
      uint32_t maxExpectedGroupSize = 0u;
      for (uint32_t groupSize : resource->matrixGroupSizes)
        maxExpectedGroupSize = (std::max)(maxExpectedGroupSize, groupSize);
      const uint32_t dynamicContractGroupSlotLimit = [&]() {
        if (!resource->matrixGroupSizes.empty()) {
          return (std::min)(255u,
                            uint32_t(resource->matrixGroupSizes.size() - 1u));
        }
        if (dynamicPosePaletteLimit != 0u)
          return dynamicPosePaletteLimit - 1u;
        return 255u;
      }();
      const bool hasPackedDynamicGroups =
          dynamicPosePaletteLimit != 0u &&
          TryResolveMeshDynamicPackedRuntimeGroups(
              resolvedRecord, dynamicVertexCount, dynamicPosePaletteLimit,
              maxExpectedGroupSize, dynamicPose, dynamicTupleGroups,
              hasLayerContract ? &layerContract : nullptr);
      if (hasPackedDynamicGroups) {
        dynamicGroupSlots = dynamicTupleGroups.groupSlots;
        dynamicRuntimeGroupPalette = dynamicTupleGroups.runtimeGroupPalette;
        dynamicMaxGroupSlot = dynamicTupleGroups.maxGroupSlot;
        dynamicGroupHash = dynamicTupleGroups.dynamicHash;
        dynamicGroupStride = dynamicTupleGroups.strideUsed;
        dynamicGroupOffset = dynamicTupleGroups.offsetUsed;
        dynamicGroupsUsePrimaryStream = dynamicTupleGroups.usedPrimaryStream;
        dynamicGroupCandidateSource = dynamicTupleGroups.auxEntryIndexUsed;
      }
      const bool hasDynamicGroupSlots =
          hasPackedDynamicGroups ||
          (dynamicContractGroupSlotLimit < 256u &&
            TryResolveMeshDynamicGroupSlots(resolvedRecord, dynamicVertexCount,
                                            dynamicContractGroupSlotLimit,
                                            dynamicGroupSlots,
                                            dynamicMaxGroupSlot, dynamicGroupHash,
                                            dynamicGroupStride,
                                            dynamicGroupOffset,
                                            hasLayerContract ? &layerContract : nullptr,
                                            dynamicGroupsUsePrimaryStream,
                                            dynamicGroupCandidateSource));
      const bool canDynamicSkin =
          hasPackedDynamicGroups ||
          (hasDynamicGroupSlots &&
           TryBuildDirectPosePaletteForDynamicGroups(
               dynamicPose, dynamicMaxGroupSlot, dynamicRuntimeGroupPalette));
      if (!canDynamicSkin) {
        uint32_t staticMaxGroupSlot = 0u;
        std::vector<Matrix4> staticRuntimeGroupPalette;
        if (TryBuildDirectPosePaletteForStaticVertexGroups(
                *resource, dynamicVertexCount, dynamicPose,
                staticMaxGroupSlot, staticRuntimeGroupPalette)) {
          dynamicMaxGroupSlot = staticMaxGroupSlot;
          dynamicRuntimeGroupPalette = std::move(staticRuntimeGroupPalette);
        }
      }
      const bool effectiveDynamicSkin =
          !dynamicRuntimeGroupPalette.empty() &&
          dynamicMaxGroupSlot < dynamicRuntimeGroupPalette.size();
      const uint32_t staticVertexCount =
          resource->vertexCount != 0u
              ? resource->vertexCount
              : uint32_t(resource->positions.size() / 3u);
      const bool staticGeometryReadyForSkinnedRescue =
          effectiveDynamicSkin && !resource->positions.empty() &&
          resource->hasSkinningData() && staticVertexCount != 0u &&
          dynamicVertexCount == staticVertexCount &&
          resource->vertexGroupIndices.size() >= size_t(staticVertexCount) &&
          (!resource->indices.empty() || !resource->primitiveRecords.empty());
      const bool preferFrameLocalDynamicMesh =
          sceneSubmissionRuntime &&
          dxvk::war3::internal::
              kShadowSemanticCorePreferFrameLocalDynamicMeshForSkinned;
      const bool allowLivePaletteOnlySkinned =
          sceneSubmissionRuntime && hasDynamicGroupSlots &&
          dynamicMaxGroupSlot <= dynamicContractGroupSlotLimit;
      const bool preferStaticSkinnedRescue =
          !preferFrameLocalDynamicMesh &&
          staticGeometryReadyForSkinnedRescue &&
          (dynamicIndexStream == nullptr || dynamicIndexCount == 0u);
      const bool treatFrameLocalAsPreSkinned =
          preferFrameLocalDynamicMesh &&
          dxvk::war3::internal::
              kShadowSemanticCoreTreatFrameLocalDynamicMeshAsPreSkinned;
      const bool finalDynamicSkin =
          effectiveDynamicSkin && !treatFrameLocalAsPreSkinned;
      const bool finalLivePaletteOnlySkinned =
          !finalDynamicSkin && allowLivePaletteOnlySkinned;

      if (!hasDynamicPrimaryStream && !preferStaticSkinnedRescue &&
          !finalLivePaletteOnlySkinned) {
        return false;
      }

      finalizeResolvedPacket(resolvedRecord);
      outPacket.resource = MakePacketResourceRef(*resource);
      outPacket.resource.vertexCount =
          (preferStaticSkinnedRescue || finalLivePaletteOnlySkinned)
              ? staticVertexCount
              : dynamicVertexCount;
      if (!preferStaticSkinnedRescue && !finalLivePaletteOnlySkinned) {
        outPacket.resource.dynamicPositionStream = dynamicStreamPtr;
        outPacket.resource.dynamicPositionStride = dynamicStride;
        if (dynamicIndexStream != nullptr && dynamicIndexCount != 0u &&
            !TryOwnDynamicIndexSlice(outPacket.resource, dynamicIndexStream,
                                     dynamicIndexCount)) {
          return false;
        }
        outPacket.resource.dynamicIndexHash = dynamicIndexHash;
        outPacket.resource.dynamicPrimitiveBaseIndex = dynamicPrimitiveBaseIndex;
        outPacket.resource.topology = dynamicTopology;
        outPacket.resource.contentHash =
            bit::fnv1a_iter(resource->contentHash, dynamicHash);
        outPacket.resource.contentHash =
            bit::fnv1a_iter(outPacket.resource.contentHash, dynamicIndexHash);
      }
      if (hasDynamicGroupSlots) {
        outPacket.resource.ownedVertexGroupIndices = std::move(dynamicGroupSlots);
        outPacket.resource.vertexGroupIndices =
            &outPacket.resource.ownedVertexGroupIndices;
        outPacket.resource.contentHash =
            bit::fnv1a_iter(outPacket.resource.contentHash, dynamicGroupHash);
      }
      outPacket.pose = dynamicPose;
      outPacket.usesDynamicMeshPositions =
          !preferStaticSkinnedRescue && !finalLivePaletteOnlySkinned;
      outPacket.path =
          (finalDynamicSkin || finalLivePaletteOnlySkinned)
              ? ShadowDrawPath::Skinned
              : ShadowDrawPath::Rigid;
      outPacket.hasRuntimeGroupPalette = finalDynamicSkin;
      outPacket.matrixGroupsUseAveraging =
          finalDynamicSkin && hasPackedDynamicGroups &&
          dynamicTupleGroups.usesAveraging;
      outPacket.maxVertexGroupSlot =
          (finalDynamicSkin || finalLivePaletteOnlySkinned)
              ? dynamicMaxGroupSlot
              : 0u;
      outPacket.runtimeGroupPalette = finalDynamicSkin
                                          ? std::move(dynamicRuntimeGroupPalette)
                                          : std::vector<Matrix4>{};
      outPacket.runtimeGroupPaletteHash =
          outPacket.runtimeGroupPalette.empty()
              ? 0ull
              : HashMatrixPalette(outPacket.runtimeGroupPalette);

      if (SemanticCoreTraceEnabled()) {
        static std::atomic<uint32_t> s_dynamicMeshRescueLogCount{0};
        const uint32_t logIndex =
            s_dynamicMeshRescueLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 32u || (logIndex % 2048u) == 0u) {
          uint32_t aux0Head0 = 0u;
          uint32_t aux0Head4 = 0u;
          uint32_t aux0Sample10 = 0u;
          uint32_t aux0Sample14 = 0u;
          uint32_t aux0Sample20 = 0u;
          uint32_t aux0Sample24 = 0u;
          uint32_t aux1Head0 = 0u;
          uint32_t aux1Head4 = 0u;
          uint32_t stream1Word0 = 0u;
          uint32_t stream1Word1 = 0u;
          uint32_t stream1Word2 = 0u;
          uint32_t state1CWord0 = 0u;
          uint32_t state1CWord4 = 0u;
          uint32_t state1CWord8 = 0u;
          uint32_t state20Word0 = 0u;
          uint32_t state20Word4 = 0u;
          uint32_t state20Word8 = 0u;
          void* rawStream1Ptr = nullptr;
          if (resolvedRecord.meshData != nullptr) {
            dxvk::war3::SafeReadPtrFast(resolvedRecord.meshData,
                                        dxvk::war3::MeshDataOffsets::Stream1Ptr,
                                        rawStream1Ptr);
          }
          const bool stream1Word0Valid =
              TryReadLinearWordSample(rawStream1Ptr, 0u, stream1Word0);
          const bool stream1Word1Valid =
              TryReadLinearWordSample(rawStream1Ptr, 4u, stream1Word1);
          const bool stream1Word2Valid =
              TryReadLinearWordSample(rawStream1Ptr, 8u, stream1Word2);
          const bool aux0HeadValid =
              hasLayerContract &&
              TryReadAuxStreamHead(layerContract.auxStreamPtr0, aux0Head0,
                                   aux0Head4);
          const bool aux0Sample1Valid =
              hasLayerContract &&
              TryReadAuxStreamSample(layerContract.auxStreamPtr0, 1u,
                                     aux0Sample10, aux0Sample14);
          const bool aux0Sample2Valid =
              hasLayerContract &&
              TryReadAuxStreamSample(layerContract.auxStreamPtr0, 2u,
                                     aux0Sample20, aux0Sample24);
          const bool aux1HeadValid =
              hasLayerContract &&
              TryReadAuxStreamHead(layerContract.auxStreamPtr1, aux1Head0,
                                   aux1Head4);
          const bool state1CValid =
              hasLayerContract &&
              LooksLikeDynamicAuxPointerCandidate(layerContract.layerStateWord1C) &&
              TryReadPointerWordWindow(
                  reinterpret_cast<const void*>(
                      uintptr_t(layerContract.layerStateWord1C)),
                  state1CWord0, state1CWord4, state1CWord8);
          const bool state20Valid =
              hasLayerContract &&
              LooksLikeDynamicAuxPointerCandidate(layerContract.layerStateWord20) &&
              TryReadPointerWordWindow(
                  reinterpret_cast<const void*>(
                      uintptr_t(layerContract.layerStateWord20)),
                  state20Word0, state20Word4, state20Word8);
          dxvk::war3dbg::Print(
              "DXVK SemanticCore: dynamic-mesh rescue runtime=%p model=%p "
              "mesh=%p geoIdx=%u stride=%u vertices=%u dynIdx=%u "
              "topo=%u baseIdx=%u dynGrp=%zu dynGrpStride=%u dynGrpOff=%u "
              "dynGrpPacked=%d dynGrpPrimary=%d dynGrpSrc=%d "
              "state[canon=%d base=%d view=%d] "
              "bind[p=%u mode=%u stage=%u/%u a0=%u/%d/%p/%u a1=%u/%d/%p/%u] "
              "preset[sceneBase=%u spanBase=%u idx=%u/%u resolved=%u/%u] "
              "raw[state=%08X/%08X/%08X/%08X aux0=%08X/%08X aux1=%08X/%08X "
              "stream1=%d:%08X/%08X/%08X "
              "aux0v=%d:%08X/%08X|%08X/%08X|%08X/%08X "
              "head1=%d:%08X/%08X "
              "l1c=%d:%08X/%08X/%08X l20=%d:%08X/%08X/%08X] "
              "dynSkin=%d "
              "meshPose=%d poseCount=%u maxSlot=%u "
              "handle=0x%08X raw=0x%08X\n",
              resolvedRecord.runtimeModelPtr, resource->modelResourcePtr, resolvedRecord.meshData,
              resource->geosetIndex, dynamicStride, outPacket.resource.vertexCount,
              dynamicIndexCount, uint32_t(dynamicTopology),
              dynamicPrimitiveBaseIndex,
              outPacket.resource.vertexGroupIndexVec().size(), dynamicGroupStride,
              dynamicGroupOffset,
              hasPackedDynamicGroups ? 1 : 0,
              dynamicGroupsUsePrimaryStream ? 1 : 0,
              dynamicGroupCandidateSource,
              hasLayerContract && layerContract.usesCanonicalLayerStateRecord ? 1 : 0,
              hasLayerContract && layerContract.batchLayerStateMatchesRecordBase ? 1 : 0,
              hasLayerContract && layerContract.batchLayerStateMatchesStateView ? 1 : 0,
              hasLayerContract ? layerContract.primaryResourceBinding : 0u,
              hasLayerContract ? layerContract.blendOrDrawMode : 0u,
              hasLayerContract ? layerContract.stageMode0 : 0u,
              hasLayerContract ? layerContract.stageMode1 : 0u,
              hasLayerContract ? layerContract.auxRefEnable0 : 0u,
              hasLayerContract ? layerContract.auxRefIndex0 : -1,
              hasLayerContract && layerContract.hasAuxStream0
                  ? reinterpret_cast<const void*>(layerContract.auxStreamPtr0)
                  : nullptr,
              hasLayerContract ? layerContract.auxStreamStride0 : 0u,
              hasLayerContract ? layerContract.auxRefEnable1 : 0u,
              hasLayerContract ? layerContract.auxRefIndex1 : -1,
              hasLayerContract && layerContract.hasAuxStream1
                  ? reinterpret_cast<const void*>(layerContract.auxStreamPtr1)
                  : nullptr,
              hasLayerContract ? layerContract.auxStreamStride1 : 0u,
              hasLayerContract ? layerContract.sceneStagePresetBaseIndex : 0u,
              hasLayerContract ? layerContract.stagePresetSpanBaseIndex : 0u,
              hasLayerContract ? layerContract.stagePresetIndex0 : 0u,
              hasLayerContract ? layerContract.stagePresetIndex1 : 0u,
              hasLayerContract ? layerContract.resolvedStagePresetIndex0 : 0u,
              hasLayerContract ? layerContract.resolvedStagePresetIndex1 : 0u,
              hasLayerContract ? layerContract.layerStateWord0 : 0u,
              hasLayerContract ? layerContract.layerStateWord18 : 0u,
              hasLayerContract ? layerContract.layerStateWord1C : 0u,
              hasLayerContract ? layerContract.layerStateWord20 : 0u,
              hasLayerContract && layerContract.hasAuxEntry0Snapshot
                  ? layerContract.auxEntry0Word0
                  : 0u,
              hasLayerContract ? layerContract.auxEntry0Word8 : 0u,
              hasLayerContract && layerContract.hasAuxEntry1Snapshot
                  ? layerContract.auxEntry1Word0
                  : 0u,
              hasLayerContract ? layerContract.auxEntry1Word8 : 0u,
              stream1Word0Valid ? 1 : 0,
              stream1Word0,
              stream1Word1Valid ? stream1Word1 : 0u,
              stream1Word2Valid ? stream1Word2 : 0u,
              aux0HeadValid ? 1 : 0,
              aux0Head0,
              aux0Head4,
              aux0Sample1Valid ? aux0Sample10 : 0u,
              aux0Sample1Valid ? aux0Sample14 : 0u,
              aux0Sample2Valid ? aux0Sample20 : 0u,
              aux0Sample2Valid ? aux0Sample24 : 0u,
              aux1HeadValid ? 1 : 0,
              aux1Head0,
              aux1Head4,
              state1CValid ? 1 : 0,
              state1CWord0,
              state1CWord4,
              state1CWord8,
              state20Valid ? 1 : 0,
              state20Word0,
              state20Word4,
              state20Word8,
              effectiveDynamicSkin ? 1 : 0,
              meshPoseHit ? 1 : 0, dynamicPose.matrixCount,
              dynamicMaxGroupSlot,
              resolvedRecord.jHandle, resolvedRecord.rawcode);
        }
      }
      return true;
    };

    std::vector<Matrix4> runtimeGroupPalette;
    uint32_t maxVertexGroupSlot = 0u;
    bool usesAveraging = false;
    RuntimeGroupPaletteMissDetail paletteMissDetail = {};
    auto noteRuntimeGroupPaletteMiss = [&]() {
      AccumulateRuntimeGroupPaletteMissStats(ioStats, paletteMissDetail);
    };
    ShadowRenderableRecord resolvedRenderable = resolvedRecord;
    auto logRuntimeGroupPaletteSkip = [&](const char* phaseTag,
                                          size_t descendantRuntimeCount = 0u,
                                          size_t descendantPoseHitCount = 0u,
                                          uint32_t bestDescendantPoseCount = 0u,
                                          size_t matchingModelDescendantCount = 0u,
                                          uint32_t bestMatchingModelPoseCount = 0u) {
      if (!SemanticCoreTraceEnabled())
        return;
      static std::atomic<uint32_t> s_runtimeGroupPaletteSkipLogCount{0u};
      const uint32_t logIndex =
          s_runtimeGroupPaletteSkipLogCount.fetch_add(1u,
                                                     std::memory_order_relaxed);
      if (logIndex >= 24u && (logIndex % 1024u) != 0u)
        return;
      war3dbg::Print(
          "DXVK SemanticCore: no-runtime-group-palette phase=%s reason=%u "
          "group=%u matrix=%u poseCount=%u groupCount=%u matrixIndices=%u "
          "maxSlot=%u runtime=%p model=%p modelRes=%p runtimeGeo=%p "
          "runtimeGeoData=%p scene=%p mesh=%p handle=%u raw=%08X kind=%u\n",
          phaseTag, uint32_t(paletteMissDetail.reason),
          paletteMissDetail.group, paletteMissDetail.matrixIndex,
          paletteMissDetail.poseCount, paletteMissDetail.groupCount,
          paletteMissDetail.matrixIndexCount,
          paletteMissDetail.maxVertexGroupSlot,
          resolvedRenderable.runtimeModelPtr, resource->modelResourcePtr,
          resolvedRenderable.modelResourcePtr,
          resolvedRenderable.runtimeGeosetPtr,
          resolvedRenderable.runtimeGeosetDataPtr,
          resolvedRenderable.sceneNode, resolvedRenderable.meshData,
          resolvedRenderable.jHandle, resolvedRenderable.rawcode,
          uint32_t(resolvedRenderable.objectKind));
      if (std::strcmp(phaseTag, "no-descendant-runtime") == 0u) {
        war3dbg::Print(
            "DXVK SemanticCore: no-descendant-runtime detail runtime=%p model=%p "
            "descRuntimeCount=%zu descPoseHitCount=%zu bestDescPoseCount=%u "
            "matchingModelDescCount=%zu bestMatchingModelPoseCount=%u\n",
            resolvedRenderable.runtimeModelPtr, resource->modelResourcePtr,
            descendantRuntimeCount, descendantPoseHitCount,
            bestDescendantPoseCount, matchingModelDescendantCount,
            bestMatchingModelPoseCount);
      }
    };

    if (sceneSubmissionRuntime &&
        dxvk::war3::internal::
            kShadowSemanticCorePreferFrameLocalDynamicMeshForSkinned &&
        tryDynamicMeshRescue(pose)) {
      ioStats.skinnedResolved++;
      ioStats.resolved++;
      return true;
    }

    bool runtimeGroupPaletteReady = false;
    {
      ResolvePhaseTimer timer{ioStats.slowestRuntimeGroupPaletteUs};
      runtimeGroupPaletteReady =
          TryBuildRuntimeGroupPalette(*resource, resolvedRenderable, pose, poses,
                                      runtimeGroupPalette,
                                      maxVertexGroupSlot, usesAveraging,
                                      &paletteMissDetail);
    }
    if (!runtimeGroupPaletteReady) {
      ResolvePhaseTimer rescueTimer{
          ioStats.slowestRuntimeGroupPaletteRescueUs};
      bool rescuedByMeshPoseContext = false;
      void* meshPoseRuntimeModelPtr = nullptr;
      ShadowPoseRecord meshPose = {};
      if (TryResolveMeshPoseContext(resolvedRecord, poses, meshPoseRuntimeModelPtr,
                                    meshPose) &&
          meshPoseRuntimeModelPtr != nullptr &&
          meshPoseRuntimeModelPtr != resolvedRecord.runtimeModelPtr) {
        ShadowRenderableRecord meshPoseRenderable = resolvedRecord;
        meshPoseRenderable.runtimeModelPtr = meshPoseRuntimeModelPtr;
        runtimeGroupPalette.clear();
        maxVertexGroupSlot = 0u;
        usesAveraging = false;
        if (TryBuildRuntimeGroupPalette(*resource, meshPoseRenderable, meshPose,
                                        poses, runtimeGroupPalette,
                                        maxVertexGroupSlot, usesAveraging,
                                        &paletteMissDetail)) {
          resolvedRenderable = meshPoseRenderable;
          pose = std::move(meshPose);
          rescuedByMeshPoseContext = true;
          ioStats.runtimeGroupPaletteRescueByMeshPoseContext++;
        }
      }

      bool rescuedByResourceMatchedPose = false;
      const bool allowResourceMatchedPoseRescue = !sceneSubmissionRuntime;
      if (!rescuedByMeshPoseContext && resource->modelResourcePtr != nullptr &&
          allowResourceMatchedPoseRescue) {
        size_t scannedPoseCount = 0u;
        constexpr size_t kMaxResourcePoseScan = 48u;
        for (const auto& candidatePose : poses.records()) {
          if (scannedPoseCount++ >= kMaxResourcePoseScan)
            break;
          if (candidatePose.runtimeModelPtr == nullptr ||
              candidatePose.matrixCount == 0u ||
              candidatePose.matrixPalette.empty()) {
            continue;
          }

          void* candidateModelResourcePtr =
              TryResolveDirectModelResourceFromRuntimeModel(
                  candidatePose.runtimeModelPtr);
          candidateModelResourcePtr =
              model::ShadowModelResourceCache::instance()
                  .resolveDirectModelResourcePtr(candidateModelResourcePtr);
          if (candidateModelResourcePtr != resource->modelResourcePtr)
            continue;

          ShadowRenderableRecord candidateRenderable = resolvedRecord;
          candidateRenderable.runtimeModelPtr = candidatePose.runtimeModelPtr;
          if (candidateRenderable.modelResourcePtr == nullptr)
            candidateRenderable.modelResourcePtr = resource->modelResourcePtr;
          runtimeGroupPalette.clear();
          maxVertexGroupSlot = 0u;
          usesAveraging = false;
          if (!TryBuildRuntimeGroupPalette(*resource, candidateRenderable,
                                           candidatePose, poses,
                                           runtimeGroupPalette,
                                           maxVertexGroupSlot, usesAveraging,
                                           &paletteMissDetail)) {
            continue;
          }

          resolvedRenderable = candidateRenderable;
          pose = candidatePose;
          rescuedByResourceMatchedPose = true;
          ioStats.runtimeGroupPaletteRescueByResourceMatchedPose++;
          break;
        }
      } else if (!rescuedByMeshPoseContext &&
                 resource->modelResourcePtr != nullptr &&
                 sceneSubmissionRuntime) {
        // modelResource is shared by every instance of the same unit model.
        // Borrowing a pose by resource alone makes one caster's silhouette leak
        // onto sibling casters, so the formal semantic scene path must only use
        // instance-bound pose sources.
        ioStats.runtimeGroupPaletteResourceMatchedPoseSuppressed++;
      }

      bool rescuedByDescendantRuntime = false;
      void* childRuntimeModelPtr = nullptr;
      ShadowPoseRecord childPose = {};
      constexpr bool kPreferCanonicalGroupPaletteOnly =
          dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled;
      std::vector<void*> runtimeRoots;
      CollectRenderableRuntimeModelRoots(resolvedRecord, runtimeRoots);
      const bool runtimeRootPoseHit = !rescuedByMeshPoseContext &&
          !rescuedByResourceMatchedPose &&
          [&]() {
            for (void* runtimeRoot : runtimeRoots) {
              if (runtimeRoot == nullptr ||
                  runtimeRoot == resolvedRecord.runtimeModelPtr) {
                continue;
              }

              ShadowPoseRecord candidatePose = {};
              if (!TryResolvePoseByRuntimeModelSnapshotOrLive(
                      poses, runtimeRoot, candidatePose)) {
                continue;
              }

              ShadowRenderableRecord candidateRenderable = resolvedRecord;
              candidateRenderable.runtimeModelPtr = runtimeRoot;
              runtimeGroupPalette.clear();
              maxVertexGroupSlot = 0u;
              usesAveraging = false;
              if (!TryBuildRuntimeGroupPalette(*resource, candidateRenderable,
                                               candidatePose, poses,
                                               runtimeGroupPalette,
                                               maxVertexGroupSlot,
                                               usesAveraging,
                                               &paletteMissDetail)) {
                continue;
              }

              childRuntimeModelPtr = runtimeRoot;
              childPose = std::move(candidatePose);
              ioStats.runtimeGroupPaletteRescueByRuntimeRoot++;
              return true;
            }
            return false;
          }();
      const bool childPoseHit = !runtimeRootPoseHit &&
          !rescuedByMeshPoseContext &&
          !rescuedByResourceMatchedPose &&
          resource->modelResourcePtr != nullptr &&
          [&]() {
            for (void* runtimeRoot : runtimeRoots) {
              if (!TryResolveChildRuntimeModelByModelResource(
                      runtimeRoot, resource->modelResourcePtr,
                      childRuntimeModelPtr, runtimeTreeMaxModels,
                      runtimeTreeMaxLinks) ||
                  childRuntimeModelPtr == nullptr ||
                  childRuntimeModelPtr == runtimeRoot) {
                continue;
              }

              ShadowPoseRecord candidatePose = {};
              if (!TryResolvePoseByRuntimeModelSnapshotOrLive(
                      poses, childRuntimeModelPtr, candidatePose)) {
                continue;
              }

              childPose = std::move(candidatePose);
              ioStats.runtimeGroupPaletteRescueByChildRuntime++;
              return true;
            }
            return false;
          }();

      if (!rescuedByMeshPoseContext && !rescuedByResourceMatchedPose &&
          !childPoseHit) {
        size_t descendantRuntimeCount = 0u;
        size_t descendantPoseHitCount = 0u;
        uint32_t bestDescendantPoseCount = 0u;
        size_t matchingModelDescendantCount = 0u;
        uint32_t bestMatchingModelPoseCount = 0u;
        for (void* runtimeRoot : runtimeRoots) {
          const auto runtimeTree = CollectRuntimeModelTree(
              runtimeRoot, runtimeTreeMaxModels, runtimeTreeMaxLinks);
          descendantRuntimeCount += runtimeTree.size();
          for (void* candidateRuntimeModelPtr : runtimeTree) {
            if (candidateRuntimeModelPtr == nullptr ||
                candidateRuntimeModelPtr == runtimeRoot) {
              continue;
            }

            ShadowPoseRecord candidatePose = {};
            if (!TryResolvePoseByRuntimeModelSnapshotOrLive(
                    poses, candidateRuntimeModelPtr, candidatePose)) {
              continue;
            }
            descendantPoseHitCount += 1u;
            bestDescendantPoseCount =
                (std::max)(bestDescendantPoseCount, candidatePose.matrixCount);

            if (resource->modelResourcePtr != nullptr &&
                TryResolveDirectModelResourceFromRuntimeModel(
                    candidateRuntimeModelPtr) == resource->modelResourcePtr) {
              matchingModelDescendantCount += 1u;
              bestMatchingModelPoseCount =
                  (std::max)(bestMatchingModelPoseCount,
                             candidatePose.matrixCount);
            }

            ShadowRenderableRecord candidateRenderable = resolvedRecord;
            candidateRenderable.runtimeModelPtr = candidateRuntimeModelPtr;
            runtimeGroupPalette.clear();
            maxVertexGroupSlot = 0u;
            usesAveraging = false;
            if (!TryBuildRuntimeGroupPalette(*resource, candidateRenderable,
                                             candidatePose, poses,
                                             runtimeGroupPalette,
                                             maxVertexGroupSlot, usesAveraging,
                                             &paletteMissDetail)) {
              continue;
            }

            resolvedRenderable = candidateRenderable;
            pose = std::move(candidatePose);
            rescuedByDescendantRuntime = true;
            ioStats.runtimeGroupPaletteRescueByDescendantRuntime++;
            break;
          }

          if (rescuedByDescendantRuntime)
            break;
        }

        if (!rescuedByDescendantRuntime) {
          if constexpr (kPreferCanonicalGroupPaletteOnly) {
            noteRuntimeGroupPaletteMiss();
            if (tryBuildAttachmentRigidPacket(true))
              return true;
            logRuntimeGroupPaletteSkip("no-descendant-runtime",
                                       descendantRuntimeCount,
                                       descendantPoseHitCount,
                                       bestDescendantPoseCount,
                                       matchingModelDescendantCount,
                                       bestMatchingModelPoseCount);
            ioStats.skippedNoRuntimeGroupPalette++;
            return false;
          }
          if (tryDynamicMeshRescue(pose)) {
            ioStats.skinnedResolved++;
            ioStats.resolved++;
            return true;
          }
          ioStats.skippedNoRuntimeGroupPalette++;
          return false;
        }
      } else if (!rescuedByMeshPoseContext && !rescuedByResourceMatchedPose) {
        resolvedRenderable.runtimeModelPtr = childRuntimeModelPtr;
        runtimeGroupPalette.clear();
        maxVertexGroupSlot = 0u;
        usesAveraging = false;
        if (!TryBuildRuntimeGroupPalette(*resource, resolvedRenderable, childPose,
                                         poses, runtimeGroupPalette,
                                         maxVertexGroupSlot, usesAveraging,
                                         &paletteMissDetail)) {
          pose = std::move(childPose);
        } else {
          pose = std::move(childPose);
        }
      }

      if (runtimeGroupPalette.empty()) {
        if constexpr (kPreferCanonicalGroupPaletteOnly) {
          noteRuntimeGroupPaletteMiss();
          if (tryBuildAttachmentRigidPacket(true))
            return true;
          logRuntimeGroupPaletteSkip("mesh-pose-or-descendant-empty");
          ioStats.skippedNoRuntimeGroupPalette++;
          return false;
        }
        if (tryDynamicMeshRescue(pose)) {
          ioStats.skinnedResolved++;
          ioStats.resolved++;
          return true;
        }
      }

      if (runtimeGroupPalette.empty()) {
        if constexpr (kPreferCanonicalGroupPaletteOnly) {
          noteRuntimeGroupPaletteMiss();
          if (tryBuildAttachmentRigidPacket(true))
            return true;
          logRuntimeGroupPaletteSkip("explicit-blend-skipped");
          ioStats.skippedNoRuntimeGroupPalette++;
          return false;
        }
        ExplicitBlendSkinResult explicitBlendRescue = {};
        if (resolvedVertexCountEarly != 0u && hasLayerContract &&
            TryResolveMeshDynamicExplicitBlendSkinning(
                resolvedRenderable, resolvedVertexCountEarly,
                uint32_t((std::min)(pose.matrixPalette.size(), size_t(256u))),
                maxExpectedGroupSize, pose, &layerContract,
                explicitBlendRescue, &ioStats)) {
          outPacket.resource.ownedVertexBlendWeights =
              std::move(explicitBlendRescue.weights);
          outPacket.resource.vertexBlendWeights =
              &outPacket.resource.ownedVertexBlendWeights;
          outPacket.resource.ownedVertexBlendIndices =
              std::move(explicitBlendRescue.indices);
          outPacket.resource.vertexBlendIndices =
              &outPacket.resource.ownedVertexBlendIndices;
          outPacket.resource.explicitBlendCount =
              explicitBlendRescue.blendCount;
          outPacket.resource.contentHash = bit::fnv1a_iter(
              outPacket.resource.contentHash, explicitBlendRescue.dynamicHash);
          finalizeResolvedPacket(resolvedRenderable);
          attachDynamicIndexSlice(resolvedRenderable);
          outPacket.pose = pose;
          outPacket.path = ShadowDrawPath::Skinned;
          outPacket.hasRuntimeGroupPalette = true;
          outPacket.matrixGroupsUseAveraging = false;
          outPacket.maxVertexGroupSlot = explicitBlendRescue.maxGroupSlot;
          outPacket.runtimeGroupPalette =
              std::move(explicitBlendRescue.runtimeGroupPalette);
          outPacket.runtimeGroupPaletteHash =
              outPacket.runtimeGroupPalette.empty()
                  ? 0ull
                  : HashMatrixPalette(outPacket.runtimeGroupPalette);
          ioStats.explicitBlendResolved++;
          if (explicitBlendRescue.usedSpanRemap)
            ioStats.explicitBlendSpanRemapResolved++;
          ioStats.skinnedCandidateRuntimeGroupPaletteReadyCount++;
          ioStats.skinnedResolved++;
          ioStats.resolved++;
          return true;
        }
      }

      if (runtimeGroupPalette.empty()) {
        noteRuntimeGroupPaletteMiss();
        if (tryBuildAttachmentRigidPacket(true))
          return true;
        logRuntimeGroupPaletteSkip("final-empty");
        ioStats.skippedNoRuntimeGroupPalette++;
        return false;
      }
    }

    finalizeResolvedPacket(resolvedRenderable);
    attachDynamicIndexSlice(resolvedRenderable);
    outPacket.pose = std::move(pose);
    outPacket.path = ShadowDrawPath::Skinned;
    outPacket.hasRuntimeGroupPalette = true;
    outPacket.matrixGroupsUseAveraging = usesAveraging;
    outPacket.maxVertexGroupSlot = maxVertexGroupSlot;
    outPacket.runtimeGroupPalette = std::move(runtimeGroupPalette);
    outPacket.runtimeGroupPaletteHash =
        outPacket.runtimeGroupPalette.empty()
            ? 0ull
            : HashMatrixPalette(outPacket.runtimeGroupPalette);
    ioStats.skinnedCandidateRuntimeGroupPaletteReadyCount++;
    ioStats.skinnedResolved++;
  } else {
    ShadowPoseRecord pose = {};
    TryResolveBestPoseForRenderable(resolvedRecord, poses, pose);
    outPacket.pose = std::move(pose);
    ioStats.rigidResolved++;
  }

  if (!outPacket.material.valid())
    outPacket.material = BuildShadowMaterialSignature(outPacket.renderable);
  ioStats.resolved++;
  return true;
}

ShadowValidationRuntime& ShadowValidationRuntime::instance() {
  static ShadowValidationRuntime* s_instance =
      new ShadowValidationRuntime();
  return *s_instance;
}

namespace {

bool IsSemanticContractNewer(
    const std::shared_ptr<const ShadowFrameManifest>& lhs,
    const std::shared_ptr<const ShadowFrameManifest>& rhs) {
  if (!lhs)
    return false;
  if (!rhs)
    return true;
  if (lhs->publishRevision != rhs->publishRevision)
    return lhs->publishRevision > rhs->publishRevision;
  return lhs->frameSerial > rhs->frameSerial;
}

bool IsSemanticContractStrictlyNewerThanBuild(
    const std::shared_ptr<const ShadowFrameManifest>& manifest,
    uint64_t buildPublishRevision, uint64_t buildFrameSerial) {
  if (!manifest)
    return false;
  if (manifest->publishRevision != buildPublishRevision)
    return manifest->publishRevision > buildPublishRevision;
  return manifest->frameSerial > buildFrameSerial;
}

uint64_t CountAttachmentSupplementalCandidates(
    const std::shared_ptr<const ShadowAttachmentRigidStore>& attachments) {
  if (attachments == nullptr)
    return 0u;

  uint64_t count = 0u;
  for (const auto& attachment : attachments->records()) {
    if (attachment.childRuntimeModelPtr != nullptr &&
        (attachment.childModelResourcePtr != nullptr ||
         attachment.childModelKey != 0u)) {
      ++count;
    }
  }
  return count;
}

bool IsMissOnlyPreviewBuild(const ShadowValidationBuildWork* work) {
  if (work == nullptr)
    return false;
  constexpr size_t kMinMissOnlyRecordsBeforeSupersede = 64u;
  if (work->nextRecordIndex < kMinMissOnlyRecordsBeforeSupersede)
    return false;
  if (!work->frame.draws.empty())
    return false;
  const auto& resolve = work->stats.resolve;
  if (resolve.considered < kMinMissOnlyRecordsBeforeSupersede)
    return false;
  return resolve.resolved == 0u &&
         resolve.skippedResourceMiss == resolve.considered;
}

std::shared_ptr<ShadowValidationBuildWork> CreateShadowValidationBuildWork(
    std::shared_ptr<const ShadowFrameManifest> manifest,
    std::shared_ptr<const ShadowModelResourceStore> resources,
    std::shared_ptr<const ShadowPoseStore> poses,
    std::shared_ptr<const ShadowAttachmentRigidStore> attachments) {
  if (!manifest || !resources || !poses || !attachments)
    return nullptr;

  auto work = std::make_shared<ShadowValidationBuildWork>();
  work->manifest = std::move(manifest);
  work->resources = std::move(resources);
  work->poses = std::move(poses);
  work->attachments = std::move(attachments);
  work->frame.frameSerial = work->manifest->frameSerial;
  work->frame.sourcePublishRevision = work->manifest->publishRevision;
  work->frame.resourceStore = work->resources;
  work->stats.frameSerial = work->manifest->frameSerial;
  work->stats.sourcePublishRevision = work->manifest->publishRevision;
  work->stats.sourceVisibleCount = work->manifest->visibleCount;
  for (const auto& record : work->manifest->records) {
    if (record.hasStableIdentity())
      work->stats.sourceStableIdentityCount++;
    if (record.hasResolvedGeoset())
      work->stats.sourceResolvedGeosetCount++;
    if (record.objectKind == dxvk::war3::render::ObjectKind::Unit)
      work->stats.sourceUnitCount++;
  }
  return work;
}

uint32_t CountSkinnedSubmissionDraws(const ShadowSubmissionFrame* frame) {
  if (frame == nullptr)
    return 0u;

  uint32_t count = 0u;
  for (const auto& draw : frame->draws) {
    if (draw.path == ShadowDrawPath::Skinned)
      ++count;
  }
  return count;
}

bool ShouldPreferRenderableSubmissionFrame(
    const ShadowSubmissionFrame* candidate,
    const ShadowSubmissionFrame* current) {
  if (candidate == nullptr || candidate->frameSerial == 0u ||
      candidate->draws.empty())
    return false;
  if (current == nullptr || current->frameSerial == 0u ||
      current->draws.empty())
    return true;

  const uint32_t candidateSkinned = CountSkinnedSubmissionDraws(candidate);
  const uint32_t currentSkinned = CountSkinnedSubmissionDraws(current);
  constexpr uint64_t kDynamicFrameRevisionGrace = 16u;

  if (candidate->sourcePublishRevision < current->sourcePublishRevision) {
    const uint64_t reverseGap =
        current->sourcePublishRevision - candidate->sourcePublishRevision;
    if (reverseGap <= kDynamicFrameRevisionGrace) {
      if (candidateSkinned > currentSkinned)
        return true;
      if (candidateSkinned == currentSkinned &&
          candidate->draws.size() > current->draws.size())
        return true;
    }
    return false;
  }
  if (candidate->sourcePublishRevision == current->sourcePublishRevision) {
    if (candidateSkinned != currentSkinned)
      return candidateSkinned > currentSkinned;
    if (candidate->draws.size() != current->draws.size())
      return candidate->draws.size() > current->draws.size();
    return candidate->frameSerial > current->frameSerial;
  }

  const uint64_t revisionGap =
      candidate->sourcePublishRevision - current->sourcePublishRevision;

  // A chunked in-progress build can publish a newer partial rigid-only frame
  // while the previous completed frame still contains the canonical skinned
  // packets. For the validation/scene consumer, a near-current dynamic frame is
  // preferable to a slightly newer but weaker partial frame.
  if (revisionGap <= kDynamicFrameRevisionGrace) {
    if (currentSkinned != 0u && candidateSkinned == 0u)
      return false;
    if (candidateSkinned < currentSkinned)
      return false;
  }

  if (candidateSkinned > currentSkinned)
    return true;
  if (candidateSkinned == currentSkinned &&
      candidate->draws.size() >= current->draws.size())
    return true;

  return revisionGap > kDynamicFrameRevisionGrace;
}

uint32_t GetSemanticPreviewBuildRecordCap() {
  static const uint32_t s_cap = []() {
    const std::string value = dxvk::env::getEnvVar(
        "DXVK_WAR3_SEMANTIC_SHADOW_BUILD_RECORD_CAP");
    if (value.empty())
      return 0u;
    const int parsed = std::atoi(value.c_str());
    return parsed > 0 ? static_cast<uint32_t>(parsed) : 0u;
  }();
  return s_cap;
}

uint32_t InferPreviewOwnerGeosetIndex(
    const ShadowRenderableRecord& record,
    const model::ShadowModelResourceRecord& ownerResource) {
  if (record.geosetIndex != kInvalidShadowContractGeosetIndex)
    return record.geosetIndex;

  if (record.runtimeGeosetPtr != nullptr) {
    for (uint32_t i = 0u; i < ownerResource.geosetPtrs.size(); ++i) {
      if (ownerResource.geosetPtrs[i] == record.runtimeGeosetPtr)
        return i;
    }
  }
  if (record.runtimeGeosetDataPtr != nullptr) {
    for (uint32_t i = 0u; i < ownerResource.geosetDataPtrs.size(); ++i) {
      if (ownerResource.geosetDataPtrs[i] == record.runtimeGeosetDataPtr)
        return i;
    }
  }

  return kInvalidShadowContractGeosetIndex;
}

template <typename Fn>
void ForEachPreviewRuntimeAlias(void* runtimeModelPtr, Fn&& fn) {
  if (runtimeModelPtr == nullptr)
    return;

  fn(runtimeModelPtr);

  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const uintptr_t base = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  const uintptr_t plusAlias = base + kCModelComplexExtensionOffset;
  if (plusAlias >= 0x10000u && plusAlias != base)
    fn(reinterpret_cast<void*>(plusAlias));
  if (base > kCModelComplexExtensionOffset) {
    const uintptr_t minusAlias = base - kCModelComplexExtensionOffset;
    if (minusAlias >= 0x10000u && minusAlias != base)
      fn(reinterpret_cast<void*>(minusAlias));
  }
}

const ShadowModelResourceRecord* TryFindPreviewCapResource(
    const ShadowRenderableRecord& record,
    const ShadowModelResourceStore& resources,
    model::ShadowModelResourceRecord* outOwnerResource = nullptr) {
  if (outOwnerResource != nullptr)
    *outOwnerResource = {};
  if (record.runtimeGeosetPtr != nullptr) {
    if (const auto* resource =
            resources.findByRuntimeGeoset(record.runtimeGeosetPtr)) {
      return resource;
    }
  }
  if (record.runtimeGeosetDataPtr != nullptr) {
    if (const auto* resource =
            resources.findByRuntimeGeosetData(record.runtimeGeosetDataPtr)) {
      return resource;
    }
  }
  if (record.runtimeModelPtr != nullptr &&
      record.geosetIndex != kInvalidShadowContractGeosetIndex) {
    if (const auto* resource =
            resources.findByRuntimeModel(record.runtimeModelPtr,
                                         record.geosetIndex)) {
      return resource;
    }
  }
  if (record.modelResourcePtr != nullptr &&
      record.geosetIndex != kInvalidShadowContractGeosetIndex) {
    if (const auto* resource =
            resources.findByModelResource(record.modelResourcePtr,
                                          record.geosetIndex)) {
      return resource;
    }
  }
  if ((record.runtimeGeosetPtr != nullptr ||
       record.runtimeGeosetDataPtr != nullptr)) {
    model::ShadowModelResourceRecord ownerResource = {};
    if (model::ShadowModelResourceCache::instance().findRuntimeModelOwner(
            record.runtimeGeosetPtr, record.runtimeGeosetDataPtr,
            record.geosetIndex, record.modelResourcePtr, ownerResource)) {
      if (outOwnerResource != nullptr)
        *outOwnerResource = ownerResource;
      const uint32_t ownerGeosetIndex =
          InferPreviewOwnerGeosetIndex(record, ownerResource);
      if (ownerGeosetIndex == kInvalidShadowContractGeosetIndex)
        return nullptr;
      if (ownerGeosetIndex < ownerResource.geosetPtrs.size() &&
          ownerResource.geosetPtrs[ownerGeosetIndex] != nullptr) {
        if (const auto* resource =
                resources.findByRuntimeGeoset(
                    ownerResource.geosetPtrs[ownerGeosetIndex])) {
          return resource;
        }
      }
      if (ownerGeosetIndex < ownerResource.geosetDataPtrs.size() &&
          ownerResource.geosetDataPtrs[ownerGeosetIndex] != nullptr) {
        if (const auto* resource = resources.findByRuntimeGeosetData(
                ownerResource.geosetDataPtrs[ownerGeosetIndex])) {
          return resource;
        }
      }
      if (ownerResource.runtimeModelPtr != nullptr) {
        if (const auto* resource = resources.findByRuntimeModel(
                ownerResource.runtimeModelPtr, ownerGeosetIndex)) {
          return resource;
        }
      }
      if (ownerResource.modelResourcePtr != nullptr) {
        if (const auto* resource = resources.findByModelResource(
                ownerResource.modelResourcePtr, ownerGeosetIndex)) {
          return resource;
        }
      }
    }
  }
  return nullptr;
}

bool ApplyPreviewCapResourceHints(ShadowRenderableRecord& record,
                                  const ShadowModelResourceStore& resources) {
  model::ShadowModelResourceRecord ownerResource = {};
  ShadowModelResourceRecord cacheResource = {};
  const ShadowModelResourceRecord* resource =
      TryFindPreviewCapResource(record, resources, &ownerResource);
  if (resource == nullptr &&
      TryFindRenderableResourceFromCache(record, record.frameSerial,
                                         cacheResource, &ownerResource)) {
    resource = &cacheResource;
  }
  bool changed = false;

  if (ownerResource.runtimeModelPtr != nullptr &&
      record.runtimeModelPtr == nullptr) {
    record.runtimeModelPtr = ownerResource.runtimeModelPtr;
    changed = true;
  }
  if (ownerResource.modelResourcePtr != nullptr &&
      record.modelResourcePtr == nullptr) {
    record.modelResourcePtr = ownerResource.modelResourcePtr;
    changed = true;
  }
  if (ownerResource.modelKey != 0u && record.modelKey == 0u) {
    record.modelKey = ownerResource.modelKey;
    changed = true;
  }

  if (record.geosetIndex != kInvalidShadowContractGeosetIndex) {
    if (record.runtimeGeosetPtr == nullptr &&
        record.geosetIndex < ownerResource.geosetPtrs.size() &&
        ownerResource.geosetPtrs[record.geosetIndex] != nullptr) {
      record.runtimeGeosetPtr = ownerResource.geosetPtrs[record.geosetIndex];
      changed = true;
    }
    if (record.runtimeGeosetDataPtr == nullptr &&
        record.geosetIndex < ownerResource.geosetDataPtrs.size() &&
        ownerResource.geosetDataPtrs[record.geosetIndex] != nullptr) {
      record.runtimeGeosetDataPtr =
          ownerResource.geosetDataPtrs[record.geosetIndex];
      changed = true;
    }
  }

  if (resource == nullptr)
    return changed;

  if (record.runtimeGeosetPtr == nullptr &&
      resource->runtimeGeosetPtr != nullptr) {
    record.runtimeGeosetPtr = resource->runtimeGeosetPtr;
    changed = true;
  }
  if (record.runtimeGeosetDataPtr == nullptr &&
      resource->runtimeGeosetDataPtr != nullptr) {
    record.runtimeGeosetDataPtr = resource->runtimeGeosetDataPtr;
    changed = true;
  }
  if (record.modelResourcePtr == nullptr &&
      resource->modelResourcePtr != nullptr) {
    record.modelResourcePtr = resource->modelResourcePtr;
    changed = true;
  }
  if (record.modelKey == 0u && resource->modelKey != 0u) {
    record.modelKey = resource->modelKey;
    changed = true;
  }
  if (record.geosetIndex == kInvalidShadowContractGeosetIndex &&
      resource->geosetIndex != kInvalidShadowContractGeosetIndex) {
    record.geosetIndex = resource->geosetIndex;
    changed = true;
  }

  return changed;
}

void AppendPoseResourcePreviewSeeds(ShadowFrameManifest& manifest,
                                    const ShadowModelResourceStore& resources,
                                    const ShadowPoseStore& poses) {
  constexpr uint32_t kMaxPosePreviewGeosets = 4u;
  constexpr size_t kMaxPosePreviewRecords = 64u;

  std::unordered_set<uint64_t> dedup;
  dedup.reserve(kMaxPosePreviewRecords * 2u);

  auto makeKey = [](void* runtimeModelPtr,
                    const ShadowModelResourceRecord& resource) {
    const uint64_t h1 =
        uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr));
    const uint64_t h2 = uint64_t(reinterpret_cast<uintptr_t>(
        resource.runtimeGeosetPtr != nullptr
            ? resource.runtimeGeosetPtr
            : resource.runtimeGeosetDataPtr != nullptr
                  ? resource.runtimeGeosetDataPtr
                  : resource.modelResourcePtr));
    return h1 ^ (h2 + 0x9E3779B97F4A7C15ull +
                 (uint64_t(resource.geosetIndex) << 32u));
  };

  for (const auto& pose : poses.records()) {
    if (manifest.records.size() >= kMaxPosePreviewRecords)
      break;
    if (pose.runtimeModelPtr == nullptr)
      continue;
    if (pose.matrixCount == 0u || pose.matrixPalette.empty())
      continue;

    ForEachPreviewRuntimeAlias(pose.runtimeModelPtr, [&](void* alias) {
      if (manifest.records.size() >= kMaxPosePreviewRecords)
        return;
      model::ShadowModelResourceRecord runtimeResource = {};
      const bool hasRuntimeResource =
          model::ShadowModelResourceCache::instance().findRuntimeModelResource(
              alias, runtimeResource) &&
          runtimeResource.readyForShadowConsumer();
      void* directModelResourcePtr =
          hasRuntimeResource ? runtimeResource.modelResourcePtr : nullptr;

      for (uint32_t geosetIndex = 0u;
           geosetIndex < kMaxPosePreviewGeosets &&
           manifest.records.size() < kMaxPosePreviewRecords;
           ++geosetIndex) {
        const ShadowModelResourceRecord* resource =
            resources.findByRuntimeModel(alias, geosetIndex);
        if (resource == nullptr && pose.runtimeModelPtr != alias)
          resource = resources.findByRuntimeModel(pose.runtimeModelPtr,
                                                  geosetIndex);
        if (resource == nullptr && directModelResourcePtr != nullptr)
          resource = resources.findByModelResource(directModelResourcePtr,
                                                   geosetIndex);
        if (resource == nullptr || !resource->readyForConsumer() ||
            !resource->hasSkinningData()) {
          continue;
        }

        const uint64_t key = makeKey(pose.runtimeModelPtr, *resource);
        if (!dedup.insert(key).second)
          continue;

        ShadowRenderableRecord record = {};
        record.sceneNode = pose.sceneNode;
        record.unitPtr = pose.unitPtr;
        record.runtimeModelPtr = pose.runtimeModelPtr;
        record.modelResourcePtr = resource->modelResourcePtr;
        record.runtimeGeosetPtr = resource->runtimeGeosetPtr;
        record.runtimeGeosetDataPtr = resource->runtimeGeosetDataPtr;
        record.modelKey = resource->modelKey;
        record.meshIndex = resource->geosetIndex;
        record.geosetIndex = resource->geosetIndex;
        record.objectKind =
            pose.unitPtr != nullptr ? render::ObjectKind::Unit
                                    : render::ObjectKind::Unknown;
        record.queueKind = render::VisibleRenderableQueueKind::MainQueue;
        record.frameSerial = manifest.frameSerial;
        manifest.records.push_back(std::move(record));
      }
    });
  }
}

struct PreviewCapPoseAvailability {
  std::vector<void*> paletteRuntimeModels;
  std::vector<void*> paletteSceneNodes;
  std::vector<void*> paletteUnits;
  std::vector<void*> worldRuntimeModels;
  std::vector<void*> worldSceneNodes;
  std::vector<void*> worldUnits;

  explicit PreviewCapPoseAvailability(const ShadowPoseStore& poses) {
    paletteRuntimeModels.reserve(poses.records().size());
    paletteSceneNodes.reserve(poses.records().size());
    paletteUnits.reserve(poses.records().size());
    worldRuntimeModels.reserve(poses.records().size());
    worldSceneNodes.reserve(poses.records().size());
    worldUnits.reserve(poses.records().size());

    auto pushUnique = [](std::vector<void*>& values, void* value) {
      if (value == nullptr)
        return;
      if (std::find(values.begin(), values.end(), value) == values.end())
        values.push_back(value);
    };

    for (const auto& pose : poses.records()) {
      const bool hasPalette =
          pose.matrixCount != 0u && !pose.matrixPalette.empty();
      if (hasPalette) {
        pushUnique(paletteRuntimeModels, pose.runtimeModelPtr);
        pushUnique(paletteSceneNodes, pose.sceneNode);
        pushUnique(paletteUnits, pose.unitPtr);
      }
      if (pose.hasWorldTransform) {
        pushUnique(worldRuntimeModels, pose.runtimeModelPtr);
        pushUnique(worldSceneNodes, pose.sceneNode);
        pushUnique(worldUnits, pose.unitPtr);
      }
    }
  }

  bool hasPalette(const ShadowRenderableRecord& record) const {
    auto contains = [](const std::vector<void*>& values, void* value) {
      return value != nullptr &&
             std::find(values.begin(), values.end(), value) != values.end();
    };
    return (record.runtimeModelPtr != nullptr &&
            contains(paletteRuntimeModels, record.runtimeModelPtr)) ||
           (record.sceneNode != nullptr &&
            contains(paletteSceneNodes, record.sceneNode)) ||
           (record.unitPtr != nullptr && contains(paletteUnits, record.unitPtr));
  }

  bool hasWorldTransform(const ShadowRenderableRecord& record) const {
    auto contains = [](const std::vector<void*>& values, void* value) {
      return value != nullptr &&
             std::find(values.begin(), values.end(), value) != values.end();
    };
    return (record.runtimeModelPtr != nullptr &&
            contains(worldRuntimeModels, record.runtimeModelPtr)) ||
           (record.sceneNode != nullptr &&
            contains(worldSceneNodes, record.sceneNode)) ||
           (record.unitPtr != nullptr && contains(worldUnits, record.unitPtr));
  }
};

uint32_t ScorePreviewCapRecord(const ShadowRenderableRecord& record,
                               const ShadowModelResourceStore& resources,
                               const PreviewCapPoseAvailability& poses) {
  ShadowModelResourceRecord cacheResource = {};
  const ShadowModelResourceRecord* resource =
      TryFindPreviewCapResource(record, resources);
  if (resource == nullptr &&
      TryFindRenderableResourceFromCache(record, record.frameSerial,
                                         cacheResource)) {
    resource = &cacheResource;
  }
  const bool resourceReady = resource != nullptr && resource->readyForConsumer();
  const bool skinnedReady = resourceReady && resource->hasSkinningData();

  const bool hasPalette = poses.hasPalette(record);
  const bool hasWorldTransform = poses.hasWorldTransform(record);
  const bool rootUnit =
      record.objectKind == render::ObjectKind::Unit &&
      record.runtimeModelPtr != nullptr && record.modelResourcePtr != nullptr &&
      record.hasResolvedGeoset();

  // Preview builds are deliberately budgeted. Spend the first records on the
  // packets most likely to become visible dynamic unit shadows rather than on
  // anonymous or static fragments that can be picked up by a later pass.
  if (rootUnit && skinnedReady && hasPalette)
    return 0u;
  if (record.objectKind == render::ObjectKind::Unit && skinnedReady &&
      hasPalette) {
    return 1u;
  }
  if (skinnedReady && hasPalette)
    return 2u;
  if (rootUnit && resourceReady && hasWorldTransform)
    return 3u;
  if (record.objectKind == render::ObjectKind::Unit && resourceReady &&
      hasWorldTransform) {
    return 4u;
  }
  if (record.objectKind == render::ObjectKind::Unit && resourceReady)
    return 5u;
  if (IsStaticMeshDataPreviewCandidate(record))
    return 6u;
  if ((record.objectKind == render::ObjectKind::Building ||
       record.objectKind == render::ObjectKind::Destructible) &&
      resourceReady && hasWorldTransform) {
    return 8u;
  }
  if (resourceReady && hasWorldTransform)
    return 9u;
  if (resourceReady)
    return 10u;
  if (record.hasResolvedGeoset())
    return 11u;
  return 12u;
}

std::shared_ptr<const ShadowFrameManifest> MaybeCapPreviewManifest(
    std::shared_ptr<const ShadowFrameManifest> manifest,
    const ShadowModelResourceStore& resources,
    const ShadowPoseStore& poses) {
  if (!manifest ||
      !dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled())
    return manifest;

  const uint32_t cap = GetSemanticPreviewBuildRecordCap();
  if (manifest->records.size() <= 1u)
    return manifest;

  auto capped = std::make_shared<ShadowFrameManifest>();
  capped->frameSerial = manifest->frameSerial;
  capped->publishRevision = manifest->publishRevision;
  capped->visibleCount = manifest->visibleCount;
  capped->mainQueueCount = manifest->mainQueueCount;
  capped->transparentCount = manifest->transparentCount;

  struct ScoredRecord {
    uint32_t score = 0u;
    size_t index = 0u;
  };
  std::vector<ScoredRecord> order;
  order.reserve(manifest->records.size());
  std::vector<ShadowRenderableRecord> preparedRecords;
  preparedRecords.reserve(manifest->records.size());
  const PreviewCapPoseAvailability poseAvailability(poses);
  for (size_t i = 0u; i < manifest->records.size(); ++i) {
    ShadowRenderableRecord prepared = manifest->records[i];
    ApplyPreviewCapResourceHints(prepared, resources);
    preparedRecords.push_back(std::move(prepared));
    order.push_back({ScorePreviewCapRecord(preparedRecords.back(), resources,
                                           poseAvailability),
                     i});
  }

  std::stable_sort(order.begin(), order.end(),
                   [](const ScoredRecord& lhs, const ScoredRecord& rhs) {
                     return lhs.score < rhs.score;
                   });

  const bool hasReadyRecords =
      std::any_of(order.begin(), order.end(),
                  [](const ScoredRecord& entry) { return entry.score < 11u; });
  if (!hasReadyRecords &&
      dxvk::war3::internal::
          kShadowSemanticCoreScenePoseResourcePreviewSeedsEnabled) {
    AppendPoseResourcePreviewSeeds(*capped, resources, poses);
    return capped;
  }

  const size_t limit =
      cap == 0u ? manifest->records.size()
                : std::min<size_t>(manifest->records.size(), cap);
  constexpr size_t kMaxPreviewStaticMeshDataCandidates = 12u;
  size_t staticMeshDataCandidates = 0u;
  capped->records.reserve(limit);
  for (const auto& entry : order) {
    if (capped->records.size() >= limit)
      break;
    if (hasReadyRecords && entry.score >= 11u)
      continue;
    const bool staticMeshDataCandidate =
        IsStaticMeshDataPreviewCandidate(preparedRecords[entry.index]);
    if (staticMeshDataCandidate &&
        staticMeshDataCandidates >= kMaxPreviewStaticMeshDataCandidates) {
      continue;
    }
    capped->records.push_back(std::move(preparedRecords[entry.index]));
    if (staticMeshDataCandidate)
      ++staticMeshDataCandidates;
  }

  if (capped->records.size() < 32u &&
      dxvk::war3::internal::
          kShadowSemanticCoreScenePoseResourcePreviewSeedsEnabled) {
    AppendPoseResourcePreviewSeeds(*capped, resources, poses);
  }

  if (capped->records.empty())
    return manifest;

  return capped;
}

} // namespace

void ShadowValidationRuntime::clearPendingBuildLocked() {
  if (m_pendingManifest || m_pendingResources || m_pendingPoses ||
      m_pendingAttachments)
    ++m_stalePendingBuildClearedCount;
  m_pendingManifest.reset();
  m_pendingResources.reset();
  m_pendingPoses.reset();
  m_pendingAttachments.reset();
}

void ShadowValidationRuntime::requestLatestFrameBuild() {
  const auto bundle =
      ShadowRuntimeContractCache::instance().snapshotBundleShared();
  if (!bundle.valid())
    return;

  requestFrameBuildForContract(bundle.manifest, bundle.resources, bundle.poses,
                               bundle.attachments);
}

void ShadowValidationRuntime::requestFrameBuildForContract(
    std::shared_ptr<const ShadowFrameManifest> manifest,
    std::shared_ptr<const ShadowModelResourceStore> resources,
    std::shared_ptr<const ShadowPoseStore> poses,
    std::shared_ptr<const ShadowAttachmentRigidStore> attachments) {
  if (!manifest || !resources || !poses || !attachments)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  const uint64_t supplementalCandidateCount =
      CountAttachmentSupplementalCandidates(attachments);
  const bool attachmentSupplementalNeedsRebuild =
      supplementalCandidateCount >
      m_lastStats.resolve.attachmentRigidSupplementalAttachmentCount;
  const bool alreadyBuilt =
      m_lastStats.sourcePublishRevision != 0u &&
      m_lastStats.sourcePublishRevision == manifest->publishRevision &&
      m_lastFrame != nullptr &&
      m_lastFrame->frameSerial == manifest->frameSerial;
  if (alreadyBuilt && !attachmentSupplementalNeedsRebuild) {
    if (!IsSemanticContractNewer(m_pendingManifest, manifest))
      clearPendingBuildLocked();
    return;
  }

  if ((attachmentSupplementalNeedsRebuild ||
       IsSemanticContractStrictlyNewerThanBuild(
           manifest, m_buildPublishRevision, m_buildFrameSerial)) &&
      (attachmentSupplementalNeedsRebuild ||
       IsSemanticContractNewer(manifest, m_pendingManifest))) {
    m_pendingManifest = std::move(manifest);
    m_pendingResources = std::move(resources);
    m_pendingPoses = std::move(poses);
    m_pendingAttachments = std::move(attachments);
  }
}

void ShadowValidationRuntime::ensureLatestFrameBuilt() {
  requestLatestFrameBuild();

  std::shared_ptr<const ShadowFrameManifest> manifest;
  std::shared_ptr<const ShadowModelResourceStore> resources;
  std::shared_ptr<const ShadowPoseStore> poses;
  std::shared_ptr<const ShadowAttachmentRigidStore> attachments;
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (m_buildInProgress && m_buildWork != nullptr) {
      const bool hasNewerPending =
          m_pendingManifest && m_pendingResources && m_pendingPoses &&
          m_pendingAttachments &&
          IsSemanticContractNewer(m_pendingManifest, m_buildWork->manifest);
      if (hasNewerPending && IsMissOnlyPreviewBuild(m_buildWork.get())) {
        ++m_stalePendingBuildClearedCount;
        m_buildWork.reset();
        m_buildInProgress = false;
        m_buildFrameSerial = 0;
        m_buildPublishRevision = 0;
        manifest = std::move(m_pendingManifest);
        resources = std::move(m_pendingResources);
        poses = std::move(m_pendingPoses);
        attachments = std::move(m_pendingAttachments);
      } else {
        // 场景提交模式下让当前 build 单飞完成，只保留最新 pending contract。
        // 如果在 build 进行中不断用更晚的 manifest 抢断当前 work，
        // 高 publish-revision 压力下会长期停在 buildInProgress=true、
        // 但 semanticCoreFrameSerial 永远追不上 manifest 的“饥饿态”。
        // 这里只允许“已证明是空 miss-only 的预览帧”被更新帧抢占。
        manifest = m_buildWork->manifest;
        resources = m_buildWork->resources;
        poses = m_buildWork->poses;
        attachments = m_buildWork->attachments;
      }
    } else
    if (!m_buildInProgress && m_pendingManifest && m_pendingResources &&
        m_pendingPoses && m_pendingAttachments) {
      manifest = std::move(m_pendingManifest);
      resources = std::move(m_pendingResources);
      poses = std::move(m_pendingPoses);
      attachments = std::move(m_pendingAttachments);
    }
  }

  if (!manifest || !resources || !poses || !attachments) {
    const auto bundle =
        ShadowRuntimeContractCache::instance().snapshotBundleShared();
    if (!bundle.valid())
      return;
    manifest = bundle.manifest;
    resources = bundle.resources;
    poses = bundle.poses;
    attachments = bundle.attachments;
  }

  ensureFrameBuiltForContract(std::move(manifest), std::move(resources),
                              std::move(poses), std::move(attachments));
}

void ShadowValidationRuntime::drainPendingBuildForControlPlane(
    uint32_t maxChunks,
    uint64_t maxTotalBudgetUs,
    uint64_t recordCeiling) {
  if (maxChunks == 0u || maxTotalBudgetUs == 0u)
    return;

  const auto drainStart = std::chrono::steady_clock::now();
  for (uint32_t i = 0u; i < maxChunks; ++i) {
    const auto before = buildStateSnapshot();
    if (!before.buildInProgress && !before.buildRequestPending)
      return;

    if (before.buildInProgress && recordCeiling != 0u &&
        before.buildRecordCount > recordCeiling) {
      return;
    }

    ensureLatestFrameBuilt();

    const auto after = buildStateSnapshot();
    if (!after.buildInProgress)
      return;

    if (after.buildCurrentRecordIndex == before.buildCurrentRecordIndex &&
        after.buildChunkCount == before.buildChunkCount &&
        before.buildInProgress) {
      return;
    }

    const uint64_t elapsedUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - drainStart)
            .count());
    if (elapsedUs >= maxTotalBudgetUs)
      return;
  }
}

void ShadowValidationRuntime::ensureFrameBuiltForContract(
    std::shared_ptr<const ShadowFrameManifest> manifest,
    std::shared_ptr<const ShadowModelResourceStore> resources,
    std::shared_ptr<const ShadowPoseStore> poses,
    std::shared_ptr<const ShadowAttachmentRigidStore> attachments) {
  if (!manifest || !resources || !poses || !attachments)
    return;
  manifest = MaybeCapPreviewManifest(std::move(manifest), *resources, *poses);
  std::shared_ptr<ShadowValidationBuildWork> buildWork;
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    const uint64_t supplementalCandidateCount =
        CountAttachmentSupplementalCandidates(attachments);
    const bool attachmentSupplementalNeedsRebuild =
        supplementalCandidateCount >
        m_lastStats.resolve.attachmentRigidSupplementalAttachmentCount;
    if (m_lastFrame != nullptr &&
        m_lastStats.sourcePublishRevision != 0u &&
        m_lastStats.sourcePublishRevision == manifest->publishRevision &&
        m_lastFrame->frameSerial == manifest->frameSerial &&
        !attachmentSupplementalNeedsRebuild) {
      if (!IsSemanticContractNewer(m_pendingManifest, manifest))
        clearPendingBuildLocked();
      return;
    }

    if (m_buildInProgress) {
      buildWork = m_buildWork;
      if ((attachmentSupplementalNeedsRebuild ||
           IsSemanticContractStrictlyNewerThanBuild(
               manifest, m_buildPublishRevision, m_buildFrameSerial)) &&
          (attachmentSupplementalNeedsRebuild ||
           IsSemanticContractNewer(manifest, m_pendingManifest))) {
        m_pendingManifest = std::move(manifest);
        m_pendingResources = std::move(resources);
        m_pendingPoses = std::move(poses);
        m_pendingAttachments = std::move(attachments);
      }
    } else {
      m_buildInProgress = true;
      m_buildFrameSerial = manifest->frameSerial;
      m_buildPublishRevision = manifest->publishRevision;
      m_buildWork = CreateShadowValidationBuildWork(std::move(manifest),
                                                    std::move(resources),
                                                    std::move(poses),
                                                    std::move(attachments));
      buildWork = m_buildWork;
    }
  }

  if (!buildWork || !buildWork->manifest || !buildWork->resources ||
      !buildWork->poses || !buildWork->attachments) {
    return;
  }

  constexpr uint32_t kSemanticBuildChunkMaxRecords = 8u;
  // Keep semantic preview builds genuinely incremental on the render thread.
  // A completed frame is less useful if the build itself stalls the game long
  // enough to block screenshots, control-plane commands, or presents.
  constexpr uint64_t kSemanticBuildChunkBudgetUs = 2000u;
  const auto chunkStart = std::chrono::steady_clock::now();
  buildWork->nextRecordIndex = m_core.buildFrameChunk(
      *buildWork->manifest, *buildWork->resources, *buildWork->poses,
      *buildWork->attachments,
      buildWork->nextRecordIndex, kSemanticBuildChunkMaxRecords,
      kSemanticBuildChunkBudgetUs, buildWork->frame, buildWork->stats.resolve);
  buildWork->chunkCount++;
  buildWork->totalBuildDurationUs += static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - chunkStart)
          .count());

  if (buildWork->nextRecordIndex < buildWork->manifest->records.size()) {
    if (buildWork->frame.frameSerial != 0u && !buildWork->frame.draws.empty()) {
      auto partialFrame =
          std::make_shared<ShadowSubmissionFrame>(buildWork->frame);
      std::unique_lock<std::shared_mutex> lock(m_mutex);
      if (ShouldPreferRenderableSubmissionFrame(partialFrame.get(),
                                                m_lastRenderableFrame.get())) {
        m_lastRenderableFrame = std::move(partialFrame);
      }
    }
    if (SemanticCoreTraceEnabled()) {
      static std::atomic<uint32_t> s_buildProgressLogCount{0u};
      const uint32_t progressLogCount =
          s_buildProgressLogCount.fetch_add(1u, std::memory_order_relaxed);
      if (progressLogCount < 64u || (progressLogCount % 256u) == 0u) {
      war3dbg::Print(
          "DXVK SemanticCore: build-progress manifest=%llu rev=%llu idx=%llu/%llu "
          "resolved=%llu skipNoId=%llu skipNoGeo=%llu skipNoPose=%llu chunks=%llu\n",
          static_cast<unsigned long long>(buildWork->manifest->frameSerial),
          static_cast<unsigned long long>(buildWork->manifest->publishRevision),
          static_cast<unsigned long long>(buildWork->nextRecordIndex),
          static_cast<unsigned long long>(buildWork->manifest->records.size()),
          static_cast<unsigned long long>(buildWork->stats.resolve.resolved),
          static_cast<unsigned long long>(
              buildWork->stats.resolve.skippedNoIdentity),
          static_cast<unsigned long long>(
              buildWork->stats.resolve.skippedNoGeoset),
          static_cast<unsigned long long>(buildWork->stats.resolve.skippedNoPose),
          static_cast<unsigned long long>(buildWork->chunkCount));
      }
    }
    return;
  }

  buildWork->stats.coreDrawPacketCount = buildWork->frame.draws.size();

  if constexpr (dxvk::war3::internal::kUpperLayerShadowConsumerEnabled) {
    std::unordered_set<uint64_t> dedupKeys;
    dedupKeys.reserve(buildWork->frame.draws.size() * 2u + 1u);
    for (const auto& draw : buildWork->frame.draws)
      dedupKeys.insert(MakeDrawDedupKey(draw));

    const auto upperResolved =
        render::UpperLayerShadowRegistry::instance().snapshotResolvedItems();
    buildWork->stats.upperLayerResolvedItems = upperResolved.size();
    for (const auto& item : upperResolved) {
      ShadowDrawPacket packet = {};
      if (!TryConvertUpperLayerResolvedItem(item, buildWork->manifest->frameSerial,
                                            packet)) {
        continue;
      }

      if (!dedupKeys.insert(MakeDrawDedupKey(packet)).second)
        continue;

      buildWork->frame.draws.emplace_back(std::move(packet));
      buildWork->stats.supplementalUpperLayerDrawPacketCount++;
    }
  }

  buildWork->stats.drawPacketCount = buildWork->frame.draws.size();

  if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled) {
    buildWork->stats.submittedDrawCount = buildWork->frame.draws.size();
  } else {
    DxvkValidationBackend backend = {};
    m_core.submitFrame(buildWork->frame, backend);
    buildWork->stats.submittedDrawCount = backend.submittedDrawCount();
  }
  buildWork->stats.buildDurationUs = buildWork->totalBuildDurationUs;

  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_lastBuildDurationUs = buildWork->stats.buildDurationUs;
    m_lastStats = buildWork->stats;
    auto completedFrame =
        std::make_shared<ShadowSubmissionFrame>(std::move(buildWork->frame));
    m_lastFrame = completedFrame;
    if (completedFrame->frameSerial != 0u && !completedFrame->draws.empty()) {
      if (ShouldPreferRenderableSubmissionFrame(completedFrame.get(),
                                                m_lastRenderableFrame.get()))
        m_lastRenderableFrame = completedFrame;
    }
    m_buildWork.reset();
    m_buildInProgress = false;
    m_buildFrameSerial = 0;
    m_buildPublishRevision = 0;
    if (!IsSemanticContractNewer(m_pendingManifest, buildWork->manifest))
      clearPendingBuildLocked();
  }

  if (SemanticCoreTraceEnabled()) {
    static std::atomic<uint32_t> s_buildLogCount{0u};
    const uint32_t buildLogCount =
        s_buildLogCount.fetch_add(1u, std::memory_order_relaxed);
    if (buildLogCount < 128u || buildWork->stats.frameSerial >= 880u ||
        (buildLogCount % 120u) == 0u) {
    war3dbg::Print(
        "DXVK SemanticCore: build manifest=%llu rev=%llu resolved=%llu "
        "skinned=%llu skipNoId=%llu skipNoGeo=%llu skipNoPose=%llu "
        "skipNoGrp=%llu explicit=%llu/%llu draws=%llu chunks=%llu\n",
        static_cast<unsigned long long>(buildWork->stats.frameSerial),
        static_cast<unsigned long long>(buildWork->stats.sourcePublishRevision),
        static_cast<unsigned long long>(buildWork->stats.resolve.resolved),
        static_cast<unsigned long long>(buildWork->stats.resolve.skinnedResolved),
        static_cast<unsigned long long>(
            buildWork->stats.resolve.skippedNoIdentity),
        static_cast<unsigned long long>(buildWork->stats.resolve.skippedNoGeoset),
        static_cast<unsigned long long>(buildWork->stats.resolve.skippedNoPose),
        static_cast<unsigned long long>(
            buildWork->stats.resolve.skippedNoRuntimeGroupPalette),
        static_cast<unsigned long long>(
            buildWork->stats.resolve.explicitBlendAttempts),
        static_cast<unsigned long long>(
            buildWork->stats.resolve.explicitBlendResolved),
        static_cast<unsigned long long>(buildWork->stats.drawPacketCount),
        static_cast<unsigned long long>(buildWork->chunkCount));
    }
  }
}

void ShadowValidationRuntime::runObserveValidation() {
  ensureLatestFrameBuilt();
}

void ShadowValidationRuntime::reset() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_buildInProgress = false;
  m_buildFrameSerial = 0;
  m_buildPublishRevision = 0;
  m_pendingManifest.reset();
  m_pendingResources.reset();
  m_pendingPoses.reset();
  m_pendingAttachments.reset();
  m_buildWork.reset();
  m_stalePendingBuildClearedCount = 0;
  m_lastStats = {};
  m_lastFrame = std::make_shared<ShadowSubmissionFrame>();
  m_lastRenderableFrame = std::make_shared<ShadowSubmissionFrame>();
}

ShadowValidationFrameStats ShadowValidationRuntime::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  if (m_buildInProgress && m_buildWork != nullptr) {
    ShadowValidationFrameStats stats = m_buildWork->stats;
    stats.coreDrawPacketCount = m_buildWork->frame.draws.size();
    stats.drawPacketCount = m_buildWork->frame.draws.size();
    stats.buildDurationUs = m_buildWork->totalBuildDurationUs;
    return stats;
  }
  return m_lastStats;
}

ShadowValidationBuildState ShadowValidationRuntime::buildStateSnapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  ShadowValidationBuildState state = {};
  state.buildInProgress = m_buildInProgress;
  state.buildRequestPending =
      m_pendingManifest != nullptr && m_pendingResources != nullptr &&
      m_pendingPoses != nullptr && m_pendingAttachments != nullptr;
  state.buildFrameSerial = m_buildFrameSerial;
  state.buildPublishRevision = m_buildPublishRevision;
  state.pendingFrameSerial =
      m_pendingManifest != nullptr ? m_pendingManifest->frameSerial : 0u;
  state.pendingPublishRevision =
      m_pendingManifest != nullptr ? m_pendingManifest->publishRevision : 0u;
  state.lastBuildDurationUs = m_lastBuildDurationUs;
  if (m_buildWork != nullptr && m_buildWork->manifest != nullptr) {
    state.buildCurrentRecordIndex = m_buildWork->nextRecordIndex;
    state.buildRecordCount = m_buildWork->manifest->records.size();
    state.buildChunkCount = m_buildWork->chunkCount;
    state.lastBuildDurationUs = m_buildWork->totalBuildDurationUs;
  }
  state.stalePendingBuildClearedCount = m_stalePendingBuildClearedCount;
  return state;
}

ShadowSubmissionFrame ShadowValidationRuntime::snapshotFrame() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_lastFrame != nullptr ? *m_lastFrame : ShadowSubmissionFrame{};
}

std::shared_ptr<const ShadowSubmissionFrame>
ShadowValidationRuntime::snapshotFrameShared() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_lastFrame;
}

std::shared_ptr<const ShadowSubmissionFrame>
ShadowValidationRuntime::snapshotRenderableFrameShared() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  if (m_lastRenderableFrame != nullptr &&
      m_lastRenderableFrame->frameSerial != 0u &&
      !m_lastRenderableFrame->draws.empty() &&
      ShouldPreferRenderableSubmissionFrame(m_lastRenderableFrame.get(),
                                            m_lastFrame.get())) {
    return m_lastRenderableFrame;
  }
  if (m_lastFrame != nullptr && m_lastFrame->frameSerial != 0u &&
      !m_lastFrame->draws.empty()) {
    return m_lastFrame;
  }
  return m_lastRenderableFrame;
}

} // namespace dxvk::war3::shadow
