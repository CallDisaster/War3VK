#include "war3_render_queue.h"
#include "../render/war3_render_objects.h"
#include "../render/war3_render_state.h"
#include "../tools/war3_perf_monitor.h"
#include <limits>

namespace dxvk {
namespace war3 {
namespace reimpl {

/**
 * @brief 比较 LayerState 前 20 字节是否一致。
 *
 * 原版逻辑只关心前 20B；这里改为 5 个 uint32_t 直接比较，
 * 避免热路径频繁调用通用 memcmp 带来的额外开销。
 */
static inline bool LayerStatePrefix20Equal(const void *lhs, const void *rhs) {
  if (lhs == rhs)
    return true;
  if (!lhs || !rhs)
    return false;

  const auto *a = reinterpret_cast<const uint32_t *>(lhs);
  const auto *b = reinterpret_cast<const uint32_t *>(rhs);
  return (a[0] == b[0]) && (a[1] == b[1]) && (a[2] == b[2]) &&
         (a[3] == b[3]) && (a[4] == b[4]);
}

static inline int PointerTieBreakLess(const void *lhs, const void *rhs) {
  return reinterpret_cast<uintptr_t>(lhs) < reinterpret_cast<uintptr_t>(rhs)
             ? 1
             : 0;
}

int RenderQueue::BatchComparator(const void *a, const void *b) {
  const RenderBatchElement *batchA =
      *static_cast<const RenderBatchElement *const *>(a);
  const RenderBatchElement *batchB =
      *static_cast<const RenderBatchElement *const *>(b);

  // 按 RenderablePart 排序 (减少网格切换)
  if (batchA->renderablePart != batchB->renderablePart) {
    return batchA->renderablePart < batchB->renderablePart ? -1 : 1;
  }
  // 同对象按子索引排序 (对应可见层顺序)
  if (batchA->subIndex != batchB->subIndex) {
    return batchA->subIndex < batchB->subIndex ? -1 : 1;
  }
  return 0;
}

#if defined(_M_IX86) && defined(_MSC_VER)
int RenderQueue::ItemLess_Asm(const RenderBatchElement *a,
                              const RenderBatchElement *b) {
  // 使用优化的 C++ 实现（与原生算法完全一致，但避免内联汇编的兼容性问题）
  // 阶段 1: 比较 (flags & 3) == 3
  const uint32_t flagsA = a->flags;
  const uint32_t flagsB = b->flags;
  const bool isSpecialA = (flagsA & 3) == 3;
  const bool isSpecialB = (flagsB & 3) == 3;
  if (isSpecialA != isSpecialB)
    return isSpecialA ? 1 : 0;

  // 阶段 2: 比较 flags & 2
  const bool hasFlag2A = (flagsA & 2) != 0;
  const bool hasFlag2B = (flagsB & 2) != 0;

  if (hasFlag2A) {
    if (!hasFlag2B)
      return 1; // A 有 flag2，B 没有

    // 比较 renderablePart->meshData (+0x0C)
    const uint32_t meshA = *reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(a->renderablePart) + 0x0C);
    const uint32_t meshB = *reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(b->renderablePart) + 0x0C);
    if (meshA != meshB)
      return meshA < meshB ? 1 : 0;

    // 比较 subIndex
    if (a->subIndex != b->subIndex)
      return a->subIndex < b->subIndex ? 1 : 0;

    // 比较 layerStatePtr[3] 和前 20 字节
    const uint32_t *lsA = reinterpret_cast<const uint32_t *>(a->layerStatePtr);
    const uint32_t *lsB = reinterpret_cast<const uint32_t *>(b->layerStatePtr);
    if (lsA[3] != lsB[3])
      return lsA[3] < lsB[3] ? 1 : 0;

    // 逐 DWORD 比较前 20 字节 (5 DWORDs)
    for (int i = 0; i < 5; i++) {
      if (lsA[i] != lsB[i])
        return lsA[i] < lsB[i] ? 1 : 0;
    }
    // std::sort 需要严格弱序；完全相等时必须回退到稳定 tie-break。
    return PointerTieBreakLess(a, b);
  }

  // A 没有 flag2
  if (hasFlag2B)
    return 0; // B 有 flag2

  // 两者都没有 flag2: 比较 layerStatePtr
  const uint32_t *lsA = reinterpret_cast<const uint32_t *>(a->layerStatePtr);
  const uint32_t *lsB = reinterpret_cast<const uint32_t *>(b->layerStatePtr);
  if (lsA[3] != lsB[3])
    return lsA[3] < lsB[3] ? 1 : 0;

  // 逐 DWORD 比较前 20 字节
  for (int i = 0; i < 5; i++) {
    if (lsA[i] != lsB[i])
      return lsA[i] < lsB[i] ? 1 : 0;
  }

