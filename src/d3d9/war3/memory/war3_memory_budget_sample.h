#pragma once
#include "war3_adaptive_memory_budget.h"
#include "../../../dxvk/dxvk_device.h"

namespace dxvk::war3::memory {
inline AdaptiveBudgetInput SampleShadowMemoryBudget(DxvkDevice* device,
    uint32_t heapIndex, uint64_t resident, uint64_t hardCap,
    uint64_t fallbackCap, uint64_t pageBytes, uint32_t quotaEighths) {
  AdaptiveBudgetInput in{};
  in.resident = resident; in.hardCap = hardCap; in.fallbackCap = fallbackCap;
  in.pageBytes = pageBytes; in.quotaEighths = quotaEighths;
  MEMORYSTATUSEX status{}; status.dwLength = sizeof(status);
  in.vaValid = GlobalMemoryStatusEx(&status) != FALSE;
  in.availableVA = in.vaValid ? status.ullAvailVirtual : 0;
  in.supported = device && device->features().extMemoryBudget;
  if (in.supported && heapIndex != UINT32_MAX) {
    const auto info = device->adapter()->getMemoryHeapInfo();
    if (heapIndex < info.heapCount) {
      const auto& heap = info.heaps[heapIndex];
      in.heapValid = (heap.heapFlags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      in.heapSize = heap.heapSize; in.budget = heap.memoryBudget;
      in.committed = heap.memoryCommitted;
    }
  }
  return in;
}
}
