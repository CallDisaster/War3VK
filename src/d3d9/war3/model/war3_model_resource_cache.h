#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace dxvk::war3::model {

inline constexpr uint32_t kInvalidShadowGeosetIndex = 0xFFFFFFFFu;

struct ShadowGeosetPrimitiveRecord {
  uint32_t primitiveTypeOrMaterialSlot = 0;
  uint32_t indexCount = 0;
};

struct ShadowGeosetUvLayerRecord {
  uint32_t uvCount = 0;
  std::vector<float> uvPairs;
};

struct ShadowGeosetResourceRecord {
  void *geosetPtr = nullptr;
  void *geosetDataPtr = nullptr;
  void *modelResourcePtr = nullptr;
  uint64_t modelKey = 0;
  bool prefersRuntimeContract = false;
  uint32_t geosetIndex = kInvalidShadowGeosetIndex;
  uint32_t materialOrLayoutSlot = 0;
  uint32_t layoutOrMaterialSlot = 0;
  uint32_t mergedGeosetSlotOrBindingIndex = 0;

  uint32_t vertexCount = 0;
  std::vector<float> positions;

  uint32_t normalCount = 0;
  std::vector<float> normals;

  uint32_t vertexGroupCount = 0;
  std::vector<uint8_t> vertexGroupIndices;

  uint32_t uvLayerCount = 0;
  std::vector<ShadowGeosetUvLayerRecord> uvLayers;

  uint32_t primitiveCount = 0;
  std::vector<ShadowGeosetPrimitiveRecord> primitiveRecords;

  uint32_t indexCount = 0;
  std::vector<uint16_t> indices;

  uint32_t matrixGroupCount = 0;
  std::vector<uint32_t> matrixGroupSizes;

  uint32_t matrixIndexCount = 0;
  std::vector<uint32_t> matrixIndices;

  uint64_t contentHash = 0;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;
  uint64_t lastRuntimeRefreshFrame = 0;

  bool readyForShadowConsumer() const {
    return !positions.empty() && (!indices.empty() || !primitiveRecords.empty());
  }

  bool hasSkinningData() const {
    return !vertexGroupIndices.empty() &&
           (!matrixGroupSizes.empty() || !matrixIndices.empty());
  }
};

// 消费者载荷以替换方式发布，发布后不再原地修改。长生命周期 GPU 消费者持有该
// 快照即可越过缓存锁的生命周期，无需复制模型的每个 vector。
using ShadowGeosetResourceSnapshot =
    std::shared_ptr<const ShadowGeosetResourceRecord>;

// 面向已持有完整不可变记录的热路径消费者，提供一个只在锁内读取的小型新鲜度证明。
// 这样无需为了证明缓存仍指向同一份内容而复制所有顶点/索引 vector。
struct ShadowGeosetResourceStamp {
  void* geosetDataPtr = nullptr;
  uint64_t contentHash = 0;
  uint32_t vertexCount = 0;
};

struct ShadowModelResourceRecord {
  void *runtimeModelPtr = nullptr;
  void *modelResourcePtr = nullptr;
  uint64_t modelKey = 0;
  uint32_t geosetCount = 0;
  std::vector<void *> geosetPtrs;
  std::vector<void *> geosetDataPtrs;
  uint32_t readyGeosetCount = 0;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;

  bool readyForShadowConsumer() const { return readyGeosetCount != 0; }
};

// 基于 capacity 的账本刻意排除 unordered_map 的节点和桶开销。这里只统计重复
// Game.dll 模型数据的自有载荷数组，采集时无需再次复制这些数组。
struct ShadowModelResourceMemorySnapshot {
  uint64_t uniqueGeosetRecords = 0;
  uint64_t residentGeosetRecords = 0;
  uint64_t uniqueGeosetCapacityBytes = 0;
  uint64_t residentGeosetCapacityBytes = 0;
  uint64_t aliasDuplicateCapacityBytes = 0;
  uint64_t positionsCapacityBytes = 0;
  uint64_t normalsCapacityBytes = 0;
  uint64_t groupSlotsCapacityBytes = 0;
  uint64_t uvCapacityBytes = 0;
  uint64_t primitiveCapacityBytes = 0;
  uint64_t indicesCapacityBytes = 0;
  uint64_t matrixGroupsCapacityBytes = 0;
  uint64_t matrixIndicesCapacityBytes = 0;
  uint64_t modelPointerCapacityBytes = 0;
};

class ShadowModelResourceCache {
public:
  static ShadowModelResourceCache &instance();

  void beginFrame();
  void endFrame();

