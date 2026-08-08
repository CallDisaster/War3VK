// war3_hook_widget_identity.cpp - CWidget identity sync hook impl
//
// Read-only hook for CWidget_RegisterFootprintAndShadowMask @ 0x6F65A140.
// We only read widget+0x0C (magic) and widget+0x30 (rawcode), then push the
// (widgetPtr -> {rawcode, jHandle, kind}) mapping into a map-session cache so
// that destructible/path-blocker shadow filtering, outline matching,
// and bloom matching can resolve identity without depending on
// Hook_WorldObjects_RenderGroup (which destructibles never pass through).

#include "war3_hook_widget_identity.h"

#include "war3_hook_install_util.h"
#include "war3_hook_perf.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../debug/war3_debug.h"
#include "../game/war3_unit.h"
#include "../game/war3_agent.h"
#include "../handle/war3_handle_resolver.h"

#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "../../../util/log/log.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace dxvk::war3::hooks {

namespace {

// CWidget runtime type signature (paper 06_fogmask_static_shadow chapter 6).
// Stamped at widget+0x0C on every CWidget-derived object that is registered
// to the live game world. CDoodads do NOT have this magic. Verified across
// CDestructable / CUnit / CBuilding lifecycle helpers in IDA.
constexpr uint32_t kWidgetTypeMagic = 0x2B5DB42Cu;

// Field offsets shared by CWidget / CUnit / CDestructable / CBuilding.
constexpr size_t kOffsetTypeMagic = 0x0C;
constexpr size_t kOffsetRawcode = 0x30;

// Hooked function prototype (from IDA decompile of 0x6F65A140).
// IMPORTANT (Phase 7.99 实测发现)：IDA 反编译把这个函数标成 __fastcall 是错的。
// caller `CDestructable_create_Shadow` 在 0x6F4081B7 处 `mov edx, esi` 把
// **fourcc 传到 EDX**（不是 widget instance）。运行时 hook 实测到的 EDX 都是
// 'YTab/YTac' 这类 fourcc 字符串。真实调用约定看 caller `mov ecx, ebx; mov edx, esi`，
// 但这是某种"先准备 ECX/EDX 再 push stack"模式：caller 之外还会 push 8 个
// stack 参数。实际有效的"widget"参数其实在某个 stack 偏移上。
//
// 我们改用 __cdecl 接 8 个 stack 参，并保留 ECX/EDX 用裸 asm 抓取。
using WidgetRegisterFootprintFn = int(__fastcall*)(
    int a1,
    void* widgetPtr,
    int* posXYZ,
    float* posY,
    int a5,
    int a6,
    int a7,
    int a8);

WidgetRegisterFootprintFn g_originalWidgetRegister = nullptr;
WidgetRegisterFootprintFn g_trampolineWidgetRegister = nullptr;
std::atomic<uintptr_t> g_widgetIdentityGameBase{0u};

// One identity record per widget. Addresses and handles may be reused by the
// next map, so Present-owned map transition code clears both indexes.
struct WidgetIdentityRecord {
  void* widgetPtr = nullptr;
  uint32_t rawcode = 0;
  uint32_t jHandle = 0;
  uint32_t handleId = 0;
  void* agentPtr = nullptr;
  uint32_t agentType = 0;
  dxvk::war3::render::ObjectKind kind =
      dxvk::war3::render::ObjectKind::Unknown;
  std::atomic<uint64_t> hitCount{0};

