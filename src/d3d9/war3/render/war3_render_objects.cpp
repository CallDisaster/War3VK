// war3_render_objects.cpp - 渲染对象追踪模块实现

#include "war3_render_objects.h"
#include "../model/war3_model_hook.h"
#include "../model/war3_model_registry.h"
#include "war3_render_state.h"
#include "war3_shadow_object_registry.h"
#include "war3_shadow_runtime_bridge.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../debug/war3_debug.h"
#include "../game/war3_agent.h"
#include "../game/war3_unit.h"
#include "../handle/war3_handle_resolver.h"
#include <algorithm>
#include <thread>
#include <unordered_set>

namespace dxvk {
namespace war3 {
namespace render {

namespace {

bool ShouldFeedShadowRuntimeRegistries() {
  return dxvk::War3RenderState::NeedsShadowObjectIdentity();
}

struct BridgeRawcodeState {
  std::unordered_map<uint32_t, uint32_t> handleToRawcode;
  std::unordered_map<uint32_t, void*> handleToUnitPtr;
  std::unordered_set<uint32_t> failedHandles;
  std::unordered_set<uint32_t> loggedRawcodes;
};

struct UnitMetaCacheEntry {
  uint32_t rawcode = 0;
  uint32_t flags5C = 0;
  ObjectKind kind = ObjectKind::Unknown;
};

enum class RawcodeSource : uint8_t {
  Unknown = 0,
  UnitPtr = 1,
  HandleCache = 2,
  HandleResolved = 3,
};

BridgeRawcodeState &GetBridgeRawcodeState() {
  static BridgeRawcodeState s_state;
  return s_state;
}

std::unordered_map<uint64_t, UnitMetaCacheEntry> &GetUnitMetaCache() {
  static std::unordered_map<uint64_t, UnitMetaCacheEntry> s_cache;
  return s_cache;
}

inline uint64_t BuildUnitMetaCacheKey(void *unitPtr, uint32_t jHandle) {
  const uint64_t ptrBits = reinterpret_cast<uint64_t>(unitPtr);
  return (ptrBits >> 4) ^ (static_cast<uint64_t>(jHandle) << 32);
}

inline char ToPrintableFourCcByte(uint32_t v) {
  const uint8_t c = static_cast<uint8_t>(v & 0xFFu);
  return (c >= 0x20u && c <= 0x7Eu) ? static_cast<char>(c) : '.';
}

inline void FormatFourCc(uint32_t rawcode, char out[5]) {
  // 统一输出为编辑器可读顺序（高字节在前），例如 0x68666F6F -> "hfoo"。
  out[0] = ToPrintableFourCcByte(rawcode >> 24);
  out[1] = ToPrintableFourCcByte(rawcode >> 16);
  out[2] = ToPrintableFourCcByte(rawcode >> 8);
  out[3] = ToPrintableFourCcByte(rawcode);
  out[4] = '\0';
}

uint32_t ResolveRawcodeByHandleCached(uint32_t jHandle, bool *outFromCache) {
  if (outFromCache)
    *outFromCache = false;
  if (jHandle == 0)
    return 0;

  auto &state = GetBridgeRawcodeState();
  if (auto it = state.handleToRawcode.find(jHandle);
      it != state.handleToRawcode.end()) {
    if (outFromCache)
      *outFromCache = true;
    return it->second;
  }

  void* unitPtr = nullptr;
  if (auto it = state.handleToUnitPtr.find(jHandle);
      it != state.handleToUnitPtr.end()) {
    unitPtr = it->second;
  } else {
    if (state.failedHandles.find(jHandle) != state.failedHandles.end())
      return 0;

    const uint32_t handleId = GetHandleId(jHandle);
    if (handleId == 0) {
      state.failedHandles.insert(jHandle);
      return 0;
    }

    void *objPtr = nullptr;
    if (!HandleResolver::instance().resolveHandle(handleId, 0, &objPtr) ||
        !objPtr) {
      state.failedHandles.insert(jHandle);
      return 0;
    }

    game::AgentWrapper agent(objPtr);
    unitPtr = agent.GetUnitPtr();
    if (!unitPtr)
      unitPtr = objPtr; // 兼容“handle 直接指向 CUnit*”路径

    game::UnitWrapper unit(unitPtr);
    if (!unit.IsValid()) {
      state.failedHandles.insert(jHandle);
      return 0;
    }
    state.handleToUnitPtr[jHandle] = unitPtr;
  }

  game::UnitWrapper unit(unitPtr);
  if (!unit.IsValid()) {
    state.failedHandles.insert(jHandle);
    state.handleToUnitPtr.erase(jHandle);
    return 0;
  }

  const uint32_t rawcode = unit.GetRawcode();
  if (rawcode == 0) {
    state.failedHandles.insert(jHandle);
    return 0;
  }

  state.handleToRawcode[jHandle] = rawcode;
  return rawcode;
}

void* ResolveUnitPtrByHandleCached(uint32_t jHandle, bool* outFromCache) {
  if (outFromCache)
    *outFromCache = false;
  if (jHandle == 0)
    return nullptr;

  auto& state = GetBridgeRawcodeState();
  if (auto it = state.handleToUnitPtr.find(jHandle);
      it != state.handleToUnitPtr.end()) {
    if (outFromCache)
      *outFromCache = true;
    return it->second;
  }

  if (state.failedHandles.find(jHandle) != state.failedHandles.end())
    return nullptr;

  const uint32_t handleId = GetHandleId(jHandle);
  if (handleId == 0) {
    state.failedHandles.insert(jHandle);
    return nullptr;
  }

  void *objPtr = nullptr;
  if (!HandleResolver::instance().resolveHandle(handleId, 0, &objPtr) || !objPtr) {
    state.failedHandles.insert(jHandle);
    return nullptr;
  }

  void *unitPtr = nullptr;
  game::AgentWrapper agent(objPtr);
  unitPtr = agent.GetUnitPtr();
  if (!unitPtr)
    unitPtr = objPtr; // 兼容“handle 直接指向 CUnit*”路径

  game::UnitWrapper unit(unitPtr);
  if (!unit.IsValid()) {
    state.failedHandles.insert(jHandle);
    return nullptr;
  }

  state.handleToUnitPtr[jHandle] = unitPtr;
  return unitPtr;
}

const char *RawcodeSourceToString(RawcodeSource source) {
  switch (source) {
  case RawcodeSource::UnitPtr:
    return "unitPtr";
  case RawcodeSource::HandleCache:
    return "handleCache";
  case RawcodeSource::HandleResolved:
    return "handleResolve";
  default:
    return "unknown";
  }
}

void LogRawcodeOnce(uint32_t rawcode, uint32_t jHandle, int groupIdx,
                    ObjectKind kind, RawcodeSource source) {
  if (rawcode == 0)
    return;

  auto &state = GetBridgeRawcodeState();
  const auto [it, inserted] = state.loggedRawcodes.insert(rawcode);
  if (!inserted)
    return;

  char fourcc[5] = {};
  FormatFourCc(rawcode, fourcc);
  WAR3_LOG_INFO(
      "DXVK_BRIDGE_RAWCODE: fourcc='%s' (0x%08X) firstSeen "
      "handle=0x%08X group=%d kind=%s source=%s\n",
      fourcc, rawcode, jHandle, groupIdx, ObjectKindToString(kind),
      RawcodeSourceToString(source));
}

ObjectKind GuessObjectKindFromUnitFlags(uint32_t flags5C) {
  const uint32_t exponent = (flags5C >> 23) & 0xFFu;
  if (exponent >= 0x3Eu && exponent <= 0x43u &&
      (flags5C & 0x007FFFFFu) != 0u) {
    return ObjectKind::Destructible;
  }

  if ((flags5C & 0x80000000u) != 0u || flags5C > 0x7FFFFFFFu)
    return ObjectKind::Destructible;

  if ((flags5C & UnitFlags5C::Building) != 0u)
    return ObjectKind::Building;
  return ObjectKind::Unit;
}

bool TryResolveUnitMetadataFast(void *unitPtr, UnitMetaCacheEntry &out) {
  if (unitPtr == nullptr || !IsReadableRangeFast(unitPtr, 0x64))
    return false;

  uint32_t rawcode = 0;
  uint32_t flags5C = 0;
  SafeReadU32Fast(unitPtr, CUnitOffsets::Rawcode, rawcode);
  SafeReadU32Fast(unitPtr, CUnitOffsets::Flags5C, flags5C);

  out.rawcode = rawcode;
  out.flags5C = flags5C;
  out.kind = GuessObjectKindFromUnitFlags(flags5C);
  return true;
}

} // namespace

// ============================================================================
// TLS 变量
// ============================================================================

thread_local uint32_t g_tlsCurrentBatchHandle = 0;
thread_local const RenderObjectInfo *g_tlsCurrentBatchObject = nullptr;

uint32_t GetCurrentBatchHandle() { return g_tlsCurrentBatchHandle; }

void SetCurrentBatchHandle(uint32_t jHandle) {
  g_tlsCurrentBatchHandle = jHandle;
}

const RenderObjectInfo *GetCurrentBatchObject() {
  return g_tlsCurrentBatchObject;
}

void SetCurrentBatchObject(const RenderObjectInfo *info) {
  g_tlsCurrentBatchObject = info;
}

void ResetRenderObjectMapSessionCaches() {
  // Handles and Warcraft object addresses are both reusable immediately after
  // leaving a map. Periodic size/age eviction is therefore not a correctness
  // boundary for the first frames of the next map.
  auto& rawcode = GetBridgeRawcodeState();
  rawcode.handleToRawcode.clear();
  rawcode.handleToUnitPtr.clear();
  rawcode.failedHandles.clear();
  rawcode.loggedRawcodes.clear();
  GetUnitMetaCache().clear();

  g_tlsCurrentBatchHandle = 0u;
  g_tlsCurrentBatchObject = nullptr;
}

// ============================================================================
// ObjectKind 工具
// ============================================================================

const char *ObjectKindToString(ObjectKind kind) {
  switch (kind) {
  case ObjectKind::Unit:
    return "Unit";
  case ObjectKind::Building:
    return "Building";
  case ObjectKind::Destructible:
    return "Destructible";
  case ObjectKind::Item:
    return "Item";
  case ObjectKind::Effect:
    return "Effect";
  default:
    return "Unknown";
  }
}

// ============================================================================
// RenderObjectRegistry 实现
// ============================================================================

RenderObjectRegistry &RenderObjectRegistry::instance() {
  static RenderObjectRegistry s_instance;
  return s_instance;
}

RenderObjectRegistry::Snapshot &RenderObjectRegistry::writeSnapshot() {
  return m_snapshots[m_writeIndex];
}

const RenderObjectRegistry::Snapshot &
RenderObjectRegistry::readSnapshot() const {
  const uint32_t index = m_publishedIndex.load(std::memory_order_acquire);
  return m_snapshots[index];
}

const RenderObjectRegistry::Snapshot &
RenderObjectRegistry::snapshotForThread() const {
  if (std::this_thread::get_id() == m_renderThreadId) {
    return m_snapshots[m_writeIndex];
  }
  return readSnapshot();
}

void RenderObjectRegistry::beginFrame() {
  m_renderThreadId = std::this_thread::get_id();
  const uint32_t published = m_publishedIndex.load(std::memory_order_relaxed);
  m_writeIndex = (published + 1u) % kSnapshotCount;

  Snapshot &snap = m_snapshots[m_writeIndex];
  const size_t reserveEntry = snap.lastEntryCount;
  const size_t reserveScene = snap.lastSceneCount;
  const size_t reserveHandle = snap.lastHandleCount;
  snap.byEntry.clear();
  snap.sceneToInfo.clear();
  snap.handleToInfo.clear();
  if (reserveEntry)
    snap.byEntry.reserve(reserveEntry);
  if (reserveScene)
    snap.sceneToInfo.reserve(reserveScene);
  if (reserveHandle)
    snap.handleToInfo.reserve(reserveHandle);
  const uint64_t frameNo = m_frameNumber.fetch_add(1, std::memory_order_relaxed) + 1u;
  if constexpr (dxvk::war3::internal::kBridgeRawcodeOneShotLogEnabled) {
    // 周期性清理 handle 级缓存，避免句柄复用导致的长期陈旧映射。
    if ((frameNo & 0x7FFu) == 0u) {
      auto &state = GetBridgeRawcodeState();
      state.handleToRawcode.clear();
      state.failedHandles.clear();
    }
  }
  if ((frameNo & 0x3FFu) == 0u) {
    auto &unitMetaCache = GetUnitMetaCache();
    if (unitMetaCache.size() > 131072u) {
      unitMetaCache.clear();
    }
  }
}

void RenderObjectRegistry::endFrame() {
  Snapshot &snap = m_snapshots[m_writeIndex];
  snap.lastEntryCount = snap.byEntry.size();
  snap.lastSceneCount = snap.sceneToInfo.size();
  snap.lastHandleCount = snap.handleToInfo.size();
  m_publishedIndex.store(m_writeIndex, std::memory_order_release);
}

void RenderObjectRegistry::registerWorldObject(void *worldObjectEntry,
                                               void *unitPtr, int groupIdx) {
  if (!worldObjectEntry)
    return;

  Snapshot &snap = writeSnapshot();
  RenderObjectInfo &info = snap.byEntry[worldObjectEntry];
  info.worldObjectEntry = worldObjectEntry;
  info.unitPtr = unitPtr;
  info.groupIdx = groupIdx;

  // 立即解析 Handle 和属性
  resolveObjectInfo(info);

  if (ShouldFeedShadowRuntimeRegistries()) {
    NoteShadowRuntimeRenderObject(info);
  }

  // 更新 Handle 索引
  if (info.jHandle != 0) {
    snap.handleToInfo[info.jHandle] = &info;
  }
}

void RenderObjectRegistry::registerWorldObjectsBatch(
    const std::vector<RenderObjectBatchItem> &items,
    RenderObjectBatchResolveMode mode) {
  if (items.empty())
    return;

  const bool resolveFull = mode == RenderObjectBatchResolveMode::FullResolve;
  const bool resolveMetadata = mode != RenderObjectBatchResolveMode::IdentityOnly;
  const bool feedShadowRuntime = ShouldFeedShadowRuntimeRegistries();

  Snapshot &snap = writeSnapshot();
  snap.byEntry.reserve(snap.byEntry.size() + items.size());
  snap.sceneToInfo.reserve(snap.sceneToInfo.size() + items.size());
  // 注意：即便 resolveFull=false，只要 SceneCollector 提供了 jHandle，我们仍会写入 handleToInfo。
  // 因此这里必须始终 reserve，否则在大场景下会频繁 rehash，导致 Hook_WorldObjects_RenderGroup 开销暴涨。
  snap.handleToInfo.reserve(snap.handleToInfo.size() + items.size());
  std::vector<const RenderObjectInfo *> shadowRuntimeInfos;
  if (feedShadowRuntime)
    shadowRuntimeInfos.reserve(items.size());

  for (const auto &item : items) {
    if (!item.worldObjectEntry)
      continue;

    RenderObjectInfo &info = snap.byEntry[item.worldObjectEntry];
    RenderObjectInfo *infoPtr = &info;
    void *prevUnitPtr = info.unitPtr;
    uint32_t prevHandle = info.jHandle;
    info.worldObjectEntry = item.worldObjectEntry;
    info.unitPtr = item.unitPtr;
    info.sceneNode = item.sceneNode;
    info.groupIdx = item.groupIdx;

    // 默认策略（性能优先）：
    // - 如果 SceneCollector 已经提供了 jHandle，则不再调用 HandleResolver（避免每帧全量解析）。
    // - 仅填充基础属性（rawcode/flags/kind），满足大多数 Shader/调试需求。
    // - 如需完整 agentType/agentPtr 等信息，可通过 resolveFull=true 强制走旧逻辑。

    info.handleId = 0;
    info.jHandle = 0;
    info.agentPtr = nullptr;
    info.agentType = 0;

    if (item.jHandle != 0) {
      info.jHandle = item.jHandle;
      info.handleId = GetHandleId(item.jHandle);
      snap.handleToInfo[info.jHandle] = infoPtr;
    }

    if (resolveFull) {
      // 旧行为：完整解析 Handle/Agent/Kind（成本较高）
      const bool shouldResolve = (prevUnitPtr != item.unitPtr) ||
                                 (prevHandle != info.jHandle) ||
                                 (info.kind == ObjectKind::Unknown);
      if (shouldResolve) {
        resolveObjectInfo(info);
      }
      if (info.jHandle != 0) {
        snap.handleToInfo[info.jHandle] = infoPtr;
      }
    } else if (resolveMetadata) {
      // 快速模式：仅解析基础字段（无需 HandleResolver）
      info.rawcode = 0;
      info.flags5C = 0;
      info.kind = ObjectKind::Unknown;
      RawcodeSource rawcodeSource = RawcodeSource::Unknown;
      bool resolvedUnitFromHandle = false;
      bool resolvedUnitFromHandleCache = false;

      if (info.unitPtr == nullptr && info.jHandle != 0) {
        info.unitPtr =
            ResolveUnitPtrByHandleCached(info.jHandle, &resolvedUnitFromHandleCache);
        resolvedUnitFromHandle = info.unitPtr != nullptr;
      }

      if (info.unitPtr) {
        const uint64_t cacheKey = BuildUnitMetaCacheKey(info.unitPtr, info.jHandle);
        auto &unitMetaCache = GetUnitMetaCache();
        auto cacheIt = unitMetaCache.find(cacheKey);
        if (cacheIt != unitMetaCache.end()) {
          info.rawcode = cacheIt->second.rawcode;
          info.flags5C = cacheIt->second.flags5C;
          info.kind = cacheIt->second.kind;
          if (info.rawcode != 0) {
            rawcodeSource =
                resolvedUnitFromHandle
                    ? (resolvedUnitFromHandleCache ? RawcodeSource::HandleCache
                                                   : RawcodeSource::HandleResolved)
                    : RawcodeSource::UnitPtr;
          }
        } else {
          UnitMetaCacheEntry meta = {};
          if (TryResolveUnitMetadataFast(info.unitPtr, meta)) {
            info.rawcode = meta.rawcode;
            info.flags5C = meta.flags5C;
            info.kind = meta.kind;
            unitMetaCache.emplace(
                cacheKey, UnitMetaCacheEntry{info.rawcode, info.flags5C, info.kind});
            if (info.rawcode != 0)
              rawcodeSource =
                  resolvedUnitFromHandle
                      ? (resolvedUnitFromHandleCache ? RawcodeSource::HandleCache
                                                     : RawcodeSource::HandleResolved)
                      : RawcodeSource::UnitPtr;
          }
        }
      }

      // 高效桥接兜底：当当前条目只有 jHandle 没有 unitPtr 时，
      // 仅在缓存 miss 时做一次逻辑层解析，后续直接走 handle->rawcode 缓存。
      if (info.rawcode == 0 && info.jHandle != 0) {
        bool fromCache = false;
        info.rawcode = ResolveRawcodeByHandleCached(info.jHandle, &fromCache);
        if (info.rawcode != 0) {
          rawcodeSource = fromCache ? RawcodeSource::HandleCache
                                    : RawcodeSource::HandleResolved;
        }
      }

      if constexpr (dxvk::war3::internal::kBridgeRawcodeOneShotLogEnabled) {
        LogRawcodeOnce(info.rawcode, info.jHandle, info.groupIdx, info.kind,
                       rawcodeSource);
      }
    } else {
      info.rawcode = 0;
      info.flags5C = 0;
      info.kind = ObjectKind::Unknown;
    }

    if (item.sceneNode) {
      snap.sceneToInfo[item.sceneNode] = infoPtr;
    }

    if (feedShadowRuntime)
      shadowRuntimeInfos.push_back(infoPtr);
  }

  if (feedShadowRuntime && !shadowRuntimeInfos.empty())
    NoteShadowRuntimeRenderObjectsBatch(shadowRuntimeInfos);
}

void RenderObjectRegistry::mapSceneNode(void *worldObjectEntry,
                                         void *sceneNode) {
  if (!worldObjectEntry || !sceneNode)
    return;

  Snapshot &snap = writeSnapshot();
  RenderObjectInfo &info = snap.byEntry[worldObjectEntry];
  info.worldObjectEntry = worldObjectEntry;
  info.sceneNode = sceneNode;
  snap.sceneToInfo[sceneNode] = &info;

  if (ShouldFeedShadowRuntimeRegistries()) {
    NoteShadowRuntimeRenderObject(info);
  }
}

bool RenderObjectRegistry::queryBySceneNode(void *sceneNode,
                                            RenderObjectInfo &out) const {
  if (!sceneNode)
    return false;

  const Snapshot &snap = snapshotForThread();

  auto itScene = snap.sceneToInfo.find(sceneNode);
  if (itScene == snap.sceneToInfo.end() || itScene->second == nullptr)
    return false;

  out = *itScene->second;
  return true;
}

const RenderObjectInfo *
RenderObjectRegistry::findBySceneNode(void *sceneNode) const {
  if (!sceneNode)
    return nullptr;

  const Snapshot &snap = snapshotForThread();
  auto itScene = snap.sceneToInfo.find(sceneNode);
  if (itScene == snap.sceneToInfo.end() || itScene->second == nullptr)
    return nullptr;
  return itScene->second;
}

bool RenderObjectRegistry::queryByEntry(void *worldObjectEntry,
                                        RenderObjectInfo &out) const {
  if (!worldObjectEntry)
    return false;

  const Snapshot &snap = snapshotForThread();
  auto it = snap.byEntry.find(worldObjectEntry);
  if (it == snap.byEntry.end())
    return false;

  out = it->second;
  return true;
}

const RenderObjectInfo *
RenderObjectRegistry::findByEntry(void *worldObjectEntry) const {
  if (!worldObjectEntry)
    return nullptr;

  const Snapshot &snap = snapshotForThread();
  auto it = snap.byEntry.find(worldObjectEntry);
  if (it == snap.byEntry.end())
    return nullptr;
  return &it->second;
}

const RenderObjectInfo *
RenderObjectRegistry::findByHandle(uint32_t jHandle) const {
  if (jHandle == 0)
    return nullptr;

  const Snapshot &snap = snapshotForThread();
  auto itHandle = snap.handleToInfo.find(jHandle);
  if (itHandle == snap.handleToInfo.end() || itHandle->second == nullptr)
    return nullptr;
  return itHandle->second;
}

std::vector<RenderObjectInfo> RenderObjectRegistry::getAllObjects() const {
  const Snapshot &snap = snapshotForThread();

  std::vector<RenderObjectInfo> result;
  result.reserve(snap.byEntry.size());

  for (const auto &pair : snap.byEntry) {
    result.push_back(pair.second);
  }

  return result;
}

std::vector<RenderObjectInfo>
RenderObjectRegistry::getObjectsByKind(ObjectKind kind) const {
  const Snapshot &snap = snapshotForThread();

  std::vector<RenderObjectInfo> result;

  for (const auto &pair : snap.byEntry) {
    if (pair.second.kind == kind) {
      result.push_back(pair.second);
    }
  }

  return result;
}

std::vector<RenderObjectInfo> RenderObjectRegistry::getUnits() const {
  return getObjectsByKind(ObjectKind::Unit);
}

std::vector<RenderObjectInfo> RenderObjectRegistry::getBuildings() const {
  return getObjectsByKind(ObjectKind::Building);
}

bool RenderObjectRegistry::getObjectByIndex(size_t index,
                                            RenderObjectInfo &out) const {
  const Snapshot &snap = snapshotForThread();
  if (index >= snap.byEntry.size())
    return false;

  size_t current = 0;
  for (const auto &pair : snap.byEntry) {
    if (current == index) {
      out = pair.second;
      return true;
    }
    ++current;
  }
  return false;
}

size_t RenderObjectRegistry::getObjectCount() const {
  const Snapshot &snap = snapshotForThread();
  return snap.byEntry.size();
}

size_t RenderObjectRegistry::getSceneNodeMappingCount() const {
  const Snapshot &snap = snapshotForThread();
  return snap.sceneToInfo.size();
}

void RenderObjectRegistry::resolveObjectInfo(RenderObjectInfo &info) const {
  game::UnitWrapper unit(info.unitPtr);

  if (!unit.IsValid())
    return;

  // 读取 CUnit 属性
  info.rawcode = unit.GetRawcode();
  info.flags5C = unit.GetFlags5C();

  // 解析 Handle 和 Agent 信息
  uint32_t handleId = info.handleId;
  void *agentPtr = info.agentPtr;

  bool found = false;
  if (handleId != 0) {
    if (agentPtr == nullptr) {
      HandleResolver::instance().resolveHandle(handleId, 0, &agentPtr);
    }
    found = true;
  } else {
    // 只有在完全没有 Handle 且 unitPtr 有效时才进行线性扫描（代价高）
    // 建议仅在非渲染线程或低频触发
    found = HandleResolver::instance().findHandleByUnitPtr(
        info.unitPtr, &handleId, &agentPtr);
  }

  if (found) {
    info.handleId = handleId;
    info.jHandle = MakeJHandle(handleId);
    info.agentPtr = agentPtr;

    // 使用 AgentWrapper 读取类型
    if (agentPtr) {
      game::AgentWrapper agent(agentPtr);
      info.agentType = agent.GetTypeFourCC();
    }
  }

  // 兼容：handle 表返回 CUnit 指针时，避免用 AgentWrapper 解析
  if (info.agentPtr && info.agentPtr == info.unitPtr) {
    info.agentPtr = nullptr;
    info.agentType = 0;

    static std::atomic<uint32_t> s_logged{0};
    if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
      WAR3_LOG_DEBUG("RenderObjectRegistry: handle points to CUnit "
                     "(unitPtr=0x%p), skip agent type\n",
                     info.unitPtr);
    }
  }

