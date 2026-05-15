#pragma once

#include "../render/war3_render_objects.h"
#include "../render/war3_visible_renderables.h"

#include "../../util/util_matrix.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace dxvk::war3::shadow {

inline constexpr uint32_t kInvalidShadowContractGeosetIndex = 0xFFFFFFFFu;

struct ShadowRenderableRecord {
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* unitPtr = nullptr;
  void* renderablePart = nullptr;
  void* payload = nullptr;
  void* meshData = nullptr;
  void* layerState = nullptr;
  void* runtimeModelPtr = nullptr;
  void* modelResourcePtr = nullptr;
  void* runtimeGeosetPtr = nullptr;
  void* runtimeGeosetDataPtr = nullptr;
  uint64_t modelKey = 0;
  uint32_t flags = 0;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  uint32_t unitFlags5C = 0;
  uint32_t layerIndex = 0;
  uint32_t subIndex = 0;
  uint32_t transparentType = 0;
  uint32_t transparentSortKey = 0;
  uint32_t meshIndex = kInvalidShadowContractGeosetIndex;
  uint32_t geosetIndex = kInvalidShadowContractGeosetIndex;
  render::ObjectKind objectKind = render::ObjectKind::Unknown;
  render::VisibleRenderableQueueKind queueKind =
      render::VisibleRenderableQueueKind::MainQueue;
  int8_t groupIdx = -1;
  uint64_t frameSerial = 0;

  bool hasStableIdentity() const {
    return worldObjectEntry != nullptr || sceneNode != nullptr ||
           unitPtr != nullptr ||
           runtimeModelPtr != nullptr || modelResourcePtr != nullptr ||
           modelKey != 0 || jHandle != 0 || rawcode != 0;
  }

  bool hasResolvedGeoset() const {
    return geosetIndex != kInvalidShadowContractGeosetIndex ||
           runtimeGeosetPtr != nullptr || runtimeGeosetDataPtr != nullptr;
  }
};

struct ShadowFrameManifest {
  uint64_t frameSerial = 0;
  uint64_t publishRevision = 0;
  uint64_t visibleCount = 0;
  uint64_t mainQueueCount = 0;
  uint64_t transparentCount = 0;
  std::vector<ShadowRenderableRecord> records;
};

struct ShadowPrimitiveRecord {
  uint32_t primitiveTypeOrMaterialSlot = 0;
  uint32_t indexCount = 0;
};

struct ShadowModelResourceRecord {
  void* runtimeGeosetPtr = nullptr;
  void* runtimeGeosetDataPtr = nullptr;
  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0;
  bool prefersRuntimeContract = false;
  uint32_t geosetIndex = kInvalidShadowContractGeosetIndex;
  uint32_t vertexCount = 0;
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<uint8_t> vertexGroupIndices;
  std::vector<ShadowPrimitiveRecord> primitiveRecords;
  std::vector<uint32_t> matrixGroupSizes;
  std::vector<uint32_t> matrixIndices;
  std::vector<uint16_t> indices;
  std::vector<std::vector<float>> uvLayers;
  uint64_t contentHash = 0;
  uint64_t frameSerial = 0;

  bool readyForConsumer() const {
    return !positions.empty() && (!indices.empty() || vertexCount != 0);
  }

  bool hasSkinningData() const {
    return !vertexGroupIndices.empty() &&
           (!matrixGroupSizes.empty() || !matrixIndices.empty());
  }
};

class ShadowModelResourceStore {
public:
  void clear();
  void add(ShadowModelResourceRecord record);
  void bindRuntimeModelAlias(void* runtimeModelPtr, uint32_t geosetIndex,
                             void* modelResourcePtr);

