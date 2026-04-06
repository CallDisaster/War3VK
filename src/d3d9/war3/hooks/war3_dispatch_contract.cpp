#include "war3_dispatch_contract.h"

#include "../../d3d9_war3_debug.h"

#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../render/war3_render_queue_tracker.h"
#include "../state/war3_render_state.h"

#include <algorithm>
#include <atomic>

namespace dxvk::war3::hooks {

static_assert((kDispatchTagStageCacheSlotCount &
               (kDispatchTagStageCacheSlotCount - 1)) == 0,
              "DispatchTagStageCache slot count must be power-of-two");

static thread_local DispatchLocalMergeState t_dispatchLocalMergeState;
static thread_local DispatchTagStageCacheState t_dispatchTagStageCacheState;

static std::atomic<uint64_t> s_dispatchTagStageCacheCalls{0};
static std::atomic<uint64_t> s_dispatchTagStageCacheHits{0};
static std::atomic<uint64_t> s_dispatchTagStageCacheMisses{0};
static std::atomic<uint64_t> s_dispatchTagStageCacheLastLoggedCall{0};

DispatchLocalMergeState &GetDispatchLocalMergeState() {
  return t_dispatchLocalMergeState;
}

void ResetDispatchLocalMergeState(DispatchLocalMergeState &state) {
  state.active = false;
  state.isType3 = false;
  state.renderablePart = nullptr;
  state.tag = War3BatchTag::Unknown;
  state.stage = -1;
}

void ResetDispatchLocalMergeState() {
  ResetDispatchLocalMergeState(t_dispatchLocalMergeState);
}

DispatchTagStageCacheState &GetDispatchTagStageCacheState() {
  return t_dispatchTagStageCacheState;
}

static inline uint32_t HashDispatchTagStageCacheKey(const void *key) {
  const auto value = reinterpret_cast<uintptr_t>(key);
  return static_cast<uint32_t>((value >> 4) &
                               (kDispatchTagStageCacheSlotCount - 1));
}

void ResetDispatchTagStageCacheState(DispatchTagStageCacheState &state) {
  state.clock = 0;
  state.hotRenderableValid = false;
  state.hotRenderableResolved = false;
  state.hotRenderablePart = nullptr;
  state.hotRenderableTag = War3BatchTag::Unknown;
  state.hotRenderableStage = -1;
  state.hotSceneValid = false;
  state.hotSceneResolved = false;
  state.hotSceneNode = nullptr;
  state.hotSceneTag = War3BatchTag::Unknown;
  state.hotSceneStage = -1;
  for (auto &slot : state.slots) {
    slot = DispatchTagStageCacheEntry{};
  }
}

void ResetDispatchTagStageCacheState() {
  ResetDispatchTagStageCacheState(t_dispatchTagStageCacheState);
}

static void MaybeLogDispatchTagStageCacheStats() {
  if constexpr (!dxvk::war3::internal::kNativeDispatchTagStageCacheEnabled ||
                !dxvk::war3::internal::kNativeDispatchTagStageCacheStatsLogging) {
    return;
  }

  constexpr uint64_t kInterval =
      dxvk::war3::internal::kNativeDispatchTagStageCacheStatsInterval;
  if (kInterval == 0)
    return;

  const uint64_t calls =
      s_dispatchTagStageCacheCalls.load(std::memory_order_relaxed);
  if (calls == 0 || (calls % kInterval) != 0)
    return;

  const uint64_t lastLogged = s_dispatchTagStageCacheLastLoggedCall.exchange(
      calls, std::memory_order_relaxed);
  if (lastLogged == calls)
    return;

  const uint64_t hits =
      s_dispatchTagStageCacheHits.load(std::memory_order_relaxed);
  const uint64_t misses =
      s_dispatchTagStageCacheMisses.load(std::memory_order_relaxed);
  const double hitPct =
      calls > 0
          ? (100.0 * static_cast<double>(hits) / static_cast<double>(calls))
          : 0.0;

  war3dbg::Print(
      "DXVK War3Hook: DispatchTagStageCache calls=%llu hit=%llu miss=%llu hitPct=%.2f\n",
      static_cast<unsigned long long>(calls),
      static_cast<unsigned long long>(hits),
      static_cast<unsigned long long>(misses), hitPct);
}

static inline void TrackDispatchTagStageCacheCall() {
  if constexpr (dxvk::war3::internal::kNativeDispatchTagStageCacheStatsLogging) {
    s_dispatchTagStageCacheCalls.fetch_add(1, std::memory_order_relaxed);
  }
}

static inline void TrackDispatchTagStageCacheHit() {
  if constexpr (dxvk::war3::internal::kNativeDispatchTagStageCacheStatsLogging) {
    s_dispatchTagStageCacheHits.fetch_add(1, std::memory_order_relaxed);
    MaybeLogDispatchTagStageCacheStats();
  }
}

static inline void TrackDispatchTagStageCacheMiss() {
  if constexpr (dxvk::war3::internal::kNativeDispatchTagStageCacheStatsLogging) {
    s_dispatchTagStageCacheMisses.fetch_add(1, std::memory_order_relaxed);
    MaybeLogDispatchTagStageCacheStats();
  }
}

static void *TryReadSceneNodeFromRenderablePart(void *renderablePart) {
  if (!renderablePart)
    return nullptr;

  const auto *base = reinterpret_cast<const std::uint8_t *>(renderablePart);
  const auto *slot = base + 0x14;

  if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
    return *reinterpret_cast<void *const *>(slot);
  }