  // 完全相等：比较 renderablePart->meshData
  const uint32_t meshA = *reinterpret_cast<const uint32_t *>(
      reinterpret_cast<const uint8_t *>(a->renderablePart) + 0x0C);
  const uint32_t meshB = *reinterpret_cast<const uint32_t *>(
      reinterpret_cast<const uint8_t *>(b->renderablePart) + 0x0C);
  if (meshA != meshB)
    return meshA < meshB ? 1 : 0;
  return PointerTieBreakLess(a, b);
}
#endif

void RenderQueue::InnerSort(void **sortedPtrs, uint32_t count,
                            ItemComparatorFn gameComparator) {
#if defined(_M_IX86) && defined(_MSC_VER)
  // 使用优化的内联比较器
  std::sort(sortedPtrs, sortedPtrs + count, [](void *lhs, void *rhs) {
    if (lhs == rhs)
      return false;
    return ItemLess_Asm(reinterpret_cast<const RenderBatchElement *>(lhs),
                        reinterpret_cast<const RenderBatchElement *>(rhs)) != 0;
  });
#else
  // 回退：使用原生比较器
  const auto addrLess = [](const void *lhs, const void *rhs) {
    return reinterpret_cast<uintptr_t>(lhs) < reinterpret_cast<uintptr_t>(rhs);
  };

  std::sort(sortedPtrs, sortedPtrs + count, [&](void *lhs, void *rhs) {
    if (lhs == rhs)
      return false;

    void *a = lhs;
    void *b = rhs;
    const int ab = gameComparator(&a, &b);
    if (ab < 0) {
      void *ra = rhs;
      void *rb = lhs;
      const int ba = gameComparator(&ra, &rb);
      if (ba < 0)
        return addrLess(lhs, rhs);
      return true;
    }
    return (ab == 0) ? addrLess(lhs, rhs) : false;
  });
#endif
}

bool RenderQueue::FlushSortedItems_StdSort(D3D9DeviceEx *device,
                                           const RenderQueueGlobals &g,
                                           ItemComparatorFn gameComparator,
                                           DispatchCommonFn dispatchCommon,
                                           DispatchSpecialFn dispatchSpecial,
                                           const RenderQueueFns &fns) {
#if !defined(_M_IX86) && !defined(__i386__)
  return false;
#endif
  if (!g.numOfElementsPtr || !g.batchArrayPtr || !g.sortedBatchCountPtr ||
      !g.sortedBatchPtrs)
    return false;

  const uint32_t num = *g.numOfElementsPtr;
  void *batchArrayRaw = *g.batchArrayPtr;
  void **sortedPtrs = g.sortedBatchPtrs;
  if (!batchArrayRaw || !sortedPtrs)
    return false;

  const uint32_t count = (std::min)(num, MAX_BATCHES_PER_FRAME);
  *g.sortedBatchCountPtr = count;

  uint8_t *batchBase = reinterpret_cast<uint8_t *>(batchArrayRaw);
  for (uint32_t i = 0; i < count; i++) {
    sortedPtrs[i] = batchBase + i * kBatchElementStride;
  }

  // 3) 遍历调度（保持与原始流程一致：Common/Special）
  if (!dispatchCommon || !dispatchSpecial)
    return false;

  // [TEST] 强制禁用状态优化，验证是否是比较逻辑导致的状态泄漏
  const bool stateOptEnabled =
      (g.stateOptEnabledPtr != nullptr) ? (*g.stateOptEnabledPtr != 0u) : true;

  // 渲染队列 Instancing（实验性）开关
  const bool instancingEnabled =
      dxvk::war3::internal::kNativeQueueAutoInstancingEnabled;

  auto &pendingInstances = GetPendingInstances();
  if (!instancingEnabled && !pendingInstances.empty()) {
    // 关闭 Instancing 时清空残留，避免跨帧污染
    pendingInstances.clear();
  }

  War3InstanceBuffer *instBuf = nullptr;
  if (instancingEnabled) {
    // 仅在启用 Instancing 时初始化全局状态，降低常规路径成本
    GetDispatchCommon() = dispatchCommon;
    GetDispatchSpecial() = dispatchSpecial;
    GetNativeFuncs() = fns;
    GetStateOptEnabled() = stateOptEnabled;

    auto &instBufPtr = GetInstBufPtr();
    if (!instBufPtr)
      instBufPtr = dxvk::war3::reimpl::War3InstanceBuffer::Get(device);
    instBuf = instBufPtr;

    // Register Callback (Lazy Init)
    auto &cb = GetBatchFlushCallback();
    if (!cb) {
      cb = [](D3D9DeviceEx *dev) { FlushPendingBatches(dev); };
    }
  }

  // [SAFETY] 指针验证（避免战役/特殊场景下的崩溃）
  //
  // Gemini 版本曾在这里对每个元素调用 IsReadableRange，导致在对象很多时出现
  // 大量 VirtualQuery（非常慢），表现为“镜头靠近对象立刻未响应”。
  //
  // 这里改为：
  // 1) 仅对批次数组与指针数组做“增量范围校验”（最多增长时触发一次
  // VirtualQuery） 2) 对每个元素做纯算术范围校验（不触发 VirtualQuery）

  const size_t batchBytes = size_t(count) * kBatchElementStride;
  const size_t ptrBytes = size_t(count) * sizeof(void *);

  // 快速路径：跳过冗余的内存安全检查（原版游戏不做这些检查）
  if constexpr (!dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
    // 慢速路径：完整的安全检查
    {
      static void *s_lastBatchArray = nullptr;
      static size_t s_lastBatchBytes = 0;
      static bool s_batchArrayOk = false;
      if (batchArrayRaw != s_lastBatchArray || batchBytes > s_lastBatchBytes) {
        s_lastBatchArray = batchArrayRaw;
        s_lastBatchBytes = batchBytes;
        s_batchArrayOk = (batchBytes == 0) ||
                         dxvk::war3::IsReadableRange(batchBase, batchBytes);
      }
      if (!s_batchArrayOk)
        return false;
    }

    {
      static void *s_lastSortedPtrs = nullptr;
      static size_t s_lastPtrBytes = 0;
      static bool s_sortedPtrsOk = false;
      if (sortedPtrs != s_lastSortedPtrs || ptrBytes > s_lastPtrBytes) {
        s_lastSortedPtrs = sortedPtrs;
        s_lastPtrBytes = ptrBytes;
        s_sortedPtrsOk = (ptrBytes == 0) ||
                         dxvk::war3::IsReadableRange(sortedPtrs, ptrBytes);
      }
      if (!s_sortedPtrsOk)
        return false;
    }

    const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(batchBase);
    const uintptr_t endAddr = baseAddr + batchBytes;

    // 预扫描：由于开启了
    // kNativeFlushUnsafePathEnabled，我们相信游戏内部队列的有效性，跳过 O(N)
    // 检查以节省 ~2ms CPU 开销。
    if (!dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
      for (uint32_t i = 0; i < count; i++) {
        auto *batch = reinterpret_cast<RenderBatchElement *>(sortedPtrs[i]);
        const uintptr_t batchAddr = reinterpret_cast<uintptr_t>(batch);
        if (!batch || batchAddr < baseAddr ||
            batchAddr + sizeof(RenderBatchElement) > endAddr ||
            ((batchAddr - baseAddr) % kBatchElementStride) != 0) {
          return false;
        }

        const uintptr_t entryAddr =
            reinterpret_cast<uintptr_t>(batch->renderablePart);
        if (entryAddr < 0x1000u)
          return false;

        if (!dxvk::war3::IsReadableRangeFast(batch->renderablePart, 0x18))
          return false;
      }
    }
  }

  if (!count) {
    // 即便没有批次，原始实现仍可能需要执行一次阶段更新/尾部清理。
    if (fns.stageUpdate)
      fns.stageUpdate(0);

    if (g.stateCleanupPendingPtr && *g.stateCleanupPendingPtr) {
      if (fns.gxCleanup74)
        fns.gxCleanup74();
      if (fns.gxCleanup78)
        fns.gxCleanup78();
      *g.stateCleanupPendingPtr = 0;
    }
    return true;
  }

  {
    auto sortScope =
        war3::War3PerfMonitor::instance().cpuScope("FQ_Sort_Opaque");
    if (count > 1 && gameComparator) {
      bool needSort = true;

      // 中小批次优先做“已排序预检”：若本身有序则直接跳过排序。
      if constexpr (dxvk::war3::internal::kNativeQueueSkipSortIfAlreadySorted) {
        if (count <= dxvk::war3::internal::kNativeQueueSkipSortCheckMaxCount) {
          needSort = false;
          for (uint32_t i = 1; i < count; i++) {
            void **lhs = &sortedPtrs[i - 1];
            void **rhs = &sortedPtrs[i];
            if (gameComparator(lhs, rhs) > 0) {
              needSort = true;
              break;
            }
          }
        }
      }

      if (needSort) {
        InnerSort(sortedPtrs, count, gameComparator);
      }
    } else if (count > 1) {
      // 本地简单回调排序 (Fallback)
      std::sort(sortedPtrs, sortedPtrs + count, [](void *lhs, void *rhs) {
        auto *a = reinterpret_cast<RenderBatchElement *>(lhs);
        auto *b = reinterpret_cast<RenderBatchElement *>(rhs);
        if (a->renderablePart != b->renderablePart)
          return a->renderablePart < b->renderablePart;
        return a->subIndex < b->subIndex;
      });
    }
  }

  // 初始状态应用（必须在排序完成后，取首元素）
  void *lastLayerStatePtr =
      reinterpret_cast<RenderBatchElement *>(sortedPtrs[0])->layerStatePtr;
  if (fns.applyStateBlock && lastLayerStatePtr &&
      dxvk::war3::IsReadableRangeFast(lastLayerStatePtr, 36))
    fns.applyStateBlock(lastLayerStatePtr);

  auto dispatchScope =
      war3::War3PerfMonitor::instance().cpuScope("FQ_Dispatch_Opaque");

  void *lastMeshData = nullptr;
  uint32_t lastLayerIndex = 0;
  bool lastWasSpecial = false;
  constexpr bool kQueueDiag =
      dxvk::war3::internal::kNativeRenderQueueDiagnosticStatsEnabled;

  // [STATS] 批次合并诊断统计
  struct QueueDiagStats {
    uint32_t layerUnchanged = 0;          // layerChanged=0 的次数
    uint32_t stateUnchanged = 0;          // stateChanged=0 的次数
    uint32_t currentRun = 1;              // 当前连续相同状态的批次数
    uint32_t maxRun = 1;                  // 最大连续相同状态批次数
    uint32_t commonCalls = 0;             // dispatchCommon 调用次数
    uint32_t specialCalls = 0;            // dispatchSpecial 调用次数
    uint32_t contextMerges = 0;           // 成功合并上下文的次数
    uint32_t mergeCandidates = 0;         // 进入 instancing 判定的批次数
    uint32_t mergeAccepted = 0;           // 判定可并入 pending 的批次数
    uint32_t instancedGroups = 0;         // 真正走 Path A 的组次数
    uint32_t fallbackNoShader = 0;        // Path B（无 instancing shader）
    uint32_t fallbackAllocOrPortrait = 0; // Path C（Alloc 失败或 Portrait）
    uint32_t appendBreak = 0;             // Path A append 提前中断次数
  };
  [[maybe_unused]] QueueDiagStats stats = {};

  // 上下文追踪
  void *lastBeginSceneNode = nullptr;
  dxvk::war3::render::ExecBatchContext currentCtx;
  bool contextActive = false;
  bool lastWasType3 = false;

  // 分部分（RenderablePart）缓存：连续批次大概率属于同一部分，避免重复
  // GetTagStage
  void *lastTaggedPart = nullptr;
  dxvk::War3BatchTag lastPartTag = dxvk::War3BatchTag::Unknown;
  int lastPartStage = -1;
  // SceneNode 级缓存：同一单位多子网格连续提交时复用最近 tag/stage。
  void *lastTaggedSceneNode = nullptr;
  dxvk::War3BatchTag lastSceneTag = dxvk::War3BatchTag::Unknown;
  int lastSceneStage = -1;

  // [BATCH MERGE PROTOTYPE]（仅诊断模式启用，避免热路径额外开销）
  [[maybe_unused]] War3BatchMerger batchMerger;
  if constexpr (kQueueDiag) {
    batchMerger.Reset();
  }

  for (uint32_t i = 0; i < count; i++) {
    auto *batch = reinterpret_cast<RenderBatchElement *>(sortedPtrs[i]);
    if (!batch || !batch->renderablePart)
      continue;

    void *renderablePart = batch->renderablePart;

    // 快速路径：直接内存读取（原版游戏的做法）
    void *sceneNode = nullptr;
    void *meshData = nullptr;
    if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
      // 直接读取，不做安全检查
      sceneNode = *reinterpret_cast<void **>(
          reinterpret_cast<uint8_t *>(renderablePart) + 0x14);
      meshData = *reinterpret_cast<void **>(
          reinterpret_cast<uint8_t *>(renderablePart) + 0x0C);
      if (!sceneNode || !meshData)
        continue;
    } else {
      // 慢速路径：安全检查
      if (!dxvk::war3::SafeReadPtrFast(renderablePart, 0x14, sceneNode) ||
          !sceneNode) {
        continue;
      }
      if (!dxvk::war3::SafeReadPtrFast(renderablePart, 0x0C, meshData) ||
          !meshData) {
        continue;
      }
    }

    // stateChanged：严格复现原始逻辑
    bool stateChanged = true;
    if (stateOptEnabled && meshData && meshData == lastMeshData &&
        batch->layerIndex == lastLayerIndex) {
      // 快速路径：直接读取 meshFlag
      if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
        uint32_t meshFlag104 = *reinterpret_cast<uint32_t *>(
            reinterpret_cast<uint8_t *>(meshData) + 0x104);
        if (meshFlag104 == 0) {
          stateChanged = false;
        }
      } else {
        uint32_t meshFlag104 = 0;
        if (dxvk::war3::SafeReadU32Fast(meshData, 0x104, meshFlag104) &&
            meshFlag104 == 0) {
          stateChanged = false;
        }
      }
    }