  bool findByRuntimeGeoset(void* runtimeGeosetPtr,
                           ShadowModelResourceRecord& out) const;
  const ShadowModelResourceRecord* findByRuntimeGeoset(
      void* runtimeGeosetPtr) const;
  bool findByRuntimeGeosetData(void* runtimeGeosetDataPtr,
                               ShadowModelResourceRecord& out) const;
  const ShadowModelResourceRecord* findByRuntimeGeosetData(
      void* runtimeGeosetDataPtr) const;
  bool findByRuntimeModel(void* runtimeModelPtr, uint32_t geosetIndex,
                          ShadowModelResourceRecord& out) const;
  const ShadowModelResourceRecord* findByRuntimeModel(
      void* runtimeModelPtr, uint32_t geosetIndex) const;
  bool findByModelResource(void* modelResourcePtr, uint32_t geosetIndex,
                           ShadowModelResourceRecord& out) const;
  const ShadowModelResourceRecord* findByModelResource(
      void* modelResourcePtr, uint32_t geosetIndex) const;

  const std::vector<ShadowModelResourceRecord>& records() const {
    return m_records;
  }

private:
  struct ModelGeosetKey {
    void* ptr = nullptr;
    uint32_t geosetIndex = kInvalidShadowContractGeosetIndex;

    bool operator==(const ModelGeosetKey& other) const {
      return ptr == other.ptr && geosetIndex == other.geosetIndex;
    }
  };

  struct ModelGeosetKeyHash {
    size_t operator()(const ModelGeosetKey& key) const;
  };

  std::vector<ShadowModelResourceRecord> m_records;
  std::unordered_map<void*, size_t> m_byRuntimeGeoset;
  std::unordered_map<void*, size_t> m_byRuntimeGeosetData;
  std::unordered_map<ModelGeosetKey, size_t, ModelGeosetKeyHash>
      m_byRuntimeModel;
  std::unordered_map<ModelGeosetKey, size_t, ModelGeosetKeyHash>
      m_byModelResource;
};

struct ShadowPoseRecord {
  void* runtimeModelPtr = nullptr;
  void* sceneNode = nullptr;
  void* unitPtr = nullptr;
  uint32_t matrixCount = 0;
  uint64_t matrixHash = 0;
  std::vector<Matrix4> matrixPalette;
  bool hasWorldTransform = false;
  Matrix4 worldTransform;
  uint64_t frameSerial = 0;
};

class ShadowPoseStore {
public:
  void clear();
  void reserve(size_t count);
  void add(ShadowPoseRecord record);

  bool findByRuntimeModel(void* runtimeModelPtr, ShadowPoseRecord& out) const;
  bool findBySceneNode(void* sceneNode, ShadowPoseRecord& out) const;
  bool findByUnitPtr(void* unitPtr, ShadowPoseRecord& out) const;
  const ShadowPoseRecord* findByRuntimeModelPtr(void* runtimeModelPtr) const;
  const ShadowPoseRecord* findBySceneNodePtr(void* sceneNode) const;
  const ShadowPoseRecord* findByUnitPtrPtr(void* unitPtr) const;

  const std::vector<ShadowPoseRecord>& records() const {
    return m_records;
  }

private:
  std::vector<ShadowPoseRecord> m_records;
  std::unordered_map<void*, size_t> m_byRuntimeModel;
  std::unordered_map<void*, size_t> m_bySceneNode;
  std::unordered_map<void*, size_t> m_byUnitPtr;
};

struct ShadowAttachmentRigidRecord {
  void* rootRuntimeModelPtr = nullptr;
  void* ownerRuntimeModelPtr = nullptr;
  void* childRuntimeModelPtr = nullptr;
  void* childSpritePtr = nullptr;
  void* childModelResourcePtr = nullptr;
  uint64_t childModelKey = 0;
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
  uint64_t frameSerial = 0;
};

class ShadowAttachmentRigidStore {
public:
  void clear();
  void reserve(size_t count);
  void add(ShadowAttachmentRigidRecord record);

