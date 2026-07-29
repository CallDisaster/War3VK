// war3_storm_hook.cpp
// Storm 内存分配拦截实现（从 StormBreaker 整合）
//
// 改动点（相较原 StormHook.cpp）：
//   1. 使用 dxvk 的 InstallMinHook 替换 Detours
//   2. 日志改用 war3dbg::Print（仅 warn/error 及周期报告）
//   3. 用页目录负过滤 + allocator 锁内精确块/私有头校验替代全局 map
//   4. 生产策略固定为 0xFE7C 大块 TLSF + 原生小块 search 修复
//   5. 四个导出与小块内部 Hook 作为一个批次安装，禁止部分生效
//   6. 整合进 dxvk::war3::memory 命名空间

#include "war3_storm_hook.h"
#include "../../d3d9_war3_debug.h" // war3dbg::Print
#include "../hooks/war3_hook_install_util.h"
#include "../hooks/war3_hook_perf.h"
#include "war3_storm_native_small_repair.h"
#include "war3_tlsf_pool.h"

#include <MinHook.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <windows.h>

namespace dxvk::war3::memory {

// ============================================================================
// 内部常量与状态
// ============================================================================

// Storm 1.27a 在 size > 0xFE7B 时进入原生 VirtualAlloc 大块路径。
constexpr size_t kStormNativeLargeBlockThreshold = 0xFE7Cu;
constexpr uintptr_t kStormTotalAllocOffset = 0x5738Cu;
constexpr size_t kFreedPointerTableSize = 4096u;
constexpr uint32_t kManagedHeaderBusyMagic = 0x42555359u; // 'BUSY'
constexpr uint32_t kFreedHeaderMagic = 0xDEADDEADu;
constexpr uint32_t kFreedHeaderCookie = 0x46524545u; // 'FREE'
constexpr uint16_t kFreedHeaderTag = 0x4652u;        // 'FR'
constexpr uint8_t kExpectedStormSha256[32] = {
    0xF8, 0xF5, 0x19, 0xCF, 0xAA, 0x62, 0x75, 0xA5,
    0x17, 0x2A, 0x01, 0x4F, 0x0A, 0xBE, 0xD2, 0x21,
    0x22, 0x84, 0x39, 0x0A, 0x33, 0xF1, 0x19, 0x46,
    0x77, 0x15, 0x5A, 0x7D, 0x40, 0x8E, 0x63, 0xEB,
};
static_assert((kFreedPointerTableSize & (kFreedPointerTableSize - 1u)) == 0u,
              "近期释放表容量必须为 2 的幂");

namespace {

std::atomic<size_t> g_largeBlockThreshold{kStormNativeLargeBlockThreshold};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_installing{false};
std::atomic<bool> g_installHealthy{false};
std::atomic<bool> g_redirectEnabled{true};
std::atomic<size_t> g_managedCount{0};
std::atomic<size_t> g_managedSize{0};
std::array<std::atomic<uintptr_t>, kFreedPointerTableSize>
    g_recentlyFreedPointers{};
std::atomic<uint64_t> g_rejectedManagedPointers{0};
HANDLE g_integrationSignal = nullptr;

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

size_t FreedPointerSlot(const void *userPtr) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(userPtr) >> 4u;
  return static_cast<size_t>((value * 2654435761u) &
                             (kFreedPointerTableSize - 1u));
}

void ForgetFreedPointer(void *userPtr) {
  if (!userPtr)
    return;
  const uintptr_t value = reinterpret_cast<uintptr_t>(userPtr);
  auto &slot = g_recentlyFreedPointers[FreedPointerSlot(userPtr)];
  uintptr_t observed = slot.load(std::memory_order_relaxed);
  if (observed == value)
    slot.compare_exchange_strong(observed, 0u, std::memory_order_relaxed);
}

void RememberFreedPointer(void *userPtr) {
  if (!userPtr)
    return;
  g_recentlyFreedPointers[FreedPointerSlot(userPtr)].store(
      reinterpret_cast<uintptr_t>(userPtr), std::memory_order_release);
}

bool IsRecentlyFreedPointer(const void *userPtr) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(userPtr);
  if (value < sizeof(StormTlsfHeader))
    return false;
  return g_recentlyFreedPointers[FreedPointerSlot(userPtr)].load(
             std::memory_order_acquire) == value;
}

bool IsRejectedManagedPointer(void *userPtr) {
  if (IsRecentlyFreedPointer(userPtr))
    return true;
  const uintptr_t value = reinterpret_cast<uintptr_t>(userPtr);
  if (value < sizeof(StormTlsfHeader))
    return false;
  // 原生 Storm 块不可能位于 TLSF 的独占 VirtualAlloc 范围内。即使私有头
  // 已损坏或调用方传入了内部指针，也必须在这里拒绝，绝不能回落到 Storm。
  return TlsfPool_IsFromPool(HeaderFromUserPtr(userPtr));
}

void NoteRejectedManagedPointer(void *userPtr, const char *operation) {
  const uint64_t count =
      g_rejectedManagedPointers.fetch_add(1u, std::memory_order_relaxed) + 1u;
  // 只记录前四次和 2 的幂次数，避免损坏调用方制造日志风暴。
  if (count <= 4u || (count & (count - 1u)) == 0u) {
    war3dbg::Print(
        "[WARN] DXVK War3[StormHook]: 拒绝把已释放的托管指针交回 Storm "
        "op=%s ptr=%p count=%llu\n",
        operation ? operation : "?", userPtr,
        static_cast<unsigned long long>(count));
  }
}

bool IsValidManagedHeader(const StormTlsfHeader *hdr) {
  if (!hdr)
    return false;
  auto *magic = reinterpret_cast<volatile LONG *>(
      const_cast<uint32_t *>(&hdr->magic));
  if (static_cast<uint32_t>(InterlockedCompareExchange(magic, 0, 0)) !=
      kStormBreakerMagic)
    return false;
  if (hdr->headerSize != sizeof(StormTlsfHeader))
    return false;
  if (hdr->rejectTag != kStormBreakerRejectTag)
    return false;
  const bool valid =
      hdr->sizeCookie == (hdr->requestedSize ^ kStormBreakerCookie);
  return valid &&
         static_cast<uint32_t>(InterlockedCompareExchange(magic, 0, 0)) ==
             kStormBreakerMagic;
}

