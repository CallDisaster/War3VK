#if defined(WARVK_STARTUP_WITNESS_DEV)
#include "war3_startup_witness_core.h"
#include <windows.h>
#include <new>

namespace dxvk::war3::diagnostics {
namespace {
using namespace startupwitness;
std::atomic_flag g_attempted = ATOMIC_FLAG_INIT;
std::atomic<Core*> g_core{nullptr};
alignas(Core) unsigned char g_coreStorage[sizeof(Core)];
// Successful resources intentionally live until process exit: no detach-time
// close/unmap may race an in-flight initialization/pipe-thread callback.
HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_mapping = nullptr;
Journal* g_journal = nullptr;

struct PreserveLastError {
  DWORD saved = GetLastError();
  ~PreserveLastError() { SetLastError(saved); }
};

Sample sample(Core& core, uint32_t detail, uint32_t extra) noexcept {
  LARGE_INTEGER ticks{};
  if (!QueryPerformanceCounter(&ticks)) {
    ticks.QuadPart = 0;
    core.clockFailure();
  }
  return {GetCurrentThreadId(), uint64_t(ticks.QuadPart), detail, extra};
}
} // namespace

void InitializeStartupWitness() noexcept {
  PreserveLastError preserve;
  if (g_attempted.test_and_set(std::memory_order_acq_rel)) return;
  wchar_t enabled[4]{};
  if (GetEnvironmentVariableW(L"DXVK_WAR3_STARTUP_WITNESS", enabled, 4) != 1 ||
      enabled[0] != L'1') return;
  wchar_t path[1024]{};
  const DWORD length = GetEnvironmentVariableW(
    L"DXVK_WAR3_STARTUP_WITNESS_FILE", path, 1024);
  if (length < 4 || length >= 1024 ||
      !((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) ||
      path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
      !(created.dwLowDateTime || created.dwHighDateTime)) return;
  LARGE_INTEGER frequency{};
  const bool frequencyKnown = QueryPerformanceFrequency(&frequency) != FALSE &&
                              frequency.QuadPart > 0;
  if (!frequencyKnown) frequency.QuadPart = 0;
  g_file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) return;
  g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READWRITE, 0,
                                sizeof(Journal), nullptr);
  if (!g_mapping) {
    CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; return;
  }
  void* mapped = MapViewOfFile(g_mapping, FILE_MAP_WRITE, 0, 0, sizeof(Journal));
  if (!mapped) {
    CloseHandle(g_mapping); g_mapping = nullptr;
    CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; return;
  }
  // Once-only setup, outside the event cell and outside DllMain. Never expose
  // the pointer before the entire zeroed journal and owner header are ready.
  g_journal = new(mapped) Journal{};
  auto& h = g_journal->header.words;
  h[0] = kMagic; h[1] = kVersion; h[2] = kHeaderWords;
  h[3] = kEventWords; h[4] = kCapacity;
  h[6] = GetCurrentProcessId(); h[7] = created.dwLowDateTime;
  h[8] = created.dwHighDateTime;
  h[15] = frequencyKnown ? 0u : 1u;
  h[16] = uint32_t(uint64_t(frequency.QuadPart));
  h[17] = uint32_t(uint64_t(frequency.QuadPart) >> 32);
  Core* core = new(g_coreStorage) Core(*g_journal);
  h[9].store(1, std::memory_order_release);
  g_core.store(core, std::memory_order_release);
}

uint32_t BeginStartupStage(StartupStage stage, uint32_t detail,
                          uint32_t extra) noexcept {
  Core* core = g_core.load(std::memory_order_acquire);
  if (!core) return 0;
  PreserveLastError preserve;
  const Sample values = sample(*core, detail, extra);
  return core->begin(stage, values);
}
void EndStartupStage(StartupStage stage, uint32_t token,
                     uint32_t detail, uint32_t extra) noexcept {
  if (!token) return;
  Core* core = g_core.load(std::memory_order_acquire);
  if (!core) return;
  PreserveLastError preserve;
  const Sample values = sample(*core, detail, extra);
  static_cast<void>(core->end(stage, token, values));
}
void NoteStartupStage(StartupStage stage, uint32_t detail,
                      uint32_t extra) noexcept {
  Core* core = g_core.load(std::memory_order_acquire);
  if (!core) return;
  PreserveLastError preserve;
  const Sample values = sample(*core, detail, extra);
  static_cast<void>(core->note(stage, values));
}
} // namespace dxvk::war3::diagnostics
#endif
