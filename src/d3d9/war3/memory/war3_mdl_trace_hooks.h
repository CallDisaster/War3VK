#pragma once

// Diagnostic-only ABI adapters. The same entry functions are exercised by the
// x86 CPU fixture with fake originals; no allocator policy lives here.
#include "war3_mdl_trace_core.h"
#include "war3_storm_realloc_contract.h"
#include <windows.h>
#include <intrin.h>
#if defined(_MSC_VER)
#define WARVK_MDL_RETURN_ADDRESS() _ReturnAddress()
#else
#define WARVK_MDL_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace dxvk::war3::memory::mdltrace {

struct HookState {
  Core* core = nullptr;
  uint32_t gameBase = 0;
  uint32_t gameSize = 0;
  bool (*read)(uint32_t, void*, uint32_t) = nullptr;
  uint32_t (*thread)() = nullptr;

  uint32_t caller(const void* address) const noexcept {
    const auto value = uintptr_t(address);
    return value >= gameBase && value - gameBase < gameSize
      ? uint32_t(value - gameBase) : 0;
  }
  bool selected(const void* name) const noexcept {
    return uintptr_t(name) == uintptr_t(gameBase) + 0xB7FF90u;
  }
  bool table(const void* p) const noexcept {
    return uintptr_t(p) == uintptr_t(gameBase) + 0xBC6AC8u;
  }
  Event event(uint32_t kind, const void* returnAddress) const noexcept {
    Event e;
    e.words[1] = kind;
    e.words[2] = thread();
    e.words[3] = caller(returnAddress);
    return e;
  }
  // Each block is a separate, best-effort RPM observation, NOT an atomic game
  // state or a claim of ownership. Never dereference name or chase link chains.
  void fields(Event& e, uint32_t node, uint32_t tableAddress,
              uint32_t headAddress = 0) const noexcept {
    uint32_t words[5] = {};
    if (node) {
      if (read(node, words, 20)) {
        for (unsigned i = 0; i != 5; ++i) e.words[14+i] = words[i];
        e.words[13] |= 1;
      } else e.words[13] |= 8;
    }
    if (tableAddress) {
      if (tableAddress <= UINT32_MAX - 16u && read(tableAddress+4, words, 12)) {
        for (unsigned i = 0; i != 3; ++i) e.words[19+i] = words[i];
        e.words[13] |= 2;
      } else e.words[13] |= 8;
    }
    if (headAddress) {
      if (read(headAddress, words, 8)) {
        e.words[22] = headAddress;
        e.words[23] = words[0];
        e.words[24] = words[1];
        e.words[13] |= 4;
      } else e.words[13] |= 8;
    }
  }
};

struct Hooks {
  using AllocFn = void* (__fastcall *)(int, int, size_t, const char*, DWORD, DWORD);
  using FreeFn = int (__stdcall *)(void*, const char*, int, DWORD);
  using ReallocFn = void* (__fastcall *)(int, int, void*, size_t, const char*, int, DWORD);
  using BucketFn = void* (__fastcall *)(void*, void*, void*, uint32_t, uint32_t);
  using InsertFn = void* (__fastcall *)(void*, void*, uint32_t, uint32_t, uint32_t);
  using UnlinkFn = uint32_t (__fastcall *)(void*, void*, void*);
  inline static HookState* state = nullptr;
  inline static AllocFn originalAlloc = nullptr;
  inline static FreeFn originalFree = nullptr;
  inline static ReallocFn originalRealloc = nullptr;
  inline static BucketFn originalBucket = nullptr;
  inline static InsertFn originalInsert = nullptr;
  inline static UnlinkFn originalUnlink = nullptr;

  static void* __fastcall alloc(int ecx, int edx, size_t size,
      const char* name, DWORD line, DWORD flags) {
    void* result = originalAlloc(ecx, edx, size, name, line, flags);
    const DWORD error = GetLastError();
    if (state && state->selected(name)) {
      auto e = state->event(1, WARVK_MDL_RETURN_ADDRESS());
      e.words[4] = e.words[12] = uint32_t(uintptr_t(result));
      e.words[8] = uint32_t(size); e.words[9] = flags;
      e.words[10] = 0xB7FF90u; e.words[11] = line;
      // Before construction: argument and return values only, no node reads.
      state->core->noteAllocation(e);
    }
    SetLastError(error);
    return result;
  }

  static int __stdcall free(void* ptr, const char* name, int line, DWORD flags) {
    const DWORD before = GetLastError();
    Event e;
    Token token;
    if (state) {
      e = state->event(2, WARVK_MDL_RETURN_ADDRESS());
      e.words[4] = uint32_t(uintptr_t(ptr));
      e.words[9] = flags; e.words[11] = uint32_t(line);
      const bool named = state->selected(name);
      e.words[10] = named ? 0xB7FF90u : 0;
      token = state->core->beginOperation(e, named);
    }
    SetLastError(before);
    const int result = originalFree(ptr, name, line, flags);
    const DWORD error = GetLastError();
    if (state && token.admitted) {
      e.words[1] = 3; e.words[12] = uint32_t(result);
      state->core->endOperation(token, e, false);
    }
    SetLastError(error);
    return result;
  }

