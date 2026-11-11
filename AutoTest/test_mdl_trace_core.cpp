// Executes the production bounded core; no game/allocator/lifetime claim.
// --emit PATH additionally writes an actual production-journal fixture for the
// strict Python reader. Only the test process owns this cold journal.
#include "../src/d3d9/war3/memory/war3_mdl_trace_core.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace m = dxvk::war3::memory::mdltrace;

static std::atomic<bool> countAllocations { false };
static std::atomic<uint32_t> allocationCount { 0 };
static unsigned checks = 0;
static m::Journal journal;

void* operator new(std::size_t bytes) {
  if (countAllocations.load(std::memory_order_relaxed))
    allocationCount.fetch_add(1, std::memory_order_relaxed);
  if (void* ptr = std::malloc(bytes ? bytes : 1))
    return ptr;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

static void check(bool value, const char* expression, unsigned line) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL line %u: %s\n", line, expression);
    std::exit(1);
  }
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)

static_assert(noexcept(std::declval<m::Core&>().noteAllocation(m::Event{})));
static_assert(noexcept(std::declval<m::Core&>().beginOperation(m::Event{}, true)));
static_assert(noexcept(std::declval<m::Core&>().endOperation(m::Token{}, m::Event{}, false)));
static_assert(noexcept(std::declval<m::Core&>().noteBoundary(m::Event{}, true)));
static_assert(!std::is_copy_constructible<m::Core>::value);
static_assert(std::is_trivially_copyable<m::Event>::value);
static_assert(std::is_trivially_copyable<m::Token>::value);

namespace dxvk::war3::memory::mdltrace {
struct CoreTestAccess {
  static bool acquire(Core& core) noexcept {
    return !core.m_cell.test_and_set(std::memory_order_acquire);
  }
  static void release(Core& core) noexcept {
    core.m_cell.clear(std::memory_order_release);
  }
  static uint32_t serial(Core& core, uint32_t ptr) noexcept {
    auto* entry = core.find(ptr);
    return entry ? entry->serial : 0;
  }
  static uint32_t operation(Core& core, uint32_t ptr) noexcept {
    auto* entry = core.find(ptr);
    return entry ? entry->operation : 0;
  }
  static uint32_t hash(uint32_t ptr) noexcept { return Core::hash(ptr); }
  static void seedSerial(Core& core, uint32_t value) noexcept { core.m_nextSerial = value; }
  static void seedOperation(Core& core, uint32_t value) noexcept { core.m_nextOperation = value; }
};
}

static uint32_t header(uint32_t word) {
  return journal.header.words[word].load(std::memory_order_relaxed);
}
static uint32_t word(uint32_t index, uint32_t field) {
  return journal.events[index].words[field].load(std::memory_order_relaxed);
}
static void resetJournal() {
  for (auto& item : journal.header.words)
    item.store(0, std::memory_order_relaxed);
  for (auto& event : journal.events)
    for (auto& item : event.words)
      item.store(0, std::memory_order_relaxed);
  const uint32_t values[] = { m::kMagic, m::kVersion, m::kHeaderWords,
    m::kEventWords, m::kEventCapacity };
  for (uint32_t i = 0; i < 5; ++i)
    journal.header.words[i].store(values[i], std::memory_order_relaxed);
  journal.header.words[6].store(1234, std::memory_order_relaxed);
  journal.header.words[7].store(0x55667788, std::memory_order_relaxed);
  journal.header.words[8].store(0x11223344, std::memory_order_relaxed);
  journal.header.words[9].store(0x6F000000, std::memory_order_relaxed);
  journal.header.words[10].store(0x15000000, std::memory_order_relaxed);
  journal.header.words[11].store(2, std::memory_order_relaxed);
  journal.header.words[12].store(0x3F, std::memory_order_relaxed);
  journal.header.words[22].store(15, std::memory_order_relaxed);
  // Nonzero synthetic metadata; this fixture is not a real binary identity.
  journal.header.words[23].store(0x12345678, std::memory_order_relaxed);
  journal.header.words[24].store(0x00C00000, std::memory_order_relaxed);
  journal.header.words[25].store(0x56BD0E34, std::memory_order_relaxed);
  journal.header.words[26].store(0x00061000, std::memory_order_relaxed);
}
static m::Event event(m::Kind kind, uint32_t ptr = 0) {
  m::Event result;
  result.words[1] = uint32_t(kind);
  result.words[2] = 8;
  result.words[3] = 0x128EDD;
  result.words[4] = ptr;
  result.words[10] = 0xB7FF90;
  if (kind == m::Kind::Alloc || kind == m::Kind::BucketReturn
   || kind == m::Kind::InsertEnd)
    result.words[12] = ptr;
  return result;
}
static m::Event reallocEnd(uint32_t oldPtr, uint32_t newPtr) {
  auto result = event(m::Kind::ReallocEnd, oldPtr);
  result.words[5] = newPtr;
  result.words[12] = newPtr;
  return result;
}
static uint32_t serial(m::Core& core, uint32_t ptr) {
  return m::CoreTestAccess::serial(core, ptr);
}