  bool findByChildRuntimeModel(void* childRuntimeModelPtr,
                               ShadowAttachmentRigidRecord& out) const;
  bool findByChildSpritePtr(void* childSpritePtr,
                            ShadowAttachmentRigidRecord& out) const;
  bool findByOwnerRuntimeModel(void* ownerRuntimeModelPtr,
                               ShadowAttachmentRigidRecord& out) const;
  bool findByRootRuntimeModel(void* rootRuntimeModelPtr,
                              ShadowAttachmentRigidRecord& out) const;
  bool findByAnyRuntimeModel(void* runtimeModelPtr,
                             ShadowAttachmentRigidRecord& out) const;
  bool findUniqueByChildModelResource(void* modelResourcePtr, uint64_t modelKey,
                                      ShadowAttachmentRigidRecord& out) const;
  bool findUniqueWithAnyIdentity(ShadowAttachmentRigidRecord& out) const;
  bool findByWorldObjectEntry(void* worldObjectEntry,
                              ShadowAttachmentRigidRecord& out) const;
  bool findBySceneNode(void* sceneNode,
                       ShadowAttachmentRigidRecord& out) const;
  bool findByUnitPtr(void* unitPtr, ShadowAttachmentRigidRecord& out) const;
  bool findByHandle(uint32_t jHandle, ShadowAttachmentRigidRecord& out) const;
  bool findBySourceObject(void* sourceObjectPtr,
                          ShadowAttachmentRigidRecord& out) const;
  bool findBySourceSpriteObject(void* sourceSpriteObjectPtr,
                                ShadowAttachmentRigidRecord& out) const;

  const std::vector<ShadowAttachmentRigidRecord>& records() const {
    return m_records;
  }

private:
  std::vector<ShadowAttachmentRigidRecord> m_records;
  std::unordered_map<void*, size_t> m_byChildRuntimeModel;
  std::unordered_map<void*, size_t> m_byChildSpritePtr;
  std::unordered_map<void*, size_t> m_byOwnerRuntimeModel;
  std::unordered_map<void*, size_t> m_byRootRuntimeModel;
  std::unordered_map<void*, size_t> m_byWorldObjectEntry;
  std::unordered_map<void*, size_t> m_bySceneNode;
  std::unordered_map<void*, size_t> m_byUnitPtr;
  std::unordered_map<uint32_t, size_t> m_byHandle;
  std::unordered_map<void*, size_t> m_bySourceObject;
  std::unordered_map<void*, size_t> m_bySourceSpriteObject;
};

