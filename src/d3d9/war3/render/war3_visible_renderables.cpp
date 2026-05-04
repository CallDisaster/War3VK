#include "war3_visible_renderables.h"

#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_scene.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../game/war3_unit.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "war3_render_objects.h"
#include "war3_render_queue_tracker.h"
#include "war3_shadow_object_registry.h"
#include "war3_shadow_runtime_bridge.h"

#include <atomic>

namespace dxvk::war3::render {

namespace {

constexpr size_t kRenderBatchElementStride = 20;

struct RenderablePartBasicFields {
  void *sceneNode = nullptr;
  void *meshData = nullptr;
};

bool TryReadRenderablePartPtr(void *renderablePart, size_t offset, void *&outPtr) {
  outPtr = nullptr;
  if (!renderablePart)
    return false;

  const uintptr_t base = reinterpret_cast<uintptr_t>(renderablePart);
  if (base < 0x10000u || base + offset < base)
    return false;

  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigTrustVisibleRenderablePartPointers) {
    outPtr = *reinterpret_cast<void **>(
        reinterpret_cast<std::uint8_t *>(renderablePart) + offset);
    return outPtr != nullptr;
  }

  return dxvk::war3::SafeReadPtrFast(renderablePart, offset, outPtr) &&
         outPtr != nullptr;
}

bool TryReadSceneNodeFromRenderablePart(void *renderablePart, void *&outSceneNode) {
  outSceneNode = nullptr;
  if (!renderablePart)
    return false;
  return TryReadRenderablePartPtr(renderablePart,
                                  RenderablePartFieldOffsets::SceneNode,
                                  outSceneNode);
}

bool TryReadMeshDataFromRenderablePart(void *renderablePart, void *&outMeshData) {
  outMeshData = nullptr;
  if (!renderablePart)
    return false;
  return TryReadRenderablePartPtr(renderablePart,
                                  RenderablePartFieldOffsets::MeshData,
                                  outMeshData);
}

bool TryReadU32Fast(const void *base, size_t offset, uint32_t &out) {
  out = 0u;
  return base != nullptr && dxvk::war3::SafeReadU32Fast(base, offset, out);
}

bool TryReadPtrFast(const void *base, size_t offset, void *&outPtr) {
  outPtr = nullptr;
  return base != nullptr && dxvk::war3::SafeReadPtrFast(base, offset, outPtr) &&
         outPtr != nullptr;
}

bool LooksLikeRuntimeModelPtr(void* candidate) {
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
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          ownedHandlePtr) &&
      ownedHandlePtr != nullptr &&
      model::ShadowModelResourceCache::instance().resolveDirectModelResourcePtr(
          ownedHandlePtr) != nullptr;
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

void *TryReadRuntimeModelFromSprite(void *spritePtr) {
  void *runtimeModelPtr = nullptr;
  if (!spritePtr)
    return nullptr;
  if (!dxvk::war3::SafeReadPtrFast(spritePtr, dxvk::war3::CSpriteOffsets::Model,
                                   runtimeModelPtr) ||
      !LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return nullptr;
  return runtimeModelPtr;
}

bool IsObviouslyInvalidRuntimeModelForRenderable(
    void* runtimeModelPtr, void* worldObjectEntry, void* sceneNode,
    void* spritePtr, void* renderablePart, void* meshData) {
  if (runtimeModelPtr == nullptr)
    return true;
  if (LooksLikeRuntimeModelPtr(runtimeModelPtr))
    return false;
  return runtimeModelPtr == worldObjectEntry || runtimeModelPtr == sceneNode ||
         runtimeModelPtr == spritePtr || runtimeModelPtr == renderablePart ||
         runtimeModelPtr == meshData;
}

void SanitizeRuntimeModelPtrForRecord(VisibleRenderableRecord& record) {
  void* spritePtr = nullptr;
  if (record.identity.unitPtr != nullptr) {
    game::UnitWrapper unit(record.identity.unitPtr);
    if (unit.IsValid())
      spritePtr = unit.GetSprite();
  }

  if (!IsObviouslyInvalidRuntimeModelForRenderable(
          record.runtimeModelPtr, record.identity.worldObjectEntry,
          record.sceneNode, spritePtr, record.renderablePart, record.meshData)) {
    return;
  }

  record.runtimeModelPtr = nullptr;
}

void* TryReadRuntimeModelFromPointerWindow(void* owner, size_t maxOffset) {
  if (owner == nullptr)
    return nullptr;

  constexpr size_t kPointerStride = sizeof(void*);
  for (size_t offset = 0u; offset <= maxOffset; offset += kPointerStride) {
    void* candidate = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(owner, offset, candidate) ||
        candidate == nullptr) {
      continue;
    }
    if (LooksLikeRuntimeModelPtr(candidate))
      return candidate;
  }

  return nullptr;
}

void* TryReadRuntimeModelFromMeshData(void* meshData) {
  if (!meshData)
    return nullptr;

  void* poseOrTransformCtx = nullptr;
  if (dxvk::war3::SafeReadPtrFast(
          meshData, dxvk::war3::MeshDataOffsets::TransformOrPoseCtx,
          poseOrTransformCtx) &&
      poseOrTransformCtx != nullptr) {
    if (LooksLikeRuntimeModelPtr(poseOrTransformCtx))
      return poseOrTransformCtx;

    // Render-dispatch MeshData usually points at a small pose/transform context
    // rather than CModel directly.  Keep this to a tight pointer window so the
    // visible end-frame hydrate can recover the owning CModel without bringing
    // back the old hot-path registry scans.
    if (void* runtimeModelPtr =
            TryReadRuntimeModelFromPointerWindow(poseOrTransformCtx, 0x60u)) {
      return runtimeModelPtr;
    }
  }

  // Some prepared mesh variants keep the runtime model near the MeshData tail.
  // This is deliberately small and guarded by LooksLikeRuntimeModelPtr: meshData
  // itself is a shared resource key and must never become object identity.
  constexpr size_t kMeshDataRuntimeCandidateOffsets[] = {
      0xE8u, 0xECu, 0xF0u, 0xF4u, 0xF8u, 0xFCu, 0x100u, 0x104u,
      0x108u, 0x10Cu, 0x110u, 0x114u, 0x118u, 0x11Cu, 0x120u};
  for (size_t offset : kMeshDataRuntimeCandidateOffsets) {
    void* candidate = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(meshData, offset, candidate) ||
        candidate == nullptr) {
      continue;
    }
    if (LooksLikeRuntimeModelPtr(candidate))
      return candidate;
  }

  return nullptr;
}

void* TryReadDirectModelResourceFromRuntimeModel(void* runtimeModelPtr) {
  void* ownedHandlePtr = nullptr;
  if (!runtimeModelPtr)
    return nullptr;
  if (!dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          ownedHandlePtr)) {
    return nullptr;
  }

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  return resourceCache.resolveDirectModelResourcePtr(ownedHandlePtr);
}

bool TryResolveRenderablePartFromTransparentPayload(
    void *payload, uint32_t transparentType,
    const RenderObjectIdentitySnapshot &identity, void *&outRenderablePart,
    void *&outSceneNode, void *&outMeshData) {
  outRenderablePart = nullptr;
  outSceneNode = nullptr;
  outMeshData = nullptr;
  if (!payload)
    return false;

  void *sceneCandidate = nullptr;
  void *meshCandidate = nullptr;
  if (!TryReadSceneNodeFromRenderablePart(payload, sceneCandidate) ||
      !TryReadMeshDataFromRenderablePart(payload, meshCandidate)) {
    return false;
  }

  const bool looksLikeDirectPart = transparentType == 0u;
  const bool matchesIdentityScene =
      identity.sceneNode != nullptr && sceneCandidate == identity.sceneNode;
  if (!looksLikeDirectPart && !matchesIdentityScene)
    return false;

  outRenderablePart = payload;
  outSceneNode = sceneCandidate;
  outMeshData = meshCandidate;
  return true;
}

bool TryReadMeshIndexFromMeshData(void *meshData, uint32_t &outMeshIndex) {
  // 仅对 canonical MeshData 结构成立；若 `renderablePart + 0x0C`
  // 实际上是 CGeosetData*，则 0x108 更像 layout/material slot，
  // 不应继续当作 runtime geoset index 解释。
  outMeshIndex = kInvalidVisibleMeshIndex;
  if (!meshData)
    return false;
  return TryReadU32Fast(meshData, dxvk::war3::MeshDataOffsets::MeshIndex,
                        outMeshIndex);
}

bool LooksLikeGeosetDataPtr(void* candidate) {
  if (candidate == nullptr)
    return false;

  uint32_t vertexCount = 0;
  uint32_t primitiveCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t matrixIndexCount = 0;
  void* positions = nullptr;
  void* primitiveRecords = nullptr;
  void* matrixGroupSizes = nullptr;
  void* matrixIndices = nullptr;

  if (!TryReadU32Fast(candidate, dxvk::war3::CGeosetDataOffsets::VertexCount,
                      vertexCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::VertexPositions,
                      positions) ||
      !TryReadU32Fast(candidate,
                      dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
                      primitiveCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
                      primitiveRecords) ||
      !TryReadU32Fast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
                      matrixGroupCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
                      matrixGroupSizes) ||
      !TryReadU32Fast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
                      matrixIndexCount) ||
      !TryReadPtrFast(candidate,
                      dxvk::war3::CGeosetDataOffsets::MatrixIndices,
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
             positions, std::min<size_t>(size_t(vertexCount) * 3u * sizeof(float),
                                         64u)) &&
         dxvk::war3::IsReadableRange(
             primitiveRecords,
             std::min<size_t>(
                 size_t(primitiveCount) *
                    sizeof(dxvk::war3::GeosetPrimitiveRecord),
                 64u)) &&
         dxvk::war3::IsReadableRange(
             matrixGroupSizes,
             std::min<size_t>(size_t(matrixGroupCount) * sizeof(uint32_t), 64u)) &&
         dxvk::war3::IsReadableRange(
             matrixIndices,
             std::min<size_t>(size_t(matrixIndexCount) * sizeof(uint32_t), 64u));
}

