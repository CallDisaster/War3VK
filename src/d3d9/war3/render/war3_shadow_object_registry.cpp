// war3_shadow_object_registry.cpp - 阴影对象运行时骨架实现

#include "war3_shadow_object_registry.h"

#include "../core/war3_memory.h"
#include "../game/war3_unit.h"
#include "../model/war3_model_registry.h"

namespace dxvk {
namespace war3 {
namespace render {

namespace {
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
} // namespace

ShadowObjectRegistry &ShadowObjectRegistry::instance() {
  static ShadowObjectRegistry s_instance;
  return s_instance;
}

void ShadowObjectRegistry::beginFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_frameNumber;
}

void ShadowObjectRegistry::endFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
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

void ShadowObjectRegistry::noteRenderObject(const RenderObjectInfo &info) {
  if (!info.worldObjectEntry && !info.sceneNode && !info.unitPtr &&
      !info.hasValidHandle())
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  noteInstanceIdentityLocked(info.worldObjectEntry, info.sceneNode, info.unitPtr,
                             nullptr, info.jHandle, info.rawcode, info.kind);
}

void ShadowObjectRegistry::noteRenderObjectsBatch(
    const std::vector<const RenderObjectInfo *> &infos) {
  if (infos.empty())
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  for (const auto *info : infos) {
    if (info == nullptr)
      continue;
    if (!info->worldObjectEntry && !info->sceneNode && !info->unitPtr &&
        !info->hasValidHandle()) {
      continue;
    }
    noteInstanceIdentityLocked(info->worldObjectEntry, info->sceneNode,
                               info->unitPtr, nullptr, info->jHandle,
                               info->rawcode, info->kind);
  }
}

void ShadowObjectRegistry::noteInstanceIdentity(void *worldObjectEntry,
                                                void *sceneNode,
                                                void *unitPtr,
                                                void *spritePtr,
                                                uint32_t jHandle,
                                                uint32_t rawcode,
                                                ObjectKind kind) {
  std::lock_guard<std::mutex> lock(m_mutex);
  noteInstanceIdentityLocked(worldObjectEntry, sceneNode, unitPtr, spritePtr,
                             jHandle, rawcode, kind);
}

void ShadowObjectRegistry::noteInstanceIdentityLocked(void *worldObjectEntry,
                                                      void *sceneNode,
                                                      void *unitPtr,
                                                      void *spritePtr,
                                                      uint32_t jHandle,
                                                      uint32_t rawcode,
                                                      ObjectKind kind) {
  if (!worldObjectEntry && !sceneNode && !unitPtr && !spritePtr &&
      jHandle == 0u && rawcode == 0u) {
    return;
  }

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
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
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

  std::lock_guard<std::mutex> lock(m_mutex);
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
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
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

  std::lock_guard<std::mutex> lock(m_mutex);
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
  record.lastRootPoseFrame = m_frameNumber;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

void ShadowObjectRegistry::noteSpriteFramePose(
    void *runtimeModelPtr, void *spritePtr, void *sceneNode, void *unitPtr,
    float dt, uint32_t sequenceId, float sequenceTime, float scale, float yaw,
    float pitch, float roll, float height, bool hasWorldTransform,
    const Matrix4 *worldTransform, uint32_t matrixCount, uint64_t matrixHash) {
  if (!runtimeModelPtr && !spritePtr && !sceneNode && !unitPtr)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
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
  record.lastSpriteFramePoseFrame = m_frameNumber;
  record.spriteFrameSampleCount = 1;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

bool ShadowObjectRegistry::findByWorldObjectEntry(void *worldObjectEntry,
                                                  ShadowObjectRecord &out) const {
  if (!worldObjectEntry)
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

std::vector<ShadowObjectRecord> ShadowObjectRegistry::snapshot() const {
  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& it : m_byRuntimeModel) {
    const auto& record = it.second;
    if (record.runtimeModelPtr != nullptr && record.spritePtr != nullptr)
      ++count;
  }
  return count;
}

size_t ShadowObjectRegistry::completeIdentityCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
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
  std::lock_guard<std::mutex> lock(m_mutex);
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

uint64_t ShadowObjectRegistry::frameNumber() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_frameNumber;
}

} // namespace render
} // namespace war3
} // namespace dxvk
