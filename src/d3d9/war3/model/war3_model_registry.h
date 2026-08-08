// war3_model_registry.h - War3 模型资源/实例/姿态运行时骨架
#pragma once

#include "../../../util/util_matrix.h"

#include <cstdint>
#include <atomic>
#include <mutex>
#include <shared_mutex>
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
  // Exact provenance for the periodic SceneCollector batch.  lastSeenFrame is
  // insufficient for same-frame deduplication because pose/model hooks also
  // refresh it.
  uint64_t lastSceneCollectorBatchFrame = 0;
};

// Fixed-size projection used by the per-draw semantic augment path.  Keeping
// only the seven fields consumed by AugmentShadowSemanticContext makes the TLS
// cache independent of the registry's larger provenance record.
struct ModelInstanceAugmentView {
  void *worldObjectEntry = nullptr;
  void *sceneNode = nullptr;
  void *runtimeModelPtr = nullptr;
  void *modelResourcePtr = nullptr;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  uint64_t modelKey = 0;
};

struct ModelIdentitySameFrameDedupStats {
  // attempts == hits + every miss* counter. batchMarked is independent.
  uint64_t attempts = 0;
  uint64_t hits = 0;
  uint64_t missMissingAlias = 0;
  uint64_t missCrossFrame = 0;
  uint64_t missNoBatchProof = 0;
  uint64_t missIncomplete = 0;
  uint64_t missInputMismatch = 0;
  uint64_t missAliasConflict = 0;
  uint64_t missRuntimeOwner = 0;
  uint64_t batchMarked = 0;
};

/**
 * \brief ModelInstance 健康聚合校验差异位
 *
 * 每帧 runtime bridge 的正常路径只读取锁内维护的 O(1) 聚合计数；
 * DXVK_WAR3_REGISTRY_HEALTH_VERIFY=1 时才在同一 shared lock 下追加一次
 * 全表扫描，并将逐项差异写入 verifierMismatchMask。
 *
 * 校验矩阵：
 *   1. 新 key 插入：old=空，new 分类加入聚合；
 *   2. 字段升级：old 分类扣除后按 new 分类加入；
 *   3. 字段降级：与升级使用同一 old-subtract/new-add 路径；
 *   4. alias 覆盖：所有 m_byRuntimeModel 写点必须通过集中 helper。
 * DXVK_WAR3_REGISTRY_HEALTH_VERIFY_ASSERT=1 可在显式校验会话中对差异
 * 断言；默认两项均关闭，不执行全表扫描。
 */
enum ModelInstanceTrackingHealthMismatch : uint32_t {
  ModelInstanceTrackingHealthMismatchNone = 0u,
  ModelInstanceTrackingHealthMismatchRecordCount = 1u << 0,
  ModelInstanceTrackingHealthMismatchRuntimeBoundCount = 1u << 1,
  ModelInstanceTrackingHealthMismatchCompleteIdentityCount = 1u << 2,
};

/**
 * \brief ModelInstance 每帧健康聚合快照
 */
struct ModelInstanceTrackingHealthSnapshot {
  uint64_t frameNumber = 0;
  uint64_t recordCount = 0;
  uint64_t runtimeBoundCount = 0;
  uint64_t completeIdentityCount = 0;
  uint64_t aggregateReadPasses = 0;
  uint64_t verifierScanPasses = 0;
  uint64_t verifierRecordsScanned = 0;
  uint64_t verifierMismatchCount = 0;
  uint32_t verifierMismatchMask = ModelInstanceTrackingHealthMismatchNone;
};

/**
 * \brief Pose 健康聚合校验差异位
 */
enum PoseTrackingHealthMismatch : uint32_t {
  PoseTrackingHealthMismatchNone = 0u,
  PoseTrackingHealthMismatchRecordCount = 1u << 0,
  PoseTrackingHealthMismatchReadyPoseCount = 1u << 1,
  PoseTrackingHealthMismatchSpriteFramePoseCount = 1u << 2,
  PoseTrackingHealthMismatchMatrixPaletteCount = 1u << 3,
};

/**
 * \brief Pose 每帧健康聚合快照
 */
struct PoseTrackingHealthSnapshot {
  uint64_t frameNumber = 0;
  uint64_t recordCount = 0;
  uint64_t readyPoseCount = 0;
  uint64_t spriteFramePoseCount = 0;
  uint64_t matrixPaletteCount = 0;
  uint64_t aggregateReadPasses = 0;
  uint64_t verifierScanPasses = 0;
  uint64_t verifierRecordsScanned = 0;
  uint64_t verifierMismatchCount = 0;
  uint32_t verifierMismatchMask = PoseTrackingHealthMismatchNone;
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

// Fixed-size projection of PoseRecord for the per-draw shadow augment path,
// which reads only the transforms, freshness frames and matrix identity and
// never touches matrixPalette. Copying this POD under the registry lock avoids
// the per-draw heap deep-copy of matrixPalette (up to 256 Matrix4) that a full
// PoseRecord copy would incur for every skinned unit every frame.
struct PoseAugmentView {
  bool hasWorldTransform = false;
  Matrix4 worldTransform;
  bool hasSpriteFrameTransform = false;
  Matrix4 spriteFrameTransform;
  float scale = 1.0f;
  float height = 0.0f;
  uint32_t matrixCount = 0;
  uint64_t matrixHash = 0;
  uint64_t lastRootPoseFrame = 0;
  uint64_t lastSpriteFramePoseFrame = 0;
  uint64_t lastMatrixPaletteFrame = 0;
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
  void resetMapSession();

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

