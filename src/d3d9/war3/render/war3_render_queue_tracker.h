// war3_render_queue_tracker.h - RenderQueue 元素标签/阶段跟踪

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "war3_render_identity_bridge.h"
#include "war3_render_state.h"

namespace dxvk::war3::render {

struct RenderObjectInfo;

inline constexpr uint32_t kRenderQueueUnknownLayerIndex = ~uint32_t{0};

enum RenderQueueSemanticConflict : uint32_t {
  kRenderQueueSemanticConflictNone = 0u,
  kRenderQueueSemanticConflictTag = 1u << 0,
  kRenderQueueSemanticConflictStage = 1u << 1,
  kRenderQueueSemanticConflictLayer = 1u << 2,
};

struct RenderQueueSemanticState {
  War3BatchTag tag = War3BatchTag::Unknown;
  int stage = -1;
  uint32_t layerIndex = kRenderQueueUnknownLayerIndex;
  uint32_t conflictMask = kRenderQueueSemanticConflictNone;
  uint32_t epoch = 0;

  bool HasConflict() const {
    return conflictMask != kRenderQueueSemanticConflictNone;
  }

  bool HasKnownLayer() const {
    return layerIndex != kRenderQueueUnknownLayerIndex;
  }
};

struct RenderQueueSemanticConflictStats {
  uint64_t conflictingEntries = 0;
  uint64_t tagConflicts = 0;
  uint64_t stageConflicts = 0;
  uint64_t layerConflicts = 0;
};

class RenderQueueTracker {
public:
  static RenderQueueTracker &instance();

  // 清空缓存 (仅增加 epoch)
  void Reset();

  // 设置 RenderQueue 全局指针 (供 batch 安全检查使用)
  void SetGlobals(uint32_t *numElementsPtr, void **batchArrayPtr);

  // 追踪新生成的 CWorld group 批次，并写入 ASM 确认的 tag/stage。
  void TrackNewBatches(uint32_t before, int groupIdx);

  // 批量标记标签/阶段，并从每条 20-byte record 的 +0x08 采集 layer。
  // Unknown tag 或负 stage 表示保留同 epoch 条目的现有字段；空/旧条目
  // 仍会创建。不同的已知值保留首值并设置 sticky conflict。
  void MarkTagStage(void *batchArray, uint32_t before, uint32_t after,
                    War3BatchTag tag, int stage);
  void MarkTags(void *batchArray, uint32_t before, uint32_t after,
                War3BatchTag tag);
  void MarkStages(void *batchArray, uint32_t before, uint32_t after, int stage);

  // 查询标签/阶段 (完全无锁)。兼容接口会返回首个已知值；GPU skin
  // eligibility 必须改用 GetSemanticState 检查 conflict/layer/epoch。
  bool GetTag(void *element, War3BatchTag &outTag) const;
  bool GetStage(void *element, int &outStage) const;
  bool GetTagStage(void *element, War3BatchTag &outTag, int &outStage) const;
  bool GetSemanticState(void *element,
                        RenderQueueSemanticState &outState) const;
  RenderQueueSemanticConflictStats GetSemanticConflictStats() const;
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

  bool GetSemanticStateWithProbeCount(
      void *element, RenderQueueSemanticState &outState,
      uint32_t &outProbeCount) const;

  // 对齐到 32 字节以消除伪共享 (False Sharing)
  struct alignas(32) AtomicEntry {
    std::atomic<void *> key;
    std::atomic<uint32_t> state;
    std::atomic<uint32_t> layerIndex;
    std::atomic<uint32_t>
        semanticConflictStateEpoch; // bits 0-7: conflict mask, 8-23: epoch

    mutable std::atomic<const RenderObjectInfo *> info;
    mutable std::atomic<uint32_t> jHandle;
    mutable std::atomic<uint32_t>
        infoStateEpoch; // bits 0-7: State, 8-31: Epoch
    mutable std::atomic<uint32_t> identityStateEpoch;
    RenderObjectIdentitySnapshot identity;

    AtomicEntry()
        : key(nullptr), state(0), layerIndex(kRenderQueueUnknownLayerIndex),
          semanticConflictStateEpoch(0), info(nullptr), jHandle(0),
          infoStateEpoch(0), identityStateEpoch(0), identity() {
    }

    // 显式移动构造支持 vector resize
    AtomicEntry(AtomicEntry &&other) noexcept
        : key(other.key.load(std::memory_order_relaxed)),
          state(other.state.load(std::memory_order_relaxed)),
          layerIndex(other.layerIndex.load(std::memory_order_relaxed)),
          semanticConflictStateEpoch(other.semanticConflictStateEpoch.load(
              std::memory_order_relaxed)),
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
      layerIndex.store(other.layerIndex.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
      semanticConflictStateEpoch.store(
          other.semanticConflictStateEpoch.load(std::memory_order_relaxed),
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
  std::atomic<uint64_t> m_semanticConflictingEntries{0};
  std::atomic<uint64_t> m_semanticTagConflicts{0};
  std::atomic<uint64_t> m_semanticStageConflicts{0};
  std::atomic<uint64_t> m_semanticLayerConflicts{0};
  uint32_t m_statFrame = 0;

  std::atomic<bool> m_globalsValid{false};
  uint32_t *m_numOfElementsPtr = nullptr;
  void **m_batchArrayPtr = nullptr;

  inline uint32_t PackState(War3BatchTag tag, int stage, uint32_t epoch) const {
    const uint32_t packedTag =
        tag == War3BatchTag::Unknown
            ? 0xFFu
            : (static_cast<uint32_t>(tag) & 0xFFu);
    const uint32_t packedStage =
        stage < 0 ? 0xFFu : (static_cast<uint32_t>(stage) & 0xFFu);
    return packedTag | (packedStage << 8) |
           ((epoch & 0xFFFFu) << 16);
  }

  inline void UnpackState(uint32_t state, War3BatchTag &tag, int &stage,
                          uint32_t &epoch) const {
    const uint32_t packedTag = state & 0xFFu;
    const uint32_t packedStage = (state >> 8) & 0xFFu;
    tag = packedTag == 0xFFu ? War3BatchTag::Unknown
                             : static_cast<War3BatchTag>(packedTag);
    stage = packedStage == 0xFFu ? -1 : static_cast<int>(packedStage);
    epoch = (state >> 16) & 0xFFFFu;
  }

  inline uint32_t PackSemanticConflictState(uint32_t conflictMask,
                                            uint32_t epoch) const {
    return (conflictMask & 0xFFu) | ((epoch & 0xFFFFu) << 8);
  }

  inline void UnpackSemanticConflictState(uint32_t state,
                                          uint32_t &conflictMask,
                                          uint32_t &epoch) const {
    conflictMask = state & 0xFFu;
    epoch = (state >> 8) & 0xFFFFu;
  }
};

} // namespace dxvk::war3::render
