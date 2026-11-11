// Actual production core, no game/GPU/ready acceptance. --emit PATH writes a
// fresh binary fixture through the SAME production writer, not a JSON mock.
#include "../src/d3d9/war3/diagnostics/war3_startup_witness_core.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace w = dxvk::war3::diagnostics::startupwitness;
using S = dxvk::war3::diagnostics::StartupStage;
static unsigned checks = 0;
static std::atomic<bool> countAllocations{false};
static std::atomic<unsigned> allocations{0};
static w::Journal journal;

void* operator new(std::size_t bytes) {
  if (countAllocations.load(std::memory_order_relaxed)) ++allocations;
  if (void* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
static void check(bool ok, const char* expression, unsigned line) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL %u: %s\n", line, expression);
    std::exit(1);
  }
}
#define CHECK(x) check(bool(x), #x, __LINE__)

namespace dxvk::war3::diagnostics::startupwitness {
struct CoreTestAccess {
  static bool acquire(Core& core) {
    return !core.m_cell.test_and_set(std::memory_order_acquire);
  }
  static void release(Core& core) { core.m_cell.clear(std::memory_order_release); }
};
}
static_assert(noexcept(std::declval<w::Core&>().begin(S::Bootstrap, w::Sample{})));
static_assert(noexcept(std::declval<w::Core&>().end(S::Bootstrap, 1, w::Sample{})));
static_assert(!std::is_copy_constructible<w::Core>::value);
static_assert(std::is_trivially_copyable<w::Sample>::value);

static uint32_t h(uint32_t n) { return journal.header.words[n].load(); }
static uint32_t e(uint32_t n, uint32_t m) { return journal.events[n].words[m].load(); }
static w::Sample sample(uint32_t tid = 1, uint64_t ticks = 100,
                        uint32_t detail = 0, uint32_t extra = 0) {
  return {tid, ticks, detail, extra};
}
static void reset() {
  for (auto& word : journal.header.words) word.store(0);
  for (auto& event : journal.events)
    for (auto& word : event.words) word.store(0);
  const uint32_t words[] = {w::kMagic, w::kVersion, w::kHeaderWords,
                            w::kEventWords, w::kCapacity};
  for (unsigned i = 0; i < 5; ++i) journal.header.words[i] = words[i];
  journal.header.words[6] = 1234;
  journal.header.words[7] = 0x55667788;
  journal.header.words[8] = 0x11223344;
  journal.header.words[9] = 1;
  journal.header.words[16] = 10000000;
}

static void basics() {
  reset(); w::Core core(journal);
  CHECK(sizeof(journal) == 16640);
  const auto values = sample(7, 0x1122334455667788ull, 42, 91);
  const auto token = core.begin(S::Bootstrap, values);
  CHECK(token == 1 && h(5) == 1);
  CHECK(e(0, 0) == 1 && e(0, 1) == uint32_t(S::Bootstrap));
  CHECK(e(0, 2) == 1 && e(0, 3) == 7 && e(0, 4) == 1);
  CHECK(e(0, 5) == 0x55667788 && e(0, 6) == 0x11223344);
  CHECK(e(0, 7) == 42 && e(0, 8) == 91);
  CHECK(values.thread == 7 && values.detail == 42 && values.extra == 91);
  CHECK(core.end(S::Bootstrap, token, values));
  CHECK(h(5) == 2 && e(1, 2) == 2 && e(1, 4) == token);
  CHECK(core.note(S::RuntimeReady, sample(7)));
  CHECK(h(5) == 3 && e(2, 2) == 3 && e(2, 4) == 0);
  for (uint32_t row = 0; row < w::kCapacity; ++row)
    for (uint32_t col = 9; col < w::kEventWords; ++col) CHECK(e(row, col) == 0);
  for (uint32_t col = 18; col < w::kHeaderWords; ++col) CHECK(h(col) == 0);
  CHECK(h(6) == 1234 && h(7) == 0x55667788 && h(8) == 0x11223344);
}

