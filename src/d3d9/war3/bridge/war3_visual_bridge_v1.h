#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#define WARVK_VISUAL_V1_CALL __cdecl
#elif defined(__GNUC__) && defined(__i386__)
#define WARVK_VISUAL_V1_CALL __attribute__((cdecl))
#else
#define WARVK_VISUAL_V1_CALL
#endif

// Narrow, versioned C ABI between war3map.dll and the DXVK renderer.  This is
// deliberately not a native/JAPI registry, function table or raw renderer
// object surface: callers resolve each exact export and verify ABI + manifest.
extern "C" {

std::uint32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_GetAbiVersion();
const char* WARVK_VISUAL_V1_CALL WarVKVisualV1_GetManifestSha256();
std::uint32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_GetFeatureFlags();
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_IsRuntimeReady();

std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightCreate(
    float x, float y, float z, float radius,
    float red, float green, float blue, float intensity);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightDestroy(
    std::int32_t lightId);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightSetEnabled(
    std::int32_t lightId, std::int32_t enabled);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightSetPosition(
    std::int32_t lightId, float x, float y, float z);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightSetColorIntensity(
    std::int32_t lightId, float red, float green, float blue, float intensity);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightSetRadius(
    std::int32_t lightId, float radius);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightSetShadowEnabled(
    std::int32_t lightId, std::int32_t enabled);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightSetShadowConfig(
    std::int32_t lightId, std::int32_t resolution, float bias);
std::int32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightIsAlive(
    std::int32_t lightId);
std::uint32_t WARVK_VISUAL_V1_CALL WarVKVisualV1_PointLightCount();

}

#undef WARVK_VISUAL_V1_CALL
