// war3_tlsf_pool.cpp
// TLSF 内存池实现（从 StormBreaker 整合）
//
// 改动点（相较原 MemoryPool.cpp）：
//   1. 去除独立 Logger，改用 dxvk war3dbg::Print
//   2. 添加 TlsfPool_AllocArenaBlock（Shadow Arena 专用预留接口）
//   3. 使用独占 Spinlock 保护 TLSF 元数据，避免 shared_mutex 的错误并发
//   4. 池范围表通过原子快照发布，IsFromPool 零锁读取
//   4. 整合进 dxvk::war3::memory 命名空间

#include "war3_tlsf_pool.h"
#include "../../d3d9_war3_debug.h" // war3dbg::Print
#include "../../../util/sync/sync_spinlock.h"
#include "tlsf.h"


#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include <windows.h>


namespace dxvk::war3::memory {

// ============================================================================
// 内部常量
// ============================================================================

constexpr size_t kInitialPoolSize = 64 * 1024 * 1024;   // 64 MB
constexpr size_t kMaxPoolSize = 1024 * 1024 * 1024;     // 1 GB
constexpr size_t kExtendGranularity = 16 * 1024 * 1024; // 16 MB
constexpr size_t kDefaultAlignment = 16;
constexpr size_t kMaxExtraPoolCount =
    (kMaxPoolSize - kInitialPoolSize) / kExtendGranularity;

// ============================================================================
// 内部状态
// ============================================================================

namespace {

struct ExtraPool {
  void *base = nullptr;
  size_t size = 0;
  pool_t handle = nullptr;
};

struct ArenaBlock {
  void *base = nullptr;
  size_t size = 0;
};

struct PoolRange {
  void *base = nullptr;
  size_t size = 0;
};

struct PoolRangeSnapshot {
  uint32_t count = 0;
  std::array<PoolRange, kMaxExtraPoolCount + 1> ranges = {};
};

tlsf_t g_tlsf = nullptr;
void *g_mainPool = nullptr;
std::vector<ExtraPool> g_extraPools;
std::vector<ArenaBlock> g_arenaBlocks;
std::vector<std::unique_ptr<PoolRangeSnapshot>> g_rangeSnapshots;
std::atomic<PoolRangeSnapshot *> g_activeRanges{nullptr};

// 线程安全开关：默认开启，但单线程场景可禁用以获得更低开销
std::atomic<bool> g_lockEnabled{true};
sync::Spinlock g_poolLock;

enum InitState : uint32_t {
  kInitUninitialized = 0u,
  kInitInitializing = 1u,
  kInitReady = 2u,
};

std::atomic<uint32_t> g_initState{kInitUninitialized};

// 统计
std::atomic<size_t> g_totalSize{0};
std::atomic<size_t> g_usedSize{0};
std::atomic<size_t> g_peakUsed{0};
std::atomic<size_t> g_allocCount{0};
std::atomic<size_t> g_freeCount{0};
std::atomic<size_t> g_extendCount{0};
std::atomic<size_t> g_trimCount{0};

// ---- 辅助 ----

inline bool InRange(void *ptr, void *base, size_t size) {
  auto p = reinterpret_cast<uintptr_t>(ptr);
  auto b = reinterpret_cast<uintptr_t>(base);
  return p >= b && p < b + size;
}

void UpdatePeak(size_t cur) {
  size_t peak = g_peakUsed.load(std::memory_order_relaxed);
  while (cur > peak && !g_peakUsed.compare_exchange_weak(
                           peak, cur, std::memory_order_relaxed)) {
  }
}

void PublishRangeSnapshotLocked();

bool AddExtraPool(size_t size) {
  void *mem =
      VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!mem) {
    war3dbg::Print("DXVK War3[TlsfPool]: VirtualAlloc 失败 size=%zu err=%lu\n",
                   size, GetLastError());
    return false;
  }
  pool_t ph = tlsf_add_pool(g_tlsf, mem, size);
  if (!ph) {
    war3dbg::Print("DXVK War3[TlsfPool]: tlsf_add_pool 失败\n");
    VirtualFree(mem, 0, MEM_RELEASE);
    return false;
  }
  g_extraPools.push_back(ExtraPool{mem, size, ph});
  PublishRangeSnapshotLocked();
  g_totalSize.fetch_add(size, std::memory_order_relaxed);
  g_extendCount.fetch_add(1, std::memory_order_relaxed);
  war3dbg::Print("DXVK War3[TlsfPool]: 扩展池 +%zu MB，当前总计 %zu MB\n",
                 size >> 20, g_totalSize.load() >> 20);
  return true;
}

void PublishRangeSnapshotLocked() {
  auto snapshot = std::make_unique<PoolRangeSnapshot>();
  if (g_mainPool) {
    snapshot->ranges[snapshot->count++] = PoolRange{g_mainPool,
                                                    kInitialPoolSize};
  }

  for (const auto &ep : g_extraPools) {
    if (!ep.base || !ep.size)
      continue;
    if (snapshot->count >= snapshot->ranges.size())
      break;
    snapshot->ranges[snapshot->count++] = PoolRange{ep.base, ep.size};
  }

  PoolRangeSnapshot *published = snapshot.get();
  g_rangeSnapshots.push_back(std::move(snapshot));
  g_activeRanges.store(published, std::memory_order_release);
}

// 带自动扩展的分配（持锁版本，供 Alloc/AllocAligned 调用）
void *AllocWithExtendLocked(size_t size, size_t align) {
  // 先尝试不扩展
  void *ptr =
      align ? tlsf_memalign(g_tlsf, align, size) : tlsf_malloc(g_tlsf, size);
  if (ptr)
    return ptr;

  // 扩展后重试
  if (g_totalSize.load(std::memory_order_relaxed) >= kMaxPoolSize)
    return nullptr;

  size_t ext = std::max(kExtendGranularity,
                        ((size + kExtendGranularity - 1) / kExtendGranularity) *
                            kExtendGranularity);
  if (!AddExtraPool(ext))
    return nullptr;

  return align ? tlsf_memalign(g_tlsf, align, size) : tlsf_malloc(g_tlsf, size);
}

} // anonymous namespace

