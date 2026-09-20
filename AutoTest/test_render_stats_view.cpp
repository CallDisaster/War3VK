#include "../src/d3d9/war3/ui/war3_render_stats_view.h"
#include "../src/d3d9/war3/memory/war3_shadow_arena_stats.h"
#include "../src/d3d9/war3/memory/war3_snapshot_pool_stats.h"
#include <array>
#include <cstdio>
#include <cstdint>
#include <type_traits>

int main() {
  using namespace dxvk::war3::ui;
  int checks = 0, failures = 0;
  const auto check = [&](bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
  };
  RenderStatsRefresh gate;
  check(gate.due(0), "zero is valid first sample");
  check(!gate.due(0), "no second sample at zero");
  for (uint64_t ms = 1; ms < 250; ++ms)
    check(!gate.due(ms), "bounded poll frequency");
  check(gate.due(250), "quarter second boundary");
  check(!gate.due(499), "next quarter excluded");
  check(gate.due(500), "next boundary");
  check(gate.due(20000), "long hidden interval");
  check(gate.due(7), "clock rollback permits refresh");
  gate.reset();
  check(gate.due(7), "UI/device restart discards old timing");
  check(!gate.due(8), "restart re-establishes bound");
  check(BytesToMiB(0) == 0, "zero bytes");
  check(BytesToMiB(uint64_t(1) << 20) == 1, "MiB not MB");
  check(BytesToMiB(uint64_t(1) << 32) == 4096, "no Win32 truncation");
  check(BudgetFraction(1, 0) == 0, "zero budget no divide");
  check(BudgetFraction(1, 4) == .25f, "fraction");
  check(BudgetFraction(4, 4) == 1, "exact limit");
  check(BudgetFraction(UINT64_MAX, 4) == 1, "over limit clamped without overflow");
  check(BudgetFraction(UINT64_MAX / 2, UINT64_MAX) > .49f, "wide ratio");
  check(std::is_trivially_copyable_v<dxvk::war3::memory::ShadowArenaMemoryStats>, "no ownership retention");
  struct Page { uint64_t used; } p0{10}, p1{20};
  struct Stats {
    uint64_t drawTimeSnapshotPageResidentBytes = 0;
    uint64_t drawTimeSnapshotPageUsedBytes = 0;
    uint64_t drawTimeSnapshotPageReclaimedCount = 0;
    uint64_t frameCreateCount = 0;
  } stats;
  std::array<Page*, 2> pages{&p0, &p1};
  const auto sample = [&] {
    dxvk::war3::memory::SampleSnapshotPoolStats(pages, 64, 7, stats);
  };
  sample();
  check(stats.drawTimeSnapshotPageResidentBytes == 64, "pool capacity sampled");
  check(stats.drawTimeSnapshotPageUsedBytes == 30, "pool used sampled");
  for (int i = 0; i < 100; ++i) {
    stats = {}; // cache-hit-only frame: no allocations, but pages still exist
    sample();
    check(stats.drawTimeSnapshotPageResidentBytes == 64 &&
        stats.drawTimeSnapshotPageUsedBytes == 30 &&
        stats.drawTimeSnapshotPageReclaimedCount == 7 &&
        stats.frameCreateCount == 0, "frame reset does not invent zero residency");
  }
  std::array<Page*, 1> retained{&p1};
  dxvk::war3::memory::SampleSnapshotPoolStats(retained, 32, 8, stats);
  check(stats.drawTimeSnapshotPageUsedBytes == 20 &&
      stats.drawTimeSnapshotPageResidentBytes == 32, "reclamation decreases real gauges");
  std::array<Page*, 0> empty{};
  dxvk::war3::memory::SampleSnapshotPoolStats(empty, 0, 9, stats);
  check(stats.drawTimeSnapshotPageResidentBytes == 0 &&
      stats.drawTimeSnapshotPageUsedBytes == 0, "real reset publishes zero, not old maximum");
  p0.used = UINT64_MAX; p1.used = 1;
  sample();
  check(stats.drawTimeSnapshotPageUsedBytes == UINT64_MAX, "saturating sum");
  std::printf("render-stats-view: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
