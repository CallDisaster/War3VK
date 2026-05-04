#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

#include "war3_render_identity_bridge.h"

namespace dxvk::war3::render {

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
  bool queryByWorldObjectEntry(void *worldObjectEntry,
                               VisibleRenderableRecord &out) const;
  bool queryByHandle(uint32_t jHandle, VisibleRenderableRecord &out) const;
  bool queryBySceneNode(void *sceneNode, VisibleRenderableRecord &out) const;
  bool queryByRuntimeModel(void *runtimeModelPtr,
                           VisibleRenderableRecord &out) const;

  const std::vector<VisibleRenderableRecord>& getAllVisibleView() const;
  std::vector<VisibleRenderableRecord> getAllVisible() const;
  size_t getVisibleCount() const;
  size_t getMainQueueCount() const;
  size_t getTransparentCount() const;
  uint64_t getFrameNumber() const {
    return m_frameNumber.load(std::memory_order_relaxed);
  }

  struct Snapshot {
    std::vector<VisibleRenderableRecord> records;
    std::unordered_map<void *, uint32_t> byPayload;
    std::unordered_map<void *, uint32_t> byRenderablePart;
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
    size_t lastRecordCount = 0;
    size_t lastPayloadCount = 0;
    size_t lastRenderablePartCount = 0;
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
  void appendRecord(Snapshot &snap, VisibleRenderableRecord &record);
  Snapshot &writeSnapshot();
  const Snapshot &readSnapshot() const;
  const Snapshot &snapshotForThread() const;

  std::array<Snapshot, kSnapshotCount> m_snapshots = {};
  std::atomic<uint32_t> m_publishedIndex{0};
  uint32_t m_writeIndex = 0;
  std::thread::id m_renderThreadId = {};
  std::atomic<uint64_t> m_frameNumber{0};
};

} // namespace dxvk::war3::render
