#if defined(WARVK_MDLDATA_TRACE_DEV)
#include "war3_mdl_trace.h"
#include "war3_mdl_trace_hooks.h"
#include "../../d3d9_war3_debug.h"
#include <MinHook.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <new>

namespace dxvk::war3::memory {
namespace {
using namespace mdltrace;
std::atomic_flag g_attempted = ATOMIC_FLAG_INIT;
alignas(Core) unsigned char g_coreStorage[sizeof(Core)];
HookState g_state;
Journal* g_journal = nullptr;
// Process-lifetime mapping/handles/trampolines. No in-process detach can free
// storage while a native callback still uses it. OS teardown owns final close.
HANDLE g_file = INVALID_HANDLE_VALUE, g_mapping = nullptr;

bool ReadWords(uint32_t address, void* out, uint32_t bytes) {
  SIZE_T read = 0;
  return address && bytes && address <= UINT32_MAX-bytes &&
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                      out, bytes, &read) && read == bytes;
}
uint32_t Thread() { return GetCurrentThreadId(); }

bool HashModule(HMODULE module, const std::array<uint8_t,32>& expected) {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  // Refuse concurrent file writes/deletion while the exact hash is computed.
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::array<uint8_t,32> digest{};
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
      nullptr, 0) >= 0 && BCryptCreateHash(algorithm, &hash, nullptr, 0,
      nullptr, 0, 0) >= 0;
  uint8_t buffer[65536];
  while (ok) {
    DWORD bytes = 0;
    if (!ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr)) { ok = false; break; }
    if (!bytes) break;
    ok = BCryptHashData(hash, buffer, bytes, 0) >= 0;
  }
  if (ok) ok = BCryptFinishHash(hash, digest.data(), DWORD(digest.size()), 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  CloseHandle(file);
  return ok && digest == expected;
}

bool PeIdentity(uint32_t base, uint32_t timestamp, uint32_t size) {
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS32 nt{};
  if (!ReadWords(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < 0 || uint32_t(dos.e_lfanew) > 4096 ||
      base > UINT32_MAX-uint32_t(dos.e_lfanew)) return false;
  return ReadWords(base+uint32_t(dos.e_lfanew), &nt, sizeof(nt)) &&
    nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
    nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
    nt.FileHeader.TimeDateStamp == timestamp && nt.OptionalHeader.SizeOfImage == size;
}

struct Target { uint32_t rva; void* detour; void** original; const uint8_t* bytes; uint32_t count; };
const uint8_t kBucket[] = {0x55,0x8b,0xec,0x8b,0x45,0x10};
const uint8_t kInsert[] = {0x55,0x8b,0xec,0x53,0x56,0x8b,0xf1,0x57};
const uint8_t kUnlink[] = {0x55,0x8b,0xec,0x56,0x8b,0x75,0x08};

bool Exact(uint32_t address, const uint8_t* expected, uint32_t count) {
  uint8_t bytes[16] = {};
  return count <= sizeof(bytes) && ReadWords(address, bytes, count) &&
    std::memcmp(bytes, expected, count) == 0;
}
bool Import(uint32_t base, uint32_t thunk, uint32_t iat, HMODULE storm, uint16_t ordinal) {
  uint8_t bytes[6] = {0xff,0x25};
  const uint32_t relocated = base+iat;
  std::memcpy(bytes+2, &relocated, 4);
  uint32_t function = 0;
  return Exact(base+thunk, bytes, 6) && ReadWords(base+iat, &function, 4) &&
    function == uintptr_t(GetProcAddress(storm, MAKEINTRESOURCEA(ordinal)));
}

bool OpenJournal() {
  wchar_t path[MAX_PATH] = {};
  const DWORD count = GetEnvironmentVariableW(L"DXVK_WAR3_MDLDATA_TRACE_FILE", path, MAX_PATH);
  // Explicit fresh local artifact path supplied only by the isolated harness.
  // Never truncate an existing artifact and never choose a player directory.
  if (count < 4 || count >= MAX_PATH || path[1] != L':' ||
      (path[2] != L'\\' && path[2] != L'/')) return false;
  g_file = CreateFileW(path, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) return false;
  g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READWRITE, 0, sizeof(Journal), nullptr);
  if (!g_mapping) { CloseHandle(g_file); g_file=INVALID_HANDLE_VALUE; return false; }
  void* mapped = MapViewOfFile(g_mapping, FILE_MAP_WRITE, 0, 0, sizeof(Journal));
  if (!mapped) { CloseHandle(g_mapping); CloseHandle(g_file); return false; }
  g_journal = new(mapped) Journal{};
  auto& h = g_journal->header.words;
  h[0]=0x4D444C54; h[1]=1; h[2]=64; h[3]=32; h[4]=16384;
  h[6]=GetCurrentProcessId();
  FILETIME created{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user)) return false;
  h[7]=created.dwLowDateTime; h[8]=created.dwHighDateTime;
  h[22]=15; // Preinstall, Game-thunk-only, other unlink, native-route gaps.
  return true;
}
}

