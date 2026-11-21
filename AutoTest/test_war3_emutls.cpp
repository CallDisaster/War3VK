// CPU test of the product DLL's emulated-TLS runtime
// (src/d3d9/war3/memory/war3_emutls.cpp; built and run by run_war3_emutls_test.py).
//
// One source, four binaries (all i686 llvm-mingw, -femulated-tls):
//   payload.dll     WAR3_EMUTLS_PAYLOAD + war3_emutls.cpp: many thread_locals
//                   (templates, zero fill, 64/4096 alignment, a 4 MiB array, an
//                   object with a destructor); DllMain disables thread calls as
//                   d3d9_main.cpp does.
//   payload_rt.dll  the same without war3_emutls.cpp: compiler-rt's runtime,
//                   the control for the exit scenario.
//   late.dll        WAR3_EMUTLS_LATE: at its own DLL_PROCESS_DETACH, which comes
//                   after the payload's, it touches the payload's thread_locals
//                   (the game: Game.dll/Storm.dll teardown entering our hooks).
//   emutls_test.exe the checks below; "--exit <late> <payload>" is the exit
//                   scenario, "--exit-fresh" the same with the payload's TLS
//                   first touched during the exit (index and storage made then).
#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(WAR3_EMUTLS_PAYLOAD)
#include "d3d9/war3/memory/war3_emutls.h"

namespace {

struct Templated {
  int a = 7;
  double b = 2.5;
  char name[8] = "emutls";
};
struct alignas(64) Line {
  uint32_t words[16] = {1, 2, 3};
};
struct alignas(4096) Page {
  uint8_t bytes[4096];
};

LONG g_goodDestructors = 0;
LONG g_badDestructors = 0;
struct WithDestructor {
  uint32_t canary = 0x5EED1234u;
  std::vector<int> values;
  ~WithDestructor() {
    InterlockedIncrement(canary == 0x5EED1234u ? &g_goodDestructors : &g_badDestructors);
  }
};

thread_local Templated t_templated;
thread_local Line t_line;
thread_local Page t_page;
thread_local uint8_t t_big[4u << 20];
thread_local uint32_t t_zero[1024];
thread_local WithDestructor t_withDestructor;

// 300 small variables with non-zero templates: the slot array grows.
#define V10(n) thread_local int t_v##n##0 = n * 10 + 1, t_v##n##1 = n * 10 + 2, \
  t_v##n##2 = n * 10 + 3, t_v##n##3 = n * 10 + 4, t_v##n##4 = n * 10 + 5,     \
  t_v##n##5 = n * 10 + 6, t_v##n##6 = n * 10 + 7, t_v##n##7 = n * 10 + 8,     \
  t_v##n##8 = n * 10 + 9, t_v##n##9 = n * 10 + 10;
V10(0) V10(1) V10(2) V10(3) V10(4) V10(5) V10(6) V10(7) V10(8) V10(9)
V10(10) V10(11) V10(12) V10(13) V10(14) V10(15) V10(16) V10(17) V10(18) V10(19)
V10(20) V10(21) V10(22) V10(23) V10(24) V10(25) V10(26) V10(27) V10(28) V10(29)
#undef V10

#define R10(n) &t_v##n##0, &t_v##n##1, &t_v##n##2, &t_v##n##3, &t_v##n##4, \
  &t_v##n##5, &t_v##n##6, &t_v##n##7, &t_v##n##8, &t_v##n##9,
int *Small(int i) {
  int *all[300] = {R10(0) R10(1) R10(2) R10(3) R10(4) R10(5) R10(6) R10(7) R10(8) R10(9)
                   R10(10) R10(11) R10(12) R10(13) R10(14) R10(15) R10(16) R10(17) R10(18) R10(19)
                   R10(20) R10(21) R10(22) R10(23) R10(24) R10(25) R10(26) R10(27) R10(28) R10(29)};
  return all[i];
}
#undef R10

thread_local bool t_seeded = false;
thread_local int t_seed = 0;
thread_local uint32_t t_late = 0; // first touched only by the late scenario

} // namespace