bool TryResolveGeosetFromArray(void *ownerPtr, size_t countOffset,
                               size_t arrayOffset, uint32_t geosetIndex,
                               void *&outGeosetPtr, void *&outGeosetDataPtr) {
  outGeosetPtr = nullptr;
  outGeosetDataPtr = nullptr;
  if (ownerPtr == nullptr || geosetIndex == kInvalidVisibleMeshIndex)
    return false;

  uint32_t geosetCount = 0;
  void *geosetArray = nullptr;
  if (!TryReadU32Fast(ownerPtr, countOffset, geosetCount) ||
      !TryReadPtrFast(ownerPtr, arrayOffset, geosetArray) ||
      geosetCount == 0 || geosetIndex >= geosetCount ||
      !dxvk::war3::IsReadableRange(geosetArray,
                                   size_t(geosetCount) * sizeof(void *))) {
    return false;
  }

  auto **entries = reinterpret_cast<void **>(geosetArray);
  outGeosetPtr = entries[geosetIndex];
  if (outGeosetPtr == nullptr)
    return false;

  TryReadPtrFast(outGeosetPtr, dxvk::war3::CGeosetOffsets::GeosetData,
                 outGeosetDataPtr);
  return true;
}

void ResolveGeosetMetadata(VisibleRenderableRecord &record) {
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (record.modelResourcePtr != nullptr) {
    record.modelResourcePtr =
        resourceCache.resolveDirectModelResourcePtr(record.modelResourcePtr);
  }

  record.meshIndex = kInvalidVisibleMeshIndex;
  record.geosetIndex = kInvalidVisibleMeshIndex;
  record.runtimeGeosetPtr = nullptr;
  record.runtimeGeosetDataPtr = nullptr;

  model::ShadowGeosetResourceRecord directGeosetRecord = {};
  if (record.meshData != nullptr &&
      resourceCache.findGeosetByData(record.meshData, directGeosetRecord)) {
    record.runtimeGeosetDataPtr = record.meshData;
    if (directGeosetRecord.geosetPtr != nullptr)
      record.runtimeGeosetPtr = directGeosetRecord.geosetPtr;
    if (record.modelResourcePtr == nullptr &&
        directGeosetRecord.modelResourcePtr != nullptr) {
      record.modelResourcePtr = directGeosetRecord.modelResourcePtr;
    }
    if (record.modelKey == 0u && directGeosetRecord.modelKey != 0u)
      record.modelKey = directGeosetRecord.modelKey;
    if (directGeosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex) {
      record.geosetIndex = directGeosetRecord.geosetIndex;
      record.meshIndex = directGeosetRecord.geosetIndex;
    }
  } else if (record.meshData != nullptr && LooksLikeGeosetDataPtr(record.meshData)) {
    // 新确认的 native 链路里，某些 batch item + 0x0C 直接就是 CGeosetData*，
    // 不能再一概当 MeshData* 解释，否则会把 layout slot 误读成 mesh index。
    record.runtimeGeosetDataPtr = record.meshData;
  }

  if (record.runtimeGeosetDataPtr == nullptr && record.meshData != nullptr)
    TryReadMeshIndexFromMeshData(record.meshData, record.meshIndex);

  if (record.meshIndex == kInvalidVisibleMeshIndex)
    record.geosetIndex = kInvalidVisibleMeshIndex;
  else
    record.geosetIndex = record.meshIndex;

  if (record.runtimeGeosetDataPtr == nullptr &&
      record.meshIndex != kInvalidVisibleMeshIndex) {
    TryResolveGeosetFromArray(record.runtimeModelPtr,
                              dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                              dxvk::war3::CModelOffsets::RuntimeGeosets,
                              record.meshIndex, record.runtimeGeosetPtr,
                              record.runtimeGeosetDataPtr);
  }

  if (record.runtimeGeosetPtr != nullptr || record.runtimeGeosetDataPtr != nullptr) {
    resourceCache.noteRuntimeGeosetBinding(
        record.runtimeModelPtr, record.meshIndex, record.runtimeGeosetPtr,
        record.runtimeGeosetDataPtr, record.modelResourcePtr, record.modelKey);
  }

  model::ShadowGeosetResourceRecord geosetRecord = {};
  if (record.runtimeGeosetPtr != nullptr &&
      resourceCache.findGeosetByPtr(record.runtimeGeosetPtr, geosetRecord)) {
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = geosetRecord.geosetDataPtr;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = geosetRecord.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = geosetRecord.modelKey;
    return;
  }

  if (record.runtimeGeosetDataPtr != nullptr &&
      resourceCache.findGeosetByData(record.runtimeGeosetDataPtr, geosetRecord)) {
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = geosetRecord.geosetPtr;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = geosetRecord.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = geosetRecord.modelKey;
    return;
  }

  if (record.runtimeModelPtr != nullptr &&
      resourceCache.findRuntimeModelGeoset(record.runtimeModelPtr,
                                           record.meshIndex, geosetRecord)) {
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = geosetRecord.geosetPtr;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = geosetRecord.geosetDataPtr;
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = geosetRecord.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = geosetRecord.modelKey;
    return;
  }

  if (record.modelResourcePtr == nullptr)
    return;

  if (record.meshIndex == kInvalidVisibleMeshIndex) {
    model::ShadowModelResourceRecord modelRecord = {};
    if (resourceCache.findModelResource(record.modelResourcePtr, modelRecord) &&
        modelRecord.geosetCount == 1u) {
      record.meshIndex = 0u;
      record.geosetIndex = 0u;
    }
  }

  if (record.meshIndex == kInvalidVisibleMeshIndex)
    return;

  if (resourceCache.findModelGeoset(record.modelResourcePtr, record.meshIndex,
                                    geosetRecord)) {
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = geosetRecord.geosetPtr;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = geosetRecord.geosetDataPtr;
    if (geosetRecord.geosetIndex != model::kInvalidShadowGeosetIndex)
      record.geosetIndex = geosetRecord.geosetIndex;
    return;
  }

  void *resourceGeosetPtr = nullptr;
  void *resourceGeosetDataPtr = nullptr;
  if (TryResolveGeosetFromArray(record.modelResourcePtr,
                                dxvk::war3::CModelDataOffsets::GeosetCount,
                                dxvk::war3::CModelDataOffsets::Geosets,
                                record.meshIndex, resourceGeosetPtr,
                                resourceGeosetDataPtr)) {
    if (record.runtimeGeosetPtr == nullptr)
      record.runtimeGeosetPtr = resourceGeosetPtr;
    if (record.runtimeGeosetDataPtr == nullptr)
      record.runtimeGeosetDataPtr = resourceGeosetDataPtr;
  }
}

bool ResolveRuntimeOwnerFromGeosetBinding(VisibleRenderableRecord& record) {
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  model::ShadowModelResourceRecord runtimeOwner = {};
  if (!resourceCache.findRuntimeModelOwner(record.runtimeGeosetPtr,
                                           record.runtimeGeosetDataPtr,
                                           record.geosetIndex,
                                           record.modelResourcePtr,
                                           runtimeOwner)) {
    return false;
  }

  bool changed = false;
  if (record.runtimeModelPtr == nullptr &&
      runtimeOwner.runtimeModelPtr != nullptr) {
    record.runtimeModelPtr = runtimeOwner.runtimeModelPtr;
    changed = true;
  }
  if (record.modelResourcePtr == nullptr &&
      runtimeOwner.modelResourcePtr != nullptr) {
    record.modelResourcePtr = runtimeOwner.modelResourcePtr;
    changed = true;
  }
  if (record.modelKey == 0u && runtimeOwner.modelKey != 0u) {
    record.modelKey = runtimeOwner.modelKey;
    changed = true;
  }

  return changed;
}