bool TryClaimManagedHeader(StormTlsfHeader *hdr) {
  if (!hdr)
    return false;
  return static_cast<uint32_t>(InterlockedCompareExchange(
             reinterpret_cast<volatile LONG *>(&hdr->magic),
             static_cast<LONG>(kManagedHeaderBusyMagic),
             static_cast<LONG>(kStormBreakerMagic))) == kStormBreakerMagic;
}

void RestoreManagedHeaderClaim(StormTlsfHeader *hdr) {
  if (!hdr)
    return;
  InterlockedCompareExchange(
      reinterpret_cast<volatile LONG *>(&hdr->magic),
      static_cast<LONG>(kStormBreakerMagic),
      static_cast<LONG>(kManagedHeaderBusyMagic));
}

struct ManagedBlockQueryContext {
  StormTlsfHeader **outHdr = nullptr;
  size_t *outSize = nullptr;
  bool claim = false;
};

bool InspectManagedBlock(void *block, void *opaque) {
  auto *hdr = static_cast<StormTlsfHeader *>(block);
  auto *context = static_cast<ManagedBlockQueryContext *>(opaque);
  if (!context || !IsValidManagedHeader(hdr))
    return false;

  const size_t requestedSize = static_cast<size_t>(hdr->requestedSize);
  if (context->claim && !TryClaimManagedHeader(hdr))
    return false;

  if (context->outHdr)
    *context->outHdr = hdr;
  if (context->outSize)
    *context->outSize = requestedSize;
  return true;
}

bool QueryManagedBlock(void *userPtr, StormTlsfHeader **outHdr = nullptr,
                       size_t *outSize = nullptr, bool claim = false) {
  if (!userPtr)
    return false;

  const uintptr_t value = reinterpret_cast<uintptr_t>(userPtr);
  if (value < sizeof(StormTlsfHeader) || (value & 0x0Fu) != 0u ||
      IsRecentlyFreedPointer(userPtr))
    return false;

  auto *hdr = HeaderFromUserPtr(userPtr);
  ManagedBlockQueryContext context{outHdr, outSize, claim};
  // 页目录只做 O(1) 负过滤；精确块起点、头部读取和原子 claim 必须
  // 同处 allocator 锁域，防止扩展池在两次检查之间被退役。
  return TlsfPool_InspectExactBlock(hdr, InspectManagedBlock, &context);
}

void SetupManagedHeader(void *userPtr, size_t requestSize) {
  ForgetFreedPointer(userPtr);
  auto *hdr = HeaderFromUserPtr(userPtr);
  hdr->requestedSize = static_cast<uint32_t>(requestSize);
  hdr->sizeCookie = static_cast<uint32_t>(requestSize) ^ kStormBreakerCookie;
  hdr->headerSize = sizeof(StormTlsfHeader);
  hdr->rejectTag = kStormBreakerRejectTag;
  InterlockedExchange(reinterpret_cast<volatile LONG *>(&hdr->magic),
                      static_cast<LONG>(kStormBreakerMagic));
}

void PoisonManagedHeader(StormTlsfHeader *hdr) {
  if (!hdr)
    return;
  hdr->requestedSize = 0u;
  hdr->sizeCookie = kFreedHeaderCookie;
  hdr->headerSize = sizeof(StormTlsfHeader);
  hdr->rejectTag = kFreedHeaderTag;
  InterlockedExchange(reinterpret_cast<volatile LONG *>(&hdr->magic),
                      static_cast<LONG>(kFreedHeaderMagic));
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
  if (size > std::numeric_limits<uint32_t>::max() ||
      size > std::numeric_limits<size_t>::max() - sizeof(StormTlsfHeader))
    return nullptr;
  const size_t allocSize = size + sizeof(StormTlsfHeader);
  // 强制 16 字节对齐：header=16 字节 + userPtr，rawPtr 对齐 16 则 userPtr
  // 也对齐 16。 War3 渲染路径使用 SSE movaps 等指令，要求内存地址严格 16
  // 字节对齐。
  void *rawPtr = TlsfPool_AllocAligned(allocSize, 16);
  if (!rawPtr)
    return nullptr;
  // 普通 tlsf_realloc 只保证 TLSF 的基础对齐；托管块则把 16-byte
  // 对齐作为公开 ABI（Game 渲染路径会使用 movaps）。任何后端回归都必须
  // fail-closed，不能发布一个下一轮 QueryManagedBlock 必然拒绝的活块。
  if ((reinterpret_cast<uintptr_t>(rawPtr) & 0x0Fu) != 0u) {
    TlsfPool_Free(rawPtr);
    return nullptr;
  }

  void *userPtr = reinterpret_cast<uint8_t *>(rawPtr) + sizeof(StormTlsfHeader);
  SetupManagedHeader(userPtr, size);
  NoteManagedAlloc(size);
  return userPtr;
}

bool ReleaseClaimedManagedBlock(void *userPtr, StormTlsfHeader *hdr,
                                size_t requestedSize) {
  if (!userPtr || !hdr)
    return false;
  RememberFreedPointer(userPtr);
  PoisonManagedHeader(hdr);
  if (!TlsfPool_Free(hdr)) {
    // 后端若拒绝精确块，恢复对调用方仍有效的旧块，禁止把它误交给 Storm。
    SetupManagedHeader(userPtr, requestedSize);
    NoteRejectedManagedPointer(userPtr, "tlsf-free");
    return false;
  }
  NoteManagedFree(requestedSize);
  return true;
}

bool DiscardManagedBlock(void *userPtr) {
  StormTlsfHeader *hdr = nullptr;
  size_t requestedSize = 0;
  return QueryManagedBlock(userPtr, &hdr, &requestedSize, true) &&
         ReleaseClaimedManagedBlock(userPtr, hdr, requestedSize);
}

