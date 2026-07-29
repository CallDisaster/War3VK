#include "war3_shadow_runtime_bridge.h"
#include "../tools/war3_perf_monitor.h"

#include "../../d3d9_war3_hook.h"
#include "../model/war3_model_hook.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../core/war3_runtime_profile.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../hooks/war3_hook_render_identity.h"
#include "../hooks/war3_hook_shadow.h"
#include "../hooks/war3_hook_widget_identity.h"
#include "../shadow/war3_shadow_native_runtime.h"
#include "../shadow/war3_shadow_runtime_contract.h"
#include "../shadow/war3_shadow_alpha_test_payload.h"
#include "../shadow/war3_shadow_renderer_core.h"
#include "war3_current_draw_contract.h"
#include "war3_scene_collector.h"
#include "war3_shadow_object_registry.h"
#include "war3_upper_layer_shadow.h"
#include "war3_visible_renderables.h"
#include "../state/war3_render_state.h"
#include "../../util/log/log.h"
#include "../../util/util_env.h"
#include "../../util/util_time.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Phase 7.108b：shadow append survey 在 d3d9_device.cpp 实现。
namespace dxvk::war3_diag {
  uint64_t QueryShadowAppendTotal();
  uint64_t QueryShadowAppendUnique();
  uint32_t QueryShadowAppendRawcodeAt(uint32_t idx);
}

namespace dxvk::war3::render {

thread_local uint64_t
    g_worldObjectsPhase1PurePeriodicDispatchSequence = 0u;
thread_local WorldObjectsPhase1DispatchCaptureKind
    g_worldObjectsPhase1DispatchCaptureKind =
        WorldObjectsPhase1DispatchCaptureKind::None;

namespace {

enum class SemanticBuildSkippedReason : uint64_t {
  None = 0,
  SemanticDataModuleDisabled = 1,
  ModelProducerDisabled = 2,
  FrameRegistriesDisabled = 3,
  ContractCaptureDisabled = 4,
  ConsumerDisabled = 5,
  SceneSubmissionDisabled = 6,
};

bool SemanticAugmentBatchLookupRuntime() {
  static const bool s_enabled = [] {
    const std::string value =
        env::getEnvVar("DXVK_WAR3_SEMANTIC_AUGMENT_BATCH_LOOKUP");
    return value.empty() || value != "0";
  }();
  return s_enabled;
}

bool SemanticAugmentCompactShadowViewRuntime() {
  static const bool s_enabled = [] {
    const std::string value =
        env::getEnvVar("DXVK_WAR3_SEMANTIC_AUGMENT_COMPACT_SHADOW_VIEW");
    return value.empty() || value != "0";
  }();
  return s_enabled;
}

bool SemanticAugmentTlsCacheRuntime() {
  static const bool s_enabled = [] {
    const std::string value =
        env::getEnvVar("DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE");
    // The first isolated A-B-B-A did not produce a measurable frame-time win
    // and made ShadowCapture/Gates slightly slower. Keep the experiment
    // available for follow-up cache-shape work, but do not charge production
    // frames for an unproven optimization.
    return !value.empty() && value != "0";
  }();
  return s_enabled;
}

bool SemanticAugmentTlsCacheStatsRuntime() {
  static const bool s_enabled =
      env::getEnvVar("DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE_STATS") == "1";
  return s_enabled;
}

struct SemanticAugmentModelCacheKey {
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* primaryUnitPtr = nullptr;
  void* secondaryUnitPtr = nullptr;
  uint32_t jHandle = 0u;

  bool operator==(const SemanticAugmentModelCacheKey& other) const noexcept {
    return worldObjectEntry == other.worldObjectEntry &&
        sceneNode == other.sceneNode &&
        primaryUnitPtr == other.primaryUnitPtr &&
        secondaryUnitPtr == other.secondaryUnitPtr &&
        jHandle == other.jHandle;
  }
};

struct SemanticAugmentShadowCacheKey {
  void* worldObjectEntry = nullptr;
  void* sceneNode = nullptr;
  void* primaryUnitPtr = nullptr;
  void* secondaryUnitPtr = nullptr;
  uint32_t jHandle = 0u;
  void* runtimeModelPtr = nullptr;

  bool operator==(const SemanticAugmentShadowCacheKey& other) const noexcept {
    return worldObjectEntry == other.worldObjectEntry &&
        sceneNode == other.sceneNode &&
        primaryUnitPtr == other.primaryUnitPtr &&
        secondaryUnitPtr == other.secondaryUnitPtr &&
        jHandle == other.jHandle &&
        runtimeModelPtr == other.runtimeModelPtr;
  }
};

uint64_t MixSemanticAugmentCacheHash(uint64_t hash,
                                     uintptr_t value) noexcept {
  hash ^= uint64_t(value) + 0x9e3779b97f4a7c15ull +
      (hash << 6u) + (hash >> 2u);
  return hash;
}

size_t HashSemanticAugmentCacheKey(
    const SemanticAugmentModelCacheKey& key) noexcept {
  uint64_t hash = 0xcbf29ce484222325ull;
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.worldObjectEntry));
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.sceneNode));
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.primaryUnitPtr));
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.secondaryUnitPtr));
  hash = MixSemanticAugmentCacheHash(hash, uintptr_t(key.jHandle));
  return size_t(hash);
}

size_t HashSemanticAugmentCacheKey(
    const SemanticAugmentShadowCacheKey& key) noexcept {
  uint64_t hash = 0xcbf29ce484222325ull;
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.worldObjectEntry));
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.sceneNode));
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.primaryUnitPtr));
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.secondaryUnitPtr));
  hash = MixSemanticAugmentCacheHash(hash, uintptr_t(key.jHandle));
  hash = MixSemanticAugmentCacheHash(
      hash, reinterpret_cast<uintptr_t>(key.runtimeModelPtr));
  return size_t(hash);
}

// The high-pressure scene resolves roughly 330 distinct augment keys per
// frame.  A 64-entry direct-mapped table therefore evicts otherwise reusable
// positive and negative results before their next draw: the explicit cache
// census measured about 43% key collisions.  Keep the existing exact-key and
// registry-generation validation contract, but size the diagnostic cache to
// hold one representative render frame without conflict-thrashing.
constexpr size_t kSemanticAugmentTlsCacheCapacity = 1024u;
static_assert(
    (kSemanticAugmentTlsCacheCapacity &
     (kSemanticAugmentTlsCacheCapacity - 1u)) == 0u);

struct SemanticAugmentModelCacheEntry {
  bool valid = false;
  bool found = false;
  uint64_t mutationGeneration = 0u;
  SemanticAugmentModelCacheKey key = {};
  model::ModelInstanceAugmentView value = {};
};

struct SemanticAugmentShadowCacheEntry {
  bool valid = false;
  bool found = false;
  uint64_t mutationGeneration = 0u;
  uint64_t registryFrame = 0u;
  SemanticAugmentShadowCacheKey key = {};
  ShadowObjectAugmentView value = {};
};

struct SemanticAugmentTlsCacheAtomicStats {
  std::atomic<uint64_t> modelLookups{0u};
  std::atomic<uint64_t> modelHits{0u};
  std::atomic<uint64_t> modelNegativeHits{0u};
  std::atomic<uint64_t> modelMisses{0u};
  std::atomic<uint64_t> modelGenerationMismatches{0u};
  std::atomic<uint64_t> modelCollisions{0u};
  std::atomic<uint64_t> shadowLookups{0u};
  std::atomic<uint64_t> shadowHits{0u};
  std::atomic<uint64_t> shadowNegativeHits{0u};
  std::atomic<uint64_t> shadowMisses{0u};
  std::atomic<uint64_t> shadowGenerationMismatches{0u};
  std::atomic<uint64_t> shadowCollisions{0u};
};

SemanticAugmentTlsCacheAtomicStats g_semanticAugmentTlsCacheStats;

struct SemanticAugmentTlsCacheStorage {
  std::array<SemanticAugmentModelCacheEntry,
             kSemanticAugmentTlsCacheCapacity> model = {};
  std::array<SemanticAugmentShadowCacheEntry,
             kSemanticAugmentTlsCacheCapacity> shadow = {};
};

SemanticAugmentTlsCacheStorage* GetSemanticAugmentTlsCacheStorage() noexcept {
  // The experiment is default-off.  A direct thread_local array would reserve
  // hundreds of KiB on every DXVK/driver worker even when no augment lookup
  // ever uses it. Allocate only on a thread that enters the enabled cache, but
  // retain TLS ownership so short-lived threads release the storage on exit.
  // A fragmented 32-bit address space may reject the ~0.33 MiB allocation;
  // fail closed to the exact uncached registry lookup instead of throwing
  // through a draw hook.
  static thread_local std::unique_ptr<SemanticAugmentTlsCacheStorage> s_storage;
  static thread_local bool s_allocationAttempted = false;
  if (!s_storage && !s_allocationAttempted) {
    s_allocationAttempted = true;
    s_storage.reset(new (std::nothrow) SemanticAugmentTlsCacheStorage());
  }
  return s_storage.get();
}

bool FindModelInstanceAugmentCached(
    model::ModelInstanceRegistry& registry,
    const SemanticAugmentModelCacheKey& key,
    model::ModelInstanceAugmentView& out) {
  auto& stats = g_semanticAugmentTlsCacheStats;
  const bool collectStats = SemanticAugmentTlsCacheStatsRuntime();
  if (collectStats)
    stats.modelLookups.fetch_add(1u, std::memory_order_relaxed);

  auto* const cacheStorage = GetSemanticAugmentTlsCacheStorage();
  if (cacheStorage == nullptr) {
    if (collectStats)
      stats.modelMisses.fetch_add(1u, std::memory_order_relaxed);
    return registry.findFirstForAugmentView(
        key.worldObjectEntry, key.sceneNode, key.primaryUnitPtr,
        key.secondaryUnitPtr, key.jHandle, out, nullptr);
  }

  auto& entry = cacheStorage->model[
      HashSemanticAugmentCacheKey(key) &
      (kSemanticAugmentTlsCacheCapacity - 1u)];
  const uint64_t generationBefore = registry.mutationGeneration();
  if (entry.valid && entry.key == key) {
    if ((generationBefore & 1u) == 0u &&
        entry.mutationGeneration == generationBefore) {
      const bool found = entry.found;
      const uint64_t generationAfter = registry.mutationGeneration();
      if (generationAfter == generationBefore) {
        if (collectStats)
          stats.modelHits.fetch_add(1u, std::memory_order_relaxed);
        if (!found) {
          if (collectStats) {
            stats.modelNegativeHits.fetch_add(
                1u, std::memory_order_relaxed);
          }
          return false;
        }
        // The cache entry is thread-local and cannot mutate underneath this
        // copy.  Avoid the previous entry->temporary->out double copy.
        out = entry.value;
        return true;
      }
    }
    if (collectStats) {
      stats.modelGenerationMismatches.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else if (entry.valid) {
    if (collectStats)
      stats.modelCollisions.fetch_add(1u, std::memory_order_relaxed);
  }

  if (collectStats)
    stats.modelMisses.fetch_add(1u, std::memory_order_relaxed);
  model::ModelInstanceAugmentView value = {};
  uint64_t lookupGeneration = 0u;
  const bool found = registry.findFirstForAugmentView(
      key.worldObjectEntry, key.sceneNode, key.primaryUnitPtr,
      key.secondaryUnitPtr, key.jHandle, value, &lookupGeneration);
  entry.valid = true;
  entry.found = found;
  entry.mutationGeneration = lookupGeneration;
  entry.key = key;
  entry.value = found ? value : model::ModelInstanceAugmentView{};
  if (found)
    out = value;
  return found;
}

bool FindShadowObjectAugmentCached(
    ShadowObjectRegistry& registry,
    const SemanticAugmentShadowCacheKey& key,
    ShadowObjectAugmentView& out,
    uint64_t& registryFrameOut) {
  auto& stats = g_semanticAugmentTlsCacheStats;
  const bool collectStats = SemanticAugmentTlsCacheStatsRuntime();
  if (collectStats)
    stats.shadowLookups.fetch_add(1u, std::memory_order_relaxed);

  auto* const cacheStorage = GetSemanticAugmentTlsCacheStorage();
  if (cacheStorage == nullptr) {
    if (collectStats)
      stats.shadowMisses.fetch_add(1u, std::memory_order_relaxed);
    return registry.findFirstForAugmentView(
        key.worldObjectEntry, key.sceneNode, key.primaryUnitPtr,
        key.secondaryUnitPtr, key.jHandle, key.runtimeModelPtr, out, nullptr,
        &registryFrameOut);
  }

  auto& entry = cacheStorage->shadow[
      HashSemanticAugmentCacheKey(key) &
      (kSemanticAugmentTlsCacheCapacity - 1u)];
  const uint64_t generationBefore = registry.mutationGeneration();
  if (entry.valid && entry.key == key) {
    if ((generationBefore & 1u) == 0u &&
        entry.mutationGeneration == generationBefore) {
      const bool found = entry.found;
      const uint64_t registryFrame = entry.registryFrame;
      const uint64_t generationAfter = registry.mutationGeneration();
      if (generationAfter == generationBefore) {
        if (collectStats)
          stats.shadowHits.fetch_add(1u, std::memory_order_relaxed);
        registryFrameOut = registryFrame;
        if (!found) {
          if (collectStats) {
            stats.shadowNegativeHits.fetch_add(
                1u, std::memory_order_relaxed);
          }
          return false;
        }
        // The cache entry is thread-local and cannot mutate underneath this
        // copy.  Avoid the previous entry->temporary->out double copy.
        out = entry.value;
        return true;
      }
    }
    if (collectStats) {
      stats.shadowGenerationMismatches.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else if (entry.valid) {
    if (collectStats)
      stats.shadowCollisions.fetch_add(1u, std::memory_order_relaxed);
  }

  if (collectStats)
    stats.shadowMisses.fetch_add(1u, std::memory_order_relaxed);
  ShadowObjectAugmentView value = {};
  uint64_t lookupGeneration = 0u;
  uint64_t registryFrame = 0u;
  const bool found = registry.findFirstForAugmentView(
      key.worldObjectEntry, key.sceneNode, key.primaryUnitPtr,
      key.secondaryUnitPtr, key.jHandle, key.runtimeModelPtr, value,
      &lookupGeneration, &registryFrame);
  entry.valid = true;
  entry.found = found;
  entry.mutationGeneration = lookupGeneration;
  entry.registryFrame = registryFrame;
  entry.key = key;
  entry.value = found ? value : ShadowObjectAugmentView{};
  registryFrameOut = registryFrame;
  if (found)
    out = value;
  return found;
}

struct WorldObjectsPhase1AtomicTiming {
  std::atomic<uint64_t> calls{0u};
  std::atomic<uint64_t> ticks{0u};
  std::atomic<uint64_t> maxTicks{0u};
};

struct WorldObjectsPhase1AtomicGroup {
  WorldObjectsPhase1AtomicTiming hookInclusive;
  WorldObjectsPhase1AtomicTiming collectorInclusive;
  WorldObjectsPhase1AtomicTiming collectorSetup;
  WorldObjectsPhase1AtomicTiming collectorIterate;
  WorldObjectsPhase1AtomicTiming collectorRegister;
  WorldObjectsPhase1AtomicTiming collectorTail;
  WorldObjectsPhase1AtomicTiming modelFeed;
  WorldObjectsPhase1AtomicTiming shadowFeed;
  std::atomic<uint64_t> listEntries{0u};
  std::atomic<uint64_t> acceptedEntries{0u};
  std::atomic<uint64_t> sceneNodeEntries{0u};
  std::atomic<uint64_t> handleEntries{0u};
  std::atomic<uint64_t> collectorPartitionMismatchCount{0u};
  std::atomic<uint64_t> hookContainmentViolationCount{0u};
  std::atomic<uint64_t> registerFeedContainmentViolationCount{0u};
  std::atomic<uint64_t> acceptedCountViolationCount{0u};
  std::atomic<uint64_t> sceneNodeCountViolationCount{0u};
  std::atomic<uint64_t> handleCountViolationCount{0u};
  std::array<std::atomic<uint64_t>,
             kWorldObjectsPhase1CollectorOutcomeCount>
      outcomeCounts{};
};

struct WorldObjectsPhase1TlsContext {
  uint64_t eventSequence = 0u;
  uint64_t dispatchCaptureEventSequence = 0u;
  uint32_t collectorGroup = kWorldObjectsPhase1GroupCount;
  bool collectorActive = false;
  WorldObjectsPhase1DispatchCaptureKind dispatchCaptureKind =
      WorldObjectsPhase1DispatchCaptureKind::None;
  WorldObjectsPhase1PeriodicDispatch periodicDispatch{};
  uint64_t modelFeedTicks = 0u;
  uint64_t shadowFeedTicks = 0u;
  uint64_t modelFeedMaxTicks = 0u;
  uint64_t shadowFeedMaxTicks = 0u;
  uint32_t modelFeedCalls = 0u;
  uint32_t shadowFeedCalls = 0u;
  std::array<uint64_t, kWorldObjectsPhase1GroupCount>
      pendingCollectorTicks{};
  std::array<uint32_t, kWorldObjectsPhase1GroupCount>
      pendingCollectorCalls{};
};

std::atomic<uint64_t> g_worldObjectsPhase1WritesStarted{0u};
std::atomic<uint64_t> g_worldObjectsPhase1WritesCompleted{0u};
std::atomic<uint32_t> g_worldObjectsPhase1Writers{0u};
std::atomic<uint64_t> g_worldObjectsPhase1SnapshotGeneration{0u};
std::atomic_flag g_worldObjectsPhase1WriterLock = ATOMIC_FLAG_INIT;
std::atomic<uint64_t> g_worldObjectsPhase1ActiveEventSequence{0u};
std::atomic<uint64_t> g_worldObjectsPhase1EventSequence{0u};
std::atomic<uint64_t> g_worldObjectsPhase1TrackingAttempts{0u};
std::atomic<uint64_t> g_worldObjectsPhase1TrackingHealthFastPathCalls{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthFullSummaryCompatibilityCalls{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthModelInstanceAggregateReadPasses{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthPoseAggregateReadPasses{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthModelInstanceVerifierScanPasses{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthPoseVerifierScanPasses{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthModelInstanceVerifierRecordsScanned{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthPoseVerifierRecordsScanned{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthModelInstanceVerifierMismatchCount{0u};
std::atomic<uint64_t>
    g_worldObjectsPhase1TrackingHealthPoseVerifierMismatchCount{0u};
std::atomic<uint32_t>
    g_worldObjectsPhase1TrackingHealthModelInstanceVerifierMismatchMask{0u};
std::atomic<uint32_t>
    g_worldObjectsPhase1TrackingHealthPoseVerifierMismatchMask{0u};
std::atomic<uint64_t> g_worldObjectsPhase1IdentityRequests{0u};
std::atomic<uint64_t> g_worldObjectsPhase1FallbackRequests{0u};
std::atomic<uint64_t> g_worldObjectsPhase1CollectorWithoutEventCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1CollectorReentryCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1CollectorWithoutHookCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1HookWithoutCollectorCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1RegistryFeedOutsideCollectorCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1UnexpectedGroupCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1PairedCaptureDuplicatePublishCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1PairedCaptureLostPublishCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1PairedCaptureSlotMismatchCount{0u};
std::atomic<uint64_t> g_worldObjectsPhase1ActiveDispatchCaptureSequence{0u};
std::atomic<uint64_t> g_worldObjectsPhase1ActiveDispatchCaptureOwner{0u};
std::atomic<uint32_t> g_worldObjectsPhase1ActiveDispatchCaptureKind{0u};
WorldObjectsPhase1AtomicTiming g_worldObjectsPhase1TrackingInclusive;
WorldObjectsPhase1AtomicTiming g_worldObjectsPhase1TrackingQuery;
WorldObjectsPhase1AtomicTiming g_worldObjectsPhase1TrackingDecision;
std::array<std::atomic<uint64_t>,
           kWorldObjectsPhase1TrackingReasonCount>
    g_worldObjectsPhase1ReasonCounts{};
std::array<WorldObjectsPhase1AtomicGroup,
           kWorldObjectsPhase1GroupCount>
    g_worldObjectsPhase1Groups{};
std::mutex g_worldObjectsPhase1EventMutex;
std::array<WorldObjectsPhase1Event,
           kWorldObjectsPhase1EventSlotCount>
    g_worldObjectsPhase1Events{};
thread_local WorldObjectsPhase1TlsContext g_worldObjectsPhase1Tls{};

class WorldObjectsPhase1WriteGuard {
public:
  WorldObjectsPhase1WriteGuard() noexcept {
    while (g_worldObjectsPhase1WriterLock.test_and_set(
        std::memory_order_acquire)) {
    }
    // Serializing writers keeps the odd/even generation a real seqlock even
    // if a future producer moves off the render thread.
    g_worldObjectsPhase1SnapshotGeneration.fetch_add(
        1u, std::memory_order_acq_rel);
    g_worldObjectsPhase1Writers.fetch_add(
        1u, std::memory_order_relaxed);
    g_worldObjectsPhase1WritesStarted.fetch_add(
        1u, std::memory_order_relaxed);
  }

  ~WorldObjectsPhase1WriteGuard() {
    g_worldObjectsPhase1WritesCompleted.fetch_add(
        1u, std::memory_order_relaxed);
    g_worldObjectsPhase1Writers.fetch_sub(
        1u, std::memory_order_relaxed);
    g_worldObjectsPhase1SnapshotGeneration.fetch_add(
        1u, std::memory_order_release);
    g_worldObjectsPhase1WriterLock.clear(std::memory_order_release);
  }

  WorldObjectsPhase1WriteGuard(const WorldObjectsPhase1WriteGuard&) = delete;
  WorldObjectsPhase1WriteGuard& operator=(
      const WorldObjectsPhase1WriteGuard&) = delete;
};

uint64_t WorldObjectsPhase1TickDelta(int64_t begin, int64_t end) noexcept {
  return end >= begin ? uint64_t(end - begin) : 0u;
}

void RecordWorldObjectsPhase1AtomicTimingSamples(
    WorldObjectsPhase1AtomicTiming& timing, uint64_t calls,
    uint64_t ticks, uint64_t maxTicks) noexcept {
  timing.calls.fetch_add(calls, std::memory_order_relaxed);
  timing.ticks.fetch_add(ticks, std::memory_order_relaxed);
  uint64_t current = timing.maxTicks.load(std::memory_order_relaxed);
  while (current < maxTicks &&
         !timing.maxTicks.compare_exchange_weak(
             current, maxTicks, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void RecordWorldObjectsPhase1AtomicTiming(
    WorldObjectsPhase1AtomicTiming& timing, uint64_t ticks) noexcept {
  RecordWorldObjectsPhase1AtomicTimingSamples(timing, 1u, ticks, ticks);
}

WorldObjectsPhase1RawTiming CopyWorldObjectsPhase1AtomicTiming(
    const WorldObjectsPhase1AtomicTiming& timing) noexcept {
  WorldObjectsPhase1RawTiming result = {};
  result.calls = timing.calls.load(std::memory_order_relaxed);
  result.ticks = timing.ticks.load(std::memory_order_relaxed);
  result.maxTicks = timing.maxTicks.load(std::memory_order_relaxed);
  return result;
}

bool IsWorldObjectsPhase1RawTimingZero(
    const WorldObjectsPhase1RawTiming& timing) noexcept {
  return timing.calls == 0u && timing.ticks == 0u &&
      timing.maxTicks == 0u;
}

void RecordWorldObjectsPhase1Counter(
    std::atomic<uint64_t>& counter, uint64_t value = 1u) noexcept {
  WorldObjectsPhase1WriteGuard writeGuard;
  counter.fetch_add(value, std::memory_order_relaxed);
}

uint32_t WorldObjectsPhase1ReasonBit(
    WorldObjectsPhase1TrackingReason reason) noexcept {
  const uint32_t index = static_cast<uint32_t>(reason);
  return index > 0u && index < kWorldObjectsPhase1TrackingReasonCount
      ? (1u << index)
      : 0u;
}

bool IsWorldObjectsPhase1PairedCaptureKind(
    WorldObjectsPhase1DispatchCaptureKind kind) noexcept {
  return kind == WorldObjectsPhase1DispatchCaptureKind::PurePeriodic ||
      kind == WorldObjectsPhase1DispatchCaptureKind::PostPeriodicControl;
}

void ClearWorldObjectsPhase1PairedCaptureTls() noexcept {
  g_worldObjectsPhase1PurePeriodicDispatchSequence = 0u;
  g_worldObjectsPhase1DispatchCaptureKind =
      WorldObjectsPhase1DispatchCaptureKind::None;
}

void ArmWorldObjectsPhase1PairedCapture(
    uint64_t eventSequence, WorldObjectsPhase1DispatchCaptureKind kind,
    uint64_t captureFrameSerial, uint64_t ownerThreadId) noexcept {
  g_worldObjectsPhase1Tls.dispatchCaptureEventSequence = eventSequence;
  g_worldObjectsPhase1Tls.dispatchCaptureKind = kind;
  g_worldObjectsPhase1Tls.periodicDispatch = {};
  g_worldObjectsPhase1Tls.periodicDispatch.captureRequested = true;
  g_worldObjectsPhase1Tls.periodicDispatch.captureFrameSerial =
      captureFrameSerial;
  g_worldObjectsPhase1Tls.periodicDispatch.ownerThreadId = ownerThreadId;
  g_worldObjectsPhase1Tls.periodicDispatch.flushTopologyHash =
      14695981039346656037ull;
  // This legacy TLS name is intentionally the low-cost render-hook admission
  // token for both members of the pair. The capture kind selects the eventual
  // event destination; no control frame becomes an identity/collector event.
  g_worldObjectsPhase1PurePeriodicDispatchSequence = eventSequence;
  g_worldObjectsPhase1DispatchCaptureKind = kind;
  g_worldObjectsPhase1ActiveDispatchCaptureOwner.store(
      ownerThreadId, std::memory_order_relaxed);
  g_worldObjectsPhase1ActiveDispatchCaptureKind.store(
      static_cast<uint32_t>(kind), std::memory_order_relaxed);
  g_worldObjectsPhase1ActiveDispatchCaptureSequence.store(
      eventSequence, std::memory_order_release);
}

void SettleWorldObjectsPhase1ActiveDispatchCaptureMarker(
    uint64_t completedEventSequence,
    WorldObjectsPhase1DispatchCaptureKind completedKind) noexcept {
  const uint64_t activeSequence =
      g_worldObjectsPhase1ActiveDispatchCaptureSequence.load(
          std::memory_order_acquire);
  if (activeSequence == 0u)
    return;
  const uint64_t activeOwner =
      g_worldObjectsPhase1ActiveDispatchCaptureOwner.load(
          std::memory_order_relaxed);
  const uint32_t activeKind =
      g_worldObjectsPhase1ActiveDispatchCaptureKind.load(
          std::memory_order_relaxed);
  if (activeSequence != completedEventSequence ||
      activeOwner != uint64_t(::GetCurrentThreadId()) ||
      activeKind != static_cast<uint32_t>(completedKind)) {
    g_worldObjectsPhase1PairedCaptureLostPublishCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_worldObjectsPhase1ActiveDispatchCaptureSequence.store(
      0u, std::memory_order_release);
  g_worldObjectsPhase1ActiveDispatchCaptureOwner.store(
      0u, std::memory_order_relaxed);
  g_worldObjectsPhase1ActiveDispatchCaptureKind.store(
      0u, std::memory_order_relaxed);
}

bool PublishWorldObjectsPhase1PairedCapture(
    uint64_t eventSequence, WorldObjectsPhase1DispatchCaptureKind kind,
    WorldObjectsPhase1PeriodicDispatch capture) noexcept {
  if (eventSequence == 0u || !IsWorldObjectsPhase1PairedCaptureKind(kind))
    return true;
  capture.captureRequested = true;
  capture.finalized = true;
  if (capture.ownerThreadId == 0u ||
      capture.ownerThreadId != uint64_t(::GetCurrentThreadId())) {
    g_worldObjectsPhase1PairedCaptureLostPublishCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }

  std::lock_guard<std::mutex> lock(g_worldObjectsPhase1EventMutex);
  auto& event = g_worldObjectsPhase1Events[
      (eventSequence - 1u) % kWorldObjectsPhase1EventSlotCount];
  if (event.sequence != eventSequence) {
    g_worldObjectsPhase1PairedCaptureSlotMismatchCount.fetch_add(
        1u, std::memory_order_relaxed);
    g_worldObjectsPhase1PairedCaptureLostPublishCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }
  auto& destination =
      kind == WorldObjectsPhase1DispatchCaptureKind::PurePeriodic
      ? event.periodicDispatch : event.postPeriodicControl;
  if (destination.finalized) {
    g_worldObjectsPhase1PairedCaptureDuplicatePublishCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }
  if (!destination.captureRequested ||
      destination.captureFrameSerial != capture.captureFrameSerial ||
      destination.ownerThreadId != capture.ownerThreadId) {
    g_worldObjectsPhase1PairedCaptureLostPublishCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }
  destination = capture;
  return true;
}

bool PrepareWorldObjectsPhase1PostPeriodicControl(
    uint64_t eventSequence, uint64_t periodicFrameSerial,
    uint64_t controlFrameSerial, uint64_t ownerThreadId) noexcept {
  if (eventSequence == 0u || periodicFrameSerial == 0u ||
      controlFrameSerial != periodicFrameSerial + 1u)
    return false;
  std::lock_guard<std::mutex> lock(g_worldObjectsPhase1EventMutex);
  auto& event = g_worldObjectsPhase1Events[
      (eventSequence - 1u) % kWorldObjectsPhase1EventSlotCount];
  if (event.sequence != eventSequence) {
    g_worldObjectsPhase1PairedCaptureSlotMismatchCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }
  if (event.postPeriodicControl.captureRequested ||
      event.postPeriodicControl.finalized) {
    g_worldObjectsPhase1PairedCaptureDuplicatePublishCount.fetch_add(
        1u, std::memory_order_relaxed);
    return false;
  }
  event.postPeriodicControl.captureRequested = true;
  event.postPeriodicControl.captureFrameSerial = controlFrameSerial;
  event.postPeriodicControl.ownerThreadId = ownerThreadId;
  event.postPeriodicControl.flushTopologyHash = 14695981039346656037ull;
  return true;
}

void RecordWorldObjectsPhase1Tracking(
    uint64_t frameSerial, uint64_t collectionFrameSerial,
    uint64_t poseSerial,
    WorldObjectsPhase1TrackingReason reason, uint32_t reasonMask,
    uint64_t refreshPeriod, uint64_t warmupFrames,
    const ShadowRuntimeBridgeTrackingDecision& decision,
    uint64_t inclusiveTicks, uint64_t queryTicks,
    uint64_t decisionTicks) noexcept {
  const uint64_t completedDispatchEventSequence =
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence;
  const auto completedDispatchCaptureKind =
      g_worldObjectsPhase1Tls.dispatchCaptureKind;
  auto completedPeriodicDispatch =
      g_worldObjectsPhase1Tls.periodicDispatch;
  uint64_t abandonedCollectorCalls = 0u;
  for (uint32_t group = 0u; group < kWorldObjectsPhase1GroupCount; ++group)
    abandonedCollectorCalls +=
        g_worldObjectsPhase1Tls.pendingCollectorCalls[group];

  WorldObjectsPhase1WriteGuard writeGuard;
  SettleWorldObjectsPhase1ActiveDispatchCaptureMarker(
      completedDispatchEventSequence, completedDispatchCaptureKind);
  g_worldObjectsPhase1Tls = {};
  ClearWorldObjectsPhase1PairedCaptureTls();
  const uint32_t reasonIndex = static_cast<uint32_t>(reason);
  const uint32_t safeReasonIndex =
      reasonIndex < kWorldObjectsPhase1TrackingReasonCount ? reasonIndex : 0u;

  PublishWorldObjectsPhase1PairedCapture(
      completedDispatchEventSequence, completedDispatchCaptureKind,
      completedPeriodicDispatch);
  if (abandonedCollectorCalls != 0u) {
    g_worldObjectsPhase1CollectorWithoutHookCount.fetch_add(
        abandonedCollectorCalls, std::memory_order_relaxed);
  }
  g_worldObjectsPhase1TrackingAttempts.fetch_add(
      1u, std::memory_order_relaxed);
  const auto addTrackingHealthCounter =
      [](std::atomic<uint64_t>& counter, uint64_t value) {
        if (value != 0u)
          counter.fetch_add(value, std::memory_order_relaxed);
      };
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthFastPathCalls,
      decision.trackingHealthFastPathCalls);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthFullSummaryCompatibilityCalls,
      decision.trackingHealthFullSummaryCompatibilityCalls);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthModelInstanceAggregateReadPasses,
      decision.trackingHealthModelInstanceAggregateReadPasses);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthPoseAggregateReadPasses,
      decision.trackingHealthPoseAggregateReadPasses);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthModelInstanceVerifierScanPasses,
      decision.trackingHealthModelInstanceVerifierScanPasses);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthPoseVerifierScanPasses,
      decision.trackingHealthPoseVerifierScanPasses);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthModelInstanceVerifierRecordsScanned,
      decision.trackingHealthModelInstanceVerifierRecordsScanned);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthPoseVerifierRecordsScanned,
      decision.trackingHealthPoseVerifierRecordsScanned);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthModelInstanceVerifierMismatchCount,
      decision.trackingHealthModelInstanceVerifierMismatchCount);
  addTrackingHealthCounter(
      g_worldObjectsPhase1TrackingHealthPoseVerifierMismatchCount,
      decision.trackingHealthPoseVerifierMismatchCount);
  if (decision.trackingHealthModelInstanceVerifierMismatchMask != 0u) {
    g_worldObjectsPhase1TrackingHealthModelInstanceVerifierMismatchMask
        .fetch_or(
            decision.trackingHealthModelInstanceVerifierMismatchMask,
            std::memory_order_relaxed);
  }
  if (decision.trackingHealthPoseVerifierMismatchMask != 0u) {
    g_worldObjectsPhase1TrackingHealthPoseVerifierMismatchMask.fetch_or(
        decision.trackingHealthPoseVerifierMismatchMask,
        std::memory_order_relaxed);
  }
  g_worldObjectsPhase1ReasonCounts[safeReasonIndex].fetch_add(
      1u, std::memory_order_relaxed);
  if (decision.wantsObjectIdentity) {
    g_worldObjectsPhase1IdentityRequests.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (decision.wantsFallbackBridge) {
    g_worldObjectsPhase1FallbackRequests.fetch_add(
        1u, std::memory_order_relaxed);
  }
  RecordWorldObjectsPhase1AtomicTiming(
      g_worldObjectsPhase1TrackingInclusive, inclusiveTicks);
  RecordWorldObjectsPhase1AtomicTiming(
      g_worldObjectsPhase1TrackingQuery, queryTicks);
  RecordWorldObjectsPhase1AtomicTiming(
      g_worldObjectsPhase1TrackingDecision, decisionTicks);

  const bool cleanPostPeriodicControl =
      completedDispatchCaptureKind ==
          WorldObjectsPhase1DispatchCaptureKind::PurePeriodic &&
      reason == WorldObjectsPhase1TrackingReason::None && reasonMask == 0u &&
      !decision.wantsObjectIdentity && !decision.wantsFallbackBridge &&
      collectionFrameSerial ==
          completedPeriodicDispatch.captureFrameSerial + 1u;
  if (reason == WorldObjectsPhase1TrackingReason::None ||
      !decision.wantsObjectIdentity) {
    g_worldObjectsPhase1ActiveEventSequence.store(
        0u, std::memory_order_release);
    if (cleanPostPeriodicControl &&
        PrepareWorldObjectsPhase1PostPeriodicControl(
            completedDispatchEventSequence,
            completedPeriodicDispatch.captureFrameSerial,
            collectionFrameSerial, uint64_t(::GetCurrentThreadId()))) {
      ArmWorldObjectsPhase1PairedCapture(
          completedDispatchEventSequence,
          WorldObjectsPhase1DispatchCaptureKind::PostPeriodicControl,
          collectionFrameSerial, uint64_t(::GetCurrentThreadId()));
    }
    return;
  }

  const uint64_t eventSequence =
      g_worldObjectsPhase1EventSequence.fetch_add(
          1u, std::memory_order_relaxed) + 1u;
  WorldObjectsPhase1Event event = {};
  event.sequence = eventSequence;
  event.frameSerial = frameSerial;
  event.collectionFrameSerial = collectionFrameSerial;
  event.poseSerial = poseSerial;
  event.trackingInclusiveTicks = inclusiveTicks;
  event.trackingQueryTicks = queryTicks;
  event.trackingDecisionTicks = decisionTicks;
  event.refreshPeriod = refreshPeriod;
  event.warmupFrames = warmupFrames;
  event.reason = reason;
  event.reasonMask = reasonMask;
  event.wantsObjectIdentity = decision.wantsObjectIdentity;
  event.wantsFallbackBridge = decision.wantsFallbackBridge;
  const uint32_t periodicReasonBit = WorldObjectsPhase1ReasonBit(
      WorldObjectsPhase1TrackingReason::PeriodicMaintenance);
  const bool purePeriodicDispatchCapture =
      reason == WorldObjectsPhase1TrackingReason::PeriodicMaintenance &&
      reasonMask == periodicReasonBit;
  event.periodicDispatch.captureRequested =
      purePeriodicDispatchCapture;
  if (purePeriodicDispatchCapture) {
    event.periodicDispatch.captureFrameSerial = collectionFrameSerial;
    event.periodicDispatch.ownerThreadId = uint64_t(::GetCurrentThreadId());
    event.periodicDispatch.flushTopologyHash = 14695981039346656037ull;
  }
  {
    std::lock_guard<std::mutex> lock(g_worldObjectsPhase1EventMutex);
    g_worldObjectsPhase1Events[
        (eventSequence - 1u) % kWorldObjectsPhase1EventSlotCount] = event;
  }
  g_worldObjectsPhase1Tls.eventSequence = eventSequence;
  g_worldObjectsPhase1Tls.collectorGroup =
      kWorldObjectsPhase1GroupCount;
  if (purePeriodicDispatchCapture) {
    ArmWorldObjectsPhase1PairedCapture(
        eventSequence, WorldObjectsPhase1DispatchCaptureKind::PurePeriodic,
        collectionFrameSerial, uint64_t(::GetCurrentThreadId()));
  }
  g_worldObjectsPhase1ActiveEventSequence.store(
      eventSequence, std::memory_order_release);
}

bool IsSemanticDataModuleEnabled() {
  return runtime::IsWar3RuntimeModuleEnabled(
      runtime::War3RuntimeModule::SemanticData);
}

bool IsSemanticModelProducerEnabled() {
  return IsSemanticDataModuleEnabled() &&
         internal::kWar3RuntimeConfigSemanticModelProducerEffective;
}

bool IsSemanticFrameRegistriesEnabled() {
  return IsSemanticModelProducerEnabled() &&
         internal::kWar3RuntimeConfigSemanticFrameRegistriesEffective;
}

bool IsSemanticContractCaptureEnabled() {
  return IsSemanticModelProducerEnabled() &&
         internal::kWar3RuntimeConfigSemanticContractCaptureEffective;
}

bool IsSemanticConsumerEnabled() {
  return IsSemanticModelProducerEnabled() &&
         internal::kWar3RuntimeConfigSemanticConsumerEffective;
}

SemanticBuildSkippedReason CurrentSemanticBuildSkippedReason() {
  if (!IsSemanticDataModuleEnabled())
    return SemanticBuildSkippedReason::SemanticDataModuleDisabled;
  if (!IsSemanticModelProducerEnabled())
    return SemanticBuildSkippedReason::ModelProducerDisabled;
  if (!IsSemanticFrameRegistriesEnabled())
    return SemanticBuildSkippedReason::FrameRegistriesDisabled;
  if (!IsSemanticContractCaptureEnabled())
    return SemanticBuildSkippedReason::ContractCaptureDisabled;
  if (!IsSemanticConsumerEnabled())
    return SemanticBuildSkippedReason::ConsumerDisabled;
  if (!internal::IsSemanticSceneSubmissionRuntimeEnabled())
    return SemanticBuildSkippedReason::SceneSubmissionDisabled;
  return SemanticBuildSkippedReason::None;
}

std::atomic<uint64_t> g_shadowBridgeRepairUntilFrame{0u};
std::atomic<uint64_t> g_shadowBridgeRepairCooldownUntilFrame{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStagePrepareAttemptCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStagePrepareSuccessCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageExecuteAttemptCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageExecuteSuccessCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidateCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidatePrepareCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidateRefreshCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageCandidateExecuteCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateStage{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateA3{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateA4{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateA5{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateJassReady{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateGameStarted{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastCandidateRuntimeFrame{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastPrepareStage{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastExecuteStage{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastPrepareFrameSerial{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastExecuteFrameSerial{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastPrepareDrawCount{0u};
std::atomic<uint64_t> g_nativeSemanticWorldStageLastExecuteDrawCount{0u};
// Phase 7.86：reader 路径每帧 1+ 次（trace JSON 生成 / status query），
// writer 路径每帧 1 次（NoteShadowSceneStats）。改 shared_mutex 让 reader
// 路径不互斥，trace 拉取与统计写入并发更安全。
std::shared_mutex g_shadowSceneStatsMutex;
War3ShadowCaptureStats g_shadowSceneStats = {};
uint64_t g_shadowSceneStatsPublishCount = 0u;
std::mutex g_shadowCadenceMutex;
std::array<ShadowRuntimeCadenceSample,
           kShadowRuntimeCadenceSampleCapacity>
    g_shadowCadenceSamples = {};
uint64_t g_shadowCadenceNextSerial = 0u;
uint64_t g_shadowCadenceSampleCountTotal = 0u;
uint32_t g_shadowCadenceWriteIndex = 0u;
uint32_t g_shadowCadenceSampleCount = 0u;
uint64_t g_shadowCadenceLastDynamicPoseSignature = 0u;
uint64_t g_shadowCadenceLastSceneFrameSerial = 0u;
uint64_t g_shadowCadenceSameDynamicPoseStreak = 0u;
uint64_t g_shadowCadenceSameDynamicPoseStreakMax = 0u;
uint64_t g_shadowCadenceSameSceneFrameStreak = 0u;
uint64_t g_shadowCadenceSameSceneFrameStreakMax = 0u;
uint64_t g_shadowCadenceShadowMapReuseStreak = 0u;
uint64_t g_shadowCadenceShadowMapReuseStreakMax = 0u;

struct ShadowPoseFullTraceConfigSnapshot {
  bool enabled = false;
  bool includePoseRecords = true;
  bool includeShadowObjectRecords = true;
  bool includeCurrentDrawRecords = true;
  bool includeFinalCasterRecords = true;
  bool includeMatrixBytes = false;
  uint32_t maxSeconds = 15u;
  uint32_t maxPoseRecords = 0u;
  uint32_t maxShadowObjectRecords = 0u;
  uint32_t maxCurrentDrawRecords = 0u;
  uint32_t maxFinalCasterRecords = 0u;
  uint32_t finalCasterSampleBytes = 4096u;
  uint64_t epoch = 0u;
};

std::mutex g_shadowPoseFullTraceMutex;
std::ofstream g_shadowPoseFullTraceStream;
bool g_shadowPoseFullTraceEnvLoaded = false;
bool g_shadowPoseFullTraceEnvEnabled = false;
bool g_shadowPoseFullTraceManualEnabled = false;
bool g_shadowPoseFullTraceOpened = false;
bool g_shadowPoseFullTraceStoppedByLimit = false;
bool g_shadowPoseFullTraceIncludePoseRecords = true;
bool g_shadowPoseFullTraceIncludeShadowObjectRecords = true;
bool g_shadowPoseFullTraceIncludeCurrentDrawRecords = true;
bool g_shadowPoseFullTraceIncludeFinalCasterRecords = true;
bool g_shadowPoseFullTraceIncludeMatrixBytes = false;
uint32_t g_shadowPoseFullTraceMaxSeconds = 15u;
uint32_t g_shadowPoseFullTraceMaxPoseRecords = 0u;
uint32_t g_shadowPoseFullTraceMaxShadowObjectRecords = 0u;
uint32_t g_shadowPoseFullTraceMaxCurrentDrawRecords = 0u;
uint32_t g_shadowPoseFullTraceMaxFinalCasterRecords = 0u;
uint32_t g_shadowPoseFullTraceFinalCasterSampleBytes = 4096u;
uint64_t g_shadowPoseFullTraceEpoch = 0u;
uint64_t g_shadowPoseFullTraceFrameEventsWritten = 0u;
uint64_t g_shadowPoseFullTraceRecordEventsWritten = 0u;
uint64_t g_shadowPoseFullTraceLastFinalCasterFrameSerial = 0u;
const War3FrameScene* g_shadowPoseFullTraceLastFinalCasterScene = nullptr;
std::chrono::steady_clock::time_point g_shadowPoseFullTraceStart = {};
std::string g_shadowPoseFullTracePath;
constexpr size_t kSemanticPerfTagCount =
    static_cast<size_t>(SemanticDataPerfTag::Count);
std::array<std::atomic<uint64_t>, kSemanticPerfTagCount>
    g_semanticPerfCalls = {};
std::array<std::atomic<uint64_t>, kSemanticPerfTagCount> g_semanticPerfUs = {};
std::atomic<uint64_t> g_semanticConsumerBuildSkippedFresh{0u};
std::atomic<uint64_t> g_semanticSummaryRefreshFrameSerial{0u};
std::atomic<uint64_t> g_semanticSummaryRefreshPublishRevision{0u};
std::atomic<uint64_t> g_semanticLastHotFunctionTag{0u};
std::atomic<uint64_t> g_semanticLastHotFunctionUs{0u};

constexpr uint64_t kShadowBridgeRepairBurstFrames = 24u;
constexpr uint64_t kShadowBridgeRepairCooldownFrames = 120u;

bool ParseTraceBool(const std::string& value, bool defaultValue) {
  if (value.empty())
    return defaultValue;
  return value == "1" || value == "true" || value == "TRUE" ||
         value == "yes" || value == "YES" || value == "on" ||
         value == "ON";
}

uint32_t ParseTraceU32(const std::string& value, uint32_t defaultValue) {
  if (value.empty())
    return defaultValue;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str())
    return defaultValue;
  return static_cast<uint32_t>((std::min)(parsed, 0xFFFFFFFFul));
}

std::string ResolveWar3LogDirectory() {
  char exePath[MAX_PATH] = {};
  if (::GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    return {};

  std::string exeDir(exePath);
  const size_t slash = exeDir.find_last_of("\\/");
  if (slash == std::string::npos)
    return {};

  const std::string warVkDir = exeDir.substr(0, slash + 1u) + "WarVK\\";
  const std::string logDir = warVkDir + "Log\\";
  ::CreateDirectoryA(warVkDir.c_str(), nullptr);
  ::CreateDirectoryA(logDir.c_str(), nullptr);
  return logDir;
}

std::string BuildShadowPoseFullTracePath() {
  const std::string logDir = ResolveWar3LogDirectory();
  SYSTEMTIME st = {};
  ::GetLocalTime(&st);
  char name[160] = {};
  std::snprintf(name, sizeof(name),
                "shadow_pose_full_trace_%04u_%02u_%02u_%02u_%02u_%02u.jsonl",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond);
  return logDir.empty() ? std::string(name) : logDir + name;
}

void InitializeShadowPoseFullTraceEnvLocked() {
  if (g_shadowPoseFullTraceEnvLoaded)
    return;
  g_shadowPoseFullTraceEnvLoaded = true;

  g_shadowPoseFullTraceEnvEnabled = ParseTraceBool(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE"), false);
  if (!g_shadowPoseFullTraceEnvEnabled)
    return;

  g_shadowPoseFullTraceIncludePoseRecords = ParseTraceBool(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_POSES"), true);
  g_shadowPoseFullTraceIncludeShadowObjectRecords = ParseTraceBool(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_OBJECTS"), true);
  g_shadowPoseFullTraceIncludeCurrentDrawRecords = ParseTraceBool(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CONTRACTS"), true);
  g_shadowPoseFullTraceIncludeFinalCasterRecords = ParseTraceBool(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTERS"), true);
  g_shadowPoseFullTraceIncludeMatrixBytes = ParseTraceBool(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MATRIX_BYTES"), false);
  g_shadowPoseFullTraceMaxSeconds = ParseTraceU32(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_SEC"), 15u);
  g_shadowPoseFullTraceMaxPoseRecords = ParseTraceU32(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_POSES"), 0u);
  g_shadowPoseFullTraceMaxShadowObjectRecords = ParseTraceU32(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_OBJECTS"), 0u);
  g_shadowPoseFullTraceMaxCurrentDrawRecords = ParseTraceU32(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CONTRACTS"), 0u);
  g_shadowPoseFullTraceMaxFinalCasterRecords = ParseTraceU32(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CASTERS"), 0u);
  g_shadowPoseFullTraceFinalCasterSampleBytes = ParseTraceU32(
      env::getEnvVar("DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTER_SAMPLE_BYTES"),
      4096u);
  ++g_shadowPoseFullTraceEpoch;
}

ShadowPoseFullTraceConfigSnapshot ShadowPoseFullTraceConfigLocked() {
  InitializeShadowPoseFullTraceEnvLocked();

  ShadowPoseFullTraceConfigSnapshot config = {};
  config.enabled =
      (g_shadowPoseFullTraceEnvEnabled || g_shadowPoseFullTraceManualEnabled) &&
      !g_shadowPoseFullTraceStoppedByLimit;
  config.includePoseRecords = g_shadowPoseFullTraceIncludePoseRecords;
  config.includeShadowObjectRecords =
      g_shadowPoseFullTraceIncludeShadowObjectRecords;
  config.includeCurrentDrawRecords =
      g_shadowPoseFullTraceIncludeCurrentDrawRecords;
  config.includeFinalCasterRecords =
      g_shadowPoseFullTraceIncludeFinalCasterRecords;
  config.includeMatrixBytes = g_shadowPoseFullTraceIncludeMatrixBytes;
  config.maxSeconds = g_shadowPoseFullTraceMaxSeconds;
  config.maxPoseRecords = g_shadowPoseFullTraceMaxPoseRecords;
  config.maxShadowObjectRecords =
      g_shadowPoseFullTraceMaxShadowObjectRecords;
  config.maxCurrentDrawRecords =
      g_shadowPoseFullTraceMaxCurrentDrawRecords;
  config.maxFinalCasterRecords =
      g_shadowPoseFullTraceMaxFinalCasterRecords;
  config.finalCasterSampleBytes =
      g_shadowPoseFullTraceFinalCasterSampleBytes;
  config.epoch = g_shadowPoseFullTraceEpoch;
  return config;
}

bool ShadowPoseFullTraceDeadlineReachedLocked() {
  if (g_shadowPoseFullTraceMaxSeconds == 0u ||
      g_shadowPoseFullTraceStart == std::chrono::steady_clock::time_point{}) {
    return false;
  }
  const double elapsedSec = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() -
                                g_shadowPoseFullTraceStart)
                                .count();
  return elapsedSec >= double(g_shadowPoseFullTraceMaxSeconds);
}

void CloseShadowPoseFullTraceLocked() {
  if (g_shadowPoseFullTraceStream.is_open()) {
    g_shadowPoseFullTraceStream.flush();
    g_shadowPoseFullTraceStream.close();
  }
  g_shadowPoseFullTraceOpened = false;
}

void WriteJsonEscaped(std::ostream& os, const std::string& value) {
  os << '"';
  for (const char ch : value) {
    switch (ch) {
      case '\\': os << "\\\\"; break;
      case '"': os << "\\\""; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20u) {
          os << "\\u"
             << std::hex << std::setw(4) << std::setfill('0')
             << unsigned(static_cast<unsigned char>(ch))
             << std::dec << std::setfill(' ');
        } else {
          os << ch;
        }
        break;
    }
  }
  os << '"';
}

void WriteHexValue(std::ostream& os, uint64_t value) {
  os << "\"0x" << std::hex << std::setw(16) << std::setfill('0') << value
     << std::dec << std::setfill(' ') << '"';
}

void WriteHexField(std::ostream& os, const char* name, uint64_t value) {
  os << ",\"" << name << "\":";
  WriteHexValue(os, value);
}

void WritePtrField(std::ostream& os, const char* name, const void* value) {
  WriteHexField(os, name, uint64_t(reinterpret_cast<uintptr_t>(value)));
}

void WriteRawHexBytes(std::ostream& os, const void* data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  os << '"';
  for (size_t i = 0u; i < size; ++i) {
    os << kHex[bytes[i] >> 4u] << kHex[bytes[i] & 0x0Fu];
  }
  os << '"';
}

void WriteMatrixJson(std::ostream& os, const Matrix4& matrix) {
  os << '[';
  for (uint32_t row = 0u; row < 4u; ++row) {
    if (row != 0u)
      os << ',';
    os << '[';
    for (uint32_t col = 0u; col < 4u; ++col) {
      if (col != 0u)
        os << ',';
      os << matrix[row][col];
    }
    os << ']';
  }
  os << ']';
}

uint64_t ShadowCasterTraceHashInit() {
  return 14695981039346656037ull;
}

void ShadowCasterTraceHashBytes(uint64_t& hash, const void* data, size_t size) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0u; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= 1099511628211ull;
  }
}

template <typename T>
void ShadowCasterTraceHashValue(uint64_t& hash, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  ShadowCasterTraceHashBytes(hash, &value, sizeof(value));
}

template <typename Handle>
uint64_t ShadowCasterTraceHandleBits(Handle handle) {
  uint64_t bits = 0u;
  static_assert(sizeof(handle) <= sizeof(bits));
  std::memcpy(&bits, &handle, sizeof(handle));
  return bits;
}

uint64_t ShadowCasterTraceBufferSampleHash(
    const DxvkResourceBufferInfo& info, uint32_t maxBytes) {
  if (info.mapPtr == nullptr || info.size == 0u || maxBytes == 0u)
    return 0u;

  const uint64_t boundedSize64 =
      (std::min)(uint64_t(info.size), uint64_t(SIZE_MAX));
  const size_t size = static_cast<size_t>(boundedSize64);
  const size_t budget = (std::min)(size, size_t(maxBytes));
  if (budget == 0u)
    return 0u;

  uint64_t hash = ShadowCasterTraceHashInit();
  ShadowCasterTraceHashValue(hash, boundedSize64);
  const auto* bytes = reinterpret_cast<const uint8_t*>(info.mapPtr);
  if (budget == size) {
    ShadowCasterTraceHashBytes(hash, bytes, size);
  } else {
    const size_t firstBytes = budget / 2u;
    const size_t lastBytes = budget - firstBytes;
    ShadowCasterTraceHashBytes(hash, bytes, firstBytes);
    ShadowCasterTraceHashBytes(hash, bytes + size - lastBytes, lastBytes);
  }
  return hash;
}

uint64_t ShadowCasterTraceIdentityHash(const War3ShadowCasterDraw& draw) {
  uint64_t hash = ShadowCasterTraceHashInit();
  ShadowCasterTraceHashValue(hash, draw.stage);
  const uint32_t category = static_cast<uint32_t>(draw.category);
  const int32_t batchTag = static_cast<int32_t>(draw.batchTag);
  ShadowCasterTraceHashValue(hash, category);
  ShadowCasterTraceHashValue(hash, batchTag);
  ShadowCasterTraceHashValue(hash, draw.objectKind);
  ShadowCasterTraceHashValue(hash, draw.rawcode);
  ShadowCasterTraceHashValue(hash, draw.jHandle);
  ShadowCasterTraceHashValue(hash, draw.batchHandle);
  ShadowCasterTraceHashValue(hash, draw.indexed);
  ShadowCasterTraceHashValue(hash, draw.topology);
  ShadowCasterTraceHashValue(hash, draw.indexCount);
  ShadowCasterTraceHashValue(hash, draw.firstIndex);
  ShadowCasterTraceHashValue(hash, draw.vertexOffset);
  ShadowCasterTraceHashValue(hash, draw.vertexCount);
  ShadowCasterTraceHashValue(hash, draw.firstVertex);
  ShadowCasterTraceHashValue(hash, draw.minVertexIndex);
  ShadowCasterTraceHashValue(hash, draw.numVertices);
  return hash;
}

uint64_t ShadowCasterTraceBackingHash(const War3ShadowCasterDraw& draw) {
  uint64_t hash = ShadowCasterTraceHashInit();
  const uint64_t positionStorage =
      uint64_t(reinterpret_cast<uintptr_t>(draw.positionStorage.ptr()));
  const uint64_t indexStorage =
      uint64_t(reinterpret_cast<uintptr_t>(draw.indexStorage.ptr()));
  const uint64_t blendStorage =
      uint64_t(reinterpret_cast<uintptr_t>(draw.blendStorage.ptr()));
  const uint64_t uvStorage =
      uint64_t(reinterpret_cast<uintptr_t>(draw.uvStorage.ptr()));
  ShadowCasterTraceHashValue(hash, positionStorage);
  ShadowCasterTraceHashValue(
      hash, ShadowCasterTraceHandleBits(draw.positionInfo.buffer));
  ShadowCasterTraceHashValue(hash, draw.positionInfo.offset);
  ShadowCasterTraceHashValue(hash, draw.positionInfo.size);
  ShadowCasterTraceHashValue(hash, indexStorage);
  ShadowCasterTraceHashValue(
      hash, ShadowCasterTraceHandleBits(draw.indexInfo.buffer));
  ShadowCasterTraceHashValue(hash, draw.indexInfo.offset);
  ShadowCasterTraceHashValue(hash, draw.indexInfo.size);
  ShadowCasterTraceHashValue(hash, blendStorage);
  ShadowCasterTraceHashValue(
      hash, ShadowCasterTraceHandleBits(draw.blendInfo.buffer));
  ShadowCasterTraceHashValue(hash, draw.blendInfo.offset);
  ShadowCasterTraceHashValue(hash, draw.blendInfo.size);
  ShadowCasterTraceHashValue(hash, uvStorage);
  ShadowCasterTraceHashValue(
      hash, ShadowCasterTraceHandleBits(draw.uvInfo.buffer));
  ShadowCasterTraceHashValue(hash, draw.uvInfo.offset);
  ShadowCasterTraceHashValue(hash, draw.uvInfo.size);
  if (draw.positionStorage != nullptr) {
    ShadowCasterTraceHashValue(
        hash, draw.positionStorage->diagnosticStorageGeneration());
  }
  if (draw.indexStorage != nullptr) {
    ShadowCasterTraceHashValue(
        hash, draw.indexStorage->diagnosticStorageGeneration());
  }
  if (draw.blendStorage != nullptr) {
    ShadowCasterTraceHashValue(
        hash, draw.blendStorage->diagnosticStorageGeneration());
  }
  if (draw.uvStorage != nullptr) {
    ShadowCasterTraceHashValue(
        hash, draw.uvStorage->diagnosticStorageGeneration());
  }
  return hash;
}

uint64_t ShadowCasterTraceContentHash(
    const War3ShadowCasterDraw& draw, uint32_t sampleBytes) {
  uint64_t hash = ShadowCasterTraceIdentityHash(draw);
  const uint64_t backingHash = ShadowCasterTraceBackingHash(draw);
  ShadowCasterTraceHashValue(hash, backingHash);
  ShadowCasterTraceHashBytes(hash, &draw.worldMatrix,
                             sizeof(draw.worldMatrix));
  ShadowCasterTraceHashBytes(hash, &draw.boundsCenter,
                             sizeof(draw.boundsCenter));
  ShadowCasterTraceHashValue(hash, draw.boundsRadius);
  ShadowCasterTraceHashValue(hash, draw.positionStride);
  ShadowCasterTraceHashValue(hash, draw.positionOffset);
  ShadowCasterTraceHashValue(hash, draw.positionFormat);
  ShadowCasterTraceHashValue(hash, draw.indexType);
  ShadowCasterTraceHashValue(hash, draw.vertexBlendEnabled);
  ShadowCasterTraceHashValue(hash, draw.vertexBlendIndexed);
  ShadowCasterTraceHashValue(hash, draw.vertexBlendCount);
  ShadowCasterTraceHashValue(hash, draw.paletteIndex);
  ShadowCasterTraceHashValue(hash, draw.alphaTestEnabled);
  ShadowCasterTraceHashValue(hash, draw.alphaBlendEnabled);
  ShadowCasterTraceHashValue(hash, draw.alphaRef);
  ShadowCasterTraceHashValue(hash, draw.uvStride);
  ShadowCasterTraceHashValue(hash, draw.uvOffset);
  ShadowCasterTraceHashValue(hash, draw.uvFormat);
  ShadowCasterTraceHashValue(hash, draw.uvBinding);
  const uint64_t positionSample =
      ShadowCasterTraceBufferSampleHash(draw.positionInfo, sampleBytes);
  const uint64_t indexSample =
      ShadowCasterTraceBufferSampleHash(draw.indexInfo, sampleBytes);
  ShadowCasterTraceHashValue(hash, positionSample);
  ShadowCasterTraceHashValue(hash, indexSample);
  return hash;
}

bool ShadowCasterTraceFiniteMatrix(const Matrix4& matrix) {
  for (uint32_t row = 0u; row < 4u; ++row) {
    for (uint32_t col = 0u; col < 4u; ++col) {
      if (!std::isfinite(matrix[row][col]))
        return false;
    }
  }
  return true;
}

uint32_t ShadowCasterTraceValidationFlags(
    const War3ShadowCasterDraw& draw) {
  uint32_t flags = 0u;
  if (!ShadowCasterTraceFiniteMatrix(draw.worldMatrix))
    flags |= 1u << 0u;
  if (draw.positionStorage == nullptr ||
      draw.positionInfo.buffer == VK_NULL_HANDLE ||
      draw.positionInfo.size == 0u ||
      draw.positionStride == 0u ||
      draw.positionOffset >= draw.positionStride) {
    flags |= 1u << 1u;
  }
  if (draw.indexed &&
      (draw.indexStorage == nullptr ||
       draw.indexInfo.buffer == VK_NULL_HANDLE ||
       draw.indexInfo.size == 0u ||
       draw.indexCount == 0u)) {
    flags |= 1u << 2u;
  }
  if (!std::isfinite(draw.boundsCenter.x) ||
      !std::isfinite(draw.boundsCenter.y) ||
      !std::isfinite(draw.boundsCenter.z) ||
      !std::isfinite(draw.boundsRadius) ||
      draw.boundsRadius < 0.0f) {
    flags |= 1u << 3u;
  }
  if (draw.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
      draw.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) {
    flags |= 1u << 4u;
  }
  return flags;
}

void WriteFinalShadowCasterRecordEvent(
    std::ostream& os,
    uint64_t epoch,
    uint64_t frameSerial,
    uint32_t index,
    const War3ShadowCasterDraw& draw,
    uint32_t sampleBytes) {
  const uint64_t identityHash = ShadowCasterTraceIdentityHash(draw);
  const uint64_t backingHash = ShadowCasterTraceBackingHash(draw);
  const uint64_t contentHash =
      ShadowCasterTraceContentHash(draw, sampleBytes);
  const uint64_t positionSampleHash =
      ShadowCasterTraceBufferSampleHash(draw.positionInfo, sampleBytes);
  const uint64_t indexSampleHash =
      ShadowCasterTraceBufferSampleHash(draw.indexInfo, sampleBytes);
  const uint64_t positionGeneration =
      draw.positionStorage != nullptr
          ? draw.positionStorage->diagnosticStorageGeneration()
          : 0u;
  const uint64_t indexGeneration =
      draw.indexStorage != nullptr
          ? draw.indexStorage->diagnosticStorageGeneration()
          : 0u;
  const uint64_t blendGeneration =
      draw.blendStorage != nullptr
          ? draw.blendStorage->diagnosticStorageGeneration()
          : 0u;
  const uint64_t uvGeneration =
      draw.uvStorage != nullptr
          ? draw.uvStorage->diagnosticStorageGeneration()
          : 0u;

  os << std::setprecision(9)
     << "{\"type\":\"shadowFinalCasterRecord\""
     << ",\"version\":1"
     << ",\"epoch\":" << epoch
     << ",\"frameSerial\":" << frameSerial
     << ",\"index\":" << index
     << ",\"stage\":" << draw.stage
     << ",\"category\":" << static_cast<uint32_t>(draw.category)
     << ",\"batchTag\":" << static_cast<int32_t>(draw.batchTag)
     << ",\"objectKind\":" << static_cast<uint32_t>(draw.objectKind)
     << ",\"rawcode\":" << draw.rawcode
     << ",\"jHandle\":" << draw.jHandle
     << ",\"batchHandle\":" << draw.batchHandle
     << ",\"layerIndex\":" << draw.shadowLayerIndex
     << ",\"pathBlocker\":" << (draw.pathBlocker ? 1 : 0)
     << ",\"pathBlockerGeometryMarker\":"
     << (draw.pathBlockerGeometryMarker ? 1 : 0)
     << ",\"indexed\":" << (draw.indexed ? 1 : 0)
     << ",\"topology\":" << static_cast<uint32_t>(draw.topology)
     << ",\"indexCount\":" << draw.indexCount
     << ",\"firstIndex\":" << draw.firstIndex
     << ",\"vertexOffset\":" << draw.vertexOffset
     << ",\"vertexCount\":" << draw.vertexCount
     << ",\"firstVertex\":" << draw.firstVertex
     << ",\"minVertexIndex\":" << draw.minVertexIndex
     << ",\"numVertices\":" << draw.numVertices
     << ",\"positionStride\":" << draw.positionStride
     << ",\"positionOffset\":" << draw.positionOffset
     << ",\"positionFormat\":"
     << static_cast<uint32_t>(draw.positionFormat)
     << ",\"positionBufferOffset\":" << draw.positionInfo.offset
     << ",\"positionBufferSize\":" << draw.positionInfo.size
     << ",\"positionStorageGeneration\":" << positionGeneration
     << ",\"indexType\":" << static_cast<uint32_t>(draw.indexType)
     << ",\"indexBufferOffset\":" << draw.indexInfo.offset
     << ",\"indexBufferSize\":" << draw.indexInfo.size
     << ",\"indexStorageGeneration\":" << indexGeneration
     << ",\"blendStorageGeneration\":" << blendGeneration
     << ",\"uvStorageGeneration\":" << uvGeneration
     << ",\"vertexBlendEnabled\":"
     << (draw.vertexBlendEnabled ? 1 : 0)
     << ",\"vertexBlendIndexed\":"
     << (draw.vertexBlendIndexed ? 1 : 0)
     << ",\"vertexBlendCount\":"
     << static_cast<uint32_t>(draw.vertexBlendCount)
     << ",\"paletteIndex\":" << draw.paletteIndex
     << ",\"alphaTestEnabled\":" << (draw.alphaTestEnabled ? 1 : 0)
     << ",\"alphaBlendEnabled\":" << (draw.alphaBlendEnabled ? 1 : 0)
     << ",\"additiveBlend\":" << (draw.additiveBlend ? 1 : 0)
     << ",\"alphaRef\":" << draw.alphaRef
     << ",\"uvStride\":" << draw.uvStride
     << ",\"uvOffset\":" << draw.uvOffset
     << ",\"uvFormat\":" << static_cast<uint32_t>(draw.uvFormat)
     << ",\"uvBinding\":" << draw.uvBinding
     << ",\"diffuseSamplerIndex\":" << draw.diffuseSamplerIndex
     << ",\"alphaPayloadComplete\":"
     << (draw.alphaPayloadComplete ? 1 : 0)
     << ",\"alphaMetadataFrameSerial\":"
     << draw.alphaMetadataFrameSerial
     << ",\"metadataBlockerReason\":"
     << static_cast<uint32_t>(draw.shadowMetadataBlockerReason)
     << ",\"partLifecycleState\":"
     << static_cast<uint32_t>(draw.shadowPartLifecycleState)
     << ",\"boundsCenter\":[" << draw.boundsCenter.x << ','
     << draw.boundsCenter.y << ',' << draw.boundsCenter.z << ','
     << draw.boundsCenter.w << ']'
     << ",\"boundsRadius\":" << draw.boundsRadius
     << ",\"worldTranslation\":[" << draw.worldMatrix[3].x << ','
     << draw.worldMatrix[3].y << ',' << draw.worldMatrix[3].z << ']'
     << ",\"worldMatrix\":";
  WriteMatrixJson(os, draw.worldMatrix);
  WriteHexField(os, "identityHash", identityHash);
  WriteHexField(os, "backingHash", backingHash);
  WriteHexField(os, "contentHash", contentHash);
  WriteHexField(os, "metadataKeyHash", draw.shadowMetadataKeyHash);
  WritePtrField(os, "renderablePart", draw.shadowRenderablePart);
  WriteHexField(os, "positionSampleHash", positionSampleHash);
  WriteHexField(os, "indexSampleHash", indexSampleHash);
  WritePtrField(os, "positionStoragePtr", draw.positionStorage.ptr());
  WriteHexField(
      os, "positionBuffer",
      ShadowCasterTraceHandleBits(draw.positionInfo.buffer));
  WritePtrField(os, "indexStoragePtr", draw.indexStorage.ptr());
  WriteHexField(os, "indexBuffer",
                ShadowCasterTraceHandleBits(draw.indexInfo.buffer));
  WritePtrField(os, "blendStoragePtr", draw.blendStorage.ptr());
  WriteHexField(os, "blendBuffer",
                ShadowCasterTraceHandleBits(draw.blendInfo.buffer));
  WritePtrField(os, "uvStoragePtr", draw.uvStorage.ptr());
  WriteHexField(os, "uvBuffer",
                ShadowCasterTraceHandleBits(draw.uvInfo.buffer));
  WritePtrField(os, "diffuseTexturePtr", draw.diffuseTexture.ptr());
  WritePtrField(os, "diffuseSamplerPtr", draw.diffuseSampler.ptr());
  os << ",\"validationFlags\":"
     << ShadowCasterTraceValidationFlags(draw)
     << ",\"gpuSkin\":{"
     << "\"valid\":" << (draw.gpuSkinInput.valid ? 1 : 0)
     << ",\"irreversible\":"
     << (draw.gpuSkinInput.irreversible ? 1 : 0)
     << ",\"storageLeaseId\":" << draw.gpuSkinInput.storageLeaseId
     << ",\"storagePageGeneration\":"
     << draw.gpuSkinInput.storagePageGeneration
     << ",\"storagePageId\":" << draw.gpuSkinInput.storagePageId
     << ",\"mapEpoch\":" << draw.gpuSkinInput.desc.mapEpoch
     << ",\"deviceEpoch\":" << draw.gpuSkinInput.desc.deviceEpoch
     << ",\"frameTag\":" << draw.gpuSkinInput.desc.frameTag
     << ",\"token\":" << draw.gpuSkinInput.desc.token
     << ",\"dispatchEpoch\":" << draw.gpuSkinInput.desc.dispatchEpoch
     << ",\"uploadEpoch\":" << draw.gpuSkinInput.desc.uploadEpoch
     << ",\"staticByteOffset\":"
     << draw.gpuSkinInput.desc.staticByteOffset
     << ",\"staticByteLength\":"
     << draw.gpuSkinInput.desc.staticByteLength
     << ",\"paletteByteOffset\":"
     << draw.gpuSkinInput.desc.paletteByteOffset
     << ",\"paletteByteLength\":"
     << draw.gpuSkinInput.desc.paletteByteLength
     << ",\"vertexCount\":" << draw.gpuSkinInput.desc.vertexCount
     << ",\"paletteMatrixCount\":"
     << draw.gpuSkinInput.desc.paletteMatrixCount
     << ",\"sourceUvLayerCount\":"
     << draw.gpuSkinInput.desc.sourceUvLayerCount
     << ",\"outputFormat\":" << draw.gpuSkinInput.desc.outputFormat
     << ",\"layoutGeneration\":"
     << draw.gpuSkinInput.desc.layoutGeneration
     << ",\"consumerBits\":" << draw.gpuSkinInput.desc.consumerBits
     << "}}\n";
}

template <typename T>
uint32_t ApplyTraceRecordLimit(const std::vector<T>& records,
                               uint32_t maxRecords) {
  if (maxRecords == 0u)
    return static_cast<uint32_t>((std::min)(records.size(),
                                            size_t(0xFFFFFFFFu)));
  return static_cast<uint32_t>(
      (std::min)(records.size(), size_t(maxRecords)));
}

void WriteCadenceJson(std::ostream& os,
                      const ShadowRuntimeCadenceSample& sample) {
  os << "\"cadence\":{"
     << "\"serial\":" << sample.serial
     << ",\"frameIndex\":" << sample.frameIndex
     << ",\"sceneFrameSerial\":" << sample.sceneFrameSerial
     << ",\"selectedFrameSerial\":" << sample.selectedFrameSerial
     << ",\"reusableFrameSerial\":" << sample.reusableFrameSerial
     << ",\"sourcePublishRevision\":" << sample.sourcePublishRevision
     << ",\"targetPublishRevision\":" << sample.targetPublishRevision
     << ",\"populateReturnReason\":" << sample.populateReturnReason
     << ",\"inputDrawCount\":" << sample.inputDrawCount
     << ",\"inputSkinnedCount\":" << sample.inputSkinnedCount
     << ",\"submittedDrawCount\":" << sample.submittedDrawCount
     << ",\"submittedSkinnedCount\":" << sample.submittedSkinnedCount
     << ",\"directSubmittedRecordCount\":"
     << sample.directSubmittedRecordCount
     << ",\"directSubmittedObjectCount\":"
     << sample.directSubmittedObjectCount
     << ",\"shadowCastersCount\":" << sample.shadowCastersCount
     << ",\"replayDrawsCount\":" << sample.replayDrawsCount
     << ",\"shadowMapDrawnCasters\":" << sample.shadowMapDrawnCasters
     << ",\"shadowMapSkinnedDrawnCount\":"
     << sample.shadowMapSkinnedDrawnCount
     << ",\"receiverNeedShadowMap\":" << sample.receiverNeedShadowMap
     << ",\"receiverHasCompleteShadowMap\":"
     << sample.receiverHasCompleteShadowMap
     << ",\"receiverReuseShadowMap\":" << sample.receiverReuseShadowMap
     << ",\"shadowMapExecutedThisFrame\":"
     << sample.shadowMapExecutedThisFrame
     << ",\"receiverRunEarlyReturnReason\":"
     << sample.receiverRunEarlyReturnReason
     << ",\"receiverRunEntryFlags\":" << sample.receiverRunEntryFlags
     << ",\"receiverActiveStrengthMilli\":"
     << sample.receiverActiveStrengthMilli
     << ",\"receiverCsmCascadeCount\":" << sample.receiverCsmCascadeCount
     << ",\"receiverHoldInvalidCsm\":" << sample.receiverHoldInvalidCsm
     << ",\"receiverHoldEmptyReplay\":" << sample.receiverHoldEmptyReplay
     << ",\"receiverHoldIdentityChurn\":"
     << sample.receiverHoldIdentityChurn;
  WriteHexField(os, "dynamicPoseSignature", sample.dynamicPoseSignature);
  WriteHexField(os, "submittedIdentityHash", sample.submittedIdentityHash);
  WriteHexField(os, "lastSubmittedPaletteHash",
                sample.lastSubmittedPaletteHash);
  WriteHexField(os, "lastSubmittedGroupHash", sample.lastSubmittedGroupHash);
  os << ",\"currentDrawPublishReadyCount\":"
     << sample.currentDrawPublishReadyCount
     << ",\"currentDrawQueryHitCount\":" << sample.currentDrawQueryHitCount
     << ",\"currentDrawLastRenderFrameIndex\":"
     << sample.currentDrawLastRenderFrameIndex
     << ",\"currentDrawLastFrameTag\":" << sample.currentDrawLastFrameTag
     << ",\"submitPaletteContentAgeSampleCount\":"
     << sample.submitPaletteContentAgeSampleCount
     << ",\"submitPaletteContentAgeLag3PlusCount\":"
     << sample.submitPaletteContentAgeLag3PlusCount
     << ",\"shadowMatrixUploadSerial\":"
     << sample.shadowMatrixUploadSerial
     << ",\"shadowMatrixBufferOffset\":"
     << sample.shadowMatrixBufferOffset
     << ",\"shadowMatrixBufferSize\":" << sample.shadowMatrixBufferSize
     << ",\"shadowMatrixBufferGpuAddress\":"
     << sample.shadowMatrixBufferGpuAddress
     << ",\"shadowMapRenderSerial\":" << sample.shadowMapRenderSerial;
  WriteHexField(os, "shadowMatrixSceneKey", sample.shadowMatrixSceneKey);
  WriteHexField(os, "shadowMatrixBufferObjectPtr",
                sample.shadowMatrixBufferObjectPtr);
  WriteHexField(os, "shadowMapImagePtr", sample.shadowMapImagePtr);
  WriteHexField(os, "shadowMapSampleViewPtr",
                sample.shadowMapSampleViewPtr);
  WriteHexField(os, "shadowCurrentImagePtr", sample.shadowCurrentImagePtr);
  WriteHexField(os, "shadowCurrentViewPtr", sample.shadowCurrentViewPtr);
  WriteHexField(os, "shadowHistoryReadImagePtr",
                sample.shadowHistoryReadImagePtr);
  WriteHexField(os, "shadowHistoryReadViewPtr",
                sample.shadowHistoryReadViewPtr);
  WriteHexField(os, "shadowHistoryWriteImagePtr",
                sample.shadowHistoryWriteImagePtr);
  WriteHexField(os, "shadowHistoryWriteViewPtr",
                sample.shadowHistoryWriteViewPtr);
  os << ",\"shadowVisibilityExecutedThisFrame\":"
     << sample.shadowVisibilityExecutedThisFrame
     << ",\"receiverDrawExecutedThisFrame\":"
     << sample.receiverDrawExecutedThisFrame
     << ",\"shadowTaaMode\":" << sample.shadowTaaMode
     << ",\"shadowHistoryValidBefore\":"
     << sample.shadowHistoryValidBefore
     << ",\"shadowHistoryValidAfter\":"
     << sample.shadowHistoryValidAfter
     << ",\"shadowHistoryReadIndex\":" << sample.shadowHistoryReadIndex
     << ",\"shadowHistoryWriteIndex\":" << sample.shadowHistoryWriteIndex
     << ",\"shadowHistoryAdvancedThisFrame\":"
     << sample.shadowHistoryAdvancedThisFrame
     << ",\"shadowHistoryAdvanceSkippedIncomplete\":"
     << sample.shadowHistoryAdvanceSkippedIncomplete
     << ",\"shadowReceiverSampleSource\":"
     << sample.shadowReceiverSampleSource << '}';
}

void WriteTraceFrameEvent(
    std::ostream& os,
    const ShadowPoseFullTraceConfigSnapshot& config,
    const ShadowRuntimeCadenceSample& sample,
    const War3ShadowCaptureStats& stats,
    const CurrentDrawContractDiagnosticsSummary& currentDraw,
    size_t poseRecordCount,
    size_t shadowObjectRecordCount,
    size_t currentDrawRecordCount) {
  const double elapsedMs =
      g_shadowPoseFullTraceStart == std::chrono::steady_clock::time_point{}
          ? 0.0
          : std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                g_shadowPoseFullTraceStart)
                .count();

  os << std::setprecision(9);
  os << "{\"type\":\"shadowPoseFullTraceFrame\""
     << ",\"version\":1"
     << ",\"epoch\":" << config.epoch
     << ",\"elapsedMs\":" << elapsedMs
     << ",\"statsSizeBytes\":" << sizeof(War3ShadowCaptureStats)
     << ",\"currentDrawSummarySizeBytes\":"
     << sizeof(CurrentDrawContractDiagnosticsSummary)
     << ",\"poseRegistryFrame\":"
     << model::PoseRegistry::instance().frameNumber()
     << ",\"modelInstanceRegistryFrame\":"
     << model::ModelInstanceRegistry::instance().frameNumber()
     << ",\"shadowObjectRegistryFrame\":"
     << ShadowObjectRegistry::instance().frameNumber()
     << ",\"poseRecordCount\":" << poseRecordCount
     << ",\"shadowObjectRecordCount\":" << shadowObjectRecordCount
     << ",\"currentDrawRecordCount\":" << currentDrawRecordCount << ',';
  WriteCadenceJson(os, sample);
  os << ",\"keyStats\":{"
     << "\"semanticSceneSubmitted\":" << stats.semanticSceneSubmitted
     << ",\"semanticSceneSubmittedSkinned\":"
     << stats.semanticSceneSubmittedSkinned
     << ",\"semanticSceneDirectPartLeaseRestoredCount\":"
     << stats.semanticSceneDirectPartLeaseRestoredCount
     << ",\"semanticSceneDirectPartLeaseUpdatedCount\":"
     << stats.semanticSceneDirectPartLeaseUpdatedCount
     << ",\"semanticSceneDirectPartLeaseExpiredCount\":"
     << stats.semanticSceneDirectPartLeaseExpiredCount
     << ",\"semanticSceneDirectPartLeaseRejectedDynamicMeshCount\":"
     << stats.semanticSceneDirectPartLeaseRejectedDynamicMeshCount
     << ",\"semanticSceneDirectPartLeaseRejectedNotSelfContainedCount\":"
     << stats.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount
     << ",\"semanticSceneDirectPartLeaseRejectedUnsafeBackingCount\":"
     << stats.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount
     << ",\"semanticSceneDirectPartLeaseRejectedSelfRenewCount\":"
     << stats.semanticSceneDirectPartLeaseRejectedSelfRenewCount
     << ",\"semanticSceneDirectPartLeaseBudgetLimitCount\":"
     << stats.semanticSceneDirectPartLeaseBudgetLimitCount
     << ",\"semanticSceneShadowManifestPartLeaseRestoredCount\":"
     << stats.semanticSceneShadowManifestPartLeaseRestoredCount
     << ",\"semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount\":"
     << stats.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount
     << ",\"semanticSceneShadowManifestPartLeaseExpiredCount\":"
     << stats.semanticSceneShadowManifestPartLeaseExpiredCount
     << ",\"semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount\":"
     << stats.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount
     << ",\"semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount\":"
     << stats.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount
     << ",\"semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount\":"
     << stats.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount
     << ",\"semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount\":"
     << stats
            .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount
     << ",\"semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount\":"
     << stats.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount
     << ",\"semanticSceneShadowManifestPartLeaseBudgetLimitCount\":"
     << stats.semanticSceneShadowManifestPartLeaseBudgetLimitCount
     << ",\"semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount\":"
     << stats.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount
     << ",\"semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount\":"
     << stats.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount
     << ",\"semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount\":"
     << stats.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount
     << ",\"semanticSceneShadowManifestObjectCount\":"
     << stats.semanticSceneShadowManifestObjectCount
     << ",\"semanticSceneShadowManifestPartCount\":"
     << stats.semanticSceneShadowManifestPartCount
     << ",\"semanticSceneShadowManifestFreshPartCount\":"
     << stats.semanticSceneShadowManifestFreshPartCount
     << ",\"semanticSceneShadowManifestPoseStalePartCount\":"
     << stats.semanticSceneShadowManifestPoseStalePartCount
     << ",\"semanticSceneShadowManifestSliceStalePartCount\":"
     << stats.semanticSceneShadowManifestSliceStalePartCount
     << ",\"semanticSceneSubmittedSkinnedPaletteSourceNoneCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteSourceNoneCount
     << ",\"semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount
     << ",\"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount\":"
     << stats
            .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount
     << ",\"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount\":"
     << stats
            .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount
     << ",\"semanticSceneSubmittedSkinnedPaletteHashChurnCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteHashChurnCount
     << ",\"semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount
     << ",\"semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount
     << ",\"semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount
     << ",\"semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount\":"
     << stats.semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount
     << ",\"semanticSceneShadowManifestPartLeasePaletteRefreshHitCount\":"
     << stats.semanticSceneShadowManifestPartLeasePaletteRefreshHitCount
     << ",\"semanticSceneShadowManifestPartLeasePaletteRefreshMissCount\":"
     << stats.semanticSceneShadowManifestPartLeasePaletteRefreshMissCount
     << ",\"semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount\":"
     << stats
            .semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount
     << ",\"semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount\":"
     << stats
            .semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount
     << ",\"semanticSceneShadowManifestLeaseExpiredBackingOnlyCount\":"
     << stats.semanticSceneShadowManifestLeaseExpiredBackingOnlyCount
     << ",\"semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount\":"
     << stats
            .semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount
     << ",\"semanticSceneShadowManifestMissingRequiredPartCount\":"
     << stats.semanticSceneShadowManifestMissingRequiredPartCount
     << ",\"semanticSceneShadowManifestGraceUsedCount\":"
     << stats.semanticSceneShadowManifestGraceUsedCount
     << ",\"semanticSceneShadowManifestTombstoneRetiredCount\":"
     << stats.semanticSceneShadowManifestTombstoneRetiredCount
     << ",\"semanticSceneShadowManifestObjectCoreCompleteCount\":"
     << stats.semanticSceneShadowManifestObjectCoreCompleteCount
     << ",\"semanticSceneShadowManifestObjectCoreIncompleteSkipCount\":"
     << stats.semanticSceneShadowManifestObjectCoreIncompleteSkipCount
     << ",\"semanticSceneShadowManifestPartOmittedIncompleteCoreCount\":"
     << stats.semanticSceneShadowManifestPartOmittedIncompleteCoreCount
     << ",\"semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount\":"
     << stats
            .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount
     << ",\"semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount\":"
     << stats
            .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount
     << ",\"semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount\":"
     << stats
            .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount
     << ",\"semanticSceneShadowManifestObjectCoreEpochMissingPartCount\":"
     << stats.semanticSceneShadowManifestObjectCoreEpochMissingPartCount
     << ",\"semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount\":"
     << stats
            .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount
     << ",\"submitPaletteFrameLagSampleCount\":"
     << currentDraw.submitPaletteFrameLagSampleCount
     << ",\"submitPaletteFrameLag3To5Count\":"
     << currentDraw.submitPaletteFrameLag3To5Count
     << ",\"submitPaletteFrameLag6PlusCount\":"
     << currentDraw.submitPaletteFrameLag6PlusCount
     << ",\"submitPaletteFrameLagMax\":"
     << currentDraw.submitPaletteFrameLagMax
     << ",\"submitPaletteContentAgeSampleCount\":"
     << currentDraw.submitPaletteContentAgeSampleCount
     << ",\"submitPaletteContentAgeLag3To5Count\":"
     << currentDraw.submitPaletteContentAgeLag3To5Count
     << ",\"submitPaletteContentAgeLag6PlusCount\":"
     << currentDraw.submitPaletteContentAgeLag6PlusCount
     << ",\"submitPaletteContentAgeMax\":"
     << currentDraw.submitPaletteContentAgeMax
     // Phase 7.49：per-publish provenance probe
     << ",\"publishCallCumulative\":"
     << currentDraw.publishCallCumulative
     << ",\"publishTrustedHitCumulative\":"
     << currentDraw.publishTrustedHitCumulative
     << ",\"publishRawFallbackCumulative\":"
     << currentDraw.publishRawFallbackCumulative
     << ",\"publishRejectedNoTrustedCumulative\":"
     << currentDraw.publishRejectedNoTrustedCumulative
     << ",\"publishRecordFrameTagSameRunMax\":"
     << currentDraw.publishRecordFrameTagSameRunMax
     << ",\"publishRecordFrameTagCurrentSameRun\":"
     << currentDraw.publishRecordFrameTagCurrentSameRun
     << ",\"publishRecordFrameTagLast\":"
     << currentDraw.publishRecordFrameTagLast
     << ",\"publishLiveGamePaletteFrameTagLast\":"
     << currentDraw.publishLiveGamePaletteFrameTagLast
     << ",\"publishLiveGamePaletteFrameTagMin\":"
     << currentDraw.publishLiveGamePaletteFrameTagMin
     << ",\"publishLiveGamePaletteFrameTagMax\":"
     << currentDraw.publishLiveGamePaletteFrameTagMax
     << ",\"publishRecordFrameTagMin\":"
     << currentDraw.publishRecordFrameTagMin
     << ",\"publishRecordFrameTagMax\":"
     << currentDraw.publishRecordFrameTagMax
     << ",\"publishRecordFrameTagBehindLiveMaxDelta\":"
     << currentDraw.publishRecordFrameTagBehindLiveMaxDelta
     << ",\"publishRecordFrameTagEqualsLiveCount\":"
     << currentDraw.publishRecordFrameTagEqualsLiveCount
     << ",\"publishRecordFrameTagBehindLiveCount\":"
     << currentDraw.publishRecordFrameTagBehindLiveCount
     << ",\"publishRecordFrameTagAheadLiveCount\":"
     << currentDraw.publishRecordFrameTagAheadLiveCount;
  // Phase 7.48：per-frame submitted skinned palette 聚合诊断。
  // 核心判据：CombinedHash 跨多帧是否也不变 => 真冻结；否则是指标错觉。
  os << ",\"semanticSceneSubmittedSkinnedPaletteDistinctSampleCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteDistinctSampleCount
     << ",\"semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax\":"
     << stats.semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax
     << ",\"semanticSceneSubmittedSkinnedPaletteZeroHashCount\":"
     << stats.semanticSceneSubmittedSkinnedPaletteZeroHashCount;
  WriteHexField(os, "semanticSceneSubmittedSkinnedPaletteCombinedHash",
                stats.semanticSceneSubmittedSkinnedPaletteCombinedHash);
  WriteHexField(os, "semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash",
                stats.semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash);
  // Phase 7.47 dt gate probe：查询 probe counter 追加到 keyStats。
  // 这组数据让 trace 可以直接对齐"dt=0 帧数" vs "writer 静默帧数"。
  const auto probeSummary = model::QueryRuntimeOverrideOutputProbeSummary();
  os << ",\"spriteUberPreRenderTotalCount\":"
     << probeSummary.spriteUberPreRenderTotalCount
     << ",\"spriteUberPreRenderDtZeroCount\":"
     << probeSummary.spriteUberPreRenderDtZeroCount
     << ",\"spriteUberPreRenderDtBelowEpsilonCount\":"
     << probeSummary.spriteUberPreRenderDtBelowEpsilonCount
     << ",\"spriteUberPreRenderDtPositiveCount\":"
     << probeSummary.spriteUberPreRenderDtPositiveCount
     << ",\"spriteUberPreRenderDtNegativeCount\":"
     << probeSummary.spriteUberPreRenderDtNegativeCount
     << ",\"spriteUberPreRenderLastDtBits\":"
     << probeSummary.spriteUberPreRenderLastDtBits
     << ",\"spriteUberPreRenderLastZeroDtFrameTag\":"
     << probeSummary.spriteUberPreRenderLastZeroDtFrameTag
     << ",\"spriteUberPreRenderLastPositiveDtFrameTag\":"
     << probeSummary.spriteUberPreRenderLastPositiveDtFrameTag
     << ",\"runtimeMatrixWriteCount\":"
     << probeSummary.runtimeMatrixWriteCount
     << ",\"runtimeMatrixWriteFramesWithHitCount\":"
     << probeSummary.runtimeMatrixWriteFramesWithHitCount
     << ",\"runtimeMatrixWriteFramesEmptyCount\":"
     << probeSummary.runtimeMatrixWriteFramesEmptyCount
     << ",\"runtimeGroupPaletteWrapperCallCount\":"
     << probeSummary.runtimeGroupPaletteWrapperCallCount
     << ",\"runtimeGroupPaletteWrapperFramesWithHitCount\":"
     << probeSummary.runtimeGroupPaletteWrapperFramesWithHitCount
     << ",\"runtimeGroupPaletteWrapperFramesEmptyCount\":"
     << probeSummary.runtimeGroupPaletteWrapperFramesEmptyCount
     << ",\"runtimeSimpleGroupPaletteCallCount\":"
     << probeSummary.runtimeSimpleGroupPaletteCallCount
     << ",\"runtimeSimpleGroupPaletteFramesWithHitCount\":"
     << probeSummary.runtimeSimpleGroupPaletteFramesWithHitCount
     << ",\"runtimeSimpleGroupPaletteFramesEmptyCount\":"
     << probeSummary.runtimeSimpleGroupPaletteFramesEmptyCount
     // Phase 7.52：把 renderablePart palette snapshot 的捕获/查询计数输出到
     // full trace keyStats，直接用来判断修复是否生效。
     // FROZEN 段里如果 CapturedCount 仍然按每帧 ~N 增长，就说明 bindings
     // 表正在被 arena 最新 bytes 刷新，不再吃旧 snapshot。
     << ",\"renderablePartPaletteSnapshotCapturedCount\":"
     << probeSummary.renderablePartPaletteSnapshotCapturedCount
     << ",\"renderablePartPaletteSnapshotTooLargeCount\":"
     << probeSummary.renderablePartPaletteSnapshotTooLargeCount
     << ",\"renderablePartPaletteSnapshotUnreadableCount\":"
     << probeSummary.renderablePartPaletteSnapshotUnreadableCount
     << ",\"renderablePartPaletteSnapshotQueryHitCount\":"
     << probeSummary.renderablePartPaletteSnapshotQueryHitCount
     << ",\"renderablePartPaletteSnapshotQueryMissCount\":"
     << probeSummary.renderablePartPaletteSnapshotQueryMissCount
     << ",\"renderablePartPaletteBindingQueryHitCount\":"
     << probeSummary.renderablePartPaletteBindingQueryHitCount
     << ",\"renderablePartPaletteBindingQueryMissCount\":"
     << probeSummary.renderablePartPaletteBindingQueryMissCount
     // Phase 7.52 续：submit-live-rebuild counter 纳入 keyStats。
     // rebuild Hit 应随 Phase 7.52 snapshot 刷新率上升而上升；
     // Applied 应约等于 Hit（Phase 7.51 EveryFrame=1 默认）。
     << ",\"submitLiveRebuildAttemptCount\":"
     << currentDraw.submitLiveRebuildAttemptCount
     << ",\"submitLiveRebuildHitCount\":"
     << currentDraw.submitLiveRebuildHitCount
     << ",\"submitLiveRebuildMissCount\":"
     << currentDraw.submitLiveRebuildMissCount
     << ",\"submitLiveRebuildAppliedCount\":"
     << currentDraw.submitLiveRebuildAppliedCount;
  // Phase 7.53 producer-side hash 探针：
  // 直接看 0x12E600 / 0x12FDC0 producer hook 在最后一次写入时的 matrix hash。
  // 如果这两个值在 CombinedHash FROZEN 窗口里也一起冻结，证明 War3 引擎本身
  // 就在 8 帧周期内不更新骨骼 pose（producer 早退或 dt gate）；
  // 如果它们每帧都变但 CombinedHash 还冻结，那是我们读取链路的 bug。
  WriteHexField(os, "runtimeMatrixWriteLastMatrixHash",
                probeSummary.runtimeMatrixWriteLastMatrixHash);
  WriteHexField(os, "runtimeMatrixRangeCopyLastMatrixHash",
                probeSummary.runtimeMatrixRangeCopyLastMatrixHash);
  os << ",\"runtimeMatrixWriteLastMatrixCount\":"
     << probeSummary.runtimeMatrixWriteLastMatrixCount
     << ",\"runtimeMatrixRangeCopyLastMatrixCount\":"
     << probeSummary.runtimeMatrixRangeCopyLastMatrixCount;
  // Phase 7.55：draw-time D3D palette 聚合 hash。
  // 如果 CombinedHash 冻结但这个值每帧变 → draw-time D3D palette 是 fresh 的。
  WriteHexField(os, "drawTimeD3DPaletteCombinedHash",
                stats.semanticSceneDrawTimePoseCombinedHash);
  os << ",\"drawTimeD3DPaletteCombinedSampleCount\":"
     << stats.semanticSceneDrawTimePoseCombinedSampleCount
     << ",\"drawTimeVBCacheCaptureCount\":"
     << stats.drawTimeVBCacheCaptureCount
     << ",\"drawTimeVBCacheConsumeHitCount\":"
     << stats.drawTimeVBCacheConsumeHitCount
     << ",\"drawTimeVBCacheConsumeMissCount\":"
     << stats.drawTimeVBCacheConsumeMissCount
     << ",\"drawTimeSemanticProducerVisibleCandidateCount\":"
     << stats.drawTimeSemanticProducerVisibleCandidateCount
     << ",\"drawTimeSemanticProducerFreshEntryCount\":"
     << stats.drawTimeSemanticProducerFreshEntryCount
     << ",\"drawTimeSemanticProducerClaimedCount\":"
     << stats.drawTimeSemanticProducerClaimedCount
     << ",\"drawTimeSemanticProducerSubmittedCount\":"
     << stats.drawTimeSemanticProducerSubmittedCount
     << ",\"drawTimeSemanticProducerMissNoFreshEntryCount\":"
     << stats.drawTimeSemanticProducerMissNoFreshEntryCount
     << ",\"drawTimeSemanticProducerFallbackCurrentDrawCount\":"
     << stats.drawTimeSemanticProducerFallbackCurrentDrawCount
     << ",\"drawTimeSemanticProducerOwnedDirectGroupedSkipCount\":"
     << stats.drawTimeSemanticProducerOwnedDirectGroupedSkipCount
     << ",\"drawTimeSemanticProducerLifecycleMergedCount\":"
     << stats.drawTimeSemanticProducerLifecycleMergedCount
     << ",\"semanticSceneRejectedPathBlockerCount\":"
     << stats.semanticSceneRejectedPathBlockerCount
     << ",\"semanticSceneRejectedPathBlockerEarlyBypassCount\":"
     << stats.semanticSceneRejectedPathBlockerEarlyBypassCount
     << ",\"semanticSceneRejectedPathBlockerEligibilityGateCount\":"
     << stats.semanticSceneRejectedPathBlockerEligibilityGateCount
     << ",\"semanticSceneRejectedPathBlockerAppendEntryCount\":"
     << stats.semanticSceneRejectedPathBlockerAppendEntryCount
     << ",\"semanticSceneRejectedPathBlockerAppendEntryByJHandleCount\":"
     << stats.semanticSceneRejectedPathBlockerAppendEntryByJHandleCount
     << ",\"semanticSceneRejectedPathBlockerAppendVbBlendCount\":"
     << stats.semanticSceneRejectedPathBlockerAppendVbBlendCount
     << ",\"semanticSceneRejectedPathBlockerFastAppendCount\":"
     << stats.semanticSceneRejectedPathBlockerFastAppendCount
     << ",\"semanticSceneRejectedPathBlockerDirectGroupedCount\":"
     << stats.semanticSceneRejectedPathBlockerDirectGroupedCount
     << ",\"semanticSceneRejectedPathBlockerProducerCount\":"
     << stats.semanticSceneRejectedPathBlockerProducerCount
     << ",\"semanticSceneRejectedPathBlockerStaticSupplementCount\":"
     << stats.semanticSceneRejectedPathBlockerStaticSupplementCount
     << ",\"semanticSceneRejectedPathBlockerLegacyCaptureCount\":"
     << stats.semanticSceneRejectedPathBlockerLegacyCaptureCount
     << ",\"semanticSceneDirectDrawTimePrebuildBypassAttemptCount\":"
     << stats.semanticSceneDirectDrawTimePrebuildBypassAttemptCount
     << ",\"semanticSceneDirectDrawTimePrebuildBypassHitCount\":"
     << stats.semanticSceneDirectDrawTimePrebuildBypassHitCount
     << ",\"semanticSceneFastAppendBoundsPoseAvailableCount\":"
     << stats.semanticSceneFastAppendBoundsPoseAvailableCount
     << ",\"semanticSceneFastAppendBoundsSceneReadSuccessCount\":"
     << stats.semanticSceneFastAppendBoundsSceneReadSuccessCount
     << ",\"semanticSceneFastAppendBoundsPoseDeltaLe1Count\":"
     << stats.semanticSceneFastAppendBoundsPoseDeltaLe1Count
     << ",\"semanticSceneFastAppendBoundsPoseDeltaLe4Count\":"
     << stats.semanticSceneFastAppendBoundsPoseDeltaLe4Count
     << ",\"semanticSceneFastAppendBoundsPoseDeltaLe16Count\":"
     << stats.semanticSceneFastAppendBoundsPoseDeltaLe16Count
     << ",\"semanticSceneFastAppendBoundsPoseDeltaGt16Count\":"
     << stats.semanticSceneFastAppendBoundsPoseDeltaGt16Count
     << ",\"semanticSceneFastAppendBoundsPoseDeltaMaxMilli\":"
     << stats.semanticSceneFastAppendBoundsPoseDeltaMaxMilli
     << ",\"drawTimeVBCacheTotalEntered\":"
     << stats.drawTimeVBCacheTotalEntered
     << ",\"drawTimeVBCacheRejectNoRenderablePart\":"
     << stats.drawTimeVBCacheRejectNoRenderablePart
     << ",\"drawTimeVBCacheRejectNoLayerContext\":"
     << stats.drawTimeVBCacheRejectNoLayerContext
     << ",\"drawTimeVBCacheRejectNoDecl\":"
     << stats.drawTimeVBCacheRejectNoDecl
     << ",\"drawTimeVBCacheRejectNoPosition\":"
     << stats.drawTimeVBCacheRejectNoPosition
     << ",\"drawTimeVBCacheRejectInvalidStride\":"
     << stats.drawTimeVBCacheRejectInvalidStride
     << ",\"drawTimeVBCacheRejectNoSlice\":"
     << stats.drawTimeVBCacheRejectNoSlice
     << ",\"drawTimeVBCacheRejectInvalidRange\":"
     << stats.drawTimeVBCacheRejectInvalidRange
     << ",\"drawTimeVBCacheRejectInsufficientLength\":"
     << stats.drawTimeVBCacheRejectInsufficientLength
     << ",\"drawTimeVBCacheRejectNoBuffer\":"
     << stats.drawTimeVBCacheRejectNoBuffer
     << ",\"drawTimeVBCacheRejectIncompleteIndex\":"
     << stats.drawTimeVBCacheRejectIncompleteIndex
     << ",\"drawTimeVBCachePositionCopyCount\":"
     << stats.drawTimeVBCachePositionCopyCount
     << ",\"drawTimeVBCachePositionCopyBytes\":"
     << stats.drawTimeVBCachePositionCopyBytes
     << ",\"drawTimeVBCachePositionAllocCount\":"
     << stats.drawTimeVBCachePositionAllocCount
     << ",\"drawTimeVBCacheUvCopyCount\":"
     << stats.drawTimeVBCacheUvCopyCount
     << ",\"drawTimeVBCacheUvCopyBytes\":"
     << stats.drawTimeVBCacheUvCopyBytes
     << ",\"drawTimeVBCacheUvSharedPositionCount\":"
     << stats.drawTimeVBCacheUvSharedPositionCount
     << ",\"drawTimeVBCacheUvAllocCount\":"
     << stats.drawTimeVBCacheUvAllocCount
     << ",\"drawTimeVBCacheIndexCopyCount\":"
     << stats.drawTimeVBCacheIndexCopyCount
     << ",\"drawTimeVBCacheIndexCopyBytes\":"
     << stats.drawTimeVBCacheIndexCopyBytes
     << ",\"drawTimeVBCacheIndexAllocCount\":"
     << stats.drawTimeVBCacheIndexAllocCount
     << ",\"drawTimeVBCacheIndexedUnknownRangeFallbackCount\":"
     << stats.drawTimeVBCacheIndexedUnknownRangeFallbackCount
     << ",\"drawTimeVBCacheUnitCaptureCount\":"
     << stats.drawTimeVBCacheUnitCaptureCount
     << ",\"drawTimeVBCacheBuildingCaptureCount\":"
     << stats.drawTimeVBCacheBuildingCaptureCount
     << ",\"drawTimeVBCacheDestructibleCaptureCount\":"
     << stats.drawTimeVBCacheDestructibleCaptureCount
     << ",\"drawTimeVBCacheEffectCaptureCount\":"
     << stats.drawTimeVBCacheEffectCaptureCount
     << ",\"drawTimeVBCacheOtherKindCaptureCount\":"
     << stats.drawTimeVBCacheOtherKindCaptureCount
     << ",\"drawTimeVBCacheAlphaTestStateCaptureCount\":"
     << stats.drawTimeVBCacheAlphaTestStateCaptureCount
     << ",\"drawTimeVBCacheAlphaBlendStateCaptureCount\":"
     << stats.drawTimeVBCacheAlphaBlendStateCaptureCount
     << ",\"drawTimeVBCacheDiffuseTextureCaptureCount\":"
     << stats.drawTimeVBCacheDiffuseTextureCaptureCount
     << ",\"drawTimeVBCacheSameFrameDedupHit\":"
     << stats.drawTimeVBCacheSameFrameDedupHit
     << ",\"drawTimeVBCacheSameFrameDedupMiss\":"
     << stats.drawTimeVBCacheSameFrameDedupMiss
     << ",\"drawTimeVBCacheSameFrameStateRefresh\":"
     << stats.drawTimeVBCacheSameFrameStateRefresh
     << ",\"drawTimeD3DPoseAttemptCount\":"
     << stats.semanticSceneDrawTimePoseAttemptCount
     << ",\"drawTimeD3DPosePublishedCount\":"
     << stats.semanticSceneDrawTimePosePublishedCount
     << ",\"drawTimeD3DPoseRejectNoVertexBlendCount\":"
     << stats.semanticSceneDrawTimePoseRejectNoVertexBlendCount
     << ",\"drawTimeD3DPoseRejectNoContextCount\":"
     << stats.semanticSceneDrawTimePoseRejectNoContextCount
     << ",\"drawTimeD3DPoseRejectNoRuntimeModelCount\":"
     << stats.semanticSceneDrawTimePoseRejectNoRuntimeModelCount
     << ",\"drawTimeD3DPoseDedupedCount\":"
     << stats.semanticSceneDrawTimePoseDedupedCount;
  WriteHexField(os, "semanticSceneDirectLastSubmittedIdentityHash",
                stats.semanticSceneDirectLastSubmittedIdentityHash);
  WriteHexField(os, "semanticSceneDirectLastSubmittedPaletteHash",
                stats.semanticSceneDirectLastSubmittedPaletteHash);
  WriteHexField(os, "semanticSceneDirectLastSubmittedGroupHash",
                stats.semanticSceneDirectLastSubmittedGroupHash);
  WriteHexField(os, "dynamicPoseSignature", stats.dynamicPoseSignature);
  os << ",\"semanticSceneShadowMatrixUploadSerial\":"
     << stats.semanticSceneShadowMatrixUploadSerial
     << ",\"semanticSceneShadowMatrixBufferOffset\":"
     << stats.semanticSceneShadowMatrixBufferOffset
     << ",\"semanticSceneShadowMatrixBufferSize\":"
     << stats.semanticSceneShadowMatrixBufferSize
     << ",\"semanticSceneShadowMatrixBufferGpuAddress\":"
     << stats.semanticSceneShadowMatrixBufferGpuAddress
     << ",\"semanticSceneShadowMapRenderSerial\":"
     << stats.semanticSceneShadowMapRenderSerial
     << ",\"semanticSceneShadowMapPreparedDrawCount\":"
     << stats.semanticSceneShadowMapPreparedDrawCount
     << ",\"semanticSceneShadowMapAlphaTestPreparedCount\":"
     << stats.semanticSceneShadowMapAlphaTestPreparedCount
     << ",\"semanticSceneShadowMapAlphaPromotedPreparedCount\":"
     << stats.semanticSceneShadowMapAlphaPromotedPreparedCount
     << ",\"semanticSceneShadowMapDynamicPreparedCount\":"
     << stats.semanticSceneShadowMapDynamicPreparedCount
     << ",\"semanticSceneShadowMapStaticPreparedCount\":"
     << stats.semanticSceneShadowMapStaticPreparedCount
     << ",\"semanticSceneShadowMapOtherPreparedCount\":"
     << stats.semanticSceneShadowMapOtherPreparedCount
     << ",\"semanticSceneShadowMapCascade0DrawnCount\":"
     << stats.semanticSceneShadowMapCascade0DrawnCount
     << ",\"semanticSceneShadowMapCascade1DrawnCount\":"
     << stats.semanticSceneShadowMapCascade1DrawnCount
     << ",\"semanticSceneShadowMapCascade2DrawnCount\":"
     << stats.semanticSceneShadowMapCascade2DrawnCount
     << ",\"semanticSceneShadowMapCascade3DrawnCount\":"
     << stats.semanticSceneShadowMapCascade3DrawnCount
     << ",\"semanticSceneShadowMapCascade0CulledCount\":"
     << stats.semanticSceneShadowMapCascade0CulledCount
     << ",\"semanticSceneShadowMapCascade1CulledCount\":"
     << stats.semanticSceneShadowMapCascade1CulledCount
     << ",\"semanticSceneShadowMapCascade2CulledCount\":"
     << stats.semanticSceneShadowMapCascade2CulledCount
     << ",\"semanticSceneShadowMapCascade3CulledCount\":"
     << stats.semanticSceneShadowMapCascade3CulledCount;
  WriteHexField(os, "semanticSceneShadowMatrixSceneKey",
                stats.semanticSceneShadowMatrixSceneKey);
  WriteHexField(os, "semanticSceneShadowMatrixBufferObjectPtr",
                stats.semanticSceneShadowMatrixBufferObjectPtr);
  WriteHexField(os, "semanticSceneShadowMapImagePtr",
                stats.semanticSceneShadowMapImagePtr);
  WriteHexField(os, "semanticSceneShadowMapSampleViewPtr",
                stats.semanticSceneShadowMapSampleViewPtr);
  WriteHexField(os, "semanticSceneShadowCurrentImagePtr",
                stats.semanticSceneShadowCurrentImagePtr);
  WriteHexField(os, "semanticSceneShadowCurrentViewPtr",
                stats.semanticSceneShadowCurrentViewPtr);
  WriteHexField(os, "semanticSceneShadowHistoryReadImagePtr",
                stats.semanticSceneShadowHistoryReadImagePtr);
  WriteHexField(os, "semanticSceneShadowHistoryReadViewPtr",
                stats.semanticSceneShadowHistoryReadViewPtr);
  WriteHexField(os, "semanticSceneShadowHistoryWriteImagePtr",
                stats.semanticSceneShadowHistoryWriteImagePtr);
  WriteHexField(os, "semanticSceneShadowHistoryWriteViewPtr",
                stats.semanticSceneShadowHistoryWriteViewPtr);
  os << ",\"semanticSceneShadowVisibilityExecutedThisFrame\":"
     << stats.semanticSceneShadowVisibilityExecutedThisFrame
     << ",\"semanticSceneReceiverDrawExecutedThisFrame\":"
     << stats.semanticSceneReceiverDrawExecutedThisFrame
     << ",\"semanticSceneShadowTaaMode\":"
     << stats.semanticSceneShadowTaaMode
     << ",\"semanticSceneShadowHistoryValidBefore\":"
     << stats.semanticSceneShadowHistoryValidBefore
     << ",\"semanticSceneShadowHistoryValidAfter\":"
     << stats.semanticSceneShadowHistoryValidAfter
     << ",\"semanticSceneShadowHistoryReadIndex\":"
     << stats.semanticSceneShadowHistoryReadIndex
     << ",\"semanticSceneShadowHistoryWriteIndex\":"
     << stats.semanticSceneShadowHistoryWriteIndex
     << ",\"semanticSceneShadowReceiverSampleSource\":"
     << stats.semanticSceneShadowReceiverSampleSource;
  os << "},\"statsRawHex\":";
  WriteRawHexBytes(os, &stats, sizeof(stats));
  os << ",\"currentDrawRawHex\":";
  WriteRawHexBytes(os, &currentDraw, sizeof(currentDraw));
  os << "}\n";
}

void WritePoseRecordEvent(std::ostream& os, uint64_t epoch, uint64_t frameSerial,
                          uint32_t index,
                          const model::PoseRecord& record,
                          bool includeMatrixBytes) {
  os << std::setprecision(9);
  os << "{\"type\":\"shadowPoseFullTracePose\""
     << ",\"epoch\":" << epoch
     << ",\"frameSerial\":" << frameSerial
     << ",\"index\":" << index;
  WritePtrField(os, "runtimeModelPtr", record.runtimeModelPtr);
  WritePtrField(os, "sceneNode", record.sceneNode);
  WritePtrField(os, "unitPtr", record.unitPtr);
  WritePtrField(os, "spritePtr", record.spritePtr);
  os << ",\"sequenceId\":" << record.sequenceId
     << ",\"sequenceTime\":" << record.sequenceTime
     << ",\"scale\":" << record.scale
     << ",\"yaw\":" << record.yaw
     << ",\"pitch\":" << record.pitch
     << ",\"roll\":" << record.roll
     << ",\"height\":" << record.height
     << ",\"hasWorldTransform\":" << (record.hasWorldTransform ? 1 : 0)
     << ",\"worldTransform\":";
  WriteMatrixJson(os, record.worldTransform);
  os << ",\"hasSpriteFrameTransform\":"
     << (record.hasSpriteFrameTransform ? 1 : 0)
     << ",\"spriteFrameTransform\":";
  WriteMatrixJson(os, record.spriteFrameTransform);
  os << ",\"spriteFrameDt\":" << record.spriteFrameDt
     << ",\"matrixCount\":" << record.matrixCount;
  WriteHexField(os, "matrixHash", record.matrixHash);
  os << ",\"matrixPaletteSize\":" << record.matrixPalette.size()
     << ",\"lastRootPoseFrame\":" << record.lastRootPoseFrame
     << ",\"lastSpriteFramePoseFrame\":"
     << record.lastSpriteFramePoseFrame
     << ",\"lastMatrixPaletteFrame\":" << record.lastMatrixPaletteFrame
     << ",\"spriteFrameSampleCount\":" << record.spriteFrameSampleCount
     << ",\"firstSeenFrame\":" << record.firstSeenFrame
     << ",\"lastSeenFrame\":" << record.lastSeenFrame;
  if (includeMatrixBytes && !record.matrixPalette.empty()) {
    os << ",\"matrixPaletteRawHex\":";
    WriteRawHexBytes(os, record.matrixPalette.data(),
                     record.matrixPalette.size() * sizeof(Matrix4));
  }
  os << "}\n";
}

void WriteShadowObjectRecordEvent(std::ostream& os, uint64_t epoch,
                                  uint64_t frameSerial, uint32_t index,
                                  const ShadowObjectRecord& record) {
  os << std::setprecision(9);
  os << "{\"type\":\"shadowPoseFullTraceObject\""
     << ",\"epoch\":" << epoch
     << ",\"frameSerial\":" << frameSerial
     << ",\"index\":" << index;
  WritePtrField(os, "worldObjectEntry", record.worldObjectEntry);
  WritePtrField(os, "sceneNode", record.sceneNode);
  WritePtrField(os, "unitPtr", record.unitPtr);
  WritePtrField(os, "spritePtr", record.spritePtr);
  WritePtrField(os, "runtimeModelPtr", record.runtimeModelPtr);
  WritePtrField(os, "modelResourcePtr", record.modelResourcePtr);
  os << ",\"modelPath\":";
  WriteJsonEscaped(os, record.modelPath);
  os << ",\"jHandle\":" << record.jHandle
     << ",\"rawcode\":" << record.rawcode
     << ",\"kind\":" << uint32_t(record.kind);
  WriteHexField(os, "modelKey", record.modelKey);
  os << ",\"modelType\":" << record.modelType
     << ",\"modelFlags\":" << record.modelFlags
     << ",\"sequenceId\":" << record.sequenceId
     << ",\"sequenceTime\":" << record.sequenceTime
     << ",\"scale\":" << record.scale
     << ",\"yaw\":" << record.yaw
     << ",\"pitch\":" << record.pitch
     << ",\"roll\":" << record.roll
     << ",\"height\":" << record.height
     << ",\"hasWorldTransform\":" << (record.hasWorldTransform ? 1 : 0)
     << ",\"worldTransform\":";
  WriteMatrixJson(os, record.worldTransform);
  os << ",\"hasSpriteFrameTransform\":"
     << (record.hasSpriteFrameTransform ? 1 : 0)
     << ",\"spriteFrameTransform\":";
  WriteMatrixJson(os, record.spriteFrameTransform);
  os << ",\"spriteFrameDt\":" << record.spriteFrameDt
     << ",\"matrixCount\":" << record.matrixCount;
  WriteHexField(os, "matrixHash", record.matrixHash);
  os << ",\"lastRootPoseFrame\":" << record.lastRootPoseFrame
     << ",\"lastSpriteFramePoseFrame\":"
     << record.lastSpriteFramePoseFrame
     << ",\"lastMatrixPaletteFrame\":" << record.lastMatrixPaletteFrame
     << ",\"spriteFrameSampleCount\":" << record.spriteFrameSampleCount
     << ",\"firstSeenFrame\":" << record.firstSeenFrame
     << ",\"lastSeenFrame\":" << record.lastSeenFrame
     << "}\n";
}

void WriteCurrentDrawRecordEvent(std::ostream& os, uint64_t epoch,
                                 uint64_t frameSerial, uint32_t index,
                                 const CurrentDrawContractRecord& record) {
  os << "{\"type\":\"shadowPoseFullTraceCurrentDraw\""
     << ",\"epoch\":" << epoch
     << ",\"frameSerial\":" << frameSerial
     << ",\"index\":" << index
     << ",\"known\":" << (record.known ? 1 : 0);
  WritePtrField(os, "sceneNode", record.sceneNode);
  WritePtrField(os, "renderablePart", record.renderablePart);
  WritePtrField(os, "meshPayloadPtr", record.meshPayloadPtr);
  WritePtrField(os, "worldObjectEntry", record.worldObjectEntry);
  WritePtrField(os, "unitPtr", record.unitPtr);
  os << ",\"jHandle\":" << record.jHandle
     << ",\"rawcode\":" << record.rawcode
     << ",\"objectKind\":" << uint32_t(record.objectKind)
     << ",\"layerIndex\":" << record.layerIndex
     << ",\"paletteSlotIndex\":" << record.paletteSlotIndex;
  WritePtrField(os, "paletteAddress", record.paletteAddress);
  os << ",\"payloadWordF0\":" << record.payloadWordF0
     << ",\"payloadWord104\":" << record.payloadWord104
     << ",\"payloadWord108\":" << record.payloadWord108
     << ",\"payloadWord11C\":" << record.payloadWord11C
     << ",\"payloadWord48\":" << record.payloadWord48;
  WritePtrField(os, "stream1Ptr", record.stream1Ptr);
  os << ",\"stream1Stride\":" << record.stream1Stride
     << ",\"capturedPaletteCount\":" << record.capturedPaletteCount
     << ",\"frameTag\":" << record.frameTag
     << ",\"stage\":" << record.stage
     << ",\"batchTag\":" << int32_t(record.batchTag)
     << ",\"producerStage\":" << record.producerStage
     << ",\"producerGroup\":" << int32_t(record.producerGroup)
     << ",\"sourceKind\":" << uint32_t(record.sourceKind)
     << ",\"producerFreshThisFrame\":"
     << (record.producerFreshThisFrame ? 1 : 0)
     << ",\"stagePolicyRevision\":" << record.stagePolicyRevision
     << ",\"fromGrace\":" << (record.fromGrace ? 1 : 0)
     << ",\"graceAge\":" << record.graceAge
     << ",\"alphaPayloadComplete\":"
     << (record.alphaPayloadComplete ? 1 : 0)
     << ",\"visibleFrameSerial\":" << record.visibleFrameSerial
     << ",\"renderFrameIndex\":" << record.renderFrameIndex
     << ",\"captureSerial\":" << record.captureSerial
     << "}\n";
}

bool EnsureShadowPoseFullTraceOpenLocked(
    const ShadowPoseFullTraceConfigSnapshot& config) {
  if (g_shadowPoseFullTraceOpened && g_shadowPoseFullTraceStream.is_open())
    return true;

  g_shadowPoseFullTracePath = BuildShadowPoseFullTracePath();
  g_shadowPoseFullTraceStream.open(g_shadowPoseFullTracePath,
                                   std::ios::out | std::ios::trunc);
  if (!g_shadowPoseFullTraceStream.is_open()) {
    Logger::err("DXVK War3Shadow: failed to open shadow pose full trace log");
    g_shadowPoseFullTraceStoppedByLimit = true;
    return false;
  }

  g_shadowPoseFullTraceOpened = true;
  g_shadowPoseFullTraceStart = std::chrono::steady_clock::now();
  g_shadowPoseFullTraceStream
      << "{\"type\":\"shadowPoseFullTraceMeta\""
      << ",\"version\":1"
      << ",\"epoch\":" << config.epoch
      << ",\"maxSeconds\":" << config.maxSeconds
      << ",\"maxPoseRecords\":" << config.maxPoseRecords
      << ",\"maxShadowObjectRecords\":" << config.maxShadowObjectRecords
      << ",\"maxCurrentDrawRecords\":" << config.maxCurrentDrawRecords
      << ",\"maxFinalCasterRecords\":" << config.maxFinalCasterRecords
      << ",\"finalCasterSampleBytes\":"
      << config.finalCasterSampleBytes
      << ",\"includePoseRecords\":"
      << (config.includePoseRecords ? 1 : 0)
      << ",\"includeShadowObjectRecords\":"
      << (config.includeShadowObjectRecords ? 1 : 0)
      << ",\"includeCurrentDrawRecords\":"
      << (config.includeCurrentDrawRecords ? 1 : 0)
      << ",\"includeFinalCasterRecords\":"
      << (config.includeFinalCasterRecords ? 1 : 0)
      << ",\"includeMatrixBytes\":"
      << (config.includeMatrixBytes ? 1 : 0)
      << ",\"statsLayout\":\"War3ShadowCaptureStats raw hex uses the current "
         "src/d3d9/d3d9_war3_scene.h layout\""
      << ",\"statsSizeBytes\":" << sizeof(War3ShadowCaptureStats)
      << ",\"currentDrawSummarySizeBytes\":"
      << sizeof(CurrentDrawContractDiagnosticsSummary)
      << "}\n";
  Logger::info("DXVK War3Shadow: shadow pose full trace started: " +
               g_shadowPoseFullTracePath);
  return true;
}

void MaybeWriteShadowPoseFullTrace(
    const ShadowRuntimeCadenceSample& sample,
    const War3ShadowCaptureStats& stats,
    const CurrentDrawContractDiagnosticsSummary& currentDraw) {
  ShadowPoseFullTraceConfigSnapshot config = {};
  {
    std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
    config = ShadowPoseFullTraceConfigLocked();
    if (!config.enabled)
      return;
    if (ShadowPoseFullTraceDeadlineReachedLocked()) {
      g_shadowPoseFullTraceStoppedByLimit = true;
      CloseShadowPoseFullTraceLocked();
      Logger::info("DXVK War3Shadow: shadow pose full trace stopped by "
                   "duration limit");
      return;
    }
  }

  std::vector<model::PoseRecord> poseRecords;
  std::vector<ShadowObjectRecord> shadowObjectRecords;
  std::vector<CurrentDrawContractRecord> currentDrawRecords;

  if (config.includePoseRecords)
    poseRecords = model::PoseRegistry::instance().snapshot();
  if (config.includeShadowObjectRecords)
    shadowObjectRecords = ShadowObjectRegistry::instance().snapshot();
  if (config.includeCurrentDrawRecords) {
    CurrentDrawContractSnapshotOptions options = {};
    options.minVisibleFrameSerial = 0u;
    options.readyOnly = false;
    options.maxRecords = config.maxCurrentDrawRecords;
    options.unitsOnly = false;
    options.pruneOlderThanMinVisibleFrame = false;
    currentDrawRecords = SnapshotPublishedCurrentDrawContracts(options);
  }

  const uint32_t poseLimit =
      ApplyTraceRecordLimit(poseRecords, config.maxPoseRecords);
  const uint32_t objectLimit = ApplyTraceRecordLimit(
      shadowObjectRecords, config.maxShadowObjectRecords);
  const uint32_t currentDrawLimit = ApplyTraceRecordLimit(
      currentDrawRecords, config.maxCurrentDrawRecords);

  std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
  config = ShadowPoseFullTraceConfigLocked();
  if (!config.enabled)
    return;
  if (ShadowPoseFullTraceDeadlineReachedLocked()) {
    g_shadowPoseFullTraceStoppedByLimit = true;
    CloseShadowPoseFullTraceLocked();
    Logger::info("DXVK War3Shadow: shadow pose full trace stopped by "
                 "duration limit");
    return;
  }
  if (!EnsureShadowPoseFullTraceOpenLocked(config))
    return;

  WriteTraceFrameEvent(g_shadowPoseFullTraceStream, config, sample, stats,
                       currentDraw, poseRecords.size(),
                       shadowObjectRecords.size(), currentDrawRecords.size());
  ++g_shadowPoseFullTraceFrameEventsWritten;

  for (uint32_t i = 0u; i < poseLimit; ++i) {
    WritePoseRecordEvent(g_shadowPoseFullTraceStream, config.epoch,
                         sample.serial, i, poseRecords[i],
                         config.includeMatrixBytes);
    ++g_shadowPoseFullTraceRecordEventsWritten;
  }
  for (uint32_t i = 0u; i < objectLimit; ++i) {
    WriteShadowObjectRecordEvent(g_shadowPoseFullTraceStream, config.epoch,
                                 sample.serial, i, shadowObjectRecords[i]);
    ++g_shadowPoseFullTraceRecordEventsWritten;
  }
  for (uint32_t i = 0u; i < currentDrawLimit; ++i) {
    WriteCurrentDrawRecordEvent(g_shadowPoseFullTraceStream, config.epoch,
                                sample.serial, i, currentDrawRecords[i]);
    ++g_shadowPoseFullTraceRecordEventsWritten;
  }
  g_shadowPoseFullTraceStream.flush();
}

uint64_t CurrentRuntimeBridgeFrame() {
  if (model::IsPoseHookEnabled())
    return model::PoseRegistry::instance().frameNumber();
  return model::ModelInstanceRegistry::instance().frameNumber();
}

uint64_t CountSkinnedSubmissionDraws(
    const shadow::ShadowSubmissionFrame* frame) {
  if (frame == nullptr)
    return 0u;

  uint64_t count = 0u;
  for (const auto& draw : frame->draws) {
    if (draw.path == shadow::ShadowDrawPath::Skinned)
      ++count;
  }
  return count;
}

void RequestShadowBridgeRepairBurst(uint64_t nowFrame) {
  if (nowFrame == 0u)
    return;

  const uint64_t activeUntil =
      g_shadowBridgeRepairUntilFrame.load(std::memory_order_relaxed);
  if (nowFrame <= activeUntil)
    return;

  const uint64_t cooldownUntil =
      g_shadowBridgeRepairCooldownUntilFrame.load(std::memory_order_relaxed);
  if (nowFrame < cooldownUntil)
    return;

  g_shadowBridgeRepairCooldownUntilFrame.store(
      nowFrame + kShadowBridgeRepairCooldownFrames,
      std::memory_order_relaxed);
  g_shadowBridgeRepairUntilFrame.store(nowFrame + kShadowBridgeRepairBurstFrames,
                                       std::memory_order_relaxed);
}

bool IsRegistryFrameFresh(uint64_t sampleFrame, uint64_t registryFrame) {
  return sampleFrame != 0 && sampleFrame + 1 >= registryFrame;
}

bool HasRuntimeOwnerIdentity(const model::ModelInstanceRegistry& instanceRegistry,
                             void* runtimeModelPtr) {
  if (runtimeModelPtr == nullptr)
    return false;

  model::ModelInstanceRecord instanceRecord = {};
  if (instanceRegistry.findOwnerByRuntimeModel(runtimeModelPtr, instanceRecord)) {
    return instanceRecord.worldObjectEntry != nullptr ||
           instanceRecord.sceneNode != nullptr || instanceRecord.unitPtr != nullptr ||
           instanceRecord.jHandle != 0u || instanceRecord.rawcode != 0u;
  }
  if (instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord)) {
    return instanceRecord.worldObjectEntry != nullptr ||
           instanceRecord.sceneNode != nullptr || instanceRecord.unitPtr != nullptr ||
           instanceRecord.jHandle != 0u || instanceRecord.rawcode != 0u;
  }
  return false;
}

bool TryGetRuntimeCreateCallerRva(
    const model::ModelInstanceRegistry& instanceRegistry, void* runtimeModelPtr,
    uint32_t& outCallerRva) {
  outCallerRva = 0u;
  if (runtimeModelPtr == nullptr)
    return false;

  model::ModelInstanceRecord instanceRecord = {};
  if (!instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord))
    return false;

  outCallerRva = instanceRecord.runtimeCreatorCallerRva;
  return outCallerRva != 0u ||
         instanceRecord.runtimeCreatorModelDataPtr != nullptr;
}

bool TryGetRuntimeResolveProvenance(
    const model::ModelInstanceRegistry& instanceRegistry, void* runtimeModelPtr,
    void*& outCreateHandlePtr, uint32_t& outResolveCallerRva) {
  outCreateHandlePtr = nullptr;
  outResolveCallerRva = 0u;
  if (runtimeModelPtr == nullptr)
    return false;

  model::ModelInstanceRecord instanceRecord = {};
  if (!instanceRegistry.findByRuntimeModel(runtimeModelPtr, instanceRecord))
    return false;

  outCreateHandlePtr = instanceRecord.runtimeCreatorHandlePtr;
  outResolveCallerRva = instanceRecord.runtimeResolveCallerRva;
  return outCreateHandlePtr != nullptr || outResolveCallerRva != 0u;
}

bool TryGetRuntimeRecordSnapshot(
    const model::ModelInstanceRegistry& instanceRegistry, void* runtimeModelPtr,
    model::ModelInstanceRecord& outRecord) {
  outRecord = {};
  if (runtimeModelPtr == nullptr)
    return false;
  return instanceRegistry.findByRuntimeModel(runtimeModelPtr, outRecord);
}

bool TryGetRuntimeModelResourceSnapshot(void* runtimeModelPtr,
                                        model::ModelResourceRecord& outRecord) {
  outRecord = {};
  if (runtimeModelPtr == nullptr)
    return false;
  if (model::ModelRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                          outRecord)) {
    return true;
  }

  void* modelResourcePtr = nullptr;
  uint64_t modelKey = 0u;
  model::ShadowModelResourceRecord resourceRecord = {};
  auto& resourceCache = model::ShadowModelResourceCache::instance();
  if (resourceCache.findRuntimeModelResource(runtimeModelPtr, resourceRecord)) {
    modelResourcePtr = resourceRecord.modelResourcePtr;
    modelKey = resourceRecord.modelKey;
  } else {
    void* ownedModelDataHandle = nullptr;
    if (dxvk::war3::SafeReadPtrFast(
            runtimeModelPtr, dxvk::war3::CModelOffsets::OwnedModelDataHandle,
            ownedModelDataHandle) &&
        ownedModelDataHandle != nullptr) {
      modelResourcePtr =
          resourceCache.resolveDirectModelResourcePtr(ownedModelDataHandle);
      if (modelResourcePtr != nullptr &&
          resourceCache.findModelResource(modelResourcePtr, resourceRecord)) {
        modelKey = resourceRecord.modelKey;
      }
    }
  }

  if (modelResourcePtr == nullptr && modelKey == 0u)
    return false;

  outRecord.runtimeModelPtr = runtimeModelPtr;
  outRecord.modelResourcePtr = modelResourcePtr;
  outRecord.modelKey = modelKey;
  return true;
}

bool TryGetRuntimePoseSnapshot(void* runtimeModelPtr,
                               model::PoseRecord& outRecord) {
  outRecord = {};
  if (runtimeModelPtr == nullptr)
    return false;
  return model::PoseRegistry::instance().findByRuntimeModel(runtimeModelPtr,
                                                            outRecord);
}

void MaybeApplyPoseSnapshot(dxvk::War3ShadowSemanticContext& semantic,
                            bool hasTransform, const Matrix4& transform,
                            float scale, float height, bool fromSpriteFrame,
                            uint64_t sampleFrame, uint64_t& bestFrame) {
  if (!hasTransform || sampleFrame == 0 || sampleFrame < bestFrame)
    return;
  if (sampleFrame == bestFrame && !fromSpriteFrame &&
      semantic.hasPoseTransform && semantic.poseFromSpriteFrame) {
    // 对飞行单位/挂点模型，sprite-frame pose 通常比 root pose 更接近
    // 本帧最终渲染姿态。相同 frame 的情况下不允许 root pose 把它覆盖掉。
    return;
  }

  semantic.hasPoseTransform = true;
  semantic.poseFromSpriteFrame = fromSpriteFrame;
  semantic.poseTransform = transform;
  semantic.poseScale = scale;
  semantic.poseHeight = height;
  bestFrame = sampleFrame;
}

void MaybeApplyPoseMatrices(dxvk::War3ShadowSemanticContext& semantic,
                            uint32_t matrixCount, uint64_t matrixHash,
                            uint64_t sampleFrame, uint64_t& bestFrame) {
  if (matrixCount == 0 || sampleFrame == 0 || sampleFrame < bestFrame)
    return;

  semantic.poseMatrixCount = matrixCount;
  semantic.poseMatrixHash = matrixHash;
  bestFrame = sampleFrame;
}

void MergeRenderObject(dxvk::War3ShadowSemanticContext& semantic,
                       const RenderObjectInfo* object) {
  if (object == nullptr)
    return;

  if (semantic.object == nullptr)
    semantic.object = object;
  if (semantic.sceneNode == nullptr)
    semantic.sceneNode = object->sceneNode;
  if (semantic.worldObjectEntry == nullptr)
    semantic.worldObjectEntry = object->worldObjectEntry;
  if (semantic.jHandle == 0u)
    semantic.jHandle = object->jHandle;
  if (semantic.rawcode == 0u)
    semantic.rawcode = object->rawcode;
  if (static_cast<uint32_t>(semantic.objectKind) == 0u)
    semantic.objectKind = object->kind;
}

} // namespace

SemanticAugmentTlsCacheStats QuerySemanticAugmentTlsCacheStats() noexcept {
  const auto& source = g_semanticAugmentTlsCacheStats;
  SemanticAugmentTlsCacheStats result = {};
  result.enabled = SemanticAugmentTlsCacheRuntime();
  result.telemetryEnabled = SemanticAugmentTlsCacheStatsRuntime();
  result.capacityPerRegistry =
      static_cast<uint32_t>(kSemanticAugmentTlsCacheCapacity);
  result.modelLookups =
      source.modelLookups.load(std::memory_order_relaxed);
  result.modelHits = source.modelHits.load(std::memory_order_relaxed);
  result.modelNegativeHits =
      source.modelNegativeHits.load(std::memory_order_relaxed);
  result.modelMisses = source.modelMisses.load(std::memory_order_relaxed);
  result.modelGenerationMismatches =
      source.modelGenerationMismatches.load(std::memory_order_relaxed);
  result.modelCollisions =
      source.modelCollisions.load(std::memory_order_relaxed);
  result.shadowLookups =
      source.shadowLookups.load(std::memory_order_relaxed);
  result.shadowHits = source.shadowHits.load(std::memory_order_relaxed);
  result.shadowNegativeHits =
      source.shadowNegativeHits.load(std::memory_order_relaxed);
  result.shadowMisses =
      source.shadowMisses.load(std::memory_order_relaxed);
  result.shadowGenerationMismatches =
      source.shadowGenerationMismatches.load(std::memory_order_relaxed);
  result.shadowCollisions =
      source.shadowCollisions.load(std::memory_order_relaxed);
  result.modelRegistryGeneration =
      model::ModelInstanceRegistry::instance().mutationGeneration();
  result.shadowRegistryGeneration =
      ShadowObjectRegistry::instance().mutationGeneration();
  return result;
}

void FinalizeWorldObjectsPhase1PreviousFrameWithoutNewDecision() noexcept {
  const uint64_t completedEventSequence =
      g_worldObjectsPhase1Tls.eventSequence;
  const uint64_t completedDispatchEventSequence =
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence;
  const auto completedDispatchCaptureKind =
      g_worldObjectsPhase1Tls.dispatchCaptureKind;
  auto completedPeriodicDispatch =
      g_worldObjectsPhase1Tls.periodicDispatch;
  uint64_t abandonedCollectorCalls = 0u;
  for (uint32_t group = 0u; group < kWorldObjectsPhase1GroupCount; ++group) {
    abandonedCollectorCalls +=
        g_worldObjectsPhase1Tls.pendingCollectorCalls[group];
  }
  const uint64_t activeDispatchCaptureSequence =
      g_worldObjectsPhase1ActiveDispatchCaptureSequence.load(
          std::memory_order_acquire);
  const bool hadLocalFrameState =
      completedEventSequence != 0u ||
      completedDispatchEventSequence != 0u ||
      activeDispatchCaptureSequence != 0u ||
      g_worldObjectsPhase1Tls.collectorActive ||
      abandonedCollectorCalls != 0u ||
      g_worldObjectsPhase1PurePeriodicDispatchSequence != 0u;

  if (!hadLocalFrameState) {
    if (g_worldObjectsPhase1ActiveEventSequence.load(
            std::memory_order_acquire) != 0u) {
      g_worldObjectsPhase1ActiveEventSequence.store(
          0u, std::memory_order_release);
    }
    return;
  }

  // This is the exact Present boundary at which the normal decision path
  // calls RecordWorldObjectsPhase1Tracking and takes the same previous-frame
  // TLS state.  Clear before publishing so no later dispatch can append to a
  // frame that has already been settled.
  WorldObjectsPhase1WriteGuard writeGuard;
  SettleWorldObjectsPhase1ActiveDispatchCaptureMarker(
      completedDispatchEventSequence, completedDispatchCaptureKind);
  g_worldObjectsPhase1Tls = {};
  ClearWorldObjectsPhase1PairedCaptureTls();

  PublishWorldObjectsPhase1PairedCapture(
      completedDispatchEventSequence, completedDispatchCaptureKind,
      completedPeriodicDispatch);
  if (abandonedCollectorCalls != 0u) {
    g_worldObjectsPhase1CollectorWithoutHookCount.fetch_add(
        abandonedCollectorCalls, std::memory_order_relaxed);
  }
  g_worldObjectsPhase1ActiveEventSequence.store(
      0u, std::memory_order_release);
}

bool IsWorldObjectsPhase1CaptureActive(int groupIdx) noexcept {
  return groupIdx >= 0 &&
      uint32_t(groupIdx) < kWorldObjectsPhase1GroupCount &&
      g_worldObjectsPhase1Tls.eventSequence != 0u;
}

bool IsWorldObjectsPhase1CollectorCaptureActive() noexcept {
  return g_worldObjectsPhase1Tls.eventSequence != 0u &&
      g_worldObjectsPhase1Tls.collectorActive;
}

bool BeginWorldObjectsPhase1Collector(int groupIdx) noexcept {
  if (groupIdx < 0 || uint32_t(groupIdx) >= kWorldObjectsPhase1GroupCount) {
    if (g_worldObjectsPhase1Tls.eventSequence != 0u)
      RecordWorldObjectsPhase1Counter(
          g_worldObjectsPhase1UnexpectedGroupCount);
    return false;
  }

  if (g_worldObjectsPhase1Tls.eventSequence == 0u) {
    if (g_worldObjectsPhase1ActiveEventSequence.load(
            std::memory_order_acquire) != 0u) {
      RecordWorldObjectsPhase1Counter(
          g_worldObjectsPhase1CollectorWithoutEventCount);
    }
    return false;
  }

  if (g_worldObjectsPhase1Tls.collectorActive) {
    RecordWorldObjectsPhase1Counter(
        g_worldObjectsPhase1CollectorReentryCount);
    return false;
  }

  const uint32_t group = uint32_t(groupIdx);
  if (g_worldObjectsPhase1Tls.pendingCollectorCalls[group] != 0u) {
    RecordWorldObjectsPhase1Counter(
        g_worldObjectsPhase1CollectorWithoutHookCount,
        g_worldObjectsPhase1Tls.pendingCollectorCalls[group]);
    g_worldObjectsPhase1Tls.pendingCollectorCalls[group] = 0u;
    g_worldObjectsPhase1Tls.pendingCollectorTicks[group] = 0u;
  }
  g_worldObjectsPhase1Tls.collectorActive = true;
  g_worldObjectsPhase1Tls.collectorGroup = group;
  g_worldObjectsPhase1Tls.modelFeedTicks = 0u;
  g_worldObjectsPhase1Tls.shadowFeedTicks = 0u;
  g_worldObjectsPhase1Tls.modelFeedMaxTicks = 0u;
  g_worldObjectsPhase1Tls.shadowFeedMaxTicks = 0u;
  g_worldObjectsPhase1Tls.modelFeedCalls = 0u;
  g_worldObjectsPhase1Tls.shadowFeedCalls = 0u;
  return true;
}

void CompleteWorldObjectsPhase1Collector(
    const WorldObjectsPhase1CollectorObservation& observation) noexcept {
  if (!g_worldObjectsPhase1Tls.collectorActive ||
      g_worldObjectsPhase1Tls.eventSequence == 0u ||
      g_worldObjectsPhase1Tls.collectorGroup >=
          kWorldObjectsPhase1GroupCount) {
    RecordWorldObjectsPhase1Counter(
        g_worldObjectsPhase1CollectorWithoutEventCount);
    return;
  }

  const uint32_t group = g_worldObjectsPhase1Tls.collectorGroup;
  const uint64_t eventSequence = g_worldObjectsPhase1Tls.eventSequence;
  const uint64_t modelFeedTicks = g_worldObjectsPhase1Tls.modelFeedTicks;
  const uint64_t shadowFeedTicks = g_worldObjectsPhase1Tls.shadowFeedTicks;
  const uint64_t modelFeedMaxTicks =
      g_worldObjectsPhase1Tls.modelFeedMaxTicks;
  const uint64_t shadowFeedMaxTicks =
      g_worldObjectsPhase1Tls.shadowFeedMaxTicks;
  const uint32_t modelFeedCalls = g_worldObjectsPhase1Tls.modelFeedCalls;
  const uint32_t shadowFeedCalls = g_worldObjectsPhase1Tls.shadowFeedCalls;
  const uint32_t outcomeIndex =
      static_cast<uint32_t>(observation.outcome);
  const uint32_t safeOutcomeIndex =
      outcomeIndex < kWorldObjectsPhase1CollectorOutcomeCount
          ? outcomeIndex
          : static_cast<uint32_t>(
                WorldObjectsPhase1CollectorOutcome::Unclassified);
  const uint64_t partitionTicks = observation.setupTicks +
      observation.iterateTicks + observation.registerTicks +
      observation.tailTicks;
  const uint64_t feedTicks = modelFeedTicks + shadowFeedTicks;

  const uint64_t pairedCaptureSequence =
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence();
  if (pairedCaptureSequence != 0u) {
    RecordWorldObjectsPhase1PairedTiming(
        pairedCaptureSequence,
        WorldObjectsPhase1PairedTimingStage::WorldCollector,
        observation.inclusiveTicks);
  }

  WorldObjectsPhase1WriteGuard writeGuard;
  auto& aggregate = g_worldObjectsPhase1Groups[group];
  RecordWorldObjectsPhase1AtomicTiming(
      aggregate.collectorInclusive, observation.inclusiveTicks);
  RecordWorldObjectsPhase1AtomicTiming(
      aggregate.collectorSetup, observation.setupTicks);
  RecordWorldObjectsPhase1AtomicTiming(
      aggregate.collectorIterate, observation.iterateTicks);
  RecordWorldObjectsPhase1AtomicTiming(
      aggregate.collectorRegister, observation.registerTicks);
  RecordWorldObjectsPhase1AtomicTiming(
      aggregate.collectorTail, observation.tailTicks);
  if (modelFeedCalls != 0u) {
    RecordWorldObjectsPhase1AtomicTimingSamples(
        aggregate.modelFeed, modelFeedCalls, modelFeedTicks,
        modelFeedMaxTicks);
  }
  if (shadowFeedCalls != 0u) {
    RecordWorldObjectsPhase1AtomicTimingSamples(
        aggregate.shadowFeed, shadowFeedCalls, shadowFeedTicks,
        shadowFeedMaxTicks);
  }
  aggregate.listEntries.fetch_add(
      observation.listEntries, std::memory_order_relaxed);
  aggregate.acceptedEntries.fetch_add(
      observation.acceptedEntries, std::memory_order_relaxed);
  aggregate.sceneNodeEntries.fetch_add(
      observation.sceneNodeEntries, std::memory_order_relaxed);
  aggregate.handleEntries.fetch_add(
      observation.handleEntries, std::memory_order_relaxed);
  aggregate.outcomeCounts[safeOutcomeIndex].fetch_add(
      1u, std::memory_order_relaxed);
  if (partitionTicks != observation.inclusiveTicks) {
    aggregate.collectorPartitionMismatchCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (feedTicks > observation.registerTicks) {
    aggregate.registerFeedContainmentViolationCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (observation.acceptedEntries > observation.listEntries) {
    aggregate.acceptedCountViolationCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (observation.sceneNodeEntries > observation.acceptedEntries) {
    aggregate.sceneNodeCountViolationCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (observation.handleEntries > observation.acceptedEntries) {
    aggregate.handleCountViolationCount.fetch_add(
        1u, std::memory_order_relaxed);
  }

  {
    std::lock_guard<std::mutex> lock(g_worldObjectsPhase1EventMutex);
    auto& event = g_worldObjectsPhase1Events[
        (eventSequence - 1u) % kWorldObjectsPhase1EventSlotCount];
    if (event.sequence == eventSequence) {
      auto& eventGroup = event.groups[group];
      const uint32_t groupBit = 1u << group;
      if (eventGroup.collectorCalls != 0u)
        event.duplicateCollectorGroupMask |= groupBit;
      event.collectorGroupMask |= groupBit;
      eventGroup.collectorCalls += 1u;
      eventGroup.collectorInclusiveTicks += observation.inclusiveTicks;
      eventGroup.collectorSetupTicks += observation.setupTicks;
      eventGroup.collectorIterateTicks += observation.iterateTicks;
      eventGroup.collectorRegisterTicks += observation.registerTicks;
      eventGroup.collectorTailTicks += observation.tailTicks;
      eventGroup.modelFeedTicks += modelFeedTicks;
      eventGroup.shadowFeedTicks += shadowFeedTicks;
      eventGroup.modelFeedCalls += modelFeedCalls;
      eventGroup.shadowFeedCalls += shadowFeedCalls;
      eventGroup.listEntries += observation.listEntries;
      eventGroup.acceptedEntries += observation.acceptedEntries;
      eventGroup.sceneNodeEntries += observation.sceneNodeEntries;
      eventGroup.handleEntries += observation.handleEntries;
      eventGroup.outcomeCounts[safeOutcomeIndex] += 1u;
    }
  }

  g_worldObjectsPhase1Tls.pendingCollectorTicks[group] =
      observation.inclusiveTicks;
  g_worldObjectsPhase1Tls.pendingCollectorCalls[group] = 1u;
  g_worldObjectsPhase1Tls.collectorActive = false;
  g_worldObjectsPhase1Tls.collectorGroup =
      kWorldObjectsPhase1GroupCount;
  g_worldObjectsPhase1Tls.modelFeedTicks = 0u;
  g_worldObjectsPhase1Tls.shadowFeedTicks = 0u;
  g_worldObjectsPhase1Tls.modelFeedMaxTicks = 0u;
  g_worldObjectsPhase1Tls.shadowFeedMaxTicks = 0u;
  g_worldObjectsPhase1Tls.modelFeedCalls = 0u;
  g_worldObjectsPhase1Tls.shadowFeedCalls = 0u;
}

void RecordWorldObjectsPhase1HookInclusive(int groupIdx,
                                           uint64_t ticks) noexcept {
  if (groupIdx < 0 || uint32_t(groupIdx) >= kWorldObjectsPhase1GroupCount) {
    if (g_worldObjectsPhase1Tls.eventSequence != 0u)
      RecordWorldObjectsPhase1Counter(
          g_worldObjectsPhase1UnexpectedGroupCount);
    return;
  }
  if (g_worldObjectsPhase1Tls.eventSequence == 0u)
    return;

  const uint32_t group = uint32_t(groupIdx);
  const uint64_t eventSequence = g_worldObjectsPhase1Tls.eventSequence;
  const uint32_t pendingCalls =
      g_worldObjectsPhase1Tls.pendingCollectorCalls[group];
  const uint64_t pendingTicks =
      g_worldObjectsPhase1Tls.pendingCollectorTicks[group];

  WorldObjectsPhase1WriteGuard writeGuard;
  auto& aggregate = g_worldObjectsPhase1Groups[group];
  RecordWorldObjectsPhase1AtomicTiming(aggregate.hookInclusive, ticks);
  if (pendingCalls == 0u) {
    g_worldObjectsPhase1HookWithoutCollectorCount.fetch_add(
        1u, std::memory_order_relaxed);
  } else if (ticks < pendingTicks) {
    aggregate.hookContainmentViolationCount.fetch_add(
        1u, std::memory_order_relaxed);
  }

  {
    std::lock_guard<std::mutex> lock(g_worldObjectsPhase1EventMutex);
    auto& event = g_worldObjectsPhase1Events[
        (eventSequence - 1u) % kWorldObjectsPhase1EventSlotCount];
    if (event.sequence == eventSequence) {
      auto& eventGroup = event.groups[group];
      const uint32_t groupBit = 1u << group;
      if (eventGroup.hookCalls != 0u)
        event.duplicateHookGroupMask |= groupBit;
      event.hookGroupMask |= groupBit;
      eventGroup.hookCalls += 1u;
      eventGroup.hookInclusiveTicks += ticks;
    }
  }

  g_worldObjectsPhase1Tls.pendingCollectorCalls[group] = 0u;
  g_worldObjectsPhase1Tls.pendingCollectorTicks[group] = 0u;
}

void RecordWorldObjectsPhase1RegistryFeed(uint64_t modelTicks,
                                          uint64_t shadowTicks) noexcept {
  if (!g_worldObjectsPhase1Tls.collectorActive ||
      g_worldObjectsPhase1Tls.eventSequence == 0u) {
    RecordWorldObjectsPhase1Counter(
        g_worldObjectsPhase1RegistryFeedOutsideCollectorCount);
    return;
  }
  g_worldObjectsPhase1Tls.modelFeedTicks += modelTicks;
  g_worldObjectsPhase1Tls.shadowFeedTicks += shadowTicks;
  g_worldObjectsPhase1Tls.modelFeedMaxTicks = std::max(
      g_worldObjectsPhase1Tls.modelFeedMaxTicks, modelTicks);
  g_worldObjectsPhase1Tls.shadowFeedMaxTicks = std::max(
      g_worldObjectsPhase1Tls.shadowFeedMaxTicks, shadowTicks);
  g_worldObjectsPhase1Tls.modelFeedCalls += 1u;
  g_worldObjectsPhase1Tls.shadowFeedCalls += 1u;
}

void RecordWorldObjectsPhase1PeriodicDispatch(
    uint64_t eventSequence, bool special, bool group0Stage,
    bool worldFastEligibleIgnoringIdentity,
    bool worldFastBlockedByIdentity) noexcept {
  if (eventSequence == 0u ||
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence() !=
          eventSequence ||
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence != eventSequence ||
      !IsWorldObjectsPhase1PairedCaptureKind(
          g_worldObjectsPhase1Tls.dispatchCaptureKind))
    return;

  auto& dispatch = g_worldObjectsPhase1Tls.periodicDispatch;
  dispatch.commonCalls += special ? 0u : 1u;
  dispatch.specialCalls += special ? 1u : 0u;
  dispatch.group0Calls += group0Stage ? 1u : 0u;
  dispatch.otherStageCalls += group0Stage ? 0u : 1u;
  dispatch.worldFastEligibleIgnoringIdentity +=
      worldFastEligibleIgnoringIdentity ? 1u : 0u;
  dispatch.worldFastBlockedByIdentity +=
      worldFastBlockedByIdentity ? 1u : 0u;
}

void RecordWorldObjectsPhase1PeriodicDispatchRoot(
    uint64_t eventSequence, uint64_t ticks) noexcept {
  if (eventSequence == 0u ||
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence() !=
          eventSequence ||
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence != eventSequence ||
      !IsWorldObjectsPhase1PairedCaptureKind(
          g_worldObjectsPhase1Tls.dispatchCaptureKind))
    return;

  auto& dispatch = g_worldObjectsPhase1Tls.periodicDispatch;
  dispatch.dispatchRootCalls += 1u;
  dispatch.dispatchRootTicks += ticks;
}

void RecordWorldObjectsPhase1PeriodicGetTagStage(
    uint64_t eventSequence, bool hit, bool conflict,
    uint32_t probes, uint64_t ticks) noexcept {
  if (eventSequence == 0u ||
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence() !=
          eventSequence ||
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence != eventSequence ||
      !IsWorldObjectsPhase1PairedCaptureKind(
          g_worldObjectsPhase1Tls.dispatchCaptureKind))
    return;

  auto& dispatch = g_worldObjectsPhase1Tls.periodicDispatch;
  dispatch.getTagStageCalls += 1u;
  dispatch.getTagStageHits += hit ? 1u : 0u;
  dispatch.getTagStageMisses += hit ? 0u : 1u;
  dispatch.getTagStageConflicts += hit && conflict ? 1u : 0u;
  dispatch.getTagStageProbes += probes;
  dispatch.getTagStageTicks += ticks;
  // RenderQueueTracker brackets every recorded GetTagStage sample with one
  // begin/end QPC pair. Account those reads here so the pair-level QPC
  // contract remains exact without widening the tracker hot-path API.
  dispatch.qpcReadCount += 2u;
}

void RecordWorldObjectsPhase1PairedTiming(
    uint64_t eventSequence, WorldObjectsPhase1PairedTimingStage stage,
    uint64_t ticks) noexcept {
  const uint32_t stageIndex = static_cast<uint32_t>(stage);
  if (eventSequence == 0u ||
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence() !=
          eventSequence ||
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence != eventSequence ||
      !IsWorldObjectsPhase1PairedCaptureKind(
          g_worldObjectsPhase1Tls.dispatchCaptureKind) ||
      stageIndex >= kWorldObjectsPhase1PairedTimingStageCount)
    return;
  auto& timing =
      g_worldObjectsPhase1Tls.periodicDispatch.stageTimings[stageIndex];
  timing.calls += 1u;
  timing.ticks += ticks;
  timing.maxTicks = std::max(timing.maxTicks, ticks);
}

void RecordWorldObjectsPhase1PairedQpcReads(
    uint64_t eventSequence, uint64_t reads) noexcept {
  if (eventSequence == 0u || reads == 0u ||
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence() !=
          eventSequence ||
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence != eventSequence ||
      !IsWorldObjectsPhase1PairedCaptureKind(
          g_worldObjectsPhase1Tls.dispatchCaptureKind))
    return;
  g_worldObjectsPhase1Tls.periodicDispatch.qpcReadCount += reads;
}

void RecordWorldObjectsPhase1PairedFlushTopology(
    uint64_t eventSequence, uint32_t opaqueCount,
    uint32_t transparentCount) noexcept {
  if (eventSequence == 0u ||
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence() !=
          eventSequence ||
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence != eventSequence ||
      !IsWorldObjectsPhase1PairedCaptureKind(
          g_worldObjectsPhase1Tls.dispatchCaptureKind))
    return;
  auto& capture = g_worldObjectsPhase1Tls.periodicDispatch;
  capture.flushTopologyCalls += 1u;
  capture.opaqueCountTotal += opaqueCount;
  capture.transparentCountTotal += transparentCount;
  constexpr uint64_t kFnvPrime = 1099511628211ull;
  capture.flushTopologyHash ^= uint64_t(opaqueCount);
  capture.flushTopologyHash *= kFnvPrime;
  capture.flushTopologyHash ^= uint64_t(transparentCount);
  capture.flushTopologyHash *= kFnvPrime;
}

void RecordWorldObjectsPhase1PairedFlushTerminal(
    uint64_t eventSequence,
    WorldObjectsPhase1FlushTerminal terminal) noexcept {
  const uint32_t terminalIndex = static_cast<uint32_t>(terminal);
  if (eventSequence == 0u ||
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence() !=
          eventSequence ||
      g_worldObjectsPhase1Tls.dispatchCaptureEventSequence != eventSequence ||
      !IsWorldObjectsPhase1PairedCaptureKind(
          g_worldObjectsPhase1Tls.dispatchCaptureKind) ||
      terminalIndex >= kWorldObjectsPhase1FlushTerminalCount)
    return;
  g_worldObjectsPhase1Tls.periodicDispatch
      .flushTerminalCounts[terminalIndex] += 1u;
}

WorldObjectsPhase1TelemetrySummary QueryWorldObjectsPhase1Telemetry() {
  WorldObjectsPhase1TelemetrySummary summary = {};
  summary.qpcFrequency = uint64_t(
      dxvk::high_resolution_clock::get_frequency());
  summary.snapshotGenerationBefore =
      g_worldObjectsPhase1SnapshotGeneration.load(
          std::memory_order_acquire);
  summary.snapshotWritersBefore =
      g_worldObjectsPhase1Writers.load(std::memory_order_acquire);
  summary.snapshotWritesStartedBefore =
      g_worldObjectsPhase1WritesStarted.load(std::memory_order_acquire);
  summary.snapshotWritesCompletedBefore =
      g_worldObjectsPhase1WritesCompleted.load(std::memory_order_acquire);
  summary.trackingAttempts =
      g_worldObjectsPhase1TrackingAttempts.load(std::memory_order_relaxed);
  summary.trackingHealthFastPathCalls =
      g_worldObjectsPhase1TrackingHealthFastPathCalls.load(
          std::memory_order_relaxed);
  summary.trackingHealthFullSummaryCompatibilityCalls =
      g_worldObjectsPhase1TrackingHealthFullSummaryCompatibilityCalls.load(
          std::memory_order_relaxed);
  summary.trackingHealthModelInstanceAggregateReadPasses =
      g_worldObjectsPhase1TrackingHealthModelInstanceAggregateReadPasses.load(
          std::memory_order_relaxed);
  summary.trackingHealthPoseAggregateReadPasses =
      g_worldObjectsPhase1TrackingHealthPoseAggregateReadPasses.load(
          std::memory_order_relaxed);
  summary.trackingHealthModelInstanceVerifierScanPasses =
      g_worldObjectsPhase1TrackingHealthModelInstanceVerifierScanPasses.load(
          std::memory_order_relaxed);
  summary.trackingHealthPoseVerifierScanPasses =
      g_worldObjectsPhase1TrackingHealthPoseVerifierScanPasses.load(
          std::memory_order_relaxed);
  summary.trackingHealthModelInstanceVerifierRecordsScanned =
      g_worldObjectsPhase1TrackingHealthModelInstanceVerifierRecordsScanned
          .load(std::memory_order_relaxed);
  summary.trackingHealthPoseVerifierRecordsScanned =
      g_worldObjectsPhase1TrackingHealthPoseVerifierRecordsScanned.load(
          std::memory_order_relaxed);
  summary.trackingHealthModelInstanceVerifierMismatchCount =
      g_worldObjectsPhase1TrackingHealthModelInstanceVerifierMismatchCount
          .load(std::memory_order_relaxed);
  summary.trackingHealthPoseVerifierMismatchCount =
      g_worldObjectsPhase1TrackingHealthPoseVerifierMismatchCount.load(
          std::memory_order_relaxed);
  summary.trackingHealthModelInstanceVerifierMismatchMask =
      g_worldObjectsPhase1TrackingHealthModelInstanceVerifierMismatchMask.load(
          std::memory_order_relaxed);
  summary.trackingHealthPoseVerifierMismatchMask =
      g_worldObjectsPhase1TrackingHealthPoseVerifierMismatchMask.load(
          std::memory_order_relaxed);
  summary.identityRequests =
      g_worldObjectsPhase1IdentityRequests.load(std::memory_order_relaxed);
  summary.fallbackRequests =
      g_worldObjectsPhase1FallbackRequests.load(std::memory_order_relaxed);
  summary.eventCountLifetime =
      g_worldObjectsPhase1EventSequence.load(std::memory_order_relaxed);
  summary.latestEventSequence = summary.eventCountLifetime;
  summary.collectorWithoutEventCount =
      g_worldObjectsPhase1CollectorWithoutEventCount.load(
          std::memory_order_relaxed);
  summary.collectorReentryCount =
      g_worldObjectsPhase1CollectorReentryCount.load(
          std::memory_order_relaxed);
  summary.collectorWithoutHookCount =
      g_worldObjectsPhase1CollectorWithoutHookCount.load(
          std::memory_order_relaxed);
  summary.hookWithoutCollectorCount =
      g_worldObjectsPhase1HookWithoutCollectorCount.load(
          std::memory_order_relaxed);
  summary.registryFeedOutsideCollectorCount =
      g_worldObjectsPhase1RegistryFeedOutsideCollectorCount.load(
          std::memory_order_relaxed);
  summary.unexpectedGroupCount =
      g_worldObjectsPhase1UnexpectedGroupCount.load(
          std::memory_order_relaxed);
  summary.pairedCaptureDuplicatePublishCount =
      g_worldObjectsPhase1PairedCaptureDuplicatePublishCount.load(
          std::memory_order_relaxed);
  summary.pairedCaptureLostPublishCount =
      g_worldObjectsPhase1PairedCaptureLostPublishCount.load(
          std::memory_order_relaxed);
  summary.pairedCaptureSlotMismatchCount =
      g_worldObjectsPhase1PairedCaptureSlotMismatchCount.load(
          std::memory_order_relaxed);
  summary.trackingInclusive = CopyWorldObjectsPhase1AtomicTiming(
      g_worldObjectsPhase1TrackingInclusive);
  summary.trackingQuery = CopyWorldObjectsPhase1AtomicTiming(
      g_worldObjectsPhase1TrackingQuery);
  summary.trackingDecision = CopyWorldObjectsPhase1AtomicTiming(
      g_worldObjectsPhase1TrackingDecision);

  const bool trackingHealthPathPartitionClean =
      summary.trackingHealthFastPathCalls <= summary.trackingAttempts &&
      summary.trackingHealthFullSummaryCompatibilityCalls ==
          summary.trackingAttempts - summary.trackingHealthFastPathCalls;
  if constexpr (internal::kNativeRendererHookTakeoverEnabled) {
    summary.trackingHealthPathClosureClean =
        trackingHealthPathPartitionClean &&
        summary.trackingHealthFullSummaryCompatibilityCalls ==
            summary.trackingAttempts &&
        summary.trackingHealthFastPathCalls == 0u &&
        summary.trackingHealthModelInstanceAggregateReadPasses == 0u &&
        summary.trackingHealthPoseAggregateReadPasses == 0u &&
        summary.trackingHealthModelInstanceVerifierScanPasses == 0u &&
        summary.trackingHealthPoseVerifierScanPasses == 0u &&
        summary.trackingHealthModelInstanceVerifierRecordsScanned == 0u &&
        summary.trackingHealthPoseVerifierRecordsScanned == 0u &&
        summary.trackingHealthModelInstanceVerifierMismatchCount == 0u &&
        summary.trackingHealthPoseVerifierMismatchCount == 0u &&
        summary.trackingHealthModelInstanceVerifierMismatchMask == 0u &&
        summary.trackingHealthPoseVerifierMismatchMask == 0u;
  } else {
    const bool verifierPassesClean =
        summary.trackingHealthModelInstanceVerifierScanPasses ==
            summary.trackingHealthPoseVerifierScanPasses &&
        (summary.trackingHealthModelInstanceVerifierScanPasses == 0u ||
         summary.trackingHealthModelInstanceVerifierScanPasses ==
             summary.trackingAttempts);
    const bool verifierRecordsClean =
        summary.trackingHealthModelInstanceVerifierScanPasses != 0u ||
        (summary.trackingHealthModelInstanceVerifierRecordsScanned == 0u &&
         summary.trackingHealthPoseVerifierRecordsScanned == 0u);
    const bool verifierMismatchesClean =
        summary.trackingHealthModelInstanceVerifierMismatchCount == 0u &&
        summary.trackingHealthPoseVerifierMismatchCount == 0u &&
        summary.trackingHealthModelInstanceVerifierMismatchMask == 0u &&
        summary.trackingHealthPoseVerifierMismatchMask == 0u;
    summary.trackingHealthPathClosureClean =
        trackingHealthPathPartitionClean &&
        summary.trackingHealthFastPathCalls == summary.trackingAttempts &&
        summary.trackingHealthFullSummaryCompatibilityCalls == 0u &&
        summary.trackingHealthModelInstanceAggregateReadPasses ==
            summary.trackingAttempts &&
        summary.trackingHealthPoseAggregateReadPasses ==
            summary.trackingAttempts &&
        verifierPassesClean && verifierRecordsClean &&
        verifierMismatchesClean;
  }

  uint64_t classifiedTrackingAttempts = 0u;
  uint64_t classifiedTrackingEvents = 0u;
  for (uint32_t reason = 0u;
       reason < kWorldObjectsPhase1TrackingReasonCount; ++reason) {
    summary.reasonCounts[reason] =
        g_worldObjectsPhase1ReasonCounts[reason].load(
            std::memory_order_relaxed);
    classifiedTrackingAttempts += summary.reasonCounts[reason];
    if (reason != static_cast<uint32_t>(
                      WorldObjectsPhase1TrackingReason::None)) {
      classifiedTrackingEvents += summary.reasonCounts[reason];
    }
  }
  summary.trackingReasonClosureClean =
      classifiedTrackingAttempts == summary.trackingAttempts &&
      summary.trackingInclusive.calls == summary.trackingAttempts &&
      summary.trackingQuery.calls == summary.trackingAttempts &&
      summary.trackingDecision.calls == summary.trackingAttempts &&
      summary.trackingInclusive.ticks ==
          summary.trackingQuery.ticks + summary.trackingDecision.ticks;
  summary.eventCountClosureClean =
      classifiedTrackingEvents == summary.eventCountLifetime;

  bool lifetimeObservedContentClean = true;
  summary.lifetimeUnobservedGroupsZeroClean = true;
  for (uint32_t group = 0u; group < kWorldObjectsPhase1GroupCount; ++group) {
    const auto& source = g_worldObjectsPhase1Groups[group];
    auto& destination = summary.groups[group];
    destination.hookInclusive = CopyWorldObjectsPhase1AtomicTiming(
        source.hookInclusive);
    destination.collectorInclusive = CopyWorldObjectsPhase1AtomicTiming(
        source.collectorInclusive);
    destination.collectorSetup = CopyWorldObjectsPhase1AtomicTiming(
        source.collectorSetup);
    destination.collectorIterate = CopyWorldObjectsPhase1AtomicTiming(
        source.collectorIterate);
    destination.collectorRegister = CopyWorldObjectsPhase1AtomicTiming(
        source.collectorRegister);
    destination.collectorTail = CopyWorldObjectsPhase1AtomicTiming(
        source.collectorTail);
    destination.modelFeed = CopyWorldObjectsPhase1AtomicTiming(
        source.modelFeed);
    destination.shadowFeed = CopyWorldObjectsPhase1AtomicTiming(
        source.shadowFeed);
    destination.listEntries =
        source.listEntries.load(std::memory_order_relaxed);
    destination.acceptedEntries =
        source.acceptedEntries.load(std::memory_order_relaxed);
    destination.sceneNodeEntries =
        source.sceneNodeEntries.load(std::memory_order_relaxed);
    destination.handleEntries =
        source.handleEntries.load(std::memory_order_relaxed);
    destination.collectorPartitionMismatchCount =
        source.collectorPartitionMismatchCount.load(
            std::memory_order_relaxed);
    destination.hookContainmentViolationCount =
        source.hookContainmentViolationCount.load(
            std::memory_order_relaxed);
    destination.registerFeedContainmentViolationCount =
        source.registerFeedContainmentViolationCount.load(
            std::memory_order_relaxed);
    destination.acceptedCountViolationCount =
        source.acceptedCountViolationCount.load(
            std::memory_order_relaxed);
    destination.sceneNodeCountViolationCount =
        source.sceneNodeCountViolationCount.load(
            std::memory_order_relaxed);
    destination.handleCountViolationCount =
        source.handleCountViolationCount.load(
            std::memory_order_relaxed);
    uint64_t classifiedCollectorCalls = 0u;
    for (uint32_t outcome = 0u;
         outcome < kWorldObjectsPhase1CollectorOutcomeCount; ++outcome) {
      destination.outcomeCounts[outcome] =
          source.outcomeCounts[outcome].load(std::memory_order_relaxed);
      classifiedCollectorCalls += destination.outcomeCounts[outcome];
    }
    destination.outcomeClosureClean =
        classifiedCollectorCalls == destination.collectorInclusive.calls;
    destination.collectorPartitionClean =
        destination.collectorPartitionMismatchCount == 0u &&
        destination.collectorSetup.calls ==
            destination.collectorInclusive.calls &&
        destination.collectorIterate.calls ==
            destination.collectorInclusive.calls &&
        destination.collectorRegister.calls ==
            destination.collectorInclusive.calls &&
        destination.collectorTail.calls ==
            destination.collectorInclusive.calls &&
        destination.collectorInclusive.ticks ==
            destination.collectorSetup.ticks +
            destination.collectorIterate.ticks +
            destination.collectorRegister.ticks +
            destination.collectorTail.ticks;
    destination.hookCollectorCallClosureClean =
        destination.hookInclusive.calls ==
            destination.collectorInclusive.calls;
    destination.containmentClean =
        destination.hookContainmentViolationCount == 0u &&
        destination.registerFeedContainmentViolationCount == 0u &&
        destination.hookInclusive.ticks >=
            destination.collectorInclusive.ticks &&
        destination.collectorRegister.ticks >=
            destination.modelFeed.ticks + destination.shadowFeed.ticks &&
        destination.modelFeed.calls == destination.shadowFeed.calls;
    destination.entryCountBoundsClean =
        destination.acceptedCountViolationCount == 0u &&
        destination.sceneNodeCountViolationCount == 0u &&
        destination.handleCountViolationCount == 0u &&
        destination.acceptedEntries <= destination.listEntries &&
        destination.sceneNodeEntries <= destination.acceptedEntries &&
        destination.handleEntries <= destination.acceptedEntries;
    const uint32_t groupBit = 1u << group;
    if (destination.collectorInclusive.calls != 0u)
      summary.lifetimeCollectorGroupMask |= groupBit;
    if (destination.hookInclusive.calls != 0u)
      summary.lifetimeHookGroupMask |= groupBit;
    destination.observed =
        destination.collectorInclusive.calls != 0u ||
        destination.hookInclusive.calls != 0u;
    const bool zeroResidual =
        IsWorldObjectsPhase1RawTimingZero(destination.hookInclusive) &&
        IsWorldObjectsPhase1RawTimingZero(destination.collectorInclusive) &&
        IsWorldObjectsPhase1RawTimingZero(destination.collectorSetup) &&
        IsWorldObjectsPhase1RawTimingZero(destination.collectorIterate) &&
        IsWorldObjectsPhase1RawTimingZero(destination.collectorRegister) &&
        IsWorldObjectsPhase1RawTimingZero(destination.collectorTail) &&
        IsWorldObjectsPhase1RawTimingZero(destination.modelFeed) &&
        IsWorldObjectsPhase1RawTimingZero(destination.shadowFeed) &&
        destination.listEntries == 0u &&
        destination.acceptedEntries == 0u &&
        destination.sceneNodeEntries == 0u &&
        destination.handleEntries == 0u &&
        destination.collectorPartitionMismatchCount == 0u &&
        destination.hookContainmentViolationCount == 0u &&
        destination.registerFeedContainmentViolationCount == 0u &&
        destination.acceptedCountViolationCount == 0u &&
        destination.sceneNodeCountViolationCount == 0u &&
        destination.handleCountViolationCount == 0u &&
        classifiedCollectorCalls == 0u;
    destination.unobservedZeroClean =
        destination.observed || zeroResidual;
    if (destination.observed) {
      lifetimeObservedContentClean = lifetimeObservedContentClean &&
          destination.outcomeClosureClean &&
          destination.collectorPartitionClean &&
          destination.hookCollectorCallClosureClean &&
          destination.containmentClean &&
          destination.entryCountBoundsClean;
    } else {
      summary.lifetimeUnobservedGroupsZeroClean =
          summary.lifetimeUnobservedGroupsZeroClean && zeroResidual;
    }
  }
  summary.lifetimeObservedGroupMask =
      summary.lifetimeCollectorGroupMask |
      summary.lifetimeHookGroupMask;
  summary.lifetimeObservedGroupsClosureClean =
      summary.lifetimeCollectorGroupMask ==
          summary.lifetimeHookGroupMask &&
      lifetimeObservedContentClean &&
      summary.lifetimeUnobservedGroupsZeroClean;

  summary.retainedEventsClosureClean = true;
  {
    std::lock_guard<std::mutex> lock(g_worldObjectsPhase1EventMutex);
    const uint64_t retained = std::min<uint64_t>(
        summary.latestEventSequence, kWorldObjectsPhase1EventSlotCount);
    summary.retainedEventExpectedCount = uint32_t(retained);
    const uint64_t firstSequence = retained != 0u
        ? summary.latestEventSequence - retained + 1u
        : 0u;
    for (uint64_t sequence = firstSequence;
         sequence != 0u && sequence <= summary.latestEventSequence;
         ++sequence) {
      const auto& stored = g_worldObjectsPhase1Events[
          (sequence - 1u) % kWorldObjectsPhase1EventSlotCount];
      if (stored.sequence != sequence) {
        summary.missingRetainedEventCount += 1u;
        summary.retainedEventsClosureClean = false;
        continue;
      }
      auto event = stored;
      event.trackingPartitionClean =
          event.trackingInclusiveTicks ==
              event.trackingQueryTicks + event.trackingDecisionTicks;
      const auto finalizeDispatchCapture =
          [](WorldObjectsPhase1PeriodicDispatch& dispatch) {
        const uint64_t dispatchCalls =
            dispatch.commonCalls + dispatch.specialCalls;
        dispatch.dispatchPathClosureClean =
            dispatchCalls ==
                dispatch.group0Calls + dispatch.otherStageCalls;
        dispatch.dispatchRootClosureClean =
            (dispatchCalls == 0u || dispatch.dispatchRootCalls != 0u) &&
            (dispatch.dispatchRootCalls != 0u ||
             dispatch.dispatchRootTicks == 0u);
        dispatch.worldFastClosureClean =
            dispatch.worldFastBlockedByIdentity <=
                dispatch.worldFastEligibleIgnoringIdentity &&
            dispatch.worldFastEligibleIgnoringIdentity <= dispatchCalls;
        dispatch.getTagStageClosureClean =
            dispatch.getTagStageCalls ==
                dispatch.getTagStageHits + dispatch.getTagStageMisses &&
            dispatch.getTagStageConflicts <= dispatch.getTagStageHits &&
            dispatch.getTagStageProbes >= dispatch.getTagStageHits &&
            dispatch.getTagStageProbes <=
                dispatch.getTagStageCalls *
                    kWorldObjectsPhase1GetTagStageMaxProbes;

        const auto timing = [&](WorldObjectsPhase1PairedTimingStage stage)
            -> const WorldObjectsPhase1RawTiming& {
          return dispatch.stageTimings[static_cast<uint32_t>(stage)];
        };
        const auto& flushRoot =
            timing(WorldObjectsPhase1PairedTimingStage::FlushRoot);
        const auto& dispatchRoot =
            timing(WorldObjectsPhase1PairedTimingStage::DispatchRoot);
        const auto& worldRoot =
            timing(WorldObjectsPhase1PairedTimingStage::WorldHookInclusive);
        const uint64_t worldKnownTicks =
            timing(WorldObjectsPhase1PairedTimingStage::WorldCollector).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::WorldOriginal).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       WorldTrackNewBatches).ticks;
        const uint64_t flushKnownTicks =
            timing(WorldObjectsPhase1PairedTimingStage::FlushNotify).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushTransactionBegin).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushOriginalBody).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushReimplOpaque).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushReimplTransparent).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushTransactionEnd).ticks;
        const uint64_t dispatchKnownTicks =
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchResolveSemantic).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchNativeBegin).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchExecBegin).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchOriginal).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchPublishVisible).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchExecEnd).ticks +
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchNativeEnd).ticks;
        uint64_t terminalCalls = 0u;
        for (uint64_t calls : dispatch.flushTerminalCounts)
          terminalCalls += calls;
        uint64_t pairedTimedCalls = 0u;
        bool rawTimingClosureClean = true;
        for (uint32_t stage = 0u;
             stage < kWorldObjectsPhase1PairedTimingStageCount; ++stage) {
          const auto& raw = dispatch.stageTimings[stage];
          const bool maxProductFits = raw.maxTicks == 0u ||
              raw.calls <= UINT64_MAX / raw.maxTicks;
          rawTimingClosureClean = rawTimingClosureClean &&
              raw.maxTicks <= raw.ticks &&
              (raw.calls == 0u
                   ? raw.ticks == 0u && raw.maxTicks == 0u
                   : maxProductFits &&
                       raw.ticks <= raw.calls * raw.maxTicks);
          if (stage != static_cast<uint32_t>(
                           WorldObjectsPhase1PairedTimingStage::
                               WorldCollector)) {
            pairedTimedCalls += raw.calls;
          }
        }
        dispatch.rawTimingClosureClean = rawTimingClosureClean;
        dispatch.qpcReadClosureClean =
            pairedTimedCalls <= UINT64_MAX / 2u &&
            dispatch.getTagStageCalls <=
                UINT64_MAX / 2u - pairedTimedCalls &&
            dispatch.qpcReadCount ==
                2u * (pairedTimedCalls + dispatch.getTagStageCalls);
        dispatch.flushTopologyClosureClean =
            flushRoot.calls == dispatch.dispatchRootCalls &&
            flushRoot.ticks == dispatch.dispatchRootTicks &&
            dispatch.flushTopologyCalls == flushRoot.calls &&
            terminalCalls == flushRoot.calls &&
            dispatch.flushTerminalCounts[static_cast<uint32_t>(
                WorldObjectsPhase1FlushTerminal::Unclassified)] == 0u;
        dispatch.pairedTimingClosureClean =
            timing(WorldObjectsPhase1PairedTimingStage::
                       PresentPreTracking).calls == 1u &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushNotify).calls == flushRoot.calls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushTransactionBegin).calls == flushRoot.calls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushOriginalBody).calls <= flushRoot.calls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushReimplOpaque).calls <= flushRoot.calls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushReimplTransparent).calls <= flushRoot.calls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushOriginalBody).calls +
                    timing(WorldObjectsPhase1PairedTimingStage::
                               FlushReimplOpaque).calls >=
                flushRoot.calls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       FlushTransactionEnd).calls <=
                timing(WorldObjectsPhase1PairedTimingStage::
                           FlushTransactionBegin).calls &&
            dispatchRoot.calls == dispatchCalls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchResolveSemantic).calls == dispatchCalls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchNativeBegin).calls == dispatchCalls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchOriginal).calls == dispatchCalls &&
            timing(WorldObjectsPhase1PairedTimingStage::
                       DispatchNativeEnd).calls == dispatchCalls &&
            dispatch.rawTimingClosureClean &&
            dispatch.qpcReadClosureClean &&
            worldRoot.ticks >= worldKnownTicks &&
            flushRoot.ticks >= flushKnownTicks &&
            dispatchRoot.ticks >= dispatchKnownTicks &&
            (flushRoot.calls == 0u || dispatch.qpcReadCount != 0u);

        bool extendedZero = dispatch.captureFrameSerial == 0u &&
            dispatch.ownerThreadId == 0u && dispatch.qpcReadCount == 0u &&
            dispatch.flushTopologyCalls == 0u &&
            dispatch.opaqueCountTotal == 0u &&
            dispatch.transparentCountTotal == 0u &&
            dispatch.flushTopologyHash == 0u;
        for (uint64_t calls : dispatch.flushTerminalCounts)
          extendedZero = extendedZero && calls == 0u;
        for (const auto& stage : dispatch.stageTimings) {
          extendedZero = extendedZero && stage.calls == 0u &&
              stage.ticks == 0u && stage.maxTicks == 0u;
        }
        const bool periodicDispatchZero =
            dispatch.commonCalls == 0u && dispatch.specialCalls == 0u &&
            dispatch.group0Calls == 0u &&
            dispatch.otherStageCalls == 0u &&
            dispatch.dispatchRootCalls == 0u &&
            dispatch.dispatchRootTicks == 0u &&
            dispatch.worldFastEligibleIgnoringIdentity == 0u &&
            dispatch.worldFastBlockedByIdentity == 0u &&
            dispatch.getTagStageCalls == 0u &&
            dispatch.getTagStageHits == 0u &&
            dispatch.getTagStageMisses == 0u &&
            dispatch.getTagStageConflicts == 0u &&
            dispatch.getTagStageProbes == 0u &&
            dispatch.getTagStageTicks == 0u && extendedZero;
        dispatch.closureClean = dispatch.captureRequested
            ? dispatch.finalized && dispatch.dispatchPathClosureClean &&
                dispatch.dispatchRootClosureClean &&
                dispatch.worldFastClosureClean &&
                dispatch.getTagStageClosureClean &&
                dispatch.pairedTimingClosureClean &&
                dispatch.flushTopologyClosureClean
            : !dispatch.finalized && periodicDispatchZero;
      };
      finalizeDispatchCapture(event.periodicDispatch);
      finalizeDispatchCapture(event.postPeriodicControl);
      event.pairLifecycleClosureClean =
          event.periodicDispatch.captureRequested &&
          event.periodicDispatch.finalized &&
          event.postPeriodicControl.captureRequested &&
          event.postPeriodicControl.finalized &&
          event.frameSerial != UINT64_MAX &&
          event.collectionFrameSerial == event.frameSerial + 1u &&
          event.periodicDispatch.captureFrameSerial ==
              event.collectionFrameSerial &&
          event.periodicDispatch.captureFrameSerial != UINT64_MAX &&
          event.postPeriodicControl.captureFrameSerial ==
              event.periodicDispatch.captureFrameSerial + 1u &&
          event.periodicDispatch.ownerThreadId != 0u &&
          event.periodicDispatch.ownerThreadId ==
              event.postPeriodicControl.ownerThreadId;
      const bool periodicGetTagQpcBounded =
          event.periodicDispatch.getTagStageCalls <= UINT64_MAX / 2u &&
          event.periodicDispatch.qpcReadCount >=
              2u * event.periodicDispatch.getTagStageCalls;
      const bool controlGetTagQpcBounded =
          event.postPeriodicControl.getTagStageCalls <= UINT64_MAX / 2u &&
          event.postPeriodicControl.qpcReadCount >=
              2u * event.postPeriodicControl.getTagStageCalls;
      event.pairQpcBalancedExcludingGetTag =
          periodicGetTagQpcBounded && controlGetTagQpcBounded &&
          event.periodicDispatch.qpcReadCount -
                  2u * event.periodicDispatch.getTagStageCalls ==
              event.postPeriodicControl.qpcReadCount -
                  2u * event.postPeriodicControl.getTagStageCalls;
      event.pairQpcBalancedIncludingGetTag =
          event.periodicDispatch.qpcReadCount ==
              event.postPeriodicControl.qpcReadCount;
      event.pairTopologyComparable = event.pairLifecycleClosureClean &&
          event.periodicDispatch.closureClean &&
          event.postPeriodicControl.closureClean &&
          event.periodicDispatch.flushTopologyCalls > 0u &&
          event.postPeriodicControl.flushTopologyCalls > 0u &&
          event.periodicDispatch.stageTimings[static_cast<uint32_t>(
              WorldObjectsPhase1PairedTimingStage::WorldHookInclusive)]
                  .calls > 0u &&
          event.postPeriodicControl.stageTimings[static_cast<uint32_t>(
              WorldObjectsPhase1PairedTimingStage::WorldHookInclusive)]
                  .calls > 0u &&
          event.periodicDispatch.stageTimings[static_cast<uint32_t>(
              WorldObjectsPhase1PairedTimingStage::WorldHookInclusive)]
                  .calls ==
              event.postPeriodicControl.stageTimings[static_cast<uint32_t>(
                  WorldObjectsPhase1PairedTimingStage::WorldHookInclusive)]
                      .calls &&
          event.periodicDispatch.stageTimings[static_cast<uint32_t>(
              WorldObjectsPhase1PairedTimingStage::DispatchRoot)]
                  .calls > 0u &&
          event.postPeriodicControl.stageTimings[static_cast<uint32_t>(
              WorldObjectsPhase1PairedTimingStage::DispatchRoot)]
                  .calls > 0u &&
          event.periodicDispatch.dispatchRootCalls ==
              event.postPeriodicControl.dispatchRootCalls &&
          event.periodicDispatch.commonCalls ==
              event.postPeriodicControl.commonCalls &&
          event.periodicDispatch.specialCalls ==
              event.postPeriodicControl.specialCalls &&
          event.periodicDispatch.group0Calls ==
              event.postPeriodicControl.group0Calls &&
          event.periodicDispatch.otherStageCalls ==
              event.postPeriodicControl.otherStageCalls &&
          event.periodicDispatch.worldFastEligibleIgnoringIdentity ==
              event.postPeriodicControl.worldFastEligibleIgnoringIdentity &&
          event.postPeriodicControl.worldFastBlockedByIdentity == 0u &&
          event.periodicDispatch.flushTopologyCalls ==
              event.postPeriodicControl.flushTopologyCalls &&
          event.periodicDispatch.opaqueCountTotal ==
              event.postPeriodicControl.opaqueCountTotal &&
          event.periodicDispatch.transparentCountTotal ==
              event.postPeriodicControl.transparentCountTotal &&
          event.periodicDispatch.flushTopologyHash ==
              event.postPeriodicControl.flushTopologyHash &&
          event.periodicDispatch.flushTerminalCounts ==
              event.postPeriodicControl.flushTerminalCounts &&
          event.pairQpcBalancedExcludingGetTag;
      constexpr uint32_t kValidObservedGroupMask =
          (1u << kWorldObjectsPhase1GroupCount) - 1u;
      const uint32_t rawObservedGroupMask =
          event.collectorGroupMask | event.hookGroupMask;
      event.observedGroupMask =
          rawObservedGroupMask & kValidObservedGroupMask;
      event.groupClosureClean = true;
      event.unobservedGroupsZeroClean = true;
      uint64_t periodicHookCalls = 0u;
      uint64_t periodicHookTicks = 0u;
      uint64_t periodicCollectorCalls = 0u;
      uint64_t periodicCollectorTicks = 0u;
      for (uint32_t group = 0u;
           group < kWorldObjectsPhase1GroupCount; ++group) {
        auto& eventGroup = event.groups[group];
        periodicHookCalls += eventGroup.hookCalls;
        periodicHookTicks += eventGroup.hookInclusiveTicks;
        periodicCollectorCalls += eventGroup.collectorCalls;
        periodicCollectorTicks += eventGroup.collectorInclusiveTicks;
        const uint32_t groupBit = 1u << group;
        eventGroup.observed =
            (event.observedGroupMask & groupBit) != 0u;
        uint64_t outcomeCalls = 0u;
        for (uint32_t outcome = 0u;
             outcome < kWorldObjectsPhase1CollectorOutcomeCount; ++outcome) {
          outcomeCalls += eventGroup.outcomeCounts[outcome];
        }
        eventGroup.outcomeClosureClean =
            outcomeCalls == eventGroup.collectorCalls;
        eventGroup.collectorPartitionClean =
            eventGroup.collectorInclusiveTicks ==
                eventGroup.collectorSetupTicks +
                eventGroup.collectorIterateTicks +
                eventGroup.collectorRegisterTicks +
                eventGroup.collectorTailTicks;
        eventGroup.hookCollectorCallClosureClean =
            eventGroup.hookCalls == eventGroup.collectorCalls;
        eventGroup.hookContainsCollector =
            eventGroup.hookInclusiveTicks >=
                eventGroup.collectorInclusiveTicks;
        eventGroup.registerContainsFeeds =
            eventGroup.collectorRegisterTicks >=
                eventGroup.modelFeedTicks + eventGroup.shadowFeedTicks &&
            eventGroup.modelFeedCalls == eventGroup.shadowFeedCalls;
        eventGroup.entryCountBoundsClean =
            eventGroup.acceptedEntries <= eventGroup.listEntries &&
            eventGroup.sceneNodeEntries <= eventGroup.acceptedEntries &&
            eventGroup.handleEntries <= eventGroup.acceptedEntries;
        const bool zeroResidual =
            eventGroup.hookInclusiveTicks == 0u &&
            eventGroup.collectorInclusiveTicks == 0u &&
            eventGroup.collectorSetupTicks == 0u &&
            eventGroup.collectorIterateTicks == 0u &&
            eventGroup.collectorRegisterTicks == 0u &&
            eventGroup.collectorTailTicks == 0u &&
            eventGroup.modelFeedTicks == 0u &&
            eventGroup.shadowFeedTicks == 0u &&
            eventGroup.listEntries == 0u &&
            eventGroup.acceptedEntries == 0u &&
            eventGroup.sceneNodeEntries == 0u &&
            eventGroup.handleEntries == 0u &&
            eventGroup.hookCalls == 0u &&
            eventGroup.collectorCalls == 0u &&
            eventGroup.modelFeedCalls == 0u &&
            eventGroup.shadowFeedCalls == 0u &&
            outcomeCalls == 0u;
        eventGroup.unobservedZeroClean =
            eventGroup.observed || zeroResidual;
        if (eventGroup.observed) {
          event.groupClosureClean = event.groupClosureClean &&
              eventGroup.hookCalls != 0u &&
              eventGroup.collectorCalls != 0u &&
              eventGroup.outcomeClosureClean &&
              eventGroup.collectorPartitionClean &&
              eventGroup.hookCollectorCallClosureClean &&
              eventGroup.hookContainsCollector &&
              eventGroup.registerContainsFeeds &&
              eventGroup.entryCountBoundsClean;
        } else {
          event.unobservedGroupsZeroClean =
              event.unobservedGroupsZeroClean && zeroResidual;
          event.groupClosureClean =
              event.groupClosureClean && zeroResidual;
        }
      }
      event.completeObservedGroups =
          rawObservedGroupMask == event.observedGroupMask &&
          event.collectorGroupMask == event.hookGroupMask &&
          event.duplicateCollectorGroupMask == 0u &&
          event.duplicateHookGroupMask == 0u &&
          event.groupClosureClean &&
          event.unobservedGroupsZeroClean;
      const auto& periodicWorldHook =
          event.periodicDispatch.stageTimings[static_cast<uint32_t>(
              WorldObjectsPhase1PairedTimingStage::WorldHookInclusive)];
      const auto& periodicWorldCollector =
          event.periodicDispatch.stageTimings[static_cast<uint32_t>(
              WorldObjectsPhase1PairedTimingStage::WorldCollector)];
      event.periodicEventSubsetClosureClean =
          periodicWorldCollector.calls == periodicCollectorCalls &&
          periodicWorldCollector.ticks == periodicCollectorTicks &&
          periodicWorldHook.calls >= periodicHookCalls &&
          periodicWorldHook.ticks >= periodicHookTicks;
      event.pairTopologyComparable = event.pairTopologyComparable &&
          event.completeObservedGroups &&
          event.periodicEventSubsetClosureClean;
      event.pairComparable = event.pairTopologyComparable;
      summary.retainedEventsClosureClean =
          summary.retainedEventsClosureClean &&
          event.trackingPartitionClean && event.completeObservedGroups &&
          event.groupClosureClean;
      summary.events[summary.retainedEventCount++] = event;
    }
    summary.retainedEventsClosureClean =
        summary.retainedEventsClosureClean &&
        summary.missingRetainedEventCount == 0u &&
        summary.retainedEventCount == summary.retainedEventExpectedCount;
  }

  summary.snapshotWritesStartedAfter =
      g_worldObjectsPhase1WritesStarted.load(std::memory_order_acquire);
  summary.snapshotWritesCompletedAfter =
      g_worldObjectsPhase1WritesCompleted.load(std::memory_order_acquire);
  summary.snapshotWritersAfter =
      g_worldObjectsPhase1Writers.load(std::memory_order_acquire);
  summary.snapshotGenerationAfter =
      g_worldObjectsPhase1SnapshotGeneration.load(
          std::memory_order_acquire);
  summary.snapshotStable =
      summary.snapshotGenerationBefore == summary.snapshotGenerationAfter &&
      (summary.snapshotGenerationAfter & 1u) == 0u &&
      summary.snapshotWritersBefore == 0u &&
      summary.snapshotWritersAfter == 0u &&
      summary.snapshotWritesStartedBefore ==
          summary.snapshotWritesCompletedBefore &&
      summary.snapshotWritesStartedAfter ==
          summary.snapshotWritesCompletedAfter &&
      summary.snapshotWritesStartedBefore ==
          summary.snapshotWritesStartedAfter &&
      summary.snapshotWritesCompletedBefore ==
          summary.snapshotWritesCompletedAfter;
  summary.lifecycleClosureClean =
      summary.collectorWithoutEventCount == 0u &&
      summary.collectorReentryCount == 0u &&
      summary.collectorWithoutHookCount == 0u &&
      summary.hookWithoutCollectorCount == 0u &&
      summary.registryFeedOutsideCollectorCount == 0u &&
      summary.unexpectedGroupCount == 0u &&
      summary.pairedCaptureDuplicatePublishCount == 0u &&
      summary.pairedCaptureLostPublishCount == 0u &&
      summary.pairedCaptureSlotMismatchCount == 0u;
  summary.overallClosureClean =
      summary.snapshotStable &&
      summary.trackingHealthPathClosureClean &&
      summary.trackingReasonClosureClean &&
      summary.eventCountClosureClean &&
      summary.lifecycleClosureClean &&
      summary.lifetimeObservedGroupsClosureClean &&
      summary.lifetimeUnobservedGroupsZeroClean &&
      summary.retainedEventsClosureClean;
  for (const auto& group : summary.groups) {
    if (group.observed) {
      summary.overallClosureClean = summary.overallClosureClean &&
          group.outcomeClosureClean &&
          group.collectorPartitionClean &&
          group.hookCollectorCallClosureClean &&
          group.containmentClean &&
          group.entryCountBoundsClean;
    } else {
      summary.overallClosureClean = summary.overallClosureClean &&
          group.unobservedZeroClean;
    }
  }
  return summary;
}

void NoteShadowRuntimeRenderObject(const RenderObjectInfo& info) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().noteRenderObject(info);
  ShadowObjectRegistry::instance().noteRenderObject(info);
}

void NoteShadowRuntimeRenderObjectsBatch(
    const std::vector<const RenderObjectInfo*>& infos) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled || infos.empty())
    return;

  const bool phase1Capture =
      IsWorldObjectsPhase1CollectorCaptureActive();
  const int64_t modelBegin = phase1Capture
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  model::ModelInstanceRegistry::instance().noteRenderObjectsBatch(infos);
  const int64_t shadowBegin = phase1Capture
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  ShadowObjectRegistry::instance().noteRenderObjectsBatch(infos);
  if (phase1Capture) {
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    RecordWorldObjectsPhase1RegistryFeed(
        WorldObjectsPhase1TickDelta(modelBegin, shadowBegin),
        WorldObjectsPhase1TickDelta(shadowBegin, end));
  }
}

void NoteShadowRuntimeIdentity(void* worldObjectEntry, void* sceneNode,
                               void* unitPtr, void* spritePtr,
                               uint32_t jHandle, uint32_t rawcode,
                               ObjectKind kind) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().noteInstanceIdentity(
      worldObjectEntry, sceneNode, unitPtr, spritePtr, jHandle, rawcode);
  ShadowObjectRegistry::instance().noteInstanceIdentity(
      worldObjectEntry, sceneNode, unitPtr, spritePtr, jHandle, rawcode, kind);
}

void NoteShadowRuntimeModelBinding(void* spritePtr, void* runtimeModelPtr,
                                   void* modelResourcePtr,
                                   const std::string& modelPath,
                                   uint32_t modelType, uint32_t modelFlags,
                                   uint64_t modelKey) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::ModelInstanceRegistry::instance().bindRuntimeModelToSprite(
      spritePtr, runtimeModelPtr, modelKey, modelResourcePtr);
  ShadowObjectRegistry::instance().noteModelBinding(
      spritePtr, runtimeModelPtr, modelResourcePtr, modelPath, modelType,
      modelFlags, modelKey);
}

void NoteShadowRuntimePose(void* runtimeModelPtr, void* sceneNode, void* unitPtr,
                           uint32_t sequenceId, float sequenceTime, float scale,
                           float yaw, float pitch, float roll, float height,
                           bool hasWorldTransform,
                           const Matrix4* worldTransform, uint32_t matrixCount,
                           uint64_t matrixHash) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::PoseRegistry::instance().recordPose(
      runtimeModelPtr, sceneNode, unitPtr, sequenceId, sequenceTime, scale, yaw,
      pitch, roll, height, hasWorldTransform, worldTransform);
  ShadowObjectRegistry::instance().notePose(
      runtimeModelPtr, sceneNode, unitPtr, sequenceId, sequenceTime, scale, yaw,
      pitch, roll, height, hasWorldTransform, worldTransform, matrixCount,
      matrixHash);
}

void NoteShadowSceneStats(const War3ShadowCaptureStats& stats) {
  std::unique_lock<std::shared_mutex> lock(g_shadowSceneStatsMutex);
  War3ShadowCaptureStats merged = stats;
  const auto& previous = g_shadowSceneStats;
  const auto hasReceiverDetails = [](const War3ShadowCaptureStats& value) {
    return value.semanticSceneShadowMapDrawnCasters != 0u ||
           value.semanticSceneShadowMapPreparedDrawCount != 0u ||
           value.semanticSceneShadowMapAlphaTestPreparedCount != 0u ||
           value.semanticSceneShadowMapDynamicPreparedCount != 0u ||
           value.semanticSceneShadowMapStaticPreparedCount != 0u ||
           value.semanticSceneShadowMapCascade0DrawnCount != 0u ||
           value.semanticSceneShadowMapSkinnedPreparedCount != 0u ||
           value.semanticSceneShadowMapSkinnedInvalidBufferCount != 0u ||
           value.semanticSceneShadowMapSkinnedInvalidPipelineCount != 0u ||
           value.semanticSceneShadowMapSkinnedDrawnCount != 0u ||
           value.semanticSceneReceiverInputValid != 0u ||
           value.semanticSceneReceiverInputRejectReason != 0u ||
           value.semanticSceneReceiverNeedPass != 0u ||
           value.semanticSceneReceiverNeedShadowMap != 0u ||
           value.semanticSceneReceiverActiveStrengthMilli != 0u ||
           value.semanticSceneReceiverUboStrengthMilli != 0u ||
           value.semanticSceneReceiverDebugMode != 0u ||
           value.semanticSceneReceiverCsmCascadeCount != 0u ||
           value.semanticSceneReceiverRunEntryFlags != 0u ||
           value.semanticSceneReceiverRunEarlyReturnReason != 0u ||
           value.semanticSceneShadowMapExecutedThisFrame != 0u ||
           value.semanticSceneReceiverSettingsShadowsEnabled != 0u ||
           value.semanticSceneReceiverSettingsOutlineEnabled != 0u ||
           value.semanticSceneReceiverSettingsRawStrengthMilli != 0u ||
           value.semanticSceneReceiverComputedShadowStrengthMilli != 0u ||
           value.semanticSceneReceiverHasSunShadow != 0u ||
           value.semanticSceneReceiverHasPointShadow != 0u ||
           value.semanticSceneReceiverNeedOutlinePass != 0u ||
           value.semanticSceneReceiverZeroStrengthFrameCount != 0u ||
           value.semanticSceneReceiverDrawnWithZeroStrengthCount != 0u ||
           value.semanticSceneReceiverNoCompleteShadowMapCount != 0u ||
           value.semanticSceneReceiverNoShadowMapImageCount != 0u ||
           value.semanticSceneReceiverNoShadowMapSampleViewCount != 0u ||
           value.semanticSceneReceiverNoCandidateCsmCount != 0u ||
           value.semanticSceneReceiverCsmFallbackToLastGoodCount != 0u ||
           value.semanticSceneReceiverHoldInvalidCsmCount != 0u ||
           value.semanticSceneReceiverHoldEmptyReplayCount != 0u ||
           value.semanticSceneReceiverHoldIdentityChurnCount != 0u ||
           value.semanticSceneReceiverReuseInvalidatedAfterEnsureCount != 0u ||
           value.semanticSceneShadowMapRenderSkippedNoResourcesCount != 0u ||
           value.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount != 0u ||
           value.semanticSceneShadowMatrixSceneKey != 0u ||
           value.semanticSceneShadowMatrixUploadSerial != 0u ||
           value.semanticSceneShadowMapRenderSerial != 0u ||
           value.semanticSceneShadowMapImagePtr != 0u ||
           value.semanticSceneShadowMapSampleViewPtr != 0u ||
           value.semanticSceneShadowTaaMode != 0u ||
           value.semanticSceneReceiverDrawExecutedThisFrame != 0u ||
           value.semanticSceneReceiverViewportWidth != 0u ||
           value.semanticSceneReceiverViewportHeight != 0u;
  };
  if (merged.semanticSceneReplayDrawsCount != 0u ||
      merged.semanticSceneShadowCastersCount != 0u) {
    const bool incomingHasReceiverDetails = hasReceiverDetails(merged);
    const bool previousHasReceiverDetails = hasReceiverDetails(previous);
    // The semantic scene is published once before the receiver pass runs and
    // again after receiver reconciliation is available. A pre-receiver publish
    // can legitimately contain replay draws but no receiver fields at all; do
    // not let that transient placeholder zero the last completed shadow-map /
    // receiver reconciliation, since hot status polling then reports a global
    // off-frame even though the replay input is non-empty.
    if (!incomingHasReceiverDetails && previousHasReceiverDetails) {
      merged.semanticSceneShadowMapDrawnCasters =
          previous.semanticSceneShadowMapDrawnCasters;
      merged.semanticSceneShadowMapCascadeCulledCount =
          previous.semanticSceneShadowMapCascadeCulledCount;
      merged.semanticSceneShadowMapPreparedDrawCount =
          previous.semanticSceneShadowMapPreparedDrawCount;
      merged.semanticSceneShadowMapAlphaTestPreparedCount =
          previous.semanticSceneShadowMapAlphaTestPreparedCount;
      merged.semanticSceneShadowMapAlphaPromotedPreparedCount =
          previous.semanticSceneShadowMapAlphaPromotedPreparedCount;
      merged.semanticSceneShadowMapDynamicPreparedCount =
          previous.semanticSceneShadowMapDynamicPreparedCount;
      merged.semanticSceneShadowMapStaticPreparedCount =
          previous.semanticSceneShadowMapStaticPreparedCount;
      merged.semanticSceneShadowMapOtherPreparedCount =
          previous.semanticSceneShadowMapOtherPreparedCount;
      merged.semanticSceneShadowMapCascade0DrawnCount =
          previous.semanticSceneShadowMapCascade0DrawnCount;
      merged.semanticSceneShadowMapCascade1DrawnCount =
          previous.semanticSceneShadowMapCascade1DrawnCount;
      merged.semanticSceneShadowMapCascade2DrawnCount =
          previous.semanticSceneShadowMapCascade2DrawnCount;
      merged.semanticSceneShadowMapCascade3DrawnCount =
          previous.semanticSceneShadowMapCascade3DrawnCount;
      merged.semanticSceneShadowMapCascade0CulledCount =
          previous.semanticSceneShadowMapCascade0CulledCount;
      merged.semanticSceneShadowMapCascade1CulledCount =
          previous.semanticSceneShadowMapCascade1CulledCount;
      merged.semanticSceneShadowMapCascade2CulledCount =
          previous.semanticSceneShadowMapCascade2CulledCount;
      merged.semanticSceneShadowMapCascade3CulledCount =
          previous.semanticSceneShadowMapCascade3CulledCount;
      merged.semanticSceneShadowMapSkinnedPreparedCount =
          previous.semanticSceneShadowMapSkinnedPreparedCount;
      merged.semanticSceneShadowMapSkinnedInvalidBufferCount =
          previous.semanticSceneShadowMapSkinnedInvalidBufferCount;
      merged.semanticSceneShadowMapSkinnedInvalidPipelineCount =
          previous.semanticSceneShadowMapSkinnedInvalidPipelineCount;
      merged.semanticSceneShadowMapSkinnedDrawnCount =
          previous.semanticSceneShadowMapSkinnedDrawnCount;
      merged.semanticSceneShadowTaaActive =
          previous.semanticSceneShadowTaaActive;
      merged.semanticSceneReceiverReuseShadowMap =
          previous.semanticSceneReceiverReuseShadowMap;
      merged.semanticSceneReceiverInputValid =
          previous.semanticSceneReceiverInputValid;
      merged.semanticSceneReceiverInputRejectReason =
          previous.semanticSceneReceiverInputRejectReason;
      merged.semanticSceneReceiverNeedPass =
          previous.semanticSceneReceiverNeedPass;
      merged.semanticSceneReceiverNeedShadowMap =
          previous.semanticSceneReceiverNeedShadowMap;
      merged.semanticSceneReceiverHasCompleteShadowMap =
          previous.semanticSceneReceiverHasCompleteShadowMap;
      merged.semanticSceneReceiverHasUsableDirectionalShadow =
          previous.semanticSceneReceiverHasUsableDirectionalShadow;
      merged.semanticSceneReceiverActiveStrengthMilli =
          previous.semanticSceneReceiverActiveStrengthMilli;
      merged.semanticSceneReceiverUboStrengthMilli =
          previous.semanticSceneReceiverUboStrengthMilli;
      merged.semanticSceneReceiverDebugMode =
          previous.semanticSceneReceiverDebugMode;
      merged.semanticSceneReceiverCsmCascadeCount =
          previous.semanticSceneReceiverCsmCascadeCount;
      merged.semanticSceneReceiverRunEntryFlags =
          previous.semanticSceneReceiverRunEntryFlags;
      merged.semanticSceneReceiverRunEarlyReturnReason =
          previous.semanticSceneReceiverRunEarlyReturnReason;
      merged.semanticSceneShadowMapExecutedThisFrame =
          previous.semanticSceneShadowMapExecutedThisFrame;
      merged.semanticSceneReceiverSettingsShadowsEnabled =
          previous.semanticSceneReceiverSettingsShadowsEnabled;
      merged.semanticSceneReceiverSettingsOutlineEnabled =
          previous.semanticSceneReceiverSettingsOutlineEnabled;
      merged.semanticSceneReceiverSettingsRawStrengthMilli =
          previous.semanticSceneReceiverSettingsRawStrengthMilli;
      merged.semanticSceneReceiverComputedShadowStrengthMilli =
          previous.semanticSceneReceiverComputedShadowStrengthMilli;
      merged.semanticSceneReceiverHasSunShadow =
          previous.semanticSceneReceiverHasSunShadow;
      merged.semanticSceneReceiverHasPointShadow =
          previous.semanticSceneReceiverHasPointShadow;
      merged.semanticSceneReceiverNeedOutlinePass =
          previous.semanticSceneReceiverNeedOutlinePass;
      merged.semanticSceneReceiverZeroStrengthFrameCount =
          previous.semanticSceneReceiverZeroStrengthFrameCount;
      merged.semanticSceneReceiverDrawnWithZeroStrengthCount =
          previous.semanticSceneReceiverDrawnWithZeroStrengthCount;
      merged.semanticSceneReceiverNoCompleteShadowMapCount =
          previous.semanticSceneReceiverNoCompleteShadowMapCount;
      merged.semanticSceneReceiverNoShadowMapImageCount =
          previous.semanticSceneReceiverNoShadowMapImageCount;
      merged.semanticSceneReceiverNoShadowMapSampleViewCount =
          previous.semanticSceneReceiverNoShadowMapSampleViewCount;
      merged.semanticSceneReceiverNoCandidateCsmCount =
          previous.semanticSceneReceiverNoCandidateCsmCount;
      merged.semanticSceneReceiverCsmFallbackToLastGoodCount =
          previous.semanticSceneReceiverCsmFallbackToLastGoodCount;
      merged.semanticSceneReceiverHoldInvalidCsmCount =
          previous.semanticSceneReceiverHoldInvalidCsmCount;
      merged.semanticSceneReceiverHoldEmptyReplayCount =
          previous.semanticSceneReceiverHoldEmptyReplayCount;
      merged.semanticSceneReceiverHoldIdentityChurnCount =
          previous.semanticSceneReceiverHoldIdentityChurnCount;
      merged.semanticSceneReceiverReuseInvalidatedAfterEnsureCount =
          previous.semanticSceneReceiverReuseInvalidatedAfterEnsureCount;
      merged.semanticSceneShadowMapRenderSkippedNoResourcesCount =
          previous.semanticSceneShadowMapRenderSkippedNoResourcesCount;
      merged.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount =
          previous.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount;
      merged.semanticSceneShadowMatrixSceneKey =
          previous.semanticSceneShadowMatrixSceneKey;
      merged.semanticSceneShadowMatrixUploadSerial =
          previous.semanticSceneShadowMatrixUploadSerial;
      merged.semanticSceneShadowMatrixBufferObjectPtr =
          previous.semanticSceneShadowMatrixBufferObjectPtr;
      merged.semanticSceneShadowMatrixBufferOffset =
          previous.semanticSceneShadowMatrixBufferOffset;
      merged.semanticSceneShadowMatrixBufferSize =
          previous.semanticSceneShadowMatrixBufferSize;
      merged.semanticSceneShadowMatrixBufferGpuAddress =
          previous.semanticSceneShadowMatrixBufferGpuAddress;
      merged.semanticSceneReceiverCameraHash =
          previous.semanticSceneReceiverCameraHash;
      merged.semanticSceneReceiverSunDirectionHash =
          previous.semanticSceneReceiverSunDirectionHash;
      merged.semanticSceneReceiverCsmHash =
          previous.semanticSceneReceiverCsmHash;
      merged.semanticSceneReceiverCameraDeltaNano =
          previous.semanticSceneReceiverCameraDeltaNano;
      merged.semanticSceneReceiverSunDeltaNano =
          previous.semanticSceneReceiverSunDeltaNano;
      merged.semanticSceneReceiverCsmDeltaNano =
          previous.semanticSceneReceiverCsmDeltaNano;
      merged.semanticSceneReceiverSnappedCenterDeltaTexelsNano =
          previous.semanticSceneReceiverSnappedCenterDeltaTexelsNano;
      merged.semanticSceneReceiverTexelSizeDeltaNano =
          previous.semanticSceneReceiverTexelSizeDeltaNano;
      merged.semanticSceneReplayBackingHash =
          previous.semanticSceneReplayBackingHash;
      merged.semanticSceneStage13ReplayContentHash =
          previous.semanticSceneStage13ReplayContentHash;
      merged.semanticSceneStage13ReplayBackingHash =
          previous.semanticSceneStage13ReplayBackingHash;
      merged.semanticSceneStage13ReplayDrawCount =
          previous.semanticSceneStage13ReplayDrawCount;
      merged.semanticSceneShadowMapRenderSerial =
          previous.semanticSceneShadowMapRenderSerial;
      merged.semanticSceneShadowMapImagePtr =
          previous.semanticSceneShadowMapImagePtr;
      merged.semanticSceneShadowMapSampleViewPtr =
          previous.semanticSceneShadowMapSampleViewPtr;
      merged.semanticSceneShadowCurrentImagePtr =
          previous.semanticSceneShadowCurrentImagePtr;
      merged.semanticSceneShadowCurrentViewPtr =
          previous.semanticSceneShadowCurrentViewPtr;
      merged.semanticSceneShadowHistoryReadImagePtr =
          previous.semanticSceneShadowHistoryReadImagePtr;
      merged.semanticSceneShadowHistoryReadViewPtr =
          previous.semanticSceneShadowHistoryReadViewPtr;
      merged.semanticSceneShadowHistoryWriteImagePtr =
          previous.semanticSceneShadowHistoryWriteImagePtr;
      merged.semanticSceneShadowHistoryWriteViewPtr =
          previous.semanticSceneShadowHistoryWriteViewPtr;
      merged.semanticSceneShadowVisibilityExecutedThisFrame =
          previous.semanticSceneShadowVisibilityExecutedThisFrame;
      merged.semanticSceneReceiverDrawExecutedThisFrame =
          previous.semanticSceneReceiverDrawExecutedThisFrame;
      merged.semanticSceneShadowTaaMode =
          previous.semanticSceneShadowTaaMode;
      merged.semanticSceneShadowHistoryValidBefore =
          previous.semanticSceneShadowHistoryValidBefore;
      merged.semanticSceneShadowHistoryValidAfter =
          previous.semanticSceneShadowHistoryValidAfter;
      merged.semanticSceneShadowHistoryReadIndex =
          previous.semanticSceneShadowHistoryReadIndex;
      merged.semanticSceneShadowHistoryWriteIndex =
          previous.semanticSceneShadowHistoryWriteIndex;
      merged.semanticSceneShadowHistoryAdvancedThisFrame =
          previous.semanticSceneShadowHistoryAdvancedThisFrame;
      merged.semanticSceneShadowHistoryAdvanceSkippedIncomplete =
          previous.semanticSceneShadowHistoryAdvanceSkippedIncomplete;
      merged.semanticSceneShadowHistoryInvalidationMask =
          previous.semanticSceneShadowHistoryInvalidationMask;
      merged.semanticSceneShadowReceiverSampleSource =
          previous.semanticSceneShadowReceiverSampleSource;
      merged.semanticSceneReceiverViewportX =
          previous.semanticSceneReceiverViewportX;
      merged.semanticSceneReceiverViewportY =
          previous.semanticSceneReceiverViewportY;
      merged.semanticSceneReceiverViewportWidth =
          previous.semanticSceneReceiverViewportWidth;
      merged.semanticSceneReceiverViewportHeight =
          previous.semanticSceneReceiverViewportHeight;
    }
  }

  // The command-list tail republishes its immutable pre-receiver scene copy
  // after Run() has already published the real receiver reconciliation. That
  // copy can carry older non-zero receiver fields, so hasReceiverDetails()
  // alone cannot identify it, while these newly added per-frame outcomes are
  // still both zero. Under the corrected contract a Temporal frame always has
  // exactly one terminal outcome: advance after all four passes complete, or
  // skip because the write was incomplete. PrepassCurrentOnly intentionally
  // has no history outcome and must not inherit a preceding Temporal result.
  const bool missingTaaHistoryOutcome =
      merged.semanticSceneShadowTaaActive != 0u &&
      merged.semanticSceneShadowTaaMode >= 2u &&
      merged.semanticSceneShadowHistoryAdvancedThisFrame == 0u &&
      merged.semanticSceneShadowHistoryAdvanceSkippedIncomplete == 0u;
  if (missingTaaHistoryOutcome) {
    merged.semanticSceneShadowHistoryAdvancedThisFrame =
        previous.semanticSceneShadowHistoryAdvancedThisFrame;
    merged.semanticSceneShadowHistoryAdvanceSkippedIncomplete =
        previous.semanticSceneShadowHistoryAdvanceSkippedIncomplete;
    merged.semanticSceneShadowHistoryInvalidationMask |=
        previous.semanticSceneShadowHistoryInvalidationMask;
  }

  // 主线程会在 receiver 命令真正执行前发布一次场景占位统计；该占位对象中的
  // VS-S1 字段为 0，绝不能把 CS 线程已经发布的单调累计值清回 0。
  merged.gpuSkinVsShadowDirectAttempts = std::max(
      merged.gpuSkinVsShadowDirectAttempts,
      previous.gpuSkinVsShadowDirectAttempts);
  merged.gpuSkinVsShadowDirectInputRejects = std::max(
      merged.gpuSkinVsShadowDirectInputRejects,
      previous.gpuSkinVsShadowDirectInputRejects);
  merged.gpuSkinVsShadowDirectStateRejects = std::max(
      merged.gpuSkinVsShadowDirectStateRejects,
      previous.gpuSkinVsShadowDirectStateRejects);
  merged.gpuSkinVsShadowDirectDrawsSubmitted = std::max(
      merged.gpuSkinVsShadowDirectDrawsSubmitted,
      previous.gpuSkinVsShadowDirectDrawsSubmitted);
  merged.gpuSkinVsShadowDirectBindingsCleared = std::max(
      merged.gpuSkinVsShadowDirectBindingsCleared,
      previous.gpuSkinVsShadowDirectBindingsCleared);
  merged.gpuSkinVsShadowReplayDirectional = std::max(
      merged.gpuSkinVsShadowReplayDirectional,
      previous.gpuSkinVsShadowReplayDirectional);
  merged.gpuSkinVsShadowReplayPoint = std::max(
      merged.gpuSkinVsShadowReplayPoint,
      previous.gpuSkinVsShadowReplayPoint);
  merged.gpuSkinVsShadowReplayUnknown = std::max(
      merged.gpuSkinVsShadowReplayUnknown,
      previous.gpuSkinVsShadowReplayUnknown);
  g_shadowSceneStats = merged;
  ++g_shadowSceneStatsPublishCount;
}

GpuSkinVsShadowRuntimeCounters QueryGpuSkinVsShadowRuntimeCounters() {
  std::shared_lock<std::shared_mutex> lock(g_shadowSceneStatsMutex);
  GpuSkinVsShadowRuntimeCounters result;
  result.directAttempts = g_shadowSceneStats.gpuSkinVsShadowDirectAttempts;
  result.directInputRejects =
      g_shadowSceneStats.gpuSkinVsShadowDirectInputRejects;
  result.directStateRejects =
      g_shadowSceneStats.gpuSkinVsShadowDirectStateRejects;
  result.directDrawsSubmitted =
      g_shadowSceneStats.gpuSkinVsShadowDirectDrawsSubmitted;
  result.directBindingsCleared =
      g_shadowSceneStats.gpuSkinVsShadowDirectBindingsCleared;
  result.replayDirectional =
      g_shadowSceneStats.gpuSkinVsShadowReplayDirectional;
  result.replayPoint = g_shadowSceneStats.gpuSkinVsShadowReplayPoint;
  result.replayUnknown = g_shadowSceneStats.gpuSkinVsShadowReplayUnknown;
  return result;
}

void NoteFinalShadowCasterFrame(
    const War3FrameScene& scene,
    const std::vector<const War3ShadowCasterDraw*>& draws,
    uint64_t frameSerial) {
  if (frameSerial == 0u)
    return;

  std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
  auto config = ShadowPoseFullTraceConfigLocked();
  if (!config.enabled || !config.includeFinalCasterRecords)
    return;
  if (ShadowPoseFullTraceDeadlineReachedLocked()) {
    g_shadowPoseFullTraceStoppedByLimit = true;
    CloseShadowPoseFullTraceLocked();
    Logger::info("DXVK War3Shadow: shadow pose full trace stopped by "
                 "duration limit");
    return;
  }
  if (g_shadowPoseFullTraceLastFinalCasterFrameSerial == frameSerial &&
      g_shadowPoseFullTraceLastFinalCasterScene == &scene) {
    return;
  }
  if (!EnsureShadowPoseFullTraceOpenLocked(config))
    return;

  std::array<uint32_t, kWar3ShadowStageHistogramBinCount> stageCounts = {};
  std::array<uint32_t, kWar3ShadowCategoryHistogramBinCount>
      categoryCounts = {};
  uint64_t identityXor = 0u;
  uint64_t identitySum = 0u;
  uint64_t backingXor = 0u;
  uint64_t contentXor = 0u;
  uint32_t nullDrawCount = 0u;
  uint32_t validationRejectCandidateCount = 0u;

  for (const War3ShadowCasterDraw* draw : draws) {
    if (draw == nullptr) {
      ++nullDrawCount;
      continue;
    }
    const size_t stageBin =
        draw->stage >= 0 &&
                draw->stage <
                    static_cast<int16_t>(kWar3ShadowStageHistogramStageCount)
            ? static_cast<size_t>(draw->stage)
            : kWar3ShadowStageHistogramBinCount - 1u;
    ++stageCounts[stageBin];
    const size_t categoryBin =
        (std::min)(size_t(static_cast<uint32_t>(draw->category)),
                   kWar3ShadowCategoryHistogramBinCount - 1u);
    ++categoryCounts[categoryBin];

    const uint64_t identityHash = ShadowCasterTraceIdentityHash(*draw);
    identityXor ^= identityHash;
    identitySum += identityHash;
    backingXor ^= ShadowCasterTraceBackingHash(*draw);
    contentXor ^=
        ShadowCasterTraceContentHash(*draw, config.finalCasterSampleBytes);
    if (ShadowCasterTraceValidationFlags(*draw) != 0u)
      ++validationRejectCandidateCount;
  }

  const uint32_t recordLimit =
      config.maxFinalCasterRecords == 0u
          ? static_cast<uint32_t>(
                (std::min)(draws.size(), size_t(0xFFFFFFFFu)))
          : static_cast<uint32_t>(
                (std::min)(draws.size(),
                           size_t(config.maxFinalCasterRecords)));
  g_shadowPoseFullTraceStream
      << "{\"type\":\"shadowFinalCasterFrame\""
      << ",\"version\":1"
      << ",\"epoch\":" << config.epoch
      << ",\"frameSerial\":" << frameSerial
      << ",\"worldCameraValid\":" << (scene.worldCamera.valid ? 1 : 0)
      << ",\"worldCameraFrameSerial\":" << scene.worldCamera.frameSerial
      << ",\"shadowCastersSize\":" << scene.shadowCasters.size()
      << ",\"shadowInstancesSize\":" << scene.shadowInstances.size()
      << ",\"shadowFallbacksSize\":" << scene.shadowFallbacks.size()
      << ",\"finalDrawCount\":" << draws.size()
      << ",\"recordCount\":" << recordLimit
      << ",\"truncated\":"
      << (recordLimit < draws.size() ? 1 : 0)
      << ",\"nullDrawCount\":" << nullDrawCount
      << ",\"validationRejectCandidateCount\":"
      << validationRejectCandidateCount;
  WritePtrField(g_shadowPoseFullTraceStream, "scenePtr", &scene);
  WriteHexField(g_shadowPoseFullTraceStream, "identityXor", identityXor);
  WriteHexField(g_shadowPoseFullTraceStream, "identitySum", identitySum);
  WriteHexField(g_shadowPoseFullTraceStream, "backingXor", backingXor);
  WriteHexField(g_shadowPoseFullTraceStream, "contentXor", contentXor);
  g_shadowPoseFullTraceStream << ",\"stageCounts\":[";
  for (size_t i = 0u; i < stageCounts.size(); ++i) {
    if (i != 0u)
      g_shadowPoseFullTraceStream << ',';
    g_shadowPoseFullTraceStream << stageCounts[i];
  }
  g_shadowPoseFullTraceStream << "],\"categoryCounts\":[";
  for (size_t i = 0u; i < categoryCounts.size(); ++i) {
    if (i != 0u)
      g_shadowPoseFullTraceStream << ',';
    g_shadowPoseFullTraceStream << categoryCounts[i];
  }
  g_shadowPoseFullTraceStream << "]}\n";
  ++g_shadowPoseFullTraceFrameEventsWritten;

  for (uint32_t i = 0u; i < recordLimit; ++i) {
    if (draws[i] == nullptr)
      continue;
    WriteFinalShadowCasterRecordEvent(
        g_shadowPoseFullTraceStream, config.epoch, frameSerial, i,
        *draws[i], config.finalCasterSampleBytes);
    ++g_shadowPoseFullTraceRecordEventsWritten;
  }
  g_shadowPoseFullTraceStream.flush();
  g_shadowPoseFullTraceLastFinalCasterFrameSerial = frameSerial;
  g_shadowPoseFullTraceLastFinalCasterScene = &scene;
}

void NoteCurrentDrawSnapshotFrame(
    const std::vector<CurrentDrawContractRecord>& records,
    uint64_t frameSerial) {
  if (frameSerial == 0u)
    return;

  std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
  auto config = ShadowPoseFullTraceConfigLocked();
  if (!config.enabled || !config.includeCurrentDrawRecords)
    return;
  if (ShadowPoseFullTraceDeadlineReachedLocked()) {
    g_shadowPoseFullTraceStoppedByLimit = true;
    CloseShadowPoseFullTraceLocked();
    Logger::info("DXVK War3Shadow: shadow pose full trace stopped by "
                 "duration limit");
    return;
  }
  if (!EnsureShadowPoseFullTraceOpenLocked(config))
    return;

  const uint32_t recordLimit =
      config.maxCurrentDrawRecords == 0u
          ? uint32_t(std::min<size_t>(
                records.size(), size_t(std::numeric_limits<uint32_t>::max())))
          : uint32_t(std::min<size_t>(records.size(),
                                     config.maxCurrentDrawRecords));
  g_shadowPoseFullTraceStream
      << "{\"type\":\"shadowCurrentDrawSnapshotFrame\""
      << ",\"version\":1"
      << ",\"epoch\":" << config.epoch
      << ",\"frameSerial\":" << frameSerial
      << ",\"recordCount\":" << records.size()
      << ",\"writtenRecordCount\":" << recordLimit
      << ",\"truncated\":" << (recordLimit < records.size() ? 1 : 0)
      << "}\n";
  ++g_shadowPoseFullTraceFrameEventsWritten;
  for (uint32_t i = 0u; i < recordLimit; ++i) {
    WriteCurrentDrawRecordEvent(g_shadowPoseFullTraceStream, config.epoch,
                                frameSerial, i, records[i]);
    ++g_shadowPoseFullTraceRecordEventsWritten;
  }
  g_shadowPoseFullTraceStream.flush();
}

void NoteShadowFrameCadenceSample(uint64_t frameIndex,
                                  const War3ShadowCaptureStats& stats) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;

  const auto currentDraw = QueryCurrentDrawContractDiagnosticsSummary();

  ShadowRuntimeCadenceSample sample = {};
  sample.frameIndex = frameIndex;
  sample.sceneFrameSerial = stats.semanticSceneLastFrameSerial;
  sample.selectedFrameSerial = stats.semanticSceneLastSelectedFrameSerial;
  sample.reusableFrameSerial = stats.semanticSceneLastReusableFrameSerial;
  sample.sourcePublishRevision = stats.semanticSceneLastSourcePublishRevision;
  sample.targetPublishRevision = stats.semanticSceneLastTargetPublishRevision;
  sample.populateReturnReason = stats.semanticScenePopulateLastReturnReason;
  sample.inputDrawCount = stats.semanticSceneLastInputDrawCount;
  sample.inputSkinnedCount = stats.semanticSceneLastInputSkinnedCount;
  sample.submittedDrawCount = stats.semanticSceneLastSubmittedDrawCount;
  sample.submittedSkinnedCount = stats.semanticSceneSubmittedSkinned;
  sample.directSubmittedRecordCount =
      stats.semanticSceneDirectLastSubmittedRecordCount;
  sample.directSubmittedObjectCount =
      stats.semanticSceneDirectLastSubmittedObjectCount;
  sample.shadowCastersCount = stats.semanticSceneShadowCastersCount;
  sample.replayDrawsCount = stats.semanticSceneReplayDrawsCount;
  sample.shadowMapDrawnCasters = stats.semanticSceneShadowMapDrawnCasters;
  sample.shadowMapSkinnedDrawnCount =
      stats.semanticSceneShadowMapSkinnedDrawnCount;
  sample.receiverNeedShadowMap = stats.semanticSceneReceiverNeedShadowMap;
  sample.receiverHasCompleteShadowMap =
      stats.semanticSceneReceiverHasCompleteShadowMap;
  sample.receiverReuseShadowMap = stats.semanticSceneReceiverReuseShadowMap;
  sample.shadowMapExecutedThisFrame =
      stats.semanticSceneShadowMapExecutedThisFrame;
  sample.receiverRunEarlyReturnReason =
      stats.semanticSceneReceiverRunEarlyReturnReason;
  sample.receiverRunEntryFlags = stats.semanticSceneReceiverRunEntryFlags;
  sample.receiverActiveStrengthMilli =
      stats.semanticSceneReceiverActiveStrengthMilli;
  sample.receiverCsmCascadeCount = stats.semanticSceneReceiverCsmCascadeCount;
  sample.receiverHoldInvalidCsm =
      stats.semanticSceneReceiverHoldInvalidCsmCount;
  sample.receiverHoldEmptyReplay =
      stats.semanticSceneReceiverHoldEmptyReplayCount;
  sample.receiverHoldIdentityChurn =
      stats.semanticSceneReceiverHoldIdentityChurnCount;
  sample.dynamicPoseSignature = stats.dynamicPoseSignature;
  sample.submittedIdentityHash =
      stats.semanticSceneDirectLastSubmittedIdentityHash;
  sample.lastSubmittedPaletteHash =
      stats.semanticSceneDirectLastSubmittedPaletteHash;
  sample.lastSubmittedGroupHash =
      stats.semanticSceneDirectLastSubmittedGroupHash;
  sample.currentDrawPublishReadyCount = currentDraw.publishReadyCount;
  sample.currentDrawQueryHitCount = currentDraw.queryHitCount;
  sample.currentDrawLastRenderFrameIndex = currentDraw.lastRenderFrameIndex;
  sample.currentDrawLastFrameTag = currentDraw.lastFrameTag;
  sample.submitPaletteContentAgeSampleCount =
      currentDraw.submitPaletteContentAgeSampleCount;
  sample.submitPaletteContentAgeLag3PlusCount =
      currentDraw.submitPaletteContentAgeLag3To5Count +
      currentDraw.submitPaletteContentAgeLag6PlusCount;
  sample.shadowMatrixSceneKey = stats.semanticSceneShadowMatrixSceneKey;
  sample.shadowMatrixUploadSerial =
      stats.semanticSceneShadowMatrixUploadSerial;
  sample.shadowMatrixBufferObjectPtr =
      stats.semanticSceneShadowMatrixBufferObjectPtr;
  sample.shadowMatrixBufferOffset =
      stats.semanticSceneShadowMatrixBufferOffset;
  sample.shadowMatrixBufferSize =
      stats.semanticSceneShadowMatrixBufferSize;
  sample.shadowMatrixBufferGpuAddress =
      stats.semanticSceneShadowMatrixBufferGpuAddress;
  sample.receiverCameraHash =
      stats.semanticSceneReceiverCameraHash;
  sample.receiverSunDirectionHash =
      stats.semanticSceneReceiverSunDirectionHash;
  sample.receiverCsmHash =
      stats.semanticSceneReceiverCsmHash;
  sample.receiverCameraDeltaNano =
      stats.semanticSceneReceiverCameraDeltaNano;
  sample.receiverSunDeltaNano =
      stats.semanticSceneReceiverSunDeltaNano;
  sample.receiverCsmDeltaNano =
      stats.semanticSceneReceiverCsmDeltaNano;
  sample.receiverSnappedCenterDeltaTexelsNano =
      stats.semanticSceneReceiverSnappedCenterDeltaTexelsNano;
  sample.receiverTexelSizeDeltaNano =
      stats.semanticSceneReceiverTexelSizeDeltaNano;
  sample.replayBackingHash =
      stats.semanticSceneReplayBackingHash;
  sample.stage13ReplayContentHash =
      stats.semanticSceneStage13ReplayContentHash;
  sample.stage13ReplayBackingHash =
      stats.semanticSceneStage13ReplayBackingHash;
  sample.stage13ReplayDrawCount =
      stats.semanticSceneStage13ReplayDrawCount;
  sample.shadowMapRenderSerial = stats.semanticSceneShadowMapRenderSerial;
  sample.shadowMapImagePtr = stats.semanticSceneShadowMapImagePtr;
  sample.shadowMapSampleViewPtr =
      stats.semanticSceneShadowMapSampleViewPtr;
  sample.shadowCurrentImagePtr =
      stats.semanticSceneShadowCurrentImagePtr;
  sample.shadowCurrentViewPtr = stats.semanticSceneShadowCurrentViewPtr;
  sample.shadowHistoryReadImagePtr =
      stats.semanticSceneShadowHistoryReadImagePtr;
  sample.shadowHistoryReadViewPtr =
      stats.semanticSceneShadowHistoryReadViewPtr;
  sample.shadowHistoryWriteImagePtr =
      stats.semanticSceneShadowHistoryWriteImagePtr;
  sample.shadowHistoryWriteViewPtr =
      stats.semanticSceneShadowHistoryWriteViewPtr;
  sample.shadowVisibilityExecutedThisFrame =
      stats.semanticSceneShadowVisibilityExecutedThisFrame;
  sample.receiverDrawExecutedThisFrame =
      stats.semanticSceneReceiverDrawExecutedThisFrame;
  sample.shadowTaaMode = stats.semanticSceneShadowTaaMode;
  sample.shadowHistoryValidBefore =
      stats.semanticSceneShadowHistoryValidBefore;
  sample.shadowHistoryValidAfter =
      stats.semanticSceneShadowHistoryValidAfter;
  sample.shadowHistoryReadIndex = stats.semanticSceneShadowHistoryReadIndex;
  sample.shadowHistoryWriteIndex =
      stats.semanticSceneShadowHistoryWriteIndex;
  sample.shadowHistoryAdvancedThisFrame =
      stats.semanticSceneShadowHistoryAdvancedThisFrame;
  sample.shadowHistoryAdvanceSkippedIncomplete =
      stats.semanticSceneShadowHistoryAdvanceSkippedIncomplete;
  sample.shadowHistoryInvalidationMask =
      stats.semanticSceneShadowHistoryInvalidationMask;
  sample.shadowReceiverSampleSource =
      stats.semanticSceneShadowReceiverSampleSource;

  {
    std::lock_guard<std::mutex> lock(g_shadowCadenceMutex);
    sample.serial = ++g_shadowCadenceNextSerial;
    ++g_shadowCadenceSampleCountTotal;

    if (sample.dynamicPoseSignature != 0u &&
        g_shadowCadenceLastDynamicPoseSignature ==
            sample.dynamicPoseSignature) {
      ++g_shadowCadenceSameDynamicPoseStreak;
    } else {
      g_shadowCadenceSameDynamicPoseStreak = 0u;
    }
    if (sample.dynamicPoseSignature != 0u)
      g_shadowCadenceLastDynamicPoseSignature = sample.dynamicPoseSignature;
    g_shadowCadenceSameDynamicPoseStreakMax = std::max(
        g_shadowCadenceSameDynamicPoseStreakMax,
        g_shadowCadenceSameDynamicPoseStreak);

    if (sample.sceneFrameSerial != 0u &&
        g_shadowCadenceLastSceneFrameSerial == sample.sceneFrameSerial) {
      ++g_shadowCadenceSameSceneFrameStreak;
    } else {
      g_shadowCadenceSameSceneFrameStreak = 0u;
    }
    if (sample.sceneFrameSerial != 0u)
      g_shadowCadenceLastSceneFrameSerial = sample.sceneFrameSerial;
    g_shadowCadenceSameSceneFrameStreakMax =
        std::max(g_shadowCadenceSameSceneFrameStreakMax,
                 g_shadowCadenceSameSceneFrameStreak);

    const bool shadowMapReused =
        sample.receiverReuseShadowMap != 0u ||
        (sample.receiverNeedShadowMap != 0u &&
         sample.receiverHasCompleteShadowMap != 0u &&
         sample.shadowMapExecutedThisFrame == 0u);
    if (shadowMapReused)
      ++g_shadowCadenceShadowMapReuseStreak;
    else
      g_shadowCadenceShadowMapReuseStreak = 0u;
    g_shadowCadenceShadowMapReuseStreakMax =
        std::max(g_shadowCadenceShadowMapReuseStreakMax,
                 g_shadowCadenceShadowMapReuseStreak);

    g_shadowCadenceSamples[g_shadowCadenceWriteIndex] = sample;
    g_shadowCadenceWriteIndex =
        (g_shadowCadenceWriteIndex + 1u) %
        static_cast<uint32_t>(kShadowRuntimeCadenceSampleCapacity);
    if (g_shadowCadenceSampleCount < kShadowRuntimeCadenceSampleCapacity)
      ++g_shadowCadenceSampleCount;
  }

  MaybeWriteShadowPoseFullTrace(sample, stats, currentDraw);
}

void StartShadowPoseFullTrace(uint32_t maxSeconds, bool includeMatrixBytes,
                              uint32_t maxPoseRecords,
                              uint32_t maxShadowObjectRecords,
                              uint32_t maxCurrentDrawRecords) {
  std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
  InitializeShadowPoseFullTraceEnvLocked();
  CloseShadowPoseFullTraceLocked();
  g_shadowPoseFullTraceManualEnabled = true;
  g_shadowPoseFullTraceStoppedByLimit = false;
  g_shadowPoseFullTraceIncludePoseRecords = true;
  g_shadowPoseFullTraceIncludeShadowObjectRecords = true;
  g_shadowPoseFullTraceIncludeCurrentDrawRecords = true;
  g_shadowPoseFullTraceIncludeFinalCasterRecords = true;
  g_shadowPoseFullTraceIncludeMatrixBytes = includeMatrixBytes;
  g_shadowPoseFullTraceMaxSeconds = maxSeconds == 0u ? 15u : maxSeconds;
  g_shadowPoseFullTraceMaxPoseRecords = maxPoseRecords;
  g_shadowPoseFullTraceMaxShadowObjectRecords = maxShadowObjectRecords;
  g_shadowPoseFullTraceMaxCurrentDrawRecords = maxCurrentDrawRecords;
  g_shadowPoseFullTraceMaxFinalCasterRecords = 0u;
  g_shadowPoseFullTraceFrameEventsWritten = 0u;
  g_shadowPoseFullTraceRecordEventsWritten = 0u;
  g_shadowPoseFullTraceLastFinalCasterFrameSerial = 0u;
  g_shadowPoseFullTraceLastFinalCasterScene = nullptr;
  g_shadowPoseFullTraceStart = {};
  g_shadowPoseFullTracePath.clear();
  ++g_shadowPoseFullTraceEpoch;
}

void StopShadowPoseFullTrace() {
  std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
  g_shadowPoseFullTraceManualEnabled = false;
  g_shadowPoseFullTraceEnvEnabled = false;
  g_shadowPoseFullTraceStoppedByLimit = false;
  g_shadowPoseFullTraceLastFinalCasterFrameSerial = 0u;
  g_shadowPoseFullTraceLastFinalCasterScene = nullptr;
  CloseShadowPoseFullTraceLocked();
}

ShadowPoseFullTraceStatus QueryShadowPoseFullTraceStatus() {
  std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
  const auto config = ShadowPoseFullTraceConfigLocked();

  ShadowPoseFullTraceStatus status = {};
  status.enabled = config.enabled;
  status.active = config.enabled && g_shadowPoseFullTraceOpened &&
                  g_shadowPoseFullTraceStream.is_open();
  status.includePoseRecords = config.includePoseRecords;
  status.includeShadowObjectRecords = config.includeShadowObjectRecords;
  status.includeCurrentDrawRecords = config.includeCurrentDrawRecords;
  status.includeFinalCasterRecords = config.includeFinalCasterRecords;
  status.includeMatrixBytes = config.includeMatrixBytes;
  status.stoppedByLimit = g_shadowPoseFullTraceStoppedByLimit;
  status.maxSeconds = config.maxSeconds;
  status.maxPoseRecords = config.maxPoseRecords;
  status.maxShadowObjectRecords = config.maxShadowObjectRecords;
  status.maxCurrentDrawRecords = config.maxCurrentDrawRecords;
  status.maxFinalCasterRecords = config.maxFinalCasterRecords;
  status.traceEpoch = config.epoch;
  status.frameEventsWritten = g_shadowPoseFullTraceFrameEventsWritten;
  status.recordEventsWritten = g_shadowPoseFullTraceRecordEventsWritten;
  status.path = g_shadowPoseFullTracePath;
  return status;
}

void NoteSemanticDataPerf(SemanticDataPerfTag tag, uint64_t durationUs) {
  const auto index = static_cast<size_t>(tag);
  if (index == 0u || index >= kSemanticPerfTagCount)
    return;
  g_semanticPerfCalls[index].fetch_add(1u, std::memory_order_relaxed);
  g_semanticPerfUs[index].fetch_add(durationUs, std::memory_order_relaxed);

  uint64_t lastHotUs =
      g_semanticLastHotFunctionUs.load(std::memory_order_relaxed);
  while (durationUs > lastHotUs &&
         !g_semanticLastHotFunctionUs.compare_exchange_weak(
             lastHotUs, durationUs, std::memory_order_relaxed)) {
  }
  if (durationUs >= lastHotUs)
    g_semanticLastHotFunctionTag.store(static_cast<uint64_t>(tag),
                                       std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStageCandidate(int stage, int a3, int a4, int a5,
                                           bool jassReady, bool gameStarted) {
  g_nativeSemanticWorldStageCandidateCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (stage == dxvk::war3::internal::kNativeSemanticShadowPrepareStage) {
    g_nativeSemanticWorldStageCandidatePrepareCount.fetch_add(
        1u, std::memory_order_relaxed);
  } else if (
      stage ==
      dxvk::war3::internal::kNativeSemanticShadowRefreshPrepareStage) {
    g_nativeSemanticWorldStageCandidateRefreshCount.fetch_add(
        1u, std::memory_order_relaxed);
  } else if (stage ==
             dxvk::war3::internal::kNativeSemanticShadowExecuteStage) {
    g_nativeSemanticWorldStageCandidateExecuteCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_nativeSemanticWorldStageLastCandidateStage.store(
      static_cast<uint64_t>(stage), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA3.store(
      static_cast<uint32_t>(a3), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA4.store(
      static_cast<uint32_t>(a4), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA5.store(
      static_cast<uint32_t>(a5), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateJassReady.store(
      jassReady ? 1u : 0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateGameStarted.store(
      gameStarted ? 1u : 0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateRuntimeFrame.store(
      CurrentRuntimeBridgeFrame(), std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStageSkippedRuntimeNotReady(int stage) {
  (void)stage;
  g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount.fetch_add(
      1u, std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStagePrepare(int stage, bool success) {
  g_nativeSemanticWorldStagePrepareAttemptCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (success) {
    g_nativeSemanticWorldStagePrepareSuccessCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  auto& nativeRuntime = shadow::NativeD3D9BackendRuntime::instance();
  const bool nativePrepared = nativeRuntime.buildLatestFrame();
  if (nativePrepared &&
      dxvk::war3::internal::
          IsNativeRendererHostExecuteValidationRuntimeEnabled()) {
    const auto nativeBeforeExecute = nativeRuntime.snapshot();
    const bool needsCanonicalCatchup =
        nativeBeforeExecute.submittedDrawCount != 0u &&
        nativeBeforeExecute.frameSerial !=
            nativeBeforeExecute.lastSuccessfulExecutedFrameSerial;
    if (needsCanonicalCatchup)
      nativeRuntime.executePreparedFrame();
  }
  const auto nativeSummary = nativeRuntime.snapshot();
  g_nativeSemanticWorldStageLastPrepareStage.store(
      static_cast<uint64_t>(stage), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareFrameSerial.store(
      nativeSummary.frameSerial, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareDrawCount.store(
      nativeSummary.submittedDrawCount, std::memory_order_relaxed);
}

void NoteNativeSemanticWorldStageExecute(int stage, bool success) {
  g_nativeSemanticWorldStageExecuteAttemptCount.fetch_add(
      1u, std::memory_order_relaxed);
  if (success) {
    g_nativeSemanticWorldStageExecuteSuccessCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  const auto nativeSummary =
      shadow::NativeD3D9BackendRuntime::instance().snapshot();
  const uint64_t executedDrawCount =
      nativeSummary.lastSuccessfulExecutedDrawCount != 0u
          ? nativeSummary.lastSuccessfulExecutedDrawCount
          : nativeSummary.executedDrawCount;
  const uint64_t executedFrameSerial =
      nativeSummary.lastSuccessfulExecutedFrameSerial != 0u
          ? nativeSummary.lastSuccessfulExecutedFrameSerial
          : nativeSummary.executedFrameSerial;
  g_nativeSemanticWorldStageLastExecuteStage.store(
      static_cast<uint64_t>(stage), std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteFrameSerial.store(
      executedFrameSerial, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteDrawCount.store(
      executedDrawCount, std::memory_order_relaxed);
}

void NoteShadowRuntimeSpriteFramePose(void* runtimeModelPtr, void* spritePtr,
                                      void* sceneNode, void* unitPtr, float dt,
                                      uint32_t sequenceId, float sequenceTime,
                                      float scale, float yaw, float pitch,
                                      float roll, float height,
                                      bool hasWorldTransform,
                                      const Matrix4* worldTransform,
                                      uint32_t matrixCount,
                                      uint64_t matrixHash) {
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return;
  model::PoseRegistry::instance().recordSpriteFramePose(
      runtimeModelPtr, spritePtr, sceneNode, unitPtr, dt, sequenceId,
      sequenceTime, scale, yaw, pitch, roll, height, hasWorldTransform,
      worldTransform);
  ShadowObjectRegistry::instance().noteSpriteFramePose(
      runtimeModelPtr, spritePtr, sceneNode, unitPtr, dt, sequenceId,
      sequenceTime, scale, yaw, pitch, roll, height, hasWorldTransform,
      worldTransform, matrixCount, matrixHash);
}

ShadowRuntimeBridgeSummary QueryShadowRuntimeBridgeSummary(
    bool refreshSemanticFrameIfStale) {
  ShadowRuntimeBridgeSummary summary = {};
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return summary;
  const auto semanticSkipReason = CurrentSemanticBuildSkippedReason();
  const auto contractStats =
      shadow::ShadowRuntimeContractCache::instance().snapshotStats();
  summary.semanticDataModuleEnabled = IsSemanticDataModuleEnabled() ? 1u : 0u;
  summary.semanticModelProducerEnabled =
      IsSemanticModelProducerEnabled() ? 1u : 0u;
  summary.semanticPoseProducerEnabled =
      (IsSemanticModelProducerEnabled() &&
       internal::kWar3RuntimeConfigSemanticPoseProducerEffective)
          ? 1u
          : 0u;
  summary.semanticAttachmentProducerEnabled =
      (IsSemanticModelProducerEnabled() &&
       internal::kWar3RuntimeConfigSemanticAttachmentProducerEffective)
          ? 1u
          : 0u;
  summary.semanticFrameRegistriesEnabled =
      IsSemanticFrameRegistriesEnabled() ? 1u : 0u;
  summary.semanticContractCaptureEnabled =
      IsSemanticContractCaptureEnabled() ? 1u : 0u;
  summary.semanticConsumerEnabled = IsSemanticConsumerEnabled() ? 1u : 0u;
  summary.semanticBuildSkippedReason =
      static_cast<uint64_t>(semanticSkipReason);
  auto& validationRuntime = shadow::ShadowValidationRuntime::instance();
  if (refreshSemanticFrameIfStale &&
      semanticSkipReason == SemanticBuildSkippedReason::None) {
    bool alreadyFresh = false;
    const auto preCore = validationRuntime.snapshot();
    const auto preBuildState = validationRuntime.buildStateSnapshot();
    const auto preBundle =
        shadow::ShadowRuntimeContractCache::instance().snapshotBundleShared();
    const uint64_t targetFrameSerial =
        preBundle.valid() && preBundle.manifest != nullptr
            ? preBundle.manifest->frameSerial
            : 0u;
    const uint64_t targetPublishRevision =
        preBundle.valid() && preBundle.manifest != nullptr
            ? preBundle.manifest->publishRevision
            : 0u;
    if (preBundle.valid() && preBundle.manifest != nullptr &&
        preCore.frameSerial != 0u &&
        preCore.sourcePublishRevision != 0u &&
        !preBuildState.buildInProgress &&
        !preBuildState.buildRequestPending) {
      const uint64_t frameLag =
          preBundle.manifest->frameSerial >= preCore.frameSerial
              ? preBundle.manifest->frameSerial - preCore.frameSerial
              : 0u;
      const uint64_t publishRevisionLag =
          preBundle.manifest->publishRevision >= preCore.sourcePublishRevision
              ? preBundle.manifest->publishRevision -
                    preCore.sourcePublishRevision
              : 0u;
      const bool requiresExactPublishRevision =
          preBundle.stats.rootUnitSupplementAppended != 0u ||
          preBundle.stats.rootUnitSupplementReusedFromPrior != 0u;
      alreadyFresh =
          frameLag <= 1u &&
          (requiresExactPublishRevision ? publishRevisionLag == 0u
                                        : publishRevisionLag <= 16u);
    }

    if (alreadyFresh) {
      g_semanticConsumerBuildSkippedFresh.fetch_add(
          1u, std::memory_order_relaxed);
    } else {
      const bool buildAlreadyActive =
          preBuildState.buildInProgress || preBuildState.buildRequestPending;
      const bool sameSummaryRefresh =
          targetFrameSerial != 0u && targetPublishRevision != 0u &&
          g_semanticSummaryRefreshFrameSerial.load(std::memory_order_relaxed) ==
              targetFrameSerial &&
          g_semanticSummaryRefreshPublishRevision.load(
              std::memory_order_relaxed) == targetPublishRevision;
      if (buildAlreadyActive || sameSummaryRefresh) {
        g_semanticConsumerBuildSkippedFresh.fetch_add(
            1u, std::memory_order_relaxed);
      } else {
        g_semanticSummaryRefreshFrameSerial.store(
            targetFrameSerial, std::memory_order_relaxed);
        g_semanticSummaryRefreshPublishRevision.store(
            targetPublishRevision, std::memory_order_relaxed);
      const auto semanticRefreshStart = std::chrono::steady_clock::now();
      // scene-submission 模式下只发“异步追最新 contract”的请求。
      // control-plane 不能同步替 render thread 消费 semantic build；否则
      // pipe 请求会把 buildFrameChunk 压到控制线程上，低压图 tail 状态下
      // 很容易复现 3s 响应超时。真正的消费必须发生在 scene submit /
      // EndFrame 的 render-thread 小步推进里。
      validationRuntime.requestLatestFrameBuild();

      const auto semanticRefreshElapsed =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - semanticRefreshStart)
              .count();
      NoteSemanticDataPerf(
          SemanticDataPerfTag::SummaryRefreshRequest,
          semanticRefreshElapsed > 0
              ? static_cast<uint64_t>(semanticRefreshElapsed)
              : 0u);
      }
    }
  }
  summary.runtimePoseHooksActive = model::IsPoseHookEnabled();
  summary.modelRegistryCount = model::ModelRegistry::instance().recordCount();
  summary.instanceRegistryCount =
      model::ModelInstanceRegistry::instance().recordCount();
  summary.runtimeBoundCount =
      model::ModelInstanceRegistry::instance().runtimeBoundCount();
  summary.runtimeCreationProvenanceCount =
      model::ModelInstanceRegistry::instance().runtimeCreationProvenanceCount();
  summary.runtimeResolveProvenanceCount =
      model::ModelInstanceRegistry::instance().runtimeResolveProvenanceCount();
  summary.runtimeOwnerIdentityCount =
      model::ModelInstanceRegistry::instance().runtimeOwnerIdentityCount();
  summary.completeIdentityCount =
      model::ModelInstanceRegistry::instance().completeIdentityCount();
  summary.poseReadyCount = model::PoseRegistry::instance().readyPoseCount();
  summary.spriteFramePoseCount =
      model::PoseRegistry::instance().spriteFramePoseCount();
  summary.matrixPaletteCount = (std::max)(
      uint64_t(model::PoseRegistry::instance().matrixPaletteCount()),
      contractStats.matrixPaletteCount);
  summary.shadowGeosetResourceCount =
      model::ShadowModelResourceCache::instance().geosetRecordCount();
  summary.shadowReadyGeosetCount =
      model::ShadowModelResourceCache::instance().readyGeosetCount();
  summary.shadowModelResourceCount =
      model::ShadowModelResourceCache::instance().modelResourceCount();
  summary.shadowRuntimeModelCount =
      model::ShadowModelResourceCache::instance().runtimeModelRecordCount();
  summary.visibleRenderableCount =
      VisibleRenderableRegistry::instance().getVisibleCount();
  summary.visibleRenderableMainCount =
      VisibleRenderableRegistry::instance().getMainQueueCount();
  summary.visibleRenderableTransparentCount =
      VisibleRenderableRegistry::instance().getTransparentCount();
  {
    const auto& visibleRecords =
        VisibleRenderableRegistry::instance().getAllVisibleView();
    auto& instanceRegistry = model::ModelInstanceRegistry::instance();
    auto& shadowRegistry = ShadowObjectRegistry::instance();
    auto& poseRegistry = model::PoseRegistry::instance();
    auto& resourceCache = model::ShadowModelResourceCache::instance();
    for (const auto& record : visibleRecords) {
      const bool isBuilding =
          record.identity.kind == ObjectKind::Building;
      const bool isDestructible =
          record.identity.kind == ObjectKind::Destructible;
      const bool maybeDoodadOrEffect =
          record.identity.kind != ObjectKind::Unit &&
          record.identity.kind != ObjectKind::Building &&
          record.identity.kind != ObjectKind::Destructible &&
          record.identity.unitPtr == nullptr &&
          (record.identity.groupIdx >= 2 || record.meshData != nullptr ||
           record.sceneNode != nullptr || record.identity.sceneNode != nullptr);
      const bool isStaticCandidate =
          isBuilding || isDestructible || maybeDoodadOrEffect;
      if (!isStaticCandidate)
        continue;

      summary.semanticStaticCandidateCount++;
      if (isBuilding)
        summary.semanticStaticCandidateBuildingCount++;
      if (isDestructible)
        summary.semanticStaticCandidateDestructibleCount++;
      if (maybeDoodadOrEffect)
        summary.semanticStaticCandidateMaybeDoodadOrEffectCount++;

      const bool hasStableIdentity = record.HasStableIdentity();
      const bool hasMeshData = record.meshData != nullptr;
      const bool hasRuntimeModel = record.runtimeModelPtr != nullptr;
      const bool hasModelResource =
          record.modelResourcePtr != nullptr || record.modelKey != 0u;
      const bool hasResolvedGeoset = record.HasResolvedGeoset();

      if (hasStableIdentity)
        summary.semanticStaticCandidateWithStableIdentity++;
      if (hasMeshData)
        summary.semanticStaticCandidateWithMeshData++;
      if (hasRuntimeModel)
        summary.semanticStaticCandidateWithRuntimeModel++;
      if (hasModelResource)
        summary.semanticStaticCandidateWithModelResource++;
      if (hasResolvedGeoset)
        summary.semanticStaticCandidateWithResolvedGeoset++;

      if (internal::kShadowSemanticCoreSceneUnitsOnly)
        summary.semanticStaticCandidateRejectedUnitsOnlyFilter++;
      if (!hasStableIdentity)
        summary.semanticStaticCandidateRejectedNoIdentity++;
      if (!hasMeshData)
        summary.semanticStaticCandidateRejectedNoMeshData++;
      if (!hasModelResource)
        summary.semanticStaticCandidateRejectedNoResource++;
      if (!hasResolvedGeoset)
        summary.semanticStaticCandidateRejectedNoGeoset++;
      if (maybeDoodadOrEffect && !isBuilding && !isDestructible)
        summary.semanticStaticCandidateRejectedNonCanonicalKind++;
    }
    auto writeVisibleSample = [&](const VisibleRenderableRecord& record,
                                  bool sample0) {
      const uint64_t worldObjectEntry =
          reinterpret_cast<uint64_t>(record.identity.worldObjectEntry);
      const uint64_t sceneNode = reinterpret_cast<uint64_t>(
          record.identity.sceneNode != nullptr ? record.identity.sceneNode
                                               : record.sceneNode);
      const uint64_t unitPtr =
          reinterpret_cast<uint64_t>(record.identity.unitPtr);
      const uint64_t runtimeModelPtr =
          reinterpret_cast<uint64_t>(record.runtimeModelPtr);
      const uint64_t modelResourcePtr =
          reinterpret_cast<uint64_t>(record.modelResourcePtr);
      const uint64_t runtimeGeosetPtr =
          reinterpret_cast<uint64_t>(record.runtimeGeosetPtr);
      const uint64_t runtimeGeosetDataPtr =
          reinterpret_cast<uint64_t>(record.runtimeGeosetDataPtr);
      model::ModelInstanceRecord instanceRecord = {};
      const bool instanceHit =
          sceneNode != 0u &&
          instanceRegistry.findBySceneNode(reinterpret_cast<void*>(sceneNode),
                                           instanceRecord);
      ShadowObjectRecord shadowRecord = {};
      const bool shadowHit =
          sceneNode != 0u &&
          shadowRegistry.findBySceneNode(reinterpret_cast<void*>(sceneNode),
                                         shadowRecord);
      model::PoseRecord poseRecord = {};
      const bool poseHit =
          sceneNode != 0u &&
          poseRegistry.findBySceneNode(reinterpret_cast<void*>(sceneNode),
                                       poseRecord);
      model::ShadowGeosetResourceRecord geosetRecord = {};
      bool geosetHit = false;
      if (record.runtimeGeosetDataPtr != nullptr) {
        geosetHit =
            resourceCache.findGeosetByData(record.runtimeGeosetDataPtr, geosetRecord);
      }
      if (!geosetHit && record.runtimeGeosetPtr != nullptr) {
        geosetHit =
            resourceCache.findGeosetByPtr(record.runtimeGeosetPtr, geosetRecord);
      }
      const uint64_t sceneInstanceRuntimeModelPtr = reinterpret_cast<uint64_t>(
          instanceHit ? instanceRecord.runtimeModelPtr : nullptr);
      const uint64_t sceneInstanceModelResourcePtr = reinterpret_cast<uint64_t>(
          instanceHit ? instanceRecord.modelResourcePtr : nullptr);
      const uint64_t sceneShadowRuntimeModelPtr = reinterpret_cast<uint64_t>(
          shadowHit ? shadowRecord.runtimeModelPtr : nullptr);
      const uint64_t sceneShadowModelResourcePtr = reinterpret_cast<uint64_t>(
          shadowHit ? shadowRecord.modelResourcePtr : nullptr);
      const uint64_t scenePoseMatrixCount =
          poseHit ? uint64_t(poseRecord.matrixCount) : 0u;
      const uint64_t geosetModelResourcePtr = reinterpret_cast<uint64_t>(
          geosetHit ? geosetRecord.modelResourcePtr : nullptr);
      const uint64_t geosetModelKey =
          geosetHit ? geosetRecord.modelKey : 0u;
      if (sample0) {
        summary.visibleRenderableSample0WorldObjectEntryPtr = worldObjectEntry;
        summary.visibleRenderableSample0SceneNodePtr = sceneNode;
        summary.visibleRenderableSample0UnitPtr = unitPtr;
        summary.visibleRenderableSample0JHandle = record.identity.jHandle;
        summary.visibleRenderableSample0Rawcode = record.identity.rawcode;
        summary.visibleRenderableSample0RuntimeModelPtr = runtimeModelPtr;
        summary.visibleRenderableSample0ModelResourcePtr = modelResourcePtr;
        summary.visibleRenderableSample0RuntimeGeosetPtr = runtimeGeosetPtr;
        summary.visibleRenderableSample0RuntimeGeosetDataPtr =
            runtimeGeosetDataPtr;
        summary.visibleRenderableSample0SceneInstanceRuntimeModelPtr =
            sceneInstanceRuntimeModelPtr;
        summary.visibleRenderableSample0SceneInstanceModelResourcePtr =
            sceneInstanceModelResourcePtr;
        summary.visibleRenderableSample0SceneShadowRuntimeModelPtr =
            sceneShadowRuntimeModelPtr;
        summary.visibleRenderableSample0SceneShadowModelResourcePtr =
            sceneShadowModelResourcePtr;
        summary.visibleRenderableSample0ScenePoseMatrixCount =
            scenePoseMatrixCount;
        summary.visibleRenderableSample0GeosetModelResourcePtr =
            geosetModelResourcePtr;
        summary.visibleRenderableSample0GeosetModelKey = geosetModelKey;
      } else {
        summary.visibleRenderableSample1WorldObjectEntryPtr = worldObjectEntry;
        summary.visibleRenderableSample1SceneNodePtr = sceneNode;
        summary.visibleRenderableSample1UnitPtr = unitPtr;
        summary.visibleRenderableSample1JHandle = record.identity.jHandle;
        summary.visibleRenderableSample1Rawcode = record.identity.rawcode;
        summary.visibleRenderableSample1RuntimeModelPtr = runtimeModelPtr;
        summary.visibleRenderableSample1ModelResourcePtr = modelResourcePtr;
        summary.visibleRenderableSample1RuntimeGeosetPtr = runtimeGeosetPtr;
        summary.visibleRenderableSample1RuntimeGeosetDataPtr =
            runtimeGeosetDataPtr;
        summary.visibleRenderableSample1SceneInstanceRuntimeModelPtr =
            sceneInstanceRuntimeModelPtr;
        summary.visibleRenderableSample1SceneInstanceModelResourcePtr =
            sceneInstanceModelResourcePtr;
        summary.visibleRenderableSample1SceneShadowRuntimeModelPtr =
            sceneShadowRuntimeModelPtr;
        summary.visibleRenderableSample1SceneShadowModelResourcePtr =
            sceneShadowModelResourcePtr;
        summary.visibleRenderableSample1ScenePoseMatrixCount =
            scenePoseMatrixCount;
        summary.visibleRenderableSample1GeosetModelResourcePtr =
            geosetModelResourcePtr;
        summary.visibleRenderableSample1GeosetModelKey = geosetModelKey;
      }
    };
    if (!visibleRecords.empty())
      writeVisibleSample(visibleRecords[0], true);
    if (visibleRecords.size() > 1u)
      writeVisibleSample(visibleRecords[1], false);
  }
  const auto sceneCollectorSummary = QuerySceneCollectorIdentityProbeSummary();
  summary.sceneCollectorGroupLocalAggregationEnabled =
      sceneCollectorSummary.groupLocalAggregationEnabled;
  summary.worldObjectListEntryCount =
      sceneCollectorSummary.worldObjectListEntryCount;
  summary.worldObjectListNullEntryCount =
      sceneCollectorSummary.worldObjectListNullEntryCount;
  summary.worldObjectListOwnerHintZeroCount =
      sceneCollectorSummary.worldObjectListOwnerHintZeroCount;
  summary.worldObjectListOwnerHintNonzeroCount =
      sceneCollectorSummary.worldObjectListOwnerHintNonzeroCount;
  summary.worldObjectListOwnerHintHandleCount =
      sceneCollectorSummary.worldObjectListOwnerHintHandleCount;
  summary.worldObjectListOwnerHintUnitPtrCount =
      sceneCollectorSummary.worldObjectListOwnerHintUnitPtrCount;
  summary.worldObjectListOwnerHintZeroContextAcceptedCount =
      sceneCollectorSummary.worldObjectListOwnerHintZeroContextAcceptedCount;
  summary.worldObjectListAcceptedIdentityCount =
      sceneCollectorSummary.worldObjectListAcceptedIdentityCount;
  summary.lastWorldObjectListEntryWorldObjectEntryPtr =
      sceneCollectorSummary.lastWorldObjectListEntryWorldObjectEntryPtr;
  summary.lastWorldObjectListEntryOwnerHintValue =
      sceneCollectorSummary.lastWorldObjectListEntryOwnerHintValue;
  summary.lastWorldObjectListEntrySceneNodePtr =
      sceneCollectorSummary.lastWorldObjectListEntrySceneNodePtr;
  const auto renderIdentitySummary =
      hooks::QueryRenderIdentityLifecycleProbeSummary();
  summary.renderIdentityFullDiagnostics =
      renderIdentitySummary.fullDiagnostics;
  summary.worldObjectListEntryWriteProbeHookInstalled =
      renderIdentitySummary.worldObjectListEntryWriteProbeHookInstalled;
  summary.worldObjectEntryRenderContextHookInstalled =
      renderIdentitySummary.worldObjectEntryRenderContextHookInstalled;
  summary.worldObjectEntryRenderPrePostProbeEnabled =
      renderIdentitySummary.worldObjectEntryRenderPrePostProbeEnabled;
  summary.renderQueueIdentityPrimingHookInstalled =
      renderIdentitySummary.renderQueueIdentityPrimingHookInstalled;
  summary.worldObjectEntryRenderCallCount =
      renderIdentitySummary.worldObjectEntryRenderCallCount;
  summary.worldObjectEntryRenderSceneNodeReadyBeforeCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeReadyBeforeCount;
  summary.worldObjectEntryRenderSceneNodeReadyAfterCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeReadyAfterCount;
  summary.worldObjectEntryRenderSceneNodeFilledByCallCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeFilledByCallCount;
  summary.worldObjectEntryRenderSceneNodeChangedCount =
      renderIdentitySummary.worldObjectEntryRenderSceneNodeChangedCount;
  summary.worldObjectEntryRenderKnownListOwnerHintZeroCount =
      renderIdentitySummary.worldObjectEntryRenderKnownListOwnerHintZeroCount;
  summary.worldObjectEntryRenderKnownListOwnerHintNonzeroCount =
      renderIdentitySummary.worldObjectEntryRenderKnownListOwnerHintNonzeroCount;
  summary.worldObjectEntryRenderUnknownListOwnerHintCount =
      renderIdentitySummary.worldObjectEntryRenderUnknownListOwnerHintCount;
  summary.worldObjectListEntryWriteCallCount =
      renderIdentitySummary.worldObjectListEntryWriteCallCount;
  summary.worldObjectListEntryWriteOwnerHintZeroCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintZeroCount;
  summary.worldObjectListEntryWriteOwnerHintNonzeroCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintNonzeroCount;
  summary.worldObjectListEntryWriteOwnerHintHandleCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintHandleCount;
  summary.worldObjectListEntryWriteOwnerHintUnitPtrCount =
      renderIdentitySummary.worldObjectListEntryWriteOwnerHintUnitPtrCount;
  summary.lastWorldObjectEntryRenderEntryPtr =
      renderIdentitySummary.lastWorldObjectEntryRenderEntryPtr;
  summary.lastWorldObjectEntryRenderResolvedListOwnerHintValue =
      renderIdentitySummary.lastWorldObjectEntryRenderResolvedListOwnerHintValue;
  summary.lastWorldObjectListEntryWriteListPtr =
      renderIdentitySummary.lastWorldObjectListEntryWriteListPtr;
  summary.lastWorldObjectListEntryWriteWorldObjectEntryPtr =
      renderIdentitySummary.lastWorldObjectListEntryWriteWorldObjectEntryPtr;
  summary.lastWorldObjectListEntryWriteOwnerHintValue =
      renderIdentitySummary.lastWorldObjectListEntryWriteOwnerHintValue;
  summary.lastWorldObjectEntryRenderSceneNodeBeforePtr =
      renderIdentitySummary.lastWorldObjectEntryRenderSceneNodeBeforePtr;
  summary.lastWorldObjectEntryRenderSceneNodeAfterPtr =
      renderIdentitySummary.lastWorldObjectEntryRenderSceneNodeAfterPtr;
  summary.shadowRuntimeBoundCount =
      ShadowObjectRegistry::instance().runtimeBoundCount();
  summary.shadowIdentityCount =
      ShadowObjectRegistry::instance().completeIdentityCount();
  summary.shadowPoseReadyCount =
      ShadowObjectRegistry::instance().poseReadyCount();
  const auto overrideSummary = model::QueryRuntimeOverrideOutputProbeSummary();
  auto& instanceRegistry = model::ModelInstanceRegistry::instance();
  summary.runtimeModelCtorCount = overrideSummary.runtimeModelCtorCount;
  summary.runtimeModelComplexCtorCount =
      overrideSummary.runtimeModelComplexCtorCount;
  summary.runtimeModelPlainCtorCount =
      overrideSummary.runtimeModelPlainCtorCount;
  summary.runtimeModelCtorCallerPromoteCount =
      overrideSummary.runtimeModelCtorCallerPromoteCount;
  summary.runtimeModelCtorCallerOtherCount =
      overrideSummary.runtimeModelCtorCallerOtherCount;
  summary.runtimeModelCreateCount = overrideSummary.runtimeModelCreateCount;
  summary.runtimeModelResolveCount = overrideSummary.runtimeModelResolveCount;
  summary.runtimeModelResolveResolvedIdentityCount =
      overrideSummary.runtimeModelResolveResolvedIdentityCount;
  summary.runtimeModelCreateCallerBuildChildLinksCount =
      overrideSummary.runtimeModelCreateCallerBuildChildLinksCount;
  summary.runtimeModelCreateCallerCreateSpriteRuntimeCount =
      overrideSummary.runtimeModelCreateCallerCreateSpriteRuntimeCount;
  summary.runtimeModelCreateCallerOtherCount =
      overrideSummary.runtimeModelCreateCallerOtherCount;
  summary.runtimeModelInitCopyCount =
      overrideSummary.runtimeModelInitCopyCount;
  summary.runtimeModelInitCopyPublishedFallbackCount =
      overrideSummary.runtimeModelInitCopyPublishedFallbackCount;
  summary.attachmentChildLineageBootstrapAttemptCount =
      overrideSummary.attachmentChildLineageBootstrapAttemptCount;
  summary.attachmentChildLineageBootstrapSuccessCount =
      overrideSummary.attachmentChildLineageBootstrapSuccessCount;
  summary.attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount =
      overrideSummary.attachmentChildLineageBootstrapByRuntimeBucketOrdinalCount;
  summary.attachmentChildLineageBootstrapMissNoModelDataLinksCount =
      overrideSummary.attachmentChildLineageBootstrapMissNoModelDataLinksCount;
  summary.attachmentChildLineageBootstrapMissNoUniqueChildCount =
      overrideSummary.attachmentChildLineageBootstrapMissNoUniqueChildCount;
  summary.runtimeSourceObjectCount = instanceRegistry.runtimeSourceObjectCount();
  const auto attachmentRecords =
      model::AttachmentRigidRegistry::instance().snapshot();
  summary.attachmentRigidCount = attachmentRecords.size();
  for (size_t i = 0u; i < attachmentRecords.size(); ++i) {
    const auto& attachment = attachmentRecords[i];
    uint32_t rootCreateCallerRva = 0u;
    uint32_t ownerCreateCallerRva = 0u;
    uint32_t childCreateCallerRva = 0u;
    void* rootCreateHandlePtr = nullptr;
    void* ownerCreateHandlePtr = nullptr;
    void* childCreateHandlePtr = nullptr;
    uint32_t rootResolveCallerRva = 0u;
    uint32_t ownerResolveCallerRva = 0u;
    uint32_t childResolveCallerRva = 0u;
    if (attachment.sourceObjectPtr != nullptr ||
        attachment.sourceSpriteObjectPtr != nullptr) {
      summary.attachmentRigidCountWithSourceObject++;
    }
    const bool hasAnyIdentity =
        attachment.worldObjectEntry != nullptr ||
        attachment.sceneNode != nullptr ||
        attachment.unitPtr != nullptr ||
        attachment.jHandle != 0u ||
        attachment.rawcode != 0u;
    if (hasAnyIdentity)
      summary.attachmentRigidCountWithAnyIdentity++;
    if (attachment.worldObjectEntry != nullptr)
      summary.attachmentRigidCountWithWorldObjectEntry++;
    if (attachment.sceneNode != nullptr)
      summary.attachmentRigidCountWithSceneNode++;
    if (attachment.unitPtr != nullptr)
      summary.attachmentRigidCountWithUnitPtr++;
    if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                     attachment.childRuntimeModelPtr,
                                     childCreateCallerRva)) {
      summary.attachmentRigidChildRuntimeCreateCallerKnownCount++;
    }
    if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                       attachment.childRuntimeModelPtr,
                                       childCreateHandlePtr,
                                       childResolveCallerRva)) {
      if (childCreateHandlePtr != nullptr)
        summary.attachmentRigidChildRuntimeCreateHandleKnownCount++;
      if (childResolveCallerRva != 0u)
        summary.attachmentRigidChildRuntimeResolveCallerKnownCount++;
    }
    if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                     attachment.ownerRuntimeModelPtr,
                                     ownerCreateCallerRva)) {
      summary.attachmentRigidOwnerRuntimeCreateCallerKnownCount++;
    }
    if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                       attachment.ownerRuntimeModelPtr,
                                       ownerCreateHandlePtr,
                                       ownerResolveCallerRva)) {
      if (ownerCreateHandlePtr != nullptr)
        summary.attachmentRigidOwnerRuntimeCreateHandleKnownCount++;
      if (ownerResolveCallerRva != 0u)
        summary.attachmentRigidOwnerRuntimeResolveCallerKnownCount++;
    }
    if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                     attachment.rootRuntimeModelPtr,
                                     rootCreateCallerRva)) {
      summary.attachmentRigidRootRuntimeCreateCallerKnownCount++;
    }
    if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                       attachment.rootRuntimeModelPtr,
                                       rootCreateHandlePtr,
                                       rootResolveCallerRva)) {
      if (rootCreateHandlePtr != nullptr)
        summary.attachmentRigidRootRuntimeCreateHandleKnownCount++;
      if (rootResolveCallerRva != 0u)
        summary.attachmentRigidRootRuntimeResolveCallerKnownCount++;
    }
    if (HasRuntimeOwnerIdentity(instanceRegistry, attachment.childRuntimeModelPtr))
      summary.attachmentRigidChildRuntimeOwnerIdentityCount++;
    if (HasRuntimeOwnerIdentity(instanceRegistry, attachment.ownerRuntimeModelPtr))
      summary.attachmentRigidOwnerRuntimeOwnerIdentityCount++;
    if (HasRuntimeOwnerIdentity(instanceRegistry, attachment.rootRuntimeModelPtr))
      summary.attachmentRigidRootRuntimeOwnerIdentityCount++;
    model::ModelInstanceRecord childRuntimeRecord = {};
    model::ModelInstanceRecord ownerRuntimeRecord = {};
    model::ModelInstanceRecord rootRuntimeRecord = {};
    model::ModelResourceRecord childRuntimeResource = {};
    model::PoseRecord childRuntimePose = {};
    const bool childRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
        instanceRegistry, attachment.childRuntimeModelPtr, childRuntimeRecord);
    const bool ownerRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
        instanceRegistry, attachment.ownerRuntimeModelPtr, ownerRuntimeRecord);
    const bool rootRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
        instanceRegistry, attachment.rootRuntimeModelPtr, rootRuntimeRecord);
    const bool childRuntimeResourceKnown = TryGetRuntimeModelResourceSnapshot(
        attachment.childRuntimeModelPtr, childRuntimeResource);
    const bool childRuntimePoseKnown = TryGetRuntimePoseSnapshot(
        attachment.childRuntimeModelPtr, childRuntimePose);
    if (childRuntimeRecordKnown)
      summary.attachmentRigidChildRuntimeRecordKnownCount++;
    if (ownerRuntimeRecordKnown)
      summary.attachmentRigidOwnerRuntimeRecordKnownCount++;
    if (rootRuntimeRecordKnown)
      summary.attachmentRigidRootRuntimeRecordKnownCount++;
    if (childRuntimeResourceKnown)
      summary.attachmentRigidChildRuntimeModelResourceKnownCount++;
    if (childRuntimePoseKnown)
      summary.attachmentRigidChildRuntimePoseKnownCount++;
    model::RuntimeParentLinkQueryResult childParentLink = {};
    const bool childParentLinkKnown = model::QueryRuntimeParentLink(
        attachment.childRuntimeModelPtr, childParentLink);
    if (childParentLinkKnown)
      summary.attachmentRigidChildRuntimeParentLinkKnownCount++;
    if (attachment.childRuntimeModelPtr != nullptr &&
        uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
            overrideSummary.lastAttachedEffectInitChildRuntimeModelPtr) {
      summary.attachmentRigidChildRuntimeMatchesAttachedEffectInitCount++;
    }
    if (attachment.childRuntimeModelPtr != nullptr &&
        uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
            overrideSummary.lastAttachModelToPointChildRuntimeModelPtr) {
      summary.attachmentRigidChildRuntimeMatchesAttachModelToPointCount++;
    }
    if (i == 0u) {
      summary.attachmentRigidSample0RootRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
      summary.attachmentRigidSample0OwnerRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.ownerRuntimeModelPtr));
      summary.attachmentRigidSample0ChildRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
      summary.attachmentRigidSample0ChildSpritePtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
      summary.attachmentRigidSample0SourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
      summary.attachmentRigidSample0RootRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
      summary.attachmentRigidSample0OwnerRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
      summary.attachmentRigidSample0ChildRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
      summary.attachmentRigidSample0RootRuntimeCreateCallerRva =
          rootCreateCallerRva;
      summary.attachmentRigidSample0OwnerRuntimeCreateCallerRva =
          ownerCreateCallerRva;
      summary.attachmentRigidSample0ChildRuntimeCreateCallerRva =
          childCreateCallerRva;
      summary.attachmentRigidSample0RootRuntimeResolveCallerRva =
          rootResolveCallerRva;
      summary.attachmentRigidSample0OwnerRuntimeResolveCallerRva =
          ownerResolveCallerRva;
      summary.attachmentRigidSample0ChildRuntimeResolveCallerRva =
          childResolveCallerRva;
      summary.attachmentRigidSample0RootRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample0OwnerRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample0RootRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample0OwnerRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample0RootRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample0ChildRuntimeParentRuntimeModelPtr =
          childParentLink.parentRuntimeModelPtr;
      summary.attachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame =
          childParentLink.lastSeenFrame;
      summary.attachmentRigidSample0ChildRuntimeParentLinkSourceMeta =
          childParentLink.sourceMeta;
      summary.attachmentRigidSample0ChildRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample0ChildRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample0ChildRuntimeModelResourcePtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeResource.modelResourcePtr));
      summary.attachmentRigidSample0ChildRuntimeModelKey =
          childRuntimeResource.modelKey;
      summary.attachmentRigidSample0ChildRuntimePoseMatrixCount =
          childRuntimePose.matrixCount;
      summary.attachmentRigidSample0FirstSeenFrame = attachment.firstSeenFrame;
      summary.attachmentRigidSample0LastSeenFrame = attachment.lastSeenFrame;
    } else if (i == 1u) {
      summary.attachmentRigidSample1RootRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
      summary.attachmentRigidSample1OwnerRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.ownerRuntimeModelPtr));
      summary.attachmentRigidSample1ChildRuntimeModelPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
      summary.attachmentRigidSample1ChildSpritePtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
      summary.attachmentRigidSample1SourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
      summary.attachmentRigidSample1RootRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
      summary.attachmentRigidSample1OwnerRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
      summary.attachmentRigidSample1ChildRuntimeCreateHandlePtr =
          uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
      summary.attachmentRigidSample1RootRuntimeCreateCallerRva =
          rootCreateCallerRva;
      summary.attachmentRigidSample1OwnerRuntimeCreateCallerRva =
          ownerCreateCallerRva;
      summary.attachmentRigidSample1ChildRuntimeCreateCallerRva =
          childCreateCallerRva;
      summary.attachmentRigidSample1RootRuntimeResolveCallerRva =
          rootResolveCallerRva;
      summary.attachmentRigidSample1OwnerRuntimeResolveCallerRva =
          ownerResolveCallerRva;
      summary.attachmentRigidSample1ChildRuntimeResolveCallerRva =
          childResolveCallerRva;
      summary.attachmentRigidSample1RootRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample1OwnerRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample1RootRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample1OwnerRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample1RootRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              rootRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              ownerRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample1ChildRuntimeParentRuntimeModelPtr =
          childParentLink.parentRuntimeModelPtr;
      summary.attachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame =
          childParentLink.lastSeenFrame;
      summary.attachmentRigidSample1ChildRuntimeParentLinkSourceMeta =
          childParentLink.sourceMeta;
      summary.attachmentRigidSample1ChildRuntimeCreateModelDataPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.runtimeCreatorModelDataPtr));
      summary.attachmentRigidSample1ChildRuntimeSourceObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceObjectPtr));
      summary.attachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeRecord.sourceSpriteObjectPtr));
      summary.attachmentRigidSample1ChildRuntimeModelResourcePtr =
          uint64_t(reinterpret_cast<uintptr_t>(
              childRuntimeResource.modelResourcePtr));
      summary.attachmentRigidSample1ChildRuntimeModelKey =
          childRuntimeResource.modelKey;
      summary.attachmentRigidSample1ChildRuntimePoseMatrixCount =
          childRuntimePose.matrixCount;
      summary.attachmentRigidSample1FirstSeenFrame = attachment.firstSeenFrame;
      summary.attachmentRigidSample1LastSeenFrame = attachment.lastSeenFrame;
    }
  }
  const auto contractAttachments =
      shadow::ShadowRuntimeContractCache::instance().snapshotAttachmentsShared();
  if (contractAttachments != nullptr) {
    summary.contractAttachmentRigidCount = contractAttachments->records().size();
    const auto& records = contractAttachments->records();
    for (size_t i = 0u; i < records.size(); ++i) {
      const auto& attachment = records[i];
      uint32_t rootCreateCallerRva = 0u;
      uint32_t ownerCreateCallerRva = 0u;
      uint32_t childCreateCallerRva = 0u;
      void* rootCreateHandlePtr = nullptr;
      void* ownerCreateHandlePtr = nullptr;
      void* childCreateHandlePtr = nullptr;
      uint32_t rootResolveCallerRva = 0u;
      uint32_t ownerResolveCallerRva = 0u;
      uint32_t childResolveCallerRva = 0u;
      if (attachment.sourceObjectPtr != nullptr ||
          attachment.sourceSpriteObjectPtr != nullptr) {
        summary.contractAttachmentRigidCountWithSourceObject++;
      }
      const bool hasAnyIdentity =
          attachment.worldObjectEntry != nullptr ||
          attachment.sceneNode != nullptr ||
          attachment.unitPtr != nullptr ||
          attachment.jHandle != 0u ||
          attachment.rawcode != 0u;
      if (hasAnyIdentity)
        summary.contractAttachmentRigidCountWithAnyIdentity++;
      if (attachment.worldObjectEntry != nullptr)
        summary.contractAttachmentRigidCountWithWorldObjectEntry++;
      if (attachment.sceneNode != nullptr)
        summary.contractAttachmentRigidCountWithSceneNode++;
      if (attachment.unitPtr != nullptr)
        summary.contractAttachmentRigidCountWithUnitPtr++;
      if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                       attachment.childRuntimeModelPtr,
                                       childCreateCallerRva)) {
        summary.contractAttachmentRigidChildRuntimeCreateCallerKnownCount++;
      }
      if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                         attachment.childRuntimeModelPtr,
                                         childCreateHandlePtr,
                                         childResolveCallerRva)) {
        if (childCreateHandlePtr != nullptr)
          summary.contractAttachmentRigidChildRuntimeCreateHandleKnownCount++;
        if (childResolveCallerRva != 0u)
          summary.contractAttachmentRigidChildRuntimeResolveCallerKnownCount++;
      }
      if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                       attachment.ownerRuntimeModelPtr,
                                       ownerCreateCallerRva)) {
        summary.contractAttachmentRigidOwnerRuntimeCreateCallerKnownCount++;
      }
      if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                         attachment.ownerRuntimeModelPtr,
                                         ownerCreateHandlePtr,
                                         ownerResolveCallerRva)) {
        if (ownerCreateHandlePtr != nullptr)
          summary.contractAttachmentRigidOwnerRuntimeCreateHandleKnownCount++;
        if (ownerResolveCallerRva != 0u)
          summary.contractAttachmentRigidOwnerRuntimeResolveCallerKnownCount++;
      }
      if (TryGetRuntimeCreateCallerRva(instanceRegistry,
                                       attachment.rootRuntimeModelPtr,
                                       rootCreateCallerRva)) {
        summary.contractAttachmentRigidRootRuntimeCreateCallerKnownCount++;
      }
      if (TryGetRuntimeResolveProvenance(instanceRegistry,
                                         attachment.rootRuntimeModelPtr,
                                         rootCreateHandlePtr,
                                         rootResolveCallerRva)) {
        if (rootCreateHandlePtr != nullptr)
          summary.contractAttachmentRigidRootRuntimeCreateHandleKnownCount++;
        if (rootResolveCallerRva != 0u)
          summary.contractAttachmentRigidRootRuntimeResolveCallerKnownCount++;
      }
      if (HasRuntimeOwnerIdentity(instanceRegistry,
                                  attachment.childRuntimeModelPtr)) {
        summary.contractAttachmentRigidChildRuntimeOwnerIdentityCount++;
      }
      if (HasRuntimeOwnerIdentity(instanceRegistry,
                                  attachment.ownerRuntimeModelPtr)) {
        summary.contractAttachmentRigidOwnerRuntimeOwnerIdentityCount++;
      }
      if (HasRuntimeOwnerIdentity(instanceRegistry,
                                  attachment.rootRuntimeModelPtr)) {
        summary.contractAttachmentRigidRootRuntimeOwnerIdentityCount++;
      }
      model::ModelInstanceRecord childRuntimeRecord = {};
      model::ModelInstanceRecord ownerRuntimeRecord = {};
      model::ModelInstanceRecord rootRuntimeRecord = {};
      model::ModelResourceRecord childRuntimeResource = {};
      model::PoseRecord childRuntimePose = {};
      const bool childRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
          instanceRegistry, attachment.childRuntimeModelPtr, childRuntimeRecord);
      const bool ownerRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
          instanceRegistry, attachment.ownerRuntimeModelPtr, ownerRuntimeRecord);
      const bool rootRuntimeRecordKnown = TryGetRuntimeRecordSnapshot(
          instanceRegistry, attachment.rootRuntimeModelPtr, rootRuntimeRecord);
      const bool childRuntimeResourceKnown = TryGetRuntimeModelResourceSnapshot(
          attachment.childRuntimeModelPtr, childRuntimeResource);
      const bool childRuntimePoseKnown = TryGetRuntimePoseSnapshot(
          attachment.childRuntimeModelPtr, childRuntimePose);
      if (childRuntimeRecordKnown)
        summary.contractAttachmentRigidChildRuntimeRecordKnownCount++;
      if (ownerRuntimeRecordKnown)
        summary.contractAttachmentRigidOwnerRuntimeRecordKnownCount++;
      if (rootRuntimeRecordKnown)
        summary.contractAttachmentRigidRootRuntimeRecordKnownCount++;
      if (childRuntimeResourceKnown)
        summary.contractAttachmentRigidChildRuntimeModelResourceKnownCount++;
      if (childRuntimePoseKnown)
        summary.contractAttachmentRigidChildRuntimePoseKnownCount++;
      model::RuntimeParentLinkQueryResult childParentLink = {};
      const bool childParentLinkKnown = model::QueryRuntimeParentLink(
          attachment.childRuntimeModelPtr, childParentLink);
      if (childParentLinkKnown)
        summary.contractAttachmentRigidChildRuntimeParentLinkKnownCount++;
      if (attachment.childRuntimeModelPtr != nullptr &&
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
              overrideSummary.lastAttachedEffectInitChildRuntimeModelPtr) {
        summary
            .contractAttachmentRigidChildRuntimeMatchesAttachedEffectInitCount++;
      }
      if (attachment.childRuntimeModelPtr != nullptr &&
          uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr)) ==
              overrideSummary.lastAttachModelToPointChildRuntimeModelPtr) {
        summary
            .contractAttachmentRigidChildRuntimeMatchesAttachModelToPointCount++;
      }
      if (i == 0u) {
        summary.contractAttachmentRigidSample0RootRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                attachment.ownerRuntimeModelPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
        summary.contractAttachmentRigidSample0ChildSpritePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
        summary.contractAttachmentRigidSample0SourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
        summary.contractAttachmentRigidSample0WorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.worldObjectEntry));
        summary.contractAttachmentRigidSample0SceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sceneNode));
        summary.contractAttachmentRigidSample0RootRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
        summary.contractAttachmentRigidSample0ChildRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
        summary.contractAttachmentRigidSample0RootRuntimeCreateCallerRva =
            rootCreateCallerRva;
        summary.contractAttachmentRigidSample0OwnerRuntimeCreateCallerRva =
            ownerCreateCallerRva;
        summary.contractAttachmentRigidSample0ChildRuntimeCreateCallerRva =
            childCreateCallerRva;
        summary.contractAttachmentRigidSample0RootRuntimeResolveCallerRva =
            rootResolveCallerRva;
        summary.contractAttachmentRigidSample0OwnerRuntimeResolveCallerRva =
            ownerResolveCallerRva;
        summary.contractAttachmentRigidSample0ChildRuntimeResolveCallerRva =
            childResolveCallerRva;
        summary.contractAttachmentRigidSample0RootRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample0RootRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample0RootRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample0OwnerRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample0OwnerRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample0RootRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample0RootRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample0OwnerRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceSpriteObjectPtr));
        summary
            .contractAttachmentRigidSample0ChildRuntimeParentRuntimeModelPtr =
            childParentLink.parentRuntimeModelPtr;
        summary.contractAttachmentRigidSample0ChildRuntimeParentLinkLastSeenFrame =
            childParentLink.lastSeenFrame;
        summary.contractAttachmentRigidSample0ChildRuntimeParentLinkSourceMeta =
            childParentLink.sourceMeta;
        summary.contractAttachmentRigidSample0ChildRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample0ChildRuntimeModelResourcePtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeResource.modelResourcePtr));
        summary.contractAttachmentRigidSample0ChildRuntimeModelKey =
            childRuntimeResource.modelKey;
        summary.contractAttachmentRigidSample0ChildRuntimePoseMatrixCount =
            childRuntimePose.matrixCount;
      } else if (i == 1u) {
        summary.contractAttachmentRigidSample1RootRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.rootRuntimeModelPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                attachment.ownerRuntimeModelPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeModelPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childRuntimeModelPtr));
        summary.contractAttachmentRigidSample1ChildSpritePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.childSpritePtr));
        summary.contractAttachmentRigidSample1SourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sourceObjectPtr));
        summary.contractAttachmentRigidSample1WorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.worldObjectEntry));
        summary.contractAttachmentRigidSample1SceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(attachment.sceneNode));
        summary.contractAttachmentRigidSample1RootRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootCreateHandlePtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerCreateHandlePtr));
        summary.contractAttachmentRigidSample1ChildRuntimeCreateHandlePtr =
            uint64_t(reinterpret_cast<uintptr_t>(childCreateHandlePtr));
        summary.contractAttachmentRigidSample1RootRuntimeCreateCallerRva =
            rootCreateCallerRva;
        summary.contractAttachmentRigidSample1OwnerRuntimeCreateCallerRva =
            ownerCreateCallerRva;
        summary.contractAttachmentRigidSample1ChildRuntimeCreateCallerRva =
            childCreateCallerRva;
        summary.contractAttachmentRigidSample1RootRuntimeResolveCallerRva =
            rootResolveCallerRva;
        summary.contractAttachmentRigidSample1OwnerRuntimeResolveCallerRva =
            ownerResolveCallerRva;
        summary.contractAttachmentRigidSample1ChildRuntimeResolveCallerRva =
            childResolveCallerRva;
        summary.contractAttachmentRigidSample1RootRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample1RootRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample1RootRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(rootRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample1OwnerRuntimeWorldObjectEntryPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.worldObjectEntry));
        summary.contractAttachmentRigidSample1OwnerRuntimeSceneNodePtr =
            uint64_t(reinterpret_cast<uintptr_t>(ownerRuntimeRecord.sceneNode));
        summary.contractAttachmentRigidSample1RootRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample1RootRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                rootRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample1OwnerRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                ownerRuntimeRecord.sourceSpriteObjectPtr));
        summary
            .contractAttachmentRigidSample1ChildRuntimeParentRuntimeModelPtr =
            childParentLink.parentRuntimeModelPtr;
        summary.contractAttachmentRigidSample1ChildRuntimeParentLinkLastSeenFrame =
            childParentLink.lastSeenFrame;
        summary.contractAttachmentRigidSample1ChildRuntimeParentLinkSourceMeta =
            childParentLink.sourceMeta;
        summary.contractAttachmentRigidSample1ChildRuntimeCreateModelDataPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.runtimeCreatorModelDataPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeSourceObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceObjectPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeSourceSpriteObjectPtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeRecord.sourceSpriteObjectPtr));
        summary.contractAttachmentRigidSample1ChildRuntimeModelResourcePtr =
            uint64_t(reinterpret_cast<uintptr_t>(
                childRuntimeResource.modelResourcePtr));
        summary.contractAttachmentRigidSample1ChildRuntimeModelKey =
            childRuntimeResource.modelKey;
        summary.contractAttachmentRigidSample1ChildRuntimePoseMatrixCount =
            childRuntimePose.matrixCount;
      }
    }
  }
  const auto upperLayerStats = UpperLayerShadowRegistry::instance().snapshotStats();
  summary.upperLayerResolveAttempts = upperLayerStats.resolveAttempts;
  summary.upperLayerResolveVisibleMiss = upperLayerStats.resolveVisibleMiss;
  summary.upperLayerResolveVisibleUnresolvedGeoset =
      upperLayerStats.resolveVisibleUnresolvedGeoset;
  summary.upperLayerResolveGeosetMiss = upperLayerStats.resolveGeosetMiss;
  summary.upperLayerResolvePoseMiss = upperLayerStats.resolvePoseMiss;
  summary.upperLayerResolveRuntimeGroupPaletteMiss =
      upperLayerStats.resolveRuntimeGroupPaletteMiss;
  summary.upperLayerResolveAuthoritativeRigid =
      upperLayerStats.resolveAuthoritativeRigid;
  summary.upperLayerResolveAuthoritativeSkinned =
      upperLayerStats.resolveAuthoritativeSkinned;
  summary.upperLayerResolvedAuthoritativeItems =
      upperLayerStats.resolvedAuthoritativeItems;
  summary.upperLayerEmitted = upperLayerStats.emitted;
  summary.upperLayerDuplicateOrSuppressed =
      upperLayerStats.duplicateOrSuppressed;
  {
    std::shared_lock<std::shared_mutex> lock(g_shadowSceneStatsMutex);
    summary.fallbackDrawCount = g_shadowSceneStats.fallbackDrawCount;
    summary.fallbackDrawCountTerrain =
        g_shadowSceneStats.fallbackDrawCountTerrain;
    summary.fallbackDrawCountWorldObject =
        g_shadowSceneStats.fallbackDrawCountWorldObject;
    summary.fallbackDrawCountUnitObject =
        g_shadowSceneStats.fallbackDrawCountUnitObject;
    summary.objectFallbackDrawCount =
        g_shadowSceneStats.fallbackDrawCountWorldObject +
        g_shadowSceneStats.fallbackDrawCountUnitObject;
    summary.semanticSceneSubmitted = g_shadowSceneStats.semanticSceneSubmitted;
    summary.semanticSceneSubmittedUnit =
        g_shadowSceneStats.semanticSceneSubmittedUnit;
    summary.semanticSceneSubmittedSkinned =
        g_shadowSceneStats.semanticSceneSubmittedSkinned;
    summary.semanticSceneSubmittedSkinnedNonUnitResolvedCount =
        g_shadowSceneStats.semanticSceneSubmittedSkinnedNonUnitResolvedCount;
    summary.semanticSceneSubmittedSkinnedUnknownPacketKindCount =
        g_shadowSceneStats.semanticSceneSubmittedSkinnedUnknownPacketKindCount;
    summary.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount;
    summary.semanticSceneSubmittedSkinnedGroupNonZeroCount =
        g_shadowSceneStats.semanticSceneSubmittedSkinnedGroupNonZeroCount;
    summary.semanticSceneSubmittedSkinnedTransparentQueueCount =
        g_shadowSceneStats.semanticSceneSubmittedSkinnedTransparentQueueCount;
    summary.semanticSceneSubmittedSkinnedMissingUnitPtrCount =
        g_shadowSceneStats.semanticSceneSubmittedSkinnedMissingUnitPtrCount;
    summary.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount;
    summary.semanticSceneSubmittedBuilding =
        g_shadowSceneStats.semanticSceneSubmittedBuilding;
    summary.semanticSceneSubmittedDestructible =
        g_shadowSceneStats.semanticSceneSubmittedDestructible;
    summary.semanticSceneSubmittedCutout =
        g_shadowSceneStats.semanticSceneSubmittedCutout;
    summary.semanticSceneSubmittedAlphaBlend =
        g_shadowSceneStats.semanticSceneSubmittedAlphaBlend;
    summary.semanticSceneMaterialObservedCutoutCount =
        g_shadowSceneStats.semanticSceneMaterialObservedCutoutCount;
    summary.semanticSceneMaterialObservedAlphaBlendCount =
        g_shadowSceneStats.semanticSceneMaterialObservedAlphaBlendCount;
    summary.semanticSceneRejectedCutoutSkinnedContract =
        g_shadowSceneStats.semanticSceneRejectedCutoutSkinnedContract;
    summary.semanticSceneRejectedAlphaBlendSkinnedContract =
        g_shadowSceneStats.semanticSceneRejectedAlphaBlendSkinnedContract;
    summary.semanticSceneRejectedCutoutGeometry =
        g_shadowSceneStats.semanticSceneRejectedCutoutGeometry;
    summary.semanticSceneRejectedAlphaBlendGeometry =
        g_shadowSceneStats.semanticSceneRejectedAlphaBlendGeometry;
    summary.semanticSceneRejectedCutoutVisualPolicy =
        g_shadowSceneStats.semanticSceneRejectedCutoutVisualPolicy;
    summary.semanticSceneRejectedAlphaBlendVisualPolicy =
        g_shadowSceneStats.semanticSceneRejectedAlphaBlendVisualPolicy;
    summary.semanticSceneMaterialLayerContractResolvedCount =
        g_shadowSceneStats.semanticSceneMaterialLayerContractResolvedCount;
    summary.semanticSceneMaterialLayerContractFailedCount =
        g_shadowSceneStats.semanticSceneMaterialLayerContractFailedCount;
    summary.semanticSceneMaterialBlendMode0Count =
        g_shadowSceneStats.semanticSceneMaterialBlendMode0Count;
    summary.semanticSceneMaterialBlendMode1Count =
        g_shadowSceneStats.semanticSceneMaterialBlendMode1Count;
    summary.semanticSceneMaterialBlendMode2PlusCount =
        g_shadowSceneStats.semanticSceneMaterialBlendMode2PlusCount;
    summary.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount =
        g_shadowSceneStats.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount;
    summary.semanticSceneMaterialLastMeshIndex =
        g_shadowSceneStats.semanticSceneMaterialLastMeshIndex;
    summary.semanticSceneMaterialLastLayerIndex =
        g_shadowSceneStats.semanticSceneMaterialLastLayerIndex;
    summary.semanticSceneMaterialLastLayerCount =
        g_shadowSceneStats.semanticSceneMaterialLastLayerCount;
    summary.semanticSceneMaterialLastBlendOrDrawMode =
        g_shadowSceneStats.semanticSceneMaterialLastBlendOrDrawMode;
    summary.semanticSceneLivePaletteRefreshAttemptCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshAttemptCount;
    summary.semanticSceneLivePaletteRefreshHitCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshHitCount;
    summary.semanticSceneLivePaletteRefreshMissCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshMissCount;
    summary.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount =
        g_shadowSceneStats
            .semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount;
    summary.semanticScenePaletteOverrideNoComposeCount =
        g_shadowSceneStats.semanticScenePaletteOverrideNoComposeCount;
    summary.semanticScenePaletteOverrideWouldComposeCount =
        g_shadowSceneStats.semanticScenePaletteOverrideWouldComposeCount;
    summary.semanticScenePalettePacketWorldComposeCount =
        g_shadowSceneStats.semanticScenePalettePacketWorldComposeCount;
    summary.semanticSceneLivePaletteRefreshLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshLastRuntimeModelPtr;
    summary.semanticSceneLivePaletteRefreshLastMatrixCount =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshLastMatrixCount;
    summary.semanticSceneLivePaletteRefreshLastMatrixHash =
        g_shadowSceneStats.semanticSceneLivePaletteRefreshLastMatrixHash;
    summary.semanticSceneLivePaletteMotionSampleCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionSampleCount;
    summary.semanticSceneLivePaletteMotionNewRuntimeCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionNewRuntimeCount;
    summary.semanticSceneLivePaletteMotionRawChangedCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionRawChangedCount;
    summary.semanticSceneLivePaletteMotionRawStableCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionRawStableCount;
    summary.semanticSceneLivePaletteMotionGroupChangedCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionGroupChangedCount;
    summary.semanticSceneLivePaletteMotionGroupStableCount =
        g_shadowSceneStats.semanticSceneLivePaletteMotionGroupStableCount;
    summary.semanticSceneLivePaletteMotionLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastRuntimeModelPtr;
    summary.semanticSceneLivePaletteMotionLastPrevRawHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastPrevRawHash;
    summary.semanticSceneLivePaletteMotionLastRawHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastRawHash;
    summary.semanticSceneLivePaletteMotionLastPrevGroupHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastPrevGroupHash;
    summary.semanticSceneLivePaletteMotionLastGroupHash =
        g_shadowSceneStats.semanticSceneLivePaletteMotionLastGroupHash;
    summary.semanticSceneDrawTimePoseAttemptCount =
        g_shadowSceneStats.semanticSceneDrawTimePoseAttemptCount;
    summary.semanticSceneDrawTimePosePublishedCount =
        g_shadowSceneStats.semanticSceneDrawTimePosePublishedCount;
    summary.semanticSceneDrawTimePoseChangedCount =
        g_shadowSceneStats.semanticSceneDrawTimePoseChangedCount;
    summary.semanticSceneDrawTimePoseStableCount =
        g_shadowSceneStats.semanticSceneDrawTimePoseStableCount;
    summary.semanticSceneDrawTimePoseLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneDrawTimePoseLastRuntimeModelPtr;
    summary.semanticSceneDrawTimePoseLastPrevHash =
        g_shadowSceneStats.semanticSceneDrawTimePoseLastPrevHash;
    summary.semanticSceneDrawTimePoseLastHash =
        g_shadowSceneStats.semanticSceneDrawTimePoseLastHash;
    summary.semanticSceneSubmittedPaletteMotionSampleCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionSampleCount;
    summary.semanticSceneSubmittedPaletteMotionNewRuntimeCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionNewRuntimeCount;
    summary.semanticSceneSubmittedPaletteMotionChangedCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionChangedCount;
    summary.semanticSceneSubmittedPaletteMotionStableCount =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionStableCount;
    summary.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr;
    summary.semanticSceneSubmittedPaletteMotionLastPrevHash =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionLastPrevHash;
    summary.semanticSceneSubmittedPaletteMotionLastHash =
        g_shadowSceneStats.semanticSceneSubmittedPaletteMotionLastHash;
    summary.semanticSceneSkinnedDynamicIndexSliceCount =
        g_shadowSceneStats.semanticSceneSkinnedDynamicIndexSliceCount;
    summary.semanticSceneSubmittedOwnedGroupSlots =
        g_shadowSceneStats.semanticSceneSubmittedOwnedGroupSlots;
    summary.semanticSceneCurrentDrawContractKnownCount =
        g_shadowSceneStats.semanticSceneCurrentDrawContractKnownCount;
    summary.semanticSceneCurrentDrawPaletteReadyCount =
        g_shadowSceneStats.semanticSceneCurrentDrawPaletteReadyCount;
    summary.semanticSceneCurrentDrawGroupSlotReadyCount =
        g_shadowSceneStats.semanticSceneCurrentDrawGroupSlotReadyCount;
    summary.semanticSceneCurrentDrawResolveReadyCount =
        g_shadowSceneStats.semanticSceneCurrentDrawResolveReadyCount;
    summary.semanticSceneCurrentDrawMissNoContract =
        g_shadowSceneStats.semanticSceneCurrentDrawMissNoContract;
    summary.semanticSceneCurrentDrawMissNoPalette =
        g_shadowSceneStats.semanticSceneCurrentDrawMissNoPalette;
    summary.semanticSceneCurrentDrawMissNoGroupSlots =
        g_shadowSceneStats.semanticSceneCurrentDrawMissNoGroupSlots;
    summary.semanticSceneCurrentDrawMissStaleVisibleFrame =
        g_shadowSceneStats.semanticSceneCurrentDrawMissStaleVisibleFrame;
    summary.semanticSceneCurrentDrawResolveReadyRejectedCount =
        g_shadowSceneStats.semanticSceneCurrentDrawResolveReadyRejectedCount;
    summary.semanticSceneCanonicalReadyCount =
        g_shadowSceneStats.semanticSceneCanonicalReadyCount;
    summary.semanticSceneCanonicalReadyCutoutCount =
        g_shadowSceneStats.semanticSceneCanonicalReadyCutoutCount;
    summary.semanticSceneCanonicalReadyAlphaBlendCount =
        g_shadowSceneStats.semanticSceneCanonicalReadyAlphaBlendCount;
    summary.semanticSceneCanonicalRejectNoStableIdentity =
        g_shadowSceneStats.semanticSceneCanonicalRejectNoStableIdentity;
    summary.semanticSceneCanonicalRejectNoMesh =
        g_shadowSceneStats.semanticSceneCanonicalRejectNoMesh;
    summary.semanticSceneCanonicalRejectNoWorldTransform =
        g_shadowSceneStats.semanticSceneCanonicalRejectNoWorldTransform;
    summary.semanticSceneCanonicalRejectNoPalette =
        g_shadowSceneStats.semanticSceneCanonicalRejectNoPalette;
    summary.semanticSceneCanonicalRejectNoSlotContract =
        g_shadowSceneStats.semanticSceneCanonicalRejectNoSlotContract;
    summary.semanticSceneCanonicalRejectStaleProducer =
        g_shadowSceneStats.semanticSceneCanonicalRejectStaleProducer;
    summary.semanticSceneCanonicalRejectInvalidVertexIndex =
        g_shadowSceneStats.semanticSceneCanonicalRejectInvalidVertexIndex;
    summary.semanticSceneCanonicalRejectExplicitBlendIncomplete =
        g_shadowSceneStats.semanticSceneCanonicalRejectExplicitBlendIncomplete;
    summary.semanticSceneCanonicalRejectAfterReadyCount =
        g_shadowSceneStats.semanticSceneCanonicalRejectAfterReadyCount;
    summary.semanticSceneSubmittedExplicitBlendContract =
        g_shadowSceneStats.semanticSceneSubmittedExplicitBlendContract;
    summary.semanticSceneSubmittedSingleMatrixGroupSkinning =
        g_shadowSceneStats.semanticSceneSubmittedSingleMatrixGroupSkinning;
    summary.semanticSceneSubmittedMultiGroupSlotSkinning =
        g_shadowSceneStats.semanticSceneSubmittedMultiGroupSlotSkinning;
    summary.semanticSceneSkinnedMinUniqueGroupSlots =
        g_shadowSceneStats.semanticSceneSkinnedMinUniqueGroupSlots;
    summary.semanticSceneSkinnedMaxUniqueGroupSlots =
        g_shadowSceneStats.semanticSceneSkinnedMaxUniqueGroupSlots;
    summary.semanticSceneSkinnedGroupSlotsUnique1Count =
        g_shadowSceneStats.semanticSceneSkinnedGroupSlotsUnique1Count;
    summary.semanticSceneSkinnedGroupSlotsUnique2To4Count =
        g_shadowSceneStats.semanticSceneSkinnedGroupSlotsUnique2To4Count;
    summary.semanticSceneSkinnedGroupSlotsUnique5To8Count =
        g_shadowSceneStats.semanticSceneSkinnedGroupSlotsUnique5To8Count;
    summary.semanticSceneSkinnedGroupSlotsUnique9To16Count =
        g_shadowSceneStats.semanticSceneSkinnedGroupSlotsUnique9To16Count;
    summary.semanticSceneSkinnedGroupSlotsUnique17PlusCount =
        g_shadowSceneStats.semanticSceneSkinnedGroupSlotsUnique17PlusCount;
    summary.semanticSceneExplicitBlendUnavailableCurrentDraw =
        g_shadowSceneStats.semanticSceneExplicitBlendUnavailableCurrentDraw;
    summary.semanticSceneSkinnedFullIndexFallbackCount =
        g_shadowSceneStats.semanticSceneSkinnedFullIndexFallbackCount;
    summary.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount =
        g_shadowSceneStats
            .semanticSceneSkinnedMissingVisibleIndexSliceRejectCount;
    summary.semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr =
        g_shadowSceneStats
            .semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr;
    summary.semanticSceneSkinnedFullIndexFallbackLastIndexCount =
        g_shadowSceneStats
            .semanticSceneSkinnedFullIndexFallbackLastIndexCount;
    summary.semanticSceneSubmittedFrameLocal =
        g_shadowSceneStats.semanticSceneSubmittedFrameLocal;
    summary.semanticSceneSubmittedPersistent =
        g_shadowSceneStats.semanticSceneSubmitted >
                g_shadowSceneStats.semanticSceneSubmittedFrameLocal
            ? (g_shadowSceneStats.semanticSceneSubmitted -
               g_shadowSceneStats.semanticSceneSubmittedFrameLocal)
            : 0u;
    summary.semanticSceneStatsPublishCount = g_shadowSceneStatsPublishCount;
    summary.semanticSceneInputDrawCount =
        g_shadowSceneStats.semanticSceneInputDrawCount;
    summary.semanticSceneInputSkinnedCount =
        g_shadowSceneStats.semanticSceneInputSkinnedCount;
    summary.semanticSceneTailBoundaryCandidateCount =
        g_shadowSceneStats.semanticSceneTailBoundaryCandidateCount;
    summary.semanticSceneTailBoundaryCommitCount =
        g_shadowSceneStats.semanticSceneTailBoundaryCommitCount;
    summary.semanticScenePopulateAttemptCount =
        g_shadowSceneStats.semanticScenePopulateAttemptCount;
    summary.semanticScenePopulateUnitsOnlyCount =
        g_shadowSceneStats.semanticScenePopulateUnitsOnlyCount;
    summary.semanticScenePopulateLastReturnReason =
        g_shadowSceneStats.semanticScenePopulateLastReturnReason;
    summary.semanticScenePopulateLastProducerPublishAttemptDelta =
        g_shadowSceneStats.semanticScenePopulateLastProducerPublishAttemptDelta;
    summary.semanticScenePopulateLastProducerPublishReadyDelta =
        g_shadowSceneStats.semanticScenePopulateLastProducerPublishReadyDelta;
    summary.semanticScenePopulateLastProducerQueryAttemptDelta =
        g_shadowSceneStats.semanticScenePopulateLastProducerQueryAttemptDelta;
    summary.semanticScenePopulateLastProducerQueryHitDelta =
        g_shadowSceneStats.semanticScenePopulateLastProducerQueryHitDelta;
    summary.semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta =
        g_shadowSceneStats
            .semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta;
    summary.semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta =
        g_shadowSceneStats
            .semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta;
    summary.semanticScenePopulateLastProducerGroupDecodeAttemptDelta =
        g_shadowSceneStats
            .semanticScenePopulateLastProducerGroupDecodeAttemptDelta;
    summary.semanticScenePopulateLastProducerGroupDecodeHitDelta =
        g_shadowSceneStats
            .semanticScenePopulateLastProducerGroupDecodeHitDelta;
    summary.semanticSceneDirectCurrentDrawRecordCount =
        g_shadowSceneStats.semanticSceneDirectCurrentDrawRecordCount;
    summary.semanticSceneDirectCurrentDrawBuiltPacketCount =
        g_shadowSceneStats.semanticSceneDirectCurrentDrawBuiltPacketCount;
    summary.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount =
        g_shadowSceneStats.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount;
    summary.semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount =
        g_shadowSceneStats
            .semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount;
    summary.semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount =
        g_shadowSceneStats
            .semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount;
    summary.semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount =
        g_shadowSceneStats
            .semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount;
    // Phase 7.1: caster selection stability diagnostics
    summary.semanticSceneDirectLastRawRecordCount =
        g_shadowSceneStats.semanticSceneDirectLastRawRecordCount;
    summary.semanticSceneDirectLastEligibleRecordCount =
        g_shadowSceneStats.semanticSceneDirectLastEligibleRecordCount;
    summary.semanticSceneDirectLastSubmittedRecordCount =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedRecordCount;
    summary.semanticSceneDirectLastUniqueObjectCount =
        g_shadowSceneStats.semanticSceneDirectLastUniqueObjectCount;
    summary.semanticSceneDirectLastSubmittedObjectCount =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedObjectCount;
    summary.semanticSceneDirectLastRecordCapPartialObjectCount =
        g_shadowSceneStats.semanticSceneDirectLastRecordCapPartialObjectCount;
    summary.semanticSceneDirectLastScanCapPartialObjectCount =
        g_shadowSceneStats.semanticSceneDirectLastScanCapPartialObjectCount;
    summary.semanticSceneDirectLastMinGeosetsPerObject =
        g_shadowSceneStats.semanticSceneDirectLastMinGeosetsPerObject;
    summary.semanticSceneDirectLastMaxGeosetsPerObject =
        g_shadowSceneStats.semanticSceneDirectLastMaxGeosetsPerObject;
    summary.semanticSceneDirectLastSubmittedIdentityHash =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedIdentityHash;
    summary.semanticSceneDirectRecordCapHitCount =
        g_shadowSceneStats.semanticSceneDirectRecordCapHitCount;
    summary.semanticSceneDirectRecordCapTruncatedRecordCount =
        g_shadowSceneStats.semanticSceneDirectRecordCapTruncatedRecordCount;
    summary.semanticSceneDirectScanCapHitCount =
        g_shadowSceneStats.semanticSceneDirectScanCapHitCount;
    summary.semanticSceneDirectIdentityChurnCount =
        g_shadowSceneStats.semanticSceneDirectIdentityChurnCount;
    summary.semanticSceneDirectObjectGroupedSubmitCount =
        g_shadowSceneStats.semanticSceneDirectObjectGroupedSubmitCount;
    summary.semanticSceneDirectObjectGroupedSkipCount =
        g_shadowSceneStats.semanticSceneDirectObjectGroupedSkipCount;
    summary.semanticSceneDirectRecordCapSkipObjectCount =
        g_shadowSceneStats.semanticSceneDirectRecordCapSkipObjectCount;
    summary.semanticSceneDirectRecordCapAppendFailCount =
        g_shadowSceneStats.semanticSceneDirectRecordCapAppendFailCount;
    summary.semanticSceneDirectSelectionLeaseActiveKeyCount =
        g_shadowSceneStats.semanticSceneDirectSelectionLeaseActiveKeyCount;
    summary.semanticSceneDirectSelectionLeasePrunedKeyCount =
        g_shadowSceneStats.semanticSceneDirectSelectionLeasePrunedKeyCount;
    summary.semanticSceneDirectSelectionLeaseSubmittedKeyCount =
        g_shadowSceneStats.semanticSceneDirectSelectionLeaseSubmittedKeyCount;
    summary.semanticSceneDirectStickyFillBudgetRecordCount =
        g_shadowSceneStats.semanticSceneDirectStickyFillBudgetRecordCount;
    summary.semanticSceneDirectStickyFillAppendedCount =
        g_shadowSceneStats.semanticSceneDirectStickyFillAppendedCount;
    summary.semanticSceneDirectStickyFillSubmittedCount =
        g_shadowSceneStats.semanticSceneDirectStickyFillSubmittedCount;
    summary.semanticSceneDirectStickyFillMissedCount =
        g_shadowSceneStats.semanticSceneDirectStickyFillMissedCount;
    summary.semanticSceneDirectPartLeaseRestoredCount =
        g_shadowSceneStats.semanticSceneDirectPartLeaseRestoredCount;
    summary.semanticSceneDirectPartLeaseUpdatedCount =
        g_shadowSceneStats.semanticSceneDirectPartLeaseUpdatedCount;
    summary.semanticSceneDirectPartLeaseExpiredCount =
        g_shadowSceneStats.semanticSceneDirectPartLeaseExpiredCount;
    summary.semanticSceneDirectPartLeaseRejectedDynamicMeshCount =
        g_shadowSceneStats.semanticSceneDirectPartLeaseRejectedDynamicMeshCount;
    summary.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount =
        g_shadowSceneStats
            .semanticSceneDirectPartLeaseRejectedNotSelfContainedCount;
    summary.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount =
        g_shadowSceneStats.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount;
    summary.semanticSceneDirectPartLeaseRejectedSelfRenewCount =
        g_shadowSceneStats.semanticSceneDirectPartLeaseRejectedSelfRenewCount;
    summary.semanticSceneDirectPartLeaseBudgetLimitCount =
        g_shadowSceneStats.semanticSceneDirectPartLeaseBudgetLimitCount;
    summary.semanticSceneShadowManifestPartLeaseRestoredCount =
        g_shadowSceneStats.semanticSceneShadowManifestPartLeaseRestoredCount;
    summary.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount;
    summary.semanticSceneShadowManifestPartLeaseExpiredCount =
        g_shadowSceneStats.semanticSceneShadowManifestPartLeaseExpiredCount;
    summary.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount;
    summary.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount;
    summary.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount;
    summary
        .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount;
    summary.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount;
    summary.semanticSceneShadowManifestPartLeaseBudgetLimitCount =
        g_shadowSceneStats.semanticSceneShadowManifestPartLeaseBudgetLimitCount;
    summary.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount;
    summary.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount;
    summary.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount;
    summary.semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeasePaletteRefreshAttemptCount;
    summary.semanticSceneShadowManifestPartLeasePaletteRefreshHitCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeasePaletteRefreshHitCount;
    summary.semanticSceneShadowManifestPartLeasePaletteRefreshMissCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeasePaletteRefreshMissCount;
    summary.semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount;
    summary.semanticSceneShadowManifestPartLeasePaletteRefreshFallbackCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartLeasePaletteRefreshFallbackCount;
    summary.semanticSceneShadowManifestObjectCoreCompleteCount =
        g_shadowSceneStats.semanticSceneShadowManifestObjectCoreCompleteCount;
    summary.semanticSceneShadowManifestObjectCoreIncompleteSkipCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestObjectCoreIncompleteSkipCount;
    summary.semanticSceneShadowManifestPartOmittedIncompleteCoreCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestPartOmittedIncompleteCoreCount;
    // Phase 7.25 core epoch planner 专属计数器。
    summary
        .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount;
    summary
        .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount;
    summary
        .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount;
    summary.semanticSceneShadowManifestObjectCoreEpochMissingPartCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestObjectCoreEpochMissingPartCount;
    summary
        .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount;
    summary
        .semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount;
    summary
        .semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount;
    summary.semanticSceneShadowManifestLeaseExpiredBackingOnlyCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestLeaseExpiredBackingOnlyCount;
    summary
        .semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount;
    summary.semanticSceneShadowManifestMissingRequiredPartCount =
        g_shadowSceneStats.semanticSceneShadowManifestMissingRequiredPartCount;
    summary.semanticSceneShadowManifestGraceUsedCount =
        g_shadowSceneStats.semanticSceneShadowManifestGraceUsedCount;
    summary.semanticSceneShadowManifestTombstoneRetiredCount =
        g_shadowSceneStats.semanticSceneShadowManifestTombstoneRetiredCount;
    // Phase 7.28：skinned palette content stability probe。
    summary.semanticSceneSubmittedSkinnedPaletteSourceNoneCount =
        g_shadowSceneStats.semanticSceneSubmittedSkinnedPaletteSourceNoneCount;
    summary.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount;
    summary.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount;
    summary.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount;
    summary
        .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount;
    summary
        .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount;
    summary.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStablePartSampleCount;
    summary.semanticSceneSubmittedSkinnedPaletteHashChurnCount =
        g_shadowSceneStats.semanticSceneSubmittedSkinnedPaletteHashChurnCount;
    summary.semanticSceneSubmittedSkinnedPaletteSourceChurnCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSourceChurnCount;
    summary.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount;
    summary.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax;
    summary.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax;
    summary.semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount;
    summary.semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount;
    summary.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount;
    summary.semanticSceneSubmittedSkinnedPaletteCountChurnCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteCountChurnCount;
    summary.semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount;
    summary.semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount;
    summary.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount;
    summary.semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount;
    summary.semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount;
    summary
        .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount;
    summary
        .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount;
    summary
        .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount;
    summary.semanticSceneDirectPaletteAttributionSnapshotHitCount =
        g_shadowSceneStats
            .semanticSceneDirectPaletteAttributionSnapshotHitCount;
    summary.semanticSceneDirectPaletteCaptureTrustedSourceHitCount =
        g_shadowSceneStats
            .semanticSceneDirectPaletteCaptureTrustedSourceHitCount;
    summary.semanticSceneDirectPaletteCaptureTrustedSourceMissCount =
        g_shadowSceneStats
            .semanticSceneDirectPaletteCaptureTrustedSourceMissCount;
    // Phase 7.30 Step A：stale→live 过渡归因。
    summary
        .semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount;
    summary
        .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount;
    summary
        .semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount =
        g_shadowSceneStats
            .semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount;
    summary.semanticSceneDirectStickyPartSelectionRetainedCount =
        g_shadowSceneStats.semanticSceneDirectStickyPartSelectionRetainedCount;
    summary.semanticSceneDirectStickyPartSelectionDroppedCount =
        g_shadowSceneStats.semanticSceneDirectStickyPartSelectionDroppedCount;
    summary.semanticSceneDirectStickyPartSelectionFallbackCount =
        g_shadowSceneStats.semanticSceneDirectStickyPartSelectionFallbackCount;
    // Phase 7.5: object completeness diagnostics
    summary.semanticSceneDirectManifestObjectCount =
        g_shadowSceneStats.semanticSceneDirectManifestObjectCount;
    summary.semanticSceneDirectManifestObservedPartCount =
        g_shadowSceneStats.semanticSceneDirectManifestObservedPartCount;
    summary.semanticSceneDirectManifestShadowEligiblePartCount =
        g_shadowSceneStats.semanticSceneDirectManifestShadowEligiblePartCount;
    summary.semanticSceneDirectObjectCompleteEligibleCount =
        g_shadowSceneStats.semanticSceneDirectObjectCompleteEligibleCount;
    summary.semanticSceneDirectObjectIncompleteByScanCapCount =
        g_shadowSceneStats.semanticSceneDirectObjectIncompleteByScanCapCount;
    summary.semanticSceneDirectObjectIncompleteByAlphaPolicyCount =
        g_shadowSceneStats.semanticSceneDirectObjectIncompleteByAlphaPolicyCount;
    summary.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount =
        g_shadowSceneStats
            .semanticSceneDirectObjectIncompleteBySliceUnresolvedCount;
    summary.semanticSceneDirectObjectIncompleteByPacketBuildFailCount =
        g_shadowSceneStats
            .semanticSceneDirectObjectIncompleteByPacketBuildFailCount;
    summary.semanticSceneDirectObjectIncompleteByAppendFailCount =
        g_shadowSceneStats.semanticSceneDirectObjectIncompleteByAppendFailCount;
    summary.semanticSceneDirectSubmittedCompleteObjectCount =
        g_shadowSceneStats.semanticSceneDirectSubmittedCompleteObjectCount;
    summary.semanticSceneDirectSubmittedPartialObjectCount =
        g_shadowSceneStats.semanticSceneDirectSubmittedPartialObjectCount;
    summary.semanticSceneDirectPreparedSliceAuthoritativeCount =
        g_shadowSceneStats.semanticSceneDirectPreparedSliceAuthoritativeCount;
    summary.semanticSceneDirectPreparedSliceFallbackLayerIndexCount =
        g_shadowSceneStats
            .semanticSceneDirectPreparedSliceFallbackLayerIndexCount;
    summary.semanticSceneDirectPreparedSliceMissingCount =
        g_shadowSceneStats.semanticSceneDirectPreparedSliceMissingCount;
    summary.semanticScenePreparedProbeAttemptCount =
        g_shadowSceneStats.semanticScenePreparedProbeAttemptCount;
    summary.semanticScenePreparedProbeContextReadyCount =
        g_shadowSceneStats.semanticScenePreparedProbeContextReadyCount;
    summary.semanticScenePreparedProbeBackingReadableCount =
        g_shadowSceneStats.semanticScenePreparedProbeBackingReadableCount;
    summary.semanticScenePreparedSliceRecordedCount =
        g_shadowSceneStats.semanticScenePreparedSliceRecordedCount;
    summary.semanticScenePreparedSliceQueryAttemptCount =
        g_shadowSceneStats.semanticScenePreparedSliceQueryAttemptCount;
    summary.semanticScenePreparedSliceQueryHitCount =
        g_shadowSceneStats.semanticScenePreparedSliceQueryHitCount;
    summary.semanticScenePreparedSliceQueryMissCount =
        g_shadowSceneStats.semanticScenePreparedSliceQueryMissCount;
    summary.semanticSceneShadowManifestObjectCount =
        g_shadowSceneStats.semanticSceneShadowManifestObjectCount;
    summary.semanticSceneShadowManifestPartCount =
        g_shadowSceneStats.semanticSceneShadowManifestPartCount;
    summary.semanticSceneShadowManifestStableObjectCount =
        g_shadowSceneStats.semanticSceneShadowManifestStableObjectCount;
    summary.semanticSceneShadowManifestNewObjectCount =
        g_shadowSceneStats.semanticSceneShadowManifestNewObjectCount;
    summary.semanticSceneShadowManifestExpiredObjectCount =
        g_shadowSceneStats.semanticSceneShadowManifestExpiredObjectCount;
    summary.semanticSceneShadowManifestFreshPartCount =
        g_shadowSceneStats.semanticSceneShadowManifestFreshPartCount;
    summary.semanticSceneShadowManifestLeaseablePartCount =
        g_shadowSceneStats.semanticSceneShadowManifestLeaseablePartCount;
    summary.semanticSceneShadowManifestPoseStalePartCount =
        g_shadowSceneStats.semanticSceneShadowManifestPoseStalePartCount;
    summary.semanticSceneShadowManifestSliceStalePartCount =
        g_shadowSceneStats.semanticSceneShadowManifestSliceStalePartCount;
    summary.semanticSceneShadowManifestExpiredPartCount =
        g_shadowSceneStats.semanticSceneShadowManifestExpiredPartCount;
    summary.semanticSceneShadowManifestMultiSlicePartCount =
        g_shadowSceneStats.semanticSceneShadowManifestMultiSlicePartCount;
    summary.semanticSceneShadowManifestPayload11CChurnCount =
        g_shadowSceneStats.semanticSceneShadowManifestPayload11CChurnCount;
    summary.semanticSceneShadowManifestRenderablePartChurnCount =
        g_shadowSceneStats
            .semanticSceneShadowManifestRenderablePartChurnCount;
    summary.semanticSceneShadowManifestCModelPoseHitCount =
        g_shadowSceneStats.semanticSceneShadowManifestCModelPoseHitCount;
    summary.semanticSceneShadowManifestCModelPoseMissCount =
        g_shadowSceneStats.semanticSceneShadowManifestCModelPoseMissCount;
    summary.semanticSceneShadowManifestCModelPoseNoRuntimeCount =
        g_shadowSceneStats.semanticSceneShadowManifestCModelPoseNoRuntimeCount;
    summary.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr =
        g_shadowSceneStats
            .semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr;
    summary.semanticSceneShadowManifestCModelPoseLastMatrixCount =
        g_shadowSceneStats.semanticSceneShadowManifestCModelPoseLastMatrixCount;
    summary.semanticSceneShadowManifestCModelPoseLastMatrixHash =
        g_shadowSceneStats.semanticSceneShadowManifestCModelPoseLastMatrixHash;
    summary.semanticSceneSubmittedObjectJaccardMilli =
        g_shadowSceneStats.semanticSceneSubmittedObjectJaccardMilli;
    summary.semanticSceneSubmittedPartJaccardMilli =
        g_shadowSceneStats.semanticSceneSubmittedPartJaccardMilli;
    summary.semanticSceneVisibleLookupPartLayerHitCount =
        g_shadowSceneStats.semanticSceneVisibleLookupPartLayerHitCount;
    summary.semanticSceneVisibleLookupSingleFallbackCount =
        g_shadowSceneStats.semanticSceneVisibleLookupSingleFallbackCount;
    summary.semanticSceneVisibleLookupMissCount =
        g_shadowSceneStats.semanticSceneVisibleLookupMissCount;
    summary.semanticSceneDirectMainWorldBackingNotCheckedCount =
        g_shadowSceneStats.semanticSceneDirectMainWorldBackingNotCheckedCount;
    summary.semanticSceneDirectMainWorldBackingPassCount =
        g_shadowSceneStats.semanticSceneDirectMainWorldBackingPassCount;
    summary.semanticSceneDirectMainWorldBackingFailNoRenderablePartCount =
        g_shadowSceneStats
            .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount;
    summary.semanticSceneDirectMainWorldBackingFailLookupMissCount =
        g_shadowSceneStats.semanticSceneDirectMainWorldBackingFailLookupMissCount;
    summary.semanticSceneDirectMainWorldBackingFailNonMainQueueCount =
        g_shadowSceneStats
            .semanticSceneDirectMainWorldBackingFailNonMainQueueCount;
    summary.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount =
        g_shadowSceneStats
            .semanticSceneDirectMainWorldBackingFailNonWorldGroupCount;
    summary.semanticSceneDirectMainWorldBackingFailIdentityMismatchCount =
        g_shadowSceneStats
            .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount;
    summary.semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount =
        g_shadowSceneStats
            .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount;
    summary.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount =
        g_shadowSceneStats
            .semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount;
    // Phase 7.2: single-caster flicker diagnostics
    summary.semanticSceneDirectLastSubmittedSceneNode =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedSceneNode;
    summary.semanticSceneDirectLastSubmittedPaletteHash =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedPaletteHash;
    summary.semanticSceneDirectLastSubmittedGroupHash =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedGroupHash;
    summary.semanticSceneDirectLastSubmittedStableGroupHash =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedStableGroupHash;
    summary.semanticSceneDirectLastSubmittedStream1Ptr =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedStream1Ptr;
    summary.semanticSceneDirectLastSubmittedGeometrySourceHash =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedGeometrySourceHash;
    summary.semanticSceneDirectLastSubmittedRenderablePart =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedRenderablePart;
    summary.semanticSceneDirectLastSubmittedMeshData =
        g_shadowSceneStats.semanticSceneDirectLastSubmittedMeshData;
    summary.semanticSceneDirectPaletteHashChurnCount =
        g_shadowSceneStats.semanticSceneDirectPaletteHashChurnCount;
    summary.semanticSceneDirectGroupHashChurnCount =
        g_shadowSceneStats.semanticSceneDirectGroupHashChurnCount;
    summary.semanticSceneDirectStableGroupHashChurnCount =
        g_shadowSceneStats.semanticSceneDirectStableGroupHashChurnCount;
    summary.semanticSceneDirectStream1PtrChurnCount =
        g_shadowSceneStats.semanticSceneDirectStream1PtrChurnCount;
    summary.semanticSceneDirectGeometrySourceHashChurnCount =
        g_shadowSceneStats.semanticSceneDirectGeometrySourceHashChurnCount;
    summary.semanticSceneDirectSameCasterComparisonCount =
        g_shadowSceneStats.semanticSceneDirectSameCasterComparisonCount;
    summary.semanticSceneDirectIdentitySkippedChurnCount =
        g_shadowSceneStats.semanticSceneDirectIdentitySkippedChurnCount;
    summary.semanticSceneDirectPaletteRootDeltaSampleCount =
        g_shadowSceneStats.semanticSceneDirectPaletteRootDeltaSampleCount;
    summary.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount =
        g_shadowSceneStats
            .semanticSceneDirectPaletteRootHashChangedTinyDeltaCount;
    summary.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount =
        g_shadowSceneStats
            .semanticSceneDirectPaletteRootHashChangedSmallDeltaCount;
    summary.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount =
        g_shadowSceneStats
            .semanticSceneDirectPaletteRootHashChangedMediumDeltaCount;
    summary.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount =
        g_shadowSceneStats
            .semanticSceneDirectPaletteRootHashChangedLargeDeltaCount;
    summary.semanticSceneDirectPaletteRootMaxDeltaMilli =
        g_shadowSceneStats.semanticSceneDirectPaletteRootMaxDeltaMilli;
    summary.semanticSceneDirectSelectionKeyUnitPtrCount =
        g_shadowSceneStats.semanticSceneDirectSelectionKeyUnitPtrCount;
    summary.semanticSceneDirectSelectionKeyJHandleCount =
        g_shadowSceneStats.semanticSceneDirectSelectionKeyJHandleCount;
    summary.semanticSceneDirectSelectionKeyRuntimeModelCount =
        g_shadowSceneStats.semanticSceneDirectSelectionKeyRuntimeModelCount;
    summary.semanticSceneDirectSelectionKeyWorldObjectCount =
        g_shadowSceneStats.semanticSceneDirectSelectionKeyWorldObjectCount;
    summary.semanticSceneDirectSelectionKeySceneNodeCount =
        g_shadowSceneStats.semanticSceneDirectSelectionKeySceneNodeCount;
    summary.semanticSceneDirectSelectionKeyModelMeshCount =
        g_shadowSceneStats.semanticSceneDirectSelectionKeyModelMeshCount;
    summary.semanticSceneDirectSelectionKeyRenderablePartCount =
        g_shadowSceneStats.semanticSceneDirectSelectionKeyRenderablePartCount;
    summary.semanticSceneLastAppendedGeometrySourceHash =
        g_shadowSceneStats.semanticSceneLastAppendedGeometrySourceHash;
    summary.semanticSceneLastAppendedGeometryId =
        g_shadowSceneStats.semanticSceneLastAppendedGeometryId;
    summary.semanticSceneLastAppendedRenderablePart =
        g_shadowSceneStats.semanticSceneLastAppendedRenderablePart;
    summary.semanticSceneLastAppendedMeshData =
        g_shadowSceneStats.semanticSceneLastAppendedMeshData;
    summary.terrainS1CaptureAttemptCount =
        g_shadowSceneStats.terrainS1CaptureAttemptCount;
    summary.terrainS1CaptureAcceptedCount =
        g_shadowSceneStats.terrainS1CaptureAcceptedCount;
    summary.terrainS1WorldIdentityLikeCount =
        g_shadowSceneStats.terrainS1WorldIdentityLikeCount;
    summary.terrainS1WorldNonIdentityCount =
        g_shadowSceneStats.terrainS1WorldNonIdentityCount;
    summary.terrainS1WorldNonFiniteCount =
        g_shadowSceneStats.terrainS1WorldNonFiniteCount;
    summary.terrainS1ForceIdentityWorldCount =
        g_shadowSceneStats.terrainS1ForceIdentityWorldCount;
    summary.terrainS1WorldMatrixHash =
        g_shadowSceneStats.terrainS1WorldMatrixHash;
    summary.terrainS1WorldTranslationMilliMax =
        g_shadowSceneStats.terrainS1WorldTranslationMilliMax;
    for (size_t i = 0u; i < summary.shadowCasterStageHistogram.size(); ++i) {
      summary.shadowCasterStageHistogram[i] =
          g_shadowSceneStats.shadowCasterStageHistogram[i];
    }
    for (size_t i = 0u; i < summary.shadowCasterCategoryHistogram.size(); ++i) {
      summary.shadowCasterCategoryHistogram[i] =
          g_shadowSceneStats.shadowCasterCategoryHistogram[i];
    }
    summary.stage13CaptureAttemptCount =
        g_shadowSceneStats.stage13CaptureAttemptCount;
    summary.stage13CaptureRejectedNoDemandCount =
        g_shadowSceneStats.stage13CaptureRejectedNoDemandCount;
    summary.stage13CaptureRejectedAfterBeforeUiCount =
        g_shadowSceneStats.stage13CaptureRejectedAfterBeforeUiCount;
    summary.stage13CaptureConsideredCount =
        g_shadowSceneStats.stage13CaptureConsideredCount;
    summary.beforeUiStage13BoundaryCandidateCount =
        g_shadowSceneStats.beforeUiStage13BoundaryCandidateCount;
    summary.beforeUiStage13BoundaryCommitCount =
        g_shadowSceneStats.beforeUiStage13BoundaryCommitCount;
    summary.stage13RetentionBaseEligibleCount =
        g_shadowSceneStats.stage13RetentionBaseEligibleCount;
    summary.stage13SourcePositionInvalidCount =
        g_shadowSceneStats.stage13SourcePositionInvalidCount;
    summary.stage13SourceIndexInvalidCount =
        g_shadowSceneStats.stage13SourceIndexInvalidCount;
    summary.stage13SourceIdentityValidCount =
        g_shadowSceneStats.stage13SourceIdentityValidCount;
    summary.stage13SourceIdentityHitCount =
        g_shadowSceneStats.stage13SourceIdentityHitCount;
    summary.stage13SourceIdentityMissCount =
        g_shadowSceneStats.stage13SourceIdentityMissCount;
    summary.stage13StrongScanCount =
        g_shadowSceneStats.stage13StrongScanCount;
    summary.stage13SnapshotBuildCount =
        g_shadowSceneStats.stage13SnapshotBuildCount;
    summary.stage13SnapshotContentRekeyCount =
        g_shadowSceneStats.stage13SnapshotContentRekeyCount;
    summary.stage13FreezeCopyBytes =
        g_shadowSceneStats.stage13FreezeCopyBytes;
    summary.stage13CpuSnapshotCopyBytes =
        g_shadowSceneStats.stage13CpuSnapshotCopyBytes;
    summary.stage13RetentionSnapshotBytes =
        g_shadowSceneStats.stage13RetentionSnapshotBytes;
    summary.stage13RetainedEntryCountMax =
        g_shadowSceneStats.stage13RetainedEntryCountMax;
    summary.stage13RetainedContentMatchCount =
        g_shadowSceneStats.stage13RetainedContentMatchCount;
    summary.stage13RetainedIdentityMatchCount =
        g_shadowSceneStats.stage13RetainedIdentityMatchCount;
    summary.stage13RetainedWorldMatchCount =
        g_shadowSceneStats.stage13RetainedWorldMatchCount;
    summary.stage13RetainedMaterialMatchCount =
        g_shadowSceneStats.stage13RetainedMaterialMatchCount;
    summary.stage13RetainedLayoutMatchCount =
        g_shadowSceneStats.stage13RetainedLayoutMatchCount;
    summary.stage13RetainedAllSemanticMatchCount =
        g_shadowSceneStats.stage13RetainedAllSemanticMatchCount;
    // submitted/replay/executed reconciliation
    summary.semanticSceneShadowCastersCount =
        g_shadowSceneStats.semanticSceneShadowCastersCount;
    summary.semanticSceneReplayDrawsCount =
        g_shadowSceneStats.semanticSceneReplayDrawsCount;
    summary.semanticSceneShadowMapDrawnCasters =
        g_shadowSceneStats.semanticSceneShadowMapDrawnCasters;
    summary.semanticSceneShadowMapCascadeCulledCount =
        g_shadowSceneStats.semanticSceneShadowMapCascadeCulledCount;
    summary.semanticSceneShadowMapSkinnedCasterCount =
        g_shadowSceneStats.semanticSceneShadowMapSkinnedCasterCount;
    summary.semanticSceneShadowMapSkinnedPreparedCount =
        g_shadowSceneStats.semanticSceneShadowMapSkinnedPreparedCount;
    summary.semanticSceneShadowMapSkinnedInvalidBufferCount =
        g_shadowSceneStats.semanticSceneShadowMapSkinnedInvalidBufferCount;
    summary.semanticSceneShadowMapSkinnedInvalidPipelineCount =
        g_shadowSceneStats.semanticSceneShadowMapSkinnedInvalidPipelineCount;
    summary.semanticSceneShadowMapSkinnedDrawnCount =
        g_shadowSceneStats.semanticSceneShadowMapSkinnedDrawnCount;
    summary.semanticSceneShadowTaaActive =
        g_shadowSceneStats.semanticSceneShadowTaaActive;
    summary.semanticSceneReceiverReuseShadowMap =
        g_shadowSceneStats.semanticSceneReceiverReuseShadowMap;
    summary.semanticSceneReceiverInputValid =
        g_shadowSceneStats.semanticSceneReceiverInputValid;
    summary.semanticSceneReceiverInputRejectReason =
        g_shadowSceneStats.semanticSceneReceiverInputRejectReason;
    summary.semanticSceneReceiverNeedPass =
        g_shadowSceneStats.semanticSceneReceiverNeedPass;
    summary.semanticSceneReceiverNeedShadowMap =
        g_shadowSceneStats.semanticSceneReceiverNeedShadowMap;
    summary.semanticSceneReceiverHasCompleteShadowMap =
        g_shadowSceneStats.semanticSceneReceiverHasCompleteShadowMap;
    summary.semanticSceneReceiverHasUsableDirectionalShadow =
        g_shadowSceneStats.semanticSceneReceiverHasUsableDirectionalShadow;
    summary.semanticSceneReceiverActiveStrengthMilli =
        g_shadowSceneStats.semanticSceneReceiverActiveStrengthMilli;
    summary.semanticSceneReceiverUboStrengthMilli =
        g_shadowSceneStats.semanticSceneReceiverUboStrengthMilli;
    summary.semanticSceneReceiverDebugMode =
        g_shadowSceneStats.semanticSceneReceiverDebugMode;
    summary.semanticSceneReceiverCsmCascadeCount =
        g_shadowSceneStats.semanticSceneReceiverCsmCascadeCount;
    summary.semanticSceneReceiverRunEntryFlags =
        g_shadowSceneStats.semanticSceneReceiverRunEntryFlags;
    summary.semanticSceneReceiverRunEarlyReturnReason =
        g_shadowSceneStats.semanticSceneReceiverRunEarlyReturnReason;
    summary.semanticSceneShadowMapExecutedThisFrame =
        g_shadowSceneStats.semanticSceneShadowMapExecutedThisFrame;
    summary.semanticSceneReceiverSettingsShadowsEnabled =
        g_shadowSceneStats.semanticSceneReceiverSettingsShadowsEnabled;
    summary.semanticSceneReceiverSettingsOutlineEnabled =
        g_shadowSceneStats.semanticSceneReceiverSettingsOutlineEnabled;
    summary.semanticSceneReceiverSettingsRawStrengthMilli =
        g_shadowSceneStats.semanticSceneReceiverSettingsRawStrengthMilli;
    summary.semanticSceneReceiverComputedShadowStrengthMilli =
        g_shadowSceneStats.semanticSceneReceiverComputedShadowStrengthMilli;
    summary.semanticSceneReceiverHasSunShadow =
        g_shadowSceneStats.semanticSceneReceiverHasSunShadow;
    summary.semanticSceneReceiverHasPointShadow =
        g_shadowSceneStats.semanticSceneReceiverHasPointShadow;
    summary.semanticSceneReceiverNeedOutlinePass =
        g_shadowSceneStats.semanticSceneReceiverNeedOutlinePass;
    summary.semanticSceneReceiverZeroStrengthFrameCount =
        g_shadowSceneStats.semanticSceneReceiverZeroStrengthFrameCount;
    summary.semanticSceneReceiverDrawnWithZeroStrengthCount =
        g_shadowSceneStats.semanticSceneReceiverDrawnWithZeroStrengthCount;
    summary.semanticSceneReceiverNoCompleteShadowMapCount =
        g_shadowSceneStats.semanticSceneReceiverNoCompleteShadowMapCount;
    summary.semanticSceneReceiverNoShadowMapImageCount =
        g_shadowSceneStats.semanticSceneReceiverNoShadowMapImageCount;
    summary.semanticSceneReceiverNoShadowMapSampleViewCount =
        g_shadowSceneStats.semanticSceneReceiverNoShadowMapSampleViewCount;
    summary.semanticSceneReceiverNoCandidateCsmCount =
        g_shadowSceneStats.semanticSceneReceiverNoCandidateCsmCount;
    summary.semanticSceneReceiverCsmFallbackToLastGoodCount =
        g_shadowSceneStats.semanticSceneReceiverCsmFallbackToLastGoodCount;
    summary.semanticSceneReceiverHoldInvalidCsmCount =
        g_shadowSceneStats.semanticSceneReceiverHoldInvalidCsmCount;
    summary.semanticSceneReceiverHoldEmptyReplayCount =
        g_shadowSceneStats.semanticSceneReceiverHoldEmptyReplayCount;
    summary.semanticSceneReceiverHoldIdentityChurnCount =
        g_shadowSceneStats.semanticSceneReceiverHoldIdentityChurnCount;
    summary.semanticSceneReceiverReuseInvalidatedAfterEnsureCount =
        g_shadowSceneStats.semanticSceneReceiverReuseInvalidatedAfterEnsureCount;
    summary.semanticSceneShadowMapRenderSkippedNoResourcesCount =
        g_shadowSceneStats.semanticSceneShadowMapRenderSkippedNoResourcesCount;
    summary.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount =
        g_shadowSceneStats
            .semanticSceneShadowMapRenderSkippedNoMatrixBufferCount;
    summary.semanticSceneReceiverViewportX =
        g_shadowSceneStats.semanticSceneReceiverViewportX;
    summary.semanticSceneReceiverViewportY =
        g_shadowSceneStats.semanticSceneReceiverViewportY;
    summary.semanticSceneReceiverViewportWidth =
        g_shadowSceneStats.semanticSceneReceiverViewportWidth;
    summary.semanticSceneReceiverViewportHeight =
        g_shadowSceneStats.semanticSceneReceiverViewportHeight;
    summary.dynamicPoseSignature = g_shadowSceneStats.dynamicPoseSignature;
    summary.semanticSceneLastInputDrawCount =
        g_shadowSceneStats.semanticSceneLastInputDrawCount;
    summary.semanticSceneLastInputSkinnedCount =
        g_shadowSceneStats.semanticSceneLastInputSkinnedCount;
    summary.semanticSceneLastSubmittedDrawCount =
        g_shadowSceneStats.semanticSceneLastSubmittedDrawCount;
    summary.semanticSceneLastUnitsOnlyFilteredCount =
        g_shadowSceneStats.semanticSceneLastUnitsOnlyFilteredCount;
    summary.semanticSceneCatchupAttemptCount =
        g_shadowSceneStats.semanticSceneCatchupAttemptCount;
    summary.semanticSceneCatchupSuccessCount =
        g_shadowSceneStats.semanticSceneCatchupSuccessCount;
    summary.semanticSceneSkippedEmptyFrameCount =
        g_shadowSceneStats.semanticSceneSkippedEmptyFrameCount;
    summary.semanticSceneZeroSubmitCount =
        g_shadowSceneStats.semanticSceneZeroSubmitCount;
    summary.semanticSceneSelectedFrameEligibleZeroCount =
        g_shadowSceneStats.semanticSceneSelectedFrameEligibleZeroCount;
    summary.semanticSceneReusableFrameForcedCount =
        g_shadowSceneStats.semanticSceneReusableFrameForcedCount;
    summary.semanticSceneReusableFrameUnavailableCount =
        g_shadowSceneStats.semanticSceneReusableFrameUnavailableCount;
    summary.semanticSceneReusableFrameRejectedNativeValidationCount =
        g_shadowSceneStats
            .semanticSceneReusableFrameRejectedNativeValidationCount;
    summary.semanticSceneLastFrameSerial =
        g_shadowSceneStats.semanticSceneLastFrameSerial;
    summary.semanticSceneLastSelectedFrameSerial =
        g_shadowSceneStats.semanticSceneLastSelectedFrameSerial;
    summary.semanticSceneLastReusableFrameSerial =
        g_shadowSceneStats.semanticSceneLastReusableFrameSerial;
    summary.semanticSceneLastSourcePublishRevision =
        g_shadowSceneStats.semanticSceneLastSourcePublishRevision;
    summary.semanticSceneLastTargetPublishRevision =
        g_shadowSceneStats.semanticSceneLastTargetPublishRevision;
    summary.semanticSceneBypassUnitLikeCount =
        g_shadowSceneStats.semanticSceneBypassUnitLikeCount;
    summary.semanticSceneBypassUnitLikeWithRuntimeModel =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithRuntimeModel;
    summary.semanticSceneBypassUnitLikeWithModelResource =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithModelResource;
    summary.semanticSceneBypassUnitLikeWithPose =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithPose;
    summary.semanticSceneBypassUnitLikeWithRenderable =
        g_shadowSceneStats.semanticSceneBypassUnitLikeWithRenderable;
    summary.semanticSceneBypassPublishedVisibleCandidate =
        g_shadowSceneStats.semanticSceneBypassPublishedVisibleCandidate;
    summary.semanticSceneBypassPublishMiss =
        g_shadowSceneStats.semanticSceneBypassPublishMiss;
    summary.semanticSceneSkippedUnitsOnlyFilter =
        g_shadowSceneStats.semanticSceneSkippedUnitsOnlyFilter;
    summary.semanticSceneAcceptedExplicitResourceOwnerRigid =
        g_shadowSceneStats.semanticSceneAcceptedExplicitResourceOwnerRigid;
    summary.semanticSceneRejectedGeometry =
        g_shadowSceneStats.semanticSceneRejectedGeometry;
    summary.semanticSceneRejectedGeometryFrameLocal =
        g_shadowSceneStats.semanticSceneRejectedGeometryFrameLocal;
    summary.semanticSceneRejectedGeometryPersistent =
        g_shadowSceneStats.semanticSceneRejectedGeometryPersistent;
    const auto currentDrawSummary =
        QueryCurrentDrawContractDiagnosticsSummary();
    summary.currentDrawContractPublishAttemptCount =
        currentDrawSummary.publishAttemptCount;
    summary.currentDrawContractPublishReadyCount =
        currentDrawSummary.publishReadyCount;
    summary.currentDrawContractPublishMissNoRenderablePart =
        currentDrawSummary.publishMissNoRenderablePart;
    summary.currentDrawContractPublishMissNoMeshPayload =
        currentDrawSummary.publishMissNoMeshPayload;
    summary.currentDrawContractPublishMissInvalidPaletteSlot =
        currentDrawSummary.publishMissInvalidPaletteSlot;
    summary.currentDrawContractPublishMissInvalidPaletteCount =
        currentDrawSummary.publishMissInvalidPaletteCount;
    summary.currentDrawContractPublishMissNoGlobalPalette =
        currentDrawSummary.publishMissNoGlobalPalette;
    summary.currentDrawContractPublishSkippedNonWorldContext =
        currentDrawSummary.publishSkippedNonWorldContext;
    summary.currentDrawContractPublishSkippedSmallViewport =
        currentDrawSummary.publishSkippedSmallViewport;
    summary.currentDrawContractQueryAttemptCount =
        currentDrawSummary.queryAttemptCount;
    summary.currentDrawContractQueryHitCount =
        currentDrawSummary.queryHitCount;
    summary.currentDrawContractQueryMissNoRecord =
        currentDrawSummary.queryMissNoRecord;
    summary.currentDrawContractQueryMissFrameTagMismatch =
        currentDrawSummary.queryMissFrameTagMismatch;
    summary.currentDrawContractQueryMissCacheCollision =
        currentDrawSummary.queryMissCacheCollision;
    summary.currentDrawCapturedPaletteQueryAttemptCount =
        currentDrawSummary.capturedPaletteQueryAttemptCount;
    summary.currentDrawCapturedPaletteQueryHitCount =
        currentDrawSummary.capturedPaletteQueryHitCount;
    summary.currentDrawCapturedPaletteMissNoContract =
        currentDrawSummary.capturedPaletteMissNoContract;
    summary.currentDrawCapturedPaletteMissInvalidCount =
        currentDrawSummary.capturedPaletteMissInvalidCount;
    summary.currentDrawCapturedPaletteMissNoSnapshot =
        currentDrawSummary.capturedPaletteMissNoSnapshot;
    summary.currentDrawCapturedPaletteMissUnreadablePalette =
        currentDrawSummary.capturedPaletteMissUnreadablePalette;
    summary.currentDrawGroupSlotDecodeAttemptCount =
        currentDrawSummary.groupSlotDecodeAttemptCount;
    summary.currentDrawGroupSlotDecodeHitCount =
        currentDrawSummary.groupSlotDecodeHitCount;
    summary.currentDrawGroupSlotDecodeMissDisabledStream =
        currentDrawSummary.groupSlotDecodeMissDisabledStream;
    summary.currentDrawGroupSlotDecodeMissNoStream =
        currentDrawSummary.groupSlotDecodeMissNoStream;
    summary.currentDrawGroupSlotDecodeMissUnreadableStream =
        currentDrawSummary.groupSlotDecodeMissUnreadableStream;
    summary.currentDrawGroupSlotDecodeMissGroupOutOfRange =
        currentDrawSummary.groupSlotDecodeMissGroupOutOfRange;
    summary.currentDrawPreparedSliceProbeAttemptCount =
        currentDrawSummary.preparedSliceProbeAttemptCount;
    summary.currentDrawPreparedSliceProbeContextReadyCount =
        currentDrawSummary.preparedSliceProbeContextReadyCount;
    summary.currentDrawPreparedSliceProbeBackingReadableCount =
        currentDrawSummary.preparedSliceProbeBackingReadableCount;
    summary.currentDrawPreparedSliceRecordedCount =
        currentDrawSummary.preparedSliceRecordedCount;
    summary.currentDrawPreparedSliceQueryAttemptCount =
        currentDrawSummary.preparedSliceQueryAttemptCount;
    summary.currentDrawPreparedSliceQueryHitCount =
        currentDrawSummary.preparedSliceQueryHitCount;
    summary.currentDrawPreparedSliceQueryMissCount =
        currentDrawSummary.preparedSliceQueryMissCount;
    summary.currentDrawStream1PublishNoStreamCount =
        currentDrawSummary.stream1PublishNoStreamCount;
    summary.currentDrawStream1PublishStride0Count =
        currentDrawSummary.stream1PublishStride0Count;
    summary.currentDrawStream1PublishStride1Count =
        currentDrawSummary.stream1PublishStride1Count;
    summary.currentDrawStream1PublishStride8Count =
        currentDrawSummary.stream1PublishStride8Count;
    summary.currentDrawStream1PublishStride12Count =
        currentDrawSummary.stream1PublishStride12Count;
    summary.currentDrawStream1PublishStride16Count =
        currentDrawSummary.stream1PublishStride16Count;
    summary.currentDrawStream1PublishStride20Count =
        currentDrawSummary.stream1PublishStride20Count;
    summary.currentDrawStream1PublishStrideOtherCount =
        currentDrawSummary.stream1PublishStrideOtherCount;
    summary.currentDrawStream1PublishLastRawStride =
        currentDrawSummary.stream1PublishLastRawStride;
    summary.currentDrawStream1PublishMaxRawStride =
        currentDrawSummary.stream1PublishMaxRawStride;
    summary.currentDrawLastRenderablePart =
        currentDrawSummary.lastRenderablePart;
    summary.currentDrawLastSceneNode =
        currentDrawSummary.lastSceneNode;
    summary.currentDrawLastMeshPayloadPtr =
        currentDrawSummary.lastMeshPayloadPtr;
    summary.currentDrawLastPaletteAddress =
        currentDrawSummary.lastPaletteAddress;
    summary.currentDrawLastStream1Ptr =
        currentDrawSummary.lastStream1Ptr;
    summary.currentDrawLastCaptureSerial =
        currentDrawSummary.lastCaptureSerial;
    summary.currentDrawLastPaletteSlotIndex =
        currentDrawSummary.lastPaletteSlotIndex;
    summary.currentDrawLastCapturedPaletteCount =
        currentDrawSummary.lastCapturedPaletteCount;
    summary.currentDrawLastStream1Stride =
        currentDrawSummary.lastStream1Stride;
    summary.currentDrawLastFrameTag =
        currentDrawSummary.lastFrameTag;
    summary.currentDrawLastVisibleFrameSerial =
        currentDrawSummary.lastVisibleFrameSerial;
    summary.currentDrawLastRenderFrameIndex =
        currentDrawSummary.lastRenderFrameIndex;
    summary.currentDrawLastSmallViewportWidth =
        currentDrawSummary.lastSmallViewportWidth;
    summary.currentDrawLastSmallViewportHeight =
        currentDrawSummary.lastSmallViewportHeight;
    summary.currentDrawLastMissReason =
        currentDrawSummary.lastMissReason;
    // Phase 7.35 Pose-lag 诊断：submit lag 分桶透传。
    summary.submitPaletteFrameLag0Count =
        currentDrawSummary.submitPaletteFrameLag0Count;
    summary.submitPaletteFrameLag1Count =
        currentDrawSummary.submitPaletteFrameLag1Count;
    summary.submitPaletteFrameLag2Count =
        currentDrawSummary.submitPaletteFrameLag2Count;
    summary.submitPaletteFrameLag3To5Count =
        currentDrawSummary.submitPaletteFrameLag3To5Count;
    summary.submitPaletteFrameLag6PlusCount =
        currentDrawSummary.submitPaletteFrameLag6PlusCount;
    summary.submitPaletteFrameLagMax =
        currentDrawSummary.submitPaletteFrameLagMax;
    summary.submitPaletteFrameLagSampleCount =
        currentDrawSummary.submitPaletteFrameLagSampleCount;
    // Phase 7.39：palette 内容年龄分桶透传。
    summary.submitPaletteContentAgeLag0Count =
        currentDrawSummary.submitPaletteContentAgeLag0Count;
    summary.submitPaletteContentAgeLag1Count =
        currentDrawSummary.submitPaletteContentAgeLag1Count;
    summary.submitPaletteContentAgeLag2Count =
        currentDrawSummary.submitPaletteContentAgeLag2Count;
    summary.submitPaletteContentAgeLag3To5Count =
        currentDrawSummary.submitPaletteContentAgeLag3To5Count;
    summary.submitPaletteContentAgeLag6PlusCount =
        currentDrawSummary.submitPaletteContentAgeLag6PlusCount;
    summary.submitPaletteContentAgeMax =
        currentDrawSummary.submitPaletteContentAgeMax;
    summary.submitPaletteContentAgeSampleCount =
        currentDrawSummary.submitPaletteContentAgeSampleCount;
    summary.submitPaletteContentAgeUnknownCount =
        currentDrawSummary.submitPaletteContentAgeUnknownCount;
    // Phase 7.35 路径 1 诊断：capture 端 Exact 查询分布透传。
    summary.paletteCaptureExactHitCount =
        currentDrawSummary.paletteCaptureExactHitCount;
    summary.paletteCaptureBestEffortHitCount =
        currentDrawSummary.paletteCaptureBestEffortHitCount;
    summary.paletteCaptureSlotOverflowMissCount =
        currentDrawSummary.paletteCaptureSlotOverflowMissCount;
    summary.paletteCaptureInvalidEntryMissCount =
        currentDrawSummary.paletteCaptureInvalidEntryMissCount;
    summary.paletteCaptureFrameTagMismatchMissCount =
        currentDrawSummary.paletteCaptureFrameTagMismatchMissCount;
    summary.paletteCaptureShortResultMissCount =
        currentDrawSummary.paletteCaptureShortResultMissCount;
    // Phase 7.35 路径 2 诊断：submit-side live rebuild 分桶透传。
    summary.submitLiveRebuildAttemptCount =
        currentDrawSummary.submitLiveRebuildAttemptCount;
    summary.submitLiveRebuildHitCount =
        currentDrawSummary.submitLiveRebuildHitCount;
    summary.submitLiveRebuildMissCount =
        currentDrawSummary.submitLiveRebuildMissCount;
    summary.submitLiveRebuildAppliedCount =
        currentDrawSummary.submitLiveRebuildAppliedCount;
    // AlphaTest payload plumbing 诊断透传（Claude AlphaTest lane, Phase B）。
    //
    // 读取是无锁 atomic load，调用链全程不经过 mutex；即使生产侧与消费侧
    // 在不同帧/线程并发写入，这里拿到的也只是一份快照，用于 control plane
    // 侧做趋势判断，不作为帧精确时序的依据。
    {
      const auto alphaTestSnapshot =
          dxvk::war3::shadow::ReadWar3ShadowAlphaTestPayloadCountersSnapshot();
      summary.shadowAlphaTestPayloadAttemptCount =
          alphaTestSnapshot.attemptCount;
      summary.shadowAlphaTestPayloadHitCount = alphaTestSnapshot.hitCount;
      summary.shadowAlphaTestPayloadMissNoUvCount =
          alphaTestSnapshot.missNoUvCount;
      summary.shadowAlphaTestPayloadMissNoDiffuseCount =
          alphaTestSnapshot.missNoDiffuseCount;
      summary.shadowAlphaTestPayloadMissStageInvalidCount =
          alphaTestSnapshot.missStageInvalidCount;
      summary.shadowAlphaTestPayloadAppliedCount =
          alphaTestSnapshot.appliedCount;
      summary.shadowAlphaTestPayloadFallbackRejectCount =
          alphaTestSnapshot.fallbackRejectCount;
      summary.shadowAlphaTestPayloadStashCapturedCount =
          alphaTestSnapshot.stashCapturedCount;
      summary.shadowAlphaTestPayloadStashSkipNoSemanticKeyCount =
          alphaTestSnapshot.stashSkipNoSemanticKeyCount;
      summary.shadowAlphaTestPayloadStashSkipNoUvCount =
          alphaTestSnapshot.stashSkipNoUvCount;
      summary.shadowAlphaTestPayloadStashSkipNoDiffuseCount =
          alphaTestSnapshot.stashSkipNoDiffuseCount;
      summary.shadowAlphaTestPayloadStashSkipNoUploadCount =
          alphaTestSnapshot.stashSkipNoUploadCount;
      summary.shadowAlphaTestPayloadCacheEvictedCount =
          alphaTestSnapshot.cacheEvictedCount;
      summary.shadowAlphaTestPayloadCacheSizeGauge =
          alphaTestSnapshot.cacheSizeGauge;
      summary.shadowMetadataClassifiedCount =
          alphaTestSnapshot.metadataClassifiedCount;
      summary.shadowMetadataCapturedCount =
          alphaTestSnapshot.metadataCapturedCount;
      summary.shadowMetadataAppliedCount =
          alphaTestSnapshot.metadataAppliedCount;
      summary.shadowMetadataRejectedFrameCount =
          alphaTestSnapshot.metadataRejectedFrameCount;
      summary.shadowMetadataRejectedGenerationCount =
          alphaTestSnapshot.metadataRejectedGenerationCount;
      summary.shadowMetadataAmbiguousCount =
          alphaTestSnapshot.metadataAmbiguousCount;
      summary.shadowMetadataRejectedNoMaterialCount =
          alphaTestSnapshot.metadataRejectedNoMaterialCount;
      summary.shadowMetadataRejectedOpaqueCount =
          alphaTestSnapshot.metadataRejectedOpaqueCount;
      summary.shadowMetadataRejectedNoUvCount =
          alphaTestSnapshot.metadataRejectedNoUvCount;
      summary.shadowMetadataRejectedNoDiffuseCount =
          alphaTestSnapshot.metadataRejectedNoDiffuseCount;
      summary.shadowMetadataRejectedUploadCount =
          alphaTestSnapshot.metadataRejectedUploadCount;
      summary.shadowMetadataRejectedDuplicateCount =
          alphaTestSnapshot.metadataRejectedDuplicateCount;
      summary.shadowMetadataBlockerKnownRawcodeCount =
          alphaTestSnapshot.blockerKnownRawcodeCount;
      summary.shadowMetadataBlockerWidgetIdentityCount =
          alphaTestSnapshot.blockerWidgetIdentityCount;
      summary.shadowMetadataBlockerSmallFlatCount =
          alphaTestSnapshot.blockerSmallFlatCount;
      summary.shadowMetadataBlockerBelowGroundCount =
          alphaTestSnapshot.blockerBelowGroundCount;
      summary.shadowMetadataBlockerUnreadableCount =
          alphaTestSnapshot.blockerUnreadableCount;
      summary.shadowMetadataBlockerFinalLeakCount =
          alphaTestSnapshot.blockerFinalLeakCount;
    }
    summary.semanticFallbackPruned = g_shadowSceneStats.semanticFallbackPruned;
    summary.semanticFallbackPrunedByHandle =
        g_shadowSceneStats.semanticFallbackPrunedByHandle;
    summary.semanticFallbackPrunedByWorldObjectEntry =
        g_shadowSceneStats.semanticFallbackPrunedByWorldObjectEntry;
    summary.semanticFallbackPrunedBySceneNode =
        g_shadowSceneStats.semanticFallbackPrunedBySceneNode;
    summary.semanticFallbackPrunedByRuntimeModel =
        g_shadowSceneStats.semanticFallbackPrunedByRuntimeModel;
    summary.persistentGeometryCount =
        g_shadowSceneStats.persistentGeometryCount;
    summary.duplicateGeometryInstances =
        g_shadowSceneStats.duplicateGeometryInstances;
    summary.instancedGeometryDrawsSaved =
        g_shadowSceneStats.instancedGeometryDrawsSaved;
  }
  {
    std::lock_guard<std::mutex> lock(g_shadowCadenceMutex);
    summary.shadowCadenceSampleSerial = g_shadowCadenceNextSerial;
    summary.shadowCadenceSampleCountTotal = g_shadowCadenceSampleCountTotal;
    summary.shadowCadenceSameDynamicPoseStreak =
        g_shadowCadenceSameDynamicPoseStreak;
    summary.shadowCadenceSameDynamicPoseStreakMax =
        g_shadowCadenceSameDynamicPoseStreakMax;
    summary.shadowCadenceSameSceneFrameStreak =
        g_shadowCadenceSameSceneFrameStreak;
    summary.shadowCadenceSameSceneFrameStreakMax =
        g_shadowCadenceSameSceneFrameStreakMax;
    summary.shadowCadenceShadowMapReuseStreak =
        g_shadowCadenceShadowMapReuseStreak;
    summary.shadowCadenceShadowMapReuseStreakMax =
        g_shadowCadenceShadowMapReuseStreakMax;
    summary.shadowCadenceSampleCount = g_shadowCadenceSampleCount;
    const uint32_t first =
        (g_shadowCadenceWriteIndex +
         static_cast<uint32_t>(kShadowRuntimeCadenceSampleCapacity) -
         g_shadowCadenceSampleCount) %
        static_cast<uint32_t>(kShadowRuntimeCadenceSampleCapacity);
    for (uint32_t i = 0u; i < g_shadowCadenceSampleCount; ++i) {
      const uint32_t index =
          (first + i) %
          static_cast<uint32_t>(kShadowRuntimeCadenceSampleCapacity);
      summary.shadowCadenceSamples[i] = g_shadowCadenceSamples[index];
    }
  }
  const auto semanticCore =
      shadow::ShadowValidationRuntime::instance().snapshot();
  const auto buildState =
      shadow::ShadowValidationRuntime::instance().buildStateSnapshot();
  const auto publishedBundle =
      shadow::ShadowRuntimeContractCache::instance().snapshotBundleShared();
  const auto& publishedManifest = publishedBundle.manifest;
  summary.semanticContractCaptureSkippedStableSameFrame =
      publishedBundle.stats.contractCaptureSkippedStableSameFrame;
  summary.semanticContractCaptureSkippedEmpty =
      publishedBundle.stats.contractCaptureSkippedEmpty;
  summary.semanticContractCaptureSkippedDuplicateSameFrame =
      publishedBundle.stats.contractCaptureSkippedDuplicateSameFrame;
  // Phase 7.98 mini probe：widget identity hook 状态。
  {
    const auto widgetStats =
        ::dxvk::war3::hooks::GetWidgetIdentityHookStats();
    summary.widgetIdentityEnterCount = widgetStats.enterCount;
    summary.widgetIdentityMagicMatchedCount = widgetStats.magicMatchedCount;
    summary.widgetIdentityMagicMismatchCount = widgetStats.magicMismatchCount;
    summary.widgetIdentityCacheInsertCount = widgetStats.cacheInsertCount;
    summary.widgetIdentityCacheUpdateCount = widgetStats.cacheUpdateCount;
    summary.widgetIdentityHandleResolvedCount = widgetStats.handleResolvedCount;
    summary.widgetIdentityHandleMissingCount = widgetStats.handleMissingCount;
    summary.widgetIdentityCacheSize =
        ::dxvk::war3::hooks::GetWidgetIdentityCacheSize();
    summary.widgetIdentityInstallAttempted = widgetStats.installAttempted;
    summary.widgetIdentityInstallSucceeded = widgetStats.installSucceeded;
    summary.widgetIdentityInstallFailedAddrNull =
        widgetStats.installFailedAddrNull;
    summary.widgetIdentityInstallFailedEnvDisabled =
        widgetStats.installFailedEnvDisabled;
    summary.widgetIdentityInstallFailedMinHook =
        widgetStats.installFailedMinHook;
    // Phase 7.99：path blocker 拦截分桶（让 trace + control plane 直接看到
    // 拦了多少 + 在哪条出口）。
    {
      std::shared_lock<std::shared_mutex> sceneLock(g_shadowSceneStatsMutex);
      summary.semanticSceneRejectedPathBlockerCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerCount;
      summary.semanticSceneRejectedPathBlockerEarlyBypassCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerEarlyBypassCount;
      summary.semanticSceneRejectedPathBlockerEligibilityGateCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerEligibilityGateCount;
      summary.semanticSceneRejectedPathBlockerAppendEntryCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerAppendEntryCount;
      summary.semanticSceneRejectedPathBlockerAppendEntryByJHandleCount =
          g_shadowSceneStats
              .semanticSceneRejectedPathBlockerAppendEntryByJHandleCount;
      summary.semanticSceneRejectedPathBlockerAppendVbBlendCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerAppendVbBlendCount;
      summary.semanticSceneRejectedPathBlockerFastAppendCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerFastAppendCount;
      summary.semanticSceneRejectedPathBlockerDirectGroupedCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerDirectGroupedCount;
      summary.semanticSceneRejectedPathBlockerProducerCount =
          g_shadowSceneStats.semanticSceneRejectedPathBlockerProducerCount;
      summary.semanticSceneRejectedPathBlockerStaticSupplementCount =
          g_shadowSceneStats
              .semanticSceneRejectedPathBlockerStaticSupplementCount;
      summary.semanticSceneRejectedPathBlockerLegacyCaptureCount =
          g_shadowSceneStats
              .semanticSceneRejectedPathBlockerLegacyCaptureCount;
    }
    // Phase 7.100：WriteMaskRegion 静态阴影治理统计。
    summary.writeMaskRegionEnterCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionEnterCount();
    summary.writeMaskRegionRejectedIdx3Count =
        ::dxvk::war3::hooks::QueryWriteMaskRegionRejectedIdx3Count();
    summary.writeMaskRegionPassFogCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionPassFogCount();
    summary.writeMaskRegionPassLosCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionPassLosCount();
    summary.writeMaskRegionPassPathCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionPassPathCount();
    summary.writeMaskRegionPassOtherCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionPassOtherCount();
    // Phase 7.112：caller-aware 静态阴影屏蔽。
    summary.writeMaskRegionFromBuildingStampCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionFromBuildingStampCount();
    summary.writeMaskRegionRejectedBuildingCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionRejectedBuildingCount();
    summary.writeMaskRegionFromRegisterFootprintCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionFromRegisterFootprintCount();
    summary.writeMaskRegionFromRebuildMaskCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionFromRebuildMaskCount();
    summary.writeMaskRegionFromActorRuntimeCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionFromActorRuntimeCount();
    summary.writeMaskRegionFromForObjectCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionFromForObjectCount();
    summary.writeMaskRegionFromOtherCallerCount =
        ::dxvk::war3::hooks::QueryWriteMaskRegionFromOtherCallerCount();

    // Phase 7.116：DispatchToShape (建筑/装饰物/可破坏物 shadow footprint)。
    summary.dispatchToShapeEnterCount =
        ::dxvk::war3::hooks::QueryDispatchToShapeEnterCount();
    summary.dispatchToShapeRejectedCount =
        ::dxvk::war3::hooks::QueryDispatchToShapeRejectedCount();
    summary.dispatchToShapeFromRebuildMaskCount =
        ::dxvk::war3::hooks::QueryDispatchToShapeFromRebuildMaskCount();
    summary.dispatchToShapeFromShadowSetupCount =
        ::dxvk::war3::hooks::QueryDispatchToShapeFromShadowSetupCount();
    summary.dispatchToShapeFromOtherCallerCount =
        ::dxvk::war3::hooks::QueryDispatchToShapeFromOtherCallerCount();
    // Phase 7.108：ShadowProjector 永久统计。
    summary.projectorAddFromObjectEnterCount =
        ::dxvk::war3::hooks::QueryShadowProjectorAddFromObjectEnterCount();
    summary.projectorAddFromObjectBlockedCount =
        ::dxvk::war3::hooks::QueryShadowProjectorAddFromObjectBlockedCount();
    summary.projectorAddFromObjectFourCCExtractedCount =
        ::dxvk::war3::hooks::QueryShadowProjectorAddFromObjectFourCCExtractedCount();
    summary.projectorAddFromObjectFourCCMissCount =
        ::dxvk::war3::hooks::QueryShadowProjectorAddFromObjectFourCCMissCount();
    summary.projectorAddFromObjectBlockedFourCCCount =
        ::dxvk::war3::hooks::QueryShadowProjectorAddFromObjectBlockedFourCCCount();
    summary.projectorAddSimpleEnterCount =
        ::dxvk::war3::hooks::QueryShadowProjectorAddSimpleEnterCount();
    summary.projectorAddSimpleBlockedCount =
        ::dxvk::war3::hooks::QueryShadowProjectorAddSimpleBlockedCount();
    for (uint32_t i = 0u; i < 8u; ++i) {
      summary.projectorObservedFourCCSamples[i] =
          ::dxvk::war3::hooks::QueryShadowProjectorObservedFourCCSampleAt(i);
      summary.projectorBlockedFourCCSamples[i] =
          ::dxvk::war3::hooks::QueryShadowProjectorBlockedFourCCSampleAt(i);
    }
    // Phase 7.108b：shadowCasters append survey。
    summary.shadowAppendTotalCount =
        ::dxvk::war3_diag::QueryShadowAppendTotal();
    summary.shadowAppendRawcodeUniqueCount =
        ::dxvk::war3_diag::QueryShadowAppendUnique();
    for (uint32_t i = 0u; i < 16u; ++i) {
      summary.shadowAppendRawcodeSamples[i] =
          ::dxvk::war3_diag::QueryShadowAppendRawcodeAt(i);
    }
  }
  // Phase 7.97 诊断：直接拉 atomic getter，反映最近一次 ManifestCopy 真实
  // 遍历情况（不受 sameFrameDataNotGrowing publish 早退掩盖）。
  {
    auto& contractCache = shadow::ShadowRuntimeContractCache::instance();
    summary.semanticManifestCopyVisibleScanned =
        contractCache.lastManifestCopyVisibleScanned();
    summary.semanticManifestCopyAppended =
        contractCache.lastManifestCopyAppended();
    summary.semanticManifestCopyDeduplicatedSkipped =
        contractCache.lastManifestCopyDeduplicatedSkipped();
    summary.semanticManifestCopyRejectedSkipped =
        contractCache.lastManifestCopyRejectedSkipped();
    summary.semanticManifestCopySkipStableCount =
        contractCache.manifestCopySkipStableCount();
    // 透传 ManifestCopy 真实进入次数（应该 ≈ 帧数）
    summary.semanticManifestCopyEnterCount =
        contractCache.manifestCopyEnterCount();
    summary.semanticManifestCopyMaxScanned =
        contractCache.manifestCopyMaxScanned();
    summary.semanticManifestCopyTotalScanned =
        contractCache.manifestCopyTotalScanned();
    summary.semanticManifestCopyTotalChronoNs =
        contractCache.manifestCopyTotalChronoNs();
    summary.semanticManifestCopyMaxChronoNs =
        contractCache.manifestCopyMaxChronoNs();
    summary.semanticManifestResolveSourceCompleteSkipCount =
        contractCache.manifestResolveSourceCompleteSkipCount();
    summary.semanticManifestResolveLegacyCacheHitCount =
        contractCache.manifestResolveLegacyCacheHitCount();
    summary.semanticManifestResolveRawScanCount =
        contractCache.manifestResolveRawScanCount();
    summary.semanticManifestResolveRawScanEntryVisitCount =
        contractCache.manifestResolveRawScanEntryVisitCount();
    summary.semanticManifestResolveRawScanMissCount =
        contractCache.manifestResolveRawScanMissCount();
    summary.semanticManifestResolveVerifierAttemptCount =
        contractCache.manifestResolveVerifierAttemptCount();
    summary.semanticManifestResolveVerifierMismatchCount =
        contractCache.manifestResolveVerifierMismatchCount();
    summary.semanticManifestResolveMaxRuntimeGeosetCount =
        contractCache.manifestResolveMaxRuntimeGeosetCount();
    summary.semanticManifestModelResourceAttemptCount =
        contractCache.manifestModelResourceAttemptCount();
    summary.semanticManifestModelResourceCacheHitCount =
        contractCache.manifestModelResourceCacheHitCount();
    summary.semanticManifestModelResourceDeepResolveCount =
        contractCache.manifestModelResourceDeepResolveCount();
    summary.semanticManifestModelResourceNullResultCount =
        contractCache.manifestModelResourceNullResultCount();
    summary.semanticManifestModelResourceVerifierAttemptCount =
        contractCache.manifestModelResourceVerifierAttemptCount();
    summary.semanticManifestModelResourceVerifierMismatchCount =
        contractCache.manifestModelResourceVerifierMismatchCount();
  }
  summary.semanticVisibleDirectUnitCandidateAccepted =
      publishedBundle.stats.visibleDirectUnitCandidateAccepted;
  summary.semanticVisibleDirectUnitRejectedNotUnitLike =
      publishedBundle.stats.visibleDirectUnitRejectedNotUnitLike;
  summary.semanticVisibleDirectUnitRejectedGroup =
      publishedBundle.stats.visibleDirectUnitRejectedGroup;
  summary.semanticVisibleDirectUnitRejectedNoUnitPtr =
      publishedBundle.stats.visibleDirectUnitRejectedNoUnitPtr;
  summary.semanticVisibleDirectUnitRejectedNoIdentity =
      publishedBundle.stats.visibleDirectUnitRejectedNoIdentity;
  summary.semanticVisibleDirectUnitRejectedNoMesh =
      publishedBundle.stats.visibleDirectUnitRejectedNoMesh;
  summary.semanticVisibleDirectUnitRejectedBuilding =
      publishedBundle.stats.visibleDirectUnitRejectedBuilding;
  summary.semanticVisibleDirectUnitRejectedNoGeoset =
      publishedBundle.stats.visibleDirectUnitRejectedNoGeoset;
  if (publishedManifest != nullptr) {
    summary.semanticCoreManifestFrameSerial = publishedManifest->frameSerial;
    summary.semanticCoreManifestPublishRevision =
        publishedManifest->publishRevision;
  }
  summary.semanticCoreFrameSerial = semanticCore.frameSerial;
  summary.semanticCoreSourcePublishRevision =
      semanticCore.sourcePublishRevision;
  if (summary.semanticCoreSourcePublishRevision >
      summary.semanticSceneLastSourcePublishRevision) {
    summary.semanticScenePublishRevisionLag =
        summary.semanticCoreSourcePublishRevision -
        summary.semanticSceneLastSourcePublishRevision;
  }
  summary.semanticCoreSourceVisibleCount = semanticCore.sourceVisibleCount;
  summary.semanticCoreSourceStableIdentityCount =
      semanticCore.sourceStableIdentityCount;
  summary.semanticCoreSourceResolvedGeosetCount =
      semanticCore.sourceResolvedGeosetCount;
  summary.semanticCoreSourceUnitCount = semanticCore.sourceUnitCount;
  if (summary.semanticCoreManifestFrameSerial >= summary.semanticCoreFrameSerial) {
    summary.semanticCoreFrameLag =
        summary.semanticCoreManifestFrameSerial - summary.semanticCoreFrameSerial;
  }
  if (summary.semanticCoreManifestPublishRevision >=
      summary.semanticCoreSourcePublishRevision) {
    summary.semanticCorePublishRevisionLag =
        summary.semanticCoreManifestPublishRevision -
        summary.semanticCoreSourcePublishRevision;
  }
  const bool publishLagFreshEnough =
      summary.semanticCorePublishRevisionLag <= 16u;
  summary.semanticCoreFrameFresh =
      summary.semanticCoreFrameSerial != 0u &&
      summary.semanticCoreSourcePublishRevision != 0u &&
      !buildState.buildInProgress &&
      summary.semanticCoreFrameLag <= 1u &&
      publishLagFreshEnough;
  summary.semanticCoreConsidered = semanticCore.resolve.considered;
  summary.semanticCoreResolved = semanticCore.resolve.resolved;
  summary.semanticCoreRigidResolved = semanticCore.resolve.rigidResolved;
  summary.semanticCoreSkinnedResolved = semanticCore.resolve.skinnedResolved;
  summary.semanticCoreSlowestRecordResolveUs =
      semanticCore.resolve.slowestRecordResolveUs;
  summary.semanticCoreSlowestRecordIndex =
      semanticCore.resolve.slowestRecordIndex;
  summary.semanticCoreSlowestRecordRuntimeModelPtr =
      semanticCore.resolve.slowestRecordRuntimeModelPtr;
  summary.semanticCoreSlowestRecordModelResourcePtr =
      semanticCore.resolve.slowestRecordModelResourcePtr;
  summary.semanticCoreSlowestRecordRuntimeGeosetPtr =
      semanticCore.resolve.slowestRecordRuntimeGeosetPtr;
  summary.semanticCoreSlowestRecordRuntimeGeosetDataPtr =
      semanticCore.resolve.slowestRecordRuntimeGeosetDataPtr;
  summary.semanticCoreSlowestRecordGeosetIndex =
      semanticCore.resolve.slowestRecordGeosetIndex;
  summary.semanticCoreSlowestRecordObjectKind =
      semanticCore.resolve.slowestRecordObjectKind;
  summary.semanticCoreSlowestResourceLookupUs =
      semanticCore.resolve.slowestResourceLookupUs;
  summary.semanticCoreSlowestPoseResolveUs =
      semanticCore.resolve.slowestPoseResolveUs;
  summary.semanticCoreSlowestPoseDirectLookupUs =
      semanticCore.resolve.slowestPoseDirectLookupUs;
  summary.semanticCoreSlowestPoseOwnerLookupUs =
      semanticCore.resolve.slowestPoseOwnerLookupUs;
  summary.semanticCoreSlowestPoseSpriteLookupUs =
      semanticCore.resolve.slowestPoseSpriteLookupUs;
  summary.semanticCoreSlowestPoseInstanceRegistryUs =
      semanticCore.resolve.slowestPoseInstanceRegistryUs;
  summary.semanticCoreSlowestPoseShadowRegistryUs =
      semanticCore.resolve.slowestPoseShadowRegistryUs;
  summary.semanticCoreSlowestPoseRenderRegistryUs =
      semanticCore.resolve.slowestPoseRenderRegistryUs;
  summary.semanticCoreSlowestPoseRuntimeRootsUs =
      semanticCore.resolve.slowestPoseRuntimeRootsUs;
  summary.semanticCoreSlowestPoseMeshPoseContextUs =
      semanticCore.resolve.slowestPoseMeshPoseContextUs;
  summary.semanticCoreSlowestPoseMissDiagnosticUs =
      semanticCore.resolve.slowestPoseMissDiagnosticUs;
  summary.semanticCoreSlowestLayerContractUs =
      semanticCore.resolve.slowestLayerContractUs;
  summary.semanticCoreSlowestRuntimeGroupPaletteUs =
      semanticCore.resolve.slowestRuntimeGroupPaletteUs;
  summary.semanticCoreSlowestRuntimeGroupPaletteRescueUs =
      semanticCore.resolve.slowestRuntimeGroupPaletteRescueUs;
  summary.semanticCoreSlowestAttachmentRigidResolveUs =
      semanticCore.resolve.slowestAttachmentRigidResolveUs;
  summary.semanticCoreRigidCandidateCount =
      semanticCore.resolve.rigidCandidateCount;
  summary.semanticCoreSkinnedCandidateCount =
      semanticCore.resolve.skinnedCandidateCount;
  summary.semanticCoreSkinnedCandidatePoseReadyCount =
      semanticCore.resolve.skinnedCandidatePoseReadyCount;
  summary.semanticCoreSkinnedCandidateRuntimeGroupPaletteReadyCount =
      semanticCore.resolve.skinnedCandidateRuntimeGroupPaletteReadyCount;
  summary.semanticCoreSkinnedCandidateResolvedAsAttachmentRigidCount =
      semanticCore.resolve.skinnedCandidateResolvedAsAttachmentRigidCount;
  summary.semanticCoreRuntimeGroupPaletteMissNoSkinningData =
      semanticCore.resolve.runtimeGroupPaletteMissNoSkinningData;
  summary.semanticCoreRuntimeGroupPaletteMissNoPosePalette =
      semanticCore.resolve.runtimeGroupPaletteMissNoPosePalette;
  summary.semanticCoreRuntimeGroupPaletteMissNoVertexGroups =
      semanticCore.resolve.runtimeGroupPaletteMissNoVertexGroups;
  summary.semanticCoreRuntimeGroupPaletteMissInvalidGroupTable =
      semanticCore.resolve.runtimeGroupPaletteMissInvalidGroupTable;
  summary.semanticCoreRuntimeGroupPaletteMissMatrixIndexOutOfRange =
      semanticCore.resolve.runtimeGroupPaletteMissMatrixIndexOutOfRange;
  summary.semanticCoreRuntimeGroupPaletteMissVertexGroupOutOfRange =
      semanticCore.resolve.runtimeGroupPaletteMissVertexGroupOutOfRange;
  summary.semanticCoreRuntimeGroupPaletteMissFallbacksFailed =
      semanticCore.resolve.runtimeGroupPaletteMissFallbacksFailed;
  summary.semanticCoreRuntimeGroupPaletteMissLastPoseCount =
      semanticCore.resolve.runtimeGroupPaletteMissLastPoseCount;
  summary.semanticCoreRuntimeGroupPaletteMissLastGroupCount =
      semanticCore.resolve.runtimeGroupPaletteMissLastGroupCount;
  summary.semanticCoreRuntimeGroupPaletteMissLastMaxVertexGroupSlot =
      semanticCore.resolve.runtimeGroupPaletteMissLastMaxVertexGroupSlot;
  summary.semanticCoreRuntimeGroupPaletteMissLastMatrixIndexCount =
      semanticCore.resolve.runtimeGroupPaletteMissLastMatrixIndexCount;
  summary.semanticCoreRuntimeGroupPaletteMissLastMatrixIndex =
      semanticCore.resolve.runtimeGroupPaletteMissLastMatrixIndex;
  summary.semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext =
      semanticCore.resolve.runtimeGroupPaletteRescueByMeshPoseContext;
  summary.semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose =
      semanticCore.resolve.runtimeGroupPaletteRescueByResourceMatchedPose;
  summary.semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot =
      semanticCore.resolve.runtimeGroupPaletteRescueByRuntimeRoot;
  summary.semanticCoreRuntimeGroupPaletteRescueByChildRuntime =
      semanticCore.resolve.runtimeGroupPaletteRescueByChildRuntime;
  summary.semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime =
      semanticCore.resolve.runtimeGroupPaletteRescueByDescendantRuntime;
  summary.semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed =
      semanticCore.resolve.runtimeGroupPaletteResourceMatchedPoseSuppressed;
  summary.semanticCoreExplicitResourceOwnerRigidResolved =
      semanticCore.resolve.explicitResourceOwnerRigidResolved;
  summary.semanticCoreExplicitResourceOwnerRigidWorldTransformResolved =
      semanticCore.resolve.explicitResourceOwnerRigidWorldTransformResolved;
  summary.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette =
      semanticCore.resolve.explicitResourceOwnerRigidNoMatrixPalette;
  summary.semanticCoreAttachmentRigidMatchByChildRuntimeModel =
      semanticCore.resolve.attachmentRigidMatchByChildRuntimeModel;
  summary.semanticCoreAttachmentRigidMatchByChildSprite =
      semanticCore.resolve.attachmentRigidMatchByChildSprite;
  summary.semanticCoreAttachmentRigidMatchByChildRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByChildRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByChildSpriteRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByChildSpriteRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByOwnerRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByOwnerRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByRootRuntimeGeoset =
      semanticCore.resolve.attachmentRigidMatchByRootRuntimeGeoset;
  summary.semanticCoreAttachmentRigidMatchByResourceRuntimeOwner =
      semanticCore.resolve.attachmentRigidMatchByResourceRuntimeOwner;
  summary.semanticCoreAttachmentRigidMatchByRenderableRuntimeRoot =
      semanticCore.resolve.attachmentRigidMatchByRenderableRuntimeRoot;
  summary.semanticCoreAttachmentRigidMatchByWorldObjectEntry =
      semanticCore.resolve.attachmentRigidMatchByWorldObjectEntry;
  summary.semanticCoreAttachmentRigidMatchBySceneNode =
      semanticCore.resolve.attachmentRigidMatchBySceneNode;
  summary.semanticCoreAttachmentRigidMatchByUnitPtr =
      semanticCore.resolve.attachmentRigidMatchByUnitPtr;
  summary.semanticCoreAttachmentRigidMatchByHandle =
      semanticCore.resolve.attachmentRigidMatchByHandle;
  summary.semanticCoreAttachmentRigidMatchByChildModelResource =
      semanticCore.resolve.attachmentRigidMatchByChildModelResource;
  summary.semanticCoreAttachmentRigidMatchByUniqueIdentity =
      semanticCore.resolve.attachmentRigidMatchByUniqueIdentity;
  summary.semanticCoreAttachmentRigidMatchMiss =
      semanticCore.resolve.attachmentRigidMatchMiss;
  summary.lastAttachmentRigidMatchMissRuntimeModelPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissRuntimeModelPtr;
  summary.lastAttachmentRigidMatchMissModelResourcePtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissModelResourcePtr;
  summary.lastAttachmentRigidMatchMissRuntimeGeosetPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissRuntimeGeosetPtr;
  summary.lastAttachmentRigidMatchMissRuntimeGeosetDataPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissRuntimeGeosetDataPtr;
  summary.lastAttachmentRigidMatchMissGeosetIndex =
      semanticCore.resolve.lastAttachmentRigidMatchMissGeosetIndex;
  summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr =
      semanticCore.resolve.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr;
  if (summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr != 0u) {
    const auto missRuntimeOwnerPtr = reinterpret_cast<void*>(
        static_cast<uintptr_t>(
            summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPtr));
    model::ModelInstanceRecord runtimeOwnerRecord = {};
    if (model::ModelInstanceRegistry::instance().findByRuntimeModel(
            missRuntimeOwnerPtr, runtimeOwnerRecord) ||
        model::ModelInstanceRegistry::instance().findOwnerByRuntimeModel(
            missRuntimeOwnerPtr, runtimeOwnerRecord)) {
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerWorldObjectEntryPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.worldObjectEntry);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSceneNodePtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.sceneNode);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerUnitPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.unitPtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.spritePtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceObjectPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.sourceObjectPtr);
      summary
          .lastAttachmentRigidMatchMissResourceRuntimeOwnerSourceSpriteObjectPtr =
          reinterpret_cast<uint64_t>(runtimeOwnerRecord.sourceSpriteObjectPtr);
      summary
          .lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateModelDataPtr =
          reinterpret_cast<uint64_t>(
              runtimeOwnerRecord.runtimeCreatorModelDataPtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateHandlePtr =
          reinterpret_cast<uint64_t>(
              runtimeOwnerRecord.runtimeCreatorHandlePtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerCreateCallerRva =
          runtimeOwnerRecord.runtimeCreatorCallerRva;
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerResolveCallerRva =
          runtimeOwnerRecord.runtimeResolveCallerRva;
    }

    if (summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr != 0u) {
      const auto contractAttachments =
          shadow::ShadowRuntimeContractCache::instance().snapshotAttachmentsShared();
      if (contractAttachments != nullptr) {
        shadow::ShadowAttachmentRigidRecord contractAttachment = {};
        const auto missOwnerSpritePtr = reinterpret_cast<void*>(
            static_cast<uintptr_t>(
                summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerSpritePtr));
        if (contractAttachments->findByChildSpritePtr(missOwnerSpritePtr,
                                                      contractAttachment)) {
          summary.lastAttachmentRigidMatchMissOwnerSpriteContractHit = 1u;
          summary
              .lastAttachmentRigidMatchMissOwnerSpriteContractChildRuntimeModelPtr =
              reinterpret_cast<uint64_t>(contractAttachment.childRuntimeModelPtr);
          summary
              .lastAttachmentRigidMatchMissOwnerSpriteContractOwnerRuntimeModelPtr =
              reinterpret_cast<uint64_t>(contractAttachment.ownerRuntimeModelPtr);
        }
      }
    }

    model::ShadowModelResourceRecord runtimeOwnerResource = {};
    if (model::ShadowModelResourceCache::instance().findRuntimeModelResource(
            missRuntimeOwnerPtr, runtimeOwnerResource)) {
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerModelResourcePtr =
          reinterpret_cast<uint64_t>(runtimeOwnerResource.modelResourcePtr);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerModelKey =
          runtimeOwnerResource.modelKey;
    }

    model::PoseRecord runtimeOwnerPose = {};
    if (model::PoseRegistry::instance().findByRuntimeModel(missRuntimeOwnerPtr,
                                                           runtimeOwnerPose)) {
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPoseMatrixCount =
          runtimeOwnerPose.matrixCount;
    }

    const uintptr_t missRuntimeOwnerValue =
        reinterpret_cast<uintptr_t>(missRuntimeOwnerPtr);
    constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
    if (missRuntimeOwnerValue >= 0x10000u) {
      void* plusA0Ptr = reinterpret_cast<void*>(
          missRuntimeOwnerValue + kCModelComplexExtensionOffset);
      summary.lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0Ptr =
          reinterpret_cast<uint64_t>(plusA0Ptr);
      model::ModelInstanceRecord plusA0Record = {};
      if (model::ModelInstanceRegistry::instance().findByRuntimeModel(
              plusA0Ptr, plusA0Record) ||
          model::ModelInstanceRegistry::instance().findOwnerByRuntimeModel(
              plusA0Ptr, plusA0Record)) {
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceObjectPtr =
            reinterpret_cast<uint64_t>(plusA0Record.sourceObjectPtr);
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0SourceSpriteObjectPtr =
            reinterpret_cast<uint64_t>(plusA0Record.sourceSpriteObjectPtr);
      }
      model::PoseRecord plusA0Pose = {};
      if (model::PoseRegistry::instance().findByRuntimeModel(plusA0Ptr,
                                                             plusA0Pose)) {
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0PoseMatrixCount =
            plusA0Pose.matrixCount;
        summary
            .lastAttachmentRigidMatchMissResourceRuntimeOwnerPlusA0HasWorldTransform =
            (plusA0Pose.hasWorldTransform ||
             plusA0Pose.hasSpriteFrameTransform)
                ? 1u
                : 0u;
      }
    }
  }
  summary.semanticCoreAttachmentRigidPoseMissNoRecord =
      semanticCore.resolve.attachmentRigidPoseMissNoRecord;
  summary.semanticCoreAttachmentRigidPoseMissMissingRuntimes =
      semanticCore.resolve.attachmentRigidPoseMissMissingRuntimes;
  summary.semanticCoreAttachmentRigidPoseMissNoRootPose =
      semanticCore.resolve.attachmentRigidPoseMissNoRootPose;
  summary.semanticCoreAttachmentRigidPoseMissNoRootWorldTransform =
      semanticCore.resolve.attachmentRigidPoseMissNoRootWorldTransform;
  summary.semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromLivePose =
      semanticCore.resolve
          .attachmentRigidPoseRecoveredWorldTransformFromLivePose;
  summary
      .semanticCoreAttachmentRigidPoseRecoveredWorldTransformFromShadowRegistry =
      semanticCore.resolve
          .attachmentRigidPoseRecoveredWorldTransformFromShadowRegistry;
  summary.semanticCoreExplicitBlendAttempts =
      semanticCore.resolve.explicitBlendAttempts;
  summary.semanticCoreExplicitBlendAttemptWithSpanRemapTable =
      semanticCore.resolve.explicitBlendAttemptWithSpanRemapTable;
  summary.semanticCoreExplicitBlendResolved =
      semanticCore.resolve.explicitBlendResolved;
  summary.semanticCoreExplicitBlendSpanRemapResolved =
      semanticCore.resolve.explicitBlendSpanRemapResolved;
  summary.semanticCoreExplicitBlendStrideSearchMiss =
      semanticCore.resolve.explicitBlendStrideSearchMiss;
  summary.semanticCoreExplicitBlendFinalDecodeMiss =
      semanticCore.resolve.explicitBlendFinalDecodeMiss;
  summary.semanticCoreCoreDrawPacketCount = semanticCore.coreDrawPacketCount;
  summary.semanticCoreUpperLayerResolvedItems =
      semanticCore.upperLayerResolvedItems;
  summary.semanticCoreSupplementalUpperLayerDrawPacketCount =
      semanticCore.supplementalUpperLayerDrawPacketCount;
  summary.semanticCoreDrawPacketCount = semanticCore.drawPacketCount;
  summary.semanticCoreSubmittedDrawCount = semanticCore.submittedDrawCount;
  {
    const auto lastFrame =
        shadow::ShadowValidationRuntime::instance().snapshotFrameShared();
    if (lastFrame != nullptr) {
      summary.semanticCoreLastFrameSourcePublishRevision =
          lastFrame->sourcePublishRevision;
      summary.semanticCoreLastFrameDrawCount = lastFrame->draws.size();
      summary.semanticCoreLastFrameSkinnedDrawCount =
          CountSkinnedSubmissionDraws(lastFrame.get());
      if (summary.semanticCoreSubmittedDrawCount == 0u &&
          summary.semanticSceneSubmittedSkinned != 0u) {
        // DXVK scene submission can reuse the last completed semantic frame and
        // refresh CModel palettes at submit time. In that mode the validation
        // runtime's submitted counter may be zero even though the scene path
        // emitted semantic skinned draws this frame, so expose the effective
        // reusable packet count rather than making the summary look idle.
        summary.semanticCoreSubmittedDrawCount =
            static_cast<uint64_t>(lastFrame->draws.size());
      }
    }

    const auto renderableFrame = shadow::ShadowValidationRuntime::instance()
                                     .snapshotRenderableFrameShared();
    if (renderableFrame != nullptr) {
      summary.semanticCoreRenderableFrameSourcePublishRevision =
          renderableFrame->sourcePublishRevision;
      summary.semanticCoreRenderableFrameDrawCount =
          renderableFrame->draws.size();
      summary.semanticCoreRenderableFrameSkinnedDrawCount =
          CountSkinnedSubmissionDraws(renderableFrame.get());
    }
  }
  summary.semanticCoreSkippedNoIdentity =
      semanticCore.resolve.skippedNoIdentity;
  summary.semanticCoreSkippedNoResolvedGeoset =
      semanticCore.resolve.skippedNoResolvedGeoset;
  summary.semanticCoreSkippedNoGeoset =
      semanticCore.resolve.skippedNoGeoset;
  summary.semanticCoreSkippedResourceMiss =
      semanticCore.resolve.skippedResourceMiss;
  summary.semanticCoreSkippedResourceNotReady =
      semanticCore.resolve.skippedResourceNotReady;
  summary.semanticCoreSkippedNoPose =
      semanticCore.resolve.skippedNoPose;
  summary.semanticCoreSkippedNoPoseNoContext =
      semanticCore.resolve.skippedNoPoseNoContext;
  summary.semanticCoreSkippedNoPoseAnonymousSubpart =
      semanticCore.resolve.skippedNoPoseAnonymousSubpart;
  summary.semanticCoreSkippedNoPoseLookupMiss =
      semanticCore.resolve.skippedNoPoseLookupMiss;
  summary.semanticCoreSkippedNoRuntimeGroupPalette =
      semanticCore.resolve.skippedNoRuntimeGroupPalette;
  summary.semanticCoreAttachmentRigidResolved =
      semanticCore.resolve.attachmentRigidResolved;
  summary.semanticCoreAttachmentRigidSupplementalAttachmentCount =
      semanticCore.resolve.attachmentRigidSupplementalAttachmentCount;
  summary.semanticCoreAttachmentRigidSupplementalResourceCandidateCount =
      semanticCore.resolve.attachmentRigidSupplementalResourceCandidateCount;
  summary.semanticCoreAttachmentRigidSupplementalResolvedCount =
      semanticCore.resolve.attachmentRigidSupplementalResolvedCount;
  summary.semanticCoreAttachmentRigidSupplementalResourceMissCount =
      semanticCore.resolve.attachmentRigidSupplementalResourceMissCount;
  summary.semanticCoreBuildDurationUs = semanticCore.buildDurationUs;
  summary.semanticCoreBuildInProgress = buildState.buildInProgress;
  summary.semanticCoreBuildRequestPending = buildState.buildRequestPending;
  summary.semanticCoreBuildFrameSerial = buildState.buildFrameSerial;
  summary.semanticCoreBuildPublishRevision = buildState.buildPublishRevision;
  summary.semanticCorePendingFrameSerial = buildState.pendingFrameSerial;
  summary.semanticCorePendingPublishRevision =
      buildState.pendingPublishRevision;
  summary.semanticCoreBuildCurrentRecordIndex =
      buildState.buildCurrentRecordIndex;
  summary.semanticCoreBuildRecordCount = buildState.buildRecordCount;
  summary.semanticCoreBuildChunkCount = buildState.buildChunkCount;
  summary.semanticCoreStalePendingBuildClearedCount =
      buildState.stalePendingBuildClearedCount;
  auto semanticPerfCalls = [](SemanticDataPerfTag tag) {
    return g_semanticPerfCalls[static_cast<size_t>(tag)].load(
        std::memory_order_relaxed);
  };
  auto semanticPerfUs = [](SemanticDataPerfTag tag) {
    return g_semanticPerfUs[static_cast<size_t>(tag)].load(
        std::memory_order_relaxed);
  };
  summary.semanticModelHookCalls =
      overrideSummary.runtimeModelCtorCount +
      overrideSummary.runtimeModelCreateCount +
      overrideSummary.runtimeModelResolveCount +
      overrideSummary.runtimeModelInitCopyCount +
      overrideSummary.runtimeChildLinkBuildCount +
      overrideSummary.spriteHostBindCount +
      overrideSummary.runtimeSourceObjectPublishCount;
  summary.semanticModelHookUs =
      semanticPerfUs(SemanticDataPerfTag::ModelHook);
  summary.semanticPoseHookCalls =
      overrideSummary.runtimeMatrixRangeCopyCount +
      overrideSummary.runtimeMatrixFlushCount +
      overrideSummary.primaryPresetWriteCount +
      overrideSummary.sharedPresetWriteCount +
      overrideSummary.localPointWriteCount +
      overrideSummary.spriteFramePoseBaseAliasPublishCount +
      overrideSummary.spriteFramePoseBaseAliasMatrixPaletteCount;
  summary.semanticPoseHookUs =
      semanticPerfUs(SemanticDataPerfTag::PoseHook);
  summary.semanticAttachmentHookCalls =
      overrideSummary.attachedEffectInitBindCount +
      overrideSummary.attachedEffectDirectBindCount +
      overrideSummary.attachModelToPointBindCount +
      overrideSummary.attachmentRigidPublishedWithSourceObjectCount +
      overrideSummary.spriteFrameAttachmentRootRuntimeHitCount +
      overrideSummary.spriteFrameAttachmentOwnerRuntimeHitCount +
      overrideSummary.spriteFrameAttachmentChildRuntimeHitCount;
  summary.semanticAttachmentHookUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentHook);
  summary.semanticFrameRegistryPublishCalls =
      semanticPerfCalls(SemanticDataPerfTag::FrameRegistryPublish);
  summary.semanticFrameRegistryPublishUs =
      semanticPerfUs(SemanticDataPerfTag::FrameRegistryPublish);
  summary.semanticContractCaptureCalls =
      semanticPerfCalls(SemanticDataPerfTag::ContractCapture);
  summary.semanticContractCaptureUs =
      semanticPerfUs(SemanticDataPerfTag::ContractCapture);
  summary.semanticConsumerBuildCalls =
      semanticPerfCalls(SemanticDataPerfTag::ConsumerBuild);
  summary.semanticConsumerBuildUs =
      semanticPerfUs(SemanticDataPerfTag::ConsumerBuild);
  summary.semanticConsumerBuildSkippedFresh =
      g_semanticConsumerBuildSkippedFresh.load(std::memory_order_relaxed);
  summary.semanticSummaryRefreshRequestCalls =
      semanticPerfCalls(SemanticDataPerfTag::SummaryRefreshRequest);
  summary.semanticSummaryRefreshRequestUs =
      semanticPerfUs(SemanticDataPerfTag::SummaryRefreshRequest);
  summary.shadowMetadataCaptureFrameCalls =
      semanticPerfCalls(SemanticDataPerfTag::ShadowMetadataCapture);
  summary.shadowMetadataCaptureUs =
      semanticPerfUs(SemanticDataPerfTag::ShadowMetadataCapture);
  summary.semanticLastHotFunctionTag =
      g_semanticLastHotFunctionTag.load(std::memory_order_relaxed);
  summary.semanticLastHotFunctionUs =
      g_semanticLastHotFunctionUs.load(std::memory_order_relaxed);
  summary.semanticModelBuildChildPreScanCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelBuildChildPreScan);
  summary.semanticModelBuildChildPreScanUs =
      semanticPerfUs(SemanticDataPerfTag::ModelBuildChildPreScan);
  summary.semanticModelRuntimeChildCollectCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildCollect);
  summary.semanticModelRuntimeChildCollectUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildCollect);
  summary.semanticModelRuntimeChildBootstrapCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildBootstrap);
  summary.semanticModelRuntimeChildBootstrapUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildBootstrap);
  summary.semanticModelRuntimeChildParentMapCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildParentMap);
  summary.semanticModelRuntimeChildParentMapUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildParentMap);
  summary.semanticModelRuntimeChildOwnerPropagateCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeChildOwnerPropagate);
  summary.semanticModelRuntimeChildOwnerPropagateUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeChildOwnerPropagate);
  summary.semanticModelPromoteRuntimeCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelPromoteRuntime);
  summary.semanticModelPromoteRuntimeUs =
      semanticPerfUs(SemanticDataPerfTag::ModelPromoteRuntime);
  summary.semanticModelSpriteHostBindCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelSpriteHostBind);
  summary.semanticModelSpriteHostBindUs =
      semanticPerfUs(SemanticDataPerfTag::ModelSpriteHostBind);
  summary.semanticModelRuntimeModelBindingCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeModelBinding);
  summary.semanticModelRuntimeModelBindingUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeModelBinding);
  summary.semanticModelGeosetResourceCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelGeosetResource);
  summary.semanticModelGeosetResourceUs =
      semanticPerfUs(SemanticDataPerfTag::ModelGeosetResource);
  summary.semanticModelRuntimeCtorCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeCtor);
  summary.semanticModelRuntimeCtorUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeCtor);
  summary.semanticModelRuntimeResolveCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeResolve);
  summary.semanticModelRuntimeResolveUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeResolve);
  summary.semanticModelRuntimeInitCopyCalls =
      semanticPerfCalls(SemanticDataPerfTag::ModelRuntimeInitCopy);
  summary.semanticModelRuntimeInitCopyUs =
      semanticPerfUs(SemanticDataPerfTag::ModelRuntimeInitCopy);
  summary.semanticPoseRuntimePoseCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimePose);
  summary.semanticPoseRuntimePoseUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimePose);
  summary.semanticPoseRuntimePaletteTreeCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimePaletteTree);
  summary.semanticPoseRuntimePaletteTreeUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimePaletteTree);
  summary.semanticPoseRuntimeMatrixPaletteCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimeMatrixPalette);
  summary.semanticPoseRuntimeMatrixPaletteUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimeMatrixPalette);
  summary.semanticPoseSpriteFrameSourceIdentityCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteFrameSourceIdentity);
  summary.semanticPoseSpriteFrameSourceIdentityUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteFrameSourceIdentity);
  summary.semanticPoseSpriteFramePoseCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteFramePose);
  summary.semanticPoseSpriteFramePoseUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteFramePose);
  summary.semanticPoseRuntimeMatrixPublisherCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseRuntimeMatrixPublisher);
  summary.semanticPoseRuntimeMatrixPublisherUs =
      semanticPerfUs(SemanticDataPerfTag::PoseRuntimeMatrixPublisher);
  summary.semanticPoseSpriteAttachmentHitCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteAttachmentHit);
  summary.semanticPoseSpriteAttachmentHitUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteAttachmentHit);
  summary.semanticPoseSpriteTransformReadCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteTransformRead);
  summary.semanticPoseSpriteTransformReadUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteTransformRead);
  summary.semanticPoseSpriteIdentityLookupCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteIdentityLookup);
  summary.semanticPoseSpriteIdentityLookupUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteIdentityLookup);
  summary.semanticPoseSpriteBaseAliasCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpriteBaseAlias);
  summary.semanticPoseSpriteBaseAliasUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpriteBaseAlias);
  summary.semanticPoseSpritePublishPoseCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpritePublishPose);
  summary.semanticPoseSpritePublishPoseUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpritePublishPose);
  summary.semanticPoseSpritePaletteGateCalls =
      semanticPerfCalls(SemanticDataPerfTag::PoseSpritePaletteGate);
  summary.semanticPoseSpritePaletteGateUs =
      semanticPerfUs(SemanticDataPerfTag::PoseSpritePaletteGate);
  summary.semanticAttachmentAttachedEffectInitCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentAttachedEffectInit);
  summary.semanticAttachmentAttachedEffectInitUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentAttachedEffectInit);
  summary.semanticAttachmentAttachedEffectDirectCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentAttachedEffectDirect);
  summary.semanticAttachmentAttachedEffectDirectUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentAttachedEffectDirect);
  summary.semanticAttachmentAttachModelToPointCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentAttachModelToPoint);
  summary.semanticAttachmentAttachModelToPointUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentAttachModelToPoint);
  summary.semanticAttachmentOverrideSharedPresetCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentOverrideSharedPreset);
  summary.semanticAttachmentOverrideSharedPresetUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentOverrideSharedPreset);
  summary.semanticAttachmentOverrideLocalPointCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentOverrideLocalPoint);
  summary.semanticAttachmentOverrideLocalPointUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentOverrideLocalPoint);
  summary.semanticAttachmentOverridePrimaryPresetCalls =
      semanticPerfCalls(SemanticDataPerfTag::AttachmentOverridePrimaryPreset);
  summary.semanticAttachmentOverridePrimaryPresetUs =
      semanticPerfUs(SemanticDataPerfTag::AttachmentOverridePrimaryPreset);
  const auto nativeSummary =
      shadow::NativeD3D9BackendRuntime::instance().snapshot();
  summary.nativeD3D9BackendFrameSerial = nativeSummary.frameSerial;
  summary.nativeD3D9BackendSourcePublishRevision =
      nativeSummary.sourcePublishRevision;
  summary.nativeD3D9BackendSubmittedDrawCount =
      nativeSummary.submittedDrawCount;
  summary.nativeD3D9BackendSubmittedRigidDrawCount =
      nativeSummary.submittedRigidDrawCount;
  summary.nativeD3D9BackendSubmittedSkinnedDrawCount =
      nativeSummary.submittedSkinnedDrawCount;
  summary.nativeD3D9BackendExecutedFrameSerial =
      nativeSummary.executedFrameSerial;
  summary.nativeD3D9BackendExecutedDrawCount =
      nativeSummary.executedDrawCount;
  summary.nativeD3D9BackendExecutedRigidDrawCount =
      nativeSummary.executedRigidDrawCount;
  summary.nativeD3D9BackendExecutedSkinnedDrawCount =
      nativeSummary.executedSkinnedDrawCount;
  summary.nativeD3D9BackendExecuteAttemptCount =
      nativeSummary.executeAttemptCount;
  summary.nativeD3D9BackendExecuteSuccessCount =
      nativeSummary.executeSuccessCount;
  summary.nativeD3D9BackendLastSuccessfulExecutedFrameSerial =
      nativeSummary.lastSuccessfulExecutedFrameSerial;
  summary.nativeD3D9BackendLastSuccessfulExecutedDrawCount =
      nativeSummary.lastSuccessfulExecutedDrawCount;
  summary.nativeD3D9BackendExecuteSkippedNoDeviceCount =
      nativeSummary.executeSkippedNoDeviceCount;
  summary.nativeD3D9BackendExecuteSkippedNoDrawsCount =
      nativeSummary.executeSkippedNoDrawsCount;
  summary.nativeD3D9BackendLastExecuteSubmittedDrawCount =
      nativeSummary.lastExecuteSubmittedDrawCount;
  summary.nativeD3D9BackendLastExecuteFailedDrawCount =
      nativeSummary.lastExecuteFailedDrawCount;
  summary.nativeD3D9BackendLastExecuteSubmittedRigidDrawCount =
      nativeSummary.lastExecuteSubmittedRigidDrawCount;
  summary.nativeD3D9BackendLastExecuteSubmittedSkinnedDrawCount =
      nativeSummary.lastExecuteSubmittedSkinnedDrawCount;
  summary.nativeD3D9BackendLastExecuteExecutedRigidDrawCount =
      nativeSummary.lastExecuteExecutedRigidDrawCount;
  summary.nativeD3D9BackendLastExecuteExecutedSkinnedDrawCount =
      nativeSummary.lastExecuteExecutedSkinnedDrawCount;
    summary.nativeD3D9BackendGeometryCount = nativeSummary.geometryCount;
    summary.nativeD3D9BackendPaletteCount = nativeSummary.paletteCount;
    summary.nativeD3D9BackendMaterialCount = nativeSummary.materialCount;
    summary.nativeD3D9BackendCanonicalDrawCount =
        nativeSummary.canonicalDrawCount;
    summary.nativeD3D9BackendCanonicalFrameSerial =
        nativeSummary.canonicalFrameSerial;
    summary.nativeD3D9BackendCanonicalPublishCount =
        nativeSummary.canonicalPublishCount;
    summary.nativeD3D9BackendCanonicalPublishRejectNotReadyCount =
        nativeSummary.canonicalPublishRejectNotReadyCount;
    summary.nativeD3D9BackendCanonicalPublishRejectNoPositionsCount =
        nativeSummary.canonicalPublishRejectNoPositionsCount;
    summary.nativeD3D9BackendGeometryRejectCount =
        nativeSummary.geometryRejectCount;
    summary.nativeD3D9BackendPaletteRejectCount =
        nativeSummary.paletteRejectCount;
    summary.nativeD3D9BackendMaterialRejectCount =
        nativeSummary.materialRejectCount;
    summary.nativeD3D9BackendSubmitRejectCount =
        nativeSummary.submitRejectCount;
    summary.nativeD3D9BackendUsedCanonicalFrame =
        nativeSummary.usedCanonicalFrame;
    summary.nativeD3D9BackendHasDevice = nativeSummary.hasDevice;
    summary.nativeSemanticWorldStageCandidateCount =
        g_nativeSemanticWorldStageCandidateCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageCandidatePrepareCount =
        g_nativeSemanticWorldStageCandidatePrepareCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageCandidateRefreshCount =
        g_nativeSemanticWorldStageCandidateRefreshCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageCandidateExecuteCount =
        g_nativeSemanticWorldStageCandidateExecuteCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageSkippedRuntimeNotReadyCount =
        g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateStage =
        g_nativeSemanticWorldStageLastCandidateStage.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateA3 =
        g_nativeSemanticWorldStageLastCandidateA3.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateA4 =
        g_nativeSemanticWorldStageLastCandidateA4.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateA5 =
        g_nativeSemanticWorldStageLastCandidateA5.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateJassReady =
        g_nativeSemanticWorldStageLastCandidateJassReady.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateGameStarted =
        g_nativeSemanticWorldStageLastCandidateGameStarted.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastCandidateRuntimeFrame =
        g_nativeSemanticWorldStageLastCandidateRuntimeFrame.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStagePrepareAttemptCount =
        g_nativeSemanticWorldStagePrepareAttemptCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStagePrepareSuccessCount =
        g_nativeSemanticWorldStagePrepareSuccessCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageExecuteAttemptCount =
        g_nativeSemanticWorldStageExecuteAttemptCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageExecuteSuccessCount =
        g_nativeSemanticWorldStageExecuteSuccessCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastPrepareStage =
        g_nativeSemanticWorldStageLastPrepareStage.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastExecuteStage =
        g_nativeSemanticWorldStageLastExecuteStage.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastPrepareFrameSerial =
        g_nativeSemanticWorldStageLastPrepareFrameSerial.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastExecuteFrameSerial =
        g_nativeSemanticWorldStageLastExecuteFrameSerial.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastPrepareDrawCount =
        g_nativeSemanticWorldStageLastPrepareDrawCount.load(
            std::memory_order_relaxed);
    summary.nativeSemanticWorldStageLastExecuteDrawCount =
        g_nativeSemanticWorldStageLastExecuteDrawCount.load(
            std::memory_order_relaxed);
    summary.runtimeChildLinkBuildCount =
        overrideSummary.runtimeChildLinkBuildCount;
  summary.runtimeChildLinkBuiltChildCount =
      overrideSummary.runtimeChildLinkBuiltChildCount;
  summary.runtimeChildBuildTimeDirectPublishCount =
      overrideSummary.runtimeChildBuildTimeDirectPublishCount;
  summary.runtimeChildBuildTimeDirectPublishWithResourceCount =
      overrideSummary.runtimeChildBuildTimeDirectPublishWithResourceCount;
  summary.runtimeChildBuildModelDataPreLinkCount =
      overrideSummary.runtimeChildBuildModelDataPreLinkCount;
  summary.runtimeChildBuildModelDataPostLinkCount =
      overrideSummary.runtimeChildBuildModelDataPostLinkCount;
  summary.runtimeChildBuildModelDataPreUnreadableLinkCount =
      overrideSummary.runtimeChildBuildModelDataPreUnreadableLinkCount;
  summary.runtimeChildBuildModelDataPostUnreadableLinkCount =
      overrideSummary.runtimeChildBuildModelDataPostUnreadableLinkCount;
  summary.runtimeMatrixRangeCopyCount =
      overrideSummary.runtimeMatrixRangeCopyCount;
  summary.runtimeMatrixFlushCount =
      overrideSummary.runtimeMatrixFlushCount;
  summary.runtimeMatrixPublisherPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherPaletteReadyCount;
  summary.runtimePoseUpdatePalettePublishCount =
      overrideSummary.runtimePoseUpdatePalettePublishCount;
  summary.runtimePoseUpdateLastRuntimeModelPtr =
      overrideSummary.runtimePoseUpdateLastRuntimeModelPtr;
  summary.runtimePoseUpdateLastMatrixCount =
      overrideSummary.runtimePoseUpdateLastMatrixCount;
  summary.runtimePoseUpdateLastMatrixHash =
      overrideSummary.runtimePoseUpdateLastMatrixHash;
  summary.runtimeMatrixWriteCount =
      overrideSummary.runtimeMatrixWriteCount;
  summary.runtimeMatrixWritePublishCount =
      overrideSummary.runtimeMatrixWritePublishCount;
  summary.runtimeMatrixWriteMissCount =
      overrideSummary.runtimeMatrixWriteMissCount;
  summary.runtimeGroupPaletteWrapperCallCount =
      overrideSummary.runtimeGroupPaletteWrapperCallCount;
  summary.runtimeGroupPaletteWrapperPartCount =
      overrideSummary.runtimeGroupPaletteWrapperPartCount;
  summary.runtimeGroupPaletteWrapperBindingCount =
      overrideSummary.runtimeGroupPaletteWrapperBindingCount;
  summary.runtimeSimpleGroupPaletteCallCount =
      overrideSummary.runtimeSimpleGroupPaletteCallCount;
  summary.runtimeSimpleGroupPaletteSlotCapturedCount =
      overrideSummary.runtimeSimpleGroupPaletteSlotCapturedCount;
  summary.runtimeSimpleGroupPaletteSlotUnreadableCount =
      overrideSummary.runtimeSimpleGroupPaletteSlotUnreadableCount;
  summary.renderablePartPaletteBindingQueryHitCount =
      overrideSummary.renderablePartPaletteBindingQueryHitCount;
  summary.renderablePartPaletteBindingQueryMissCount =
      overrideSummary.renderablePartPaletteBindingQueryMissCount;
  summary.renderablePartPaletteSnapshotCapturedCount =
      overrideSummary.renderablePartPaletteSnapshotCapturedCount;
  summary.renderablePartPaletteSnapshotTooLargeCount =
      overrideSummary.renderablePartPaletteSnapshotTooLargeCount;
  summary.renderablePartPaletteSnapshotUnreadableCount =
      overrideSummary.renderablePartPaletteSnapshotUnreadableCount;
  summary.renderablePartPaletteSnapshotQueryHitCount =
      overrideSummary.renderablePartPaletteSnapshotQueryHitCount;
  summary.renderablePartPaletteSnapshotQueryMissCount =
      overrideSummary.renderablePartPaletteSnapshotQueryMissCount;
  // Phase 7.47 dt gate probe
  summary.spriteUberPreRenderTotalCount =
      overrideSummary.spriteUberPreRenderTotalCount;
  summary.spriteUberPreRenderDtZeroCount =
      overrideSummary.spriteUberPreRenderDtZeroCount;
  summary.spriteUberPreRenderDtBelowEpsilonCount =
      overrideSummary.spriteUberPreRenderDtBelowEpsilonCount;
  summary.spriteUberPreRenderDtPositiveCount =
      overrideSummary.spriteUberPreRenderDtPositiveCount;
  summary.spriteUberPreRenderDtNegativeCount =
      overrideSummary.spriteUberPreRenderDtNegativeCount;
  summary.spriteUberPreRenderLastDtBits =
      overrideSummary.spriteUberPreRenderLastDtBits;
  summary.spriteUberPreRenderLastZeroDtFrameTag =
      overrideSummary.spriteUberPreRenderLastZeroDtFrameTag;
  summary.spriteUberPreRenderLastPositiveDtFrameTag =
      overrideSummary.spriteUberPreRenderLastPositiveDtFrameTag;
  summary.runtimeMatrixWriteFramesWithHitCount =
      overrideSummary.runtimeMatrixWriteFramesWithHitCount;
  summary.runtimeMatrixWriteFramesEmptyCount =
      overrideSummary.runtimeMatrixWriteFramesEmptyCount;
  summary.runtimeGroupPaletteWrapperFramesWithHitCount =
      overrideSummary.runtimeGroupPaletteWrapperFramesWithHitCount;
  summary.runtimeGroupPaletteWrapperFramesEmptyCount =
      overrideSummary.runtimeGroupPaletteWrapperFramesEmptyCount;
  summary.runtimeSimpleGroupPaletteFramesWithHitCount =
      overrideSummary.runtimeSimpleGroupPaletteFramesWithHitCount;
  summary.runtimeSimpleGroupPaletteFramesEmptyCount =
      overrideSummary.runtimeSimpleGroupPaletteFramesEmptyCount;
  summary.runtimeMatrixWriteLastRuntimeModelPtr =
      overrideSummary.runtimeMatrixWriteLastRuntimeModelPtr;
  summary.runtimeMatrixWriteLastMatrixIndex =
      overrideSummary.runtimeMatrixWriteLastMatrixIndex;
  summary.runtimeMatrixWriteLastMatrixCount =
      overrideSummary.runtimeMatrixWriteLastMatrixCount;
  summary.runtimeMatrixWriteLastMatrixHash =
      overrideSummary.runtimeMatrixWriteLastMatrixHash;
  summary.runtimeMatrixRangeCopyPalettePublishHitCount =
      overrideSummary.runtimeMatrixRangeCopyPalettePublishHitCount;
  summary.runtimeMatrixRangeCopyPalettePublishMissCount =
      overrideSummary.runtimeMatrixRangeCopyPalettePublishMissCount;
  summary.runtimeMatrixRangeCopyPaletteFallbackCModelCount =
      overrideSummary.runtimeMatrixRangeCopyPaletteFallbackCModelCount;
  summary.runtimeMatrixFlushPaletteSuppressedCount =
      overrideSummary.runtimeMatrixFlushPaletteSuppressedCount;
  summary.runtimeMatrixRangeCopyLastRuntimeModelPtr =
      overrideSummary.runtimeMatrixRangeCopyLastRuntimeModelPtr;
  summary.runtimeMatrixRangeCopyLastContextPtr =
      overrideSummary.runtimeMatrixRangeCopyLastContextPtr;
  summary.runtimeMatrixRangeCopyLastSourceBasePtr =
      overrideSummary.runtimeMatrixRangeCopyLastSourceBasePtr;
  summary.runtimeMatrixRangeCopyLastMatrixCount =
      overrideSummary.runtimeMatrixRangeCopyLastMatrixCount;
  summary.runtimeMatrixRangeCopyLastMatrixHash =
      overrideSummary.runtimeMatrixRangeCopyLastMatrixHash;
  summary.runtimeMatrixPublisherAttachmentRootHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentRootHitCount;
  summary.runtimeMatrixPublisherAttachmentOwnerHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentOwnerHitCount;
  summary.runtimeMatrixPublisherAttachmentChildHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentChildHitCount;
  summary.runtimeMatrixPublisherAttachmentAliasHitCount =
      overrideSummary.runtimeMatrixPublisherAttachmentAliasHitCount;
  summary.runtimeMatrixPublisherAttachmentRootPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherAttachmentRootPaletteReadyCount;
  summary.runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherAttachmentOwnerPaletteReadyCount;
  summary.runtimeMatrixPublisherAttachmentChildPaletteReadyCount =
      overrideSummary.runtimeMatrixPublisherAttachmentChildPaletteReadyCount;
  summary.attachmentAncestorIdentityHintWriteCount =
      overrideSummary.attachmentAncestorIdentityHintWriteCount;
  summary.sourceObjectRenderBridgeResolvedByEntryCount =
      overrideSummary.sourceObjectRenderBridgeResolvedByEntryCount;
  summary.sourceObjectRenderBridgeResolvedBySceneNodeCount =
      overrideSummary.sourceObjectRenderBridgeResolvedBySceneNodeCount;
  summary.spriteHostBindCount = overrideSummary.spriteHostBindCount;
  summary.spriteHostBindResolvedIdentityCount =
      overrideSummary.spriteHostBindResolvedIdentityCount;
  summary.spriteHostBindResolvedUnitCount =
      overrideSummary.spriteHostBindResolvedUnitCount;
  summary.spriteHostBindResolvedHandleCount =
      overrideSummary.spriteHostBindResolvedHandleCount;
  summary.spriteHostBindResolvedRawcodeCount =
      overrideSummary.spriteHostBindResolvedRawcodeCount;
  // Phase 7.105 opening-skip 诊断透传。
  summary.spriteHostBindOpeningSkipCount =
      overrideSummary.spriteHostBindOpeningSkipCount;
  summary.runtimePaletteTreeOpeningSkipCount =
      overrideSummary.runtimePaletteTreeOpeningSkipCount;
  summary.spriteFrameSourceHintCount =
      overrideSummary.spriteFrameSourceHintCount;
  summary.spriteFrameSourceResolvedIdentityCount =
      overrideSummary.spriteFrameSourceResolvedIdentityCount;
  summary.spriteFrameSourceResolvedUnitCount =
      overrideSummary.spriteFrameSourceResolvedUnitCount;
  summary.spriteFrameSourceResolvedHandleCount =
      overrideSummary.spriteFrameSourceResolvedHandleCount;
  summary.spriteFrameSourceResolvedRawcodeCount =
      overrideSummary.spriteFrameSourceResolvedRawcodeCount;
  summary.spriteFrameSourceBaseAliasPublishCount =
      overrideSummary.spriteFrameSourceBaseAliasPublishCount;
  summary.spriteFrameSourceDeepIdentityResolvedCount =
      overrideSummary.spriteFrameSourceDeepIdentityResolvedCount;
  summary.spriteFrameSourceObjectRuntimeFieldCandidateCount =
      overrideSummary.spriteFrameSourceObjectRuntimeFieldCandidateCount;
  summary.spriteFrameSourceObjectRegistryFieldHitCount =
      overrideSummary.spriteFrameSourceObjectRegistryFieldHitCount;
  summary.spriteFramePoseBaseAliasPublishCount =
      overrideSummary.spriteFramePoseBaseAliasPublishCount;
  summary.spriteFramePoseBaseAliasMatrixPaletteCount =
      overrideSummary.spriteFramePoseBaseAliasMatrixPaletteCount;
  summary.spriteFrameAttachmentRootRuntimeHitCount =
      overrideSummary.spriteFrameAttachmentRootRuntimeHitCount;
  summary.spriteFrameAttachmentOwnerRuntimeHitCount =
      overrideSummary.spriteFrameAttachmentOwnerRuntimeHitCount;
  summary.spriteFrameAttachmentChildRuntimeHitCount =
      overrideSummary.spriteFrameAttachmentChildRuntimeHitCount;
  summary.spriteFrameAttachmentContextHintCount =
      overrideSummary.spriteFrameAttachmentContextHintCount;
  summary.spriteFrameAttachmentFullUpdateHitCount =
      overrideSummary.spriteFrameAttachmentFullUpdateHitCount;
  summary.spriteFrameAttachmentLiteUpdateHitCount =
      overrideSummary.spriteFrameAttachmentLiteUpdateHitCount;
  summary.spriteFrameAttachmentCallerKnownCount =
      overrideSummary.spriteFrameAttachmentCallerKnownCount;
  summary.spriteFrameAttachmentCallerChangedCount =
      overrideSummary.spriteFrameAttachmentCallerChangedCount;
  summary.spriteFrameAttachmentAttachScopeHitCount =
      overrideSummary.spriteFrameAttachmentAttachScopeHitCount;
  summary.spriteFrameAttachmentAttachScopeOwnerHitCount =
      overrideSummary.spriteFrameAttachmentAttachScopeOwnerHitCount;
  summary.spriteFrameAttachmentAttachScopeParentRuntimeMatchCount =
      overrideSummary.spriteFrameAttachmentAttachScopeParentRuntimeMatchCount;
  summary.attachedEffectInitBindCount =
      overrideSummary.attachedEffectInitBindCount;
  summary.attachedEffectInitResolvedIdentityCount =
      overrideSummary.attachedEffectInitResolvedIdentityCount;
  summary.attachedEffectInitResolvedUnitCount =
      overrideSummary.attachedEffectInitResolvedUnitCount;
  summary.attachedEffectInitResolvedHandleCount =
      overrideSummary.attachedEffectInitResolvedHandleCount;
  summary.attachedEffectInitResolvedRawcodeCount =
      overrideSummary.attachedEffectInitResolvedRawcodeCount;
  summary.attachedEffectInitParentRuntimeOwnerPublishCount =
      overrideSummary.attachedEffectInitParentRuntimeOwnerPublishCount;
  summary.attachedEffectDirectBindCount =
      overrideSummary.attachedEffectDirectBindCount;
  summary.attachedEffectDirectResolvedIdentityCount =
      overrideSummary.attachedEffectDirectResolvedIdentityCount;
  summary.attachedEffectDirectResolvedUnitCount =
      overrideSummary.attachedEffectDirectResolvedUnitCount;
  summary.attachedEffectDirectResolvedHandleCount =
      overrideSummary.attachedEffectDirectResolvedHandleCount;
  summary.attachedEffectDirectResolvedRawcodeCount =
      overrideSummary.attachedEffectDirectResolvedRawcodeCount;
  summary.attachModelToPointBindCount =
      overrideSummary.attachModelToPointBindCount;
  summary.attachModelToPointResolvedIdentityCount =
      overrideSummary.attachModelToPointResolvedIdentityCount;
  summary.attachModelToPointResolvedUnitCount =
      overrideSummary.attachModelToPointResolvedUnitCount;
  summary.attachModelToPointResolvedHandleCount =
      overrideSummary.attachModelToPointResolvedHandleCount;
  summary.attachModelToPointResolvedRawcodeCount =
      overrideSummary.attachModelToPointResolvedRawcodeCount;
  summary.attachModelToPointPromotedAttachmentChildRuntimeCount =
      overrideSummary.attachModelToPointPromotedAttachmentChildRuntimeCount;
  summary.attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount =
      overrideSummary
          .attachModelToPointPromotedAttachmentChildRuntimeWithResourceCount;
  summary.currentRenderIdentityHintCount =
      overrideSummary.currentRenderIdentityHintCount;
  summary.currentRenderIdentityResolvedCount =
      overrideSummary.currentRenderIdentityResolvedCount;
  summary.sourceObjectIdentityHintResolvedCount =
      overrideSummary.sourceObjectIdentityHintResolvedCount;
  summary.runtimeSourceObjectPublishCount =
      overrideSummary.runtimeSourceObjectPublishCount;
  summary.runtimeModelResolveCount = overrideSummary.runtimeModelResolveCount;
  summary.runtimeModelResolveResolvedIdentityCount =
      overrideSummary.runtimeModelResolveResolvedIdentityCount;
  summary.attachmentRigidPublishedWithSourceObjectCount =
      overrideSummary.attachmentRigidPublishedWithSourceObjectCount;
  summary.attachmentRigidSourceObjectFromChildRuntimeCount =
      overrideSummary.attachmentRigidSourceObjectFromChildRuntimeCount;
  summary.attachmentRigidSourceObjectFromOwnerRuntimeCount =
      overrideSummary.attachmentRigidSourceObjectFromOwnerRuntimeCount;
  summary.attachmentRigidSourceObjectFromRootRuntimeCount =
      overrideSummary.attachmentRigidSourceObjectFromRootRuntimeCount;
  summary.overrideOutputSampleFrame = overrideSummary.sampleFrame;
  summary.overrideOutputLastActiveFrame = overrideSummary.lastActiveFrame;
  summary.overridePrimaryPresetWriteCount =
      overrideSummary.primaryPresetWriteCount;
  summary.overrideSharedPresetWriteCount =
      overrideSummary.sharedPresetWriteCount;
  summary.overrideLocalPointWriteCount =
      overrideSummary.localPointWriteCount;
  summary.overrideLocalPointNonZeroWriteCount =
      overrideSummary.localPointNonZeroWriteCount;
  summary.overrideLocalPointObservedChildLinkWriteCount =
      overrideSummary.localPointObservedChildLinkWriteCount;
  summary.overrideLocalPointMatchedChildLinkWriteCount =
      overrideSummary.localPointMatchedChildLinkWriteCount;
  summary.overrideLocalPointMatchedChildPaletteReadyWriteCount =
      overrideSummary.localPointMatchedChildPaletteReadyWriteCount;
  summary.overrideLocalPointMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary.localPointMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary.localPointMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.overrideLocalPointContextRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointContextRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointContextMatchedChildLinkWriteCount =
      overrideSummary.localPointContextMatchedChildLinkWriteCount;
  summary.overrideLocalPointContextMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary.localPointContextMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointContextMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary
          .localPointContextMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.overrideLocalPointScratchRootRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointScratchRootRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointScratchRootMatchedChildLinkWriteCount =
      overrideSummary.localPointScratchRootMatchedChildLinkWriteCount;
  summary.overrideLocalPointScratchRootMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointScratchRootMatchedChildLinkBySourceRecordWriteCount;
  summary
      .overrideLocalPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary
          .localPointScratchRootMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.overrideLocalPointArgBlockRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointArgBlockRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointArgBlockMatchedChildLinkWriteCount =
      overrideSummary.localPointArgBlockMatchedChildLinkWriteCount;
  summary.overrideLocalPointArgBlockMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointArgBlockMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointArgBlockIdentityHintWriteCount =
      overrideSummary.localPointArgBlockIdentityHintWriteCount;
  summary.overrideLocalPointArg4BlockRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointArg4BlockRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointArg4BlockMatchedChildLinkWriteCount =
      overrideSummary.localPointArg4BlockMatchedChildLinkWriteCount;
  summary.overrideLocalPointArg4BlockMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointArg4BlockMatchedChildLinkBySourceRecordWriteCount;
  summary.overrideLocalPointArg4BlockIdentityHintWriteCount =
      overrideSummary.localPointArg4BlockIdentityHintWriteCount;
  summary.overrideLocalPointChildSourceMetaIdentityHintWriteCount =
      overrideSummary.localPointChildSourceMetaIdentityHintWriteCount;
  summary.overrideLocalPointSpriteBoundCandidateWriteCount =
      overrideSummary.localPointSpriteBoundCandidateWriteCount;
  summary.overrideLocalPointParentSpriteIdentityHintWriteCount =
      overrideSummary.localPointParentSpriteIdentityHintWriteCount;
  summary.overrideLocalPointRootRuntimeHitWriteCount =
      overrideSummary.localPointRootRuntimeHitWriteCount;
  summary.overrideLocalPointRootRuntimeWithChildLinksWriteCount =
      overrideSummary.localPointRootRuntimeWithChildLinksWriteCount;
  summary.overrideLocalPointRootRuntimeMatchedChildLinkWriteCount =
      overrideSummary.localPointRootRuntimeMatchedChildLinkWriteCount;
  summary.overrideLocalPointRootRuntimeMatchedChildPaletteReadyWriteCount =
      overrideSummary.localPointRootRuntimeMatchedChildPaletteReadyWriteCount;
  summary.overrideLocalPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount =
      overrideSummary
          .localPointRootRuntimeMatchedChildLinkBySourceRecordWriteCount;
  summary
      .overrideLocalPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount =
      overrideSummary
          .localPointRootRuntimeMatchedChildPaletteReadyBySourceRecordWriteCount;
  summary.attachmentRigidPublishedCount =
      overrideSummary.attachmentRigidPublishedCount;
  summary.overrideMaxPrimaryPresetSlotIndex =
      overrideSummary.maxPrimaryPresetSlotIndex;
  summary.overrideMaxSharedPresetSlotIndex =
      overrideSummary.maxSharedPresetSlotIndex;
  summary.overrideMaxLocalPointSlotIndex =
      overrideSummary.maxLocalPointSlotIndex;
  summary.overrideMaxObservedChildLinkCount =
      overrideSummary.maxObservedChildLinkCount;
  summary.overrideMaxObservedChildLinkTag =
      overrideSummary.maxObservedChildLinkTag;
  summary.overrideLastPrimaryPresetHash =
      overrideSummary.lastPrimaryPresetHash;
  summary.overrideLastSharedPresetHash =
      overrideSummary.lastSharedPresetHash;
  summary.overrideLastRuntimeModelPtr =
      overrideSummary.lastRuntimeModelPtr;
  summary.overrideLastMatchedChildRuntimeModelPtr =
      overrideSummary.lastMatchedChildRuntimeModelPtr;
  summary.overrideLastMatchedChildBySourceRecordRuntimeModelPtr =
      overrideSummary.lastMatchedChildBySourceRecordRuntimeModelPtr;
  summary.overrideLastContextRuntimeWithChildLinksPtr =
      overrideSummary.lastContextRuntimeWithChildLinksPtr;
  summary.overrideLastScratchRootPtr = overrideSummary.lastScratchRootPtr;
  summary.overrideLastScratchRootRuntimeModelPtr =
      overrideSummary.lastScratchRootRuntimeModelPtr;
  summary.overrideLastArgBlockPtr = overrideSummary.lastArgBlockPtr;
  summary.overrideLastArgBlockRuntimeModelPtr =
      overrideSummary.lastArgBlockRuntimeModelPtr;
  summary.overrideLastArgBlockIdentityHintPtr =
      overrideSummary.lastArgBlockIdentityHintPtr;
  summary.overrideLastArg4BlockPtr = overrideSummary.lastArg4BlockPtr;
  summary.overrideLastArg4BlockRuntimeModelPtr =
      overrideSummary.lastArg4BlockRuntimeModelPtr;
  summary.overrideLastArg4BlockIdentityHintPtr =
      overrideSummary.lastArg4BlockIdentityHintPtr;
  summary.overrideLastChildSourceMetaPtr =
      overrideSummary.lastChildSourceMetaPtr;
  summary.overrideLastChildSourceMetaRuntimeModelPtr =
      overrideSummary.lastChildSourceMetaRuntimeModelPtr;
  summary.overrideLastSpriteBoundCandidateSpritePtr =
      overrideSummary.lastSpriteBoundCandidateSpritePtr;
  summary.overrideLastSpriteBoundCandidateRuntimeModelPtr =
      overrideSummary.lastSpriteBoundCandidateRuntimeModelPtr;
  summary.overrideLastParentSpriteIdentityHintSpritePtr =
      overrideSummary.lastParentSpriteIdentityHintSpritePtr;
  summary.overrideLastParentSpriteIdentityHintRuntimeModelPtr =
      overrideSummary.lastParentSpriteIdentityHintRuntimeModelPtr;
  summary.overrideLastRootRuntimeModelPtr =
      overrideSummary.lastRootRuntimeModelPtr;
  summary.lastSourceObjectRenderBridgeSourceObjectPtr =
      overrideSummary.lastSourceObjectRenderBridgeSourceObjectPtr;
  summary.lastSourceObjectRenderBridgeSceneNodePtr =
      overrideSummary.lastSourceObjectRenderBridgeSceneNodePtr;
  summary.lastSourceObjectIdentityHintSourceObjectPtr =
      overrideSummary.lastSourceObjectIdentityHintSourceObjectPtr;
  summary.lastSourceObjectIdentityHintCandidatePtr =
      overrideSummary.lastSourceObjectIdentityHintCandidatePtr;
  summary.lastSpriteHostSourceObjectPtr =
      overrideSummary.lastSpriteHostSourceObjectPtr;
  summary.lastSpriteHostSpritePtr = overrideSummary.lastSpriteHostSpritePtr;
  summary.lastSpriteHostRuntimeModelPtr =
      overrideSummary.lastSpriteHostRuntimeModelPtr;
  summary.lastSpriteHostUnitPtr = overrideSummary.lastSpriteHostUnitPtr;
  summary.lastSpriteFrameSourceObjectPtr =
      overrideSummary.lastSpriteFrameSourceObjectPtr;
  summary.lastSpriteFrameSourceRuntimeModelPtr =
      overrideSummary.lastSpriteFrameSourceRuntimeModelPtr;
  summary.lastSpriteFrameSourceBaseRuntimeModelPtr =
      overrideSummary.lastSpriteFrameSourceBaseRuntimeModelPtr;
  summary.lastSpriteFrameSourceObjectVtablePtr =
      overrideSummary.lastSpriteFrameSourceObjectVtablePtr;
  summary.lastSpriteFrameSourceObjectSceneNodeCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectSceneNodeCandidatePtr;
  summary.lastSpriteFrameSourceObjectSpriteCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectSpriteCandidatePtr;
  summary.lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectRuntimeFieldCandidatePtr;
  summary.lastSpriteFrameSourceObjectRegistryFieldCandidatePtr =
      overrideSummary.lastSpriteFrameSourceObjectRegistryFieldCandidatePtr;
  summary.lastSpriteFrameSourceDeepIdentityCandidatePtr =
      overrideSummary.lastSpriteFrameSourceDeepIdentityCandidatePtr;
  summary.lastSpriteFrameSourceWorldObjectEntryPtr =
      overrideSummary.lastSpriteFrameSourceWorldObjectEntryPtr;
  summary.lastSpriteFrameSourceSceneNodePtr =
      overrideSummary.lastSpriteFrameSourceSceneNodePtr;
  summary.lastSpriteFrameSourceUnitPtr =
      overrideSummary.lastSpriteFrameSourceUnitPtr;
  summary.lastSpriteFramePoseBaseRuntimeModelPtr =
      overrideSummary.lastSpriteFramePoseBaseRuntimeModelPtr;
  summary.lastSpriteFramePoseBaseMatrixCount =
      overrideSummary.lastSpriteFramePoseBaseMatrixCount;
  summary.lastSpriteFrameAttachmentSpritePtr =
      overrideSummary.lastSpriteFrameAttachmentSpritePtr;
  summary.lastSpriteFrameAttachmentRuntimeModelPtr =
      overrideSummary.lastSpriteFrameAttachmentRuntimeModelPtr;
  summary.lastSpriteFrameAttachmentContextPtr =
      overrideSummary.lastSpriteFrameAttachmentContextPtr;
  summary.lastAttachedEffectInitOwnerWidgetPtr =
      overrideSummary.lastAttachedEffectInitOwnerWidgetPtr;
  summary.lastAttachedEffectInitChildSpritePtr =
      overrideSummary.lastAttachedEffectInitChildSpritePtr;
  summary.lastAttachedEffectInitChildRuntimeModelPtr =
      overrideSummary.lastAttachedEffectInitChildRuntimeModelPtr;
  summary.lastAttachedEffectInitUnitPtr =
      overrideSummary.lastAttachedEffectInitUnitPtr;
  summary.lastAttachedEffectDirectOwnerWidgetPtr =
      overrideSummary.lastAttachedEffectDirectOwnerWidgetPtr;
  summary.lastAttachedEffectDirectChildSpritePtr =
      overrideSummary.lastAttachedEffectDirectChildSpritePtr;
  summary.lastAttachedEffectDirectChildRuntimeModelPtr =
      overrideSummary.lastAttachedEffectDirectChildRuntimeModelPtr;
  summary.lastAttachedEffectDirectUnitPtr =
      overrideSummary.lastAttachedEffectDirectUnitPtr;
  summary.lastAttachModelToPointParentSpritePtr =
      overrideSummary.lastAttachModelToPointParentSpritePtr;
  summary.lastAttachModelToPointChildSpritePtr =
      overrideSummary.lastAttachModelToPointChildSpritePtr;
  summary.lastAttachModelToPointChildRuntimeModelPtr =
      overrideSummary.lastAttachModelToPointChildRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedOwnerRuntimeModelPtr =
      overrideSummary.lastAttachModelToPointPromotedOwnerRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr =
      overrideSummary
          .lastAttachModelToPointPromotedPreviousChildRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedChildRuntimeModelPtr =
      overrideSummary.lastAttachModelToPointPromotedChildRuntimeModelPtr;
  summary.lastAttachModelToPointPromotedChildModelResourcePtr =
      overrideSummary.lastAttachModelToPointPromotedChildModelResourcePtr;
  summary.lastAttachModelToPointUnitPtr =
      overrideSummary.lastAttachModelToPointUnitPtr;
  summary.lastAttachScopeParentSpritePtr =
      overrideSummary.lastAttachScopeParentSpritePtr;
  summary.lastAttachScopeParentRuntimeModelPtr =
      overrideSummary.lastAttachScopeParentRuntimeModelPtr;
  summary.lastAttachScopeChildSpritePtr =
      overrideSummary.lastAttachScopeChildSpritePtr;
  summary.lastAttachScopeChildRuntimeModelPtr =
      overrideSummary.lastAttachScopeChildRuntimeModelPtr;
  summary.lastAttachScopeHitRuntimeModelPtr =
      overrideSummary.lastAttachScopeHitRuntimeModelPtr;
  summary.lastCurrentRenderIdentityWorldObjectEntryPtr =
      overrideSummary.lastCurrentRenderIdentityWorldObjectEntryPtr;
  summary.lastCurrentRenderIdentitySceneNodePtr =
      overrideSummary.lastCurrentRenderIdentitySceneNodePtr;
  summary.lastCurrentRenderIdentityUnitPtr =
      overrideSummary.lastCurrentRenderIdentityUnitPtr;
  summary.lastRuntimeSourceObjectPtr =
      overrideSummary.lastRuntimeSourceObjectPtr;
  summary.lastRuntimeSourceSpriteObjectPtr =
      overrideSummary.lastRuntimeSourceSpriteObjectPtr;
  summary.lastRuntimeSourceRuntimeModelPtr =
      overrideSummary.lastRuntimeSourceRuntimeModelPtr;
  summary.lastRuntimeModelResolveRuntimeModelPtr =
      overrideSummary.lastRuntimeModelResolveRuntimeModelPtr;
  summary.lastRuntimeModelResolveHandlePtr =
      overrideSummary.lastRuntimeModelResolveHandlePtr;
  summary.lastRuntimeModelCreateRuntimeModelPtr =
      overrideSummary.lastRuntimeModelCreateRuntimeModelPtr;
  summary.lastRuntimeModelCreateModelDataPtr =
      overrideSummary.lastRuntimeModelCreateModelDataPtr;
  summary.lastRuntimeModelInitRuntimeModelPtr =
      overrideSummary.lastRuntimeModelInitRuntimeModelPtr;
  summary.lastRuntimeModelInitModelDataPtr =
      overrideSummary.lastRuntimeModelInitModelDataPtr;
  summary.lastAttachmentRigidSourceObjectPtr =
      overrideSummary.lastAttachmentRigidSourceObjectPtr;
  summary.lastAttachmentRigidSourceSpriteObjectPtr =
      overrideSummary.lastAttachmentRigidSourceSpriteObjectPtr;
  summary.lastRuntimeChildLinkBuildParentRuntimeModelPtr =
      overrideSummary.lastRuntimeChildLinkBuildParentRuntimeModelPtr;
  summary.lastRuntimeChildLinkBuildChildRuntimeModelPtr =
      overrideSummary.lastRuntimeChildLinkBuildChildRuntimeModelPtr;
  summary.lastRuntimeChildLinkBuildModelDataPtr =
      overrideSummary.lastRuntimeChildLinkBuildModelDataPtr;
  summary.lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectParentRuntimeModelPtr;
  summary.lastRuntimeChildBuildTimeDirectParentModelDataPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectParentModelDataPtr;
  summary.lastRuntimeChildBuildTimeDirectRuntimeModelPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectRuntimeModelPtr;
  summary.lastRuntimeChildBuildTimeDirectModelDataPtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectModelDataPtr;
  summary.lastRuntimeChildBuildTimeDirectModelResourcePtr =
      overrideSummary.lastRuntimeChildBuildTimeDirectModelResourcePtr;
  summary.lastRuntimeChildBuildModelDataParentRuntimeModelPtr =
      overrideSummary.lastRuntimeChildBuildModelDataParentRuntimeModelPtr;
  summary.lastRuntimeChildBuildModelDataPtr =
      overrideSummary.lastRuntimeChildBuildModelDataPtr;
  summary.lastRuntimeChildBuildModelDataGroupRecordsPtr =
      overrideSummary.lastRuntimeChildBuildModelDataGroupRecordsPtr;
  summary.lastRuntimeChildBuildModelDataHeadPtr =
      overrideSummary.lastRuntimeChildBuildModelDataHeadPtr;
  summary.lastRuntimeChildBuildModelDataLinkNodePtr =
      overrideSummary.lastRuntimeChildBuildModelDataLinkNodePtr;
  summary.lastRuntimeChildBuildModelDataChildModelDataPtr =
      overrideSummary.lastRuntimeChildBuildModelDataChildModelDataPtr;
  summary.lastRuntimeChildBuildModelDataChildModelResourcePtr =
      overrideSummary.lastRuntimeChildBuildModelDataChildModelResourcePtr;
  summary.lastRuntimeMatrixPublisherRuntimeModelPtr =
      overrideSummary.lastRuntimeMatrixPublisherRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherMatchedRuntimeModelPtr =
      overrideSummary.lastRuntimeMatrixPublisherMatchedRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherMatrixCount;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentRootHitRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentRootHitOwnerRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentRootHitChildRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentRootHitMatrixCount;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentOwnerHitRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentOwnerHitRootRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentOwnerHitChildRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentOwnerHitMatrixCount;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentChildHitRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentChildHitRootRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr =
      overrideSummary
          .lastRuntimeMatrixPublisherAttachmentChildHitOwnerRuntimeModelPtr;
  summary.lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount =
      overrideSummary.lastRuntimeMatrixPublisherAttachmentChildHitMatrixCount;
  summary.lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapCandidate0ModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr =
      overrideSummary
          .lastAttachmentChildLineageBootstrapCandidate0ModelResourcePtr;
  summary.lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapCandidate1ModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr =
      overrideSummary
          .lastAttachmentChildLineageBootstrapCandidate1ModelResourcePtr;
  summary.lastAttachmentChildLineageBootstrapParentRuntimeModelPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapParentRuntimeModelPtr;
  summary.lastAttachmentChildLineageBootstrapChildRuntimeModelPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapChildRuntimeModelPtr;
  summary.lastAttachmentChildLineageBootstrapParentModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapParentModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapChildModelDataPtr =
      overrideSummary.lastAttachmentChildLineageBootstrapChildModelDataPtr;
  summary.lastAttachmentChildLineageBootstrapChildModelResourcePtr =
      overrideSummary.lastAttachmentChildLineageBootstrapChildModelResourcePtr;
  summary.lastAttachmentAncestorFromRuntimeModelPtr =
      overrideSummary.lastAttachmentAncestorFromRuntimeModelPtr;
  summary.lastAttachmentAncestorRuntimeModelPtr =
      overrideSummary.lastAttachmentAncestorRuntimeModelPtr;
  summary.overrideLastLocalPointSlotIndex =
      overrideSummary.lastLocalPointSlotIndex;
  summary.overrideLastLocalPointSourceRecordIndex =
      overrideSummary.lastLocalPointSourceRecordIndex;
  summary.overrideLastObservedChildLinkCount =
      overrideSummary.lastObservedChildLinkCount;
  summary.overrideLastMatchedChildLinkCount =
      overrideSummary.lastMatchedChildLinkCount;
  summary.overrideLastMatchedChildMatrixCount =
      overrideSummary.lastMatchedChildMatrixCount;
  summary.overrideLastMatchedChildBySourceRecordLinkCount =
      overrideSummary.lastMatchedChildBySourceRecordLinkCount;
  summary.overrideLastMatchedChildBySourceRecordMatrixCount =
      overrideSummary.lastMatchedChildBySourceRecordMatrixCount;
  summary.overrideLastContextRuntimeWithChildLinksOffset =
      overrideSummary.lastContextRuntimeWithChildLinksOffset;
  summary.overrideLastContextRuntimeWithChildLinksCount =
      overrideSummary.lastContextRuntimeWithChildLinksCount;
  summary.overrideLastContextRuntimeWithChildLinksMaxTag =
      overrideSummary.lastContextRuntimeWithChildLinksMaxTag;
  summary.overrideLastScratchRootRuntimeChildLinkCount =
      overrideSummary.lastScratchRootRuntimeChildLinkCount;
  summary.overrideLastScratchRootRuntimeMaxTag =
      overrideSummary.lastScratchRootRuntimeMaxTag;
  summary.overrideLastArgBlockRuntimeOffset =
      overrideSummary.lastArgBlockRuntimeOffset;
  summary.overrideLastArgBlockRuntimeChildLinkCount =
      overrideSummary.lastArgBlockRuntimeChildLinkCount;
  summary.overrideLastArgBlockRuntimeMaxTag =
      overrideSummary.lastArgBlockRuntimeMaxTag;
  summary.overrideLastArgBlockIdentityHintOffset =
      overrideSummary.lastArgBlockIdentityHintOffset;
  summary.overrideLastArg4BlockRuntimeOffset =
      overrideSummary.lastArg4BlockRuntimeOffset;
  summary.overrideLastArg4BlockRuntimeChildLinkCount =
      overrideSummary.lastArg4BlockRuntimeChildLinkCount;
  summary.overrideLastArg4BlockRuntimeMaxTag =
      overrideSummary.lastArg4BlockRuntimeMaxTag;
  summary.overrideLastArg4BlockIdentityHintOffset =
      overrideSummary.lastArg4BlockIdentityHintOffset;
  summary.overrideLastRootRuntimeChildLinkCount =
      overrideSummary.lastRootRuntimeChildLinkCount;
  summary.overrideLastRootRuntimeMaxTag =
      overrideSummary.lastRootRuntimeMaxTag;
  summary.lastSpriteHostJHandle = overrideSummary.lastSpriteHostJHandle;
  summary.lastSpriteHostRawcode = overrideSummary.lastSpriteHostRawcode;
  summary.lastSpriteFrameSourceJHandle =
      overrideSummary.lastSpriteFrameSourceJHandle;
  summary.lastSpriteFrameSourceRawcode =
      overrideSummary.lastSpriteFrameSourceRawcode;
  summary.lastSpriteFrameSourceObjectRuntimeFieldOffset =
      overrideSummary.lastSpriteFrameSourceObjectRuntimeFieldOffset;
  summary.lastSpriteFrameSourceObjectRegistryFieldOffset =
      overrideSummary.lastSpriteFrameSourceObjectRegistryFieldOffset;
  summary.lastSpriteFrameSourceDeepIdentityOffset =
      overrideSummary.lastSpriteFrameSourceDeepIdentityOffset;
  summary.lastSpriteFrameAttachmentRoleMask =
      overrideSummary.lastSpriteFrameAttachmentRoleMask;
  summary.lastSpriteFrameAttachmentUpdateKind =
      overrideSummary.lastSpriteFrameAttachmentUpdateKind;
  summary.lastSpriteFrameAttachmentCallerRva =
      overrideSummary.lastSpriteFrameAttachmentCallerRva;
  summary.lastSourceObjectIdentityHintOffset =
      overrideSummary.lastSourceObjectIdentityHintOffset;
  summary.lastAttachedEffectInitJHandle =
      overrideSummary.lastAttachedEffectInitJHandle;
  summary.lastAttachedEffectInitRawcode =
      overrideSummary.lastAttachedEffectInitRawcode;
  summary.lastAttachedEffectDirectJHandle =
      overrideSummary.lastAttachedEffectDirectJHandle;
  summary.lastAttachedEffectDirectRawcode =
      overrideSummary.lastAttachedEffectDirectRawcode;
  summary.lastAttachModelToPointJHandle =
      overrideSummary.lastAttachModelToPointJHandle;
  summary.lastAttachModelToPointRawcode =
      overrideSummary.lastAttachModelToPointRawcode;
  summary.lastAttachModelToPointAttachPointIndex =
      overrideSummary.lastAttachModelToPointAttachPointIndex;
  summary.lastAttachScopeCallerRva =
      overrideSummary.lastAttachScopeCallerRva;
  summary.lastAttachScopeHitRoleMask =
      overrideSummary.lastAttachScopeHitRoleMask;
  summary.lastAttachedEffectInitParentRuntimeModelPtr =
      overrideSummary.lastAttachedEffectInitParentRuntimeModelPtr;
  summary.lastRuntimeModelCtorRuntimeModelPtr =
      overrideSummary.lastRuntimeModelCtorRuntimeModelPtr;
  summary.lastRuntimeModelCtorCallerRva =
      overrideSummary.lastRuntimeModelCtorCallerRva;
  summary.lastRuntimeModelCtorKind =
      overrideSummary.lastRuntimeModelCtorKind;
  summary.lastRuntimeModelResolveCallerRva =
      overrideSummary.lastRuntimeModelResolveCallerRva;
  summary.lastRuntimeModelCreateCallerRva =
      overrideSummary.lastRuntimeModelCreateCallerRva;
  summary.lastRuntimeModelInitCallerRva =
      overrideSummary.lastRuntimeModelInitCallerRva;
  summary.lastRuntimeChildLinkBuildSourceMeta =
      overrideSummary.lastRuntimeChildLinkBuildSourceMeta;
  summary.lastRuntimeChildBuildModelDataPhase =
      overrideSummary.lastRuntimeChildBuildModelDataPhase;
  summary.lastRuntimeChildBuildModelDataGroupCount =
      overrideSummary.lastRuntimeChildBuildModelDataGroupCount;
  summary.lastRuntimeChildBuildModelDataLinkCount =
      overrideSummary.lastRuntimeChildBuildModelDataLinkCount;
  summary.lastRuntimeChildBuildModelDataUnreadableLinkCount =
      overrideSummary.lastRuntimeChildBuildModelDataUnreadableLinkCount;
  summary.lastRuntimeChildBuildModelDataSourceMeta =
      overrideSummary.lastRuntimeChildBuildModelDataSourceMeta;
  summary.lastRuntimeMatrixPublisherKind =
      overrideSummary.lastRuntimeMatrixPublisherKind;
  summary.lastRuntimeMatrixPublisherRoleMask =
      overrideSummary.lastRuntimeMatrixPublisherRoleMask;
  summary.lastAttachmentChildLineageBootstrapSourceMeta =
      overrideSummary.lastAttachmentChildLineageBootstrapSourceMeta;
  summary.lastAttachmentChildLineageBootstrapBucketIndex =
      overrideSummary.lastAttachmentChildLineageBootstrapBucketIndex;
  summary.lastAttachmentChildLineageBootstrapModelDataLinkCount =
      overrideSummary.lastAttachmentChildLineageBootstrapModelDataLinkCount;
  summary.lastAttachmentChildLineageBootstrapRuntimeLinkCount =
      overrideSummary.lastAttachmentChildLineageBootstrapRuntimeLinkCount;
  summary.lastAttachmentChildLineageBootstrapStrictCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapStrictCandidateCount;
  summary.lastAttachmentChildLineageBootstrapSourceCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapSourceCandidateCount;
  summary.lastAttachmentChildLineageBootstrapBucketCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapBucketCandidateCount;
  summary.lastAttachmentChildLineageBootstrapAllCandidateCount =
      overrideSummary.lastAttachmentChildLineageBootstrapAllCandidateCount;
  summary.lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal =
      overrideSummary.lastAttachmentChildLineageBootstrapRuntimeBucketOrdinal;
  summary.lastAttachmentChildLineageBootstrapModelDataBucketCount =
      overrideSummary.lastAttachmentChildLineageBootstrapModelDataBucketCount;
  summary.lastAttachmentAncestorDepth =
      overrideSummary.lastAttachmentAncestorDepth;
  summary.overrideLastLocalPointX = overrideSummary.lastLocalPointX;
  summary.overrideLastLocalPointY = overrideSummary.lastLocalPointY;
  summary.overrideLastLocalPointZ = overrideSummary.lastLocalPointZ;
  summary.poseFrame = summary.runtimePoseHooksActive
                          ? model::PoseRegistry::instance().frameNumber()
                          : model::ModelInstanceRegistry::instance().frameNumber();

  summary.runtimeChainWarm =
      summary.modelRegistryCount >= 16 &&
      summary.runtimeBoundCount >= 16 &&
      summary.completeIdentityCount + 4 >= summary.runtimeBoundCount &&
      (!summary.runtimePoseHooksActive ||
       (summary.poseReadyCount >= 16 &&
        (summary.spriteFramePoseCount + summary.matrixPaletteCount) >= 8));

  const uint64_t identityGap =
      summary.runtimeBoundCount > summary.completeIdentityCount
          ? (summary.runtimeBoundCount - summary.completeIdentityCount)
          : 0u;
  const uint64_t runtimeBindGap =
      summary.modelRegistryCount > summary.runtimeBoundCount
          ? (summary.modelRegistryCount - summary.runtimeBoundCount)
          : 0u;
  const uint64_t poseGap =
      summary.runtimeBoundCount > summary.poseReadyCount
          ? (summary.runtimeBoundCount - summary.poseReadyCount)
          : 0u;

  const bool identityNeedsRepair =
      summary.runtimeBoundCount >= 24u && identityGap > 12u &&
      identityGap * 4u > summary.runtimeBoundCount;
  const bool runtimeBindNeedsRepair =
      summary.modelRegistryCount >= 32u && runtimeBindGap > 8u &&
      runtimeBindGap * 2u > summary.modelRegistryCount;
  const bool poseNeedsRepair =
      summary.runtimePoseHooksActive && summary.runtimeBoundCount >= 24u &&
      ((poseGap > 12u && poseGap * 4u > summary.runtimeBoundCount) ||
       (summary.spriteFramePoseCount + summary.matrixPaletteCount) == 0u);

  summary.runtimeChainNeedsRepair =
      identityNeedsRepair || runtimeBindNeedsRepair || poseNeedsRepair;
  const bool nativeTakeoverWarm =
      summary.runtimeChainWarm &&
      summary.semanticCoreFrameFresh &&
      summary.nativeD3D9BackendHasDevice &&
      summary.nativeD3D9BackendSubmittedDrawCount != 0u;
  if (nativeTakeoverWarm) {
    dxvk::War3Hook::MaybeInstallNativeRendererTakeover(
        "runtime-bridge-warm");
  }
  return summary;
}

namespace {

struct ShadowRuntimeBridgeTrackingHealth {
  uint64_t poseFrame = 0u;
  uint64_t fastPathCalls = 0u;
  uint64_t fullSummaryCompatibilityCalls = 0u;
  uint64_t modelInstanceAggregateReadPasses = 0u;
  uint64_t poseAggregateReadPasses = 0u;
  uint64_t modelInstanceVerifierScanPasses = 0u;
  uint64_t poseVerifierScanPasses = 0u;
  uint64_t modelInstanceVerifierRecordsScanned = 0u;
  uint64_t poseVerifierRecordsScanned = 0u;
  uint64_t modelInstanceVerifierMismatchCount = 0u;
  uint64_t poseVerifierMismatchCount = 0u;
  uint32_t modelInstanceVerifierMismatchMask = 0u;
  uint32_t poseVerifierMismatchMask = 0u;
  bool runtimePoseHooksActive = false;
  bool runtimeChainWarm = false;
  bool runtimeChainNeedsRepair = false;
};

ShadowRuntimeBridgeTrackingHealth QueryShadowRuntimeBridgeTrackingHealth() {
  ShadowRuntimeBridgeTrackingHealth health = {};
  if constexpr (internal::kNativeRendererHookTakeoverEnabled) {
    // QueryShadowRuntimeBridgeSummary also owns the historical native takeover
    // installation side effect.  Preserve that exact behavior if takeover is
    // ever compiled on; the current production configuration compiles this
    // large compatibility path out of per-frame tracking.
    const auto summary = QueryShadowRuntimeBridgeSummary();
    health.poseFrame = summary.poseFrame;
    health.fullSummaryCompatibilityCalls = 1u;
    health.runtimePoseHooksActive = summary.runtimePoseHooksActive;
    health.runtimeChainWarm = summary.runtimeChainWarm;
    health.runtimeChainNeedsRepair = summary.runtimeChainNeedsRepair;
  } else {
    const auto contractStats =
        shadow::ShadowRuntimeContractCache::instance().snapshotStats();
    health.runtimePoseHooksActive = model::IsPoseHookEnabled();
    const uint64_t modelRegistryCount =
        uint64_t(model::ModelRegistry::instance().recordCount());
    const auto instanceHealth =
        model::ModelInstanceRegistry::instance().trackingHealthSnapshot();
    const auto poseHealth =
        model::PoseRegistry::instance().trackingHealthSnapshot();

    health.fastPathCalls = 1u;
    health.modelInstanceAggregateReadPasses =
        instanceHealth.aggregateReadPasses;
    health.poseAggregateReadPasses = poseHealth.aggregateReadPasses;
    health.modelInstanceVerifierScanPasses =
        instanceHealth.verifierScanPasses;
    health.poseVerifierScanPasses = poseHealth.verifierScanPasses;
    health.modelInstanceVerifierRecordsScanned =
        instanceHealth.verifierRecordsScanned;
    health.poseVerifierRecordsScanned =
        poseHealth.verifierRecordsScanned;
    health.modelInstanceVerifierMismatchCount =
        instanceHealth.verifierMismatchCount;
    health.poseVerifierMismatchCount =
        poseHealth.verifierMismatchCount;
    health.modelInstanceVerifierMismatchMask =
        instanceHealth.verifierMismatchMask;
    health.poseVerifierMismatchMask = poseHealth.verifierMismatchMask;
    health.poseFrame = health.runtimePoseHooksActive
        ? poseHealth.frameNumber
        : instanceHealth.frameNumber;

    const uint64_t matrixPaletteCount = (std::max)(
        poseHealth.matrixPaletteCount, contractStats.matrixPaletteCount);

    // Keep these predicates equivalent to the corresponding
    // fields in QueryShadowRuntimeBridgeSummary.  Only their data acquisition
    // is consolidated; warm/repair policy remains unchanged.
    health.runtimeChainWarm =
        modelRegistryCount >= 16u &&
        instanceHealth.runtimeBoundCount >= 16u &&
        instanceHealth.completeIdentityCount + 4u >=
            instanceHealth.runtimeBoundCount &&
        (!health.runtimePoseHooksActive ||
         (poseHealth.readyPoseCount >= 16u &&
          (poseHealth.spriteFramePoseCount + matrixPaletteCount) >= 8u));

    const uint64_t identityGap =
        instanceHealth.runtimeBoundCount >
                instanceHealth.completeIdentityCount
            ? (instanceHealth.runtimeBoundCount -
               instanceHealth.completeIdentityCount)
            : 0u;
    const uint64_t runtimeBindGap =
        modelRegistryCount > instanceHealth.runtimeBoundCount
            ? (modelRegistryCount - instanceHealth.runtimeBoundCount)
            : 0u;
    const uint64_t poseGap =
        instanceHealth.runtimeBoundCount > poseHealth.readyPoseCount
            ? (instanceHealth.runtimeBoundCount - poseHealth.readyPoseCount)
            : 0u;

    const bool identityNeedsRepair =
        instanceHealth.runtimeBoundCount >= 24u && identityGap > 12u &&
        identityGap * 4u > instanceHealth.runtimeBoundCount;
    const bool runtimeBindNeedsRepair =
        modelRegistryCount >= 32u && runtimeBindGap > 8u &&
        runtimeBindGap * 2u > modelRegistryCount;
    const bool poseNeedsRepair =
        health.runtimePoseHooksActive &&
        instanceHealth.runtimeBoundCount >= 24u &&
        ((poseGap > 12u &&
          poseGap * 4u > instanceHealth.runtimeBoundCount) ||
         (poseHealth.spriteFramePoseCount + matrixPaletteCount) == 0u);

    health.runtimeChainNeedsRepair =
        identityNeedsRepair || runtimeBindNeedsRepair || poseNeedsRepair;
  }
  return health;
}

} // namespace

ShadowRuntimeBridgeTrackingDecision ComputeShadowRuntimeBridgeTracking() {
  ShadowRuntimeBridgeTrackingDecision decision = {};
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled) {
    FinalizeWorldObjectsPhase1PreviousFrameWithoutNewDecision();
    return decision;
  }

  const int64_t trackingBegin =
      dxvk::high_resolution_clock::get_counter();
  const auto health = QueryShadowRuntimeBridgeTrackingHealth();
  const int64_t queryEnd =
      dxvk::high_resolution_clock::get_counter();
  decision.trackingHealthFastPathCalls = health.fastPathCalls;
  decision.trackingHealthFullSummaryCompatibilityCalls =
      health.fullSummaryCompatibilityCalls;
  decision.trackingHealthModelInstanceAggregateReadPasses =
      health.modelInstanceAggregateReadPasses;
  decision.trackingHealthPoseAggregateReadPasses =
      health.poseAggregateReadPasses;
  decision.trackingHealthModelInstanceVerifierScanPasses =
      health.modelInstanceVerifierScanPasses;
  decision.trackingHealthPoseVerifierScanPasses =
      health.poseVerifierScanPasses;
  decision.trackingHealthModelInstanceVerifierRecordsScanned =
      health.modelInstanceVerifierRecordsScanned;
  decision.trackingHealthPoseVerifierRecordsScanned =
      health.poseVerifierRecordsScanned;
  decision.trackingHealthModelInstanceVerifierMismatchCount =
      health.modelInstanceVerifierMismatchCount;
  decision.trackingHealthPoseVerifierMismatchCount =
      health.poseVerifierMismatchCount;
  decision.trackingHealthModelInstanceVerifierMismatchMask =
      health.modelInstanceVerifierMismatchMask;
  decision.trackingHealthPoseVerifierMismatchMask =
      health.poseVerifierMismatchMask;
  const bool semanticSceneOwnsUnits =
      dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
      dxvk::war3::internal::
          IsSemanticSceneBypassLegacyUnitCaptureRuntimeEnabled() &&
      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly;
  const uint64_t refreshPeriod = health.runtimePoseHooksActive ? 240u : 300u;
  uint64_t warmupFrames = health.runtimePoseHooksActive ? 60u : 24u;
  if (semanticSceneOwnsUnits) {
    // 语义 scene 已经能直接消费 runtimeModel + pose palette 时，
    // 不再需要在低 FPS 阶段坚持几十帧的“全量 identity warmup”。
    // 否则 poseFrame 会长时间卡在 warmup 区间，导致每帧重复 CollectWorldObjects，
    // 形成 FPS 越低、warmup 越走不完的恶性循环。
    warmupFrames = health.runtimeChainWarm ? 4u : 8u;
  }
  const bool repairBurstActive =
      health.poseFrame != 0u &&
      health.poseFrame <=
          g_shadowBridgeRepairUntilFrame.load(std::memory_order_relaxed);
  const bool warmupActive = health.poseFrame < warmupFrames;
  const bool periodicMaintenanceActive =
      health.poseFrame >= warmupFrames &&
      (health.poseFrame % refreshPeriod) == 0u;
  const bool shouldRefreshIdentity =
      warmupActive || periodicMaintenanceActive ||
      repairBurstActive ||
      health.runtimeChainNeedsRepair;

  decision.wantsObjectIdentity =
      shouldRefreshIdentity || !health.runtimeChainWarm;
  decision.wantsFallbackBridge =
      !health.runtimeChainWarm || health.poseFrame < warmupFrames ||
      repairBurstActive ||
      health.runtimeChainNeedsRepair;

  if (semanticSceneOwnsUnits && health.runtimeChainWarm &&
      !repairBurstActive && !health.runtimeChainNeedsRepair) {
    // units-first semantic scene 已接管后，legacy fallback 只保留修复/诊断角色。
    decision.wantsFallbackBridge = false;
  }

  uint32_t reasonMask = 0u;
  if (!health.runtimeChainWarm) {
    reasonMask |= WorldObjectsPhase1ReasonBit(
        WorldObjectsPhase1TrackingReason::ColdBootstrap);
  }
  if (warmupActive) {
    reasonMask |= WorldObjectsPhase1ReasonBit(
        WorldObjectsPhase1TrackingReason::Warmup);
  }
  if (periodicMaintenanceActive) {
    reasonMask |= WorldObjectsPhase1ReasonBit(
        WorldObjectsPhase1TrackingReason::PeriodicMaintenance);
  }
  if (repairBurstActive) {
    reasonMask |= WorldObjectsPhase1ReasonBit(
        WorldObjectsPhase1TrackingReason::RepairBurst);
  }
  if (health.runtimeChainNeedsRepair) {
    reasonMask |= WorldObjectsPhase1ReasonBit(
        WorldObjectsPhase1TrackingReason::RuntimeChainRepair);
  }

  WorldObjectsPhase1TrackingReason reason =
      WorldObjectsPhase1TrackingReason::None;
  if (health.runtimeChainNeedsRepair) {
    reason = WorldObjectsPhase1TrackingReason::RuntimeChainRepair;
  } else if (repairBurstActive) {
    reason = WorldObjectsPhase1TrackingReason::RepairBurst;
  } else if (warmupActive) {
    reason = WorldObjectsPhase1TrackingReason::Warmup;
  } else if (!health.runtimeChainWarm) {
    reason = WorldObjectsPhase1TrackingReason::ColdBootstrap;
  } else if (periodicMaintenanceActive) {
    reason = WorldObjectsPhase1TrackingReason::PeriodicMaintenance;
  }

  const uint64_t frameSerial =
      RenderObjectRegistry::instance().getFrameNumber();
  const uint64_t collectionFrameSerial = frameSerial + 1u;
  const int64_t trackingEnd =
      dxvk::high_resolution_clock::get_counter();
  RecordWorldObjectsPhase1Tracking(
      frameSerial, collectionFrameSerial, health.poseFrame, reason,
      reasonMask, refreshPeriod, warmupFrames, decision,
      WorldObjectsPhase1TickDelta(trackingBegin, trackingEnd),
      WorldObjectsPhase1TickDelta(trackingBegin, queryEnd),
      WorldObjectsPhase1TickDelta(queryEnd, trackingEnd));
  return decision;
}

void ResetShadowRuntimeBridgeState() {
  g_shadowBridgeRepairUntilFrame.store(0u, std::memory_order_relaxed);
  g_shadowBridgeRepairCooldownUntilFrame.store(0u,
                                               std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidateCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidatePrepareCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidateRefreshCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageCandidateExecuteCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageSkippedRuntimeNotReadyCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateStage.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA3.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA4.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateA5.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateJassReady.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateGameStarted.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastCandidateRuntimeFrame.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStagePrepareAttemptCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStagePrepareSuccessCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageExecuteAttemptCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageExecuteSuccessCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareStage.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteStage.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareFrameSerial.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteFrameSerial.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastPrepareDrawCount.store(
      0u, std::memory_order_relaxed);
  g_nativeSemanticWorldStageLastExecuteDrawCount.store(
      0u, std::memory_order_relaxed);
  g_semanticSummaryRefreshFrameSerial.store(0u, std::memory_order_relaxed);
  g_semanticSummaryRefreshPublishRevision.store(0u,
                                                std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_shadowCadenceMutex);
    g_shadowCadenceSamples = {};
    g_shadowCadenceNextSerial = 0u;
    g_shadowCadenceSampleCountTotal = 0u;
    g_shadowCadenceWriteIndex = 0u;
    g_shadowCadenceSampleCount = 0u;
    g_shadowCadenceLastDynamicPoseSignature = 0u;
    g_shadowCadenceLastSceneFrameSerial = 0u;
    g_shadowCadenceSameDynamicPoseStreak = 0u;
    g_shadowCadenceSameDynamicPoseStreakMax = 0u;
    g_shadowCadenceSameSceneFrameStreak = 0u;
    g_shadowCadenceSameSceneFrameStreakMax = 0u;
    g_shadowCadenceShadowMapReuseStreak = 0u;
    g_shadowCadenceShadowMapReuseStreakMax = 0u;
  }
  {
    std::lock_guard<std::mutex> lock(g_shadowPoseFullTraceMutex);
    CloseShadowPoseFullTraceLocked();
    g_shadowPoseFullTraceStoppedByLimit = false;
    g_shadowPoseFullTraceFrameEventsWritten = 0u;
    g_shadowPoseFullTraceRecordEventsWritten = 0u;
    g_shadowPoseFullTraceStart = {};
    g_shadowPoseFullTracePath.clear();
    ++g_shadowPoseFullTraceEpoch;
  }
  shadow::NativeD3D9BackendRuntime::instance().reset();
}

bool AugmentShadowSemanticContext(
    dxvk::War3ShadowSemanticContext& semantic,
    const RenderObjectInfo* currentObj,
    const ShadowSemanticAugmentTrace* trace) {
  // 该函数可在每个 caster/draw 路径调用；默认 detail scope 为零开销空对象。
  auto augmentDetailScope = dxvk::war3::War3PerfMonitor::instance()
      .cpuDetailScope("Semantic/AugmentShadowContext");
  if (!dxvk::war3::internal::kShadowRuntimeBridgeEnabled)
    return false;
  const void* currentUnitPtr = currentObj != nullptr ? currentObj->unitPtr : nullptr;
  uint64_t bestPoseFrame = 0;
  uint64_t bestMatrixFrame = 0;

  const bool runtimePoseHooksActive = model::IsPoseHookEnabled();
  const bool needsRuntimePoseAugment =
      runtimePoseHooksActive &&
      (currentUnitPtr != nullptr ||
       semantic.objectKind == ObjectKind::Unit ||
       (currentObj != nullptr && currentObj->kind == ObjectKind::Unit) ||
       semantic.runtimeModelPtr != nullptr);
  const bool needsStableIdentityAugment =
      semantic.object == nullptr || semantic.sceneNode == nullptr ||
      semantic.worldObjectEntry == nullptr || semantic.jHandle == 0u ||
      semantic.rawcode == 0u || semantic.objectKind == ObjectKind::Unknown;
  const bool needsRuntimeRecovery =
      needsStableIdentityAugment &&
      (semantic.sceneNode != nullptr || semantic.worldObjectEntry != nullptr ||
       semantic.jHandle != 0u || currentUnitPtr != nullptr);
  // ModelInstance lookup only supplies these seven fields.  Runtime-pose
  // augmentation may still require ShadowObject/Pose below, but consulting
  // ModelInstance when every field is already populated is a read-only no-op.
  // Keep this test field-exact so incomplete first-seen objects retain the
  // historical recovery order and result.
  const bool needsModelInstanceRecovery =
      semantic.sceneNode == nullptr ||
      semantic.worldObjectEntry == nullptr ||
      semantic.runtimeModelPtr == nullptr ||
      semantic.modelResourcePtr == nullptr || semantic.modelKey == 0u ||
      semantic.jHandle == 0u || semantic.rawcode == 0u;
  const bool needsAnyRuntimeRegistryLookup =
      needsRuntimePoseAugment || needsStableIdentityAugment ||
      needsRuntimeRecovery;
  void* const semanticUnitPtr =
      needsAnyRuntimeRegistryLookup && semantic.object != nullptr
          ? semantic.object->unitPtr
          : nullptr;
  const bool hasRuntimeRegistryLookupKey =
      semantic.worldObjectEntry != nullptr || semantic.sceneNode != nullptr ||
      semanticUnitPtr != nullptr || currentUnitPtr != nullptr ||
      semantic.jHandle != 0u || semantic.runtimeModelPtr != nullptr;
  // A keyless lookup can only miss.  Keep it when the explicitly enabled TLS
  // cache is active, because that diagnostic mode observes negative-cache
  // insertions and hit/miss counters.
  const bool allowRuntimeRegistryLookup =
      needsAnyRuntimeRegistryLookup &&
      (hasRuntimeRegistryLookupKey || SemanticAugmentTlsCacheRuntime());

  if (trace != nullptr)
    trace->enter(ShadowSemanticAugmentTracePhase::ModelInstance);
  if (allowRuntimeRegistryLookup) {
    auto registryDetailScope = dxvk::war3::War3PerfMonitor::instance()
        .cpuDetailScope("Semantic/AugmentShadowContext/RegistryRecovery");
    if (needsModelInstanceRecovery) {
      auto instanceRegistryDetailScope =
          dxvk::war3::War3PerfMonitor::instance().cpuDetailScope(
              "Semantic/AugmentShadowContext/RegistryRecovery/ModelInstance");
      auto& instanceRegistry = model::ModelInstanceRegistry::instance();
      const auto applyInstanceRecord = [&](const auto& instanceRecord) {
        if (semantic.sceneNode == nullptr)
          semantic.sceneNode = instanceRecord.sceneNode;
        if (semantic.worldObjectEntry == nullptr)
          semantic.worldObjectEntry = instanceRecord.worldObjectEntry;
        if (semantic.runtimeModelPtr == nullptr)
          semantic.runtimeModelPtr = instanceRecord.runtimeModelPtr;
        if (semantic.modelResourcePtr == nullptr)
          semantic.modelResourcePtr = instanceRecord.modelResourcePtr;
        if (semantic.modelKey == 0u)
          semantic.modelKey = instanceRecord.modelKey;
        if (semantic.jHandle == 0u)
          semantic.jHandle = instanceRecord.jHandle;
        if (semantic.rawcode == 0u)
          semantic.rawcode = instanceRecord.rawcode;
      };

      if (SemanticAugmentBatchLookupRuntime()) {
        model::ModelInstanceAugmentView instanceView = {};
        bool instanceRecordHit = false;
        if (SemanticAugmentTlsCacheRuntime()) {
          const SemanticAugmentModelCacheKey cacheKey = {
              semantic.worldObjectEntry,
              semantic.sceneNode,
              semanticUnitPtr,
              const_cast<void*>(currentUnitPtr),
              semantic.jHandle,
          };
          instanceRecordHit = FindModelInstanceAugmentCached(
              instanceRegistry, cacheKey, instanceView);
        } else {
          instanceRecordHit = instanceRegistry.findFirstForAugmentView(
              semantic.worldObjectEntry, semantic.sceneNode, semanticUnitPtr,
              const_cast<void*>(currentUnitPtr), semantic.jHandle,
              instanceView);
        }
        if (instanceRecordHit) {
          applyInstanceRecord(instanceView);
        }
      } else {
        model::ModelInstanceRecord instanceRecord = {};
        bool instanceRecordHit = false;
        if (!instanceRecordHit && semantic.worldObjectEntry != nullptr)
          instanceRecordHit = instanceRegistry.findByWorldObjectEntry(
              semantic.worldObjectEntry, instanceRecord);
        if (!instanceRecordHit && semantic.sceneNode != nullptr)
          instanceRecordHit = instanceRegistry.findBySceneNode(
              semantic.sceneNode, instanceRecord);
        if (!instanceRecordHit && semanticUnitPtr != nullptr)
          instanceRecordHit = instanceRegistry.findByUnitPtr(
              semanticUnitPtr, instanceRecord);
        if (!instanceRecordHit && currentUnitPtr != nullptr)
          instanceRecordHit = instanceRegistry.findByUnitPtr(
              const_cast<void*>(currentUnitPtr), instanceRecord);
        if (!instanceRecordHit && semantic.jHandle != 0u)
          instanceRecordHit = instanceRegistry.findByHandle(
              semantic.jHandle, instanceRecord);

        if (instanceRecordHit)
          applyInstanceRecord(instanceRecord);
      }
    }

    if (trace != nullptr)
      trace->enter(ShadowSemanticAugmentTracePhase::ShadowObject);
    const bool needsShadowRegistryRecovery =
        needsRuntimePoseAugment || semantic.sceneNode == nullptr ||
        semantic.worldObjectEntry == nullptr || semantic.jHandle == 0u ||
        semantic.rawcode == 0u || semantic.objectKind == ObjectKind::Unknown;
    if (needsShadowRegistryRecovery) {
      auto shadowRegistryDetailScope =
          dxvk::war3::War3PerfMonitor::instance().cpuDetailScope(
              "Semantic/AugmentShadowContext/RegistryRecovery/ShadowObject");
      auto& shadowRegistry = ShadowObjectRegistry::instance();
      uint64_t shadowRegistryFrame = shadowRegistry.frameNumber();
      const auto applyShadowRecord = [&](const auto& shadowRecord) {
        if (semantic.sceneNode == nullptr)
          semantic.sceneNode = shadowRecord.sceneNode;
        if (semantic.worldObjectEntry == nullptr)
          semantic.worldObjectEntry = shadowRecord.worldObjectEntry;
        if (semantic.runtimeModelPtr == nullptr)
          semantic.runtimeModelPtr = shadowRecord.runtimeModelPtr;
        if (semantic.modelResourcePtr == nullptr)
          semantic.modelResourcePtr = shadowRecord.modelResourcePtr;
        if (semantic.jHandle == 0u)
          semantic.jHandle = shadowRecord.jHandle;
        if (semantic.rawcode == 0u)
          semantic.rawcode = shadowRecord.rawcode;
        if (semantic.modelKey == 0u)
          semantic.modelKey = shadowRecord.modelKey;
        if (semantic.objectKind == ObjectKind::Unknown)
          semantic.objectKind = shadowRecord.kind;
        if (IsRegistryFrameFresh(shadowRecord.lastSpriteFramePoseFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, shadowRecord.hasSpriteFrameTransform,
                                 shadowRecord.spriteFrameTransform,
                                 shadowRecord.scale, shadowRecord.height, true,
                                 shadowRecord.lastSpriteFramePoseFrame,
                                 bestPoseFrame);
        if (IsRegistryFrameFresh(shadowRecord.lastRootPoseFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, shadowRecord.hasWorldTransform,
                                 shadowRecord.worldTransform,
                                 shadowRecord.scale, shadowRecord.height, false,
                                 shadowRecord.lastRootPoseFrame, bestPoseFrame);
        if (IsRegistryFrameFresh(shadowRecord.lastMatrixPaletteFrame,
                                 shadowRegistryFrame))
          MaybeApplyPoseMatrices(semantic, shadowRecord.matrixCount,
                                 shadowRecord.matrixHash,
                                 shadowRecord.lastMatrixPaletteFrame,
                                 bestMatrixFrame);
      };

      if (SemanticAugmentBatchLookupRuntime() &&
          SemanticAugmentCompactShadowViewRuntime()) {
        ShadowObjectAugmentView shadowView = {};
        bool shadowRecordHit = false;
        if (SemanticAugmentTlsCacheRuntime()) {
          const SemanticAugmentShadowCacheKey cacheKey = {
              semantic.worldObjectEntry,
              semantic.sceneNode,
              semanticUnitPtr,
              const_cast<void*>(currentUnitPtr),
              semantic.jHandle,
              semantic.runtimeModelPtr,
          };
          shadowRecordHit = FindShadowObjectAugmentCached(
              shadowRegistry, cacheKey, shadowView, shadowRegistryFrame);
        } else {
          shadowRecordHit = shadowRegistry.findFirstForAugmentView(
              semantic.worldObjectEntry, semantic.sceneNode, semanticUnitPtr,
              const_cast<void*>(currentUnitPtr), semantic.jHandle,
              semantic.runtimeModelPtr, shadowView);
        }
        if (shadowRecordHit) {
          applyShadowRecord(shadowView);
        }
      } else {
        ShadowObjectRecord shadowRecord = {};
        bool shadowRecordHit = false;
        if (SemanticAugmentBatchLookupRuntime()) {
          shadowRecordHit = shadowRegistry.findFirstForAugment(
              semantic.worldObjectEntry, semantic.sceneNode, semanticUnitPtr,
              const_cast<void*>(currentUnitPtr), semantic.jHandle,
              semantic.runtimeModelPtr, shadowRecord);
        } else {
          if (!shadowRecordHit && semantic.worldObjectEntry != nullptr)
            shadowRecordHit = shadowRegistry.findByWorldObjectEntry(
                semantic.worldObjectEntry, shadowRecord);
          if (!shadowRecordHit && semantic.sceneNode != nullptr)
            shadowRecordHit = shadowRegistry.findBySceneNode(
                semantic.sceneNode, shadowRecord);
          if (!shadowRecordHit && semanticUnitPtr != nullptr)
            shadowRecordHit = shadowRegistry.findByUnitPtr(
                semanticUnitPtr, shadowRecord);
          if (!shadowRecordHit && currentUnitPtr != nullptr)
            shadowRecordHit = shadowRegistry.findByUnitPtr(
                const_cast<void*>(currentUnitPtr), shadowRecord);
          if (!shadowRecordHit && semantic.jHandle != 0u)
            shadowRecordHit = shadowRegistry.findByHandle(
                semantic.jHandle, shadowRecord);
          if (!shadowRecordHit && semantic.runtimeModelPtr != nullptr)
            shadowRecordHit = shadowRegistry.findByRuntimeModel(
                semantic.runtimeModelPtr, shadowRecord);
        }

        if (shadowRecordHit)
          applyShadowRecord(shadowRecord);
      }
    }

    if (trace != nullptr)
      trace->enter(ShadowSemanticAugmentTracePhase::Pose);
    if (needsRuntimePoseAugment) {
      auto poseRegistryDetailScope =
          dxvk::war3::War3PerfMonitor::instance().cpuDetailScope(
              "Semantic/AugmentShadowContext/RegistryRecovery/Pose");
      model::PoseAugmentView poseRecord = {};
      bool poseRecordHit = false;
      auto& poseRegistry = model::PoseRegistry::instance();
      const uint64_t poseRegistryFrame = poseRegistry.frameNumber();
      if (semantic.runtimeModelPtr != nullptr)
        poseRecordHit = poseRegistry.findByRuntimeModelAugment(
            semantic.runtimeModelPtr, poseRecord);
      if (!poseRecordHit && semantic.sceneNode != nullptr)
        poseRecordHit = poseRegistry.findBySceneNodeAugment(semantic.sceneNode,
                                                            poseRecord);
      if (!poseRecordHit && semantic.object != nullptr &&
          semantic.object->unitPtr != nullptr) {
        poseRecordHit = poseRegistry.findByUnitPtrAugment(
            semantic.object->unitPtr, poseRecord);
      }
      if (!poseRecordHit && currentUnitPtr != nullptr)
        poseRecordHit = poseRegistry.findByUnitPtrAugment(
            const_cast<void*>(currentUnitPtr), poseRecord);

      if (poseRecordHit) {
        if (IsRegistryFrameFresh(poseRecord.lastSpriteFramePoseFrame,
                                 poseRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, poseRecord.hasSpriteFrameTransform,
                                 poseRecord.spriteFrameTransform,
                                 poseRecord.scale, poseRecord.height, true,
                                 poseRecord.lastSpriteFramePoseFrame,
                                 bestPoseFrame);
        if (IsRegistryFrameFresh(poseRecord.lastRootPoseFrame, poseRegistryFrame))
          MaybeApplyPoseSnapshot(semantic, poseRecord.hasWorldTransform,
                                 poseRecord.worldTransform, poseRecord.scale,
                                 poseRecord.height, false,
                                 poseRecord.lastRootPoseFrame, bestPoseFrame);
        if (IsRegistryFrameFresh(poseRecord.lastMatrixPaletteFrame,
                                 poseRegistryFrame))
          MaybeApplyPoseMatrices(semantic, poseRecord.matrixCount,
                                 poseRecord.matrixHash,
                                 poseRecord.lastMatrixPaletteFrame,
                                 bestMatrixFrame);
      }
    }
  }

  if (trace != nullptr)
    trace->enter(ShadowSemanticAugmentTracePhase::RenderObject);
  if (semantic.object == nullptr) {
    auto renderObjectDetailScope =
        dxvk::war3::War3PerfMonitor::instance().cpuDetailScope(
            "Semantic/AugmentShadowContext/RenderObjectRecovery");
    auto& registry = RenderObjectRegistry::instance();
    const RenderObjectInfo* object = nullptr;
    if (semantic.sceneNode != nullptr)
      object = registry.findBySceneNode(semantic.sceneNode);
    if (object == nullptr && semantic.worldObjectEntry != nullptr)
      object = registry.findByEntry(semantic.worldObjectEntry);
    if (object == nullptr && semantic.jHandle != 0u)
      object = registry.findByHandle(semantic.jHandle);
    MergeRenderObject(semantic, object);
  }

  if (trace != nullptr)
    trace->enter(ShadowSemanticAugmentTracePhase::Finalize);
  semantic.pathBlocker =
      semantic.pathBlocker ||
      dxvk::war3::internal::IsPathBlockerFourCc(semantic.rawcode);

  const bool hasMeaningfulContext =
      semantic.sceneNode != nullptr || semantic.worldObjectEntry != nullptr ||
      semantic.jHandle != 0u || currentUnitPtr != nullptr ||
      semantic.runtimeModelPtr != nullptr || semantic.object != nullptr;
  const bool bridgeReady = semantic.HasStableIdentity() || semantic.hasPoseTransform ||
                           semantic.poseMatrixCount != 0u;

  // 某个对象第一次进入视野时，如果这帧仍拿不到稳定身份/姿态，
  // 不要等到全局周期刷新再补票。这里对“当前缺失对象”
  // 触发一个很短的 repair burst，但带冷却，避免在超大地图上
  // 长时间把全量对象身份桥常驻打开。
  if (!bridgeReady && hasMeaningfulContext) {
    const uint64_t nowFrame = CurrentRuntimeBridgeFrame();
    RequestShadowBridgeRepairBurst(nowFrame);
  }

  return bridgeReady;
}

} // namespace dxvk::war3::render
