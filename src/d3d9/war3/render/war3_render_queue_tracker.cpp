#include "war3_render_queue_tracker.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "war3_render_objects.h"
#include "war3_shadow_runtime_bridge.h"
#include "war3_render_state.h"
#include "../../../util/util_time.h"
#include <cstdint>
#include <limits>
#include <mutex>

namespace dxvk::war3::render {

namespace {
constexpr size_t kElementStride = 20;
constexpr size_t kElementLayerIndexOffset = 0x08;
constexpr size_t kMaxProbes =
    kWorldObjectsPhase1GetTagStageMaxProbes; // 线性探测最大步长
constexpr uint8_t kInfoStateUnknown = 0;
constexpr uint8_t kInfoStateHit = 1;
constexpr uint8_t kInfoStateMiss = 2;

size_t HashElementKey(void *element) {
  uintptr_t v = reinterpret_cast<uintptr_t>(element);
  // 高性能扰动哈希
  v ^= v >> 16;
  v *= 0x85ebca6b;
  v ^= v >> 13;
  v *= 0xc2b2ae35;
  v ^= v >> 16;
  return static_cast<size_t>(v);
}

bool ReadSceneNodeFromElement(void *element, void *&outSceneNode) {
  outSceneNode = nullptr;
  if (!element)
    return false;
  void *sceneNode = nullptr;
  if (!dxvk::war3::SafeReadPtrFast(element, 0x14, sceneNode) || !sceneNode)
    return false;
  outSceneNode = sceneNode;
  return true;
}

const dxvk::war3::render::RenderObjectInfo *
ResolveInfoByElement(void *element, void *sceneNodeHint) {
  void *sceneNode = sceneNodeHint;
  if (!sceneNode && !ReadSceneNodeFromElement(element, sceneNode))
    return nullptr;
  if (!sceneNode)
    return nullptr;
  return dxvk::war3::render::RenderObjectRegistry::instance().findBySceneNode(
      sceneNode);
}
} // namespace

RenderQueueTracker::RenderQueueTracker() {
  uint32_t capacity =
      dxvk::war3::internal::kNativeRenderQueueFastTrackerCapacity;
  if (capacity == 0 || (capacity & (capacity - 1u)) != 0u) {
    capacity = 1u << 18; // 默认 26 万条目
  }
  m_entries.resize(capacity);
  m_mask = capacity - 1u;
}

RenderQueueTracker &RenderQueueTracker::instance() {
  static RenderQueueTracker *s_instance = new RenderQueueTracker();
  return *s_instance;
}

void RenderQueueTracker::SetGlobals(uint32_t *numElementsPtr,
                                    void **batchArrayPtr) {
  m_numOfElementsPtr = numElementsPtr;
  m_batchArrayPtr = batchArrayPtr;
  m_globalsValid.store(m_numOfElementsPtr != nullptr &&
                           m_batchArrayPtr != nullptr,
                       std::memory_order_relaxed);
}

void RenderQueueTracker::TrackNewBatches(uint32_t before, int groupIdx) {
  if (!m_globalsValid.load(std::memory_order_relaxed) || !m_numOfElementsPtr ||
      !m_batchArrayPtr)
    return;

  uint32_t after = *m_numOfElementsPtr;
  void *batchArray = *m_batchArrayPtr;

  War3BatchTag tag = War3BatchTag::Unknown;
  int stage = -1;
  switch (groupIdx) {
  case 0:
    tag = War3BatchTag::WorldObjects;
    stage = 11;
    break;
  case 1:
    tag = War3BatchTag::SelectionOverlay;
    stage = 12;
    break;
  case 2:
    tag = War3BatchTag::Decorations;
    stage = 13;
    break;
  default:
    break;
  }

  if (batchArray && after > before && tag != War3BatchTag::Unknown) {
    MarkTagStage(batchArray, before, after, tag, stage);
  }
}

void RenderQueueTracker::Reset() {
  // 逻辑增加 epoch 即可使旧条目失效，极快
  uint32_t oldEpoch = m_epoch.load(std::memory_order_relaxed);
  uint32_t nextEpoch = (oldEpoch + 1) & 0xFFFFu;
  if (nextEpoch == 0)
    nextEpoch = 1;
  m_epoch.store(nextEpoch, std::memory_order_relaxed);

  if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
    m_statFrame++;
    if (m_statFrame %
            dxvk::war3::internal::kNativeRenderQueueTrackerStatsInterval ==
        0) {
      uint64_t fHit = m_fastHit.exchange(0);
      uint64_t fMiss = m_fastMiss.exchange(0);
      uint64_t iHit = m_infoHit.exchange(0);
      uint64_t iMiss = m_infoMiss.exchange(0);
      uint64_t iFill = m_infoFill.exchange(0);
      const RenderQueueSemanticConflictStats semanticStats =
          GetSemanticConflictStats();
      war3dbg::Print("DXVK War3Hook: LockFree Tracker stats hit=%llu miss=%llu "
                     "infoHit=%llu infoMiss=%llu infoFill=%llu "
                     "semanticConflict=%llu tagConflict=%llu "
                     "stageConflict=%llu layerConflict=%llu\n",
                     (unsigned long long)fHit, (unsigned long long)fMiss,
                     (unsigned long long)iHit, (unsigned long long)iMiss,
                     (unsigned long long)iFill,
                     (unsigned long long)semanticStats.conflictingEntries,
                     (unsigned long long)semanticStats.tagConflicts,
                     (unsigned long long)semanticStats.stageConflicts,
                     (unsigned long long)semanticStats.layerConflicts);
    }
  }
}

