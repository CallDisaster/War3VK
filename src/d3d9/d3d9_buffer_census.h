#pragma once

#include "d3d9_caller_census.h"
#include <ostream>

namespace dxvk::d3d9::migration {

// Vertex/index buffer creation classes and their Lock patterns, for the x64
// proxy's buffer path. Kept apart from the caller-census v1 record. Creation
// is attributed by the creating entry's return address, each Lock by the Lock
// entry's. The per-buffer cursor lives in the buffer object and is touched
// only inside this census's own bounded try-cell, like everything else here.

inline constexpr std::size_t BufferClassCapacity = 16;
// Upper bounds of the Lock size bins; one last bin for anything larger.
inline constexpr std::array<uint32_t, 7> LockSizeBounds = {{ 256, 1024, 4096, 16384, 65536, 262144, 1048576 }};
inline constexpr uint32_t NotClassified = std::numeric_limits<uint32_t>::max();

// D3DLOCK_* values, kept local so the component builds without d3d9 headers.
inline constexpr uint32_t LockReadOnly = 0x10, LockNoSysLock = 0x800, LockNoOverwrite = 0x1000,
  LockDiscard = 0x2000, LockDoNotWait = 0x4000, LockNoDirtyUpdate = 0x8000;

enum class BufferKind : uint32_t { Vertex, Index, Count };

struct BufferFacts {
  BufferKind kind = BufferKind::Vertex;
  uint32_t length = 0;
  uint32_t usage = 0;
  uint32_t pool = 0;
  uint32_t format = 0;  // D3DFORMAT of an index buffer, 0 for vertex data
  uint32_t fvf = 0;
};

// Owned by one buffer object. lastEnd/written are byte offsets in it.
struct BufferCursor {
  uint32_t classIndex = NotClassified;
  uint32_t lastEnd = 0;   // end of the previous observed Lock range
  uint32_t written = 0;   // highest end of any observed Lock range
  uint32_t depth = 0;     // observed Locks minus observed Unlocks
  bool locked = false;    // at least one Lock observed
};

struct BufferClass {
  uint32_t caller = 0;    // creator
  BufferFacts facts;
  uint64_t created = 0;
  std::array<uint64_t, CallerCount> lockCallers = {};
  uint64_t locks = 0, unlocks = 0, bytes = 0;
  uint32_t maxBytes = 0;
  uint64_t discard = 0, noOverwrite = 0, readOnly = 0, noSysLock = 0, doNotWait = 0, noDirtyUpdate = 0, otherFlags = 0;
  uint64_t sizeZero = 0;      // SizeToLock 0: to the end of the buffer
  uint64_t whole = 0;         // the range covers the whole buffer
  uint64_t outOfRange = 0;    // the range does not fit the buffer
  uint64_t nested = 0;        // Lock while this buffer is already locked
  uint64_t sequential = 0;    // starts where the previous Lock of this buffer ended
  uint64_t wrapped = 0;       // starts before that end
  uint64_t wrappedDiscard = 0;
  uint64_t beyondWritten = 0; // starts past every byte any Lock covered so far
  std::array<uint64_t, LockSizeBounds.size() + 1> sizeBins = {};
  uint64_t framesLocked = 0, maxLocksPerFrame = 0, maxBytesPerFrame = 0;
  uint64_t frame = 0, frameLocks = 0, frameBytes = 0;  // the frame being accumulated
};

struct BufferSnapshot {
  std::array<BufferClass, BufferClassCapacity> classes = {};
  uint32_t classCount = 0;
  uint64_t presents = 0;
  uint64_t classesDropped = 0;
  uint64_t unclassifiedLocks = 0;
  uint64_t unclassifiedUnlocks = 0;
  uint64_t saturatedUpdates = 0;
};

class BufferCensus {
public:
  void noteCreate(BufferCursor& cursor, Caller caller, const BufferFacts& facts) noexcept {
    if (std::size_t(caller) >= CallerCount || std::size_t(facts.kind) >= std::size_t(BufferKind::Count)) return;
    auto& s = m_snapshot;
    uint32_t index = 0;
    while (index < s.classCount && !same(s.classes[index], caller, facts)) ++index;
    if (index == s.classCount) {
      if (s.classCount == s.classes.size()) {
        add(s.classesDropped);
        return;
      }
      auto& entry = s.classes[s.classCount++];
      entry.caller = uint32_t(caller);
      entry.facts = facts;
    }
    add(s.classes[index].created);
    cursor.classIndex = index;
  }

