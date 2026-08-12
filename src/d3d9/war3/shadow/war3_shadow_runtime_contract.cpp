#include "war3_shadow_runtime_contract.h"

#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../core/war3_memory.h"
#include "../game/war3_agent.h"
#include "../game/war3_unit.h"
#include "../model/war3_model_hook.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "../render/war3_render_objects.h"
#include "../render/war3_render_identity_bridge.h"
#include "../render/war3_shadow_object_registry.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../render/war3_visible_renderables.h"
#include "../tools/war3_perf_monitor.h"
#include "war3_shadow_renderer_core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace dxvk::war3::shadow {

namespace {

class SemanticPerfScope {
public:
  explicit SemanticPerfScope(render::SemanticDataPerfTag tag)
      : m_tag(tag), m_start(std::chrono::steady_clock::now()) {
  }

  ~SemanticPerfScope() {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_start)
            .count();
    render::NoteSemanticDataPerf(
        m_tag, elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0u);
  }

private:
  render::SemanticDataPerfTag m_tag;
  std::chrono::steady_clock::time_point m_start;
};

dxvk::war3::War3PerfMonitor::ScopedCpuScope ContractCpuScope(
    const char* name) {
  return dxvk::war3::War3PerfMonitor::instance().cpuScope(name);
}

struct PointerBoolCacheEntry {
  void* ptr = nullptr;
  uint64_t mapEpoch = 0u;
  bool value = false;
};

struct ManifestResolveDiagnostics {
  uint64_t sourceCompleteSkipCount = 0u;
  uint64_t legacyCacheHitCount = 0u;
  uint64_t rawScanCount = 0u;
  uint64_t rawScanEntryVisitCount = 0u;
  uint64_t rawScanMissCount = 0u;
  uint64_t verifierAttemptCount = 0u;
  uint64_t verifierMismatchCount = 0u;
  uint64_t modelResourceAttemptCount = 0u;
  uint64_t modelResourceCacheHitCount = 0u;
  uint64_t modelResourceDeepResolveCount = 0u;
  uint64_t modelResourceNullResultCount = 0u;
  uint64_t modelResourceVerifierAttemptCount = 0u;
  uint64_t modelResourceVerifierMismatchCount = 0u;
  uint32_t maxRuntimeGeosetCount = 0u;
};

struct ManifestSourceBackingConfig {
  bool enabled = true;
  bool verify = false;
  bool assertOnMismatch = false;
};

const ManifestSourceBackingConfig& GetManifestSourceBackingConfig() {
  static const ManifestSourceBackingConfig s_config = []() {
    const auto exactFlag = [](const char* name) {
      const char* raw = std::getenv(name);
      return raw != nullptr && raw[0] == '1';
    };
    ManifestSourceBackingConfig config = {};
    const char* enabled =
        std::getenv("DXVK_WAR3_MANIFEST_SOURCE_BACKING_FAST_PATH");
    config.enabled = enabled != nullptr && enabled[0] == '1';
    config.assertOnMismatch = exactFlag(
        "DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY_ASSERT");
    config.verify =
        exactFlag("DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY") ||
        config.assertOnMismatch;
    return config;
  }();
  return s_config;
}

struct ManifestModelResourceCacheConfig {
  bool enabled = true;
  bool verify = false;
  bool assertOnMismatch = false;
};

const ManifestModelResourceCacheConfig&
GetManifestModelResourceCacheConfig() {
  static const ManifestModelResourceCacheConfig s_config = []() {
    const auto exactFlag = [](const char* name) {
      const char* raw = std::getenv(name);
      return raw != nullptr && raw[0] == '1';
    };
    ManifestModelResourceCacheConfig config = {};
    const char* enabled =
        std::getenv("DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE");
    config.enabled = enabled == nullptr || enabled[0] != '0';
    config.assertOnMismatch = exactFlag(
        "DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY_ASSERT");
    config.verify =
        exactFlag("DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY") ||
        config.assertOnMismatch;
    return config;
  }();
  return s_config;
}

template <size_t N, typename Fn>
bool CachedPointerBool(std::array<PointerBoolCacheEntry, N>& cache, void* ptr,
                       Fn&& fn) {
  if (ptr == nullptr)
    return false;
  static_assert((N & (N - 1u)) == 0u, "cache size must be a power of two");
  const uint64_t mapEpoch =
      model::ShadowModelResourceCache::instance().mapEpoch();
  const uintptr_t hash = reinterpret_cast<uintptr_t>(ptr) >> 4u;
  auto& slot = cache[hash & (N - 1u)];
  if (slot.ptr == ptr && slot.mapEpoch == mapEpoch)
    return slot.value;
  const bool value = fn();
  slot.ptr = ptr;
  slot.mapEpoch = mapEpoch;
  slot.value = value;
  return value;
}

bool LooksLikeRuntimeModelPtrForContract(void* candidate) {
  if (candidate == nullptr)
    return false;

  thread_local std::array<PointerBoolCacheEntry, 4096u> s_cache = {};
  return CachedPointerBool(s_cache, candidate, [&]() {

  const uintptr_t value = reinterpret_cast<uintptr_t>(candidate);
  if (value < 0x10000u)
    return false;

  uint32_t runtimeGeosetCount = 0;
  void* runtimeGeosets = nullptr;
  const bool hasRuntimeGeosets =
      dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CModelOffsets::RuntimeGeosetCount,
          runtimeGeosetCount) &&
      runtimeGeosetCount > 0u && runtimeGeosetCount < 4096u &&
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::RuntimeGeosets,
          runtimeGeosets) &&
      runtimeGeosets != nullptr &&
      dxvk::war3::IsReadableRangeFast(
          runtimeGeosets,
          size_t((runtimeGeosetCount > 4u ? 4u : runtimeGeosetCount)) *
              sizeof(void*));

  uint32_t finalPoseMatrixCount = 0;
  void* finalPoseMatrixArray = nullptr;
  const bool hasFinalPoseArray =
      dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          finalPoseMatrixCount) &&
      finalPoseMatrixCount > 0u && finalPoseMatrixCount <= 512u &&
      dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          finalPoseMatrixArray) &&
      finalPoseMatrixArray != nullptr &&
      dxvk::war3::IsReadableRangeFast(finalPoseMatrixArray,
                                      sizeof(float) * 16u);

  return hasRuntimeGeosets || hasFinalPoseArray;
  });
}

bool LooksLikeGeosetDataPtrForContract(void* candidate) {
  if (candidate == nullptr)
    return false;

  thread_local std::array<PointerBoolCacheEntry, 4096u> s_cache = {};
  return CachedPointerBool(s_cache, candidate, [&]() {

  uint32_t vertexCount = 0;
  uint32_t primitiveCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t matrixIndexCount = 0;
  void* positions = nullptr;
  void* primitiveRecords = nullptr;
  void* matrixGroupSizes = nullptr;
  void* matrixIndices = nullptr;

  if (!dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CGeosetDataOffsets::VertexCount,
          vertexCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::VertexPositions,
          positions) ||
      !dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CGeosetDataOffsets::PrimitiveRecordCount,
          primitiveCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::PrimitiveRecords,
          primitiveRecords) ||
      !dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixGroupCount,
          matrixGroupCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixGroupSizes,
          matrixGroupSizes) ||
      !dxvk::war3::SafeReadU32Fast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixIndexCount,
          matrixIndexCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidate, dxvk::war3::CGeosetDataOffsets::MatrixIndices,
          matrixIndices)) {
    return false;
  }

  return vertexCount > 0u && vertexCount < (1u << 20) &&
         primitiveCount > 0u && primitiveCount < (1u << 16) &&
         matrixGroupCount > 0u && matrixGroupCount < 4096u &&
         matrixIndexCount > 0u && matrixIndexCount < (1u << 16) &&
         positions != nullptr && primitiveRecords != nullptr &&
         matrixGroupSizes != nullptr && matrixIndices != nullptr &&
         dxvk::war3::IsReadableRangeFast(positions, 12u) &&
         dxvk::war3::IsReadableRangeFast(primitiveRecords, 8u) &&
         dxvk::war3::IsReadableRangeFast(matrixGroupSizes, sizeof(uint32_t)) &&
         dxvk::war3::IsReadableRangeFast(matrixIndices, sizeof(uint32_t));
  });
}

void* ResolveModelResourceForContract(
    void* runtimeModelPtr, model::ShadowModelResourceCache& resourceCache,
    ManifestResolveDiagnostics& diagnostics) {
  if (runtimeModelPtr == nullptr)
    return nullptr;

  ++diagnostics.modelResourceAttemptCount;
  void* ownedModelDataHandle = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
          ownedModelDataHandle) ||
      ownedModelDataHandle == nullptr) {
    ++diagnostics.modelResourceNullResultCount;
    return nullptr;
  }

  struct CacheEntry {
    void* runtimeModelPtr = nullptr;
    void* ownedModelDataHandle = nullptr;
    void* modelResourcePtr = nullptr;
    uint64_t resourceRevision = 0u;
    bool valid = false;
  };
  thread_local std::array<CacheEntry, 4096u> s_cache = {};
  const uintptr_t hash =
      (reinterpret_cast<uintptr_t>(runtimeModelPtr) >> 4u) ^
      (reinterpret_cast<uintptr_t>(ownedModelDataHandle) >> 7u);
  CacheEntry& entry = s_cache[hash & (s_cache.size() - 1u)];
  const uint64_t resourceRevision = resourceCache.revision();
  const auto& config = GetManifestModelResourceCacheConfig();
  if (config.enabled && entry.valid &&
      entry.runtimeModelPtr == runtimeModelPtr &&
      entry.ownedModelDataHandle == ownedModelDataHandle &&
      entry.resourceRevision == resourceRevision) {
    ++diagnostics.modelResourceCacheHitCount;
    if (config.verify) {
      ++diagnostics.modelResourceVerifierAttemptCount;
      void* legacy =
          resourceCache.resolveDirectModelResourcePtr(ownedModelDataHandle);
      if (legacy != entry.modelResourcePtr) {
        ++diagnostics.modelResourceVerifierMismatchCount;
        if (config.assertOnMismatch)
          std::abort();
      }
    }
    if (entry.modelResourcePtr == nullptr)
      ++diagnostics.modelResourceNullResultCount;
    return entry.modelResourcePtr;
  }

  ++diagnostics.modelResourceDeepResolveCount;
  void* resolved =
      resourceCache.resolveDirectModelResourcePtr(ownedModelDataHandle);
  if (config.enabled) {
    entry.runtimeModelPtr = runtimeModelPtr;
    entry.ownedModelDataHandle = ownedModelDataHandle;
    entry.modelResourcePtr = resolved;
    entry.resourceRevision = resourceRevision;
    entry.valid = true;
  }
  if (resolved == nullptr)
    ++diagnostics.modelResourceNullResultCount;
  return resolved;
}

void ResolveCurrentRuntimeGeosetFromDataLegacy(
    ShadowRenderableRecord& record, ManifestResolveDiagnostics& diagnostics) {
  if (record.runtimeGeosetDataPtr == nullptr)
    return;
  if (record.runtimeModelPtr == nullptr)
    return;

  // Phase 7.99：thread_local cache `(runtimeModel, runtimeGeosetDataPtr) -> (geosetPtr, index)`。
  // 桥/斜坡场景下 ManifestCopy 单 record 慢的根因是这里最差走 4096 次
  // SafeReadPtrFast 循环。同 caller 在多帧调用时键完全相同。
  struct ResolveCacheEntry {
    void* runtimeModel = nullptr;
    void* runtimeGeosetData = nullptr;
    void* geosetPtr = nullptr;
    uint64_t mapEpoch = 0u;
    uint32_t index = 0;
    bool valid = false;
  };
  thread_local std::array<ResolveCacheEntry, 256u> s_cache = {};
  const uintptr_t hk = (reinterpret_cast<uintptr_t>(record.runtimeModelPtr) ^
                        reinterpret_cast<uintptr_t>(record.runtimeGeosetDataPtr));
  const size_t slot = (hk >> 4) & (s_cache.size() - 1u);
  ResolveCacheEntry& entry = s_cache[slot];
  const uint64_t mapEpoch =
      model::ShadowModelResourceCache::instance().mapEpoch();
  if (entry.valid && entry.runtimeModel == record.runtimeModelPtr &&
      entry.runtimeGeosetData == record.runtimeGeosetDataPtr &&
      entry.mapEpoch == mapEpoch) {
    ++diagnostics.legacyCacheHitCount;
    record.runtimeGeosetPtr = entry.geosetPtr;
    record.meshIndex = entry.index;
    record.geosetIndex = entry.index;
    return;
  }

  uint32_t runtimeGeosetCount = 0;
  void* runtimeGeosets = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(
          record.runtimeModelPtr, dxvk::war3::CModelOffsets::RuntimeGeosetCount,
          runtimeGeosetCount) ||
      runtimeGeosetCount == 0u || runtimeGeosetCount >= 4096u ||
      !dxvk::war3::SafeReadPtrFast(
          record.runtimeModelPtr, dxvk::war3::CModelOffsets::RuntimeGeosets,
          runtimeGeosets) ||
      runtimeGeosets == nullptr ||
      !dxvk::war3::IsReadableRangeFast(
          runtimeGeosets, size_t(runtimeGeosetCount) * sizeof(void*))) {
    return;
  }

  ++diagnostics.rawScanCount;
  diagnostics.maxRuntimeGeosetCount =
      std::max(diagnostics.maxRuntimeGeosetCount, runtimeGeosetCount);
  auto** entries = reinterpret_cast<void**>(runtimeGeosets);
  for (uint32_t i = 0u; i < runtimeGeosetCount; ++i) {
    ++diagnostics.rawScanEntryVisitCount;
    void* geosetPtr = entries[i];
    if (geosetPtr == nullptr)
      continue;

    void* geosetDataPtr = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(
            geosetPtr, dxvk::war3::CGeosetOffsets::GeosetData,
            geosetDataPtr) ||
        geosetDataPtr != record.runtimeGeosetDataPtr) {
      continue;
    }

    record.runtimeGeosetPtr = geosetPtr;
    record.meshIndex = i;
    record.geosetIndex = i;
    // 写 cache
    entry.runtimeModel = record.runtimeModelPtr;
    entry.runtimeGeosetData = record.runtimeGeosetDataPtr;
    entry.geosetPtr = geosetPtr;
    entry.mapEpoch = mapEpoch;
    entry.index = i;
    entry.valid = true;
    return;
  }
  ++diagnostics.rawScanMissCount;
}

void ResolveCurrentRuntimeGeosetFromData(
    ShadowRenderableRecord& record, ManifestResolveDiagnostics& diagnostics) {
  if (record.runtimeGeosetDataPtr == nullptr ||
      record.runtimeModelPtr == nullptr) {
    return;
  }

  const bool sourceBackingComplete =
      record.runtimeGeosetPtr != nullptr &&
      record.meshIndex != kInvalidShadowContractGeosetIndex &&
      record.geosetIndex != kInvalidShadowContractGeosetIndex &&
      record.meshIndex == record.geosetIndex;
  const auto& config = GetManifestSourceBackingConfig();
  if (!config.enabled || !sourceBackingComplete) {
    ResolveCurrentRuntimeGeosetFromDataLegacy(record, diagnostics);
    return;
  }

  ++diagnostics.sourceCompleteSkipCount;
  if (!config.verify)
    return;

  ++diagnostics.verifierAttemptCount;
  ShadowRenderableRecord legacy = record;
  ManifestResolveDiagnostics verifierDiagnostics = {};
  ResolveCurrentRuntimeGeosetFromDataLegacy(legacy, verifierDiagnostics);
  diagnostics.legacyCacheHitCount +=
      verifierDiagnostics.legacyCacheHitCount;
  diagnostics.rawScanCount += verifierDiagnostics.rawScanCount;
  diagnostics.rawScanEntryVisitCount +=
      verifierDiagnostics.rawScanEntryVisitCount;
  diagnostics.rawScanMissCount += verifierDiagnostics.rawScanMissCount;
  diagnostics.maxRuntimeGeosetCount =
      std::max(diagnostics.maxRuntimeGeosetCount,
               verifierDiagnostics.maxRuntimeGeosetCount);

  const bool matches =
      legacy.runtimeGeosetPtr == record.runtimeGeosetPtr &&
      legacy.runtimeGeosetDataPtr == record.runtimeGeosetDataPtr &&
      legacy.meshIndex == record.meshIndex &&
      legacy.geosetIndex == record.geosetIndex;
  if (matches)
    return;

  ++diagnostics.verifierMismatchCount;
  if (config.assertOnMismatch)
    std::abort();
}

bool IsContractUnitCandidate(const ShadowRenderableRecord& record) {
  return record.objectKind == render::ObjectKind::Unit &&
         record.groupIdx <= 0 && record.unitPtr != nullptr &&
         (record.jHandle != 0u || record.rawcode != 0u);
}

bool IsVisibleDirectGeosetUnitCandidate(
    const render::VisibleRenderableRecord& record) {
  const bool unitLike =
      record.identity.kind == render::ObjectKind::Unit ||
      record.identity.unitPtr != nullptr;
  if (!unitLike || record.identity.groupIdx > 0 ||
      record.identity.unitPtr == nullptr ||
      (record.identity.jHandle == 0u && record.identity.rawcode == 0u) ||
      record.meshData == nullptr) {
    return false;
  }

  if (record.identity.flags5C != 0u &&
      (record.identity.flags5C & dxvk::war3::UnitFlags5C::Building) != 0u)
    return false;

  return record.runtimeGeosetDataPtr != nullptr ||
         LooksLikeGeosetDataPtrForContract(record.meshData);
}

uint32_t VisibleDirectGeosetUnitRejectReason(
    const render::VisibleRenderableRecord& record) {
  const bool unitLike =
      record.identity.kind == render::ObjectKind::Unit ||
      record.identity.unitPtr != nullptr;
  if (!unitLike)
    return 1u;
  if (record.identity.groupIdx > 0)
    return 2u;
  if (record.identity.unitPtr == nullptr)
    return 3u;
  if (record.identity.jHandle == 0u && record.identity.rawcode == 0u)
    return 4u;
  if (record.meshData == nullptr)
    return 5u;
  if (record.identity.flags5C != 0u &&
      (record.identity.flags5C & dxvk::war3::UnitFlags5C::Building) != 0u)
    return 6u;
  if (record.runtimeGeosetDataPtr == nullptr &&
      !LooksLikeGeosetDataPtrForContract(record.meshData))
    return 7u;
  return 0u;
}

uint64_t MakeVisibleDirectUnitKey(
    const render::VisibleRenderableRecord& record) {
  uint64_t key = reinterpret_cast<uintptr_t>(record.identity.unitPtr);
  auto mix = [&key](uint64_t value) {
    key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
  };
  mix(reinterpret_cast<uintptr_t>(record.meshData));
  mix(reinterpret_cast<uintptr_t>(record.runtimeGeosetDataPtr));
  mix(reinterpret_cast<uintptr_t>(record.renderablePart));
  mix(reinterpret_cast<uintptr_t>(record.layerState));
  mix(uint64_t(record.layerIndex));
  mix(uint64_t(record.subIndex));
  mix(uint64_t(record.meshIndex));
  mix(uint64_t(record.geosetIndex));
  mix(uint64_t(record.transparentType));
  mix(uint64_t(record.transparentSortKey));
  mix(uint64_t(record.identity.jHandle));
  mix(uint64_t(record.identity.rawcode));
  return key;
}

bool BackfillVisibleUnitGeosetBindingFromCache(ShadowRenderableRecord& record) {
  if (record.runtimeGeosetDataPtr == nullptr)
    return false;

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  model::ShadowReadyGeosetBinding geoset = {};
  if (!((record.runtimeGeosetPtr != nullptr &&
         resourceCache.findReadyGeosetBindingByPtr(
             record.runtimeGeosetPtr, geoset)) ||
        resourceCache.findReadyGeosetBindingByData(
            record.runtimeGeosetDataPtr, geoset))) {
    return false;
  }

  if (record.runtimeGeosetPtr == nullptr)
    record.runtimeGeosetPtr = geoset.geosetPtr;
  if (record.modelResourcePtr == nullptr)
    record.modelResourcePtr = geoset.modelResourcePtr;
  if (record.modelKey == 0u)
    record.modelKey = geoset.modelKey;
  if (geoset.geosetIndex != model::kInvalidShadowGeosetIndex) {
    record.geosetIndex = geoset.geosetIndex;
    record.meshIndex = geoset.geosetIndex;
  }
  return true;
}

bool TryPublishMissingVisibleUnitGeosetBinding(
    ShadowRenderableRecord& record) {
  if (!IsContractUnitCandidate(record))
    return false;
  if (record.runtimeModelPtr == nullptr ||
      record.runtimeGeosetDataPtr == nullptr ||
      record.geosetIndex == model::kInvalidShadowGeosetIndex) {
    return false;
  }

  // This is intentionally a demand-fill, not a per-frame refresh.  The current
  // visible render path often exposes CGeosetData* directly at renderable+0x0C;
  // copying that geometry on every contract conversion caused the 5-13ms
  // ManifestCopy storm.  Capture once per unique geoset data pointer and let
  // live CModel pose refresh handle animation freshness.
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  resourceCache.noteRuntimeGeosetBinding(
      record.runtimeModelPtr, record.geosetIndex, record.runtimeGeosetPtr,
      record.runtimeGeosetDataPtr, record.modelResourcePtr, record.modelKey);

  return BackfillVisibleUnitGeosetBindingFromCache(record);
}

