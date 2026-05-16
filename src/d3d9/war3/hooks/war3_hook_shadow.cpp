// Phase 7.100 marker bump 102042
#include "war3_hook_shadow.h"
#include "war3_hook_install_util.h"
#include "war3_shadow_filter_policy.h"

#include "../../d3d9_war3_debug.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_memory.h"
#include "../native/war3_native_shadow_hint.h"
#include "../render/war3_render_state.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include "../../../util/log/log.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace dxvk::war3::hooks {

#define WAR3_HOOK_HOTPATH_LOG(fmt, ...)                                        \
  do {                                                                         \
    if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {    \
      WAR3_RENDER_LOG(fmt, ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

namespace {

// ---------------------------------------------------------------------------
// 阴影域本地函数与 trampoline 指针
// ---------------------------------------------------------------------------

using dxvk::war3::IsReadableRange;

using TerrainRenderShadowLayerFn = void(__fastcall *)(void *, void *, int, int,
                                                      int);
using TerrainRenderListAFn = int(__fastcall *)(void *, void *, void *);
using TerrainRenderListBFn = void(__fastcall *)(void *, void *, int, int);
using ShadowUpdateWriteEntryFn = int(__fastcall *)(void *, void *, void *,
                                                   float);
using ShadowProjectorAddFromObjectFn = int(__fastcall *)(void *, void *, void *,
                                                         int, int);
using ShadowProjectorAddSimpleFn = int(__fastcall *)(void *, void *, int, int,
                                                     int, int, int, int, int);
using ShadowPathStaticStampToggleFn = void(__fastcall *)(void *, void *, int,
                                                         int);

TerrainRenderShadowLayerFn g_originalTerrainShadowLayer = nullptr;
TerrainRenderShadowLayerFn g_trampolineTerrainShadowLayer = nullptr;
TerrainRenderListAFn g_originalTerrainRenderListA = nullptr;
TerrainRenderListAFn g_trampolineTerrainRenderListA = nullptr;
TerrainRenderListBFn g_originalTerrainRenderListB = nullptr;
TerrainRenderListBFn g_trampolineTerrainRenderListB = nullptr;
ShadowUpdateWriteEntryFn g_originalShadowUpdateWriteEntry = nullptr;
ShadowUpdateWriteEntryFn g_trampolineShadowUpdateWriteEntry = nullptr;
ShadowProjectorAddFromObjectFn g_originalShadowProjectorAddFromObject = nullptr;
ShadowProjectorAddFromObjectFn g_trampolineShadowProjectorAddFromObject =
    nullptr;
ShadowProjectorAddSimpleFn g_originalShadowProjectorAddSimple = nullptr;
ShadowProjectorAddSimpleFn g_trampolineShadowProjectorAddSimple = nullptr;
ShadowPathStaticStampToggleFn g_originalShadowPathStaticStampToggle = nullptr;
ShadowPathStaticStampToggleFn g_trampolineShadowPathStaticStampToggle = nullptr;
uintptr_t g_shadowPathObjectProjectorRuntimeAddr = 0;
uintptr_t g_shadowPathObjectProjectorJassBridgeAddr = 0;
uintptr_t g_shadowProjectorSimpleBridgeAddr = 0;

// Phase 7.108：ShadowProjector 永久 atomic 计数器。
// 这条路径独立于 D3D9 mesh draw（CTerrainUberSplats 系统），
// 必须永久计数才能在不开 verbose 编译期开关时也判断
// path blocker 是否走这条路径。
std::atomic<uint64_t> g_projectorAddFromObjectEnterCount{0};
std::atomic<uint64_t> g_projectorAddFromObjectBlockedCount{0};
std::atomic<uint64_t> g_projectorAddFromObjectFourCCExtractedCount{0};
std::atomic<uint64_t> g_projectorAddFromObjectFourCCMissCount{0};
std::atomic<uint64_t> g_projectorAddFromObjectBlockedFourCCCount{0};
std::atomic<uint64_t> g_projectorAddSimpleEnterCount{0};
std::atomic<uint64_t> g_projectorAddSimpleBlockedCount{0};

// 环形采样：被 reject 的 fourcc 与近期 observed fourcc（前 8 个 unique）。
std::array<std::atomic<uint32_t>, 8> g_projectorBlockedFourCCSamples{};
std::array<std::atomic<uint32_t>, 8> g_projectorObservedFourCCSamples{};

// 获取 Game.dll 基址，供回调 RVA 统计与路径识别使用。
uintptr_t GetGameDllBase() {
  return reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
}

// 安全读取 32 位字段，避免无效地址导致访问违规。
bool ReadU32Safe(const void *base, size_t offset, uint32_t &outValue) {
  outValue = 0;
  if (!base)
    return false;
  const auto *p =
      reinterpret_cast<const uint8_t *>(base) + static_cast<uintptr_t>(offset);
  if (!IsReadableRange(p, sizeof(uint32_t)))
    return false;
  outValue = *reinterpret_cast<const uint32_t *>(p);
  return true;
}

// 判断返回地址是否落在指定函数范围内，用于识别调用来源路径。
bool IsAddrInFuncRange(uintptr_t addr, uintptr_t funcStart, size_t funcSize) {
  if (!addr || !funcStart || funcSize == 0)
    return false;
  return (addr >= funcStart) && (addr < (funcStart + funcSize));
}

// 读取当前调用点返回地址（编译器相关实现）。
uintptr_t GetCallReturnAddress() {
#if defined(_MSC_VER)
  return reinterpret_cast<uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__)
  return reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#else
  return 0;
#endif
}

int CallShadowProjectorAddFromObjectOriginal(void* a1, void* a2, void* arg0,
                                             int arg1, int arg2) {
  if (g_trampolineShadowProjectorAddFromObject)
    return g_trampolineShadowProjectorAddFromObject(a1, a2, arg0, arg1, arg2);
  if (g_originalShadowProjectorAddFromObject)
    return g_originalShadowProjectorAddFromObject(a1, a2, arg0, arg1, arg2);
  return -1;
}

int CallShadowProjectorAddSimpleOriginal(void* a1, void* a2, int arg0, int arg1,
                                         int arg2, int arg3, int arg4, int arg5,
                                         int arg6) {
  if (g_trampolineShadowProjectorAddSimple) {
    return g_trampolineShadowProjectorAddSimple(a1, a2, arg0, arg1, arg2, arg3,
                                                arg4, arg5, arg6);
  }
  if (g_originalShadowProjectorAddSimple) {
    return g_originalShadowProjectorAddSimple(a1, a2, arg0, arg1, arg2, arg3,
                                              arg4, arg5, arg6);
  }
  return -1;
}

void CallShadowPathStaticStampToggleOriginal(void* thisPtr,
                                             int shadowObjectPtr,
                                             int enable) {
  if (g_trampolineShadowPathStaticStampToggle) {
    g_trampolineShadowPathStaticStampToggle(thisPtr, nullptr, shadowObjectPtr,
                                            enable);
    return;
  }
  if (g_originalShadowPathStaticStampToggle) {
    g_originalShadowPathStaticStampToggle(thisPtr, nullptr, shadowObjectPtr,
                                          enable);
  }
}

struct ShadowCallbackStatEntry {
  uint32_t rva = 0u;
  uint32_t count = 0u;
};
std::array<ShadowCallbackStatEntry, 128> g_shadowCallbackStats = {};

// 以固定开地址哈希方式记录 callback RVA 频次，避免引入动态分配。
void RecordShadowCallbackRva(uint32_t rva) {
  if (rva == 0u)
    return;

  constexpr uint32_t kMask = 127u;
  uint32_t slot = (rva * 2654435761u) & kMask;
  for (uint32_t i = 0; i < 8u; ++i) {
    ShadowCallbackStatEntry &e = g_shadowCallbackStats[(slot + i) & kMask];
    if (e.rva == rva) {
      ++e.count;
      return;
    }
    if (e.rva == 0u) {
      e.rva = rva;
      e.count = 1u;
      return;
    }
  }
}

// 输出 TopN（当前为 Top4）回调频次，辅助定位上游阴影写入来源。
void DumpShadowCallbackTopStats(uint32_t calls, uint32_t mode) {
  struct Pair {
    uint32_t rva = 0u;
    uint32_t count = 0u;
  };
  Pair top1 = {};
  Pair top2 = {};
  Pair top3 = {};
  Pair top4 = {};
  for (const auto &e : g_shadowCallbackStats) {
    if (e.rva == 0u || e.count == 0u)
      continue;
    Pair p{e.rva, e.count};
    if (p.count > top1.count) {
      top4 = top3;
      top3 = top2;
      top2 = top1;
      top1 = p;
    } else if (p.count > top2.count) {
      top4 = top3;
      top3 = top2;
      top2 = p;
    } else if (p.count > top3.count) {
      top4 = top3;
      top3 = p;
    } else if (p.count > top4.count) {
      top4 = p;
    }
  }

  war3dbg::Print(
      "DXVK War3Hook: ShadowUpdate cbTop mode=%u calls=%u "
      "top1=0x%08X/%u top2=0x%08X/%u top3=0x%08X/%u top4=0x%08X/%u\n",
      static_cast<unsigned>(mode), static_cast<unsigned>(calls),
      static_cast<unsigned>(top1.rva), static_cast<unsigned>(top1.count),
      static_cast<unsigned>(top2.rva), static_cast<unsigned>(top2.count),
      static_cast<unsigned>(top3.rva), static_cast<unsigned>(top3.count),
      static_cast<unsigned>(top4.rva), static_cast<unsigned>(top4.count));
}

// 地形阴影层入口：
// - mode=1：关闭该入口触发的 ListB（stage14 直调链路由 ListB Hook 处理）
// - mode>=2：完全禁用原生阴影层
void __fastcall Hook_Terrain_RenderShadowLayer(void *thisPtr, void *edx, int a2,
                                               int a3, int a4) {
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  static uint32_t s_shadowLayerLog = 0;
  if (s_shadowLayerLog < 10) {
    s_shadowLayerLog++;
    WAR3_HOOK_HOTPATH_LOG(
        "DXVK War3Hook: ShadowLayer mode=%u a2=%d a3=%d a4=%d\n", mode, a2, a3,
        a4);
  }
  if (mode == 1u) {
    // [ASM 已确认] TerrainShadow_RenderLayer:
    // - a2!=0 => RenderListA
    // - a3!=0 => RenderListB
    // mode=1 默认只关闭该入口触发的 ListB；stage14 的 ListB 直调由
    // Hook_Terrain_RenderListB 处理。
    a3 = 0;
  } else if (mode >= 2u) {
    // 完全禁用原生阴影
    a2 = 0;
    a3 = 0;
  }

  if (g_trampolineTerrainShadowLayer) {
    g_trampolineTerrainShadowLayer(thisPtr, edx, a2, a3, a4);
  } else if (g_originalTerrainShadowLayer) {
    g_originalTerrainShadowLayer(thisPtr, edx, a2, a3, a4);
  }
}

int __fastcall Hook_Terrain_RenderListA(void *thisPtr, void *edx, void *entry) {
  // ListA 路径采用保守策略：默认不激进拦截，优先保留雾/边界稳定性。
  const uint32_t mode = War3RenderState::GetNativeShadowMode();

  // mode>=2：完全禁用原生阴影，直接跳过 ListA。
  if (mode >= 2u)
    return 0;

  bool blocked = false;
  const char *reason = "PassThrough";
  uint32_t typeId = 0xFFFFFFFFu;
  uint32_t texKey = 0u;
  uint32_t groupCount = 0u;

  if (entry) {
    ReadU32Safe(entry, 0x00, typeId);     // entry->typeId
    ReadU32Safe(entry, 0x84, texKey);     // entry->blobArg / 纹理 key
    ReadU32Safe(entry, 0x58, groupCount); // entry->groupCountB
  }

  if (mode == 1u) {
    // 仅在显式开启时按 groupCountB 过滤。
    // 默认关闭，避免误伤战争迷雾/边界等条目。
    if (dxvk::war3::internal::kNativeShadowListABlockGroupEntriesEnabled &&
        groupCount > 0u) {
      blocked = true;
      reason = "Mode1_BlockListAGroupEntry";
    }

    // 可选白名单：仅保留前 N 个 ListA 纹理 key（用于保留雾/边界等基础贴图）。
    if (!blocked && dxvk::war3::internal::kNativeShadowListAWhitelistEnabled) {
      static std::unordered_set<uint32_t> s_whitelist;
      static uint32_t s_lastMode = 0xFFFFFFFFu;
      if (s_lastMode != mode) {
        s_whitelist.clear();
        s_lastMode = mode;
      }

      const uint32_t key = (texKey != 0u) ? texKey : typeId;
      if (key != 0u) {
        const auto it = s_whitelist.find(key);
        if (it == s_whitelist.end()) {
          if (s_whitelist.size() <
              dxvk::war3::internal::kNativeShadowListAWhitelistMaxTex) {
            s_whitelist.insert(key);
          } else {
            blocked = true;
            reason = "Mode1_ListAWhitelistOverflowBlock";
          }
        }
      }

      if constexpr (dxvk::war3::internal::
                        kNativeShadowListAWhitelistVerboseLogging) {
        static std::atomic<uint32_t> s_wlLogCount{0};
        const uint32_t n =
            s_wlLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 120u || (n % 4000u) == 0u) {
          war3dbg::Print(
              "DXVK War3Hook: ListA whitelist mode=%u key=0x%08X type=0x%08X "
              "groups=%u size=%u blocked=%d reason=%s\n",
              static_cast<unsigned>(mode), static_cast<unsigned>(texKey),
              static_cast<unsigned>(typeId), static_cast<unsigned>(groupCount),
              static_cast<unsigned>(s_whitelist.size()), blocked ? 1 : 0,
              reason);
        }
      }
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowListAStatsLogging) {
    // 低频统计：用于 DebugView 验证拦截命中情况。
    static std::atomic<uint32_t> s_calls{0};
    static std::atomic<uint32_t> s_blocked{0};
    const uint32_t calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (blocked)
      s_blocked.fetch_add(1, std::memory_order_relaxed);
    if ((calls % 8000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ListA stats calls=%u blocked=%u mode=%u "
          "lastType=0x%08X lastTex=0x%08X lastGroups=%u\n",
          static_cast<unsigned>(calls),
          static_cast<unsigned>(s_blocked.load(std::memory_order_relaxed)),
          static_cast<unsigned>(mode), static_cast<unsigned>(typeId),
          static_cast<unsigned>(texKey), static_cast<unsigned>(groupCount));
    }
  }

  if (blocked)
    return 0;

  if (g_trampolineTerrainRenderListA) {
    return g_trampolineTerrainRenderListA(thisPtr, edx, entry);
  }
  if (g_originalTerrainRenderListA) {
    return g_originalTerrainRenderListA(thisPtr, edx, entry);
  }
  return 0;
}

// ListB 按类型渲染入口（ASM: 0x737400）
// 关键点：
// - argType 对应 ListBEntry.type（RenderListB 内部先 cmp [entry], argType）
// - stage14 直调链路会直接走 argType=4，不经过 Terrain_RenderShadowLayer(a3)。
void __fastcall Hook_Terrain_RenderListB(void *thisPtr, void *edx, int argType,
                                         int passMode) {
  // ListB 是静态建筑阴影重点链路：支持按 mode + type 精确拦截。
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  bool blocked = false;
  const char *reason = "PassThrough";

  if (mode >= 2u && dxvk::war3::internal::kNativeShadowListBBlockAllWhenMode2) {
    blocked = true;
    reason = "Mode>=2_BlockAllListB";
  } else if (mode == 1u &&
             dxvk::war3::internal::kNativeShadowListBBlockType4WhenMode1 &&
             argType == 4) {
    blocked = true;
    reason = "Mode1_BlockListBType4";
  }

  if constexpr (dxvk::war3::internal::kNativeShadowListBStatsLogging) {
    static std::atomic<uint32_t> s_calls{0};
    static std::atomic<uint32_t> s_blocked{0};
    static std::array<std::atomic<uint32_t>, 8> s_typeCalls{};
    static std::array<std::atomic<uint32_t>, 8> s_typeBlocked{};
    const uint32_t calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (blocked) {
      s_blocked.fetch_add(1, std::memory_order_relaxed);
    }
    if (argType >= 0 && argType < static_cast<int>(s_typeCalls.size())) {
      s_typeCalls[static_cast<size_t>(argType)].fetch_add(
          1, std::memory_order_relaxed);
      if (blocked) {
        s_typeBlocked[static_cast<size_t>(argType)].fetch_add(
            1, std::memory_order_relaxed);
      }
    }

    if ((calls % 6000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ListB stats calls=%u blocked=%u mode=%u "
          "lastType=%d lastPass=%d t1=%u/%u t2=%u/%u t3=%u/%u t4=%u/%u "
          "t5=%u/%u\n",
          static_cast<unsigned>(calls),
          static_cast<unsigned>(s_blocked.load(std::memory_order_relaxed)),
          static_cast<unsigned>(mode), argType, passMode,
          static_cast<unsigned>(
              s_typeBlocked[1].load(std::memory_order_relaxed)),
          static_cast<unsigned>(s_typeCalls[1].load(std::memory_order_relaxed)),
          static_cast<unsigned>(
              s_typeBlocked[2].load(std::memory_order_relaxed)),
          static_cast<unsigned>(s_typeCalls[2].load(std::memory_order_relaxed)),
          static_cast<unsigned>(
              s_typeBlocked[3].load(std::memory_order_relaxed)),
          static_cast<unsigned>(s_typeCalls[3].load(std::memory_order_relaxed)),
          static_cast<unsigned>(
              s_typeBlocked[4].load(std::memory_order_relaxed)),
          static_cast<unsigned>(s_typeCalls[4].load(std::memory_order_relaxed)),
          static_cast<unsigned>(
              s_typeBlocked[5].load(std::memory_order_relaxed)),
          static_cast<unsigned>(
              s_typeCalls[5].load(std::memory_order_relaxed)));
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowListBVerboseLogging) {
    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t n = s_logCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 80u || (n % 3000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ListB %s mode=%u type=%d pass=%d this=%p reason=%s\n",
          blocked ? "BLOCK" : "PASS", static_cast<unsigned>(mode), argType,
          passMode, thisPtr, reason);
    }
  }

  if (blocked) {
    return;
  }

  if (g_trampolineTerrainRenderListB) {
    g_trampolineTerrainRenderListB(thisPtr, edx, argType, passMode);
    return;
  }
  if (g_originalTerrainRenderListB) {
    g_originalTerrainRenderListB(thisPtr, edx, argType, passMode);
  }
}

// 阴影贴图写入链路上游拦截（0x73F7A0）
// 目标：在“写入阶段”做精确过滤，避免在渲染末端误杀雾/边界。
int __fastcall Hook_ShadowUpdate_WriteEntry(void *list, void *edx, void *entry,
                                            float strength) {
  // 写入入口的目标是“可观测 + 可精确拦截”：
  // 先采样 callback/参数，再按配置执行拦截策略。
  (void)edx;
  const uint32_t mode = War3RenderState::GetNativeShadowMode();

  uint32_t callbackU32 = 0u;
  uint32_t flags0C = 0u;
  uint32_t useRadius = 0u;
  uint32_t fadeEnabled = 0u;
  uint32_t cbArgA = 0u;
  uint32_t cbArgB = 0u;
  uint32_t minX = 0u;
  uint32_t minY = 0u;
  uint32_t maxX = 0u;
  uint32_t maxY = 0u;

  if (entry) {
    ReadU32Safe(entry, 0x30, callbackU32); // ShadowUpdateEntry::callback
    ReadU32Safe(entry, 0x0C, flags0C);     // flags0C
    ReadU32Safe(entry, 0x20, useRadius);   // useRadius
    ReadU32Safe(entry, 0x5C, fadeEnabled); // fadeEnabled
    ReadU32Safe(entry, 0x50, cbArgA);      // cbArgA
    ReadU32Safe(entry, 0x54, cbArgB);      // cbArgB
    ReadU32Safe(entry, 0x10, minX);        // boundMinX
    ReadU32Safe(entry, 0x14, minY);        // boundMinY
    ReadU32Safe(entry, 0x18, maxX);        // boundMaxX
    ReadU32Safe(entry, 0x1C, maxY);        // boundMaxY
  }

  const uintptr_t callback = static_cast<uintptr_t>(callbackU32);
  const uintptr_t gameBase = GetGameDllBase();
  uint32_t callbackRva = 0u;
  if (gameBase && callback >= gameBase && callback < (gameBase + 0x04000000u)) {
    callbackRva = static_cast<uint32_t>(callback - gameBase);
  }
  RecordShadowCallbackRva(callbackRva);

  bool blocked = false;
  const char *reason = "PassThrough";

  if constexpr (dxvk::war3::internal::kNativeShadowBlockUpdateListEnabled) {
    blocked = true;
    reason = "BlockUpdateListEnabled";
  }

  if (!blocked &&
      dxvk::war3::internal::kNativeShadowBlockUpdateByCallbackEnabled) {
    for (uint32_t i = 0;
         i < dxvk::war3::internal::kNativeShadowBlockedCallbackRvasCount; ++i) {
      const uint32_t blockedRva =
          dxvk::war3::internal::kNativeShadowBlockedCallbackRvas[i];
      if (blockedRva != 0u && callbackRva == blockedRva) {
        blocked = true;
        reason = "BlockByCallbackRva";
        break;
      }
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowUpdateVerboseLogging) {
    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t n = s_logCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 160u || (n % 4000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ShadowUpdateWrite %s mode=%u list=%p entry=%p "
          "cb=0x%08X rva=0x%08X flags0C=%u useRadius=%u fade=%u argA=%u "
          "argB=%u bounds=[%d,%d,%d,%d] strength=%.3f reason=%s\n",
          blocked ? "BLOCK" : "PASS", static_cast<unsigned>(mode), list, entry,
          static_cast<unsigned>(callbackU32),
          static_cast<unsigned>(callbackRva), static_cast<unsigned>(flags0C),
          static_cast<unsigned>(useRadius), static_cast<unsigned>(fadeEnabled),
          static_cast<unsigned>(cbArgA), static_cast<unsigned>(cbArgB),
          static_cast<int32_t>(minX), static_cast<int32_t>(minY),
          static_cast<int32_t>(maxX), static_cast<int32_t>(maxY),
          static_cast<double>(strength), reason);
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowUpdateStatsLogging) {
    static std::atomic<uint32_t> s_calls{0};
    static std::atomic<uint32_t> s_blocked{0};
    const uint32_t calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (blocked)
      s_blocked.fetch_add(1, std::memory_order_relaxed);
    if ((calls % 3000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ShadowUpdateWrite stats calls=%u blocked=%u mode=%u "
          "lastCb=0x%08X lastCbRva=0x%08X lastUseRadius=%u lastFade=%u "
          "lastBounds=[%d,%d,%d,%d]\n",
          static_cast<unsigned>(calls),
          static_cast<unsigned>(s_blocked.load(std::memory_order_relaxed)),
          static_cast<unsigned>(mode), static_cast<unsigned>(callbackU32),
          static_cast<unsigned>(callbackRva), static_cast<unsigned>(useRadius),
          static_cast<unsigned>(fadeEnabled), static_cast<int32_t>(minX),
          static_cast<int32_t>(minY), static_cast<int32_t>(maxX),
          static_cast<int32_t>(maxY));
      DumpShadowCallbackTopStats(calls, mode);
    }
  }

  if (blocked)
    return 1;

  if (g_trampolineShadowUpdateWriteEntry) {
    return g_trampolineShadowUpdateWriteEntry(list, nullptr, entry, strength);
  }
  if (g_originalShadowUpdateWriteEntry) {
    return g_originalShadowUpdateWriteEntry(list, nullptr, entry, strength);
  }
  return 0;
}

void __fastcall Hook_ShadowPath_StaticStamp_Toggle(void* thisPtr,
                                                   void* edx,
                                                   int shadowObjectPtr,
                                                   int enable) {
  (void)edx;
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  bool blocked = false;
  const char* reason = "PassThrough";

  // Only suppress new static-stamp writes. Disable calls must pass through so
  // War3 can clear existing stamps instead of leaving stale mask bits behind.
  if (enable != 0) {
    if (mode >= 2u &&
        dxvk::war3::internal::kNativeShadowBlockStaticStampPathWhenMode2) {
      blocked = true;
      reason = "Mode>=2_BlockStaticStampPathEnable";
    } else if (mode == 1u &&
               dxvk::war3::internal::kNativeShadowBlockStaticStampPathWhenMode1) {
      blocked = true;
      reason = "Mode1_BlockStaticStampPathEnable";
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowStaticStampPathVerboseLogging) {
    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t n = s_logCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 80u || (n % 4000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: StaticStampPath %s mode=%u this=%p obj=0x%08X "
          "enable=%d reason=%s\n",
          blocked ? "BLOCK" : "PASS", static_cast<unsigned>(mode), thisPtr,
          static_cast<unsigned>(shadowObjectPtr), enable, reason);
    }
  }

  if (blocked)
    return;

  CallShadowPathStaticStampToggleOriginal(thisPtr, shadowObjectPtr, enable);
}

int __fastcall Hook_ShadowProjector_Add_FromObject(void *a1, void *a2,
                                                   void *arg0, int arg1,
                                                   int arg2) {
  // 入口职责：
  // 1) 识别调用来源（Runtime/JassBridge）；
  // 2) 采样 key；
  // 3) 执行 key/FourCC 规则拦截。

  // Phase 7.108：永久计数本 hook enter 数。
  g_projectorAddFromObjectEnterCount.fetch_add(1, std::memory_order_relaxed);

  // FourCC 检查路径无条件启用（轻量，只读 +0x30 一次），用于诊断
  // path blocker 的真实路径。
  const bool needsFourCCInspection = true;
  const bool needsSourcePath =
      dxvk::war3::internal::kNativeShadowBlockProjectorFromObjectEnabled ||
      dxvk::war3::internal::kNativeShadowBlockProjectorFromAltEnabled ||
      dxvk::war3::internal::kNativeShadowProjectorVerboseLogging ||
      dxvk::war3::internal::kNativeShadowProjectorStatsLogging ||
      dxvk::war3::internal::kWar3ShadowProjectorNativeHintEnabled;
  const uintptr_t retAddr = needsSourcePath ? GetCallReturnAddress() : 0u;
  const bool fromRuntimePath =
      needsSourcePath &&
      IsAddrInFuncRange(retAddr, g_shadowPathObjectProjectorRuntimeAddr, 0x54u);
  const bool fromAltPath =
      needsSourcePath &&
      IsAddrInFuncRange(retAddr, g_shadowPathObjectProjectorJassBridgeAddr, 0x64u);

  char keyBuf[64] = {};
  const bool needsKeyInspection =
      dxvk::war3::internal::kNativeShadowBlockBuildingProjectorEnabled ||
      dxvk::war3::internal::kNativeShadowProjectorVerboseLogging;
  const bool needsProjectorInspection =
      dxvk::war3::internal::kNativeShadowBlockAllProjectorEnabled ||
      needsSourcePath || needsKeyInspection || needsFourCCInspection;
  if (!needsProjectorInspection)
    return CallShadowProjectorAddFromObjectOriginal(a1, a2, arg0, arg1, arg2);

  const bool hasKey =
      needsKeyInspection &&
      shadowfilter::ReadAsciiCStringSafe(reinterpret_cast<const char *>(a2),
                                         keyBuf, sizeof(keyBuf));
  if (hasKey && dxvk::war3::internal::kNativeShadowProjectorVerboseLogging) {
    shadowfilter::RecordProjectorKeySample(keyBuf);
  }

  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  bool blocked = false;
  const char *reason = "PassThrough";
  uint32_t arg0FourCC = 0u;

  static std::atomic<uint32_t> s_calls{0};
  static std::atomic<uint32_t> s_blocked{0};
  static std::atomic<uint32_t> s_runtimeCalls{0};
  static std::atomic<uint32_t> s_altCalls{0};
  uint32_t calls = 0u;
  if constexpr (dxvk::war3::internal::kNativeShadowProjectorStatsLogging) {
    calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fromRuntimePath)
      s_runtimeCalls.fetch_add(1, std::memory_order_relaxed);
    if (fromAltPath)
      s_altCalls.fetch_add(1, std::memory_order_relaxed);
  }

  if constexpr (dxvk::war3::internal::kNativeShadowBlockAllProjectorEnabled) {
    blocked = true;
    reason = "BlockAllProjector";
  }

  if (!blocked && fromRuntimePath &&
      dxvk::war3::internal::kNativeShadowBlockProjectorFromObjectEnabled) {
    blocked = true;
    reason = "BlockFromObjectPath";
  }

  if (!blocked && fromAltPath &&
      dxvk::war3::internal::kNativeShadowBlockProjectorFromAltEnabled) {
    blocked = true;
    reason = "BlockFromAltPath";
  }

  // 精确 key 拦截：按配置定向屏蔽（例如 OLAR）。
  if (!blocked && hasKey &&
      dxvk::war3::internal::kNativeShadowBlockBuildingProjectorEnabled &&
      shadowfilter::IsBlockedShadowKey(keyBuf)) {
    blocked = true;
    reason = "BlockedUbersplatKey";
  }

  // FourCC 拦截（用于 Path Blocker 等）
  if (!blocked && needsFourCCInspection && arg0) {
    if (shadowfilter::TryExtractShadowObjectFourCC(arg0, arg0FourCC) &&
        shadowfilter::IsBlockedFourCC(arg0FourCC)) {
      blocked = true;
      reason = "BlockedFourCC";
    }
  }

  // Phase 7.108：无条件累加诊断 counter，记录 path blocker 究竟走没走这条路径。
  if (arg0FourCC != 0u) {
    g_projectorAddFromObjectFourCCExtractedCount.fetch_add(
        1, std::memory_order_relaxed);
    // 环形采样 observed fourcc（前 8 个 unique）。
    bool seen = false;
    uint32_t freeSlot = 8u;
    for (uint32_t i = 0u; i < 8u; ++i) {
      const uint32_t cur =
          g_projectorObservedFourCCSamples[i].load(std::memory_order_relaxed);
      if (cur == arg0FourCC) {
        seen = true;
        break;
      }
      if (cur == 0u && freeSlot == 8u)
        freeSlot = i;
    }
    if (!seen && freeSlot < 8u) {
      uint32_t expected = 0u;
      g_projectorObservedFourCCSamples[freeSlot].compare_exchange_strong(
          expected, arg0FourCC, std::memory_order_relaxed);
    }
  } else if (arg0 != nullptr) {
    g_projectorAddFromObjectFourCCMissCount.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (blocked && reason && std::strcmp(reason, "BlockedFourCC") == 0) {
    g_projectorAddFromObjectBlockedFourCCCount.fetch_add(
        1, std::memory_order_relaxed);
    bool seen = false;
    uint32_t freeSlot = 8u;
    for (uint32_t i = 0u; i < 8u; ++i) {
      const uint32_t cur =
          g_projectorBlockedFourCCSamples[i].load(std::memory_order_relaxed);
      if (cur == arg0FourCC) {
        seen = true;
        break;
      }
      if (cur == 0u && freeSlot == 8u)
        freeSlot = i;
    }
    if (!seen && freeSlot < 8u) {
      uint32_t expected = 0u;
      g_projectorBlockedFourCCSamples[freeSlot].compare_exchange_strong(
          expected, arg0FourCC, std::memory_order_relaxed);
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowProjectorVerboseLogging) {
    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t n = s_logCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 120u || (n % 2000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ProjectorFromObject %s mode=%u fromRuntime=%d "
          "fromAlt=%d key='%s' arg0=%p fourcc=0x%08X a1=%p reason=%s\n",
          blocked ? "BLOCK" : "PASS", static_cast<unsigned>(mode),
          fromRuntimePath ? 1 : 0, fromAltPath ? 1 : 0,
          hasKey ? keyBuf : "(null)", arg0,
          static_cast<unsigned>(arg0FourCC), a1, reason);
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowProjectorStatsLogging) {
    if ((calls % 4000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ProjectorFromObject stats calls=%u blocked=%u "
          "runtime=%u alt=%u mode=%u\n",
          static_cast<unsigned>(calls),
          static_cast<unsigned>(s_blocked.load(std::memory_order_relaxed)),
          static_cast<unsigned>(s_runtimeCalls.load(std::memory_order_relaxed)),
          static_cast<unsigned>(s_altCalls.load(std::memory_order_relaxed)),
          static_cast<unsigned>(mode));
    }
  }

  if constexpr (dxvk::war3::internal::kWar3ShadowProjectorNativeHintEnabled) {
    dxvk::war3::native::War3NativeShadowHintRegistry::instance()
        .recordFromObject(arg0, fromRuntimePath, fromAltPath, blocked);
  }

  if (blocked) {
    g_projectorAddFromObjectBlockedCount.fetch_add(1, std::memory_order_relaxed);
    if constexpr (dxvk::war3::internal::kNativeShadowProjectorStatsLogging)
      s_blocked.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }

  return CallShadowProjectorAddFromObjectOriginal(a1, a2, arg0, arg1, arg2);
}

int __fastcall Hook_ShadowProjector_Add_Simple(void *a1, void *a2, int arg0,
                                               int arg1, int arg2, int arg3,
                                               int arg4, int arg5, int arg6) {
  // Add_Simple 覆盖 FromObject 之外的投影链路，作为静态阴影补充拦截点。

  // Phase 7.108：永久计数。
  g_projectorAddSimpleEnterCount.fetch_add(1, std::memory_order_relaxed);

  const bool needsSourcePath =
      dxvk::war3::internal::kNativeShadowProjectorVerboseLogging ||
      dxvk::war3::internal::kNativeShadowProjectorStatsLogging ||
      dxvk::war3::internal::kWar3ShadowProjectorNativeHintEnabled;
  const bool needsProjectorInspection =
      dxvk::war3::internal::kNativeShadowBlockAllProjectorEnabled ||
      dxvk::war3::internal::kNativeShadowBlockProjectorSimpleEnabled ||
      needsSourcePath;
  if (!needsProjectorInspection) {
    return CallShadowProjectorAddSimpleOriginal(a1, a2, arg0, arg1, arg2, arg3,
                                                arg4, arg5, arg6);
  }

  const uintptr_t retAddr = needsSourcePath ? GetCallReturnAddress() : 0u;
  const bool fromSimpleBridge =
      needsSourcePath &&
      IsAddrInFuncRange(retAddr, g_shadowProjectorSimpleBridgeAddr, 0xB7u);
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  static std::atomic<uint32_t> s_calls{0};
  static std::atomic<uint32_t> s_blocked{0};
  static std::atomic<uint32_t> s_bridgeCalls{0};

  uint32_t calls = 0u;
  if constexpr (dxvk::war3::internal::kNativeShadowProjectorStatsLogging) {
    calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fromSimpleBridge)
      s_bridgeCalls.fetch_add(1, std::memory_order_relaxed);
  }

  bool blocked = false;
  const char *reason = "PassThrough";

  if constexpr (dxvk::war3::internal::kNativeShadowBlockAllProjectorEnabled) {
    blocked = true;
    reason = "BlockAllProjector";
  }

  if (!blocked &&
      dxvk::war3::internal::kNativeShadowBlockProjectorSimpleEnabled) {
    blocked = true;
    reason = "BlockSimplePath";
  }

  if constexpr (dxvk::war3::internal::kNativeShadowProjectorVerboseLogging) {
    static std::atomic<uint32_t> s_logCount{0};
    const uint32_t n = s_logCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 60u || (n % 2000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ProjectorSimple %s mode=%u fromBridge=%d ret=0x%p "
          "a1=%p a2=%p args=[%d,%d,%d,%d,%d,%d,%d] reason=%s\n",
          blocked ? "BLOCK" : "PASS", static_cast<unsigned>(mode),
          fromSimpleBridge ? 1 : 0, reinterpret_cast<void *>(retAddr), a1, a2,
          arg0, arg1, arg2, arg3, arg4, arg5, arg6, reason);
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowProjectorStatsLogging) {
    if ((calls % 4000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: ProjectorSimple stats calls=%u blocked=%u "
          "fromBridge=%u mode=%u\n",
          static_cast<unsigned>(calls),
          static_cast<unsigned>(s_blocked.load(std::memory_order_relaxed)),
          static_cast<unsigned>(s_bridgeCalls.load(std::memory_order_relaxed)),
          static_cast<unsigned>(mode));
    }
  }

  if constexpr (dxvk::war3::internal::kWar3ShadowProjectorNativeHintEnabled) {
    dxvk::war3::native::War3NativeShadowHintRegistry::instance().recordSimple(
        a1, fromSimpleBridge, blocked);
  }

  if (blocked) {
    g_projectorAddSimpleBlockedCount.fetch_add(1, std::memory_order_relaxed);
    if constexpr (dxvk::war3::internal::kNativeShadowProjectorStatsLogging)
      s_blocked.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }

  return CallShadowProjectorAddSimpleOriginal(a1, a2, arg0, arg1, arg2, arg3,
                                              arg4, arg5, arg6);
}

// =====================================================================
// Phase 7.100: TerrainShadow_WriteMaskRegion hook (静态阴影治理方案 A)
// =====================================================================
// 论文 §6 §7.1 决定性发现：War3 1.27a 的建筑/装饰物预渲染贴花阴影
// (我们项目一直无法剔除的"魔兽自带的静态阴影")真正写入路径就是
// TerrainShadow_WriteMaskRegion @ 0x6F234710。30+ caller 全部汇聚到这。
//
// IDA 反编译验证（0x6F234710）：
//   if (a4) result = 4;
//   else    result = *(unsigned __int16 *)(a2 + 268);   // a2 + 0x10C
// 这个 result 就是 maskIdx。idx==3 是 shadow footprint，idx==0/1/2 分别对应
// fog/LOS/path（共享 mask grid 但语义独立）。
//
// 拦截 idx==3 即可干净屏蔽建筑阴影，对 fog/LOS/path 零影响。
typedef int (__thiscall *TerrainShadowWriteMaskRegionFn)(void *thisPtr, void *a2,
                                                         int a3, void *a4,
                                                         int a5);
TerrainShadowWriteMaskRegionFn g_trampolineTerrainShadowWriteMaskRegion = nullptr;

std::atomic<uint64_t> g_writeMaskRegionEnterCount{0};
std::atomic<uint64_t> g_writeMaskRegionRejectedIdx3Count{0};
std::atomic<uint64_t> g_writeMaskRegionPassFogCount{0};   // idx==0
std::atomic<uint64_t> g_writeMaskRegionPassLosCount{0};   // idx==1
std::atomic<uint64_t> g_writeMaskRegionPassPathCount{0};  // idx==2
std::atomic<uint64_t> g_writeMaskRegionPassOtherCount{0};

// Phase 7.112：caller-aware 拦截统计。
std::atomic<uint64_t> g_writeMaskRegionFromBuildingStampCount{0};
std::atomic<uint64_t> g_writeMaskRegionRejectedBuildingCount{0};
std::atomic<uint64_t> g_writeMaskRegionFromRegisterFootprintCount{0};
std::atomic<uint64_t> g_writeMaskRegionFromRebuildMaskCount{0};
std::atomic<uint64_t> g_writeMaskRegionFromActorRuntimeCount{0};
std::atomic<uint64_t> g_writeMaskRegionFromForObjectCount{0};
std::atomic<uint64_t> g_writeMaskRegionFromOtherCallerCount{0};

// 静态地址：CUnit_StampBuildingShadowFootprint @ Game.dll+0x514F40，size 0x81
constexpr uintptr_t kCUnitStampBuildingShadowFootprintRva = 0x514F40u;
constexpr uintptr_t kCUnitStampBuildingShadowFootprintSize = 0x81u;
// CWidget_RegisterFootprintAndShadowMask @ Game.dll+0x65A140, size 0x3FE
constexpr uintptr_t kCWidgetRegisterFootprintRva = 0x65A140u;
constexpr uintptr_t kCWidgetRegisterFootprintSize = 0x3FEu;
// TerrainShadow_RebuildMaskFromObjectLists @ Game.dll+0x233E90, size 0x513
constexpr uintptr_t kTerrainShadowRebuildMaskRva = 0x233E90u;
constexpr uintptr_t kTerrainShadowRebuildMaskSize = 0x513u;
// TerrainShadow_WriteMaskRegion_FromActorRuntime @ Game.dll+0x3DB260, size 0x128
constexpr uintptr_t kTerrainShadowFromActorRuntimeRva = 0x3DB260u;
constexpr uintptr_t kTerrainShadowFromActorRuntimeSize = 0x128u;
// TerrainShadow_WriteMaskRegion_ForObject @ Game.dll+0x2346FC, size 0xE9
constexpr uintptr_t kTerrainShadowForObjectRva = 0x2346FCu;
constexpr uintptr_t kTerrainShadowForObjectSize = 0xE9u;

// Phase 7.108：ShadowProjector 永久 atomic 计数器。
// 这条路径独立于 D3D9 mesh draw（CTerrainUberSplats 系统），
// 必须永久计数才能在不开 verbose 编译期开关时也判断
// path blocker 是否走这条路径。

int __thiscall Hook_TerrainShadow_WriteMaskRegion(void *thisPtr, void *a2,
                                                   int a3, void *a4, int a5) {
  g_writeMaskRegionEnterCount.fetch_add(1, std::memory_order_relaxed);

  // Phase 7.112：caller-aware 静态阴影屏蔽。
  // 通过返回地址判定调用来源，仅拦截"建筑物 shadow footprint"路径。
  // fog/LOS/path/visibility 等共享路径完全不受影响。
  // GCC/MinGW 用 __builtin_return_address；MSVC 用 _ReturnAddress。
#if defined(_MSC_VER) && !defined(__GNUC__)
  const uintptr_t retAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
  const uintptr_t retAddr =
      reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
  const uintptr_t gameBase = GetGameDllBase();
  const uintptr_t buildingStampStart = gameBase + kCUnitStampBuildingShadowFootprintRva;
  const uintptr_t registerFootprintStart = gameBase + kCWidgetRegisterFootprintRva;
  const uintptr_t rebuildMaskStart = gameBase + kTerrainShadowRebuildMaskRva;
  const uintptr_t fromActorRuntimeStart = gameBase + kTerrainShadowFromActorRuntimeRva;
  const uintptr_t forObjectStart = gameBase + kTerrainShadowForObjectRva;

  const bool fromBuildingStamp = IsAddrInFuncRange(
      retAddr, buildingStampStart, kCUnitStampBuildingShadowFootprintSize);
  const bool fromRegisterFootprint = IsAddrInFuncRange(
      retAddr, registerFootprintStart, kCWidgetRegisterFootprintSize);
  const bool fromRebuildMask = IsAddrInFuncRange(
      retAddr, rebuildMaskStart, kTerrainShadowRebuildMaskSize);
  const bool fromActorRuntime = IsAddrInFuncRange(
      retAddr, fromActorRuntimeStart, kTerrainShadowFromActorRuntimeSize);
  const bool fromForObject = IsAddrInFuncRange(
      retAddr, forObjectStart, kTerrainShadowForObjectSize);

  // 来源分桶（永久统计，可用 control plane summary 实时验证）
  if (fromBuildingStamp) {
    g_writeMaskRegionFromBuildingStampCount.fetch_add(
        1, std::memory_order_relaxed);
  } else if (fromRegisterFootprint) {
    g_writeMaskRegionFromRegisterFootprintCount.fetch_add(
        1, std::memory_order_relaxed);
  } else if (fromRebuildMask) {
    g_writeMaskRegionFromRebuildMaskCount.fetch_add(
        1, std::memory_order_relaxed);
  } else if (fromActorRuntime) {
    g_writeMaskRegionFromActorRuntimeCount.fetch_add(
        1, std::memory_order_relaxed);
  } else if (fromForObject) {
    g_writeMaskRegionFromForObjectCount.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    g_writeMaskRegionFromOtherCallerCount.fetch_add(
        1, std::memory_order_relaxed);
  }

  // Phase 7.112 第一刀：仅 reject 来自 CUnit_StampBuildingShadowFootprint 的写入。
  // 该 caller 只在建筑物 lifecycle 调用，是建筑预渲染贴花阴影的唯一专属入口。
  // 不影响 fog/LOS/path 或 visibility（它们走 RebuildMask / RegisterFootprint /
  // ForObject 等其它 caller）。
  if (fromBuildingStamp &&
      dxvk::war3::internal::kNativeStaticShadowHideBuildingFootprintEnabled) {
    const uint64_t logIdx = g_writeMaskRegionRejectedBuildingCount.fetch_add(
        1, std::memory_order_relaxed);
    if (dxvk::war3::internal::
            kNativeStaticShadowHideBuildingFootprintDebugLog &&
        logIdx < 8u) {
      uint32_t magic = 0u;
      uint32_t rawcode = 0u;
      if (a2) {
        ::dxvk::war3::SafeReadU32Fast(a2, 0x0Cu, magic);
        ::dxvk::war3::SafeReadU32Fast(a2, 0x30u, rawcode);
      }
      char fc[5] = {char((rawcode >> 24) & 0xFF),
                    char((rawcode >> 16) & 0xFF),
                    char((rawcode >> 8) & 0xFF), char(rawcode & 0xFF), 0};
      char buf[200];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: BUILDING SHADOW REJECT #%llu "
               "widget=%p magic=0x%08X rawcode=0x%08X (%s) a3=0x%X a4=%p",
               static_cast<unsigned long long>(logIdx + 1u), a2,
               static_cast<unsigned int>(magic),
               static_cast<unsigned int>(rawcode),
               (rawcode != 0u ? fc : "----"),
               static_cast<unsigned int>(a3), a4);
      ::dxvk::Logger::info(buf);
    }
    return 0;
  }

  // Phase 7.112 第二刀：检查 widget+0x60 的 SHADOW_FOOTPRINT_PRESENT 标志位。
  // 来自 CUnit_StampBuildingShadowFootprint 反编译：
  //   *(_DWORD *)(unit + 96) |= 0x400u;   // (unit+0x60) bit10 = SHADOW_FOOTPRINT_FLAG
  // 当 widget 在 lifecycle 中已经被标记为"具备 shadow footprint"，无论它现在
  // 走的是 RebuildMask / ForObject / RegisterFootprint 哪条 caller，
  // 这次 WriteMaskRegion 调用的写入对象都涉及该 widget 的 shadow footprint mask bit。
  //
  // 这条规则比"caller=BuildingStamp"覆盖更广（建筑物会被定时刷新走 RebuildMask，
  // 此时 caller 已经不是 BuildingStamp，但 widget 标志位仍然在）。
  // 对应的 widget 类型主要是 building / destructible 静态对象。
  //
  // 风险：fog/LOS/visibility 不依赖这个标志位，不会受影响。

  // Phase 7.112 第二刀诊断：前 12 次 fire 时 dump widget +0x60 +0x14C 字段语义。
  {
    static std::atomic<uint32_t> s_dump60{0};
    const uint32_t cur = s_dump60.fetch_add(1, std::memory_order_relaxed);
    if (cur < 12u && a2 != nullptr) {
      uint32_t magic = 0u;
      uint32_t rawcode = 0u;
      uint32_t flags60 = 0u;
      uint32_t flags20 = 0u;
      uint32_t flagsCC = 0u;
      uint32_t flags14C = 0u;
      uint32_t flags5C = 0u;
      ::dxvk::war3::SafeReadU32Fast(a2, 0x0Cu, magic);
      ::dxvk::war3::SafeReadU32Fast(a2, 0x30u, rawcode);
      ::dxvk::war3::SafeReadU32Fast(a2, 0x60u, flags60);
      ::dxvk::war3::SafeReadU32Fast(a2, 0x20u, flags20);
      ::dxvk::war3::SafeReadU32Fast(a2, 0xCCu, flagsCC);
      ::dxvk::war3::SafeReadU32Fast(a2, 0x14Cu, flags14C);
      ::dxvk::war3::SafeReadU32Fast(a2, 0x5Cu, flags5C);
      char fc[5] = {char((rawcode >> 24) & 0xFF),
                    char((rawcode >> 16) & 0xFF),
                    char((rawcode >> 8) & 0xFF), char(rawcode & 0xFF), 0};
      char buf[280];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow] WMR_FLAGS #%u widget=%p a3=0x%X a4=%p "
               "magic=0x%08X raw=0x%08X(%s) "
               "+0x20=0x%08X +0x5C=0x%08X +0x60=0x%08X +0xCC=0x%08X +0x14C=0x%08X",
               cur + 1, a2, static_cast<unsigned>(a3), a4, magic, rawcode,
               (rawcode != 0u ? fc : "----"), flags20, flags5C, flags60,
               flagsCC, flags14C);
      ::dxvk::Logger::info(buf);
    }
  }
  if (a2 != nullptr &&
      dxvk::war3::internal::kNativeStaticShadowHideBuildingFootprintEnabled) {
    uint32_t widgetFlags = 0u;
    if (::dxvk::war3::SafeReadU32Fast(a2, 0x60u, widgetFlags) &&
        (widgetFlags & 0x400u) != 0u) {
      const uint64_t logIdx = g_writeMaskRegionRejectedBuildingCount.fetch_add(
          1, std::memory_order_relaxed);
      if (dxvk::war3::internal::
              kNativeStaticShadowHideBuildingFootprintDebugLog &&
          logIdx < 16u) {
        uint32_t magic = 0u;
        uint32_t rawcode = 0u;
        ::dxvk::war3::SafeReadU32Fast(a2, 0x0Cu, magic);
        ::dxvk::war3::SafeReadU32Fast(a2, 0x30u, rawcode);
        char fc[5] = {char((rawcode >> 24) & 0xFF),
                      char((rawcode >> 16) & 0xFF),
                      char((rawcode >> 8) & 0xFF),
                      char(rawcode & 0xFF), 0};
        char callerKind[24] = "Other";
        if (fromRebuildMask)
          std::strcpy(callerKind, "RebuildMask");
        else if (fromForObject)
          std::strcpy(callerKind, "ForObject");
        else if (fromRegisterFootprint)
          std::strcpy(callerKind, "RegisterFootprint");
        else if (fromActorRuntime)
          std::strcpy(callerKind, "ActorRuntime");
        char buf[260];
        snprintf(buf, sizeof(buf),
                 "DXVK War3Hook[Shadow]: BUILDING SHADOW (FlagPath) REJECT #%llu "
                 "widget=%p flags+60=0x%08X magic=0x%08X rawcode=0x%08X (%s) "
                 "a3=0x%X a4=%p caller=%s",
                 static_cast<unsigned long long>(logIdx + 1u), a2,
                 static_cast<unsigned int>(widgetFlags),
                 static_cast<unsigned int>(magic),
                 static_cast<unsigned int>(rawcode),
                 (rawcode != 0u ? fc : "----"),
                 static_cast<unsigned int>(a3), a4, callerKind);
        ::dxvk::Logger::info(buf);
      }
      return 0;
    }
  }

  // 历史 idx==3 推断已被推翻（保留代码做 A/B 对照），默认关闭。
  if constexpr (dxvk::war3::internal::kNativeStaticShadowMaskHideEnabled) {
    uint32_t maskIdx = 0xFFFFu;
    if (a4 != nullptr) {
      maskIdx = 4u;
    } else if (a2 != nullptr) {
      uint32_t idx32 = 0;
      if (::dxvk::war3::SafeReadU32Fast(a2, 0x10Cu, idx32))
        maskIdx = idx32 & 0xFFFFu;
    }
    if (maskIdx == 3u) {
      g_writeMaskRegionRejectedIdx3Count.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }
    switch (maskIdx) {
    case 0u:
      g_writeMaskRegionPassFogCount.fetch_add(1, std::memory_order_relaxed);
      break;
    case 1u:
      g_writeMaskRegionPassLosCount.fetch_add(1, std::memory_order_relaxed);
      break;
    case 2u:
      g_writeMaskRegionPassPathCount.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      g_writeMaskRegionPassOtherCount.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }

  if (g_trampolineTerrainShadowWriteMaskRegion)
    return g_trampolineTerrainShadowWriteMaskRegion(thisPtr, a2, a3, a4, a5);
  return 0;
}

} // namespace

// Phase 7.100：跨 TU 暴露的 WriteMaskRegion 诊断 atomic 读取器。
// 实现放在 anonymous namespace 之内不可见；这里把读取器函数定义在
// dxvk::war3::hooks 命名空间内，引用 anonymous namespace 内的 atomic。
// 注意：anonymous namespace 内的 globals 可以被同 TU 的非匿名函数访问。
uint64_t QueryWriteMaskRegionEnterCount() {
  return g_writeMaskRegionEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionRejectedIdx3Count() {
  return g_writeMaskRegionRejectedIdx3Count.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionPassFogCount() {
  return g_writeMaskRegionPassFogCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionPassLosCount() {
  return g_writeMaskRegionPassLosCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionPassPathCount() {
  return g_writeMaskRegionPassPathCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionPassOtherCount() {
  return g_writeMaskRegionPassOtherCount.load(std::memory_order_relaxed);
}

// Phase 7.112：caller-aware 静态阴影屏蔽诊断 atomic 读取器。
uint64_t QueryWriteMaskRegionFromBuildingStampCount() {
  return g_writeMaskRegionFromBuildingStampCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionRejectedBuildingCount() {
  return g_writeMaskRegionRejectedBuildingCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionFromRegisterFootprintCount() {
  return g_writeMaskRegionFromRegisterFootprintCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionFromRebuildMaskCount() {
  return g_writeMaskRegionFromRebuildMaskCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionFromActorRuntimeCount() {
  return g_writeMaskRegionFromActorRuntimeCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionFromForObjectCount() {
  return g_writeMaskRegionFromForObjectCount.load(std::memory_order_relaxed);
}
uint64_t QueryWriteMaskRegionFromOtherCallerCount() {
  return g_writeMaskRegionFromOtherCallerCount.load(std::memory_order_relaxed);
}

// Phase 7.108：ShadowProjector 永久 atomic 读取器。
uint64_t QueryShadowProjectorAddFromObjectEnterCount() {
  return g_projectorAddFromObjectEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryShadowProjectorAddFromObjectBlockedCount() {
  return g_projectorAddFromObjectBlockedCount.load(std::memory_order_relaxed);
}
uint64_t QueryShadowProjectorAddFromObjectFourCCExtractedCount() {
  return g_projectorAddFromObjectFourCCExtractedCount.load(
      std::memory_order_relaxed);
}
uint64_t QueryShadowProjectorAddFromObjectFourCCMissCount() {
  return g_projectorAddFromObjectFourCCMissCount.load(
      std::memory_order_relaxed);
}
uint64_t QueryShadowProjectorAddFromObjectBlockedFourCCCount() {
  return g_projectorAddFromObjectBlockedFourCCCount.load(
      std::memory_order_relaxed);
}
uint64_t QueryShadowProjectorAddSimpleEnterCount() {
  return g_projectorAddSimpleEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryShadowProjectorAddSimpleBlockedCount() {
  return g_projectorAddSimpleBlockedCount.load(std::memory_order_relaxed);
}
uint32_t QueryShadowProjectorBlockedFourCCSampleAt(uint32_t idx) {
  if (idx >= g_projectorBlockedFourCCSamples.size())
    return 0u;
  return g_projectorBlockedFourCCSamples[idx].load(std::memory_order_relaxed);
}
uint32_t QueryShadowProjectorObservedFourCCSampleAt(uint32_t idx) {
  if (idx >= g_projectorObservedFourCCSamples.size())
    return 0u;
  return g_projectorObservedFourCCSamples[idx].load(std::memory_order_relaxed);
}

// Phase 7.100：跨 TU 暴露的 WriteMaskRegion 诊断 atomic 读取器。
// 这些函数声明在 dxvk::war3::hooks 命名空间内、anonymous namespace 之外，
// bridge.cpp 可以直接调用。
uint64_t QueryWriteMaskRegionEnterCount();
uint64_t QueryWriteMaskRegionRejectedIdx3Count();
uint64_t QueryWriteMaskRegionPassFogCount();
uint64_t QueryWriteMaskRegionPassLosCount();
uint64_t QueryWriteMaskRegionPassPathCount();
uint64_t QueryWriteMaskRegionPassOtherCount();

// Phase 7.100：跨 TU 暴露的 WriteMaskRegion 诊断 atomic 读取器。
// 这些函数声明在 dxvk::war3::hooks 命名空间内、anonymous namespace 之外，
// bridge.cpp 可以直接调用。
uint64_t QueryWriteMaskRegionEnterCount();
uint64_t QueryWriteMaskRegionRejectedIdx3Count();
uint64_t QueryWriteMaskRegionPassFogCount();
uint64_t QueryWriteMaskRegionPassLosCount();
uint64_t QueryWriteMaskRegionPassPathCount();
uint64_t QueryWriteMaskRegionPassOtherCount();

bool InstallShadowHooks(const ShadowHookAddresses &addrs) {
  // 先保存原函数指针与来源地址，再执行分项安装，保证失败时仍可回退。
  g_originalTerrainShadowLayer =
      reinterpret_cast<TerrainRenderShadowLayerFn>(addrs.terrainShadowLayerAddr);
  g_originalTerrainRenderListA =
      reinterpret_cast<TerrainRenderListAFn>(addrs.terrainRenderListAAddr);
  g_originalTerrainRenderListB =
      reinterpret_cast<TerrainRenderListBFn>(addrs.terrainRenderListBAddr);
  g_originalShadowUpdateWriteEntry = reinterpret_cast<ShadowUpdateWriteEntryFn>(
      addrs.shadowUpdateWriteEntryAddr);
  g_originalShadowProjectorAddFromObject =
      reinterpret_cast<ShadowProjectorAddFromObjectFn>(
          addrs.shadowProjectorAddFromObjectAddr);
  g_originalShadowProjectorAddSimple =
      reinterpret_cast<ShadowProjectorAddSimpleFn>(
          addrs.shadowProjectorAddSimpleAddr);
  g_originalShadowPathStaticStampToggle =
      reinterpret_cast<ShadowPathStaticStampToggleFn>(
          addrs.shadowPathStaticStampToggleAddr);
  g_shadowPathObjectProjectorRuntimeAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowPathObjectProjectorRuntimeAddr);
  g_shadowPathObjectProjectorJassBridgeAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowPathObjectProjectorJassBridgeAddr);
  g_shadowProjectorSimpleBridgeAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowProjectorSimpleBridgeAddr);

  bool anyInstalled = false;
  if constexpr (dxvk::war3::internal::kWar3ShadowTerrainHookEnabled) {
    // 基础阴影层入口通常最关键，优先尝试安装。
    anyInstalled |= InstallMinHook(
        addrs.terrainShadowLayerAddr,
        reinterpret_cast<LPVOID>(&Hook_Terrain_RenderShadowLayer),
        reinterpret_cast<LPVOID *>(&g_trampolineTerrainShadowLayer),
        "Shadow", "Terrain_RenderShadowLayer", false, true);
  } else {
    war3dbg::Print("DXVK War3Hook: 二分诊断态关闭 Shadow/Terrain hook 安装\n");
  }

  // ListA/ListB/UpdateWrite 按开关启用，便于逐步回归与灰度验证。
  if constexpr (dxvk::war3::internal::kWar3ShadowTerrainHookEnabled &&
                dxvk::war3::internal::kNativeShadowListAHookEnabled) {
    anyInstalled |= InstallMinHook(
        addrs.terrainRenderListAAddr,
        reinterpret_cast<LPVOID>(&Hook_Terrain_RenderListA),
        reinterpret_cast<LPVOID *>(&g_trampolineTerrainRenderListA),
        "Shadow", "TerrainShadow_RenderListA", false, true);
  }
  if constexpr (dxvk::war3::internal::kWar3ShadowTerrainHookEnabled &&
                dxvk::war3::internal::kNativeShadowListBHookEnabled) {
    anyInstalled |= InstallMinHook(
        addrs.terrainRenderListBAddr,
        reinterpret_cast<LPVOID>(&Hook_Terrain_RenderListB),
        reinterpret_cast<LPVOID *>(&g_trampolineTerrainRenderListB),
        "Shadow", "TerrainShadow_RenderListB", false, true);
  }
  if constexpr (dxvk::war3::internal::kWar3ShadowUpdateHookEnabled &&
                dxvk::war3::internal::kNativeShadowUpdateWriteHookEnabled) {
    anyInstalled |= InstallMinHook(
        addrs.shadowUpdateWriteEntryAddr,
        reinterpret_cast<LPVOID>(&Hook_ShadowUpdate_WriteEntry),
        reinterpret_cast<LPVOID *>(&g_trampolineShadowUpdateWriteEntry),
        "Shadow", "ShadowUpdate_WriteEntry", false, true);
  } else if constexpr (!dxvk::war3::internal::kWar3ShadowUpdateHookEnabled) {
    war3dbg::Print(
        "DXVK War3Hook: 二分诊断态关闭 Shadow/UpdateWrite hook 安装\n");
  }

  if constexpr (dxvk::war3::internal::kWar3ShadowUpdateHookEnabled &&
                dxvk::war3::internal::kNativeShadowStaticStampPathHookEnabled) {
    anyInstalled |= InstallMinHook(
        addrs.shadowPathStaticStampToggleAddr,
        reinterpret_cast<LPVOID>(&Hook_ShadowPath_StaticStamp_Toggle),
        reinterpret_cast<LPVOID *>(&g_trampolineShadowPathStaticStampToggle),
        "Shadow", "ShadowPath_StaticStamp_Toggle", false, true);
  }

  if constexpr (dxvk::war3::internal::kWar3ShadowProjectorHookEnabled) {
    anyInstalled |= InstallMinHook(
        addrs.shadowProjectorAddFromObjectAddr,
        reinterpret_cast<LPVOID>(&Hook_ShadowProjector_Add_FromObject),
        reinterpret_cast<LPVOID *>(&g_trampolineShadowProjectorAddFromObject),
        "Shadow", "ShadowProjector_Add_FromObject", false, true);
    // Add_Simple 作为 FromObject 之外的补充路径，防止漏拦截。
    anyInstalled |= InstallMinHook(
        addrs.shadowProjectorAddSimpleAddr,
        reinterpret_cast<LPVOID>(&Hook_ShadowProjector_Add_Simple),
        reinterpret_cast<LPVOID *>(&g_trampolineShadowProjectorAddSimple),
        "Shadow", "ShadowProjector_Add_Simple", false, true);
  } else {
    war3dbg::Print(
        "DXVK War3Hook: 二分诊断态关闭 Shadow/Projector hook 安装\n");
  }

  // Phase 7.100/7.101: TerrainShadow_WriteMaskRegion hook 安装。
  // 实测推翻论文 idx==3 推断（详见 war3_internal_test_config.h 注释）。
  // 当前默认 kNativeStaticShadowMaskHideEnabled=false：装上 hook + 保留
  // 诊断 counter，但不 reject。装入 gate 单独由 kNativeStaticShadowMaskHookInstall 控制。
  if constexpr (dxvk::war3::internal::kNativeStaticShadowMaskHookInstall) {
    if (addrs.terrainShadowWriteMaskRegionAddr != nullptr) {
      anyInstalled |= InstallMinHook(
          addrs.terrainShadowWriteMaskRegionAddr,
          reinterpret_cast<LPVOID>(&Hook_TerrainShadow_WriteMaskRegion),
          reinterpret_cast<LPVOID *>(&g_trampolineTerrainShadowWriteMaskRegion),
          "Shadow", "TerrainShadow_WriteMaskRegion", false, true);
    }
  }
  return anyInstalled;
}

} // namespace dxvk::war3::hooks
