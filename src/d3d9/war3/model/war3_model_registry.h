// war3_model_registry.h - War3 模型资源/实例/姿态运行时骨架
#pragma once

#include "../../../util/util_matrix.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxvk {
namespace war3 {
namespace render {
struct RenderObjectInfo;
}
namespace model {

struct ModelResourceRecord {
  void *spritePtr = nullptr;
  void *runtimeModelPtr = nullptr;
  void *modelResourcePtr = nullptr;
  std::string modelPath;
  uint32_t modelType = 0;
  uint32_t flags = 0;
  uint64_t modelKey = 0;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;
};

struct ModelInstanceRecord {
  void *worldObjectEntry = nullptr;
  void *sceneNode = nullptr;
  void *unitPtr = nullptr;
  void *spritePtr = nullptr;
  void *runtimeModelPtr = nullptr;
  void *sourceObjectPtr = nullptr;
  void *sourceSpriteObjectPtr = nullptr;
  void *runtimeCreatorModelDataPtr = nullptr;
  void *runtimeCreatorHandlePtr = nullptr;
  void *modelResourcePtr = nullptr;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  uint32_t runtimeCreatorCallerRva = 0;
  uint32_t runtimeResolveCallerRva = 0;
  uint64_t modelKey = 0;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;
};

struct PoseRecord {
  void *runtimeModelPtr = nullptr;
  void *sceneNode = nullptr;
  void *unitPtr = nullptr;
  void *spritePtr = nullptr;
  uint32_t sequenceId = 0;
  float sequenceTime = 0.0f;
  float scale = 1.0f;
  float yaw = 0.0f;
  float pitch = 0.0f;
  float roll = 0.0f;
  float height = 0.0f;
  bool hasWorldTransform = false;
  Matrix4 worldTransform;
  bool hasSpriteFrameTransform = false;
  Matrix4 spriteFrameTransform;
  float spriteFrameDt = 0.0f;
  uint32_t matrixCount = 0;
  uint64_t matrixHash = 0;
  std::vector<Matrix4> matrixPalette;
  uint64_t lastRootPoseFrame = 0;
  uint64_t lastSpriteFramePoseFrame = 0;
  uint64_t lastMatrixPaletteFrame = 0;
  uint32_t spriteFrameSampleCount = 0;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;
};

struct AttachmentRigidRecord {
  void* rootRuntimeModelPtr = nullptr;
  void* ownerRuntimeModelPtr = nullptr;
  void* childRuntimeModelPtr = nullptr;
  void* childSpritePtr = nullptr;
  void* sourceObjectPtr = nullptr;
  void* sourceSpriteObjectPtr = nullptr;
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  uint32_t slotIndex = 0;
  uint32_t sourceRecordIndex = 0;
  uint32_t childTag = 0;
  float localPointX = 0.0f;
  float localPointY = 0.0f;
  float localPointZ = 0.0f;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;
};

class ModelRegistry {
public:
  static ModelRegistry &instance();

  void beginFrame();
  void endFrame();

  void recordSpriteModelPath(void *spritePtr, const char *modelPath,
                             uint32_t modelType, uint32_t flags);
  void recordRuntimeModelBinding(void *spritePtr, void *runtimeModelPtr,
                                 void *modelResourcePtr, uint32_t modelType,
                                 uint32_t flags);

  bool findBySprite(void *spritePtr, ModelResourceRecord &out) const;
  bool findByRuntimeModel(void *runtimeModelPtr, ModelResourceRecord &out) const;
  bool findByPath(const std::string &modelPath, ModelResourceRecord &out) const;
  std::vector<ModelResourceRecord> snapshot() const;
  size_t recordCount() const;

  uint64_t frameNumber() const;

private:
  ModelRegistry() = default;

  static uint64_t HashModelKey(const std::string &path, uint32_t modelType,
                               uint32_t flags);
  static uint64_t HashRuntimeModelKey(void *modelResourcePtr,
                                      void *runtimeModelPtr,
                                      uint32_t modelType, uint32_t flags);

  mutable std::mutex m_mutex;
  std::unordered_map<void *, ModelResourceRecord> m_bySprite;
  std::unordered_map<void *, ModelResourceRecord> m_byRuntimeModel;
  std::unordered_map<std::string, ModelResourceRecord> m_byPath;
  uint64_t m_frameNumber = 0;
};

class ModelInstanceRegistry {
public:
  static ModelInstanceRegistry &instance();

  void beginFrame();
  void endFrame();