void InstallMdlAllocationTrace(uintptr_t gameBase) {
  static_assert(sizeof(void*)==4, "MDLDATA probe is x86-only");
  wchar_t enabled[4] = {};
  if (GetEnvironmentVariableW(L"DXVK_WAR3_MDLDATA_TRACE", enabled, 4) != 1 ||
      enabled[0] != L'1') return;
  if (g_attempted.test_and_set(std::memory_order_acq_rel)) return;
  if (!OpenJournal()) {
    war3dbg::Print("DXVK MDLDATA trace: output unavailable; no hooks installed\n");
    return;
  }
  auto& h = g_journal->header.words;
  HMODULE storm = GetModuleHandleW(L"Storm.dll");
  const uint32_t base = uint32_t(gameBase), sb = uint32_t(uintptr_t(storm));
  h[9]=base; h[10]=sb; h[23]=0x56BD0E1C; h[24]=0xCD8000;
  h[25]=0x56BD0E34; h[26]=0x61000;
  constexpr std::array<uint8_t,32> gameHash = {0xE0,0x4D,0x17,0x16,0x60,0x3C,0x07,0x5E,
    0xB0,0xC8,0xE1,0xE2,0x1C,0xF1,0x09,0x3A,0x66,0x4A,0xDC,0x52,0x49,0xEF,0xAB,0x39,
    0x6B,0xFA,0x08,0xD7,0xB0,0x9D,0x0C,0x3A};
  constexpr std::array<uint8_t,32> stormHash = {0xF8,0xF5,0x19,0xCF,0xAA,0x62,0x75,0xA5,
    0x17,0x2A,0x01,0x4F,0x0A,0xBE,0xD2,0x21,0x22,0x84,0x39,0x0A,0x33,0xF1,0x19,0x46,
    0x77,0x15,0x5A,0x7D,0x40,0x8E,0x63,0xEB};
  Target targets[] = {
    {0x120628, reinterpret_cast<void*>(Hooks::alloc), reinterpret_cast<void**>(&Hooks::originalAlloc), nullptr,0},
    {0x1205CE, reinterpret_cast<void*>(Hooks::free), reinterpret_cast<void**>(&Hooks::originalFree), nullptr,0},
    {0x12062E, reinterpret_cast<void*>(Hooks::realloc), reinterpret_cast<void**>(&Hooks::originalRealloc), nullptr,0},
    {0x128EC0, reinterpret_cast<void*>(Hooks::bucket), reinterpret_cast<void**>(&Hooks::originalBucket), kBucket,sizeof(kBucket)},
    {0x129240, reinterpret_cast<void*>(Hooks::insert), reinterpret_cast<void**>(&Hooks::originalInsert), kInsert,sizeof(kInsert)},
    {0x12D910, reinterpret_cast<void*>(Hooks::unlink), reinterpret_cast<void**>(&Hooks::originalUnlink), kUnlink,sizeof(kUnlink)},
  };
  bool exact = base <= UINT32_MAX-0xCD8000 && sb &&
    PeIdentity(base,0x56BD0E1C,0xCD8000) && PeIdentity(sb,0x56BD0E34,0x61000) &&
    HashModule(reinterpret_cast<HMODULE>(base),gameHash) && HashModule(storm,stormHash) &&
    Import(base,0x120628,0x94E678,storm,401) && Import(base,0x1205CE,0x94E5C8,storm,403) &&
    Import(base,0x12062E,0x94E67C,storm,405);
  for (const auto& t : targets) if (t.count) exact &= Exact(base+t.rva,t.bytes,t.count);
  if (!exact) {
    h[11].store(3,std::memory_order_release);
    war3dbg::Print("DXVK MDLDATA trace: exact profile rejected; no hooks installed\n");
    return;
  }
  g_state = {new(g_coreStorage) Core(*g_journal),base,0xCD8000,ReadWords,Thread};
  Hooks::state = &g_state;
  h[11].store(1,std::memory_order_release);
  unsigned created = 0, enabledCount = 0;
  for (; created != 6; ++created) {
    const auto& t=targets[created];
    if (MH_CreateHook(reinterpret_cast<void*>(base+t.rva), t.detour,t.original) != MH_OK) break;
  }
  // A create-only trampoline must not be treated as a live-byte freeze. Check
  // target entries again before enabling our precise target list.
  bool stillExact = created == 6 &&
    Import(base,0x120628,0x94E678,storm,401) && Import(base,0x1205CE,0x94E5C8,storm,403) &&
    Import(base,0x12062E,0x94E67C,storm,405);
  for (const auto& t : targets) if (t.count) stillExact &= Exact(base+t.rva,t.bytes,t.count);
  if (stillExact) for (; enabledCount != 6; ++enabledCount) {
    if (MH_EnableHook(reinterpret_cast<void*>(base+targets[enabledCount].rva)) != MH_OK) break;
    h[12].store((1u << (enabledCount+1))-1u,std::memory_order_release);
  }
  if (enabledCount == 6) {
    h[11].store(2,std::memory_order_release);
    war3dbg::Print("DXVK MDLDATA trace: six Game-boundary hooks installed; nativeRoute=unknown allLifetimeCertified=0\n");
    return;
  }
  bool rolledBack = true;
  uint32_t active=0;
  // Only our known targets, never MH_ALL_HOOKS. Retain all trampolines even if
  // rollback fails; deleting a possibly executing trampoline is not safe.
  for (unsigned i=0; i<created; ++i) {
    const auto status=MH_DisableHook(reinterpret_cast<void*>(base+targets[i].rva));
    if (status!=MH_OK && status!=MH_ERROR_DISABLED) { rolledBack=false; active|=1u<<i; }
  }
  h[12].store(active,std::memory_order_release);
  h[11].store(rolledBack?4:5,std::memory_order_release);
  war3dbg::Print("DXVK MDLDATA trace: installation failed rollback=%u; no allocator policy changed\n",rolledBack?1:0);
}
}
#endif
