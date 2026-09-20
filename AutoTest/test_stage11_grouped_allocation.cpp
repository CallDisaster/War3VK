#include "../src/d3d9/war3/render/war3_stage11_lifetime_page_policy.h"
#include "../src/dxvk/dxvk_buffer_allocation_guard.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace dxvk::war3::render;
using Life = War3Stage11PageLifetime;
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (0)

void TestInitializationAndUnwind() {
  unsigned assigned = 0, published = 0, destroyed = 0;
  auto assign = [&](auto&&) { ++assigned; };
  auto publish = [&] { ++published; };
  try {
    dxvk::DxvkPublishInitialBufferStorage(std::unique_ptr<int>{}, assign, publish);
    CHECK(false);
  } catch (const dxvk::DxvkBufferAllocationError&) { }
  CHECK(assigned == 0 && published == 0);
  dxvk::DxvkPublishInitialBufferStorage(std::make_unique<int>(1), assign, publish);
  CHECK(assigned == 1 && published == 1);
  published = 0;
  try {
    dxvk::DxvkPublishInitialBufferStorage(std::make_unique<int>(1),
        [](auto&&) { throw std::bad_alloc(); }, publish);
    CHECK(false);
  } catch (const std::bad_alloc&) { }
  CHECK(published == 0);
  std::weak_ptr<int> allocation;
  try {
    auto storage = std::make_shared<int>(1);
    allocation = storage;
    std::shared_ptr<int> assignedStorage;
    dxvk::DxvkPublishInitialBufferStorage(std::move(storage),
        [&](auto&& s) { assignedStorage = std::move(s); },
        [] { throw std::bad_alloc(); });
  } catch (const std::bad_alloc&) { }
  CHECK(allocation.expired());
  for (bool transfer : {false, true}) {
    { dxvk::DxvkUnboundResourceGuard guard([&] { ++destroyed; });
      if (transfer) guard.release(); }
    CHECK(destroyed == 1);
  }
  try {
    dxvk::DxvkUnboundResourceGuard guard([&] { ++destroyed; });
    throw std::bad_alloc(); // requirements/diagnostic/allocation failure
  } catch (const std::bad_alloc&) { }
  CHECK(destroyed == 2);
  // Untyped errors (including device loss) must retain their original type.
  bool caughtGeneric = false;
  try {
    dxvk::DxvkPublishInitialBufferStorage(std::make_unique<int>(1),
        [](auto&&) { throw dxvk::DxvkError(std::string("device lost")); }, publish);
  } catch (const dxvk::DxvkBufferAllocationError&) { CHECK(false); }
    catch (const dxvk::DxvkError&) { caughtGeneric = true; }
  CHECK(caughtGeneric && published == 0);
}

struct Pool {
  std::vector<War3Stage11LifetimePageView> pages;
  uint64_t id = 1, peak = 0;
  bool grouped = true;
  uint64_t resident() const {
    uint64_t n = 0;
    for (const auto& p : pages) n += p.capacity;
    return n;
  }
  War3Stage11LifetimePagePlan allocate(uint64_t size, Life life, bool create = true) {
    War3Stage11LifetimePageRequest r{};
    r.requiredBytes = size; r.residentBytes = resident();
    r.mapEpoch = 1; r.deviceEpoch = 2;
    r.pageCreateGateOpen = create; r.sameRetentionIntentOnly = grouped;
    r.lifetime = grouped ? life : Life::Unknown;
    // Frozen old production behavior was newest-page-first, not the new
    // policy's best-fit selection with every page renamed Unknown.
    const auto choose = [&] {
      if (grouped)
        return War3PlanStage11LifetimePage(r, pages.data(), uint32_t(pages.size()));
      for (auto i = pages.rbegin(); i != pages.rend(); ++i) {
        const auto sub = War3PlanStage11SnapshotSuballocation(i->used, i->capacity, size);
        if (!sub.valid) continue;
        War3Stage11LifetimePagePlan result{};
        result.kind = War3Stage11LifetimePagePlanKind::ExistingPage;
        result.pageId = i->id; result.offset = sub.offset;
        result.nextUsed = sub.nextUsed;
        return result;
      }
      return War3PlanStage11LifetimePage(r, pages.data(), uint32_t(pages.size()));
    };
    auto plan = choose();
    if (plan.createsPage()) {
      pages.push_back({id++, plan.pageBytes, 0u, r.lifetime,
          War3Stage11PageOwnerState::Active, 1, 2});
      r.residentBytes = resident(); r.pageCreateGateOpen = false;
      plan = choose();
    }
    if (plan.usesExistingPage()) {
      for (auto& p : pages) if (p.id == plan.pageId) {
        CHECK(plan.offset >= p.used && plan.nextUsed <= p.capacity);
        p.used = plan.nextUsed;
      }
    }
    peak = std::max(peak, resident());
    CHECK(resident() <= kWar3Stage11SnapshotResidentCapBytes);
    return plan;
  }
};