void RenderQueueTracker::MarkTagStage(void *batchArray, uint32_t before,
                                      uint32_t after, War3BatchTag tag,
                                      int stage) {
  if (!batchArray || after <= before)
    return;

  auto *base = reinterpret_cast<std::uint8_t *>(batchArray);
  uint32_t currentEpoch = m_epoch.load(std::memory_order_relaxed);

  auto addSemanticConflicts = [&](AtomicEntry &entry,
                                  uint32_t conflictMask) {
    if (conflictMask == kRenderQueueSemanticConflictNone)
      return;

    uint32_t observed =
        entry.semanticConflictStateEpoch.load(std::memory_order_relaxed);
    uint32_t oldMask = 0;
    uint32_t conflictEpoch = 0;
    uint32_t newMask = 0;
    for (;;) {
      UnpackSemanticConflictState(observed, oldMask, conflictEpoch);
      if (conflictEpoch != currentEpoch)
        oldMask = kRenderQueueSemanticConflictNone;
      newMask = oldMask | conflictMask;
      const uint32_t desired =
          PackSemanticConflictState(newMask, currentEpoch);
      if (entry.semanticConflictStateEpoch.compare_exchange_weak(
              observed, desired, std::memory_order_release,
              std::memory_order_relaxed)) {
        break;
      }
    }

    const uint32_t addedMask = newMask & ~oldMask;
    if (addedMask == kRenderQueueSemanticConflictNone)
      return;
    if (oldMask == kRenderQueueSemanticConflictNone)
      m_semanticConflictingEntries.fetch_add(1, std::memory_order_relaxed);
    if ((addedMask & kRenderQueueSemanticConflictTag) != 0u)
      m_semanticTagConflicts.fetch_add(1, std::memory_order_relaxed);
    if ((addedMask & kRenderQueueSemanticConflictStage) != 0u)
      m_semanticStageConflicts.fetch_add(1, std::memory_order_relaxed);
    if ((addedMask & kRenderQueueSemanticConflictLayer) != 0u)
      m_semanticLayerConflicts.fetch_add(1, std::memory_order_relaxed);
  };

  for (uint32_t i = before; i < after; ++i) {
    auto *record = base + i * kElementStride;
    void *element = *reinterpret_cast<void **>(record);
    if (!element)
      continue;
    const uint32_t layerIndex = *reinterpret_cast<const uint32_t *>(
        record + kElementLayerIndexOffset);

    size_t h = HashElementKey(element) & m_mask;
    for (size_t p = 0; p < kMaxProbes; ++p) {
      auto &entry = m_entries[(h + p) & m_mask];
      void *currentKey = entry.key.load(std::memory_order_relaxed);

      if (currentKey == element) {
        uint32_t oldEpoch = 0u;
        War3BatchTag oldTag = War3BatchTag::Unknown;
        int oldStage = -1;
        const uint32_t oldState =
            entry.state.load(std::memory_order_acquire);
        UnpackState(oldState, oldTag, oldStage, oldEpoch);

        if (oldEpoch != currentEpoch) {
          entry.layerIndex.store(layerIndex, std::memory_order_relaxed);
          entry.semanticConflictStateEpoch.store(
              PackSemanticConflictState(kRenderQueueSemanticConflictNone,
                                        currentEpoch),
              std::memory_order_relaxed);
          entry.state.store(PackState(tag, stage, currentEpoch),
                            std::memory_order_release);
          break;
        }

        // Nested producers may revisit one renderablePart before Reset. Keep
        // the first known tuple canonical and make any disagreement sticky.
        War3BatchTag mergedTag = oldTag;
        int mergedStage = oldStage;
        uint32_t mergedLayer =
            entry.layerIndex.load(std::memory_order_relaxed);
        uint32_t conflictMask = kRenderQueueSemanticConflictNone;

        if (tag != War3BatchTag::Unknown) {
          if (oldTag == War3BatchTag::Unknown)
            mergedTag = tag;
          else if (oldTag != tag)
            conflictMask |= kRenderQueueSemanticConflictTag;
        }
        if (stage >= 0) {
          if (oldStage < 0)
            mergedStage = stage;
          else if (oldStage != stage)
            conflictMask |= kRenderQueueSemanticConflictStage;
        }
        if (layerIndex != kRenderQueueUnknownLayerIndex) {
          if (mergedLayer == kRenderQueueUnknownLayerIndex)
            mergedLayer = layerIndex;
          else if (mergedLayer != layerIndex)
            conflictMask |= kRenderQueueSemanticConflictLayer;
        }

        entry.layerIndex.store(mergedLayer, std::memory_order_relaxed);
        addSemanticConflicts(entry, conflictMask);
        if (mergedTag != oldTag || mergedStage != oldStage) {
          entry.state.store(PackState(mergedTag, mergedStage, currentEpoch),
                            std::memory_order_release);
        }
        break;
      }

      bool reclaimable = currentKey == nullptr;
      if (!reclaimable) {
        const uint32_t entryState =
            entry.state.load(std::memory_order_relaxed);
        reclaimable = ((entryState >> 16) & 0xFFFFu) != currentEpoch;
      }
      if (reclaimable) {
        void *expectedKey = currentKey;
        if (entry.key.compare_exchange_strong(expectedKey, element,
                                              std::memory_order_relaxed)) {
          entry.layerIndex.store(layerIndex, std::memory_order_relaxed);
          entry.semanticConflictStateEpoch.store(
              PackSemanticConflictState(kRenderQueueSemanticConflictNone,
                                        currentEpoch),
              std::memory_order_relaxed);
          entry.state.store(PackState(tag, stage, currentEpoch),
                            std::memory_order_release);
          break;
        }
      }
    }
  }
}

