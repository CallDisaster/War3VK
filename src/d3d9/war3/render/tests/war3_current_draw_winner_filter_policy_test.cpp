#include "../war3_current_draw_winner_filter_policy.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

struct Winner {
  uint32_t id = 0u;
  bool exactOwned = false;
};

void TestRejectedTopWinnerDoesNotBackfill() {
  const std::vector<Winner> canonicalOrder = {
      {10u, true},
      {20u, false},
  };
  std::vector<uint32_t> observed;
  std::vector<uint32_t> emitted;
  const auto outcome =
      dxvk::war3::render::MaterializeCurrentDrawCanonicalWinnerPrefix(
          canonicalOrder, 1u,
          [](const Winner& winner) -> const Winner& { return winner; },
          [&](const Winner& winner) { observed.push_back(winner.id); },
          [](const Winner& winner) { return !winner.exactOwned; },
          [&](const Winner& winner) { emitted.push_back(winner.id); });
  assert(outcome.canonicalWinnerCount == 1u);
  assert(outcome.filteredWinnerCount == 1u);
  assert((observed == std::vector<uint32_t>{10u}));
  assert(emitted.empty());
}

void TestFilteringPreservesCanonicalOrder() {
  const std::vector<Winner> canonicalOrder = {
      {10u, false},
      {20u, true},
      {30u, false},
  };
  std::vector<uint32_t> emitted;
  const auto outcome =
      dxvk::war3::render::MaterializeCurrentDrawCanonicalWinnerPrefix(
          canonicalOrder, 3u,
          [](const Winner& winner) -> const Winner& { return winner; },
          [](const Winner&) {},
          [](const Winner& winner) { return !winner.exactOwned; },
          [&](const Winner& winner) { emitted.push_back(winner.id); });
  assert(outcome.canonicalWinnerCount == 3u);
  assert(outcome.filteredWinnerCount == 1u);
  assert((emitted == std::vector<uint32_t>{10u, 30u}));
}

void TestUnboundedPrefixObservesEveryWinnerOnce() {
  const std::vector<Winner> canonicalOrder = {
      {1u, false}, {2u, false}, {3u, false},
  };
  uint32_t observed = 0u;
  uint32_t emitted = 0u;
  const auto outcome =
      dxvk::war3::render::MaterializeCurrentDrawCanonicalWinnerPrefix(
          canonicalOrder, 0u,
          [](const Winner& winner) -> const Winner& { return winner; },
          [&](const Winner&) { observed++; },
          [](const Winner&) { return true; },
          [&](const Winner&) { emitted++; });
  assert(outcome.canonicalWinnerCount == 3u);
  assert(outcome.filteredWinnerCount == 0u);
  assert(observed == 3u);
  assert(emitted == 3u);
}

} // namespace

int main() {
  TestRejectedTopWinnerDoesNotBackfill();
  TestFilteringPreservesCanonicalOrder();
  TestUnboundedPrefixObservesEveryWinnerOnce();
  return 0;
}