struct ShadowFrameStats {
  uint64_t frameSerial = 0;
  uint64_t publishRevision = 0;
  uint64_t visibleCount = 0;
  uint64_t mainQueueCount = 0;
  uint64_t transparentCount = 0;
  uint64_t shadowReadyGeosetCount = 0;
  uint64_t shadowRuntimeModelCount = 0;
  uint64_t matrixPaletteCount = 0;
  uint64_t attachmentRigidCount = 0;
  uint64_t visibleDirectUnitCandidateAccepted = 0;
  uint64_t visibleDirectUnitRejectedNotUnitLike = 0;
  uint64_t visibleDirectUnitRejectedGroup = 0;
  uint64_t visibleDirectUnitRejectedNoUnitPtr = 0;
  uint64_t visibleDirectUnitRejectedNoIdentity = 0;
  uint64_t visibleDirectUnitRejectedNoMesh = 0;
  uint64_t visibleDirectUnitRejectedBuilding = 0;
  uint64_t visibleDirectUnitRejectedNoGeoset = 0;
  uint64_t contractCaptureSkippedStableSameFrame = 0;
  uint64_t contractCaptureSkippedEmpty = 0;
  uint64_t contractCaptureSkippedDuplicateSameFrame = 0;
  // Phase 7.97: ManifestCopy 诊断 — 跟踪本次 capture 实际遍历多少 visible
  // record 与最终 push 进 manifest 的数量。这是桥/斜坡场景下 ManifestCopy
  // 2.2ms/frame 的根因定位入口，让 control-plane 直接看到爆炸倍数。
  uint64_t manifestCopyVisibleScanned = 0;
  uint64_t manifestCopyAppended = 0;
  uint64_t manifestCopyDeduplicatedSkipped = 0;
  uint64_t manifestCopyRejectedSkipped = 0;
  uint64_t rootUnitSupplementSeedCount = 0;
  uint64_t rootUnitSupplementUnitSeedCount = 0;
  uint64_t rootUnitSupplementSkippedNoIdentity = 0;
  uint64_t rootUnitSupplementSkippedAttachmentChild = 0;
  uint64_t rootUnitSupplementSkippedNoPose = 0;
  uint64_t rootUnitSupplementSkippedNoResource = 0;
  uint64_t rootUnitSupplementSkippedNoGeoset = 0;
  uint64_t rootUnitSupplementSkippedNoGeosetZeroCount = 0;
  uint64_t rootUnitSupplementSkippedNoGeosetStoreMiss = 0;
  uint64_t rootUnitSupplementSkippedNoGeosetNotReady = 0;
  uint64_t rootUnitSupplementSkippedDuplicate = 0;
  uint64_t rootUnitSupplementAppended = 0;
  uint64_t rootUnitSupplementReusedFromPrior = 0;
  uint64_t rootUnitSupplementResourceCacheMiss = 0;
  uint64_t rootUnitSupplementResourceCacheNotReady = 0;
  uint64_t rootUnitSupplementResourceSemanticKeyResolved = 0;
  uint64_t rootUnitSupplementResourceSemanticKeyReady = 0;
  uint64_t rootUnitSupplementGeosetCacheFallback = 0;
  uint64_t directPoseSupplementAttemptCount = 0;
  uint64_t directPoseSupplementResolvedCount = 0;
  uint64_t directPoseSupplementSkippedExisting = 0;
  uint64_t directPoseSupplementSkippedInvalid = 0;
  uint64_t upperLayerResolveAttempts = 0;
  uint64_t upperLayerResolveAuthoritativeRigid = 0;
  uint64_t upperLayerResolveAuthoritativeSkinned = 0;
  uint64_t upperLayerEmitted = 0;
};

struct ShadowPublishedContractBundle {
  std::shared_ptr<const ShadowFrameManifest> manifest;
  std::shared_ptr<const ShadowModelResourceStore> resources;
  std::shared_ptr<const ShadowPoseStore> poses;
  std::shared_ptr<const ShadowAttachmentRigidStore> attachments;
  ShadowFrameStats stats = {};

  bool valid() const {
    return manifest != nullptr && resources != nullptr && poses != nullptr &&
           attachments != nullptr;
  }
};

class ShadowRuntimeContractCache {
public:
  static ShadowRuntimeContractCache& instance();

  void beginFrame();
  void captureLiveState();
  void capturePoseOnlyLiveState();

  ShadowFrameManifest snapshotManifest() const;
  std::shared_ptr<const ShadowFrameManifest> snapshotManifestShared() const;
  ShadowModelResourceStore snapshotResources() const;
  std::shared_ptr<const ShadowModelResourceStore> snapshotResourcesShared() const;
  ShadowPoseStore snapshotPoses() const;
  std::shared_ptr<const ShadowPoseStore> snapshotPosesShared() const;
  ShadowAttachmentRigidStore snapshotAttachments() const;
  std::shared_ptr<const ShadowAttachmentRigidStore>
      snapshotAttachmentsShared() const;
  ShadowFrameStats snapshotStats() const;
  ShadowPublishedContractBundle snapshotBundleShared() const;

private:
  ShadowRuntimeContractCache() = default;

