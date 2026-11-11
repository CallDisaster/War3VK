// x86 CPU tests of the actual low-address helper and native Storm adapters.
// Build only this TU plus the project's MinHook C sources; the two production
// .cpp files below are deliberately included, not replaced by a test model.
//
// Usage:
//   test.exe --core
//   test.exe --storm <ABSOLUTE Storm.dll> --repair-mode off|search
//            --option plain|debug|protect [--inject-high] [--disabled]
// Native mode verifies the exact file hash BEFORE LoadLibrary. Run only in the
// existing restricted-Medium, non-input desktop/owned Job CPU test harness.
// This executable never loads Game.dll, launches Warcraft, or proves gameplay.
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../src/minhook/include/MinHook.h"
#include "../src/d3d9/war3/memory/war3_storm_low_domain.cpp"
#include "../src/d3d9/war3/memory/war3_storm_native_small_repair.cpp"
#include "../src/d3d9/war3/memory/war3_storm_realloc_contract.h"

namespace m = dxvk::war3::memory;
namespace la = dxvk::war3::memory::lowaddress;
static_assert(sizeof(void*) == 4, "Native Storm tests require the real x86 ABI");
static std::atomic<unsigned> checks{0}, failures{0};
static void Check(bool condition, const char* expression, unsigned line) {
  checks.fetch_add(1, std::memory_order_relaxed);
  if (!condition) {
    failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "FAIL line=%u %s\n", line, expression);
  }
}
#define CHECK(e) Check(bool(e), #e, __LINE__)

// A bounded adversarial OS adapter. It executes the production algorithm, not
// a reimplementation of its range or search decisions.
struct FakeOps {
  uintptr_t initial = 0, succeedsAt = 0x10000, wrongAt = 0, wrongResult = 0;
  la::Region region{0x10000, la::kLimit - 0x10000, true};
  bool queryFails = false, releaseFails = false, allOccupied = false;
  unsigned allocations = 0, releases = 0, queries = 0;
  uint32_t initialFlags = 0, explicitFlags = 0, protection = 0;
  uint64_t requested = 0;
  uintptr_t lastAllocated = 0, lastReleased = 0;

  uintptr_t allocate(uintptr_t at, uint64_t bytes, uint32_t flags, uint32_t protect) noexcept {
    ++allocations; requested = bytes; protection = protect; lastAllocated = at;
    if (!at) { initialFlags = flags; return initial; }
    explicitFlags = flags;
    if (at == wrongAt) return wrongResult;
    return at == succeedsAt ? at : 0;
  }
  bool release(uintptr_t at) noexcept {
    ++releases; lastReleased = at; return !releaseFails;
  }
  bool query(uintptr_t at, la::Region& out) noexcept {
    ++queries;
    if (queryFails) return false;
    if (allOccupied) { out = {at, 1, false}; return true; }
    out = region; return true;
  }
};

