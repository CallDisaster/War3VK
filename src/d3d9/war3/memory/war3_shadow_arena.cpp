// war3_shadow_arena.cpp
#include "war3_shadow_arena.h"

#include "../../d3d9_device.h"
#include "../../d3d9_war3_debug.h"
#include "../war3.h"
#include "../../../util/util_env.h"

#include <atomic>
#include <cstdlib>
#include <limits>
#include <vector>

namespace dxvk::war3::memory {

namespace {

constexpr uint32_t kDefaultArenaPageSize = 64 * 1024 * 1024;
constexpr uint32_t kCaptureArenaPageSize = 64 * 1024 * 1024;
constexpr uint32_t kCaptureArenaMaxFrameSize = 384 * 1024 * 1024;
constexpr uint64_t kArenaMaxResidentSize = 1152ull * 1024ull * 1024ull;
constexpr uint32_t kDefaultArenaFrameCount = 3;
constexpr uint32_t kCaptureArenaFrameCount = 3;
constexpr uint32_t kInvalidGenerationIndex = std::numeric_limits<uint32_t>::max();

struct ShadowArenaPage {
  Rc<DxvkBuffer> storage;
  uint32_t capacity = 0u;
};

struct ShadowArenaFrameState {
  std::vector<ShadowArenaPage> pages;
  uint32_t currentPage = 0u;
  uint32_t currentOffset = 0u;
  uint32_t committedBytes = 0u;
  uint32_t totalCapacity = 0u;
  uint64_t retireSerial = 0u;
  uint64_t generation = 0u;
  uint64_t frameSerial = 0u;
};

std::vector<ShadowArenaFrameState> g_frameStates;
std::atomic<uint32_t> g_currentFrameIndex{kInvalidGenerationIndex};
uint32_t g_arenaPageSize = kDefaultArenaPageSize;
uint32_t g_arenaMaxFrameSize = kDefaultArenaPageSize;
uint32_t g_arenaFrameCount = kDefaultArenaFrameCount;
std::atomic<uint64_t> g_residentBytes{0u};
std::atomic<uint64_t> g_generationCounter{0u};
std::atomic<uint64_t> g_lastSubmittedSerial{0u};
std::atomic<uint64_t> g_lastCompletedSerial{0u};
std::atomic<uint64_t> g_busyReuseRejectCount{0u};
std::atomic<uint64_t> g_totalOverflowCount{0u};
std::atomic<uint64_t> g_currentUsedBytes{0u};
std::atomic<uint32_t> g_activeGenerationCount{0u};
std::atomic<uint32_t> g_frameIncomplete{0u};

// 帧内 overflow 计数：BeginFrame 时汇总打印。
std::atomic<uint32_t> g_frameOverflowCount{0};
std::atomic<uint32_t> g_lastOverflowCapacityBytes{0u};

uint32_t ParseArenaMegabytesEnv(const char* name) {
  const std::string overrideMb = dxvk::env::getEnvVar(name);
  if (!overrideMb.empty()) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(overrideMb.c_str(), &end, 10);
    if (end != overrideMb.c_str() && *end == '\0' && parsed > 0ul) {
      const uint64_t bytes = uint64_t(parsed) * 1024ull * 1024ull;
      if (bytes <= std::numeric_limits<uint32_t>::max())
        return static_cast<uint32_t>(bytes);
    }
  }

  return 0u;
}

uint32_t ResolveArenaPageSize() {
  // Fixed page granularity keeps residency/accounting deterministic. The old
  // page override could silently recreate the 384 MiB eager-page regression.
  return kCaptureArenaPageSize;
}

uint32_t ResolveArenaMaxFrameSize(uint32_t pageSize) {
  if (const uint32_t maxBytes =
          ParseArenaMegabytesEnv("DXVK_WAR3_SHADOW_ARENA_MAX_MB")) {
    return (std::min)((std::max)(maxBytes, pageSize),
                      kCaptureArenaMaxFrameSize);
  }

  // 兼容旧约定：原来的 DXVK_WAR3_SHADOW_ARENA_MB 是“单帧总容量”。
  if (const uint32_t legacyBytes =
          ParseArenaMegabytesEnv("DXVK_WAR3_SHADOW_ARENA_MB")) {
    return (std::min)((std::max)(legacyBytes, pageSize),
                      kCaptureArenaMaxFrameSize);
  }

  return (std::max)(kCaptureArenaMaxFrameSize, pageSize);
}

uint32_t ResolveArenaFrameCount() {
  const std::string overrideFrames =
      dxvk::env::getEnvVar("DXVK_WAR3_SHADOW_ARENA_FRAMES");
  if (!overrideFrames.empty()) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(overrideFrames.c_str(), &end, 10);
    if (end != overrideFrames.c_str() && *end == '\0' && parsed >= 1ul &&
        parsed <= std::numeric_limits<uint32_t>::max()) {
      return static_cast<uint32_t>(parsed);
    }
  }