void ResolveModelMetadata(const RenderObjectIdentitySnapshot &identity,
                          void *sceneNode, void* renderablePart, void* meshData,
                          void *&outRuntimeModelPtr,
                          void *&outModelResourcePtr, uint64_t &outModelKey) {
  outRuntimeModelPtr = nullptr;
  outModelResourcePtr = nullptr;
  outModelKey = 0;

  model::ModelInstanceRecord instanceRecord = {};
  bool hasInstanceRecord = false;
  auto &instanceRegistry = model::ModelInstanceRegistry::instance();
  if (identity.worldObjectEntry != nullptr)
    hasInstanceRecord = instanceRegistry.findByWorldObjectEntry(
        identity.worldObjectEntry, instanceRecord);
  if (!hasInstanceRecord && sceneNode != nullptr)
    hasInstanceRecord = instanceRegistry.findBySceneNode(sceneNode, instanceRecord);
  if (!hasInstanceRecord && identity.jHandle != 0u)
    hasInstanceRecord = instanceRegistry.findByHandle(identity.jHandle, instanceRecord);
  if (!hasInstanceRecord && identity.unitPtr != nullptr)
    hasInstanceRecord = instanceRegistry.findByUnitPtr(identity.unitPtr, instanceRecord);

  if (hasInstanceRecord) {
    outRuntimeModelPtr = instanceRecord.runtimeModelPtr;
    outModelResourcePtr = instanceRecord.modelResourcePtr;
    outModelKey = instanceRecord.modelKey;
  }

  ShadowObjectRecord shadowRecord = {};
  bool hasShadowRecord = false;
  auto &shadowRegistry = ShadowObjectRegistry::instance();
  if (identity.worldObjectEntry != nullptr)
    hasShadowRecord = shadowRegistry.findByWorldObjectEntry(
        identity.worldObjectEntry, shadowRecord);
  if (!hasShadowRecord && sceneNode != nullptr)
    hasShadowRecord = shadowRegistry.findBySceneNode(sceneNode, shadowRecord);
  if (!hasShadowRecord && identity.jHandle != 0u)
    hasShadowRecord = shadowRegistry.findByHandle(identity.jHandle, shadowRecord);
  if (!hasShadowRecord && identity.unitPtr != nullptr)
    hasShadowRecord = shadowRegistry.findByUnitPtr(identity.unitPtr, shadowRecord);

  if (hasShadowRecord) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = shadowRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = shadowRecord.modelResourcePtr;
    if (outModelKey == 0)
      outModelKey = shadowRecord.modelKey;
  }

  if (outRuntimeModelPtr != nullptr) {
    if (!hasInstanceRecord)
      hasInstanceRecord =
          instanceRegistry.findByRuntimeModel(outRuntimeModelPtr, instanceRecord);
    if (hasInstanceRecord) {
      if (outModelResourcePtr == nullptr)
        outModelResourcePtr = instanceRecord.modelResourcePtr;
      if (outModelKey == 0u)
        outModelKey = instanceRecord.modelKey;
    }

    if (!hasShadowRecord)
      hasShadowRecord =
          shadowRegistry.findByRuntimeModel(outRuntimeModelPtr, shadowRecord);
    if (hasShadowRecord) {
      if (outModelResourcePtr == nullptr)
        outModelResourcePtr = shadowRecord.modelResourcePtr;
      if (outModelKey == 0u)
        outModelKey = shadowRecord.modelKey;
    }
  }

  const RenderObjectInfo* renderObject = nullptr;
  auto& renderRegistry = RenderObjectRegistry::instance();
  if (sceneNode != nullptr)
    renderObject = renderRegistry.findBySceneNode(sceneNode);
  if (renderObject == nullptr && identity.worldObjectEntry != nullptr)
    renderObject = renderRegistry.findByEntry(identity.worldObjectEntry);
  if (renderObject == nullptr && identity.jHandle != 0u)
    renderObject = renderRegistry.findByHandle(identity.jHandle);

  void *spritePtr = nullptr;
  auto trySpriteLikeRuntimeModel = [&](void* spriteLikePtr) {
    if (spriteLikePtr == nullptr || outRuntimeModelPtr != nullptr)
      return;

    void* runtimeModelCandidate = TryReadRuntimeModelFromSprite(spriteLikePtr);
    if (runtimeModelCandidate == nullptr)
      return;

    outRuntimeModelPtr = runtimeModelCandidate;
    if (spritePtr == nullptr)
      spritePtr = spriteLikePtr;
  };

  auto tryDirectRuntimeModel = [&](void* candidatePtr) {
    if (candidatePtr == nullptr || outRuntimeModelPtr != nullptr)
      return;
    if (!LooksLikeRuntimeModelPtr(candidatePtr))
      return;
    outRuntimeModelPtr = candidatePtr;
  };

  // 这批 anonymous visible subpart 在 renderable 侧经常只有
  // `worldObjectEntry/sceneNode`，没有稳定 `unit->sprite`。当前 live 证据
  // 显示 sceneNode 本身可直接是 CModel；worldObjectEntry 也可能长得像
  // CModel，但把它当 direct runtime 会扩大误命中面，导致错误 caster 共享。
  // 因此 direct runtime 只信 sceneNode，worldObjectEntry 继续走 sprite-like
  // host 试读路径。
  tryDirectRuntimeModel(sceneNode);
  trySpriteLikeRuntimeModel(identity.worldObjectEntry);
  trySpriteLikeRuntimeModel(sceneNode);

  void* resolvedUnitPtr =
      identity.unitPtr != nullptr ? identity.unitPtr
                                  : renderObject != nullptr
                                        ? renderObject->unitPtr
                                        : nullptr;
  if (resolvedUnitPtr != nullptr) {
    game::UnitWrapper unit(resolvedUnitPtr);
    if (unit.IsValid())
      spritePtr = unit.GetSprite();
  }

  if (!hasInstanceRecord && spritePtr != nullptr)
    hasInstanceRecord =
        instanceRegistry.findBySpritePtr(spritePtr, instanceRecord);
  if (!hasShadowRecord && spritePtr != nullptr)
    hasShadowRecord = shadowRegistry.findBySpritePtr(spritePtr, shadowRecord);

  if (hasInstanceRecord) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = instanceRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = instanceRecord.modelResourcePtr;
    if (outModelKey == 0u)
      outModelKey = instanceRecord.modelKey;
  }
  if (hasShadowRecord) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = shadowRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = shadowRecord.modelResourcePtr;
    if (outModelKey == 0u)
      outModelKey = shadowRecord.modelKey;
  }

  auto sanitizeRuntimeModelPtr = [&]() {
    if (!IsObviouslyInvalidRuntimeModelForRenderable(
            outRuntimeModelPtr, identity.worldObjectEntry, sceneNode, spritePtr,
            renderablePart, meshData)) {
      return;
    }
    outRuntimeModelPtr = nullptr;
  };

  sanitizeRuntimeModelPtr();

  if (outRuntimeModelPtr == nullptr && spritePtr != nullptr)
    outRuntimeModelPtr = TryReadRuntimeModelFromSprite(spritePtr);
  sanitizeRuntimeModelPtr();

  if (outRuntimeModelPtr == nullptr && meshData != nullptr)
    outRuntimeModelPtr = TryReadRuntimeModelFromMeshData(meshData);
  sanitizeRuntimeModelPtr();

  if (outModelResourcePtr == nullptr && outRuntimeModelPtr != nullptr)
    outModelResourcePtr =
        TryReadDirectModelResourceFromRuntimeModel(outRuntimeModelPtr);

  model::ShadowModelResourceRecord runtimeResourceRecord = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  auto backfillRuntimeResourceCache = [&]() {
    if (outModelResourcePtr != nullptr) {
      outModelResourcePtr =
          resourceCache.resolveDirectModelResourcePtr(outModelResourcePtr);
    }

    if (outRuntimeModelPtr != nullptr) {
      model::ShadowModelResourceRecord existingRuntimeRecord = {};
      const bool hasExistingRuntimeRecord =
          resourceCache.findRuntimeModelResource(outRuntimeModelPtr,
                                                 existingRuntimeRecord);
      const bool needsRuntimeBackfill =
          !hasExistingRuntimeRecord ||
          (existingRuntimeRecord.modelResourcePtr == nullptr &&
           outModelResourcePtr != nullptr) ||
          (existingRuntimeRecord.modelKey == 0u && outModelKey != 0u);
      if (needsRuntimeBackfill) {
        resourceCache.noteRuntimeModelBinding(outRuntimeModelPtr,
                                              outModelResourcePtr, outModelKey);
      }
    }

    if (outModelResourcePtr != nullptr) {
      model::ShadowModelResourceRecord existingModelRecord = {};
      const bool hasExistingModelRecord =
          resourceCache.findModelResource(outModelResourcePtr,
                                          existingModelRecord);
      const bool needsModelBackfill =
          !hasExistingModelRecord ||
          (existingModelRecord.modelKey == 0u && outModelKey != 0u);
      if (needsModelBackfill) {
        resourceCache.noteModelResourceBinding(outModelResourcePtr, outModelKey);
      }
    }
  };

  if (outRuntimeModelPtr != nullptr &&
      resourceCache.findRuntimeModelResource(outRuntimeModelPtr,
                                             runtimeResourceRecord)) {
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = runtimeResourceRecord.modelResourcePtr;
    if (outModelKey == 0u)
      outModelKey = runtimeResourceRecord.modelKey;
  }

  model::ModelResourceRecord modelRecord = {};
  auto &modelRegistry = model::ModelRegistry::instance();
  if (outRuntimeModelPtr != nullptr &&
      modelRegistry.findByRuntimeModel(outRuntimeModelPtr, modelRecord)) {
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = modelRecord.modelResourcePtr;
    if (outModelKey == 0)
      outModelKey = modelRecord.modelKey;
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = modelRecord.runtimeModelPtr;
    backfillRuntimeResourceCache();
    return;
  }

  if (spritePtr != nullptr && modelRegistry.findBySprite(spritePtr, modelRecord)) {
    if (outRuntimeModelPtr == nullptr)
      outRuntimeModelPtr = modelRecord.runtimeModelPtr;
    if (outModelResourcePtr == nullptr)
      outModelResourcePtr = modelRecord.modelResourcePtr;
    if (outModelKey == 0)
      outModelKey = modelRecord.modelKey;
  }

  if (outModelResourcePtr != nullptr) {
    outModelResourcePtr =
        resourceCache.resolveDirectModelResourcePtr(outModelResourcePtr);
  }

  const bool needsRuntimeBridgeAugment =
      sceneNode != nullptr &&
      (outRuntimeModelPtr == nullptr || outModelResourcePtr == nullptr ||
       outModelKey == 0u);
  if (needsRuntimeBridgeAugment) {
    dxvk::War3ShadowSemanticContext semantic = {};
    semantic.renderablePart = renderablePart;
    semantic.sceneNode = sceneNode;
    semantic.worldObjectEntry = identity.worldObjectEntry;
    semantic.runtimeModelPtr = outRuntimeModelPtr;
    semantic.modelResourcePtr = outModelResourcePtr;
    semantic.jHandle = identity.jHandle;
    semantic.rawcode = identity.rawcode;
    semantic.modelKey = outModelKey;
    semantic.objectKind = identity.kind;
    if (render::AugmentShadowSemanticContext(semantic, nullptr)) {
      if (outRuntimeModelPtr == nullptr)
        outRuntimeModelPtr = semantic.runtimeModelPtr;
      if (outModelResourcePtr == nullptr)
        outModelResourcePtr = semantic.modelResourcePtr;
      if (outModelKey == 0u)
        outModelKey = semantic.modelKey;

      if (outModelResourcePtr != nullptr) {
        outModelResourcePtr =
            resourceCache.resolveDirectModelResourcePtr(outModelResourcePtr);
      }
    }
  }

  sanitizeRuntimeModelPtr();

  backfillRuntimeResourceCache();
}

