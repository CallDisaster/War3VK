// war3_model_registry.cpp - War3 模型资源/实例/姿态运行时骨架实现

#include "war3_model_registry.h"

#include "../game/war3_unit.h"
#include "../core/war3_memory.h"
#include "../render/war3_render_objects.h"

namespace dxvk {
namespace war3 {
namespace model {

namespace {
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
  uint64_t x = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
  return x ? x : 0xA5A5A5A5A5A5A5A5ull;
}

void* TryReadRuntimeModelFromSprite(void* spritePtr) {
  void* runtimeModelPtr = nullptr;
  if (!spritePtr)
    return nullptr;
  SafeReadPtrFast(spritePtr, 0x20, runtimeModelPtr);
  return runtimeModelPtr;
}

void MergeModelResourceRecord(ModelResourceRecord &dst,
                              const ModelResourceRecord &src) {
  if (src.spritePtr)
    dst.spritePtr = src.spritePtr;
  if (src.runtimeModelPtr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (src.modelResourcePtr)
    dst.modelResourcePtr = src.modelResourcePtr;
  if (!src.modelPath.empty())
    dst.modelPath = src.modelPath;
  if (src.modelType != 0)
    dst.modelType = src.modelType;
  if (src.flags != 0)
    dst.flags = src.flags;
  if (src.modelKey != 0)
    dst.modelKey = src.modelKey;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
}

void MergeModelInstanceRecord(ModelInstanceRecord &dst,
                              const ModelInstanceRecord &src) {
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
  if (src.jHandle != 0)
    dst.jHandle = src.jHandle;
  if (src.rawcode != 0)
    dst.rawcode = src.rawcode;
  if (src.modelKey != 0)
    dst.modelKey = src.modelKey;
  if (dst.firstSeenFrame == 0 ||
      (src.firstSeenFrame != 0 && src.firstSeenFrame < dst.firstSeenFrame)) {
    dst.firstSeenFrame = src.firstSeenFrame;
  }
  if (src.lastSeenFrame > dst.lastSeenFrame)
    dst.lastSeenFrame = src.lastSeenFrame;
}

void MergePoseRecord(PoseRecord &dst, const PoseRecord &src) {
  if (src.runtimeModelPtr)
    dst.runtimeModelPtr = src.runtimeModelPtr;
  if (src.sceneNode)
    dst.sceneNode = src.sceneNode;
  if (src.unitPtr)
    dst.unitPtr = src.unitPtr;
  if (src.spritePtr)
    dst.spritePtr = src.spritePtr;
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
    dst.matrixPalette = src.matrixPalette;
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
} // namespace

ModelRegistry &ModelRegistry::instance() {
  static ModelRegistry s_instance;
  return s_instance;
}

uint64_t ModelRegistry::HashModelKey(const std::string &path, uint32_t modelType,
                                     uint32_t flags) {
  uint64_t hash = Fnv1a64(path.data(), path.size());
  hash = CombineHash(hash, uint64_t(modelType));
  hash = CombineHash(hash, uint64_t(flags));
  return hash;
}

uint64_t ModelRegistry::HashRuntimeModelKey(void *modelResourcePtr,
                                            void *runtimeModelPtr,
                                            uint32_t modelType,
                                            uint32_t flags) {
  uint64_t hash = 1469598103934665603ull;
  hash = CombineHash(hash, reinterpret_cast<uintptr_t>(modelResourcePtr));
  hash = CombineHash(hash, reinterpret_cast<uintptr_t>(runtimeModelPtr));
  hash = CombineHash(hash, uint64_t(modelType));
  hash = CombineHash(hash, uint64_t(flags));
  return hash;
}

void ModelRegistry::beginFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_frameNumber;
}

void ModelRegistry::endFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto &it : m_bySprite)
    it.second.lastSeenFrame = m_frameNumber;
  for (auto &it : m_byRuntimeModel)
    it.second.lastSeenFrame = m_frameNumber;
  for (auto &it : m_byPath)
    it.second.lastSeenFrame = m_frameNumber;
}

void ModelRegistry::recordSpriteModelPath(void *spritePtr, const char *modelPath,
                                          uint32_t modelType, uint32_t flags) {
  if (!spritePtr || !modelPath || !modelPath[0])
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  ModelResourceRecord record = {};
  auto it = m_bySprite.find(spritePtr);
  if (it != m_bySprite.end())
    record = it->second;
  record.spritePtr = spritePtr;
  record.modelPath = modelPath;
  record.modelType = modelType;
  record.flags = flags;
  record.modelKey =
      record.modelResourcePtr != nullptr || record.runtimeModelPtr != nullptr
          ? HashRuntimeModelKey(record.modelResourcePtr, record.runtimeModelPtr,
                                record.modelType, record.flags)
          : HashModelKey(record.modelPath, record.modelType, record.flags);
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  m_bySprite[spritePtr] = record;
  if (record.runtimeModelPtr)
    m_byRuntimeModel[record.runtimeModelPtr] = record;
  m_byPath[record.modelPath] = record;
}

void ModelRegistry::recordRuntimeModelBinding(void *spritePtr,
                                              void *runtimeModelPtr,
                                              void *modelResourcePtr,
                                              uint32_t modelType,
                                              uint32_t flags) {
  if (!spritePtr || !runtimeModelPtr)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  ModelResourceRecord record = {};
  auto itSprite = m_bySprite.find(spritePtr);
  if (itSprite != m_bySprite.end())
    MergeModelResourceRecord(record, itSprite->second);
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelResourceRecord(record, itRuntime->second);

  record.spritePtr = spritePtr;
  record.runtimeModelPtr = runtimeModelPtr;
  if (modelResourcePtr)
    record.modelResourcePtr = modelResourcePtr;
  if (modelType != 0)
    record.modelType = modelType;
  if (flags != 0)
    record.flags = flags;
  record.modelKey = HashRuntimeModelKey(record.modelResourcePtr,
                                        record.runtimeModelPtr,
                                        record.modelType, record.flags);
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;

  m_bySprite[spritePtr] = record;
  m_byRuntimeModel[runtimeModelPtr] = record;
  if (!record.modelPath.empty())
    m_byPath[record.modelPath] = record;
}

bool ModelRegistry::findBySprite(void *spritePtr, ModelResourceRecord &out) const {
  if (!spritePtr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_bySprite.find(spritePtr);
  if (it == m_bySprite.end())
    return false;
  out = it->second;
  return true;
}

bool ModelRegistry::findByRuntimeModel(void *runtimeModelPtr,
                                       ModelResourceRecord &out) const {
  if (!runtimeModelPtr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool ModelRegistry::findByPath(const std::string &modelPath,
                               ModelResourceRecord &out) const {
  if (modelPath.empty())
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byPath.find(modelPath);
  if (it == m_byPath.end())
    return false;
  out = it->second;
  return true;
}

std::vector<ModelResourceRecord> ModelRegistry::snapshot() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<ModelResourceRecord> out;
  out.reserve(m_bySprite.size());
  for (const auto &it : m_bySprite)
    out.push_back(it.second);
  return out;
}

size_t ModelRegistry::recordCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

uint64_t ModelRegistry::frameNumber() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_frameNumber;
}

ModelInstanceRegistry &ModelInstanceRegistry::instance() {
  static ModelInstanceRegistry s_instance;
  return s_instance;
}

void ModelInstanceRegistry::beginFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_frameNumber;
}

void ModelInstanceRegistry::endFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto &it : m_bySceneNode)
    it.second.lastSeenFrame = m_frameNumber;
  for (auto &it : m_byUnitPtr)
    it.second.lastSeenFrame = m_frameNumber;
  for (auto &it : m_bySpritePtr)
    it.second.lastSeenFrame = m_frameNumber;
  for (auto &it : m_byRuntimeModel)
    it.second.lastSeenFrame = m_frameNumber;
  for (auto &it : m_byHandle)
    it.second.lastSeenFrame = m_frameNumber;
}

