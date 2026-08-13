#pragma once

#include <cstdint>

namespace dxvk::war3::render {

enum class War3Stage11PositionAllocationClass : uint8_t {
  NewEntry = 0u,
  ExistingMissingBacking,
  CapacityGrowth,
  GpuSkinLeaseDetach,
};

constexpr War3Stage11PositionAllocationClass
ClassifyWar3Stage11PositionAllocation(
    bool entryExisted, bool hadPositionBuffer,
    uint64_t previousCapacity, uint64_t requiredBytes,
    bool replacingGpuSkinLease) noexcept {
  if (!entryExisted)
    return War3Stage11PositionAllocationClass::NewEntry;
  if (replacingGpuSkinLease)
    return War3Stage11PositionAllocationClass::GpuSkinLeaseDetach;
  if (hadPositionBuffer && previousCapacity < requiredBytes)
    return War3Stage11PositionAllocationClass::CapacityGrowth;
  return War3Stage11PositionAllocationClass::ExistingMissingBacking;
}

} // namespace dxvk::war3::render
