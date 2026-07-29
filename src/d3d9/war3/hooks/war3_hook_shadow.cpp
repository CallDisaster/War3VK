// Phase 7.100 marker bump 102042
#include "war3_hook_shadow.h"
#include "war3_hook_install_util.h"
#include "war3_hook_perf.h"
#include "war3_hook_widget_identity.h"
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
#include <cstdlib>
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
using ShadowRegisterImageEntryFn = int(__fastcall *)(void *, void *, int,
                                                     void *, void *, int, int);
using ShadowPathStaticStampToggleFn = void(__fastcall *)(void *, void *, int,
                                                         int);
using CUnitUiRecordSetUnitShadowFn = int(__fastcall *)(void *, int);
using CUnitUiRecordSetStructureShadowFn = int(__fastcall *)(void *, int);

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
ShadowRegisterImageEntryFn g_originalShadowRegisterImageEntry = nullptr;
ShadowRegisterImageEntryFn g_trampolineShadowRegisterImageEntry = nullptr;
ShadowPathStaticStampToggleFn g_originalShadowPathStaticStampToggle = nullptr;
ShadowPathStaticStampToggleFn g_trampolineShadowPathStaticStampToggle = nullptr;
CUnitUiRecordSetUnitShadowFn g_originalCUnitUiRecordSetUnitShadow = nullptr;
CUnitUiRecordSetUnitShadowFn g_trampolineCUnitUiRecordSetUnitShadow = nullptr;
CUnitUiRecordSetStructureShadowFn
    g_originalCUnitUiRecordSetStructureShadow = nullptr;
CUnitUiRecordSetStructureShadowFn
    g_trampolineCUnitUiRecordSetStructureShadow = nullptr;
uintptr_t g_shadowPathObjectProjectorRuntimeAddr = 0;
uintptr_t g_shadowPathObjectProjectorJassBridgeAddr = 0;
uintptr_t g_shadowProjectorSimpleBridgeAddr = 0;
uintptr_t g_shadowRegisterRetWithParamsAddr = 0;
uintptr_t g_shadowRegisterRetSelectionCircleAddr = 0;
uintptr_t g_shadowRegisterRetStaticStampAddr = 0;
uintptr_t g_shadowRegisterRetEmitterStampAddr = 0;
uintptr_t g_shadowRegisterRetObjectBridgeAddr = 0;
uintptr_t g_shadowRegisterRetMarkOcclusionAddr = 0;
uintptr_t g_shadowRegisterRetFromPointAddr = 0;
uintptr_t g_shadowRegisterRetFromTwoPointsAddr = 0;

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
std::atomic<uint64_t> g_registerImageEnterCount{0};
std::atomic<uint64_t> g_registerImageBlockedCount{0};
std::atomic<uint64_t> g_registerImageStaticStampCount{0};
std::atomic<uint64_t> g_registerImageEmitterStampCount{0};
std::atomic<uint64_t> g_registerImageSelectionCount{0};
std::atomic<uint64_t> g_registerImageOcclusionCount{0};
std::atomic<uint64_t> g_registerImageWithParamsCount{0};
std::atomic<uint64_t> g_registerImageObjectBridgeCount{0};
std::atomic<uint64_t> g_registerImageFromPointCount{0};
std::atomic<uint64_t> g_registerImageFromTwoPointsCount{0};
std::atomic<uint64_t> g_registerImageUnknownSourceCount{0};
std::atomic<uint64_t> g_cunitUiRecordSetUnitShadowEnterCount{0};
std::atomic<uint64_t> g_cunitUiRecordSetUnitShadowBlockedCount{0};
std::atomic<uint64_t> g_cunitUiRecordSetStructureShadowEnterCount{0};
std::atomic<uint64_t> g_cunitUiRecordSetStructureShadowBlockedCount{0};

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

