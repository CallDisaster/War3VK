#include "war3_storm_low_domain.h"
#include "war3_low_address_range.h"
#include <atomic>
#include <cstring>
#include <intrin.h>
#if defined(_MSC_VER)
#define WARVK_LOW_RETURN_ADDRESS() _ReturnAddress()
#else
#define WARVK_LOW_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace dxvk::war3::memory {
namespace {
constexpr char kSource[] = ".?AUMDLDATAHash@@";
constexpr uint32_t kHeap = 0x1E955348u;
using CreateFn = StormNativeArena* (__fastcall*)(const char*, int32_t, uint32_t,
                                               uint32_t, uint32_t, uint32_t, uint32_t);
using VirtualAllocFn = LPVOID (WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
CreateFn g_create = nullptr;
VirtualAllocFn g_alloc = nullptr;
void* volatile* g_import = nullptr;
std::atomic<bool> g_enabled{false};
bool g_requested = false;
thread_local bool g_selected = false;
std::atomic<uint32_t> g_scopes{0}, g_reserves{0}, g_high{0}, g_queries{0}, g_failures{0};
std::atomic<uint32_t> g_creates{0}, g_createdLow{0};
uint32_t g_preexisting = 0, g_preexistingHigh = 0;
uint32_t g_preexistingExternal = 0;
uintptr_t g_stormBase = 0;
CRITICAL_SECTION* g_installLock = nullptr;
bool g_installLockHeld = false;
DWORD g_originalImportProtection = 0;
bool g_haveImportProtection = false;

void ReleaseInstallLock() {
  if (g_installLockHeld) {
    g_installLockHeld = false;
    LeaveCriticalSection(g_installLock);
  }
}

bool Selected(const char* source, int32_t line, uint32_t heap) noexcept {
  // Storm hashes the STRING+line, not its address. Arena growth passes its own
  // copied name; pointer-only matching would silently let later arenas escape.
  return heap == kHeap && line == -2 && source &&
         std::strncmp(source, kSource, sizeof(kSource)) == 0;
}

struct WinOps {
  uintptr_t allocate(uintptr_t at, uint64_t bytes, uint32_t flags, uint32_t protect) noexcept {
    return uintptr_t(g_alloc(reinterpret_cast<void*>(at), SIZE_T(bytes), flags, protect));
  }
  bool release(uintptr_t at) noexcept { return VirtualFree(reinterpret_cast<void*>(at), 0, MEM_RELEASE); }
  bool query(uintptr_t at, lowaddress::Region& r) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) != sizeof(info)) return false;
    r = {uintptr_t(info.BaseAddress), info.RegionSize, info.State == MEM_FREE};
    return true;
  }
};

LPVOID WINAPI LowVirtualAlloc(LPVOID at, SIZE_T bytes, DWORD flags, DWORD protect) {
  // Only Storm's own IAT is redirected. No kernel32 hook or global allocation
  // policy. Preserve fixed-address commits and every non-target native heap.
  const uintptr_t caller = uintptr_t(WARVK_LOW_RETURN_ADDRESS()) - g_stormBase;
  if (!g_selected || !g_enabled.load(std::memory_order_relaxed) || at ||
      (caller!=0x2A38E && caller!=0x2A7A9 && caller!=0x2A7CA) ||
      !(flags & (MEM_RESERVE|MEM_COMMIT)))
    return g_alloc(at, bytes, flags, protect);
  const DWORD before = GetLastError();
  g_reserves.fetch_add(1, std::memory_order_relaxed);
  WinOps ops;
  const auto result = lowaddress::Allocate(bytes, flags, protect, ops);
  g_high.fetch_add(result.rejectedHigh, std::memory_order_relaxed);
  g_queries.fetch_add(result.queries, std::memory_order_relaxed);
  if (!result.address) g_failures.fetch_add(1, std::memory_order_relaxed);
  // Success does not manufacture an error from a rejected high trial/query.
  SetLastError(result.address ? before : ERROR_NOT_ENOUGH_MEMORY);
  return reinterpret_cast<void*>(result.address);
}

