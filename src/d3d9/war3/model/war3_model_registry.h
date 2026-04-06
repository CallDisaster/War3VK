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
  void *modelResourcePtr = nullptr;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
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
  void noteInstanceIdentity(void *worldObjectEntry, void *sceneNode,
                            void *unitPtr, void *spritePtr,
                            uint32_t jHandle, uint32_t rawcode);
  void bindSpriteToInstance(void *unitPtr, void *spritePtr);
  void bindRuntimeModelToSprite(void *spritePtr, void *runtimeModelPtr,
                                uint64_t modelKey,
                                void *modelResourcePtr = nullptr);
  void bindModelToInstance(void *sceneNode, uint64_t modelKey);

  bool findBySceneNode(void *sceneNode, ModelInstanceRecord &out) const;
  bool findByUnitPtr(void *unitPtr, ModelInstanceRecord &out) const;
  bool findBySpritePtr(void *spritePtr, ModelInstanceRecord &out) const;
  bool findByRuntimeModel(void *runtimeModelPtr, ModelInstanceRecord &out) const;
  bool findByHandle(uint32_t jHandle, ModelInstanceRecord &out) const;
  std::vector<ModelInstanceRecord> snapshot() const;
  size_t recordCount() const;
  size_t runtimeBoundCount() const;
  size_t completeIdentityCount() const;

  uint64_t frameNumber() const;

private:
  ModelInstanceRegistry() = default;

  void storeRecord(const ModelInstanceRecord &record);

  mutable std::mutex m_mutex;
  std::unordered_map<void *, ModelInstanceRecord> m_bySceneNode;
  std::unordered_map<void *, ModelInstanceRecord> m_byUnitPtr;
  std::unordered_map<void *, ModelInstanceRecord> m_bySpritePtr;
  std::unordered_map<void *, ModelInstanceRecord> m_byRuntimeModel;
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

} // namespace model
} // namespace war3
} // namespace dxvk
