#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace dxvk::war3::memory {

// Embedded in a page-owned bounded table. Rc-compatible, but final release
// returns the slot rather than deleting it. Every pending copy/draw must retain
// this object through the existing DXVK command-list object tracker.
class SnapshotSlice {
public:
  void incRef() noexcept { m_refs.fetch_add(1, std::memory_order_relaxed); }
  void decRef() noexcept {
    if (m_refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
      return;
    // Do not advertise free while m_owner is still being modified. Keeping a
    // local owner permits the page (and this object) to die on return.
    auto owner = std::move(m_owner);
    auto* revision = m_revision;
    m_available.store(true, std::memory_order_release);
    revision->fetch_add(1, std::memory_order_release);
  }
  bool available() const noexcept {
    return m_available.load(std::memory_order_acquire);
  }
  uint64_t offset() const noexcept { return m_offset; }
  uint64_t size() const noexcept { return m_size; }
private:
  template<size_t> friend class SnapshotSliceTable;
  std::atomic<uint32_t> m_refs{0};
  std::atomic<bool> m_available{true};
  std::shared_ptr<void> m_owner;
  std::atomic<uint64_t>* m_revision = nullptr;
  uint64_t m_offset = 0, m_size = 0;
};

template<size_t Slots = 2048>
class SnapshotSliceTable {
public:
  // Owner-only. Claim returns one initial reference, adopted by the caller.
  // No heap allocations, including exhaustion/reclamation paths. Metadata and
  // interval are reused only after ALL CPU/CS/GPU tracking references disappear.
  SnapshotSlice* claim(std::shared_ptr<void> owner, uint64_t offset,
                       uint64_t size) noexcept {
    if (!owner || !size || offset > UINT64_MAX - size)
      return nullptr;
    for (size_t n = 0; n < Slots; ++n) {
      const size_t i = (m_hint + n) % Slots;
      auto& slot = m_slots[i];
      if (!slot.available()) continue;
      slot.m_available.store(false, std::memory_order_relaxed);
      slot.m_offset = offset;
      slot.m_size = size;
      slot.m_owner = std::move(owner);
      slot.m_revision = &m_revision;
      slot.m_refs.store(1, std::memory_order_relaxed);
      m_revision.fetch_add(1, std::memory_order_relaxed);
      m_hint = (i + 1) % Slots;
      return &slot;
    }
    return nullptr;
  }
  bool hasSlot() const noexcept {
    if (m_slots[m_hint].available()) return true;
    for (const auto& slot : m_slots)
      if (slot.available()) return true;
    return false;
  }
  // Called only when the append tail cannot satisfy the request. A bounded
  // sorted interval census replaces moving bytes or guessing a frame delay.
  bool findHole(uint64_t capacity, uint64_t bytes, uint64_t& offset) const noexcept {
    if (!bytes || bytes > capacity) return false;
    const auto revision = m_revision.load(std::memory_order_acquire);
    // Negative cache only: never grants reuse or stores a reusable offset.
    // A concurrent retirement can make this conservatively miss once; its
    // release revision invalidates the miss on the next attempt.
    if (m_failedBytes && m_failedRevision == revision &&
        m_failedCapacity == capacity && bytes >= m_failedBytes) return false;
    struct Span { uint64_t begin, end; };
    std::array<Span, Slots> spans{};
    size_t count = 0;
    for (const auto& slot : m_slots) {
      if (slot.available()) continue;
      if (slot.m_offset > capacity || slot.m_size > capacity - slot.m_offset)
        return false;
      spans[count++] = {slot.m_offset, slot.m_offset + slot.m_size};
    }
    std::sort(spans.begin(), spans.begin() + count,
        [](const Span& a, const Span& b) { return a.begin < b.begin; });
    uint64_t cursor = 0;
    for (size_t i = 0; i < count; ++i) {
      if (spans[i].begin >= cursor && bytes <= spans[i].begin - cursor) {
        offset = cursor;
        return true;
      }
      cursor = std::max(cursor, spans[i].end);
    }
    if (cursor <= capacity && bytes <= capacity - cursor) {
      offset = cursor;
      return true;
    }
    if (m_revision.load(std::memory_order_acquire) == revision) {
      m_failedRevision = revision;
      m_failedCapacity = capacity;
      m_failedBytes = bytes;
    }
    return false;
  }
  uint64_t liveBytes() const noexcept {
    uint64_t bytes = 0;
    for (const auto& slot : m_slots)
      if (!slot.available()) bytes += slot.m_size;
    return bytes;
  }
private:
  std::array<SnapshotSlice, Slots> m_slots{};
  size_t m_hint = 0;
  std::atomic<uint64_t> m_revision{0};
  mutable uint64_t m_failedRevision = 0, m_failedCapacity = 0, m_failedBytes = 0;
};

// Shared by all geometry consumers (shadow, outline, input capture and shader
// callbacks). Buffer tracking alone does not retain a suballocation interval.
template<typename Tracker, typename Draw>
void TrackSnapshotSlices(Tracker& tracker, const Draw& draw) {
  if (draw.positionSnapshotLease != nullptr) tracker.track(draw.positionSnapshotLease);
  if (draw.indexSnapshotLease != nullptr) tracker.track(draw.indexSnapshotLease);
  if (draw.uvSnapshotLease != nullptr && draw.uvSnapshotLease != draw.positionSnapshotLease)
    tracker.track(draw.uvSnapshotLease);
}

} // namespace dxvk::war3::memory