static void testLayoutAndPreservation() {
  resetJournal();
  m::Core core(journal);
  CHECK(sizeof(m::Journal) == 2097408);
  CHECK(sizeof(m::Header) == 256);
  CHECK(sizeof(m::JournalEvent) == 128);
  CHECK(header(0) == m::kMagic && header(11) == 2 && header(22) == 15);
  CHECK(header(6) == 1234 && header(7) == 0x55667788 && header(8) == 0x11223344);
  auto input = event(m::Kind::Alloc, 0x7F165820);
  input.words[0] = 999;
  input.words[6] = 999;
  input.words[7] = 999;
  input.words[8] = 0x5784;
  input.words[9] = 8;
  for (uint32_t i = 25; i < 32; ++i)
    input.words[i] = 0xFFFFFFFFu;
  CHECK(core.noteAllocation(input));
  CHECK(header(5) == 1 && word(0, 0) == 1);
  CHECK(word(0, 6) == 1 && word(0, 7) == 0);
  CHECK(word(0, 8) == 0x5784 && word(0, 9) == 8);
  CHECK(input.words[0] == 999 && input.words[6] == 999 && input.words[7] == 999);
  for (uint32_t i = 25; i < 32; ++i)
    CHECK(word(0, i) == 0);
  CHECK(core.noteBoundary(event(m::Kind::BucketReturn, input.words[4]), false));
  CHECK(word(1, 6) == 1 && word(1, 7) == 0);
  CHECK(header(18) == 0);
}

static void testFreeAndRepeatedCompletion() {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t ptr = 0x7F165820;
  CHECK(core.noteAllocation(event(m::Kind::Alloc, ptr)));
  const auto token = core.beginOperation(event(m::Kind::FreeBegin, ptr), false);
  CHECK(token.admitted && token.tracked && token.serial == 1 && token.operation == 1);
  auto end = event(m::Kind::FreeEnd, ptr);
  end.words[12] = 0; // A raw failure-like value is NOT a physical-free proof.
  CHECK(core.endOperation(token, end, false));
  CHECK(serial(core, ptr) == 0);
  CHECK(word(2, 12) == 0 && (word(2, 13) & m::ReleasedOld) == 0);
  CHECK(core.noteAllocation(event(m::Kind::Alloc, ptr)));
  CHECK(serial(core, ptr) == 2);
  CHECK(!core.endOperation(token, end, false));
  CHECK(serial(core, ptr) == 2);
  CHECK(header(17) == 1 && header(5) == 5);
  CHECK(word(4, 6) == 1 && word(4, 7) == 1);
}