  // Phase 7.83：reader 路径主走 findBySprite/findByRuntimeModel/findByPath，
  // writer 路径只在 noteResource* / beginFrame / endFrame 调用。
  mutable std::shared_mutex m_mutex;
  std::unordered_map<void *, ModelResourceRecord> m_bySprite;
  std::unordered_map<void *, ModelResourceRecord> m_byRuntimeModel;
  std::unordered_map<std::string, ModelResourceRecord> m_byPath;
  std::atomic<uint64_t> m_frameNumber{0};
};

class ModelInstanceRegistry {
public:
  static ModelInstanceRegistry &instance();

  void beginFrame();
  void endFrame();
  void resetMapSession();

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
  // Per-draw semantic augment lookup. Preserves the legacy key priority while
  // holding one shared lock instead of reacquiring it for every miss.
  bool findFirstForAugment(void* worldObjectEntry, void* sceneNode,
                           void* primaryUnitPtr, void* secondaryUnitPtr,
                           uint32_t jHandle,
                           ModelInstanceRecord& out) const;
  bool findFirstForAugmentView(void* worldObjectEntry, void* sceneNode,
                               void* primaryUnitPtr, void* secondaryUnitPtr,
                               uint32_t jHandle,
                               ModelInstanceAugmentView& out,
                               uint64_t* mutationGenerationOut = nullptr) const;
  std::vector<ModelInstanceRecord> snapshot() const;
  size_t recordCount() const;
  size_t runtimeBoundCount() const;
  size_t runtimeCreationProvenanceCount() const;
  size_t runtimeResolveProvenanceCount() const;
  size_t runtimeSourceObjectCount() const;
  size_t runtimeOwnerIdentityCount() const;
  size_t completeIdentityCount() const;
  ModelInstanceTrackingHealthSnapshot trackingHealthSnapshot() const;
  /**
   * \brief 强制执行一次 ModelInstance 健康聚合全表校验
   *
   * 供离线测试或显式诊断入口调用；不依赖启动环境变量。
   */
  ModelInstanceTrackingHealthSnapshot
  debugVerifyTrackingHealthAggregate() const;
  ModelIdentitySameFrameDedupStats sameFrameIdentityDedupStats() const;

  uint64_t frameNumber() const;
  uint64_t mutationGeneration() const;

private:
  ModelInstanceRegistry() = default;

  void storeRecord(const ModelInstanceRecord &record);
  /**
   * \brief 替换 runtime-model 主表记录并同步健康聚合
   *
   * 调用方必须持有 m_mutex 的 unique_lock。该函数先读取旧分类，再写入
   * 新记录并执行 old-subtract/new-add，因此升级与降级使用同一合同。
   */
  void storeRuntimeModelRecordLocked(const ModelInstanceRecord& record);
  /**
   * \brief 在已持有 shared_lock 时生成健康快照
   *
   * runVerifier=false 只复制 O(1) 聚合；true 时追加同锁全表校验。
   */
  ModelInstanceTrackingHealthSnapshot trackingHealthSnapshotLocked(
      bool runVerifier) const;
  bool trySkipExactSameFrameRenderObjectLocked(
      const render::RenderObjectInfo &info, uint64_t currentFrame);
  void noteInstanceIdentityLocked(void *worldObjectEntry, void *sceneNode,
                                  void *unitPtr, void *spritePtr,
                                  uint32_t jHandle, uint32_t rawcode,
                                  bool sceneCollectorBatch);
  void propagateRuntimeSourceObjectLocked(
      void *rootRuntimeModelPtr, const ModelInstanceRecord &sourceRecord);
  void propagateRuntimeOwnerIdentityLocked(
      void *rootRuntimeModelPtr, const ModelInstanceRecord &ownerRecord);