void RenderQueueTracker::MarkTags(void *batchArray, uint32_t before,
                                  uint32_t after, War3BatchTag tag) {
  MarkTagStage(batchArray, before, after, tag, -1);
}

void RenderQueueTracker::MarkStages(void *batchArray, uint32_t before,
                                    uint32_t after, int stage) {
  MarkTagStage(batchArray, before, after, War3BatchTag::Unknown, stage);
}

bool RenderQueueTracker::GetTagStage(void *element, War3BatchTag &outTag,
                                     int &outStage) const {
  const uint64_t periodicEventSequence =
      CurrentWorldObjectsPhase1PurePeriodicDispatchSequence();
  const bool periodicCapture = periodicEventSequence != 0u;
  const int64_t begin = periodicCapture
      ? dxvk::high_resolution_clock::get_counter()
      : 0;
  RenderQueueSemanticState state = {};
  uint32_t probes = 0u;
  const bool hit = periodicCapture
      ? GetSemanticStateWithProbeCount(element, state, probes)
      : GetSemanticState(element, state);
  if (periodicCapture) {
    const int64_t end = dxvk::high_resolution_clock::get_counter();
    RecordWorldObjectsPhase1PeriodicGetTagStage(
        periodicEventSequence, hit, hit && state.HasConflict(), probes,
        end >= begin ? uint64_t(end - begin) : 0u);
  }
  if (!hit)
    return false;
  outTag = state.tag;
  outStage = state.stage;
  return true;
}