  WidgetIdentityRecord() = default;
  WidgetIdentityRecord(const WidgetIdentityRecord& other) {
    widgetPtr = other.widgetPtr;
    rawcode = other.rawcode;
    jHandle = other.jHandle;
    handleId = other.handleId;
    agentPtr = other.agentPtr;
    agentType = other.agentType;
    kind = other.kind;
    hitCount.store(other.hitCount.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
  }
};

struct WidgetIdentityCache {
  // Reader-heavy: shadow filter and outline lookup hit this on every miss
  // packet; writes happen only on lifecycle events (CDestructable_create,
  // CUnit_setHidden, etc.) - low-frequency relative to draw rate.
  mutable std::shared_mutex mutex;
  std::unordered_map<void*, WidgetIdentityRecord> byPtr;
  std::unordered_map<uint32_t, void*> byHandle;
};

WidgetIdentityCache& cache() {
  static WidgetIdentityCache s_cache;
  return s_cache;
}

struct GlobalStats {
  std::atomic<uint64_t> enterCount{0};
  std::atomic<uint64_t> magicMatchedCount{0};
  std::atomic<uint64_t> magicMismatchCount{0};
  std::atomic<uint64_t> cacheInsertCount{0};
  std::atomic<uint64_t> cacheUpdateCount{0};
  std::atomic<uint64_t> handleResolvedCount{0};
  std::atomic<uint64_t> handleMissingCount{0};
  // Phase 7.99 install 状态诊断
  std::atomic<uint64_t> installAttempted{0};
  std::atomic<uint64_t> installSucceeded{0};
  std::atomic<uint64_t> installFailedAddrNull{0};
  std::atomic<uint64_t> installFailedEnvDisabled{0};
  std::atomic<uint64_t> installFailedMinHook{0};
  std::atomic<uint32_t> lastCallerRva{0u};
  std::atomic<uint32_t> lastArgumentMask{0u};
  std::atomic<uint64_t> callerChangeCount{0u};
  std::atomic<uint64_t> callerOutsideGameModuleCount{0u};
  std::atomic<uint64_t> lifecycleObserverCount{0u};
};

GlobalStats& stats() {
  static GlobalStats s_stats;
  return s_stats;
}

bool WidgetIdentityHookEnabled() {
  if constexpr (!dxvk::war3::internal::kWar3ShadowHookEnabled) {
    return false;
  }
  static const bool s_enabled = []() {
    const char* env = std::getenv("DXVK_WAR3_WIDGET_IDENTITY_HOOK");
    if (env == nullptr || env[0] == '\0')
      return true;
    return env[0] != '0';
  }();
  return s_enabled;
}

uint32_t GetWidgetRegisterCallerRva(uintptr_t callerAddress) {
  const uintptr_t gameBase =
      g_widgetIdentityGameBase.load(std::memory_order_relaxed);
  if (gameBase == 0u || callerAddress < gameBase) {
    stats().callerOutsideGameModuleCount.fetch_add(
        1u, std::memory_order_relaxed);
    return 0u;
  }
  const uintptr_t rva = callerAddress - gameBase;
  if (rva > 0x02000000u) {
    stats().callerOutsideGameModuleCount.fetch_add(
        1u, std::memory_order_relaxed);
    return 0u;
  }
  return static_cast<uint32_t>(rva);
}

void ObserveWidgetLifecycleCaller(
    uint32_t callerRva,
    int a1,
    void* widgetPtr,
    int* posXYZ,
    float* posY,
    int a5,
    int a6,
    int a7,
    int a8) {
  // Preserve only a compact, non-semantic argument shape. Do not infer
  // Hidden/Removed from these bits: several verified callers reuse this
  // function for create, move and footprint refresh operations.
  uint32_t mask = 0u;
  mask |= a1 != 0 ? 1u << 0 : 0u;
  mask |= widgetPtr != nullptr ? 1u << 1 : 0u;
  mask |= posXYZ != nullptr ? 1u << 2 : 0u;
  mask |= posY != nullptr ? 1u << 3 : 0u;
  mask |= a5 != 0 ? 1u << 4 : 0u;
  mask |= a6 != 0 ? 1u << 5 : 0u;
  mask |= a7 != 0 ? 1u << 6 : 0u;
  mask |= a8 != 0 ? 1u << 7 : 0u;
  stats().lastArgumentMask.store(mask, std::memory_order_relaxed);
  const uint32_t previous =
      stats().lastCallerRva.exchange(callerRva, std::memory_order_relaxed);
  if (previous != 0u && callerRva != 0u && previous != callerRva) {
    stats().callerChangeCount.fetch_add(1u, std::memory_order_relaxed);
  }
  stats().lifecycleObserverCount.fetch_add(1u, std::memory_order_relaxed);
}

// Best-effort kind classification using agent type or unit flags.
dxvk::war3::render::ObjectKind GuessKindFromAgent(uint32_t agentType,
                                                   uint32_t flags5C) {
  using namespace dxvk::war3::render;
  if (agentType == AgentTypeFourCC::Destructible_LE ||
      agentType == AgentTypeFourCC::DestructibleID) {
    return ObjectKind::Destructible;
  }
  if (agentType == AgentTypeFourCC::Item_LE ||
      agentType == AgentTypeFourCC::Item) {
    return ObjectKind::Item;
  }
  // Same heuristic as RenderObjectRegistry::GuessObjectKindFromUnitFlags.
  const uint32_t exponent = (flags5C >> 23) & 0xFFu;
  if (exponent >= 0x3Eu && exponent <= 0x43u &&
      (flags5C & 0x007FFFFFu) != 0u) {
    return ObjectKind::Destructible;
  }
  if ((flags5C & 0x80000000u) != 0u || flags5C > 0x7FFFFFFFu) {
    return ObjectKind::Destructible;
  }
  if ((flags5C & UnitFlags5C::Building) != 0u) {
    return ObjectKind::Building;
  }
  return ObjectKind::Unit;
}

void ProcessWidgetRegister(void* widgetPtr) {
  if (widgetPtr == nullptr)
    return;

  stats().enterCount.fetch_add(1, std::memory_order_relaxed);

  // Bound check before any field reads.
  if (!IsReadableRangeFast(widgetPtr, 0x40))
    return;

  // CWidget magic check: 0x2B5DB42C. Non-CWidget objects (CDoodads etc.)
  // are skipped here, see paper chapter 6 section 6 for the rationale.
  uint32_t magic = 0;
  if (!SafeReadU32Fast(widgetPtr, kOffsetTypeMagic, magic) ||
      magic != kWidgetTypeMagic) {
    stats().magicMismatchCount.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  stats().magicMatchedCount.fetch_add(1, std::memory_order_relaxed);

  uint32_t rawcode = 0;
  if (!SafeReadU32Fast(widgetPtr, kOffsetRawcode, rawcode))
    return;

  uint32_t flags5C = 0;
  SafeReadU32Fast(widgetPtr, CUnitOffsets::Flags5C, flags5C);

  // Fast path: shared_lock probes existing record. If rawcode is unchanged
  // and we already have a jHandle, count the hit and return without writing.
  bool needsInsertOrUpdate = false;
  {
    std::shared_lock<std::shared_mutex> rl(cache().mutex);
    auto it = cache().byPtr.find(widgetPtr);
    if (it != cache().byPtr.end()) {
      const auto& rec = it->second;
      if (rec.rawcode == rawcode && rec.jHandle != 0) {
        it->second.hitCount.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      needsInsertOrUpdate = true;
    } else {
      needsInsertOrUpdate = true;
    }
  }
  if (!needsInsertOrUpdate)
    return;

  // Slow path: resolve jHandle + agent metadata. findHandleByUnitPtr scans
  // the handle table; this only fires once per widget per lifecycle change.
  uint32_t handleId = 0;
  void* agentPtr = nullptr;
  bool foundHandle = HandleResolver::instance().findHandleByUnitPtr(
      widgetPtr, &handleId, &agentPtr);
  if (foundHandle) {
    stats().handleResolvedCount.fetch_add(1, std::memory_order_relaxed);
  } else {
    stats().handleMissingCount.fetch_add(1, std::memory_order_relaxed);
  }

  uint32_t agentType = 0;
  if (agentPtr != nullptr && agentPtr != widgetPtr) {
    dxvk::war3::game::AgentWrapper agent(agentPtr);
    agentType = agent.GetTypeFourCC();
  } else if (agentPtr == widgetPtr) {
    // Handle table directly stored a CUnit*; do not run AgentWrapper.
    agentPtr = nullptr;
  }

  const uint32_t jHandle = handleId != 0 ? (handleId | 0x100000u) : 0u;

  WidgetIdentityRecord rec;
  rec.widgetPtr = widgetPtr;
  rec.rawcode = rawcode;
  rec.jHandle = jHandle;
  rec.handleId = handleId;
  rec.agentPtr = agentPtr;
  rec.agentType = agentType;
  rec.kind = GuessKindFromAgent(agentType, flags5C);

  {
    std::unique_lock<std::shared_mutex> wl(cache().mutex);
    auto it = cache().byPtr.find(widgetPtr);
    if (it == cache().byPtr.end()) {
      auto [insertedIt, inserted] =
          cache().byPtr.emplace(widgetPtr, std::move(rec));
      if (inserted) {
        stats().cacheInsertCount.fetch_add(1, std::memory_order_relaxed);
        if (jHandle != 0)
          cache().byHandle[jHandle] = widgetPtr;
      }
    } else {
      // Update: rawcode might have changed (rare) or jHandle was missing.
      auto& existing = it->second;
      if (existing.jHandle != jHandle && jHandle != 0)
        cache().byHandle[jHandle] = widgetPtr;
      existing.rawcode = rawcode;
      if (jHandle != 0) {
        existing.jHandle = jHandle;
        existing.handleId = handleId;
      }
      if (agentPtr != nullptr) {
        existing.agentPtr = agentPtr;
        existing.agentType = agentType;
      }
      existing.kind = rec.kind;
      stats().cacheUpdateCount.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

int __fastcall HookedWidgetRegisterFootprintAndShadowMask(
    int a1,
    void* widgetPtr,
    int* posXYZ,
    float* posY,
    int a5,
    int a6,
    int a7,
    int a8) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WidgetRegisterFootprint, 8u);
  uintptr_t callerAddress = 0u;
#if defined(_MSC_VER)
  callerAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__)
  callerAddress =
      reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
  const uint32_t callerRva =
      GetWidgetRegisterCallerRva(callerAddress);
  ObserveWidgetLifecycleCaller(
      callerRva, a1, widgetPtr, posXYZ, posY, a5, a6, a7, a8);
  // Always proxy to the original game function, even on internal failure.
  if (WidgetIdentityHookEnabled())
    ProcessWidgetRegister(widgetPtr);

  if (g_trampolineWidgetRegister) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return g_trampolineWidgetRegister(a1, widgetPtr, posXYZ, posY, a5, a6, a7,
                                       a8);
  }
  return 0;
}

}  // namespace

void ResetWidgetIdentityMapSession() {
  std::unique_lock<std::shared_mutex> lock(cache().mutex);
  cache().byPtr.clear();
  cache().byHandle.clear();
}

bool InstallWidgetIdentityHook(void* widgetRegisterAddr) {
  // Phase 7.99：避免重复 install（早装 + 常规 install 会调两次）。
  static std::atomic<bool> s_installed{false};
  if (s_installed.load(std::memory_order_acquire)) {
    stats().installAttempted.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  stats().installAttempted.fetch_add(1, std::memory_order_relaxed);
  if (widgetRegisterAddr == nullptr) {
    stats().installFailedAddrNull.fetch_add(1, std::memory_order_relaxed);
    war3dbg::Print(
        "DXVK War3Hook: WidgetIdentityHook skipped - address unresolved\n");
    return false;
  }
  if (!WidgetIdentityHookEnabled()) {
    stats().installFailedEnvDisabled.fetch_add(1, std::memory_order_relaxed);
    war3dbg::Print(
        "DXVK War3Hook: WidgetIdentityHook disabled by env\n");
    return false;
  }

  g_originalWidgetRegister =
      reinterpret_cast<WidgetRegisterFootprintFn>(widgetRegisterAddr);
  const HMODULE gameModule = ::GetModuleHandleA("Game.dll");
  g_widgetIdentityGameBase.store(
      reinterpret_cast<uintptr_t>(gameModule), std::memory_order_release);

  const bool ok = InstallMinHook(
      widgetRegisterAddr,
      reinterpret_cast<LPVOID>(&HookedWidgetRegisterFootprintAndShadowMask),
      reinterpret_cast<LPVOID*>(&g_trampolineWidgetRegister),
      "WidgetIdentity",
      "CWidget_RegisterFootprintAndShadowMask",
      /*ensureInitialized=*/false,
      /*logSuccess=*/true);
  if (ok) {
    stats().installSucceeded.fetch_add(1, std::memory_order_relaxed);
    s_installed.store(true, std::memory_order_release);
  } else {
    stats().installFailedMinHook.fetch_add(1, std::memory_order_relaxed);
  }
  return ok;
}

bool QueryWidgetIdentityByPtr(
    void* widgetPtr,
    dxvk::war3::render::RenderObjectInfo& out) {
  if (widgetPtr == nullptr)
    return false;
  std::shared_lock<std::shared_mutex> rl(cache().mutex);
  auto it = cache().byPtr.find(widgetPtr);
  if (it == cache().byPtr.end())
    return false;
  const auto& rec = it->second;
  out = {};
  out.unitPtr = rec.widgetPtr;
  out.agentPtr = rec.agentPtr;
  out.handleId = rec.handleId;
  out.jHandle = rec.jHandle;
  out.rawcode = rec.rawcode;
  out.agentType = rec.agentType;
  out.kind = rec.kind;
  return true;
}

bool QueryWidgetIdentityByHandle(
    uint32_t jHandle,
    dxvk::war3::render::RenderObjectInfo& out) {
  if (jHandle == 0)
    return false;
  std::shared_lock<std::shared_mutex> rl(cache().mutex);
  auto handleIt = cache().byHandle.find(jHandle);
  if (handleIt == cache().byHandle.end())
    return false;
  auto ptrIt = cache().byPtr.find(handleIt->second);
  if (ptrIt == cache().byPtr.end())
    return false;
  const auto& rec = ptrIt->second;
  out = {};
  out.unitPtr = rec.widgetPtr;
  out.agentPtr = rec.agentPtr;
  out.handleId = rec.handleId;
  out.jHandle = rec.jHandle;
  out.rawcode = rec.rawcode;
  out.agentType = rec.agentType;
  out.kind = rec.kind;
  return true;
}

uint32_t QueryWidgetRawcodeByPtr(void* widgetPtr) {
  if (widgetPtr == nullptr)
    return 0;
  std::shared_lock<std::shared_mutex> rl(cache().mutex);
  auto it = cache().byPtr.find(widgetPtr);
  if (it == cache().byPtr.end())
    return 0;
  return it->second.rawcode;
}

uint32_t QueryWidgetRawcodeByHandle(uint32_t jHandle) {
  if (jHandle == 0)
    return 0;
  std::shared_lock<std::shared_mutex> rl(cache().mutex);
  auto handleIt = cache().byHandle.find(jHandle);
  if (handleIt == cache().byHandle.end())
    return 0;
  auto ptrIt = cache().byPtr.find(handleIt->second);
  if (ptrIt == cache().byPtr.end())
    return 0;
  return ptrIt->second.rawcode;
}

void NoteWidgetIdentityFromDrawcall(
    void* widgetPtr,
    uint32_t rawcode,
    uint32_t jHandle) {
  // Phase 7.101 write-through：D3D9 draw call 路径已经直读 magic + rawcode 验证，
  // 把结果写回 widget cache，下次同 widgetPtr/jHandle 直接 O(1) 命中。
  // 与 ProcessWidgetRegister 的 cache 复用同一份 std::shared_mutex / unordered_map。
  if (widgetPtr == nullptr || rawcode == 0)
    return;
  if (!WidgetIdentityHookEnabled())
    return;

  // Quick read-only probe: 已存在且 rawcode/jHandle 一致就跳过 unique_lock。
  {
    std::shared_lock<std::shared_mutex> rl(cache().mutex);
    auto it = cache().byPtr.find(widgetPtr);
    if (it != cache().byPtr.end()) {
      const auto& rec = it->second;
      if (rec.rawcode == rawcode && (jHandle == 0 || rec.jHandle == jHandle)) {
        it->second.hitCount.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  WidgetIdentityRecord rec;
  rec.widgetPtr = widgetPtr;
  rec.rawcode = rawcode;
  rec.jHandle = jHandle;
  rec.handleId = (jHandle != 0u) ? (jHandle & ~0x100000u) : 0u;
  rec.agentPtr = nullptr;
  rec.agentType = 0;
  rec.kind = dxvk::war3::render::ObjectKind::Unknown;

  std::unique_lock<std::shared_mutex> wl(cache().mutex);
  auto it = cache().byPtr.find(widgetPtr);
  if (it == cache().byPtr.end()) {
    auto [insertedIt, inserted] =
        cache().byPtr.emplace(widgetPtr, std::move(rec));
    if (inserted) {
      stats().cacheInsertCount.fetch_add(1, std::memory_order_relaxed);
      if (jHandle != 0u)
        cache().byHandle[jHandle] = widgetPtr;
    }
  } else {
    auto& existing = it->second;
    bool changed = false;
    if (existing.rawcode != rawcode) {
      existing.rawcode = rawcode;
      changed = true;
    }
    if (jHandle != 0u && existing.jHandle != jHandle) {
      existing.jHandle = jHandle;
      existing.handleId = jHandle & ~0x100000u;
      cache().byHandle[jHandle] = widgetPtr;
      changed = true;
    }
    if (changed)
      stats().cacheUpdateCount.fetch_add(1, std::memory_order_relaxed);
  }
}

uint64_t GetWidgetIdentityCacheSize() {
  std::shared_lock<std::shared_mutex> rl(cache().mutex);
  return cache().byPtr.size();
}

WidgetIdentityHookStats GetWidgetIdentityHookStats() {
  WidgetIdentityHookStats out;
  out.enterCount = stats().enterCount.load(std::memory_order_relaxed);
  out.magicMatchedCount =
      stats().magicMatchedCount.load(std::memory_order_relaxed);
  out.magicMismatchCount =
      stats().magicMismatchCount.load(std::memory_order_relaxed);
  out.cacheInsertCount =
      stats().cacheInsertCount.load(std::memory_order_relaxed);
  out.cacheUpdateCount =
      stats().cacheUpdateCount.load(std::memory_order_relaxed);
  out.handleResolvedCount =
      stats().handleResolvedCount.load(std::memory_order_relaxed);
  out.handleMissingCount =
      stats().handleMissingCount.load(std::memory_order_relaxed);
  out.installAttempted =
      stats().installAttempted.load(std::memory_order_relaxed);
  out.installSucceeded =
      stats().installSucceeded.load(std::memory_order_relaxed);
  out.installFailedAddrNull =
      stats().installFailedAddrNull.load(std::memory_order_relaxed);
  out.installFailedEnvDisabled =
      stats().installFailedEnvDisabled.load(std::memory_order_relaxed);
  out.installFailedMinHook =
      stats().installFailedMinHook.load(std::memory_order_relaxed);
  out.lastCallerRva =
      stats().lastCallerRva.load(std::memory_order_relaxed);
  out.lastArgumentMask =
      stats().lastArgumentMask.load(std::memory_order_relaxed);
  out.callerChangeCount =
      stats().callerChangeCount.load(std::memory_order_relaxed);
  out.callerOutsideGameModuleCount =
      stats().callerOutsideGameModuleCount.load(std::memory_order_relaxed);
  out.lifecycleObserverCount =
      stats().lifecycleObserverCount.load(std::memory_order_relaxed);
  return out;
}

}  // namespace dxvk::war3::hooks
