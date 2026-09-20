#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace dxvk::war3::render {

// Exact Stage11 snapshots are short, independent slices.  Creating one
// Vulkan buffer per slice makes the safety allocation gate reject otherwise
// valid casters.  Pages amortize the Vulkan allocation while retaining a
// distinct, immutable logical range for every cache entry.
inline constexpr uint64_t kWar3Stage11SnapshotAlignment = 256u;
inline constexpr uint64_t kWar3Stage11SnapshotResidentCapBytes = 384u << 20u;
// Keep a full resident generation below the unchanged 32-create safety gate:
// 384 MiB / 16 MiB = 24 pages.  The previous 4 MiB granularity could require
// more than 32 real Vulkan buffer creations during one high-pressure camera
// transition even though total resident capacity was still available.
inline constexpr uint64_t kWar3Stage11SnapshotPageBytes = 16u << 20u;
// 2026-09-19 实机证据（snapshot-alloc/v1）：24 页/384 MiB 常驻且 used=372.6 MiB（97%），
// 新的 512 KiB caster 几何请求被 ResidentCapacity 拒绝 4548 次 ⇒ caster 被 fail-closed 省略 ⇒ 阴影消失。
// 该上限可配置，且**封顶 32 页 = 512 MiB**（沿用本文件既有的 32-create 安全门）。
inline constexpr uint64_t kWar3Stage11SnapshotResidentCapMinBytes = 128u << 20u;
inline constexpr uint64_t kWar3Stage11SnapshotResidentCapMaxBytes = 512u << 20u;
inline uint64_t War3Stage11SnapshotResidentCapBytes() noexcept {
  static const uint64_t value = []() noexcept -> uint64_t {
    const char* s = std::getenv("DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB");
    if (!s) return kWar3Stage11SnapshotResidentCapBytes;
    const long mb = std::atol(s);
    if (mb <= 0) return kWar3Stage11SnapshotResidentCapBytes;
    uint64_t bytes = uint64_t(mb) * 1024ull * 1024ull;
    if (bytes < kWar3Stage11SnapshotResidentCapMinBytes)
      bytes = kWar3Stage11SnapshotResidentCapMinBytes;
    if (bytes > kWar3Stage11SnapshotResidentCapMaxBytes)
      bytes = kWar3Stage11SnapshotResidentCapMaxBytes;
    return bytes;
  }();
  return value;
}

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
    uint64_t requiredBytes, uint64_t capBytes) noexcept {
  uint64_t aligned = 0u;
  if (!War3TryAlignStage11SnapshotBytes(requiredBytes, aligned) ||
      aligned > capBytes) {
    return 0u;
  }
  if (aligned <= kWar3Stage11SnapshotPageBytes)
    return kWar3Stage11SnapshotPageBytes;
  const uint64_t pages =
      (aligned + kWar3Stage11SnapshotPageBytes - 1u) /
      kWar3Stage11SnapshotPageBytes;
  if (pages > capBytes / kWar3Stage11SnapshotPageBytes)
    return 0u;
  return pages * kWar3Stage11SnapshotPageBytes;
}
inline constexpr uint64_t War3Stage11SnapshotPageCapacity(
    uint64_t requiredBytes) noexcept {
  return War3Stage11SnapshotPageCapacity(
      requiredBytes, kWar3Stage11SnapshotResidentCapBytes);
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
    uint64_t residentBytes, uint64_t pageBytes, uint64_t capBytes) noexcept {
  return pageBytes != 0u && residentBytes <= capBytes &&
      pageBytes <= capBytes - residentBytes;
}
inline constexpr bool War3Stage11SnapshotCanAddPage(
    uint64_t residentBytes, uint64_t pageBytes) noexcept {
  return War3Stage11SnapshotCanAddPage(
      residentBytes, pageBytes, kWar3Stage11SnapshotResidentCapBytes);
}

enum class War3Stage11PagePublication : uint8_t {
  Success, InvalidState, NullPage, HostAllocationFailure,
};

// Owner-thread transaction: construct a private page, publish its shared_ptr,
// then commit scalar accounting. Pages is the owner's vector of shared_ptrs;
// their noexcept move gives push_back the strong exception guarantee. The
// factory must not mutate owner state. This does not acquire a GPU reuse lease.
template<typename Pages, typename MakePage>
War3Stage11PagePublication War3PublishStage11SnapshotPage(
    Pages& pages, uint64_t& nextId, uint64_t& residentBytes,
    uint64_t pageBytes, uint64_t capBytes, MakePage&& makePage) {
  if (nextId == 0u || nextId == std::numeric_limits<uint64_t>::max() ||
      !War3Stage11SnapshotCanAddPage(residentBytes, pageBytes, capBytes))
    return War3Stage11PagePublication::InvalidState;
  try {
    auto page = makePage();
    if (!page)
      return War3Stage11PagePublication::NullPage;
    page->id = nextId;
    pages.push_back(std::move(page));
  } catch (const std::bad_alloc&) {
    return War3Stage11PagePublication::HostAllocationFailure;
  }
  // No throwing operation remains after publication. Unknown exceptions are
  // deliberately not swallowed; in particular this is not device-loss recovery.
  residentBytes += pageBytes;
  ++nextId;
  return War3Stage11PagePublication::Success;
}
template<typename Pages, typename MakePage>
War3Stage11PagePublication War3PublishStage11SnapshotPage(
    Pages& pages, uint64_t& nextId, uint64_t& residentBytes,
    uint64_t pageBytes, MakePage&& makePage) {
  return War3PublishStage11SnapshotPage(pages, nextId, residentBytes,
      pageBytes, kWar3Stage11SnapshotResidentCapBytes,
      std::forward<MakePage>(makePage));
}

} // namespace dxvk::war3::render
