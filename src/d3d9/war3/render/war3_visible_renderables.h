#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

#include "war3_render_identity_bridge.h"

namespace dxvk::war3::render {

struct CurrentDrawContractRecord;

inline constexpr uint32_t kInvalidVisibleMeshIndex = 0xFFFFFFFFu;

enum class VisibleRenderableQueueKind : uint8_t {
  MainQueue = 0,
  Transparent = 1,
};

struct VisibleRenderableRecord {
  void *payload = nullptr;
  void *renderablePart = nullptr;
  void *sceneNode = nullptr;
  void *meshData = nullptr;
  void *layerState = nullptr;
  void *runtimeModelPtr = nullptr;
  void *modelResourcePtr = nullptr;
  void *runtimeGeosetPtr = nullptr;
  void *runtimeGeosetDataPtr = nullptr;

  uint64_t modelKey = 0;
  uint32_t flags = 0;
  uint32_t layerIndex = 0;
  uint32_t subIndex = 0;
  uint32_t queueSlot = 0;
  uint32_t transparentType = 0;
  uint32_t transparentSortKey = 0;
  uint32_t meshIndex = kInvalidVisibleMeshIndex;
  uint32_t geosetIndex = kInvalidVisibleMeshIndex;
  float transparentDistanceSq = 0.0f;
  VisibleRenderableQueueKind queueKind = VisibleRenderableQueueKind::MainQueue;

  RenderObjectIdentitySnapshot identity = {};

  bool HasStableIdentity() const {
    return identity.HasStableIdentity() || runtimeModelPtr != nullptr ||
           modelResourcePtr != nullptr || modelKey != 0;
  }

  bool IsTransparent() const {
    return queueKind == VisibleRenderableQueueKind::Transparent;
  }

  bool HasResolvedGeoset() const {
    return geosetIndex != kInvalidVisibleMeshIndex ||
           runtimeGeosetPtr != nullptr || runtimeGeosetDataPtr != nullptr;
  }
};

struct VisibleRenderableModelMetadataCacheEntry {
  void *runtimeModelPtr = nullptr;
  void *modelResourcePtr = nullptr;
  uint64_t modelKey = 0;
};

class VisibleRenderableRegistry {
public:
  struct DebugSummary {
    uint64_t frameNumber = 0;
    uint64_t visibleCount = 0;
    uint64_t mainQueueCount = 0;
    uint64_t transparentCount = 0;
    uint64_t mainQueueRangeCallCount = 0;
    uint64_t mainQueueRangeRecordCount = 0;
    uint64_t semanticCandidateCallCount = 0;
    uint64_t semanticCandidateMergedCount = 0;
    uint64_t semanticCandidateAppendedCount = 0;
    uint64_t transparentEntryCallCount = 0;
  };

  struct ShadowManifestSummary {
    uint64_t frameNumber = 0;
    uint64_t objectCount = 0;
    uint64_t partCount = 0;
    uint64_t stableObjectCount = 0;
    uint64_t newObjectCount = 0;
    uint64_t expiredObjectCount = 0;
    uint64_t freshPartCount = 0;
    uint64_t leaseablePartCount = 0;
    uint64_t poseStalePartCount = 0;
    uint64_t sliceStalePartCount = 0;
    uint64_t expiredPartCount = 0;
    uint64_t multiSlicePartCount = 0;
    uint64_t payload11CChurnCount = 0;
    uint64_t renderablePartChurnCount = 0;
    uint64_t cModelPoseHitCount = 0;
    uint64_t cModelPoseMissCount = 0;
    uint64_t cModelPoseNoRuntimeCount = 0;
    uint64_t cModelPoseLastRuntimeModelPtr = 0;
    uint64_t cModelPoseLastMatrixCount = 0;
    uint64_t cModelPoseLastMatrixHash = 0;
    uint64_t visibleLookupPartLayerHitCount = 0;
    uint64_t visibleLookupSingleFallbackCount = 0;
    uint64_t visibleLookupMissCount = 0;
  };