bool ReadCStringPreview(int strPtr, char *out, size_t outSize) {
  if (!out || outSize == 0)
    return false;
  out[0] = '\0';
  if (strPtr == 0)
    return false;

  const auto *p =
      reinterpret_cast<const char *>(
          static_cast<uintptr_t>(static_cast<uint32_t>(strPtr)));
  size_t n = 0;
  for (; n + 1 < outSize; ++n) {
    if (!IsReadableRange(p + n, 1))
      break;
    const char c = p[n];
    if (c == '\0')
      break;
    out[n] = (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  out[n] = '\0';
  return n > 0;
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

int CallShadowRegisterImageEntryOriginal(void* thisPtr, int keyPtr,
                                         void* sizePtr, void* posPtr,
                                         int ownerArg, int typeArg) {
  if (g_trampolineShadowRegisterImageEntry) {
    return g_trampolineShadowRegisterImageEntry(thisPtr, nullptr, keyPtr,
                                                sizePtr, posPtr, ownerArg,
                                                typeArg);
  }
  if (g_originalShadowRegisterImageEntry) {
    return g_originalShadowRegisterImageEntry(thisPtr, nullptr, keyPtr,
                                              sizePtr, posPtr, ownerArg,
                                              typeArg);
  }
  return -1;
}

int CallCUnitUiRecordSetUnitShadowOriginal(void *thisPtr, int shadowNamePtr) {
  if (g_trampolineCUnitUiRecordSetUnitShadow)
    return g_trampolineCUnitUiRecordSetUnitShadow(thisPtr, shadowNamePtr);
  if (g_originalCUnitUiRecordSetUnitShadow)
    return g_originalCUnitUiRecordSetUnitShadow(thisPtr, shadowNamePtr);
  return 0;
}

int CallCUnitUiRecordSetStructureShadowOriginal(void *thisPtr,
                                                int shadowNamePtr) {
  if (g_trampolineCUnitUiRecordSetStructureShadow)
    return g_trampolineCUnitUiRecordSetStructureShadow(thisPtr, shadowNamePtr);
  if (g_originalCUnitUiRecordSetStructureShadow)
    return g_originalCUnitUiRecordSetStructureShadow(thisPtr, shadowNamePtr);
  return 0;
}

ShadowRegisterSource ResolveShadowRegisterSource(uintptr_t retAddr) {
  if (retAddr == g_shadowRegisterRetStaticStampAddr)
    return ShadowRegisterSource::StaticStamp;
  if (retAddr == g_shadowRegisterRetEmitterStampAddr)
    return ShadowRegisterSource::EmitterStamp;
  if (retAddr == g_shadowRegisterRetSelectionCircleAddr)
    return ShadowRegisterSource::SelectionCircleColorFriend;
  if (retAddr == g_shadowRegisterRetMarkOcclusionAddr)
    return ShadowRegisterSource::MarkColorOcclusion;
  if (retAddr == g_shadowRegisterRetWithParamsAddr)
    return ShadowRegisterSource::WithParams;
  if (retAddr == g_shadowRegisterRetObjectBridgeAddr)
    return ShadowRegisterSource::ObjectBridge;
  if (retAddr == g_shadowRegisterRetFromPointAddr)
    return ShadowRegisterSource::FromPoint;
  if (retAddr == g_shadowRegisterRetFromTwoPointsAddr)
    return ShadowRegisterSource::FromTwoPoints;
  return ShadowRegisterSource::Unknown;
}

ShadowOwnerKind ToShadowOwnerKind(dxvk::war3::render::ObjectKind kind) {
  switch (kind) {
  case dxvk::war3::render::ObjectKind::Building:
    return ShadowOwnerKind::Building;
  case dxvk::war3::render::ObjectKind::Destructible:
    return ShadowOwnerKind::Destructible;
  case dxvk::war3::render::ObjectKind::Item:
    return ShadowOwnerKind::Item;
  case dxvk::war3::render::ObjectKind::Unit:
    return ShadowOwnerKind::Unit;
  default:
    return ShadowOwnerKind::Unknown;
  }
}

ShadowOwnerKind ResolveShadowRegisterOwnerKind(int ownerArg,
                                               uint32_t& outRawcode) {
  outRawcode = 0u;
  if (ownerArg <= 0)
    return ShadowOwnerKind::Unknown;

  const uintptr_t owner = static_cast<uintptr_t>(ownerArg);
  const uintptr_t candidates[] = {
      owner,
      owner >= 0x0Cu ? owner - 0x0Cu : 0u,
      owner >= 0x10u ? owner - 0x10u : 0u,
  };

  for (uintptr_t candidate : candidates) {
    if (candidate == 0u)
      continue;
    dxvk::war3::render::RenderObjectInfo cachedIdentity = {};
    if (QueryWidgetIdentityByPtr(reinterpret_cast<void*>(candidate),
                                 cachedIdentity)) {
      if (cachedIdentity.rawcode != 0u)
        outRawcode = cachedIdentity.rawcode;
      const ShadowOwnerKind cachedKind =
          ToShadowOwnerKind(cachedIdentity.kind);
      if (cachedKind != ShadowOwnerKind::Unknown)
        return cachedKind;
    }

    uint32_t magic = 0u;
    if (!ReadU32Safe(reinterpret_cast<const void *>(candidate), 0x0Cu, magic) ||
        magic != 0x2B5DB42Cu) {
      continue;
    }
    uint32_t rawcode = 0u;
    ReadU32Safe(reinterpret_cast<const void *>(candidate), 0x30u, rawcode);
    if (rawcode != 0u)
      outRawcode = rawcode;

    uint32_t flags5C = 0u;
    ReadU32Safe(reinterpret_cast<const void *>(candidate), 0x5Cu, flags5C);
    if ((flags5C & 0x10000u) != 0u)
      return ShadowOwnerKind::Building;
    if (rawcode != 0u && shadowfilter::IsBlockedFourCC(rawcode))
      return ShadowOwnerKind::Destructible;
    return ShadowOwnerKind::Unit;
  }

  return ShadowOwnerKind::Unknown;
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

bool ShouldBlockCUnitUiBuildingShadowWrite(uint32_t mode) {
  if constexpr (
      dxvk::war3::internal::kNativeShadowBlockCUnitUiBuildingShadowByDefault) {
    return true;
  }
  if (mode >= 2u)
    return dxvk::war3::internal::
        kNativeShadowBlockCUnitUiBuildingShadowWhenMode2;
  if (mode == 1u)
    return dxvk::war3::internal::
        kNativeShadowBlockCUnitUiBuildingShadowWhenMode1;
  return false;
}

bool ShouldBlockCUnitUiUnitShadowWrite(uint32_t mode) {
  if constexpr (
      dxvk::war3::internal::kNativeShadowBlockCUnitUiUnitShadowByDefault) {
    return true;
  }
  if (mode >= 2u)
    return dxvk::war3::internal::kNativeShadowBlockCUnitUiUnitShadowWhenMode2;
  if (mode == 1u)
    return dxvk::war3::internal::kNativeShadowBlockCUnitUiUnitShadowWhenMode1;
  return false;
}

int __fastcall Hook_CUnitUIManager_RecordSetUnitShadow(void *thisPtr,
                                                       int shadowNamePtr) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::ShadowUnitUiRecord, 8u);
  const auto callNativeOriginal = [&](int effectiveShadowNamePtr) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return CallCUnitUiRecordSetUnitShadowOriginal(
        thisPtr, effectiveShadowNamePtr);
  };
  const uint64_t calls =
      g_cunitUiRecordSetUnitShadowEnterCount.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  const bool blocked =
      shadowNamePtr != 0 && ShouldBlockCUnitUiUnitShadowWrite(mode);

  if constexpr (dxvk::war3::internal::
                    kNativeShadowCUnitUiUnitShadowStatsLogging) {
    if (!blocked && calls <= 16u) {
      char name[96] = {};
      if (shadowNamePtr == 0) {
        snprintf(name, sizeof(name), "(null)");
      } else if (!ReadCStringPreview(shadowNamePtr, name, sizeof(name))) {
        snprintf(name, sizeof(name), "(unreadable)");
      }
      war3dbg::Print(
          "DXVK War3Hook: CUnitUI unitShadow PASS calls=%llu mode=%u "
          "this=%p shadow=0x%08X name=%s\n",
          static_cast<unsigned long long>(calls), static_cast<unsigned>(mode),
          thisPtr, static_cast<unsigned>(static_cast<uint32_t>(shadowNamePtr)),
          name);
    }
  }

  if (blocked) {
    const uint64_t blockedCount =
        g_cunitUiRecordSetUnitShadowBlockedCount.fetch_add(
            1, std::memory_order_relaxed) +
        1;

    if constexpr (dxvk::war3::internal::
                      kNativeShadowCUnitUiUnitShadowStatsLogging ||
                  dxvk::war3::internal::
                      kNativeShadowCUnitUiUnitShadowVerboseLogging) {
      const bool sample =
          blockedCount <= 16u ||
          (dxvk::war3::internal::
                   kNativeShadowCUnitUiUnitShadowStatsInterval != 0u &&
           (calls % dxvk::war3::internal::
                        kNativeShadowCUnitUiUnitShadowStatsInterval) == 0u);
      if (sample || dxvk::war3::internal::
                        kNativeShadowCUnitUiUnitShadowVerboseLogging) {
        char name[96] = {};
        if (!ReadCStringPreview(shadowNamePtr, name, sizeof(name)))
          snprintf(name, sizeof(name), "(unreadable)");
        war3dbg::Print(
            "DXVK War3Hook: CUnitUI unitShadow BLOCK calls=%llu "
            "blocked=%llu mode=%u this=%p shadow=0x%08X name=%s\n",
            static_cast<unsigned long long>(calls),
            static_cast<unsigned long long>(blockedCount),
            static_cast<unsigned>(mode), thisPtr,
            static_cast<unsigned>(static_cast<uint32_t>(shadowNamePtr)), name);
      }
    }

    // Preserve Blizzard's cleanup path for record +0x4C, but suppress the
    // replacement string so the legacy unit blob shadow is never produced.
    return callNativeOriginal(0);
  }

  return callNativeOriginal(shadowNamePtr);
}

int __fastcall Hook_CUnitUIManager_RecordSetStructureShadow(void *thisPtr,
                                                            int shadowNamePtr) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::ShadowStructureUiRecord, 8u);
  const auto callNativeOriginal = [&](int effectiveShadowNamePtr) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return CallCUnitUiRecordSetStructureShadowOriginal(
        thisPtr, effectiveShadowNamePtr);
  };
  const uint64_t calls =
      g_cunitUiRecordSetStructureShadowEnterCount.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  const bool blocked =
      shadowNamePtr != 0 && ShouldBlockCUnitUiBuildingShadowWrite(mode);

  if constexpr (dxvk::war3::internal::
                    kNativeShadowCUnitUiBuildingShadowStatsLogging) {
    if (!blocked && calls <= 16u) {
      char name[96] = {};
      if (shadowNamePtr == 0) {
        snprintf(name, sizeof(name), "(null)");
      } else if (!ReadCStringPreview(shadowNamePtr, name, sizeof(name))) {
        snprintf(name, sizeof(name), "(unreadable)");
      }
      war3dbg::Print(
          "DXVK War3Hook: CUnitUI buildingShadow PASS calls=%llu mode=%u "
          "this=%p shadow=0x%08X name=%s\n",
          static_cast<unsigned long long>(calls), static_cast<unsigned>(mode),
          thisPtr, static_cast<unsigned>(static_cast<uint32_t>(shadowNamePtr)),
          name);
    }
  }

  if (blocked) {
    const uint64_t blockedCount =
        g_cunitUiRecordSetStructureShadowBlockedCount.fetch_add(
            1, std::memory_order_relaxed) +
        1;

    if constexpr (dxvk::war3::internal::
                      kNativeShadowCUnitUiBuildingShadowStatsLogging ||
                  dxvk::war3::internal::
                      kNativeShadowCUnitUiBuildingShadowVerboseLogging) {
      const bool sample =
          blockedCount <= 16u ||
          (dxvk::war3::internal::
                   kNativeShadowCUnitUiBuildingShadowStatsInterval != 0u &&
           (calls % dxvk::war3::internal::
                        kNativeShadowCUnitUiBuildingShadowStatsInterval) == 0u);
      if (sample || dxvk::war3::internal::
                        kNativeShadowCUnitUiBuildingShadowVerboseLogging) {
        char name[96] = {};
        if (!ReadCStringPreview(shadowNamePtr, name, sizeof(name)))
          snprintf(name, sizeof(name), "(unreadable)");
        war3dbg::Print(
            "DXVK War3Hook: CUnitUI buildingShadow BLOCK calls=%llu "
            "blocked=%llu mode=%u this=%p shadow=0x%08X name=%s\n",
            static_cast<unsigned long long>(calls),
            static_cast<unsigned long long>(blockedCount),
            static_cast<unsigned>(mode), thisPtr,
            static_cast<unsigned>(static_cast<uint32_t>(shadowNamePtr)), name);
      }
    }

    // Preserve Blizzard's cleanup path: the original function frees the old
    // record +0x50 string first, then only writes a new copy when EDX != 0.
    return callNativeOriginal(0);
  }

  return callNativeOriginal(shadowNamePtr);
}

