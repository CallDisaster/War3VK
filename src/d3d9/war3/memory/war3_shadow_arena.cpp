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

constexpr uint32_t kDefaultArenaPageSize = 8 * 1024 * 1024;
constexpr uint32_t kCaptureArenaPageSize = 384 * 1024 * 1024;
constexpr uint32_t kCaptureArenaMaxFrameSize = 1024 * 1024 * 1024;
constexpr uint32_t kDefaultArenaFrameCount = 3;
constexpr uint32_t kCaptureArenaFrameCount = 3;

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
};

std::vector<ShadowArenaFrameState> g_frameStates;
std::atomic<uint32_t> g_currentFrameIndex{0};
uint32_t g_arenaPageSize = kDefaultArenaPageSize;
uint32_t g_arenaMaxFrameSize = kDefaultArenaPageSize;
uint32_t g_arenaFrameCount = kDefaultArenaFrameCount;

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
  if (const uint32_t pageBytes =
          ParseArenaMegabytesEnv("DXVK_WAR3_SHADOW_ARENA_PAGE_MB")) {
    return pageBytes;
  }

  return dxvk::env::getEnvVar("DXVK_WAR3_SHADOW_ARENA_CAPTURE") != "0"
             ? kCaptureArenaPageSize
             : kDefaultArenaPageSize;
}

uint32_t ResolveArenaMaxFrameSize(uint32_t pageSize) {
  if (const uint32_t maxBytes =
          ParseArenaMegabytesEnv("DXVK_WAR3_SHADOW_ARENA_MAX_MB")) {
    return (std::max)(maxBytes, pageSize);
  }

  // 兼容旧约定：原来的 DXVK_WAR3_SHADOW_ARENA_MB 是“单帧总容量”。
  if (const uint32_t legacyBytes =
          ParseArenaMegabytesEnv("DXVK_WAR3_SHADOW_ARENA_MB")) {
    return (std::max)(legacyBytes, pageSize);
  }

  return dxvk::env::getEnvVar("DXVK_WAR3_SHADOW_ARENA_CAPTURE") != "0"
             ? (std::max)(kCaptureArenaMaxFrameSize, pageSize)
             : pageSize;
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

  Rc<DxvkBuffer> pageBuffer = device->GetDXVKDevice()->createBuffer(
      MakeArenaBufferInfo(pageCapacity), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (pageBuffer == nullptr) {
    war3dbg::Print("DXVK War3[ShadowArena]: Arena 页创建失败 size=%u MB。\n",
                   pageCapacity >> 20);
    return false;
  }

  frameState.pages.push_back(ShadowArenaPage{std::move(pageBuffer), pageCapacity});
  frameState.totalCapacity += pageCapacity;

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

  // 每个 frame slot 先预热 1 页，保持旧版“默认 384MB x 3”基线；
  // 额外页只在高压场景下按需增长。
  for (uint32_t i = 0; i < g_arenaFrameCount; i++) {
    if (!AllocateArenaPage(g_frameStates[i], g_arenaPageSize, false)) {
      war3dbg::Print(
          "DXVK War3[ShadowArena]: 初始化失败，无法为 frameSlot=%u 预热 %u MB。\n",
          i, g_arenaPageSize >> 20);
      g_frameStates.clear();
      return false;
    }
  }

  g_currentFrameIndex.store(0u, std::memory_order_release);
  g_lastOverflowCapacityBytes.store(0u, std::memory_order_release);

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

void ShadowArena_BeginFrame(uint32_t frameIndex) {
  if (!ShadowArena_IsInitialized())
    return;

  const uint32_t prevOverflow =
      g_frameOverflowCount.exchange(0u, std::memory_order_relaxed);
  if (prevOverflow > 0u) {
    war3dbg::Print(
        "[WARN] DXVK War3[Arena]: 上帧 Arena 溢出 %u 次，"
        "capacity=%u MB。部分 shadow caster 已回退 LegacyFreeze。\n",
        prevOverflow,
        g_lastOverflowCapacityBytes.exchange(0u, std::memory_order_relaxed) >>
            20);
  }

  const uint32_t frameSlot = frameIndex % g_arenaFrameCount;
  g_currentFrameIndex.store(frameSlot, std::memory_order_release);

  auto& frameState = g_frameStates[frameSlot];
  frameState.currentPage = 0u;
  frameState.currentOffset = 0u;
  frameState.committedBytes = 0u;
}

ShadowArenaAllocation ShadowArena_Alloc(uint32_t size, uint32_t alignment) {
  ShadowArenaAllocation result = {};
  if (!ShadowArena_IsInitialized() || size == 0u)
    return result;

  if (alignment == 0u)
    alignment = 16u;

  auto& frameState =
      g_frameStates[g_currentFrameIndex.load(std::memory_order_acquire)];

  while (true) {
    if (frameState.pages.empty()) {
      if (!AllocateArenaPage(frameState, size, false)) {
        g_frameOverflowCount.fetch_add(1u, std::memory_order_relaxed);
        g_lastOverflowCapacityBytes.store(frameState.totalCapacity,
                                          std::memory_order_relaxed);
        return result;
      }
    }

    if (frameState.currentPage >= frameState.pages.size()) {
      if (!AllocateArenaPage(frameState, size, true)) {
        g_frameOverflowCount.fetch_add(1u, std::memory_order_relaxed);
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

  auto& frameState =
      g_frameStates[g_currentFrameIndex.load(std::memory_order_acquire)];
  frameState.currentPage = 0u;
  frameState.currentOffset = 0u;
  frameState.committedBytes = 0u;
}

uint32_t ShadowArena_UsedBytes() {
  if (!ShadowArena_IsInitialized())
    return 0u;

  const auto& frameState =
      g_frameStates[g_currentFrameIndex.load(std::memory_order_acquire)];
  return frameState.committedBytes + frameState.currentOffset;
}

uint32_t ShadowArena_CapacityBytes() {
  if (!ShadowArena_IsInitialized())
    return 0u;

  const auto& frameState =
      g_frameStates[g_currentFrameIndex.load(std::memory_order_acquire)];
  return frameState.totalCapacity;
}

} // namespace dxvk::war3::memory