bool RenderQueueTracker::GetSemanticState(
    void *element, RenderQueueSemanticState &outState) const {
  outState = {};
  if (!element)
    return false;

  size_t h = HashElementKey(element) & m_mask;
  uint32_t currentEpoch = m_epoch.load(std::memory_order_relaxed);

  for (size_t p = 0; p < kMaxProbes; ++p) {
    const auto &entry = m_entries[(h + p) & m_mask];
    void *k = entry.key.load(std::memory_order_relaxed);
    if (k == element) {
      for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
        const uint32_t stateBefore =
            entry.state.load(std::memory_order_acquire);
        War3BatchTag tag = War3BatchTag::Unknown;
        int stage = -1;
        uint32_t epoch = 0;
        UnpackState(stateBefore, tag, stage, epoch);
        if (epoch != currentEpoch)
          break;

        const uint32_t layerIndex =
            entry.layerIndex.load(std::memory_order_relaxed);
        const uint32_t conflictState =
            entry.semanticConflictStateEpoch.load(std::memory_order_acquire);
        const uint32_t stateAfter =
            entry.state.load(std::memory_order_acquire);
        if (stateBefore != stateAfter)
          continue;

        uint32_t conflictMask = kRenderQueueSemanticConflictNone;
        uint32_t conflictEpoch = 0;
        UnpackSemanticConflictState(conflictState, conflictMask,
                                    conflictEpoch);
        if (conflictEpoch != currentEpoch ||
            m_epoch.load(std::memory_order_relaxed) != currentEpoch ||
            entry.key.load(std::memory_order_relaxed) != element) {
          break;
        }

        outState.tag = tag;
        outState.stage = stage;
        outState.layerIndex = layerIndex;
        outState.conflictMask = conflictMask;
        outState.epoch = epoch;
        if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
          m_fastHit.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
      }
    }
    if (k == nullptr)
      break;
  }

  if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
    m_fastMiss.fetch_add(1, std::memory_order_relaxed);
  }
  return false;
}

