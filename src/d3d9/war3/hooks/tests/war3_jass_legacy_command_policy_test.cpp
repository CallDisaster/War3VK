#include "../war3_jass_legacy_command_policy.h"

#include <iostream>
#include <string>

namespace {

using namespace dxvk::war3::hooks::legacy;

int g_failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++g_failures;
  }
}

void TestBuildCapability() {
#if defined(WARVK_EXPECT_LEGACY_JASS_COMMANDS_DEV) && \
    WARVK_EXPECT_LEGACY_JASS_COMMANDS_DEV
  Check(kDevelopmentCommandsEnabled,
        "dedicated developer test build must enable legacy commands");
#else
  Check(!kDevelopmentCommandsEnabled,
        "release/default build must disable legacy commands");
#endif
}

void TestFiniteFloatParser() {
  float value = 123.0f;
  Check(ParseFiniteFloat("0", value) && value == 0.0f,
        "zero must parse");
  Check(ParseFiniteFloat("-1.25", value) && value == -1.25f,
        "finite decimal must parse");
  Check(ParseFiniteFloat("1e3", value) && value == 1000.0f,
        "finite exponent must parse");
  Check(!ParseFiniteFloat("nan", value), "NaN must fail closed");
  Check(!ParseFiniteFloat("-nan", value), "signed NaN must fail closed");
  Check(!ParseFiniteFloat("inf", value), "infinity must fail closed");
  Check(!ParseFiniteFloat("-inf", value),
        "negative infinity must fail closed");
  Check(!ParseFiniteFloat("1e999", value), "overflow must fail closed");
  Check(!ParseFiniteFloat("1.0tail", value),
        "trailing characters must fail closed");
  Check(!ParseFiniteFloat(" 1.0", value),
        "leading whitespace must fail closed");
  Check(!ParseFiniteFloat("", value), "empty value must fail closed");
}

void TestFeaturePolicy() {
  constexpr uint32_t implemented =
      kFeatureSun | kFeatureCsm | kFeaturePointLight | kFeatureVolumetric |
      kFeatureDayNight | kFeatureLightning;

  Check(RequiredFeatureMask("set-sun-direction") == kFeatureSun,
        "sun command must require Sun");
  Check(RequiredFeatureMask("set-shadow-bias") == kFeatureCsm,
        "shadow command must require CSM");
  Check(RequiredFeatureMask("add-point-light") == kFeaturePointLight,
        "point command must require PointLight");
  Check(RequiredFeatureMask("set-volumetric-height-fog") ==
            kFeatureVolumetric,
        "volumetric command must require Volumetric");
  Check(RequiredFeatureMask("lightning-create") == kFeatureLightning,
        "lightning command must require Lightning");
  Check(RequiredFeatureMask("set-outline-enabled") == kFeatureOutline,
        "outline command must require its unavailable feature bit");
  Check(RequiredFeatureMask("unknown-command") == 0u,
        "unknown legacy command must fail closed");

#if defined(WARVK_EXPECT_LEGACY_JASS_COMMANDS_DEV) && \
    WARVK_EXPECT_LEGACY_JASS_COMMANDS_DEV
  Check(IsCommandAllowed("set-sun-direction", implemented),
        "developer build may use an implemented feature");
  Check(!IsCommandAllowed("set-sun-direction", 0u),
        "runtime-unavailable feature must be rejected");
  Check(!IsCommandAllowed("set-outline-enabled", implemented),
        "unimplemented feature must remain rejected in developer build");
  Check(!IsCommandAllowed("unknown-command", 0xffffffffu),
        "unknown command must remain rejected in developer build");
#else
  Check(!IsCommandAllowed("set-sun-direction", implemented),
        "release/default build must reject even implemented legacy commands");
#endif
}

} // namespace

int main() {
  TestBuildCapability();
  TestFiniteFloatParser();
  TestFeaturePolicy();
  if (g_failures != 0) {
    std::cerr << g_failures << " legacy command policy test(s) failed\n";
    return 1;
  }
  std::cout << "war3_jass_legacy_command_policy_test: PASS\n";
  return 0;
}
