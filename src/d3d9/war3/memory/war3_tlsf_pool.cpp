// war3_tlsf_pool.cpp
// TLSF 内存池实现（从 StormBreaker 整合）
//
// 改动点（相较原 MemoryPool.cpp）：
//   1. 去除独立 Logger，改用 dxvk war3dbg::Print
//   2. 添加 TlsfPool_AllocArenaBlock（Shadow Arena 专用预留接口）
//   3. 使用独占 Spinlock 保护 TLSF 元数据，避免 shared_mutex 的错误并发
//   4. 使用固定 64 KiB 页目录完成 O(1) 所有权负查询
//   5. 扩池尺寸按 TLSF 真实 size class 计算，空扩展池即时归还
//   6. 所有返回给 Storm/Game 的范围必须完整位于低 2 GiB
//   7. 整合进 dxvk::war3::memory 命名空间

#include "war3_tlsf_pool.h"
#include "../../d3d9_war3_debug.h" // war3dbg::Print
#include "../../../util/sync/sync_spinlock.h"
#include "tlsf.h"


#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
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
constexpr size_t kAddressPageShift = 16;
constexpr size_t kAddressPageSize = size_t{1} << kAddressPageShift;
constexpr size_t kAddressPageCount = size_t{1} << (32 - kAddressPageShift);
constexpr uintptr_t kStormSignedAddressLimit = 0x80000000u;

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

tlsf_t g_tlsf = nullptr;
void *g_mainPool = nullptr;
std::vector<ExtraPool> g_extraPools;
std::vector<ArenaBlock> g_arenaBlocks;
std::array<std::atomic<uint8_t>, kAddressPageCount> g_addressDirectory{};

// 兼容旧接口保留该状态；生产实现始终保持 allocator 生命周期锁开启。
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

inline bool InRange(const void *ptr, const void *base, size_t size) {
  auto p = reinterpret_cast<uintptr_t>(ptr);
  auto b = reinterpret_cast<uintptr_t>(base);
  return p >= b && p < b + size;
}

bool IsStormCompatibleAddressRange(const void *base, size_t size) {
  if (!base || size == 0)
    return false;
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  return start < kStormSignedAddressLimit &&
         size <= kStormSignedAddressLimit - start;
}

void *AllocateLowRange(size_t size, const char *purpose) {
  void *base =
      VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!base || IsStormCompatibleAddressRange(base, size))
    return base;

  war3dbg::Print(
      "DXVK War3[TlsfPool]: 拒绝高地址范围 purpose=%s base=%p size=%zu\n",
      purpose ? purpose : "?", base, size);
  VirtualFree(base, 0, MEM_RELEASE);
  return nullptr;
}

bool RegisterAddressRangeLocked(const void *base, size_t size) {
  if (!IsStormCompatibleAddressRange(base, size))
    return false;
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  const size_t first = start >> kAddressPageShift;
  const size_t last = (start + size - 1) >> kAddressPageShift;
  if (last >= g_addressDirectory.size())
    return false;

  for (size_t page = first; page <= last; ++page) {
    if (g_addressDirectory[page].load(std::memory_order_relaxed) != 0)
      return false;
  }
  for (size_t page = first; page <= last; ++page)
    g_addressDirectory[page].store(1, std::memory_order_release);
  return true;
}

void UnregisterAddressRangeLocked(const void *base, size_t size) {
  if (!base || size == 0)
    return;
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  const size_t first = start >> kAddressPageShift;
  const size_t last = (start + size - 1) >> kAddressPageShift;
  if (last >= g_addressDirectory.size())
    return;
  for (size_t page = first; page <= last; ++page)
    g_addressDirectory[page].store(0, std::memory_order_release);
}

void UpdatePeak(size_t cur) {
  size_t peak = g_peakUsed.load(std::memory_order_relaxed);
  while (cur > peak && !g_peakUsed.compare_exchange_weak(
                           peak, cur, std::memory_order_relaxed)) {
  }
}

