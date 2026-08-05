#include "war3_storm_native_small_repair.h"

#include "../../d3d9_war3_debug.h"

#include <atomic>
#include <cstring>

namespace dxvk::war3::memory {
namespace {

constexpr uint32_t kAllocPageRva = 0x2A510u;
constexpr uint32_t kRebuildFreeListRva = 0x2A920u;
constexpr uint8_t kFreeFlag = 0x02u;
constexpr uint32_t kSmallRequestLimit = 0xFE7Bu;
constexpr uint32_t kMaximumArenaReserve = 0x10000000u;

using AllocPageFn = void *(__fastcall *)(StormNativeArena *, uint32_t,
                                         uint32_t);
using RebuildFreeListFn = void(__fastcall *)(StormNativeArena *);

std::atomic<StormNativeSmallRepairMode> g_mode{
    StormNativeSmallRepairMode::Off};
AllocPageFn g_originalAllocPage = nullptr;
RebuildFreeListFn g_rebuildFreeList = nullptr;
volatile uint32_t *g_debugMemoryEnabled = nullptr;
volatile uint32_t *g_protectMemoryEnabled = nullptr;
volatile LONG g_calls = 0;
volatile LONG g_promotions = 0;
volatile LONG g_rebuilds = 0;
volatile LONG g_invalidArenaSkips = 0;
volatile LONG g_bypasses = 0;

struct Candidate {
  StormNativeFreeBlock *block = nullptr;
  StormNativeFreeBlock **link = nullptr;
  bool valid = true;
};

bool ReadNamedMode(const char *name, StormNativeSmallRepairMode *mode,
                   bool *found) {
  char value[32] = {};
  SetLastError(ERROR_SUCCESS);
  const DWORD length =
      GetEnvironmentVariableA(name, value, static_cast<DWORD>(sizeof(value)));
  if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
    *found = false;
    return true;
  }
  *found = true;
  if (length == 0 || length >= sizeof(value))
    return false;
  if (_stricmp(value, "off") == 0)
    *mode = StormNativeSmallRepairMode::Off;
  else if (_stricmp(value, "search") == 0)
    *mode = StormNativeSmallRepairMode::Search;
  else if (_stricmp(value, "coalesce") == 0)
    *mode = StormNativeSmallRepairMode::Coalesce;
  else
    return false;
  return true;
}

bool ReadMode(StormNativeSmallRepairMode *mode) {
  bool found = false;
  if (!ReadNamedMode("DXVK_WAR3_STORM_NATIVE_SMALL_REPAIR", mode, &found))
    return false;
  if (found)
    return true;
  if (!ReadNamedMode("STORMBREAKER_NATIVE_SMALL_REPAIR", mode, &found))
    return false;
  if (found)
    return true;
  *mode = StormNativeSmallRepairMode::Search;
  return true;
}

bool IsArenaLayoutPlausible(const StormNativeArena *arena) noexcept {
  if (!arena || !arena->dataStart || !arena->bumpEnd ||
      arena->reservedBytes < 0x1000u ||
      arena->reservedBytes > kMaximumArenaReserve ||
      arena->committedBytes > arena->reservedBytes)
    return false;

  const uint64_t base = reinterpret_cast<uintptr_t>(arena);
  const uint64_t data = reinterpret_cast<uintptr_t>(arena->dataStart);
  const uint64_t bump = reinterpret_cast<uintptr_t>(arena->bumpEnd);
  const uint64_t end = base + arena->reservedBytes;
  return data >= base + 108u && data <= bump && bump <= end &&
         (data & 7u) == 0 && (bump & 7u) == 0;
}

bool IsFreeBlockPlausible(const StormNativeArena *arena,
                          const StormNativeFreeBlock *block) noexcept {
  if (!block)
    return false;
  const uintptr_t address = reinterpret_cast<uintptr_t>(block);
  const uintptr_t data = reinterpret_cast<uintptr_t>(arena->dataStart);
  const uintptr_t bump = reinterpret_cast<uintptr_t>(arena->bumpEnd);
  const uint32_t total = block->totalBytes;
  return address >= data && address + sizeof(StormNativeFreeBlock) <= bump &&
         (address & 7u) == 0 && total >= sizeof(StormNativeFreeBlock) &&
         (total & 7u) == 0 && address + total <= bump &&
         (block->flags & kFreeFlag) != 0;
}

Candidate FindCandidate(StormNativeArena *arena, uint32_t bin,
                        uint32_t required) noexcept {
  Candidate result = {};
  StormNativeFreeBlock **link = &arena->freeBins[bin];
  uint32_t bestRemainder = 0x7FFFFFFFu;
  uint32_t tolerance = 16u;
  const uintptr_t data = reinterpret_cast<uintptr_t>(arena->dataStart);
  const uintptr_t bump = reinterpret_cast<uintptr_t>(arena->bumpEnd);
  const size_t maximumNodes = (bump - data) / 8u + 1u;

  for (size_t visited = 0; *link; ++visited) {
    if (visited >= maximumNodes || !IsFreeBlockPlausible(arena, *link)) {
      result.valid = false;
      result.block = nullptr;
      result.link = nullptr;
      return result;
    }
    StormNativeFreeBlock *block = *link;
    const uint32_t rawBlockBin = block->totalBytes >> 5;
    const uint32_t blockBin = rawBlockBin < 8u ? rawBlockBin : 8u;
    if (blockBin != bin) {
      result.valid = false;
      result.block = nullptr;
      result.link = nullptr;
      return result;
    }
    if (block->totalBytes >= required) {
      const uint32_t remainder = block->totalBytes - required;
      if (remainder < bestRemainder) {
        result.block = block;
        result.link = link;
        bestRemainder = remainder;
        if (remainder < tolerance)
          break;
        tolerance += 4u;
      }
    }
    link = &block->next;
  }
  return result;
}

uint32_t RequiredTotalBytes(uint32_t requested, bool debug) noexcept {
  const uint32_t unaligned = requested + 8u + (debug ? 2u : 0u);
  return (unaligned + 7u) & ~7u;
}

void *__fastcall HookedAllocPage(StormNativeArena *arena, uint32_t requested,
                                 uint32_t headerFlags) noexcept {
  InterlockedIncrement(&g_calls);
  if (!g_originalAllocPage)
    return nullptr;
  const StormNativeSmallRepairMode mode =
      g_mode.load(std::memory_order_acquire);
  if (mode == StormNativeSmallRepairMode::Off) {
    InterlockedIncrement(&g_bypasses);
    return g_originalAllocPage(arena, requested, headerFlags);
  }

  if (!arena || requested > kSmallRequestLimit || !g_protectMemoryEnabled ||
      *g_protectMemoryEnabled != 0) {
    InterlockedIncrement(&g_bypasses);
    return g_originalAllocPage(arena, requested, headerFlags);
  }

  const bool debug = g_debugMemoryEnabled && *g_debugMemoryEnabled != 0;
  const uint32_t required = RequiredTotalBytes(requested, debug);
  if (!IsArenaLayoutPlausible(arena)) {
    InterlockedIncrement(&g_invalidArenaSkips);
    return g_originalAllocPage(arena, requested, headerFlags);
  }

  const uint32_t rawTarget = required >> 5;
  const uint32_t target = rawTarget < 8u ? rawTarget : 8u;
  StormNativeFreeBlock *const head = arena->freeBins[target];
  if (!head) {
    // 目标桶为空时，原生 Storm 本来就会继续查高位桶并按需重建。
    InterlockedIncrement(&g_bypasses);
    return g_originalAllocPage(arena, requested, headerFlags);
  }
  if (!IsFreeBlockPlausible(arena, head)) {
    InterlockedIncrement(&g_invalidArenaSkips);
    return g_originalAllocPage(arena, requested, headerFlags);
  }
  const uint32_t rawHeadBin = head->totalBytes >> 5;
  const uint32_t headBin = rawHeadBin < 8u ? rawHeadBin : 8u;
  if (headBin != target) {
    InterlockedIncrement(&g_invalidArenaSkips);
    return g_originalAllocPage(arena, requested, headerFlags);
  }
  if (head->totalBytes >= required ||
      (target == 8u && mode == StormNativeSmallRepairMode::Search)) {
    InterlockedIncrement(&g_bypasses);
    return g_originalAllocPage(arena, requested, headerFlags);
  }

  auto result = StormNativeSmallRepair_RepairFreeLists(arena, required);
  if (result == StormNativeSmallRepairResult::PromotedHigherBin) {
    InterlockedIncrement(&g_promotions);
  } else if (result == StormNativeSmallRepairResult::InvalidArena) {
    InterlockedIncrement(&g_invalidArenaSkips);
  } else if (result == StormNativeSmallRepairResult::NoFit &&
             mode == StormNativeSmallRepairMode::Coalesce &&
             arena->adjacentFreeHint >= 4u && g_rebuildFreeList) {
    g_rebuildFreeList(arena);
    InterlockedIncrement(&g_rebuilds);
    result = StormNativeSmallRepair_RepairFreeLists(arena, required);
    if (result == StormNativeSmallRepairResult::PromotedHigherBin)
      InterlockedIncrement(&g_promotions);
    else if (result == StormNativeSmallRepairResult::InvalidArena)
      InterlockedIncrement(&g_invalidArenaSkips);
  }
  return g_originalAllocPage(arena, requested, headerFlags);
}

} // namespace

