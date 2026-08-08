// war3_shadow_object_registry.cpp - 闃村奖瀵硅薄杩愯鏃堕鏋跺疄鐜?

#include "war3_shadow_object_registry.h"

#include "../core/war3_memory.h"
#include "../game/war3_unit.h"
#include "../model/war3_model_registry.h"

namespace dxvk {
namespace war3 {
namespace render {

namespace {
template <typename Map>
void ClearRegistryMap(Map& map) {
  Map empty;
  map.swap(empty);
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
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
                      ownedHandlePtr) &&
      ownedHandlePtr != nullptr;
  const bool hasRuntimeGeosets =
      SafeReadU32Fast(candidate, dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                      runtimeGeosetCount) &&
      runtimeGeosetCount < 4096u &&
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::RuntimeGeosets,
                      runtimeGeosets) &&
      runtimeGeosets != nullptr;
  const bool hasFinalPoseArray =
      SafeReadU32Fast(candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
                      finalPoseMatrixCount) &&
      finalPoseMatrixCount <= 256u &&
      SafeReadPtrFast(candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
                      finalPoseMatrixArray) &&
      finalPoseMatrixArray != nullptr;
  return hasOwnedHandle || hasRuntimeGeosets || hasFinalPoseArray;
}

void MergeShadowRecord(ShadowObjectRecord &dst,
                       const ShadowObjectRecord &src) {
  if (src.worldObjectEntry)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (src.sceneNode)
    dst.sceneNode = src.sceneNode;
  if (src.unitPtr)
    dst.unitPtr = src.unitPtr;
  if (src.spritePtr)
    dst.spritePtr = src.spritePtr;
  if (src.runtimeModelPtr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (src.modelResourcePtr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (!src.modelPath.empty())
    dst.modelPath = src.modelPath;
  if (src.jHandle != 0)
    dst.jHandle = src.jHandle;
  if (src.rawcode != 0)
    dst.rawcode = src.rawcode;
  if (src.kind != ObjectKind::Unknown)
    dst.kind = src.kind;
  if (src.modelKey != 0)
    dst.modelKey = src.modelKey;
  if (src.modelType != 0)
    dst.modelType = src.modelType;
  if (src.modelFlags != 0)
    dst.modelFlags = src.modelFlags;
  if (src.sequenceId != 0)
    dst.sequenceId = src.sequenceId;
  if (src.sequenceTime != 0.0f)
    dst.sequenceTime = src.sequenceTime;
  if (src.scale != 1.0f || dst.scale == 0.0f)
    dst.scale = src.scale;
  if (src.yaw != 0.0f)
    dst.yaw = src.yaw;
  if (src.pitch != 0.0f)
    dst.pitch = src.pitch;
  if (src.roll != 0.0f)
    dst.roll = src.roll;
  if (src.height != 0.0f)
    dst.height = src.height;
  if (src.hasWorldTransform) {
    dst.hasWorldTransform = true;
    dst.worldTransform = src.worldTransform;
  }
  if (src.hasSpriteFrameTransform) {
    dst.hasSpriteFrameTransform = true;
    dst.spriteFrameTransform = src.spriteFrameTransform;
  }
  if (src.spriteFrameDt != 0.0f)
    dst.spriteFrameDt = src.spriteFrameDt;
  if (src.matrixCount != 0) {
    dst.matrixCount = src.matrixCount;
    dst.matrixHash = src.matrixHash;
  }
  if (src.lastRootPoseFrame != 0)
    dst.lastRootPoseFrame = src.lastRootPoseFrame;
  if (src.lastSpriteFramePoseFrame != 0)
    dst.lastSpriteFramePoseFrame = src.lastSpriteFramePoseFrame;
  if (src.lastMatrixPaletteFrame != 0)
    dst.lastMatrixPaletteFrame = src.lastMatrixPaletteFrame;
  if (src.spriteFrameSampleCount != 0)
    dst.spriteFrameSampleCount = src.spriteFrameSampleCount;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
  if (src.lastSceneCollectorBatchFrame >
      dst.lastSceneCollectorBatchFrame) {
    dst.lastSceneCollectorBatchFrame = src.lastSceneCollectorBatchFrame;
  }
}

void ProjectShadowObjectAugmentView(const ShadowObjectRecord& src,
                                    ShadowObjectAugmentView& out) {
  out.worldObjectEntry = src.worldObjectEntry;
  out.sceneNode = src.sceneNode;
  out.runtimeModelPtr = src.runtimeModelPtr;
  out.modelResourcePtr = src.modelResourcePtr;
  out.jHandle = src.jHandle;
  out.rawcode = src.rawcode;
  out.kind = src.kind;
  out.modelKey = src.modelKey;
  out.scale = src.scale;
  out.height = src.height;
  out.hasWorldTransform = src.hasWorldTransform;
  out.worldTransform = src.worldTransform;
  out.hasSpriteFrameTransform = src.hasSpriteFrameTransform;
  out.spriteFrameTransform = src.spriteFrameTransform;
  out.matrixCount = src.matrixCount;
  out.matrixHash = src.matrixHash;
  out.lastRootPoseFrame = src.lastRootPoseFrame;
  out.lastSpriteFramePoseFrame = src.lastSpriteFramePoseFrame;
  out.lastMatrixPaletteFrame = src.lastMatrixPaletteFrame;
}

bool HasSameShadowObjectPayloadExact(const ShadowObjectRecord& a,
                                     const ShadowObjectRecord& b) {
  return a.worldObjectEntry == b.worldObjectEntry &&
         a.sceneNode == b.sceneNode && a.unitPtr == b.unitPtr &&
         a.spritePtr == b.spritePtr &&
         a.runtimeModelPtr == b.runtimeModelPtr &&
         a.modelResourcePtr == b.modelResourcePtr &&
         a.modelPath == b.modelPath && a.jHandle == b.jHandle &&
         a.rawcode == b.rawcode && a.kind == b.kind &&
         a.modelKey == b.modelKey && a.modelType == b.modelType &&
         a.modelFlags == b.modelFlags && a.sequenceId == b.sequenceId &&
         a.sequenceTime == b.sequenceTime && a.scale == b.scale &&
         a.yaw == b.yaw && a.pitch == b.pitch && a.roll == b.roll &&
         a.height == b.height &&
         a.hasWorldTransform == b.hasWorldTransform &&
         a.worldTransform == b.worldTransform &&
         a.hasSpriteFrameTransform == b.hasSpriteFrameTransform &&
         a.spriteFrameTransform == b.spriteFrameTransform &&
         a.spriteFrameDt == b.spriteFrameDt &&
         a.matrixCount == b.matrixCount && a.matrixHash == b.matrixHash &&
         a.lastRootPoseFrame == b.lastRootPoseFrame &&
         a.lastSpriteFramePoseFrame == b.lastSpriteFramePoseFrame &&
         a.lastMatrixPaletteFrame == b.lastMatrixPaletteFrame &&
         a.spriteFrameSampleCount == b.spriteFrameSampleCount &&
         a.firstSeenFrame == b.firstSeenFrame;
}

void* TryReadRuntimeModelFromSprite(void* spritePtr) {
  void* runtimeModelPtr = nullptr;
  if (!spritePtr)
    return nullptr;
  if (!SafeReadPtrFast(spritePtr, 0x20, runtimeModelPtr) ||
      !LooksLikeRuntimeModelPtr(runtimeModelPtr)) {
    return nullptr;
  }
  return runtimeModelPtr;
}

class RegistryMutationGenerationGuard {
public:
  explicit RegistryMutationGenerationGuard(
      std::atomic<uint64_t>& generation) noexcept
  : m_generation(generation) {
    m_generation.fetch_add(1u, std::memory_order_acq_rel);
  }

  ~RegistryMutationGenerationGuard() {
    m_generation.fetch_add(1u, std::memory_order_release);
  }

  RegistryMutationGenerationGuard(
      const RegistryMutationGenerationGuard&) = delete;
  RegistryMutationGenerationGuard& operator=(
      const RegistryMutationGenerationGuard&) = delete;

private:
  std::atomic<uint64_t>& m_generation;
};
} // namespace

ShadowObjectRegistry &ShadowObjectRegistry::instance() {
  static ShadowObjectRegistry s_instance;
  return s_instance;
}

void ShadowObjectRegistry::beginFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  m_frameNumber.fetch_add(1u, std::memory_order_relaxed);
}

void ShadowObjectRegistry::endFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
}

void ShadowObjectRegistry::resetMapSession() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  ClearRegistryMap(m_byWorldObjectEntry);
  ClearRegistryMap(m_bySceneNode);
  ClearRegistryMap(m_byUnitPtr);
  ClearRegistryMap(m_bySpritePtr);
  ClearRegistryMap(m_byRuntimeModel);
  ClearRegistryMap(m_byHandle);
  m_sameFrameIdentityDedupStats = {};
  m_lastSceneCollectorBatchFrame = 0u;
  m_frameNumber.fetch_add(1u, std::memory_order_relaxed);
}