static void RangeTests() {
  CHECK(!la::Fits(0, 1)); CHECK(!la::Fits(0xFFFF, 1));
  CHECK(la::Fits(0x10000, 1)); CHECK(!la::Fits(0x10000, 0));
  CHECK(la::Fits(0x7FFF0000, 0x10000));
  CHECK(!la::Fits(0x7FFF0000, 0x10001));
  CHECK(!la::Fits(0x80000000, 1)); CHECK(!la::Fits(UINT64_MAX, 1));
  for (uint64_t bytes : {uint64_t(0), UINT64_MAX, la::kLimit, la::kLimit - 0xFFFF}) {
    FakeOps ops;
    const auto r = la::Allocate(bytes, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(r.invalid && !r.address && !r.attempts && !r.queries);
    CHECK(!ops.allocations && !ops.queries && !ops.releases);
  }
  {
    FakeOps ops; ops.initial = 0x20000;
    const auto r = la::Allocate(0x5784, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(r.address == 0x20000 && r.attempts == 1 && !r.queries && !r.rejectedHigh);
    CHECK(ops.requested == 0x5784 && ops.initialFlags == MEM_RESERVE);
    CHECK(ops.protection == PAGE_NOACCESS && !ops.releases);
  }
  {
    FakeOps ops; ops.initial = 0x90000000;
    const auto r = la::Allocate(0x5784, MEM_COMMIT, PAGE_READWRITE, ops);
    CHECK(r.address == 0x10000 && r.rejectedHigh == 1 && r.attempts == 2);
    CHECK(ops.releases == 1 && ops.lastReleased == 0x90000000);
    CHECK(ops.initialFlags == MEM_COMMIT);
    CHECK(ops.explicitFlags == (MEM_COMMIT | MEM_RESERVE));
    CHECK(ops.protection == PAGE_READWRITE);
  }
  {
    FakeOps ops; ops.initial = 0x7FFFF000;
    const auto r = la::Allocate(0x1001, MEM_RESERVE, PAGE_READWRITE, ops);
    CHECK(r.address == 0x10000 && ops.releases == 1);
  }
  {
    FakeOps ops; ops.initial = 0x90000000; ops.releaseFails = true;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(!r.address && r.releaseFailed && r.rejectedHigh == 1);
    CHECK(ops.allocations == 1 && ops.releases == 1 && !ops.queries);
  }
  {
    FakeOps ops; ops.succeedsAt = 0x30000;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(r.address == 0x30000 && r.attempts == 4 && r.queries == 3);
  }
  {
    FakeOps ops; ops.wrongAt = 0x10000; ops.wrongResult = 0x40000; ops.succeedsAt = 0x20000;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(r.address == 0x20000 && r.rejectedHigh == 1);
    CHECK(ops.releases == 1 && ops.lastReleased == 0x40000);
  }
  {
    FakeOps ops; ops.queryFails = true;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(!r.address && r.attempts == 1 && r.queries == 1);
  }
  {
    FakeOps ops; ops.allOccupied = true;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(!r.address && r.attempts == 1);
    CHECK(r.queries && r.queries <= la::kMaxQueries);
    CHECK(ops.allocations == 1); // No unbounded final NULL fallback.
  }
  {
    FakeOps ops; ops.region = {0x10000, la::kLimit - 0x10000, true}; ops.succeedsAt = 0;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(!r.address && r.queries <= la::kMaxQueries);
    CHECK(r.attempts <= la::kMaxQueries + 1);
    CHECK(ops.initialFlags == MEM_RESERVE && ops.lastAllocated != 0);
  }
  for (const la::Region region : {
      la::Region{0x20000, 0x10000, true}, // Region starts after queried address.
      la::Region{0x10000, 0, true},
      la::Region{0x10000, UINT64_MAX, true},
      la::Region{0, 0x10000, true}}) {
    FakeOps ops; ops.region = region;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(!r.address && r.attempts == 1 && r.queries == 1);
  }
  {
    // Following a tiny region, the aligned candidate is outside the free
    // region. end-candidate must not underflow into an enormous fit.
    struct TinyRegionOps : FakeOps {
      bool query(uintptr_t at, la::Region& out) noexcept {
        ++queries;
        if (queries == 1) { out = {0x10000, 1, false}; return true; }
        if (queries == 2) { out = {0x10001, 0xFFFF, true}; return true; }
        (void)at; return false;
      }
    } ops;
    ops.succeedsAt = 0x20000;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_NOACCESS, ops);
    CHECK(!r.address && r.attempts == 1);
  }
  {
    FakeOps ops; ops.initial = 0x7FFFF000;
    const auto r = la::Allocate(0x1000, MEM_RESERVE, PAGE_READWRITE, ops);
    CHECK(r.address == 0x7FFFF000 && !r.rejectedHigh && !ops.releases);
  }
}

static DWORD WINAPI ScopeThread(void*) {
  CHECK(!m::g_selected);
  for (unsigned i = 0; i < 1000; ++i) {
    m::StormLowDomainScope yes(".?AUMDLDATAHash@@", -2, 0x1E955348u);
    CHECK(m::g_selected);
    {
      m::StormLowDomainScope no("another-domain", -2, 0x1E955348u);
      CHECK(!m::g_selected);
      {
        m::StormLowDomainScope inner(".?AUMDLDATAHash@@", -2, 0x1E955348u);
        CHECK(m::g_selected);
      }
      CHECK(!m::g_selected);
    }
    CHECK(m::g_selected);
  }
  CHECK(!m::g_selected);
  return 0;
}

static void ScopeTests() {
  const bool wasEnabled = m::g_enabled.exchange(true);
  CHECK(!m::g_selected);
  char copied[] = ".?AUMDLDATAHash@@";
  CHECK(m::Selected(copied, -2, 0x1E955348u));
  CHECK(!m::Selected(copied, -1, 0x1E955348u));
  CHECK(!m::Selected(copied, -2, 0x1E955349u));
  CHECK(!m::Selected(nullptr, -2, 0x1E955348u));
  CHECK(!m::Selected(".?AUMDLDATAHash@@extra", -2, 0x1E955348u));
  CHECK(!m::Selected(".?AUMDLDATAHasH@@", -2, 0x1E955348u));
  CHECK(m::StormLowDomain_RequiresNativeAllocation(".?AUMDLDATAHash@@", -2));
  CHECK(m::StormLowDomain_RequiresNativeAllocation(copied, -2));
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(".?AUMDLDATAHash@@extra", -2));
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(copied, -1));
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(copied, 0));
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(nullptr, -2));
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(".?AUMDLDATAHasH@@", -2));
  {
    m::StormLowDomainScope outer(copied, -2, 0x1E955348u);
    CHECK(m::g_selected);
    HANDLE threads[2] = {CreateThread(nullptr, 0, ScopeThread, nullptr, 0, nullptr),
                         CreateThread(nullptr, 0, ScopeThread, nullptr, 0, nullptr)};
    CHECK(threads[0] && threads[1]);
    for (HANDLE thread : threads) if (thread) {
      CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
      DWORD exit = 1; CHECK(GetExitCodeThread(thread, &exit) && exit == 0);
      CloseHandle(thread);
    }
    CHECK(m::g_selected);
    {
      m::StormLowDomainScope absent(static_cast<const m::StormNativeArena*>(nullptr));
      CHECK(!m::g_selected);
    }
    CHECK(m::g_selected);
  }
  CHECK(!m::g_selected);
  alignas(m::StormNativeArena) uint8_t storage[160]{};
  auto* arena = reinterpret_cast<m::StormNativeArena*>(storage);
  arena->heapId = 0x1E955348u; arena->sourceLine = -2;
  std::memcpy(storage + offsetof(m::StormNativeArena, sourceName), copied, sizeof(copied));
  {
    m::StormLowDomainScope copiedArena(arena);
    CHECK(m::g_selected);
  }
  CHECK(!m::g_selected);
  m::g_enabled.store(false);
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(copied, -2));
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(".?AUMDLDATAHash@@", -2));
  {
    m::StormLowDomainScope disabled(copied, -2, 0x1E955348u);
    CHECK(!m::g_selected);
  }
  m::g_enabled.store(wasEnabled);
  const void* old = reinterpret_cast<void*>(0x10000u);
  const void* moved = reinterpret_cast<void*>(0x20000u);
  CHECK(!m::NativeReallocReleasedOldPointer(nullptr, nullptr));
  CHECK(!m::NativeReallocReleasedOldPointer(nullptr, moved));
  CHECK(!m::NativeReallocReleasedOldPointer(old, nullptr));
  CHECK(!m::NativeReallocReleasedOldPointer(old, old));
  CHECK(m::NativeReallocReleasedOldPointer(old, moved));
}