    const bool isSpecial = ((batch->flags & 3) == 3);

    // [CONTEXT MERGE] 核心优化
    // 如果属于同一个 SceneNode，我们可以复用 ExecBatch 状态

    // We calculate tag/stage early for BatchMerge prototype
    dxvk::War3BatchTag currentTag = dxvk::War3BatchTag::Unknown;
    int currentStage = -1;
    bool tagStageReady = false;
    auto ensureTagStage = [&]() {
      if (tagStageReady)
        return;
      tagStageReady = true;
      // RenderablePart 级缓存
      if (renderablePart == lastTaggedPart) {
        currentTag = lastPartTag;
        currentStage = lastPartStage;
        return;
      }
      // SceneNode 级缓存
      if (sceneNode && sceneNode == lastTaggedSceneNode) {
        currentTag = lastSceneTag;
        currentStage = lastSceneStage;
        lastTaggedPart = renderablePart;
        lastPartTag = currentTag;
        lastPartStage = currentStage;
        return;
      }
      if (fns.getTrackerTagStage) {
        fns.getTrackerTagStage(renderablePart, currentTag, currentStage);
        lastTaggedPart = renderablePart;
        lastPartTag = currentTag;
        lastPartStage = currentStage;
        lastTaggedSceneNode = sceneNode;
        lastSceneTag = currentTag;
        lastSceneStage = currentStage;
      }
    };

    // [BATCH MERGE PROTOTYPE]
    if constexpr (kQueueDiag) {
      ensureTagStage();
      batchMerger.Analyze(batch, currentTag);
    }

