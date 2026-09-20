#pragma once
#include <cstdint>

namespace warvk::host::lab {
// Self-process, cold laboratory checkpoints only. Bytes, not GPU allocation or
// game memory savings. A partial API result remains invalid, never zero usage.
struct ProcessMemory {
  bool valid=false;
  uint32_t win32=0;
  uint64_t privateBytes=0, peakPrivateBytes=0, workingSetBytes=0;
  uint64_t usedVirtualBytes=0, privateVirtualBytes=0, mappedVirtualBytes=0, regions=0;
};
ProcessMemory ProbeOwnProcessMemory() noexcept;
void PrintProcessMemory(const ProcessMemory&) noexcept; // One JSON object, no newline.
}