// 地形阴影层入口：
// - mode=1：关闭该入口触发的 ListB（stage14 直调链路由 ListB Hook 处理）
// - mode>=2：完全禁用原生阴影层
void __fastcall Hook_Terrain_RenderShadowLayer(void *thisPtr, void *edx, int a2,
                                               int a3, int a4) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::ShadowTerrainLayer, 4u);
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
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineTerrainShadowLayer(thisPtr, edx, a2, a3, a4);
  } else if (g_originalTerrainShadowLayer) {
    War3HotHookNativeScope nativeTiming(hookTiming);
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
  War3HotHookCallTiming hookTiming(War3HotHookId::ShadowTerrainListB, 4u);
  // 2026-07-08 复核：建筑静态阴影已前移到 UnitUI buildingShadow producer
  // gate；unitShadow producer gate 清旧版单位动态黑色 blob 圆影。ListB type=4
  // 还承载 S19 建筑地面贴花/UberSplat，默认不能全杀。
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  bool blocked = false;
  const char *reason = "PassThrough";
  const bool preserveType4ByDefault =
      argType == 4 &&
      dxvk::war3::internal::kNativeShadowListBPreserveType4ByDefault;

  if (preserveType4ByDefault) {
    reason = "PreserveType4UberSplat";
  } else if constexpr (dxvk::war3::internal::kNativeShadowListBBlockAllByDefault) {
    blocked = true;
    reason = "Default_BlockListB";
  } else if (mode >= 2u &&
             dxvk::war3::internal::kNativeShadowListBBlockAllWhenMode2) {
    blocked = true;
    reason = "Mode>=2_BlockAllListB";
  } else if (mode == 1u &&
             dxvk::war3::internal::kNativeShadowListBBlockAllWhenMode1) {
    blocked = true;
    reason = "Mode1_BlockAllListB";
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
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineTerrainRenderListB(thisPtr, edx, argType, passMode);
    return;
  }
  if (g_originalTerrainRenderListB) {
    War3HotHookNativeScope nativeTiming(hookTiming);
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
  War3HotHookCallTiming hookTiming(
      War3HotHookId::ShadowProjectorFromObject, 4u);
  const auto callNativeOriginal = [&]() {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return CallShadowProjectorAddFromObjectOriginal(
        a1, a2, arg0, arg1, arg2);
  };
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
      dxvk::war3::internal::kNativeShadowBlockRuntimeObjectProjectorFallback ||
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
    return callNativeOriginal();

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

  if (!blocked && fromRuntimePath &&
      dxvk::war3::internal::kNativeShadowBlockRuntimeObjectProjectorFallback) {
    blocked = true;
    reason = "FallbackBlockRuntimeObjectProjector";
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
    if constexpr (dxvk::war3::internal::
                      kNativeShadowRuntimeObjectProjectorFallbackLogging) {
      static std::atomic<uint32_t> s_fallbackLogCount{0};
      const uint32_t n =
          s_fallbackLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 64u || (n % 1000u) == 0u) {
        war3dbg::Print(
            "DXVK War3Hook: RuntimeObjectProjectorFallback BLOCK n=%u mode=%u "
            "fromRuntime=%d fromAlt=%d key='%s' arg0=%p fourcc=0x%08X "
            "a1=%p reason=%s\n",
            static_cast<unsigned>(n), static_cast<unsigned>(mode),
            fromRuntimePath ? 1 : 0, fromAltPath ? 1 : 0,
            hasKey ? keyBuf : "(null)", arg0,
            static_cast<unsigned>(arg0FourCC), a1, reason);
      }
    }
    return -1;
  }

  return callNativeOriginal();
}

int __fastcall Hook_ShadowProjector_Add_Simple(void *a1, void *a2, int arg0,
                                               int arg1, int arg2, int arg3,
                                               int arg4, int arg5, int arg6) {
  War3HotHookCallTiming hookTiming(War3HotHookId::ShadowProjectorSimple, 4u);
  const auto callNativeOriginal = [&]() {
    War3HotHookNativeScope nativeTiming(hookTiming);
    return CallShadowProjectorAddSimpleOriginal(
        a1, a2, arg0, arg1, arg2, arg3, arg4, arg5, arg6);
  };
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
    return callNativeOriginal();
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

  return callNativeOriginal();
}

int __fastcall Hook_TerrainShadow_RegisterImageEntry(void *thisPtr, void *edx,
                                                     int keyPtr, void *sizePtr,
                                                     void *posPtr,
                                                     int ownerArg,
                                                     int typeArg) {
  (void)edx;
  (void)sizePtr;
  (void)posPtr;

  g_registerImageEnterCount.fetch_add(1, std::memory_order_relaxed);

  const uint32_t mode = War3RenderState::GetNativeShadowMode();
#if defined(_MSC_VER)
  const uintptr_t retAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());
#elif defined(__GNUC__)
  const uintptr_t retAddr =
      reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#else
  const uintptr_t retAddr = 0u;