const VisibleRenderableRecord *
FindPriorRecord(const VisibleRenderableRegistry::Snapshot &prior,
                const VisibleRenderableRecord &record) {
  auto findByIndex = [&](const auto &map, const auto &key)
      -> const VisibleRenderableRecord * {
    const auto it = map.find(key);
    if (it == map.end() || it->second >= prior.records.size())
      return nullptr;
    return &prior.records[it->second];
  };

  if (record.identity.worldObjectEntry != nullptr) {
    if (const auto *match =
            findByIndex(prior.byWorldObjectEntry, record.identity.worldObjectEntry))
      return match;
  }
  if (record.sceneNode != nullptr) {
    if (const auto *match = findByIndex(prior.bySceneNode, record.sceneNode))
      return match;
  }
  if (record.identity.jHandle != 0u) {
    if (const auto *match = findByIndex(prior.byHandle, record.identity.jHandle))
      return match;
  }
  if (record.runtimeModelPtr != nullptr) {
    if (const auto *match =
            findByIndex(prior.byRuntimeModel, record.runtimeModelPtr))
      return match;
  }
  if (record.renderablePart != nullptr) {
    if (const auto *match =
            findByIndex(prior.byRenderablePart, record.renderablePart))
      return match;
  }
  if (record.payload != nullptr) {
    if (const auto *match = findByIndex(prior.byPayload, record.payload))
      return match;
  }

  return nullptr;
}

const VisibleRenderableRecord*
FindSnapshotRecord(const VisibleRenderableRegistry::Snapshot& snap,
                   const VisibleRenderableRecord& record) {
  auto findByIndex = [&](const auto& map, const auto& key)
      -> const VisibleRenderableRecord* {
    const auto it = map.find(key);
    if (it == map.end() || it->second >= snap.records.size())
      return nullptr;
    return &snap.records[it->second];
  };

  if (record.identity.worldObjectEntry != nullptr) {
    if (const auto* match =
            findByIndex(snap.byWorldObjectEntry, record.identity.worldObjectEntry)) {
      return match;
    }
  }
  if (record.sceneNode != nullptr) {
    if (const auto* match = findByIndex(snap.bySceneNode, record.sceneNode))
      return match;
  }
  if (record.identity.jHandle != 0u) {
    if (const auto* match = findByIndex(snap.byHandle, record.identity.jHandle))
      return match;
  }
  if (record.runtimeModelPtr != nullptr) {
    if (const auto* match = findByIndex(snap.byRuntimeModel, record.runtimeModelPtr))
      return match;
  }
  if (record.renderablePart != nullptr) {
    if (const auto* match =
            findByIndex(snap.byRenderablePart, record.renderablePart)) {
      return match;
    }
  }
  if (record.payload != nullptr) {
    if (const auto* match = findByIndex(snap.byPayload, record.payload))
      return match;
  }

  return nullptr;
}

void MergeIdentityFromPrior(const VisibleRenderableRecord &prior,
                            VisibleRenderableRecord &record) {
  if (record.identity.worldObjectEntry == nullptr)
    record.identity.worldObjectEntry = prior.identity.worldObjectEntry;
  if (record.identity.sceneNode == nullptr)
    record.identity.sceneNode = prior.identity.sceneNode;
  if (record.identity.unitPtr == nullptr)
    record.identity.unitPtr = prior.identity.unitPtr;
  if (record.identity.agentPtr == nullptr)
    record.identity.agentPtr = prior.identity.agentPtr;
  if (record.identity.handleId == 0u)
    record.identity.handleId = prior.identity.handleId;
  if (record.identity.jHandle == 0u)
    record.identity.jHandle = prior.identity.jHandle;
  if (record.identity.rawcode == 0u)
    record.identity.rawcode = prior.identity.rawcode;
  if (record.identity.agentType == 0u)
    record.identity.agentType = prior.identity.agentType;
  if (record.identity.flags5C == 0u)
    record.identity.flags5C = prior.identity.flags5C;
  if (record.identity.kind == ObjectKind::Unknown &&
      prior.identity.kind != ObjectKind::Unknown) {
    record.identity.kind = prior.identity.kind;
  }
  if (record.identity.groupIdx < 0 && prior.identity.groupIdx >= 0)
    record.identity.groupIdx = prior.identity.groupIdx;

  if (record.sceneNode == nullptr)
    record.sceneNode = prior.sceneNode;
  if (record.runtimeModelPtr == nullptr)
    record.runtimeModelPtr = prior.runtimeModelPtr;
  if (record.modelResourcePtr == nullptr)
    record.modelResourcePtr = prior.modelResourcePtr;
  if (record.modelKey == 0u)
    record.modelKey = prior.modelKey;
  // Do not copy per-renderable geoset/slice data from a sibling or prior record.
  // Multiple units and multiple submeshes can share sceneNode/modelResource keys,
  // and copying the previous geoset here produced the "one caster fragment on
  // every caster" artifact.  Current-frame meshData must resolve its own geoset.
}

void FinalizeVisibleRecord(VisibleRenderableRegistry::Snapshot &snap,
                           const VisibleRenderableRegistry::Snapshot &prior,
                           VisibleRenderableRecord &record) {
  if (record.payload == nullptr)
    record.payload = record.renderablePart;

  if (record.sceneNode == nullptr && record.identity.sceneNode != nullptr)
    record.sceneNode = record.identity.sceneNode;
  if (record.sceneNode == nullptr && record.renderablePart != nullptr)
    TryReadSceneNodeFromRenderablePart(record.renderablePart, record.sceneNode);
  if (record.identity.sceneNode == nullptr && record.sceneNode != nullptr)
    record.identity.sceneNode = record.sceneNode;

  if (record.meshData == nullptr && record.renderablePart != nullptr)
    TryReadMeshDataFromRenderablePart(record.renderablePart, record.meshData);

  const bool identityMissingClassification =
      record.identity.kind == ObjectKind::Unknown &&
      (record.identity.worldObjectEntry != nullptr || record.sceneNode != nullptr ||
       record.identity.sceneNode != nullptr);
  const bool identityMissingRawcode =
      record.identity.rawcode == 0u &&
      (record.identity.jHandle != 0u || record.identity.unitPtr != nullptr ||
       record.identity.worldObjectEntry != nullptr);
  const bool needsIdentityResolve =
      !record.identity.HasStableIdentity() ||
      (record.sceneNode == nullptr && record.identity.sceneNode == nullptr) ||
      identityMissingClassification || identityMissingRawcode;
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeIdentityResolve) {
    if (needsIdentityResolve) {
    RenderObjectIdentitySnapshot resolvedIdentity = {};
    if (TryResolveRenderObjectIdentity(record.identity.worldObjectEntry,
                                       record.sceneNode, resolvedIdentity)) {
      record.identity = resolvedIdentity;
      if (record.sceneNode == nullptr)
        record.sceneNode = resolvedIdentity.sceneNode;
    }
    }
  }

  if ((record.identity.kind == ObjectKind::Unknown ||
       record.identity.rawcode == 0u ||
       !record.identity.HasStableIdentity())) {
    if constexpr (!dxvk::war3::internal::
                      kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery) {
      if (const auto *priorRecord = FindPriorRecord(prior, record))
        MergeIdentityFromPrior(*priorRecord, record);
    }
  }
  SanitizeRuntimeModelPtrForRecord(record);

  if (record.runtimeModelPtr == nullptr && record.modelResourcePtr == nullptr &&
      record.modelKey == 0u && record.sceneNode != nullptr) {
    const auto it = snap.modelMetadataBySceneNode.find(record.sceneNode);
    if (it != snap.modelMetadataBySceneNode.end()) {
      record.runtimeModelPtr = it->second.runtimeModelPtr;
      record.modelResourcePtr = it->second.modelResourcePtr;
      record.modelKey = it->second.modelKey;
    }
  }

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeModelMetadata) {
    if (record.runtimeModelPtr == nullptr || record.modelResourcePtr == nullptr ||
        record.modelKey == 0u) {
      ResolveModelMetadata(record.identity, record.sceneNode, record.renderablePart,
                           record.meshData,
                           record.runtimeModelPtr, record.modelResourcePtr,
                           record.modelKey);
      if (record.sceneNode != nullptr) {
        snap.modelMetadataBySceneNode[record.sceneNode] = {
            record.runtimeModelPtr, record.modelResourcePtr, record.modelKey};
      }
    }
  }

  if (record.identity.kind == ObjectKind::Unknown ||
      record.identity.rawcode == 0u || record.identity.jHandle == 0u) {
    if constexpr (!dxvk::war3::internal::
                      kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery) {
      if (const auto *priorRecord = FindPriorRecord(prior, record))
        MergeIdentityFromPrior(*priorRecord, record);
    }
  }
  SanitizeRuntimeModelPtrForRecord(record);

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeGeosetMetadata) {
    ResolveGeosetMetadata(record);
    if (ResolveRuntimeOwnerFromGeosetBinding(record)) {
      SanitizeRuntimeModelPtrForRecord(record);
      ResolveGeosetMetadata(record);
    }
  }

  const bool needsSiblingRecovery =
      !record.identity.HasStableIdentity() ||
      record.runtimeModelPtr == nullptr ||
      record.modelResourcePtr == nullptr || record.modelKey == 0u ||
      record.runtimeGeosetPtr == nullptr ||
      record.runtimeGeosetDataPtr == nullptr;
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeSiblingRecovery) {
    if (needsSiblingRecovery) {
      if (const auto* siblingRecord = FindSnapshotRecord(snap, record))
        MergeIdentityFromPrior(*siblingRecord, record);
      if (const auto* priorRecord = FindPriorRecord(prior, record))
        MergeIdentityFromPrior(*priorRecord, record);
    }
  }
  SanitizeRuntimeModelPtrForRecord(record);

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeModelMetadata) {
    if (record.runtimeModelPtr == nullptr || record.modelResourcePtr == nullptr ||
        record.modelKey == 0u) {
      ResolveModelMetadata(record.identity, record.sceneNode, record.renderablePart,
                           record.meshData,
                           record.runtimeModelPtr, record.modelResourcePtr,
                           record.modelKey);
      if (record.sceneNode != nullptr) {
        snap.modelMetadataBySceneNode[record.sceneNode] = {
            record.runtimeModelPtr, record.modelResourcePtr, record.modelKey};
      }
    }
  }

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleFinalizeGeosetMetadata) {
    ResolveGeosetMetadata(record);
    if (ResolveRuntimeOwnerFromGeosetBinding(record)) {
      SanitizeRuntimeModelPtrForRecord(record);
      ResolveGeosetMetadata(record);
    }
  }
}

} // namespace

