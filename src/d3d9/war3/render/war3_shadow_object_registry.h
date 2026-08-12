// war3_shadow_object_registry.h - 阴影对象运行时骨架
#pragma once

#include "war3_render_objects.h"
#include "../../../util/util_matrix.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
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
  // Set only by SceneCollector's batch feed.  Other same-frame pose/model
  // updates may refresh lastSeenFrame but cannot authorize deduplication.
  uint64_t lastSceneCollectorBatchFrame = 0;
};

// Fixed-size projection used by the per-draw semantic augment path.  The full
// registry record owns modelPath and several fields that this consumer never
// reads; copying that record on every lookup can allocate/copy a path string.
struct ShadowObjectAugmentView {
  void *worldObjectEntry = nullptr;
  void *sceneNode = nullptr;
  void *unitPtr = nullptr;
  void *runtimeModelPtr = nullptr;
  void *modelResourcePtr = nullptr;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  ObjectKind kind = ObjectKind::Unknown;
  uint64_t modelKey = 0;
  float scale = 1.0f;
  float height = 0.0f;
  bool hasWorldTransform = false;
  Matrix4 worldTransform;
  bool hasSpriteFrameTransform = false;
  Matrix4 spriteFrameTransform;
  uint32_t matrixCount = 0;
  uint64_t matrixHash = 0;
  uint64_t lastRootPoseFrame = 0;
  uint64_t lastSpriteFramePoseFrame = 0;
  uint64_t lastMatrixPaletteFrame = 0;
};

struct ShadowIdentitySameFrameDedupStats {
  // attempts == hits + every miss* counter. batchMarked is independent.
  uint64_t attempts = 0;
  uint64_t hits = 0;
  uint64_t missMissingAlias = 0;
  uint64_t missCrossFrame = 0;
  uint64_t missNoBatchProof = 0;
  uint64_t missIncomplete = 0;
  uint64_t missInputMismatch = 0;
  uint64_t missAliasConflict = 0;
  uint64_t batchMarked = 0;
};

class ShadowObjectRegistry {
public:
  static ShadowObjectRegistry &instance();

  void beginFrame();
  void endFrame();
  void resetMapSession();

  void noteRenderObject(const RenderObjectInfo &info);
  void noteRenderObjectsBatch(
      const std::vector<const RenderObjectInfo *> &infos);
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

  bool findByWorldObjectEntry(void *worldObjectEntry,
                              ShadowObjectRecord &out) const;
  bool findBySceneNode(void *sceneNode, ShadowObjectRecord &out) const;
  bool findByUnitPtr(void *unitPtr, ShadowObjectRecord &out) const;
  bool findByHandle(uint32_t jHandle, ShadowObjectRecord &out) const;
  bool findBySpritePtr(void *spritePtr, ShadowObjectRecord &out) const;
  bool findByRuntimeModel(void *runtimeModelPtr, ShadowObjectRecord &out) const;
  // Direct packet lookup keeps the historical alias priority while holding
  // one shared lock. The two runtime aliases may describe different owners.
  bool findFirstForDirectPacket(void* worldObjectEntry, void* sceneNode,
                                uint32_t jHandle,
                                void* primaryRuntimeModelPtr,
                                void* secondaryRuntimeModelPtr,
                                ShadowObjectRecord& out) const;
  bool findFirstForDirectPacketView(void* worldObjectEntry, void* sceneNode,
                                    uint32_t jHandle,
                                    void* primaryRuntimeModelPtr,
                                    void* secondaryRuntimeModelPtr,
                                    ShadowObjectAugmentView& out) const;
  // Per-draw semantic augment lookup. Preserves the legacy key priority while
  // holding one shared lock instead of reacquiring it for every miss.
  bool findFirstForAugment(void* worldObjectEntry, void* sceneNode,
                           void* primaryUnitPtr, void* secondaryUnitPtr,
                           uint32_t jHandle, void* runtimeModelPtr,
                           ShadowObjectRecord& out) const;
  bool findFirstForAugmentView(void* worldObjectEntry, void* sceneNode,
                               void* primaryUnitPtr, void* secondaryUnitPtr,
                               uint32_t jHandle, void* runtimeModelPtr,
                               ShadowObjectAugmentView& out,
                               uint64_t* mutationGenerationOut = nullptr,
                               uint64_t* frameNumberOut = nullptr) const;
  std::vector<ShadowObjectRecord> snapshot() const;
  size_t runtimeBoundCount() const;
  size_t completeIdentityCount() const;
  size_t poseReadyCount() const;
  ShadowIdentitySameFrameDedupStats sameFrameIdentityDedupStats() const;

  uint64_t frameNumber() const;
  uint64_t mutationGeneration() const;

private:
  ShadowObjectRegistry() = default;

  void storeRecord(const ShadowObjectRecord &record);
  bool trySkipExactSameFrameRenderObjectLocked(const RenderObjectInfo &info,
                                                uint64_t currentFrame);
  void noteInstanceIdentityLocked(void *worldObjectEntry, void *sceneNode,
                                  void *unitPtr, void *spritePtr,
                                  uint32_t jHandle, uint32_t rawcode,
                                  ObjectKind kind,
                                  bool sceneCollectorBatch);

  // Phase 7.83：reader 路径（findBy*/snapshot/recordCount/frameNumber）远多于 writer。
  mutable std::shared_mutex m_mutex;
  std::unordered_map<void *, ShadowObjectRecord> m_byWorldObjectEntry;
  std::unordered_map<void *, ShadowObjectRecord> m_bySceneNode;
  std::unordered_map<void *, ShadowObjectRecord> m_byUnitPtr;
  std::unordered_map<void *, ShadowObjectRecord> m_bySpritePtr;
  std::unordered_map<void *, ShadowObjectRecord> m_byRuntimeModel;
  std::unordered_map<uint32_t, ShadowObjectRecord> m_byHandle;
  ShadowIdentitySameFrameDedupStats m_sameFrameIdentityDedupStats = {};
  // Protected by m_mutex.  The exact alias proof runs only after this frame
  // received a valid SceneCollector batch entry.
  uint64_t m_lastSceneCollectorBatchFrame = 0;
  std::atomic<uint64_t> m_frameNumber{0};
  // Even values are stable; odd values mean a writer holds m_mutex. TLS
  // caches retain value projections only and validate hits with two loads.
  std::atomic<uint64_t> m_mutationGeneration{0};
};

} // namespace render
} // namespace war3
} // namespace dxvk
