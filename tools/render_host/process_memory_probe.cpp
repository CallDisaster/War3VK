#include "process_memory_probe.h"
#include <windows.h>
#include <psapi.h>
#include <cstdio>

namespace warvk::host::lab {
ProcessMemory ProbeOwnProcessMemory() noexcept {
  ProcessMemory out;
  PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
  if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))) {
    out.win32=GetLastError();return out;
  }
  out.privateBytes=counters.PrivateUsage;
  out.peakPrivateBytes=counters.PeakPagefileUsage;
  out.workingSetBytes=counters.WorkingSetSize;
  SYSTEM_INFO system{};GetSystemInfo(&system);
  auto cursor=reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
  const auto last=reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
  while(cursor<=last) {
    MEMORY_BASIC_INFORMATION region{};
    if(VirtualQuery(reinterpret_cast<const void*>(cursor),&region,sizeof(region))!=sizeof(region)) {
      out.win32=GetLastError();return out;
    }
    const auto base=reinterpret_cast<uintptr_t>(region.BaseAddress);
    if(!region.RegionSize || base>cursor || region.RegionSize>UINTPTR_MAX-base || base+region.RegionSize<=cursor) {
      out.win32=ERROR_INVALID_DATA;return out;
    }
    const auto size=uint64_t(region.RegionSize);
    ++out.regions;
    if(region.State!=MEM_FREE)out.usedVirtualBytes+=size;
    if(region.Type==MEM_PRIVATE)out.privateVirtualBytes+=size;
    if(region.Type==MEM_MAPPED)out.mappedVirtualBytes+=size;
    cursor=base+region.RegionSize;
  }
  out.valid=true;return out;
}
void PrintProcessMemory(const ProcessMemory& m) noexcept {
  std::printf("{\"valid\":%s,\"win32\":%u,\"privateBytes\":%llu,\"peakPrivateBytes\":%llu,\"workingSetBytes\":%llu,"
    "\"usedVirtualBytes\":%llu,\"privateVirtualBytes\":%llu,\"mappedVirtualBytes\":%llu,\"regions\":%llu}",
    m.valid?"true":"false",unsigned(m.win32),(unsigned long long)m.privateBytes,(unsigned long long)m.peakPrivateBytes,
    (unsigned long long)m.workingSetBytes,(unsigned long long)m.usedVirtualBytes,(unsigned long long)m.privateVirtualBytes,
    (unsigned long long)m.mappedVirtualBytes,(unsigned long long)m.regions);
}
}