#endif
  const uintptr_t gameBase = GetGameDllBase();
  const uint32_t retRva =
      (gameBase != 0u && retAddr >= gameBase)
          ? static_cast<uint32_t>(retAddr - gameBase)
          : 0u;
  const ShadowRegisterSource source = ResolveShadowRegisterSource(retAddr);
  switch (source) {
  case ShadowRegisterSource::StaticStamp:
    g_registerImageStaticStampCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::EmitterStamp:
    g_registerImageEmitterStampCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::SelectionCircleColorFriend:
    g_registerImageSelectionCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::MarkColorOcclusion:
    g_registerImageOcclusionCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::WithParams:
    g_registerImageWithParamsCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::ObjectBridge:
    g_registerImageObjectBridgeCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::FromPoint:
    g_registerImageFromPointCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::FromTwoPoints:
    g_registerImageFromTwoPointsCount.fetch_add(1, std::memory_order_relaxed);
    break;
  case ShadowRegisterSource::Unknown:
    g_registerImageUnknownSourceCount.fetch_add(1, std::memory_order_relaxed);
    break;
  default:
    break;
  }

  char keyBuf[128] = {};
  const bool hasKey = shadowfilter::ReadAsciiCStringSafe(
      reinterpret_cast<const char *>(static_cast<uintptr_t>(keyPtr)), keyBuf,
      sizeof(keyBuf));

  uint32_t ownerRawcode = 0u;
  const ShadowOwnerKind ownerKind =
      ResolveShadowRegisterOwnerKind(ownerArg, ownerRawcode);

  ShadowRegisterContext ctx = {};
  ctx.mode = mode;
  ctx.source = source;
  ctx.ownerKind = ownerKind;
  ctx.ownerRawcode = ownerRawcode;
  ctx.retRva = retRva;
  ctx.argType = typeArg;
  ctx.hasKey = hasKey;
  ctx.key = hasKey ? keyBuf : nullptr;

  const ShadowRegisterDecision decision =
      shadowfilter::DecideRegisterImage(ctx);
  if (decision.blocked)
    g_registerImageBlockedCount.fetch_add(1, std::memory_order_relaxed);

  if constexpr (dxvk::war3::internal::kNativeShadowRegisterImageVerboseLogging) {
    static std::atomic<uint32_t> s_verboseCount{0};
    const uint32_t n = s_verboseCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 80u || (n % 1000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook: RegisterImage %s mode=%u source=%s owner=%s "
          "raw=0x%08X type=%d key='%s' ret=0x%08X reason=%s\n",
          decision.blocked ? "BLOCK" : "PASS", static_cast<unsigned>(mode),
          shadowfilter::ToString(source), shadowfilter::ToString(ownerKind),
          static_cast<unsigned>(ownerRawcode), typeArg,
          hasKey ? keyBuf : "(null)", static_cast<unsigned>(retRva),
          decision.reason);
    }
  }

  if constexpr (dxvk::war3::internal::kNativeShadowRegisterImageStatsLogging ||
                dxvk::war3::internal::kNativeShadowRegisterSourceStatsLogging) {
    const uint64_t calls =
        g_registerImageEnterCount.load(std::memory_order_relaxed);
    if ((calls % dxvk::war3::internal::kNativeShadowRegisterStatsInterval) ==
        0u) {
      char buf[640];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: RegisterImage stats calls=%llu "
               "blocked=%llu srcStatic=%llu srcEmitter=%llu "
               "srcSelection=%llu srcOcclusion=%llu srcWithParams=%llu "
               "srcObjectBridge=%llu srcFromPoint=%llu "
               "srcFromTwoPoints=%llu srcUnknown=%llu mode=%u "
               "lastSource=%s lastOwner=%s lastType=%d lastKey=%s "
               "lastReason=%s",
               static_cast<unsigned long long>(calls),
               static_cast<unsigned long long>(
                   g_registerImageBlockedCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageStaticStampCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageEmitterStampCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageSelectionCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageOcclusionCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageWithParamsCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageObjectBridgeCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageFromPointCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageFromTwoPointsCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned long long>(
                   g_registerImageUnknownSourceCount.load(
                       std::memory_order_relaxed)),
               static_cast<unsigned>(mode), shadowfilter::ToString(source),
               shadowfilter::ToString(ownerKind), typeArg,
               hasKey ? keyBuf : "(null)", decision.reason);
      ::dxvk::Logger::info(buf);
    }
  }

  if (decision.blocked)
    return -1;

  return CallShadowRegisterImageEntryOriginal(thisPtr, keyPtr, sizePtr, posPtr,
                                              ownerArg, typeArg);
}

// =====================================================================
// 2026-05-30: CDoodads 贴花阴影拦截（魔兽自带可见静态阴影 + path blocker 治理）
// =====================================================================
// TerrainShadow_ToggleStaticStampFromObject (0x74DB30) 是 CDoodads
// （树木/装饰物/可破坏物/path blocker）的"地面贴花阴影"对象级注册入口
// （写 RegisterImage type=0）。它由 CDoodads_CreateDoodadAndActivate /
// EnableFeatures / SetTodAndRefreshStamp 在对象创建、特性重激活、TOD 变化时
// 调用。历史上只拦了 ListA 直写路径（ShadowPath_StaticStamp_Toggle 0x74E420），
// **没有拦这条 RegisterImage 贴花路径**，所以魔兽自带的树木/装饰物地面贴花
// 阴影一直可见。
//
// 这是干净 __thiscall(this, doodadSlot, enable)，可安全 hook。
// 策略：仅当 canonical runtime A/B gate 开启且 enable!=0 时跳过 enable
// 写入（return 0），让贴花阴影不被注册。enable==0（移除）始终放行，
// 确保已注册的 stamp 能被清除。
//
// 注意：姊妹函数 ToggleEmitterStamp (0x74DE40) 是 __userpurge（edi=this 作为
// 隐式参数），不能用标准 __fastcall trampoline 安全 hook（passthrough 会丢
// edi）。emitter stamp（type=4）主要是发光体/技能特效/腐地 puff，不是静态
// 树木/建筑阴影本体，因此本轮不拦它，只拦 StaticStamp（type=0）这条阴影主路径。
using TerrainShadowToggleStaticStampFn = int(__fastcall *)(void *, void *, int,
                                                           int);
TerrainShadowToggleStaticStampFn g_originalDoodadStaticStamp = nullptr;
TerrainShadowToggleStaticStampFn g_trampolineDoodadStaticStamp = nullptr;

std::atomic<uint64_t> g_doodadStaticStampEnterCount{0};
std::atomic<uint64_t> g_doodadStaticStampBlockedCount{0};
std::atomic<uint64_t> g_doodadStaticStampPassthroughCleanupCount{0};
std::atomic<uint64_t> g_doodadStaticStampGateActiveCount{0};
std::atomic<uint64_t> g_doodadEmitterStampEnterCount{0};
std::atomic<uint64_t> g_doodadEmitterStampBlockedCount{0};

bool NativeDoodadStaticStampRuntimeGateActive() {
  static const bool gateActive = [] {
    const char *value =
        std::getenv("DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW");
    // Historical diagnostic spelling remains a fallback only. The canonical
    // policy/UI/report state is DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW.
    if (value == nullptr)
      value = std::getenv("DXVK_WAR3_NATIVE_DOODAD_STATIC_STAMP");
    if (value != nullptr)
      return value[0] == '1' && value[1] == '\0';
    return dxvk::war3::internal::
        kNativeDoodadStaticStampRuntimeGateDefault;
  }();
  return gateActive;
}

int CallDoodadStaticStampOriginal(void *thisPtr, void *edx, int doodadSlot,
                                  int enable) {
  if (g_trampolineDoodadStaticStamp)
    return g_trampolineDoodadStaticStamp(thisPtr, edx, doodadSlot, enable);
  if (g_originalDoodadStaticStamp)
    return g_originalDoodadStaticStamp(thisPtr, edx, doodadSlot, enable);
  return 0;
}

