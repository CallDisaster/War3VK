#include "war3_native_capture.h"
#include "war3_hook_lifecycle.h"
#include "war3_hook_install_util.h"
#include "../../d3d9_war3_debug.h"
#include "../tools/war3_async_screenshot.h"
#include "../tools/war3_internal_test_api.h"
#include "../../../util/sha1/sha1.h"
#include "war3_native_frame_sync_owner.h"
#include "war3_native_frame_sync_policy.h"
#include <array>
#include <cstdlib>
namespace dxvk::war3::hooks {
static bool IsReadableRange(const void* ptr, size_t size) {
  const auto address = reinterpret_cast<uintptr_t>(ptr);
  if (!address || !size || address > UINTPTR_MAX - size) return false;
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(ptr, &info, sizeof(info)) != sizeof(info) ||
      info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD)) return false;
  const DWORD p = info.Protect & 0xFFu;
  if (p != PAGE_READONLY && p != PAGE_READWRITE && p != PAGE_WRITECOPY &&
      p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) return false;
  const auto start = reinterpret_cast<uintptr_t>(info.BaseAddress);
  return info.RegionSize <= UINTPTR_MAX - start && address + size <= start + info.RegionSize;
}
// 1.27a E04D input, Game+ED080: snapshots +560 BEFORE E65A0 resets it.
// E65A0's complete disassembly never reads its stack argument. ED080 otherwise
// uses that argument only in (request || capturedPixelRequest). Preserve the
// original function, screenshot snapshot, all pixel reads, EndScene and Present.
using NativeFrameSyncFn = int(__thiscall *)(void*, int32_t);
static NativeFrameSyncFn g_nativeFrameSyncTrampoline = nullptr;
static bool g_nativeFrameSyncCandidate = false;
static thread_local uint32_t t_nativeFrameSyncDepth = 0;
static NativeFrameSyncOwner g_nativeFrameSyncOwner;
static std::atomic<uint32_t> g_syncCalls{0}, g_syncElided{0}, g_syncRetained{0}, g_syncPresents{0};

static bool PersistentNativeFrameSyncEnabled() {
  static const bool enabled = [] {
    const auto* persistent = std::getenv("DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT");
    const auto* elide = std::getenv("DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE");
    return (!persistent || !std::strcmp(persistent,"1")) && (!elide || !std::strcmp(elide,"1"));
  }();
  return enabled;
}
void BeginNativeFrameSyncPresent(const void* swapchain) {
  if (PersistentNativeFrameSyncEnabled())
    g_nativeFrameSyncOwner.begin(reinterpret_cast<uintptr_t>(swapchain), GetCurrentThreadId());
}
void CompleteNativeFrameSyncPresent(const void* swapchain) {
  if (PersistentNativeFrameSyncEnabled())
    g_nativeFrameSyncOwner.complete(reinterpret_cast<uintptr_t>(swapchain), GetCurrentThreadId());
  if (::dxvk::war3::tools::IsInternalTestApiEnabled()) ++g_syncPresents;
}
void RevokeNativeFrameSyncPresent(bool permanent) {
  if (PersistentNativeFrameSyncEnabled()) g_nativeFrameSyncOwner.revoke(permanent);
}
NativeFrameSyncStatus QueryNativeFrameSyncStatus() {
  NativeFrameSyncStatus s;
  if (!::dxvk::war3::tools::IsInternalTestApiEnabled()) return s;
  s.persistent = PersistentNativeFrameSyncEnabled();
  s.installed = g_nativeFrameSyncTrampoline != nullptr;
  s.owner = g_nativeFrameSyncOwner.owns(GetCurrentThreadId(), GetMainLoopThreadId());
  s.calls = g_syncCalls.load(); s.elided = g_syncElided.load();
  s.retained = g_syncRetained.load(); s.presents = g_syncPresents.load();
  LARGE_INTEGER ticks{}, frequency{};
  QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
  s.queryTicks = ticks.QuadPart; s.frequency = frequency.QuadPart;
  return s;
}

static int __fastcall Hook_NativeFrameSync(void* object, void*, int32_t request) {
  const bool nested = t_nativeFrameSyncDepth++ != 0;
  const uintptr_t base = reinterpret_cast<uintptr_t>(object);
  const bool readable = base && base <= UINTPTR_MAX - 0x564u &&
      IsReadableRange(reinterpret_cast<const void*>(base + 0x560u), 4u);
  uint32_t capture = 0;
  if (readable)
    std::memcpy(&capture, reinterpret_cast<const void*>(base + 0x560u), 4u);
  const bool authorizedOwner = PersistentNativeFrameSyncEnabled() &&
      g_nativeFrameSyncOwner.owns(GetCurrentThreadId(), GetMainLoopThreadId());
  const bool elided = CanElideNativeFrameSync(g_nativeFrameSyncCandidate, authorizedOwner,
      readable, nested, request, capture);
  if (::dxvk::war3::tools::IsInternalTestApiEnabled()) {
    ++g_syncCalls;
    if (elided) ++g_syncElided; else ++g_syncRetained;
  }
  const int result = g_nativeFrameSyncTrampoline(object, elided ? 0 : request);
  --t_nativeFrameSyncDepth;
  return result;
}

