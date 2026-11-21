// war3_emutls.cpp
// Emulated thread-local storage runtime of the x86 product DLL, built with
// -femulated-tls (docs/research/2026-11-17-x64-static-tls-b-prime.md).
//
// clang turns every thread_local access into __emutls_get_address(control)
// (i686 symbol ___emutls_get_address). Defining it here keeps the linker from
// pulling compiler-rt's emutls object, which
//   - frees its TLS index from the DLL's atexit at unload, so code that runs
//     after this DLL's DLL_PROCESS_DETACH at process exit (Game.dll/Storm.dll
//     teardown calling our Storm hooks) aborts with 0xC0000409;
//   - never frees a thread's storage when the thread exits;
//   - lets TlsGetValue clear the caller's last error.
// Rules here:
//   - process termination: nothing is released; later accesses keep working.
//   - FreeLibrary (tests and tools; the game keeps this DLL, whose Storm
//     hooks stay installed): the image's last static destructor frees every
//     thread's storage and the TLS index; nothing of this image runs after it.
//   - thread exit: a TLS callback placed after the C runtime's thread_local
//     destructors (.CRT$XLB) frees the thread's storage (heap free only, no
//     lock that can block: the registry spin lock guards two pointer writes).
//   - every access preserves the caller's last error: the hot path reads the
//     thread's TLS slot from the TEB (<winternl.h> layout) and calls nothing;
//     the first access of a variable on a thread saves and restores it.
// This file has no thread_local and calls only Win32 (it runs under the
// loader lock and inside thread_local accesses of every other file).
// GCC builds keep libgcc's runtime (the 1.22.x release line): nothing here.

#include "war3_emutls.h"

#if defined(__clang__) && defined(_WIN32)