int __fastcall Hook_Doodad_ToggleStaticStampFromObject(void *thisPtr, void *edx,
                                                       int doodadSlot,
                                                       int enable) {
  g_doodadStaticStampEnterCount.fetch_add(1, std::memory_order_relaxed);
  const bool gateActive = NativeDoodadStaticStampRuntimeGateActive();
  if (gateActive)
    g_doodadStaticStampGateActiveCount.fetch_add(
        1, std::memory_order_relaxed);

  // Cleanup/remove is an invariant, not an A/B option: even with the runtime
  // gate active it must reach the original function so an existing type=0
  // stamp can be retired.
  if (enable == 0) {
    g_doodadStaticStampPassthroughCleanupCount.fetch_add(
        1, std::memory_order_relaxed);
    return CallDoodadStaticStampOriginal(thisPtr, edx, doodadSlot, enable);
  }

  // The runtime gate is the sole blocking authority. NativeShadowMode is
  // deliberately not consulted, so the default-installed hook is a clean
  // pass-through unless the exact A/B flag is active.
  const bool blocked = gateActive && enable != 0;

  if constexpr (dxvk::war3::internal::kNativeShadowDoodadStampStatsLogging) {
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((calls % dxvk::war3::internal::kNativeShadowDoodadStampStatsInterval) ==
        0u) {
      // 写 war3_d3d9.log（Logger::info），不是 DebugView，方便用户直接读取。
      char buf[160];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: DoodadStaticStamp calls=%u blocked=%llu "
               "gate=%u enable=%d",
               static_cast<unsigned>(calls),
               static_cast<unsigned long long>(
                   g_doodadStaticStampBlockedCount.load(
                       std::memory_order_relaxed)),
               gateActive ? 1u : 0u, enable);
      ::dxvk::Logger::info(buf);
    }
  }

  if (blocked) {
    g_doodadStaticStampBlockedCount.fetch_add(1, std::memory_order_relaxed);
    // 返回 0 表示"无 stamp 写入"；调用方据此不回填 stamp index（保持 -1）。
    return 0;
  }

  return CallDoodadStaticStampOriginal(thisPtr, edx, doodadSlot, enable);
}

// =====================================================================
// Phase 7.143 证伪归档: ListA render hook
// =====================================================================
// 0x7370A0 / 0x737110 曾被误判为静态阴影 consumer。用户实机验证会
// 破坏悬崖/地形 tile；IDA 复核显示 RenderAllEntries 通过 sub_6F725F80
// 按 148-byte stride 取 terrain tile geometry。此 hook 保留作历史诊断，
// 默认禁止安装，不应再作为静态阴影/path blocker 方案启用。
using TerrainShadowListARenderFn = int(__fastcall *)(void *, void *);
TerrainShadowListARenderFn g_trampolineListARenderPreparedGroups = nullptr;
TerrainShadowListARenderFn g_trampolineListARenderAllEntries = nullptr;

std::atomic<uint64_t> g_listARenderPreparedGroupsEnterCount{0};
std::atomic<uint64_t> g_listARenderPreparedGroupsBlockedCount{0};
std::atomic<uint64_t> g_listARenderAllEntriesEnterCount{0};
std::atomic<uint64_t> g_listARenderAllEntriesBlockedCount{0};

int __fastcall Hook_TerrainShadow_ListA_RenderPreparedGroups(void *thisPtr,
                                                             void *edx) {
  g_listARenderPreparedGroupsEnterCount.fetch_add(1, std::memory_order_relaxed);
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  const bool blocked =
      (mode == 1u &&
       dxvk::war3::internal::kNativeShadowBlockListARenderWhenMode1) ||
      mode >= 2u;

  if constexpr (dxvk::war3::internal::kNativeShadowListARenderStatsLogging) {
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((calls % dxvk::war3::internal::kNativeShadowListARenderStatsInterval) ==
        0u) {
      war3dbg::Print(
          "DXVK War3Hook: ListA_RenderPreparedGroups calls=%u blocked=%llu "
          "mode=%u\n",
          static_cast<unsigned>(calls),
          static_cast<unsigned long long>(
              g_listARenderPreparedGroupsBlockedCount.load(
                  std::memory_order_relaxed)),
          static_cast<unsigned>(mode));
    }
  }

  if (blocked) {
    g_listARenderPreparedGroupsBlockedCount.fetch_add(
        1, std::memory_order_relaxed);
    return 0;
  }

  if (g_trampolineListARenderPreparedGroups)
    return g_trampolineListARenderPreparedGroups(thisPtr, edx);
  return 0;
}

int __fastcall Hook_TerrainShadow_ListA_RenderAllEntries(void *thisPtr,
                                                         void *edx) {
  g_listARenderAllEntriesEnterCount.fetch_add(1, std::memory_order_relaxed);
  const uint32_t mode = War3RenderState::GetNativeShadowMode();
  const bool blocked =
      (mode == 1u &&
       dxvk::war3::internal::kNativeShadowBlockListARenderWhenMode1) ||
      mode >= 2u;

  if constexpr (dxvk::war3::internal::kNativeShadowListARenderStatsLogging) {
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((calls % dxvk::war3::internal::kNativeShadowListARenderStatsInterval) ==
        0u) {
      war3dbg::Print(
          "DXVK War3Hook: ListA_RenderAllEntries calls=%u blocked=%llu "
          "mode=%u\n",
          static_cast<unsigned>(calls),
          static_cast<unsigned long long>(
              g_listARenderAllEntriesBlockedCount.load(
                  std::memory_order_relaxed)),
          static_cast<unsigned>(mode));
    }
  }

  if (blocked) {
    g_listARenderAllEntriesBlockedCount.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }

  if (g_trampolineListARenderAllEntries)
    return g_trampolineListARenderAllEntries(thisPtr, edx);
  return 0;
}

// =====================================================================
// Phase 7.100/7.101: TerrainShadow_WriteMaskRegion hook (diagnostic-only)
// =====================================================================
// 论文 §6 §7.1 曾推断：
//   if (a4) result = 4;
//   else    result = *(unsigned __int16 *)(a2 + 268);   // a2 + 0x10C
// 其中 result 是 maskIdx，idx==3 可屏蔽建筑阴影。后续实测推翻：
// a2+0x10C 是 footprint/shape 结果，样本为 6/7/9；该函数服务
// fog/LOS/path/footprint 共享 mask grid，不能作为生产 reject 点。
// 当前默认仅安装 hook 与计数器，reject 开关保持关闭。
typedef int (__thiscall *TerrainShadowWriteMaskRegionFn)(void *thisPtr, void *a2,
                                                         int a3, void *a4,
                                                         int a5);
TerrainShadowWriteMaskRegionFn g_trampolineTerrainShadowWriteMaskRegion = nullptr;

// Phase 7.116：DispatchToShape 签名（__thiscall, this+a2+a3）。
// IDA: int __thiscall TerrainShadow_DispatchToShape(_DWORD *this, _DWORD *a2, int a3)
typedef int (__thiscall *TerrainShadowDispatchToShapeFn)(void *thisPtr, void *a2,
                                                         int a3);
TerrainShadowDispatchToShapeFn g_trampolineTerrainShadowDispatchToShape = nullptr;

std::atomic<uint64_t> g_writeMaskRegionEnterCount{0};
std::atomic<uint64_t> g_writeMaskRegionRejectedIdx3Count{0};
std::atomic<uint64_t> g_writeMaskRegionPassFogCount{0};   // idx==0
std::atomic<uint64_t> g_writeMaskRegionPassLosCount{0};   // idx==1
std::atomic<uint64_t> g_writeMaskRegionPassPathCount{0};  // idx==2
std::atomic<uint64_t> g_writeMaskRegionPassOtherCount{0};

// Phase 7.116：DispatchToShape 旧实验 atomic 计数器。
// 默认不安装 hook；灰度开启时用于 control plane 验证拦截命中频率。
std::atomic<uint64_t> g_dispatchToShapeEnterCount{0};
std::atomic<uint64_t> g_dispatchToShapeRejectedCount{0};
std::atomic<uint64_t> g_dispatchToShapeFromRebuildMaskCount{0};
std::atomic<uint64_t> g_dispatchToShapeFromShadowSetupCount{0};
std::atomic<uint64_t> g_dispatchToShapeFromOtherCallerCount{0};

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