  return dxvk::env::getEnvVar("DXVK_WAR3_SHADOW_ARENA_CAPTURE") != "0"
             ? kCaptureArenaFrameCount
             : kDefaultArenaFrameCount;
}

DxvkBufferCreateInfo MakeArenaBufferInfo(uint32_t size) {
  DxvkBufferCreateInfo info = {};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.stages =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  info.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                VK_ACCESS_INDEX_READ_BIT |
                VK_ACCESS_TRANSFER_WRITE_BIT;
  info.debugName = "War3ShadowArena";
  return info;
}

bool AllocateArenaPage(ShadowArenaFrameState& frameState,
                       uint32_t minCapacityBytes,
                       bool logGrowth) {
  auto* device = dxvk::war3::GetActiveDevice();
  if (!device)
    return false;

  if (frameState.totalCapacity >= g_arenaMaxFrameSize)
    return false;

  const uint32_t remainingCapacity = g_arenaMaxFrameSize - frameState.totalCapacity;
  const uint32_t requestedCapacity =
      (std::max)(g_arenaPageSize, minCapacityBytes);
  if (requestedCapacity > remainingCapacity && minCapacityBytes > remainingCapacity)
    return false;

  const uint32_t pageCapacity =
      requestedCapacity <= remainingCapacity ? requestedCapacity : remainingCapacity;

  const uint64_t residentBefore =
      g_residentBytes.load(std::memory_order_acquire);
  if (residentBefore + uint64_t(pageCapacity) > kArenaMaxResidentSize)
    return false;

  Rc<DxvkBuffer> pageBuffer = device->GetDXVKDevice()->createBuffer(
      MakeArenaBufferInfo(pageCapacity), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (pageBuffer == nullptr || !pageBuffer->storage()) {
    war3dbg::Print("DXVK War3[ShadowArena]: Arena 页创建失败 size=%u MB。\n",
                   pageCapacity >> 20);
    return false;
  }

  frameState.pages.push_back(ShadowArenaPage{std::move(pageBuffer), pageCapacity});
  frameState.totalCapacity += pageCapacity;
  g_residentBytes.fetch_add(pageCapacity, std::memory_order_release);

  if (logGrowth) {
    war3dbg::Print(
        "DXVK War3[ShadowArena]: Arena 扩容 +%uMB => %uMB/%uMB (pages=%u)\n",
        pageCapacity >> 20, frameState.totalCapacity >> 20,
        g_arenaMaxFrameSize >> 20, uint32_t(frameState.pages.size()));
  }

  return true;
}

} // namespace

bool ShadowArena_Init() {
  if (ShadowArena_IsInitialized())
    return true;

  auto* device = dxvk::war3::GetActiveDevice();
  if (!device) {
    war3dbg::Print(
        "DXVK War3[ShadowArena]: 活跃 D3D9 设备为空，初始化延期。\n");
    return false;
  }

  g_arenaPageSize = ResolveArenaPageSize();
  g_arenaMaxFrameSize = ResolveArenaMaxFrameSize(g_arenaPageSize);
  g_arenaFrameCount = ResolveArenaFrameCount();

  g_frameStates.clear();
  g_frameStates.resize(g_arenaFrameCount);
  g_residentBytes.store(0u, std::memory_order_release);

  // Three 64 MiB warm pages = 192 MiB. Extra pages grow on demand only.
  for (uint32_t i = 0; i < g_arenaFrameCount; i++) {
    if (!AllocateArenaPage(g_frameStates[i], g_arenaPageSize, false)) {
      war3dbg::Print(
          "DXVK War3[ShadowArena]: 初始化失败，无法为 frameSlot=%u 预热 %u MB。\n",
          i, g_arenaPageSize >> 20);
      g_frameStates.clear();
      g_residentBytes.store(0u, std::memory_order_release);
      return false;
    }
  }

  g_currentFrameIndex.store(kInvalidGenerationIndex,
                            std::memory_order_release);
  g_lastOverflowCapacityBytes.store(0u, std::memory_order_release);
  g_generationCounter.store(0u, std::memory_order_release);
  g_lastSubmittedSerial.store(0u, std::memory_order_release);
  g_lastCompletedSerial.store(0u, std::memory_order_release);
  g_busyReuseRejectCount.store(0u, std::memory_order_release);
  g_totalOverflowCount.store(0u, std::memory_order_release);
  g_currentUsedBytes.store(0u, std::memory_order_release);
  g_activeGenerationCount.store(0u, std::memory_order_release);
  g_frameIncomplete.store(0u, std::memory_order_release);

  war3dbg::Print(
      "DXVK War3[ShadowArena]: 初始化成功 (DEVICE_LOCAL) page=%uMB "
      "maxPerFrame=%uMB frames=%u warmupTotal=%uMB\n",
      g_arenaPageSize >> 20, g_arenaMaxFrameSize >> 20, g_arenaFrameCount,
      (g_arenaPageSize * g_arenaFrameCount) >> 20);
  return true;
}