void ZeroRequestedRange(void *ptr, size_t begin, size_t end) {
  if (ptr && end > begin)
    std::memset(reinterpret_cast<uint8_t *>(ptr) + begin, 0, end - begin);
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

struct StormHookBinding {
  const char *name = nullptr;
  LPVOID target = nullptr;
  LPVOID detour = nullptr;
  LPVOID *original = nullptr;
  bool created = false;
};

enum class HookBatchInstallResult : uint8_t {
  Success,
  CleanFailure,
  IndeterminateFailure,
};

struct StormExportProfile {
  const char *name;
  WORD ordinal;
  uint32_t rva;
  uint8_t prologLength;
  uint8_t prolog[8];
};

bool HashFileSha256(const wchar_t *path, uint8_t (&output)[32],
                    uint64_t &fileSize) {
  using OpenAlgorithmFn = NTSTATUS(WINAPI *)(BCRYPT_ALG_HANDLE *, LPCWSTR,
                                             LPCWSTR, ULONG);
  using GetPropertyFn = NTSTATUS(WINAPI *)(BCRYPT_HANDLE, LPCWSTR, PUCHAR,
                                           ULONG, ULONG *, ULONG);
  using CreateHashFn = NTSTATUS(WINAPI *)(BCRYPT_ALG_HANDLE,
                                          BCRYPT_HASH_HANDLE *, PUCHAR, ULONG,
                                          PUCHAR, ULONG, ULONG);
  using HashDataFn = NTSTATUS(WINAPI *)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG,
                                        ULONG);
  using FinishHashFn = NTSTATUS(WINAPI *)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG,
                                          ULONG);
  using DestroyHashFn = NTSTATUS(WINAPI *)(BCRYPT_HASH_HANDLE);
  using CloseAlgorithmFn = NTSTATUS(WINAPI *)(BCRYPT_ALG_HANDLE, ULONG);

  HMODULE bcrypt = LoadLibraryW(L"bcrypt.dll");
  if (!bcrypt)
    return false;
  const auto openAlgorithm = reinterpret_cast<OpenAlgorithmFn>(
      GetProcAddress(bcrypt, "BCryptOpenAlgorithmProvider"));
  const auto getProperty = reinterpret_cast<GetPropertyFn>(
      GetProcAddress(bcrypt, "BCryptGetProperty"));
  const auto createHash = reinterpret_cast<CreateHashFn>(
      GetProcAddress(bcrypt, "BCryptCreateHash"));
  const auto hashData = reinterpret_cast<HashDataFn>(
      GetProcAddress(bcrypt, "BCryptHashData"));
  const auto finishHash = reinterpret_cast<FinishHashFn>(
      GetProcAddress(bcrypt, "BCryptFinishHash"));
  const auto destroyHash = reinterpret_cast<DestroyHashFn>(
      GetProcAddress(bcrypt, "BCryptDestroyHash"));
  const auto closeAlgorithm = reinterpret_cast<CloseAlgorithmFn>(
      GetProcAddress(bcrypt, "BCryptCloseAlgorithmProvider"));
  if (!openAlgorithm || !getProperty || !createHash || !hashData ||
      !finishHash || !destroyHash || !closeAlgorithm) {
    FreeLibrary(bcrypt);
    return false;
  }

  HANDLE file = CreateFileW(path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    FreeLibrary(bcrypt);
    return false;
  }

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  PUCHAR hashObject = nullptr;
  PUCHAR buffer = nullptr;
  LARGE_INTEGER length = {};
  ULONG objectSize = 0;
  ULONG resultSize = 0;
  bool ok = GetFileSizeEx(file, &length) && length.QuadPart >= 0;
  if (ok)
    fileSize = static_cast<uint64_t>(length.QuadPart);
  if (ok) {
    ok = BCRYPT_SUCCESS(openAlgorithm(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                      nullptr, 0)) &&
         BCRYPT_SUCCESS(getProperty(
             algorithm, BCRYPT_OBJECT_LENGTH,
             reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
             &resultSize, 0)) &&
         objectSize != 0;
  }
  if (ok) {
    hashObject = static_cast<PUCHAR>(HeapAlloc(GetProcessHeap(), 0, objectSize));
    buffer = static_cast<PUCHAR>(HeapAlloc(GetProcessHeap(), 0, 64u * 1024u));
    ok = hashObject && buffer &&
         BCRYPT_SUCCESS(createHash(algorithm, &hash, hashObject, objectSize,
                                   nullptr, 0, 0));
  }
  while (ok) {
    DWORD bytesRead = 0;
    if (!ReadFile(file, buffer, 64u * 1024u, &bytesRead, nullptr)) {
      ok = false;
      break;
    }
    if (bytesRead == 0)
      break;
    ok = BCRYPT_SUCCESS(hashData(hash, buffer, bytesRead, 0));
  }
  if (ok)
    ok = BCRYPT_SUCCESS(finishHash(hash, output, sizeof(output), 0));

  if (hash)
    destroyHash(hash);
  if (algorithm)
    closeAlgorithm(algorithm, 0);
  if (buffer)
    HeapFree(GetProcessHeap(), 0, buffer);
  if (hashObject)
    HeapFree(GetProcessHeap(), 0, hashObject);
  CloseHandle(file);
  FreeLibrary(bcrypt);
  return ok;
}

constexpr StormExportProfile kStormExports[] = {
    {"SMemAlloc", 401u, 0x2B830u, 6u,
     {0x55, 0x8B, 0xEC, 0x51, 0x83, 0x3D, 0x7C, 0x6F}},
    {"SMemFree", 403u, 0x2BE40u, 5u,
     {0x55, 0x8B, 0xEC, 0x83, 0x3D, 0x7C, 0x6F, 0x05}},
    {"SMemGetSize", 404u, 0x2C000u, 5u,
     {0x55, 0x8B, 0xEC, 0x83, 0x3D, 0x7C, 0x6F, 0x05}},
    {"SMemReAlloc", 405u, 0x2C8B0u, 5u,
     {0x55, 0x8B, 0xEC, 0x83, 0x3D, 0x7C, 0x6F, 0x05}},
};

