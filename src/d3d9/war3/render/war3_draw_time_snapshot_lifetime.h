#pragma once

#include <cstdint>

namespace dxvk::war3::render {

// Logical cache-owned slice capacity, NOT page residency or physical VRAM.
// A retained allocation counts even if this capture failed. Aliased UV is
// counted only once; externally-owned/direct bindings have capacity zero.
template<typename Entry>
uint64_t War3DrawTimeOwnedSnapshotBytes(const Entry& entry) noexcept {
  return uint64_t(entry.positionCapacity) + uint64_t(entry.indexCapacity) +
      (entry.uvSharesPositionBuffer ? 0u : uint64_t(entry.uvCapacity));
}

enum class War3DrawTimeUvCensusSpanStatus : uint8_t {
  None,
  Independent,
  PositionAlias,
  Invalid,
};

struct War3DrawTimeUvCensusSpan {
  uint64_t offset = 0u;
  uint64_t capacity = 0u;
};

// Resolve the census interval for UV without mutating the entry.  A shared UV
// aliases the position allocation: uvCapacity may be zero or stale, so the
// interval comes from positionCapacity after exact page/offset proof.  An
// external/lease alias with both page pointers null has no census-owned range.
// Inconsistent state returns Invalid so the caller can fail the sample visible.
template<typename Entry>
War3DrawTimeUvCensusSpanStatus War3ResolveDrawTimeUvCensusSpan(
    const Entry& entry, War3DrawTimeUvCensusSpan& out) noexcept {
  out = {};
  if (entry.uvSharesPositionBuffer) {
    if (entry.uvSnapshotPage != entry.positionSnapshotPage ||
        entry.uvSnapshotOffset != entry.positionSnapshotOffset) {
      return War3DrawTimeUvCensusSpanStatus::Invalid;
    }
    if (!entry.positionSnapshotPage) {
      if (entry.positionCapacity != 0u ||
          entry.positionSnapshotOffset != 0u) {
        return War3DrawTimeUvCensusSpanStatus::Invalid;
      }
      return War3DrawTimeUvCensusSpanStatus::None;
    }
    if (entry.positionCapacity == 0u)
      return War3DrawTimeUvCensusSpanStatus::Invalid;
    out.offset = entry.positionSnapshotOffset;
    out.capacity = entry.positionCapacity;
    return War3DrawTimeUvCensusSpanStatus::PositionAlias;
  }

  if (!entry.uvSnapshotPage) {
    if (entry.uvCapacity != 0u || entry.uvSnapshotOffset != 0u)
      return War3DrawTimeUvCensusSpanStatus::Invalid;
    return War3DrawTimeUvCensusSpanStatus::None;
  }
  if (entry.uvCapacity == 0u)
    return War3DrawTimeUvCensusSpanStatus::Invalid;
  out.offset = entry.uvSnapshotOffset;
  out.capacity = entry.uvCapacity;
  return War3DrawTimeUvCensusSpanStatus::Independent;
}

// Revoke a position backing and its UV alias as one transition. Drop the
// cache's references only: pending CS closures and DxvkContext resource
// tracking still retain their own Buffer/allocation references. This neither
// rewinds a page nor authorizes reuse of a GPU-in-flight range.
template<typename Entry>
void War3ReleaseDrawTimePositionBacking(Entry& entry) noexcept {
  if (entry.uvSharesPositionBuffer) {
    entry.uvBuffer = nullptr;
    entry.uvPinnedAllocation = nullptr;
    entry.uvSnapshotPage.reset();
    entry.uvSnapshotOffset = 0u;
    entry.uvInfo = {};
    entry.uvStride = 0u;
    entry.uvOffset = 0u;
    entry.uvFormat = {};
    entry.uvCapacity = 0u;
    entry.uvSharesPositionBuffer = false;
  }
  entry.positionBuffer = nullptr;
  entry.positionPinnedAllocation = nullptr;
  entry.positionSnapshotPage.reset();
  entry.positionSnapshotOffset = 0u;
  entry.positionInfo = {};
  entry.positionCapacity = 0u;
  entry.ownedGpuBytes = War3DrawTimeOwnedSnapshotBytes(entry);
}

// Release only the cache's UV backing fields after a completed capture whose
// current draw has no usable UV source. This intentionally does not touch the
// position page/alias fields, page allocation, or budget. Pending EmitCs
// closures and draw packets keep their own Rc<DxvkBuffer> owners.
template<typename Entry>
void War3ReleaseDrawTimeUvBacking(Entry& entry) noexcept {
  entry.uvBuffer = nullptr;
  entry.uvPinnedAllocation = nullptr;
  entry.uvSnapshotPage.reset();
  entry.uvSnapshotOffset = 0u;
  entry.uvInfo = {};
  entry.uvStride = 0u;
  entry.uvOffset = 0u;
  entry.uvFormat = {};
  entry.uvSharesPositionBuffer = false;
  entry.uvCapacity = 0u;
  entry.uvSourceProof = {};
  entry.ownedGpuBytes = War3DrawTimeOwnedSnapshotBytes(entry);
}

// Owner-thread, stack-only transaction shared by production and CPU tests.
// Last attempt is diagnostic; only explicit successful completion refreshes
// the frame/working-set lifetime. All early breaks and exceptions account for
// retained capacity without publishing an incomplete capture.
template<typename Entry>
class War3DrawTimeSnapshotCaptureAttempt {
public:
  War3DrawTimeSnapshotCaptureAttempt(Entry& entry, uint64_t frame) noexcept
  : m_entry(entry), m_frame(frame) {
    m_entry.captureComplete = false;
    m_entry.lastAttemptFrameSerial = frame;
  }
  War3DrawTimeSnapshotCaptureAttempt(const War3DrawTimeSnapshotCaptureAttempt&) = delete;
  War3DrawTimeSnapshotCaptureAttempt& operator=(const War3DrawTimeSnapshotCaptureAttempt&) = delete;
  ~War3DrawTimeSnapshotCaptureAttempt() {
    if (!m_committed)
      m_entry.captureComplete = false;
    m_entry.ownedGpuBytes = War3DrawTimeOwnedSnapshotBytes(m_entry);
  }
  bool commit() noexcept {
    if (!m_entry.HasCompleteBacking())
      return false;
    m_entry.frameSerial = m_frame;
    m_entry.lastAccessFrameSerial = m_frame;
    m_entry.ownedGpuBytes = War3DrawTimeOwnedSnapshotBytes(m_entry);
    m_committed = true;
    return true;
  }
private:
  Entry& m_entry;
  uint64_t m_frame;
  bool m_committed = false;
};

} // namespace dxvk::war3::render