void ShadowObjectRegistry::storeRecord(const ShadowObjectRecord &record) {
  ShadowObjectRecord merged = {};
  if (record.worldObjectEntry) {
    auto it = m_byWorldObjectEntry.find(record.worldObjectEntry);
    if (it != m_byWorldObjectEntry.end())
      MergeShadowRecord(merged, it->second);
  }
  if (record.sceneNode) {
    auto it = m_bySceneNode.find(record.sceneNode);
    if (it != m_bySceneNode.end())
      MergeShadowRecord(merged, it->second);
  }
  if (record.unitPtr) {
    auto it = m_byUnitPtr.find(record.unitPtr);
    if (it != m_byUnitPtr.end())
      MergeShadowRecord(merged, it->second);
  }
  if (record.spritePtr) {
    auto it = m_bySpritePtr.find(record.spritePtr);
    if (it != m_bySpritePtr.end())
      MergeShadowRecord(merged, it->second);
  }
  if (record.runtimeModelPtr) {
    auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      MergeShadowRecord(merged, it->second);
  }
  if (record.jHandle != 0) {
    auto it = m_byHandle.find(record.jHandle);
    if (it != m_byHandle.end())
      MergeShadowRecord(merged, it->second);
  }
  MergeShadowRecord(merged, record);

  if (!merged.worldObjectEntry && !merged.sceneNode && !merged.unitPtr &&
      !merged.spritePtr && !merged.runtimeModelPtr &&
      merged.jHandle == 0u) {
    return;
  }

  // Invalidate value caches before the first lookup-visible map assignment.
  // Readers that already observed the previous generation may finish using
  // their private value copy, while later readers must reacquire m_mutex.
  RegistryMutationGenerationGuard mutation(m_mutationGeneration);
  if (merged.worldObjectEntry)
    m_byWorldObjectEntry[merged.worldObjectEntry] = merged;
  if (merged.sceneNode)
    m_bySceneNode[merged.sceneNode] = merged;
  if (merged.unitPtr)
    m_byUnitPtr[merged.unitPtr] = merged;
  if (merged.spritePtr)
    m_bySpritePtr[merged.spritePtr] = merged;
  if (merged.runtimeModelPtr)
    m_byRuntimeModel[merged.runtimeModelPtr] = merged;
  if (merged.jHandle != 0)
    m_byHandle[merged.jHandle] = merged;
}