static bool ExactStormHash(HANDLE file) {
  constexpr uint8_t expected[32] = {
    0xF8,0xF5,0x19,0xCF,0xAA,0x62,0x75,0xA5,0x17,0x2A,0x01,0x4F,0x0A,0xBE,0xD2,0x21,
    0x22,0x84,0x39,0x0A,0x33,0xF1,0x19,0x46,0x77,0x15,0x5A,0x7D,0x40,0x8E,0x63,0xEB};
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
  DWORD objectBytes = 0, actual = 0;
  if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &actual, 0) >= 0;
  uint8_t object[1024]{};
  if (ok) ok = objectBytes <= sizeof(object) &&
      BCryptCreateHash(algorithm, &hash, object, objectBytes, nullptr, 0, 0) >= 0;
  LARGE_INTEGER zero{};
  if (ok) ok = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != 0;
  uint8_t buffer[32768], digest[32]{};
  DWORD bytes = 0;
  while (ok) {
    if (!ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr)) { ok = false; break; }
    if (!bytes) break;
    ok = BCryptHashData(hash, buffer, bytes, 0) >= 0;
  }
  if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0 &&
      std::memcmp(digest, expected, sizeof(digest)) == 0;
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

struct NativeApi {
  using Alloc = void* (__fastcall*)(int, int, uint32_t, const char*, int32_t, uint32_t);
  using Free = int (__stdcall*)(void*, const char*, int32_t, uint32_t);
  using Size = int (__stdcall*)(const void*, const char*, int32_t);
  using Realloc = void* (__fastcall*)(int, int, void*, uint32_t, const char*, int32_t, uint32_t);
  using Heap = uint32_t (__stdcall*)(const void*);
  using Option = int (__stdcall*)(uint32_t, uint32_t);
  Alloc alloc = nullptr; Free free = nullptr; Size size = nullptr;
  Realloc realloc = nullptr; Heap heap = nullptr; Option option = nullptr;
  explicit NativeApi(HMODULE module) {
    alloc = reinterpret_cast<Alloc>(GetProcAddress(module, MAKEINTRESOURCEA(401)));
    free = reinterpret_cast<Free>(GetProcAddress(module, MAKEINTRESOURCEA(403)));
    size = reinterpret_cast<Size>(GetProcAddress(module, MAKEINTRESOURCEA(404)));
    realloc = reinterpret_cast<Realloc>(GetProcAddress(module, MAKEINTRESOURCEA(405)));
    heap = reinterpret_cast<Heap>(GetProcAddress(module, MAKEINTRESOURCEA(484)));
    option = reinterpret_cast<Option>(GetProcAddress(module, MAKEINTRESOURCEA(496)));
  }
  bool valid() const { return alloc && free && size && realloc && heap && option; }
};