void DemandFillVisibleUnitGeosetBindings(ShadowFrameManifest& manifest) {
  auto demandFillScope = ContractCpuScope(
      "War3SemanticScene/CaptureContract/VisibleGeosetDemandFill");
  constexpr uint32_t kMaxDemandFillPerCapture = 64u;
  static std::atomic<uint64_t> s_demandFillCursor{0u};
  std::array<void*, kMaxDemandFillPerCapture> seenMissingGeosetData = {};
  size_t seenMissingCount = 0u;
  uint32_t capturedThisFrame = 0u;
  if (manifest.records.empty())
    return;

  const size_t recordCount = manifest.records.size();
  // A byte per manifest record avoids repeating a successful scalar binding
  // lookup in the final repair sweep. No pointer or identity crosses frames.
  std::vector<uint8_t> readyBinding(recordCount, uint8_t(0u));
  const size_t startIndex =
      size_t(s_demandFillCursor.fetch_add(1u, std::memory_order_relaxed) %
             uint64_t(recordCount));

  for (size_t offset = 0u; offset < recordCount; ++offset) {
    const size_t recordIndex = (startIndex + offset) % recordCount;
    auto& record = manifest.records[recordIndex];
    if (!IsContractUnitCandidate(record))
      continue;
    if (record.runtimeModelPtr == nullptr ||
        record.runtimeGeosetDataPtr == nullptr ||
        record.geosetIndex == model::kInvalidShadowGeosetIndex) {
      continue;
    }
    if (BackfillVisibleUnitGeosetBindingFromCache(record)) {
      readyBinding[recordIndex] = uint8_t(1u);
      continue;
    }
    if (capturedThisFrame >= kMaxDemandFillPerCapture)
      continue;

    const auto seenEnd = seenMissingGeosetData.begin() + seenMissingCount;
    if (std::find(seenMissingGeosetData.begin(), seenEnd,
                  record.runtimeGeosetDataPtr) != seenEnd) {
      continue;
    }
    seenMissingGeosetData[seenMissingCount++] =
        record.runtimeGeosetDataPtr;

    if (TryPublishMissingVisibleUnitGeosetBinding(record))
      readyBinding[recordIndex] = uint8_t(1u);
    ++capturedThisFrame;
  }

  for (size_t i = 0u; i < recordCount; ++i) {
    if (readyBinding[i] == 0u)
      BackfillVisibleUnitGeosetBindingFromCache(manifest.records[i]);
  }
}

ShadowRenderableRecord ConvertVisible(
    const render::VisibleRenderableRecord& src,
    uint64_t frameSerial,
    ManifestResolveDiagnostics& resolveDiagnostics) {
  ShadowRenderableRecord dst = {};
  dst.worldObjectEntry = src.identity.worldObjectEntry;
  dst.sceneNode =
      src.identity.sceneNode != nullptr ? src.identity.sceneNode : src.sceneNode;
  dst.unitPtr = src.identity.unitPtr;
  dst.renderablePart = src.renderablePart;
  dst.payload = src.payload;
  dst.meshData = src.meshData;
  dst.layerState = src.layerState;
  dst.runtimeModelPtr = src.runtimeModelPtr;
  dst.modelResourcePtr = src.modelResourcePtr;
  dst.runtimeGeosetPtr = src.runtimeGeosetPtr;
  dst.runtimeGeosetDataPtr = src.runtimeGeosetDataPtr;
  dst.modelKey = src.modelKey;
  dst.jHandle = src.identity.jHandle;
  dst.rawcode = src.identity.rawcode;
  dst.unitFlags5C = src.identity.flags5C;
  dst.layerIndex = src.layerIndex;
  dst.subIndex = src.subIndex;
  dst.meshIndex = src.meshIndex;
  dst.geosetIndex = src.geosetIndex;
  dst.objectKind = src.identity.kind;
  if (dst.objectKind == render::ObjectKind::Unknown && dst.unitPtr != nullptr &&
      (dst.unitFlags5C & dxvk::war3::UnitFlags5C::Building) == 0u) {
    dst.objectKind = render::ObjectKind::Unit;
  }
  dst.queueKind = src.queueKind;
  dst.groupIdx = src.identity.groupIdx;
  dst.frameSerial = frameSerial;
  dst.stage = src.stage;
  dst.pathBlocker =
      src.pathBlocker ||
      dxvk::war3::internal::IsPathBlockerFourCc(dst.rawcode);

  if (dst.runtimeModelPtr == nullptr &&
      LooksLikeRuntimeModelPtrForContract(dst.sceneNode)) {
    dst.runtimeModelPtr = dst.sceneNode;
  }

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (dst.modelResourcePtr == nullptr && dst.runtimeModelPtr != nullptr) {
    dst.modelResourcePtr = ResolveModelResourceForContract(
        dst.runtimeModelPtr, resourceCache, resolveDiagnostics);
  }

  if (dst.runtimeGeosetDataPtr == nullptr &&
      LooksLikeGeosetDataPtrForContract(dst.meshData)) {
    dst.runtimeGeosetDataPtr = dst.meshData;
  }
  ResolveCurrentRuntimeGeosetFromData(dst, resolveDiagnostics);
  return dst;
}

const ShadowRenderableRecord* FindPriorRenderableRecord(
    const ShadowFrameManifest& priorManifest,
    const ShadowRenderableRecord& record) {
  auto matchRecord = [&](const ShadowRenderableRecord& candidate) {
    return (record.worldObjectEntry != nullptr &&
            candidate.worldObjectEntry == record.worldObjectEntry) ||
           (record.sceneNode != nullptr && candidate.sceneNode == record.sceneNode) ||
           (record.unitPtr != nullptr && candidate.unitPtr == record.unitPtr) ||
           (record.runtimeModelPtr != nullptr &&
            candidate.runtimeModelPtr == record.runtimeModelPtr) ||
           (record.jHandle != 0u && candidate.jHandle == record.jHandle) ||
           (record.renderablePart != nullptr &&
            candidate.renderablePart == record.renderablePart) ||
           (record.payload != nullptr && candidate.payload == record.payload);
  };

  for (const auto& candidate : priorManifest.records) {
    if (matchRecord(candidate))
      return &candidate;
  }

  return nullptr;
}

void MergeRenderableIdentityFromPrior(const ShadowRenderableRecord& prior,
                                      ShadowRenderableRecord& record) {
  if (record.worldObjectEntry == nullptr)
    record.worldObjectEntry = prior.worldObjectEntry;
  if (record.sceneNode == nullptr)
    record.sceneNode = prior.sceneNode;
  if (record.unitPtr == nullptr)
    record.unitPtr = prior.unitPtr;
  if (record.runtimeModelPtr == nullptr)
    record.runtimeModelPtr = prior.runtimeModelPtr;
  if (record.modelResourcePtr == nullptr)
    record.modelResourcePtr = prior.modelResourcePtr;
  if (record.modelKey == 0u)
    record.modelKey = prior.modelKey;
  // meshData/geoset fields are current-renderable slice metadata, not stable
  // object identity.  Do not inherit them across prior/sibling manifest records:
  // shared model meshes can otherwise make one caster's partial silhouette drive
  // every caster in the ShadowMap.
  if (record.jHandle == 0u)
    record.jHandle = prior.jHandle;
  if (record.rawcode == 0u)
    record.rawcode = prior.rawcode;
  record.pathBlocker =
      record.pathBlocker || prior.pathBlocker ||
      dxvk::war3::internal::IsPathBlockerFourCc(record.rawcode);
  if (record.objectKind == render::ObjectKind::Unknown &&
      prior.objectKind != render::ObjectKind::Unknown) {
    record.objectKind = prior.objectKind;
  }
}

void RepairManifestIdentityFromPrior(const ShadowFrameManifest& priorManifest,
                                     ShadowFrameManifest& manifest) {
  if (priorManifest.records.empty() || manifest.records.empty())
    return;

  // Phase 7.96：当 priorManifest 较大时，O(N*M) 线性扫描会在桥/斜坡场景下
  // 严重拖慢 ManifestHydrate。建立指针索引把查找从 O(N*M) 降到 O(N+M)。
  // 阈值 64：N <= 64 时线性扫描的 cache friendly 优势 > hash 索引开销；
  // N > 64 时 hash 索引构建+查找的总成本明显低于 N*M。
  constexpr size_t kHashIndexThreshold = 64u;
  const bool useHashIndex = priorManifest.records.size() > kHashIndexThreshold;
  std::unordered_map<void*, const ShadowRenderableRecord*> byWorldObject;
  std::unordered_map<void*, const ShadowRenderableRecord*> bySceneNode;
  std::unordered_map<void*, const ShadowRenderableRecord*> byRuntimeModel;
  std::unordered_map<uint32_t, const ShadowRenderableRecord*> byHandle;
  if (useHashIndex) {
    byWorldObject.reserve(priorManifest.records.size());
    bySceneNode.reserve(priorManifest.records.size());
    byRuntimeModel.reserve(priorManifest.records.size());
    byHandle.reserve(priorManifest.records.size());
    for (const auto& candidate : priorManifest.records) {
      if (candidate.worldObjectEntry != nullptr)
        byWorldObject.emplace(candidate.worldObjectEntry, &candidate);
      if (candidate.sceneNode != nullptr)
        bySceneNode.emplace(candidate.sceneNode, &candidate);
      if (candidate.runtimeModelPtr != nullptr)
        byRuntimeModel.emplace(candidate.runtimeModelPtr, &candidate);
      if (candidate.jHandle != 0u)
        byHandle.emplace(candidate.jHandle, &candidate);
    }
  }

  for (auto& record : manifest.records) {
    const bool hasStrongIdentity =
        record.objectKind != render::ObjectKind::Unknown &&
        record.rawcode != 0u && record.jHandle != 0u &&
        (record.worldObjectEntry != nullptr || record.sceneNode != nullptr ||
         record.unitPtr != nullptr || record.runtimeModelPtr != nullptr);
    if (hasStrongIdentity) {
      continue;
    }

    const ShadowRenderableRecord* priorRecord = nullptr;
    if (useHashIndex) {
      // Phase 7.96：hash index O(1) 查找最常用的 4 个 key。
      if (record.worldObjectEntry != nullptr) {
        auto it = byWorldObject.find(record.worldObjectEntry);
        if (it != byWorldObject.end()) priorRecord = it->second;
      }
      if (priorRecord == nullptr && record.sceneNode != nullptr) {
        auto it = bySceneNode.find(record.sceneNode);
        if (it != bySceneNode.end()) priorRecord = it->second;
      }
      if (priorRecord == nullptr && record.runtimeModelPtr != nullptr) {
        auto it = byRuntimeModel.find(record.runtimeModelPtr);
        if (it != byRuntimeModel.end()) priorRecord = it->second;
      }
      if (priorRecord == nullptr && record.jHandle != 0u) {
        auto it = byHandle.find(record.jHandle);
        if (it != byHandle.end()) priorRecord = it->second;
      }

      // 罕见 key（unitPtr / renderablePart / payload）走线性扫描兜底。
      if (priorRecord == nullptr &&
          (record.unitPtr != nullptr || record.renderablePart != nullptr ||
           record.payload != nullptr)) {
        priorRecord = FindPriorRenderableRecord(priorManifest, record);
      }
    } else {
      // 小规模直接线性扫描（cache friendly）。
      priorRecord = FindPriorRenderableRecord(priorManifest, record);
    }

    if (priorRecord != nullptr) {
      MergeRenderableIdentityFromPrior(*priorRecord, record);
    }
  }
}

void HydrateManifestRuntimeOwnersFromIndexedCache(
    ShadowFrameManifest& manifest,
    model::ShadowModelResourceCache& resourceCache) {
  if (manifest.records.empty())
    return;

  for (auto& record : manifest.records) {
    if (record.runtimeModelPtr != nullptr ||
        (record.runtimeGeosetPtr == nullptr &&
         record.runtimeGeosetDataPtr == nullptr)) {
      continue;
    }

    model::ShadowModelResourceRecord owner = {};
    if (!resourceCache.findRuntimeModelOwnerIndexed(record.runtimeGeosetPtr,
                                                    record.runtimeGeosetDataPtr,
                                                    owner)) {
      continue;
    }

    record.runtimeModelPtr = owner.runtimeModelPtr;
    if (record.modelResourcePtr == nullptr)
      record.modelResourcePtr = owner.modelResourcePtr;
    if (record.modelKey == 0u)
      record.modelKey = owner.modelKey;
  }
}

ShadowModelResourceRecord ConvertGeoset(
    const model::ShadowGeosetResourceRecord& src,
    uint64_t frameSerial) {
  ShadowModelResourceRecord dst = {};
  dst.runtimeGeosetPtr = src.geosetPtr;
  dst.runtimeGeosetDataPtr = src.geosetDataPtr;
  dst.modelResourcePtr = src.modelResourcePtr;
  dst.modelKey = src.modelKey;
  dst.prefersRuntimeContract = src.prefersRuntimeContract;
  dst.geosetIndex = src.geosetIndex;
  dst.vertexCount = src.vertexCount;
  dst.positions = src.positions;
  dst.normals = src.normals;
  dst.vertexGroupIndices = src.vertexGroupIndices;
  dst.primitiveRecords.reserve(src.primitiveRecords.size());
  for (const auto& primitive : src.primitiveRecords) {
    dst.primitiveRecords.push_back(ShadowPrimitiveRecord{
        primitive.primitiveTypeOrMaterialSlot, primitive.indexCount});
  }
  dst.matrixGroupSizes = src.matrixGroupSizes;
  dst.matrixIndices = src.matrixIndices;
  dst.indices = src.indices;
  dst.uvLayers.reserve(src.uvLayers.size());
  for (const auto& uvLayer : src.uvLayers)
    dst.uvLayers.push_back(uvLayer.uvPairs);
  dst.contentHash = src.contentHash;
  dst.mapEpoch = src.mapEpoch;
  dst.immutableModelGeneration = src.immutableModelGeneration;
  dst.localBounds = src.localBounds;
  dst.frameSerial = frameSerial;
  return dst;
}

ShadowPoseRecord ConvertPose(const model::PoseRecord& src,
                             uint64_t frameSerial) {
  ShadowPoseRecord dst = {};
  dst.runtimeModelPtr = src.runtimeModelPtr;
  dst.sceneNode = src.sceneNode;
  dst.unitPtr = src.unitPtr;
  dst.matrixCount = src.matrixCount;
  dst.matrixHash = src.matrixHash;
  dst.matrixPalette = src.matrixPalette;
  dst.hasWorldTransform = src.hasWorldTransform || src.hasSpriteFrameTransform;
  dst.worldTransform =
      src.hasSpriteFrameTransform ? src.spriteFrameTransform : src.worldTransform;
  dst.frameSerial = frameSerial;
  return dst;
}

Matrix4 DecodeRuntimePoseMatrix48(const void* poseBytes) {
  if (poseBytes == nullptr)
    return Matrix4();

  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseBytes, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

void HashBytesFnv64Append(uint64_t& hash, const void* data, size_t size) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0u; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
}

bool TryReadLiveCModelPose(void* runtimeModelPtr, uint64_t frameSerial,
                           ShadowPoseRecord& out) {
  out = {};
  if (runtimeModelPtr == nullptr)
    return false;

  uint32_t matrixCount = 0u;
  void* matrixBase = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          matrixCount) ||
      !dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          matrixBase) ||
      matrixBase == nullptr || matrixCount == 0u) {
    return false;
  }

  matrixCount = (std::min)(matrixCount, 256u);
  const size_t bytes = size_t(matrixCount) * 48u;
  if (!dxvk::war3::IsReadableRange(matrixBase, bytes))
    return false;

  out.runtimeModelPtr = runtimeModelPtr;
  out.frameSerial = frameSerial;
  out.matrixCount = matrixCount;
  out.matrixPalette.resize(matrixCount);
  const auto* raw = reinterpret_cast<const uint8_t*>(matrixBase);
  uint64_t matrixHash = 1469598103934665603ull;
  HashBytesFnv64Append(matrixHash, &matrixCount, sizeof(matrixCount));
  for (uint32_t i = 0u; i < matrixCount; ++i) {
    const auto* matrixBytes = raw + size_t(i) * 48u;
    out.matrixPalette[i] = DecodeRuntimePoseMatrix48(matrixBytes);
    HashBytesFnv64Append(matrixHash, matrixBytes, 48u);
  }
  out.matrixHash = matrixHash;

  const auto* worldMatrixPtr =
      reinterpret_cast<const uint8_t*>(runtimeModelPtr) +
      dxvk::war3::CModelOffsets::WorldMatrix3x4;
  if (dxvk::war3::IsReadableRange(worldMatrixPtr, 48u)) {
    out.worldTransform = DecodeRuntimePoseMatrix48(worldMatrixPtr);
    out.hasWorldTransform = true;
  }

  return out.matrixCount != 0u && !out.matrixPalette.empty();
}

struct RootUnitSupplementSeed {
  void* runtimeModelPtr = nullptr;
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* unitPtr = nullptr;
  void* sourceObjectPtr = nullptr;
  void* sourceSpriteObjectPtr = nullptr;
  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0;
  uint32_t jHandle = 0;
  uint32_t rawcode = 0;
  render::ObjectKind objectKind = render::ObjectKind::Unknown;
};

void MergeRootUnitSeedFromInstance(
    RootUnitSupplementSeed& seed,
    const model::ModelInstanceRecord& record) {
  if (seed.runtimeModelPtr == nullptr)
    seed.runtimeModelPtr = record.runtimeModelPtr;
  if (seed.worldObjectEntry == nullptr)
    seed.worldObjectEntry = record.worldObjectEntry;
  if (seed.sceneNode == nullptr)
    seed.sceneNode = record.sceneNode;
  if (seed.unitPtr == nullptr)
    seed.unitPtr = record.unitPtr;
  if (seed.sourceObjectPtr == nullptr)
    seed.sourceObjectPtr = record.sourceObjectPtr;
  if (seed.sourceSpriteObjectPtr == nullptr)
    seed.sourceSpriteObjectPtr = record.sourceSpriteObjectPtr;
  if (seed.modelResourcePtr == nullptr)
    seed.modelResourcePtr = record.modelResourcePtr;
  if (seed.modelKey == 0u)
    seed.modelKey = record.modelKey;
  if (seed.jHandle == 0u)
    seed.jHandle = record.jHandle;
  if (seed.rawcode == 0u)
    seed.rawcode = record.rawcode;
  if (seed.objectKind == render::ObjectKind::Unknown &&
      record.unitPtr != nullptr) {
    seed.objectKind = render::ObjectKind::Unit;
  }
}

void MergeRootUnitSeedFromShadow(
    RootUnitSupplementSeed& seed,
    const render::ShadowObjectRecord& record) {
  if (seed.runtimeModelPtr == nullptr)
    seed.runtimeModelPtr = record.runtimeModelPtr;
  if (seed.worldObjectEntry == nullptr)
    seed.worldObjectEntry = record.worldObjectEntry;
  if (seed.sceneNode == nullptr)
    seed.sceneNode = record.sceneNode;
  if (seed.unitPtr == nullptr)
    seed.unitPtr = record.unitPtr;
  if (seed.modelResourcePtr == nullptr)
    seed.modelResourcePtr = record.modelResourcePtr;
  if (seed.modelKey == 0u)
    seed.modelKey = record.modelKey;
  if (seed.jHandle == 0u)
    seed.jHandle = record.jHandle;
  if (seed.rawcode == 0u)
    seed.rawcode = record.rawcode;
  if (seed.objectKind == render::ObjectKind::Unknown &&
      record.kind != render::ObjectKind::Unknown) {
    seed.objectKind = record.kind;
  }
}

void MergeRootUnitSeedFromPose(RootUnitSupplementSeed& seed,
                               const model::PoseRecord& record) {
  if (seed.runtimeModelPtr == nullptr)
    seed.runtimeModelPtr = record.runtimeModelPtr;
  if (seed.sceneNode == nullptr)
    seed.sceneNode = record.sceneNode;
  if (seed.unitPtr == nullptr)
    seed.unitPtr = record.unitPtr;
  if (seed.objectKind == render::ObjectKind::Unknown &&
      record.unitPtr != nullptr) {
    seed.objectKind = render::ObjectKind::Unit;
  }
}

void MergeRootUnitSeedFromAttachment(
    RootUnitSupplementSeed& seed,
    const ShadowAttachmentRigidRecord& attachment) {
  if (seed.worldObjectEntry == nullptr)
    seed.worldObjectEntry = attachment.worldObjectEntry;
  if (seed.sceneNode == nullptr)
    seed.sceneNode = attachment.sceneNode;
  if (seed.unitPtr == nullptr)
    seed.unitPtr = attachment.unitPtr;
  if (seed.sourceObjectPtr == nullptr)
    seed.sourceObjectPtr = attachment.sourceObjectPtr;
  if (seed.sourceSpriteObjectPtr == nullptr)
    seed.sourceSpriteObjectPtr = attachment.sourceSpriteObjectPtr;
  if (seed.jHandle == 0u)
    seed.jHandle = attachment.jHandle;
  if (seed.rawcode == 0u)
    seed.rawcode = attachment.rawcode;
  if (seed.objectKind == render::ObjectKind::Unknown &&
      attachment.unitPtr != nullptr) {
    seed.objectKind = render::ObjectKind::Unit;
  }
}

bool HasRootUnitSupplementIdentity(const RootUnitSupplementSeed& seed) {
  return seed.runtimeModelPtr != nullptr &&
         (seed.objectKind == render::ObjectKind::Unit ||
          seed.unitPtr != nullptr) &&
         (seed.worldObjectEntry != nullptr || seed.sceneNode != nullptr ||
          seed.unitPtr != nullptr || seed.jHandle != 0u ||
          seed.rawcode != 0u);
}

template <typename Fn>
void ForEachRuntimeAlias(void* runtimeModelPtr, Fn&& fn) {
  if (runtimeModelPtr == nullptr)
    return;

  fn(runtimeModelPtr);

  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const auto value = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (value < 0x10000u)
    return;
  fn(reinterpret_cast<void*>(value + kCModelComplexExtensionOffset));
  if (value > kCModelComplexExtensionOffset)
    fn(reinterpret_cast<void*>(value - kCModelComplexExtensionOffset));
}