  void noteLock(BufferCursor& cursor, Caller caller, uint32_t offset, uint32_t size, uint32_t flags) noexcept {
    auto& s = m_snapshot;
    if (cursor.classIndex >= s.classCount || std::size_t(caller) >= CallerCount) {
      add(s.unclassifiedLocks);
      return;
    }
    auto& c = s.classes[cursor.classIndex];
    const uint32_t length = c.facts.length;
    add(c.lockCallers[std::size_t(caller)]);
    add(c.locks);
    if (flags & LockDiscard) add(c.discard);
    if (flags & LockNoOverwrite) add(c.noOverwrite);
    if (flags & LockReadOnly) add(c.readOnly);
    if (flags & LockNoSysLock) add(c.noSysLock);
    if (flags & LockDoNotWait) add(c.doNotWait);
    if (flags & LockNoDirtyUpdate) add(c.noDirtyUpdate);
    if (flags & ~(LockDiscard | LockNoOverwrite | LockReadOnly | LockNoSysLock | LockDoNotWait | LockNoDirtyUpdate))
      add(c.otherFlags);
    if (!size) add(c.sizeZero);
    const bool fits = offset <= length && size <= length - offset;
    if (!fits) add(c.outOfRange);
    const uint32_t bytes = !fits ? 0u : size ? size : length - offset;
    const uint32_t end = offset + bytes;
    if (fits && !offset && bytes == length) add(c.whole);
    if (cursor.depth) add(c.nested);
    if (cursor.locked) {
      if (offset == cursor.lastEnd) add(c.sequential);
      else if (offset < cursor.lastEnd) {
        add(c.wrapped);
        if (flags & LockDiscard) add(c.wrappedDiscard);
      }
    }
    if (offset > cursor.written) add(c.beyondWritten);
    add(c.bytes, bytes);
    if (bytes > c.maxBytes) c.maxBytes = bytes;
    if (fits) {
      std::size_t bin = 0;
      while (bin < LockSizeBounds.size() && bytes > LockSizeBounds[bin]) ++bin;
      add(c.sizeBins[bin]);
    }
    if (!c.frameLocks || c.frame != s.presents) {
      c.frame = s.presents;
      c.frameLocks = c.frameBytes = 0;
      add(c.framesLocked);
    }
    add(c.frameLocks);
    add(c.frameBytes, bytes);
    if (c.frameLocks > c.maxLocksPerFrame) c.maxLocksPerFrame = c.frameLocks;
    if (c.frameBytes > c.maxBytesPerFrame) c.maxBytesPerFrame = c.frameBytes;
    cursor.locked = true;
    if (cursor.depth != std::numeric_limits<uint32_t>::max()) ++cursor.depth;
    if (fits) {
      cursor.lastEnd = end;
      if (end > cursor.written) cursor.written = end;
    }
  }

  void noteUnlock(BufferCursor& cursor) noexcept {
    auto& s = m_snapshot;
    if (cursor.classIndex >= s.classCount) {
      add(s.unclassifiedUnlocks);
      return;
    }
    add(s.classes[cursor.classIndex].unlocks);
    if (cursor.depth) --cursor.depth;
  }

  void notePresent() noexcept { add(m_snapshot.presents); }

  const BufferSnapshot& snapshot() const noexcept { return m_snapshot; }

private:
  static bool same(const BufferClass& c, Caller caller, const BufferFacts& f) noexcept {
    return c.caller == uint32_t(caller) && c.facts.kind == f.kind && c.facts.length == f.length
      && c.facts.usage == f.usage && c.facts.pool == f.pool && c.facts.format == f.format && c.facts.fvf == f.fvf;
  }

  void add(uint64_t& value, uint64_t amount = 1) noexcept {
    if (SaturatingAdd(value, amount))
      SaturatingAdd(m_snapshot.saturatedUpdates);
  }

  BufferSnapshot m_snapshot;
};

struct BufferCensusCopy {
  BufferSnapshot values;
  uint64_t sequence = 0;
  uint32_t recordBusy = 0;  // independent atomic read, not coherent with values
};

// Same production boundary as ConcurrentCallerCensus: one bounded try-cell,
// no waiting or retry; a refused observation changes nothing but recordBusy.
class ConcurrentBufferCensus {
public:
  void setRanges(const CallerRanges& ranges) noexcept { m_ranges = ranges; }  // cold, before publication
  bool gameModuleKnown() const noexcept { return m_ranges.game.end > m_ranges.game.begin; }

  void noteCreate(BufferCursor& cursor, const BufferFacts& facts, const void* caller) noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.noteCreate(cursor, m_ranges.classify(caller), facts);
  }
  void noteLock(BufferCursor& cursor, uint32_t offset, uint32_t size, uint32_t flags, const void* caller) noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.noteLock(cursor, m_ranges.classify(caller), offset, size, flags);
  }
  void noteUnlock(BufferCursor& cursor) noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.noteUnlock(cursor);
  }
  void notePresent() noexcept {
    Cell cell(*this);
    if (cell.acquired) m_census.notePresent();
  }

  bool trySnapshot(BufferCensusCopy& out) noexcept {
    Cell cell(*this);
    if (!cell.acquired || m_sequence == std::numeric_limits<uint64_t>::max()) return false;
    out.values = m_census.snapshot();
    out.sequence = ++m_sequence;
    out.recordBusy = m_recordBusy.load(std::memory_order_relaxed);
    return true;
  }