static m::VirtualAllocFn realAllocate = nullptr;
static unsigned injectionRemaining = 0, injections = 0, injectionMisses = 0;
static LPVOID WINAPI InjectOneHighReservation(LPVOID at, SIZE_T bytes, DWORD flags, DWORD protect) {
  if (!at && (flags & (MEM_RESERVE | MEM_COMMIT)) && injectionRemaining) {
    --injectionRemaining;
    // Explicit test-process-only injection. Do not assume MEM_TOP_DOWN actually
    // yields high VA; reserve a real known high address and verify the result.
    for (uintptr_t candidate = 0x90000000u; candidate < 0xA0000000u; candidate += 0x10000u) {
      void* result = realAllocate(reinterpret_cast<void*>(candidate), bytes, flags | MEM_RESERVE, protect);
      if (result) { ++injections; return result; }
    }
    ++injectionMisses;
  }
  return realAllocate(at, bytes, flags, protect);
}

static bool AllBytes(const void* pointer, size_t bytes, uint8_t expected) {
  const auto* data = static_cast<const uint8_t*>(pointer);
  for (size_t i = 0; i < bytes; ++i) if (data[i] != expected) return false;
  return true;
}

static void CheckNativeAlignment(const void* pointer, uint32_t requested,
                                 uint32_t options, uint32_t pageSize) {
  if (options & 4u) {
    // Storm +2A75E..2A807 puts the last requested byte immediately before a
    // guard page: ptr = VA + (-(requested + 16) & (pageSize - 1)) + 16.
    // Thus a request congruent to 4 mod 8 has a pointer congruent to 4, and
    // odd requested sizes need not even yield a four-byte-aligned pointer.
    CHECK((uintptr_t(pointer) + requested) % pageSize == 0);
  } else {
    CHECK((uintptr_t(pointer) & 7u) == 0);
  }
}

struct ZeroReallocResult {
  bool allocated = false;
  bool resultNull = false;
  bool samePointer = false;
  int liveSize = -1;
};