StormNativeSmallRepairResult StormNativeSmallRepair_RepairFreeLists(
    StormNativeArena *arena, uint32_t requiredTotalBytes) noexcept {
  if (!IsArenaLayoutPlausible(arena) || requiredTotalBytes < 8u ||
      requiredTotalBytes > 0xFFFFu || (requiredTotalBytes & 7u) != 0)
    return StormNativeSmallRepairResult::InvalidArena;

  const uint32_t rawTarget = requiredTotalBytes >> 5;
  const uint32_t target = rawTarget < 8u ? rawTarget : 8u;
  Candidate targetCandidate =
      FindCandidate(arena, target, requiredTotalBytes);
  if (!targetCandidate.valid)
    return StormNativeSmallRepairResult::InvalidArena;
  if (targetCandidate.block)
    return StormNativeSmallRepairResult::FitAlreadyAvailable;

  // 只有“目标桶非空但没有适配块”才是原生算法缺口。
  if (!arena->freeBins[target]) {
    for (uint32_t bin = target + 1u; bin < 9u; ++bin) {
      if (!arena->freeBins[bin])
        continue;
      Candidate higher = FindCandidate(arena, bin, requiredTotalBytes);
      if (!higher.valid)
        return StormNativeSmallRepairResult::InvalidArena;
      return higher.block ? StormNativeSmallRepairResult::FitAlreadyAvailable
                          : StormNativeSmallRepairResult::NoFit;
    }
    return StormNativeSmallRepairResult::NoFit;
  }

  for (uint32_t bin = target + 1u; bin < 9u; ++bin) {
    if (!arena->freeBins[bin])
      continue;
    Candidate higher = FindCandidate(arena, bin, requiredTotalBytes);
    if (!higher.valid)
      return StormNativeSmallRepairResult::InvalidArena;
    if (!higher.block)
      continue;
    *higher.link = higher.block->next;
    higher.block->next = arena->freeBins[target];
    arena->freeBins[target] = higher.block;
    return StormNativeSmallRepairResult::PromotedHigherBin;
  }
  return StormNativeSmallRepairResult::NoFit;
}