bool AddExtraPool(size_t size, void **addedBase = nullptr) {
  if (addedBase)
    *addedBase = nullptr;
  void *mem = AllocateLowRange(size, "growth");
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
  if (!RegisterAddressRangeLocked(mem, size)) {
    war3dbg::Print(
        "DXVK War3[TlsfPool]: 扩展池页目录登记失败 base=%p size=%zu\n",
        mem, size);
    tlsf_remove_pool(g_tlsf, ph);
    VirtualFree(mem, 0, MEM_RELEASE);
    return false;
  }
  try {
    const uintptr_t address = reinterpret_cast<uintptr_t>(mem);
    const auto insertion = std::lower_bound(
        g_extraPools.begin(), g_extraPools.end(), address,
        [](const ExtraPool &pool, uintptr_t value) {
          return reinterpret_cast<uintptr_t>(pool.base) < value;
        });
    g_extraPools.insert(insertion, ExtraPool{mem, size, ph});
  } catch (...) {
    UnregisterAddressRangeLocked(mem, size);
    tlsf_remove_pool(g_tlsf, ph);
    VirtualFree(mem, 0, MEM_RELEASE);
    return false;
  }
  g_totalSize.fetch_add(size, std::memory_order_relaxed);
  g_extendCount.fetch_add(1, std::memory_order_relaxed);
  if (addedBase)
    *addedBase = mem;
  war3dbg::Print("DXVK War3[TlsfPool]: 扩展池 +%zu MB，当前总计 %zu MB\n",
                 size >> 20, g_totalSize.load() >> 20);
  return true;
}

void ApplyUsedSizeDelta(size_t oldSize, size_t newSize) {
  if (newSize > oldSize) {
    const size_t growth = newSize - oldSize;
    UpdatePeak(g_usedSize.fetch_add(growth, std::memory_order_relaxed) +
               growth);
  } else if (oldSize > newSize) {
    g_usedSize.fetch_sub(oldSize - newSize, std::memory_order_relaxed);
  }
}

auto FindExtraPoolLocked(const void *ptr) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
  auto candidate = std::upper_bound(
      g_extraPools.begin(), g_extraPools.end(), address,
      [](uintptr_t value, const ExtraPool &pool) {
        return value < reinterpret_cast<uintptr_t>(pool.base);
      });
  if (candidate == g_extraPools.begin())
    return g_extraPools.end();
  --candidate;
  return InRange(ptr, candidate->base, candidate->size)
             ? candidate
             : g_extraPools.end();
}

bool IsExactAllocatedBlockLocked(void *ptr) {
  if (!ptr)
    return false;
  if (InRange(ptr, g_mainPool, kInitialPoolSize))
    return tlsf_block_is_valid_in_range(ptr, g_mainPool,
                                        kInitialPoolSize) != 0;
  const auto owner = FindExtraPoolLocked(ptr);
  return owner != g_extraPools.end() &&
         tlsf_block_is_valid_in_range(ptr, owner->base, owner->size) != 0;
}

bool RemoveEmptyExtraPoolLocked(void *base) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(base);
  const auto found = std::lower_bound(
      g_extraPools.begin(), g_extraPools.end(), address,
      [](const ExtraPool &pool, uintptr_t value) {
        return reinterpret_cast<uintptr_t>(pool.base) < value;
      });
  if (found == g_extraPools.end() || !tlsf_pool_is_empty(found->handle))
    return false;
  if (found->base != base)
    return false;

  const size_t size = found->size;
  tlsf_remove_pool(g_tlsf, found->handle);
  UnregisterAddressRangeLocked(found->base, found->size);
  VirtualFree(found->base, 0, MEM_RELEASE);
  g_extraPools.erase(found);
  g_totalSize.fetch_sub(size, std::memory_order_relaxed);
  g_trimCount.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// 带自动扩展的分配（持锁版本，供 Alloc/AllocAligned 调用）
