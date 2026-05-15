#pragma once

#include <atomic>
#include <cstdint>
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
  bool hydrateGeosetByKnownPtrs(void* geosetPtr, void* geosetDataPtr,
                                ShadowGeosetResourceRecord& out);
  const ShadowGeosetResourceRecord* findGeosetByPtrRef(void* geosetPtr) const;
  const ShadowGeosetResourceRecord* findGeosetByDataRef(
      void* geosetDataPtr) const;
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
  uint64_t frameNumber() const;
  uint64_t revision() const;

private:
  ShadowModelResourceCache() = default;

  void storeGeosetRecord(const ShadowGeosetResourceRecord &record);
  void storeModelRecord(const ShadowModelResourceRecord &record);
  void storeRuntimeModelRecord(const ShadowModelResourceRecord &record);

  // Phase 7.83：reader 路径（findGeoset*/findModel*/findRuntime*/snapshot*/*Count）
  // 远多于 writer。改 shared_mutex 让 reader 走 shared_lock，writer 走 unique_lock。
  // m_frameNumber / m_revision 是单调写、reader 高频读，改 atomic 让 const 读
  // 路径走 relaxed load。
  mutable std::shared_mutex m_mutex;
  std::unordered_map<void *, ShadowGeosetResourceRecord> m_byGeoset;
  std::unordered_map<void *, ShadowGeosetResourceRecord> m_byGeosetData;
  std::unordered_map<void *, ShadowModelResourceRecord> m_byModelResource;
  std::unordered_map<void *, ShadowModelResourceRecord> m_byRuntimeModel;
  std::unordered_map<void *, void *> m_runtimeOwnerByGeoset;
  std::unordered_map<void *, void *> m_runtimeOwnerByGeosetData;
  std::atomic<uint64_t> m_frameNumber{0};
  std::atomic<uint64_t> m_revision{0};
};

} // namespace dxvk::war3::model