bool VerifyStorm127a(HMODULE stormModule, StormHookBinding *bindings) {
  if (!stormModule || !bindings || sizeof(void *) != 4)
    return false;

  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(stormModule);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(
      reinterpret_cast<const uint8_t *>(stormModule) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      nt->FileHeader.TimeDateStamp != 0x56BD0E34u ||
      nt->OptionalHeader.SizeOfImage != 0x00061000u)
    return false;

  wchar_t path[MAX_PATH] = {};
  uint8_t digest[32] = {};
  uint64_t fileSize = 0;
  if (!GetModuleFileNameW(stormModule, path, MAX_PATH) ||
      !HashFileSha256(path, digest, fileSize))
    return false;
  if (fileSize != 334312u ||
      std::memcmp(digest, kExpectedStormSha256, sizeof(digest)) != 0)
    return false;

  const uintptr_t base = reinterpret_cast<uintptr_t>(stormModule);
  for (size_t i = 0; i < std::size(kStormExports); ++i) {
    const auto &profile = kStormExports[i];
    LPVOID target = reinterpret_cast<LPVOID>(GetProcAddress(
        stormModule,
        reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(profile.ordinal))));
    if (!target || reinterpret_cast<uintptr_t>(target) != base + profile.rva ||
        std::memcmp(target, profile.prolog, profile.prologLength) != 0)
      return false;
    bindings[i].name = profile.name;
    bindings[i].target = target;
  }
  return true;
}

struct NativeLargeBlockInfo {
  bool valid = false;
  size_t actualSize = 0;
  size_t stormAccountedSize = 0;
  size_t counterCorrection = 0;
};

bool NativeReallocReleasedOldPointer(void *oldPtr, void *newPtr,
                                     size_t newSize) noexcept {
  // Storm 405 把 size==0 定义为释放旧块，成功返回 nullptr；这里与
  // StormBreaker v1.3.0 的原生大块计数合同保持一致。
  return oldPtr && ((newPtr && newPtr != oldPtr) || newSize == 0);
}

NativeLargeBlockInfo QueryNativeLargeBlockForCounterFix(void *userPtr) {
  NativeLargeBlockInfo info{};
  if (!userPtr)
    return info;

  const uintptr_t value = reinterpret_cast<uintptr_t>(userPtr);
  if (value < 16u || (value & 0x07u) != 0u)
    return info;

  // 这里只处理即将交给 Storm 原生 Free/ReAlloc 的合法指针；损坏指针本就
  // 不具备原生 allocator 合同。避免在每个小块释放上引入 ReadProcessMemory
  // 系统调用，同时保持与 v1.3.0 相同的头部判定顺序。
  auto *ptr = static_cast<uint8_t *>(userPtr);
  const uint8_t userFlags = *(ptr - 5);
  if ((userFlags & 0x08u) == 0u)
    return info;

  auto *stormHeader = *reinterpret_cast<uint16_t **>(ptr - 12);
  if (!stormHeader)
    return info;
  const uint8_t stormHeaderFlags =
      *(reinterpret_cast<uint8_t *>(stormHeader) + 3);
  if ((stormHeaderFlags & 0x04u) == 0u)
    return info;

  const size_t actualSize = *reinterpret_cast<uint32_t *>(ptr - 16);
  const size_t accountedSize = *stormHeader;
  if (actualSize < kStormNativeLargeBlockThreshold ||
      actualSize <= accountedSize)
    return info;

  info.valid = true;
  info.actualSize = actualSize;
  info.stormAccountedSize = accountedSize;
  info.counterCorrection = actualSize - accountedSize;
  return info;
}

void ApplyNativeLargeFreeCounterCorrection(const NativeLargeBlockInfo &info) {
  if (!info.valid || info.counterCorrection == 0u)
    return;

  HMODULE stormModule = GetModuleHandleA("Storm.dll");
  if (!stormModule)
    return;

  auto *counter = reinterpret_cast<volatile LONG *>(
      reinterpret_cast<uintptr_t>(stormModule) + kStormTotalAllocOffset);
  LONG observed = *counter;
  for (;;) {
    const uint32_t current = static_cast<uint32_t>(observed);
    const uint32_t correction = static_cast<uint32_t>(std::min<size_t>(
        info.counterCorrection, std::numeric_limits<uint32_t>::max()));
    const uint32_t desired = current > correction ? current - correction : 0u;
    const LONG previous = InterlockedCompareExchange(
        counter, static_cast<LONG>(desired), observed);
    if (previous == observed)
      break;
    observed = previous;
  }
}

void *CallOriginalAlloc(int ecx, int edx, size_t size, const char *name,
                        DWORD argList, DWORD flags) {
  void *result = g_origAlloc(ecx, edx, size, name, argList, flags);
  if (result)
    ForgetFreedPointer(result);
  return result;
}

int CallOriginalFree(void *ptr, const char *name, int argList, DWORD flags) {
  const NativeLargeBlockInfo nativeLargeInfo =
      QueryNativeLargeBlockForCounterFix(ptr);
  const int result = g_origFree(ptr, name, argList, flags);
  if (result != 0)
    ApplyNativeLargeFreeCounterCorrection(nativeLargeInfo);
  return result;
}

void *CallOriginalReAlloc(int ecx, int edx, void *oldPtr, size_t newSize,
                          const char *name, DWORD argList, DWORD flags) {
  const NativeLargeBlockInfo nativeLargeInfo =
      QueryNativeLargeBlockForCounterFix(oldPtr);
  void *result =
      g_origReAlloc(ecx, edx, oldPtr, newSize, name, argList, flags);
  if (result)
    ForgetFreedPointer(result);
  if ((flags & 0x10u) == 0u &&
      NativeReallocReleasedOldPointer(oldPtr, result, newSize))
    ApplyNativeLargeFreeCounterCorrection(nativeLargeInfo);
  return result;
}