void *AllocWithExtendLocked(size_t size, size_t align) {
  const size_t effectiveAlignment = align ? align : tlsf_align_size();
  if (tlsf_allocation_pool_size(size, effectiveAlignment) == 0u)
    return nullptr;
  // 先尝试不扩展
  void *ptr =
      align ? tlsf_memalign(g_tlsf, align, size) : tlsf_malloc(g_tlsf, size);
  if (ptr)
    return ptr;

  // 扩展后重试
  if (g_totalSize.load(std::memory_order_relaxed) >= kMaxPoolSize)
    return nullptr;

  constexpr size_t kGrowthSlack = 64 * 1024;
  const size_t allocationAlignment = align ? align : tlsf_align_size();
  const size_t minimum =
      tlsf_allocation_pool_size(size, allocationAlignment);
  if (!minimum || minimum > SIZE_MAX - kGrowthSlack)
    return nullptr;
  const size_t required = minimum + kGrowthSlack;
  const size_t granularity =
      required > kExtendGranularity ? kAddressPageSize : kExtendGranularity;
  if (required > SIZE_MAX - (granularity - 1))
    return nullptr;

  size_t ext = ((required + granularity - 1) / granularity) * granularity;
  const size_t total = g_totalSize.load(std::memory_order_relaxed);
  const size_t remaining = total < kMaxPoolSize ? kMaxPoolSize - total : 0;
  if (ext > remaining) {
    if (remaining < required)
      return nullptr;
    ext = remaining;
  }

  void *addedBase = nullptr;
  if (!AddExtraPool(ext, &addedBase))
    return nullptr;

  ptr = align ? tlsf_memalign(g_tlsf, align, size) : tlsf_malloc(g_tlsf, size);
  if (!ptr && addedBase)
    RemoveEmptyExtraPoolLocked(addedBase);
  return ptr;
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

  // vendored locality cache 是进程级单例，会把已释放块继续标为 used；
  // 这既破坏并发隔离，也会让 O(1) 空池判断永远无法闭合。
  tlsf_toggle_optimized_memory_locality(0);

  g_mainPool = AllocateLowRange(kInitialPoolSize, "main");
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

  if (!RegisterAddressRangeLocked(g_mainPool, kInitialPoolSize)) {
    war3dbg::Print("DXVK War3[TlsfPool]: 主池页目录登记失败\n");
    VirtualFree(g_mainPool, 0, MEM_RELEASE);
    g_mainPool = nullptr;
    g_tlsf = nullptr;
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
    if (ep.handle) {
      tlsf_remove_pool(g_tlsf, ep.handle);
      UnregisterAddressRangeLocked(ep.base, ep.size);
    }
    if (ep.base)
      VirtualFree(ep.base, 0, MEM_RELEASE);
  }
  g_extraPools.clear();
  for (auto &ab : g_arenaBlocks) {
    if (ab.base)
      VirtualFree(ab.base, 0, MEM_RELEASE);
  }
  g_arenaBlocks.clear();

  if (g_mainPool) {
    UnregisterAddressRangeLocked(g_mainPool, kInitialPoolSize);
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
  const size_t minimumAlignment = tlsf_align_size();
  if (alignment < minimumAlignment ||
      (alignment & (alignment - 1u)) != 0u ||
      alignment > std::numeric_limits<size_t>::max() - size ||
      size + alignment >
          std::numeric_limits<size_t>::max() - tlsf_pool_overhead() ||
      tlsf_allocation_pool_size(size, alignment) == 0u)
    return nullptr;

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
  if (tlsf_allocation_pool_size(newSize, tlsf_align_size()) == 0u)
    return nullptr;

  size_t oldSize = 0;
  size_t newBlockSize = 0;
  void *newPtr = nullptr;

  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    if (!IsExactAllocatedBlockLocked(ptr))
      return nullptr;
    const auto oldPool = FindExtraPoolLocked(ptr);
    void *oldPoolBase =
        oldPool != g_extraPools.end() ? oldPool->base : nullptr;
    oldSize = tlsf_block_size(ptr);
    newPtr = tlsf_realloc(g_tlsf, ptr, newSize);
    if (newPtr) {
      newBlockSize = tlsf_block_size(newPtr);
      ApplyUsedSizeDelta(oldSize, newBlockSize);
    }
    if (newPtr && oldPoolBase)
      RemoveEmptyExtraPoolLocked(oldPoolBase);
  } else {
    if (!IsExactAllocatedBlockLocked(ptr))
      return nullptr;
    oldSize = tlsf_block_size(ptr);
    newPtr = tlsf_realloc(g_tlsf, ptr, newSize);
    if (newPtr) {
      newBlockSize = tlsf_block_size(newPtr);
      ApplyUsedSizeDelta(oldSize, newBlockSize);
    }
  }
  return newPtr;
}

void *TlsfPool_ReallocInPlace(void *ptr, size_t newSize) {
  if (!TlsfPool_IsInitialized() || !ptr || !newSize)
    return nullptr;
  if (tlsf_allocation_pool_size(newSize, tlsf_align_size()) == 0u)
    return nullptr;

  size_t oldSize = 0;
  size_t newBlockSize = 0;
  void *result = nullptr;
  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    if (!IsExactAllocatedBlockLocked(ptr))
      return nullptr;
    oldSize = tlsf_block_size(ptr);
    result = tlsf_realloc_in_place(g_tlsf, ptr, newSize);
    if (result)
      newBlockSize = tlsf_block_size(result);
  } else {
    if (!IsExactAllocatedBlockLocked(ptr))
      return nullptr;
    oldSize = tlsf_block_size(ptr);
    result = tlsf_realloc_in_place(g_tlsf, ptr, newSize);
    if (result)
      newBlockSize = tlsf_block_size(result);
  }

  if (result) {
    if (newBlockSize > oldSize) {
      const size_t growth = newBlockSize - oldSize;
      UpdatePeak(g_usedSize.fetch_add(growth, std::memory_order_relaxed) +
                 growth);
    } else if (oldSize > newBlockSize) {
      g_usedSize.fetch_sub(oldSize - newBlockSize,
                           std::memory_order_relaxed);
    }
  }
  return result;
}