    // [War3] Auto-Instancing (State-Aware)
    if (instancingEnabled) {
      auto &pendingTag = GetPendingTag();
      auto &pendingStage = GetPendingStage();

      // 现在可以启用了，因为 d3d9_device.cpp 中的 SetTexture
      // 钩子会检测状态变化并自动拆分 batch
      ensureTagStage();
      bool isInstancing =
          (currentTag == dxvk::War3BatchTag::WorldObjects && !isSpecial);
      bool mergeable = false;
      if (isInstancing) {
        if constexpr (kQueueDiag) {
          stats.mergeCandidates++;
        }
      }

      if (isInstancing && instBuf) {
        if (pendingInstances.empty()) {
          // 仅当“下一项也满足合批条件”时才起批，避免大量 singleton 空转。
          if (i + 1 < count) {
            auto *nextBatch =
                reinterpret_cast<RenderBatchElement *>(sortedPtrs[i + 1]);
            if (nextBatch && nextBatch->renderablePart &&
                ((nextBatch->flags & 3) != 3) &&
                nextBatch->layerIndex == batch->layerIndex &&
                nextBatch->layerStatePtr == batch->layerStatePtr) {
              void *nextMeshData = nullptr;
              if constexpr (dxvk::war3::internal::
                                kNativeFlushUnsafePathEnabled) {
                nextMeshData = *reinterpret_cast<void **>(
                    reinterpret_cast<uint8_t *>(nextBatch->renderablePart) +
                    0x0C);
              } else {
                dxvk::war3::SafeReadPtrFast(nextBatch->renderablePart, 0x0C,
                                            nextMeshData);
              }
              mergeable = (nextMeshData == meshData);
            }
          }

          if (mergeable) {
            pendingTag = currentTag;
            pendingStage = currentStage;
          }
        } else {
          // [RESTORED] Merge Logic
          auto *first = pendingInstances[0];

          // [War3] Batch by MeshData (Geometry) instead of RenderablePart
          // (Unit)
          void *firstMeshData = nullptr;
          if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
            firstMeshData = *reinterpret_cast<void **>(
                reinterpret_cast<uint8_t *>(first->renderablePart) + 0x0C);
          } else {
            dxvk::war3::SafeReadPtrFast(first->renderablePart, 0x0C,
                                        firstMeshData);
          }

          if (firstMeshData == meshData &&
              first->layerIndex == batch->layerIndex &&
              first->layerStatePtr == batch->layerStatePtr &&
              pendingInstances.size() <
                  4096) { // [LIMIT] Prevent massive batches
            mergeable = true;

            // [Fix] Outline and Bloom State Verification
            void *sceneNodeFirst = nullptr;
            void *sceneNodeBatch = nullptr;
            if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
              sceneNodeFirst = *reinterpret_cast<void **>(
                  reinterpret_cast<uint8_t *>(first->renderablePart) + 0x14);
              sceneNodeBatch = *reinterpret_cast<void **>(
                  reinterpret_cast<uint8_t *>(batch->renderablePart) + 0x14);
            } else {
              dxvk::war3::SafeReadPtrFast(first->renderablePart, 0x14,
                                          sceneNodeFirst);
              dxvk::war3::SafeReadPtrFast(batch->renderablePart, 0x14,
                                          sceneNodeBatch);
            }

            if (sceneNodeFirst && sceneNodeBatch) {
              auto &reg = dxvk::war3::render::RenderObjectRegistry::instance();
              auto *infoFirst = reg.findBySceneNode(sceneNodeFirst);
              auto *infoBatch = reg.findBySceneNode(sceneNodeBatch);

              bool firstOutline = false, firstBloom = false;
              bool batchOutline = false, batchBloom = false;

              if (infoFirst && infoFirst->jHandle) {
                firstOutline =
                    dxvk::War3RenderState::IsOutlineHandle(infoFirst->jHandle);
                firstBloom =
                    dxvk::War3RenderState::IsBloomHandle(infoFirst->jHandle);
              }
              if (infoBatch && infoBatch->jHandle) {
                batchOutline =
                    dxvk::War3RenderState::IsOutlineHandle(infoBatch->jHandle);
                batchBloom =
                    dxvk::War3RenderState::IsBloomHandle(infoBatch->jHandle);
              }

              if (firstOutline != batchOutline || firstBloom != batchBloom) {
                mergeable = false; // Break batch!
              }
            }
          }
        }
      }

      if (!mergeable && !pendingInstances.empty()) {
        uint32_t count = (uint32_t)pendingInstances.size();
        uint32_t baseOffset = 0;

        // [Fix] Exclude Portraits from Batching (Small Viewport)
        D3DVIEWPORT9 vp;
        dxvk::War3RenderState::GetFastStateCache(&vp, nullptr, nullptr);
        bool isPortrait = (vp.Width < 300);

        // [Optimization] Avoid Instancing overhead for very small batches (Thresholding)
        bool skipInstancing = count < 4;

        auto *bufPtr = (isPortrait || skipInstancing || !instBuf)
                           ? nullptr
                           : instBuf->Alloc(count, baseOffset);

        if (contextActive && fns.execEndValue) {
          fns.execEndValue(&currentCtx);
          contextActive = false;
        }

        if (bufPtr) {
          // [Single Dispatch Phase 2]
          // The buffer currently holds `pendingInstances.size()` valid instances.
          // We only call the expensive `dispatchCommon` ONCE for the very first item
          // to setup the D3D9 state (shaders, textures, material).
          
          if (fns.execBeginValue) {
            auto *first = pendingInstances[0];
            fns.execBeginValue(first->renderablePart, pendingTag, pendingStage,
                               true, &currentCtx);
            contextActive = true;
          }

          dxvk::war3::reimpl::War3InstanceBuffer::SetActive(instBuf);

          auto* primaryBatch = pendingInstances[0];
          void* primarySceneNode = nullptr;
          
          if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
            primarySceneNode = *reinterpret_cast<void **>(
                reinterpret_cast<uint8_t *>(primaryBatch->renderablePart) + 0x14);
          } else {
            dxvk::war3::SafeReadPtrFast(primaryBatch->renderablePart, 0x14, primarySceneNode);
          }

          // 1. Dispatch the primary instance to bind all engine pipeline states
          dispatchCommon(primarySceneNode, primaryBatch->renderablePart,
                         reinterpret_cast<void *>(uintptr_t(primaryBatch->layerIndex)),
                         reinterpret_cast<void *>(uintptr_t(1)),
                         reinterpret_cast<void *>(uintptr_t(1)));
          if constexpr (kQueueDiag) {
            stats.commonCalls++;
          }
          
          // 2. Instruct DXVK InstanceBuffer to flush the populated hardware buffer
          // Since `dispatchCommon` bound the shader and set up the matrices,
          // DXVK's D3D9 hook intercepts drawing and will use our InstanceBuffer 
          // to issue a single DrawIndexedPrimitive(..., instanceCount)
          if (instBuf->GetCapturedInstanceCount() > 0) {
              instBuf->FlushBatch();
              if constexpr (kQueueDiag) {
                stats.instancedGroups++;
              }
          } else {
              if constexpr (kQueueDiag) {
                stats.fallbackAllocOrPortrait++;
              }
              // Fallback: draw the rest individually if Single Dispatch failed to capture
              for (size_t i = 1; i < pendingInstances.size(); i++) {
                auto *b = pendingInstances[i];
                void *sceneNodeB = nullptr;
                dxvk::war3::SafeReadPtrFast(b->renderablePart, 0x14, sceneNodeB);
                dispatchCommon(sceneNodeB, b->renderablePart,
                               reinterpret_cast<void *>(uintptr_t(b->layerIndex)),
                               reinterpret_cast<void *>(uintptr_t(0)),  // Fast State Fallback: layerChanged=0 
                               reinterpret_cast<void *>(uintptr_t(1)));
                if constexpr (kQueueDiag) {
                  stats.commonCalls++;
                }
              }
          }
          
          // Cleanup:
          // 保留 Stage1 清理以避免 TeamColor 串色，但必须同步失效“状态复用缓存”，
          // 否则后续 layerChanged/stateChanged 可能仍判定为可复用，导致空纹理被沿用。
          device->SetTexture(1, nullptr);
          dxvk::war3::reimpl::War3InstanceBuffer::SetActive(nullptr);
          lastLayerStatePtr = nullptr;
          lastMeshData = nullptr;
          lastLayerIndex = std::numeric_limits<uint32_t>::max();
          
        } else {
          // Fallback Block (No Buffer, Portrait, or Too Small)
          if constexpr (kQueueDiag) {
            stats.fallbackNoShader++;
          }
          if (fns.execBeginValue) {
            auto *first = pendingInstances[0];
            fns.execBeginValue(first->renderablePart, pendingTag, pendingStage,
                               true, &currentCtx);
            contextActive = true;
          }
          for (size_t i = 0; i < pendingInstances.size(); i++) {
            auto *b = pendingInstances[i];
            void *sceneNodeB = nullptr;
            if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
              sceneNodeB = *reinterpret_cast<void **>(
                  reinterpret_cast<uint8_t *>(b->renderablePart) + 0x14);
            } else {
              dxvk::war3::SafeReadPtrFast(b->renderablePart, 0x14, sceneNodeB);
            }
            
            // Fast State Fallback: Tell the engine layer didn't change (0) after the first one 
            dispatchCommon(sceneNodeB, b->renderablePart,
                           reinterpret_cast<void *>(uintptr_t(b->layerIndex)),
                           reinterpret_cast<void *>(uintptr_t(i == 0 ? 1 : 0)),
                           reinterpret_cast<void *>(uintptr_t(1)));
            if constexpr (kQueueDiag) {
              stats.commonCalls++;
            }
          }
        }
        pendingInstances.clear();
      }

      if (isInstancing && mergeable) {
        if constexpr (kQueueDiag) {
          stats.mergeAccepted++;
        }
        pendingInstances.push_back(batch);
        continue;
      }
    }

    bool needNewContext = (sceneNode != lastBeginSceneNode) ||
                          (isSpecial != lastWasType3) || !contextActive;

    if (needNewContext) {
      if (contextActive && fns.execEndValue) {
        fns.execEndValue(&currentCtx);
        contextActive = false;
      }

      if (fns.execBeginValue) {
        ensureTagStage();
        fns.execBeginValue(renderablePart, currentTag, currentStage, !isSpecial,
                           &currentCtx);
        contextActive = true;
      }
      lastBeginSceneNode = sceneNode;
      lastWasType3 = isSpecial;
      if constexpr (kQueueDiag) {
        stats.contextMerges++;
      }
    }

    if (isSpecial) {
      dispatchSpecial(
          sceneNode, renderablePart,
          reinterpret_cast<void *>(uintptr_t(batch->layerIndex)),
          reinterpret_cast<void *>(uintptr_t(stateChanged ? 1 : 0)));
      if constexpr (kQueueDiag) {
        stats.specialCalls++;
      }
    } else {

      // layerChanged：保守复用策略
      // 说明：
      // - 过去仅比较 layerStatePtr 前 20B，可能在“不同 mesh/layer 但状态块前缀相同”时
      //   误判为可复用，导致纹理/AlphaTest 相关状态沿用上一批次，出现透明贴图发黑。
      // - 这里增加 meshData/layerIndex 一致性约束，优先保证材质正确性。
      int layerChanged = 1;
      if (stateOptEnabled && !lastWasSpecial && lastLayerStatePtr &&
          batch->layerStatePtr && meshData == lastMeshData &&
          batch->layerIndex == lastLayerIndex) {
        // 快速路径：同指针直接视为未变化；仅在指针不同才做 20B 比较。
        if (LayerStatePrefix20Equal(lastLayerStatePtr, batch->layerStatePtr))
          layerChanged = 0;
      }

      // [STATS] 收集统计数据
      if constexpr (kQueueDiag) {
        if (layerChanged == 0) {
          stats.layerUnchanged++;
          stats.currentRun++;
          if (stats.currentRun > stats.maxRun)
            stats.maxRun = stats.currentRun;
        } else {
          stats.currentRun = 1;
        }
        if (!stateChanged)
          stats.stateUnchanged++;
        stats.commonCalls++;
      }

      dispatchCommon(sceneNode, renderablePart,
                     reinterpret_cast<void *>(uintptr_t(batch->layerIndex)),
                     reinterpret_cast<void *>(uintptr_t(layerChanged)),
                     reinterpret_cast<void *>(uintptr_t(stateChanged ? 1 : 0)));
    }

    // 原版 FlushSortedItems 每个元素调用 StageUpdate(0)（IDA 已确认）。
    // 传非零会强制更新所有 Stage，既偏离原版，又会放大性能回退风险。
    if (fns.stageUpdate)
      fns.stageUpdate(nullptr);

    lastWasSpecial = isSpecial;
    lastLayerStatePtr = batch->layerStatePtr;
    lastMeshData = meshData;
    lastLayerIndex = batch->layerIndex;
  }

  // [Instancing] 处理最后一段未刷新的合批
  if (instancingEnabled && !pendingInstances.empty()) {
    if (contextActive && fns.execEndValue) {
      fns.execEndValue(&currentCtx);
      contextActive = false;
    }
    FlushPendingBatches(device);
  }

  // 清理最后的上下文
  if (contextActive && fns.execEndValue) {
    fns.execEndValue(&currentCtx);
  }

  if constexpr (kQueueDiag) {
    // [STATS] 每隔一定帧数输出统计
    static uint32_t s_frameCounter = 0;
    static uint32_t s_accLayerUnchanged = 0;
    static uint32_t s_accStateUnchanged = 0;
    static uint32_t s_accCommonCalls = 0;
    static uint32_t s_accSpecialCalls = 0;
    static uint32_t s_accMaxRun = 0;
    static uint32_t s_accMergeCandidates = 0;
    static uint32_t s_accMergeAccepted = 0;
    static uint32_t s_accInstancedGroups = 0;
    static uint32_t s_accFallbackNoShader = 0;
    static uint32_t s_accFallbackAllocOrPortrait = 0;
    static uint32_t s_accAppendBreak = 0;

    s_accLayerUnchanged += stats.layerUnchanged;
    s_accStateUnchanged += stats.stateUnchanged;
    s_accCommonCalls += stats.commonCalls;
    s_accSpecialCalls += stats.specialCalls;
    s_accMergeCandidates += stats.mergeCandidates;
    s_accMergeAccepted += stats.mergeAccepted;
    s_accInstancedGroups += stats.instancedGroups;
    s_accFallbackNoShader += stats.fallbackNoShader;
    s_accFallbackAllocOrPortrait += stats.fallbackAllocOrPortrait;
    s_accAppendBreak += stats.appendBreak;
    if (stats.maxRun > s_accMaxRun)
      s_accMaxRun = stats.maxRun;
    s_frameCounter++;

    if (s_frameCounter >= 300) { // 每 300 帧输出一次
      const uint32_t totalCalls = s_accCommonCalls + s_accSpecialCalls;
      const float layerUnchangedPct =
          s_accCommonCalls > 0
              ? (100.0f * s_accLayerUnchanged / s_accCommonCalls)
              : 0.0f;
      const float stateUnchangedPct =
          s_accCommonCalls > 0
              ? (100.0f * s_accStateUnchanged / s_accCommonCalls)
              : 0.0f;
      const float mergeAcceptPct =
          s_accMergeCandidates > 0
              ? (100.0f * s_accMergeAccepted / s_accMergeCandidates)
              : 0.0f;
      WAR3_RENDER_LOG(
          "[BatchMergeStats] %u frames: Calls=%u (Common=%u, Special=%u), "
          "LayerUnchanged=%.1f%%, StateUnchanged=%.1f%%, MaxRun=%u, "
          "Merge=%u/%u(%.1f%%), InstGroups=%u, NoShader=%u, "
          "AllocOrPortrait=%u, AppendBreak=%u\n",
          s_frameCounter, totalCalls, s_accCommonCalls, s_accSpecialCalls,
          layerUnchangedPct, stateUnchangedPct, s_accMaxRun, s_accMergeAccepted,
          s_accMergeCandidates, mergeAcceptPct, s_accInstancedGroups,
          s_accFallbackNoShader, s_accFallbackAllocOrPortrait, s_accAppendBreak);
      s_frameCounter = 0;
      s_accLayerUnchanged = 0;
      s_accStateUnchanged = 0;
      s_accCommonCalls = 0;
      s_accSpecialCalls = 0;
      s_accMaxRun = 0;
      s_accMergeCandidates = 0;
      s_accMergeAccepted = 0;
      s_accInstancedGroups = 0;
      s_accFallbackNoShader = 0;
      s_accFallbackAllocOrPortrait = 0;
      s_accAppendBreak = 0;
    }

    // [BATCH MERGE PROTOTYPE]
    batchMerger.LogStats(s_frameCounter);
  }

  // 原始实现尾部清理
  if (g.stateCleanupPendingPtr && *g.stateCleanupPendingPtr) {
    if (fns.gxCleanup74)
      fns.gxCleanup74();
    if (fns.gxCleanup78)
      fns.gxCleanup78();
    *g.stateCleanupPendingPtr = 0;
  }

  return true;
}