extern "C" {

// First touch on a thread checks every initial value; later touches check what
// this thread wrote. Returns the number of failures.
__declspec(dllexport) int Payload_Touch(int seed) {
  int failures = 0;
  if (!t_seeded) {
    failures += t_templated.a != 7 || t_templated.b != 2.5 || strcmp(t_templated.name, "emutls") != 0;
    failures += t_line.words[0] != 1 || t_line.words[2] != 3 || t_line.words[3] != 0;
    for (uint32_t v : t_zero) failures += v != 0;
    for (size_t i = 0; i < sizeof(t_big); i += 4096) failures += t_big[i] != 0;
    for (int i = 0; i < 300; ++i) failures += *Small(i) != i + 1;
    failures += t_withDestructor.canary != 0x5EED1234u;
    t_seeded = true;
    t_seed = seed;
    for (int i = 0; i < 300; ++i) *Small(i) = seed * 1000 + i;
    t_templated.a = seed;
    t_page.bytes[4095] = uint8_t(seed);
    t_withDestructor.values.assign(16, seed);
    for (size_t i = 0; i < sizeof(t_big); i += 4096) t_big[i] = uint8_t(seed);
  } else {
    failures += t_seed != seed;
    for (int i = 0; i < 300; ++i) failures += *Small(i) != seed * 1000 + i;
    failures += t_templated.a != seed || t_page.bytes[4095] != uint8_t(seed);
    failures += t_withDestructor.values.size() != 16 || t_withDestructor.values[15] != seed;
    for (size_t i = 0; i < sizeof(t_big); i += 4096) failures += t_big[i] != uint8_t(seed);
  }
  failures += (reinterpret_cast<uintptr_t>(&t_line) & 63) != 0;
  failures += (reinterpret_cast<uintptr_t>(&t_page) & 4095) != 0;
  return failures;
}

// Only trivially destructible variables: what late.dll may still read after the
// C runtime ran this thread's thread_local destructors at process exit (the
// hooks' War3HotHookState is such a variable). Returns failures.
__declspec(dllexport) int Payload_TouchTrivial(int seed) {
  int failures = t_seed != seed || t_templated.a != seed || t_page.bytes[4095] != uint8_t(seed);
  for (int i = 0; i < 300; ++i) failures += *Small(i) != seed * 1000 + i;
  for (size_t i = 0; i < sizeof(t_big); i += 4096) failures += t_big[i] != uint8_t(seed);
  t_late += 1; // first touch on this thread: allocates during process exit
  return failures;
}

// A thread whose first touch of this image's TLS comes during process exit (the
// TLS index and its storage are created then): initial values only.
__declspec(dllexport) int Payload_TouchFresh(int) {
  int failures = t_seeded || t_templated.a != 7 || t_templated.b != 2.5 || t_line.words[2] != 3;
  for (int i = 0; i < 300; ++i) failures += *Small(i) != i + 1;
  for (uint32_t v : t_zero) failures += v != 0;
  failures += (reinterpret_cast<uintptr_t>(&t_page) & 4095) != 0;
  t_late += 1;
  return failures;
}

__declspec(dllexport) uintptr_t Payload_Address(int which) {
  switch (which) {
  case 0: return reinterpret_cast<uintptr_t>(&t_templated);
  case 1: return reinterpret_cast<uintptr_t>(&t_page);
  case 2: return reinterpret_cast<uintptr_t>(&t_big);
  default: return reinterpret_cast<uintptr_t>(Small(which % 300));
  }
}

// Touches a variable this thread never touched (the allocation path) and the
// cached path, each with a distinct last error; returns failures.
__declspec(dllexport) int Payload_LastError() {
  int failures = 0;
  SetLastError(0xBEEFu);
  volatile uint32_t value = t_late; // allocation on first use
  failures += GetLastError() != 0xBEEFu;
  SetLastError(0xCAFEu);
  value = t_late + value; // cached
  failures += GetLastError() != 0xCAFEu;
  SetLastError(ERROR_FILE_NOT_FOUND);
  t_templated.b += 1.0;
  failures += GetLastError() != ERROR_FILE_NOT_FOUND;
  return failures;
}

#if defined(WAR3_EMUTLS_COMPILER_RT) // payload_rt.dll: compiler-rt's runtime
__declspec(dllexport) int Payload_Stats(dxvk::war3::memory::EmutlsStats *) { return 0; }
__declspec(dllexport) void Payload_ReleaseForUnload(int) {}
#else
__declspec(dllexport) int Payload_Stats(dxvk::war3::memory::EmutlsStats *stats) {
  return dxvk::war3::memory::EmutlsQueryStats(*stats) ? 1 : 0;
}

__declspec(dllexport) void Payload_ReleaseForUnload(int terminating) {
  dxvk::war3::memory::EmutlsReleaseForUnload(terminating != 0);
}
#endif

__declspec(dllexport) void Payload_Destructors(LONG *good, LONG *bad) {
  *good = g_goodDestructors;
  *bad = g_badDestructors;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(instance); // as d3d9_main.cpp
  return TRUE;
}

#elif defined(WAR3_EMUTLS_LATE)

using TouchFn = int (*)(int);
static TouchFn g_touch = nullptr;

extern "C" __declspec(dllexport) void Late_SetTouch(TouchFn touch) { g_touch = touch; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
  // Process exit: the payload (initialised after this DLL) is detached first.
  if (reason == DLL_PROCESS_DETACH && reserved && g_touch) {
    const int failures = g_touch(7); // Payload_TouchTrivial: the main thread touched it with 7 before;
                                     // Payload_TouchFresh: never touched before
    if (failures)
      TerminateProcess(GetCurrentProcess(), 77);
  }
  return TRUE;
}

#else // the test executable

#include "d3d9/war3/memory/war3_emutls.h"

namespace {

using Stats = dxvk::war3::memory::EmutlsStats;
int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char *what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s\n", what);
  }
}