bool ShadowObjectRegistry::trySkipExactSameFrameRenderObjectLocked(
    const RenderObjectInfo& info, uint64_t currentFrame) {
  auto& stats = m_sameFrameIdentityDedupStats;
  ++stats.attempts;
  const auto fail = [](uint64_t& counter) {
    ++counter;
    return false;
  };

  if (info.worldObjectEntry == nullptr || info.sceneNode == nullptr)
    return fail(stats.missIncomplete);

  const auto itScene = m_bySceneNode.find(info.sceneNode);
  if (itScene == m_bySceneNode.end())
    return fail(stats.missMissingAlias);

  const ShadowObjectRecord& canonical = itScene->second;
  if (canonical.lastSeenFrame != currentFrame)
    return fail(stats.missCrossFrame);
  if (canonical.lastSceneCollectorBatchFrame != currentFrame)
    return fail(stats.missNoBatchProof);

  if (canonical.worldObjectEntry != info.worldObjectEntry ||
      canonical.sceneNode != info.sceneNode ||
      (info.unitPtr != nullptr && canonical.unitPtr != info.unitPtr) ||
      (info.jHandle != 0u && canonical.jHandle != info.jHandle) ||
      (info.rawcode != 0u && canonical.rawcode != info.rawcode) ||
      (info.kind != ObjectKind::Unknown && canonical.kind != info.kind)) {
    return fail(stats.missInputMismatch);
  }

  if (canonical.firstSeenFrame == 0u ||
      canonical.worldObjectEntry == nullptr || canonical.sceneNode == nullptr ||
      canonical.unitPtr == nullptr || canonical.spritePtr == nullptr ||
      canonical.runtimeModelPtr == nullptr || canonical.modelKey == 0u ||
      canonical.modelResourcePtr == nullptr || canonical.modelPath.empty()) {
    return fail(stats.missIncomplete);
  }

  enum : uint32_t {
    kAliasClean = 0u,
    kAliasMissing = 1u,
    kAliasCrossFrame = 2u,
    kAliasNoBatchProof = 3u,
    kAliasConflict = 4u,
  };
  const auto validateAlias = [&](const auto& map,
                                 const auto& key) -> uint32_t {
    const auto it = map.find(key);
    if (it == map.end())
      return kAliasMissing;
    const ShadowObjectRecord& alias = it->second;
    if (alias.lastSeenFrame != currentFrame)
      return kAliasCrossFrame;
    if (alias.lastSceneCollectorBatchFrame != currentFrame)
      return kAliasNoBatchProof;
    if (!HasSameShadowObjectPayloadExact(canonical, alias))
      return kAliasConflict;
    return kAliasClean;
  };
  const auto failAlias = [&](uint32_t result) {
    if (result == kAliasMissing)
      return fail(stats.missMissingAlias);
    if (result == kAliasCrossFrame)
      return fail(stats.missCrossFrame);
    if (result == kAliasNoBatchProof)
      return fail(stats.missNoBatchProof);
    return fail(stats.missAliasConflict);
  };

  uint32_t aliasResult =
      validateAlias(m_byWorldObjectEntry, canonical.worldObjectEntry);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  aliasResult = validateAlias(m_byUnitPtr, canonical.unitPtr);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  aliasResult = validateAlias(m_bySpritePtr, canonical.spritePtr);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  aliasResult = validateAlias(m_byRuntimeModel, canonical.runtimeModelPtr);
  if (aliasResult != kAliasClean)
    return failAlias(aliasResult);
  if (canonical.jHandle != 0u) {
    aliasResult = validateAlias(m_byHandle, canonical.jHandle);
    if (aliasResult != kAliasClean)
      return failAlias(aliasResult);
  }

  ++stats.hits;
  return true;
}

