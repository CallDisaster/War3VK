#pragma once

#include "war3_geoset_local_bounds.h"
#include "war3_immutable_model_generation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
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

enum class ShadowGeosetImmutableCaptureStatus : uint8_t {
  NotAttempted = 0u,
  Complete = 1u,
  AttemptedFailed = 2u,
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
  // Derived once when immutable payload is published. DirectGrouped consumes
  // the same geoset for every instance and must not rescan this array per draw.
  uint32_t maxVertexGroupSlot = 0u;

  uint32_t uvLayerCount = 0;
  std::vector<ShadowGeosetUvLayerRecord> uvLayers;

  uint32_t primitiveCount = 0;
  std::vector<ShadowGeosetPrimitiveRecord> primitiveRecords;

  uint32_t indexCount = 0;
  std::vector<uint16_t> indices;

  uint32_t matrixGroupCount = 0;
  std::vector<uint32_t> matrixGroupSizes;
  uint32_t maxMatrixGroupSize = 0u;

  uint32_t matrixIndexCount = 0;
  std::vector<uint32_t> matrixIndices;

  uint64_t contentHash = 0;
  // Map-scoped source authority minted by ShadowModelResourceCache.  The
  // process-monotonic immutable generation identifies bytes; this epoch
  // additionally proves that the address was captured in the active map.
  uint64_t mapEpoch = 0;
  // Minted only by ShadowModelResourceCache under its unique writer lock.
  // Callers may carry this value, but incoming values are never trusted or
  // merged when the cache publishes a replacement immutable snapshot.
  uint64_t immutableModelGeneration = 0;
  // Valid only as a derivative of positions belonging to the generation
  // above.  It is cleared on every unresolved/failed publication and is never
  // accepted from an incoming runtime record.
  ShadowGeosetLocalBounds localBounds = {};
  ShadowGeosetImmutableCaptureStatus immutableCaptureStatus =
      ShadowGeosetImmutableCaptureStatus::NotAttempted;
  uint64_t firstSeenFrame = 0;
  uint64_t lastSeenFrame = 0;
  uint64_t lastRuntimeRefreshFrame = 0;

  bool hasCompleteImmutableConsumerPayload() const {
    return immutableCaptureStatus ==
            ShadowGeosetImmutableCaptureStatus::Complete &&
        !positions.empty() &&
        (!indices.empty() || !primitiveRecords.empty());
  }

  bool readyForShadowConsumer() const {
    return immutableModelGeneration != 0u &&
        hasCompleteImmutableConsumerPayload();
  }

  bool hasSkinningData() const {
    return !vertexGroupIndices.empty() &&
           (!matrixGroupSizes.empty() || !matrixIndices.empty());
  }
};

inline bool SameShadowGeosetFloatPayloadBytes(
    const std::vector<float>& lhs, const std::vector<float>& rhs) noexcept {
  return lhs.size() == rhs.size() &&
      (lhs.empty() || std::memcmp(lhs.data(), rhs.data(),
          lhs.size() * sizeof(float)) == 0);
}

// Exact immutable payload equality used by the cache generation authority.
// Pointer aliases, owner/model metadata and observation timestamps are
// deliberately excluded: completing those fields does not change bytes a GPU
// package would derive from this snapshot.  Every model stream that can affect
// geometry, primitive topology or palette addressing remains included.
inline bool SameShadowGeosetImmutableConsumerPayload(
    const ShadowGeosetResourceRecord& lhs,
    const ShadowGeosetResourceRecord& rhs) {
  if (lhs.vertexCount != rhs.vertexCount ||
      !SameShadowGeosetFloatPayloadBytes(lhs.positions, rhs.positions) ||
      lhs.normalCount != rhs.normalCount ||
      !SameShadowGeosetFloatPayloadBytes(lhs.normals, rhs.normals) ||
      lhs.vertexGroupCount != rhs.vertexGroupCount ||
      lhs.vertexGroupIndices != rhs.vertexGroupIndices ||
      lhs.uvLayerCount != rhs.uvLayerCount ||
      lhs.uvLayers.size() != rhs.uvLayers.size() ||
      lhs.primitiveCount != rhs.primitiveCount ||
      lhs.primitiveRecords.size() != rhs.primitiveRecords.size() ||
      lhs.indexCount != rhs.indexCount ||
      lhs.indices != rhs.indices ||
      lhs.matrixGroupCount != rhs.matrixGroupCount ||
      lhs.matrixGroupSizes != rhs.matrixGroupSizes ||
      lhs.matrixIndexCount != rhs.matrixIndexCount ||
      lhs.matrixIndices != rhs.matrixIndices) {
    return false;
  }

  for (size_t i = 0u; i < lhs.uvLayers.size(); ++i) {
    if (lhs.uvLayers[i].uvCount != rhs.uvLayers[i].uvCount ||
        !SameShadowGeosetFloatPayloadBytes(
            lhs.uvLayers[i].uvPairs, rhs.uvLayers[i].uvPairs))
      return false;
  }
  for (size_t i = 0u; i < lhs.primitiveRecords.size(); ++i) {
    if (lhs.primitiveRecords[i].primitiveTypeOrMaterialSlot !=
            rhs.primitiveRecords[i].primitiveTypeOrMaterialSlot ||
        lhs.primitiveRecords[i].indexCount !=
            rhs.primitiveRecords[i].indexCount) {
      return false;
    }
  }
  return true;
}