bool HasSnapshotPoseForRuntimeAlias(const model::PoseRegistry& poseRegistry,
                                    void* runtimeModelPtr) {
  bool found = false;
  ForEachRuntimeAlias(runtimeModelPtr, [&](void* candidate) {
    if (found)
      return;
    model::PoseRecord pose = {};
    if (poseRegistry.findByRuntimeModel(candidate, pose) &&
        (pose.hasWorldTransform || pose.hasSpriteFrameTransform ||
         (pose.matrixCount != 0u && !pose.matrixPalette.empty()))) {
      found = true;
    }
  });
  return found;
}

bool HasContractMatrixPoseForRuntimeAlias(const ShadowPoseStore& poses,
                                          void* runtimeModelPtr) {
  bool found = false;
  ForEachRuntimeAlias(runtimeModelPtr, [&](void* candidate) {
    if (found)
      return;
    const auto* pose = poses.findByRuntimeModelPtr(candidate);
    if (pose != nullptr && pose->matrixCount != 0u &&
        !pose->matrixPalette.empty()) {
      found = true;
    }
  });
  return found;
}

const ShadowModelResourceRecord* FindManifestResourceInStore(
    const ShadowModelResourceStore& resources,
    const ShadowRenderableRecord& record) {
  if (record.runtimeGeosetPtr != nullptr) {
    if (const auto* resource =
            resources.findByRuntimeGeoset(record.runtimeGeosetPtr)) {
      return resource;
    }
  }
  if (record.runtimeGeosetDataPtr != nullptr) {
    if (const auto* resource =
            resources.findByRuntimeGeosetData(record.runtimeGeosetDataPtr)) {
      return resource;
    }
  }
  if (record.runtimeModelPtr != nullptr &&
      record.geosetIndex != kInvalidShadowContractGeosetIndex) {
    if (const auto* resource =
            resources.findByRuntimeModel(record.runtimeModelPtr,
                                         record.geosetIndex)) {
      return resource;
    }
  }
  if (record.modelResourcePtr != nullptr &&
      record.geosetIndex != kInvalidShadowContractGeosetIndex) {
    if (const auto* resource =
            resources.findByModelResource(record.modelResourcePtr,
                                          record.geosetIndex)) {
      return resource;
    }
  }
  return nullptr;
}

bool ResourceStoreHasReadyManifestCoverage(
    const ShadowModelResourceStore& resources,
    const ShadowFrameManifest& manifest) {
  constexpr size_t kMaxCoverageProbeRecords = 64u;

  size_t probed = 0u;
  for (const auto& record : manifest.records) {
    if (!record.hasResolvedGeoset())
      continue;

    ++probed;
    const auto* resource = FindManifestResourceInStore(resources, record);
    if (resource != nullptr && resource->readyForConsumer())
      return true;

    if (probed >= kMaxCoverageProbeRecords)
      break;
  }

  return probed == 0u;
}

bool IsAttachmentChildRuntime(
    void* runtimeModelPtr,
    const std::unordered_set<void*>& attachmentChildRuntimes) {
  if (runtimeModelPtr == nullptr)
    return false;

  bool matched = false;
  ForEachRuntimeAlias(runtimeModelPtr, [&](void* candidate) {
    if (!matched && attachmentChildRuntimes.find(candidate) !=
                        attachmentChildRuntimes.end()) {
      matched = true;
    }
  });
  return matched;
}

void SupplementPosesFromLiveCModels(
    const ShadowFrameManifest& manifest,
    const ShadowAttachmentRigidStore& attachments,
    ShadowPoseStore& poses,
    ShadowFrameStats& stats) {
  // This is the new low-cost pose path: after the visible manifest is known,
  // copy Blizzard's already-evaluated CModel palette once per visible runtime.
  // High-frequency SpriteFrameUpdate hooks should not rebuild registry joins.
  constexpr size_t kMaxDirectPoseAttemptsPerFrame = 768u;
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  // Phase 7.105：opening 期 12-player 地图大量对象加载，原 std::vector<void*>
  // + std::find 是 O(N²)，N 可达数百时每帧吃几 ms。改成 unordered_set。
  std::unordered_set<void*> visited;
  visited.reserve(manifest.records.size() * 3u +
                  attachments.records().size() * 6u + 256u);

  auto markVisited = [&](void* ptr) {
    if (ptr == nullptr)
      return false;
    return visited.insert(ptr).second;
  };

  auto tryRuntime = [&](void* runtimeModelPtr, void* sceneNode, void* unitPtr) {
    if (runtimeModelPtr == nullptr ||
        stats.directPoseSupplementAttemptCount >=
            kMaxDirectPoseAttemptsPerFrame) {
      return;
    }

    if (HasContractMatrixPoseForRuntimeAlias(poses, runtimeModelPtr)) {
      ++stats.directPoseSupplementSkippedExisting;
      return;
    }

    bool resolvedForRuntime = false;
    ForEachRuntimeAlias(runtimeModelPtr, [&](void* candidate) {
      if (resolvedForRuntime || candidate == nullptr ||
          stats.directPoseSupplementAttemptCount >=
              kMaxDirectPoseAttemptsPerFrame ||
          !markVisited(candidate)) {
        return;
      }

      ++stats.directPoseSupplementAttemptCount;
      ShadowPoseRecord pose = {};
      if (!TryReadLiveCModelPose(candidate, manifest.frameSerial, pose)) {
        ++stats.directPoseSupplementSkippedInvalid;
        return;
      }

      pose.sceneNode = sceneNode;
      pose.unitPtr = unitPtr;
      poses.add(std::move(pose));
      ++stats.directPoseSupplementResolvedCount;
      resolvedForRuntime = true;
    });
  };

  for (const auto& record : manifest.records)
    tryRuntime(record.runtimeModelPtr, record.sceneNode, record.unitPtr);

  for (const auto& attachment : attachments.records()) {
    tryRuntime(attachment.rootRuntimeModelPtr, attachment.sceneNode,
               attachment.unitPtr);
    if (attachment.ownerRuntimeModelPtr != attachment.rootRuntimeModelPtr)
      tryRuntime(attachment.ownerRuntimeModelPtr, attachment.sceneNode,
                 attachment.unitPtr);
    tryRuntime(attachment.childRuntimeModelPtr, attachment.sceneNode,
               attachment.unitPtr);
  }

  // Sprite-frame pose hooks are no longer the authoritative palette producer.
  // Runtime/resource hooks still publish known CModel pointers into the resource
  // cache; sample the global runtime list only as a cold-start fallback. On hot
  // frames the visible manifest already names the runtimes that can cast this
  // frame; sweeping every known runtime turns a safety net into a per-frame tax.
  //
  // Phase 7.105：12-player opening 期间 manifest.records 可能为空，导致
  // directPoseSupplementResolvedCount==0 每帧触发 → 全 registry sweep。runtime
  // model 数量可达数百，配合 tryRuntime 的 SafeRead 与 ForEachRuntimeAlias，
  // 单次 sweep 可能 >10ms。我们已经把 visited 改成 unordered_set，但 sweep
  // 本身仍要做 N 次 SafeRead。这里再加一个全局 attempt 上限：
  //   - 已经命中 kMaxDirectPoseAttemptsPerFrame 上限 = 软尾巴，仍跑
  //   - manifest.records 为空（cold start）= 限制只走前 64 个 runtime
  //   - 否则正常完成（manifest 提供了真正可见的对象，已经走完）
  if (stats.directPoseSupplementResolvedCount == 0u &&
      stats.directPoseSupplementAttemptCount <
          kMaxDirectPoseAttemptsPerFrame) {
    constexpr size_t kColdStartSweepBudget = 64u;
    size_t coldStartAttempts = 0u;
    for (const auto& runtimeRecord :
         resourceCache.snapshotRuntimeModelAliases()) {
      if (coldStartAttempts >= kColdStartSweepBudget)
        break;
      const size_t attemptsBefore = stats.directPoseSupplementAttemptCount;
      tryRuntime(runtimeRecord.runtimeModelPtr, nullptr, nullptr);
      // 只在真的尝试了（不是 visited 提前 dedup）才计 budget。
      if (stats.directPoseSupplementAttemptCount > attemptsBefore)
        ++coldStartAttempts;
    }
  }
}

bool TryResolveRuntimeModelSemanticKey(void* runtimeModelPtr,
                                       void*& outModelResourcePtr,
                                       uint64_t& outModelKey);

void TryMergeRootUnitSeedFromRegistries(
    RootUnitSupplementSeed& seed,
    const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const model::PoseRegistry& poseRegistry) {
  auto mergeByRuntime = [&](void* candidate) {
    model::ModelInstanceRecord instanceRecord = {};
    if (instanceRegistry.findByRuntimeModel(candidate, instanceRecord))
      MergeRootUnitSeedFromInstance(seed, instanceRecord);
    if (instanceRegistry.findOwnerByRuntimeModel(candidate, instanceRecord))
      MergeRootUnitSeedFromInstance(seed, instanceRecord);

    render::ShadowObjectRecord shadowRecord = {};
    if (shadowRegistry.findByRuntimeModel(candidate, shadowRecord))
      MergeRootUnitSeedFromShadow(seed, shadowRecord);

    model::PoseRecord poseRecord = {};
    if (poseRegistry.findByRuntimeModel(candidate, poseRecord))
      MergeRootUnitSeedFromPose(seed, poseRecord);
  };
  ForEachRuntimeAlias(seed.runtimeModelPtr, mergeByRuntime);

  model::ModelInstanceRecord instanceRecord = {};
  render::ShadowObjectRecord shadowRecord = {};
  model::PoseRecord poseRecord = {};

  if (instanceRegistry.findByWorldObjectEntry(seed.worldObjectEntry,
                                             instanceRecord)) {
    MergeRootUnitSeedFromInstance(seed, instanceRecord);
  }
  if (shadowRegistry.findByWorldObjectEntry(seed.worldObjectEntry,
                                           shadowRecord)) {
    MergeRootUnitSeedFromShadow(seed, shadowRecord);
  }

  if (instanceRegistry.findBySceneNode(seed.sceneNode, instanceRecord))
    MergeRootUnitSeedFromInstance(seed, instanceRecord);
  if (shadowRegistry.findBySceneNode(seed.sceneNode, shadowRecord))
    MergeRootUnitSeedFromShadow(seed, shadowRecord);
  if (poseRegistry.findBySceneNode(seed.sceneNode, poseRecord))
    MergeRootUnitSeedFromPose(seed, poseRecord);

  if (instanceRegistry.findByUnitPtr(seed.unitPtr, instanceRecord))
    MergeRootUnitSeedFromInstance(seed, instanceRecord);
  if (shadowRegistry.findByUnitPtr(seed.unitPtr, shadowRecord))
    MergeRootUnitSeedFromShadow(seed, shadowRecord);
  if (poseRegistry.findByUnitPtr(seed.unitPtr, poseRecord))
    MergeRootUnitSeedFromPose(seed, poseRecord);

  if (instanceRegistry.findByHandle(seed.jHandle, instanceRecord))
    MergeRootUnitSeedFromInstance(seed, instanceRecord);
  if (shadowRegistry.findByHandle(seed.jHandle, shadowRecord))
    MergeRootUnitSeedFromShadow(seed, shadowRecord);

  if (instanceRegistry.findBySourceObject(seed.sourceObjectPtr, instanceRecord))
    MergeRootUnitSeedFromInstance(seed, instanceRecord);
  if (instanceRegistry.findBySourceSpriteObject(seed.sourceSpriteObjectPtr,
                                               instanceRecord)) {
    MergeRootUnitSeedFromInstance(seed, instanceRecord);
  }
}

bool TryFindRuntimeModelResourceForSeed(
    const model::ShadowModelResourceCache& resourceCache,
    const RootUnitSupplementSeed& seed,
    model::ShadowModelResourceRecord& out,
    ShadowFrameStats* stats = nullptr,
    std::unordered_set<void*>* semanticKeyAttemptedRuntimes = nullptr,
    size_t* semanticKeyResolveBudget = nullptr) {
  out = {};
  bool found = false;
  bool cacheMiss = true;
  bool cacheNotReady = false;
  ForEachRuntimeAlias(seed.runtimeModelPtr, [&](void* candidate) {
    if (found)
      return;

    model::ShadowModelResourceRecord candidateRecord = {};
    if (!resourceCache.findRuntimeModelResource(candidate, candidateRecord))
      return;

    cacheMiss = false;
    if (!candidateRecord.readyForShadowConsumer()) {
      cacheNotReady = true;
      return;
    }

    out = std::move(candidateRecord);
    found = true;
  });
  if (found)
    return true;

  if (seed.modelResourcePtr != nullptr) {
    model::ShadowModelResourceRecord modelRecord = {};
    if (resourceCache.findModelResource(seed.modelResourcePtr, modelRecord)) {
      cacheMiss = false;
      if (modelRecord.readyForShadowConsumer()) {
        out = std::move(modelRecord);
        return true;
      }
      cacheNotReady = true;
    }
  }

  if (stats != nullptr) {
    if (cacheMiss)
      ++stats->rootUnitSupplementResourceCacheMiss;
    if (cacheNotReady)
      ++stats->rootUnitSupplementResourceCacheNotReady;
  }

  bool semanticKeyResolved = false;
  ForEachRuntimeAlias(seed.runtimeModelPtr, [&](void* candidate) {
    if (found)
      return;
    if (semanticKeyResolveBudget != nullptr &&
        *semanticKeyResolveBudget == 0u) {
      return;
    }
    if (semanticKeyAttemptedRuntimes != nullptr &&
        !semanticKeyAttemptedRuntimes->insert(candidate).second) {
      return;
    }

    void* modelResourcePtr = nullptr;
    uint64_t modelKey = 0u;
    if (semanticKeyResolveBudget != nullptr)
      --(*semanticKeyResolveBudget);
    if (!TryResolveRuntimeModelSemanticKey(candidate, modelResourcePtr,
                                           modelKey)) {
      return;
    }

    semanticKeyResolved = true;
    model::ShadowModelResourceRecord candidateRecord = {};
    if (resourceCache.findRuntimeModelResource(candidate, candidateRecord) &&
        candidateRecord.readyForShadowConsumer()) {
      out = std::move(candidateRecord);
      found = true;
      return;
    }

    if (modelResourcePtr != nullptr &&
        resourceCache.findModelResource(modelResourcePtr, candidateRecord) &&
        candidateRecord.readyForShadowConsumer()) {
      out = std::move(candidateRecord);
      found = true;
    }
  });

  if (stats != nullptr && semanticKeyResolved)
    ++stats->rootUnitSupplementResourceSemanticKeyResolved;
  if (stats != nullptr && found)
    ++stats->rootUnitSupplementResourceSemanticKeyReady;
  if (found) {
    return true;
  }

  return false;
}

const ShadowModelResourceRecord* TryFindSupplementGeosetResource(
    const std::shared_ptr<const ShadowModelResourceStore>& resourcesPtr,
    const model::ShadowModelResourceCache& resourceCache,
    const RootUnitSupplementSeed& seed,
    const model::ShadowModelResourceRecord& runtimeResource,
    uint32_t geosetIndex,
    uint64_t frameSerial,
    ShadowModelResourceRecord& cacheOverlay,
    ShadowFrameStats& stats,
    bool& sawStoreRecord,
    bool& sawReadyStoreRecord,
    size_t& geosetCacheFallbackBudget) {
  if (resourcesPtr == nullptr)
    return nullptr;

  auto tryStoreRecord =
      [&](const ShadowModelResourceRecord* candidate)
          -> const ShadowModelResourceRecord* {
    if (candidate == nullptr)
      return nullptr;
    sawStoreRecord = true;
    if (!candidate->readyForConsumer())
      return nullptr;
    sawReadyStoreRecord = true;
    return candidate;
  };

  if (const auto* record = tryStoreRecord(
          resourcesPtr->findByRuntimeModel(seed.runtimeModelPtr, geosetIndex)))
    return record;
  if (runtimeResource.runtimeModelPtr != nullptr) {
    if (const auto* record = tryStoreRecord(resourcesPtr->findByRuntimeModel(
            runtimeResource.runtimeModelPtr, geosetIndex))) {
      return record;
    }
  }
  if (seed.modelResourcePtr != nullptr) {
    if (const auto* record = tryStoreRecord(resourcesPtr->findByModelResource(
            seed.modelResourcePtr, geosetIndex))) {
      return record;
    }
  }
  if (runtimeResource.modelResourcePtr != nullptr &&
      runtimeResource.modelResourcePtr != seed.modelResourcePtr) {
    if (const auto* record = tryStoreRecord(resourcesPtr->findByModelResource(
            runtimeResource.modelResourcePtr, geosetIndex))) {
      return record;
    }
  }
  if (geosetIndex < runtimeResource.geosetPtrs.size()) {
    if (const auto* record = tryStoreRecord(resourcesPtr->findByRuntimeGeoset(
            runtimeResource.geosetPtrs[geosetIndex]))) {
      return record;
    }
  }
  if (geosetIndex < runtimeResource.geosetDataPtrs.size()) {
    if (const auto* record =
            tryStoreRecord(resourcesPtr->findByRuntimeGeosetData(
                runtimeResource.geosetDataPtrs[geosetIndex]))) {
      return record;
    }
  }

  if (geosetCacheFallbackBudget == 0u)
    return nullptr;
  --geosetCacheFallbackBudget;

  model::ShadowGeosetResourceRecord geosetRecord = {};
  bool found = false;
  auto acceptGeoset = [&](const model::ShadowGeosetResourceRecord& candidate) {
    if (!candidate.readyForShadowConsumer())
      return false;
    cacheOverlay = ConvertGeoset(candidate, frameSerial);
    if (!cacheOverlay.readyForConsumer())
      return false;
    ++stats.rootUnitSupplementGeosetCacheFallback;
    return true;
  };

  ForEachRuntimeAlias(seed.runtimeModelPtr, [&](void* candidate) {
    if (found)
      return;
    model::ShadowGeosetResourceRecord candidateGeoset = {};
    if (resourceCache.findRuntimeModelGeoset(candidate, geosetIndex,
                                             candidateGeoset) &&
        acceptGeoset(candidateGeoset)) {
      found = true;
    }
  });
  if (found)
    return &cacheOverlay;

  if (runtimeResource.runtimeModelPtr != nullptr &&
      resourceCache.findRuntimeModelGeoset(runtimeResource.runtimeModelPtr,
                                           geosetIndex, geosetRecord) &&
      acceptGeoset(geosetRecord)) {
    return &cacheOverlay;
  }
  if (seed.modelResourcePtr != nullptr &&
      resourceCache.findModelGeoset(seed.modelResourcePtr, geosetIndex,
                                    geosetRecord) &&
      acceptGeoset(geosetRecord)) {
    return &cacheOverlay;
  }
  if (runtimeResource.modelResourcePtr != nullptr &&
      runtimeResource.modelResourcePtr != seed.modelResourcePtr &&
      resourceCache.findModelGeoset(runtimeResource.modelResourcePtr,
                                    geosetIndex, geosetRecord) &&
      acceptGeoset(geosetRecord)) {
    return &cacheOverlay;
  }
  if (geosetIndex < runtimeResource.geosetPtrs.size() &&
      resourceCache.findGeosetByPtr(runtimeResource.geosetPtrs[geosetIndex],
                                    geosetRecord) &&
      acceptGeoset(geosetRecord)) {
    return &cacheOverlay;
  }
  if (geosetIndex < runtimeResource.geosetDataPtrs.size() &&
      resourceCache.findGeosetByData(
          runtimeResource.geosetDataPtrs[geosetIndex], geosetRecord) &&
      acceptGeoset(geosetRecord)) {
    return &cacheOverlay;
  }

  return nullptr;
}

bool ManifestHasRootUnitSupplementRecord(
    const ShadowFrameManifest& manifest,
    void* runtimeModelPtr,
    void* modelResourcePtr,
    uint32_t geosetIndex) {
  if (geosetIndex == kInvalidShadowContractGeosetIndex)
    return false;

  for (const auto& record : manifest.records) {
    if (record.geosetIndex != geosetIndex)
      continue;
    if (runtimeModelPtr != nullptr) {
      if (record.runtimeModelPtr == runtimeModelPtr)
        return true;
      continue;
    }
    if (modelResourcePtr != nullptr &&
        record.runtimeModelPtr == nullptr &&
        record.modelResourcePtr == modelResourcePtr &&
        record.objectKind == render::ObjectKind::Unit) {
      return true;
    }
  }

  return false;
}

bool IsRootUnitSemanticRecord(const ShadowRenderableRecord& record) {
  return record.objectKind == render::ObjectKind::Unit &&
         record.runtimeModelPtr != nullptr &&
         record.modelResourcePtr != nullptr &&
         record.hasResolvedGeoset();
}

bool ManifestHasRootUnitSemanticRecords(const ShadowFrameManifest& manifest) {
  return std::any_of(manifest.records.begin(), manifest.records.end(),
                     [](const ShadowRenderableRecord& record) {
                       return IsRootUnitSemanticRecord(record);
                     });
}

void PrioritizeRootUnitSemanticRecords(ShadowFrameManifest& manifest) {
  std::stable_partition(
      manifest.records.begin(), manifest.records.end(),
      [](const ShadowRenderableRecord& record) {
        return IsRootUnitSemanticRecord(record);
      });
}