StormNativeArena* __fastcall Create(const char* name, int32_t line, uint32_t heap,
    uint32_t bucket, uint32_t granularity, uint32_t committed, uint32_t reserved) {
  StormLowDomainScope scope(name, line, heap);
  const bool selected = g_selected;
  if (selected) g_creates.fetch_add(1, std::memory_order_relaxed);
  auto* result = g_create(name, line, heap, bucket, granularity, committed, reserved);
  if (selected && result && lowaddress::Fits(uintptr_t(result), reserved))
    g_createdLow.fetch_add(1, std::memory_order_relaxed);
  return result;
}

bool Read(uintptr_t address, void* out, size_t bytes) {
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, bytes, &read) && read == bytes;
}

bool Preexisting(uintptr_t base) {
  // Cold install gate only. Existing high arenas cannot be repaired by changing
  // future reservations; never relocate or relabel live native allocations.
  uintptr_t cursor = 0;
  if (!Read(base+0x56F80+(kHeap&255)*4, &cursor, 4)) return false;
  for (uint32_t visited=0; cursor && visited<65536; ++visited) {
    uint32_t fields[27]{};
    if (!Read(cursor, fields, sizeof(fields))) return false;
    if (fields[1] == kHeap) {
      char source[sizeof(kSource)]{};
      if (!Read(cursor+108, source, sizeof(source)) ||
          !Selected(source, int32_t(fields[26]), fields[1])) return false;
      ++g_preexisting;
      if (!lowaddress::Fits(cursor, fields[12])) ++g_preexistingHigh;
      // A low arena can own separate high external user blocks. Their ranges
      // are not proven by the arena range; do not silently certify this state.
      if (fields[13]) ++g_preexistingExternal;
    }
    if (fields[0] == cursor) return false;
    cursor = fields[0];
  }
  return !cursor && !g_preexistingHigh && !g_preexistingExternal;
}
} // namespace

StormLowDomainScope::StormLowDomainScope(const StormNativeArena* arena) noexcept
  : previous_(g_selected) {
  g_selected = g_enabled.load(std::memory_order_relaxed) && arena &&
    Selected(arena->sourceName, arena->sourceLine, arena->heapId);
  if (g_selected) g_scopes.fetch_add(1, std::memory_order_relaxed);
}
StormLowDomainScope::StormLowDomainScope(const char* name, int32_t line, uint32_t heap) noexcept
  : previous_(g_selected) {
  g_selected = g_enabled.load(std::memory_order_relaxed) && Selected(name, line, heap);
  if (g_selected) g_scopes.fetch_add(1, std::memory_order_relaxed);
}
StormLowDomainScope::~StormLowDomainScope() { g_selected = previous_; }