// 消费者载荷以替换方式发布，发布后不再原地修改。长生命周期 GPU 消费者持有该
// 快照即可越过缓存锁的生命周期，无需复制模型的每个 vector。
using ShadowGeosetResourceSnapshot =
    std::shared_ptr<const ShadowGeosetResourceRecord>;

// 面向已持有完整不可变记录的热路径消费者，提供一个只在锁内读取的小型新鲜度证明。
// 这样无需为了证明缓存仍指向同一份内容而复制所有顶点/索引 vector。
struct ShadowGeosetResourceStamp {
  void* geosetDataPtr = nullptr;
  uint64_t contentHash = 0;
  uint64_t mapEpoch = 0;
  uint64_t immutableModelGeneration = 0;
  uint32_t vertexCount = 0;
  ShadowGeosetImmutableCaptureStatus immutableCaptureStatus =
      ShadowGeosetImmutableCaptureStatus::NotAttempted;
};

// Scalar projection used by manifest identity hydration.  It deliberately
// carries no geometry arrays: readiness remains proven against the canonical
// immutable cache publication while callers copy only the identity fields
// they actually consume.
struct ShadowReadyGeosetBinding {
  void* geosetPtr = nullptr;
  void* geosetDataPtr = nullptr;
  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0u;
  uint32_t geosetIndex = kInvalidShadowGeosetIndex;
};

static_assert(std::is_trivially_copyable_v<ShadowReadyGeosetBinding>);

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

// Hot-path owner lookups need identity, not the model's geoset pointer arrays.
// Keep this projection trivially copyable so DirectGrouped and manifest
// hydration can resolve an owner without cloning the full model record.
struct ShadowRuntimeModelOwnerBinding {
  void* runtimeModelPtr = nullptr;
  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0u;
  uint32_t geosetCount = 0u;
};

static_assert(std::is_trivially_copyable_v<ShadowRuntimeModelOwnerBinding>);

// ResourceStore alias binding only consumes these three scalar fields.  A
// dedicated snapshot prevents that hot path from cloning the full model
// record and its geoset pointer vectors merely to enumerate alias slots.
struct ShadowModelAliasSnapshotRecord {
  void* runtimeModelPtr = nullptr;
  void* modelResourcePtr = nullptr;
  uint32_t geosetCount = 0u;
};

static_assert(std::is_trivially_copyable_v<ShadowModelAliasSnapshotRecord>);

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

struct ShadowResourceStoreCapacityHint {
  size_t geosetRecordUpperBound = 0u;
  size_t runtimeAliasUpperBound = 0u;
};

class ShadowModelResourceCache {
public:
  // Cache snapshots are immutable once published, but legacy hot paths may
  // reuse a ready pointer without re-reading Game.dll and the cache is not map
  // epoch scoped. This proves cache-content identity only; a future renderer
  // consumer still requires an exact current Stage11 source token.
  static constexpr bool kImmutableGenerationProvesCurrentGameMemory = false;
  static constexpr bool kMapEpochScopedSourceAuthorityIntegrated = true;

  static ShadowModelResourceCache &instance();