size_t ReusePriorRootUnitSupplementRecords(
    const ShadowFrameManifest* priorManifest,
    ShadowFrameManifest& manifest,
    const ShadowPoseStore& poses,
    const model::PoseRegistry& poseRegistry) {
  if (priorManifest == nullptr || priorManifest->records.empty())
    return 0u;
  if (priorManifest->frameSerial == 0u ||
      manifest.frameSerial <= priorManifest->frameSerial ||
      manifest.frameSerial > priorManifest->frameSerial + 4u) {
    return 0u;
  }

  constexpr size_t kMaxReusedRootUnitSupplementRecords = 16u;
  size_t reused = 0u;
  for (const auto& prior : priorManifest->records) {
    if (reused >= kMaxReusedRootUnitSupplementRecords)
      break;
    if (!IsRootUnitSemanticRecord(prior))
      continue;
    if (!HasContractMatrixPoseForRuntimeAlias(poses, prior.runtimeModelPtr) &&
        !HasSnapshotPoseForRuntimeAlias(poseRegistry, prior.runtimeModelPtr)) {
      continue;
    }
    if (ManifestHasRootUnitSupplementRecord(manifest, prior.runtimeModelPtr,
                                            prior.modelResourcePtr,
                                            prior.geosetIndex)) {
      continue;
    }

    ShadowRenderableRecord reusedRecord = prior;
    reusedRecord.frameSerial = manifest.frameSerial;
    manifest.records.push_back(std::move(reusedRecord));
    ++reused;
  }

  return reused;
}

void AppendRootUnitSupplementRecords(
    ShadowFrameManifest& manifest,
    const std::shared_ptr<const ShadowModelResourceStore>& resourcesPtr,
    const model::ShadowModelResourceCache& resourceCache,
    const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const model::PoseRegistry& poseRegistry,
    const ShadowPoseStore& poses,
    const ShadowAttachmentRigidStore& attachments,
    ShadowFrameStats& stats) {
  if (resourcesPtr == nullptr)
    return;

  // Formal semantic scene submission must shadow the current unit model, not
  // just the first one or two bootstrap geosets. The earlier bootstrap cap was
  // useful to prove the route, but it produced partial silhouettes that looked
  // like one caster fragment being reused across units.
  constexpr size_t kMaxSupplementRecords = 96u;
  constexpr uint32_t kMaxGeosetsPerRuntime = 12u;

  std::unordered_set<void*> attachmentChildRuntimes;
  for (const auto& attachment : attachments.records()) {
    if (attachment.childRuntimeModelPtr != nullptr)
      attachmentChildRuntimes.insert(attachment.childRuntimeModelPtr);
  }

  size_t appended = 0u;
  std::unordered_set<void*> semanticKeyAttemptedRuntimes;
  size_t semanticKeyResolveBudget = 8u;
  size_t geosetCacheFallbackBudget = 64u;
  std::vector<RootUnitSupplementSeed> seeds;
  seeds.reserve(attachments.records().size() * 2u + 32u);

  for (const auto& attachment : attachments.records()) {
    auto appendAttachmentSeed = [&](void* runtimeModelPtr) {
      if (runtimeModelPtr == nullptr)
        return;
      RootUnitSupplementSeed seed = {};
      seed.runtimeModelPtr = runtimeModelPtr;
      MergeRootUnitSeedFromAttachment(seed, attachment);
      TryMergeRootUnitSeedFromRegistries(seed, instanceRegistry, shadowRegistry,
                                         poseRegistry);
      seeds.push_back(seed);
    };
    appendAttachmentSeed(attachment.rootRuntimeModelPtr);
    if (attachment.ownerRuntimeModelPtr != attachment.rootRuntimeModelPtr)
      appendAttachmentSeed(attachment.ownerRuntimeModelPtr);
  }

  for (const auto& record : instanceRegistry.snapshot()) {
    if (record.runtimeModelPtr == nullptr || record.unitPtr == nullptr)
      continue;
    RootUnitSupplementSeed seed = {};
    MergeRootUnitSeedFromInstance(seed, record);
    TryMergeRootUnitSeedFromRegistries(seed, instanceRegistry, shadowRegistry,
                                       poseRegistry);
    seeds.push_back(seed);
  }

  for (const auto& record : shadowRegistry.snapshot()) {
    if (record.runtimeModelPtr == nullptr ||
        record.kind != render::ObjectKind::Unit) {
      continue;
    }
    RootUnitSupplementSeed seed = {};
    MergeRootUnitSeedFromShadow(seed, record);
    TryMergeRootUnitSeedFromRegistries(seed, instanceRegistry, shadowRegistry,
                                       poseRegistry);
    seeds.push_back(seed);
  }

  for (const auto& pose : poseRegistry.snapshot()) {
    if (pose.runtimeModelPtr == nullptr ||
        (pose.matrixCount == 0u && pose.matrixPalette.empty() &&
         !pose.hasWorldTransform && !pose.hasSpriteFrameTransform)) {
      continue;
    }

    RootUnitSupplementSeed seed = {};
    MergeRootUnitSeedFromPose(seed, pose);
    TryMergeRootUnitSeedFromRegistries(seed, instanceRegistry, shadowRegistry,
                                       poseRegistry);
    seeds.push_back(seed);
  }

  for (const auto& pose : poses.records()) {
    if (pose.runtimeModelPtr == nullptr ||
        (pose.matrixCount == 0u && pose.matrixPalette.empty() &&
         !pose.hasWorldTransform)) {
      continue;
    }

    RootUnitSupplementSeed seed = {};
    seed.runtimeModelPtr = pose.runtimeModelPtr;
    seed.sceneNode = pose.sceneNode;
    seed.unitPtr = pose.unitPtr;
    if (pose.unitPtr != nullptr)
      seed.objectKind = render::ObjectKind::Unit;
    TryMergeRootUnitSeedFromRegistries(seed, instanceRegistry, shadowRegistry,
                                       poseRegistry);
    seeds.push_back(seed);
  }

  for (auto seed : seeds) {
    if (appended >= kMaxSupplementRecords)
      break;
    stats.rootUnitSupplementSeedCount++;
    if (seed.objectKind == render::ObjectKind::Unit || seed.unitPtr != nullptr)
      stats.rootUnitSupplementUnitSeedCount++;
    const bool hasRuntimePose =
        seed.runtimeModelPtr != nullptr &&
        (HasSnapshotPoseForRuntimeAlias(poseRegistry, seed.runtimeModelPtr) ||
         HasContractMatrixPoseForRuntimeAlias(poses, seed.runtimeModelPtr));
    if (!HasRootUnitSupplementIdentity(seed) && !hasRuntimePose) {
      stats.rootUnitSupplementSkippedNoIdentity++;
      continue;
    }
    if (IsAttachmentChildRuntime(seed.runtimeModelPtr, attachmentChildRuntimes)) {
      stats.rootUnitSupplementSkippedAttachmentChild++;
      continue;
    }
    if (!hasRuntimePose) {
      stats.rootUnitSupplementSkippedNoPose++;
      continue;
    }

    model::ShadowModelResourceRecord runtimeResource = {};
    if (!TryFindRuntimeModelResourceForSeed(resourceCache, seed,
                                           runtimeResource, &stats,
                                           &semanticKeyAttemptedRuntimes,
                                           &semanticKeyResolveBudget)) {
      stats.rootUnitSupplementSkippedNoResource++;
      continue;
    }

    if (seed.modelResourcePtr == nullptr)
      seed.modelResourcePtr = runtimeResource.modelResourcePtr;
    if (seed.modelKey == 0u)
      seed.modelKey = runtimeResource.modelKey;

    const uint32_t geosetCount =
        (std::min)(runtimeResource.geosetCount, kMaxGeosetsPerRuntime);
    bool appendedAnyGeoset = false;
    bool sawStoreRecord = false;
    bool sawReadyStoreRecord = false;
    for (uint32_t geosetIndex = 0u;
         geosetIndex < geosetCount && appended < kMaxSupplementRecords;
         ++geosetIndex) {
      ShadowModelResourceRecord cacheOverlay = {};
      const ShadowModelResourceRecord* resource =
          TryFindSupplementGeosetResource(
              resourcesPtr, resourceCache, seed, runtimeResource, geosetIndex,
              manifest.frameSerial, cacheOverlay, stats, sawStoreRecord,
              sawReadyStoreRecord, geosetCacheFallbackBudget);
      if (resource == nullptr)
        continue;
      const uint32_t resolvedGeosetIndex =
          resource->geosetIndex != kInvalidShadowContractGeosetIndex
              ? resource->geosetIndex
              : geosetIndex;
      void* resolvedModelResource =
          resource->modelResourcePtr != nullptr
              ? resource->modelResourcePtr
              : seed.modelResourcePtr != nullptr
                    ? seed.modelResourcePtr
                    : runtimeResource.modelResourcePtr;
      if (ManifestHasRootUnitSupplementRecord(
              manifest, seed.runtimeModelPtr, resolvedModelResource,
              resolvedGeosetIndex)) {
        stats.rootUnitSupplementSkippedDuplicate++;
        continue;
      }

      ShadowRenderableRecord record = {};
      record.worldObjectEntry = seed.worldObjectEntry;
      record.sceneNode = seed.sceneNode;
      record.unitPtr = seed.unitPtr;
      record.runtimeModelPtr = seed.runtimeModelPtr;
      record.modelResourcePtr = resolvedModelResource;
      record.runtimeGeosetPtr = resource->runtimeGeosetPtr;
      record.runtimeGeosetDataPtr = resource->runtimeGeosetDataPtr;
      record.modelKey = seed.modelKey != 0u ? seed.modelKey : resource->modelKey;
      record.jHandle = seed.jHandle;
      record.rawcode = seed.rawcode;
      record.pathBlocker =
          dxvk::war3::internal::IsPathBlockerFourCc(record.rawcode);
      record.meshIndex = resolvedGeosetIndex;
      record.geosetIndex = resolvedGeosetIndex;
      record.objectKind =
          seed.objectKind != render::ObjectKind::Unknown
              ? seed.objectKind
              : seed.unitPtr != nullptr ? render::ObjectKind::Unit
                                        : render::ObjectKind::Unknown;
      record.queueKind = render::VisibleRenderableQueueKind::MainQueue;
      record.frameSerial = manifest.frameSerial;
      manifest.records.push_back(std::move(record));
      ++appended;
      ++stats.rootUnitSupplementAppended;
      appendedAnyGeoset = true;
    }
    if (!appendedAnyGeoset) {
      stats.rootUnitSupplementSkippedNoGeoset++;
      if (geosetCount == 0u)
        stats.rootUnitSupplementSkippedNoGeosetZeroCount++;
      else if (!sawStoreRecord)
        stats.rootUnitSupplementSkippedNoGeosetStoreMiss++;
      else if (!sawReadyStoreRecord)
        stats.rootUnitSupplementSkippedNoGeosetNotReady++;
    }
  }
}

ShadowAttachmentRigidRecord ConvertAttachmentRigid(
    const model::AttachmentRigidRecord& src, uint64_t frameSerial) {
  ShadowAttachmentRigidRecord dst = {};
  dst.rootRuntimeModelPtr = src.rootRuntimeModelPtr;
  dst.ownerRuntimeModelPtr = src.ownerRuntimeModelPtr;
  dst.childRuntimeModelPtr = src.childRuntimeModelPtr;
  dst.childSpritePtr = src.childSpritePtr;
  dst.childModelResourcePtr = nullptr;
  dst.childModelKey = 0u;
  dst.sourceObjectPtr = src.sourceObjectPtr;
  dst.sourceSpriteObjectPtr = src.sourceSpriteObjectPtr;
  dst.worldObjectEntry = src.worldObjectEntry;
  dst.sceneNode = src.sceneNode;
  dst.unitPtr = src.unitPtr;
  dst.jHandle = src.jHandle;
  dst.rawcode = src.rawcode;
  dst.slotIndex = src.slotIndex;
  dst.sourceRecordIndex = src.sourceRecordIndex;
  dst.childTag = src.childTag;
  dst.localPointX = src.localPointX;
  dst.localPointY = src.localPointY;
  dst.localPointZ = src.localPointZ;
  dst.frameSerial = frameSerial;
  return dst;
}

void MergeAttachmentIdentityFromInstance(
    ShadowAttachmentRigidRecord& dst, const model::ModelInstanceRecord& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
}

void MergeAttachmentIdentityFromShadow(
    ShadowAttachmentRigidRecord& dst, const render::ShadowObjectRecord& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
}

void MergeAttachmentIdentityFromPose(ShadowAttachmentRigidRecord& dst,
                                     const model::PoseRecord& src) {
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
}

void MergeAttachmentIdentityFromVisible(
    ShadowAttachmentRigidRecord& dst, const render::VisibleRenderableRecord& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.identity.worldObjectEntry;
  if (dst.sceneNode == nullptr) {
    dst.sceneNode =
        src.identity.sceneNode != nullptr ? src.identity.sceneNode : src.sceneNode;
  }
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.identity.unitPtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.identity.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.identity.rawcode;
}

void MergeAttachmentIdentityFromRenderObject(
    ShadowAttachmentRigidRecord& dst, const render::RenderObjectInfo& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
}

void MergeAttachmentIdentityFromRenderable(
    ShadowAttachmentRigidRecord& dst, const ShadowRenderableRecord& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
}

void MergeAttachmentIdentityFromRenderSnapshot(
    ShadowAttachmentRigidRecord& dst,
    const render::RenderObjectIdentitySnapshot& src) {
  if (dst.worldObjectEntry == nullptr)
    dst.worldObjectEntry = src.worldObjectEntry;
  if (dst.sceneNode == nullptr)
    dst.sceneNode = src.sceneNode;
  if (dst.unitPtr == nullptr)
    dst.unitPtr = src.unitPtr;
  if (dst.jHandle == 0u)
    dst.jHandle = src.jHandle;
  if (dst.rawcode == 0u)
    dst.rawcode = src.rawcode;
}

bool HasAttachmentIdentity(const ShadowAttachmentRigidRecord& record) {
  return record.worldObjectEntry != nullptr ||
         record.sceneNode != nullptr ||
         record.unitPtr != nullptr ||
         record.jHandle != 0u ||
         record.rawcode != 0u;
}

bool HasResolvedRenderIdentity(
    const render::RenderObjectIdentitySnapshot& snapshot) {
  return snapshot.worldObjectEntry != nullptr ||
         snapshot.unitPtr != nullptr ||
         snapshot.jHandle != 0u ||
         snapshot.rawcode != 0u ||
         snapshot.kind != render::ObjectKind::Unknown;
}

size_t CountAttachmentIdentityRecords(const ShadowAttachmentRigidStore& store) {
  size_t count = 0u;
  for (const auto& record : store.records()) {
    if (HasAttachmentIdentity(record))
      ++count;
  }
  return count;
}

bool ShouldPreferLiveAttachments(
    const std::shared_ptr<const ShadowAttachmentRigidStore>& published,
    const std::shared_ptr<const ShadowAttachmentRigidStore>& live) {
  if (live == nullptr || live->records().empty())
    return false;
  if (published == nullptr || published->records().empty())
    return true;

  const size_t publishedIdentityCount =
      CountAttachmentIdentityRecords(*published);
  const size_t liveIdentityCount = CountAttachmentIdentityRecords(*live);
  if (liveIdentityCount > publishedIdentityCount)
    return true;
  return liveIdentityCount == publishedIdentityCount &&
         live->records().size() > published->records().size();
}

bool HasShadowObjectIdentity(const render::ShadowObjectRecord& record) {
  return record.worldObjectEntry != nullptr ||
         record.sceneNode != nullptr ||
         record.unitPtr != nullptr ||
         record.jHandle != 0u ||
         record.rawcode != 0u;
}

bool HasRenderableIdentity(const ShadowRenderableRecord& record) {
  return record.worldObjectEntry != nullptr ||
         record.sceneNode != nullptr ||
         record.unitPtr != nullptr ||
         record.jHandle != 0u ||
         record.rawcode != 0u;
}

bool TryResolveRuntimeModelSemanticKey(void* runtimeModelPtr,
                                       void*& outModelResourcePtr,
                                       uint64_t& outModelKey) {
  outModelResourcePtr = nullptr;
  outModelKey = 0u;
  if (runtimeModelPtr == nullptr)
    return false;

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  model::ShadowModelResourceRecord resourceRecord = {};
  if (resourceCache.findRuntimeModelResource(runtimeModelPtr, resourceRecord)) {
    outModelResourcePtr = resourceRecord.modelResourcePtr;
    outModelKey = resourceRecord.modelKey;
    if (outModelResourcePtr != nullptr || outModelKey != 0u)
      return true;
  }

  auto tryResolveFromModelDataCandidate =
      [&](void* modelDataCandidate, uint64_t candidateModelKey) {
        if (modelDataCandidate == nullptr)
          return false;

        void* modelResourcePtr =
            resourceCache.resolveDirectModelResourcePtr(modelDataCandidate);
        if (modelResourcePtr == nullptr)
          return false;

        resourceCache.noteRuntimeModelBinding(runtimeModelPtr, modelResourcePtr,
                                             candidateModelKey);
        resourceCache.noteModelResourceBinding(modelResourcePtr,
                                              candidateModelKey);

        outModelResourcePtr = modelResourcePtr;
        if (outModelKey == 0u)
          outModelKey = candidateModelKey;

        model::ShadowModelResourceRecord directModelRecord = {};
        if (resourceCache.findModelResource(modelResourcePtr, directModelRecord) &&
            directModelRecord.modelKey != 0u) {
          outModelKey = directModelRecord.modelKey;
        }

        return outModelResourcePtr != nullptr || outModelKey != 0u;
      };

  model::ModelInstanceRecord instanceRecord = {};
  auto tryResolveFromInstanceRecord =
      [&](const model::ModelInstanceRecord& record) {
        if (record.modelResourcePtr != nullptr &&
            tryResolveFromModelDataCandidate(record.modelResourcePtr,
                                             record.modelKey)) {
          return true;
        }
        if (record.runtimeCreatorModelDataPtr != nullptr &&
            tryResolveFromModelDataCandidate(record.runtimeCreatorModelDataPtr,
                                             record.modelKey)) {
          return true;
        }
        if (record.runtimeCreatorHandlePtr != nullptr &&
            tryResolveFromModelDataCandidate(record.runtimeCreatorHandlePtr,
                                             record.modelKey)) {
          return true;
        }
        return false;
      };

  if (model::ModelInstanceRegistry::instance().findByRuntimeModel(
          runtimeModelPtr, instanceRecord) &&
      tryResolveFromInstanceRecord(instanceRecord)) {
    return true;
  }
  instanceRecord = {};
  if (model::ModelInstanceRegistry::instance().findOwnerByRuntimeModel(
          runtimeModelPtr, instanceRecord) &&
      tryResolveFromInstanceRecord(instanceRecord)) {
    return true;
  }

  model::ModelResourceRecord modelRecord = {};
  if (model::ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                          modelRecord) &&
      tryResolveFromModelDataCandidate(modelRecord.modelResourcePtr,
                                       modelRecord.modelKey)) {
    return true;
  }

  void* ownedModelDataHandle = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(runtimeModelPtr,
                                   dxvk::war3::CModelOffsets::OwnedModelDataHandle,
                                   ownedModelDataHandle) ||
      ownedModelDataHandle == nullptr) {
    return false;
  }

  outModelResourcePtr =
      resourceCache.resolveDirectModelResourcePtr(ownedModelDataHandle);
  if (outModelResourcePtr == nullptr)
    return false;

  model::ShadowModelResourceRecord directModelRecord = {};
  if (resourceCache.findModelResource(outModelResourcePtr, directModelRecord) &&
      directModelRecord.modelKey != 0u) {
    outModelKey = directModelRecord.modelKey;
  }

  return outModelResourcePtr != nullptr || outModelKey != 0u;
}

void* TryReadRuntimeModelFromSprite(void* spritePtr) {
  if (spritePtr == nullptr)
    return nullptr;

  void* runtimeModelPtr = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(spritePtr,
                                   dxvk::war3::CSpriteOffsets::Model,
                                   runtimeModelPtr) ||
      runtimeModelPtr == nullptr) {
    return nullptr;
  }

  return runtimeModelPtr;
}

bool LooksLikeChildRuntimeGroupHost(void* candidatePtr) {
  if (candidatePtr == nullptr)
    return false;

  uint32_t childGroupCount = 0u;
  void* childGroupRecords = nullptr;
  if (!dxvk::war3::SafeReadU32Fast(
          candidatePtr, dxvk::war3::CModelDataOffsets::ChildRuntimeGroupCount,
          childGroupCount) ||
      !dxvk::war3::SafeReadPtrFast(
          candidatePtr, dxvk::war3::CModelDataOffsets::ChildRuntimeGroupRecords,
          childGroupRecords)) {
    return false;
  }

  if (childGroupCount == 0u || childGroupCount > 1024u ||
      childGroupRecords == nullptr) {
    return false;
  }

  return dxvk::war3::IsReadableRange(childGroupRecords,
                                     size_t(childGroupCount) * 12u);
}

void* TryScanChildRuntimeGroupHostPtr(void* wrapperPtr) {
  if (wrapperPtr == nullptr)
    return nullptr;

  constexpr size_t kScanLimit = 0x40u;
  for (size_t offset = 0u; offset <= kScanLimit; offset += sizeof(void*)) {
    void* candidatePtr = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(wrapperPtr, offset, candidatePtr) ||
        candidatePtr == nullptr) {
      continue;
    }
    if (LooksLikeChildRuntimeGroupHost(candidatePtr))
      return candidatePtr;
  }

  return nullptr;
}

void* TryResolveParentModelDataForChildBootstrap(void* candidatePtr) {
  if (LooksLikeChildRuntimeGroupHost(candidatePtr))
    return candidatePtr;

  if (void* scannedPtr = TryScanChildRuntimeGroupHostPtr(candidatePtr);
      scannedPtr != nullptr) {
    return scannedPtr;
  }

  void* nestedModelDataPtr = nullptr;
  if (candidatePtr != nullptr &&
      dxvk::war3::SafeReadPtrFast(candidatePtr,
                                  dxvk::war3::CModelDataOffsets::ModelDataHandle,
                                  nestedModelDataPtr) &&
      nestedModelDataPtr != nullptr) {
    if (LooksLikeChildRuntimeGroupHost(nestedModelDataPtr))
      return nestedModelDataPtr;
    if (void* scannedPtr = TryScanChildRuntimeGroupHostPtr(nestedModelDataPtr);
        scannedPtr != nullptr) {
      return scannedPtr;
    }
  }

  return nullptr;
}