struct Payload {
  HMODULE module = nullptr;
  int (*touch)(int) = nullptr;
  uintptr_t (*address)(int) = nullptr;
  int (*lastError)() = nullptr;
  int (*stats)(Stats *) = nullptr;
  void (*release)(int) = nullptr;
  void (*destructors)(LONG *, LONG *) = nullptr;

  bool Load(const wchar_t *path) {
    module = LoadLibraryW(path);
    if (!module)
      return false;
    touch = reinterpret_cast<int (*)(int)>(GetProcAddress(module, "Payload_Touch"));
    address = reinterpret_cast<uintptr_t (*)(int)>(GetProcAddress(module, "Payload_Address"));
    lastError = reinterpret_cast<int (*)()>(GetProcAddress(module, "Payload_LastError"));
    stats = reinterpret_cast<int (*)(Stats *)>(GetProcAddress(module, "Payload_Stats"));
    release = reinterpret_cast<void (*)(int)>(GetProcAddress(module, "Payload_ReleaseForUnload"));
    destructors = reinterpret_cast<void (*)(LONG *, LONG *)>(GetProcAddress(module, "Payload_Destructors"));
    return touch && address && lastError && stats && release && destructors;
  }
  Stats Now() const {
    Stats s;
    stats(&s);
    return s;
  }
};

Payload g_payload;

double PrivateMiB() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                       sizeof(counters));
  return double(counters.PrivateUsage) / (1 << 20);
}

struct Worker {
  int seed = 0;
  HANDLE start = nullptr;  // touch once released
  HANDLE finish = nullptr; // exit once released (null: exit at once)
  HANDLE touched = nullptr;
  int failures = 0;
  uintptr_t addresses[4] = {};
  Payload *payload = &g_payload;
};

DWORD WINAPI WorkerMain(void *argument) {
  auto *w = static_cast<Worker *>(argument);
  if (w->start)
    WaitForSingleObject(w->start, INFINITE);
  w->failures += w->payload->touch(w->seed);
  w->failures += w->payload->touch(w->seed); // cached path
  w->failures += w->payload->lastError();
  for (int i = 0; i < 4; ++i)
    w->addresses[i] = w->payload->address(i == 3 ? 299 : i);
  if (w->touched)
    SetEvent(w->touched);
  if (w->finish) {
    WaitForSingleObject(w->finish, INFINITE);
    w->failures += w->payload->touch(w->seed); // after the payload was reloaded: fresh storage
  }
  return 0;
}

struct Held {
  Worker *w;
  HANDLE hold;
};

DWORD WINAPI HeldMain(void *argument) {
  auto *h = static_cast<Held *>(argument);
  WorkerMain(h->w);
  WaitForSingleObject(h->hold, INFINITE);
  return 0;
}

