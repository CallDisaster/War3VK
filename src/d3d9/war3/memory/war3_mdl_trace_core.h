#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace dxvk::war3::memory::mdltrace {

// A bounded diagnostic journal, NOT an allocator or proof of native lifetime.
// Callers supply owned values. This header never reads game pointers, invokes
// external code, allocates, logs, or performs I/O. The mapped file is read only
// after the producing process has exited, not as a concurrent shared snapshot.
inline constexpr uint32_t kMagic = 0x4D444C54u;
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kHeaderWords = 64;
inline constexpr uint32_t kEventWords = 32;
inline constexpr uint32_t kEventCapacity = 16384;
inline constexpr uint32_t kDirectoryCapacity = 4096;
inline constexpr uint32_t kProbeLimit = 16;

enum class Kind : uint32_t {
  Alloc = 1,
  FreeBegin = 2,
  FreeEnd = 3,
  ReallocBegin = 4,
  ReallocEnd = 5,
  BucketReturn = 6,
  InsertBegin = 7,
  InsertEnd = 8,
  UnlinkBegin = 9,
  UnlinkEnd = 10,
};

enum FactsKnown : uint32_t {
  NodeKnown = 1u << 0,
  TableKnown = 1u << 1,
  HeadKnown = 1u << 2,
  ReadFailure = 1u << 3,
  ReleasedOld = 1u << 4,
};

// Header[0..12] and [22..26] are installed by the adapter. Core preserves them.
// Header[13..21] are independent coverage-loss observations, not disjoint
// event-loss classes. CounterOverflow and BusyOverflow are sticky booleans.
enum class HeaderField : uint32_t {
  Committed = 5,
  Busy = 13,
  BusyOverflow = 14,
  JournalFull = 15,
  DirectoryFull = 16,
  StaleCompletion = 17,
  UnknownLifetime = 18,
  CounterOverflow = 19,
  OverlapConflict = 20,
  ReadFailures = 21,
};

// Input POD. The adapter fills all actual-call facts before entering Core.
// 0 sequence; 1 kind; 2 thread; 3 callerRva; 4 ptr; 5 otherPtr;
// 6 allocationSerial; 7 operation; 8 size; 9 flags; 10 sourceRva;
// 11 argList; 12 native result; 13 factsKnown; 14..18 node DWORDs;
// 19..21 table DWORDs; 22..24 head DWORDs; 25..31 reserved zero.
// ReallocEnd uses 4=old, 5=result, 6=OLD serial, and 24=NEW serial;
// it has no head payload (22/23 zero). A zero new serial means not tracked,
// not a null allocation. Boundary kinds have operation=0 and are NOT paired.
struct Event {
  uint32_t words[kEventWords] = {};
};

// The on-disk representation is deliberately constrained to the supported
// 32-bit atomic ABI. No raw writes/memset/memcpy are permitted while live.
// All event words are atomic so publishing word 0 last has an explicit release
// store; this does not authorize concurrent file readers or certify persistence
// after a forced kill/power loss. An incomplete tail must remain uncertified.
struct Header {
  std::atomic<uint32_t> words[kHeaderWords] {};
};
struct JournalEvent {
  std::atomic<uint32_t> words[kEventWords] {};
};
struct Journal {
  Header header;
  JournalEvent events[kEventCapacity];
};

static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
static_assert(alignof(std::atomic<uint32_t>) == alignof(uint32_t));
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::is_standard_layout<Header>::value);
static_assert(std::is_standard_layout<JournalEvent>::value);
static_assert(std::is_standard_layout<Journal>::value);
static_assert(std::is_trivially_copyable<Event>::value);
static_assert(sizeof(Header) == 256);
static_assert(sizeof(Event) == 128);
static_assert(sizeof(JournalEvent) == 128);
static_assert(offsetof(Journal, events) == 256);
static_assert(sizeof(Journal) == 2u * 1024u * 1024u + 256u);

