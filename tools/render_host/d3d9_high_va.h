#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

// Placing memory that only our own code touches above 2 GiB in the 32-bit
// game process, so that the low 2 GiB stay free for the game.
//
// Stock Warcraft III is not large-address-aware. Such a process has no
// address above 2 GiB at all, and the game's own code breaks on a pointer
// above 0x7FFFFFFF even in an executable that somebody patched to be
// large-address-aware. Two rules follow:
//   - a process that is not large-address-aware keeps exactly the mapping
//     calls it had (eligible() is false and MapHigh does nothing);
//   - only memory whose address never reaches the game, a caller of D3D9 or
//     any code that might treat it as a signed number may be placed here.
//     A buffer handed out by Lock / LockRect is not such memory.
namespace warvk::host::d9::highva {
constexpr uint64_t Boundary = 0x80000000ull;

struct Process {
  bool exeFlag = false; // IMAGE_FILE_LARGE_ADDRESS_AWARE in the main executable
  uint64_t maxApp = 0;  // highest application address of THIS process
  // Both must agree: the header says what the loader was asked for, the
  // range says what this process actually got.
  bool eligible() const noexcept { return sizeof(void*) == 4 && exeFlag && maxApp >= Boundary; }
};
inline Process Probe() noexcept {
  Process process;
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  process.maxApp = uint64_t(reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress));
  // The loaded image's headers: the first page of the main module.
  const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  if (!base) return process;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < LONG(sizeof(IMAGE_DOS_HEADER))
      || dos->e_lfanew > 0x1000 - LONG(sizeof(IMAGE_NT_HEADERS32)))
    return process;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return process;
  process.exeFlag = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
  return process;
}

// All of [base, base + bytes) at or above the boundary and inside the range
// this process may use. 64-bit arithmetic: the end of the range is 4 GiB.
inline bool WhollyHigh(const void* base, uint64_t bytes, uint64_t maxApp) noexcept {
  const uint64_t begin = uint64_t(reinterpret_cast<uintptr_t>(base));
  return bytes && begin >= Boundary && begin + bytes <= maxApp + 1;
}

enum class Method : uint32_t { None = 0, MapViewOfFile3 = 1, Scan = 2 };
constexpr unsigned AllMethods = 3;
inline const char* MethodName(Method method) noexcept {
  return method == Method::MapViewOfFile3 ? "MapViewOfFile3" : method == Method::Scan ? "scan" : "none";
}
// What a mapping attempt did: for the receipt.
struct Placement {
  Method method = Method::None; // None: the view is where the system put it
  uint32_t attempts = 0, error = 0;
};

// Maps `bytes` of `mapping` wholly above 2 GiB, or returns nullptr and maps
// nothing; the caller then maps as it always did. `methods` is a mask of
// Method values (a test or a diagnostic run can leave one out).
inline void* MapHigh(HANDLE mapping, bool write, size_t bytes, const Process& process, Placement& placement,
                     unsigned methods = AllMethods) noexcept {
  if (!process.eligible() || !bytes) return nullptr;
  if (methods & unsigned(Method::MapViewOfFile3)) {
    // Windows 10 1803 and later. Looked up by name: an older system simply
    // has no such export and the scan below takes over. The two structures
    // are spelled out so that this compiles against any SDK level.
    struct Requirements { void* lowest; void* highest; SIZE_T alignment; };
    struct alignas(8) Parameter { uint64_t type; uint64_t value; };
    using Map3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
    const HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    const auto map3 = module ? reinterpret_cast<Map3>(reinterpret_cast<void*>(GetProcAddress(module, "MapViewOfFile3"))) : nullptr;
    if (map3) {
      Requirements where{reinterpret_cast<void*>(uintptr_t(Boundary)), nullptr, 0};
      Parameter parameter{1 /* MemExtendedParameterAddressRequirements */, uint64_t(reinterpret_cast<uintptr_t>(&where))};
      ++placement.attempts;
      void* view = map3(mapping, GetCurrentProcess(), nullptr, 0, bytes, 0, write ? PAGE_READWRITE : PAGE_READONLY, &parameter, 1);
      if (view && WhollyHigh(view, bytes, process.maxApp)) {
        placement.method = Method::MapViewOfFile3;
        return view;
      }
      if (view) UnmapViewOfFile(view);
      else placement.error = GetLastError();
    }
  }
  if (methods & unsigned(Method::Scan)) {
    // Find a free hole above the boundary and ask for exactly that address.
    // Another thread can take the hole between the query and the map: look
    // again a bounded number of times.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uint64_t step = info.dwAllocationGranularity ? info.dwAllocationGranularity : 0x10000;
    const uint64_t limit = process.maxApp + 1;
    uint64_t cursor = Boundary;
    for (unsigned tries = 0; tries < 8 && cursor + bytes <= limit;) {
      MEMORY_BASIC_INFORMATION region{};
      if (VirtualQuery(reinterpret_cast<const void*>(uintptr_t(cursor)), &region, sizeof(region)) != sizeof(region)) break;
      const uint64_t begin = uint64_t(reinterpret_cast<uintptr_t>(region.BaseAddress));
      const uint64_t end = begin + region.RegionSize;
      if (end <= cursor) break; // no progress: do not loop on it
      const uint64_t candidate = (std::max(cursor, begin) + step - 1) / step * step;
      if (region.State != MEM_FREE || candidate + bytes > end || candidate + bytes > limit) {
        cursor = end;
        continue;
      }
      ++tries;
      ++placement.attempts;
      void* view = MapViewOfFileEx(mapping, write ? FILE_MAP_WRITE | FILE_MAP_READ : FILE_MAP_READ, 0, 0, bytes,
                                   reinterpret_cast<void*>(uintptr_t(candidate)));
      if (view && WhollyHigh(view, bytes, process.maxApp)) {
        placement.method = Method::Scan;
        return view;
      }
      if (view) UnmapViewOfFile(view);
      else placement.error = GetLastError();
      cursor = candidate + step;
    }
  }
  return nullptr;
}
} // namespace warvk::host::d9::highva
