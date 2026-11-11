// CPU-only tests of the production header. Never loads a game, DLL or GPU.
// C++17, standard library only; caller supplies the existing process timeout.
#include "../src/d3d9/war3/memory/war3_lazy_thread_array.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace memory = dxvk::war3::memory;

// Per-thread C++ allocation observations exclude std::thread creation performed
// by its parent, and do not claim to count Windows/CRT internal malloc calls.
static thread_local unsigned allocations = 0;
static thread_local unsigned deallocations = 0;
static thread_local bool failNextAllocation = false;
static thread_local unsigned failAllocationOrdinal = 0;
// The watched pointer is trivially destructible TLS. It remains available when
// the thread_local owning array's destructor returns its backing allocation.
// Count that exact pointer, not unrelated std::thread/CRT housekeeping.
static thread_local void* watchedDelete = nullptr;
static std::atomic<unsigned> watchedDeletions{0};

void* operator new(std::size_t bytes) {
  ++allocations;
  if (failNextAllocation ||
      (failAllocationOrdinal != 0 && allocations == failAllocationOrdinal)) {
    failNextAllocation = false;
    failAllocationOrdinal = 0;
    throw std::bad_alloc();
  }
  if (void* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
  if (pointer) {
    ++deallocations;
    if (pointer == watchedDelete) {
      watchedDelete = nullptr;
      watchedDeletions.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

static std::atomic<unsigned> checks{0}, failures{0};
static void Check(bool value, const char* expression, unsigned line) {
  checks.fetch_add(1, std::memory_order_relaxed);
  if (!value) {
    failures.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "FAIL line=%u %s\n", line, expression);
  }
}
#define CHECK(expression) Check(bool(expression), #expression, __LINE__)

using Values = memory::LazyThreadArray<uint32_t, 1024>;
static_assert(!std::is_copy_constructible<Values>::value, "owner must not copy");
static_assert(!std::is_copy_assignable<Values>::value, "owner must not copy assign");
static_assert(!std::is_move_constructible<Values>::value, "owner must not move");
static_assert(!std::is_move_assignable<Values>::value, "owner must not move assign");
static_assert(std::is_nothrow_default_constructible<Values>::value, "cold construction cannot throw");
static_assert(std::is_nothrow_destructible<Values>::value, "owner destruction cannot throw");
static_assert(Values::size() == 1024 && Values::capacity() == 1024, "capacity unchanged");
static_assert(sizeof(Values) == sizeof(void*), "cold TLS is one owning pointer");
static_assert(noexcept(std::declval<Values&>().peek()), "mutable inspection cannot throw");
static_assert(noexcept(std::declval<const Values&>().peek()), "const inspection cannot throw");
static_assert(noexcept(std::declval<Values&>().try_get()), "optional preparation cannot throw");
static_assert(noexcept(std::declval<const Values&>().try_get()), "const preparation cannot throw");
static_assert(noexcept(Values::size()) && noexcept(Values::capacity()), "capacity inspection cannot throw");
static_assert(std::is_same<decltype(std::declval<const Values&>().peek()),
    const Values::array_type*>::value, "const inspection cannot mutate backing storage");
static_assert(std::is_same<decltype(std::declval<const Values&>().try_get()),
    const Values::array_type*>::value, "const preparation cannot mutate backing storage");

struct Tracked {
  static std::atomic<int> live, constructed, destroyed;
  uint32_t value = 0;
  Tracked() { ++live; ++constructed; }
  ~Tracked() { --live; ++destroyed; }
};
std::atomic<int> Tracked::live{0}, Tracked::constructed{0}, Tracked::destroyed{0};

struct ConstructionFailure {};
struct Throwing {
  static unsigned attempts, live, destroyed, throwOn;
  uint32_t value = 0;
  Throwing() {
    ++attempts;
    if (attempts == throwOn) throw ConstructionFailure{};
    ++live;
  }
  ~Throwing() { --live; ++destroyed; }
};
unsigned Throwing::attempts = 0, Throwing::live = 0, Throwing::destroyed = 0, Throwing::throwOn = 0;

static void ColdWarmAndReset() {
  const unsigned cold = allocations;
  Values values;
  const Values& immutable = values;
  CHECK(!values.peek() && !immutable.peek());
  CHECK(values.size() == 1024 && values.capacity() == 1024);
  // The production invalidation shape does not warm an unused thread.
  if (auto* array = values.peek()) array->fill(7);
  CHECK(allocations == cold);

  auto& array = values.get();
  CHECK(allocations == cold + 1);
  CHECK(values.peek() == &array && immutable.peek() == &array);
  for (uint32_t value : array) CHECK(value == 0); // Actual value initialization.
  const unsigned warm = allocations;
  values[0] = 41;
  CHECK(immutable[0] == 41);
  CHECK(&immutable.get() == &array && &values.get() == &array);
  CHECK(values.begin() == array.begin() && values.end() == array.end());
  CHECK(immutable.begin() == array.cbegin() && immutable.end() == array.cend());
  CHECK(immutable.cbegin() == array.cbegin() && immutable.cend() == array.cend());
  values.fill(9);
  for (uint32_t value : immutable) CHECK(value == 9);
  if (auto* existing = values.peek()) existing->fill(0);
  CHECK(values.peek() == &array && values.capacity() == 1024);
  for (uint32_t value : array) CHECK(value == 0);
  for (unsigned i = 0; i < 10000; ++i) {
    values[i % values.size()] = i;
    CHECK(immutable[i % immutable.size()] == i);
  }
  CHECK(allocations == warm);
}

static void EveryAccessInitializes() {
  {
    Values array;
    CHECK(!array.peek());
    CHECK(array[1] == 0 && array.peek());
  }
  {
    const Values array;
    CHECK(!array.peek());
    CHECK(array.get()[1] == 0 && array.peek());
  }
  {
    const Values array;
    CHECK(!array.peek());
    CHECK(array[1] == 0 && array.peek());
  }
  {
    Values array;
    CHECK(!array.peek());
    const auto begin = array.begin();
    CHECK(begin == array.peek()->begin());
  }
  {
    Values array;
    CHECK(!array.peek());
    const auto end = array.end();
    CHECK(end == array.peek()->end());
  }
  {
    const Values array;
    CHECK(!array.peek());
    const auto begin = array.begin();
    CHECK(begin == array.peek()->cbegin());
  }
  {
    const Values array;
    CHECK(!array.peek());
    const auto end = array.end();
    CHECK(end == array.peek()->cend());
  }
  {
    const Values array;
    CHECK(!array.peek());
    const auto begin = array.cbegin();
    CHECK(begin == array.peek()->cbegin());
  }
  {
    const Values array;
    CHECK(!array.peek());
    const auto end = array.cend();
    CHECK(end == array.peek()->cend());
  }
  {
    Values array;
    array.fill(23);
    CHECK(array.peek() && (*array.peek())[1023] == 23);
  }
}

static void AllocationFailureRetries() {
  Values values;
  const unsigned before = allocations;
  failNextAllocation = true;
  bool threw = false;
  try { (void)values.get(); } catch (const std::bad_alloc&) { threw = true; }
  CHECK(threw && !values.peek());
  CHECK(allocations == before + 1);
  CHECK(values[0] == 0 && values.peek());
  CHECK(allocations == before + 2);
}

static void ConstructionFailureRetries() {
  Throwing::attempts = Throwing::live = Throwing::destroyed = 0;
  Throwing::throwOn = 4;
  const unsigned coldAllocations = allocations;
  const unsigned coldDeallocations = deallocations;
  {
    memory::LazyThreadArray<Throwing, 8> values;
    bool threw = false;
    try { (void)values.get(); } catch (const ConstructionFailure&) { threw = true; }
    CHECK(threw && !values.peek());
    CHECK(Throwing::attempts == 4 && Throwing::live == 0 && Throwing::destroyed == 3);
    CHECK(allocations == coldAllocations + 1 && deallocations == coldDeallocations + 1);
    Throwing::throwOn = 0;
    auto& array = values.get();
    CHECK(values.peek() == &array && Throwing::live == 8);
    CHECK(Throwing::attempts == 12);
    CHECK(allocations == coldAllocations + 2 && deallocations == coldDeallocations + 1);
    for (const auto& value : array) CHECK(value.value == 0);
    const unsigned warm = allocations;
    CHECK(&values.get() == &array);
    CHECK(allocations == warm);
  }
  CHECK(Throwing::live == 0 && Throwing::destroyed == 11);
  CHECK(allocations == coldAllocations + 2 && deallocations == coldDeallocations + 2);
}

static void NonthrowingPreparationRetries() {
  {
    Values values;
    const unsigned before = allocations;
    failNextAllocation = true;
    CHECK(values.try_get() == nullptr && !values.peek());
    CHECK(allocations == before + 1 && !failNextAllocation);
    auto* array = values.try_get();
    CHECK(array && array == values.peek());
    CHECK(allocations == before + 2 && (*array)[0] == 0);
    (*array)[0] = 91;
    failNextAllocation = true;
    CHECK(values.try_get() == array && values[0] == 91);
    CHECK(failNextAllocation && allocations == before + 2);
    failNextAllocation = false;
  }
  {
    const Values values;
    failNextAllocation = true;
    CHECK(values.try_get() == nullptr && !values.peek());
    CHECK(values.try_get() != nullptr && values.get()[0] == 0);
  }
  // Non-bad_alloc element-construction failures are also a refusal only for
  // try_get(). Existing get() tests above still require the original throw.
  Throwing::attempts = Throwing::live = Throwing::destroyed = 0;
  Throwing::throwOn = 4;
  const unsigned beforeDelete = deallocations;
  {
    memory::LazyThreadArray<Throwing, 8> values;
    CHECK(values.try_get() == nullptr && !values.peek());
    CHECK(Throwing::attempts == 4 && Throwing::live == 0 && Throwing::destroyed == 3);
    CHECK(deallocations == beforeDelete + 1);
    Throwing::throwOn = 0;
    CHECK(values.try_get() && Throwing::live == 8);
  }
  CHECK(Throwing::live == 0 && Throwing::destroyed == 11);
  CHECK(deallocations == beforeDelete + 2);
}

static void ThreeTableStoragePreflight() {
  // Exercise the SAME preflight used by CurrentDraw, including each possible
  // allocation-failure position. The scalar commit markers below are a CPU
  // caller contract, not an assertion that a Game hook or GPU draw ran.
  for (unsigned failedTable = 1; failedTable <= 3; ++failedTable) {
    Values contracts, palettes, ledger;
    const unsigned cold = allocations;
    unsigned published = 0, bitmap = 0, refused = 0, nativeCalls = 0;
    const auto publish = [&] {
      if (!memory::TryPrepareThreadArrays(contracts, palettes, ledger)) {
        ++refused;
        return;
      }
      contracts[0] = 17;
      palettes[0] = 31;
      ledger[0] = 47;
      bitmap = 1;
      ++published;
    };
    const auto invokeHookShape = [&](bool publishAfterOriginal) {
      if (!publishAfterOriginal) publish();
      ++nativeCalls;
      if (publishAfterOriginal) publish();
    };

    failAllocationOrdinal = cold + failedTable;
    invokeHookShape(false);
    CHECK(refused == 1 && published == 0 && bitmap == 0 && nativeCalls == 1);
    CHECK(allocations == cold + failedTable && failAllocationOrdinal == 0);
    CHECK(bool(contracts.peek()) == (failedTable > 1));
    CHECK(bool(palettes.peek()) == (failedTable > 2));
    CHECK(!ledger.peek());
    const auto* contractAddress = contracts.peek();
    const auto* paletteAddress = palettes.peek();
    if (contractAddress) for (auto value : *contractAddress) CHECK(value == 0);
    if (paletteAddress) for (auto value : *paletteAddress) CHECK(value == 0);

    // A second failure at the next cold table must still not publish. Warm
    // earlier tables must retain their exact addresses and initialized state.
    failNextAllocation = true;
    invokeHookShape(true);
    CHECK(refused == 2 && published == 0 && bitmap == 0 && nativeCalls == 2);
    CHECK(contracts.peek() == contractAddress && palettes.peek() == paletteAddress);
    CHECK(!failNextAllocation && !ledger.peek());
    invokeHookShape(false);
    CHECK(refused == 2 && published == 1 && bitmap == 1 && nativeCalls == 3);
    CHECK(contracts[0] == 17 && palettes[0] == 31 && ledger[0] == 47);
    CHECK(!contractAddress || contracts.peek() == contractAddress);
    CHECK(!paletteAddress || palettes.peek() == paletteAddress);
    // Three successful backing allocations plus the two injected refusals.
    CHECK(allocations == cold + 5);
    const unsigned warm = allocations;
    failNextAllocation = true;
    invokeHookShape(true);
    CHECK(refused == 2 && published == 2 && bitmap == 1 && nativeCalls == 4);
    CHECK(allocations == warm && failNextAllocation);
    failNextAllocation = false;

    // Reset existing values without discarding backing. The next preflight
    // remains warm and the cumulative failure observation is not cleared.
    const auto* ledgerAddress = ledger.peek();
    contracts.peek()->fill(0);
    palettes.peek()->fill(0);
    ledger.peek()->fill(0);
    CHECK(memory::TryPrepareThreadArrays(contracts, palettes, ledger));
    CHECK(ledger.peek() == ledgerAddress && allocations == warm && refused == 2);
    CHECK(contracts[0] == 0 && palettes[0] == 0 && ledger[0] == 0);
  }
}

static void DestructionAndEmptyCapacity() {
  Tracked::live = Tracked::constructed = Tracked::destroyed = 0;
  const unsigned beforeAllocations = allocations;
  const unsigned beforeDeallocations = deallocations;
  {
    memory::LazyThreadArray<Tracked, 32> cold;
    CHECK(!cold.peek() && Tracked::live == 0);
  }
  CHECK(Tracked::destroyed == 0);
  CHECK(allocations == beforeAllocations && deallocations == beforeDeallocations);
  {
    memory::LazyThreadArray<Tracked, 32> warm;
    CHECK(warm.get().size() == 32);
    CHECK(Tracked::live == 32 && Tracked::constructed == 32);
  }
  CHECK(Tracked::live == 0 && Tracked::destroyed == 32);
  CHECK(allocations == beforeAllocations + 1 && deallocations == beforeDeallocations + 1);
  memory::LazyThreadArray<int, 0> empty;
  CHECK(!empty.peek() && empty.size() == 0 && empty.capacity() == 0);
  const auto begin = empty.begin();
  CHECK(empty.peek() && begin == empty.end());
}

static void ThreadOwnershipAndExit() {
  Tracked::live = Tracked::constructed = Tracked::destroyed = 0;
  constexpr unsigned count = 4;
  const unsigned deletedBefore = watchedDeletions.load();
  std::array<std::thread, count> threads;
  std::array<const void*, count> addresses{};
  std::atomic<unsigned> ready{0};
  std::atomic<bool> release{false};
  for (unsigned i = 0; i < count; ++i) {
    threads[i] = std::thread([&, i] {
      const unsigned cold = allocations;
      thread_local memory::LazyThreadArray<Tracked, 16> cache;
      CHECK(!cache.peek());
      if (auto* array = cache.peek()) (*array)[0].value = 123;
      CHECK(allocations == cold);
      auto& array = cache.get();
      CHECK(allocations == cold + 1);
      CHECK(watchedDelete == nullptr);
      watchedDelete = &array;
      array[0].value = i + 1;
      addresses[i] = &array;
      const unsigned warm = allocations;
      ready.fetch_add(1, std::memory_order_release);
      while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
      CHECK(cache[0].value == i + 1 && cache.peek() == &array);
      CHECK(allocations == warm);
    });
  }
  while (ready.load(std::memory_order_acquire) != count) std::this_thread::yield();
  CHECK(Tracked::live == int(count * 16));
  for (unsigned i = 0; i < count; ++i)
    for (unsigned j = i + 1; j < count; ++j) CHECK(addresses[i] != addresses[j]);
  release.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();
  CHECK(Tracked::live == 0 && Tracked::constructed == int(count * 16));
  CHECK(Tracked::destroyed == int(count * 16));
  CHECK(watchedDeletions.load() == deletedBefore + count);
}

int main() {
  ColdWarmAndReset();
  EveryAccessInitializes();
  AllocationFailureRetries();
  ConstructionFailureRetries();
  NonthrowingPreparationRetries();
  ThreeTableStoragePreflight();
  DestructionAndEmptyCapacity();
  ThreadOwnershipAndExit();
  std::printf("RESULT {\"checks\":%u,\"failures\":%u,\"ok\":%s}\n",
      checks.load(), failures.load(), failures.load() ? "false" : "true");
  return failures.load() ? 1 : 0;
}