bool StormLowDomain_Prepare(HMODULE storm, StormNativeSmallRepairHookDesc* desc) {
  if (!storm || !desc || sizeof(void*)!=4 || g_enabled.load()) return false;
  *desc = {};
  char value[4]{};
  const DWORD n = GetEnvironmentVariableA("DXVK_WAR3_MDLDATA_LOW_ADDRESS", value, sizeof(value));
  if (n && (n!=1 || (value[0]!='0' && value[0]!='1'))) return false;
  g_requested = !n || value[0]!='0';
  if (!g_requested) return true;
  const uintptr_t base = uintptr_t(storm);
  g_stormBase = base;
  const uintptr_t game = uintptr_t(GetModuleHandleW(L"Game.dll"));
  if (game && !lowaddress::Fits(game, 0xCD8000)) return false;
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  if (system.dwPageSize!=lowaddress::kPage || system.dwAllocationGranularity!=lowaddress::kGranularity)
    return false;
  constexpr uint8_t prolog[] = {0x55,0x8B,0xEC,0x83,0xEC,0x08,0x8B,0x45,0x08};
  uint8_t actual[sizeof(prolog)]{};
  if (!Read(base+0x2A350,actual,sizeof(actual)) || std::memcmp(actual,prolog,sizeof(actual))) return false;
  // The caller verifies SHA/PE. Also validate the relocated import call and its
  // current owner before changing this one module-local slot.
  uint8_t call[6]{};
  uint32_t operand=0;
  if (!Read(base+0x2A388,call,6)) return false;
  std::memcpy(&operand,call+2,4);
  if (call[0]!=0xFF || call[1]!=0x15 || operand!=base+0x410E0) return false;
  g_import = reinterpret_cast<void* volatile*>(base+0x410E0);
  void* imported = nullptr;
  if (!Read(base+0x410E0,&imported,4) || imported != reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualAlloc"))) return false;
  g_alloc = reinterpret_cast<VirtualAllocFn>(imported);
  // Validate the domain against Storm's own content hash (ordinal 502).
  using HashFn = uint32_t (__stdcall*)(const char*, uint32_t, int32_t);
  const auto hash = reinterpret_cast<HashFn>(GetProcAddress(storm, MAKEINTRESOURCEA(502)));
  if (!hash || (hash(kSource,1,-2)&0x7FFFFFFF)!=kHeap) return false;
  g_create = reinterpret_cast<CreateFn>(base+0x2A350);
  *desc = {reinterpret_cast<void*>(base+0x2A350),reinterpret_cast<void*>(Create),reinterpret_cast<void**>(&g_create)};
  return true;
}

bool StormLowDomain_PublishImport() {
  if (!g_requested) return true;
  if (!g_import || !g_alloc) return false;
  uint32_t initialized = 0;
  if (!Read(g_stormBase+0x56F7C,&initialized,4) || !initialized) return false;
  g_installLock = reinterpret_cast<CRITICAL_SECTION*>(g_stormBase+0x55770+(kHeap&255)*24);
  // Cold installer only: never wait for a native owner. Keep the exact bucket
  // owned from this final preflight through the atomic MinHook batch and
  // policy publication; no other target arena can appear in the check gap.
  // MinHook uses Win32 Heap/VirtualAlloc, not Storm, and this owner can reenter
  // its own native bucket. No native CS is acquired by the reservation wrapper.
  if (!TryEnterCriticalSection(g_installLock)) return false;
  g_installLockHeld = true;
  g_preexisting = g_preexistingHigh = g_preexistingExternal = 0;
  if (!Preexisting(g_stormBase)) { ReleaseInstallLock(); return false; }
  DWORD prior=0, ignored=0;
  if (!VirtualProtect(const_cast<void**>(g_import),sizeof(void*),PAGE_READWRITE,&prior)) return false;
  g_originalImportProtection = prior;
  g_haveImportProtection = true;
  void* old = InterlockedCompareExchangePointer(g_import,reinterpret_cast<void*>(LowVirtualAlloc),reinterpret_cast<void*>(g_alloc));
  const bool protectedBack = VirtualProtect(const_cast<void**>(g_import),sizeof(void*),prior,&ignored)!=0;
  if (old != reinterpret_cast<void*>(g_alloc) || !protectedBack) return false;
  return true;
}
void StormLowDomain_ActivateAfterBatch() {
  g_enabled.store(g_requested,std::memory_order_release);
  ReleaseInstallLock();
}

bool StormLowDomain_RollbackImport() {
  g_enabled.store(false,std::memory_order_release);
  struct Release { ~Release() { ReleaseInstallLock(); } } release;
  if (!g_requested || !g_import) return true;
  DWORD prior=0, ignored=0;
  if (!VirtualProtect(const_cast<void**>(g_import),sizeof(void*),PAGE_READWRITE,&prior)) return false;
  void* old = InterlockedCompareExchangePointer(g_import,reinterpret_cast<void*>(g_alloc),reinterpret_cast<void*>(LowVirtualAlloc));
  const DWORD restoreProtection = g_haveImportProtection ? g_originalImportProtection : prior;
  const bool restored=VirtualProtect(const_cast<void**>(g_import),sizeof(void*),restoreProtection,&ignored)!=0;
  return restored && (old==reinterpret_cast<void*>(LowVirtualAlloc) || old==reinterpret_cast<void*>(g_alloc));
}
bool StormLowDomain_IsRequested() { return g_requested; }
void StormLowDomain_Disable() {
  g_enabled.store(false,std::memory_order_release);
  g_requested = false;
  ReleaseInstallLock();
}
bool StormLowDomain_RequiresNativeAllocation(const char* source, int32_t line) noexcept {
  return g_enabled.load(std::memory_order_relaxed) && Selected(source, line, kHeap);
}
StormLowDomainStats StormLowDomain_GetStats() {
  return {uint32_t(g_enabled.load()),g_scopes.load(),g_reserves.load(),g_high.load(),g_queries.load(),g_failures.load(),
    g_preexisting,g_preexistingHigh,g_creates.load(),g_createdLow.load(),g_preexistingExternal};
}
} // namespace dxvk::war3::memory