void PopulateAttachmentChildSemanticKey(ShadowAttachmentRigidRecord& attachment) {
  attachment.childModelResourcePtr = nullptr;
  attachment.childModelKey = 0u;
  if (attachment.childRuntimeModelPtr == nullptr)
    return;

  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0u;
  if (!TryResolveRuntimeModelSemanticKey(attachment.childRuntimeModelPtr,
                                         modelResourcePtr, modelKey)) {
    auto tryResolveFromChildSpriteRuntime = [&]() {
      void* childSpriteRuntimeModelPtr =
          TryReadRuntimeModelFromSprite(attachment.childSpritePtr);
      if (childSpriteRuntimeModelPtr == nullptr)
        return false;

      if (!TryResolveRuntimeModelSemanticKey(childSpriteRuntimeModelPtr,
                                             modelResourcePtr, modelKey)) {
        return false;
      }

      if (attachment.childRuntimeModelPtr != childSpriteRuntimeModelPtr &&
          attachment.childRuntimeModelPtr != nullptr &&
          modelResourcePtr != nullptr) {
        auto& resourceCache = model::ShadowModelResourceCache::instance();
        resourceCache.noteRuntimeModelBinding(attachment.childRuntimeModelPtr,
                                             modelResourcePtr, modelKey);
        resourceCache.noteModelResourceBinding(modelResourcePtr, modelKey);

        void* bridgedModelResourcePtr = nullptr;
        uint64_t bridgedModelKey = 0u;
        if (TryResolveRuntimeModelSemanticKey(attachment.childRuntimeModelPtr,
                                             bridgedModelResourcePtr,
                                             bridgedModelKey)) {
          modelResourcePtr = bridgedModelResourcePtr;
          modelKey = bridgedModelKey;
        }
      }

      return modelResourcePtr != nullptr || modelKey != 0u;
    };
    if (tryResolveFromChildSpriteRuntime()) {
      attachment.childModelResourcePtr = modelResourcePtr;
      attachment.childModelKey = modelKey;
      return;
    }

    auto tryResolveFromChildSprite = [&]() {
      if (attachment.childSpritePtr == nullptr)
        return false;

      auto& instanceRegistry = model::ModelInstanceRegistry::instance();
      model::ModelInstanceRecord instanceRecord = {};
      if (instanceRegistry.findBySpritePtr(attachment.childSpritePtr,
                                           instanceRecord)) {
        if (instanceRecord.runtimeModelPtr != nullptr &&
            TryResolveRuntimeModelSemanticKey(instanceRecord.runtimeModelPtr,
                                             modelResourcePtr, modelKey)) {
          return true;
        }
        if (instanceRecord.modelResourcePtr != nullptr)
          modelResourcePtr = instanceRecord.modelResourcePtr;
        if (instanceRecord.modelKey != 0u)
          modelKey = instanceRecord.modelKey;
        if (modelResourcePtr != nullptr || modelKey != 0u)
          return true;
      }

      model::ModelResourceRecord modelRecord = {};
      if (model::ModelRegistry::instance().findBySprite(
              attachment.childSpritePtr, modelRecord)) {
        if (modelRecord.modelResourcePtr != nullptr)
          modelResourcePtr = modelRecord.modelResourcePtr;
        if (modelRecord.modelKey != 0u)
          modelKey = modelRecord.modelKey;
        return modelResourcePtr != nullptr || modelKey != 0u;
      }

      render::ShadowObjectRecord shadowRecord = {};
      if (render::ShadowObjectRegistry::instance().findBySpritePtr(
              attachment.childSpritePtr, shadowRecord)) {
        if (shadowRecord.runtimeModelPtr != nullptr &&
            TryResolveRuntimeModelSemanticKey(shadowRecord.runtimeModelPtr,
                                             modelResourcePtr, modelKey)) {
          return true;
        }
        if (shadowRecord.modelResourcePtr != nullptr)
          modelResourcePtr = shadowRecord.modelResourcePtr;
        if (shadowRecord.modelKey != 0u)
          modelKey = shadowRecord.modelKey;
        return modelResourcePtr != nullptr || modelKey != 0u;
      }

      return false;
    };
    if (tryResolveFromChildSprite()) {
      attachment.childModelResourcePtr = modelResourcePtr;
      attachment.childModelKey = modelKey;
      return;
    }

    model::ModelInstanceRecord runtimeRecord = {};
    model::RuntimeParentLinkQueryResult parentLink = {};
    void* childModelDataPtr = nullptr;
    void* childModelResourcePtr = nullptr;
    auto tryResolveParentModelDataPtr = [&](void* parentRuntimeModelPtr) {
      if (parentRuntimeModelPtr == nullptr)
        return static_cast<void*>(nullptr);
      void* parentModelDataPtr = nullptr;
      auto tryResolveFromInstanceRecord =
          [&](const model::ModelInstanceRecord& instanceRecord) {
            if (void* directModelDataPtr =
                    TryResolveParentModelDataForChildBootstrap(
                        instanceRecord.runtimeCreatorModelDataPtr);
                directModelDataPtr != nullptr) {
              return directModelDataPtr;
            }
            if (void* directModelDataPtr =
                    TryResolveParentModelDataForChildBootstrap(
                        instanceRecord.runtimeCreatorHandlePtr);
                directModelDataPtr != nullptr) {
              return directModelDataPtr;
            }
            return static_cast<void*>(nullptr);
          };

      if (model::ModelInstanceRegistry::instance().findByRuntimeModel(
              parentRuntimeModelPtr, runtimeRecord)) {
        if (void* directModelDataPtr =
                tryResolveFromInstanceRecord(runtimeRecord);
            directModelDataPtr != nullptr) {
          return directModelDataPtr;
        }
      }
      runtimeRecord = {};
      if (model::ModelInstanceRegistry::instance().findOwnerByRuntimeModel(
              parentRuntimeModelPtr, runtimeRecord)) {
        if (void* directModelDataPtr =
                tryResolveFromInstanceRecord(runtimeRecord);
            directModelDataPtr != nullptr) {
          return directModelDataPtr;
        }
      }
      void* ownedModelDataHandle = nullptr;
      if (!dxvk::war3::SafeReadPtrFast(
              parentRuntimeModelPtr,
              dxvk::war3::CModelOffsets::OwnedModelDataHandle,
              ownedModelDataHandle) ||
          ownedModelDataHandle == nullptr) {
        return static_cast<void*>(nullptr);
      }
      if (void* directModelDataPtr =
              TryResolveParentModelDataForChildBootstrap(ownedModelDataHandle);
          directModelDataPtr != nullptr) {
        return directModelDataPtr;
      }
      if (dxvk::war3::SafeReadPtrFast(
              ownedModelDataHandle,
              dxvk::war3::CModelDataOffsets::ModelDataHandle,
              parentModelDataPtr) &&
          parentModelDataPtr != nullptr) {
        if (void* directModelDataPtr =
                TryResolveParentModelDataForChildBootstrap(parentModelDataPtr);
            directModelDataPtr != nullptr) {
          return directModelDataPtr;
        }
      }
      return static_cast<void*>(nullptr);
    };
    auto tryBootstrapFromRuntime = [&](void* parentRuntimeModelPtr) {
      if (parentRuntimeModelPtr == nullptr || parentLink.sourceMeta == 0u)
        return false;
      void* parentModelDataPtr =
          tryResolveParentModelDataPtr(parentRuntimeModelPtr);
      if (parentModelDataPtr == nullptr)
        return false;
      return model::TryBootstrapRuntimeChildLineageFromParentModelData(
          parentRuntimeModelPtr, parentModelDataPtr,
          attachment.childRuntimeModelPtr, parentLink.sourceMeta,
          parentLink.bucketIndex,
          childModelDataPtr, childModelResourcePtr);
    };

    if (model::QueryRuntimeParentLink(attachment.childRuntimeModelPtr,
                                      parentLink)) {
      if (!tryBootstrapFromRuntime(
              reinterpret_cast<void*>(parentLink.parentRuntimeModelPtr))) {
        tryBootstrapFromRuntime(attachment.ownerRuntimeModelPtr);
      }
    }

    if (!TryResolveRuntimeModelSemanticKey(attachment.childRuntimeModelPtr,
                                           modelResourcePtr, modelKey)) {
      return;
    }
  }

  if (modelResourcePtr != nullptr) {
    auto& resourceCache = model::ShadowModelResourceCache::instance();
    model::ShadowModelResourceRecord modelRecord = {};
    const bool modelRecordReady =
        resourceCache.findModelResource(modelResourcePtr, modelRecord) &&
        modelRecord.readyForShadowConsumer();
    model::ShadowModelResourceRecord runtimeRecord = {};
    const bool runtimeRecordReady =
        resourceCache.findRuntimeModelResource(attachment.childRuntimeModelPtr,
                                               runtimeRecord) &&
        runtimeRecord.modelResourcePtr == modelResourcePtr &&
        runtimeRecord.readyForShadowConsumer();
    if (!modelRecordReady)
      resourceCache.noteModelResourceBinding(modelResourcePtr, modelKey);
    if (!runtimeRecordReady) {
      resourceCache.noteRuntimeModelBinding(attachment.childRuntimeModelPtr,
                                            modelResourcePtr, modelKey);
    }
  }

  attachment.childModelResourcePtr = modelResourcePtr;
  attachment.childModelKey = modelKey;
}

void TryMergeAttachmentIdentityFromUniqueSemanticKey(
    void* runtimeModelPtr, const ShadowFrameManifest& manifest,
    const render::ShadowObjectRegistry& shadowRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0u;
  if (!TryResolveRuntimeModelSemanticKey(runtimeModelPtr, modelResourcePtr,
                                         modelKey)) {
    return;
  }

  const auto shadowSnapshot = shadowRegistry.snapshot();
  const render::ShadowObjectRecord* uniqueShadowRecord = nullptr;
  size_t shadowMatches = 0u;
  for (const auto& record : shadowSnapshot) {
    if (!HasShadowObjectIdentity(record))
      continue;
    const bool modelResourceMatch =
        modelResourcePtr != nullptr && record.modelResourcePtr == modelResourcePtr;
    const bool modelKeyMatch =
        modelKey != 0u && record.modelKey == modelKey;
    if (!modelResourceMatch && !modelKeyMatch)
      continue;
    uniqueShadowRecord = &record;
    shadowMatches += 1u;
    if (shadowMatches > 1u)
      break;
  }
  if (shadowMatches == 1u && uniqueShadowRecord != nullptr) {
    MergeAttachmentIdentityFromShadow(attachment, *uniqueShadowRecord);
  }

  const ShadowRenderableRecord* uniqueManifestRecord = nullptr;
  size_t manifestMatches = 0u;
  for (const auto& record : manifest.records) {
    if (!HasRenderableIdentity(record))
      continue;
    const bool modelResourceMatch =
        modelResourcePtr != nullptr && record.modelResourcePtr == modelResourcePtr;
    const bool modelKeyMatch =
        modelKey != 0u && record.modelKey == modelKey;
    if (!modelResourceMatch && !modelKeyMatch)
      continue;
    uniqueManifestRecord = &record;
    manifestMatches += 1u;
    if (manifestMatches > 1u)
      break;
  }
  if (manifestMatches == 1u && uniqueManifestRecord != nullptr) {
    MergeAttachmentIdentityFromRenderable(attachment, *uniqueManifestRecord);
  }
}

void* TryResolveSpritePtrFromRuntimeModel(
    void* runtimeModelPtr, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry) {
  if (runtimeModelPtr == nullptr)
    return nullptr;

  model::ModelResourceRecord modelRecord = {};
  if (model::ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                          modelRecord) &&
      modelRecord.spritePtr != nullptr) {
    return modelRecord.spritePtr;
  }

  model::ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord) &&
      instanceRecord.spritePtr != nullptr) {
    return instanceRecord.spritePtr;
  }
  if (instanceRegistry.findOwnerByRuntimeModel(runtimeModelPtr, instanceRecord) &&
      instanceRecord.spritePtr != nullptr) {
    return instanceRecord.spritePtr;
  }

  render::ShadowObjectRecord shadowRecord = {};
  if (shadowRegistry.findByRuntimeModel(runtimeModelPtr, shadowRecord) &&
      shadowRecord.spritePtr != nullptr) {
    return shadowRecord.spritePtr;
  }

  return nullptr;
}

void TryMergeAttachmentIdentityFromSpriteChain(
    void* spritePtr, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  if (spritePtr == nullptr)
    return;

  std::unordered_set<void*> visitedSprites;
  visitedSprites.reserve(8u);
  void* currentSprite = spritePtr;
  constexpr uint32_t kMaxParentDepth = 8u;
  for (uint32_t depth = 0u;
       currentSprite != nullptr && depth < kMaxParentDepth &&
       visitedSprites.insert(currentSprite).second;
       ++depth) {
    model::ModelInstanceRecord instanceRecord = {};
    if (instanceRegistry.findBySpritePtr(currentSprite, instanceRecord))
      MergeAttachmentIdentityFromInstance(attachment, instanceRecord);

    render::ShadowObjectRecord shadowRecord = {};
    if (shadowRegistry.findBySpritePtr(currentSprite, shadowRecord))
      MergeAttachmentIdentityFromShadow(attachment, shadowRecord);

    void* currentRuntimeModelPtr = nullptr;
    if (dxvk::war3::SafeReadPtrFast(currentSprite,
                                    dxvk::war3::CSpriteOffsets::Model,
                                    currentRuntimeModelPtr) &&
        currentRuntimeModelPtr != nullptr) {
      if (instanceRegistry.findOwnerByRuntimeModel(currentRuntimeModelPtr,
                                                   instanceRecord)) {
        MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
      }
      if (instanceRegistry.findByRuntimeModel(currentRuntimeModelPtr,
                                              instanceRecord)) {
        MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
      }
      if (shadowRegistry.findByRuntimeModel(currentRuntimeModelPtr,
                                            shadowRecord)) {
        MergeAttachmentIdentityFromShadow(attachment, shadowRecord);
      }
    }

    void* parentSprite = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(currentSprite,
                                     dxvk::war3::CSpriteOffsets::ParentSprite,
                                     parentSprite)) {
      parentSprite = nullptr;
    }
    if (HasAttachmentIdentity(attachment) &&
        attachment.worldObjectEntry != nullptr &&
        attachment.sceneNode != nullptr &&
        attachment.unitPtr != nullptr &&
        attachment.jHandle != 0u &&
        attachment.rawcode != 0u) {
      break;
    }
    currentSprite = parentSprite;
  }
}

void TryMergeAttachmentIdentityFromSceneNodeHint(
    void* sceneNodeHint, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const render::VisibleRenderableRegistry& visibleRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  if (sceneNodeHint == nullptr)
    return;

  model::ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findBySceneNode(sceneNodeHint, instanceRecord))
    MergeAttachmentIdentityFromInstance(attachment, instanceRecord);

  render::ShadowObjectRecord shadowRecord = {};
  if (shadowRegistry.findBySceneNode(sceneNodeHint, shadowRecord))
    MergeAttachmentIdentityFromShadow(attachment, shadowRecord);

  render::VisibleRenderableRecord visibleRecord = {};
  if (visibleRegistry.queryBySceneNode(sceneNodeHint, visibleRecord))
    MergeAttachmentIdentityFromVisible(attachment, visibleRecord);

  if (const auto* renderObject =
          render::RenderObjectRegistry::instance().findBySceneNode(
              sceneNodeHint)) {
    MergeAttachmentIdentityFromRenderObject(attachment, *renderObject);
  }

  render::RenderObjectIdentitySnapshot renderIdentity = {};
  if (render::TryResolveRenderObjectIdentity(nullptr, sceneNodeHint,
                                             renderIdentity) &&
      HasResolvedRenderIdentity(renderIdentity)) {
    if (renderIdentity.sceneNode == nullptr)
      renderIdentity.sceneNode = sceneNodeHint;
    MergeAttachmentIdentityFromRenderSnapshot(attachment, renderIdentity);
  }
}

void TryMergeAttachmentIdentityFromRenderIdentityCandidate(
    void* identityCandidate, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const render::VisibleRenderableRegistry& visibleRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  if (identityCandidate == nullptr)
    return;

  model::ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findByWorldObjectEntry(identityCandidate, instanceRecord) ||
      instanceRegistry.findBySceneNode(identityCandidate, instanceRecord)) {
    MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
  }

  render::ShadowObjectRecord shadowRecord = {};
  if (shadowRegistry.findByWorldObjectEntry(identityCandidate, shadowRecord) ||
      shadowRegistry.findBySceneNode(identityCandidate, shadowRecord)) {
    MergeAttachmentIdentityFromShadow(attachment, shadowRecord);
  }

  render::VisibleRenderableRecord visibleRecord = {};
  if (visibleRegistry.queryByWorldObjectEntry(identityCandidate, visibleRecord) ||
      visibleRegistry.queryBySceneNode(identityCandidate, visibleRecord)) {
    MergeAttachmentIdentityFromVisible(attachment, visibleRecord);
  }

  if (const auto* renderObject =
          render::RenderObjectRegistry::instance().findByEntry(
              identityCandidate)) {
    MergeAttachmentIdentityFromRenderObject(attachment, *renderObject);
  }
  if (const auto* renderObject =
          render::RenderObjectRegistry::instance().findBySceneNode(
              identityCandidate)) {
    MergeAttachmentIdentityFromRenderObject(attachment, *renderObject);
  }

  render::RenderObjectIdentitySnapshot renderIdentity = {};
  if (render::TryResolveRenderObjectIdentity(identityCandidate, nullptr,
                                             renderIdentity) &&
      HasResolvedRenderIdentity(renderIdentity)) {
    if (renderIdentity.worldObjectEntry == nullptr)
      renderIdentity.worldObjectEntry = identityCandidate;
    MergeAttachmentIdentityFromRenderSnapshot(attachment, renderIdentity);
  }

  renderIdentity = {};
  if (render::TryResolveRenderObjectIdentity(nullptr, identityCandidate,
                                             renderIdentity) &&
      HasResolvedRenderIdentity(renderIdentity)) {
    if (renderIdentity.sceneNode == nullptr)
      renderIdentity.sceneNode = identityCandidate;
    MergeAttachmentIdentityFromRenderSnapshot(attachment, renderIdentity);
  }
}

void TryMergeAttachmentIdentityFromRenderIdentityWrapper(
    void* wrapperPtr, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const render::VisibleRenderableRegistry& visibleRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  if (wrapperPtr == nullptr)
    return;

  constexpr size_t kWrapperScanLimit = 0x40u;
  constexpr size_t kPointerStride = sizeof(void*);
  std::unordered_set<void*> visitedCandidates;
  visitedCandidates.reserve(16u);

  for (size_t offset = 0u; offset <= kWrapperScanLimit; offset += kPointerStride) {
    void* candidatePtr = nullptr;
    if (!dxvk::war3::SafeReadPtrFast(wrapperPtr, offset, candidatePtr) ||
        candidatePtr == nullptr ||
        !visitedCandidates.insert(candidatePtr).second) {
      continue;
    }

    TryMergeAttachmentIdentityFromRenderIdentityCandidate(
        candidatePtr, instanceRegistry, shadowRegistry, visibleRegistry,
        attachment);
  }
}

const ShadowRenderableRecord* FindManifestAttachmentIdentityCandidate(
    const ShadowFrameManifest& manifest,
    const ShadowAttachmentRigidRecord& attachment) {
  const ShadowRenderableRecord* best = nullptr;
  uint32_t bestScore = 0u;

  for (const auto& candidate : manifest.records) {
    const bool hasIdentity =
        candidate.worldObjectEntry != nullptr ||
        candidate.sceneNode != nullptr ||
        candidate.unitPtr != nullptr ||
        candidate.jHandle != 0u ||
        candidate.rawcode != 0u;
    if (!hasIdentity)
      continue;

    uint32_t score = 0u;
    if (attachment.worldObjectEntry != nullptr &&
        candidate.worldObjectEntry == attachment.worldObjectEntry) {
      score += 32u;
    }
    if (attachment.sceneNode != nullptr &&
        candidate.sceneNode == attachment.sceneNode) {
      score += 24u;
    }
    if (attachment.unitPtr != nullptr &&
        candidate.unitPtr == attachment.unitPtr) {
      score += 24u;
    }
    if (attachment.childRuntimeModelPtr != nullptr &&
        candidate.runtimeModelPtr == attachment.childRuntimeModelPtr) {
      score += 16u;
    }
    if (attachment.rootRuntimeModelPtr != nullptr &&
        candidate.runtimeModelPtr == attachment.rootRuntimeModelPtr) {
      score += 8u;
    }
    if (attachment.jHandle != 0u && candidate.jHandle == attachment.jHandle)
      score += 4u;
    if (attachment.rawcode != 0u && candidate.rawcode == attachment.rawcode)
      score += 2u;
    if (candidate.objectKind != render::ObjectKind::Unknown)
      score += 1u;

    if (score == 0u)
      continue;

    if (best == nullptr || score > bestScore ||
        (score == bestScore && best->runtimeModelPtr == nullptr &&
         candidate.runtimeModelPtr != nullptr)) {
      best = &candidate;
      bestScore = score;
    }
  }

  return best;
}

