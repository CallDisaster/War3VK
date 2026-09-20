#include "../src/d3d9/war3/memory/war3_adaptive_memory_budget.h"
#include "../src/d3d9/war3/memory/war3_snapshot_slice.h"
#include "../src/util/util_likely.h"
#include "../src/util/rc/util_rc_ptr.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <chrono>
#include <new>
// The real production tracker implementation, without Vulkan or a substitute
// reference container. clear() models completed-command-list retirement ONLY.
#include "../src/dxvk/dxvk_access.cpp"

static std::atomic<uint64_t> allocations{0};
void* operator new(std::size_t n) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace dxvk;
using namespace dxvk::war3::memory;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
struct Page { SnapshotSliceTable<> table; };
// Same track-by-value forwarding as DxvkCommandList, with the production
// tracker underneath; no VkCommandBuffer or real fence is exercised here.
struct TestCommandTracker {
  DxvkObjectTracker objects;
  unsigned calls = 0;
  template<typename T> void track(Rc<T> value) {
    ++calls; objects.track<DxvkObjectRef<T>>(std::move(value));
  }
  void clear() { objects.clear(); }
};
struct TestDraw {
  Rc<SnapshotSlice> positionSnapshotLease, indexSnapshotLease, uvSnapshotLease;
};
static Rc<SnapshotSlice> claim(const std::shared_ptr<Page>& page, uint64_t off, uint64_t size) {
  auto* raw = page->table.claim(page, off, size);
  CHECK(raw);
  Rc<SnapshotSlice> result(raw); raw->decRef(); return result;
}
int main() {
  BudgetGrowthRetryGate retry;
  CHECK(!retry.refused(0,64)); retry.refuse(0,64); CHECK(retry.refused(0,64));
  CHECK(!retry.refused(0,32)); retry.refuse(0,32); CHECK(retry.refused(0,64));
  CHECK(!retry.refused(1,64)); retry.refuse(1,64); CHECK(retry.refused(1,64));
  CHECK(!retry.refused(0,64)); retry.reset(); CHECK(!retry.refused(1,64));
  AdaptiveBudgetInput input{};
  input.supported = input.heapValid = input.vaValid = true;
  input.heapSize = input.budget = 8 * 1024 * MemoryMiB;
  input.committed = 2 * 1024 * MemoryMiB; input.availableVA = 512 * MemoryMiB;
  input.resident = 384 * MemoryMiB; input.hardCap = 512 * MemoryMiB;
  input.fallbackCap = 384 * MemoryMiB; input.pageBytes = 16 * MemoryMiB;
  auto decision = DecideAdaptiveMemoryBudget(input);
  CHECK(decision.trusted && decision.target == 512 * MemoryMiB);
  CHECK(decision.canGrow(input.resident, 16 * MemoryMiB));
  input.budget = 2 * 1024 * MemoryMiB; input.committed = 1024 * MemoryMiB;
  decision = DecideAdaptiveMemoryBudget(input);
  CHECK(decision.target == 256 * MemoryMiB);
  CHECK(!decision.canGrow(input.resident, 16 * MemoryMiB));
  input.committed = input.budget;
  CHECK(DecideAdaptiveMemoryBudget(input).target == 0);
  input.committed = UINT64_MAX;
  CHECK(DecideAdaptiveMemoryBudget(input).headroom == 0);
  input.budget = input.heapSize + 1;
  CHECK(!DecideAdaptiveMemoryBudget(input).trusted);
  CHECK(DecideAdaptiveMemoryBudget(input).target == 384 * MemoryMiB);
  input.availableVA = 255 * MemoryMiB;
  CHECK(!DecideAdaptiveMemoryBudget(input).canGrow(0, 16 * MemoryMiB));
  input.availableVA = 512 * MemoryMiB; input.vaValid = false;
  CHECK(DecideAdaptiveMemoryBudget(input).target == 0);
  input.vaValid = true; input.pageBytes = 0;
  CHECK(DecideAdaptiveMemoryBudget(input).target == 0);
  input.pageBytes = 64 * MemoryMiB; input.quotaEighths = 3;
  input.hardCap = input.fallbackCap = 1152 * MemoryMiB;
  input.budget = input.heapSize; input.committed = 0;
  CHECK(DecideAdaptiveMemoryBudget(input).target == 1152 * MemoryMiB);

  ArenaTrimHistory trim;
  CHECK(trim.target(1, 0, 384 * MemoryMiB, 64 * MemoryMiB, false) == 384 * MemoryMiB);
  CHECK(trim.target(120, 0, 384 * MemoryMiB, 64 * MemoryMiB, false) == 384 * MemoryMiB);
  CHECK(trim.target(121, 0, 384 * MemoryMiB, 64 * MemoryMiB, false) == 320 * MemoryMiB);
  CHECK(trim.target(122, 320 * MemoryMiB, 320 * MemoryMiB, 64 * MemoryMiB, true) == 320 * MemoryMiB);

  auto page = std::make_shared<Page>();
  auto anchor = claim(page, 0, 256);
  auto slice = claim(page, 256, 16 * MemoryMiB - 256);
  Rc<SnapshotSlice> queuedCopy = slice, caster = slice, gpuCommand = slice;
  slice = nullptr;
  uint64_t offset = 0;
  CHECK(!page->table.findHole(16 * MemoryMiB, 512 * 1024, offset));
  queuedCopy = nullptr; caster = nullptr;
  CHECK(!page->table.findHole(16 * MemoryMiB, 512 * 1024, offset));
  // Completion is modeled by releasing the same Rc that production submits
  // to DxvkCommandList::track. This is NOT a GPU/fence test.
  std::thread completion([&] { gpuCommand = nullptr; }); completion.join();
  CHECK(page->table.findHole(16 * MemoryMiB, 512 * 1024, offset) && offset == 256);
  CHECK(page->table.liveBytes() == 256);
  auto reused = claim(page, offset, 512 * 1024);
  auto uvAlias = reused;
  reused = nullptr;
  CHECK(page->table.liveBytes() == 256 + 512 * 1024);
  uvAlias = nullptr;
  CHECK(page->table.liveBytes() == 256);

  auto tracked = claim(page, 256, 16 * MemoryMiB - 256);
  TestCommandTracker copyTracker, drawTracker;
  TestDraw draw{tracked, tracked, tracked};
  copyTracker.track(tracked);
  TrackSnapshotSlices(drawTracker, draw);
  CHECK(drawTracker.calls == 2); // UV alias is retained once, not twice
  draw = {};
  tracked = nullptr;
  CHECK(!page->table.findHole(16 * MemoryMiB, 512 * 1024, offset));
  copyTracker.clear();
  CHECK(!page->table.findHole(16 * MemoryMiB, 512 * 1024, offset));
  drawTracker.clear();
  CHECK(page->table.findHole(16 * MemoryMiB, 512 * 1024, offset));

  // Same-class pinning regression: original 24 pages / 384 MiB, only 6144 B
  // anchors survive. All 24 pages now accept a 512 KiB slice with no new page.
  std::vector<std::shared_ptr<Page>> pages;
  std::vector<Rc<SnapshotSlice>> anchors, pending;
  for (unsigned p = 0; p < 24; ++p) {
    pages.push_back(std::make_shared<Page>());
    anchors.push_back(claim(pages.back(), 0, 256));
    for (unsigned i = 0; i < 31; ++i)
      pending.push_back(claim(pages.back(), 256 + i * 512 * 1024, 512 * 1024));
  }
  pending.clear();
  for (const auto& p : pages) {
    CHECK(p->table.liveBytes() == 256);
    CHECK(p->table.findHole(16 * MemoryMiB, 512 * 1024, offset));
    auto live = claim(p, offset, 512 * 1024);
    CHECK(live->offset() == 256 && live->size() == 512 * 1024);
  }
  // Capacity exhausted is explicit, not an untracked slice; free slot returns.
  auto smallOwner = std::make_shared<int>(1);
  SnapshotSliceTable<2> tiny;
  auto* a = tiny.claim(smallOwner, 0, 256); auto* b = tiny.claim(smallOwner, 256, 256);
  CHECK(a && b && !tiny.claim(smallOwner, 512, 256) && !tiny.hasSlot());
  a->decRef(); CHECK(tiny.hasSlot()); b->decRef();
  const auto allocationStart = allocations.load();
  const auto clockStart = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < 10000; ++i) {
    auto* test = tiny.claim(smallOwner, 0, 256);
    CHECK(test);
    test->decRef();
    CHECK(tiny.findHole(512, 512, offset) && offset == 0);
  }
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - clockStart).count();
  CHECK(allocations.load() == allocationStart);
  std::printf("claim/release/hole: 10000 iterations, %lld us, zero heap allocations; table=%zu bytes/page\n",
      static_cast<long long>(micros), sizeof(SnapshotSliceTable<>));
  // Real cross-thread final release, with acquire/release handoff. The owner
  // alone changes ranges; completion cannot mutate the interval table.
  std::atomic<SnapshotSlice*> mailbox{nullptr};
  std::atomic<bool> done{false};
  std::thread releaser([&] {
    while (!done.load(std::memory_order_acquire)) {
      if (auto* item = mailbox.exchange(nullptr, std::memory_order_acq_rel)) item->decRef();
      else std::this_thread::yield();
    }
    if (auto* item = mailbox.exchange(nullptr)) item->decRef();
  });
  for (unsigned i = 0; i < 10000; ++i) {
    auto* item = tiny.claim(smallOwner, 0, 256);
    while (!item) { std::this_thread::yield(); item = tiny.claim(smallOwner, 0, 256); }
    // Wait for previous release BEFORE assigning the same address again.
    while (mailbox.load(std::memory_order_acquire)) std::this_thread::yield();
    mailbox.store(item, std::memory_order_release);
    while (!item->available()) std::this_thread::yield();
  }
  done.store(true, std::memory_order_release); releaser.join();
  CHECK(tiny.liveBytes() == 0);
  // Maximum-density table: adversarial fragmentation, not the tiny-table
  // benchmark above. Each free gap is only 256 B, so a 512 B request must fail.
  auto dense = std::make_shared<Page>();
  std::array<Rc<SnapshotSlice>, 2048> denseLeases;
  for (unsigned i = 0; i < denseLeases.size(); ++i)
    denseLeases[i] = claim(dense, (2047 - i) * 512, 256);
  CHECK(!dense->table.hasSlot());
  const auto denseAllocs = allocations.load();
  const auto denseStart = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < 1000; ++i)
    CHECK(!dense->table.findHole(2048 * 512, 512, offset));
  const auto denseMicros = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - denseStart).count();
  CHECK(allocations.load() == denseAllocs);
  CHECK(dense->table.findHole(2048 * 512, 256, offset)); // smaller query bypasses miss
  CHECK(dense->table.findHole(2048 * 512 + 512, 512, offset)); // capacity is part of cache
  denseLeases[1024] = nullptr;
  CHECK(dense->table.hasSlot());
  CHECK(dense->table.findHole(2048 * 512, 512, offset));
  auto holeLease = claim(dense, offset, 512);
  CHECK(!dense->table.findHole(2048 * 512, 512, offset));
  holeLease = nullptr;
  CHECK(dense->table.findHole(2048 * 512, 512, offset)); // release invalidates miss
  std::printf("dense 2048-slot census: 1000 failed searches, %lld us, zero heap allocations; NOT frame cost\n",
      static_cast<long long>(denseMicros));
  // Retiring the map drops the pool lookup, not a live lease's page.
  std::weak_ptr<Page> weak = page;
  page.reset(); CHECK(!weak.expired()); anchor = nullptr; CHECK(weak.expired());
  std::printf("adaptive/slice: %u checks PASS; CPU completion model, not GPU proof\n", checks);
}