void ConcurrentThreads() {
  const Stats before = g_payload.Now();
  constexpr int kThreads = 16;
  HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE finish = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<Worker> workers(kThreads);
  std::vector<HANDLE> threads, touched;
  for (int i = 0; i < kThreads; ++i) {
    workers[i].seed = 100 + i;
    workers[i].start = start;
    workers[i].finish = nullptr;
    workers[i].touched = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    touched.push_back(workers[i].touched);
  }
  // Keep every worker alive until all touched: addresses must all differ.
  HANDLE hold = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<Held> held(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    held[i] = {&workers[i], hold};
    threads.push_back(CreateThread(nullptr, 0, HeldMain, &held[i], 0, nullptr));
  }
  SetEvent(start);
  WaitForMultipleObjects(kThreads, touched.data(), TRUE, INFINITE);
  const Stats live = g_payload.Now();
  Check(live.liveThreads == before.liveThreads + kThreads, "every worker owns storage");
  bool distinct = true, aligned = true;
  for (int i = 0; i < kThreads; ++i) {
    Check(workers[i].failures == 0, "worker values, templates, alignment and last error");
    aligned &= (workers[i].addresses[1] & 4095) == 0;
    for (int j = 0; j < i; ++j)
      for (int k = 0; k < 4; ++k)
        distinct &= workers[i].addresses[k] != workers[j].addresses[k];
  }
  Check(distinct, "each thread has its own objects");
  Check(aligned, "alignas(4096) honoured");
  SetEvent(hold);
  WaitForMultipleObjects(kThreads, threads.data(), TRUE, INFINITE);
  const Stats after = g_payload.Now();
  Check(after.liveThreads == before.liveThreads, "thread exit frees each worker's storage");
  Check(after.liveBytes == before.liveBytes, "thread exit returns every byte");
  Check(after.threadsReleased == before.threadsReleased + kThreads, "one release per exited worker");
  Check(after.lateAccesses == before.lateAccesses, "no access after a thread's release");
  for (HANDLE h : threads) CloseHandle(h);
  for (HANDLE h : touched) CloseHandle(h);
  CloseHandle(start); CloseHandle(finish); CloseHandle(hold);
}

void ThreadChurn() {
  const Stats before = g_payload.Now();
  const double privateBefore = PrivateMiB();
  for (int i = 0; i < 200; ++i) {
    Worker w;
    w.seed = 1000 + i;
    HANDLE thread = CreateThread(nullptr, 0, WorkerMain, &w, 0, nullptr);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (w.failures) { Check(false, "churn worker"); break; }
  }
  const Stats after = g_payload.Now();
  const double grown = PrivateMiB() - privateBefore;
  std::printf("churn: 200 threads x 4 MiB, private %+0.1f MiB\n", grown);
  Check(after.liveThreads == before.liveThreads && after.liveBytes == before.liveBytes,
        "200 short threads leave nothing behind");
  Check(grown < 32.0, "private bytes do not creep with thread churn");
  LONG good = 0, bad = 0;
  g_payload.destructors(&good, &bad);
  Check(bad == 0 && good >= 216, "thread_local destructors ran before the storage was freed");
}

void MainThreadAndTerminationPath() {
  // Payload_Touch is not checked for the last error as a whole: the first touch
  // of t_withDestructor registers its destructor through mingw's
  // __cxa_thread_atexit (TlsGetValue, calloc), which changes the last error
  // with native TLS too. Payload_LastError isolates the runtime's own paths.
  Check(g_payload.touch(7) == 0, "main thread first touch");
  Check(g_payload.lastError() == 0, "allocation and cached paths keep the last error");
  const Stats before = g_payload.Now();
  g_payload.release(1); // what process termination does: nothing
  const Stats after = g_payload.Now();
  Check(after.tlsIndex == before.tlsIndex && after.liveThreads == before.liveThreads &&
        after.liveBytes == before.liveBytes && after.unloadReleases == before.unloadReleases,
        "process termination releases nothing");
  Check(g_payload.touch(7) == 0, "storage still valid after the termination path");
}