// ============================================================================
// 公共接口实现
// ============================================================================

bool TlsfPool_Init() {
  uint32_t expected = kInitUninitialized;
  if (!g_initState.compare_exchange_strong(expected, kInitInitializing,
                                           std::memory_order_acq_rel)) {
    while (expected == kInitInitializing) {
      SwitchToThread();
      expected = g_initState.load(std::memory_order_acquire);
    }
    return expected == kInitReady;
  }

  g_mainPool = VirtualAlloc(nullptr, kInitialPoolSize, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
  if (!g_mainPool) {
    war3dbg::Print("DXVK War3[TlsfPool]: 主池 VirtualAlloc 失败 err=%lu\n",
                   GetLastError());
    g_initState.store(kInitUninitialized, std::memory_order_release);
    return false;
  }

  g_tlsf = tlsf_create_with_pool(g_mainPool, kInitialPoolSize);
  if (!g_tlsf) {
    war3dbg::Print("DXVK War3[TlsfPool]: tlsf_create_with_pool 失败\n");
    VirtualFree(g_mainPool, 0, MEM_RELEASE);
    g_mainPool = nullptr;
    g_initState.store(kInitUninitialized, std::memory_order_release);
    return false;
  }

  g_totalSize.store(kInitialPoolSize, std::memory_order_relaxed);
  g_usedSize.store(0u, std::memory_order_relaxed);
  g_peakUsed.store(0u, std::memory_order_relaxed);
  g_allocCount.store(0u, std::memory_order_relaxed);
  g_freeCount.store(0u, std::memory_order_relaxed);
  g_extendCount.store(0u, std::memory_order_relaxed);
  g_trimCount.store(0u, std::memory_order_relaxed);
  {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    PublishRangeSnapshotLocked();
  }
  g_initState.store(kInitReady, std::memory_order_release);
  war3dbg::Print("DXVK War3[TlsfPool]: 初始化完成 base=%p size=%zu MB\n",
                 g_mainPool, kInitialPoolSize >> 20);
  return true;
}

void TlsfPool_Shutdown() {
  if (g_initState.exchange(kInitUninitialized, std::memory_order_acq_rel) !=
      kInitReady)
    return;

  std::lock_guard<sync::Spinlock> lock(g_poolLock);
  for (auto &ep : g_extraPools) {
    if (ep.handle)
      tlsf_remove_pool(g_tlsf, ep.handle);
    if (ep.base)
      VirtualFree(ep.base, 0, MEM_RELEASE);
  }
  g_extraPools.clear();
  for (auto &ab : g_arenaBlocks) {
    if (ab.base)
      VirtualFree(ab.base, 0, MEM_RELEASE);
  }
  g_arenaBlocks.clear();
  g_rangeSnapshots.clear();
  g_activeRanges.store(nullptr, std::memory_order_release);

  if (g_mainPool) {
    VirtualFree(g_mainPool, 0, MEM_RELEASE);
    g_mainPool = nullptr;
  }
  g_tlsf = nullptr;
  war3dbg::Print("DXVK War3[TlsfPool]: 已关闭\n");
}

bool TlsfPool_IsInitialized() {
  return g_initState.load(std::memory_order_acquire) == kInitReady;
}

// ---- 分配 ----

void *TlsfPool_Alloc(size_t size) {
  if (!TlsfPool_IsInitialized() || size == 0)
    return nullptr;

  void *ptr = nullptr;
  size_t blockSize = 0;
  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    ptr = AllocWithExtendLocked(size, 0);
    if (ptr)
      blockSize = tlsf_block_size(ptr);
  } else {
    ptr = AllocWithExtendLocked(size, 0);
    if (ptr)
      blockSize = tlsf_block_size(ptr);
  }

  if (ptr) {
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    UpdatePeak(g_usedSize.fetch_add(blockSize, std::memory_order_relaxed) +
               blockSize);
  }
  return ptr;
}