  if (!dxvk::war3::IsReadableRange(slot, sizeof(void *)))
    return nullptr;
  return *reinterpret_cast<void *const *>(slot);
}

War3DispatchQueryResult QueryTagStageCached(
    dxvk::war3::render::RenderQueueTracker &tracker,
    const War3DispatchQueryRequest &request) {
  War3DispatchQueryResult result = {};
  if (!request.renderablePart)
    return result;

  if constexpr (!dxvk::war3::internal::kNativeDispatchTagStageCacheEnabled) {
    result.resolved = tracker.GetTagStage(request.renderablePart, result.tag,
                                          result.stage);
    result.cacheSource = War3DispatchCacheSource::Tracker;
    return result;
  }

  auto &cache = GetDispatchTagStageCacheState();
  auto touchSlot = [&](DispatchTagStageCacheEntry &slot) {
    ++cache.clock;
    if (cache.clock == 0) {
      cache.clock = 1;
      for (auto &e : cache.slots)
        e.stamp >>= 1;
    }
    slot.stamp = cache.clock;
  };
  auto updateHotRenderable = [&](bool resolved, War3BatchTag tag, int stage) {
    cache.hotRenderableValid = true;
    cache.hotRenderableResolved = resolved;
    cache.hotRenderablePart = request.renderablePart;
    cache.hotRenderableTag = tag;
    cache.hotRenderableStage = stage;
  };
  auto updateHotScene = [&](const void *sceneNode, bool resolved,
                            War3BatchTag tag, int stage) {
    if (!sceneNode) {
      cache.hotSceneValid = false;
      cache.hotSceneResolved = false;
      cache.hotSceneNode = nullptr;
      cache.hotSceneTag = War3BatchTag::Unknown;
      cache.hotSceneStage = -1;
      return;
    }
    cache.hotSceneValid = true;
    cache.hotSceneResolved = resolved;
    cache.hotSceneNode = const_cast<void *>(sceneNode);
    cache.hotSceneTag = tag;
    cache.hotSceneStage = stage;
  };

  TrackDispatchTagStageCacheCall();

  if (cache.hotRenderableValid &&
      cache.hotRenderablePart == request.renderablePart) {
    result.resolved = cache.hotRenderableResolved;
    result.tag = cache.hotRenderableTag;
    result.stage = cache.hotRenderableStage;
    result.cacheSource = War3DispatchCacheSource::HotRenderable;
    TrackDispatchTagStageCacheHit();
    return result;
  }

  const uint32_t renderableSlotIdx =
      HashDispatchTagStageCacheKey(request.renderablePart);
  auto &renderableSlot = cache.slots[renderableSlotIdx];
  if (renderableSlot.valid &&
      renderableSlot.renderablePart == request.renderablePart) {
    result.resolved = renderableSlot.resolved;
    result.tag = renderableSlot.tag;
    result.stage = renderableSlot.stage;
    result.cacheSource = War3DispatchCacheSource::RenderableSlot;
    touchSlot(renderableSlot);
    updateHotRenderable(result.resolved, result.tag, result.stage);
    TrackDispatchTagStageCacheHit();
    return result;
  }

  for (auto &slot : cache.slots) {
    if (&slot == &renderableSlot)
      continue;
    if (!slot.valid || slot.renderablePart != request.renderablePart)
      continue;
    result.resolved = slot.resolved;
    result.tag = slot.tag;
    result.stage = slot.stage;
    result.cacheSource = War3DispatchCacheSource::RenderableSlotScan;
    touchSlot(slot);
    updateHotRenderable(result.resolved, result.tag, result.stage);
    TrackDispatchTagStageCacheHit();
    return result;
  }

  const void *sceneNode = TryReadSceneNodeFromRenderablePart(request.renderablePart);
  const int currentStage =
      request.stageHint >= 0 ? request.stageHint : War3RenderState::GetStage();
  if (sceneNode != nullptr) {
    const bool hotSceneStageCompatible =
        currentStage < 0 || cache.hotSceneStage == currentStage;
    if (cache.hotSceneValid && cache.hotSceneNode == sceneNode &&
        hotSceneStageCompatible) {
      result.resolved = cache.hotSceneResolved;
      result.tag = cache.hotSceneTag;
      result.stage = cache.hotSceneStage;
      result.cacheSource = War3DispatchCacheSource::HotScene;
      updateHotRenderable(result.resolved, result.tag, result.stage);
      TrackDispatchTagStageCacheHit();
      return result;
    }

    const uint32_t sceneSlotIdx = HashDispatchTagStageCacheKey(sceneNode);
    auto &sceneSlot = cache.slots[sceneSlotIdx];
    const bool sceneSlotStageCompatible =
        currentStage < 0 || sceneSlot.stage == currentStage;
    if (sceneSlot.valid && sceneSlot.sceneNode == sceneNode &&
        sceneSlotStageCompatible) {
      result.resolved = sceneSlot.resolved;
      result.tag = sceneSlot.tag;
      result.stage = sceneSlot.stage;
      result.cacheSource = War3DispatchCacheSource::SceneSlot;
      sceneSlot.renderablePart = request.renderablePart;
      touchSlot(sceneSlot);
      updateHotRenderable(result.resolved, result.tag, result.stage);
      updateHotScene(sceneNode, result.resolved, result.tag, result.stage);
      TrackDispatchTagStageCacheHit();
      return result;
    }

    for (auto &slot : cache.slots) {
      if (&slot == &sceneSlot)
        continue;
      if (!slot.valid || slot.sceneNode != sceneNode)
        continue;
      if (currentStage >= 0 && slot.stage != currentStage)
        continue;
      result.resolved = slot.resolved;
      result.tag = slot.tag;
      result.stage = slot.stage;
      result.cacheSource = War3DispatchCacheSource::SceneSlotScan;
      slot.renderablePart = request.renderablePart;
      touchSlot(slot);
      updateHotRenderable(result.resolved, result.tag, result.stage);
      updateHotScene(sceneNode, result.resolved, result.tag, result.stage);
      TrackDispatchTagStageCacheHit();
      return result;
    }
  }

  result.resolved =
      tracker.GetTagStage(request.renderablePart, result.tag, result.stage);
  result.cacheSource = War3DispatchCacheSource::Tracker;

  DispatchTagStageCacheEntry *victim = &cache.slots[0];
  for (auto &slot : cache.slots) {
    if (!slot.valid) {
      victim = &slot;
      break;
    }
    if (slot.stamp < victim->stamp)
      victim = &slot;
  }

  victim->valid = true;
  victim->resolved = result.resolved;
  victim->renderablePart = request.renderablePart;
  victim->sceneNode = const_cast<void *>(sceneNode);
  victim->tag = result.resolved ? result.tag : War3BatchTag::Unknown;
  victim->stage = result.resolved ? result.stage : -1;
  touchSlot(*victim);
  updateHotRenderable(victim->resolved, victim->tag, victim->stage);
  updateHotScene(sceneNode, victim->resolved, victim->tag, victim->stage);

  TrackDispatchTagStageCacheMiss();
  return result;
}

bool QueryTagStageCached(dxvk::war3::render::RenderQueueTracker &tracker,
                         void *renderablePart, War3BatchTag &outTag,
                         int &outStage) {
  War3DispatchQueryRequest request = {};
  request.renderablePart = renderablePart;
  const auto result = QueryTagStageCached(tracker, request);
  outTag = result.tag;
  outStage = result.stage;
  return result.resolved;
}

} // namespace dxvk::war3::hooks
