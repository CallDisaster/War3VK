#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace dxvk::war3::render {

// Exact Stage11 snapshots are short, independent slices.  Creating one
// Vulkan buffer per slice makes the safety allocation gate reject otherwise
// valid casters.  Pages amortize the Vulkan allocation while retaining a
// distinct, immutable logical range for every cache entry.
inline constexpr uint64_t kWar3Stage11SnapshotAlignment = 256u;
inline constexpr uint64_t kWar3Stage11SnapshotPageBytes = 4u << 20u;
inline constexpr uint64_t kWar3Stage11SnapshotResidentCapBytes = 384u << 20u;

struct War3Stage11SnapshotSuballocation {
  bool valid = false;
  uint64_t offset = 0u;
  uint64_t capacity = 0u;
  uint64_t nextUsed = 0u;
};

inline constexpr bool War3TryAlignStage11SnapshotBytes(
    uint64_t value, uint64_t& aligned) noexcept {
  constexpr uint64_t mask = kWar3Stage11SnapshotAlignment - 1u;
  if (value == 0u || value > std::numeric_limits<uint64_t>::max() - mask) {
    aligned = 0u;
    return false;
  }
  aligned = (value + mask) & ~mask;
  return aligned >= value;
}

inline constexpr uint64_t War3Stage11SnapshotPageCapacity(
    uint64_t requiredBytes) noexcept {
  uint64_t aligned = 0u;
  if (!War3TryAlignStage11SnapshotBytes(requiredBytes, aligned) ||
      aligned > kWar3Stage11SnapshotResidentCapBytes) {
    return 0u;
  }
  if (aligned <= kWar3Stage11SnapshotPageBytes)
    return kWar3Stage11SnapshotPageBytes;
  const uint64_t pages =
      (aligned + kWar3Stage11SnapshotPageBytes - 1u) /
      kWar3Stage11SnapshotPageBytes;
  if (pages > kWar3Stage11SnapshotResidentCapBytes /
                  kWar3Stage11SnapshotPageBytes) {
    return 0u;
  }
  return pages * kWar3Stage11SnapshotPageBytes;
}

inline constexpr War3Stage11SnapshotSuballocation
War3PlanStage11SnapshotSuballocation(uint64_t used, uint64_t capacity,
                                      uint64_t requiredBytes) noexcept {
  War3Stage11SnapshotSuballocation result = {};
  uint64_t alignedUsed = 0u;
  uint64_t alignedBytes = 0u;
  if (used > capacity ||
      !War3TryAlignStage11SnapshotBytes(
          used == 0u ? kWar3Stage11SnapshotAlignment : used, alignedUsed) ||
      !War3TryAlignStage11SnapshotBytes(requiredBytes, alignedBytes)) {
    return result;
  }
  if (used == 0u)
    alignedUsed = 0u;
  if (alignedUsed > capacity || alignedBytes > capacity - alignedUsed)
    return result;
  result.valid = true;
  result.offset = alignedUsed;
  result.capacity = alignedBytes;
  result.nextUsed = alignedUsed + alignedBytes;
  return result;
}

inline constexpr bool War3Stage11SnapshotCanAddPage(
    uint64_t residentBytes, uint64_t pageBytes) noexcept {
  return pageBytes != 0u &&
      residentBytes <= kWar3Stage11SnapshotResidentCapBytes &&
      pageBytes <= kWar3Stage11SnapshotResidentCapBytes - residentBytes;
}

} // namespace dxvk::war3::render