bool RenderQueue::FlushTransparent_StdSort(const RenderQueueGlobals &g,
                                           ItemComparatorFn gameComparator,
                                           const RenderQueueFns &fns) {
  auto totalScope =
      war3::War3PerfMonitor::instance().cpuScope("FQ_Total_Trans");
  if (!g.aucTransparentCountPtr || !g.aucTransparentArrayBase ||
      !g.aucTransparentSortedPtrs)
    return false;

  const uint32_t count = *g.aucTransparentCountPtr;
  if (count == 0)
    return true;

  const uint32_t processCount = (std::min)(count, 10000u);
  void *arrayRaw = *g.aucTransparentArrayBase;
  void **sortedPtrs = g.aucTransparentSortedPtrs;
  if (!arrayRaw || !sortedPtrs)
    return false;

  // 1) 填充指针数组
  uint8_t *entryBase = reinterpret_cast<uint8_t *>(arrayRaw);
  for (uint32_t i = 0; i < processCount; i++) {
    sortedPtrs[i] = entryBase + i * kTransparentEntryStride;
  }

  // 2) 排序 - 对齐原版 sub_6F1378D0 语义：
  //   - 主键：sortKey 升序；
  //   - 次键：+0x08 浮点字段（arg0）降序；
  //   - 再次键：稳定 tie-break（type/payload/arg1/arg2/地址）避免同键抖动。
  auto transparentDepthKey = [](const AUCTransparentEntry *entry) -> float {
    if (!entry)
      return 0.0f;
    float depth = 0.0f;
    std::memcpy(&depth, &entry->arg0, sizeof(depth));
    return depth;
  };
  auto transparentLess = [&](const AUCTransparentEntry *a,
                             const AUCTransparentEntry *b) -> bool {
    if (a == b)
      return false;
    if (!a || !b)
      return b != nullptr;
    if (a->sortKey != b->sortKey)
      return a->sortKey < b->sortKey;
    const float depthA = transparentDepthKey(a);
    const float depthB = transparentDepthKey(b);
    if (depthA != depthB)
      return depthA > depthB;
    if (a->type != b->type)
      return a->type < b->type;
    if (a->payload != b->payload)
      return a->payload < b->payload;
    if (a->arg1 != b->arg1)
      return a->arg1 < b->arg1;
    if (a->arg2 != b->arg2)
      return a->arg2 < b->arg2;
    return a < b;
  };

  bool needSort = false;
  if (processCount > 1) {
    auto *prev = reinterpret_cast<AUCTransparentEntry *>(sortedPtrs[0]);
    for (uint32_t i = 1; i < processCount; i++) {
      auto *cur = reinterpret_cast<AUCTransparentEntry *>(sortedPtrs[i]);
      if (transparentLess(cur, prev)) {
        needSort = true;
        break;
      }
      prev = cur;
    }
  }

  if (needSort) {
    std::sort(sortedPtrs, sortedPtrs + processCount, [&](void *lhs, void *rhs) {
      auto *a = reinterpret_cast<AUCTransparentEntry *>(lhs);
      auto *b = reinterpret_cast<AUCTransparentEntry *>(rhs);
      return transparentLess(a, b);
    });
  }

  // 3) 分发循环
  for (uint32_t i = 0; i < processCount; i++) {
    auto *entry = reinterpret_cast<AUCTransparentEntry *>(sortedPtrs[i]);
    if (!entry)
      continue;

    // 根据 RVA 0x138210 逻辑分发
    switch (entry->type) {
    case 0:
      if (entry->payload && fns.sub_13A0E0) {
        uintptr_t meshInfoPtr = reinterpret_cast<uintptr_t>(entry->payload);
        uint32_t ctx = *reinterpret_cast<uint32_t *>(meshInfoPtr + 20);
        fns.sub_13A0E0(ctx, entry->payload);
      }
      break;
    case 1:
      if (fns.sub_198C00)
        fns.sub_198C00(entry->payload);
      break;
    case 2:
      if (fns.sub_19DFF0)
        fns.sub_19DFF0(entry->payload);
      break;
    case 3:
      if (fns.sub_19BC20)
        fns.sub_19BC20(entry->payload);
      break;
    case 4:
      if (fns.sub_13A0B0)
        fns.sub_13A0B0(entry->payload);
      break;
    case 5: {
      using ActionFn = void(__fastcall *)(int, int);
      auto *func = reinterpret_cast<ActionFn>(entry->payload);
      if (func)
        func(entry->arg1, entry->arg2);
    } break;
    default:
      break;
    }

    // [FIX] 原版在每个透明入口后调用 StageUpdate(0)（已通过 IDA 确认）
    if (fns.stageUpdate)
      fns.stageUpdate(0);
  }
  return true;
}

