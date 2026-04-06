// war3_shadow_object_registry.h - 阴影对象运行时骨架
#pragma once

#include "war3_render_objects.h"
#include "../../../util/util_matrix.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxvk {
namespace war3 {
namespace model {
struct ModelInstanceRecord;
struct ModelResourceRecord;
struct PoseRecord;
class ModelInstanceRegistry;
class ModelRegistry;
class PoseRegistry;
}
namespace render {

struct ShadowObjectRecord {
  void *worldObjectEntry = nullptr;
  void *sceneNode = nullptr;
  void *unitPtr = nullptr;
  void *spritePtr = nullptr;
  void *runtimeModelPtr = nullptr;
  void *modelResourcePtr = nullptr;
  std::string modelPath;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  ObjectKind kind = ObjectKind::Unknown;
  uint64_t modelKey = 0;
  uint32_t modelType = 0;
  uint32_t modelFlags = 0;
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
  uint64_t lastRootPoseFrame = 0;
  uint64_t lastSpriteFramePoseFrame = 0;
  uint64_t lastMatrixPaletteFrame = 0;
  uint32_t spriteFrameSampleCount = 0;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;
};

class ShadowObjectRegistry {
public:
  static ShadowObjectRegistry &instance();

  void beginFrame();
  void endFrame();

  void noteRenderObject(const RenderObjectInfo &info);
  void noteInstanceIdentity(void *worldObjectEntry, void *sceneNode,
                            void *unitPtr, void *spritePtr, uint32_t jHandle,
                            uint32_t rawcode, ObjectKind kind);
  void noteModelBinding(void *spritePtr, void *runtimeModelPtr,
                        void *modelResourcePtr, const std::string &modelPath,
                        uint32_t modelType, uint32_t modelFlags,
                        uint64_t modelKey);
  void notePose(void *runtimeModelPtr, void *sceneNode, void *unitPtr,
                uint32_t sequenceId, float sequenceTime, float scale, float yaw,
                float pitch, float roll, float height,
                bool hasWorldTransform = false,
                const Matrix4 *worldTransform = nullptr,
                uint32_t matrixCount = 0, uint64_t matrixHash = 0);
  void noteSpriteFramePose(void *runtimeModelPtr, void *spritePtr,
                           void *sceneNode, void *unitPtr, float dt,
                           uint32_t sequenceId, float sequenceTime, float scale,
                           float yaw, float pitch, float roll, float height,
                           bool hasWorldTransform = false,
                           const Matrix4 *worldTransform = nullptr,
                           uint32_t matrixCount = 0,
                           uint64_t matrixHash = 0);

  bool findBySceneNode(void *sceneNode, ShadowObjectRecord &out) const;
  bool findByUnitPtr(void *unitPtr, ShadowObjectRecord &out) const;
  bool findByHandle(uint32_t jHandle, ShadowObjectRecord &out) const;
  bool findBySpritePtr(void *spritePtr, ShadowObjectRecord &out) const;
  bool findByRuntimeModel(void *runtimeModelPtr, ShadowObjectRecord &out) const;
  std::vector<ShadowObjectRecord> snapshot() const;
  size_t runtimeBoundCount() const;
  size_t completeIdentityCount() const;
  size_t poseReadyCount() const;

  uint64_t frameNumber() const;

private:
  ShadowObjectRegistry() = default;

  void storeRecord(const ShadowObjectRecord &record);

  mutable std::mutex m_mutex;
  std::unordered_map<void *, ShadowObjectRecord> m_bySceneNode;
  std::unordered_map<void *, ShadowObjectRecord> m_byUnitPtr;
  std::unordered_map<void *, ShadowObjectRecord> m_bySpritePtr;
  std::unordered_map<void *, ShadowObjectRecord> m_byRuntimeModel;
  std::unordered_map<uint32_t, ShadowObjectRecord> m_byHandle;
  uint64_t m_frameNumber = 0;
};

} // namespace render
} // namespace war3
} // namespace dxvk