bool ShadowArena_IsInitialized() {
  return !g_frameStates.empty();
}

bool ShadowArena_BeginFrame(uint64_t frameSerial, uint64_t completedSerial) {
  if (!ShadowArena_IsInitialized())
    return false;

  const uint32_t prevOverflow =
      g_frameOverflowCount.exchange(0u, std::memory_order_relaxed);
  if (prevOverflow > 0u) {
    war3dbg::Print(
        "[WARN] DXVK War3[Arena]: 上帧 Arena 溢出 %u 次，"
        "capacity=%u MB。对应 shadow caster 已 fail-closed。\n",
        prevOverflow,
        g_lastOverflowCapacityBytes.exchange(0u, std::memory_order_relaxed) >>
            20);
  }

  g_lastCompletedSerial.store(completedSerial, std::memory_order_release);
  g_frameIncomplete.store(0u, std::memory_order_release);
  g_currentUsedBytes.store(0u, std::memory_order_release);

  uint32_t generationIndex = kInvalidGenerationIndex;
  const uint32_t preferred =
      static_cast<uint32_t>(frameSerial % g_arenaFrameCount);
  if (preferred < g_frameStates.size() &&
      g_frameStates[preferred].retireSerial <= completedSerial) {
    generationIndex = preferred;
  } else {
    for (uint32_t i = 0u; i < g_frameStates.size(); ++i) {
      if (g_frameStates[i].retireSerial <= completedSerial) {
        generationIndex = i;
        break;
      }
    }
  }

  if (generationIndex == kInvalidGenerationIndex &&
      g_residentBytes.load(std::memory_order_acquire) + g_arenaPageSize <=
          kArenaMaxResidentSize) {
    ShadowArenaFrameState spill = {};
    if (AllocateArenaPage(spill, g_arenaPageSize, true)) {
      g_frameStates.push_back(std::move(spill));
      generationIndex = static_cast<uint32_t>(g_frameStates.size() - 1u);
    }
  }

  if (generationIndex == kInvalidGenerationIndex) {
    g_currentFrameIndex.store(kInvalidGenerationIndex,
                              std::memory_order_release);
    g_busyReuseRejectCount.fetch_add(1u, std::memory_order_relaxed);
    g_frameIncomplete.store(1u, std::memory_order_release);
    uint32_t activeCount = 0u;
    for (const auto& state : g_frameStates)
      activeCount += state.retireSerial > completedSerial ? 1u : 0u;
    g_activeGenerationCount.store(activeCount, std::memory_order_release);
    return false;
  }

  g_currentFrameIndex.store(generationIndex, std::memory_order_release);
  auto& frameState = g_frameStates[generationIndex];
  frameState.currentPage = 0u;
  frameState.currentOffset = 0u;
  frameState.committedBytes = 0u;
  frameState.frameSerial = frameSerial;
  frameState.retireSerial = 0u;
  frameState.generation =
      g_generationCounter.fetch_add(1u, std::memory_order_acq_rel) + 1u;

  uint32_t activeCount = 1u;
  for (uint32_t i = 0u; i < g_frameStates.size(); ++i) {
    if (i != generationIndex &&
        g_frameStates[i].retireSerial > completedSerial)
      ++activeCount;
  }
  g_activeGenerationCount.store(activeCount, std::memory_order_release);
  return true;
}

void ShadowArena_EndFrame(uint64_t frameSerial) {
  const uint32_t generationIndex =
      g_currentFrameIndex.load(std::memory_order_acquire);
  if (generationIndex >= g_frameStates.size())
    return;
  auto& frameState = g_frameStates[generationIndex];
  if (frameState.frameSerial != frameSerial)
    return;
  frameState.retireSerial = frameSerial;
  g_lastSubmittedSerial.store(frameSerial, std::memory_order_release);
}