// Phase 7.116：DispatchToShape caller 范围。
// sub_6F21A890 (widget shadow setup), 0xAF
constexpr uintptr_t kShadowSetup21A890Rva = 0x21A890u;
constexpr uintptr_t kShadowSetup21A890Size = 0xAFu;
// sub_6F21A9A0, 0x73
constexpr uintptr_t kShadowSetup21A9A0Rva = 0x21A9A0u;
constexpr uintptr_t kShadowSetup21A9A0Size = 0x73u;
// sub_6F21AA60, 0x6F
constexpr uintptr_t kShadowSetup21AA60Rva = 0x21AA60u;
constexpr uintptr_t kShadowSetup21AA60Size = 0x6Fu;

// Phase 7.108：ShadowProjector 永久 atomic 计数器。
// 这条路径独立于 D3D9 mesh draw（CTerrainUberSplats 系统），
// 必须永久计数才能在不开 verbose 编译期开关时也判断
// path blocker 是否走这条路径。

int __thiscall Hook_TerrainShadow_WriteMaskRegion(void *thisPtr, void *a2,
                                                   int a3, void *a4, int a5) {
  g_writeMaskRegionEnterCount.fetch_add(1, std::memory_order_relaxed);

  // Phase 7.112：caller-aware 静态阴影屏蔽。
  // 通过返回地址判定调用来源，仅拦截"建筑物 shadow footprint"路径。
  // fog/LOS/visibility 等共享路径完全不受影响。
  // GCC/MinGW 用 __builtin_return_address；MSVC 用 _ReturnAddress。
  //
  // 性能：caller 分桶 + reject 检查只在配置开关启用时执行，默认情况下
  // hook 仅做 enter counter 的原子累加（开销 ~5ns/call × 6300/frame = 32us/frame）。
  const bool needsCallerInspection =
      dxvk::war3::internal::kNativeStaticShadowHideBuildingFootprintEnabled ||
      dxvk::war3::internal::kNativeStaticShadowHideDestructibleFootprintEnabled ||
      dxvk::war3::internal::kNativeStaticShadowMaskCallerDiagnostics ||
      dxvk::war3::internal::kNativeStaticShadowMaskHideEnabled;

  bool fromBuildingStamp = false;
  bool fromRegisterFootprint = false;
  bool fromRebuildMask = false;
  bool fromActorRuntime = false;
  bool fromForObject = false;

  if (needsCallerInspection) {
#if defined(_MSC_VER) && !defined(__GNUC__)
    const uintptr_t retAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const uintptr_t retAddr =
        reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const uintptr_t gameBase = GetGameDllBase();
    const uintptr_t buildingStampStart =
        gameBase + kCUnitStampBuildingShadowFootprintRva;
    const uintptr_t registerFootprintStart =
        gameBase + kCWidgetRegisterFootprintRva;
    const uintptr_t rebuildMaskStart = gameBase + kTerrainShadowRebuildMaskRva;
    const uintptr_t fromActorRuntimeStart =
        gameBase + kTerrainShadowFromActorRuntimeRva;
    const uintptr_t forObjectStart = gameBase + kTerrainShadowForObjectRva;

    fromBuildingStamp = IsAddrInFuncRange(
        retAddr, buildingStampStart, kCUnitStampBuildingShadowFootprintSize);
    fromRegisterFootprint = IsAddrInFuncRange(
        retAddr, registerFootprintStart, kCWidgetRegisterFootprintSize);
    fromRebuildMask = IsAddrInFuncRange(retAddr, rebuildMaskStart,
                                         kTerrainShadowRebuildMaskSize);
    fromActorRuntime = IsAddrInFuncRange(retAddr, fromActorRuntimeStart,
                                          kTerrainShadowFromActorRuntimeSize);
    fromForObject = IsAddrInFuncRange(retAddr, forObjectStart,
                                       kTerrainShadowForObjectSize);

    // 来源分桶（仅诊断开启时累加）
    if (dxvk::war3::internal::kNativeStaticShadowMaskCallerDiagnostics) {
      if (fromBuildingStamp)
        g_writeMaskRegionFromBuildingStampCount.fetch_add(
            1, std::memory_order_relaxed);
      else if (fromRegisterFootprint)
        g_writeMaskRegionFromRegisterFootprintCount.fetch_add(
            1, std::memory_order_relaxed);
      else if (fromRebuildMask)
        g_writeMaskRegionFromRebuildMaskCount.fetch_add(
            1, std::memory_order_relaxed);
      else if (fromActorRuntime)
        g_writeMaskRegionFromActorRuntimeCount.fetch_add(
            1, std::memory_order_relaxed);
      else if (fromForObject)
        g_writeMaskRegionFromForObjectCount.fetch_add(
            1, std::memory_order_relaxed);
      else
        g_writeMaskRegionFromOtherCallerCount.fetch_add(
            1, std::memory_order_relaxed);
    }
  }

  // 静态消除未使用变量警告（reject 路径未启用时编译器可能优化掉）。
  (void)fromRegisterFootprint;
  (void)fromRebuildMask;
  (void)fromActorRuntime;
  (void)fromForObject;

  // Phase 7.112 第一刀旧实验：仅 reject 来自 CUnit_StampBuildingShadowFootprint。
  // 后续实测该 caller 在重建窗口 enterCount=0，默认关闭，仅保留作 A/B 诊断。
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

  // Phase 7.112 第二刀旧实验：检查 widget+0x60 的 SHADOW_FOOTPRINT_PRESENT 标志位。
  // 来自 CUnit_StampBuildingShadowFootprint 反编译：
  //   *(_DWORD *)(unit + 96) |= 0x400u;   // (unit+0x60) bit10 = SHADOW_FOOTPRINT_FLAG
  // 当 widget 在 lifecycle 中已经被标记为"具备 shadow footprint"，无论它现在
  // 走的是 RebuildMask / ForObject / RegisterFootprint 哪条 caller，
  // 这次 WriteMaskRegion 调用的写入对象都涉及该 widget 的 shadow footprint mask bit。
  //
  // 后续 WMR dump 显示 RebuildMask 路径上的 a2 不是标准 widget，默认关闭。

  // Phase 7.112 第二刀诊断：前 12 次 fire 时 dump widget +0x60 +0x14C 字段语义。
  // 默认关闭——已经 dump 过 12 次确认 widget 字段语义（见 AGENTS Phase 7.112 节）。
  // 需要再 dump 时启用 kNativeStaticShadowHideBuildingFootprintDebugLog。
  if constexpr (false) {  // disabled by default
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

// Phase 7.116：TerrainShadow_DispatchToShape 旧实验 hook。
// 历史假设认为它是建筑/装饰物/可破坏物 footprint shadow 的独立写入点；
// 后续实测 dispatchToShapeEnterCount=0，生产默认已关闭，仅保留灰度诊断。
// 若重新开启 reject，入口 return 0，caller 侧应把它当非命中 shape/result。
int __thiscall Hook_TerrainShadow_DispatchToShape(void *thisPtr, void *a2,
                                                   int a3) {
  g_dispatchToShapeEnterCount.fetch_add(1, std::memory_order_relaxed);

  // caller 分桶诊断（默认关，需要调试时开 kNativeStaticShadowDispatchToShapeCallerDiagnostics）。
  if constexpr (dxvk::war3::internal::
                    kNativeStaticShadowDispatchToShapeCallerDiagnostics) {
#if defined(_MSC_VER) && !defined(__GNUC__)
    const uintptr_t retAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const uintptr_t retAddr =
        reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    const uintptr_t gameBase = GetGameDllBase();
    const bool fromRebuild =
        IsAddrInFuncRange(retAddr, gameBase + kTerrainShadowRebuildMaskRva,
                          kTerrainShadowRebuildMaskSize);
    const bool fromSetup21A890 = IsAddrInFuncRange(
        retAddr, gameBase + kShadowSetup21A890Rva, kShadowSetup21A890Size);
    const bool fromSetup21A9A0 = IsAddrInFuncRange(
        retAddr, gameBase + kShadowSetup21A9A0Rva, kShadowSetup21A9A0Size);
    const bool fromSetup21AA60 = IsAddrInFuncRange(
        retAddr, gameBase + kShadowSetup21AA60Rva, kShadowSetup21AA60Size);
    if (fromRebuild) {
      g_dispatchToShapeFromRebuildMaskCount.fetch_add(
          1, std::memory_order_relaxed);
    } else if (fromSetup21A890 || fromSetup21A9A0 || fromSetup21AA60) {
      g_dispatchToShapeFromShadowSetupCount.fetch_add(
          1, std::memory_order_relaxed);
    } else {
      g_dispatchToShapeFromOtherCallerCount.fetch_add(
          1, std::memory_order_relaxed);
    }
  }

  // reject 默认关闭；仅用于重新 A/B DispatchToShape 假设。
  if constexpr (dxvk::war3::internal::
                    kNativeStaticShadowDispatchToShapeRejectEnabled) {
    const uint64_t logIdx = g_dispatchToShapeRejectedCount.fetch_add(
        1, std::memory_order_relaxed);
    if constexpr (dxvk::war3::internal::
                      kNativeStaticShadowDispatchToShapeDebugLog) {
      if (logIdx < 8u) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "DXVK War3Hook[Shadow]: STATIC SHADOW DISPATCH REJECT #%llu "
                 "this=%p a2=%p a3=0x%X",
                 static_cast<unsigned long long>(logIdx + 1u), thisPtr, a2,
                 static_cast<unsigned int>(a3));
        ::dxvk::Logger::info(buf);
      }
    }
    // 返回 0：caller 用返回值当形状写入完成标记或下一步 result 索引；
    // 返回非命中值让上游 caller 跳过 Box/Poly 写入即可。
    // 历史 Add_FromObject hook 拒绝时也用 -1，DispatchToShape 没有形状命中时
    // 默认返回 result（switch fall-through），非命中值即可。这里返回 0。
    return 0;
  }

  if (g_trampolineTerrainShadowDispatchToShape)
    return g_trampolineTerrainShadowDispatchToShape(thisPtr, a2, a3);
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

// Phase 7.116：DispatchToShape 永久 atomic 读取器。
uint64_t QueryDispatchToShapeEnterCount() {
  return g_dispatchToShapeEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryDispatchToShapeRejectedCount() {
  return g_dispatchToShapeRejectedCount.load(std::memory_order_relaxed);
}
uint64_t QueryDispatchToShapeFromRebuildMaskCount() {
  return g_dispatchToShapeFromRebuildMaskCount.load(std::memory_order_relaxed);
}
uint64_t QueryDispatchToShapeFromShadowSetupCount() {
  return g_dispatchToShapeFromShadowSetupCount.load(std::memory_order_relaxed);
}
uint64_t QueryDispatchToShapeFromOtherCallerCount() {
  return g_dispatchToShapeFromOtherCallerCount.load(std::memory_order_relaxed);
}

// 2026-05-30：CDoodads 贴花阴影拦截诊断读取器。
uint64_t QueryDoodadStaticStampEnterCount() {
  return g_doodadStaticStampEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryDoodadStaticStampBlockedCount() {
  return g_doodadStaticStampBlockedCount.load(std::memory_order_relaxed);
}
uint64_t QueryDoodadStaticStampPassthroughCleanupCount() {
  return g_doodadStaticStampPassthroughCleanupCount.load(
      std::memory_order_relaxed);
}
uint64_t QueryDoodadStaticStampGateActiveCount() {
  return g_doodadStaticStampGateActiveCount.load(std::memory_order_relaxed);
}
uint64_t QueryDoodadEmitterStampEnterCount() {
  return g_doodadEmitterStampEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryDoodadEmitterStampBlockedCount() {
  return g_doodadEmitterStampBlockedCount.load(std::memory_order_relaxed);
}

// 2026-05-30 根因突破：ListA stamp 渲染拦截诊断读取器。
uint64_t QueryListARenderPreparedGroupsEnterCount() {
  return g_listARenderPreparedGroupsEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryListARenderPreparedGroupsBlockedCount() {
  return g_listARenderPreparedGroupsBlockedCount.load(
      std::memory_order_relaxed);
}
uint64_t QueryListARenderAllEntriesEnterCount() {
  return g_listARenderAllEntriesEnterCount.load(std::memory_order_relaxed);
}
uint64_t QueryListARenderAllEntriesBlockedCount() {
  return g_listARenderAllEntriesBlockedCount.load(std::memory_order_relaxed);
}

bool InstallShadowHooks(const ShadowHookAddresses &addrs) {
  // 先保存原函数指针与来源地址，再执行分项安装，保证失败时仍可回退。
  g_originalDoodadStaticStamp =
      reinterpret_cast<TerrainShadowToggleStaticStampFn>(
          addrs.terrainShadowToggleStaticStampFromObjectAddr);
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
  g_originalShadowRegisterImageEntry =
      reinterpret_cast<ShadowRegisterImageEntryFn>(
          addrs.shadowRegisterImageEntryAddr);
  g_originalShadowPathStaticStampToggle =
      reinterpret_cast<ShadowPathStaticStampToggleFn>(
          addrs.shadowPathStaticStampToggleAddr);
  g_originalCUnitUiRecordSetUnitShadow =
      reinterpret_cast<CUnitUiRecordSetUnitShadowFn>(
          addrs.cunitUiRecordSetUnitShadowAddr);
  g_originalCUnitUiRecordSetStructureShadow =
      reinterpret_cast<CUnitUiRecordSetStructureShadowFn>(
          addrs.cunitUiRecordSetStructureShadowAddr);
  g_shadowPathObjectProjectorRuntimeAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowPathObjectProjectorRuntimeAddr);
  g_shadowPathObjectProjectorJassBridgeAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowPathObjectProjectorJassBridgeAddr);
  g_shadowProjectorSimpleBridgeAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowProjectorSimpleBridgeAddr);
  g_shadowRegisterRetWithParamsAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetWithParamsAddr);
  g_shadowRegisterRetSelectionCircleAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetSelectionCircleAddr);
  g_shadowRegisterRetStaticStampAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetStaticStampAddr);
  g_shadowRegisterRetEmitterStampAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetEmitterStampAddr);
  g_shadowRegisterRetObjectBridgeAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetObjectBridgeAddr);
  g_shadowRegisterRetMarkOcclusionAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetMarkOcclusionAddr);
  g_shadowRegisterRetFromPointAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetFromPointAddr);
  g_shadowRegisterRetFromTwoPointsAddr =
      reinterpret_cast<uintptr_t>(addrs.shadowRegisterRetFromTwoPointsAddr);

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

  if constexpr (dxvk::war3::internal::kWar3ShadowTypeRecordHookEnabled &&
                dxvk::war3::internal::
                    kNativeShadowCUnitUiUnitShadowHookEnabled) {
    if (addrs.cunitUiRecordSetUnitShadowAddr != nullptr) {
      const bool ok = InstallMinHook(
          addrs.cunitUiRecordSetUnitShadowAddr,
          reinterpret_cast<LPVOID>(&Hook_CUnitUIManager_RecordSetUnitShadow),
          reinterpret_cast<LPVOID *>(&g_trampolineCUnitUiRecordSetUnitShadow),
          "Shadow", "CUnitUIManager_RecordSetUnitShadow", false, true);
      anyInstalled |= ok;
      char buf[160];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: CUnitUI RecordSetUnitShadow "
               "install addr=%p result=%s defaultBlock=%d",
               addrs.cunitUiRecordSetUnitShadowAddr, ok ? "ok" : "fail",
               dxvk::war3::internal::
                   kNativeShadowBlockCUnitUiUnitShadowByDefault
                   ? 1
                   : 0);
      ::dxvk::Logger::info(buf);
    } else {
      ::dxvk::Logger::info(
          "DXVK War3Hook[Shadow]: CUnitUI RecordSetUnitShadow install "
          "SKIPPED - nullptr");
    }
  }

  if constexpr (dxvk::war3::internal::kWar3ShadowTypeRecordHookEnabled &&
                dxvk::war3::internal::
                    kNativeShadowCUnitUiBuildingShadowHookEnabled) {
    if (addrs.cunitUiRecordSetStructureShadowAddr != nullptr) {
      const bool ok = InstallMinHook(
          addrs.cunitUiRecordSetStructureShadowAddr,
          reinterpret_cast<LPVOID>(
              &Hook_CUnitUIManager_RecordSetStructureShadow),
          reinterpret_cast<LPVOID *>(
              &g_trampolineCUnitUiRecordSetStructureShadow),
          "Shadow", "CUnitUIManager_RecordSetStructureShadow", false, true);
      anyInstalled |= ok;
      char buf[160];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: CUnitUI RecordSetStructureShadow "
               "install addr=%p result=%s defaultBlock=%d",
               addrs.cunitUiRecordSetStructureShadowAddr, ok ? "ok" : "fail",
               dxvk::war3::internal::
                   kNativeShadowBlockCUnitUiBuildingShadowByDefault
                   ? 1
                   : 0);
      ::dxvk::Logger::info(buf);
    } else {
      ::dxvk::Logger::info(
          "DXVK War3Hook[Shadow]: CUnitUI RecordSetStructureShadow install "
          "SKIPPED - nullptr");
    }
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

  if constexpr (dxvk::war3::internal::kNativeShadowRegisterImageHookEnabled) {
    if (addrs.shadowRegisterImageEntryAddr != nullptr) {
      const bool ok = InstallMinHook(
          addrs.shadowRegisterImageEntryAddr,
          reinterpret_cast<LPVOID>(&Hook_TerrainShadow_RegisterImageEntry),
          reinterpret_cast<LPVOID *>(&g_trampolineShadowRegisterImageEntry),
          "Shadow", "TerrainShadow_RegisterImageEntry", false, true);
      anyInstalled |= ok;
      char buf[120];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: RegisterImageEntry install addr=%p "
               "result=%s",
               addrs.shadowRegisterImageEntryAddr, ok ? "ok" : "fail");
      ::dxvk::Logger::info(buf);
    } else {
      ::dxvk::Logger::info(
          "DXVK War3Hook[Shadow]: RegisterImageEntry install SKIPPED - nullptr");
    }
  }

  // CDoodads type=0 静态贴花入口始终安装为安全 pass-through，允许同一 DLL
  // 使用 DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW=0/1 做运行时 A/B。
  // ToggleStaticStampFromObject 是树木/装饰物/可破坏物/path blocker 地面贴花
  // 阴影的对象级注册入口（RegisterImage type=0）。只有 gate=1 且 enable!=0
  // 才跳过写入；enable==0 始终透传，保证旧 stamp 能正常清理。
  // EmitterStamp(0x74DE40) 是 __userpurge，不安全 hook，本轮不拦（它是发光体/
  // 特效，不是静态阴影本体）。
  if constexpr (dxvk::war3::internal::kNativeShadowDoodadStampHookEnabled) {
    if (addrs.terrainShadowToggleStaticStampFromObjectAddr != nullptr) {
      const bool ok = InstallMinHook(
          addrs.terrainShadowToggleStaticStampFromObjectAddr,
          reinterpret_cast<LPVOID>(&Hook_Doodad_ToggleStaticStampFromObject),
          reinterpret_cast<LPVOID *>(&g_trampolineDoodadStaticStamp),
          "Shadow", "TerrainShadow_ToggleStaticStampFromObject", false, true);
      anyInstalled |= ok;
      char buf[120];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: ToggleStaticStampFromObject install "
               "addr=%p result=%s",
               addrs.terrainShadowToggleStaticStampFromObjectAddr,
               ok ? "ok" : "fail");
      ::dxvk::Logger::info(buf);
    }
  }

  // Phase 7.143 证伪归档：这两个 ListA render hook 会破坏悬崖/地形 tile。
  // 默认不开，仅保留安装代码作历史 A/B 诊断。用 Logger::info 打安装结果
  // 到 war3_d3d9.log（war3dbg::Print 走 DebugView，不进文件）。
  if constexpr (dxvk::war3::internal::kNativeShadowListARenderHookEnabled) {
    if (addrs.terrainShadowListARenderPreparedGroupsAddr != nullptr) {
      const bool ok = InstallMinHook(
          addrs.terrainShadowListARenderPreparedGroupsAddr,
          reinterpret_cast<LPVOID>(
              &Hook_TerrainShadow_ListA_RenderPreparedGroups),
          reinterpret_cast<LPVOID *>(&g_trampolineListARenderPreparedGroups),
          "Shadow", "TerrainShadow_ListA_RenderPreparedGroups", false, true);
      anyInstalled |= ok;
      char buf[120];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: ListA_RenderPreparedGroups install "
               "addr=%p result=%s",
               addrs.terrainShadowListARenderPreparedGroupsAddr,
               ok ? "ok" : "fail");
      ::dxvk::Logger::info(buf);
    }
    if (addrs.terrainShadowListARenderAllEntriesAddr != nullptr) {
      const bool ok = InstallMinHook(
          addrs.terrainShadowListARenderAllEntriesAddr,
          reinterpret_cast<LPVOID>(&Hook_TerrainShadow_ListA_RenderAllEntries),
          reinterpret_cast<LPVOID *>(&g_trampolineListARenderAllEntries),
          "Shadow", "TerrainShadow_ListA_RenderAllEntries", false, true);
      anyInstalled |= ok;
      char buf[120];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: ListA_RenderAllEntries install "
               "addr=%p result=%s",
               addrs.terrainShadowListARenderAllEntriesAddr,
               ok ? "ok" : "fail");
      ::dxvk::Logger::info(buf);
    }
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

  // Phase 7.116：TerrainShadow_DispatchToShape 旧实验 hook 安装。
  // 默认不安装；仅在 reject 或 caller diagnostics 灰度开关打开时接入。
  if constexpr (dxvk::war3::internal::
                    kNativeStaticShadowDispatchToShapeRejectEnabled ||
                dxvk::war3::internal::
                    kNativeStaticShadowDispatchToShapeCallerDiagnostics) {
    {
      char buf[160];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: DispatchToShape install attempt addr=%p",
               addrs.terrainShadowDispatchToShapeAddr);
      ::dxvk::Logger::info(buf);
    }
    if (addrs.terrainShadowDispatchToShapeAddr != nullptr) {
      const bool ok = InstallMinHook(
          addrs.terrainShadowDispatchToShapeAddr,
          reinterpret_cast<LPVOID>(&Hook_TerrainShadow_DispatchToShape),
          reinterpret_cast<LPVOID *>(&g_trampolineTerrainShadowDispatchToShape),
          "Shadow", "TerrainShadow_DispatchToShape", false, true);
      anyInstalled |= ok;
      char buf[100];
      snprintf(buf, sizeof(buf),
               "DXVK War3Hook[Shadow]: DispatchToShape install result=%s",
               ok ? "ok" : "fail");
      ::dxvk::Logger::info(buf);
    } else {
      ::dxvk::Logger::info(
          "DXVK War3Hook[Shadow]: DispatchToShape install SKIPPED - nullptr");
    }
  }
  return anyInstalled;
}

} // namespace dxvk::war3::hooks