void TryMergeAttachmentIdentityFromRuntimeModel(
    void* runtimeModelPtr, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const model::PoseRegistry& poseRegistry,
    const render::VisibleRenderableRegistry& visibleRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  if (runtimeModelPtr == nullptr)
    return;

  model::ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
  if (instanceRegistry.findOwnerByRuntimeModel(runtimeModelPtr, instanceRecord))
    MergeAttachmentIdentityFromInstance(attachment, instanceRecord);

  render::ShadowObjectRecord shadowRecord = {};
  if (shadowRegistry.findByRuntimeModel(runtimeModelPtr, shadowRecord))
    MergeAttachmentIdentityFromShadow(attachment, shadowRecord);

  model::PoseRecord poseRecord = {};
  if (poseRegistry.findByRuntimeModel(runtimeModelPtr, poseRecord))
    MergeAttachmentIdentityFromPose(attachment, poseRecord);

  render::VisibleRenderableRecord visibleRecord = {};
  if (visibleRegistry.queryByRuntimeModel(runtimeModelPtr, visibleRecord))
    MergeAttachmentIdentityFromVisible(attachment, visibleRecord);

  void* spritePtr =
      TryResolveSpritePtrFromRuntimeModel(runtimeModelPtr, instanceRegistry,
                                          shadowRegistry);
  if (spritePtr != nullptr) {
    TryMergeAttachmentIdentityFromSpriteChain(spritePtr, instanceRegistry,
                                              shadowRegistry, attachment);
  }

  auto tryRuntimeAlias = [&](void* aliasRuntimeModelPtr) {
    if (aliasRuntimeModelPtr == nullptr ||
        aliasRuntimeModelPtr == runtimeModelPtr) {
      return;
    }

    model::ModelInstanceRecord aliasInstanceRecord = {};
    if (instanceRegistry.findByRuntimeModel(aliasRuntimeModelPtr,
                                            aliasInstanceRecord)) {
      MergeAttachmentIdentityFromInstance(attachment, aliasInstanceRecord);
    }
    if (instanceRegistry.findOwnerByRuntimeModel(aliasRuntimeModelPtr,
                                                 aliasInstanceRecord)) {
      MergeAttachmentIdentityFromInstance(attachment, aliasInstanceRecord);
    }

    render::ShadowObjectRecord aliasShadowRecord = {};
    if (shadowRegistry.findByRuntimeModel(aliasRuntimeModelPtr,
                                          aliasShadowRecord)) {
      MergeAttachmentIdentityFromShadow(attachment, aliasShadowRecord);
    }

    model::PoseRecord aliasPoseRecord = {};
    if (poseRegistry.findByRuntimeModel(aliasRuntimeModelPtr, aliasPoseRecord))
      MergeAttachmentIdentityFromPose(attachment, aliasPoseRecord);

    render::VisibleRenderableRecord aliasVisibleRecord = {};
    if (visibleRegistry.queryByRuntimeModel(aliasRuntimeModelPtr,
                                            aliasVisibleRecord)) {
      MergeAttachmentIdentityFromVisible(attachment, aliasVisibleRecord);
    }

    void* aliasSpritePtr = TryResolveSpritePtrFromRuntimeModel(
        aliasRuntimeModelPtr, instanceRegistry, shadowRegistry);
    if (aliasSpritePtr != nullptr) {
      TryMergeAttachmentIdentityFromSpriteChain(aliasSpritePtr,
                                                instanceRegistry,
                                                shadowRegistry, attachment);
    }
  };

  // Resource/geoset ownership is often keyed by the CModel base, while the
  // frame-hot sprite/source observation lands on the CModelComplex +0xA0 view.
  // Merge both aliases into the contract before the renderer consumes it.
  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const uintptr_t runtimeValue = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (runtimeValue >= 0x10000u) {
    tryRuntimeAlias(reinterpret_cast<void*>(
        runtimeValue + kCModelComplexExtensionOffset));
    if (runtimeValue > kCModelComplexExtensionOffset) {
      tryRuntimeAlias(reinterpret_cast<void*>(
          runtimeValue - kCModelComplexExtensionOffset));
    }
  }
}

void TryMergeAttachmentIdentityFromSourceObject(
    void* sourceObjectPtr, const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const render::VisibleRenderableRegistry& visibleRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  if (sourceObjectPtr == nullptr)
    return;

  model::ModelInstanceRecord sourceInstanceRecord = {};
  if (instanceRegistry.findBySourceObject(sourceObjectPtr, sourceInstanceRecord))
    MergeAttachmentIdentityFromInstance(attachment, sourceInstanceRecord);
  if (instanceRegistry.findBySourceSpriteObject(sourceObjectPtr,
                                                sourceInstanceRecord)) {
    MergeAttachmentIdentityFromInstance(attachment, sourceInstanceRecord);
  }

  auto mergeByUnitPtr = [&](void* unitPtr) {
    if (unitPtr == nullptr)
      return;
    model::ModelInstanceRecord instanceRecord = {};
    if (instanceRegistry.findByUnitPtr(unitPtr, instanceRecord))
      MergeAttachmentIdentityFromInstance(attachment, instanceRecord);

    render::ShadowObjectRecord shadowRecord = {};
    if (shadowRegistry.findByUnitPtr(unitPtr, shadowRecord))
      MergeAttachmentIdentityFromShadow(attachment, shadowRecord);
  };

  dxvk::war3::game::AgentWrapper sourceAgent(sourceObjectPtr);
  void* sourceUnitPtr = sourceObjectPtr;
  if (!dxvk::war3::game::UnitWrapper(sourceUnitPtr).IsValid())
    sourceUnitPtr = sourceAgent.GetUnitPtr();
  mergeByUnitPtr(sourceUnitPtr);

  const auto renderObjects = render::RenderObjectRegistry::instance().getAllObjects();
  const render::RenderObjectInfo* uniqueRenderObject = nullptr;
  for (const auto& candidate : renderObjects) {
    const bool matchesSourceObject = candidate.agentPtr == sourceObjectPtr;
    const bool matchesUnit =
        sourceUnitPtr != nullptr && candidate.unitPtr == sourceUnitPtr;
    if (!matchesSourceObject && !matchesUnit)
      continue;
    if (uniqueRenderObject != nullptr) {
      uniqueRenderObject = nullptr;
      break;
    }
    uniqueRenderObject = &candidate;
  }
  if (uniqueRenderObject != nullptr)
    MergeAttachmentIdentityFromRenderObject(attachment, *uniqueRenderObject);

  void* sourceSceneNode = nullptr;
  if (dxvk::war3::SafeReadPtrFast(sourceObjectPtr, 0x20u, sourceSceneNode) &&
      reinterpret_cast<uintptr_t>(sourceSceneNode) > 0x10000u &&
      sourceSceneNode != nullptr) {
    render::ShadowObjectRecord shadowRecord = {};
    if (shadowRegistry.findBySceneNode(sourceSceneNode, shadowRecord))
      MergeAttachmentIdentityFromShadow(attachment, shadowRecord);

    render::VisibleRenderableRecord visibleRecord = {};
    if (visibleRegistry.queryBySceneNode(sourceSceneNode, visibleRecord))
      MergeAttachmentIdentityFromVisible(attachment, visibleRecord);

    model::ModelInstanceRecord instanceRecord = {};
    if (instanceRegistry.findBySceneNode(sourceSceneNode, instanceRecord))
      MergeAttachmentIdentityFromInstance(attachment, instanceRecord);

    render::RenderObjectIdentitySnapshot renderIdentity = {};
    if (render::TryResolveRenderObjectIdentity(nullptr, sourceSceneNode,
                                               renderIdentity) &&
        HasResolvedRenderIdentity(renderIdentity)) {
      if (renderIdentity.sceneNode == nullptr)
        renderIdentity.sceneNode = sourceSceneNode;
      MergeAttachmentIdentityFromRenderSnapshot(attachment, renderIdentity);
    }
    TryMergeAttachmentIdentityFromSceneNodeHint(
        sourceSceneNode, instanceRegistry, shadowRegistry, visibleRegistry,
        attachment);
  }
}

void RepairAttachmentRigidIdentity(
    const ShadowFrameManifest& manifest,
    const render::VisibleRenderableRegistry& visibleRegistry,
    const model::ModelInstanceRegistry& instanceRegistry,
    const render::ShadowObjectRegistry& shadowRegistry,
    const model::PoseRegistry& poseRegistry,
    ShadowAttachmentRigidRecord& attachment) {
  TryMergeAttachmentIdentityFromRuntimeModel(
      attachment.childRuntimeModelPtr, instanceRegistry, shadowRegistry,
      poseRegistry, visibleRegistry, attachment);
  TryMergeAttachmentIdentityFromUniqueSemanticKey(
      attachment.childRuntimeModelPtr, manifest, shadowRegistry, attachment);
  if (attachment.ownerRuntimeModelPtr != nullptr &&
      attachment.ownerRuntimeModelPtr != attachment.childRuntimeModelPtr &&
      attachment.ownerRuntimeModelPtr != attachment.rootRuntimeModelPtr) {
    TryMergeAttachmentIdentityFromRuntimeModel(
        attachment.ownerRuntimeModelPtr, instanceRegistry, shadowRegistry,
        poseRegistry, visibleRegistry, attachment);
    TryMergeAttachmentIdentityFromUniqueSemanticKey(
        attachment.ownerRuntimeModelPtr, manifest, shadowRegistry, attachment);
  }
  if (attachment.rootRuntimeModelPtr != attachment.childRuntimeModelPtr) {
    TryMergeAttachmentIdentityFromRuntimeModel(
        attachment.rootRuntimeModelPtr, instanceRegistry, shadowRegistry,
        poseRegistry, visibleRegistry, attachment);
    TryMergeAttachmentIdentityFromUniqueSemanticKey(
        attachment.rootRuntimeModelPtr, manifest, shadowRegistry, attachment);
  }

  TryMergeAttachmentIdentityFromSourceObject(
      attachment.sourceObjectPtr, instanceRegistry, shadowRegistry,
      visibleRegistry, attachment);
  TryMergeAttachmentIdentityFromSourceObject(
      attachment.sourceSpriteObjectPtr, instanceRegistry, shadowRegistry,
      visibleRegistry, attachment);
  TryMergeAttachmentIdentityFromRenderIdentityWrapper(
      attachment.sourceSpriteObjectPtr, instanceRegistry, shadowRegistry,
      visibleRegistry, attachment);
  TryMergeAttachmentIdentityFromSpriteChain(
      attachment.childSpritePtr, instanceRegistry, shadowRegistry, attachment);
  TryMergeAttachmentIdentityFromSpriteChain(
      attachment.sourceSpriteObjectPtr, instanceRegistry, shadowRegistry,
      attachment);
  TryMergeAttachmentIdentityFromSceneNodeHint(
      attachment.ownerRuntimeModelPtr, instanceRegistry, shadowRegistry,
      visibleRegistry, attachment);
  if (attachment.rootRuntimeModelPtr != attachment.ownerRuntimeModelPtr) {
    TryMergeAttachmentIdentityFromSceneNodeHint(
        attachment.rootRuntimeModelPtr, instanceRegistry, shadowRegistry,
        visibleRegistry, attachment);
  }
  if (attachment.childRuntimeModelPtr != attachment.ownerRuntimeModelPtr &&
      attachment.childRuntimeModelPtr != attachment.rootRuntimeModelPtr) {
    TryMergeAttachmentIdentityFromSceneNodeHint(
        attachment.childRuntimeModelPtr, instanceRegistry, shadowRegistry,
        visibleRegistry, attachment);
  }

  model::ModelInstanceRecord instanceRecord = {};
  render::ShadowObjectRecord shadowRecord = {};
  render::VisibleRenderableRecord visibleRecord = {};
  const auto renderObjects =
      render::RenderObjectRegistry::instance().getAllObjects();

  if (attachment.worldObjectEntry != nullptr) {
    if (instanceRegistry.findByWorldObjectEntry(attachment.worldObjectEntry,
                                                instanceRecord)) {
      MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
    }
    if (shadowRegistry.findByWorldObjectEntry(attachment.worldObjectEntry,
                                              shadowRecord)) {
      MergeAttachmentIdentityFromShadow(attachment, shadowRecord);
    }
    if (visibleRegistry.queryByWorldObjectEntry(attachment.worldObjectEntry,
                                                visibleRecord)) {
      MergeAttachmentIdentityFromVisible(attachment, visibleRecord);
    }
  }

  if (attachment.sceneNode != nullptr) {
    if (instanceRegistry.findBySceneNode(attachment.sceneNode, instanceRecord))
      MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
    if (shadowRegistry.findBySceneNode(attachment.sceneNode, shadowRecord))
      MergeAttachmentIdentityFromShadow(attachment, shadowRecord);
    if (visibleRegistry.queryBySceneNode(attachment.sceneNode, visibleRecord))
      MergeAttachmentIdentityFromVisible(attachment, visibleRecord);
  }

  if (attachment.unitPtr != nullptr) {
    if (instanceRegistry.findByUnitPtr(attachment.unitPtr, instanceRecord))
      MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
    if (shadowRegistry.findByUnitPtr(attachment.unitPtr, shadowRecord))
      MergeAttachmentIdentityFromShadow(attachment, shadowRecord);
    const render::RenderObjectInfo* uniqueRenderObject = nullptr;
    for (const auto& candidate : renderObjects) {
      if (candidate.unitPtr != attachment.unitPtr)
        continue;
      if (uniqueRenderObject != nullptr) {
        uniqueRenderObject = nullptr;
        break;
      }
      uniqueRenderObject = &candidate;
    }
    if (uniqueRenderObject != nullptr)
      MergeAttachmentIdentityFromRenderObject(attachment, *uniqueRenderObject);
  }

  if (attachment.jHandle != 0u) {
    if (instanceRegistry.findByHandle(attachment.jHandle, instanceRecord))
      MergeAttachmentIdentityFromInstance(attachment, instanceRecord);
    if (shadowRegistry.findByHandle(attachment.jHandle, shadowRecord))
      MergeAttachmentIdentityFromShadow(attachment, shadowRecord);
    if (visibleRegistry.queryByHandle(attachment.jHandle, visibleRecord))
      MergeAttachmentIdentityFromVisible(attachment, visibleRecord);
    if (const auto* renderObject =
            render::RenderObjectRegistry::instance().findByHandle(
                attachment.jHandle)) {
      MergeAttachmentIdentityFromRenderObject(attachment, *renderObject);
    }
  }

  if (const auto* manifestCandidate =
          FindManifestAttachmentIdentityCandidate(manifest, attachment)) {
    MergeAttachmentIdentityFromRenderable(attachment, *manifestCandidate);
  }
}

std::shared_ptr<const ShadowAttachmentRigidStore> BuildLiveAttachmentStore(
    const ShadowFrameManifest& manifest) {
  auto liveAttachments = std::make_shared<ShadowAttachmentRigidStore>();
  const auto rawAttachments = model::AttachmentRigidRegistry::instance().snapshot();
  if (rawAttachments.empty())
    return liveAttachments;

  auto& visibleRegistry = render::VisibleRenderableRegistry::instance();
  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  auto& poseRegistry = model::PoseRegistry::instance();
  for (const auto& attachment : rawAttachments) {
    auto contractAttachment =
        ConvertAttachmentRigid(attachment, manifest.frameSerial);
    RepairAttachmentRigidIdentity(manifest, visibleRegistry, instanceRegistry,
                                  shadowRegistry, poseRegistry,
                                  contractAttachment);
    PopulateAttachmentChildSemanticKey(contractAttachment);
    liveAttachments->add(std::move(contractAttachment));
  }
  return liveAttachments;
}

} // namespace

size_t ShadowModelResourceStore::ModelGeosetKeyHash::operator()(
    const ModelGeosetKey& key) const {
  const size_t h1 = std::hash<void*>()(key.ptr);
  const size_t h2 = std::hash<uint32_t>()(key.geosetIndex);
  return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
}

ShadowModelResourceMemorySnapshot
ShadowModelResourceStore::memorySnapshot() const {
  ShadowModelResourceMemorySnapshot result;
  result.records = m_records.size();
  result.recordVectorCapacityBytes =
      m_records.capacity() * sizeof(ShadowModelResourceRecord);
  for (const auto& record : m_records) {
    result.positionsCapacityBytes +=
        record.positions.capacity() * sizeof(float);
    result.normalsCapacityBytes +=
        record.normals.capacity() * sizeof(float);
    result.groupSlotsCapacityBytes +=
        record.vertexGroupIndices.capacity() * sizeof(uint8_t);
    result.primitiveCapacityBytes +=
        record.primitiveRecords.capacity() * sizeof(ShadowPrimitiveRecord);
    result.matrixGroupsCapacityBytes +=
        record.matrixGroupSizes.capacity() * sizeof(uint32_t);
    result.matrixIndicesCapacityBytes +=
        record.matrixIndices.capacity() * sizeof(uint32_t);
    result.indicesCapacityBytes +=
        record.indices.capacity() * sizeof(uint16_t);
    result.uvCapacityBytes +=
        record.uvLayers.capacity() * sizeof(std::vector<float>);
    for (const auto& layer : record.uvLayers)
      result.uvCapacityBytes += layer.capacity() * sizeof(float);
  }
  result.payloadCapacityBytes = result.positionsCapacityBytes +
      result.normalsCapacityBytes + result.groupSlotsCapacityBytes +
      result.primitiveCapacityBytes + result.matrixGroupsCapacityBytes +
      result.matrixIndicesCapacityBytes + result.indicesCapacityBytes +
      result.uvCapacityBytes;
  return result;
}

void ShadowModelResourceStore::clear() {
  m_records.clear();
  m_byRuntimeGeoset.clear();
  m_byRuntimeGeosetData.clear();
  m_byRuntimeModel.clear();
  m_byModelResource.clear();
}

void ShadowModelResourceStore::add(ShadowModelResourceRecord record) {
  const size_t index = m_records.size();
  m_records.emplace_back(std::move(record));
  const auto& stored = m_records.back();
  if (stored.runtimeGeosetPtr != nullptr)
    m_byRuntimeGeoset[stored.runtimeGeosetPtr] = index;
  if (stored.runtimeGeosetDataPtr != nullptr)
    m_byRuntimeGeosetData[stored.runtimeGeosetDataPtr] = index;
  if (stored.modelResourcePtr != nullptr &&
      stored.geosetIndex != kInvalidShadowContractGeosetIndex) {
    const ModelGeosetKey key{stored.modelResourcePtr, stored.geosetIndex};
    const auto itExisting = m_byModelResource.find(key);
    if (itExisting == m_byModelResource.end() ||
        itExisting->second >= m_records.size() ||
        !m_records[itExisting->second].prefersRuntimeContract ||
        stored.prefersRuntimeContract) {
      m_byModelResource[key] = index;
    }
  }
}

void ShadowModelResourceStore::bindRuntimeModelAlias(void* runtimeModelPtr,
                                                     uint32_t geosetIndex,
                                                     void* modelResourcePtr) {
  if (runtimeModelPtr == nullptr ||
      geosetIndex == kInvalidShadowContractGeosetIndex)
    return;

  if (modelResourcePtr != nullptr) {
    const auto it = m_byModelResource.find({modelResourcePtr, geosetIndex});
    if (it != m_byModelResource.end() && it->second < m_records.size() &&
        m_records[it->second].prefersRuntimeContract) {
      m_byRuntimeModel[{runtimeModelPtr, geosetIndex}] = it->second;
      return;
    }
  }

  for (size_t i = 0; i < m_records.size(); ++i) {
    const auto& record = m_records[i];
    if (record.geosetIndex == geosetIndex &&
        record.prefersRuntimeContract &&
        (modelResourcePtr == nullptr ||
         record.modelResourcePtr == modelResourcePtr)) {
      m_byRuntimeModel[{runtimeModelPtr, geosetIndex}] = i;
      return;
    }
  }

  if (modelResourcePtr != nullptr) {
    const auto it = m_byModelResource.find({modelResourcePtr, geosetIndex});
    if (it != m_byModelResource.end()) {
      m_byRuntimeModel[{runtimeModelPtr, geosetIndex}] = it->second;
      return;
    }
  }

  for (size_t i = 0; i < m_records.size(); ++i) {
    const auto& record = m_records[i];
    if (record.geosetIndex == geosetIndex &&
        (modelResourcePtr == nullptr ||
         record.modelResourcePtr == modelResourcePtr)) {
      m_byRuntimeModel[{runtimeModelPtr, geosetIndex}] = i;
      return;
    }
  }
}

bool ShadowModelResourceStore::findByRuntimeGeoset(
    void* runtimeGeosetPtr, ShadowModelResourceRecord& out) const {
  out = {};
  const ShadowModelResourceRecord* record = findByRuntimeGeoset(runtimeGeosetPtr);
  if (record == nullptr)
    return false;
  out = *record;
  return true;
}

const ShadowModelResourceRecord* ShadowModelResourceStore::findByRuntimeGeoset(
    void* runtimeGeosetPtr) const {
  const auto it = m_byRuntimeGeoset.find(runtimeGeosetPtr);
  if (it == m_byRuntimeGeoset.end() || it->second >= m_records.size())
    return nullptr;
  return &m_records[it->second];
}

bool ShadowModelResourceStore::findByRuntimeGeosetData(
    void* runtimeGeosetDataPtr, ShadowModelResourceRecord& out) const {
  out = {};
  const ShadowModelResourceRecord* record =
      findByRuntimeGeosetData(runtimeGeosetDataPtr);
  if (record == nullptr)
    return false;
  out = *record;
  return true;
}

const ShadowModelResourceRecord* ShadowModelResourceStore::findByRuntimeGeosetData(
    void* runtimeGeosetDataPtr) const {
  const auto it = m_byRuntimeGeosetData.find(runtimeGeosetDataPtr);
  if (it == m_byRuntimeGeosetData.end() || it->second >= m_records.size())
    return nullptr;
  return &m_records[it->second];
}