bool RenderBatch_Submit_Reimpl(void *sceneNode, const RenderQueueGlobals &g,
                               const RenderBatchFns &fns) {
  if (!sceneNode || !g.batchCapacityPtr || !g.numOfElementsPtr ||
      !g.batchArrayPtr || !g.batchGrowStepPtr) {
    return false;
  }

  struct RenderBatchDebugStats {
    uint64_t calls = 0;
    uint64_t fails = 0;
    uint64_t renderableTotal = 0;
    uint64_t renderableNull = 0;
    uint64_t renderableSkipFlag = 0;
    uint64_t renderableCullInvisible = 0;
    uint64_t renderableTransparent = 0;
    uint64_t meshInfoMissing = 0;
    uint64_t layerInfoNull = 0;
    uint64_t stateBlockNull = 0;
    uint64_t layerDataBaseNull = 0;
    uint64_t layerVisPtrNull = 0;
    uint64_t layerVisible = 0;
    uint64_t layerInvisible = 0;
    uint64_t visibilityModeBase = 0;
    uint64_t visibilityModeOffset = 0;
    uint64_t layerVisReadFail = 0;
    uint64_t layerPrecomputeUsed = 0;
    uint64_t layerProbeTotal = 0;
    uint64_t layerProbeVisible = 0;
    uint64_t layerCountTotal = 0;
    uint64_t layerCountMax = 0;
    uint64_t layerLoopTotal = 0;
    uint64_t meshFlagSet = 0;
    uint64_t meshFlagBreak = 0;
    uint64_t batchAdded = 0;
    uint64_t transparentAdded = 0;
  };
  static RenderBatchDebugStats s_stats;

  const uint32_t batchCountStart = *g.numOfElementsPtr;
  const uint32_t transparentCountStart =
      g.aucTransparentCountPtr ? *g.aucTransparentCountPtr : 0u;

  auto readPtrSafe = [](const void *base, size_t offset, void *&out) -> bool {
    if (dxvk::war3::SafeReadPtrFast(base, offset, out))
      return true;
    return dxvk::war3::SafeReadPtr(base, offset, out);
  };
  auto readU32Safe = [](const void *base, size_t offset,
                        uint32_t &out) -> bool {
    if (dxvk::war3::SafeReadU32Fast(base, offset, out))
      return true;
    return dxvk::war3::SafeReadU32(base, offset, out);
  };
  // 原版是直接解引用：这里尽量对齐，避免 SafeRead 误判导致漏渲染。
  auto readPtrRaw = [](const void *base, size_t offset) -> void * {
    return *reinterpret_cast<void *const *>(
        reinterpret_cast<const uint8_t *>(base) + offset);
  };
  auto readU32Raw = [](const void *base, size_t offset) -> uint32_t {
    return *reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(base) + offset);
  };
  auto readU8Raw = [](const void *base, size_t offset) -> uint8_t {
    return *reinterpret_cast<const uint8_t *>(
        reinterpret_cast<const uint8_t *>(base) + offset);
  };

  // SceneNode +0x0C: RenderableCount, +0x10: RenderableList
  uint32_t renderableCount = 0;
  void *renderableList = nullptr;
  if (!readU32Safe(sceneNode, 0x0C, renderableCount) ||
      !readPtrSafe(sceneNode, 0x10, renderableList)) {
    return false;
  }

  if (renderableCount == 0 || !renderableList) {
    return true;
  }

  if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
    ++s_stats.calls;
    s_stats.renderableTotal += renderableCount;
  }

  // SceneNode +0x20: CullTable, +0x30: MeshInfoTable, +0x50: VisibilityOffset
  void *cullTable = nullptr;
  void *meshInfoTable = nullptr;
  uint32_t visibilityOffset = 0;
  if (!readPtrSafe(sceneNode, 0x20, cullTable) ||
      !readPtrSafe(sceneNode, 0x30, meshInfoTable) ||
      !readU32Safe(sceneNode, 0x50, visibilityOffset)) {
    return false;
  }
  if (!cullTable || !meshInfoTable)
    return false;

  if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
    ++s_stats.visibilityModeOffset;
  }

  // 原版逻辑（严格按汇编语义）：
  // addr = *(uint32_t*)layerData + visibilityOffset
  //
  // ⚠️ 重要坑位：
  // 曾经尝试“自动判别 base/offset（把 visibilityOffset 当指针）”会导致
  // 可见性计算错乱，进而改变 batch flags&2 与排序语义，表现为：
  // - 特效层级错位（基尔加丹特效被模型盖住）
  // - TeamColor/贴图污染
  // 该逻辑不可随意改动，必须严格对齐汇编行为。
  auto readLayerVisible = [&](uint32_t layerVisRef,
                              uint8_t &outVisible) -> bool {
    if (layerVisRef == 0) {
      outVisible = 1;
      return true;
    }
    const uintptr_t addrVal =
        reinterpret_cast<uintptr_t>(layerVisRef) + visibilityOffset;
    if (addrVal < 0x1000u) {
      outVisible = 1;
      return true;
    }
    auto *addr = reinterpret_cast<uint8_t *>(addrVal);
    if (!dxvk::war3::IsReadableRangeFast(addr, 1)) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerVisReadFail;
      }
      outVisible = 1; // 读取失败时保守可见，避免漏渲染
      return true;
    }
    outVisible = *addr;
    return true;
  };

  auto *list = reinterpret_cast<uint8_t *>(renderableList);
  for (uint32_t i = 0; i < renderableCount; ++i) {
    void *renderablePart = readPtrRaw(list, 0);
    if (!renderablePart) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.renderableNull;
      }
      list += sizeof(void *);
      continue;
    }
    list += sizeof(void *);

    uint32_t skipFlag = readU32Raw(renderablePart, 0x10);
    if (skipFlag != 0) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.renderableSkipFlag;
      }
      continue;
    }

    void *meshData = readPtrRaw(renderablePart, 0x0C);
    if (!meshData) {
      continue;
    }

    uint32_t cullIndex = readU32Raw(meshData, 0x11C);

    uint8_t cullVisible = 0;
    if (!dxvk::war3::internal::kNativeRenderBypassCull) {
      cullVisible = readU8Raw(cullTable, 16u * cullIndex + 3);
      if (cullVisible == 0) {
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.renderableCullInvisible;
        }
        continue;
      }
    }

    // RenderablePart +0x14: SceneNodeBackPtr
    *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(renderablePart) +
                               0x14) = sceneNode;

    if (!fns.canEnqueueToMainQueue || !fns.addTransparent ||
        !fns.transformPoint) {
      continue; // 函数指针缺失，跳过
    }

    if (!fns.canEnqueueToMainQueue(sceneNode, renderablePart)) {
      // 透明对象走 AUCTransparent
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.renderableTransparent;
      }
      float worldPos[3] = {0.0f, 0.0f, 0.0f};
      auto *meshPos = reinterpret_cast<float *>(
          reinterpret_cast<uint8_t *>(meshData) + 0x10C);
      auto *worldMatrix = reinterpret_cast<float *>(
          reinterpret_cast<uint8_t *>(sceneNode) + 0x64);
      // [FIX] 必须先将局部坐标转换为世界坐标！原版调用流程：
      // TransformPoint3x4(outBuffer, localPos, worldMatrix);
      // AUCTransparent_AddEntry(entry, type=0, worldPos, sortKey);
      fns.transformPoint(worldPos, meshPos, worldMatrix);

      uint32_t transparentKey = readU32Raw(meshData, 0x120);

      // 原版使用 Type 0，不是 Type 4
      int type = 0;
      fns.addTransparent(renderablePart, type, worldPos, transparentKey);
      continue;
    }

    // RenderablePart -> MeshInfo
    uint32_t meshIndex = readU32Raw(meshData, 0x108);
    void *meshInfo = readPtrRaw(meshInfoTable, meshIndex * sizeof(void *));
    if (!meshInfo) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.meshInfoMissing;
      }
      // [FIX] meshInfo 为空时跳过该 renderable，而非回滚整个 SceneNode
      continue;
    }

    uint32_t layerCount = readU32Raw(meshInfo, 0x0C);
    if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
      s_stats.layerCountTotal += layerCount;
      if (layerCount > s_stats.layerCountMax) {
        s_stats.layerCountMax = layerCount;
      }
    }
    void *stateBlockBase = readPtrRaw(meshInfo, 0x10);
    void *layerInfo = readPtrRaw(meshInfo, 0x38);
    if (layerCount == 0) {
      continue;
    }

    if (!layerInfo) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerInfoNull;
      }
      continue;
    }

    void *layerDataBase = readPtrRaw(layerInfo, 0x10);
    if (!stateBlockBase) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.stateBlockNull;
      }
      continue;
    }
    if (!layerDataBase) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerDataBaseNull;
      }
      continue;
    }

    uint32_t visibleLayerCounter = 0;
    uint8_t *layerData = reinterpret_cast<uint8_t *>(layerDataBase) + 0x1C;
    uint8_t *statePtr = reinterpret_cast<uint8_t *>(stateBlockBase) + 4;

    constexpr uint32_t kMaxPrecomputeLayers =
        dxvk::war3::internal::kNativeRenderBatchPrecomputeMaxLayers;
    uint8_t precomputeVisible[kMaxPrecomputeLayers] = {};
    uint8_t precomputeHasVisibleAfter[kMaxPrecomputeLayers] = {};
    const bool usePrecompute =
        dxvk::war3::internal::kNativeRenderBatchPrecomputeVisibilityEnabled &&
        !dxvk::war3::internal::kNativeRenderBypassLayerVisibility &&
        layerCount > 1 && layerCount <= kMaxPrecomputeLayers;

    if (usePrecompute) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerPrecomputeUsed;
      }

      uint8_t *scanData = reinterpret_cast<uint8_t *>(layerDataBase) + 0x1C;
      for (uint32_t scanIndex = 0; scanIndex < layerCount; ++scanIndex) {
        uint8_t scanVisible = 0;
        const uint32_t layerVisRef = readU32Raw(scanData, 0);
        if (!readLayerVisible(layerVisRef, scanVisible)) {
          precomputeVisible[scanIndex] = 0;
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerVisPtrNull;
          }
        } else if (scanVisible == 0) {
          precomputeVisible[scanIndex] = 0;
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerInvisible;
          }
        } else {
          precomputeVisible[scanIndex] = 1;
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerVisible;
          }
        }
        scanData += RenderQueue::kLayerDataStride;
      }

      bool seenVisible = false;
      for (int32_t scanIndex = static_cast<int32_t>(layerCount) - 1;
           scanIndex >= 0; --scanIndex) {
        precomputeHasVisibleAfter[scanIndex] = seenVisible ? 1u : 0u;
        if (precomputeVisible[scanIndex]) {
          seenVisible = true;
        }
      }
    }

    for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
      if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
        ++s_stats.layerLoopTotal;
      }

      if (!dxvk::war3::internal::kNativeRenderBypassLayerVisibility) {
        if (usePrecompute) {
          if (precomputeVisible[layerIndex] == 0) {
            layerData += RenderQueue::kLayerDataStride;
            statePtr += RenderQueue::kLayerStateStride;
            continue;
          }
        } else {
          uint8_t layerVisible = 0;
          const uint32_t layerVisRef = readU32Raw(layerData, 0);
          if (!readLayerVisible(layerVisRef, layerVisible)) {
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerVisPtrNull;
            }
            layerData += RenderQueue::kLayerDataStride;
            statePtr += RenderQueue::kLayerStateStride;
            continue;
          }
          if (layerVisible == 0) {
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerInvisible;
            }
            layerData += RenderQueue::kLayerDataStride;
            statePtr += RenderQueue::kLayerStateStride;
            continue;
          }
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerVisible;
          }
        }
      } else {
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.layerVisible;
        }
      }

      // 确保 RenderQueue 容量
      uint32_t num = *g.numOfElementsPtr;
      uint32_t next = num + 1;
      const uint32_t capacity = *g.batchCapacityPtr;
      if (next > capacity) {
        uint32_t growStep = *g.batchGrowStepPtr;
        if (growStep == 0 && fns.computeBatchGrowStep) {
          growStep = static_cast<uint32_t>(
              fns.computeBatchGrowStep(g.batchCapacityPtr, next));
        }
        if (growStep != 0 && (next % growStep) != 0) {
          next += growStep - (next % growStep);
        }
        if (!fns.reserveBatchArray) {
          break; // 无法扩容，退出层循环
        }
        fns.reserveBatchArray(g.batchCapacityPtr, next);
        num = *g.numOfElementsPtr;
      }

      auto *batchArray = reinterpret_cast<uint8_t *>(*g.batchArrayPtr);
      if (!batchArray) {
        break; // 无法获取数组，退出层循环
      }

      auto *batch = reinterpret_cast<RenderBatchElement *>(
          batchArray + (num * RenderQueue::kBatchElementStride));

      batch->renderablePart = renderablePart;
      batch->flags = 0;
      batch->layerIndex = layerIndex;
      batch->subIndex = visibleLayerCounter;
      // statePtr 已在第 826 行从 meshInfo+0x10 正确计算
      // (第 811 行已验证 stateBlockBase 非空)
      batch->layerStatePtr = statePtr;

      uint32_t meshFlag = readU32Raw(meshData, 0x104);
      if (meshFlag != 0 && !dxvk::war3::internal::kNativeRenderIgnoreMeshFlag) {
        batch->flags |= 1u;
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.meshFlagSet;
        }
      }

      if (visibleLayerCounter > 0) {
        batch->flags |= 2u;
      } else {
        if (usePrecompute) {
          if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
            ++s_stats.layerProbeTotal;
          }
          if (precomputeHasVisibleAfter[layerIndex] != 0) {
            batch->flags |= 2u;
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerProbeVisible;
            }
          }
        } else {
          // 首层：检查是否存在后续可见层
          uint32_t probeIndex = layerIndex + 1;
          uint8_t *probeData = layerData + RenderQueue::kLayerDataStride;
          for (; probeIndex < layerCount; ++probeIndex) {
            if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
              ++s_stats.layerProbeTotal;
            }
            const uint32_t probeVisRef = readU32Raw(probeData, 0);
            uint8_t probeVisible = 0;
            if (readLayerVisible(probeVisRef, probeVisible) &&
                probeVisible != 0) {
              batch->flags |= 2u;
              if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
                ++s_stats.layerProbeVisible;
              }
              break;
            }
            probeData += RenderQueue::kLayerDataStride;
          }
        }
      }

      *g.numOfElementsPtr = num + 1;
      num = *g.numOfElementsPtr;
      ++visibleLayerCounter;

      // 原版逻辑：只要 meshFlag 触发，立即跳出层循环
      if ((batch->flags & 1u) != 0 &&
          !dxvk::war3::internal::kNativeRenderIgnoreMeshFlag) {
        if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
          ++s_stats.meshFlagBreak;
        }
        break;
      }

      layerData += RenderQueue::kLayerDataStride;
      statePtr += RenderQueue::kLayerStateStride;
    }
  }

  if (dxvk::war3::internal::kNativeRenderBatchDebugCounters) {
    s_stats.batchAdded += (*g.numOfElementsPtr - batchCountStart);
    if (g.aucTransparentCountPtr) {
      s_stats.transparentAdded +=
          (*g.aucTransparentCountPtr - transparentCountStart);
    }
    if (s_stats.calls != 0 && (s_stats.calls % 300u) == 0u) {
      WAR3_RENDER_LOG(
          "DXVK War3Hook: RenderBatch stats calls=%llu fails=%llu "
          "renderable=%llu null=%llu skip=%llu cull=%llu transp=%llu "
          "meshInfoMiss=%llu layerInfoNull=%llu stateBlockNull=%llu "
          "layerDataNull=%llu visModeBase=%llu visModeOffset=%llu "
          "visReadFail=%llu precompute=%llu layerVisPtrNull=%llu "
          "layerVis=%llu layerHide=%llu layerLoop=%llu layerCountMax=%llu "
          "probe=%llu probeHit=%llu meshFlag=%llu meshBreak=%llu "
          "batchAdd=%llu transpAdd=%llu\n",
          static_cast<unsigned long long>(s_stats.calls),
          static_cast<unsigned long long>(s_stats.fails),
          static_cast<unsigned long long>(s_stats.renderableTotal),
          static_cast<unsigned long long>(s_stats.renderableNull),
          static_cast<unsigned long long>(s_stats.renderableSkipFlag),
          static_cast<unsigned long long>(s_stats.renderableCullInvisible),
          static_cast<unsigned long long>(s_stats.renderableTransparent),
          static_cast<unsigned long long>(s_stats.meshInfoMissing),
          static_cast<unsigned long long>(s_stats.layerInfoNull),
          static_cast<unsigned long long>(s_stats.stateBlockNull),
          static_cast<unsigned long long>(s_stats.layerDataBaseNull),
          static_cast<unsigned long long>(s_stats.visibilityModeBase),
          static_cast<unsigned long long>(s_stats.visibilityModeOffset),
          static_cast<unsigned long long>(s_stats.layerVisReadFail),
          static_cast<unsigned long long>(s_stats.layerPrecomputeUsed),
          static_cast<unsigned long long>(s_stats.layerVisPtrNull),
          static_cast<unsigned long long>(s_stats.layerVisible),
          static_cast<unsigned long long>(s_stats.layerInvisible),
          static_cast<unsigned long long>(s_stats.layerLoopTotal),
          static_cast<unsigned long long>(s_stats.layerCountMax),
          static_cast<unsigned long long>(s_stats.layerProbeTotal),
          static_cast<unsigned long long>(s_stats.layerProbeVisible),
          static_cast<unsigned long long>(s_stats.meshFlagSet),
          static_cast<unsigned long long>(s_stats.meshFlagBreak),
          static_cast<unsigned long long>(s_stats.batchAdded),
          static_cast<unsigned long long>(s_stats.transparentAdded));
      s_stats = {};
    }
  }

  return true;
}