void *TlsfPool_AllocAligned(size_t size, size_t alignment) {
  if (!TlsfPool_IsInitialized() || size == 0)
    return nullptr;
  if (alignment == 0)
    alignment = kDefaultAlignment;

  void *ptr = nullptr;
  size_t blockSize = 0;
  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    ptr = AllocWithExtendLocked(size, alignment);
    if (ptr)
      blockSize = tlsf_block_size(ptr);
  } else {
    ptr = AllocWithExtendLocked(size, alignment);
    if (ptr)
      blockSize = tlsf_block_size(ptr);
  }

  if (ptr) {
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    UpdatePeak(g_usedSize.fetch_add(blockSize, std::memory_order_relaxed) +
               blockSize);
  }
  return ptr;
}

void *TlsfPool_Realloc(void *ptr, size_t newSize) {
  if (!TlsfPool_IsInitialized())
    return nullptr;
  if (!ptr)
    return TlsfPool_Alloc(newSize);
  if (!newSize) {
    TlsfPool_Free(ptr);
    return nullptr;
  }

  size_t oldSize = 0;
  size_t newBlockSize = 0;
  void *newPtr = nullptr;

  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    oldSize = tlsf_block_size(ptr);
    newPtr = tlsf_realloc(g_tlsf, ptr, newSize);
    if (newPtr)
      newBlockSize = tlsf_block_size(newPtr);
  } else {
    oldSize = tlsf_block_size(ptr);
    newPtr = tlsf_realloc(g_tlsf, ptr, newSize);
    if (newPtr)
      newBlockSize = tlsf_block_size(newPtr);
  }

  if (newPtr) {
    if (newBlockSize > oldSize) {
      UpdatePeak(g_usedSize.fetch_add(newBlockSize - oldSize,
                                      std::memory_order_relaxed) +
                 (newBlockSize - oldSize));
    } else if (oldSize > newBlockSize) {
      g_usedSize.fetch_sub(oldSize - newBlockSize, std::memory_order_relaxed);
    }
  }
  return newPtr;
}

void TlsfPool_Free(void *ptr) {
  if (!ptr || !TlsfPool_IsInitialized())
    return;

  size_t blockSize = 0;

  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    blockSize = tlsf_block_size(ptr);
    tlsf_free(g_tlsf, ptr);
  } else {
    blockSize = tlsf_block_size(ptr);
    tlsf_free(g_tlsf, ptr);
  }

  g_freeCount.fetch_add(1, std::memory_order_relaxed);
  if (blockSize)
    g_usedSize.fetch_sub(blockSize, std::memory_order_relaxed);
}

bool TlsfPool_IsFromPool(void *ptr) {
  if (!ptr || !TlsfPool_IsInitialized())
    return false;

  const PoolRangeSnapshot *snapshot =
      g_activeRanges.load(std::memory_order_acquire);
  if (!snapshot)
    return false;

  for (uint32_t i = 0; i < snapshot->count; i++) {
    if (InRange(ptr, snapshot->ranges[i].base, snapshot->ranges[i].size))
      return true;
  }
  return false;
}