static void testAddressReuseABA() {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t ptr = 0x80010090;
  CHECK(core.noteAllocation(event(m::Kind::Alloc, ptr)));
  const auto old = core.beginOperation(event(m::Kind::ReallocBegin, ptr), false);
  CHECK(old.tracked && old.serial == 1);
  // A pauses; another observed allocation at the same address is incarnation B.
  CHECK(core.noteAllocation(event(m::Kind::Alloc, ptr)));
  const uint32_t b = serial(core, ptr);
  CHECK(b == 2 && header(20) == 1);
  const auto pendingB = core.beginOperation(event(m::Kind::FreeBegin, ptr), false);
  CHECK(pendingB.serial == b && pendingB.operation != old.operation);
  const uint32_t bOperation = m::CoreTestAccess::operation(core, ptr);
  CHECK(!core.endOperation(old, reallocEnd(ptr, 0x80015820), true));
  CHECK(serial(core, ptr) == b);
  CHECK(m::CoreTestAccess::operation(core, ptr) == bOperation);
  CHECK(serial(core, 0x80015820) == 0);
  CHECK(word(header(5) - 1, 24) == 0 && header(17) == 1);
  CHECK(core.endOperation(pendingB, event(m::Kind::FreeEnd, ptr), false));
  CHECK(serial(core, ptr) == 0);
}

static void testReallocSemantics() {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t a = 0x7F165820, b = 0x80010090, c = 0x80015820;
  CHECK(core.noteAllocation(event(m::Kind::Alloc, a)));
  auto token = core.beginOperation(event(m::Kind::ReallocBegin, a), false);
  CHECK(core.endOperation(token, reallocEnd(a, 0), false));
  CHECK(serial(core, a) == 1 && m::CoreTestAccess::operation(core, a) == 0);
  CHECK(word(2, 24) == 0 && !(word(2, 13) & m::ReleasedOld));
  token = core.beginOperation(event(m::Kind::ReallocBegin, a), false);
  CHECK(core.endOperation(token, reallocEnd(a, a), false));
  CHECK(serial(core, a) == 2);
  CHECK(word(4, 6) == 1 && word(4, 24) == 2);
  CHECK(!(word(4, 13) & m::ReleasedOld)); // Do not invent physical release.
  token = core.beginOperation(event(m::Kind::ReallocBegin, a), false);
  CHECK(core.endOperation(token, reallocEnd(a, b), true));
  CHECK(serial(core, a) == 0 && serial(core, b) == 3);
  CHECK(word(6, 6) == 2 && word(6, 24) == 3);
  CHECK(word(6, 13) & m::ReleasedOld);
  token = core.beginOperation(event(m::Kind::ReallocBegin, b), false);
  CHECK(core.endOperation(token, reallocEnd(b, c), false));
  CHECK(serial(core, b) == 3 && serial(core, c) == 4);
  CHECK(m::CoreTestAccess::operation(core, b) == 0);
  token = core.beginOperation(event(m::Kind::ReallocBegin, b), false);
  CHECK(core.endOperation(token, reallocEnd(b, 0), true));
  CHECK(serial(core, b) == 0 && serial(core, c) == 4);
}

static void testUnknownAndNull() {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t unknown = 0x80010090, b = 0x80015820;
  CHECK(!core.noteBoundary(event(m::Kind::UnlinkBegin, unknown), false));
  CHECK(!core.beginOperation(event(m::Kind::FreeBegin, unknown), false).admitted);
  CHECK(header(5) == 0 && header(18) == 0);
  CHECK(core.noteBoundary(event(m::Kind::InsertBegin), true));
  CHECK(header(18) == 0 && word(0, 6) == 0 && word(0, 7) == 0);
  CHECK(core.noteAllocation(event(m::Kind::Alloc)));
  CHECK(header(18) == 0 && word(1, 6) == 0);
  CHECK(core.noteBoundary(event(m::Kind::BucketReturn, unknown), true));
  CHECK(header(18) == 1 && word(2, 6) == 0);
  const auto token = core.beginOperation(event(m::Kind::ReallocBegin, unknown), true);
  CHECK(token.admitted && !token.tracked && token.serial == 0 && token.operation != 0);
  CHECK(header(18) == 2);
  CHECK(core.noteAllocation(event(m::Kind::Alloc, b)));
  CHECK(serial(core, b) == 1);
  CHECK(core.endOperation(token, reallocEnd(unknown, b), true));
  CHECK(core.endOperation(token, reallocEnd(unknown, b), true));
  CHECK(serial(core, b) == 1 && serial(core, unknown) == 0);
  CHECK(word(header(5) - 1, 24) == 0);
  const auto null = core.beginOperation(event(m::Kind::ReallocBegin, 0), true);
  CHECK(null.admitted && !null.tracked && null.serial == 0);
  CHECK(core.endOperation(null, reallocEnd(0, unknown), false));
  CHECK(serial(core, unknown) == 0 && header(18) == 2);
}