bool TlsfPool_Free(void *ptr) {
  if (!ptr || !TlsfPool_IsInitialized())
    return false;

  size_t blockSize = 0;

  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    if (!IsExactAllocatedBlockLocked(ptr))
      return false;
    const auto owner = FindExtraPoolLocked(ptr);
    void *ownerBase = owner != g_extraPools.end() ? owner->base : nullptr;
    blockSize = tlsf_block_size(ptr);
    tlsf_free(g_tlsf, ptr);
    if (blockSize)
      g_usedSize.fetch_sub(blockSize, std::memory_order_relaxed);
    if (ownerBase)
      RemoveEmptyExtraPoolLocked(ownerBase);
  } else {
    if (!IsExactAllocatedBlockLocked(ptr))
      return false;
    blockSize = tlsf_block_size(ptr);
    tlsf_free(g_tlsf, ptr);
    if (blockSize)
      g_usedSize.fetch_sub(blockSize, std::memory_order_relaxed);
  }

  g_freeCount.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool TlsfPool_InspectExactBlock(void *ptr,
                                TlsfPoolExactBlockInspector inspector,
                                void *context) {
  if (!ptr || !inspector || !TlsfPool_IsFromPool(ptr))
    return false;
  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    return IsExactAllocatedBlockLocked(ptr) && inspector(ptr, context);
  }
  return IsExactAllocatedBlockLocked(ptr) && inspector(ptr, context);
}

bool TlsfPool_IsFromPool(void *ptr) {
  if (!ptr || !TlsfPool_IsInitialized())
    return false;
  const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
  if (address >= kStormSignedAddressLimit)
    return false;
  return g_addressDirectory[address >> kAddressPageShift].load(
             std::memory_order_acquire) != 0;
}

size_t TlsfPool_BlockSize(void *ptr) {
  if (!TlsfPool_IsFromPool(ptr))
    return 0;
  if (g_lockEnabled.load(std::memory_order_acquire)) {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    return IsExactAllocatedBlockLocked(ptr) ? tlsf_block_size(ptr) : 0u;
  }
  return IsExactAllocatedBlockLocked(ptr) ? tlsf_block_size(ptr) : 0u;
}

// ---- Arena 预留 ----

void *TlsfPool_AllocArenaBlock(size_t size, const char *purpose) {
  if (!TlsfPool_IsInitialized() || size == 0)
    return nullptr;

  void *ptr = AllocateLowRange(size, purpose ? purpose : "arena");
  if (!ptr) {
    war3dbg::Print("DXVK War3[TlsfPool]: Arena 预留失败 size=%zu purpose=%s\n",
                   size, purpose ? purpose : "?");
    return nullptr;
  }
  {
    std::lock_guard<sync::Spinlock> lock(g_poolLock);
    try {
      g_arenaBlocks.push_back(ArenaBlock{ptr, size});
    } catch (...) {
      VirtualFree(ptr, 0, MEM_RELEASE);
      return nullptr;
    }
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
  for (auto it = g_extraPools.begin(); it != g_extraPools.end();) {
    if (!it->handle) {
      ++it;
      continue;
    }

    if (tlsf_pool_is_empty(it->handle)) {
      tlsf_remove_pool(g_tlsf, it->handle);
      UnregisterAddressRangeLocked(it->base, it->size);
      VirtualFree(it->base, 0, MEM_RELEASE);
      g_totalSize.fetch_sub(it->size, std::memory_order_relaxed);
      reclaimed += it->size;
      it = g_extraPools.erase(it);
    } else {
      ++it;
    }
  }

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
  // 扩展池会在 Free/Realloc 中即时退役；生产路径必须保留 allocator
  // 生命周期锁，不能再暴露旧版“假定 Storm 永远单线程”的优化开关。
  g_lockEnabled.store(true, std::memory_order_release);
  war3dbg::Print("DXVK War3[TlsfPool]: 生产路径拒绝禁用 allocator 锁\n");
}

void TlsfPool_EnableLock() {
  g_lockEnabled.store(true, std::memory_order_release);
}

bool TlsfPool_IsLockEnabled() {
  return g_lockEnabled.load(std::memory_order_acquire);
}

} // namespace dxvk::war3::memory