void ShadowObjectRegistry::noteRenderObject(const RenderObjectInfo &info) {
  if (!info.worldObjectEntry && !info.sceneNode && !info.unitPtr &&
      !info.hasValidHandle())
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  const uint64_t currentFrame =
      m_frameNumber.load(std::memory_order_relaxed);
  if (currentFrame != 0u && m_lastSceneCollectorBatchFrame == currentFrame &&
      trySkipExactSameFrameRenderObjectLocked(info, currentFrame)) {
    return;
  }
  noteInstanceIdentityLocked(info.worldObjectEntry, info.sceneNode, info.unitPtr,
                             nullptr, info.jHandle, info.rawcode, info.kind,
                             false);
}

void ShadowObjectRegistry::noteRenderObjectsBatch(
    const std::vector<const RenderObjectInfo *> &infos) {
  if (infos.empty())
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  for (const auto *info : infos) {
    if (info == nullptr)
      continue;
    if (!info->worldObjectEntry && !info->sceneNode && !info->unitPtr &&
        !info->hasValidHandle()) {
      continue;
    }
    noteInstanceIdentityLocked(info->worldObjectEntry, info->sceneNode,
                               info->unitPtr, nullptr, info->jHandle,
                               info->rawcode, info->kind, true);
  }
}

void ShadowObjectRegistry::noteInstanceIdentity(void *worldObjectEntry,
                                                void *sceneNode,
                                                void *unitPtr,
                                                void *spritePtr,
                                                uint32_t jHandle,
                                                uint32_t rawcode,
                                                ObjectKind kind) {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  noteInstanceIdentityLocked(worldObjectEntry, sceneNode, unitPtr, spritePtr,
                             jHandle, rawcode, kind, false);
}

