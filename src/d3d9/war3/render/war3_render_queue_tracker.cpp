#include "war3_render_queue_tracker.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "war3_render_objects.h"
#include "war3_render_state.h"
#include <cstdint>
#include <limits>
#include <mutex>

namespace dxvk::war3::render {

namespace {
constexpr size_t kElementStride = 20;
constexpr size_t kMaxProbes = 16; // 线性探测最大步长
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
  switch (groupIdx) {
  case 0:
    tag = War3BatchTag::WorldObjects;
    break;
  case 1:
    tag = War3BatchTag::SelectionOverlay;
    break;
  case 2:
    tag = War3BatchTag::Decorations;
    break;
  default:
    break;
  }

  if (batchArray && after > before && tag != War3BatchTag::Unknown) {
    MarkTags(batchArray, before, after, tag);
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
      war3dbg::Print("DXVK War3Hook: LockFree Tracker stats hit=%llu miss=%llu "
                     "infoHit=%llu infoMiss=%llu infoFill=%llu\n",
                     (unsigned long long)fHit, (unsigned long long)fMiss,
                     (unsigned long long)iHit, (unsigned long long)iMiss,
                     (unsigned long long)iFill);
    }
  }
}

void RenderQueueTracker::MarkTags(void *batchArray, uint32_t before,
                                  uint32_t after, War3BatchTag tag) {
  if (!batchArray || after <= before)
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

      // 如果槽位为空，尝试抢占
      if (entry.key.compare_exchange_strong(expectedKey, element,
                                            std::memory_order_relaxed) ||
          expectedKey == element) {
        // 抢占成功或已存在：更新状态
        entry.state.store(PackState(tag, -1, currentEpoch),
                          std::memory_order_relaxed);
        break;
      }
      // 如果被本帧其他对象抢占，继续探测
      uint32_t entryState = entry.state.load(std::memory_order_relaxed);
      uint32_t entryEpoch = (entryState >> 16) & 0xFFFFu;
      if (entryEpoch != currentEpoch) {
        // 槽位虽有 key 但属于旧帧，尝试收割
        if (entry.key.compare_exchange_strong(expectedKey, element,
                                              std::memory_order_relaxed)) {
          entry.state.store(PackState(tag, -1, currentEpoch),
                            std::memory_order_relaxed);
          break;
        }
      }
    }
  }
}

void RenderQueueTracker::MarkStages(void *batchArray, uint32_t before,
                                    uint32_t after, int stage) {
  if (!batchArray || after <= before)
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
      void *currentKey = entry.key.load(std::memory_order_relaxed);

      if (currentKey == element) {
        // 如果已存在，更新 stage，保持 tag 和 epoch
        uint32_t oldState = entry.state.load(std::memory_order_relaxed);
        War3BatchTag tag;
        int oldStage;
        uint32_t oldEpoch;
        UnpackState(oldState, tag, oldStage, oldEpoch);

        // 仅当是本帧数据时才更新，否则可能会错误覆盖
        if (oldEpoch == currentEpoch) {
          entry.state.store(PackState(tag, stage, currentEpoch),
                            std::memory_order_relaxed);
          break;
        }
      }
      if (currentKey == nullptr)
        break;
    }
  }
}

bool RenderQueueTracker::GetTagStage(void *element, War3BatchTag &outTag,
                                     int &outStage) const {
  if (!element)
    return false;

  size_t h = HashElementKey(element) & m_mask;
  uint32_t currentEpoch = m_epoch.load(std::memory_order_relaxed);

  for (size_t p = 0; p < kMaxProbes; ++p) {
    const auto &entry = m_entries[(h + p) & m_mask];
    void *k = entry.key.load(std::memory_order_relaxed);
    if (k == element) {
      uint32_t s = entry.state.load(std::memory_order_relaxed);
      War3BatchTag t;
      int st;
      uint32_t ep;
      UnpackState(s, t, st, ep);
      if (ep == currentEpoch) {
        outTag = t;
        outStage = st;
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