size_t TlsfPool_BlockSize(void *ptr) {
  if (!TlsfPool_IsFromPool(ptr))
    return 0;
  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    return tlsf_block_size(ptr);
  }
  return tlsf_block_size(ptr);
}

// ---- Arena 预留 ----

void *TlsfPool_AllocArenaBlock(size_t size, const char *purpose) {
  if (!TlsfPool_IsInitialized() || size == 0)
    return nullptr;

  void *ptr =
      VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!ptr) {
    war3dbg::Print("DXVK War3[TlsfPool]: Arena 预留失败 size=%zu purpose=%s\n",
                   size, purpose ? purpose : "?");
    return nullptr;
  }
  {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    g_arenaBlocks.push_back(ArenaBlock{ptr, size});
  }
  war3dbg::Print(
      "DXVK War3[TlsfPool]: Arena 预留 ptr=%p size=%zu MB purpose=%s\n", ptr,
      size >> 20, purpose ? purpose : "?");
  return ptr;
}

// ---- Trim ----

void TlsfPool_TrimFreePages() {
  if (!TlsfPool_IsInitialized())
    return;

  std::lock_guard<sync::Spinlock> lock(g_poolLock);

  size_t reclaimed = 0;
  bool rangesChanged = false;
  for (auto it = g_extraPools.begin(); it != g_extraPools.end();) {
    if (!it->handle) {
      ++it;
      continue;
    }

    // 检查此扩展池中是否有已分配块
    size_t usedInPool = 0;
    tlsf_walk_pool(
        it->handle,
        [](void *, size_t sz, int used, void *ctx) {
          if (used)
            *static_cast<size_t *>(ctx) += sz;
        },
        &usedInPool);

    if (usedInPool == 0) {
      tlsf_remove_pool(g_tlsf, it->handle);
      VirtualFree(it->base, 0, MEM_RELEASE);
      g_totalSize.fetch_sub(it->size, std::memory_order_relaxed);
      reclaimed += it->size;
      it = g_extraPools.erase(it);
      rangesChanged = true;
    } else {
      ++it;
    }
  }

  if (rangesChanged)
    PublishRangeSnapshotLocked();

  g_trimCount.fetch_add(1, std::memory_order_relaxed);
  if (reclaimed)
    war3dbg::Print("DXVK War3[TlsfPool]: Trim 回收 %zu MB\n", reclaimed >> 20);
}

// ---- 统计 ----

TlsfPoolStats TlsfPool_GetStats() {
  TlsfPoolStats s;
  s.totalSize = g_totalSize.load(std::memory_order_relaxed);
  s.usedSize = g_usedSize.load(std::memory_order_relaxed);
  s.freeSize = s.totalSize > s.usedSize ? s.totalSize - s.usedSize : 0;
  s.peakUsed = g_peakUsed.load(std::memory_order_relaxed);
  s.allocCount = g_allocCount.load(std::memory_order_relaxed);
  s.freeCount = g_freeCount.load(std::memory_order_relaxed);
  s.extendCount = g_extendCount.load(std::memory_order_relaxed);
  s.trimCount = g_trimCount.load(std::memory_order_relaxed);
  return s;
}

void TlsfPool_PrintStats() {
  auto s = TlsfPool_GetStats();
  war3dbg::Print("DXVK War3[TlsfPool]: 总=%zuMB 用=%zuMB 峰值=%zuMB 空闲=%zuMB "
                 "alloc=%zu free=%zu extend=%zu trim=%zu\n",
                 s.totalSize >> 20, s.usedSize >> 20, s.peakUsed >> 20,
                 s.freeSize >> 20, s.allocCount, s.freeCount, s.extendCount,
                 s.trimCount);
}

// ---- 线程安全控制 ----

void TlsfPool_DisableLock() {
  g_lockEnabled.store(false, std::memory_order_release);
  war3dbg::Print("DXVK War3[TlsfPool]: 锁已禁用（单线程模式）\n");
}

void TlsfPool_EnableLock() {
  g_lockEnabled.store(true, std::memory_order_release);
}

bool TlsfPool_IsLockEnabled() {
  return g_lockEnabled.load(std::memory_order_acquire);
}

} // namespace dxvk::war3::memory