static void testOverlappingOperations() {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t ptr = 0x123450;
  CHECK(core.noteAllocation(event(m::Kind::Alloc, ptr)));
  const auto first = core.beginOperation(event(m::Kind::FreeBegin, ptr), false);
  const auto second = core.beginOperation(event(m::Kind::ReallocBegin, ptr), false);
  CHECK(first.tracked && second.tracked && first.operation != second.operation);
  CHECK(header(20) == 1 && m::CoreTestAccess::operation(core, ptr) == first.operation);
  CHECK(!core.endOperation(second, reallocEnd(ptr, 0x999990), true));
  CHECK(serial(core, ptr) == first.serial && serial(core, 0x999990) == 0);
  CHECK(core.endOperation(first, event(m::Kind::FreeEnd, ptr), false));
  CHECK(serial(core, ptr) == 0 && header(17) == 1);
}

static void testBusyAndInterruptedEnd() {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t ptr = 0x123450;
  CHECK(core.noteAllocation(event(m::Kind::Alloc, ptr)));
  const auto token = core.beginOperation(event(m::Kind::FreeBegin, ptr), false);
  CHECK(m::CoreTestAccess::acquire(core));
  CHECK(!core.noteAllocation(event(m::Kind::Alloc, 0x999990)));
  CHECK(!core.noteBoundary(event(m::Kind::UnlinkBegin, ptr), false));
  CHECK(!core.beginOperation(event(m::Kind::FreeBegin, ptr), false).admitted);
  CHECK(!core.endOperation(token, event(m::Kind::FreeEnd, ptr), false));
  CHECK(header(13) == 4 && header(5) == 2);
  m::CoreTestAccess::release(core);
  CHECK(serial(core, ptr) == token.serial);
  CHECK(m::CoreTestAccess::operation(core, ptr) == token.operation);
  CHECK(serial(core, 0x999990) == 0);
  CHECK(core.endOperation(token, event(m::Kind::FreeEnd, ptr), false));
  CHECK(serial(core, ptr) == 0);
}

static void testProbeLimitAndTombstones() {
  resetJournal();
  m::Core core(journal);
  uint32_t colliding[m::kProbeLimit + 2] = {};
  uint32_t found = 0;
  const uint32_t target = m::CoreTestAccess::hash(0x1000000);
  for (uint32_t ptr = 0x1000000; found < m::kProbeLimit + 2; ptr += 16) {
    if (m::CoreTestAccess::hash(ptr) == target)
      colliding[found++] = ptr;
  }
  for (uint32_t i = 0; i < m::kProbeLimit; ++i)
    CHECK(core.noteAllocation(event(m::Kind::Alloc, colliding[i])));
  CHECK(header(16) == 0);
  CHECK(core.noteAllocation(event(m::Kind::Alloc, colliding[m::kProbeLimit])));
  CHECK(header(16) == 1 && word(m::kProbeLimit, 6) == 0);
  CHECK(serial(core, colliding[m::kProbeLimit]) == 0);
  const auto first = core.beginOperation(event(m::Kind::FreeBegin, colliding[0]), false);
  CHECK(core.endOperation(first, event(m::Kind::FreeEnd, colliding[0]), false));
  // Lookup crosses the tombstone; existing nodes never become unreachable.
  CHECK(core.noteBoundary(event(m::Kind::UnlinkBegin, colliding[m::kProbeLimit - 1]), false));
  CHECK(word(header(5) - 1, 6) == m::kProbeLimit);
  CHECK(core.noteAllocation(event(m::Kind::Alloc, colliding[m::kProbeLimit + 1])));
  CHECK(serial(core, colliding[m::kProbeLimit + 1]) == m::kProbeLimit + 1);
  CHECK(header(16) == 1);
  const uint32_t before = serial(core, colliding[1]);
  CHECK(core.noteAllocation(event(m::Kind::Alloc, colliding[1])));
  CHECK(serial(core, colliding[1]) != before && header(20) == 1);
}