  struct ShadowManifestPartLeaseInfo {
    uint64_t partKey = 0;
    uint64_t objectKey = 0;
    uint64_t lastSeenFrame = 0;
    uint64_t lastPoseFrame = 0;
    uint64_t lastSliceFrame = 0;
    uint64_t lastGoodPacketFrame = 0;
    void* runtimeModelPtr = nullptr;
    uint64_t poseAgeFrames = 0;
    uint64_t sliceAgeFrames = 0;
    uint64_t packetAgeFrames = 0;
    uint64_t cModelPoseMatrixHash = 0;
    uint32_t cModelPoseMatrixCount = 0;
    uint32_t observedFrameCount = 0;
    bool found = false;
    bool structureLive = false;
    bool poseFresh = false;
    bool sliceFresh = false;
    bool packetFresh = false;
    bool cModelPoseFresh = false;
    bool leaseable = false;
  };

  static VisibleRenderableRegistry &instance();

  void beginFrame();
  void endFrame();

  void registerMainQueueRange(void *batchArray, uint32_t before, uint32_t after,
                              const RenderObjectIdentitySnapshot &identity);
  void registerTransparentEntry(void *payload, uint32_t transparentType,
                                uint32_t queueSlot, uint32_t sortKey,
                                float distanceSq,
                                const RenderObjectIdentitySnapshot &identity);
  bool registerSemanticCandidate(const VisibleRenderableRecord &candidate);

  bool queryByPayload(void *payload, VisibleRenderableRecord &out) const;
  bool queryByRenderablePart(void *renderablePart,
                             VisibleRenderableRecord &out) const;
  bool queryByRenderablePartAndLayer(void *renderablePart,
                                     uint32_t layerIndex,
                                     VisibleRenderableRecord &out) const;
  bool queryByWorldObjectEntry(void *worldObjectEntry,
                               VisibleRenderableRecord &out) const;
  bool queryByHandle(uint32_t jHandle, VisibleRenderableRecord &out) const;
  bool queryBySceneNode(void *sceneNode, VisibleRenderableRecord &out) const;
  bool queryByRuntimeModel(void *runtimeModelPtr,
                           VisibleRenderableRecord &out) const;

  void refreshShadowManifestFromCurrentDraw(
      const std::vector<CurrentDrawContractRecord>& records,
      uint64_t frameNumber);
  void noteShadowManifestPartGoodPacket(uint64_t partKey,
                                        uint64_t frameNumber);
  ShadowManifestPartLeaseInfo queryShadowManifestPartLeaseInfo(
      uint64_t partKey, uint64_t frameNumber) const;
  ShadowManifestSummary queryShadowManifestSummary() const;

  static uint64_t computeShadowManifestObjectKey(
      const CurrentDrawContractRecord& record);
  static uint64_t computeShadowManifestPartKey(
      const CurrentDrawContractRecord& record);

  const std::vector<VisibleRenderableRecord>& getAllVisibleView() const;
  std::vector<VisibleRenderableRecord> getAllVisible() const;
  size_t getVisibleCount() const;
  size_t getMainQueueCount() const;
  size_t getTransparentCount() const;
  DebugSummary queryDebugSummary() const;
  uint64_t getFrameNumber() const {
    return m_frameNumber.load(std::memory_order_relaxed);
  }