  void recordGeosetCreate(void *geosetPtr);
  void noteModelResourceBinding(void *modelResourcePtr, uint64_t modelKey = 0);
  void noteRuntimeModelBinding(void *runtimeModelPtr,
                               void *modelResourcePtr = nullptr,
                               uint64_t modelKey = 0);
  void noteRuntimeGeosetBinding(void *runtimeModelPtr, uint32_t geosetIndex,
                                void *runtimeGeosetPtr,
                                void *runtimeGeosetDataPtr = nullptr,
                                void *modelResourcePtr = nullptr,
                                uint64_t modelKey = 0);

  bool findGeosetByPtr(void *geosetPtr, ShadowGeosetResourceRecord &out) const;
  bool findGeosetByData(void *geosetDataPtr,
                        ShadowGeosetResourceRecord &out) const;
  ShadowGeosetResourceSnapshot findGeosetSnapshotByData(
      void* geosetDataPtr) const;
  bool findGeosetStampByData(void* geosetDataPtr,
                             ShadowGeosetResourceStamp& out) const;
  bool hydrateGeosetByKnownPtrs(void* geosetPtr, void* geosetDataPtr,
                                ShadowGeosetResourceRecord& out);
  bool findModelGeoset(void *modelResourcePtr, uint32_t geosetIndex,
                       ShadowGeosetResourceRecord &out) const;
  bool findRuntimeModelGeoset(void *runtimeModelPtr, uint32_t geosetIndex,
                              ShadowGeosetResourceRecord &out) const;
  bool findRuntimeModelOwner(void* runtimeGeosetPtr, void* runtimeGeosetDataPtr,
                             uint32_t geosetIndex, void* modelResourcePtr,
                             ShadowModelResourceRecord& out) const;
  bool findRuntimeModelOwnerIndexed(void* runtimeGeosetPtr,
                                    void* runtimeGeosetDataPtr,
                                    ShadowModelResourceRecord& out) const;
  bool findModelResource(void *modelResourcePtr,
                         ShadowModelResourceRecord &out) const;
  bool findRuntimeModelResource(void *runtimeModelPtr,
                                ShadowModelResourceRecord &out) const;
  bool isDirectModelResourcePtr(void* modelResourcePtr) const;
  void* resolveDirectModelResourcePtr(void* modelResourceOrHandlePtr) const;

  std::vector<ShadowGeosetResourceRecord> snapshotGeosets() const;
  std::vector<ShadowModelResourceRecord> snapshotModels() const;
  std::vector<ShadowModelResourceRecord> snapshotRuntimeModels() const;
  size_t geosetRecordCount() const;
  size_t readyGeosetCount() const;
  size_t modelResourceCount() const;
  size_t runtimeModelRecordCount() const;
  ShadowModelResourceMemorySnapshot memorySnapshot() const;
  uint64_t frameNumber() const;
  uint64_t revision() const;

private:
  ShadowModelResourceCache() = default;

  struct GeosetObservationMetadata {
    uint64_t firstSeenFrame = 0;
    uint64_t lastSeenFrame = 0;
    uint64_t lastRuntimeRefreshFrame = 0;
  };

  void storeGeosetRecord(const ShadowGeosetResourceRecord &record);
  void storeModelRecord(const ShadowModelResourceRecord &record);
  void storeRuntimeModelRecord(const ShadowModelResourceRecord &record);
  void noteGeosetDataMetadataLocked(
      void* geosetDataPtr, const ShadowGeosetResourceRecord& record);
  ShadowGeosetResourceRecord materializeGeosetDataRecordLocked(
      void* geosetDataPtr,
      const ShadowGeosetResourceSnapshot& snapshot) const;

  // Phase 7.83：reader 路径（findGeoset*/findModel*/findRuntime*/snapshot*/*Count）
  // 远多于 writer。改 shared_mutex 让 reader 走 shared_lock，writer 走 unique_lock。
  // m_frameNumber / m_revision 是单调写、reader 高频读，改 atomic 让 const 读
  // 路径走 relaxed load。
  mutable std::shared_mutex m_mutex;
  std::unordered_map<void *, ShadowGeosetResourceRecord> m_byGeoset;
  std::unordered_map<void *, ShadowGeosetResourceSnapshot> m_byGeosetData;
  // 观察时间戳与不可变消费者字节独立变化。将它们移出快照可避免逐帧克隆 vector，
  // 同时保留旧复制型消费者取得的物化字段值。
  std::unordered_map<void *, GeosetObservationMetadata>
      m_geosetDataObservation;
  std::unordered_map<void *, ShadowModelResourceRecord> m_byModelResource;
  std::unordered_map<void *, ShadowModelResourceRecord> m_byRuntimeModel;
  std::unordered_map<void *, void *> m_runtimeOwnerByGeoset;
  std::unordered_map<void *, void *> m_runtimeOwnerByGeosetData;
  std::atomic<uint64_t> m_frameNumber{0};
  std::atomic<uint64_t> m_revision{0};
};

} // namespace dxvk::war3::model