VisibleRenderableRegistry &VisibleRenderableRegistry::instance() {
  static VisibleRenderableRegistry *s_instance = new VisibleRenderableRegistry();
  return *s_instance;
}

void VisibleRenderableRegistry::appendRecord(Snapshot &snap,
                                             VisibleRenderableRecord &record) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigLightweightSemanticVisibleRenderableWrites) {
    const Snapshot &prior = readSnapshot();
    FinalizeVisibleRecord(snap, prior, record);
  } else if (record.payload == nullptr) {
    record.payload = record.renderablePart;
  }

  const uint32_t index = static_cast<uint32_t>(snap.records.size());
  snap.records.emplace_back(record);

  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
                dxvk::war3::internal::
                    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
    if (record.payload != nullptr)
      snap.byPayload[record.payload] = index;
    if (record.renderablePart != nullptr)
      snap.byRenderablePart[record.renderablePart] = index;
    if (record.identity.worldObjectEntry != nullptr)
      snap.byWorldObjectEntry[record.identity.worldObjectEntry] = index;
    if (record.identity.jHandle != 0u)
      snap.byHandle[record.identity.jHandle] = index;
    if (record.sceneNode != nullptr)
      snap.bySceneNode[record.sceneNode] = index;
    if (record.meshData != nullptr)
      snap.byMeshData[record.meshData] = index;
    if (record.runtimeModelPtr != nullptr)
      snap.byRuntimeModel[record.runtimeModelPtr] = index;
    if (record.runtimeGeosetPtr != nullptr)
      snap.byRuntimeGeoset[record.runtimeGeosetPtr] = index;
    if (record.runtimeGeosetDataPtr != nullptr)
      snap.byRuntimeGeosetData[record.runtimeGeosetDataPtr] = index;
  }

  if (record.queueKind == VisibleRenderableQueueKind::Transparent)
    ++snap.transparentCount;
  else
    ++snap.mainQueueCount;
}

namespace {

void RebuildVisibleSnapshotIndexes(VisibleRenderableRegistry::Snapshot &snap) {
  const size_t recordCount = snap.records.size();

  snap.byPayload.clear();
  snap.byRenderablePart.clear();
  snap.byWorldObjectEntry.clear();
  snap.byHandle.clear();
  snap.bySceneNode.clear();
  snap.byMeshData.clear();
  snap.byRuntimeModel.clear();
  snap.byRuntimeGeoset.clear();
  snap.byRuntimeGeosetData.clear();
  snap.mainQueueCount = 0;
  snap.transparentCount = 0;

  snap.byPayload.reserve(recordCount);
  snap.byRenderablePart.reserve(recordCount);
  snap.byWorldObjectEntry.reserve(recordCount);
  snap.byHandle.reserve(recordCount);
  snap.bySceneNode.reserve(recordCount);
  snap.byMeshData.reserve(recordCount);
  snap.byRuntimeModel.reserve(recordCount);
  snap.byRuntimeGeoset.reserve(recordCount);
  snap.byRuntimeGeosetData.reserve(recordCount);

  for (uint32_t index = 0; index < snap.records.size(); ++index) {
    const VisibleRenderableRecord &record = snap.records[index];
    if (record.payload != nullptr)
      snap.byPayload[record.payload] = index;
    if (record.renderablePart != nullptr)
      snap.byRenderablePart[record.renderablePart] = index;
    if (record.identity.worldObjectEntry != nullptr)
      snap.byWorldObjectEntry[record.identity.worldObjectEntry] = index;
    if (record.identity.jHandle != 0u)
      snap.byHandle[record.identity.jHandle] = index;
    if (record.sceneNode != nullptr)
      snap.bySceneNode[record.sceneNode] = index;
    if (record.meshData != nullptr)
      snap.byMeshData[record.meshData] = index;
    if (record.runtimeModelPtr != nullptr)
      snap.byRuntimeModel[record.runtimeModelPtr] = index;
    if (record.runtimeGeosetPtr != nullptr)
      snap.byRuntimeGeoset[record.runtimeGeosetPtr] = index;
    if (record.runtimeGeosetDataPtr != nullptr)
      snap.byRuntimeGeosetData[record.runtimeGeosetDataPtr] = index;

    if (record.queueKind == VisibleRenderableQueueKind::Transparent)
      ++snap.transparentCount;
    else
      ++snap.mainQueueCount;
  }
}

void HydrateVisibleSnapshotBasicFields(VisibleRenderableRegistry::Snapshot &snap) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticVisibleEndFrameBasicHydrate) {
    return;
  }

  if (snap.records.empty())
    return;

  bool changed = false;
  std::unordered_map<void *, RenderablePartBasicFields> partCache;
  partCache.reserve(snap.records.size());

  for (VisibleRenderableRecord &record : snap.records) {
    if (record.payload == nullptr)
      record.payload = record.renderablePart;

    if (record.sceneNode == nullptr && record.identity.sceneNode != nullptr)
      record.sceneNode = record.identity.sceneNode;

    if (record.renderablePart != nullptr &&
        (record.sceneNode == nullptr || record.meshData == nullptr)) {
      auto it = partCache.find(record.renderablePart);
      if (it == partCache.end()) {
        RenderablePartBasicFields fields = {};
        TryReadSceneNodeFromRenderablePart(record.renderablePart, fields.sceneNode);
        TryReadMeshDataFromRenderablePart(record.renderablePart, fields.meshData);
        it = partCache.emplace(record.renderablePart, fields).first;
      }

      if (record.sceneNode == nullptr && it->second.sceneNode != nullptr) {
        record.sceneNode = it->second.sceneNode;
        changed = true;
      }
      if (record.meshData == nullptr && it->second.meshData != nullptr) {
        record.meshData = it->second.meshData;
        changed = true;
      }
    }

    if (record.identity.sceneNode == nullptr && record.sceneNode != nullptr) {
      record.identity.sceneNode = record.sceneNode;
      changed = true;
    }
  }

  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate) {
    constexpr size_t kMaxVisibleGeosetHydrateRecords = 384u;
    size_t hydrated = 0u;
    for (VisibleRenderableRecord& record : snap.records) {
      if (hydrated >= kMaxVisibleGeosetHydrateRecords)
        break;
      if (record.queueKind != VisibleRenderableQueueKind::MainQueue)
        continue;
      if (record.meshData == nullptr && record.renderablePart == nullptr)
        continue;

      const bool unitLike =
          record.identity.kind == ObjectKind::Unit ||
          record.identity.unitPtr != nullptr ||
          (record.identity.kind == ObjectKind::Unknown &&
           record.identity.rawcode != 0u);
      if (!unitLike)
        continue;

      ++hydrated;
      const void* beforeRuntime = record.runtimeModelPtr;
      const void* beforeModel = record.modelResourcePtr;
      const void* beforeGeoset = record.runtimeGeosetPtr;
      const void* beforeGeosetData = record.runtimeGeosetDataPtr;
      const uint32_t beforeGeosetIndex = record.geosetIndex;

      if (record.runtimeModelPtr == nullptr ||
          record.modelResourcePtr == nullptr || record.modelKey == 0u) {
        ResolveModelMetadata(record.identity, record.sceneNode,
                             record.renderablePart, record.meshData,
                             record.runtimeModelPtr, record.modelResourcePtr,
                             record.modelKey);
        SanitizeRuntimeModelPtrForRecord(record);
      }

      ResolveGeosetMetadata(record);
      if (ResolveRuntimeOwnerFromGeosetBinding(record)) {
        SanitizeRuntimeModelPtrForRecord(record);
        ResolveGeosetMetadata(record);
      }

      changed |= beforeRuntime != record.runtimeModelPtr ||
                 beforeModel != record.modelResourcePtr ||
                 beforeGeoset != record.runtimeGeosetPtr ||
                 beforeGeosetData != record.runtimeGeosetDataPtr ||
                 beforeGeosetIndex != record.geosetIndex;
    }
  }

  if ((changed || dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) &&
      (dxvk::war3::internal::
           kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame ||
       dxvk::war3::internal::
           kWar3RuntimeConfigSemanticVisibleEndFrameUnitGeosetHydrate))
    RebuildVisibleSnapshotIndexes(snap);
}

