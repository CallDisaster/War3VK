#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dxvk::war3::render {

struct CurrentDrawWinnerFilterOutcome {
  uint32_t canonicalWinnerCount = 0u;
  uint32_t filteredWinnerCount = 0u;
};

// Materializes only the already-selected canonical prefix. Filtering happens
// after the prefix is fixed, so a rejected winner can never resurrect a weaker
// duplicate or pull a lower-priority record across the historical bound.
template <typename WinnerRange, typename RecordAt, typename Observe,
          typename Keep, typename Emit>
inline CurrentDrawWinnerFilterOutcome
MaterializeCurrentDrawCanonicalWinnerPrefix(
    const WinnerRange& orderedWinners,
    size_t maxWinners,
    RecordAt&& recordAt,
    Observe&& observe,
    Keep&& keep,
    Emit&& emit) {
  const size_t canonicalCount = maxWinners == 0u
      ? orderedWinners.size()
      : std::min(orderedWinners.size(), maxWinners);
  CurrentDrawWinnerFilterOutcome outcome = {};
  outcome.canonicalWinnerCount = static_cast<uint32_t>(canonicalCount);
  for (size_t i = 0u; i < canonicalCount; ++i) {
    const auto& record = recordAt(orderedWinners[i]);
    observe(record);
    if (!keep(record)) {
      outcome.filteredWinnerCount++;
      continue;
    }
    emit(record);
  }
  return outcome;
}

} // namespace dxvk::war3::render
