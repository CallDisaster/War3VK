#include "war3_scene_collector.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../game/war3_agent.h"
#include "../handle/war3_handle_resolver.h"
#include "war3_native_renderer_probe.h"
#include "war3_render_exec_batch.h"
#include "war3_render_objects.h"
#include "war3_render_state.h"
#include "../tools/war3_perf_monitor.h"
#include "../../util/util_env.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace dxvk {
namespace war3 {
namespace render {

namespace {

std::atomic<uint64_t> g_worldObjectListEntryCount{0u};
std::atomic<uint64_t> g_worldObjectListNullEntryCount{0u};
std::atomic<uint64_t> g_worldObjectListOwnerHintZeroCount{0u};
std::atomic<uint64_t> g_worldObjectListOwnerHintNonzeroCount{0u};
std::atomic<uint64_t> g_worldObjectListOwnerHintHandleCount{0u};
std::atomic<uint64_t> g_worldObjectListOwnerHintUnitPtrCount{0u};
std::atomic<uint64_t> g_worldObjectListOwnerHintZeroContextAcceptedCount{0u};
std::atomic<uint64_t> g_worldObjectListAcceptedIdentityCount{0u};
std::atomic<uint64_t> g_lastWorldObjectListEntryWorldObjectEntryPtr{0u};
std::atomic<uint64_t> g_lastWorldObjectListEntryOwnerHintValue{0u};
std::atomic<uint64_t> g_lastWorldObjectListEntrySceneNodePtr{0u};

} // namespace

/**
 * @brief SceneCollector 热路径可选性能分段。
 *
 * 默认关闭细粒度采样时，直接返回空 Scope，避免高频对象收集路径上的
 * PerfMonitor 原子与路径维护开销。
 */
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeSceneCollectorCpuScope(const char *name) {
  if constexpr (dxvk::war3::internal::kNativeOptimizationPerfTrackingEnabled) {
    return war3::War3PerfMonitor::instance().cpuScope(name);
  }
  return {};
}

// 声明定义在 war3_render_exec_batch.cpp 中的全局缓存变量
extern std::unordered_map<void *, uint32_t> g_unitToHandleId;
extern std::unordered_map<void *, void *> g_unitToAgent;
extern std::unordered_set<void *> g_unitAgentLookupMiss;
extern std::shared_mutex g_entryHandleMutex;

// 声明定义在 war3_render_exec_batch.cpp 中的查找函数
extern bool FindHandleByUnitPtr(void *unitPtr, uint32_t *outHandleId,
                                void **outAgent);
// 无锁版本：仅用于已持有锁的临界区内
extern bool FindHandleByUnitPtr_NoLock(void *unitPtr, uint32_t *outHandleId,
                                       void **outAgent);

// 全局互斥锁，用于保护 RenderObjectRegistry 的并发访问（如果需要）
// 在原始 Hook 中使用了 g_entryHandleMutex，这里我们依赖 Registry 内部的锁
// 或者在 CollectWorldObjects 内部处理

void SceneCollector::CollectWorldObjects(void *gameWorldPtr, int groupIdx) {
  constexpr int kMinWorldGroupIdx = 0;
  constexpr int kMaxWorldGroupIdx = 3;
  constexpr uint32_t kMaxWorldGroupEntries = 200000u;
  auto collectScope = MakeSceneCollectorCpuScope(
      "SceneCollector/CollectWorldObjects");
  // [DEBUG] 追踪函数调用
  // [DISABLED] ENTER LOG

  // 扩展扫描范围，支持更多组 (0-7)
  if (!gameWorldPtr || groupIdx < kMinWorldGroupIdx ||
      groupIdx > kMaxWorldGroupIdx)
    return;

  // 根据 Codex 逆向分析：列表元素结构为 24 字节
  // +0x00: WorldObjectEntry* (渲染入口)
  // +0x14: CUnit* 指针 (实测) 或 handleId

  // 获取列表指针: this + 91/92/93
  uint32_t listOffset = 91 + groupIdx;
  void **thisArray = (void **)gameWorldPtr;

  if (!IsReadableRange(thisArray, (listOffset + 1) * sizeof(void *)))
    return;

  void *list = thisArray[listOffset];

  if (list && IsReadableRange(list, 0x18)) {
    auto prepareScope = MakeSceneCollectorCpuScope("SceneCollector/Prepare");
    // 列表结构（类似 std::vector）：
    // +0x0C (listData[3]): 数据指针
    // +0x14 (listData[5]): 元素数量
    uint32_t *listData = (uint32_t *)list;
    void *dataPtr = (void *)listData[3]; // +0x0C
    uint32_t count = listData[5];        // +0x14
    if (!dataPtr || count == 0)
      return;
    if (count > kMaxWorldGroupEntries) {
      static uint32_t s_invalidCountLog = 0;
      if (s_invalidCountLog < 10u || (s_invalidCountLog % 1000u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3Hook: SceneCollector skip invalid group list "
            "(group=%d count=%u data=%p)\n",
            groupIdx, count, dataPtr);
      }
      s_invalidCountLog++;
      return;
    }
    if (!IsReadableRangeFast(dataPtr, size_t(count) * 24u)) {
      static uint32_t s_invalidRangeLog = 0;
      if (s_invalidRangeLog < 10u || (s_invalidRangeLog % 1000u) == 0u) {
        WAR3_RENDER_LOG(
            "DXVK War3Hook: SceneCollector skip unreadable list "
            "(group=%d count=%u data=%p)\n",
            groupIdx, count, dataPtr);
      }
      s_invalidRangeLog++;
      return;
    }

    // 日志限流（仅在开启渲染日志时生效）
    static std::set<void *> s_loggedLists;
    const bool renderLogEnabled = ::dxvk::war3dbg::RenderLogEnabled();
    bool shouldLog = renderLogEnabled &&
                     s_loggedLists.find(list) == s_loggedLists.end() &&
                     s_loggedLists.size() < 5;
    if (shouldLog) {
      s_loggedLists.insert(list);
    }

    auto &registry = RenderObjectRegistry::instance();

    // 每组都打印一次关键信息，帮助确认 Sprite 到底在哪
    // [DISABLED] Registry fill log

    uint32_t *elements = (uint32_t *)dataPtr;
    thread_local std::vector<RenderObjectBatchItem> s_batchItems;
    s_batchItems.clear();
    s_batchItems.reserve(count);

    // [性能] 默认仅追踪“被关注的句柄”（outline/bloom），避免把全场景对象都塞进 Registry。
    // 如需完整枚举/调试，请设置：DXVK_WAR3_FORCE_OBJECT_TRACKING=1
    const bool forceTrackAll =
        War3RenderState::IsForceObjectTrackingEnabled() ||
        (dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled &&
         dxvk::war3::internal::kPathBlockerHideEnabled &&
         dxvk::war3::internal::kPathBlockerForceBridgeTrackingEnabled) ||
        dxvk::war3::internal::kBridgeRawcodeForceTrackAllEnabled;
    const bool outlineAll = War3RenderState::IsOutlineDebugAllObjectsEnabled();
    const bool shadowLiteTracking =
        War3RenderState::NeedsShadowObjectIdentity();
    const bool filtered = !forceTrackAll && !outlineAll && !shadowLiteTracking;

    // 采样/统计时也需要该快照（用于计算 trackedHit），因此 probeEnabled 时强制刷新一次。
    const bool probeEnabled = NativeRendererProbe::IsEnabled();
    thread_local std::vector<uint32_t> s_trackedHandles;
    const bool needTrackedSnapshot =
        filtered || probeEnabled || War3RenderState::HasOutlineHandles() ||
        War3RenderState::HasBloomHandles();

    // 仅在确实需要“按句柄过滤/匹配”时才解析 jHandle：
    // - filtered：需要按 tracked handles 过滤；
    // - probeEnabled：需要 trackedHit 统计；
    // - outline/bloom：后续会通过 handle 命中对象。
    // 路径阻断器全量追踪模式下（filtered=false 且无 outline/bloom），
    // 逐对象解析 jHandle 属于纯开销，可直接跳过。
    const bool needHandleResolution =
        War3RenderState::NeedsShadowDrawFallbackBridge() || filtered ||
        probeEnabled ||
        War3RenderState::HasOutlineHandles() ||
        War3RenderState::HasBloomHandles();
    if (needTrackedSnapshot) {
      auto trackedHandleScope =
          MakeSceneCollectorCpuScope("SceneCollector/SnapshotTrackedHandles");
      War3RenderState::SnapshotTrackedHandles(s_trackedHandles);
    } else {
      s_trackedHandles.clear();
    }

    // 过滤模式且无任何目标句柄时，直接提前返回：
    // - 无需遍历 WorldObject 列表；
    // - 避免执行后续 unitPtr/sceneNode 解析热路径。
    if (filtered && s_trackedHandles.empty() && !probeEnabled) {
      NativeRendererProbe::instance().OnWorldObjectsGroup(
          groupIdx, count, 0, 0, 0, 0, true);
      return;
    }

    auto isTrackedHandle = [&](uint32_t jHandle) -> bool {
      if (!filtered)
        return true;
      if (jHandle == 0 || s_trackedHandles.empty())
        return false;
      return std::binary_search(s_trackedHandles.begin(), s_trackedHandles.end(),
                                jHandle);
    };

    // [正确性修复] 过滤模式下，列表元素的 unitPtr 并不总能通过 CUnit+0x0C/+0x10 推导出 handleId。
    // 例如部分版本/对象类型会导致该偏移无效，从而 jHandle=0 被过滤，最终描边/高亮无法匹配 draw。
    // 因此这里对 tracked handles 做一次 handleId -> (agentPtr/unitPtr) 反查，并用“指针匹配”快速命中。
    struct TrackedPtrMapEntry {
      uintptr_t keyPtr = 0;
      uint32_t jHandle = 0;
    };

    thread_local std::vector<uint32_t> s_trackedHandlesCached;
    thread_local std::vector<TrackedPtrMapEntry> s_trackedPtrMap;
    thread_local bool s_cachedResolverReady = false;
    thread_local bool s_cachedBuildIncomplete = false;

    const bool resolverReady =
        dxvk::war3::HandleResolver::instance().isInitialized();
    if (needTrackedSnapshot) {
      auto trackedMapScope =
          MakeSceneCollectorCpuScope("SceneCollector/BuildTrackedPtrMap");
      const bool handlesChanged = s_trackedHandlesCached != s_trackedHandles;
      const bool resolverChanged = s_cachedResolverReady != resolverReady;
      if (handlesChanged || resolverChanged || s_cachedBuildIncomplete) {
        s_cachedResolverReady = resolverReady;
        s_trackedHandlesCached = s_trackedHandles;
        s_trackedPtrMap.clear();
        s_cachedBuildIncomplete = false;

        if (resolverReady && !s_trackedHandles.empty()) {
          s_trackedPtrMap.reserve(s_trackedHandles.size() * 2);

          uint32_t resolvedHandleCount = 0;
          for (uint32_t tracked : s_trackedHandles) {
            const uint32_t handleId = GetHandleId(tracked);
            if (handleId == 0)
              continue;

            void *agentPtr = nullptr;
            if (!dxvk::war3::HandleResolver::instance().resolveHandle(handleId, 0,
                                                                      &agentPtr) ||
                !agentPtr) {
              continue;
            }
            resolvedHandleCount++;

            const uintptr_t agentAddr = reinterpret_cast<uintptr_t>(agentPtr);
            if (agentAddr > 0x10000u) {
              s_trackedPtrMap.push_back({agentAddr, tracked});
            }

            // 额外把 CAgent->CUnit 的指针也加入映射，用于匹配“列表存 CUnit*”的情况。
            void *unitPtrFromAgent = nullptr;
            dxvk::war3::game::AgentWrapper agent(agentPtr);
            unitPtrFromAgent = agent.GetUnitPtr();
            if (unitPtrFromAgent && unitPtrFromAgent != agentPtr) {
              const uintptr_t unitAddr =
                  reinterpret_cast<uintptr_t>(unitPtrFromAgent);
              if (unitAddr > 0x10000u) {
                s_trackedPtrMap.push_back({unitAddr, tracked});
              }
            }
          }

          std::sort(s_trackedPtrMap.begin(), s_trackedPtrMap.end(),
                    [](const TrackedPtrMapEntry &a,
                       const TrackedPtrMapEntry &b) { return a.keyPtr < b.keyPtr; });
          s_trackedPtrMap.erase(
              std::unique(s_trackedPtrMap.begin(), s_trackedPtrMap.end(),
                          [](const TrackedPtrMapEntry &a,
                             const TrackedPtrMapEntry &b) {
                            return a.keyPtr == b.keyPtr;
                          }),
              s_trackedPtrMap.end());

          // 如果有 tracked handle 解析失败，则下次调用继续尝试（避免因为一次时序问题导致长期不命中）。
          s_cachedBuildIncomplete = resolvedHandleCount != s_trackedHandles.size();
        }
      }
    } else {
      s_trackedHandlesCached.clear();
      s_trackedPtrMap.clear();
      s_cachedResolverReady = false;
      s_cachedBuildIncomplete = false;
    }

    auto tryMatchTrackedByPtr = [&](void *ptr) -> uint32_t {
      if (s_trackedPtrMap.empty() || !ptr)
        return 0;

      const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
      auto it = std::lower_bound(
          s_trackedPtrMap.begin(), s_trackedPtrMap.end(), addr,
          [](const TrackedPtrMapEntry &e, uintptr_t v) { return e.keyPtr < v; });
      if (it == s_trackedPtrMap.end() || it->keyPtr != addr)
        return 0;

      return it->jHandle;
    };

    uint32_t keptSceneNodeCount = 0;
    uint32_t keptHandleCount = 0;
    uint32_t trackedHitCount = 0;

    thread_local std::unordered_map<void *, uint32_t> s_unitHandleCache;
    thread_local uint32_t s_unitHandleCacheGcCounter = 0;
    if (needHandleResolution) {
      if (s_unitHandleCache.size() > 65536u ||
          ++s_unitHandleCacheGcCounter >= 4096u) {
        s_unitHandleCache.clear();
        s_unitHandleCacheGcCounter = 0;
      }
    }

    {
      auto iterateScope =
          MakeSceneCollectorCpuScope("SceneCollector/IterateList");
      for (uint32_t i = 0; i < count; i++) {
        // 读取列表元素 (24字节/6个DWORD)
        // idx * 6 + offset_idx
        g_worldObjectListEntryCount.fetch_add(1u, std::memory_order_relaxed);
        void *worldObjectEntry =
            (void *)elements[i * 6 + (ListElementOffsets::WorldObjectEntry / 4)];
        uint32_t rawVal = elements[i * 6 + (ListElementOffsets::UnitPtr / 4)];
        g_lastWorldObjectListEntryWorldObjectEntryPtr.store(
            uint64_t(reinterpret_cast<uintptr_t>(worldObjectEntry)),
            std::memory_order_relaxed);
        g_lastWorldObjectListEntryOwnerHintValue.store(
            uint64_t(rawVal), std::memory_order_relaxed);

        const bool hasOwnerHint = rawVal != 0u;
        void *unitPtr = hasOwnerHint ? reinterpret_cast<void *>(rawVal)
                                     : nullptr;

        // 判断 rawVal 是指针还是 handleId
        // 如果 < 0x01000000 视为 handleId (较少见)
        // 否则视为 CUnit* 指针 (IsLikelyUnitObjectSoft check omitted for speed,
        // registry handles it)
        const bool isHandleVal = hasOwnerHint && rawVal < 0x01000000u;

        if (!worldObjectEntry) {
          g_worldObjectListNullEntryCount.fetch_add(1u,
                                                    std::memory_order_relaxed);
          continue;
        }

        if (!hasOwnerHint) {
          g_worldObjectListOwnerHintZeroCount.fetch_add(
              1u, std::memory_order_relaxed);
        } else {
          g_worldObjectListOwnerHintNonzeroCount.fetch_add(
              1u, std::memory_order_relaxed);
          if (isHandleVal) {
            g_worldObjectListOwnerHintHandleCount.fetch_add(
                1u, std::memory_order_relaxed);
          } else {
            g_worldObjectListOwnerHintUnitPtrCount.fetch_add(
                1u, std::memory_order_relaxed);
          }

          if (!isHandleVal) {
            // rawVal 是 CUnit* 指针
            // [Fix] Ignore 0xFFFFFFFF (Sentinel/Invalid)
            if (rawVal == 0xFFFFFFFFu)
              continue;
          }
        }

        // 先解析句柄（用于过滤与统计），再决定是否读取 sceneNode（减少不必要的内存读取）。
        uint32_t jHandle = 0;
        if (needHandleResolution) {
          if (hasOwnerHint && isHandleVal) {
            jHandle = 0x100000u | rawVal;
          } else if (unitPtr) {
            const auto cacheIt = s_unitHandleCache.find(unitPtr);
            if (cacheIt != s_unitHandleCache.end()) {
              jHandle = cacheIt->second;
            }

            if (jHandle == 0 && needTrackedSnapshot) {
              jHandle = tryMatchTrackedByPtr(unitPtr);
            }

            // Fast lookup from CUnit +0x0C/+0x10（避免热路径 VirtualQuery）
            if (jHandle == 0) {
              uint32_t maybeH = 0;
              uint32_t maybeH2 = 0;
              if (dxvk::war3::SafeReadU32Fast(unitPtr, 0x0C, maybeH) &&
                  dxvk::war3::SafeReadU32Fast(unitPtr, 0x10, maybeH2) &&
                  maybeH > 0 && maybeH < 0x100000u && maybeH == maybeH2) {
                jHandle = 0x100000u | maybeH;
              }
            }

            if (jHandle != 0) {
              s_unitHandleCache[unitPtr] = jHandle;
            }
          }
        }

        if (filtered && needHandleResolution && !isTrackedHandle(jHandle)) {
          continue;
        }

        RenderObjectBatchItem item;
        item.worldObjectEntry = worldObjectEntry;
        item.unitPtr = (!hasOwnerHint || isHandleVal) ? nullptr : unitPtr;
        item.groupIdx = groupIdx;
        item.jHandle = jHandle;

        void *sceneNode = nullptr;
        if constexpr (dxvk::war3::internal::kNativeFlushUnsafePathEnabled) {
          sceneNode = *reinterpret_cast<void **>(
              reinterpret_cast<uint8_t *>(worldObjectEntry) + 0x20);
          item.sceneNode = sceneNode;
        } else if (dxvk::war3::SafeReadPtrFast(worldObjectEntry, 0x20,
                                               sceneNode) &&
                   sceneNode) {
          item.sceneNode = sceneNode;
        } else {
          item.sceneNode = nullptr;
        }
        g_lastWorldObjectListEntrySceneNodePtr.store(
            uint64_t(reinterpret_cast<uintptr_t>(item.sceneNode)),
            std::memory_order_relaxed);

        // Probe counters（仅用于统计，不影响逻辑）
        if (item.sceneNode)
          keptSceneNodeCount++;
        if (item.jHandle)
          keptHandleCount++;
        if (probeEnabled && item.jHandle && !s_trackedHandles.empty() &&
            std::binary_search(s_trackedHandles.begin(), s_trackedHandles.end(),
                               item.jHandle)) {
          trackedHitCount++;
        } else if (!probeEnabled && filtered) {
          // filtered 模式下，能进来的都一定是 tracked
          if (item.jHandle)
            trackedHitCount++;
        }

        s_batchItems.push_back(item);
        if (!hasOwnerHint) {
          g_worldObjectListOwnerHintZeroContextAcceptedCount.fetch_add(
              1u, std::memory_order_relaxed);
        }
        g_worldObjectListAcceptedIdentityCount.fetch_add(
            1u, std::memory_order_relaxed);

        // [DEBUG PROBE] Register scene node for reverse lookup
        if (sceneNode) {
          render::ExecBatchProcessor::DebugRegisterSceneNode(sceneNode);
        }

        if (shouldLog && i < 3) {
          if (!isHandleVal) {
            WAR3_RENDER_LOG("  [%u] Entry=0x%p Unit=0x%p\n", i, worldObjectEntry,
                            unitPtr);
          } else {
            WAR3_RENDER_LOG("  [%u] Entry=0x%p Handle=0x%08X\n", i,
                            worldObjectEntry, rawVal);
          }
        }
      }
    }

    // 默认使用“快速模式”写入 Registry：避免每帧对所有对象调用 HandleResolver。
    // 如需完整解析（agentType/agentPtr 等），可设置：DXVK_WAR3_OBJECT_TRACKING_FULL_RESOLVE=1
    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    static const int s_fullResolveCached =
        dxvk::war3::internal::kObjectTrackingFullResolve ? 1 : 0;

    RenderObjectBatchResolveMode registerMode =
        RenderObjectBatchResolveMode::FastMetadata;
    if (s_fullResolveCached != 0) {
      registerMode = RenderObjectBatchResolveMode::FullResolve;
    } else {
      const bool identityOnlyTracking =
          shadowLiteTracking && !War3RenderState::NeedsShadowDrawFallbackBridge() &&
          !filtered && !probeEnabled &&
          !War3RenderState::HasOutlineHandles() &&
          !War3RenderState::HasBloomHandles();
      if (identityOnlyTracking)
        registerMode = RenderObjectBatchResolveMode::IdentityOnly;
    }

    {
      auto registerScope =
          MakeSceneCollectorCpuScope("SceneCollector/RegisterBatch");
      registry.registerWorldObjectsBatch(s_batchItems, registerMode);
    }

    // Native Renderer Probe：统计对象收集与过滤效果
    NativeRendererProbe::instance().OnWorldObjectsGroup(
        groupIdx, count, static_cast<uint32_t>(s_batchItems.size()),
        keptSceneNodeCount, keptHandleCount, trackedHitCount, filtered);

    // [PERFORMANCE] 批量更新缓存，避免后续 FindHandleByUnitPtr 进入 O(N) 循环
    // 我们在每帧第一个 Group 触发重置和刷新
    // DISABLED 2026-01-08: UpdateUnitCache 导致 18ms 性能瓶颈
    // if (groupIdx == 0) {
    //   ExecBatchProcessor::ResetFrameCaches();
    //   ExecBatchProcessor::UpdateUnitCache();
    // }

    // [PERFORMANCE] 建立CUnit→HandleId缓存，避免ExecBatch中的50万次扫描
    // 在这里一次性查找所有单位的handleId，后续ExecBatch可以O(1)查找
    // 注意：这些变量现在定义在dxvk::war3::render命名空间中

    // [PERFORMANCE OPTIMIZATION]
    // 该 miss-mark 逻辑是为了避免 FindHandleByUnitPtr 触发昂贵扫描而引入的。
    // 当前版本 FindHandleByUnitPtr 已移除线性扫描，因此这里默认关闭（降低 CPU 开销）。
    // 如需回归旧行为做对比，可设置：DXVK_WAR3_UNIT_MISS_MARK=1
    // 内部测试版本：不依赖环境变量，直接使用编译期配置。
    static const int s_missMarkEnabledCached =
        dxvk::war3::internal::kUnitMissMarkEnabled ? 1 : 0;

    if (s_missMarkEnabledCached) {
      // 重要注意：FindHandleByUnitPtr 内部会对 g_entryHandleMutex 进行 shared_lock。
      // 因此绝不能在持有 unique_lock 时调用 FindHandleByUnitPtr，否则会发生自锁死机。
      thread_local std::vector<void *> s_unitsNeedingMissMark;
      s_unitsNeedingMissMark.clear();
      s_unitsNeedingMissMark.reserve(s_batchItems.size());

      // 第一阶段：读锁下批量筛选需要写入 Miss 的 unitPtr
      {
        std::shared_lock<std::shared_mutex> readLock(g_entryHandleMutex);
        for (const auto &item : s_batchItems) {
          void *unitPtr = item.unitPtr;
          if (!unitPtr)
            continue;

          if (g_unitToHandleId.count(unitPtr) > 0)
            continue;
          if (g_unitAgentLookupMiss.count(unitPtr) > 0)
            continue;

          s_unitsNeedingMissMark.push_back(unitPtr);
        }
      }

      // 第二阶段：写锁下一次性写入 Miss（两阶段之间可能缓存被刷新，所以需要二次校验）
      if (!s_unitsNeedingMissMark.empty()) {
        std::unique_lock<std::shared_mutex> writeLock(g_entryHandleMutex);
        for (void *unitPtr : s_unitsNeedingMissMark) {
          if (!unitPtr)
            continue;

          if (g_unitToHandleId.count(unitPtr) > 0)
            continue;
          if (g_unitAgentLookupMiss.count(unitPtr) > 0)
            continue;

          g_unitAgentLookupMiss.insert(unitPtr);
        }
      }
    }

    // [DEBUG] 追踪 registry 填充状态
    // [DISABLED] Registry fill log
  }
}

SceneCollectorIdentityProbeSummary QuerySceneCollectorIdentityProbeSummary() {
  SceneCollectorIdentityProbeSummary summary = {};
  summary.worldObjectListEntryCount =
      g_worldObjectListEntryCount.load(std::memory_order_relaxed);
  summary.worldObjectListNullEntryCount =
      g_worldObjectListNullEntryCount.load(std::memory_order_relaxed);
  summary.worldObjectListOwnerHintZeroCount =
      g_worldObjectListOwnerHintZeroCount.load(std::memory_order_relaxed);
  summary.worldObjectListOwnerHintNonzeroCount =
      g_worldObjectListOwnerHintNonzeroCount.load(std::memory_order_relaxed);
  summary.worldObjectListOwnerHintHandleCount =
      g_worldObjectListOwnerHintHandleCount.load(std::memory_order_relaxed);
  summary.worldObjectListOwnerHintUnitPtrCount =
      g_worldObjectListOwnerHintUnitPtrCount.load(std::memory_order_relaxed);
  summary.worldObjectListOwnerHintZeroContextAcceptedCount =
      g_worldObjectListOwnerHintZeroContextAcceptedCount.load(
          std::memory_order_relaxed);
  summary.worldObjectListAcceptedIdentityCount =
      g_worldObjectListAcceptedIdentityCount.load(std::memory_order_relaxed);
  summary.lastWorldObjectListEntryWorldObjectEntryPtr =
      g_lastWorldObjectListEntryWorldObjectEntryPtr.load(
          std::memory_order_relaxed);
  summary.lastWorldObjectListEntryOwnerHintValue =
      g_lastWorldObjectListEntryOwnerHintValue.load(
          std::memory_order_relaxed);
  summary.lastWorldObjectListEntrySceneNodePtr =
      g_lastWorldObjectListEntrySceneNodePtr.load(std::memory_order_relaxed);
  return summary;
}

} // namespace render
} // namespace war3
} // namespace dxvk