bool ShouldHydrateStaticSemanticRecord(const VisibleRenderableRecord& record) {
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigSemanticVisibleEndFrameStaticHydrate) {
    return false;
  }

  if (!dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() ||
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
    return false;
  }

  return record.identity.kind == ObjectKind::Building ||
         record.identity.kind == ObjectKind::Destructible;
}

void HydrateVisibleSnapshotStaticSemanticFields(
    VisibleRenderableRegistry::Snapshot& snap) {
  if (snap.records.empty())
    return;

  constexpr size_t kMaxStaticHydrateRecords = 64u;
  size_t hydrated = 0u;
  bool changed = false;

  for (VisibleRenderableRecord& record : snap.records) {
    if (!ShouldHydrateStaticSemanticRecord(record))
      continue;
    if (hydrated >= kMaxStaticHydrateRecords)
      break;
    ++hydrated;

    if (record.payload == nullptr)
      record.payload = record.renderablePart;
    if (record.sceneNode == nullptr && record.identity.sceneNode != nullptr)
      record.sceneNode = record.identity.sceneNode;
    if (record.renderablePart != nullptr) {
      if (record.sceneNode == nullptr)
        changed |= TryReadSceneNodeFromRenderablePart(record.renderablePart,
                                                     record.sceneNode);
      if (record.meshData == nullptr)
        changed |=
            TryReadMeshDataFromRenderablePart(record.renderablePart,
                                             record.meshData);
    }
    if (record.identity.sceneNode == nullptr && record.sceneNode != nullptr) {
      record.identity.sceneNode = record.sceneNode;
      changed = true;
    }

    const bool hadGeoset = record.HasResolvedGeoset();
    ResolveGeosetMetadata(record);
    changed |= !hadGeoset && record.HasResolvedGeoset();
  }

  if ((changed || dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) &&
      dxvk::war3::internal::
          kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame) {
    RebuildVisibleSnapshotIndexes(snap);
  }
}

} // namespace

VisibleRenderableRegistry::Snapshot &VisibleRenderableRegistry::writeSnapshot() {
  return m_snapshots[m_writeIndex];
}

const VisibleRenderableRegistry::Snapshot &
VisibleRenderableRegistry::readSnapshot() const {
  const uint32_t index = m_publishedIndex.load(std::memory_order_acquire);
  return m_snapshots[index];
}

const VisibleRenderableRegistry::Snapshot &
VisibleRenderableRegistry::snapshotForThread() const {
  if (std::this_thread::get_id() == m_renderThreadId)
    return m_snapshots[m_writeIndex];
  return readSnapshot();
}

void VisibleRenderableRegistry::beginFrame() {
  m_renderThreadId = std::this_thread::get_id();
  const uint32_t published = m_publishedIndex.load(std::memory_order_relaxed);
  m_writeIndex = (published + 1u) % kSnapshotCount;

  Snapshot &snap = m_snapshots[m_writeIndex];
  const size_t reserveRecord = snap.lastRecordCount;
  snap.records.clear();
  snap.mainQueueCount = 0;
  snap.transparentCount = 0;
  if (reserveRecord)
    snap.records.reserve(reserveRecord);

  constexpr bool kMaintainIndexes =
      !dxvk::war3::internal::kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
      dxvk::war3::internal::
          kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes ||
      dxvk::war3::internal::kWar3RuntimeConfigBuildSemanticVisibleIndexesAtEndFrame;

  if constexpr (kMaintainIndexes) {
    const size_t reservePayload = snap.lastPayloadCount;
    const size_t reserveRenderablePart = snap.lastRenderablePartCount;
    const size_t reserveWorldObject = snap.lastWorldObjectCount;
    const size_t reserveHandle = snap.lastHandleCount;
    const size_t reserveSceneNode = snap.lastSceneNodeCount;
    const size_t reserveMeshData = snap.lastMeshDataCount;
    const size_t reserveRuntimeModel = snap.lastRuntimeModelCount;
    const size_t reserveRuntimeGeoset = snap.lastRuntimeGeosetCount;
    const size_t reserveRuntimeGeosetData = snap.lastRuntimeGeosetDataCount;
    const size_t reserveModelMetadata = snap.lastModelMetadataCount;

    snap.byPayload.clear();
    snap.byRenderablePart.clear();
    snap.byWorldObjectEntry.clear();
    snap.byHandle.clear();
    snap.bySceneNode.clear();
    snap.byMeshData.clear();
    snap.byRuntimeModel.clear();
    snap.byRuntimeGeoset.clear();
    snap.byRuntimeGeosetData.clear();
    snap.modelMetadataBySceneNode.clear();

    if (reservePayload)
      snap.byPayload.reserve(reservePayload);
    if (reserveRenderablePart)
      snap.byRenderablePart.reserve(reserveRenderablePart);
    if (reserveWorldObject)
      snap.byWorldObjectEntry.reserve(reserveWorldObject);
    if (reserveHandle)
      snap.byHandle.reserve(reserveHandle);
    if (reserveSceneNode)
      snap.bySceneNode.reserve(reserveSceneNode);
    if (reserveMeshData)
      snap.byMeshData.reserve(reserveMeshData);
    if (reserveRuntimeModel)
      snap.byRuntimeModel.reserve(reserveRuntimeModel);
    if (reserveRuntimeGeoset)
      snap.byRuntimeGeoset.reserve(reserveRuntimeGeoset);
    if (reserveRuntimeGeosetData)
      snap.byRuntimeGeosetData.reserve(reserveRuntimeGeosetData);
    if (reserveModelMetadata)
      snap.modelMetadataBySceneNode.reserve(reserveModelMetadata);
  } else {
    // 当前数据层性能模式不维护多索引表，避免每帧 clear/reserve 多张
    // unordered_map。少量查询由 query* 的线性兜底处理。
    if (!snap.byPayload.empty())
      snap.byPayload.clear();
    if (!snap.byRenderablePart.empty())
      snap.byRenderablePart.clear();
    if (!snap.byWorldObjectEntry.empty())
      snap.byWorldObjectEntry.clear();
    if (!snap.byHandle.empty())
      snap.byHandle.clear();
    if (!snap.bySceneNode.empty())
      snap.bySceneNode.clear();
    if (!snap.byMeshData.empty())
      snap.byMeshData.clear();
    if (!snap.byRuntimeModel.empty())
      snap.byRuntimeModel.clear();
    if (!snap.byRuntimeGeoset.empty())
      snap.byRuntimeGeoset.clear();
    if (!snap.byRuntimeGeosetData.empty())
      snap.byRuntimeGeosetData.clear();
    if (!snap.modelMetadataBySceneNode.empty())
      snap.modelMetadataBySceneNode.clear();
  }

  m_frameNumber.fetch_add(1, std::memory_order_relaxed);
}

void VisibleRenderableRegistry::endFrame() {
  Snapshot &snap = m_snapshots[m_writeIndex];
  HydrateVisibleSnapshotBasicFields(snap);
  HydrateVisibleSnapshotStaticSemanticFields(snap);

  snap.lastRecordCount = snap.records.size();
  snap.lastPayloadCount = snap.byPayload.size();
  snap.lastRenderablePartCount = snap.byRenderablePart.size();
  snap.lastWorldObjectCount = snap.byWorldObjectEntry.size();
  snap.lastHandleCount = snap.byHandle.size();
  snap.lastSceneNodeCount = snap.bySceneNode.size();
  snap.lastMeshDataCount = snap.byMeshData.size();
  snap.lastRuntimeModelCount = snap.byRuntimeModel.size();
  snap.lastRuntimeGeosetCount = snap.byRuntimeGeoset.size();
  snap.lastRuntimeGeosetDataCount = snap.byRuntimeGeosetData.size();
  snap.lastModelMetadataCount = snap.modelMetadataBySceneNode.size();
  m_publishedIndex.store(m_writeIndex, std::memory_order_release);

  static std::atomic<uint32_t> s_logCount{0};
  const uint32_t logCount = s_logCount.fetch_add(1, std::memory_order_relaxed);
  if ((snap.mainQueueCount != 0 || snap.transparentCount != 0) &&
      (logCount < 12u || (logCount % 120u) == 0u)) {
    dxvk::war3dbg::Print(
        "DXVK VisibleManifest: frame=%llu total=%zu main=%zu transparent=%zu "
        "payload=%zu part=%zu entry=%zu handle=%zu scene=%zu mesh=%zu "
        "runtime=%zu rtGeo=%zu rtGeoData=%zu\n",
        static_cast<unsigned long long>(
            m_frameNumber.load(std::memory_order_relaxed)),
        snap.records.size(), snap.mainQueueCount, snap.transparentCount,
        snap.byPayload.size(), snap.byRenderablePart.size(),
        snap.byWorldObjectEntry.size(), snap.byHandle.size(),
        snap.bySceneNode.size(), snap.byMeshData.size(),
        snap.byRuntimeModel.size(), snap.byRuntimeGeoset.size(),
        snap.byRuntimeGeosetData.size());
  }
}