void ModelInstanceRegistry::storeRecord(const ModelInstanceRecord &record) {
  ModelInstanceRecord merged = {};
  if (record.sceneNode) {
    auto it = m_bySceneNode.find(record.sceneNode);
    if (it != m_bySceneNode.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.unitPtr) {
    auto it = m_byUnitPtr.find(record.unitPtr);
    if (it != m_byUnitPtr.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.spritePtr) {
    auto it = m_bySpritePtr.find(record.spritePtr);
    if (it != m_bySpritePtr.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.runtimeModelPtr) {
    auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  if (record.jHandle != 0) {
    auto it = m_byHandle.find(record.jHandle);
    if (it != m_byHandle.end())
      MergeModelInstanceRecord(merged, it->second);
  }
  MergeModelInstanceRecord(merged, record);

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

void ModelInstanceRegistry::noteRenderObject(const render::RenderObjectInfo &info) {
  if (!info.worldObjectEntry && !info.sceneNode && !info.unitPtr &&
      !info.hasValidHandle())
    return;

  noteInstanceIdentity(info.worldObjectEntry, info.sceneNode, info.unitPtr,
                       nullptr, info.jHandle, info.rawcode);
}

void ModelInstanceRegistry::noteInstanceIdentity(void *worldObjectEntry,
                                                 void *sceneNode,
                                                 void *unitPtr,
                                                 void *spritePtr,
                                                 uint32_t jHandle,
                                                 uint32_t rawcode) {
  if (!worldObjectEntry && !sceneNode && !unitPtr && !spritePtr &&
      jHandle == 0u && rawcode == 0u) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  record.worldObjectEntry = worldObjectEntry;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  record.spritePtr = spritePtr;
  if (record.unitPtr) {
    game::UnitWrapper unit(record.unitPtr);
    if (unit.IsValid() && record.spritePtr == nullptr)
      record.spritePtr = unit.GetSprite();
  }
  record.jHandle = jHandle;
  record.rawcode = rawcode;
  if (record.spritePtr) {
    record.runtimeModelPtr = TryReadRuntimeModelFromSprite(record.spritePtr);
    if (record.runtimeModelPtr) {
      ModelRegistry::instance().recordRuntimeModelBinding(
          record.spritePtr, record.runtimeModelPtr, nullptr, 0u, 0u);
    }
    ModelResourceRecord modelRecord = {};
    if (ModelRegistry::instance().findBySprite(record.spritePtr, modelRecord) ||
        ModelRegistry::instance().findByRuntimeModel(record.runtimeModelPtr,
                                                     modelRecord)) {
      record.modelKey = modelRecord.modelKey;
      if (modelRecord.runtimeModelPtr)
        record.runtimeModelPtr = modelRecord.runtimeModelPtr;
      record.modelResourcePtr = modelRecord.modelResourcePtr;
    }
  }
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

void ModelInstanceRegistry::bindSpriteToInstance(void *unitPtr, void *spritePtr) {
  if (!unitPtr || !spritePtr)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  ModelInstanceRecord &record = m_byUnitPtr[unitPtr];
  record.unitPtr = unitPtr;
  record.spritePtr = spritePtr;
  record.runtimeModelPtr = TryReadRuntimeModelFromSprite(spritePtr);
  if (record.runtimeModelPtr) {
    ModelRegistry::instance().recordRuntimeModelBinding(
        spritePtr, record.runtimeModelPtr, nullptr, 0u, 0u);
  }
  ModelResourceRecord modelRecord = {};
  if (ModelRegistry::instance().findBySprite(spritePtr, modelRecord) ||
      ModelRegistry::instance().findByRuntimeModel(record.runtimeModelPtr,
                                                   modelRecord)) {
    record.modelKey = modelRecord.modelKey;
    if (modelRecord.runtimeModelPtr)
      record.runtimeModelPtr = modelRecord.runtimeModelPtr;
    record.modelResourcePtr = modelRecord.modelResourcePtr;
  }
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

void ModelInstanceRegistry::bindRuntimeModelToSprite(void *spritePtr,
                                                     void *runtimeModelPtr,
                                                     uint64_t modelKey,
                                                     void *modelResourcePtr) {
  if (!spritePtr || !runtimeModelPtr)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  ModelInstanceRecord record = {};
  auto itSprite = m_bySpritePtr.find(spritePtr);
  if (itSprite != m_bySpritePtr.end())
    record = itSprite->second;
  auto itRuntime = m_byRuntimeModel.find(runtimeModelPtr);
  if (itRuntime != m_byRuntimeModel.end())
    MergeModelInstanceRecord(record, itRuntime->second);

  record.spritePtr = spritePtr;
  record.runtimeModelPtr = runtimeModelPtr;
  if (modelKey != 0)
    record.modelKey = modelKey;
  if (modelResourcePtr)
    record.modelResourcePtr = modelResourcePtr;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

void ModelInstanceRegistry::bindModelToInstance(void *sceneNode,
                                                uint64_t modelKey) {
  if (!sceneNode || modelKey == 0)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  ModelInstanceRecord &record = m_bySceneNode[sceneNode];
  record.sceneNode = sceneNode;
  record.modelKey = modelKey;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  if (record.spritePtr)
    m_bySpritePtr[record.spritePtr] = record;
  if (record.runtimeModelPtr)
    m_byRuntimeModel[record.runtimeModelPtr] = record;
}

bool ModelInstanceRegistry::findBySceneNode(void *sceneNode,
                                            ModelInstanceRecord &out) const {
  if (!sceneNode)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findByUnitPtr(void *unitPtr,
                                          ModelInstanceRecord &out) const {
  if (!unitPtr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findBySpritePtr(void *spritePtr,
                                            ModelInstanceRecord &out) const {
  if (!spritePtr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_bySpritePtr.find(spritePtr);
  if (it == m_bySpritePtr.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findByRuntimeModel(void *runtimeModelPtr,
                                               ModelInstanceRecord &out) const {
  if (!runtimeModelPtr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool ModelInstanceRegistry::findByHandle(uint32_t jHandle,
                                         ModelInstanceRecord &out) const {
  if (jHandle == 0)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byHandle.find(jHandle);
  if (it == m_byHandle.end())
    return false;
  out = it->second;
  return true;
}

std::vector<ModelInstanceRecord> ModelInstanceRegistry::snapshot() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<ModelInstanceRecord> out;
  out.reserve(m_bySceneNode.size());
  for (const auto &it : m_bySceneNode)
    out.push_back(it.second);
  return out;
}

size_t ModelInstanceRegistry::recordCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

size_t ModelInstanceRegistry::runtimeBoundCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_byRuntimeModel) {
    const auto &record = it.second;
    if (record.runtimeModelPtr != nullptr && record.spritePtr != nullptr)
      ++count;
  }
  return count;
}

size_t ModelInstanceRegistry::completeIdentityCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_byRuntimeModel) {
    const auto &record = it.second;
    if (record.runtimeModelPtr == nullptr || record.spritePtr == nullptr)
      continue;
    if (record.sceneNode != nullptr || record.unitPtr != nullptr ||
        record.jHandle != 0u || record.rawcode != 0u)
      ++count;
  }
  return count;
}

uint64_t ModelInstanceRegistry::frameNumber() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_frameNumber;
}

PoseRegistry &PoseRegistry::instance() {
  static PoseRegistry s_instance;
  return s_instance;
}

void PoseRegistry::beginFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
  ++m_frameNumber;
}

void PoseRegistry::endFrame() {
  std::lock_guard<std::mutex> lock(m_mutex);
}

void PoseRegistry::storeRecord(const PoseRecord &record) {
  PoseRecord merged = {};
  if (record.runtimeModelPtr) {
    auto it = m_byRuntimeModel.find(record.runtimeModelPtr);
    if (it != m_byRuntimeModel.end())
      MergePoseRecord(merged, it->second);
  }
  if (record.sceneNode) {
    auto it = m_bySceneNode.find(record.sceneNode);
    if (it != m_bySceneNode.end())
      MergePoseRecord(merged, it->second);
  }
  if (record.unitPtr) {
    auto it = m_byUnitPtr.find(record.unitPtr);
    if (it != m_byUnitPtr.end())
      MergePoseRecord(merged, it->second);
  }
  MergePoseRecord(merged, record);

  if (merged.runtimeModelPtr)
    m_byRuntimeModel[merged.runtimeModelPtr] = merged;
  if (merged.sceneNode)
    m_bySceneNode[merged.sceneNode] = merged;
  if (merged.unitPtr)
    m_byUnitPtr[merged.unitPtr] = merged;
}

void PoseRegistry::recordPose(void *runtimeModelPtr, void *sceneNode,
                              void *unitPtr, uint32_t sequenceId,
                              float sequenceTime, float scale, float yaw,
                              float pitch, float roll, float height,
                              bool hasWorldTransform,
                              const Matrix4 *worldTransform) {
  if (!runtimeModelPtr && !sceneNode && !unitPtr)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  PoseRecord record = {};
  record.runtimeModelPtr = runtimeModelPtr;
  record.sceneNode = sceneNode;
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
  record.lastRootPoseFrame = m_frameNumber;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

void PoseRegistry::recordSpriteFramePose(void *runtimeModelPtr, void *spritePtr,
                                         void *sceneNode, void *unitPtr,
                                         float dt, uint32_t sequenceId,
                                         float sequenceTime, float scale,
                                         float yaw, float pitch, float roll,
                                         float height,
                                         bool hasWorldTransform,
                                         const Matrix4 *worldTransform) {
  if (!runtimeModelPtr && !spritePtr && !sceneNode && !unitPtr)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  PoseRecord record = {};
  record.runtimeModelPtr = runtimeModelPtr;
  record.spritePtr = spritePtr;
  record.sceneNode = sceneNode;
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
  record.lastSpriteFramePoseFrame = m_frameNumber;
  record.spriteFrameSampleCount = 1;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

void PoseRegistry::recordMatrixPalette(void* runtimeModelPtr, void* sceneNode,
                                       void* unitPtr, const Matrix4* matrices,
                                       uint32_t matrixCount) {
  if ((!runtimeModelPtr && !sceneNode && !unitPtr) || matrices == nullptr ||
      matrixCount == 0)
    return;

  std::lock_guard<std::mutex> lock(m_mutex);
  PoseRecord record = {};
  record.runtimeModelPtr = runtimeModelPtr;
  record.sceneNode = sceneNode;
  record.unitPtr = unitPtr;
  record.matrixCount = matrixCount;
  record.matrixPalette.assign(matrices, matrices + matrixCount);
  record.matrixHash = Fnv1a64(record.matrixPalette.data(),
                             record.matrixPalette.size() * sizeof(Matrix4));
  record.lastMatrixPaletteFrame = m_frameNumber;
  if (record.firstSeenFrame == 0)
    record.firstSeenFrame = m_frameNumber;
  record.lastSeenFrame = m_frameNumber;
  storeRecord(record);
}

bool PoseRegistry::findByRuntimeModel(void *runtimeModelPtr,
                                      PoseRecord &out) const {
  if (!runtimeModelPtr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end())
    return false;
  out = it->second;
  return true;
}

bool PoseRegistry::findBySceneNode(void *sceneNode, PoseRecord &out) const {
  if (!sceneNode)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end())
    return false;
  out = it->second;
  return true;
}

bool PoseRegistry::findByUnitPtr(void *unitPtr, PoseRecord &out) const {
  if (!unitPtr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end())
    return false;
  out = it->second;
  return true;
}

std::vector<PoseRecord> PoseRegistry::snapshot() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<PoseRecord> out;
  out.reserve(m_bySceneNode.size());
  for (const auto &it : m_bySceneNode)
    out.push_back(it.second);
  return out;
}

size_t PoseRegistry::recordCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_byRuntimeModel.size();
}

size_t PoseRegistry::readyPoseCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto &it : m_byRuntimeModel) {
    const auto &record = it.second;
    if (record.lastSpriteFramePoseFrame != 0 ||
        record.lastRootPoseFrame != 0 ||
        (record.lastMatrixPaletteFrame != 0 && record.matrixCount != 0 &&
         !record.matrixPalette.empty())) {
      ++count;
    }
  }
  return count;
}

size_t PoseRegistry::spriteFramePoseCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& it : m_byRuntimeModel) {
    if (it.second.lastSpriteFramePoseFrame != 0)
      ++count;
  }
  return count;
}

size_t PoseRegistry::matrixPaletteCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  size_t count = 0;
  for (const auto& it : m_byRuntimeModel) {
    if (it.second.lastMatrixPaletteFrame != 0 &&
        it.second.matrixCount != 0 &&
        !it.second.matrixPalette.empty()) {
      ++count;
    }
  }
  return count;
}

uint64_t PoseRegistry::frameNumber() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_frameNumber;
}

} // namespace model
} // namespace war3
} // namespace dxvk