bool StormNativeSmallRepair_Prepare(
    HMODULE stormModule, StormNativeSmallRepairHookDesc *hookDesc) {
  if (!stormModule || !hookDesc)
    return false;
  *hookDesc = {};
  StormNativeSmallRepair_Reset();

  StormNativeSmallRepairMode requested = StormNativeSmallRepairMode::Search;
  if (!ReadMode(&requested)) {
    war3dbg::Print(
        "DXVK War3[StormHook]: 原生小块修复配置无效，应为 off|search|coalesce\n");
    return false;
  }
  g_mode.store(requested, std::memory_order_release);
  if (requested == StormNativeSmallRepairMode::Off)
    return true;

  const uintptr_t base = reinterpret_cast<uintptr_t>(stormModule);
  auto *allocPage = reinterpret_cast<uint8_t *>(base + kAllocPageRva);
  auto *rebuild = reinterpret_cast<uint8_t *>(base + kRebuildFreeListRva);
  constexpr uint8_t kAllocPageProlog[] = {0x55, 0x8B, 0xEC, 0x83,
                                          0xEC, 0x48, 0x83, 0x3D};
  constexpr uint8_t kRebuildProlog[] = {0x55, 0x8B, 0xEC, 0x83,
                                        0xEC, 0x2C, 0xA1};
  if (std::memcmp(allocPage, kAllocPageProlog, sizeof(kAllocPageProlog)) != 0 ||
      std::memcmp(rebuild, kRebuildProlog, sizeof(kRebuildProlog)) != 0) {
    war3dbg::Print(
        "DXVK War3[StormHook]: Storm 小块内部函数前导字节不匹配，拒绝安装\n");
    StormNativeSmallRepair_Reset();
    return false;
  }

  g_originalAllocPage = reinterpret_cast<AllocPageFn>(allocPage);
  g_rebuildFreeList = reinterpret_cast<RebuildFreeListFn>(rebuild);
  g_debugMemoryEnabled = reinterpret_cast<volatile uint32_t *>(base + 0x5536Cu);
  g_protectMemoryEnabled =
      reinterpret_cast<volatile uint32_t *>(base + 0x56F74u);

  hookDesc->target = allocPage;
  hookDesc->detour = reinterpret_cast<LPVOID>(HookedAllocPage);
  hookDesc->originalStorage =
      reinterpret_cast<LPVOID *>(&g_originalAllocPage);
  return true;
}