void VisibleRenderableRegistry::registerMainQueueRange(
    void *batchArray, uint32_t before, uint32_t after,
    const RenderObjectIdentitySnapshot &identity) {
  if constexpr (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled) {
    return;
  }
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites) {
    return;
  }

  if (!batchArray || after <= before)
    return;

  Snapshot &snap = writeSnapshot();
  snap.records.reserve(snap.records.size() + (after - before));
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
                dxvk::war3::internal::
                    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
    snap.byPayload.reserve(snap.byPayload.size() + (after - before));
    snap.byRenderablePart.reserve(snap.byRenderablePart.size() + (after - before));
    snap.byWorldObjectEntry.reserve(snap.byWorldObjectEntry.size() + (after - before));
    snap.byHandle.reserve(snap.byHandle.size() + (after - before));
    snap.bySceneNode.reserve(snap.bySceneNode.size() + (after - before));
    snap.byMeshData.reserve(snap.byMeshData.size() + (after - before));
    snap.byRuntimeModel.reserve(snap.byRuntimeModel.size() + (after - before));
    snap.byRuntimeGeoset.reserve(snap.byRuntimeGeoset.size() + (after - before));
    snap.byRuntimeGeosetData.reserve(
        snap.byRuntimeGeosetData.size() + (after - before));
  }

  auto *base = reinterpret_cast<std::uint8_t *>(batchArray);
  for (uint32_t i = before; i < after; ++i) {
    auto *element = base + i * kRenderBatchElementStride;

    VisibleRenderableRecord record = {};
    record.queueKind = VisibleRenderableQueueKind::MainQueue;
    record.payload = *reinterpret_cast<void **>(
        element + RenderBatchElementOffsets::BatchEntry);
    record.renderablePart = *reinterpret_cast<void **>(
        element + RenderBatchElementOffsets::BatchEntry);
    record.flags = *reinterpret_cast<uint32_t *>(
        element + RenderBatchElementOffsets::Flags);
    record.layerIndex = *reinterpret_cast<uint32_t *>(
        element + RenderBatchElementOffsets::LayerIndex);
    record.subIndex = *reinterpret_cast<uint32_t *>(
        element + RenderBatchElementOffsets::LayerCounter);
    record.layerState = *reinterpret_cast<void **>(
        element + RenderBatchElementOffsets::LayerState);
    record.queueSlot = i;
    record.identity = identity;
    if (!record.identity.HasStableIdentity() ||
        record.identity.kind == ObjectKind::Unknown ||
        record.identity.rawcode == 0u || record.identity.jHandle == 0u) {
      RenderObjectIdentitySnapshot cachedIdentity = {};
      if (RenderQueueTracker::instance().GetCachedObjectIdentity(
              record.payload, cachedIdentity)) {
        if (record.identity.worldObjectEntry == nullptr)
          record.identity.worldObjectEntry = cachedIdentity.worldObjectEntry;
        if (record.identity.sceneNode == nullptr)
          record.identity.sceneNode = cachedIdentity.sceneNode;
        if (record.identity.unitPtr == nullptr)
          record.identity.unitPtr = cachedIdentity.unitPtr;
        if (record.identity.agentPtr == nullptr)
          record.identity.agentPtr = cachedIdentity.agentPtr;
        if (record.identity.handleId == 0u)
          record.identity.handleId = cachedIdentity.handleId;
        if (record.identity.jHandle == 0u)
          record.identity.jHandle = cachedIdentity.jHandle;
        if (record.identity.rawcode == 0u)
          record.identity.rawcode = cachedIdentity.rawcode;
        if (record.identity.agentType == 0u)
          record.identity.agentType = cachedIdentity.agentType;
        if (record.identity.flags5C == 0u)
          record.identity.flags5C = cachedIdentity.flags5C;
        if (record.identity.kind == ObjectKind::Unknown)
          record.identity.kind = cachedIdentity.kind;
        if (record.identity.groupIdx < 0)
          record.identity.groupIdx = cachedIdentity.groupIdx;
        if (record.sceneNode == nullptr)
          record.sceneNode = cachedIdentity.sceneNode;
      }
    }
    appendRecord(snap, record);
  }
}