private:
  struct Cell {
    explicit Cell(ConcurrentBufferCensus& owner) noexcept
    : owner(owner), acquired(!owner.m_cell.test_and_set(std::memory_order_acquire)) {
      if (!acquired) {
        auto value = owner.m_recordBusy.load(std::memory_order_relaxed);
        while (value != std::numeric_limits<uint32_t>::max()
            && !owner.m_recordBusy.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) { }
      }
    }
    ~Cell() noexcept { if (acquired) owner.m_cell.clear(std::memory_order_release); }
    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;
    ConcurrentBufferCensus& owner;
    const bool acquired;
  };

  std::atomic_flag m_cell = ATOMIC_FLAG_INIT;
  CallerRanges m_ranges;
  BufferCensus m_census;
  uint64_t m_sequence = 0;
  std::atomic<uint32_t> m_recordBusy { 0 };
};

static_assert(std::is_trivially_copyable<BufferSnapshot>::value);
static_assert(sizeof(BufferCensusCopy) <= 8192, "Copied buffer census must remain fixed and small");

// Cold serialization of an owned copy only.
inline void WriteBufferCensus(std::ostream& out, const BufferCensusCopy& copy,
    uint64_t deviceOrdinal, const char* reason, bool gameModuleKnown) {
  const auto& s = copy.values;
  out << "WARVK_D3D9_BUFFER_CENSUS {\"schema\":\"warvk-d3d9-buffer-census/v1\""
      << ",\"deviceOrdinal\":" << deviceOrdinal
      << ",\"snapshotSequence\":" << copy.sequence
      << ",\"reason\":\"" << reason << "\""
      << ",\"attribution\":\"return-address-module\",\"gameModuleKnown\":" << (gameModuleKnown ? "true" : "false")
      << ",\"recordBusy\":" << copy.recordBusy
      << ",\"presents\":" << s.presents
      << ",\"lockSizeBounds\":";
  WriteCensusArray(out, LockSizeBounds);
  out << ",\"classes\":[";
  for (uint32_t i = 0; i < s.classCount; ++i) {
    const auto& c = s.classes[i];
    out << (i ? "," : "") << "{\"caller\":\"" << CallerNames[c.caller < CallerCount ? c.caller : 2]
        << "\",\"kind\":\"" << (c.facts.kind == BufferKind::Index ? "index" : "vertex")
        << "\",\"length\":" << c.facts.length << ",\"usage\":" << c.facts.usage << ",\"pool\":" << c.facts.pool
        << ",\"format\":" << c.facts.format << ",\"fvf\":" << c.facts.fvf << ",\"created\":" << c.created
        << ",\"lockCallers\":";
    WriteCensusArray(out, c.lockCallers);
    out << ",\"locks\":" << c.locks << ",\"unlocks\":" << c.unlocks << ",\"bytes\":" << c.bytes
        << ",\"maxBytes\":" << c.maxBytes
        << ",\"flags\":{\"discard\":" << c.discard << ",\"noOverwrite\":" << c.noOverwrite
        << ",\"readOnly\":" << c.readOnly << ",\"noSysLock\":" << c.noSysLock << ",\"doNotWait\":" << c.doNotWait
        << ",\"noDirtyUpdate\":" << c.noDirtyUpdate << ",\"other\":" << c.otherFlags << '}'
        << ",\"sizeZero\":" << c.sizeZero << ",\"whole\":" << c.whole << ",\"outOfRange\":" << c.outOfRange
        << ",\"nested\":" << c.nested << ",\"sequential\":" << c.sequential << ",\"wrapped\":" << c.wrapped
        << ",\"wrappedDiscard\":" << c.wrappedDiscard << ",\"beyondWritten\":" << c.beyondWritten
        << ",\"sizeBins\":";
    WriteCensusArray(out, c.sizeBins);
    out << ",\"framesLocked\":" << c.framesLocked << ",\"maxLocksPerFrame\":" << c.maxLocksPerFrame
        << ",\"maxBytesPerFrame\":" << c.maxBytesPerFrame << '}';
  }
  out << "],\"classesDropped\":" << s.classesDropped
      << ",\"unclassifiedLocks\":" << s.unclassifiedLocks
      << ",\"unclassifiedUnlocks\":" << s.unclassifiedUnlocks
      << ",\"saturatedUpdates\":" << s.saturatedUpdates << '}';
}

}