bool RenderQueueTracker::GetSemanticStateWithProbeCount(
    void *element, RenderQueueSemanticState &outState,
    uint32_t &outProbeCount) const {
  // This is intentionally a capture-only mirror of GetSemanticState.  The
  // production lookup above stays branch-free: it must not pay an optional
  // probe-counter test on every GPU-skin semantic lookup merely to diagnose
  // one pure periodic frame out of 300.
  outState = {};
  outProbeCount = 0u;
  if (!element)
    return false;

  size_t h = HashElementKey(element) & m_mask;
  uint32_t currentEpoch = m_epoch.load(std::memory_order_relaxed);

  for (size_t p = 0; p < kMaxProbes; ++p) {
    outProbeCount += 1u;
    const auto &entry = m_entries[(h + p) & m_mask];
    void *k = entry.key.load(std::memory_order_relaxed);
    if (k == element) {
      for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
        const uint32_t stateBefore =
            entry.state.load(std::memory_order_acquire);
        War3BatchTag tag = War3BatchTag::Unknown;
        int stage = -1;
        uint32_t epoch = 0;
        UnpackState(stateBefore, tag, stage, epoch);
        if (epoch != currentEpoch)
          break;

        const uint32_t layerIndex =
            entry.layerIndex.load(std::memory_order_relaxed);
        const uint32_t conflictState =
            entry.semanticConflictStateEpoch.load(std::memory_order_acquire);
        const uint32_t stateAfter =
            entry.state.load(std::memory_order_acquire);
        if (stateBefore != stateAfter)
          continue;

        uint32_t conflictMask = kRenderQueueSemanticConflictNone;
        uint32_t conflictEpoch = 0;
        UnpackSemanticConflictState(conflictState, conflictMask,
                                    conflictEpoch);
        if (conflictEpoch != currentEpoch ||
            m_epoch.load(std::memory_order_relaxed) != currentEpoch ||
            entry.key.load(std::memory_order_relaxed) != element) {
          break;
        }

        outState.tag = tag;
        outState.stage = stage;
        outState.layerIndex = layerIndex;
        outState.conflictMask = conflictMask;
        outState.epoch = epoch;
        if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
          m_fastHit.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
      }
    }
    if (k == nullptr)
      break;
  }

  if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
    m_fastMiss.fetch_add(1, std::memory_order_relaxed);
  }
  return false;
}

RenderQueueSemanticConflictStats
RenderQueueTracker::GetSemanticConflictStats() const {
  RenderQueueSemanticConflictStats stats = {};
  stats.conflictingEntries =
      m_semanticConflictingEntries.load(std::memory_order_relaxed);
  stats.tagConflicts =
      m_semanticTagConflicts.load(std::memory_order_relaxed);
  stats.stageConflicts =
      m_semanticStageConflicts.load(std::memory_order_relaxed);
  stats.layerConflicts =
      m_semanticLayerConflicts.load(std::memory_order_relaxed);
  return stats;
}

bool RenderQueueTracker::GetTag(void *element, War3BatchTag &outTag) const {
  int stage;
  return GetTagStage(element, outTag, stage);
}

bool RenderQueueTracker::GetStage(void *element, int &outStage) const {
  War3BatchTag tag;
  return GetTagStage(element, tag, outStage);
}

bool RenderQueueTracker::GetCachedObjectIdentity(
    void *element, RenderObjectIdentitySnapshot &outIdentity) const {
  outIdentity = {};
  if (!dxvk::war3::internal::kNativeRenderQueueCacheObjectInfoEnabled ||
      !element)
    return false;

  size_t h = HashElementKey(element) & m_mask;
  uint32_t currentEpoch = m_epoch.load(std::memory_order_relaxed);

  for (size_t p = 0; p < kMaxProbes; ++p) {
    const auto &entry = m_entries[(h + p) & m_mask];
    void *k = entry.key.load(std::memory_order_relaxed);
    if (k == element) {
      const uint32_t ie =
          entry.identityStateEpoch.load(std::memory_order_acquire);
      const uint32_t state = ie & 0xFFu;
      const uint32_t epoch = ie >> 8;
      if (epoch == currentEpoch && state == kInfoStateHit) {
        outIdentity = entry.identity;
        return outIdentity.HasStableIdentity();
      }
      return false;
    }
    if (k == nullptr)
      break;
  }

  return false;
}