bool VisibleRenderableRegistry::registerSemanticCandidate(
    const VisibleRenderableRecord &candidate) {
  if constexpr (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled) {
    return false;
  }
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites) {
    return false;
  }

  if (!candidate.HasStableIdentity() && candidate.sceneNode == nullptr &&
      candidate.renderablePart == nullptr && candidate.meshData == nullptr &&
      candidate.runtimeGeosetPtr == nullptr &&
      candidate.runtimeGeosetDataPtr == nullptr) {
    return false;
  }

  Snapshot &snap = writeSnapshot();
  auto canMergeVisibleSubpart =
      [](const VisibleRenderableRecord& existing,
         const VisibleRenderableRecord& incoming) {
        const bool sameRenderablePart =
            incoming.renderablePart != nullptr &&
            existing.renderablePart == incoming.renderablePart;
        const bool samePayload =
            incoming.payload != nullptr && existing.payload == incoming.payload;
        if (!sameRenderablePart && !samePayload)
          return true;

        // A single renderable part can emit several visible layers/subparts.
        // Merging those records collapses the current primitive slice and makes
        // the semantic shadow path fall back to the whole CGeosetData, including
        // hidden construction/material variants. Only merge when the visible
        // layer identity is compatible.
        if (existing.layerIndex != incoming.layerIndex ||
            existing.subIndex != incoming.subIndex) {
          return false;
        }
        if (existing.layerState != nullptr && incoming.layerState != nullptr &&
            existing.layerState != incoming.layerState) {
          return false;
        }
        return true;
      };
  auto mergeCandidateIntoExisting = [&](uint32_t index) {
    if (index >= snap.records.size())
      return false;
    VisibleRenderableRecord& existing = snap.records[index];
    if (!canMergeVisibleSubpart(existing, candidate))
      return false;
    if (existing.payload == nullptr && candidate.payload != nullptr) {
      existing.payload = candidate.payload;
    }
    if (existing.renderablePart == nullptr && candidate.renderablePart != nullptr) {
      existing.renderablePart = candidate.renderablePart;
    }
    if (existing.sceneNode == nullptr && candidate.sceneNode != nullptr) {
      existing.sceneNode = candidate.sceneNode;
    }
    if (existing.meshData == nullptr && candidate.meshData != nullptr) {
      existing.meshData = candidate.meshData;
    }
    if (existing.layerState == nullptr && candidate.layerState != nullptr) {
      existing.layerState = candidate.layerState;
    }
    if (existing.runtimeModelPtr == nullptr && candidate.runtimeModelPtr != nullptr) {
      existing.runtimeModelPtr = candidate.runtimeModelPtr;
    }
    if (existing.modelResourcePtr == nullptr &&
        candidate.modelResourcePtr != nullptr) {
      existing.modelResourcePtr = candidate.modelResourcePtr;
    }
    if (existing.runtimeGeosetPtr == nullptr &&
        candidate.runtimeGeosetPtr != nullptr) {
      existing.runtimeGeosetPtr = candidate.runtimeGeosetPtr;
    }
    if (existing.runtimeGeosetDataPtr == nullptr &&
        candidate.runtimeGeosetDataPtr != nullptr) {
      existing.runtimeGeosetDataPtr = candidate.runtimeGeosetDataPtr;
    }
    if (existing.modelKey == 0u && candidate.modelKey != 0u) {
      existing.modelKey = candidate.modelKey;
    }
    if (existing.meshIndex == kInvalidVisibleMeshIndex &&
        candidate.meshIndex != kInvalidVisibleMeshIndex) {
      existing.meshIndex = candidate.meshIndex;
    }
    if (existing.geosetIndex == kInvalidVisibleMeshIndex &&
        candidate.geosetIndex != kInvalidVisibleMeshIndex) {
      existing.geosetIndex = candidate.geosetIndex;
    }
    if (existing.layerIndex == 0u && candidate.layerIndex != 0u)
      existing.layerIndex = candidate.layerIndex;
    if (existing.subIndex == 0u && candidate.subIndex != 0u)
      existing.subIndex = candidate.subIndex;
    if (existing.identity.worldObjectEntry == nullptr)
      existing.identity.worldObjectEntry = candidate.identity.worldObjectEntry;
    if (existing.identity.sceneNode == nullptr)
      existing.identity.sceneNode = candidate.identity.sceneNode;
    if (existing.identity.unitPtr == nullptr)
      existing.identity.unitPtr = candidate.identity.unitPtr;
    if (existing.identity.agentPtr == nullptr)
      existing.identity.agentPtr = candidate.identity.agentPtr;
    if (existing.identity.handleId == 0u)
      existing.identity.handleId = candidate.identity.handleId;
    if (existing.identity.jHandle == 0u)
      existing.identity.jHandle = candidate.identity.jHandle;
    if (existing.identity.rawcode == 0u)
      existing.identity.rawcode = candidate.identity.rawcode;
    if (existing.identity.agentType == 0u)
      existing.identity.agentType = candidate.identity.agentType;
    if (existing.identity.flags5C == 0u)
      existing.identity.flags5C = candidate.identity.flags5C;
    if (existing.identity.kind == ObjectKind::Unknown)
      existing.identity.kind = candidate.identity.kind;
    if (existing.identity.groupIdx < 0)
      existing.identity.groupIdx = candidate.identity.groupIdx;

    if constexpr (!dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild ||
                  dxvk::war3::internal::
                      kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
      if (existing.payload != nullptr)
        snap.byPayload[existing.payload] = index;
      if (existing.renderablePart != nullptr)
        snap.byRenderablePart[existing.renderablePart] = index;
      if (existing.identity.worldObjectEntry != nullptr)
        snap.byWorldObjectEntry[existing.identity.worldObjectEntry] = index;
      if (existing.identity.jHandle != 0u)
        snap.byHandle[existing.identity.jHandle] = index;
      if (existing.sceneNode != nullptr)
        snap.bySceneNode[existing.sceneNode] = index;
      if (existing.meshData != nullptr)
        snap.byMeshData[existing.meshData] = index;
      if (existing.runtimeModelPtr != nullptr)
        snap.byRuntimeModel[existing.runtimeModelPtr] = index;
      if (existing.runtimeGeosetPtr != nullptr)
        snap.byRuntimeGeoset[existing.runtimeGeosetPtr] = index;
      if (existing.runtimeGeosetDataPtr != nullptr)
        snap.byRuntimeGeosetData[existing.runtimeGeosetDataPtr] = index;
    }
    return true;
  };
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild &&
                !dxvk::war3::internal::
                    kWar3RuntimeConfigMaintainSemanticVisibleHotLookupIndexes) {
    for (uint32_t index = 0u; index < snap.records.size(); ++index) {
      const auto& existing = snap.records[index];
      if (candidate.renderablePart != nullptr &&
          existing.renderablePart == candidate.renderablePart) {
        if (mergeCandidateIntoExisting(index))
          return true;
        continue;
      }
      if (candidate.payload != nullptr && existing.payload == candidate.payload) {
        if (mergeCandidateIntoExisting(index))
          return true;
        continue;
      }
      if (candidate.renderablePart == nullptr && candidate.payload == nullptr &&
          candidate.runtimeGeosetPtr != nullptr &&
          existing.runtimeGeosetPtr == candidate.runtimeGeosetPtr) {
        if (mergeCandidateIntoExisting(index))
          return true;
        continue;
      }
      if (candidate.renderablePart == nullptr && candidate.payload == nullptr &&
          candidate.runtimeGeosetDataPtr != nullptr &&
          existing.runtimeGeosetDataPtr == candidate.runtimeGeosetDataPtr) {
        if (mergeCandidateIntoExisting(index))
          return true;
        continue;
      }
    }
  } else {
    if (candidate.renderablePart != nullptr &&
        snap.byRenderablePart.find(candidate.renderablePart) !=
            snap.byRenderablePart.end()) {
      if (mergeCandidateIntoExisting(
              snap.byRenderablePart[candidate.renderablePart])) {
        return true;
      }
    }
    if (candidate.payload != nullptr &&
        snap.byPayload.find(candidate.payload) != snap.byPayload.end()) {
      if (mergeCandidateIntoExisting(snap.byPayload[candidate.payload]))
        return true;
    }
    if (candidate.renderablePart == nullptr && candidate.payload == nullptr &&
        candidate.runtimeGeosetPtr != nullptr &&
        snap.byRuntimeGeoset.find(candidate.runtimeGeosetPtr) !=
            snap.byRuntimeGeoset.end()) {
      mergeCandidateIntoExisting(snap.byRuntimeGeoset[candidate.runtimeGeosetPtr]);
      return true;
    }
    if (candidate.renderablePart == nullptr && candidate.payload == nullptr &&
        candidate.runtimeGeosetDataPtr != nullptr &&
        snap.byRuntimeGeosetData.find(candidate.runtimeGeosetDataPtr) !=
            snap.byRuntimeGeosetData.end()) {
      mergeCandidateIntoExisting(
          snap.byRuntimeGeosetData[candidate.runtimeGeosetDataPtr]);
      return true;
    }
  }

  VisibleRenderableRecord record = candidate;
  record.queueKind = VisibleRenderableQueueKind::MainQueue;
  appendRecord(snap, record);
  return true;
}

void VisibleRenderableRegistry::registerTransparentEntry(
    void *payload, uint32_t transparentType, uint32_t queueSlot,
    uint32_t sortKey, float distanceSq,
    const RenderObjectIdentitySnapshot &identity) {
  if constexpr (!dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled) {
    return;
  }
  if constexpr (dxvk::war3::internal::
                    kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites) {
    return;
  }

  if (payload == nullptr && !identity.HasContext())
    return;

  Snapshot &snap = writeSnapshot();
  snap.records.reserve(snap.records.size() + 1u);
  if constexpr (!dxvk::war3::internal::
                    kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
    snap.byPayload.reserve(snap.byPayload.size() + 1u);
    snap.byRenderablePart.reserve(snap.byRenderablePart.size() + 1u);
    snap.byWorldObjectEntry.reserve(snap.byWorldObjectEntry.size() + 1u);
    snap.byHandle.reserve(snap.byHandle.size() + 1u);
    snap.bySceneNode.reserve(snap.bySceneNode.size() + 1u);
    snap.byMeshData.reserve(snap.byMeshData.size() + 1u);
    snap.byRuntimeModel.reserve(snap.byRuntimeModel.size() + 1u);
    snap.byRuntimeGeoset.reserve(snap.byRuntimeGeoset.size() + 1u);
    snap.byRuntimeGeosetData.reserve(snap.byRuntimeGeosetData.size() + 1u);
  }

  VisibleRenderableRecord record = {};
  record.queueKind = VisibleRenderableQueueKind::Transparent;
  record.payload = payload;
  record.queueSlot = queueSlot;
  record.transparentType = transparentType;
  record.transparentSortKey = sortKey;
  record.transparentDistanceSq = distanceSq;
  record.identity = identity;

  TryResolveRenderablePartFromTransparentPayload(payload, transparentType,
                                                 identity, record.renderablePart,
                                                 record.sceneNode,
                                                 record.meshData);
  appendRecord(snap, record);
}

bool VisibleRenderableRegistry::queryByPayload(void *payload,
                                               VisibleRenderableRecord &out) const {
  out = {};
  if (!payload)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byPayload.find(payload);
  if (it == snap.byPayload.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.payload == payload) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByRenderablePart(
    void *renderablePart, VisibleRenderableRecord &out) const {
  out = {};
  if (!renderablePart)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byRenderablePart.find(renderablePart);
  if (it == snap.byRenderablePart.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.renderablePart == renderablePart) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByWorldObjectEntry(
    void *worldObjectEntry, VisibleRenderableRecord &out) const {
  out = {};
  if (!worldObjectEntry)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byWorldObjectEntry.find(worldObjectEntry);
  if (it == snap.byWorldObjectEntry.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.identity.worldObjectEntry == worldObjectEntry) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByHandle(uint32_t jHandle,
                                              VisibleRenderableRecord &out) const {
  out = {};
  if (jHandle == 0u)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byHandle.find(jHandle);
  if (it == snap.byHandle.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.identity.jHandle == jHandle) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryBySceneNode(void *sceneNode,
                                                 VisibleRenderableRecord &out) const {
  out = {};
  if (!sceneNode)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.bySceneNode.find(sceneNode);
  if (it == snap.bySceneNode.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.sceneNode == sceneNode ||
            record.identity.sceneNode == sceneNode) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

bool VisibleRenderableRegistry::queryByRuntimeModel(
    void *runtimeModelPtr, VisibleRenderableRecord &out) const {
  out = {};
  if (!runtimeModelPtr)
    return false;

  const Snapshot &snap = snapshotForThread();
  const auto it = snap.byRuntimeModel.find(runtimeModelPtr);
  if (it == snap.byRuntimeModel.end() || it->second >= snap.records.size()) {
    if constexpr (dxvk::war3::internal::
                      kWar3RuntimeConfigDeferSemanticVisibleIndexBuild) {
      for (const auto& record : snap.records) {
        if (record.runtimeModelPtr == runtimeModelPtr) {
          out = record;
          return true;
        }
      }
    }
    return false;
  }

  out = snap.records[it->second];
  return true;
}

const std::vector<VisibleRenderableRecord>&
VisibleRenderableRegistry::getAllVisibleView() const {
  return snapshotForThread().records;
}

std::vector<VisibleRenderableRecord> VisibleRenderableRegistry::getAllVisible() const {
  return getAllVisibleView();
}

size_t VisibleRenderableRegistry::getVisibleCount() const {
  return snapshotForThread().records.size();
}

size_t VisibleRenderableRegistry::getMainQueueCount() const {
  return snapshotForThread().mainQueueCount;
}

size_t VisibleRenderableRegistry::getTransparentCount() const {
  return snapshotForThread().transparentCount;
}

} // namespace dxvk::war3::render