static void testFullDirectoryAndJournal() {
  resetJournal();
  {
    m::Core core(journal);
    // Every hash is distinct for this range, so this reaches the REAL 4096
    // directory limit rather than an accidental early probe-chain limit.
    for (uint32_t i = 0; i < m::kDirectoryCapacity; ++i)
      CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x01000000 + i * 16)));
    CHECK(header(16) == 0 && header(5) == m::kDirectoryCapacity);
    CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x02000000)));
    CHECK(header(16) == 1 && word(m::kDirectoryCapacity, 6) == 0);
    CHECK(serial(core, 0x02000000) == 0);
  }
  resetJournal();
  {
    m::Core core(journal);
    CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x123450)));
    const auto token = core.beginOperation(event(m::Kind::FreeBegin, 0x123450), false);
    while (header(5) < m::kEventCapacity)
      CHECK(core.noteBoundary(event(m::Kind::InsertBegin), true));
    CHECK(!core.endOperation(token, event(m::Kind::FreeEnd, 0x123450), false));
    CHECK(!core.noteAllocation(event(m::Kind::Alloc, 0x999990)));
    CHECK(header(5) == m::kEventCapacity && header(15) == 2);
    CHECK(serial(core, 0x123450) == token.serial && serial(core, 0x999990) == 0);
    CHECK(m::CoreTestAccess::operation(core, 0x123450) == token.operation);
    CHECK(word(m::kEventCapacity - 1, 0) == m::kEventCapacity);
    journal.header.words[15].store(0xFFFFFFFFu, std::memory_order_relaxed);
    CHECK(!core.noteAllocation(event(m::Kind::Alloc, 0x999990)));
    CHECK(header(15) == 0xFFFFFFFFu && header(19) == 1);
  }
}

static void testIdentityOverflowAndBusyOverflow() {
  resetJournal();
  {
    m::Core core(journal);
    m::CoreTestAccess::seedSerial(core, 0xFFFFFFFFu);
    CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x123450)));
    CHECK(serial(core, 0x123450) == 0xFFFFFFFFu);
    CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x999990)));
    CHECK(serial(core, 0x999990) == 0 && word(1, 6) == 0 && header(19) == 1);
    m::CoreTestAccess::seedOperation(core, 0xFFFFFFFFu);
    const auto last = core.beginOperation(event(m::Kind::FreeBegin, 0x123450), false);
    CHECK(last.admitted && last.operation == 0xFFFFFFFFu);
    const uint32_t committed = header(5);
    CHECK(!core.beginOperation(event(m::Kind::FreeBegin, 0x123450), false).admitted);
    CHECK(header(5) == committed);
    CHECK(m::CoreTestAccess::operation(core, 0x123450) == last.operation);
    CHECK(core.endOperation(last, event(m::Kind::FreeEnd, 0x123450), false));
    CHECK(serial(core, 0x123450) == 0);
  }
  resetJournal();
  {
    m::Core core(journal);
    m::CoreTestAccess::seedSerial(core, 0xFFFFFFFFu);
    CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x123450)));
    const auto old = core.beginOperation(event(m::Kind::FreeBegin, 0x123450), false);
    CHECK(old.tracked && old.serial == 0xFFFFFFFFu);
    CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x123450)));
    CHECK(word(header(5) - 1, 6) == 0 && header(19) == 1);
    CHECK(serial(core, 0x123450) == 0);
    CHECK(core.noteBoundary(event(m::Kind::BucketReturn, 0x123450), true));
    CHECK(word(header(5) - 1, 6) == 0 && header(18) == 1);
    CHECK(!core.endOperation(old, event(m::Kind::FreeEnd, 0x123450), false));
    CHECK(serial(core, 0x123450) == 0 && header(17) == 1);
  }
  resetJournal();
  {
    m::Core core(journal);
    journal.header.words[13].store(0xFFFFFFFFu, std::memory_order_relaxed);
    CHECK(m::CoreTestAccess::acquire(core));
    CHECK(!core.noteAllocation(event(m::Kind::Alloc, 0x123450)));
    CHECK(header(14) == 1 && header(19) == 1 && header(5) == 0);
    CHECK(!core.noteAllocation(event(m::Kind::Alloc, 0x123450)));
    CHECK(header(14) == 1 && header(19) == 1);
    m::CoreTestAccess::release(core);
  }
}

