#include "war3_render_exec_batch.h"
#include "../../../util/util_math.h"
#include "../../d3d9_device.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "war3_current_draw_contract.h"
#include "war3_render_objects.h"
#include "war3_render_identity_bridge.h"
#include "war3_render_queue_tracker.h"
#include "war3_native_renderer_probe.h"
#include "war3_render_state.h"

#define WAR3_BRIDGE_LOG(fmt, ...)                                              \
  ::dxvk::war3dbg::Print("DXVK_BRIDGE_LOG: " fmt, ##__VA_ARGS__)
#define WAR3_BRIDGE_WARN(fmt, ...)                                             \
  ::dxvk::war3dbg::Print("DXVK_BRIDGE_WARN: " fmt, ##__VA_ARGS__)

#include <algorithm>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace dxvk {
namespace war3 {
namespace render {

// 全局定义，用于跨模块或多处引用
std::unordered_map<void *, uint32_t> g_unitToHandleId;
std::unordered_map<void *, void *> g_unitToAgent;
std::unordered_set<void *> g_unitAgentLookupMiss;
static std::atomic<bool> g_cacheDirty{true};
std::shared_mutex g_entryHandleMutex;

// [DEBUG PROBE]
static std::unordered_set<void *> g_debugKnownModels;
static std::shared_mutex g_debugProbeMutex;

// 简单内存可读性检查 (优先使用 memory.h 中的实现)
// bool IsReadableRange(const void *ptr, size_t size); // 已在
// core/war3_memory.h 中定义

// TLS 当前批次信息
static thread_local void *t_currentBatchEntry = nullptr;

void ExecBatchProcessor::SetCurrentBatchEntry(void *entry) {
  t_currentBatchEntry = entry;
}

void *ExecBatchProcessor::GetCurrentBatchEntry() { return t_currentBatchEntry; }

void ExecBatchProcessor::DebugRegisterSceneNode(void *node) {
  if (!node)
    return;
  // 临时禁用此探测，解决部分环境下的 std::unordered_set 编译问题
}

bool TryGetHandleTable(::dxvk::war3::JassHandleTable *outTable) {
  uintptr_t gameBase =
      reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
  if (!gameBase)
    return false;

  ::dxvk::war3::CGameWar3 **ppGame =
      reinterpret_cast<::dxvk::war3::CGameWar3 **>(
          gameBase + ::dxvk::war3::GameDllOffsets::GameWar3Ptr);
  if (!IsReadableRange(ppGame, sizeof(void *)) || !*ppGame)
    return false;

  ::dxvk::war3::CGameWar3 *pGame = *ppGame;
  if (!IsReadableRange(pGame, sizeof(::dxvk::war3::CGameWar3)))
    return false;

  ::dxvk::war3::CGameState *pState = pGame->game_state;
  if (!IsReadableRange(pState, sizeof(::dxvk::war3::CGameState)))
    return false;

  if (outTable) {
    *outTable = pState->handle_table;
  }
  return true;
}

// 批量更新缓存，避免每帧数万次扫描
void ExecBatchProcessor::UpdateUnitCache() {
  // 核心优化：确保每帧只扫描一次句柄表
  if (!g_cacheDirty.exchange(false)) {
    return;
  }

  std::unique_lock<std::shared_mutex> lock(g_entryHandleMutex);

  // 清理上一帧数据
  g_unitToHandleId.clear();
  g_unitToAgent.clear();
  g_unitAgentLookupMiss.clear();

  ::dxvk::war3::JassHandleTable table = {};
  if (!TryGetHandleTable(&table))
    return;

  // 1.27a 经验值：句柄表通常不间断。
  // 我们不再对每个 obj 调用 IsReadableRange (VirtualQuery 很慢)。
  // 改为在进入循环前对 handles 数组本身进行一次性校验。
  if (!IsReadableRange(table.handle_array,
                       table.size * sizeof(::dxvk::war3::JassHandleNode))) {
    return;
  }

  const uint32_t count = (std::min)(table.size, 1000000u);
  uint32_t foundCount = 0;

  for (uint32_t i = 1; i < count; ++i) {
    const ::dxvk::war3::JassHandleNode node = table.handle_array[i];
    void *obj = node.object;

    // 启发式校验
    if (obj > (void *)0x1000 && node.ref_count > 0) {
      void *unitPtr = *reinterpret_cast<void *const *>(
          reinterpret_cast<const uint8_t *>(obj) +
          ::dxvk::war3::CAgentOffsets::UnitPtr);

      if (unitPtr > (void *)0x1000) {
        g_unitToHandleId[unitPtr] = i;
        g_unitToAgent[unitPtr] = obj;
        foundCount++;
      }
    }
  }

  if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
    static uint32_t s_logThrottle = 0;
    if (s_logThrottle++ % 100 == 0) {
      WAR3_BRIDGE_LOG("Cache Refreshed: found %u units in handle table (Frame "
                      "Sync enabled)\n",
                      foundCount);
    }
  }
}

// Lock-free 查找版本，供持有锁的场景调用
bool FindHandleByUnitPtr_NoLock(void *unitPtr, uint32_t *outHandleId,
                                void **outAgent) {
  if (!unitPtr)
    return false;

  auto it = g_unitToHandleId.find(unitPtr);
  if (it != g_unitToHandleId.end()) {
    if (outHandleId)
      *outHandleId = it->second;
    if (outAgent) {
      auto itAgent = g_unitToAgent.find(unitPtr);
      *outAgent = (itAgent != g_unitToAgent.end()) ? itAgent->second : nullptr;
    }
    return true;
  }
  return false;
}

bool FindHandleByUnitPtr(void *unitPtr, uint32_t *outHandleId,
                         void **outAgent) {
  // 优先查表
  {
    std::shared_lock<std::shared_mutex> lock(g_entryHandleMutex);
    if (FindHandleByUnitPtr_NoLock(unitPtr, outHandleId, outAgent))
      return true;

    if (g_unitAgentLookupMiss.count(unitPtr))
      return false;
  }

  // 如果没有，由于现在有 UpdateUnitCache，按理说不应该走到这里频繁扫描
  // 除非是新创建的单位。为了安全，记录为 Miss 避免重复扫描。
  // 注意：不再在这里做 50 万次扫描，因为性能代价太大。
  return false;
}

void ExecBatchProcessor::ResetFrameCaches() {
  // 标记缓存为脏，下一帧重新加载
  g_cacheDirty.store(true);
}

void ExecBatchProcessor::ResetCaches() {
  std::unique_lock<std::shared_mutex> lock(g_entryHandleMutex);
  g_unitToHandleId.clear();
  g_unitToAgent.clear();
  g_unitAgentLookupMiss.clear();
  if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging)
    WAR3_BRIDGE_LOG("Caches cleared.\n");
}

void ExecBatchProcessor::SetHandleManagerAddrs(uintptr_t table,
                                               uintptr_t array) {
  // 目前使用 TryGetHandleTable 动态解析，暂不需要手动设置
  if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
    WAR3_BRIDGE_LOG(
        "SetHandleManagerAddrs called (table=0x%llx, array=0x%llx)\n",
        (unsigned long long)table, (unsigned long long)array);
  }
}

ExecBatchContext ExecBatchProcessor::Begin(void *element, War3BatchTag tag,
                                           int elementStage, bool isType3) {
  ExecBatchContext ctx;
  ctx.prevTag = War3RenderState::GetCurrentBatchTag();
  ctx.prevShadowSemantic = War3RenderState::GetTlsShadowSemanticState();
  War3RenderState::SetBatchTag(tag);

  SetCurrentBatchObject(nullptr);
  War3RenderState::ClearTlsShadowSemanticState();
  ctx.prevStage = War3RenderState::GetStage();
  ctx.stageOverridden = elementStage >= 0;
  if (ctx.stageOverridden) {
    War3RenderState::SetStage(elementStage);
  }

  uint32_t handleId = 0;
  uint32_t resolvedHandle = 0;
  uint32_t resolvedRawcode = 0;
  void *elementSceneNode = nullptr;
  void *unitPtr = nullptr;
  void *resolvedWorldObjectEntry = nullptr;
  const RenderObjectInfo *matchedInfoPtr = nullptr;
  ObjectKind resolvedKind = static_cast<ObjectKind>(0);
  bool foundInRegistry = false;
  void *ctxEntry = nullptr;

  const bool isWorldGroup = tag == War3BatchTag::WorldObjects ||
                            tag == War3BatchTag::SelectionOverlay ||
                            tag == War3BatchTag::Decorations;
  const bool needsObjectTracking =
      isWorldGroup && War3RenderState::NeedsObjectTracking();
  const bool needsShadowObjectIdentity =
      isWorldGroup && War3RenderState::NeedsShadowObjectIdentity();
  const bool needsShadowFallbackBridge =
      isWorldGroup && War3RenderState::NeedsShadowDrawFallbackBridge();
  const bool needsShadowSemanticTracking =
      isWorldGroup && War3RenderState::NeedsShadowSemanticTracking();
  const bool shadowSemanticOnly =
      needsShadowSemanticTracking && !needsObjectTracking;

  auto mergeIdentitySnapshot =
      [&](const RenderObjectIdentitySnapshot &identity) {
        if (identity.worldObjectEntry != nullptr)
          resolvedWorldObjectEntry = identity.worldObjectEntry;
        if (identity.sceneNode != nullptr)
          elementSceneNode = identity.sceneNode;
        if (identity.unitPtr != nullptr)
          unitPtr = identity.unitPtr;
        if (identity.rawcode != 0u)
          resolvedRawcode = identity.rawcode;
        if (identity.kind != ObjectKind::Unknown)
          resolvedKind = identity.kind;
        if (identity.jHandle != 0u) {
          resolvedHandle = identity.jHandle;
          handleId = identity.jHandle & 0x0FFFFFu;
        }
        if (identity.worldObjectEntry != nullptr)
          ctxEntry = identity.worldObjectEntry;
        foundInRegistry = foundInRegistry || identity.HasStableIdentity();
      };

  // 当未启用对象/阴影语义追踪时，直接跳过桥接逻辑（降低 ExecBatch 开销）
  if (!isWorldGroup || (!needsObjectTracking && !needsShadowSemanticTracking)) {
    War3RenderState::SetTlsBatchHandle(0);
    if (unlikely(NativeRendererProbe::IsEnabled())) {
      NativeRendererProbe::instance().OnDispatch(isType3, isWorldGroup, false,
                                                 false, false);
    }
    return ctx;
  }

  // [RENDER CONTEXT BRIDGE] 终极方案 (TLS)
  // 设计目标：通过 WorldObjectEntry_Render 设置的 TLS 上下文直接获取 Entry，
  // 绕过 sceneNode 反查和 handle 倒推。
  // 现状：消费者路径已经在这里保留，但生产端还没有正式接线。
  if (isWorldGroup && needsShadowSemanticTracking && element != nullptr) {
    // semantic-only cutover still needs the lightweight render context.  Do
    // not resolve handles here; just carry scene/world-object keys forward so
    // model hooks and semantic manifest repair have something to join on.
    const CurrentDrawDispatchContext dispatchContext =
        GetCurrentDrawDispatchContext();
    if (dispatchContext.valid &&
        dispatchContext.renderablePart == element &&
        dispatchContext.sceneNode != nullptr) {
      elementSceneNode = dispatchContext.sceneNode;
    } else {
      SafeReadPtrFast(element, 0x14, elementSceneNode);
    }
  }

  if (isWorldGroup && shadowSemanticOnly) {
    RenderObjectIdentitySnapshot cachedIdentity = {};
    if (element != nullptr &&
        RenderQueueTracker::instance().GetCachedObjectIdentity(
            element, cachedIdentity)) {
      mergeIdentitySnapshot(cachedIdentity);
    } else if (elementSceneNode != nullptr &&
               TryResolveCurrentRenderObjectIdentity(elementSceneNode,
                                                     cachedIdentity)) {
      mergeIdentitySnapshot(cachedIdentity);
    }
    if (ctxEntry == nullptr)
      ctxEntry = GetCurrentBatchEntry();
  }

  if (isWorldGroup && !shadowSemanticOnly) {
    // [RENDER CONTEXT BRIDGE] 优先方案：从 Dispatch 层获取 TLS 句柄
    uint32_t dispatchHandle = War3RenderState::GetTlsDispatchHandle();
    if (dispatchHandle) {
      handleId = dispatchHandle & 0x0FFFFF;
      resolvedHandle = dispatchHandle;
      if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
        static std::atomic<uint32_t> s_dispatchSuccessCount{0};
        uint32_t dsc =
            s_dispatchSuccessCount.fetch_add(1, std::memory_order_relaxed);
        if (dsc < 20 || (dsc % 1000 == 0)) {
          dxvk::war3dbg::Print(
              "DXVK_BRIDGE_LOG: ✓ Dispatch Bridge SUCCESS! Handle=0x%08X\n",
              dispatchHandle);
        }
      }
    }

    // 1. 获取 SceneNode (位于 element + 0x14? 需要验证)
    if (!elementSceneNode && element) {
      SafeReadPtrFast(element, 0x14, elementSceneNode);

      if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
        static std::atomic<uint32_t> s_execBatchDebug{0};
        uint32_t ebc = s_execBatchDebug.fetch_add(1);
        if (ebc < 10 && IsReadableRange(element, 0x20)) {
          uint32_t *d = reinterpret_cast<uint32_t *>(element);
          dxvk::war3dbg::Print(
              "DXVK_BRIDGE_LOG: ExecBatch element=%p TAG=%d | DATA: %08X %08X "
              "%08X %08X | %08X %08X %08X %08X\n",
              element, (int)tag, d[0], d[1], d[2], d[3], d[4], d[5], d[6],
              d[7]);
        }
      }
    }

    // 2. 仅在对象追踪真的需要时，才做昂贵的 registry 反查。
    if (!shadowSemanticOnly) {
      RenderObjectIdentitySnapshot cachedIdentity = {};
      if (RenderQueueTracker::instance().GetCachedObjectIdentity(
              element, cachedIdentity)) {
        mergeIdentitySnapshot(cachedIdentity);
        foundInRegistry = true;
      }

      auto &registry = RenderObjectRegistry::instance();

      if (!matchedInfoPtr && ctxEntry) {
        matchedInfoPtr = registry.findByEntry(ctxEntry);
        if (matchedInfoPtr) {
          unitPtr = matchedInfoPtr->unitPtr;
          foundInRegistry = true;
          resolvedWorldObjectEntry = matchedInfoPtr->worldObjectEntry;
          resolvedRawcode = matchedInfoPtr->rawcode;
          resolvedKind = matchedInfoPtr->kind;
          handleId = matchedInfoPtr->jHandle & 0x0FFFFF;
          if (matchedInfoPtr->jHandle) {
            resolvedHandle = matchedInfoPtr->jHandle;
          }
          if (!elementSceneNode)
            elementSceneNode = matchedInfoPtr->sceneNode;
        }
      }

      if (!ctxEntry && elementSceneNode) {
        const RenderObjectInfo *cachedSceneInfo = nullptr;
        uint32_t cachedHandle = 0;
        if (RenderQueueTracker::instance().GetCachedObjectInfo(
                element, elementSceneNode, cachedSceneInfo, cachedHandle)) {
          matchedInfoPtr = cachedSceneInfo;
          unitPtr = cachedSceneInfo->unitPtr;
          elementSceneNode = cachedSceneInfo->sceneNode;
          foundInRegistry = true;
          resolvedWorldObjectEntry = cachedSceneInfo->worldObjectEntry;
          resolvedRawcode = cachedSceneInfo->rawcode;
          resolvedKind = cachedSceneInfo->kind;
          if (cachedHandle) {
            handleId = cachedHandle & 0x0FFFFF;
            resolvedHandle = cachedHandle;
          }
          ctxEntry = cachedSceneInfo->worldObjectEntry;
        }
      }

      // 3. 优先使用 TLS Entry 直连桥接，命中时可避免 sceneNode 反查。
      if (!matchedInfoPtr) {
        ctxEntry = GetCurrentBatchEntry();
        if (ctxEntry) {
          matchedInfoPtr = registry.findByEntry(ctxEntry);
          if (matchedInfoPtr) {
            unitPtr = matchedInfoPtr->unitPtr;
            handleId = matchedInfoPtr->jHandle & 0x0FFFFF;
            foundInRegistry = true;
            resolvedWorldObjectEntry = matchedInfoPtr->worldObjectEntry;
            resolvedRawcode = matchedInfoPtr->rawcode;
            resolvedKind = matchedInfoPtr->kind;
            if (matchedInfoPtr->jHandle) {
              resolvedHandle = matchedInfoPtr->jHandle;
            }
            if (!elementSceneNode)
              elementSceneNode = matchedInfoPtr->sceneNode;
          }
        }
      }

      // 4. 为此批次应用反向查询桥接 (SceneNode -> HandleId)
      if (!matchedInfoPtr && elementSceneNode) {
        matchedInfoPtr = registry.findBySceneNode(elementSceneNode);
        if (matchedInfoPtr) {
          unitPtr = matchedInfoPtr->unitPtr;
          handleId = matchedInfoPtr->jHandle & 0x0FFFFF;
          foundInRegistry = true;
          resolvedWorldObjectEntry = matchedInfoPtr->worldObjectEntry;
          resolvedRawcode = matchedInfoPtr->rawcode;
          resolvedKind = matchedInfoPtr->kind;

          if (matchedInfoPtr->jHandle) {
            resolvedHandle = matchedInfoPtr->jHandle;

            if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
              static std::atomic<uint32_t> s_bridgeSuccessCount{0};
              uint32_t sc =
                  s_bridgeSuccessCount.fetch_add(1, std::memory_order_relaxed);
              if (sc < 10 || (sc % 1000 == 0 && sc < 10000)) {
                dxvk::war3dbg::Print(
                    "DXVK_BRIDGE_LOG: ✓ Bridge SUCCESS! "
                    "SceneNode=%p -> Handle=0x%08X (%s) | RegistrySize=%zu\n",
                    elementSceneNode, matchedInfoPtr->jHandle,
                    ObjectKindToString(matchedInfoPtr->kind),
                    registry.getSceneNodeMappingCount());
              }
            }
          }
        }
      }
    } else {
      ctxEntry = GetCurrentBatchEntry();
    }
  }
  if (unitPtr && handleId == 0 && needsObjectTracking) {
    // 5. 使用全局缓存快速查找 HandleId
    std::shared_lock<std::shared_mutex> lock(g_entryHandleMutex);
    auto it = g_unitToHandleId.find(unitPtr);
    if (it != g_unitToHandleId.end()) {
      handleId = it->second;
    }
  }

  if (needsObjectTracking && handleId != 0 && isWorldGroup) {
    if (resolvedHandle == 0) {
      resolvedHandle = handleId;
      if (resolvedHandle < 0x100000u)
        resolvedHandle |= 0x100000u;
    }
    War3RenderState::SetLastRenderHandle(resolvedHandle);
  }

  // 6. 判断是否需要描边/高亮
  if (needsObjectTracking) {
    const bool hasOutlineHandles = War3RenderState::HasOutlineHandles();
    const bool hasBloomHandles = War3RenderState::HasBloomHandles();
    if (resolvedHandle != 0) {
      if (hasOutlineHandles &&
          War3RenderState::IsOutlineHandle(resolvedHandle)) {

        // 调试日志
        if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
          static uint32_t s_outlineMatchCount = 0;
          if (s_outlineMatchCount < 20) {
            s_outlineMatchCount++;
            WAR3_BRIDGE_LOG(
                "Outline Target IDENTIFIED! element=%p unit=%p hId=0x%x\n",
                element, unitPtr, handleId);
          }
        }
      }

      if (hasBloomHandles && War3RenderState::IsBloomHandle(resolvedHandle)) {
        if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
          static uint32_t s_bloomMatchCount = 0;
          if (s_bloomMatchCount < 20) {
            s_bloomMatchCount++;
            WAR3_BRIDGE_LOG(
                "Bloom Target IDENTIFIED! element=%p unit=%p hId=0x%x\n",
                element, unitPtr, handleId);
          }
        }
      }
    }
  }

  if (needsShadowSemanticTracking) {
    War3TlsShadowSemanticState semanticState = {};
    semanticState.renderablePart = element;
    semanticState.sceneNode = elementSceneNode;
    semanticState.worldObjectEntry = matchedInfoPtr
                                         ? matchedInfoPtr->worldObjectEntry
                                         : (resolvedWorldObjectEntry != nullptr
                                                ? resolvedWorldObjectEntry
                                                : ctxEntry);
    semanticState.object = matchedInfoPtr;
    semanticState.jHandle =
        resolvedHandle != 0 ? resolvedHandle
                            : (matchedInfoPtr ? matchedInfoPtr->jHandle : 0u);
    semanticState.rawcode =
        matchedInfoPtr ? matchedInfoPtr->rawcode : resolvedRawcode;
    semanticState.objectKind =
        matchedInfoPtr ? matchedInfoPtr->kind : resolvedKind;
    semanticState.tag = tag;
    semanticState.stage =
        elementStage >= 0 ? elementStage : War3RenderState::GetStage();
    semanticState.pathBlocker =
        dxvk::war3::internal::IsPathBlockerFourCc(semanticState.rawcode);
    War3RenderState::SetTlsShadowSemanticState(semanticState);

    if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {
      static std::atomic<uint32_t> s_shadowSemanticLogCount{0};
      const uint32_t semanticLogCount =
          s_shadowSemanticLogCount.fetch_add(1, std::memory_order_relaxed);
      if (semanticState.HasAnyContext() &&
          (semanticLogCount < 24u || semanticLogCount % 4000u == 0u)) {
        WAR3_BRIDGE_LOG(
            "Shadow Semantic READY part=%p entry=%p scene=%p handle=0x%08X "
            "raw=0x%08X kind=%s tag=%d stage=%d\n",
            semanticState.renderablePart, semanticState.worldObjectEntry,
            semanticState.sceneNode, semanticState.jHandle,
            semanticState.rawcode, ObjectKindToString(semanticState.objectKind),
            static_cast<int>(semanticState.tag), semanticState.stage);
      }
    }
  }

  // 7. 应用原有对象追踪状态
  // 即便未命中描边/高亮，也保留真实句柄，避免后续匹配遗漏
  if (needsObjectTracking) {
    War3RenderState::SetTlsBatchHandle(resolvedHandle);

    // 8. 绑定到当前批次上下文 (如有)
    if (matchedInfoPtr) {
      SetCurrentBatchObject(matchedInfoPtr);
    }
  } else {
    War3RenderState::SetTlsBatchHandle(0);
  }

  if (unlikely(NativeRendererProbe::IsEnabled())) {
    NativeRendererProbe::instance().OnDispatch(
        isType3, isWorldGroup, elementSceneNode != nullptr, foundInRegistry,
        resolvedHandle != 0);
  }

  return ctx;
}

void ExecBatchProcessor::End(const ExecBatchContext &ctx) {
  if (ctx.stageOverridden) {
    War3RenderState::SetStage(ctx.prevStage);
  }
  War3RenderState::SetBatchTag(ctx.prevTag);
  War3RenderState::SetTlsShadowSemanticState(ctx.prevShadowSemantic);
  War3RenderState::SetTlsBatchHandle(0);
  SetCurrentBatchObject(nullptr);
}

} // namespace render
} // namespace war3
} // namespace dxvk
