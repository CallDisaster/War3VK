#pragma once
#include <cstdint>
#include <limits>

namespace dxvk::war3::memory {
// Owner-thread telemetry only. Read the live pool, never carry an old maximum
// across reset/reclamation. Callers must not use this as a retirement proof.
template<class Pages, class Stats>
void SampleSnapshotPoolStats(const Pages& pages, uint64_t resident,
                             uint64_t reclaimed, Stats& stats) noexcept {
  uint64_t used = 0;
  for (const auto& page : pages) {
    const uint64_t bytes = uint64_t(page->used);
    used = bytes > std::numeric_limits<uint64_t>::max() - used
        ? std::numeric_limits<uint64_t>::max() : used + bytes;
  }
  stats.drawTimeSnapshotPageResidentBytes = resident;
  stats.drawTimeSnapshotPageUsedBytes = used;
  stats.drawTimeSnapshotPageReclaimedCount = reclaimed;
}
}
