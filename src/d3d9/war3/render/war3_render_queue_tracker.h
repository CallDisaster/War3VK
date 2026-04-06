// war3_render_queue_tracker.h - RenderQueue 元素标签/阶段跟踪

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "war3_render_identity_bridge.h"
#include "war3_render_state.h"

namespace dxvk::war3::render {

struct RenderObjectInfo;

class RenderQueueTracker {
public:
  static RenderQueueTracker &instance();

  // 清空缓存 (仅增加 epoch)
  void Reset();

  // 设置 RenderQueue 全局指针 (供 batch 安全检查使用)
  void SetGlobals(uint32_t *numElementsPtr, void **batchArrayPtr);

  // 追踪新生成的批次 (包含安全性检查)
  // 根据 groupIdx 自动推断 tag
  void TrackNewBatches(uint32_t before, int groupIdx);

  // 批量标记标签/阶段（batchArray 为 RenderQueue 元素数组）
  void MarkTags(void *batchArray, uint32_t before, uint32_t after,
                War3BatchTag tag);
  void MarkStages(void *batchArray, uint32_t before, uint32_t after, int stage);

  // 查询标签/阶段 (完全无锁)
  bool GetTag(void *element, War3BatchTag &outTag) const;
  bool GetStage(void *element, int &outStage) const;
  bool GetTagStage(void *element, War3BatchTag &outTag, int &outStage) const;
  bool GetCachedObjectIdentity(void *element,
                               RenderObjectIdentitySnapshot &outIdentity) const;
  bool GetCachedObjectInfo(void *element, void *sceneNodeHint,
                           const RenderObjectInfo *&outInfo,
                           uint32_t &outJHandle);
  void PrimeCachedObjectIdentities(
      void *batchArray, uint32_t before, uint32_t after,
      const RenderObjectIdentitySnapshot &identity);

private:
  RenderQueueTracker();

  // 对齐到 32 字节以消除伪共享 (False Sharing)
  struct alignas(32) AtomicEntry {
    std::atomic<void *> key;
    std::atomic<uint32_t> state;

    mutable std::atomic<const RenderObjectInfo *> info;
    mutable std::atomic<uint32_t> jHandle;
    mutable std::atomic<uint32_t>
        infoStateEpoch; // bits 0-7: State, 8-31: Epoch
    mutable std::atomic<uint32_t> identityStateEpoch;
    RenderObjectIdentitySnapshot identity;

    AtomicEntry()
        : key(nullptr), state(0), info(nullptr), jHandle(0), infoStateEpoch(0),
          identityStateEpoch(0), identity() {
    }

    // 显式移动构造支持 vector resize
    AtomicEntry(AtomicEntry &&other) noexcept
        : key(other.key.load(std::memory_order_relaxed)),
          state(other.state.load(std::memory_order_relaxed)),
          info(other.info.load(std::memory_order_relaxed)),
          jHandle(other.jHandle.load(std::memory_order_relaxed)),
          infoStateEpoch(other.infoStateEpoch.load(std::memory_order_relaxed)),
          identityStateEpoch(
              other.identityStateEpoch.load(std::memory_order_relaxed)),
          identity(other.identity) {
    }

    AtomicEntry &operator=(AtomicEntry &&other) noexcept {
      key.store(other.key.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
      state.store(other.state.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
      info.store(other.info.load(std::memory_order_relaxed),
                 std::memory_order_relaxed);
      jHandle.store(other.jHandle.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
      infoStateEpoch.store(other.infoStateEpoch.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
      identityStateEpoch.store(
          other.identityStateEpoch.load(std::memory_order_relaxed),
          std::memory_order_relaxed);
      identity = other.identity;
      return *this;
    }

    // 禁止拷贝
    AtomicEntry(const AtomicEntry &) = delete;
    AtomicEntry &operator=(const AtomicEntry &) = delete;
  };

  // 使用固定大小的原子哈希表，消除锁竞争
  std::vector<AtomicEntry> m_entries;
  uint32_t m_mask = 0;
  std::atomic<uint32_t> m_epoch{1};

  // 统计数据
  mutable std::atomic<uint64_t> m_fastHit{0};
  mutable std::atomic<uint64_t> m_fastMiss{0};
  mutable std::atomic<uint64_t> m_infoHit{0};
  mutable std::atomic<uint64_t> m_infoMiss{0};
  mutable std::atomic<uint64_t> m_infoFill{0};
  uint32_t m_statFrame = 0;

  std::atomic<bool> m_globalsValid{false};
  uint32_t *m_numOfElementsPtr = nullptr;
  void **m_batchArrayPtr = nullptr;

  inline uint32_t PackState(War3BatchTag tag, int stage, uint32_t epoch) const {
    return (static_cast<uint32_t>(tag) & 0xFFu) |
           ((static_cast<uint32_t>(stage) & 0xFFu) << 8) |
           ((epoch & 0xFFFFu) << 16);
  }

  inline void UnpackState(uint32_t state, War3BatchTag &tag, int &stage,
                          uint32_t &epoch) const {
    tag = static_cast<War3BatchTag>(state & 0xFFu);
    stage = static_cast<int8_t>((state >> 8) & 0xFFu);
    epoch = (state >> 16) & 0xFFFFu;
  }
};

} // namespace dxvk::war3::render