struct Token {
  bool tracked = false;
  bool admitted = false;
  uint32_t ptr = 0;
  uint32_t serial = 0;
  uint32_t operation = 0;
  uint32_t kind = 0;
};
static_assert(std::is_trivially_copyable<Token>::value);

struct CoreTestAccess;

class Core {
public:
  // One Core per fresh, zero-event journal. Root initializes immutable metadata
  // before publication; constructing Core must not erase that installation.
  // Reattaching to a previous journal is intentionally unsupported: it would
  // otherwise reuse allocation identities after losing the private directory.
  explicit Core(Journal& journal) noexcept
  : m_journal(journal), m_resumed(load(HeaderField::Committed) != 0) { }

  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  bool noteAllocation(Event event) noexcept {
    Cell cell(*this);
    if (!cell || !hasRoom())
      return false;
    if (event.words[1] != uint32_t(Kind::Alloc))
      return invalidAdapterEvent();
    event.words[6] = trackAllocation(event.words[4]);
    event.words[7] = 0;
    publish(event);
    return true;
  }

  Token beginOperation(Event event, bool includeUnknown) noexcept {
    Cell cell(*this);
    if (!cell)
      return {};
    const auto kind = event.words[1];
    if (kind != uint32_t(Kind::FreeBegin)
     && kind != uint32_t(Kind::ReallocBegin)) {
      invalidAdapterEvent();
      return {};
    }
    Entry* entry = find(event.words[4]);
    if (!entry && !includeUnknown)
      return {};
    if (!hasRoom())
      return {};
    const uint32_t operation = nextIdentity(m_nextOperation);
    if (!operation)
      return {};

    Token token { entry != nullptr, true, event.words[4],
      entry ? entry->serial : 0, operation, kind };
    if (entry) {
      // Preserve the first pending token. A second overlapping operation is
      // observable but cannot steal completion rights from the first one.
      if (entry->operation) {
        increment(HeaderField::OverlapConflict);
      } else {
        entry->operation = operation;
        entry->operationKind = kind;
      }
    } else if (token.ptr) {
      increment(HeaderField::UnknownLifetime);
    }
    event.words[6] = token.serial;
    event.words[7] = token.operation;
    publish(event);
    return token;
  }

  // A completion is an observed return, NOT proof that native memory was
  // released. FreeEnd retires OUR tracking conservatively regardless of its
  // raw native result. Realloc uses only the adapter's explicit releasedOld
  // fact; a failed call leaves old tracking intact. A successful tracked
  // realloc creates a fresh observation serial even for an in-place return.
  // Unknown completions are log-only (new serial=0): without an allocation
  // identity, they may never overwrite a newer tracked allocation.
  bool endOperation(Token token, Event event, bool releasedOld) noexcept {
    if (!token.admitted)
      return false;
    Cell cell(*this);
    if (!cell || !hasRoom())
      return false;
    const uint32_t expected = token.kind == uint32_t(Kind::FreeBegin)
      ? uint32_t(Kind::FreeEnd) : uint32_t(Kind::ReallocEnd);
    if ((token.kind != uint32_t(Kind::FreeBegin)
      && token.kind != uint32_t(Kind::ReallocBegin))
     || event.words[1] != expected || event.words[4] != token.ptr
     || !token.operation
     || (expected == uint32_t(Kind::FreeEnd) && releasedOld))
      return invalidAdapterEvent();

    event.words[6] = token.serial;
    event.words[7] = token.operation;
    event.words[13] = (event.words[13] & ~uint32_t(ReleasedOld))
      | (releasedOld ? uint32_t(ReleasedOld) : 0u);
    const bool realloc = expected == uint32_t(Kind::ReallocEnd);
    if (realloc) {
      event.words[22] = 0;
      event.words[23] = 0;
      event.words[24] = 0;
      event.words[13] &= ~uint32_t(HeadKnown);
    }

    if (!token.tracked) {
      publish(event);
      return true;
    }
    Entry* entry = find(token.ptr);
    if (!entry || entry->serial != token.serial
     || entry->operation != token.operation
     || entry->operationKind != token.kind) {
      increment(HeaderField::StaleCompletion);
      publish(event);
      return false;
    }

    entry->operation = 0;
    entry->operationKind = 0;
    if (!realloc || releasedOld || event.words[5] == token.ptr)
      retire(*entry);
    if (realloc && event.words[5])
      event.words[24] = trackAllocation(event.words[5]);
    publish(event);
    return true;
  }