  // Phase 7.83：与 PoseRegistry 同模式。reader 路径（findBy*/snapshot/*Count
  // /frameNumber）每帧 400-3000 次。
  mutable std::shared_mutex m_mutex;
  std::unordered_map<void *, ModelInstanceRecord> m_byWorldObjectEntry;
  std::unordered_map<void *, ModelInstanceRecord> m_bySceneNode;
  std::unordered_map<void *, ModelInstanceRecord> m_byUnitPtr;
  std::unordered_map<void *, ModelInstanceRecord> m_bySpritePtr;
  std::unordered_map<void *, ModelInstanceRecord> m_byRuntimeModel;
  std::unordered_map<void *, ModelInstanceRecord> m_runtimeOwnerByRuntimeModel;
  std::unordered_map<void*, ModelInstanceRecord> m_bySourceObject;
  std::unordered_map<void*, ModelInstanceRecord> m_bySourceSpriteObject;
  std::unordered_map<uint32_t, ModelInstanceRecord> m_byHandle;
  uint64_t m_trackingRuntimeBoundCount = 0;
  uint64_t m_trackingCompleteIdentityCount = 0;
  ModelIdentitySameFrameDedupStats m_sameFrameIdentityDedupStats = {};
  // Protected by m_mutex.  This keeps the exact alias proof entirely off the
  // ordinary per-frame RenderQueue path; only a SceneCollector batch in the
  // current frame can authorize the extra lookup work.
  uint64_t m_lastSceneCollectorBatchFrame = 0;
  std::atomic<uint64_t> m_frameNumber{0};
  // Even values are stable; odd values mean a writer holds m_mutex. Writers
  // advance once before and once after lookup-visible mutations, allowing a
  // value-only TLS cache to validate an unlocked hit with two acquire loads.
  std::atomic<uint64_t> m_mutationGeneration{0};
};

class PoseRegistry {
public:
  static PoseRegistry &instance();

  void beginFrame();
  void endFrame();
  void resetMapSession();

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
  // Augment-only lookups: project the matched record into a fixed-size POD
  // under the shared lock, skipping the matrixPalette deep-copy. Used by the
  // per-draw shadow semantic augment path.
  bool findBySceneNodeAugment(void *sceneNode, PoseAugmentView &out) const;
  bool findByUnitPtrAugment(void *unitPtr, PoseAugmentView &out) const;
  bool findByRuntimeModelAugment(void *runtimeModelPtr,
                                 PoseAugmentView &out) const;
  std::vector<PoseRecord> snapshot() const;
  size_t recordCount() const;
  size_t readyPoseCount() const;
  size_t spriteFramePoseCount() const;
  size_t matrixPaletteCount() const;
  PoseTrackingHealthSnapshot trackingHealthSnapshot() const;
  /**
   * \brief 强制执行一次 Pose 健康聚合全表校验
   *
   * 供离线测试或显式诊断入口调用；不依赖启动环境变量。
   */
  PoseTrackingHealthSnapshot debugVerifyTrackingHealthAggregate() const;

  uint64_t frameNumber() const;

private:
  PoseRegistry() = default;

  void storeRecord(const PoseRecord &record);
  /**
   * \brief 替换 runtime-model 姿态记录并同步健康聚合
   *
   * 调用方必须持有 m_mutex 的 unique_lock；所有姿态主表写入统一经过此处，
   * 以同时覆盖插入、分类升级和分类降级。
   */
  void storeRuntimeModelRecordLocked(const PoseRecord& record);
  /**
   * \brief 在已持有 shared_lock 时生成姿态健康快照
   *
   * runVerifier=false 只复制 O(1) 聚合；true 时追加同锁全表校验。
   */
  PoseTrackingHealthSnapshot trackingHealthSnapshotLocked(
      bool runVerifier) const;

  // Phase 7.83：reader 路径（findBy*/snapshot/*Count）每帧 200-2000 次，
  // 写路径（recordPose/recordSpriteFramePose/recordMatrixPalette/begin/endFrame）
  // 远少于 read。改 shared_mutex 后 reader 走 shared_lock，writer 走 unique_lock。
  // m_frameNumber 是 begin/endFrame 单调写、reader 高频读，改 atomic 让
  // frameNumber() 走 relaxed load 避免锁。
  mutable std::shared_mutex m_mutex;
  std::unordered_map<void *, PoseRecord> m_byRuntimeModel;
  std::unordered_map<void *, PoseRecord> m_bySceneNode;
  std::unordered_map<void *, PoseRecord> m_byUnitPtr;
  uint64_t m_trackingReadyPoseCount = 0;
  uint64_t m_trackingSpriteFramePoseCount = 0;
  uint64_t m_trackingMatrixPaletteCount = 0;
  std::atomic<uint64_t> m_frameNumber{0};
};

class AttachmentRigidRegistry {
public:
  static AttachmentRigidRegistry& instance();

  void beginFrame();
  void endFrame();
  void resetMapSession();

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

  // Phase 7.83：reader 高频，writer 低频。
  mutable std::shared_mutex m_mutex;
  std::unordered_map<void*, AttachmentRigidRecord> m_byChildRuntimeModel;
  std::unordered_map<void*, AttachmentRigidRecord> m_byOwnerRuntimeModel;
  std::unordered_map<void*, AttachmentRigidRecord> m_byRootRuntimeModel;
  std::unordered_map<void*, AttachmentRigidRecord> m_byWorldObjectEntry;
  std::unordered_map<void*, AttachmentRigidRecord> m_bySceneNode;
  std::unordered_map<void*, AttachmentRigidRecord> m_byUnitPtr;
  std::unordered_map<uint32_t, AttachmentRigidRecord> m_byHandle;
  std::atomic<uint64_t> m_frameNumber{0};
};

} // namespace model
} // namespace war3
} // namespace dxvk