static void testReadFailureAndAdapterViolation() {
  resetJournal();
  m::Core core(journal);
  auto boundary = event(m::Kind::InsertBegin);
  boundary.words[13] = m::ReadFailure;
  CHECK(core.noteBoundary(boundary, true));
  CHECK(header(21) == 1 && word(0, 13) == m::ReadFailure);
  CHECK(!core.noteAllocation(boundary));
  CHECK(!core.beginOperation(boundary, true).admitted);
  CHECK(!core.noteBoundary(event(m::Kind::Alloc), true));
  CHECK(header(20) == 3 && header(5) == 1);
  CHECK(core.noteAllocation(event(m::Kind::Alloc, 0x123450)));
  const auto token = core.beginOperation(event(m::Kind::FreeBegin, 0x123450), false);
  CHECK(!core.endOperation(token, event(m::Kind::FreeEnd, 0x999990), false));
  CHECK(header(20) == 4 && serial(core, 0x123450) == 1);
  const uint32_t before = header(5);
  CHECK(!core.endOperation(token, event(m::Kind::FreeEnd, 0x123450), true));
  CHECK(header(20) == 5 && header(5) == before);
  CHECK(serial(core, 0x123450) == 1);
  CHECK(m::CoreTestAccess::operation(core, 0x123450) == token.operation);
  CHECK(core.endOperation(token, event(m::Kind::FreeEnd, 0x123450), false));
  CHECK(serial(core, 0x123450) == 0);
  // Core may not silently attach a fresh identity directory to old evidence.
  m::Core resumed(journal);
  CHECK(!resumed.noteAllocation(event(m::Kind::Alloc, 0x999990)));
  CHECK(header(18) == 1);
}