  struct Snapshot {
    std::vector<VisibleRenderableRecord> records;
    std::unordered_map<void *, uint32_t> byPayload;
    std::unordered_map<void *, uint32_t> byRenderablePart;
    std::unordered_map<uint64_t, uint32_t> byRenderablePartLayer;
    std::unordered_map<void *, uint32_t> renderablePartRecordCount;
    std::unordered_map<void *, uint32_t> byWorldObjectEntry;
    std::unordered_map<uint32_t, uint32_t> byHandle;
    std::unordered_map<void *, uint32_t> bySceneNode;
    std::unordered_map<void *, uint32_t> byMeshData;
    std::unordered_map<void *, uint32_t> byRuntimeModel;
    std::unordered_map<void *, uint32_t> byRuntimeGeoset;
    std::unordered_map<void *, uint32_t> byRuntimeGeosetData;
    std::unordered_map<void *, VisibleRenderableModelMetadataCacheEntry>
        modelMetadataBySceneNode;
    size_t mainQueueCount = 0;
    size_t transparentCount = 0;
    uint64_t mainQueueRangeCallCount = 0;
    uint64_t mainQueueRangeRecordCount = 0;
    uint64_t semanticCandidateCallCount = 0;
    uint64_t semanticCandidateMergedCount = 0;
    uint64_t semanticCandidateAppendedCount = 0;
    uint64_t transparentEntryCallCount = 0;
    size_t lastRecordCount = 0;
    size_t lastPayloadCount = 0;
    size_t lastRenderablePartCount = 0;
    size_t lastRenderablePartLayerCount = 0;
    size_t lastRenderablePartRecordCount = 0;
    size_t lastWorldObjectCount = 0;
    size_t lastHandleCount = 0;
    size_t lastSceneNodeCount = 0;
    size_t lastMeshDataCount = 0;
    size_t lastRuntimeModelCount = 0;
    size_t lastRuntimeGeosetCount = 0;
    size_t lastRuntimeGeosetDataCount = 0;
    size_t lastModelMetadataCount = 0;
  };

private:
  VisibleRenderableRegistry() = default;

  static constexpr uint32_t kSnapshotCount = 2;
  static constexpr uint64_t kShadowManifestStructureTtlFrames = 30u;
  static constexpr uint64_t kShadowManifestSkinnedPoseTtlFrames = 1u;
  static constexpr uint64_t kShadowManifestSliceTtlFrames = 1u;
  static constexpr uint64_t kShadowManifestLastGoodPacketTtlFrames = 1u;

  struct ShadowManifestObjectEntry {
    uint64_t key = 0;
    uint64_t firstSeenFrame = 0;
    uint64_t lastSeenFrame = 0;
    uint32_t observedFrameCount = 0;
  };

  struct ShadowManifestPartEntry {
    uint64_t key = 0;
    uint64_t objectKey = 0;
    uint64_t firstSeenFrame = 0;
    uint64_t lastSeenFrame = 0;
    uint64_t lastPoseFrame = 0;
    uint64_t lastSliceFrame = 0;
    uint64_t lastGoodPacketFrame = 0;
    void* renderablePart = nullptr;
    void* sceneNode = nullptr;
    void* runtimeModelPtr = nullptr;
    uint32_t layerIndex = 0;
    uint32_t payloadWord108 = 0;
    uint32_t lastPayloadWord11C = 0;
    uint32_t renderablePartChurnCount = 0;
    uint32_t observedFrameCount = 0;
  };

  void appendRecord(Snapshot &snap, VisibleRenderableRecord &record);
  Snapshot &writeSnapshot();
  const Snapshot &readSnapshot() const;
  const Snapshot &snapshotForThread() const;

  std::array<Snapshot, kSnapshotCount> m_snapshots = {};
  std::atomic<uint32_t> m_publishedIndex{0};
  uint32_t m_writeIndex = 0;
  std::thread::id m_renderThreadId = {};
  std::atomic<uint64_t> m_frameNumber{0};
  // Phase 7.96：record cap 触发后置 true，后续 register 调用在入口直接 return。
  std::atomic<bool> m_recordCapReached{false};
  std::unordered_map<uint64_t, ShadowManifestObjectEntry>
      m_shadowManifestObjects;
  std::unordered_map<uint64_t, ShadowManifestPartEntry> m_shadowManifestParts;
  // Phase 7.31 Iteration G 回退：不再维护反向索引（sibling propagation 已默认关闭）。
  // 字段保留结构占位但不再使用（移除以减少 per-instance 开销）。
  ShadowManifestSummary m_shadowManifestSummary = {};
  mutable std::atomic<uint64_t> m_shadowManifestVisibleLookupPartLayerHitCount{
      0};
  mutable std::atomic<uint64_t> m_shadowManifestVisibleLookupSingleFallbackCount{
      0};
  mutable std::atomic<uint64_t> m_shadowManifestVisibleLookupMissCount{0};
};

} // namespace dxvk::war3::render