#include <windows.h>
#include <winternl.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace dxvk::war3::memory {
namespace {

// compiler-rt / libgcc __emutls_control: gcc_word is 32-bit on i686.
struct EmutlsControl {
  uintptr_t size;
  uintptr_t align;
  uintptr_t index; // 0 until assigned; slot index + 1
  const void *value; // initial bytes, or null for zero
};
static_assert(sizeof(EmutlsControl) == 4 * sizeof(void *), "emutls control layout");

// One per thread that touched a thread_local; slots[i] belongs to index i + 1.
struct ThreadBlock {
  ThreadBlock *prev;
  ThreadBlock *next;
  void **slots;
  uintptr_t capacity;
};

ThreadBlock *const kReleasedThread = reinterpret_cast<ThreadBlock *>(uintptr_t{1});

DWORD g_tlsIndex = TLS_OUT_OF_INDEXES;
uintptr_t g_nextIndex = 0;
LONG g_registryLock = 0;
ThreadBlock *g_threads = nullptr;

uint32_t g_liveThreads = 0;
uint32_t g_liveObjects = 0;
uint64_t g_liveBytes = 0;
uint32_t g_threadsReleased = 0;
uint32_t g_lateAccesses = 0;
uint32_t g_unloadReleases = 0;

template <typename T> void Add(T &counter, T value) {
  __atomic_fetch_add(&counter, value, __ATOMIC_RELAXED);
}
template <typename T> void Sub(T &counter, T value) {
  __atomic_fetch_sub(&counter, value, __ATOMIC_RELAXED);
}

[[noreturn]] void Fail(const char *what) {
  char text[160] = "WarVK emutls: ";
  strncat(text, what, sizeof(text) - strlen(text) - 3);
  strcat(text, "\r\n");
  OutputDebugStringA(text);
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  DWORD written = 0;
  if (err && err != INVALID_HANDLE_VALUE)
    WriteFile(err, text, DWORD(strlen(text)), &written, nullptr);
  abort(); // what compiler-rt does on the same failures
}

void LockRegistry() {
  for (unsigned spins = 0; InterlockedExchange(&g_registryLock, 1) != 0; ++spins) {
    if (spins < 64)
      YieldProcessor();
    else
      SwitchToThread();
  }
}

void UnlockRegistry() { InterlockedExchange(&g_registryLock, 0); }

DWORD ThreadIndex() {
  DWORD index = __atomic_load_n(&g_tlsIndex, __ATOMIC_ACQUIRE);
  if (index != TLS_OUT_OF_INDEXES)
    return index;
  const DWORD fresh = TlsAlloc();
  if (fresh == TLS_OUT_OF_INDEXES)
    Fail("TlsAlloc failed");
  DWORD expected = TLS_OUT_OF_INDEXES;
  if (__atomic_compare_exchange_n(&g_tlsIndex, &expected, fresh, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return fresh;
  TlsFree(fresh); // another thread won
  return expected;
}

// What TlsGetValue returns for the calling thread, read from its TEB without a
// call: TlsGetValue sets the last error to 0, this leaves it alone. A null
// expansion array means the thread never stored above slot 63 (null value).
static_assert(offsetof(TEB, TlsSlots) == (sizeof(void *) == 4 ? 0xE10 : 0x1480), "TEB layout");
static_assert(offsetof(TEB, TlsExpansionSlots) == (sizeof(void *) == 4 ? 0xF94 : 0x1780), "TEB layout");
inline void *ThreadSlot(DWORD tlsIndex) {
  const TEB *teb = NtCurrentTeb();
  if (tlsIndex < 64)
    return teb->TlsSlots[tlsIndex];
  auto *const *expansion = static_cast<void *const *>(teb->TlsExpansionSlots);
  return expansion ? expansion[tlsIndex - 64] : nullptr;
}

uintptr_t VariableIndex(EmutlsControl *control) {
  uintptr_t index = __atomic_load_n(&control->index, __ATOMIC_ACQUIRE);
  if (index)
    return index;
  const uintptr_t fresh = __atomic_add_fetch(&g_nextIndex, 1, __ATOMIC_RELAXED);
  // A lost race wastes one index: that slot simply stays empty.
  if (__atomic_compare_exchange_n(&control->index, &index, fresh, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return fresh;
  return index;
}

void *HeapBytes(size_t bytes, DWORD flags = 0) {
  void *memory = HeapAlloc(GetProcessHeap(), flags, bytes);
  if (!memory)
    Fail("out of memory");
  return memory;
}

// Each object is preceded by two words: its size, then the heap block.
void FreeBlock(ThreadBlock *block) {
  HANDLE heap = GetProcessHeap();
  uint32_t objects = 0;
  uint64_t bytes = 0;
  for (uintptr_t i = 0; i < block->capacity; ++i) {
    if (auto **object = static_cast<void **>(block->slots[i])) {
      bytes += reinterpret_cast<uintptr_t>(object[-2]);
      HeapFree(heap, 0, object[-1]);
      ++objects;
    }
  }
  HeapFree(heap, 0, block->slots);
  HeapFree(heap, 0, block);
  Sub(g_liveObjects, objects);
  Sub(g_liveBytes, bytes);
  Sub(g_liveThreads, 1u);
}

using ShutdownInProgressFn = BOOLEAN(NTAPI *)();
ShutdownInProgressFn g_shutdownInProgress = nullptr;
bool g_shutdownResolved = false;

// ntdll!RtlDllShutdownInProgress, or null when the system has none.
ShutdownInProgressFn ShutdownQuery() {
  if (!__atomic_load_n(&g_shutdownResolved, __ATOMIC_ACQUIRE)) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = ntdll ? reinterpret_cast<ShutdownInProgressFn>(
                             GetProcAddress(ntdll, "RtlDllShutdownInProgress"))
                       : nullptr;
    __atomic_store_n(&g_shutdownInProgress, query, __ATOMIC_RELAXED);
    __atomic_store_n(&g_shutdownResolved, true, __ATOMIC_RELEASE);
  }
  return __atomic_load_n(&g_shutdownInProgress, __ATOMIC_RELAXED);
}

bool ProcessTerminating() {
  const ShutdownInProgressFn query = ShutdownQuery();
  return !query || query(); // unknown: keep everything
}

ThreadBlock *NewBlock(DWORD tlsIndex) {
  auto *block = static_cast<ThreadBlock *>(HeapBytes(sizeof(ThreadBlock), HEAP_ZERO_MEMORY));
  // At process termination the other threads were stopped wherever they were,
  // maybe inside the registry lock, and nothing is released any more: a block
  // made then (a thread that first touches TLS in another DLL's teardown)
  // stays off the list instead of waiting for that lock.
  const ShutdownInProgressFn query = ShutdownQuery();
  if (!query || !query()) {
    LockRegistry();
    block->next = g_threads;
    if (g_threads)
      g_threads->prev = block;
    g_threads = block;
    UnlockRegistry();
  }
  if (!TlsSetValue(tlsIndex, block))
    Fail("TlsSetValue failed");
  Add(g_liveThreads, 1u);
  return block;
}

void *NewObject(ThreadBlock *block, const EmutlsControl *control, uintptr_t index) {
  if (index > block->capacity) {
    uintptr_t capacity = block->capacity ? block->capacity * 2 : 64;
    if (capacity < index)
      capacity = index;
    auto **slots = static_cast<void **>(HeapBytes(capacity * sizeof(void *), HEAP_ZERO_MEMORY));
    if (block->capacity)
      memcpy(slots, block->slots, block->capacity * sizeof(void *));
    HeapFree(GetProcessHeap(), 0, block->slots);
    block->slots = slots;
    block->capacity = capacity;
  }
  const size_t size = control->size;
  size_t align = control->align < sizeof(void *) ? sizeof(void *) : control->align;
  if (align & (align - 1))
    Fail("alignment is not a power of two");
  const size_t slack = align - 1 + 2 * sizeof(void *);
  if (size > SIZE_MAX - slack)
    Fail("object too large");
  char *raw = static_cast<char *>(HeapBytes(size + slack));
  char *object = reinterpret_cast<char *>(
      (reinterpret_cast<uintptr_t>(raw) + slack) & ~uintptr_t(align - 1));
  reinterpret_cast<void **>(object)[-1] = raw;
  reinterpret_cast<void **>(object)[-2] = reinterpret_cast<void *>(uintptr_t(size));
  if (control->value)
    memcpy(object, control->value, size);
  else
    memset(object, 0, size);
  block->slots[index - 1] = object;
  Add(g_liveObjects, 1u);
  Add(g_liveBytes, uint64_t(size));
  return object;
}

// Frees the calling thread's storage; runs after the C runtime's thread_local
// destructors (.CRT$XLB), which may still read their objects.
void NTAPI ThreadExitCallback(PVOID, DWORD reason, PVOID) {
  if (reason != DLL_THREAD_DETACH)
    return;
  const DWORD tlsIndex = __atomic_load_n(&g_tlsIndex, __ATOMIC_ACQUIRE);
  if (tlsIndex == TLS_OUT_OF_INDEXES)
    return;
  const DWORD error = GetLastError();
  auto *block = static_cast<ThreadBlock *>(TlsGetValue(tlsIndex));
  if (block && block != kReleasedThread) {
    LockRegistry();
    if (block->prev)
      block->prev->next = block->next;
    else if (g_threads == block)
      g_threads = block->next; // (a block made during termination is not listed)
    if (block->next)
      block->next->prev = block->prev;
    UnlockRegistry();
    FreeBlock(block);
    TlsSetValue(tlsIndex, kReleasedThread);
    Add(g_threadsReleased, 1u);
  }
  SetLastError(error);
}

// Constructed before any other static object of the image, so its destructor
// is the image's last atexit entry at DLL_PROCESS_DETACH.
struct ImageTeardown {
  ImageTeardown() noexcept {}
  ~ImageTeardown() {
    const DWORD error = GetLastError();
    EmutlsReleaseForUnload(ProcessTerminating());
    SetLastError(error);
  }
};
__attribute__((init_priority(101))) ImageTeardown g_imageTeardown;

} // namespace

bool EmutlsQueryStats(EmutlsStats &stats) noexcept {
  stats.tlsIndex = __atomic_load_n(&g_tlsIndex, __ATOMIC_ACQUIRE);
  stats.variables = uint32_t(__atomic_load_n(&g_nextIndex, __ATOMIC_RELAXED));
  stats.liveThreads = __atomic_load_n(&g_liveThreads, __ATOMIC_RELAXED);
  stats.liveObjects = __atomic_load_n(&g_liveObjects, __ATOMIC_RELAXED);
  stats.liveBytes = __atomic_load_n(&g_liveBytes, __ATOMIC_RELAXED);
  stats.threadsReleased = __atomic_load_n(&g_threadsReleased, __ATOMIC_RELAXED);
  stats.lateAccesses = __atomic_load_n(&g_lateAccesses, __ATOMIC_RELAXED);
  stats.unloadReleases = __atomic_load_n(&g_unloadReleases, __ATOMIC_RELAXED);
  return true;
}

void EmutlsReleaseForUnload(bool processTerminating) noexcept {
  if (processTerminating)
    return;
  const DWORD tlsIndex = __atomic_exchange_n(&g_tlsIndex, TLS_OUT_OF_INDEXES, __ATOMIC_ACQ_REL);
  if (tlsIndex == TLS_OUT_OF_INDEXES)
    return;
  const DWORD error = GetLastError();
  LockRegistry();
  ThreadBlock *block = g_threads;
  g_threads = nullptr;
  UnlockRegistry();
  while (block) {
    ThreadBlock *next = block->next;
    FreeBlock(block);
    block = next;
  }
  TlsFree(tlsIndex); // also clears the slot in every thread
  Add(g_unloadReleases, 1u);
  SetLastError(error);
}

} // namespace dxvk::war3::memory

namespace dxvk::war3::memory {
namespace {

// First access of a variable on this thread (or of any variable, or after the
// thread's storage was released): every Win32 call is here, between saving
// and restoring the caller's last error.
__attribute__((noinline)) void *SlowAddress(EmutlsControl *control) {
  const DWORD error = GetLastError();
  const uintptr_t index = VariableIndex(control);
  const DWORD tlsIndex = ThreadIndex();
  auto *block = static_cast<ThreadBlock *>(TlsGetValue(tlsIndex));
  void *object = block && block != kReleasedThread && index <= block->capacity
                     ? block->slots[index - 1]
                     : nullptr;
  if (!object) {
    if (block == kReleasedThread) {
      // Touched again during this thread's exit, after its storage was freed
      // (another DLL's thread detach calling into us): new storage, kept
      // until an unload release (never at process termination).
      Add(g_lateAccesses, 1u);
      block = nullptr;
    }
    if (!block)
      block = NewBlock(tlsIndex);
    object = NewObject(block, control, index);
  }
  SetLastError(error);
  return object;
}

} // namespace
} // namespace dxvk::war3::memory

using namespace dxvk::war3::memory;

extern "C" void *__emutls_get_address(void *opaque) {
  auto *control = static_cast<EmutlsControl *>(opaque);
  // Hot path: no call, so the last error is untouched.
  const uintptr_t index = __atomic_load_n(&control->index, __ATOMIC_ACQUIRE);
  const DWORD tlsIndex = __atomic_load_n(&g_tlsIndex, __ATOMIC_ACQUIRE);
  if (index && tlsIndex != TLS_OUT_OF_INDEXES) {
    auto *block = static_cast<const ThreadBlock *>(ThreadSlot(tlsIndex));
    if (block && block != kReleasedThread && index <= block->capacity)
      if (void *object = block->slots[index - 1])
        return object;
  }
  return SlowAddress(control);
}

// The loader calls the image's TLS callbacks in section order: mingw's
// thread_local destructors (.CRT$XLB), its TLS init/dtor (.CRT$XLC/.CRT$XLD),
// then this one, before .CRT$XLZ ends the list.
extern "C" __attribute__((section(".CRT$XLY"), used))
PIMAGE_TLS_CALLBACK war3_emutls_thread_exit = ThreadExitCallback;

#else

namespace dxvk::war3::memory {
bool EmutlsQueryStats(EmutlsStats &) noexcept { return false; }
void EmutlsReleaseForUnload(bool) noexcept {}
} // namespace dxvk::war3::memory

#endif
