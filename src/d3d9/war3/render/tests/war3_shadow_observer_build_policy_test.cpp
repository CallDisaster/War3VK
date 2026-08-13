#include "../war3_shadow_observer_build_policy.h"

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

namespace policy = dxvk::war3::render;

#if defined(WARVK_EXPECT_SHADOW_OBSERVERS_DEV) && \
    WARVK_EXPECT_SHADOW_OBSERVERS_DEV
constexpr bool kExpectedDevelopmentObservers = true;
#else
constexpr bool kExpectedDevelopmentObservers = false;
#endif

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "war3_shadow_observer_build_policy_test: " << message
              << '\n';
  return condition;
}

}  // namespace

int main() {
  using Mode = policy::War3ShadowObserverBuildMode;
  const bool releaseAndDevMatch =
      policy::kDevelopmentShadowObserversEnabled ==
      kExpectedDevelopmentObservers;
  const bool exactObserve =
      policy::ParseShadowObserverBuildMode(1u) == Mode::Observe;
  const bool allOtherModesOff =
      policy::ParseShadowObserverBuildMode(0u) == Mode::Off &&
      policy::ParseShadowObserverBuildMode(2u) == Mode::Off &&
      policy::ParseShadowObserverBuildMode(3u) == Mode::Off &&
      policy::ParseShadowObserverBuildMode(
          std::numeric_limits<uint32_t>::max()) == Mode::Off;

  return require(releaseAndDevMatch,
                 "build capability did not match the expected policy") &&
      require(exactObserve, "mode 1 did not resolve to Observe") &&
      require(allOtherModesOff, "a non-Observe mode was accepted")
      ? 0
      : 1;
}