bool RollbackStormHookBatch(StormHookBinding *bindings, size_t count) {
  bool closed = true;
  for (size_t i = 0; i < count; ++i) {
    if (!bindings[i].created)
      continue;
    const MH_STATUS disableStatus = MH_DisableHook(bindings[i].target);
    if (disableStatus != MH_OK && disableStatus != MH_ERROR_DISABLED &&
        disableStatus != MH_ERROR_NOT_CREATED) {
      closed = false;
      war3dbg::Print(
          "DXVK War3[StormHook]: 回滚禁用失败 name=%s status=%d\n",
          bindings[i].name ? bindings[i].name : "?",
          static_cast<int>(disableStatus));
    }
    const MH_STATUS removeStatus = MH_RemoveHook(bindings[i].target);
    if (removeStatus == MH_OK || removeStatus == MH_ERROR_NOT_CREATED) {
      bindings[i].created = false;
    } else {
      closed = false;
      war3dbg::Print(
          "DXVK War3[StormHook]: 回滚移除失败 name=%s status=%d\n",
          bindings[i].name ? bindings[i].name : "?",
          static_cast<int>(removeStatus));
    }
  }
  return closed;
}

HookBatchInstallResult InstallStormHookBatch(StormHookBinding *bindings,
                                              size_t count) {
  const MH_STATUS initStatus = MH_Initialize();
  if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
    war3dbg::Print("DXVK War3[StormHook]: MH_Initialize 失败 status=%d\n",
                   static_cast<int>(initStatus));
    return HookBatchInstallResult::CleanFailure;
  }

  for (size_t i = 0; i < count; ++i) {
    const MH_STATUS status = MH_CreateHook(bindings[i].target,
                                           bindings[i].detour,
                                           bindings[i].original);
    if (status != MH_OK) {
      war3dbg::Print(
          "DXVK War3[StormHook]: 创建批次 Hook 失败 name=%s status=%d\n",
          bindings[i].name ? bindings[i].name : "?", static_cast<int>(status));
      return RollbackStormHookBatch(bindings, count)
                 ? HookBatchInstallResult::CleanFailure
                 : HookBatchInstallResult::IndeterminateFailure;
    }
    bindings[i].created = true;
  }

  for (size_t i = 0; i < count; ++i) {
    const MH_STATUS status = MH_QueueEnableHook(bindings[i].target);
    if (status != MH_OK) {
      war3dbg::Print(
          "DXVK War3[StormHook]: 排队启用批次 Hook 失败 name=%s status=%d\n",
          bindings[i].name ? bindings[i].name : "?", static_cast<int>(status));
      return RollbackStormHookBatch(bindings, count)
                 ? HookBatchInstallResult::CleanFailure
                 : HookBatchInstallResult::IndeterminateFailure;
    }
  }

  const MH_STATUS applyStatus = MH_ApplyQueued();
  if (applyStatus != MH_OK) {
    war3dbg::Print("DXVK War3[StormHook]: 批次启用失败 status=%d\n",
                   static_cast<int>(applyStatus));
    return RollbackStormHookBatch(bindings, count)
               ? HookBatchInstallResult::CleanFailure
               : HookBatchInstallResult::IndeterminateFailure;
  }
  for (size_t i = 0; i < count; ++i) {
    hooks::RecordHookInstallState(
        "Storm", bindings[i].name, bindings[i].target,
        bindings[i].detour,
        bindings[i].original ? *bindings[i].original : nullptr,
        "installed", true);
  }
  return HookBatchInstallResult::Success;
}

struct ScopedInstallFlag {
  ~ScopedInstallFlag() { g_installing.store(false, std::memory_order_release); }
};

bool IsStandaloneStormBreakerLoaded() {
  constexpr const char *kStandaloneModules[] = {
      "StormBreaker.asi",
      "StormBreaker-TLSF.asi",
      "StormBreaker-mimalloc.asi",
      "StormBreaker-hybrid.asi",
  };
  for (const char *module : kStandaloneModules) {
    if (GetModuleHandleA(module))
      return true;
  }
  return false;
}

bool ReserveIntegrationSignal() {
  if (g_integrationSignal)
    return true;
  SetLastError(ERROR_SUCCESS);
  HANDLE signal =
      CreateEventA(nullptr, TRUE, FALSE, "DXVK_War3_StormBreaker_Active");
  const DWORD error = GetLastError();
  if (!signal) {
    war3dbg::Print(
        "DXVK War3[StormHook]: 无法创建会话级兼容信号 err=%lu\n", error);
    return false;
  }
  if (error == ERROR_ALREADY_EXISTS) {
    // 旧 standalone 只会按固定名字 OpenEvent，因此该协议天然是会话级
    // 广播而不是进程互斥。另一个 DXVK 进程已经持有信号时，本进程仍须
    // 依靠自己的 SHA/RVA/prolog 校验决定是否可安装，不能被误阻断。
    war3dbg::Print(
        "DXVK War3[StormHook]: 复用已有会话级 standalone 兼容信号\n");
  }
  g_integrationSignal = signal;
  return true;
}

void ReleaseIntegrationSignal() {
  if (!g_integrationSignal)
    return;
  CloseHandle(g_integrationSignal);
  g_integrationSignal = nullptr;
}

// ============================================================================
// Hook 函数实现
// ============================================================================

/// SMemAlloc Hook：大块走 TLSF，小块走原生。
void *__fastcall Hooked_SMemAlloc(int ecx, int edx, size_t size,
                                  const char *name, DWORD argList,
                                  DWORD flags) {
  hooks::War3HotHookCallTiming hookTiming(
      hooks::War3HotHookId::StormAlloc, 64u);
  const auto callNativeOriginal = [&]() {
    hooks::War3HotHookNativeScope nativeTiming(hookTiming);
    return CallOriginalAlloc(ecx, edx, size, name, argList, flags);
  };
  const size_t threshold =
      g_largeBlockThreshold.load(std::memory_order_relaxed);
  if (!g_redirectEnabled.load(std::memory_order_acquire) || size < threshold ||
      !TlsfPool_IsInitialized()) {
    return callNativeOriginal();
  }

  void *userPtr = AllocManagedBlock(size);
  if (!userPtr) {
    war3dbg::Print("[WARN] DXVK War3[StormHook]: TLSF 分配失败 size=%zu，回退 Storm\n",
                   size);
    return callNativeOriginal();
  }
  if ((flags & 0x08u) != 0)
    ZeroRequestedRange(userPtr, 0, size);
  return userPtr;
}

