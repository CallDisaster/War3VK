#include "../src/d3d9/war3/ui/war3_render_stats_view.h"
#include "../src/d3d9/war3/memory/war3_shadow_arena_stats.h"
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
  std::printf("render-stats-view: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