void StormNativeSmallRepair_Reset() {
  g_mode.store(StormNativeSmallRepairMode::Off, std::memory_order_release);
  g_originalAllocPage = nullptr;
  g_rebuildFreeList = nullptr;
  g_debugMemoryEnabled = nullptr;
  g_protectMemoryEnabled = nullptr;
  InterlockedExchange(&g_calls, 0);
  InterlockedExchange(&g_promotions, 0);
  InterlockedExchange(&g_rebuilds, 0);
  InterlockedExchange(&g_invalidArenaSkips, 0);
  InterlockedExchange(&g_bypasses, 0);
}

void StormNativeSmallRepair_ForcePassThrough() {
  g_mode.store(StormNativeSmallRepairMode::Off, std::memory_order_release);
}

StormNativeSmallRepairMode StormNativeSmallRepair_GetMode() {
  return g_mode.load(std::memory_order_acquire);
}

const char *StormNativeSmallRepair_GetModeName() {
  switch (StormNativeSmallRepair_GetMode()) {
  case StormNativeSmallRepairMode::Search:
    return "search";
  case StormNativeSmallRepairMode::Coalesce:
    return "coalesce";
  default:
    return "off";
  }
}

StormNativeSmallRepairStats StormNativeSmallRepair_GetStats() {
  return {static_cast<uint32_t>(InterlockedCompareExchange(&g_calls, 0, 0)),
          static_cast<uint32_t>(
              InterlockedCompareExchange(&g_promotions, 0, 0)),
          static_cast<uint32_t>(
              InterlockedCompareExchange(&g_rebuilds, 0, 0)),
          static_cast<uint32_t>(
              InterlockedCompareExchange(&g_invalidArenaSkips, 0, 0)),
          static_cast<uint32_t>(
              InterlockedCompareExchange(&g_bypasses, 0, 0))};
}

} // namespace dxvk::war3::memory