// Exact Storm F8F519CF... / #405 (+2C8B0), not a C realloc assumption:
// +2B560 skips in-place work for external blocks; +2B5B9 shrinks native-small
// blocks INCLUDING size zero, then returns the old live pointer. Only the
// fallback checks NOMOVE (+2B5F3). Without NOMOVE the fallback allocates a new
// zero-sized block (+2B60B) before freeing the old block (+2B63B).
// +2B3B0 has no zero-size shortcut: allocation failure invokes ExitProcess,
// while success owns a native header even when logical GetSize is zero.
// Run fresh blocks for each case, both before interception (non-target source)
// and through the target after interception. This independently establishes
// native semantics instead of adjusting expectations to the hooked result.
static std::array<ZeroReallocResult, 4> ZeroReallocTests(
    NativeApi& api, const char* source, uint32_t options, bool requireLow,
    const char* phase) {
  std::array<ZeroReallocResult, 4> results{};
  size_t index = 0;
  for (uint32_t oldSize : {4096u, 0x20010u}) {
    const bool external = (options & 4u) != 0 || oldSize > 0xFE7Bu;
    for (uint32_t flags : {0x10u, 0u}) {
      auto& result = results[index++];
      void* old = api.alloc(0, 0, oldSize, source, -2, 8);
      CHECK(old != nullptr);
      if (!old) continue;
      result.allocated = true;
      CHECK(api.size(old, "zero-size-owner-independent", 73) == int(oldSize));
      if (requireLow) CHECK(la::Fits(uintptr_t(old), oldSize));
      std::memset(old, 0x5C, oldSize);
      void* zero = api.realloc(0, 0, old, 0, "zero-size-owner-independent", 75, flags);
      result.resultNull = zero == nullptr;
      result.samePointer = zero == old;
      const bool mustRefuse = external && (flags & 0x10u);
      CHECK(result.resultNull == mustRefuse);
      CHECK(result.samePointer == !external);
      const bool releasedOld = m::NativeReallocReleasedOldPointer(old, zero);
      CHECK(releasedOld == (external && !(flags & 0x10u)));
      if (flags & 0x10u) {
        // NOMOVE refusal preserves the old allocation; successful shrink also
        // preserves its identity. Both require an explicit native Free.
        void* live = zero ? zero : old;
        result.liveSize = api.size(live, "zero-size-owner-independent", 77);
        CHECK(result.liveSize == (mustRefuse ? int(oldSize) : 0));
        if (mustRefuse) CHECK(AllBytes(live, oldSize, 0x5C));
        CHECK(api.free(live, "zero-size-owner-independent", 79, 0) != 0);
      } else if (zero) {
        result.liveSize = api.size(zero, "zero-size-owner-independent", 81);
        CHECK(result.liveSize == 0);
        if (requireLow) CHECK(la::Fits(uintptr_t(zero), 1));
        CHECK(api.free(zero, "zero-size-owner-independent", 83, 0) != 0);
      }
      // A surprising NULL from the moving path is already a failure. Never
      // guess that old survived and double-free it while reporting that error.
      std::printf("ZERO_REALLOC {\"phase\":\"%s\",\"oldSize\":%u,\"flags\":%u,"
          "\"external\":%s,\"resultNull\":%s,\"samePointer\":%s,\"liveSize\":%d}\n",
          phase, oldSize, flags, external ? "true" : "false",
          result.resultNull ? "true" : "false", result.samePointer ? "true" : "false",
          result.liveSize);
    }
  }
  return results;
}

static bool InstallNative(HMODULE storm, const char* repairMode, bool disabled,
                          m::StormNativeSmallRepairHookDesc (&descs)[2]) {
  CHECK(SetEnvironmentVariableA("DXVK_WAR3_MDLDATA_LOW_ADDRESS", disabled ? "0" : "1"));
  CHECK(SetEnvironmentVariableA("DXVK_WAR3_STORM_NATIVE_SMALL_REPAIR", repairMode));
  if (!m::StormLowDomain_Prepare(storm, &descs[0])) { CHECK(false); return false; }
  CHECK(m::StormLowDomain_IsRequested() == !disabled);
  CHECK(!m::StormLowDomain_GetStats().enabled);
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(".?AUMDLDATAHash@@", -2));
  if (!m::StormNativeSmallRepair_Prepare(storm, &descs[1])) { CHECK(false); return false; }
  CHECK(bool(descs[0].target) == !disabled);
  CHECK(bool(descs[1].target) == (!disabled || std::strcmp(repairMode, "off") != 0));
  if (!m::StormLowDomain_PublishImport()) {
    CHECK(false); m::StormLowDomain_RollbackImport(); return false;
  }
  CHECK(!m::StormLowDomain_GetStats().enabled); // Publish is not activation.
  CHECK(!m::StormLowDomain_RequiresNativeAllocation(".?AUMDLDATAHash@@", -2));
  const MH_STATUS status = MH_Initialize();
  CHECK(status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED);
  for (auto& desc : descs) if (desc.target) {
    if (MH_CreateHook(desc.target, desc.detour, desc.originalStorage) != MH_OK) {
      CHECK(false); m::StormLowDomain_RollbackImport(); return false;
    }
  }
  for (auto& desc : descs) if (desc.target) {
    if (MH_QueueEnableHook(desc.target) != MH_OK) {
      CHECK(false); m::StormLowDomain_RollbackImport(); return false;
    }
  }
  if (MH_ApplyQueued() != MH_OK) { CHECK(false); m::StormLowDomain_RollbackImport(); return false; }
  m::StormLowDomain_ActivateAfterBatch();
  CHECK(bool(m::StormLowDomain_GetStats().enabled) == !disabled);
  CHECK(m::StormLowDomain_RequiresNativeAllocation(".?AUMDLDATAHash@@", -2) == !disabled);
  return true;
}