  // A boundary is a single owned observation, not a begin/end invocation pair.
  // A missing allocation may be logged only when the caller independently
  // selected the exact source/table. ptr=0 (notably InsertBegin) has no lifetime
  // to classify and must not be counted as an unknown non-null allocation.
  bool noteBoundary(Event event, bool includeUnknown) noexcept {
    Cell cell(*this);
    if (!cell)
      return false;
    if (event.words[1] < uint32_t(Kind::BucketReturn)
     || event.words[1] > uint32_t(Kind::UnlinkEnd))
      return invalidAdapterEvent();
    Entry* entry = find(event.words[4]);
    if (!entry && !includeUnknown)
      return false;
    if (!hasRoom())
      return false;
    if (!entry && event.words[4])
      increment(HeaderField::UnknownLifetime);
    event.words[6] = entry ? entry->serial : 0;
    event.words[7] = 0;
    publish(event);
    return true;
  }

private:
  friend struct CoreTestAccess;

  enum class State : uint32_t { Empty, Live, Tombstone };
  struct Entry {
    State state = State::Empty;
    uint32_t ptr = 0;
    uint32_t serial = 0;
    uint32_t operation = 0;
    uint32_t operationKind = 0;
  };

  class Cell {
  public:
    explicit Cell(Core& core) noexcept : m_core(core),
      m_acquired(!core.m_cell.test_and_set(std::memory_order_acquire)) {
      if (!m_acquired)
        core.noteBusy();
    }
    ~Cell() {
      if (m_acquired)
        m_core.m_cell.clear(std::memory_order_release);
    }
    explicit operator bool() const noexcept { return m_acquired; }
    Cell(const Cell&) = delete;
    Cell& operator=(const Cell&) = delete;
  private:
    Core& m_core;
    bool m_acquired;
  };