  static void* __fastcall realloc(int ecx, int edx, void* ptr, size_t size,
      const char* name, int line, DWORD flags) {
    const DWORD before = GetLastError();
    Event e;
    Token token;
    if (state) {
      e = state->event(4, WARVK_MDL_RETURN_ADDRESS());
      e.words[4] = uint32_t(uintptr_t(ptr));
      e.words[8] = uint32_t(size); e.words[9] = flags;
      e.words[11] = uint32_t(line);
      const bool named = state->selected(name);
      e.words[10] = named ? 0xB7FF90u : 0;
      token = state->core->beginOperation(e, named);
    }
    SetLastError(before);
    void* result = originalRealloc(ecx, edx, ptr, size, name, line, flags);
    const DWORD error = GetLastError();
    if (state && token.admitted) {
      e.words[1] = 5;
      e.words[5] = e.words[12] = uint32_t(uintptr_t(result));
      // Native size zero may shrink in place to a live zero-byte block.
      // Never infer physical release from the requested size or a null return.
      const bool released = NativeReallocReleasedOldPointer(ptr, result);
      if (released) e.words[13] |= 16;
      state->core->endOperation(token, e, released);
    }
    SetLastError(error);
    return result;
  }

  static void* __fastcall bucket(void* table, void* edx, void* descriptor,
      uint32_t extra, uint32_t flags) {
    void* result = originalBucket(table, edx, descriptor, extra, flags);
    const DWORD error = GetLastError();
    if (state && state->table(table)) {
      auto e = state->event(6, WARVK_MDL_RETURN_ADDRESS());
      e.words[4] = e.words[12] = uint32_t(uintptr_t(result));
      e.words[5] = uint32_t(uintptr_t(descriptor));
      e.words[8] = extra; e.words[9] = flags;
      const uint32_t head = e.words[5] <= UINT32_MAX-4 ? e.words[5]+4 : 0;
      state->fields(e, e.words[4], uint32_t(uintptr_t(table)), head);
      state->core->noteBoundary(e, true);
    }
    SetLastError(error);
    return result;
  }

  static void* __fastcall insert(void* table, void* edx, uint32_t hash,
      uint32_t extra, uint32_t flags) {
    const DWORD before = GetLastError();
    const bool selected = state && state->table(table);
    Event e;
    uint32_t oldHead = 0;
    if (selected) {
      e = state->event(7, WARVK_MDL_RETURN_ADDRESS());
      e.words[5] = uint32_t(uintptr_t(table));
      e.words[8] = extra; e.words[9] = flags; e.words[11] = hash;
      state->fields(e, 0, e.words[5]);
      if (e.words[13] & 2) oldHead = e.words[20];
      state->fields(e, 0, 0, oldHead);
      state->core->noteBoundary(e, true);
    }
    SetLastError(before);
    void* result = originalInsert(table, edx, hash, extra, flags);
    const DWORD error = GetLastError();
    if (selected) {
      e = state->event(8, WARVK_MDL_RETURN_ADDRESS());
      e.words[4] = e.words[12] = uint32_t(uintptr_t(result));
      e.words[5] = uint32_t(uintptr_t(table));
      e.words[8] = extra; e.words[9] = flags; e.words[11] = hash;
      state->fields(e, e.words[4], e.words[5], oldHead);
      state->core->noteBoundary(e, true);
    }
    SetLastError(error);
    return result;
  }

  static uint32_t __fastcall unlink(void* ecx, void* edx, void* node) {
    const DWORD before = GetLastError();
    // ECX is a caller qualifier only: the original generic function does not
    // consume ECX. Other callers and destructor-inline unlinks remain missing.
    const bool selected = state && state->table(ecx);
    Event e;
    if (selected) {
      e = state->event(9, WARVK_MDL_RETURN_ADDRESS());
      e.words[4] = uint32_t(uintptr_t(node));
      e.words[5] = uint32_t(uintptr_t(ecx));
      state->fields(e, e.words[4], e.words[5]);
      state->core->noteBoundary(e, true);
    }
    SetLastError(before);
    const uint32_t result = originalUnlink(ecx, edx, node);
    const DWORD error = GetLastError();
    if (selected) {
      e = state->event(10, WARVK_MDL_RETURN_ADDRESS());
      e.words[4] = uint32_t(uintptr_t(node));
      e.words[5] = uint32_t(uintptr_t(ecx)); e.words[12] = result;
      // Before the caller's destructor/free; never read a node after free.
      state->fields(e, e.words[4], e.words[5]);
      state->core->noteBoundary(e, true);
    }
    SetLastError(error);
    return result;
  }
};
}
#undef WARVK_MDL_RETURN_ADDRESS