void FreeLibraryPath(const wchar_t *path) {
  // Four threads keep their 4 MiB each while the payload is freed.
  constexpr int kParked = 4;
  HANDLE finish = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<Worker> workers(kParked);
  std::vector<HANDLE> threads, touched;
  for (int i = 0; i < kParked; ++i) {
    workers[i].seed = 50 + i;
    workers[i].finish = finish;
    workers[i].touched = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    touched.push_back(workers[i].touched);
    threads.push_back(CreateThread(nullptr, 0, WorkerMain, &workers[i], 0, nullptr));
  }
  WaitForMultipleObjects(kParked, touched.data(), TRUE, INFINITE);
  const Stats live = g_payload.Now();
  const double beforeUnload = PrivateMiB();
  FreeLibrary(g_payload.module);
  const double freed = beforeUnload - PrivateMiB();
  std::printf("FreeLibrary: %u threads with storage, private -%0.1f MiB\n", live.liveThreads, freed);
  Check(freed > (kParked + 1) * 4.0 - 2.0, "FreeLibrary frees every thread's storage, parked ones too");
  Check(g_payload.Load(path), "payload loads again");
  const Stats fresh = g_payload.Now();
  Check(fresh.liveThreads == 0 && fresh.unloadReleases == 0, "a reloaded image starts empty");
  SetEvent(finish); // parked threads touch the new image: templates again, then their seed
  WaitForMultipleObjects(kParked, threads.data(), TRUE, INFINITE);
  for (int i = 0; i < kParked; ++i)
    Check(workers[i].failures == 0, "parked thread works with the reloaded image");
  const Stats after = g_payload.Now();
  Check(after.liveThreads == 0, "parked threads' new storage freed at their exit");
  for (HANDLE h : threads) CloseHandle(h);
  for (HANDLE h : touched) CloseHandle(h);
  CloseHandle(finish);
}

int ExitScenario(const wchar_t *latePath, const wchar_t *payloadPath, bool fresh) {
  // late.dll first: it is initialised first, so detached after the payload.
  HMODULE late = LoadLibraryW(latePath);
  HMODULE payload = LoadLibraryW(payloadPath);
  if (!late || !payload)
    return 90;
  auto setTouch = reinterpret_cast<void (*)(int (*)(int))>(GetProcAddress(late, "Late_SetTouch"));
  auto touch = reinterpret_cast<int (*)(int)>(GetProcAddress(payload, "Payload_Touch"));
  auto trivial = reinterpret_cast<int (*)(int)>(GetProcAddress(payload, "Payload_TouchTrivial"));
  auto first = reinterpret_cast<int (*)(int)>(GetProcAddress(payload, "Payload_TouchFresh"));
  if (!setTouch || !touch || !trivial || !first)
    return 91;
  if (fresh) {
    setTouch(first); // nothing touched the payload's TLS before the exit
    return 0;
  }
  if (touch(7) != 0)
    return 92;
  setTouch(trivial);
  return 0; // ExitProcess: payload detach, then late.dll touches the payload's TLS
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 4 && wcscmp(argv[1], L"--exit") == 0)
    return ExitScenario(argv[2], argv[3], false);
  if (argc == 4 && wcscmp(argv[1], L"--exit-fresh") == 0)
    return ExitScenario(argv[2], argv[3], true);
  // --high-index: take TLS indices until the next one is above 63, so the
  // payload's slot lives in the TEB's expansion array (games load many DLLs).
  const bool high = argc == 3 && wcscmp(argv[1], L"--high-index") == 0;
  if (high) {
    while (TlsAlloc() < 63) {
    }
    ++argv;
    --argc;
  }
  if (argc != 2) {
    std::printf("usage: emutls_test.exe [--high-index] <payload.dll> | --exit[-fresh] <late.dll> <payload.dll>\n");
    return 2;
  }
  if (!g_payload.Load(argv[1])) {
    std::printf("cannot load %ls\n", argv[1]);
    return 2;
  }
  Stats s = g_payload.Now();
  Check(s.tlsIndex == TLS_OUT_OF_INDEXES || s.liveThreads == 0, "nothing allocated before use");
  MainThreadAndTerminationPath();
  ConcurrentThreads();
  ThreadChurn();
  s = g_payload.Now();
  Check(s.variables >= 300, "every thread_local got an index");
  Check(!high || (s.tlsIndex >= 64 && s.tlsIndex != TLS_OUT_OF_INDEXES), "payload slot in the expansion array");
  const unsigned tlsIndex = s.tlsIndex;
  FreeLibraryPath(argv[1]);
  std::printf("RESULT {\"checks\":%d,\"failures\":%d,\"ok\":%s,\"tlsIndex\":%u}\n", g_checks, g_failures,
              g_failures ? "false" : "true", tlsIndex);
  return g_failures ? 1 : 0;
}

#endif