  // Phase 7.87：reader 路径（snapshot*/snapshotShared/snapshotStats/buildState）
  // 每帧多次，writer 路径（captureLiveState/capturePoseOnly/reset）每帧 1 次。
  mutable std::shared_mutex m_mutex;
  std::shared_ptr<ShadowFrameManifest> m_manifest =
      std::make_shared<ShadowFrameManifest>();
  std::shared_ptr<ShadowModelResourceStore> m_resources =
      std::make_shared<ShadowModelResourceStore>();
  std::shared_ptr<ShadowPoseStore> m_poses =
      std::make_shared<ShadowPoseStore>();
  std::shared_ptr<ShadowAttachmentRigidStore> m_attachments =
      std::make_shared<ShadowAttachmentRigidStore>();
  ShadowFrameStats m_stats = {};
  uint64_t m_resourceRevision = 0;
  uint64_t m_resourceRefreshFrameSerial = 0;
  uint64_t m_publishRevision = 0;

  // Phase 7.97：ManifestCopy 实时诊断 atomic counter（每次 captureLiveState
  // 进入 ManifestCopy 都覆盖更新，与 m_stats 不同：m_stats 受 sameFrameDataNotGrowing
  // 早退影响，可能多帧不变；这两个 counter 永远反映最近一次实际遍历）。
  mutable std::atomic<uint64_t> m_lastManifestCopyVisibleScanned{0};
  mutable std::atomic<uint64_t> m_lastManifestCopyAppended{0};
  mutable std::atomic<uint64_t> m_lastManifestCopyDeduplicatedSkipped{0};
  mutable std::atomic<uint64_t> m_lastManifestCopyRejectedSkipped{0};
  mutable std::atomic<uint64_t> m_manifestCopyEnterCount{0};
  mutable std::atomic<uint64_t> m_manifestCopySkipStableCount{0};
  // 累积 max，避免短暂触发 Scanned=0 时被覆盖。
  mutable std::atomic<uint64_t> m_manifestCopyMaxScanned{0};
  mutable std::atomic<uint64_t> m_manifestCopyTotalScanned{0};
  // Phase 7.97：用 std::chrono 直接测 ManifestCopy 墙钟时间，绕开 perf monitor。
  mutable std::atomic<uint64_t> m_manifestCopyTotalChronoNs{0};
  mutable std::atomic<uint64_t> m_manifestCopyMaxChronoNs{0};
public:
  // Phase 7.97 reader：bridge 透传时拉取最近一次 ManifestCopy 的实际遍历
  // 与 publish 跳过统计。
  uint64_t lastManifestCopyVisibleScanned() const {
    return m_lastManifestCopyVisibleScanned.load(std::memory_order_relaxed);
  }
  uint64_t lastManifestCopyAppended() const {
    return m_lastManifestCopyAppended.load(std::memory_order_relaxed);
  }
  uint64_t lastManifestCopyDeduplicatedSkipped() const {
    return m_lastManifestCopyDeduplicatedSkipped.load(std::memory_order_relaxed);
  }
  uint64_t lastManifestCopyRejectedSkipped() const {
    return m_lastManifestCopyRejectedSkipped.load(std::memory_order_relaxed);
  }
  uint64_t manifestCopyEnterCount() const {
    return m_manifestCopyEnterCount.load(std::memory_order_relaxed);
  }
  uint64_t manifestCopySkipStableCount() const {
    return m_manifestCopySkipStableCount.load(std::memory_order_relaxed);
  }
  uint64_t manifestCopyMaxScanned() const {
    return m_manifestCopyMaxScanned.load(std::memory_order_relaxed);
  }
  uint64_t manifestCopyTotalScanned() const {
    return m_manifestCopyTotalScanned.load(std::memory_order_relaxed);
  }
  uint64_t manifestCopyTotalChronoNs() const {
    return m_manifestCopyTotalChronoNs.load(std::memory_order_relaxed);
  }
  uint64_t manifestCopyMaxChronoNs() const {
    return m_manifestCopyMaxChronoNs.load(std::memory_order_relaxed);
  }
};

} // namespace dxvk::war3::shadow