  // ========================================================================
  // Fallback: 如果 HandleResolver 失败，尝试直接从 unitPtr 读取 agentType
  // ========================================================================
  // 风险：对于普通单位，unitPtr 指向 CUnit，offset 0x10 可能是随机数据。
  // 因此，我们读取后必须进行白名单校验，只接受明确的 Destructible/Item 标识。
  //
  if (info.agentType == 0) {
    game::AgentWrapper agentFromUnitPtr(info.unitPtr);
    if (agentFromUnitPtr.IsValid()) {
      uint32_t potentialType = agentFromUnitPtr.GetTypeFourCC();

      // 白名单校验：只接受已知的非 Unit 类型
      bool isValidType = (potentialType == AgentTypeFourCC::Destructible_LE) ||
                         (potentialType == AgentTypeFourCC::DestructibleID) ||
                         (potentialType == AgentTypeFourCC::Item_LE) ||
                         (potentialType == AgentTypeFourCC::Item);

      if (isValidType) {
        info.agentType = potentialType;
        if (info.agentPtr == nullptr) {
          info.agentPtr = info.unitPtr;
        }
      }
    }
  }

  // 确定对象类型 (逻辑封装在 UnitWrapper 中)
  info.kind = unit.GetKind(info.agentType);
}

} // namespace render
} // namespace war3
} // namespace dxvk