  uint32_t load(HeaderField field) const noexcept {
    return m_journal.header.words[uint32_t(field)].load(std::memory_order_relaxed);
  }
  void store(HeaderField field, uint32_t value) noexcept {
    m_journal.header.words[uint32_t(field)].store(value, std::memory_order_relaxed);
  }
  void increment(HeaderField field) noexcept {
    const uint32_t value = load(field);
    if (value == std::numeric_limits<uint32_t>::max())
      store(HeaderField::CounterOverflow, 1);
    else
      store(field, value + 1);
  }
  void noteBusy() noexcept {
    // No retry loop on contention. If this finite-width concurrent observation
    // overflows, the sticky flags make the numeric counter unusable, not valid
    // wrapped evidence. Every sequence/identity below instead stops at zero.
    const auto old = m_journal.header.words[uint32_t(HeaderField::Busy)]
      .fetch_add(1, std::memory_order_relaxed);
    if (old == std::numeric_limits<uint32_t>::max()) {
      store(HeaderField::BusyOverflow, 1);
      store(HeaderField::CounterOverflow, 1);
    }
  }
  bool hasRoom() noexcept {
    if (m_resumed) {
      increment(HeaderField::UnknownLifetime);
      return false;
    }
    if (load(HeaderField::Committed) >= kEventCapacity) {
      increment(HeaderField::JournalFull);
      return false;
    }
    return true;
  }
  bool invalidAdapterEvent() noexcept {
    // Adapter contract violations share the fail-visible conflict latch/count;
    // no malformed event or lifetime mutation is emitted.
    increment(HeaderField::OverlapConflict);
    return false;
  }
  uint32_t nextIdentity(uint32_t& next) noexcept {
    if (!next) {
      store(HeaderField::CounterOverflow, 1);
      return 0;
    }
    const uint32_t result = next;
    next = result == std::numeric_limits<uint32_t>::max() ? 0 : result + 1;
    return result;
  }
  static uint32_t hash(uint32_t ptr) noexcept {
    return (((ptr >> 4) ^ (ptr >> 16)) * 2654435761u)
      & (kDirectoryCapacity - 1);
  }
  Entry* find(uint32_t ptr) noexcept {
    if (!ptr)
      return nullptr;
    const uint32_t start = hash(ptr);
    for (uint32_t probe = 0; probe < kProbeLimit; ++probe) {
      Entry& entry = m_directory[(start + probe) & (kDirectoryCapacity - 1)];
      if (entry.state == State::Empty)
        return nullptr;
      if (entry.state == State::Live && entry.ptr == ptr)
        return &entry;
      // A deletion is a tombstone. Never stop lookup at one.
    }
    return nullptr;
  }
  Entry* findSlot(uint32_t ptr) noexcept {
    Entry* firstTombstone = nullptr;
    const uint32_t start = hash(ptr);
    for (uint32_t probe = 0; probe < kProbeLimit; ++probe) {
      Entry& entry = m_directory[(start + probe) & (kDirectoryCapacity - 1)];
      if (entry.state == State::Live && entry.ptr == ptr)
        return &entry;
      if (entry.state == State::Tombstone && !firstTombstone)
        firstTombstone = &entry;
      if (entry.state == State::Empty)
        return firstTombstone ? firstTombstone : &entry;
    }
    return firstTombstone;
  }
  uint32_t trackAllocation(uint32_t ptr) noexcept {
    if (!ptr)
      return 0;
    Entry* entry = findSlot(ptr);
    if (!entry) {
      increment(HeaderField::DirectoryFull);
      return 0;
    }
    const uint32_t serial = nextIdentity(m_nextSerial);
    if (!serial) {
      // A newly observed allocation at this address invalidates the previous
      // observation even when no fresh identity can be issued. Retaining the
      // old live entry would attach its serial to a different incarnation.
      if (entry->state == State::Live)
        retire(*entry);
      return 0;
    }
    if (entry->state == State::Live)
      increment(HeaderField::OverlapConflict);
    *entry = Entry { State::Live, ptr, serial, 0, 0 };
    return serial;
  }
  static void retire(Entry& entry) noexcept {
    entry = Entry { State::Tombstone, 0, 0, 0, 0 };
  }
  void publish(Event& event) noexcept {
    const uint32_t sequence = load(HeaderField::Committed) + 1;
    event.words[0] = sequence;
    for (uint32_t i = 25; i < kEventWords; ++i)
      event.words[i] = 0;
    if (event.words[13] & uint32_t(ReadFailure))
      increment(HeaderField::ReadFailures);
    JournalEvent& destination = m_journal.events[sequence - 1];
    for (uint32_t i = 1; i < kEventWords; ++i)
      destination.words[i].store(event.words[i], std::memory_order_relaxed);
    destination.words[0].store(sequence, std::memory_order_release);
    m_journal.header.words[uint32_t(HeaderField::Committed)]
      .store(sequence, std::memory_order_release);
  }

  Journal& m_journal;
  bool m_resumed = false;
  std::atomic_flag m_cell = ATOMIC_FLAG_INIT;
  uint32_t m_nextSerial = 1;
  uint32_t m_nextOperation = 1;
  Entry m_directory[kDirectoryCapacity] {};
};

static_assert((kDirectoryCapacity & (kDirectoryCapacity - 1)) == 0);
static_assert(kProbeLimit <= kDirectoryCapacity);
static_assert(sizeof(Core) <= 96u * 1024u);

} // namespace dxvk::war3::memory::mdltrace