ShadowArenaAllocation ShadowArena_Alloc(uint32_t size, uint32_t alignment) {
  ShadowArenaAllocation result = {};
  if (!ShadowArena_IsInitialized() || size == 0u)
    return result;

  if (alignment == 0u)
    alignment = 16u;

  const uint32_t generationIndex =
      g_currentFrameIndex.load(std::memory_order_acquire);
  if (generationIndex >= g_frameStates.size()) {
    g_busyReuseRejectCount.fetch_add(1u, std::memory_order_relaxed);
    g_frameIncomplete.store(1u, std::memory_order_release);
    return result;
  }
  auto& frameState = g_frameStates[generationIndex];

  while (true) {
    if (frameState.pages.empty()) {
      if (!AllocateArenaPage(frameState, size, false)) {
        g_frameOverflowCount.fetch_add(1u, std::memory_order_relaxed);
        g_totalOverflowCount.fetch_add(1u, std::memory_order_relaxed);
        g_frameIncomplete.store(1u, std::memory_order_release);
        g_lastOverflowCapacityBytes.store(frameState.totalCapacity,
                                          std::memory_order_relaxed);
        return result;
      }
    }

    if (frameState.currentPage >= frameState.pages.size()) {
      if (!AllocateArenaPage(frameState, size, true)) {
        g_frameOverflowCount.fetch_add(1u, std::memory_order_relaxed);
        g_totalOverflowCount.fetch_add(1u, std::memory_order_relaxed);
        g_frameIncomplete.store(1u, std::memory_order_release);
        g_lastOverflowCapacityBytes.store(frameState.totalCapacity,
                                          std::memory_order_relaxed);
        return result;
      }
    }

    auto& page = frameState.pages[frameState.currentPage];
    const uint32_t alignedStart =
        (frameState.currentOffset + alignment - 1u) & ~(alignment - 1u);
    const uint32_t newOffset = alignedStart + size;
    if (newOffset <= page.capacity) {
      frameState.currentOffset = newOffset;
      result.offset = alignedStart;
      result.size = size;
      result.storage = page.storage;
      result.info = page.storage->getSliceInfo(alignedStart, size);
      g_currentUsedBytes.store(
          uint64_t(frameState.committedBytes) + frameState.currentOffset,
          std::memory_order_release);
      return result;
    }

    frameState.committedBytes += page.capacity;
    frameState.currentPage += 1u;
    frameState.currentOffset = 0u;
  }
}

void ShadowArena_Reset() {
  if (!ShadowArena_IsInitialized())
    return;

  const uint32_t generationIndex =
      g_currentFrameIndex.load(std::memory_order_acquire);
  if (generationIndex >= g_frameStates.size())
    return;
  auto& frameState = g_frameStates[generationIndex];
  frameState.currentPage = 0u;
  frameState.currentOffset = 0u;
  frameState.committedBytes = 0u;
  g_currentUsedBytes.store(0u, std::memory_order_release);
}

uint32_t ShadowArena_UsedBytes() {
  if (!ShadowArena_IsInitialized())
    return 0u;

  const uint32_t generationIndex =
      g_currentFrameIndex.load(std::memory_order_acquire);
  if (generationIndex >= g_frameStates.size())
    return 0u;
  const auto& frameState = g_frameStates[generationIndex];
  return frameState.committedBytes + frameState.currentOffset;
}

uint32_t ShadowArena_CapacityBytes() {
  if (!ShadowArena_IsInitialized())
    return 0u;

  const uint32_t generationIndex =
      g_currentFrameIndex.load(std::memory_order_acquire);
  if (generationIndex >= g_frameStates.size())
    return 0u;
  const auto& frameState = g_frameStates[generationIndex];
  return frameState.totalCapacity;
}

uint64_t ShadowArena_ResidentBytes() {
  return g_residentBytes.load(std::memory_order_acquire);
}

ShadowArenaDiagnostics ShadowArena_QueryDiagnostics() {
  ShadowArenaDiagnostics diagnostics = {};
  diagnostics.usedBytes =
      g_currentUsedBytes.load(std::memory_order_acquire);
  diagnostics.residentBytes =
      g_residentBytes.load(std::memory_order_acquire);
  diagnostics.perGenerationCapacityBytes = g_arenaMaxFrameSize;
  diagnostics.residentLimitBytes = kArenaMaxResidentSize;
  diagnostics.generation =
      g_generationCounter.load(std::memory_order_acquire);
  diagnostics.submittedSerial =
      g_lastSubmittedSerial.load(std::memory_order_acquire);
  diagnostics.completedSerial =
      g_lastCompletedSerial.load(std::memory_order_acquire);
  diagnostics.busyReuseRejectCount =
      g_busyReuseRejectCount.load(std::memory_order_acquire);
  diagnostics.overflowCount =
      g_totalOverflowCount.load(std::memory_order_acquire);
  diagnostics.activeGenerationCount =
      g_activeGenerationCount.load(std::memory_order_acquire);
  diagnostics.frameIncomplete =
      g_frameIncomplete.load(std::memory_order_acquire);
  return diagnostics;
}

} // namespace dxvk::war3::memory