void ShadowObjectRegistry::noteInstanceIdentityLocked(void *worldObjectEntry,
                                                      void *sceneNode,
                                                      void *unitPtr,
                                                      void *spritePtr,
                                                      uint32_t jHandle,
                                                      uint32_t rawcode,
                                                      ObjectKind kind,
                                                      bool sceneCollectorBatch) {
  if (!worldObjectEntry && !sceneNode && !unitPtr && !spritePtr &&
      jHandle == 0u && rawcode == 0u) {
    return;
  }

  const uint64_t currentFrame =
      m_frameNumber.load(std::memory_order_relaxed);
  ShadowObjectRecord record = {};
  if (sceneNode) {
    auto it = m_bySceneNode.find(sceneNode);
    if (it != m_bySceneNode.end())
      MergeShadowRecord(record, it->second);
  }
  if (unitPtr) {
    auto it = m_byUnitPtr.find(unitPtr);
    if (it != m_byUnitPtr.end())
      MergeShadowRecord(record, it->second);
  }
  if (spritePtr) {
    auto it = m_bySpritePtr.find(spritePtr);
    if (it != m_bySpritePtr.end())
      MergeShadowRecord(record, it->second);
  }
  if (jHandle != 0u) {
    auto it = m_byHandle.find(jHandle);
    if (it != m_byHandle.end())
      MergeShadowRecord(record, it->second);
  }

  record.worldObjectEntry = worldObjectEntry;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  if (spritePtr)
    record.spritePtr = spritePtr;
  if (record.unitPtr && record.spritePtr == nullptr) {
    game::UnitWrapper unit(record.unitPtr);
    if (unit.IsValid())
      record.spritePtr = unit.GetSprite();
  }
  if (jHandle != 0u)
    record.jHandle = jHandle;
  if (rawcode != 0u)
    record.rawcode = rawcode;
  if (kind != ObjectKind::Unknown)
    record.kind = kind;

  if (record.spritePtr && record.runtimeModelPtr == nullptr) {
    record.runtimeModelPtr = TryReadRuntimeModelFromSprite(record.spritePtr);
    if (record.runtimeModelPtr) {
      model::ModelRegistry::instance().recordRuntimeModelBinding(
          record.spritePtr, record.runtimeModelPtr, nullptr, 0u, 0u);
    }
  }
  if (record.spritePtr &&
      (record.runtimeModelPtr == nullptr || record.modelKey == 0 ||
       record.modelResourcePtr == nullptr || record.modelPath.empty())) {
    model::ModelResourceRecord modelRecord = {};
    if (model::ModelRegistry::instance().findBySprite(record.spritePtr,
                                                      modelRecord) ||
        model::ModelRegistry::instance().findByRuntimeModel(
            record.runtimeModelPtr, modelRecord)) {
      if (modelRecord.runtimeModelPtr)
        record.runtimeModelPtr = modelRecord.runtimeModelPtr;
      record.modelResourcePtr = modelRecord.modelResourcePtr;
      record.modelPath = modelRecord.modelPath;
      record.modelKey = modelRecord.modelKey;
      record.modelType = modelRecord.modelType;
      record.modelFlags = modelRecord.flags;
    }
  }

  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = currentFrame;
  record.lastSeenFrame = currentFrame;
  if (sceneCollectorBatch) {
    record.lastSceneCollectorBatchFrame = currentFrame;
    m_lastSceneCollectorBatchFrame = currentFrame;
    ++m_sameFrameIdentityDedupStats.batchMarked;
  }
  storeRecord(record);
}