static void InstallNativeFrameSyncExperiment(uintptr_t gameBase) {
  // No recording-owner route in the release-base integration.
  if (!PersistentNativeFrameSyncEnabled()) return;
  if (g_nativeFrameSyncTrampoline) return;
  const char* candidate = std::getenv("DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE");
  g_nativeFrameSyncCandidate = !candidate || std::strcmp(candidate,"1")==0;
  // Exact whole-function fingerprints. Normalize only the nine HIGHLOW slots
  // independently extracted from E04D's relocation table, in a local copy.
  // No wildcard bytes, live code writes, or unknown-version fallback.
  auto matches = [&](uintptr_t rva, size_t size, const char* digest,
                     const auto& relocations) {
    const void* code = reinterpret_cast<const void*>(gameBase+rva);
    std::array<uint8_t,0x26D> copy{};
    if (size>copy.size() || !IsReadableRange(code,size)) return false;
    std::memcpy(copy.data(),code,size);
    return NormalizeNativeFrameCode(copy.data(),size,uint32_t(gameBase)-0x6F000000u,
        relocations.data(),relocations.size()) &&
        Sha1Hash::compute(copy.data(),size).toString()==digest;
  };
  constexpr std::array<size_t,4> frameRelocations={0x7,0xAA,0xC9,0xF0};
  constexpr std::array<size_t,5> prepareRelocations={0x1D,0x2F,0x3A,0x9C,0xA8};
  const bool identity = gameBase && gameBase<=UINTPTR_MAX-0xED2EDu &&
      matches(0xED080u,0x26Du,"9900e819c11894e87d225417a9f79a262d129793",frameRelocations) &&
      matches(0xE65A0u,0x148u,"61922bf50f949c4b2a1b9e85a228653f97fe8f56",prepareRelocations);
  const bool installed = identity && InstallMinHook(
      reinterpret_cast<LPVOID>(gameBase+0xED080u),
      reinterpret_cast<LPVOID>(&Hook_NativeFrameSync),
      reinterpret_cast<LPVOID*>(&g_nativeFrameSyncTrampoline),
      "Lifecycle","NativeFrameSyncExperiment",false,true);
  war3dbg::Print("DXVK NativeFrameSync: identity=%u installed=%u candidate=%u persistent=%u\n",
      unsigned(identity),unsigned(installed),unsigned(g_nativeFrameSyncCandidate),
      unsigned(PersistentNativeFrameSyncEnabled()));
}

// Exact 1.27a screenshot event callback. A handled event is not a saved-file
// acknowledgement; the worker reports actual file completion separately.
using NativeScreenshotEventFn = int(__thiscall *)(void*);
static NativeScreenshotEventFn g_nativeScreenshotEventTrampoline = nullptr;
static NativeScreenshotEventFn g_nativeScreenshotEventEntry = nullptr;

static int __fastcall Hook_NativeScreenshotEvent(void* event, void*) {
  uint32_t code = 0;
  if (IsReadableRange(event, sizeof(code))) {
    std::memcpy(&code, event, sizeof(code));
    if (code == 0x212u) {
      try {
        if (::dxvk::war3::tools::RequestNativeAsyncScreenshot()) return 1;
      } catch (...) { /* Preserve native behavior if the service is unavailable. */ }
    }
  }
  return g_nativeScreenshotEventTrampoline(event);
}

static void InstallNativeAsyncScreenshot(uintptr_t gameBase) {
  if (!::dxvk::war3::tools::AsyncScreenshotEnabled() || g_nativeScreenshotEventTrampoline) return;
  constexpr std::array<size_t, 2> relocations = {2, 22};
  std::array<uint8_t, 27> code{};
  bool identity = gameBase && gameBase <= UINTPTR_MAX - 0x17554Bu;
  if (identity) {
    const auto* source = reinterpret_cast<const void*>(gameBase + 0x175530u);
    identity = IsReadableRange(source, code.size());
    if (identity) {
      std::memcpy(code.data(), source, code.size());
      identity = NormalizeNativeFrameCode(code.data(), code.size(),
          uint32_t(gameBase) - 0x6F000000u, relocations.data(), relocations.size()) &&
          Sha1Hash::compute(code.data(), code.size()).toString() ==
              "f2a84946771dc3853231d4a4116fb582cbe96853";
    }
  }
  const bool installed = identity && InstallMinHook(
      reinterpret_cast<LPVOID>(gameBase + 0x175530u),
      reinterpret_cast<LPVOID>(&Hook_NativeScreenshotEvent),
      reinterpret_cast<LPVOID*>(&g_nativeScreenshotEventTrampoline),
      "Lifecycle", "NativeAsyncScreenshot", false, true);
  if (installed)
    g_nativeScreenshotEventEntry = reinterpret_cast<NativeScreenshotEventFn>(gameBase + 0x175530u);
  war3dbg::Print("DXVK AsyncScreenshot: identity=%u installed=%u\n",
      unsigned(identity), unsigned(installed));
}

bool InvokeNativeScreenshotEventForTest() {
  if (!::dxvk::war3::tools::IsInternalTestApiEnabled() ||
      !::dxvk::war3::tools::AsyncScreenshotEnabled() || !g_nativeScreenshotEventEntry ||
      !GetMainLoopThreadId() || GetCurrentThreadId() != GetMainLoopThreadId()) return false;
  uint32_t event = 0x212u;
  // Invoke the patched native entry, NOT the trampoline or a simulated saver.
  return g_nativeScreenshotEventEntry(&event) == 1;
}


void InstallNativeCapture(uintptr_t gameBase) {
  InstallNativeFrameSyncExperiment(gameBase);
  InstallNativeAsyncScreenshot(gameBase);
}
} // namespace dxvk::war3::hooks