void TestAnchorsDoNotPinTransientPages() {
  Pool legacy, grouped;
  legacy.grouped = false;
  std::vector<uint64_t> anchored;
  unsigned oldRejected = 0;
  for (unsigned cycle = 0; cycle < 80; ++cycle) {
    if (!legacy.allocate(15u << 20u, Life::ShortLived).safeToUse()) ++oldRejected;
    auto a = legacy.allocate(256u << 10u, Life::LongLived);
    if (a.safeToUse()) anchored.push_back(a.pageId);
    // Model the existing whole-page GC only; never rewind or reuse a hole.
    legacy.pages.erase(std::remove_if(legacy.pages.begin(), legacy.pages.end(),
        [&](const auto& p) { return std::find(anchored.begin(), anchored.end(), p.id)
                                   == anchored.end(); }), legacy.pages.end());
    CHECK(grouped.allocate(15u << 20u, Life::ShortLived).safeToUse());
    CHECK(grouped.allocate(256u << 10u, Life::LongLived).safeToUse());
    grouped.pages.erase(std::remove_if(grouped.pages.begin(), grouped.pages.end(),
        [](const auto& p) { return p.lifetime == Life::ShortLived; }), grouped.pages.end());
  }
  CHECK(oldRejected > 0);
  CHECK(grouped.peak <= 3u * kWar3Stage11SnapshotPageBytes);
  std::cout << "synthetic anchors: oldRejected=" << oldRejected
            << " groupedRejected=0 groupedPeakMiB=" << (grouped.peak >> 20) << '\n';
}

void TestFallbackAfterCreateFailureAndCap() {
  Pool p;
  CHECK(p.allocate(256, Life::LongLived).safeToUse());
  // The create callback failed before publication: unchanged pool, retry the
  // same production policy with create disabled. Never request another page.
  auto fallback = p.allocate(512u << 10u, Life::ShortLived, false);
  CHECK(fallback.usesExistingPage() && fallback.mixedLifetimeBorrow);
  CHECK(p.pages.size() == 1);
  Pool full;
  for (unsigned i = 0; i < 24; ++i)
    CHECK(full.allocate(kWar3Stage11SnapshotPageBytes - 256u, Life::LongLived).safeToUse());
  CHECK(full.allocate(256, Life::ShortLived).mixedLifetimeBorrow);
  CHECK(!full.allocate(512, Life::ShortLived).safeToUse());
  CHECK(full.resident() == (384u << 20u));
}

void TestPendingBufferOwnerSurvivesPageRemoval() {
  struct Page { std::shared_ptr<int> buffer = std::make_shared<int>(17); };
  auto page = std::make_shared<Page>();
  std::weak_ptr<int> allocation = page->buffer;
  auto queuedCommand = page->buffer;
  page.reset();
  CHECK(!allocation.expired() && *queuedCommand == 17);
  queuedCommand.reset();
  CHECK(allocation.expired());
}

void TestSameClassPinningRemainsVisible() {
  // Negative capacity witness, not GPU recreation: use the production planner
  // and model whole-page GC with one surviving 256-byte anchor per page.
  Pool pool;
  for (unsigned page = 0; page != 24; ++page) {
    CHECK(pool.allocate(256, Life::ShortLived).safeToUse());
    while (pool.pages.back().used < pool.pages.back().capacity) {
      const uint64_t remaining = pool.pages.back().capacity - pool.pages.back().used;
      CHECK(pool.allocate(std::min<uint64_t>(remaining, 512u << 10u), Life::ShortLived).safeToUse());
    }
  }
  CHECK(pool.pages.size() == 24);
  CHECK(pool.resident() == (384ull << 20));
  CHECK(!pool.allocate(512u << 10u, Life::ShortLived).safeToUse());
  std::cout << "UNRESOLVED same-class pinning: anchors=6144 bytes residentMiB=384 denied=1\n";
  // All anchors are explicitly released in this model; never infer this from
  // camera movement, low logical payload or a producer fence alone.
  pool.pages.clear();
  CHECK(pool.allocate(512u << 10u, Life::ShortLived).safeToUse());
}

int main() {
  try {
    TestInitializationAndUnwind();
    TestAnchorsDoNotPinTransientPages();
    TestSameClassPinningRemainsVisible();
    TestFallbackAfterCreateFailureAndCap();
    TestPendingBufferOwnerSurvivesPageRemoval();
    std::cout << "grouped allocation CPU checks=" << checks << " PASS; no GPU proof\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