  void noteRenderObject(const render::RenderObjectInfo &info);
  void noteRenderObjectsBatch(
      const std::vector<const render::RenderObjectInfo *> &infos);
  void noteInstanceIdentity(void *worldObjectEntry, void *sceneNode,
                            void *unitPtr, void *spritePtr,
                            uint32_t jHandle, uint32_t rawcode);
  void bindSpriteToInstance(void *unitPtr, void *spritePtr);
  void bindRuntimeModelToSprite(void *spritePtr, void *runtimeModelPtr,
                                uint64_t modelKey,
                                void *modelResourcePtr = nullptr,
                                bool propagateOwnerIdentity = true);
  void noteRuntimeCreationProvenance(void *runtimeModelPtr,
                                     void *modelDataPtr,
                                     uint32_t callerRva);
  void noteRuntimeResolveProvenance(void *runtimeModelPtr,
                                    void *creatorHandlePtr,
                                    uint32_t callerRva);
  void noteRuntimeSourceObject(void *runtimeModelPtr, void *sourceObjectPtr,
                               void *sourceSpriteObjectPtr,
                               void *spritePtr = nullptr);
  void noteRuntimeOwnerIdentity(void *runtimeModelPtr, void *worldObjectEntry,
                                void *sceneNode, void *unitPtr,
                                void *spritePtr, uint32_t jHandle,
                                uint32_t rawcode);
  void bindModelToInstance(void *sceneNode, uint64_t modelKey);

  bool findByWorldObjectEntry(void *worldObjectEntry,
                              ModelInstanceRecord &out) const;
  bool findBySceneNode(void *sceneNode, ModelInstanceRecord &out) const;
  bool findByUnitPtr(void *unitPtr, ModelInstanceRecord &out) const;
  bool findBySpritePtr(void *spritePtr, ModelInstanceRecord &out) const;
  bool findByRuntimeModel(void *runtimeModelPtr, ModelInstanceRecord &out) const;
  bool findOwnerByRuntimeModel(void *runtimeModelPtr,
                               ModelInstanceRecord &out) const;
  bool findBySourceObject(void* sourceObjectPtr, ModelInstanceRecord& out) const;
  bool findBySourceSpriteObject(void* sourceSpriteObjectPtr,
                                ModelInstanceRecord& out) const;
  bool findByHandle(uint32_t jHandle, ModelInstanceRecord &out) const;
  std::vector<ModelInstanceRecord> snapshot() const;
  size_t recordCount() const;
  size_t runtimeBoundCount() const;
  size_t runtimeCreationProvenanceCount() const;
  size_t runtimeResolveProvenanceCount() const;
  size_t runtimeSourceObjectCount() const;
  size_t runtimeOwnerIdentityCount() const;
  size_t completeIdentityCount() const;

  uint64_t frameNumber() const;

private:
  ModelInstanceRegistry() = default;

  void storeRecord(const ModelInstanceRecord &record);
  void noteInstanceIdentityLocked(void *worldObjectEntry, void *sceneNode,
                                  void *unitPtr, void *spritePtr,
                                  uint32_t jHandle, uint32_t rawcode);
  void propagateRuntimeSourceObjectLocked(
      void *rootRuntimeModelPtr, const ModelInstanceRecord &sourceRecord);
  void propagateRuntimeOwnerIdentityLocked(
      void *rootRuntimeModelPtr, const ModelInstanceRecord &ownerRecord);

  mutable std::mutex m_mutex;
  std::unordered_map<void *, ModelInstanceRecord> m_byWorldObjectEntry;
  std::unordered_map<void *, ModelInstanceRecord> m_bySceneNode;
  std::unordered_map<void *, ModelInstanceRecord> m_byUnitPtr;
  std::unordered_map<void *, ModelInstanceRecord> m_bySpritePtr;
  std::unordered_map<void *, ModelInstanceRecord> m_byRuntimeModel;
  std::unordered_map<void *, ModelInstanceRecord> m_runtimeOwnerByRuntimeModel;
  std::unordered_map<void*, ModelInstanceRecord> m_bySourceObject;
  std::unordered_map<void*, ModelInstanceRecord> m_bySourceSpriteObject;
  std::unordered_map<uint32_t, ModelInstanceRecord> m_byHandle;
  uint64_t m_frameNumber = 0;
};

class PoseRegistry {
public:
  static PoseRegistry &instance();

  void beginFrame();
  void endFrame();

