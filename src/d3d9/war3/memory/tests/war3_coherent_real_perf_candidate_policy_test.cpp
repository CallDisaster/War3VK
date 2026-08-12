#include "../war3_coherent_real_index_trim_contract.h"

#include <cassert>

int main() {
  using namespace dxvk::war3::memory;

  static_assert(kCoherentRealIndexTrimDevelopmentEnabled);
  static_assert(kCoherentRealPerformanceCandidateEnabled);
  static_assert(DefaultWar3CoherentRealIndexTrimConfiguredMode() == 0u);
  static_assert(!DefaultWar3CoherentRealHintDomainEnabled());

  assert(ParseWar3CoherentRealIndexTrimMode(
             DefaultWar3CoherentRealIndexTrimConfiguredMode()) ==
         War3CoherentRealIndexTrimMode::Off);
  assert(ParseWar3CoherentRealIndexTrimMode(0u) ==
         War3CoherentRealIndexTrimMode::Off);
  assert(ParseWar3CoherentRealIndexTrimMode(1u) ==
         War3CoherentRealIndexTrimMode::Observe);
  assert(ParseWar3CoherentRealIndexTrimMode(2u) ==
         War3CoherentRealIndexTrimMode::Consume);
  assert(ParseWar3CoherentRealIndexTrimMode(3u) ==
         War3CoherentRealIndexTrimMode::Off);
  return 0;
}
