#pragma once

#include <cstdint>

namespace dxvk::war3::render {

enum class War3ShadowObserverBuildMode : uint8_t {
  Off = 0u,
  Observe = 1u,
};

#if defined(WARVK_ENABLE_SHADOW_OBSERVERS_DEV) && \
    WARVK_ENABLE_SHADOW_OBSERVERS_DEV
inline constexpr bool kDevelopmentShadowObserversEnabled = true;
#else
inline constexpr bool kDevelopmentShadowObserversEnabled = false;
#endif

constexpr War3ShadowObserverBuildMode
ParseShadowObserverBuildMode(uint32_t configuredMode) noexcept {
  return configuredMode == 1u
      ? War3ShadowObserverBuildMode::Observe
      : War3ShadowObserverBuildMode::Off;
}

}  // namespace dxvk::war3::render