static void testConcurrentCell() {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t threads = 4, perThread = 100;
  std::atomic<uint32_t> ready { 0 }, accepted { 0 };
  std::atomic<bool> go { false };
  std::thread workers[threads];
  for (uint32_t i = 0; i < threads; ++i) {
    workers[i] = std::thread([&, i] {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (uint32_t n = 0; n < perThread; ++n) {
        auto value = event(m::Kind::Alloc, 0x01000000 + (i * perThread + n) * 16);
        value.words[2] = i + 1;
        value.words[8] = value.words[4] ^ 0xAAAAAAAAu;
        if (core.noteAllocation(value))
          accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != threads)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto& worker : workers)
    worker.join();
  CHECK(header(5) == accepted.load(std::memory_order_relaxed));
  CHECK(header(5) + header(13) == threads * perThread);
  CHECK(header(14) == 0 && header(16) == 0 && header(19) == 0);
  CHECK(header(5) > 0);
  for (uint32_t i = 0; i < header(5); ++i) {
    CHECK(word(i, 0) == i + 1 && word(i, 6) == i + 1);
    CHECK(word(i, 8) == (word(i, 4) ^ 0xAAAAAAAAu));
    CHECK(word(i, 2) >= 1 && word(i, 2) <= threads);
  }
}

static void testNoHeapAllocation() {
  resetJournal();
  m::Core core(journal);
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  for (uint32_t i = 0; i < 1000; ++i) {
    const uint32_t ptr = 0x01000000 + i * 16;
    CHECK(core.noteAllocation(event(m::Kind::Alloc, ptr)));
    const auto token = core.beginOperation(event(m::Kind::FreeBegin, ptr), false);
    CHECK(token.admitted && token.tracked);
    CHECK(core.endOperation(token, event(m::Kind::FreeEnd, ptr), false));
  }
  countAllocations.store(false, std::memory_order_relaxed);
  CHECK(allocationCount.load(std::memory_order_relaxed) == 0);
  CHECK(header(5) == 3000 && header(16) == 0 && header(20) == 0);
}

static void emitFixture(const char* path) {
  resetJournal();
  m::Core core(journal);
  constexpr uint32_t low = 0x7F165820, high = 0x80010090;
  auto allocation = event(m::Kind::Alloc, low);
  allocation.words[8] = 0x5784;
  allocation.words[9] = 8;
  CHECK(core.noteAllocation(allocation));
  auto bucket = event(m::Kind::BucketReturn, low);
  bucket.words[13] = m::NodeKnown;
  bucket.words[14] = 0x6F961788;
  bucket.words[15] = 0x80015820;
  CHECK(core.noteBoundary(bucket, true));
  const auto resize = core.beginOperation(event(m::Kind::ReallocBegin, low), false);
  CHECK(core.endOperation(resize, reallocEnd(low, high), true));
  auto unlink = event(m::Kind::UnlinkBegin, high);
  unlink.words[3] = 0x12833E;
  unlink.words[13] = m::NodeKnown;
  unlink.words[14] = 0x6F961788;
  unlink.words[15] = 0x7F165820; // node+4 is bucket NEXT, not the failed backlink.
  unlink.words[16] = 0x7F165820;
  unlink.words[17] = 0x8001582C;
  unlink.words[18] = 0x80015820; // node+0x10: global backlink read at Game+12D95B.
  CHECK(core.noteBoundary(unlink, false));
  const auto free = core.beginOperation(event(m::Kind::FreeBegin, high), false);
  auto returned = event(m::Kind::FreeEnd, high);
  returned.words[12] = 1;
  CHECK(core.endOperation(free, returned, false));
  CHECK(core.noteAllocation(event(m::Kind::Alloc, high)));
  CHECK(header(5) == 8 && word(3, 24) == 2 && word(7, 6) == 3);
  std::FILE* file = std::fopen(path, "wb");
  CHECK(file != nullptr);
  uint32_t buffer[m::kHeaderWords] = {};
  for (uint32_t i = 0; i < m::kHeaderWords; ++i)
    buffer[i] = header(i);
  CHECK(std::fwrite(buffer, sizeof(uint32_t), m::kHeaderWords, file) == m::kHeaderWords);
  for (uint32_t i = 0; i < m::kEventCapacity; ++i) {
    for (uint32_t j = 0; j < m::kEventWords; ++j)
      buffer[j] = word(i, j);
    CHECK(std::fwrite(buffer, sizeof(uint32_t), m::kEventWords, file) == m::kEventWords);
  }
  CHECK(std::fclose(file) == 0);
  std::printf("fixture=%s bytes=%zu events=%u\n", path, sizeof(m::Journal), header(5));
}

int main(int argc, char** argv) {
  if (argc != 1 && (argc != 3 || std::strcmp(argv[1], "--emit") != 0)) {
    std::fprintf(stderr, "usage: test_mdl_trace_core [--emit journal.bin]\n");
    return 2;
  }
  testLayoutAndPreservation();
  testFreeAndRepeatedCompletion();
  testAddressReuseABA();
  testReallocSemantics();
  testUnknownAndNull();
  testOverlappingOperations();
  testBusyAndInterruptedEnd();
  testProbeLimitAndTombstones();
  testFullDirectoryAndJournal();
  testIdentityOverflowAndBusyOverflow();
  testReadFailureAndAdapterViolation();
  testConcurrentCell();
  testNoHeapAllocation();
  if (argc == 3)
    emitFixture(argv[2]);
  std::printf("mdl-trace-core PASS checks=%u allocationWindow=%u coreBytes=%zu journalBytes=%zu\n",
    checks, allocationCount.load(std::memory_order_relaxed), sizeof(m::Core), sizeof(m::Journal));
  return 0;
}
