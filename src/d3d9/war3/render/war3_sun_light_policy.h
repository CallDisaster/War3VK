#pragma once

#include <cmath>

namespace dxvk::war3::render {

// Disable direct irradiance, not the light's independent ambient contribution.
// The caller selects the native main directional light; point/spot lights and
// unrelated slots must never be passed here. No settings or resource ownership.
template<typename Light>
inline void ApplyWar3DisabledSun(Light& light) noexcept {
  light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 0.0f;
  light.Specular.r = light.Specular.g = light.Specular.b = 0.0f;
}

inline float War3SunDirectIntensity(bool enabled, float intensity) noexcept {
  return enabled && std::isfinite(intensity) && intensity > 0.0f
      ? intensity : 0.0f;
}

} // namespace dxvk::war3::render
