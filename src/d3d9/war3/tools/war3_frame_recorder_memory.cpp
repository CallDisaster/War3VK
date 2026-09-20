#include "war3_frame_recorder_memory.h"
#include <windows.h>

namespace dxvk::war3::tools::evidence {
RecorderMemory QueryRecorderMemory(bool contiguous) noexcept {
  const DWORD savedError = GetLastError();
  struct Restore { DWORD value; ~Restore() { SetLastError(value); } } restore{savedError};
  MEMORYSTATUSEX status{}; status.dwLength = sizeof(status);
  RecorderMemory result;
  if (!GlobalMemoryStatusEx(&status)) return result;
  result.valid = true;
  result.totalVirtual = status.ullTotalVirtual;
  result.availableVirtual = status.ullAvailVirtual;
  result.availableCommit = status.ullAvailPageFile;
  if (!contiguous) return result;
  SYSTEM_INFO system{}; GetSystemInfo(&system); // caller's address space, not native WOW64 host
  const uint64_t limit = uint64_t(reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress));
  uint64_t cursor = uint64_t(reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress));
  for (uint32_t regions = 0; cursor <= limit && regions < 65536; ++regions) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(uintptr_t(cursor)), &region, sizeof(region)) != sizeof(region))
      return result; // incomplete scan never authorizes a contiguous allocation
    const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(region.BaseAddress));
    if (base > cursor || !region.RegionSize || uint64_t(region.RegionSize) > UINT64_MAX - base)
      return result;
    const uint64_t end = base + uint64_t(region.RegionSize);
    if (end <= cursor) return result;
    const uint64_t clipped = end > limit ? limit + 1 : end;
    if (region.State == MEM_FREE && clipped - cursor > result.largestFreeRegion)
      result.largestFreeRegion = clipped - cursor;
    cursor = end;
  }
  result.contiguousKnown = cursor > limit;
  return result;
}
} // namespace dxvk::war3::tools::evidence
