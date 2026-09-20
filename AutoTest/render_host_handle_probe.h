#pragma once
#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

// Optional read-only SELF-process diagnostics. Never enumerates another process,
// closes an observed handle, queries file names, or runs in the product DLL.
// Native layout reference: winsiderss/phnt ntpsapi.h ProcessHandleInformation
// (51, Windows 8+). Private NT ABI: lab diagnostic only, not an acceptance gate.
inline void ProbeOwnHandles(const char* phase) {
  if (!std::getenv("WARVK_HOST_LAB_HANDLE_PROBE")) return;
  using QueryProcess = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  using QueryObject = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  const auto module = GetModuleHandleW(L"ntdll.dll");
  const auto rawProcess = GetProcAddress(module, "NtQueryInformationProcess");
  const auto rawObject = GetProcAddress(module, "NtQueryObject");
  QueryProcess query = nullptr; QueryObject object = nullptr;
  static_assert(sizeof(query) == sizeof(rawProcess));
  std::memcpy(&query, &rawProcess, sizeof(query)); std::memcpy(&object, &rawObject, sizeof(object));
  if (!query || !object) return;
  struct Entry { HANDLE handle; SIZE_T refs, pointers; ULONG access, type, flags, reserved; };
  struct Header { ULONG_PTR count, reserved; };
  std::vector<uint8_t> bytes(1u << 20); ULONG used = 0;
  const auto status = query(GetCurrentProcess(), 51, bytes.data(), ULONG(bytes.size()), &used);
  if (status < 0 || used > bytes.size() || used < sizeof(Header)) return;
  Header header{}; std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.count > (used - sizeof(Header)) / sizeof(Entry)) return;
  static std::vector<Entry> baseline;
  if (!std::strcmp(phase, "baseline")) {
    baseline.resize(header.count);
    std::memcpy(baseline.data(), bytes.data() + sizeof(Header), baseline.size() * sizeof(Entry));
    return; // Do not format, query types, or flush stderr before the experiment.
  }
  std::fprintf(stderr, "SELF_HANDLES baseline count=%llu\n", (unsigned long long)baseline.size());
  for (auto e : baseline) std::fprintf(stderr, "%llu:%lu ", (unsigned long long)uintptr_t(e.handle), (unsigned long)e.type);
  std::fprintf(stderr, "\n");
  std::fprintf(stderr, "SELF_HANDLES %s count=%llu\n", phase, (unsigned long long)header.count);
  for (size_t i = 0; i < header.count; ++i) {
    Entry e{}; std::memcpy(&e, bytes.data() + sizeof(Header) + i * sizeof(Entry), sizeof(e));
    std::fprintf(stderr, "%llu:%lu ", (unsigned long long)uintptr_t(e.handle), (unsigned long)e.type);
  }
  std::fprintf(stderr, "\n");
  for (size_t i = 0; i < header.count; ++i) {
    Entry e{}; std::memcpy(&e, bytes.data() + sizeof(Header) + i * sizeof(Entry), sizeof(e));
    bool known = false;
    for (auto b : baseline) if (b.handle == e.handle && b.type == e.type) known = true;
    if (known) continue;
    alignas(void*) uint8_t info[2048]{}; ULONG returned = 0;
    if (object(e.handle, 2, info, sizeof(info), &returned) < 0) continue;
    UNICODE_STRING text{}; std::memcpy(&text, info, sizeof(text));
    const auto start = uintptr_t(text.Buffer), base = uintptr_t(info);
    if ((text.Length & 1) || start < base || start > base + sizeof(info) || text.Length > base + sizeof(info) - start) continue;
    std::fprintf(stderr, "ADDED %llu type=%lu name=%.*ls\n", (unsigned long long)uintptr_t(e.handle),
      (unsigned long)e.type, text.Length / 2, text.Buffer);
  }
  std::fprintf(stderr, "\n"); std::fflush(stderr);
}