  void recordPose(void *runtimeModelPtr, void *sceneNode, void *unitPtr,
                  uint32_t sequenceId, float sequenceTime, float scale, float yaw,
                  float pitch, float roll, float height,
                  bool hasWorldTransform = false,
                  const Matrix4 *worldTransform = nullptr);
  void recordSpriteFramePose(void *runtimeModelPtr, void *spritePtr,
                             void *sceneNode, void *unitPtr, float dt,
                             uint32_t sequenceId, float sequenceTime,
                             float scale, float yaw, float pitch, float roll,
                             float height, bool hasWorldTransform = false,
                             const Matrix4 *worldTransform = nullptr);
  void recordMatrixPalette(void* runtimeModelPtr, void* sceneNode,
                           void* unitPtr, const Matrix4* matrices,
                           uint32_t matrixCount);

  bool findBySceneNode(void *sceneNode, PoseRecord &out) const;
  bool findByUnitPtr(void *unitPtr, PoseRecord &out) const;
  bool findByRuntimeModel(void *runtimeModelPtr, PoseRecord &out) const;
  std::vector<PoseRecord> snapshot() const;
  size_t recordCount() const;
  size_t readyPoseCount() const;
  size_t spriteFramePoseCount() const;
  size_t matrixPaletteCount() const;

  uint64_t frameNumber() const;

private:
  PoseRegistry() = default;

  void storeRecord(const PoseRecord &record);

  mutable std::mutex m_mutex;
  std::unordered_map<void *, PoseRecord> m_byRuntimeModel;
  std::unordered_map<void *, PoseRecord> m_bySceneNode;
  std::unordered_map<void *, PoseRecord> m_byUnitPtr;
  uint64_t m_frameNumber = 0;
};

class AttachmentRigidRegistry {
public:
  static AttachmentRigidRegistry& instance();

  void beginFrame();
  void endFrame();

  void noteAttachmentRigid(void* rootRuntimeModelPtr, void* ownerRuntimeModelPtr,
                           void* childRuntimeModelPtr, void* childSpritePtr,
                           void* worldObjectEntry, void* sceneNode, void* unitPtr,
                           void* sourceObjectPtr,
                           void* sourceSpriteObjectPtr, uint32_t jHandle,
                           uint32_t rawcode, uint32_t slotIndex,
                           uint32_t sourceRecordIndex, uint32_t childTag,
                           float localPointX, float localPointY,
                           float localPointZ);
  void noteRuntimeIdentity(void* runtimeModelPtr, void* worldObjectEntry,
                           void* sceneNode, void* unitPtr,
                           void* sourceObjectPtr,
                           void* sourceSpriteObjectPtr, uint32_t jHandle,
                           uint32_t rawcode);
  bool promoteAttachmentChildRuntime(void* parentRuntimeModelPtr,
                                     void* childRuntimeModelPtr,
                                     void* childSpritePtr,
                                     AttachmentRigidRecord* outRecord = nullptr,
                                     void** outPreviousChildRuntimeModelPtr =
                                         nullptr);

  bool findByChildRuntimeModel(void* childRuntimeModelPtr,
                               AttachmentRigidRecord& out) const;
  bool findByOwnerRuntimeModel(void* ownerRuntimeModelPtr,
                               AttachmentRigidRecord& out) const;
  bool findByRootRuntimeModel(void* rootRuntimeModelPtr,
                              AttachmentRigidRecord& out) const;
  bool findByAnyRuntimeModel(void* runtimeModelPtr,
                             AttachmentRigidRecord& out) const;
  bool findByWorldObjectEntry(void* worldObjectEntry,
                              AttachmentRigidRecord& out) const;
  bool findBySceneNode(void* sceneNode, AttachmentRigidRecord& out) const;
  bool findByUnitPtr(void* unitPtr, AttachmentRigidRecord& out) const;
  bool findByHandle(uint32_t jHandle, AttachmentRigidRecord& out) const;
  std::vector<AttachmentRigidRecord> snapshot() const;
  size_t recordCount() const;

  uint64_t frameNumber() const;

private:
  AttachmentRigidRegistry() = default;

  void storeRecord(const AttachmentRigidRecord& record);

  mutable std::mutex m_mutex;
  std::unordered_map<void*, AttachmentRigidRecord> m_byChildRuntimeModel;
  std::unordered_map<void*, AttachmentRigidRecord> m_byOwnerRuntimeModel;
  std::unordered_map<void*, AttachmentRigidRecord> m_byRootRuntimeModel;
  std::unordered_map<void*, AttachmentRigidRecord> m_byWorldObjectEntry;
  std::unordered_map<void*, AttachmentRigidRecord> m_bySceneNode;
  std::unordered_map<void*, AttachmentRigidRecord> m_byUnitPtr;
  std::unordered_map<uint32_t, AttachmentRigidRecord> m_byHandle;
  uint64_t m_frameNumber = 0;
};

} // namespace model
} // namespace war3
} // namespace dxvk