static int NativeTests(const wchar_t* path, const char* repairMode, const char* optionName,
                       bool injectHigh, bool disabled) {
  if (!(path && path[0] && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))) {
    CHECK(false); return 2;
  }
  CHECK(GetModuleHandleW(L"Storm.dll") == nullptr);
  HANDLE lease = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (lease == INVALID_HANDLE_VALUE) { CHECK(false); return 2; }
  if (!ExactStormHash(lease)) { CHECK(false); CloseHandle(lease); return 2; }
  HMODULE storm = LoadLibraryExW(path, nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!storm) { CHECK(false); CloseHandle(lease); return 2; }
  const uintptr_t base = uintptr_t(storm);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(storm);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  CHECK(dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE);
  CHECK(nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386);
  CHECK(nt->FileHeader.TimeDateStamp == 0x56BD0E34 && nt->OptionalHeader.SizeOfImage == 0x61000);
  NativeApi api(storm);
  if (!api.valid()) { CHECK(false); CloseHandle(lease); return 2; }
  CHECK(uintptr_t(api.alloc) == base + 0x2B830);
  CHECK(uintptr_t(api.free) == base + 0x2BE40);
  CHECK(uintptr_t(api.size) == base + 0x2C000);
  CHECK(uintptr_t(api.realloc) == base + 0x2C8B0);
  const uint32_t options = std::strcmp(optionName, "debug") == 0 ? 1u :
                           std::strcmp(optionName, "protect") == 0 ? 4u : 0u;
  // The reference is the exact DLL with no MinHook/IAT changes and a different
  // source domain. It cannot introduce a preexisting target external buffer.
  api.option(options, 15u);
  CHECK(!*reinterpret_cast<const uint32_t*>(base + 0x56F78)); // Shuffle is off.
  const auto zeroReference = ZeroReallocTests(
      api, "warvk-low-domain-zero-reference", options, false, "unhooked-reference");
  SYSTEM_INFO systemInfo{};
  GetSystemInfo(&systemInfo);
  const uint32_t pageSize = systemInfo.dwPageSize;
  CHECK(pageSize >= 4096 && (pageSize & (pageSize - 1)) == 0);
  if (pageSize < 4096 || (pageSize & (pageSize - 1))) { CloseHandle(lease); return 2; }
  std::array<uintptr_t, 3> alignmentReference{};
  for (size_t i = 0; i < alignmentReference.size(); ++i) {
    const uint32_t bytes = 0x5784u + uint32_t(i) * 16u;
    void* reference = api.alloc(0, 0, bytes, "warvk-low-domain-alignment-reference", -2, 8);
    CHECK(reference != nullptr);
    if (!reference) continue;
    CheckNativeAlignment(reference, bytes, options, pageSize);
    alignmentReference[i] = uintptr_t(reference) & 7u;
    CHECK(api.free(reference, "alignment-owner-independent", 27, 0) != 0);
  }
  // The cold gate cannot certify external buffers created before interception,
  // even when their containing native arena is low. Establish only a plain
  // small allocation before installation; enable debug/protect after the gate.
  // This also exercises native freeing of a preexisting plain header while a
  // different option is active, without relabelling it or changing ownership.
  api.option(0, 15u); // Native API, never a write to Storm's globals.
  CHECK(!*reinterpret_cast<const uint32_t*>(base + 0x5536C));
  CHECK(!*reinterpret_cast<const uint32_t*>(base + 0x56F74));
  constexpr char target[] = ".?AUMDLDATAHash@@";
  constexpr char other[] = "warvk-low-domain-negative-control";
  void* before = api.alloc(0, 0, 0x5784, target, -2, 8);
  CHECK(before != nullptr);
  if (!before || !la::Fits(uintptr_t(before), 0x5784)) {
    CHECK(false); CloseHandle(lease); return 2;
  }
  CHECK(api.heap(before) == 0x1E955348u);
  CHECK(api.size(before, other, 29) == 0x5784);
  m::StormNativeSmallRepairHookDesc descs[2]{};
  if (!InstallNative(storm, repairMode, disabled, descs)) { CloseHandle(lease); return 2; }
  const auto installed = m::StormLowDomain_GetStats();
  if (!disabled) CHECK(installed.preexistingArenas >= 1 && !installed.preexistingHigh);
  api.option(options, 15u);
  CHECK(bool(*reinterpret_cast<const uint32_t*>(base + 0x5536C)) == bool(options & 1u));
  CHECK(bool(*reinterpret_cast<const uint32_t*>(base + 0x56F74)) == bool(options & 4u));
  const auto zeroTarget = ZeroReallocTests(api, target, options, !disabled,
      disabled ? "disabled-target" : "hooked-target");
  for (size_t i = 0; i < zeroReference.size(); ++i) {
    CHECK(zeroReference[i].allocated && zeroTarget[i].allocated);
    CHECK(zeroReference[i].resultNull == zeroTarget[i].resultNull);
    CHECK(zeroReference[i].samePointer == zeroTarget[i].samePointer);
    CHECK(zeroReference[i].liveSize == zeroTarget[i].liveSize);
  }
  realAllocate = m::g_alloc;
  if (injectHigh && !disabled) {
    injectionRemaining = 1;
    m::g_alloc = InjectOneHighReservation;
  }
  std::array<void*, 128> blocks{};
  for (size_t i = 0; i < blocks.size(); ++i) {
    const uint32_t requested = 0x5784u + uint32_t(i % 3) * 16u;
    blocks[i] = api.alloc(0, 0, requested, target, -2, 8);
    CHECK(blocks[i] != nullptr);
    if (!blocks[i]) continue;
    CHECK(la::Fits(uintptr_t(blocks[i]), requested));
    CheckNativeAlignment(blocks[i], requested, options, pageSize);
    CHECK((uintptr_t(blocks[i]) & 7u) == alignmentReference[i % 3]);
    CHECK(api.size(blocks[i], other, 37) == int(requested));
    CHECK(api.heap(blocks[i]) == 0x1E955348u);
    CHECK(AllBytes(blocks[i], requested, 0));
    CHECK(*reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(blocks[i]) - 2) == 0x6F6D);
    std::memset(blocks[i], int(i + 1), requested);
  }
  const auto grown = m::StormLowDomain_GetStats();
  if (!disabled) {
    CHECK(grown.reserves > installed.reserves);
    // Protect stores external VA for every user allocation; its tiny native
    // headers can all fit in the already established arena. In that mode a
    // fresh arena is not required to prove interception of those external VAs.
    if (!(options & 4u)) CHECK(grown.createdLow > installed.createdLow);
    CHECK(grown.createCalls == grown.createdLow);
  }
  if (injectHigh && !disabled) {
    CHECK(injections == 1 && !injectionMisses && !injectionRemaining);
    CHECK(grown.highRejected > installed.highRejected);
  }
  // Allocations larger than the native-small threshold still belong to this
  // source. The native external VA path must retain the selected arena scope.
  for (uint32_t bytes : {0xFE7Bu, 0xFE7Cu, 0x10000u, 0x20010u}) {
    void* p = api.alloc(0, 0, bytes, target, -2, 8);
    CHECK(p != nullptr);
    if (p) {
      CHECK(la::Fits(uintptr_t(p), bytes)); CHECK(api.size(p, other, 41) == int(bytes));
      // Includes odd-size FE7B: guard-tail alignment, not a blanket 4/8 rule.
      if (options & 4u) CheckNativeAlignment(p, bytes, options, pageSize);
      CHECK(AllBytes(p, bytes, 0)); CHECK(api.free(p, other, 43, 0) != 0);
    }
  }
  const auto priorNegative = m::StormLowDomain_GetStats();
  for (unsigned i = 0; i < 8; ++i) {
    void* p = api.alloc(0, 0, 0x10000, other, -2, 8);
    CHECK(p != nullptr);
    if (p) { CHECK(api.size(p, target, -2) == 0x10000); CHECK(api.free(p, target, -2, 0) != 0); }
  }
  CHECK(m::StormLowDomain_GetStats().reserves == priorNegative.reserves);
  CHECK(m::StormLowDomain_GetStats().scopes == priorNegative.scopes);
  // Source arguments on Free/GetSize/Realloc need not match allocation source;
  // the original Storm arena and original APIs remain the ownership authority.
  void* resized = api.realloc(0, 0, nullptr, 256, target, -2, 8);
  CHECK(resized != nullptr);
  if (resized) {
    CHECK(api.size(resized, other, 47) == 256 && AllBytes(resized, 256, 0));
    std::memset(resized, 0xA7, 128);
    void* shrunk = api.realloc(0, 0, resized, 128, other, 49, 0x10);
    const bool protectedExternal = (options & 4u) != 0;
    CHECK(protectedExternal ? shrunk == nullptr : shrunk == resized);
    if (shrunk) resized = shrunk;
    const int afterShrinkSize = protectedExternal ? 256 : 128;
    CHECK(api.size(resized, other, 51) == afterShrinkSize);
    CHECK(AllBytes(resized, 128, 0xA7));
    void* noMove = api.realloc(0, 0, resized, 0x20000000u, other, 53, 0x10);
    CHECK(noMove == nullptr);
    CHECK(api.size(resized, other, 55) == afterShrinkSize && AllBytes(resized, 128, 0xA7));
    void* grownBlock = api.realloc(0, 0, resized, 4096, other, 57, 8);
    CHECK(grownBlock != nullptr);
    if (grownBlock) {
      resized = grownBlock;
      CHECK(la::Fits(uintptr_t(resized), 4096));
      CHECK(api.size(resized, other, 59) == 4096);
      CHECK(AllBytes(resized, 128, 0xA7));
      CHECK(AllBytes(static_cast<uint8_t*>(resized) + 128, 4096 - 128, 0));
      CHECK(api.free(resized, other, 65, 0) != 0);
    }
    // Native moving allocation failure is fatal rather than a recoverable
    // standard-realloc failure. If it unexpectedly returns NULL, do not guess
    // old ownership; the CHECK above keeps this run red.
  }
  for (size_t i = 0; i < blocks.size(); ++i) if (blocks[i]) {
    const uint32_t requested = 0x5784u + uint32_t(i % 3) * 16u;
    CHECK(AllBytes(blocks[i], requested, uint8_t(i + 1)));
    CHECK(api.free(blocks[i], other, 67, 0) != 0);
  }
  CHECK(api.free(before, other, 69, 0) != 0);
  for (unsigned i = 0; i < 16; ++i) {
    void* p = api.alloc(0, 0, 0x5784, target, -2, 8);
    CHECK(p != nullptr);
    if (p) { CHECK(la::Fits(uintptr_t(p), 0x5784)); CHECK(api.free(p, other, 71, 0) != 0); }
  }
  m::g_alloc = realAllocate;
  const auto final = m::StormLowDomain_GetStats();
  CHECK(!final.failures && !final.preexistingHigh);
  CHECK(ExactStormHash(lease));
  std::printf("NATIVE {\"enabled\":%u,\"scopes\":%u,\"reserves\":%u,\"highRejected\":%u,"
      "\"searchQueries\":%u,\"failures\":%u,\"preexisting\":%u,\"creates\":%u,"
      "\"createdLow\":%u,\"injections\":%u,\"option\":\"%s\",\"repair\":\"%s\"}\n",
      final.enabled, final.scopes, final.reserves, final.highRejected, final.searchQueries,
      final.failures, final.preexistingArenas, final.createCalls, final.createdLow,
      injections, optionName, repairMode);
  // Native hooks and module remain resident until this CPU process exits, just
  // as in the product. No runtime unhook/unload or cross-process write occurs.
  CloseHandle(lease);
  return failures.load() ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
  RangeTests(); ScopeTests();
  if (failures.load()) return 1;
  // Synthetic scope tests ran before any Storm module or hook existed and all
  // worker threads have joined. Do not label their count as native coverage.
  m::g_scopes.store(0);
  const wchar_t* path = nullptr;
  const char* mode = "off"; const char* option = "plain";
  bool inject = false, disabled = false, core = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::wcscmp(argv[i], L"--core")) core = true;
    else if (!std::wcscmp(argv[i], L"--storm") && i + 1 < argc) path = argv[++i];
    else if (!std::wcscmp(argv[i], L"--inject-high")) inject = true;
    else if (!std::wcscmp(argv[i], L"--disabled")) disabled = true;
    else if (!std::wcscmp(argv[i], L"--repair-mode") && i + 1 < argc) {
      ++i;
      if (!std::wcscmp(argv[i], L"search")) mode = "search";
      else if (std::wcscmp(argv[i], L"off")) { CHECK(false); return 2; }
    } else if (!std::wcscmp(argv[i], L"--option") && i + 1 < argc) {
      ++i;
      if (!std::wcscmp(argv[i], L"debug")) option = "debug";
      else if (!std::wcscmp(argv[i], L"protect")) option = "protect";
      else if (std::wcscmp(argv[i], L"plain")) { CHECK(false); return 2; }
    } else { CHECK(false); return 2; }
  }
  if (core == bool(path) || (disabled && inject)) { CHECK(false); return 2; }
  const int status = path ? NativeTests(path, mode, option, inject, disabled) : 0;
  std::printf("RESULT {\"checks\":%u,\"failures\":%u,\"native\":%s,\"ok\":%s}\n",
      checks.load(), failures.load(), path ? "true" : "false",
      status == 0 && !failures.load() ? "true" : "false");
  return status ? status : failures.load() ? 1 : 0;
}
