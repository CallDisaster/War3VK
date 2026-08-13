#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dxvk::war3::render {

inline float SanitizeShadowAlphaFarRefBias(float configuredBias) noexcept {
  if (!std::isfinite(configuredBias) || configuredBias < 0.0f)
    return 0.0f;

  return std::clamp(configuredBias, 0.0f, 1.0f);
}

inline float ShadowAlphaRefBiasForCascade(float configuredBias,
                                          uint32_t cascadeIndex,
                                          uint32_t cascadeCount) noexcept {
  const float bias = SanitizeShadowAlphaFarRefBias(configuredBias);
  if (cascadeCount <= 1u)
    return 0.0f;

  const uint32_t lastCascade = cascadeCount - 1u;
  const uint32_t safeCascade = std::min(cascadeIndex, lastCascade);
  return bias * (float(safeCascade) / float(lastCascade));
}

}  // namespace dxvk::war3::render