int WorldObjects_RenderGroup_Reimpl(void *worldPtr, int groupIdx,
                                    ListGetDataFn listGetData,
                                    ListGetCountFn listGetCount,
                                    WorldObjectEntryRenderFn entryRender) {
  constexpr uint32_t kMaxWorldGroupEntries = 200000u;
  if (!worldPtr || groupIdx < 0 || groupIdx > 2 || !listGetData ||
      !listGetCount || !entryRender) {
    return 0;
  }

  // 获取正确的列表指针
  auto *worldDwords = static_cast<uintptr_t *>(worldPtr);
  void *listPtr = reinterpret_cast<void *>(worldDwords[91 + groupIdx]);
  if (!listPtr)
    return 0;

  void *listData = listGetData(listPtr);
  const uint32_t listCount = listGetCount(listPtr);
  if (!listData || listCount == 0)
    return 0;
  if (listCount > kMaxWorldGroupEntries)
    return 0;
  if (!dxvk::war3::IsReadableRangeFast(listData, size_t(listCount) * 24u))
    return 0;

  int lastResult = 0;
  uint8_t *entryPtr = static_cast<uint8_t *>(listData);
  for (uint32_t i = 0; i < listCount; ++i) {
    void *objectEntry = *reinterpret_cast<void **>(entryPtr);
    if (objectEntry)
      lastResult = entryRender(objectEntry);
    entryPtr += 24;
  }

  return lastResult;
}

} // namespace reimpl
} // namespace war3
} // namespace dxvk