static void bindingAndInvalid() {
  reset(); w::Core core(journal);
  const auto first = core.begin(S::MainRunnerOriginal, sample());
  const auto second = core.begin(S::MainRunnerOriginal, sample());
  CHECK(first == 1 && second == 2);
  CHECK(!core.end(S::MainRunnerOriginal, first, sample(2)));
  CHECK(!core.end(S::MainRunnerAltOriginal, first, sample()));
  CHECK(!core.end(S::MainRunnerOriginal, 99, sample()));
  CHECK(!core.end(S::MainRunnerOriginal, UINT32_MAX, sample()));
  CHECK(h(5) == 2 && h(13) == 4);
  CHECK(core.end(S::MainRunnerOriginal, second, sample()));
  CHECK(core.end(S::MainRunnerOriginal, first, sample()));
  CHECK(!core.end(S::MainRunnerOriginal, first, sample()));
  CHECK(!core.end(S::MainRunnerOriginal, 0, sample()));
  CHECK(h(5) == 4 && h(13) == 5);
  CHECK(core.note(S::BootstrapGate, sample()));
  CHECK(!core.end(S::BootstrapGate, 5, sample()));
  CHECK(h(13) == 6);
  CHECK(!core.begin(S(0), sample()));
  CHECK(!core.begin(S(w::kLastStage + 1), sample()));
  CHECK(!core.begin(S::Bootstrap, sample(0)));
  CHECK(!core.note(S(0), sample()));
  CHECK(h(13) == 10 && h(5) == 5);
}

static void pipeContract() {
  reset(); w::Core core(journal);
  const auto token = core.begin(S::PipeCreate, sample(2));
  CHECK(!core.end(S::PipeCreate, token, sample(2, 200, 2, 0)));
  CHECK(!core.end(S::PipeCreate, token, sample(2, 200, 1, 5)));
  CHECK(h(13) == 2 && h(5) == 1);
  CHECK(core.end(S::PipeCreate, token, sample(2, 200, 0, 5)));
  CHECK(e(1, 7) == 0 && e(1, 8) == 5);
  const auto connect = core.begin(S::PipeConnect, sample(2, 300));
  CHECK(core.end(S::PipeConnect, connect, sample(2, 400, 1, 0)));
  CHECK(core.note(S::PipeCreate, sample(2, 500, 0, 231)));
  CHECK(!core.note(S::PipeCreate, sample(2, 600, 1, 0)));
  CHECK(!core.note(S::PipeConnect, sample(2, 600, 2, 5)));
  CHECK(h(13) == 4);
}

static void budgetsAndBusy() {
  reset(); w::Core core(journal);
  CHECK(w::CoreTestAccess::acquire(core));
  CHECK(!core.begin(S::Bootstrap, sample()));
  CHECK(!core.end(S::Bootstrap, 1, sample()));
  CHECK(!core.note(S::Bootstrap, sample()));
  CHECK(!core.end(S::Bootstrap, 0, sample()));
  CHECK(h(10) == 3 && h(5) == 0);
  journal.header.words[10] = UINT32_MAX;
  CHECK(!core.note(S::Bootstrap, sample()));
  CHECK(h(10) == 0 && h(11) == 1);
  w::CoreTestAccess::release(core);
  for (unsigned i = 0; i < w::kCapacity; ++i)
    CHECK(core.note(S::BootstrapGate, sample(1, i, i)));
  CHECK(h(5) == w::kCapacity);
  CHECK(!core.begin(S::Bootstrap, sample()));
  CHECK(!core.note(S::Bootstrap, sample()));
  CHECK(h(12) == 2 && h(5) == w::kCapacity);
  journal.header.words[12] = UINT32_MAX;
  CHECK(!core.note(S::Bootstrap, sample()));
  CHECK(h(12) == 0 && h(14) == 1);
  core.clockFailure(); CHECK(h(15) == 1);
  journal.header.words[15] = UINT32_MAX;
  core.clockFailure(); CHECK(h(15) == 0 && h(14) == 1);

  reset(); w::Core another(journal);
  const auto t = another.begin(S::Bootstrap, sample());
  for (unsigned i = 1; i < w::kCapacity; ++i)
    CHECK(another.note(S::BootstrapGate, sample()));
  CHECK(!another.end(S::Bootstrap, t, sample()));
  CHECK(!another.end(S::Bootstrap, t, sample()));
  CHECK(h(12) == 2 && h(13) == 0); // Refusal did not mark the Begin completed.
}

