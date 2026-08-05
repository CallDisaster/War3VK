#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace dxvk::war3::memory {

enum class StormNativeSmallRepairMode : uint8_t {
  Off,
  Search,
  Coalesce,
};

#pragma pack(push, 4)
struct StormNativeFreeBlock {
  uint16_t totalBytes;
  uint8_t alignmentPadding;
  uint8_t flags;
  StormNativeFreeBlock *next;
};

struct StormNativeArena {
  StormNativeArena *next;
  uint32_t heapId;
  uint32_t bucketIndex;
  uint32_t blockSignature;
  uint32_t currentArena;
  uint32_t liveAllocationCount;
  uint32_t requestedLiveBytes;
  uint8_t *dataStart;
  uint8_t *bumpEnd;
  uint32_t adjacentFreeHint;
  uint32_t commitGranularity;
  uint32_t committedBytes;
  uint32_t reservedBytes;
  uint32_t externalRequestedBytes;
  uint32_t allocationCalls;
  uint32_t freeCalls;
  uint32_t reserved64;
  StormNativeFreeBlock *freeBins[9];
  int32_t sourceLine;
  char sourceName[1];
};
#pragma pack(pop)

#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(sizeof(StormNativeFreeBlock) == 8,
              "Storm 小块自由块 ABI 已变化");
static_assert(offsetof(StormNativeArena, dataStart) == 28,
              "Storm arena dataStart 偏移已变化");
static_assert(offsetof(StormNativeArena, freeBins) == 68,
              "Storm arena freeBins 偏移已变化");
static_assert(offsetof(StormNativeArena, sourceName) == 108,
              "Storm arena sourceName 偏移已变化");
#endif

enum class StormNativeSmallRepairResult : uint8_t {
  FitAlreadyAvailable,
  PromotedHigherBin,
  NoFit,
  InvalidArena,
};

struct StormNativeSmallRepairStats {
  uint32_t calls = 0;
  uint32_t promotions = 0;
  uint32_t rebuilds = 0;
  uint32_t invalidArenaSkips = 0;
  uint32_t bypasses = 0;
};

/**
 * @brief 供 Storm Hook 批量安装使用的单个 MinHook 描述。
 *
 * `originalStorage` 指向本模块保存 trampoline 的槽位；只有整个 Storm Hook
 * 批次成功后，这个内部 Hook 才会启用。
 */
struct StormNativeSmallRepairHookDesc {
  LPVOID target = nullptr;
  LPVOID detour = nullptr;
  LPVOID *originalStorage = nullptr;
};

/**
 * @brief 解析默认 `search` 策略并校验 Storm 1.27a 内部函数。
 *
 * 环境变量 `DXVK_WAR3_STORM_NATIVE_SMALL_REPAIR` 可显式设置
 * `off|search|coalesce`；兼容读取独立版的
 * `STORMBREAKER_NATIVE_SMALL_REPAIR`。`coalesce` 只用于诊断。
 */
bool StormNativeSmallRepair_Prepare(
    HMODULE stormModule, StormNativeSmallRepairHookDesc *hookDesc);

void StormNativeSmallRepair_Reset();
/** @brief 保留 trampoline，但把可能仍活跃的 detour 切成纯透传。 */
void StormNativeSmallRepair_ForcePassThrough();
StormNativeSmallRepairMode StormNativeSmallRepair_GetMode();
const char *StormNativeSmallRepair_GetModeName();
StormNativeSmallRepairStats StormNativeSmallRepair_GetStats();

/** @brief 纯 free-bin 修复算法，供 Hook 与确定性离线测试共用。 */
StormNativeSmallRepairResult StormNativeSmallRepair_RepairFreeLists(
    StormNativeArena *arena, uint32_t requiredTotalBytes) noexcept;

} // namespace dxvk::war3::memory