/// SMemFree Hook
int __stdcall Hooked_SMemFree(void *ptr, const char *name, int argList,
                              DWORD flags) {
  hooks::War3HotHookCallTiming hookTiming(
      hooks::War3HotHookId::StormFree, 64u);
  if (!ptr)
    return 1;

  StormTlsfHeader *hdr = nullptr;
  size_t origSize = 0;
  if (QueryManagedBlock(ptr, &hdr, &origSize, true)) {
    return ReleaseClaimedManagedBlock(ptr, hdr, origSize) ? 1 : 0;
  }
  if (IsRejectedManagedPointer(ptr)) {
    NoteRejectedManagedPointer(ptr, "free");
    return 1;
  }
  hooks::War3HotHookNativeScope nativeTiming(hookTiming);
  return CallOriginalFree(ptr, name, argList, flags);
}

/// SMemGetSize Hook：先查管理表，否则回退原生。
int __stdcall Hooked_SMemGetSize(void *ptr, const char *name, int argList) {
  hooks::War3HotHookCallTiming hookTiming(
      hooks::War3HotHookId::StormGetSize, 64u);
  if (!ptr)
    return 0;

  size_t size = 0;
  if (QueryManagedBlock(ptr, nullptr, &size))
    return static_cast<int>(size);
  if (IsRejectedManagedPointer(ptr)) {
    NoteRejectedManagedPointer(ptr, "get-size");
    return -1;
  }
  hooks::War3HotHookNativeScope nativeTiming(hookTiming);
  return g_origGetSize(ptr, name, argList);
}

/// SMemReAlloc Hook
void *__fastcall Hooked_SMemReAlloc(int ecx, int edx, void *oldPtr,
                                    size_t newSize, const char *name,
                                    DWORD argList, DWORD flags) {
  hooks::War3HotHookCallTiming hookTiming(
      hooks::War3HotHookId::StormReAlloc, 64u);
  const auto callNativeAlloc = [&](size_t size) {
    hooks::War3HotHookNativeScope nativeTiming(hookTiming);
    return CallOriginalAlloc(ecx, edx, size, name, argList, flags);
  };
  const auto callNativeFree = [&](void* ptr) {
    hooks::War3HotHookNativeScope nativeTiming(hookTiming);
    return CallOriginalFree(ptr, name, static_cast<int>(argList), flags);
  };
  const size_t threshold =
      g_largeBlockThreshold.load(std::memory_order_relaxed);
  constexpr DWORD kZeroNewMemory = 0x08u;
  constexpr DWORD kNoMove = 0x10u;

  if (!oldPtr)
    return Hooked_SMemAlloc(ecx, edx, newSize, name, argList, flags);

  StormTlsfHeader *oldHdr = nullptr;
  size_t origSize = 0;
  if (QueryManagedBlock(oldPtr, &oldHdr, &origSize, true)) {
    if (!newSize) {
      if ((flags & kNoMove) != 0) {
        RestoreManagedHeaderClaim(oldHdr);
        return nullptr;
      }
      ReleaseClaimedManagedBlock(oldPtr, oldHdr, origSize);
      return nullptr;
    }

    if (newSize > std::numeric_limits<uint32_t>::max() ||
        newSize > std::numeric_limits<size_t>::max() -
                      sizeof(StormTlsfHeader)) {
      RestoreManagedHeaderClaim(oldHdr);
      return nullptr;
    }

    const bool zeroNewMemory = (flags & kZeroNewMemory) != 0;
    if ((flags & kNoMove) != 0) {
      const size_t newAllocSize = newSize + sizeof(StormTlsfHeader);
      void *rawNew = TlsfPool_ReallocInPlace(oldHdr, newAllocSize);
      if (!rawNew) {
        RestoreManagedHeaderClaim(oldHdr);
        return nullptr;
      }
      void *userNew =
          reinterpret_cast<uint8_t *>(rawNew) + sizeof(StormTlsfHeader);
      SetupManagedHeader(userNew, newSize);
      NoteManagedResize(origSize, newSize);
      if (zeroNewMemory)
        ZeroRequestedRange(userNew, origSize, newSize);
      return userNew;
    }

    if (newSize < threshold) {
      // TLSF → Storm downgrade（缩容至小块）
      void *nativeNew = callNativeAlloc(newSize);
      if (!nativeNew) {
        RestoreManagedHeaderClaim(oldHdr);
        return nullptr;
      }

      CopyIntoNewBlock(nativeNew, oldPtr, origSize, newSize);
      if (!ReleaseClaimedManagedBlock(oldPtr, oldHdr, origSize)) {
        if (!callNativeFree(nativeNew))
          war3dbg::Print(
              "[WARN] DXVK War3[StormHook]: downgrade 回滚原生块失败 ptr=%p\n",
              nativeNew);
        return nullptr;
      }
      return nativeNew;
    }

    // TLSF → TLSF（先尝试原地扩容）
    //
    // 不能使用普通 TlsfPool_Realloc：初始托管块由 16-byte memalign
    // 创建，但普通 tlsf_realloc 在移动时只保证基础对齐。它曾返回 8-byte
    // 对齐的新地址；SetupManagedHeader 随后发布了这个“活块”，而下一次
    // QueryManagedBlock 又按公开 ABI 拒绝它，最终令 Game 收到 nullptr 并在
    // Game.dll+0xFB135 写空基址。原地调整保留原有对齐；失败后由下方
    // AllocManagedBlock(16-byte aligned)+copy 路径完成安全移动。
    const size_t newAllocSize = newSize + sizeof(StormTlsfHeader);
    void *rawOld = oldHdr;
    void *rawNew = TlsfPool_ReallocInPlace(rawOld, newAllocSize);
    if (rawNew) {
      void *userNew =
          reinterpret_cast<uint8_t *>(rawNew) + sizeof(StormTlsfHeader);
      SetupManagedHeader(userNew, newSize);
      NoteManagedResize(origSize, newSize);
      if (zeroNewMemory)
        ZeroRequestedRange(userNew, origSize, newSize);
      return userNew;
    }

    // TLSF → TLSF（16-byte aligned alloc+copy）
    void *userNew = AllocManagedBlock(newSize);
    if (userNew) {
      CopyIntoNewBlock(userNew, oldPtr, origSize, newSize);
      if (zeroNewMemory)
        ZeroRequestedRange(userNew, origSize, newSize);
      if (!ReleaseClaimedManagedBlock(oldPtr, oldHdr, origSize)) {
        if (!DiscardManagedBlock(userNew))
          war3dbg::Print(
              "[WARN] DXVK War3[StormHook]: alloc+copy 回滚新 TLSF 块失败 ptr=%p\n",
              userNew);
        return nullptr;
      }
      return userNew;
    }

    // TLSF → Storm 回退（TLSF OOM）
    void *nativeNew = callNativeAlloc(newSize);
    if (!nativeNew) {
      RestoreManagedHeaderClaim(oldHdr);
      return nullptr;
    }

    CopyIntoNewBlock(nativeNew, oldPtr, origSize, newSize);
    if (!ReleaseClaimedManagedBlock(oldPtr, oldHdr, origSize)) {
      if (!callNativeFree(nativeNew))
        war3dbg::Print(
            "[WARN] DXVK War3[StormHook]: OOM 回退的原生块清理失败 ptr=%p\n",
            nativeNew);
      return nullptr;
    }
    war3dbg::Print("[WARN] DXVK War3[StormHook]: TLSF ReAlloc OOM，回退 Storm "
                   "oldSz=%zu newSz=%zu\n",
                   origSize, newSize);
    return nativeNew;
  }

  if (IsRejectedManagedPointer(oldPtr)) {
    NoteRejectedManagedPointer(oldPtr, "realloc");
    return nullptr;
  }

  // 原生块保持在 Storm：SMemReAlloc 升级到 TLSF 不安全。
  // 原因：调用方（如 TSExplicitList::Resize）假设成功 ReAlloc
  // 要么原地（指针不变） 要么失败（返回
  // nullptr），并在失败时自行重建内部指针链。 若我们把原生块移入
  // TLSF（新地址）并返回非空，调用方会误判为"原地成功"，
  // 导致内部自相对指针依然指向已释放的旧 Storm 地址，产生 use-after-free 崩溃。
  // 原生块永不升级到 TLSF，避免破坏调用方保存的自相对指针。
  hooks::War3HotHookNativeScope nativeTiming(hookTiming);
  return CallOriginalReAlloc(ecx, edx, oldPtr, newSize, name, argList,
                             flags);
}

} // anonymous namespace