void ShadowObjectRegistry::noteModelBinding(void *spritePtr,
                                            void *runtimeModelPtr,
                                            void *modelResourcePtr,
                                            const std::string &modelPath,
                                            uint32_t modelType,
                                            uint32_t modelFlags,
                                            uint64_t modelKey) {
  if (!spritePtr || (!runtimeModelPtr && modelKey == 0 && modelPath.empty()))
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ShadowObjectRecord record = {};
  auto itSprite = m_bySpritePtr.find(spritePtr);
  if (itSprite != m_bySpritePtr.end())
    MergeShadowRecord(record, itSprite->second);
  if (runtimeModelPtr) {
    auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
    if (itRuntime != m_byRuntimeModel.end())
      MergeShadowRecord(record, itRuntime->second);
  }

  model::ModelInstanceRecord instanceRecord = {};
  if (model::ModelInstanceRegistry::instance().findBySpritePtr(spritePtr,
                                                               instanceRecord)) {
    record.worldObjectEntry = instanceRecord.worldObjectEntry;
    record.sceneNode = instanceRecord.sceneNode;
    record.unitPtr = instanceRecord.unitPtr;
    record.jHandle = instanceRecord.jHandle;
    record.rawcode = instanceRecord.rawcode;
  }

  record.spritePtr = spritePtr;
  if (runtimeModelPtr)
    record.runtimeModelPtr = runtimeModelPtr;
  if (modelResourcePtr)
    record.modelResourcePtr = modelResourcePtr;
  if (!modelPath.empty())
    record.modelPath = modelPath;
  if (modelKey != 0)
    record.modelKey = modelKey;
  if (modelType != 0)
    record.modelType = modelType;
  if (modelFlags != 0)
    record.modelFlags = modelFlags;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

void ShadowObjectRegistry::notePose(void *runtimeModelPtr, void *sceneNode,
                                    void *unitPtr, uint32_t sequenceId,
                                    float sequenceTime, float scale, float yaw,
                                    float pitch, float roll, float height,
                                    bool hasWorldTransform,
                                    const Matrix4 *worldTransform,
                                    uint32_t matrixCount, uint64_t matrixHash) {
  if (!runtimeModelPtr && !sceneNode && !unitPtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ShadowObjectRecord record = {};
  if (runtimeModelPtr) {
    auto it = m_byRuntimeModel.find(runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      MergeShadowRecord(record, it->second);
    else {
      model::ModelResourceRecord modelRecord = {};
      if (model::ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                              modelRecord)) {
        record.runtimeModelPtr = runtimeModelPtr;
        record.spritePtr = modelRecord.spritePtr;
        record.modelResourcePtr = modelRecord.modelResourcePtr;
        record.modelPath = modelRecord.modelPath;
        record.modelKey = modelRecord.modelKey;
        record.modelType = modelRecord.modelType;
        record.modelFlags = modelRecord.flags;
      }
    }
  }

  record.runtimeModelPtr = runtimeModelPtr;
  if (sceneNode)
    record.sceneNode = sceneNode;
  if (unitPtr)
    record.unitPtr = unitPtr;
  record.sequenceId = sequenceId;
  record.sequenceTime = sequenceTime;
  record.scale = scale;
  record.yaw = yaw;
  record.pitch = pitch;
  record.roll = roll;
  record.height = height;
  if (hasWorldTransform && worldTransform != nullptr) {
    record.hasWorldTransform = true;
    record.worldTransform = *worldTransform;
  }
  record.matrixCount = matrixCount;
  record.matrixHash = matrixHash;
  record.lastRootPoseFrame = m_frameNumber.load(std::memory_order_relaxed);
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

void ShadowObjectRegistry::noteSpriteFramePose(
    void *runtimeModelPtr, void *spritePtr, void *sceneNode, void *unitPtr,
    float dt, uint32_t sequenceId, float sequenceTime, float scale, float yaw,
    float pitch, float roll, float height, bool hasWorldTransform,
    const Matrix4 *worldTransform, uint32_t matrixCount, uint64_t matrixHash) {
  if (!runtimeModelPtr && !spritePtr && !sceneNode && !unitPtr)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  ShadowObjectRecord record = {};
  if (runtimeModelPtr) {
    auto it = m_byRuntimeModel.find(runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      MergeShadowRecord(record, it->second);
  }
  if (spritePtr) {
    auto it = m_bySpritePtr.find(spritePtr);
    if (it != m_bySpritePtr.end())
      MergeShadowRecord(record, it->second);
  }

  record.runtimeModelPtr = runtimeModelPtr;
  if (spritePtr)
    record.spritePtr = spritePtr;
  if (sceneNode)
    record.sceneNode = sceneNode;
  if (unitPtr)
    record.unitPtr = unitPtr;
  record.sequenceId = sequenceId;
  record.sequenceTime = sequenceTime;
  record.scale = scale;
  record.yaw = yaw;
  record.pitch = pitch;
  record.roll = roll;
  record.height = height;
  record.spriteFrameDt = dt;
  if (hasWorldTransform && worldTransform != nullptr) {
    record.hasSpriteFrameTransform = true;
    record.spriteFrameTransform = *worldTransform;
  }
  if (matrixCount != 0) {
    record.matrixCount = matrixCount;
    record.matrixHash = matrixHash;
  }
  record.lastSpriteFramePoseFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.spriteFrameSampleCount = 1;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  record.lastSeenFrame = m_frameNumber.load(std::memory_order_relaxed);
  storeRecord(record);
}

bool ShadowObjectRegistry::findByWorldObjectEntry(void *worldObjectEntry,
                                                  ShadowObjectRecord &out) const {
  if (!worldObjectEntry)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byWorldObjectEntry.find(worldObjectEntry);
  if (it == m_byWorldObjectEntry.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowObjectRegistry::findBySceneNode(void *sceneNode,
                                           ShadowObjectRecord &out) const {
  if (!sceneNode)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowObjectRegistry::findByUnitPtr(void *unitPtr,
                                         ShadowObjectRecord &out) const {
  if (!unitPtr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowObjectRegistry::findByHandle(uint32_t jHandle,
                                        ShadowObjectRecord &out) const {
  if (jHandle == 0)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byHandle.find(jHandle);
  if (it == m_byHandle.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowObjectRegistry::findBySpritePtr(void *spritePtr,
                                           ShadowObjectRecord &out) const {
  if (!spritePtr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_bySpritePtr.find(spritePtr);
  if (it == m_bySpritePtr.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowObjectRegistry::findByRuntimeModel(void *runtimeModelPtr,
                                               ShadowObjectRecord &out) const {
  if (!runtimeModelPtr)
    return false;
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool ShadowObjectRegistry::findFirstForAugment(
    void* worldObjectEntry, void* sceneNode, void* primaryUnitPtr,
    void* secondaryUnitPtr, uint32_t jHandle, void* runtimeModelPtr,
    ShadowObjectRecord& out) const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);

  auto findPointer = [&](const auto& map, void* key) -> bool {
    if (key == nullptr)
      return false;
    const auto it = map.find(key);
    if (it == map.end())
      return false;
    out = it->second;
    return true;
  };

  if (findPointer(m_byWorldObjectEntry, worldObjectEntry) ||
      findPointer(m_bySceneNode, sceneNode) ||
      findPointer(m_byUnitPtr, primaryUnitPtr) ||
      (secondaryUnitPtr != primaryUnitPtr &&
       findPointer(m_byUnitPtr, secondaryUnitPtr))) {
    return true;
  }

  if (jHandle != 0u) {
    const auto handleIt = m_byHandle.find(jHandle);
    if (handleIt != m_byHandle.end()) {
      out = handleIt->second;
      return true;
    }
  }

  return findPointer(m_byRuntimeModel, runtimeModelPtr);
}

bool ShadowObjectRegistry::findFirstForAugmentView(
    void* worldObjectEntry, void* sceneNode, void* primaryUnitPtr,
    void* secondaryUnitPtr, uint32_t jHandle, void* runtimeModelPtr,
    ShadowObjectAugmentView& out,
    uint64_t* mutationGenerationOut,
    uint64_t* frameNumberOut) const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  if (mutationGenerationOut != nullptr) {
    *mutationGenerationOut =
        m_mutationGeneration.load(std::memory_order_acquire);
  }
  if (frameNumberOut != nullptr)
    *frameNumberOut = m_frameNumber.load(std::memory_order_relaxed);

  const ShadowObjectRecord* record = nullptr;
  auto findPointer = [&](const auto& map, void* key) -> bool {
    if (key == nullptr)
      return false;
    const auto it = map.find(key);
    if (it == map.end())
      return false;
    record = &it->second;
    return true;
  };

  if (!findPointer(m_byWorldObjectEntry, worldObjectEntry) &&
      !findPointer(m_bySceneNode, sceneNode) &&
      !findPointer(m_byUnitPtr, primaryUnitPtr) &&
      (secondaryUnitPtr == primaryUnitPtr ||
       !findPointer(m_byUnitPtr, secondaryUnitPtr))) {
    if (jHandle != 0u) {
      const auto handleIt = m_byHandle.find(jHandle);
      if (handleIt != m_byHandle.end())
        record = &handleIt->second;
    }
    if (record == nullptr)
      findPointer(m_byRuntimeModel, runtimeModelPtr);
  }

  if (record == nullptr)
    return false;
  ProjectShadowObjectAugmentView(*record, out);
  return true;
}

std::vector<ShadowObjectRecord> ShadowObjectRegistry::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<ShadowObjectRecord> out;
  out.reserve(m_bySceneNode.size() + m_bySpritePtr.size() + m_byRuntimeModel.size());
  for (const auto &it : m_bySceneNode)
    out.push_back(it.second);
  for (const auto &it : m_bySpritePtr)
    out.push_back(it.second);
  for (const auto &it : m_byRuntimeModel)
    out.push_back(it.second);
  return out;
}

size_t ShadowObjectRegistry::runtimeBoundCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& it : m_byRuntimeModel) {
    const auto& record = it.second;
    if (record.runtimeModelPtr != nullptr && record.spritePtr != nullptr)
      ++count;
  }
  return count;
}

size_t ShadowObjectRegistry::completeIdentityCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& it : m_byRuntimeModel) {
    const auto& record = it.second;
    if (record.runtimeModelPtr == nullptr || record.spritePtr == nullptr)
      continue;
    if (record.sceneNode != nullptr || record.unitPtr != nullptr ||
        record.jHandle != 0u || record.rawcode != 0u)
      ++count;
  }
  return count;
}

size_t ShadowObjectRegistry::poseReadyCount() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& it : m_byRuntimeModel) {
    const auto& record = it.second;
    if (record.lastSpriteFramePoseFrame != 0 ||
        record.lastRootPoseFrame != 0 ||
        record.lastMatrixPaletteFrame != 0) {
      ++count;
    }
  }
  return count;
}

ShadowIdentitySameFrameDedupStats
ShadowObjectRegistry::sameFrameIdentityDedupStats() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_sameFrameIdentityDedupStats;
}

uint64_t ShadowObjectRegistry::frameNumber() const {
  // Phase 7.83锛歛tomic 鐩存帴 load銆?
  return m_frameNumber.load(std::memory_order_relaxed);
}

uint64_t ShadowObjectRegistry::mutationGeneration() const {
  return m_mutationGeneration.load(std::memory_order_acquire);
}

} // namespace render
} // namespace war3
} // namespace dxvk