  void beginFrame();
  void endFrame();
  // Drops every address alias for the old map while allowing shared immutable
  // snapshots already owned by GPU retirement records to outlive the lookup
  // table. The immutable generation issuer is deliberately not reset.
  bool resetMapEpoch(uint64_t nextMapEpoch);

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
  bool findReadyGeosetBindingByPtr(
      void* geosetPtr, ShadowReadyGeosetBinding& out) const;
  bool findReadyGeosetBindingByData(
      void* geosetDataPtr, ShadowReadyGeosetBinding& out) const;
  ShadowGeosetResourceSnapshot findGeosetSnapshotByData(
      void* geosetDataPtr) const;
  bool findGeosetStampByData(void* geosetDataPtr,
                             ShadowGeosetResourceStamp& out) const;
  bool findGeosetStampByDataForEpoch(
      void* geosetDataPtr, uint64_t expectedMapEpoch,
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
  bool findRuntimeModelOwnerBinding(
      void* runtimeGeosetPtr, void* runtimeGeosetDataPtr,
      uint32_t geosetIndex, void* modelResourcePtr,
      ShadowRuntimeModelOwnerBinding& out) const;
  bool findRuntimeModelOwnerBindingIndexed(
      void* runtimeGeosetPtr, void* runtimeGeosetDataPtr,
      ShadowRuntimeModelOwnerBinding& out) const;
  bool findModelBinding(void* modelResourcePtr,
                        ShadowRuntimeModelOwnerBinding& out) const;
  bool findRuntimeModelBinding(
      void* runtimeModelPtr, ShadowRuntimeModelOwnerBinding& out) const;
  bool findModelResource(void *modelResourcePtr,
                         ShadowModelResourceRecord &out) const;
  bool findRuntimeModelResource(void *runtimeModelPtr,
                                ShadowModelResourceRecord &out) const;
  bool isDirectModelResourcePtr(void* modelResourcePtr) const;
  void* resolveDirectModelResourcePtr(void* modelResourceOrHandlePtr) const;

  std::vector<ShadowGeosetResourceRecord> snapshotGeosets() const;
  // Invokes fn(payload, alias) once for every record represented by
  // snapshotGeosets, but without cloning payload vectors into an intermediate
  // container. References are valid only for the duration of the callback;
  // callbacks must not re-enter this cache while the shared lock is held.
  template <typename Fn>
  void forEachGeosetContractSource(Fn&& fn) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (const auto& it : m_byGeoset) {
      const ShadowGeosetResourceRecord& alias = it.second;
      if (alias.readyForShadowConsumer() &&
          alias.geosetDataPtr != nullptr) {
        const auto canonical = m_byGeosetData.find(alias.geosetDataPtr);
        if (canonical != m_byGeosetData.end() &&
            canonical->second != nullptr) {
          fn(*canonical->second, &alias);
          continue;
        }
      }
      fn(alias, nullptr);
    }
    for (const auto& it : m_byGeosetData) {
      if (it.second == nullptr)
        continue;
      const ShadowGeosetResourceRecord& record = *it.second;
      if (record.geosetPtr != nullptr &&
          m_byGeoset.find(record.geosetPtr) != m_byGeoset.end()) {
        continue;
      }
      fn(record, nullptr);
    }
  }
  std::vector<ShadowModelResourceRecord> snapshotModels() const;
  std::vector<ShadowModelResourceRecord> snapshotRuntimeModels() const;
  std::vector<ShadowModelAliasSnapshotRecord> snapshotModelAliases() const;
  std::vector<ShadowModelAliasSnapshotRecord>
  snapshotRuntimeModelAliases() const;
  // ResourceStore hydration consumes aliases immediately and never re-enters
  // this cache. Enumerate the same model-resource-then-runtime-model sequence
  // under one shared lock, avoiding two temporary vector allocations on every
  // resource-store rebuild. Callback references are value projections and are
  // valid only for the duration of the call.
  template <typename Fn>
  void forEachResourceStoreAlias(Fn&& fn) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (const auto& entry : m_byModelResource) {
      const auto& record = entry.second;
      fn(ShadowModelAliasSnapshotRecord{
          record.runtimeModelPtr, record.modelResourcePtr,
          record.geosetCount});
    }
    for (const auto& entry : m_byRuntimeModel) {
      const auto& record = entry.second;
      fn(ShadowModelAliasSnapshotRecord{
          record.runtimeModelPtr, record.modelResourcePtr,
          record.geosetCount});
    }
  }
  size_t geosetRecordCount() const;
  size_t readyGeosetCount() const;
  size_t modelResourceCount() const;
  size_t runtimeModelRecordCount() const;
  ShadowResourceStoreCapacityHint resourceStoreCapacityHint() const;
  ShadowModelResourceMemorySnapshot memorySnapshot() const;
  uint64_t frameNumber() const;
  uint64_t revision() const;
  uint64_t mapEpoch() const;

private:
  ShadowModelResourceCache() = default;

  struct GeosetObservationMetadata {
    uint64_t firstSeenFrame = 0;
    uint64_t lastSeenFrame = 0;
    uint64_t lastRuntimeRefreshFrame = 0;
  };

  ShadowGeosetResourceSnapshot storeGeosetRecord(
      const ShadowGeosetResourceRecord &record);
  void storeModelRecord(const ShadowModelResourceRecord &record);
  void storeRuntimeModelRecord(const ShadowModelResourceRecord &record);
  void noteGeosetDataMetadataLocked(
      void* geosetDataPtr, const ShadowGeosetResourceRecord& record);
  ShadowGeosetResourceRecord materializeGeosetDataRecordLocked(
      void* geosetDataPtr,
      const ShadowGeosetResourceSnapshot& snapshot) const;
  ShadowGeosetResourceRecord materializeGeosetAliasRecordLocked(
      const ShadowGeosetResourceRecord& alias) const;

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
  std::atomic<uint64_t> m_mapEpoch{1u};
  // Process-lifetime source-generation authority.  This member is neither
  // reset by map/device changes nor reachable outside the cache writer path.
  ImmutableModelGenerationIssuer m_immutableModelGenerations;
};

} // namespace dxvk::war3::model

static_assert(!dxvk::war3::model::ShadowModelResourceCache::
    kImmutableGenerationProvesCurrentGameMemory);
static_assert(dxvk::war3::model::ShadowModelResourceCache::
    kMapEpochScopedSourceAuthorityIntegrated);