static void allocationFreeWindow() {
  reset(); w::Core core(journal);
  allocations.store(0); countAllocations.store(true);
  for (unsigned i = 0; i < 100; ++i) {
    const auto t = core.begin(S::Bootstrap, sample());
    static_cast<void>(core.end(S::Bootstrap, t, sample()));
  }
  countAllocations.store(false);
  CHECK(allocations.load() == 0 && h(5) == 200);
}

static void concurrentWriters() {
  reset(); w::Core core(journal);
  std::atomic<unsigned> ready{0}, successes{0};
  std::atomic<bool> start{false};
  auto worker = [&](uint32_t tid) {
    ready.fetch_add(1);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (unsigned i = 0; i < 512; ++i)
      if (core.note(S::BootstrapGate, sample(tid, i, i))) successes.fetch_add(1);
  };
  std::thread one(worker, 1), two(worker, 2);
  while (ready.load() != 2) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  one.join(); two.join();
  CHECK(h(5) == successes.load() && h(5) <= w::kCapacity);
  CHECK(h(5) + h(10) + h(12) == 1024);
  CHECK(h(11) == 0 && h(13) == 0 && h(14) == 0);
  for (unsigned i = 0; i < h(5); ++i) {
    CHECK(e(i, 0) == i + 1);
    CHECK(e(i, 3) == 1 || e(i, 3) == 2);
    CHECK(e(i, 5) == e(i, 7));
  }
}

static void emit(const char* path) {
  reset(); w::Core core(journal);
  const auto bootstrap = core.begin(S::Bootstrap, sample(1, 10));
  const auto early = core.begin(S::MainRunnerEarlyHooks, sample(1, 20));
  CHECK(core.end(S::MainRunnerEarlyHooks, early, sample(1, 30)));
  const auto original = core.begin(S::MainRunnerOriginal, sample(1, 40));
  CHECK(core.end(S::MainRunnerOriginal, original, sample(1, 50, 123)));
  const auto activation = core.begin(S::RuntimeActivation, sample(1, 60));
  const auto server = core.begin(S::ControlServerLoop, sample(2, 70));
  const auto pipe = core.begin(S::PipeCreate, sample(2, 80));
  CHECK(core.end(S::PipeCreate, pipe, sample(2, 90, 0, 5)));
  CHECK(core.end(S::ControlServerLoop, server, sample(2, 100)));
  CHECK(core.end(S::RuntimeActivation, activation, sample(1, 110)));
  CHECK(core.end(S::Bootstrap, bootstrap, sample(1, 120)));
  CHECK(h(5) == 12);
  FILE* file = std::fopen(path, "wbx");
  CHECK(file != nullptr);
  for (const auto& word : journal.header.words) {
    uint32_t value = word.load(); CHECK(std::fwrite(&value, sizeof(value), 1, file) == 1);
  }
  for (const auto& event : journal.events)
    for (const auto& word : event.words) {
      uint32_t value = word.load(); CHECK(std::fwrite(&value, sizeof(value), 1, file) == 1);
    }
  CHECK(std::fclose(file) == 0);
}

int main(int argc, char** argv) {
  basics(); bindingAndInvalid(); pipeContract(); budgetsAndBusy();
  allocationFreeWindow(); concurrentWriters();
  if (argc == 3 && std::strcmp(argv[1], "--emit") == 0) emit(argv[2]);
  else CHECK(argc == 1);
  std::printf("startup core checks=%u failures=0 allocationWindow=%u journalBytes=%zu coreBytes=%zu\n",
              checks, allocations.load(), sizeof(journal), sizeof(w::Core));
  return 0;
}
