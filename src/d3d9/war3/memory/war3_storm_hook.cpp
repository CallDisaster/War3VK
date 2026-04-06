// war3_storm_hook.cpp
// Storm 内存分配拦截实现（从 StormBreaker 整合）
//
// 改动点（相较原 StormHook.cpp）：
//   1. 使用 dxvk 的 InstallMinHook 替换 Detours
//   2. 日志改用 war3dbg::Print（仅 warn/error 及周期报告）
//   3. 用 TLSF 地址范围 + 私有头替代全局 map，实现零锁识别
//   4. 整合进 dxvk::war3::memory 命名空间

#include "war3_storm_hook.h"
#include "../../d3d9_war3_debug.h" // war3dbg::Print
#include "../hooks/war3_hook_install_util.h"
#include "war3_tlsf_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <windows.h>

// 返回调用者地址（MinGW/Clang/GCC）
#define STORM_HOOK_CALLER() __builtin_return_address(0)

namespace dxvk::war3::memory {

// ============================================================================
// 内部常量与状态
// ============================================================================

constexpr size_t kDefaultLargeBlockThreshold = 64 * 1024; // 64 KB

namespace {

std::atomic<size_t> g_largeBlockThreshold{kDefaultLargeBlockThreshold};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_redirectEnabled{true};
std::atomic<size_t> g_managedCount{0};
std::atomic<size_t> g_managedSize{0};

// 诊断开关：禁止 ReAlloc 新增 orig→TLSF 升级路径（已有托管块仍正常处理）
std::atomic<bool> g_disableRealloc{false};
// GetSize 采样计数器（每 16 次才打印一次，避免刷屏）
std::atomic<uint32_t> g_gsCounter{0};

// ---- Storm 函数指针 ----

using SMemAlloc_t = void *(__fastcall *)(int ecx, int edx, size_t size,
                                         const char *name, DWORD argList,
                                         DWORD flags);
using SMemFree_t = int(__stdcall *)(void *ptr, const char *name, int argList,
                                    DWORD flags);
using SMemGetSize_t = int(__stdcall *)(void *ptr, const char *name,
                                       int argList);
using SMemReAlloc_t = void *(__fastcall *)(int ecx, int edx, void *oldPtr,
                                           size_t newSize, const char *name,
                                           DWORD argList, DWORD flags);
SMemAlloc_t g_origAlloc = nullptr;
SMemFree_t g_origFree = nullptr;
SMemGetSize_t g_origGetSize = nullptr;
SMemReAlloc_t g_origReAlloc = nullptr;

// ---- 辅助 ----

StormTlsfHeader *HeaderFromUserPtr(void *userPtr) {
  return reinterpret_cast<StormTlsfHeader *>(
      reinterpret_cast<uint8_t *>(userPtr) - sizeof(StormTlsfHeader));
}

bool IsValidManagedHeader(const StormTlsfHeader *hdr) {
  if (!hdr)
    return false;
  if (hdr->magic != kStormBreakerMagic)
    return false;
  if (hdr->headerSize != sizeof(StormTlsfHeader))
    return false;
  if (hdr->rejectTag != kStormBreakerRejectTag)
    return false;
  return hdr->sizeCookie == (hdr->requestedSize ^ kStormBreakerCookie);
}

bool QueryManagedBlock(void *userPtr, StormTlsfHeader **outHdr = nullptr,
                       size_t *outSize = nullptr) {
  if (!userPtr)
    return false;

  auto *hdr = HeaderFromUserPtr(userPtr);
  if (!TlsfPool_IsFromPool(hdr))
    return false;
  if (!IsValidManagedHeader(hdr))
    return false;

  if (outHdr)
    *outHdr = hdr;
  if (outSize)
    *outSize = static_cast<size_t>(hdr->requestedSize);
  return true;
}

void SetupManagedHeader(void *userPtr, size_t requestSize) {
  auto *hdr = HeaderFromUserPtr(userPtr);
  hdr->magic = kStormBreakerMagic;
  hdr->requestedSize = static_cast<uint32_t>(requestSize);
  hdr->sizeCookie = static_cast<uint32_t>(requestSize) ^ kStormBreakerCookie;
  hdr->headerSize = sizeof(StormTlsfHeader);
  hdr->rejectTag = kStormBreakerRejectTag;
}

void PoisonManagedHeader(StormTlsfHeader *hdr) {
  if (!hdr)
    return;
  hdr->magic = 0u;
  hdr->requestedSize = 0u;
  hdr->sizeCookie = 0u;
  hdr->headerSize = 0u;
  hdr->rejectTag = 0u;
}

void NoteManagedAlloc(size_t size) {
  g_managedCount.fetch_add(1, std::memory_order_relaxed);
  g_managedSize.fetch_add(size, std::memory_order_relaxed);
}

void NoteManagedFree(size_t size) {
  g_managedCount.fetch_sub(1, std::memory_order_relaxed);
  if (size)
    g_managedSize.fetch_sub(size, std::memory_order_relaxed);
}

void NoteManagedResize(size_t oldSize, size_t newSize) {
  if (newSize > oldSize) {
    g_managedSize.fetch_add(newSize - oldSize, std::memory_order_relaxed);
  } else if (oldSize > newSize) {
    g_managedSize.fetch_sub(oldSize - newSize, std::memory_order_relaxed);
  }
}

void *AllocManagedBlock(size_t size) {
  const size_t allocSize = size + sizeof(StormTlsfHeader);
  // 强制 16 字节对齐：header=16 字节 + userPtr，rawPtr 对齐 16 则 userPtr
  // 也对齐 16。 War3 渲染路径使用 SSE movaps 等指令，要求内存地址严格 16
  // 字节对齐。
  void *rawPtr = TlsfPool_AllocAligned(allocSize, 16);
  if (!rawPtr)
    return nullptr;

  void *userPtr = reinterpret_cast<uint8_t *>(rawPtr) + sizeof(StormTlsfHeader);
  SetupManagedHeader(userPtr, size);
  NoteManagedAlloc(size);
  return userPtr;
}

bool CopyIntoNewBlock(void *dst, const void *src, size_t oldSize,
                      size_t newSize) {
  if (!dst || !src)
    return dst != nullptr;

  const size_t copySize = std::min(oldSize, newSize);
  if (copySize)
    std::memcpy(dst, src, copySize);
  return true;
}

LPVOID ResolveStormExport(HMODULE hStorm, const char *name, WORD ordinal) {
  if (name) {
    if (LPVOID target = reinterpret_cast<LPVOID>(GetProcAddress(hStorm, name)))
      return target;
  }
  if (ordinal != 0u)
    return reinterpret_cast<LPVOID>(GetProcAddress(
        hStorm, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(ordinal))));
  return nullptr;
}

// ============================================================================
// Hook 函数实现
// ============================================================================

/// SMemAlloc Hook：大块走 TLSF，小块走原生。
void *__fastcall Hooked_SMemAlloc(int ecx, int edx, size_t size,
                                  const char *name, DWORD argList,
                                  DWORD flags) {
  const size_t threshold =
      g_largeBlockThreshold.load(std::memory_order_relaxed);
  if (!g_redirectEnabled.load(std::memory_order_acquire) || size < threshold ||
      !TlsfPool_IsInitialized()) {
    return g_origAlloc(ecx, edx, size, name, argList, flags);
  }

  void *userPtr = AllocManagedBlock(size);
  if (!userPtr) {
    war3dbg::Print("[WARN] DXVK War3[StormHook]: TLSF 分配失败 size=%zu，回退 Storm\n",
                   size);
    return g_origAlloc(ecx, edx, size, name, argList, flags);
  }
  return userPtr;
}

/// SMemFree Hook
int __stdcall Hooked_SMemFree(void *ptr, const char *name, int argList,
                              DWORD flags) {
  if (!ptr)
    return 1;

  StormTlsfHeader *hdr = nullptr;
  size_t origSize = 0;
  if (QueryManagedBlock(ptr, &hdr, &origSize)) {
    PoisonManagedHeader(hdr);
    TlsfPool_Free(hdr);
    NoteManagedFree(origSize);
    return 1;
  }
  return g_origFree(ptr, name, argList, flags);
}

/// SMemGetSize Hook：先查管理表，否则回退原生。
int __stdcall Hooked_SMemGetSize(void *ptr, const char *name, int argList) {
  if (!ptr)
    return 0;

  size_t size = 0;
  if (QueryManagedBlock(ptr, nullptr, &size)) {
    g_gsCounter.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>(size);
  }
  return g_origGetSize(ptr, name, argList);
}

/// SMemReAlloc Hook
void *__fastcall Hooked_SMemReAlloc(int ecx, int edx, void *oldPtr,
                                    size_t newSize, const char *name,
                                    DWORD argList, DWORD flags) {
  const size_t threshold =
      g_largeBlockThreshold.load(std::memory_order_relaxed);

  if (!oldPtr)
    return Hooked_SMemAlloc(ecx, edx, newSize, name, argList, flags);
  if (!newSize) {
    Hooked_SMemFree(oldPtr, name, argList, flags);
    return nullptr;
  }

  void *caller = STORM_HOOK_CALLER();
  const char *safeNameRA = name ? name : "(null)";

  StormTlsfHeader *oldHdr = nullptr;
  size_t origSize = 0;
  if (QueryManagedBlock(oldPtr, &oldHdr, &origSize)) {
    if (newSize < threshold) {
      // TLSF → Storm downgrade（缩容至小块）
      void *nativeNew = g_origAlloc(ecx, edx, newSize, name, argList, flags);
      if (!nativeNew)
        return nullptr;

      CopyIntoNewBlock(nativeNew, oldPtr, origSize, newSize);
      PoisonManagedHeader(oldHdr);
      TlsfPool_Free(oldHdr);
      NoteManagedFree(origSize);
      return nativeNew;
    }

    // TLSF → TLSF（尝试原地扩容）
    const size_t newAllocSize = newSize + sizeof(StormTlsfHeader);
    void *rawOld = oldHdr;
    void *rawNew = TlsfPool_Realloc(rawOld, newAllocSize);
    if (rawNew) {
      void *userNew =
          reinterpret_cast<uint8_t *>(rawNew) + sizeof(StormTlsfHeader);
      SetupManagedHeader(userNew, newSize);
      NoteManagedResize(origSize, newSize);
      return userNew;
    }

    // TLSF → TLSF（alloc+copy）
    void *userNew = AllocManagedBlock(newSize);
    if (userNew) {
      CopyIntoNewBlock(userNew, oldPtr, origSize, newSize);
      PoisonManagedHeader(oldHdr);
      TlsfPool_Free(rawOld);
      NoteManagedFree(origSize);
      return userNew;
    }

    // TLSF → Storm 回退（TLSF OOM）
    void *nativeNew = g_origAlloc(ecx, edx, newSize, name, argList, flags);
    if (!nativeNew)
      return nullptr;

    CopyIntoNewBlock(nativeNew, oldPtr, origSize, newSize);
    PoisonManagedHeader(oldHdr);
    TlsfPool_Free(rawOld);
    NoteManagedFree(origSize);
    war3dbg::Print("[WARN] DXVK War3[StormHook]: TLSF ReAlloc OOM，回退 Storm "
                   "oldSz=%zu newSz=%zu\n",
                   origSize, newSize);
    return nativeNew;
  }

  // 原生块保持在 Storm：SMemReAlloc 升级到 TLSF 不安全。
  // 原因：调用方（如 TSExplicitList::Resize）假设成功 ReAlloc
  // 要么原地（指针不变） 要么失败（返回
  // nullptr），并在失败时自行重建内部指针链。 若我们把原生块移入
  // TLSF（新地址）并返回非空，调用方会误判为"原地成功"，
  // 导致内部自相对指针依然指向已释放的旧 Storm 地址，产生 use-after-free 崩溃。
  // 诊断开关 g_disableRealloc 保留，但此路径无论如何都直通原生。
  (void)caller;
  return g_origReAlloc(ecx, edx, oldPtr, newSize, name, argList, flags);
}

} // anonymous namespace