bool ShadowModelResourceStore::findByRuntimeModel(
    void* runtimeModelPtr, uint32_t geosetIndex,
    ShadowModelResourceRecord& out) const {
  out = {};
  const ShadowModelResourceRecord* record =
      findByRuntimeModel(runtimeModelPtr, geosetIndex);
  if (record == nullptr)
    return false;
  out = *record;
  return true;
}

const ShadowModelResourceRecord* ShadowModelResourceStore::findByRuntimeModel(
    void* runtimeModelPtr, uint32_t geosetIndex) const {
  const auto it = m_byRuntimeModel.find({runtimeModelPtr, geosetIndex});
  if (it == m_byRuntimeModel.end() || it->second >= m_records.size())
    return nullptr;
  return &m_records[it->second];
}

bool ShadowModelResourceStore::findByModelResource(
    void* modelResourcePtr, uint32_t geosetIndex,
    ShadowModelResourceRecord& out) const {
  out = {};
  const ShadowModelResourceRecord* record =
      findByModelResource(modelResourcePtr, geosetIndex);
  if (record == nullptr)
    return false;
  out = *record;
  return true;
}

const ShadowModelResourceRecord* ShadowModelResourceStore::findByModelResource(
    void* modelResourcePtr, uint32_t geosetIndex) const {
  const auto it = m_byModelResource.find({modelResourcePtr, geosetIndex});
  if (it == m_byModelResource.end() || it->second >= m_records.size())
    return nullptr;
  return &m_records[it->second];
}

void ShadowPoseStore::clear() {
  m_records.clear();
  m_byRuntimeModel.clear();
  m_bySceneNode.clear();
  m_byUnitPtr.clear();
}

void ShadowPoseStore::reserve(size_t count) {
  m_records.reserve(count);
  m_byRuntimeModel.reserve(count);
  m_bySceneNode.reserve(count);
  m_byUnitPtr.reserve(count);
}

void ShadowPoseStore::add(ShadowPoseRecord record) {
  const size_t index = m_records.size();
  m_records.emplace_back(std::move(record));
  const auto& stored = m_records.back();
  if (stored.runtimeModelPtr != nullptr)
    m_byRuntimeModel[stored.runtimeModelPtr] = index;
  if (stored.sceneNode != nullptr)
    m_bySceneNode[stored.sceneNode] = index;
  if (stored.unitPtr != nullptr)
    m_byUnitPtr[stored.unitPtr] = index;
}

bool ShadowPoseStore::findByRuntimeModel(void* runtimeModelPtr,
                                         ShadowPoseRecord& out) const {
  out = {};
  const auto* record = findByRuntimeModelPtr(runtimeModelPtr);
  if (record == nullptr)
    return false;
  out = *record;
  return true;
}

bool ShadowPoseStore::findBySceneNode(void* sceneNode,
                                      ShadowPoseRecord& out) const {
  out = {};
  const auto* record = findBySceneNodePtr(sceneNode);
  if (record == nullptr)
    return false;
  out = *record;
  return true;
}

bool ShadowPoseStore::findByUnitPtr(void* unitPtr,
                                    ShadowPoseRecord& out) const {
  out = {};
  const auto* record = findByUnitPtrPtr(unitPtr);
  if (record == nullptr)
    return false;
  out = *record;
  return true;
}

const ShadowPoseRecord*
ShadowPoseStore::findByRuntimeModelPtr(void* runtimeModelPtr) const {
  const auto it = m_byRuntimeModel.find(runtimeModelPtr);
  if (it == m_byRuntimeModel.end() || it->second >= m_records.size())
    return nullptr;
  return &m_records[it->second];
}

const ShadowPoseRecord*
ShadowPoseStore::findBySceneNodePtr(void* sceneNode) const {
  const auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end() || it->second >= m_records.size())
    return nullptr;
  return &m_records[it->second];
}

const ShadowPoseRecord*
ShadowPoseStore::findByUnitPtrPtr(void* unitPtr) const {
  const auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end() || it->second >= m_records.size())
    return nullptr;
  return &m_records[it->second];
}

void ShadowAttachmentRigidStore::clear() {
  m_records.clear();
  m_byChildRuntimeModel.clear();
  m_byChildSpritePtr.clear();
  m_byOwnerRuntimeModel.clear();
  m_byRootRuntimeModel.clear();
  m_byWorldObjectEntry.clear();
  m_bySceneNode.clear();
  m_byUnitPtr.clear();
  m_byHandle.clear();
  m_bySourceObject.clear();
  m_bySourceSpriteObject.clear();
}

void ShadowAttachmentRigidStore::reserve(size_t count) {
  m_records.reserve(count);
  m_byChildRuntimeModel.reserve(count);
  m_byChildSpritePtr.reserve(count);
  m_byOwnerRuntimeModel.reserve(count);
  m_byRootRuntimeModel.reserve(count);
  m_byWorldObjectEntry.reserve(count);
  m_bySceneNode.reserve(count);
  m_byUnitPtr.reserve(count);
  m_byHandle.reserve(count);
  m_bySourceObject.reserve(count);
  m_bySourceSpriteObject.reserve(count);
}

void ShadowAttachmentRigidStore::add(ShadowAttachmentRigidRecord record) {
  const size_t index = m_records.size();
  m_records.emplace_back(std::move(record));
  const auto& stored = m_records.back();
  if (stored.childRuntimeModelPtr != nullptr)
    m_byChildRuntimeModel[stored.childRuntimeModelPtr] = index;
  if (stored.childSpritePtr != nullptr)
    m_byChildSpritePtr[stored.childSpritePtr] = index;
  if (stored.ownerRuntimeModelPtr != nullptr)
    m_byOwnerRuntimeModel[stored.ownerRuntimeModelPtr] = index;
  if (stored.rootRuntimeModelPtr != nullptr)
    m_byRootRuntimeModel[stored.rootRuntimeModelPtr] = index;
  if (stored.worldObjectEntry != nullptr)
    m_byWorldObjectEntry[stored.worldObjectEntry] = index;
  if (stored.sceneNode != nullptr)
    m_bySceneNode[stored.sceneNode] = index;
  if (stored.unitPtr != nullptr)
    m_byUnitPtr[stored.unitPtr] = index;
  if (stored.jHandle != 0u)
    m_byHandle[stored.jHandle] = index;
  if (stored.sourceObjectPtr != nullptr)
    m_bySourceObject[stored.sourceObjectPtr] = index;
  if (stored.sourceSpriteObjectPtr != nullptr)
    m_bySourceSpriteObject[stored.sourceSpriteObjectPtr] = index;
}