// ============================================================================
// 公共接口实现
// ============================================================================

bool StormHook_Install() {
  if (g_installed.load(std::memory_order_acquire))
    return g_installHealthy.load(std::memory_order_acquire);
  bool expectedInstalling = false;
  if (!g_installing.compare_exchange_strong(expectedInstalling, true,
                                             std::memory_order_acq_rel)) {
    while (g_installing.load(std::memory_order_acquire))
      SwitchToThread();
    return g_installed.load(std::memory_order_acquire) &&
           g_installHealthy.load(std::memory_order_acquire);
  }
  ScopedInstallFlag installFlag;
  if (g_installed.load(std::memory_order_acquire))
    return g_installHealthy.load(std::memory_order_acquire);
  if (!TlsfPool_IsInitialized()) {
    war3dbg::Print("DXVK War3[StormHook]: TlsfPool 未初始化，跳过安装\n");
    return false;
  }

  g_largeBlockThreshold.store(kStormNativeLargeBlockThreshold,
                              std::memory_order_release);

  // 生产 DLL 只编入稳定的大块模式；全尺寸接管留在独立实验分支。
  {
    char mode[32] = {};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA(
        "DXVK_WAR3_STORM_TAKEOVER_MODE", mode,
        static_cast<DWORD>(sizeof(mode)));
    const DWORD envError = length == 0 ? GetLastError() : ERROR_SUCCESS;
    const bool present = length != 0 || envError != ERROR_ENVVAR_NOT_FOUND;
    if (present &&
        (length == 0 || length >= sizeof(mode) ||
         (_stricmp(mode, "stable") != 0 && _stricmp(mode, "large") != 0))) {
      war3dbg::Print(
          "DXVK War3[StormHook]: 生产 DLL 仅支持 stable/large；全接管仅供独立内存诊断，拒绝安装\n");
      return false;
    }
  }

  // 只允许把阈值调高做诊断，绝不允许降到小块域。
  {
    char buf[32] = {};
    if (GetEnvironmentVariableA("DXVK_WAR3_STORM_THRESHOLD_KB", buf,
                                sizeof(buf)) > 0) {
      const size_t kb = static_cast<size_t>(atoll(buf));
      if (kb <= std::numeric_limits<size_t>::max() / 1024u &&
          kb * 1024u >= kStormNativeLargeBlockThreshold) {
        const size_t bytes = kb * 1024u;
        g_largeBlockThreshold.store(bytes, std::memory_order_release);
        war3dbg::Print(
            "DXVK War3[StormHook]: 大块阈值诊断覆盖为 %zu 字节\n", bytes);
      }
    }
  }

  HMODULE hStorm = GetModuleHandleA("Storm.dll");
  if (!hStorm) {
    war3dbg::Print("DXVK War3[StormHook]: Storm.dll 未加载\n");
    return false;
  }
  if (IsStandaloneStormBreakerLoaded()) {
    war3dbg::Print(
        "DXVK War3[StormHook]: 当前进程已加载 standalone ASI，保持原生并交由其安装\n");
    return false;
  }

  StormHookBinding bindings[5] = {};
  if (!VerifyStorm127a(hStorm, bindings)) {
    war3dbg::Print(
        "DXVK War3[StormHook]: Storm 1.27a SHA-256/PE/RVA/前导字节校验失败，保持原生\n");
    return false;
  }

  bindings[0].detour = reinterpret_cast<LPVOID>(Hooked_SMemAlloc);
  bindings[0].original = reinterpret_cast<LPVOID *>(&g_origAlloc);
  bindings[1].detour = reinterpret_cast<LPVOID>(Hooked_SMemFree);
  bindings[1].original = reinterpret_cast<LPVOID *>(&g_origFree);
  bindings[2].detour = reinterpret_cast<LPVOID>(Hooked_SMemGetSize);
  bindings[2].original = reinterpret_cast<LPVOID *>(&g_origGetSize);
  bindings[3].detour = reinterpret_cast<LPVOID>(Hooked_SMemReAlloc);
  bindings[3].original = reinterpret_cast<LPVOID *>(&g_origReAlloc);

  StormNativeSmallRepairHookDesc repair = {};
  if (!StormNativeSmallRepair_Prepare(hStorm, &repair))
    return false;

  // 只在全部只读身份/前导字节检查通过后发布兼容信号，避免 standalone
  // 因一次尚未具备安装条件的 DXVK 尝试而提前退出。
  if (!ReserveIntegrationSignal())
    return false;

  // event 只能通知尚未越过 OpenEvent 检查的旧 standalone。发布后再做一次
  // 精确前导字节核验，把检查窗口内已经修改目标的并发安装拒绝掉。
  if (!VerifyStorm127a(hStorm, bindings) ||
      !StormNativeSmallRepair_Prepare(hStorm, &repair)) {
    war3dbg::Print(
        "DXVK War3[StormHook]: 兼容信号发布后目标前导字节发生漂移，拒绝双重 Hook\n");
    ReleaseIntegrationSignal();
    return false;
  }

  size_t bindingCount = 4;
  if (repair.target) {
    bindings[bindingCount].name = "StormHeap_AllocPage/search";
    bindings[bindingCount].target = repair.target;
    bindings[bindingCount].detour = repair.detour;
    bindings[bindingCount].original = repair.originalStorage;
    ++bindingCount;
  }

  g_redirectEnabled.store(false, std::memory_order_release);
  const HookBatchInstallResult installResult =
      InstallStormHookBatch(bindings, bindingCount);
  if (installResult == HookBatchInstallResult::CleanFailure) {
    g_origAlloc = nullptr;
    g_origFree = nullptr;
    g_origGetSize = nullptr;
    g_origReAlloc = nullptr;
    StormNativeSmallRepair_Reset();
    g_redirectEnabled.store(true, std::memory_order_release);
    ReleaseIntegrationSignal();
    return false;
  }
  if (installResult == HookBatchInstallResult::IndeterminateFailure) {
    // 任何可能仍活跃的 detour 都必须保留 trampoline。redirect 关闭后，
    // 四个导出只透传原生路径；installed=true 只用于禁止危险的再次安装。
    g_redirectEnabled.store(false, std::memory_order_release);
    StormNativeSmallRepair_ForcePassThrough();
    g_installHealthy.store(false, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    war3dbg::Print(
        "DXVK War3[StormHook]: Hook 回滚无法证明闭合，进入永久透传保护态\n");
    return false;
  }

  g_installHealthy.store(true, std::memory_order_release);
  g_redirectEnabled.store(true, std::memory_order_release);
  g_installed.store(true, std::memory_order_release);
  war3dbg::Print(
      "DXVK War3[StormHook]: policy=stable-large-four-hook backend=tlsf "
      "threshold=0x%zX nativeSmallRepair=%s fullTakeover=0 hooks=%zu\n",
      g_largeBlockThreshold.load(std::memory_order_relaxed),
      StormNativeSmallRepair_GetModeName(), bindingCount);

  return true;
}

void StormHook_Uninstall() {
  // Release 路线与 StormBreaker v1.3.0 一致：Hook 和 TLSF 永久驻留到进程
  // 退出。运行中伪装成“已卸载”会允许二次安装并清空仍活跃的 trampoline。
}

bool StormHook_IsInstalled() {
  return g_installed.load(std::memory_order_acquire);
}

void StormHook_SetRedirectEnabled(bool enabled) {
  if (enabled && !g_installHealthy.load(std::memory_order_acquire)) {
    war3dbg::Print(
        "DXVK War3[StormHook]: 安装状态不健康，拒绝启用大块 redirect\n");
    return;
  }
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
  if (bytes < kStormNativeLargeBlockThreshold)
    bytes = kStormNativeLargeBlockThreshold;
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
  const auto smallRepair = StormNativeSmallRepair_GetStats();

  war3dbg::Print(
      "=== [StormBreaker 内存报告] ===\n"
      "  生产策略                      : stable-large-four-hook / TLSF / threshold=0x%zX / fullTakeover=0\n"
      "  原生小块修复                  : mode=%s calls=%u promotions=%u rebuilds=%u invalid=%u bypasses=%u\n"
      "  托管陈旧指针拒绝              : %llu\n"
      "  Storm g_TotalAllocatedMemory : %6zu MB  (TLSF 大块不入账，原生大块释放补差额)\n"
      "  TLSF 托管（大块拦截）        : count=%-6zu  size=%6zu MB\n"
      "  TLSF 池容量                  : total=%5zu MB  used=%5zu MB  free=%5zu MB  peak=%5zu MB\n"
      "  TLSF 操作统计                : alloc=%-8zu  free=%-8zu  extend=%-4zu  trim=%-4zu\n"
      "================================\n",
      g_largeBlockThreshold.load(std::memory_order_relaxed),
      StormNativeSmallRepair_GetModeName(), smallRepair.calls,
      smallRepair.promotions, smallRepair.rebuilds,
      smallRepair.invalidArenaSkips, smallRepair.bypasses,
      static_cast<unsigned long long>(
          g_rejectedManagedPointers.load(std::memory_order_relaxed)),
      stormNative >> 20,
      managedCnt, managedSz >> 20,
      ps.totalSize >> 20, ps.usedSize >> 20, ps.freeSize >> 20, ps.peakUsed >> 20,
      ps.allocCount, ps.freeCount, ps.extendCount, ps.trimCount);
}

} // namespace dxvk::war3::memory