// ============================================================================
// 公共接口实现
// ============================================================================

bool StormHook_Install() {
  if (g_installed.load(std::memory_order_acquire))
    return true;
  if (!TlsfPool_IsInitialized()) {
    war3dbg::Print("DXVK War3[StormHook]: TlsfPool 未初始化，跳过安装\n");
    return false;
  }

  // 读取诊断环境变量（可在启动前通过 env_overrides_json 传入）
  {
    char buf[32] = {};
    if (GetEnvironmentVariableA("DXVK_WAR3_STORM_THRESHOLD_KB", buf,
                                sizeof(buf)) > 0) {
      const size_t kb = static_cast<size_t>(atoll(buf));
      if (kb >= 64) {
        g_largeBlockThreshold.store(kb * 1024, std::memory_order_release);
        war3dbg::Print("DXVK War3[StormHook]: 大块阈值覆盖为 %zu KB\n", kb);
      }
    }
    if (GetEnvironmentVariableA("DXVK_WAR3_STORM_DISABLE_REALLOC", buf,
                                sizeof(buf)) > 0) {
      if (buf[0] == '1') {
        g_disableRealloc.store(true, std::memory_order_release);
        war3dbg::Print(
            "DXVK War3[StormHook]: ReAlloc Storm->TLSF 升级已禁用\n");
      }
    }
  }

  HMODULE hStorm = GetModuleHandleA("Storm.dll");
  if (!hStorm) {
    war3dbg::Print("DXVK War3[StormHook]: Storm.dll 未加载\n");
    return false;
  }

  // Storm.dll 导出函数名（均为标准导出，按 ordinal 可能更稳）
  struct StormExport {
    const char *name;
    WORD ordinal;
    LPVOID detour;
    LPVOID *original;
  };
  StormExport exports[] = {
      {"SMemAlloc", 401u, (LPVOID)Hooked_SMemAlloc, (LPVOID *)&g_origAlloc},
      {"SMemFree", 403u, (LPVOID)Hooked_SMemFree, (LPVOID *)&g_origFree},
      {"SMemGetSize", 404u, (LPVOID)Hooked_SMemGetSize,
       (LPVOID *)&g_origGetSize},
      {"SMemReAlloc", 405u, (LPVOID)Hooked_SMemReAlloc,
       (LPVOID *)&g_origReAlloc},
  };

  using namespace dxvk::war3::hooks;
  bool anyOk = false;
  for (auto &e : exports) {
    LPVOID target = ResolveStormExport(hStorm, e.name, e.ordinal);
    if (!target) {
      war3dbg::Print("DXVK War3[StormHook]: 找不到导出 %s/#%u\n", e.name,
                     unsigned(e.ordinal));
      continue;
    }
    if (InstallMinHook(target, e.detour, e.original, "StormHook", e.name, true,
                       true)) {
      anyOk = true;
    }
  }

  if (anyOk) {
    g_redirectEnabled.store(true, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    war3dbg::Print(
        "DXVK War3[StormHook]: 安装完成，大块阈值=%zu KB，redirect=1 "
        "(实时拦截生效)\n",
        g_largeBlockThreshold.load() >> 10);

    // 为独立版 StormBreaker 创建互斥/信标信号，防止独立版重复加载挂钩
    CreateEventA(nullptr, TRUE, FALSE, "DXVK_War3_StormBreaker_Active");
  }
  return anyOk;
}

void StormHook_Uninstall() {
  // MinHook 由 dxvk 统一 MH_Uninitialize 处理，这里不需要单独 disable
  g_installed.store(false, std::memory_order_release);
}

bool StormHook_IsInstalled() {
  return g_installed.load(std::memory_order_acquire);
}

void StormHook_SetRedirectEnabled(bool enabled) {
  const bool prev =
      g_redirectEnabled.exchange(enabled, std::memory_order_acq_rel);
  if (prev == enabled)
    return;

  war3dbg::Print(
      "DXVK War3[StormHook]: large-block redirect=%d managedCount=%zu "
      "managedSize=%zu KB\n",
      enabled ? 1 : 0, g_managedCount.load(std::memory_order_relaxed),
      g_managedSize.load(std::memory_order_relaxed) >> 10);
}

bool StormHook_IsRedirectEnabled() {
  return g_redirectEnabled.load(std::memory_order_acquire);
}

void StormHook_SetLargeBlockThreshold(size_t bytes) {
  g_largeBlockThreshold.store(bytes, std::memory_order_release);
}

size_t StormHook_GetLargeBlockThreshold() {
  return g_largeBlockThreshold.load(std::memory_order_relaxed);
}

bool StormHook_IsOurBlock(void *userPtr) {
  return QueryManagedBlock(userPtr, nullptr, nullptr);
}

size_t StormHook_GetManagedCount() {
  return g_managedCount.load(std::memory_order_relaxed);
}

size_t StormHook_GetManagedSize() {
  return g_managedSize.load(std::memory_order_relaxed);
}

// Storm.dll g_TotalAllocatedMemory 偏移（War3 1.27x）。
// 该变量只增不减——这正是 StormBreaker 要修复的问题：大块转 TLSF 后，
// Storm 的计数不再增长，接近 2GB 上限时不再崩溃。
static constexpr uintptr_t kStormTotalAllocOffset = 0x5738Cu;

size_t StormHook_GetStormNativeAllocated() {
  HMODULE hStorm = GetModuleHandleA("Storm.dll");
  if (!hStorm)
    return 0u;
  // 直接读取：Storm.dll 已加载、偏移已验证，无需 SEH 保护。
  const uintptr_t addr =
      reinterpret_cast<uintptr_t>(hStorm) + kStormTotalAllocOffset;
  return *reinterpret_cast<const size_t *>(addr);
}

void StormHook_PrintPeriodicReport() {
  // 60 秒报告间隔（用 steady_clock 而非帧计数，避免帧率影响间隔）
  using Clock = std::chrono::steady_clock;
  static Clock::time_point s_lastReport = Clock::time_point{};

  const auto now = Clock::now();
  if (now - s_lastReport < std::chrono::seconds(60))
    return;
  s_lastReport = now;

  const TlsfPoolStats ps = TlsfPool_GetStats();
  const size_t stormNative = StormHook_GetStormNativeAllocated();
  const size_t managedCnt  = g_managedCount.load(std::memory_order_relaxed);
  const size_t managedSz   = g_managedSize.load(std::memory_order_relaxed);

  war3dbg::Print(
      "=== [StormBreaker 内存报告] ===\n"
      "  Storm g_TotalAllocatedMemory : %6zu MB  (只增不减，超 2GB 则崩溃；TLSF 接管后应稳定)\n"
      "  TLSF 托管（大块拦截）        : count=%-6zu  size=%6zu MB\n"
      "  TLSF 池容量                  : total=%5zu MB  used=%5zu MB  free=%5zu MB  peak=%5zu MB\n"
      "  TLSF 操作统计                : alloc=%-8zu  free=%-8zu  extend=%-4zu  trim=%-4zu\n"
      "================================\n",
      stormNative >> 20,
      managedCnt, managedSz >> 20,
      ps.totalSize >> 20, ps.usedSize >> 20, ps.freeSize >> 20, ps.peakUsed >> 20,
      ps.allocCount, ps.freeCount, ps.extendCount, ps.trimCount);
}

} // namespace dxvk::war3::memory
