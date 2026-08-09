#pragma once

#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace dxvk::war3::hooks::legacy {

// The legacy warvk:cmd: surface is a developer-only diagnostic bridge.  It is
// intentionally impossible to enable through an environment variable so a
// release binary cannot inherit this authority from a launcher or user shell.
#if defined(WARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV) && \
    WARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV
inline constexpr bool kDevelopmentCommandsEnabled = true;
#else
inline constexpr bool kDevelopmentCommandsEnabled = false;
#endif

inline constexpr uint32_t kFeatureSun = 0x00000001u;
inline constexpr uint32_t kFeatureCsm = 0x00000002u;
inline constexpr uint32_t kFeaturePointLight = 0x00000004u;
inline constexpr uint32_t kFeatureVolumetric = 0x00000008u;
inline constexpr uint32_t kFeatureOutline = 0x00000010u;
inline constexpr uint32_t kFeatureBloom = 0x00000020u;
inline constexpr uint32_t kFeaturePostFx = 0x00000040u;
inline constexpr uint32_t kFeatureAa = 0x00000080u;
inline constexpr uint32_t kFeatureDayNight = 0x00000100u;
inline constexpr uint32_t kFeatureLightning = 0x00000200u;

template <size_t N>
inline bool Contains(const std::array<std::string_view, N>& values,
                     std::string_view candidate) noexcept {
  for (const std::string_view value : values) {
    if (value == candidate)
      return true;
  }
  return false;
}

// Returns zero for unknown commands.  Every known command requires a real
// public WarVK feature bit; there is no unqualified diagnostic mutation.
inline uint32_t RequiredFeatureMask(std::string_view command) noexcept {
  static constexpr std::array<std::string_view, 4u> SunCommands = {{
      "set-lighting-enabled", "set-sun-direction", "set-sun-color",
      "set-sun-intensity",
  }};
  static constexpr std::array<std::string_view, 5u> CsmCommands = {{
      "set-shadow-enabled", "set-shadow-strength", "set-shadow-bias",
      "set-shadow-pcf-radius", "set-shadow-debug-mode",
  }};
  static constexpr std::array<std::string_view, 10u> PointLightCommands = {{
      "add-point-light", "update-point-light", "update-point-light-ex",
      "set-point-light-shadow", "remove-point-light", "clear-point-lights",
      "get-point-light-count", "set-point-lights-enabled",
      "set-point-shadow-enabled", "set-point-shadow-bias",
  }};
  static constexpr std::array<std::string_view, 6u> VolumetricCommands = {{
      "set-volumetric-light-enabled", "set-volumetric-enabled",
      "set-volumetric-light-params", "set-volumetric-light-fade",
      "set-volumetric-height-fog", "set-volumetric-resolution-divisor",
  }};
  static constexpr std::array<std::string_view, 8u> OutlineCommands = {{
      "outline-add-handle", "outline-remove-handle", "outline-clear-handles",
      "set-outline-enabled", "set-outline-width", "set-outline-color",
      "set-outline-mode", "set-outline-visibility",
  }};
  static constexpr std::array<std::string_view, 5u> BloomCommands = {{
      "bloom-add-handle", "bloom-remove-handle", "bloom-clear-handles",
      "set-bloom-enabled", "set-bloom-params",
  }};
  static constexpr std::array<std::string_view, 5u> PostFxCommands = {{
      "set-postfx-enabled", "set-exposure", "set-aces-enabled",
      "set-ssao-enabled", "set-ssao-params",
  }};
  static constexpr std::array<std::string_view, 3u> AaCommands = {{
      "set-aa-mode", "set-fxaa-params", "set-smaa-params",
  }};
  static constexpr std::array<std::string_view, 3u> DayNightCommands = {{
      "set-day-night-enabled", "set-day-night-min-factor",
      "set-day-night-ambient",
  }};
  static constexpr std::array<std::string_view, 10u> LightningCommands = {{
      "lightning-create", "lightning-move", "lightning-destroy",
      "lightning-set-color", "lightning-set-width", "lightning-set-curve",
      "lightning-set-lifetime", "lightning-set-pulse",
      "lightning-active-count", "lightning-stats",
  }};

  if (Contains(SunCommands, command))
    return kFeatureSun;
  if (Contains(CsmCommands, command))
    return kFeatureCsm;
  if (Contains(PointLightCommands, command))
    return kFeaturePointLight;
  if (Contains(VolumetricCommands, command))
    return kFeatureVolumetric;
  if (Contains(OutlineCommands, command))
    return kFeatureOutline;
  if (Contains(BloomCommands, command))
    return kFeatureBloom;
  if (Contains(PostFxCommands, command))
    return kFeaturePostFx;
  if (Contains(AaCommands, command))
    return kFeatureAa;
  if (Contains(DayNightCommands, command))
    return kFeatureDayNight;
  if (Contains(LightningCommands, command))
    return kFeatureLightning;
  return 0u;
}

inline bool IsCommandAllowed(std::string_view command,
                             uint32_t availableFeatures) noexcept {
  const uint32_t required = RequiredFeatureMask(command);
  return kDevelopmentCommandsEnabled && required != 0u &&
      (availableFeatures & required) == required;
}

inline bool ParseFiniteFloat(const std::string& value, float& output) noexcept {
  if (value.empty())
    return false;
  for (const unsigned char character : value) {
    if (character <= 0x20u || character == 0x7fu)
      return false;
  }

  errno = 0;
  char* end = nullptr;
  const float parsed = std::strtof(value.c_str(), &end);
  if (errno == ERANGE || end != value.c_str() + value.size() ||
      !std::isfinite(parsed))
    return false;
  output = parsed;
  return true;
}

} // namespace dxvk::war3::hooks::legacy