bool RenderQueueTracker::GetCachedObjectInfo(void *element, void *sceneNodeHint,
                                             const RenderObjectInfo *&outInfo,
                                             uint32_t &outJHandle) {
  outInfo = nullptr;
  outJHandle = 0;
  if (!dxvk::war3::internal::kNativeRenderQueueCacheObjectInfoEnabled ||
      !element)
    return false;

  size_t h = HashElementKey(element) & m_mask;
  uint32_t currentEpoch = m_epoch.load(std::memory_order_relaxed);

  // 探测主要条目
  AtomicEntry *target = nullptr;
  for (size_t p = 0; p < kMaxProbes; ++p) {
    auto &entry = m_entries[(h + p) & m_mask];
    if (entry.key.load(std::memory_order_relaxed) == element) {
      target = &entry;
      break;
    }
    if (entry.key.load(std::memory_order_relaxed) == nullptr)
      break;
  }

  if (!target) {
    if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
      m_infoMiss.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  }

  uint32_t ie = target->infoStateEpoch.load(std::memory_order_relaxed);
  uint32_t state = ie & 0xFFu;
  uint32_t epoch = ie >> 8;

  if (epoch == currentEpoch) {
    if (state == kInfoStateHit) {
      outInfo = target->info.load(std::memory_order_relaxed);
      outJHandle = target->jHandle.load(std::memory_order_relaxed);
      if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
        m_infoHit.fetch_add(1, std::memory_order_relaxed);
      }
      return true;
    } else if (state == kInfoStateMiss) {
      if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
        m_infoMiss.fetch_add(1, std::memory_order_relaxed);
      }
      return false;
    }
  }

  // 缓存失效或缺失，执行昂贵的解析
  const auto *info = ResolveInfoByElement(element, sceneNodeHint);
  if (info) {
    target->info.store(info, std::memory_order_relaxed);
    target->jHandle.store(info->jHandle, std::memory_order_relaxed);
    target->infoStateEpoch.store(kInfoStateHit | (currentEpoch << 8),
                                 std::memory_order_relaxed);
    outInfo = info;
    outJHandle = info->jHandle;
    if (dxvk::war3::internal::kNativeRenderQueueTrackerStatsEnabled) {
      m_infoFill.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  } else {
    target->infoStateEpoch.store(kInfoStateMiss | (currentEpoch << 8),
                                 std::memory_order_relaxed);
    m_infoMiss.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
}

void RenderQueueTracker::PrimeCachedObjectIdentities(
    void *batchArray, uint32_t before, uint32_t after,
    const RenderObjectIdentitySnapshot &identity) {
  if (!batchArray || after <= before || !identity.HasStableIdentity())
    return;

  auto *base = reinterpret_cast<std::uint8_t *>(batchArray);
  uint32_t currentEpoch = m_epoch.load(std::memory_order_relaxed);

  for (uint32_t i = before; i < after; ++i) {
    void *element = *reinterpret_cast<void **>(base + i * kElementStride);
    if (!element)
      continue;

    size_t h = HashElementKey(element) & m_mask;
    for (size_t p = 0; p < kMaxProbes; ++p) {
      auto &entry = m_entries[(h + p) & m_mask];
      void *expectedKey = nullptr;

      if (entry.key.compare_exchange_strong(expectedKey, element,
                                            std::memory_order_relaxed) ||
          expectedKey == element) {
        entry.jHandle.store(identity.jHandle, std::memory_order_relaxed);
        entry.identity = identity;
        entry.identityStateEpoch.store(kInfoStateHit | (currentEpoch << 8),
                                       std::memory_order_release);
        break;
      }

      uint32_t entryState = entry.state.load(std::memory_order_relaxed);
      uint32_t entryEpoch = (entryState >> 16) & 0xFFFFu;
      if (entryEpoch != currentEpoch) {
        expectedKey = entry.key.load(std::memory_order_relaxed);
        if (entry.key.compare_exchange_strong(expectedKey, element,
                                              std::memory_order_relaxed)) {
          entry.jHandle.store(identity.jHandle, std::memory_order_relaxed);
          entry.identity = identity;
          entry.identityStateEpoch.store(kInfoStateHit | (currentEpoch << 8),
                                         std::memory_order_release);
          break;
        }
      }
    }
  }
}

} // namespace dxvk::war3::render
