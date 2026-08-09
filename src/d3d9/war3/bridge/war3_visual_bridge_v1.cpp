#include "war3_visual_bridge_v1.h"

#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_settings.h"
#define WAR3_SHADER_API_INTERNAL
#include "../../war3_shader_api.h"
#undef WAR3_SHADER_API_INTERNAL
#include "../war3.h"

#include <cmath>
#include <cstring>

namespace {

constexpr std::uint32_t kAbiVersion = 0x00010000u;
constexpr std::uint32_t kPointLightFeature = 0x00000004u;
constexpr const char* kManifestSha256 =
    "920872221B3836A5EFF69D3EC721915B21E0C4B5399C0F09F05B028CF46D27BF";

bool IsFinite(float value) {
  return std::isfinite(value);
}

bool IsValidLightId(std::int32_t lightId) {
  return lightId > 0 &&
         dxvk::War3LightManager::Instance().IsPointLightAlive(lightId);
}

bool IsValidResolution(std::int32_t resolution) {
  if (resolution < 128 || resolution > 2048)
    return false;
  const auto value = static_cast<std::uint32_t>(resolution);
  return (value & (value - 1u)) == 0u;
}

std::int32_t BoolResult(bool value) {
  return value ? 1 : 0;
}

} // namespace

extern "C" {

std::uint32_t __cdecl WarVKVisualV1_GetAbiVersion() {
  return kAbiVersion;
}

const char* __cdecl WarVKVisualV1_GetManifestSha256() {
  return kManifestSha256;
}

std::uint32_t __cdecl WarVKVisualV1_GetFeatureFlags() {
  return kPointLightFeature;
}

std::int32_t __cdecl WarVKVisualV1_IsRuntimeReady() {
  return BoolResult(dxvk::war3::HasActivePipeline());
}

std::int32_t __cdecl WarVKVisualV1_PointLightCreate(
    float x, float y, float z, float radius,
    float red, float green, float blue, float intensity) {
  if (!IsFinite(x) || !IsFinite(y) || !IsFinite(z) ||
      !IsFinite(radius) || radius <= 0.0f ||
      !IsFinite(red) || !IsFinite(green) || !IsFinite(blue) ||
      !IsFinite(intensity) || intensity < 0.0f)
    return 0;
  return war3shader::AddPointLight(
      x, y, z, radius, red, green, blue, intensity, 0.0f);
}

std::int32_t __cdecl WarVKVisualV1_PointLightDestroy(std::int32_t lightId) {
  return BoolResult(lightId > 0 && war3shader::RemovePointLight(lightId));
}

std::int32_t __cdecl WarVKVisualV1_PointLightSetEnabled(
    std::int32_t lightId, std::int32_t enabled) {
  if (enabled != 0 && enabled != 1)
    return 0;
  return BoolResult(dxvk::War3LightManager::Instance().SetPointLightActive(
      lightId, enabled != 0));
}

std::int32_t __cdecl WarVKVisualV1_PointLightSetPosition(
    std::int32_t lightId, float x, float y, float z) {
  if (!IsFinite(x) || !IsFinite(y) || !IsFinite(z))
    return 0;
  return BoolResult(dxvk::War3LightManager::Instance().SetPointLightPosition(
      lightId, x, y, z));
}

std::int32_t __cdecl WarVKVisualV1_PointLightSetColorIntensity(
    std::int32_t lightId, float red, float green, float blue, float intensity) {
  if (!IsFinite(red) || !IsFinite(green) || !IsFinite(blue) ||
      !IsFinite(intensity) || intensity < 0.0f)
    return 0;
  return BoolResult(
      dxvk::War3LightManager::Instance().SetPointLightColorIntensity(
          lightId, red, green, blue, intensity));
}

std::int32_t __cdecl WarVKVisualV1_PointLightSetRadius(
    std::int32_t lightId, float radius) {
  if (!IsFinite(radius) || radius <= 0.0f)
    return 0;
  return BoolResult(dxvk::War3LightManager::Instance().SetPointLightRadius(
      lightId, radius));
}

std::int32_t __cdecl WarVKVisualV1_PointLightSetShadowEnabled(
    std::int32_t lightId, std::int32_t enabled) {
  if ((enabled != 0 && enabled != 1) || !IsValidLightId(lightId))
    return 0;
  const bool ok = war3shader::SetPointLightShadowIntensity(
      lightId, enabled != 0 ? 1.0f : 0.0f);
  if (ok && enabled != 0) {
    static_cast<void>(war3shader::SetPointLightsEnabled(true));
    static_cast<void>(war3shader::SetPointShadowEnabled(true));
  }
  return BoolResult(ok);
}

std::int32_t __cdecl WarVKVisualV1_PointLightSetShadowConfig(
    std::int32_t lightId, std::int32_t resolution, float bias) {
  if (!IsValidLightId(lightId) || !IsValidResolution(resolution) ||
      !IsFinite(bias) || bias < 0.0f)
    return 0;
  auto settings = dxvk::war3::GetMutableSettings();
  if (settings == nullptr)
    return 0;
  // The current renderer owns one global cube-map resolution/bias policy.
  // Keep the light id in this ABI so a future per-light implementation can
  // preserve the public protocol without breaking callers.
  settings->shadows.pointShadowResolution =
      static_cast<std::uint32_t>(resolution);
  settings->shadows.pointShadowBias = bias;
  settings->shadows.pointLightsEnabled = true;
  settings->shadows.pointShadowEnabled = true;
  return 1;
}

std::int32_t __cdecl WarVKVisualV1_PointLightIsAlive(std::int32_t lightId) {
  return BoolResult(IsValidLightId(lightId));
}

std::uint32_t __cdecl WarVKVisualV1_PointLightCount() {
  return war3shader::GetPointLightCount();
}

} // extern "C"