bool ShadowAttachmentRigidStore::findByChildRuntimeModel(
    void* childRuntimeModelPtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  const auto it = m_byChildRuntimeModel.find(childRuntimeModelPtr);
  if (it == m_byChildRuntimeModel.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findByChildSpritePtr(
    void* childSpritePtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  const auto it = m_byChildSpritePtr.find(childSpritePtr);
  if (it == m_byChildSpritePtr.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findByOwnerRuntimeModel(
    void* ownerRuntimeModelPtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  const auto it = m_byOwnerRuntimeModel.find(ownerRuntimeModelPtr);
  if (it == m_byOwnerRuntimeModel.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findByRootRuntimeModel(
    void* rootRuntimeModelPtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  const auto it = m_byRootRuntimeModel.find(rootRuntimeModelPtr);
  if (it == m_byRootRuntimeModel.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findByAnyRuntimeModel(
    void* runtimeModelPtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  if (runtimeModelPtr == nullptr)
    return false;
  if (findByChildRuntimeModel(runtimeModelPtr, out))
    return true;
  if (findByOwnerRuntimeModel(runtimeModelPtr, out))
    return true;
  if (findByRootRuntimeModel(runtimeModelPtr, out))
    return true;
  return false;
}

bool ShadowAttachmentRigidStore::findUniqueByChildModelResource(
    void* modelResourcePtr, uint64_t modelKey,
    ShadowAttachmentRigidRecord& out) const {
  out = {};
  if (modelResourcePtr == nullptr && modelKey == 0u)
    return false;

  const ShadowAttachmentRigidRecord* uniqueMatch = nullptr;
  size_t matchCount = 0u;
  for (const auto& record : m_records) {
    const bool modelResourceMatch =
        modelResourcePtr != nullptr &&
        record.childModelResourcePtr == modelResourcePtr;
    const bool modelKeyMatch =
        modelKey != 0u && record.childModelKey == modelKey;
    if (!modelResourceMatch && !modelKeyMatch)
      continue;
    uniqueMatch = &record;
    matchCount += 1u;
    if (matchCount > 1u)
      return false;
  }

  if (uniqueMatch == nullptr)
    return false;

  out = *uniqueMatch;
  return true;
}

bool ShadowAttachmentRigidStore::findUniqueWithAnyIdentity(
    ShadowAttachmentRigidRecord& out) const {
  out = {};
  const ShadowAttachmentRigidRecord* uniqueMatch = nullptr;
  size_t matchCount = 0u;
  for (const auto& record : m_records) {
    if (!HasAttachmentIdentity(record))
      continue;
    uniqueMatch = &record;
    matchCount += 1u;
    if (matchCount > 1u)
      return false;
  }

  if (uniqueMatch == nullptr)
    return false;

  out = *uniqueMatch;
  return true;
}

bool ShadowAttachmentRigidStore::findByWorldObjectEntry(
    void* worldObjectEntry, ShadowAttachmentRigidRecord& out) const {
  out = {};
  const auto it = m_byWorldObjectEntry.find(worldObjectEntry);
  if (it == m_byWorldObjectEntry.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findBySceneNode(
    void* sceneNode, ShadowAttachmentRigidRecord& out) const {
  out = {};
  const auto it = m_bySceneNode.find(sceneNode);
  if (it == m_bySceneNode.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findByUnitPtr(
    void* unitPtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  const auto it = m_byUnitPtr.find(unitPtr);
  if (it == m_byUnitPtr.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findByHandle(
    uint32_t jHandle, ShadowAttachmentRigidRecord& out) const {
  out = {};
  if (jHandle == 0u)
    return false;
  const auto it = m_byHandle.find(jHandle);
  if (it == m_byHandle.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findBySourceObject(
    void* sourceObjectPtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  if (sourceObjectPtr == nullptr)
    return false;
  const auto it = m_bySourceObject.find(sourceObjectPtr);
  if (it == m_bySourceObject.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

bool ShadowAttachmentRigidStore::findBySourceSpriteObject(
    void* sourceSpriteObjectPtr, ShadowAttachmentRigidRecord& out) const {
  out = {};
  if (sourceSpriteObjectPtr == nullptr)
    return false;
  const auto it = m_bySourceSpriteObject.find(sourceSpriteObjectPtr);
  if (it == m_bySourceSpriteObject.end() || it->second >= m_records.size())
    return false;
  out = m_records[it->second];
  return true;
}

ShadowRuntimeContractCache& ShadowRuntimeContractCache::instance() {
  static ShadowRuntimeContractCache* s_instance =
      new ShadowRuntimeContractCache();
  return *s_instance;
}

void ShadowRuntimeContractCache::beginFrame() {
  // 保留上一帧完整 contract，供 control plane / AutoTest 在异步线程读取。
  // 当前实现的 live snapshot 在 EndFrame 一次性发布，因此 BeginFrame 不应
  // 把最后一个已发布快照清空，否则外部观察者会在帧中途读到“假空帧”。
}

void ShadowRuntimeContractCache::resetMapSession() {
  auto manifest = std::make_shared<ShadowFrameManifest>();
  auto resources = std::make_shared<ShadowModelResourceStore>();
  auto poses = std::make_shared<ShadowPoseStore>();
  auto attachments = std::make_shared<ShadowAttachmentRigidStore>();

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  manifest->publishRevision = ++m_publishRevision;
  m_manifest = std::move(manifest);
  m_resources = std::move(resources);
  m_poses = std::move(poses);
  m_attachments = std::move(attachments);
  m_stats = {};
  m_stats.publishRevision = m_publishRevision;
  m_resourceRevision = 0u;
  m_resourceRefreshFrameSerial = 0u;
  m_lastManifestCopyVisibleScanned.store(0u, std::memory_order_relaxed);
  m_lastManifestCopyAppended.store(0u, std::memory_order_relaxed);
  m_lastManifestCopyDeduplicatedSkipped.store(
      0u, std::memory_order_relaxed);
  m_lastManifestCopyRejectedSkipped.store(0u,
                                           std::memory_order_relaxed);
}

void ShadowRuntimeContractCache::captureLiveState() {
  if (!dxvk::war3::internal::
          kWar3RuntimeConfigSemanticContractCaptureEffective)
    return;
  SemanticPerfScope semanticPerf(
      render::SemanticDataPerfTag::ContractCapture);

  auto manifest = ShadowFrameManifest{};
  auto poses = ShadowPoseStore{};
  auto attachments = ShadowAttachmentRigidStore{};
  auto stats = ShadowFrameStats{};
  std::shared_ptr<const ShadowFrameManifest> priorManifest;
  bool shouldBuildValidationFrame = false;

  auto& visibleRegistry = render::VisibleRenderableRegistry::instance();
  manifest.frameSerial = visibleRegistry.getFrameNumber();
  manifest.visibleCount = visibleRegistry.getVisibleCount();
  manifest.mainQueueCount = visibleRegistry.getMainQueueCount();
  manifest.transparentCount = visibleRegistry.getTransparentCount();

  auto& resourceCache = model::ShadowModelResourceCache::instance();
  auto& modelRegistry = model::ModelRegistry::instance();
  auto& poseRegistry = model::PoseRegistry::instance();
  auto& attachmentRegistry = model::AttachmentRigidRegistry::instance();
  constexpr bool kCaptureAttachmentRigidContracts =
      !dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly;
  // The bootstrap predicate is a conjunction, but readyGeosetCount() is an
  // O(all geosets + aliases) scan while the other two terms are O(1) map-size
  // reads.  In the steady state runtime-model records already exist, so test
  // that decisive term first and never pay the full scan.  Registry mutation
  // and capture both run on the render-thread frame boundary; changing the
  // order therefore preserves the exact bootstrap predicate and frame order.
  const bool needsResourceBootstrap =
      resourceCache.runtimeModelRecordCount() == 0u &&
      modelRegistry.recordCount() != 0u &&
      resourceCache.readyGeosetCount() == 0u;
  if (needsResourceBootstrap) {
    auto bootstrapScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/ResourceBootstrap");
    for (const auto& record : modelRegistry.snapshot()) {
      resourceCache.noteRuntimeModelBinding(record.runtimeModelPtr,
                                            record.modelResourcePtr,
                                            record.modelKey);
      if (record.modelResourcePtr != nullptr) {
        resourceCache.noteModelResourceBinding(record.modelResourcePtr,
                                               record.modelKey);
      }
      uint32_t runtimeGeosetCount = 0u;
      void* runtimeGeosets = nullptr;
      if (record.runtimeModelPtr != nullptr &&
          SafeReadU32Fast(record.runtimeModelPtr,
                          dxvk::war3::CModelOffsets::RuntimeGeosetCount,
                          runtimeGeosetCount) &&
          runtimeGeosetCount != 0u && runtimeGeosetCount < 4096u &&
          SafeReadPtrFast(record.runtimeModelPtr,
                          dxvk::war3::CModelOffsets::RuntimeGeosets,
                          runtimeGeosets) &&
          runtimeGeosets != nullptr &&
          dxvk::war3::IsReadableRange(runtimeGeosets,
                                      size_t(runtimeGeosetCount) *
                                          sizeof(void*))) {
        const auto* runtimeGeosetArray =
            reinterpret_cast<const uint8_t*>(runtimeGeosets);
        for (uint32_t i = 0u; i < runtimeGeosetCount; ++i) {
          void* runtimeGeosetPtr = nullptr;
          void* runtimeGeosetDataPtr = nullptr;
          SafeReadPtrFast(runtimeGeosetArray + size_t(i) * sizeof(void*), 0u,
                          runtimeGeosetPtr);
          if (runtimeGeosetPtr != nullptr) {
            SafeReadPtrFast(runtimeGeosetPtr,
                            dxvk::war3::CGeosetOffsets::GeosetData,
                            runtimeGeosetDataPtr);
          }
          resourceCache.noteRuntimeGeosetBinding(
              record.runtimeModelPtr, i, runtimeGeosetPtr, runtimeGeosetDataPtr,
              record.modelResourcePtr, record.modelKey);
        }
      }
    }
  }
  uint64_t resourceRevision = resourceCache.revision();
  const size_t currentPoseRecordCount = poseRegistry.recordCount();
  const size_t currentAttachmentRecordCount =
      kCaptureAttachmentRigidContracts ? attachmentRegistry.recordCount() : 0u;
  // Phase 7.99：在 lock 之前先做一次 cheap 的 "samePublishedFrame + 内容
  // 等同于上一次 publish" 检查，避免桥/斜坡场景下每帧都要做 1.9ms 的 ManifestCopy。
  // 这是 sameFrameDataNotGrowing 的 frame-aware 增强：在 visibleCount/mainQueue/
  // transparent 全部相等时直接跳过，因为 ManifestCopy 的 records 完全由 visibleRecords 决定。
  // 注意：不要求 priorContractUsable，因为高压地图的 readyGeosetCount 可能长期为 0
  // （地图还没初始化全 geoset），但 ManifestCopy 已经在反复重做。
  {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (m_manifest != nullptr &&
        m_manifest->visibleCount == manifest.visibleCount &&
        m_manifest->mainQueueCount == manifest.mainQueueCount &&
        m_manifest->transparentCount == manifest.transparentCount) {
      const size_t previousPoseCount =
          m_poses != nullptr ? m_poses->records().size() : 0u;
      const size_t previousAttachmentCount =
          m_attachments != nullptr ? m_attachments->records().size() : 0u;
      // 如果 pose 与 attachment 也未增长，本帧没有新内容，整段 capture 跳过。
      if (currentPoseRecordCount <= previousPoseCount &&
          currentAttachmentRecordCount <= previousAttachmentCount &&
          !m_manifest->records.empty()) {
        m_manifestCopySkipStableCount.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    const bool samePublishedFrame =
        m_manifest != nullptr &&
        m_manifest->frameSerial == manifest.frameSerial;
    const bool sameFrameNotGrowing =
        samePublishedFrame &&
        manifest.visibleCount <= m_manifest->visibleCount &&
        manifest.mainQueueCount <= m_manifest->mainQueueCount &&
        manifest.transparentCount <= m_manifest->transparentCount;
    const size_t previousPoseCount =
        m_poses != nullptr ? m_poses->records().size() : 0u;
    const size_t previousAttachmentCount =
        m_attachments != nullptr ? m_attachments->records().size() : 0u;
    const bool sameFrameDataNotGrowing =
        sameFrameNotGrowing &&
        currentPoseRecordCount <= previousPoseCount &&
        currentAttachmentRecordCount <= previousAttachmentCount;
    const bool priorContractUsable =
        m_stats.matrixPaletteCount != 0u &&
        m_stats.shadowReadyGeosetCount != 0u;
    if (sameFrameDataNotGrowing && priorContractUsable) {
      ++m_stats.contractCaptureSkippedStableSameFrame;
      m_manifestCopySkipStableCount.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  const auto& visibleRecords = visibleRegistry.getAllVisibleView();
  manifest.records.reserve(visibleRecords.size());
  poses.reserve(currentPoseRecordCount + visibleRecords.size() * 3u +
                currentAttachmentRecordCount * 3u + 32u);
  attachments.reserve(currentAttachmentRecordCount);

  {
    auto manifestCopyScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/ManifestCopy");
    // Phase 7.97 直接计时：用 std::chrono 测 ManifestCopy 真实墙钟时间，
    // 与 perf monitor 报告对比。如果 perf monitor 显示 2ms 但 chrono 显示 ~0us，
    // 说明 scope 计时被外部因素干扰（perf monitor 自身锁竞争）。
    const auto manifestCopyTickStart = std::chrono::steady_clock::now();
    // Phase 7.96：record 多时用 unordered_set 避免 O(N²)，少时用 vector
    // 利用 cache friendly 优势。阈值 64 经验值，同 RepairManifestIdentity。
    constexpr size_t kHashSetThreshold = 64u;
    const bool useHashSet = visibleRecords.size() > kHashSetThreshold;
    std::unordered_set<uint64_t> seenSetLarge;
    std::vector<uint64_t> seenVecSmall;
    ManifestResolveDiagnostics resolveDiagnostics = {};
    if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
      if (useHashSet) {
        seenSetLarge.reserve(visibleRecords.size());
      } else {
        seenVecSmall.reserve(visibleRecords.size());
      }
    }
    // Phase 7.97 诊断：记录本帧实际遍历的 visible records 数。
    stats.manifestCopyVisibleScanned = uint64_t(visibleRecords.size());
    for (const auto& record : visibleRecords) {
      if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
        const uint32_t rejectReason =
            VisibleDirectGeosetUnitRejectReason(record);
        if (rejectReason != 0u) {
          switch (rejectReason) {
          case 1u:
            ++stats.visibleDirectUnitRejectedNotUnitLike;
            break;
          case 2u:
            ++stats.visibleDirectUnitRejectedGroup;
            break;
          case 3u:
            ++stats.visibleDirectUnitRejectedNoUnitPtr;
            break;
          case 4u:
            ++stats.visibleDirectUnitRejectedNoIdentity;
            break;
          case 5u:
            ++stats.visibleDirectUnitRejectedNoMesh;
            break;
          case 6u:
            ++stats.visibleDirectUnitRejectedBuilding;
            break;
          case 7u:
          default:
            ++stats.visibleDirectUnitRejectedNoGeoset;
            break;
          }
          ++stats.manifestCopyRejectedSkipped;
          continue;
        }
        ++stats.visibleDirectUnitCandidateAccepted;
        const uint64_t directUnitKey = MakeVisibleDirectUnitKey(record);
        if (useHashSet) {
          if (!seenSetLarge.insert(directUnitKey).second) {
            ++stats.manifestCopyDeduplicatedSkipped;
            continue;
          }
        } else {
          if (std::find(seenVecSmall.begin(), seenVecSmall.end(),
                        directUnitKey) != seenVecSmall.end()) {
            ++stats.manifestCopyDeduplicatedSkipped;
            continue;
          }
          seenVecSmall.push_back(directUnitKey);
        }
      }
      manifest.records.push_back(
          ConvertVisible(record, manifest.frameSerial, resolveDiagnostics));
      ++stats.manifestCopyAppended;
    }
    // Phase 7.97：在 ManifestCopy 出口写 atomic counter。即使本次 capture
    // 后续被 publish 阶段以 sameFrameDuplicateOrRegression 丢弃，atomic 仍记录
    // 真实遍历数量。这就是桥/斜坡 record 数量爆炸定位的入口。
    m_manifestCopyEnterCount.fetch_add(1, std::memory_order_relaxed);
    m_lastManifestCopyVisibleScanned.store(stats.manifestCopyVisibleScanned,
                                           std::memory_order_relaxed);
    m_lastManifestCopyAppended.store(stats.manifestCopyAppended,
                                     std::memory_order_relaxed);
    m_lastManifestCopyDeduplicatedSkipped.store(
        stats.manifestCopyDeduplicatedSkipped, std::memory_order_relaxed);
    m_lastManifestCopyRejectedSkipped.store(stats.manifestCopyRejectedSkipped,
                                            std::memory_order_relaxed);
    // 累积 total/max，避免最近一次极小覆盖。
    m_manifestCopyTotalScanned.fetch_add(stats.manifestCopyVisibleScanned,
                                         std::memory_order_relaxed);
    m_manifestResolveSourceCompleteSkipCount.fetch_add(
        resolveDiagnostics.sourceCompleteSkipCount, std::memory_order_relaxed);
    m_manifestResolveLegacyCacheHitCount.fetch_add(
        resolveDiagnostics.legacyCacheHitCount, std::memory_order_relaxed);
    m_manifestResolveRawScanCount.fetch_add(
        resolveDiagnostics.rawScanCount, std::memory_order_relaxed);
    m_manifestResolveRawScanEntryVisitCount.fetch_add(
        resolveDiagnostics.rawScanEntryVisitCount,
        std::memory_order_relaxed);
    m_manifestResolveRawScanMissCount.fetch_add(
        resolveDiagnostics.rawScanMissCount, std::memory_order_relaxed);
    m_manifestResolveVerifierAttemptCount.fetch_add(
        resolveDiagnostics.verifierAttemptCount, std::memory_order_relaxed);
    m_manifestResolveVerifierMismatchCount.fetch_add(
        resolveDiagnostics.verifierMismatchCount, std::memory_order_relaxed);
    m_manifestModelResourceAttemptCount.fetch_add(
        resolveDiagnostics.modelResourceAttemptCount,
        std::memory_order_relaxed);
    m_manifestModelResourceCacheHitCount.fetch_add(
        resolveDiagnostics.modelResourceCacheHitCount,
        std::memory_order_relaxed);
    m_manifestModelResourceDeepResolveCount.fetch_add(
        resolveDiagnostics.modelResourceDeepResolveCount,
        std::memory_order_relaxed);
    m_manifestModelResourceNullResultCount.fetch_add(
        resolveDiagnostics.modelResourceNullResultCount,
        std::memory_order_relaxed);
    m_manifestModelResourceVerifierAttemptCount.fetch_add(
        resolveDiagnostics.modelResourceVerifierAttemptCount,
        std::memory_order_relaxed);
    m_manifestModelResourceVerifierMismatchCount.fetch_add(
        resolveDiagnostics.modelResourceVerifierMismatchCount,
        std::memory_order_relaxed);
    {
      uint64_t cur = m_manifestResolveMaxRuntimeGeosetCount.load(
          std::memory_order_relaxed);
      while (resolveDiagnostics.maxRuntimeGeosetCount > cur &&
             !m_manifestResolveMaxRuntimeGeosetCount.compare_exchange_weak(
                 cur, resolveDiagnostics.maxRuntimeGeosetCount,
                 std::memory_order_relaxed))
        ;
    }
    {
      uint64_t cur = m_manifestCopyMaxScanned.load(std::memory_order_relaxed);
      while (stats.manifestCopyVisibleScanned > cur &&
             !m_manifestCopyMaxScanned.compare_exchange_weak(
                 cur, stats.manifestCopyVisibleScanned,
                 std::memory_order_relaxed))
        ;
    }
    // Phase 7.97：用 chrono 直接测 ManifestCopy 墙钟时间。
    const auto manifestCopyTickEnd = std::chrono::steady_clock::now();
    const uint64_t manifestCopyChronoNs = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            manifestCopyTickEnd - manifestCopyTickStart).count());
    m_manifestCopyTotalChronoNs.fetch_add(manifestCopyChronoNs,
                                          std::memory_order_relaxed);
    {
      uint64_t cur =
          m_manifestCopyMaxChronoNs.load(std::memory_order_relaxed);
      while (manifestCopyChronoNs > cur &&
             !m_manifestCopyMaxChronoNs.compare_exchange_weak(
                 cur, manifestCopyChronoNs, std::memory_order_relaxed))
        ;
    }
  }
  DemandFillVisibleUnitGeosetBindings(manifest);
  resourceRevision = resourceCache.revision();

  auto buildResourceStore = [&resourceCache, &manifest]() {
    auto resourceStoreScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/ResourceStoreBuild");
    auto freshResources = std::make_shared<ShadowModelResourceStore>();
    for (const auto& geoset : resourceCache.snapshotGeosets())
      freshResources->add(ConvertGeoset(geoset, manifest.frameSerial));

    // 先补 modelResource->runtimeModel alias，再补纯 runtimeModel alias。
    for (const auto& modelRecord : resourceCache.snapshotModelAliases()) {
      for (uint32_t i = 0; i < modelRecord.geosetCount; ++i) {
        freshResources->bindRuntimeModelAlias(modelRecord.runtimeModelPtr, i,
                                              modelRecord.modelResourcePtr);
      }
    }
    for (const auto& runtimeRecord :
         resourceCache.snapshotRuntimeModelAliases()) {
      for (uint32_t i = 0; i < runtimeRecord.geosetCount; ++i) {
        freshResources->bindRuntimeModelAlias(runtimeRecord.runtimeModelPtr, i,
                                              runtimeRecord.modelResourcePtr);
      }
    }
    return freshResources;
  };
  std::shared_ptr<ShadowModelResourceStore> resourcesPtr;
  bool rebuiltResourceStore = false;
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    priorManifest = m_manifest;
    constexpr uint64_t kResourceStoreRefreshIntervalFrames = 30u;
    const bool resourceStoreLooksUsable =
        m_resources != nullptr && !m_resources->records().empty();
    const bool resourceRevisionStable = m_resourceRevision == resourceRevision;
    const bool allowCooldownReuse =
        !dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled();
    const bool resourceRefreshCoolingDown =
        allowCooldownReuse && resourceStoreLooksUsable &&
        m_resourceRefreshFrameSerial != 0u &&
        manifest.frameSerial < m_resourceRefreshFrameSerial +
                                   kResourceStoreRefreshIntervalFrames;
    if (resourceStoreLooksUsable &&
        (resourceRevisionStable || resourceRefreshCoolingDown)) {
      resourcesPtr = m_resources;
    }
  }

  {
    auto hydrateScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/ManifestHydrate");
    if (priorManifest != nullptr)
      RepairManifestIdentityFromPrior(*priorManifest, manifest);
    HydrateManifestRuntimeOwnersFromIndexedCache(manifest, resourceCache);
  }

  {
    auto coverageScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/ResourceCoverage");
    if (resourcesPtr != nullptr &&
        dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
        !ResourceStoreHasReadyManifestCoverage(*resourcesPtr, manifest)) {
      resourcesPtr.reset();
    }
  }

  if (resourcesPtr == nullptr) {
    resourcesPtr = buildResourceStore();
    rebuiltResourceStore = true;
  }

  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  auto& shadowRegistry = render::ShadowObjectRegistry::instance();
  {
    auto snapshotScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/SnapshotPoseAttachment");
    for (const auto& pose : poseRegistry.snapshot())
      poses.add(ConvertPose(pose, manifest.frameSerial));
    if constexpr (kCaptureAttachmentRigidContracts) {
      for (const auto& attachment : attachmentRegistry.snapshot()) {
        auto contractAttachment =
            ConvertAttachmentRigid(attachment, manifest.frameSerial);
        RepairAttachmentRigidIdentity(manifest, visibleRegistry,
                                      instanceRegistry, shadowRegistry,
                                      poseRegistry, contractAttachment);
        PopulateAttachmentChildSemanticKey(contractAttachment);
        attachments.add(std::move(contractAttachment));
      }
    }
  }
  {
    auto directPoseScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/DirectCModelPose");
    SupplementPosesFromLiveCModels(manifest, attachments, poses, stats);
  }
  const uint64_t postAttachmentResourceRevision = resourceCache.revision();
  if (postAttachmentResourceRevision != resourceRevision) {
    resourcesPtr = buildResourceStore();
    rebuiltResourceStore = true;
    resourceRevision = postAttachmentResourceRevision;
  }
  {
    auto rootSupplementScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/RootUnitSupplement");
    if constexpr (dxvk::war3::internal::
                      kShadowSemanticCoreSceneRootUnitSupplementEnabled) {
      if (!ManifestHasRootUnitSemanticRecords(manifest)) {
        stats.rootUnitSupplementReusedFromPrior +=
            ReusePriorRootUnitSupplementRecords(priorManifest.get(), manifest,
                                                poses, poseRegistry);
      }
      if (!ManifestHasRootUnitSemanticRecords(manifest)) {
        AppendRootUnitSupplementRecords(
            manifest, resourcesPtr, resourceCache, instanceRegistry,
            shadowRegistry, poseRegistry, poses, attachments, stats);
      }
      if (stats.rootUnitSupplementAppended != 0u ||
          stats.rootUnitSupplementReusedFromPrior != 0u)
        PrioritizeRootUnitSemanticRecords(manifest);
    }
  }

  {
    auto statsScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/Stats");
    stats.frameSerial = manifest.frameSerial;
    stats.publishRevision = manifest.publishRevision;
    stats.visibleCount = manifest.visibleCount;
    stats.mainQueueCount = manifest.mainQueueCount;
    stats.transparentCount = manifest.transparentCount;
    stats.shadowReadyGeosetCount = resourceCache.readyGeosetCount();
    stats.shadowRuntimeModelCount = resourceCache.runtimeModelRecordCount();
    stats.matrixPaletteCount = 0u;
    for (const auto& pose : poses.records()) {
      if (pose.matrixCount != 0u && !pose.matrixPalette.empty())
        ++stats.matrixPaletteCount;
    }
    stats.attachmentRigidCount = attachments.records().size();
  }

  {
    auto publishScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/Publish");
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    const bool isEmptyCapture =
        manifest.records.empty() && manifest.visibleCount == 0u &&
        manifest.mainQueueCount == 0u && manifest.transparentCount == 0u;
    const bool hasPublishedCapture =
        m_manifest != nullptr &&
        (!m_manifest->records.empty() || m_manifest->visibleCount != 0u);
    if (isEmptyCapture && hasPublishedCapture) {
      ++m_stats.contractCaptureSkippedEmpty;
      return;
    }

    const bool samePublishedFrame =
        m_manifest != nullptr &&
        m_manifest->frameSerial == manifest.frameSerial;
    const size_t currentPoseCount = poses.records().size();
    const size_t previousPoseCount =
        m_poses != nullptr ? m_poses->records().size() : 0u;
    const size_t currentAttachmentCount = attachments.records().size();
    const size_t previousAttachmentCount =
        m_attachments != nullptr ? m_attachments->records().size() : 0u;
    const bool sameFrameDuplicateOrRegression =
        samePublishedFrame &&
        manifest.records.size() <= m_manifest->records.size() &&
        manifest.visibleCount <= m_manifest->visibleCount &&
        manifest.mainQueueCount <= m_manifest->mainQueueCount &&
        manifest.transparentCount <= m_manifest->transparentCount &&
        currentPoseCount <= previousPoseCount &&
        currentAttachmentCount <= previousAttachmentCount;
    if (sameFrameDuplicateOrRegression) {
      // FlushAndReset 在同一游戏帧里可能多次进入 EndFrame，
      // 其中不少 capture 只是重复/回退的中间态。
      // 若这些样本继续 bump publishRevision，会把 semantic-core 的
      // freshness 指标人为拉大，造成“frame lag 不高但 publish lag 很高”的假陈旧。
      // 这里仅保留同一 frameSerial 下更完整的 capture，重复或回退样本直接忽略。
      ++m_stats.contractCaptureSkippedDuplicateSameFrame;
      return;
    }

    manifest.publishRevision = ++m_publishRevision;
    stats.publishRevision = manifest.publishRevision;
    stats.contractCaptureSkippedStableSameFrame =
        m_stats.contractCaptureSkippedStableSameFrame;
    stats.contractCaptureSkippedEmpty =
        m_stats.contractCaptureSkippedEmpty;
    stats.contractCaptureSkippedDuplicateSameFrame =
        m_stats.contractCaptureSkippedDuplicateSameFrame;
    auto manifestPtr =
        std::make_shared<ShadowFrameManifest>(std::move(manifest));
    auto posesPtr = std::make_shared<ShadowPoseStore>(std::move(poses));
    auto attachmentsPtr =
        std::make_shared<ShadowAttachmentRigidStore>(std::move(attachments));
    m_manifest = manifestPtr;
    m_resources = resourcesPtr;
    m_poses = posesPtr;
    m_attachments = attachmentsPtr;
    m_stats = std::move(stats);
    m_resourceRevision = resourceRevision;
    if (rebuiltResourceStore && resourcesPtr != nullptr &&
        !resourcesPtr->records().empty())
      m_resourceRefreshFrameSerial = manifest.frameSerial;

    if (dxvk::war3::internal::IsSemanticCoreValidationRuntimeEnabled() ||
        dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled()) {
      // Publish first, then only enqueue a semantic-frame build request.
      // The actual build should run in a controlled render/runtime path, not
      // inline with publish or control-plane query handling.
      shouldBuildValidationFrame = true;
    }
  }

  if (shouldBuildValidationFrame) {
    ShadowValidationRuntime::instance().requestLatestFrameBuild();
  }
}

void ShadowRuntimeContractCache::capturePoseOnlyLiveState() {
  if (!dxvk::war3::internal::
          kWar3RuntimeConfigSemanticContractCaptureEffective)
    return;

  auto manifestPtr = std::shared_ptr<ShadowFrameManifest>{};
  auto resourcesPtr = std::shared_ptr<ShadowModelResourceStore>{};
  auto attachmentsPtr = std::shared_ptr<ShadowAttachmentRigidStore>{};
  ShadowFrameStats stats = {};
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (m_manifest == nullptr || m_resources == nullptr ||
        m_attachments == nullptr || m_manifest->records.empty()) {
      return;
    }
    manifestPtr = m_manifest;
    resourcesPtr = m_resources;
    attachmentsPtr = m_attachments;
    stats = m_stats;
  }

  const uint64_t currentFrame =
      render::VisibleRenderableRegistry::instance().getFrameNumber();
  const uint64_t poseFrameSerial =
      currentFrame != 0u ? currentFrame : manifestPtr->frameSerial;

  auto poses = ShadowPoseStore{};
  auto& poseRegistry = model::PoseRegistry::instance();
  poses.reserve(poseRegistry.recordCount() + manifestPtr->records.size() * 3u +
                32u);

  for (const auto& pose : poseRegistry.snapshot())
    poses.add(ConvertPose(pose, poseFrameSerial));

  stats.frameSerial = poseFrameSerial;
  stats.publishRevision = manifestPtr->publishRevision;
  stats.directPoseSupplementAttemptCount = 0u;
  stats.directPoseSupplementResolvedCount = 0u;
  stats.directPoseSupplementSkippedExisting = 0u;
  stats.directPoseSupplementSkippedInvalid = 0u;
  SupplementPosesFromLiveCModels(*manifestPtr, *attachmentsPtr, poses, stats);

  stats.matrixPaletteCount = 0u;
  for (const auto& pose : poses.records()) {
    if (pose.matrixCount != 0u && !pose.matrixPalette.empty())
      ++stats.matrixPaletteCount;
  }

  {
    auto publishScope = ContractCpuScope(
        "War3SemanticScene/CaptureContract/PoseOnlyPublish");
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto liveManifest = std::make_shared<ShadowFrameManifest>(*manifestPtr);
    liveManifest->frameSerial = poseFrameSerial;
    // Pose-only captures keep the topology revision stable but advance the
    // frame serial so ShadowValidationRuntime rebuilds skinned packets with the
    // fresh PoseStore instead of reusing an old runtimeGroupPalette.
    liveManifest->publishRevision = manifestPtr->publishRevision;
    auto newPoses = std::make_shared<ShadowPoseStore>(std::move(poses));
    m_manifest = std::move(liveManifest);
    m_resources = resourcesPtr;
    m_poses = newPoses;
    m_attachments = attachmentsPtr;
    m_stats = std::move(stats);
  }

  // Pose-only publish must not enqueue a new semantic build. The production
  // path keeps animation fresh via submit-time live palette refresh; forcing a
  // rebuild here turns every pose tick into a full semantic-frame rebuild and
  // collapses performance on real maps.
}

ShadowFrameManifest ShadowRuntimeContractCache::snapshotManifest() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_manifest != nullptr ? *m_manifest : ShadowFrameManifest{};
}

std::shared_ptr<const ShadowFrameManifest>
ShadowRuntimeContractCache::snapshotManifestShared() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_manifest;
}

ShadowModelResourceStore ShadowRuntimeContractCache::snapshotResources() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_resources != nullptr ? *m_resources : ShadowModelResourceStore{};
}

std::shared_ptr<const ShadowModelResourceStore>
ShadowRuntimeContractCache::snapshotResourcesShared() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_resources;
}

ShadowPoseStore ShadowRuntimeContractCache::snapshotPoses() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_poses != nullptr ? *m_poses : ShadowPoseStore{};
}

std::shared_ptr<const ShadowPoseStore>
ShadowRuntimeContractCache::snapshotPosesShared() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_poses;
}

ShadowAttachmentRigidStore ShadowRuntimeContractCache::snapshotAttachments()
    const {
  std::shared_ptr<const ShadowAttachmentRigidStore> attachments;
  std::shared_ptr<const ShadowFrameManifest> manifest;
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    attachments = m_attachments;
    manifest = m_manifest;
  }
  std::shared_ptr<const ShadowAttachmentRigidStore> preferred = attachments;
  if (manifest != nullptr) {
    const auto liveAttachments = BuildLiveAttachmentStore(*manifest);
    if (ShouldPreferLiveAttachments(attachments, liveAttachments))
      preferred = std::move(liveAttachments);
  }
  if (preferred != nullptr && !preferred->records().empty())
    return *preferred;
  return preferred != nullptr ? *preferred : ShadowAttachmentRigidStore{};
}

std::shared_ptr<const ShadowAttachmentRigidStore>
ShadowRuntimeContractCache::snapshotAttachmentsShared() const {
  std::shared_ptr<const ShadowAttachmentRigidStore> attachments;
  std::shared_ptr<const ShadowFrameManifest> manifest;
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    attachments = m_attachments;
    manifest = m_manifest;
  }
  if constexpr (!dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
    if (manifest != nullptr) {
    const auto liveAttachments = BuildLiveAttachmentStore(*manifest);
    if (ShouldPreferLiveAttachments(attachments, liveAttachments))
      return liveAttachments;
    }
  }
  return attachments;
}

ShadowFrameStats ShadowRuntimeContractCache::snapshotStats() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_stats;
}

ShadowPublishedContractBundle
ShadowRuntimeContractCache::snapshotBundleShared() const {
  ShadowPublishedContractBundle bundle = {};
  {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    bundle.manifest = m_manifest;
    bundle.resources = m_resources;
    bundle.poses = m_poses;
    bundle.attachments = m_attachments;
    bundle.stats = m_stats;
  }
  if constexpr (!dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly) {
    if (bundle.manifest != nullptr) {
    const auto liveAttachments = BuildLiveAttachmentStore(*bundle.manifest);
    if (ShouldPreferLiveAttachments(bundle.attachments, liveAttachments))
      bundle.attachments = std::move(liveAttachments);
    }
  }
  if (bundle.manifest != nullptr && bundle.resources != nullptr &&
      bundle.poses != nullptr && bundle.attachments != nullptr &&
      !ManifestHasRootUnitSemanticRecords(*bundle.manifest)) {
    if constexpr (!dxvk::war3::internal::
                      kShadowSemanticCoreSceneRootUnitSupplementEnabled) {
      return bundle;
    }
    auto supplementedManifest =
        std::make_shared<ShadowFrameManifest>(*bundle.manifest);
    auto supplementedStats = bundle.stats;
    AppendRootUnitSupplementRecords(
        *supplementedManifest, bundle.resources,
        model::ShadowModelResourceCache::instance(),
        model::ModelInstanceRegistry::instance(),
        render::ShadowObjectRegistry::instance(), model::PoseRegistry::instance(),
        *bundle.poses, *bundle.attachments, supplementedStats);
    if (supplementedStats.rootUnitSupplementAppended != 0u) {
      PrioritizeRootUnitSemanticRecords(*supplementedManifest);
      supplementedManifest->publishRevision =
          bundle.manifest->publishRevision + 0x100000000ull +
          supplementedStats.rootUnitSupplementAppended;
    }
    bundle.manifest = std::move(supplementedManifest);
    bundle.stats = supplementedStats;
  }
  return bundle;
}

} // namespace dxvk::war3::shadow
