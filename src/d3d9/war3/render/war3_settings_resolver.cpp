// war3_settings_resolver.cpp - see war3_settings_resolver.h
//
// Moved from d3d9_war3_pipeline.cpp without change (phase 5.1-c): the
// environment overrides of the constructor and the settings half of
// OnFrameStart.

#include "war3_settings_resolver.h"

#include "war3_render_state.h"
#include "../../d3d9_war3_debug.h"
#include "../../d3d9_war3_light.h"
#include "../../d3d9_war3_pipeline.h"
#include "../effects/war3_sun_cycle.h"
#include "../../../util/util_env.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

namespace dxvk {

    namespace {
        bool ParseEnvInt(const char* name, int& outValue) {
            const std::string v = env::getEnvVar(name);
            if (v.empty())
                return false;
            outValue = std::atoi(v.c_str());
            return true;
        }

        bool ParseEnvFloat(const char* name, float& outValue) {
            const std::string v = env::getEnvVar(name);
            if (v.empty())
                return false;
            char* end = nullptr;
            const float f = std::strtof(v.c_str(), &end);
            if (end == v.c_str() || !std::isfinite(f))
                return false;
            outValue = f;
            return true;
        }

        bool ParseInitialShadowTaaMode(War3ShadowTaaMode& outMode) {
            const std::string value =
                env::getEnvVar("DXVK_WAR3_SHADOW_TAA_MODE");
            if (value.empty())
                return false;

            if (value == "direct" || value == "direct_inline" ||
                value == "DirectInline") {
                outMode = War3ShadowTaaMode::DirectInline;
                return true;
            }
            if (value == "prepass" || value == "current" ||
                value == "prepass_current_only" ||
                value == "PrepassCurrentOnly") {
                outMode = War3ShadowTaaMode::PrepassCurrentOnly;
                return true;
            }
            if (value == "temporal" || value == "Temporal") {
                outMode = War3ShadowTaaMode::Temporal;
                return true;
            }

            char* end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0')
                return false;
            outMode = static_cast<War3ShadowTaaMode>(
                std::clamp<long>(parsed, 0, 2));
            return true;
        }

        bool ComputeDayNightFactor(float time01, const War3DayNightSettings& dayNight, float& outT) {
            if (time01 < 0.0f || time01 > 1.0f)
                return false;

            const float pi = 3.14159265f;
            float sunAnglePhase = (time01 - 0.25f) * 2.0f * pi;
            float realAltitudeRad = std::sin(sunAnglePhase) * (pi / 2.0f);
            float altDeg = realAltitudeRad * (180.0f / pi);

            float startDeg = dayNight.dayTransitionStartDeg;
            float endDeg = dayNight.dayTransitionEndDeg;
            if (endDeg <= startDeg)
                endDeg = startDeg + 0.01f;

            float t = (altDeg - startDeg) / (endDeg - startDeg);
            t = std::max(0.0f, std::min(1.0f, t));
            // smoothstep
            t = t * t * (3.0f - 2.0f * t);
            outT = t;
            return true;
        }

        float Wrap01(float value) {
            value -= std::floor(value);
            if (value < 0.0f)
                value += 1.0f;
            return value;
        }

        float ResolvePipelineDayNightTime01(
                float realGameTime,
                const War3DayNightSettings& dayNight) {
            if (dayNight.clockMode != War3LightingClockMode::GameTime)
                return Wrap01(dayNight.renderTimeHours / 24.0f);

            if (realGameTime >= 0.0f && realGameTime <= 24.0f)
                return Wrap01(realGameTime / 24.0f);

            static const auto s_start = std::chrono::steady_clock::now();
            static uint32_t s_fallbackLogCount = 0u;
            if (s_fallbackLogCount++ < 4u) {
                WAR3_RENDER_LOG(
                    "DXVK War3Pipeline: day-night using fallback time, realGameTime=%.4f\n",
                    static_cast<double>(realGameTime));
            }

            constexpr float kFallbackDayLengthSeconds = 480.0f;
            constexpr float kFallbackStartTime01 = 0.22f;
            const float elapsed =
                std::chrono::duration<float>(std::chrono::steady_clock::now() - s_start)
                    .count();
            return Wrap01(elapsed / kFallbackDayLengthSeconds +
                          kFallbackStartTime01);
        }
    }

    void ApplyWar3SettingsEnvironmentOverrides(War3RenderSettings& settings) {
        // 调试/调参：可用环境变量快速覆盖部分阴影参数，便于定位抽搐与采样伪影。
          // - DXVK_WAR3_SHADOW_DEBUG=0..9:
        //   0=None 1=Cascades 2=ShadowFactor 3=Depth 4=Motion 5=History
        //   6=PointShadow 7=CurrentVisibility(pre-TAA)
        // - DXVK_WAR3_SHADOW_STRENGTH=0..1
        // - DXVK_WAR3_SHADOW_BIAS>=0
        // - DXVK_WAR3_SHADOW_PCF>=0
        {
            int dbg = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_DEBUG", dbg)) {
                  dbg = std::clamp(dbg, 0, 9);
                settings.shadows.debugMode = static_cast<War3ShadowDebugMode>(dbg);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_DEBUG=%d\n", dbg);
            }

            float strength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_STRENGTH", strength)) {
                settings.shadows.strength = std::clamp(strength, 0.0f, 1.0f);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_STRENGTH=%.3f\n",
                               static_cast<double>(settings.shadows.strength));
            }

            float bias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_BIAS", bias)) {
                settings.shadows.receiverBias = std::max(bias, 0.0f);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_BIAS=%.6f\n",
                               static_cast<double>(settings.shadows.receiverBias));
            }

            int receiverMode = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_RECEIVER_MODE", receiverMode)) {
                receiverMode = std::clamp(receiverMode, 0, 2);
                settings.shadows.receiverMode =
                    static_cast<War3ShadowReceiverMode>(receiverMode);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_RECEIVER_MODE=%d\n",
                    receiverMode);
            }

            float normalBiasScale = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_NORMAL_BIAS_SCALE",
                              normalBiasScale)) {
                settings.shadows.normalBiasScale =
                    std::max(normalBiasScale, 0.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_NORMAL_BIAS_SCALE=%.6f\n",
                    static_cast<double>(
                        settings.shadows.normalBiasScale));
            }

            float cascadeBlendRange = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_CASCADE_BLEND_RANGE",
                              cascadeBlendRange)) {
                settings.shadows.cascadeBlendRange =
                    std::max(cascadeBlendRange, 0.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_CASCADE_BLEND_RANGE=%.3f\n",
                    static_cast<double>(
                        settings.shadows.cascadeBlendRange));
            }

            int stableSnap = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_STABLE_SNAP", stableSnap)) {
                settings.shadows.csm.stableSnap =
                    stableSnap != 0 ? 1.0f : 0.0f;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_STABLE_SNAP=%d\n",
                    stableSnap != 0 ? 1 : 0);
            }

            float splitLambda = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_SPLIT_LAMBDA", splitLambda)) {
                settings.shadows.csm.splitLambda =
                    std::clamp(splitLambda, 0.0f, 1.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_SPLIT_LAMBDA=%.3f\n",
                    static_cast<double>(
                        settings.shadows.csm.splitLambda));
            }

            float pcf = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_PCF", pcf)) {
                settings.shadows.pcfRadius = std::max(pcf, 0.0f);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_PCF=%.3f\n",
                               static_cast<double>(settings.shadows.pcfRadius));
            }

            int cascades = 0;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_CASCADES", cascades)) {
                cascades = std::clamp(cascades, 1, 4);
                settings.shadows.csm.cascadeCount = static_cast<uint32_t>(cascades);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_CASCADES=%d\n",
                               cascades);
            }

            int shadowRes = 0;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_RES", shadowRes)) {
                shadowRes = std::clamp(shadowRes, 512, 4096);
                settings.shadows.csm.shadowResolution =
                    static_cast<uint32_t>(shadowRes);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_RES=%d\n",
                               shadowRes);
            }

            int pcfKernel = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_PCF_KERNEL", pcfKernel)) {
                pcfKernel = std::clamp(pcfKernel, 0, 3);
                settings.shadows.pcfKernel =
                    static_cast<War3ShadowPcfKernel>(pcfKernel);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_PCF_KERNEL=%d\n",
                               pcfKernel);
            }

            War3ShadowTaaMode initialShadowTaaMode =
                settings.shadows.shadowTaaMode;
            if (ParseInitialShadowTaaMode(initialShadowTaaMode)) {
                settings.shadows.shadowTaaMode = initialShadowTaaMode;
                settings.shadows.shadowTaaEnabled =
                    initialShadowTaaMode == War3ShadowTaaMode::Temporal;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_TAA_MODE=%d\n",
                    static_cast<int>(initialShadowTaaMode));
            } else {
                // Compatibility fallback only. Parse it once during pipeline
                // construction so an in-game ImGui selection remains final.
                int shadowTaa = -1;
                if (ParseEnvInt("DXVK_WAR3_SHADOW_TAA", shadowTaa)) {
                    settings.shadows.shadowTaaEnabled = shadowTaa != 0;
                    settings.shadows.shadowTaaMode =
                        shadowTaa != 0
                            ? War3ShadowTaaMode::Temporal
                            : War3ShadowTaaMode::DirectInline;
                    WAR3_RENDER_LOG(
                        "DXVK War3Shadow: DXVK_WAR3_SHADOW_TAA=%d\n",
                        settings.shadows.shadowTaaEnabled ? 1 : 0);
                }
            }

            float shadowTaaWeight = 0.0f;
            if (ParseEnvFloat(
                    "DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT",
                    shadowTaaWeight)) {
                settings.shadows.shadowTaaBlendFactor =
                    std::clamp(shadowTaaWeight, 0.12f, 0.30f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: "
                    "DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT=%.3f\n",
                    static_cast<double>(
                        settings.shadows.shadowTaaBlendFactor));
            }

            int alphaHash = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_ALPHA_HASH", alphaHash)) {
                settings.shadows.alphaShadowHashed = alphaHash != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_ALPHA_HASH=%d\n",
                    settings.shadows.alphaShadowHashed ? 1 : 0);
            }

            int alphaMip = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_ALPHA_MIP", alphaMip)) {
                settings.shadows.alphaShadowUseMip = alphaMip != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_ALPHA_MIP=%d\n",
                    settings.shadows.alphaShadowUseMip ? 1 : 0);
            }

            float alphaMipBias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS",
                              alphaMipBias)) {
                settings.shadows.alphaShadowMipLodBias =
                    std::clamp(alphaMipBias, -4.0f, 4.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS=%.3f\n",
                    static_cast<double>(
                        settings.shadows.alphaShadowMipLodBias));
            }
        }

        // 抗锯齿参数：可用环境变量快速切换 AA 模式
        // - DXVK_WAR3_AA=0..5: 0=None 1=FXAA 2=SMAA_Low 3=SMAA_Medium 4=SMAA_High 5=SMAA_Ultra
        {
            int aaMode = -1;
            if (ParseEnvInt("DXVK_WAR3_AA", aaMode)) {
                aaMode = std::clamp(aaMode, 0, 5);
                settings.postFx.aa.mode = static_cast<War3AAMode>(aaMode);
                WAR3_RENDER_LOG("DXVK War3AA: DXVK_WAR3_AA=%d\n", aaMode);
            }

            float fxaaSubpix = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_FXAA_SUBPIX", fxaaSubpix)) {
                settings.postFx.aa.fxaaQualitySubpix = std::clamp(fxaaSubpix, 0.0f, 1.0f);
                WAR3_RENDER_LOG("DXVK War3AA: DXVK_WAR3_FXAA_SUBPIX=%.3f\n",
                               static_cast<double>(settings.postFx.aa.fxaaQualitySubpix));
            }
        }

        // 点光源运行时入口：默认关闭；可由 JASS 命令或环境变量打开。
        {
            int enabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_LIGHTS", enabled)) {
                settings.shadows.pointLightsEnabled = enabled != 0;
                WAR3_RENDER_LOG("DXVK War3PointLight: DXVK_WAR3_POINT_LIGHTS=%d\n",
                               settings.shadows.pointLightsEnabled ? 1 : 0);
            }

            int shadowEnabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW", shadowEnabled)) {
                settings.shadows.pointShadowEnabled = shadowEnabled != 0;
                WAR3_RENDER_LOG("DXVK War3PointLight: DXVK_WAR3_POINT_SHADOW=%d\n",
                               settings.shadows.pointShadowEnabled ? 1 : 0);
            }

            int pointShadowMaxLights = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS",
                            pointShadowMaxLights)) {
                pointShadowMaxLights = std::clamp(pointShadowMaxLights, 1, 4);
                settings.shadows.pointShadowMaxLights =
                    static_cast<uint32_t>(pointShadowMaxLights);
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointShadowMaxLights=%d\n",
                    pointShadowMaxLights);
            }

            int pointShadowResolution = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW_RESOLUTION",
                            pointShadowResolution)) {
                pointShadowResolution =
                    std::clamp(pointShadowResolution, 128, 2048);
                settings.shadows.pointShadowResolution =
                    static_cast<uint32_t>(pointShadowResolution);
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointShadowResolution=%d\n",
                    pointShadowResolution);
            }

            int pointShadowDebugLight = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW_DEBUG_LIGHT",
                            pointShadowDebugLight)) {
                pointShadowDebugLight = std::clamp(pointShadowDebugLight, 0, 3);
                settings.shadows.pointShadowDebugLightIndex =
                    static_cast<uint32_t>(pointShadowDebugLight);
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointShadowDebugLight=%d\n",
                    pointShadowDebugLight);
            }

            float pointShadowBias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_BIAS", pointShadowBias)) {
                settings.shadows.pointShadowBias = std::max(0.0f, pointShadowBias);
                WAR3_RENDER_LOG("DXVK War3PointLight: pointShadowBias=%.4f\n",
                               static_cast<double>(settings.shadows.pointShadowBias));
            }

            float pointShadowPcfNear = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_PCF_NEAR",
                              pointShadowPcfNear)) {
                settings.shadows.pointShadowPcfRadiusNear =
                    std::clamp(pointShadowPcfNear, 0.0f, 4.0f);
            }

            float pointShadowPcfFar = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_PCF_FAR",
                              pointShadowPcfFar)) {
                settings.shadows.pointShadowPcfRadiusFar =
                    std::clamp(pointShadowPcfFar,
                               settings.shadows.pointShadowPcfRadiusNear,
                               6.0f);
            }

            float pointShadowTexelBias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_TEXEL_BIAS",
                              pointShadowTexelBias)) {
                settings.shadows.pointShadowTexelBiasScale =
                    std::clamp(pointShadowTexelBias, 0.0f, 1.0f);
            }

            float pointShadowRangeFade = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_RANGE_FADE",
                              pointShadowRangeFade)) {
                settings.shadows.pointShadowRangeFadeStart =
                    std::clamp(pointShadowRangeFade, 0.50f, 0.98f);
            }

            int pointRayShadowEnabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW",
                            pointRayShadowEnabled)) {
                settings.shadows.pointRayShadowEnabled =
                    pointRayShadowEnabled != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointRayShadowEnabled=%d\n",
                    settings.shadows.pointRayShadowEnabled ? 1 : 0);
            }

            int pointRayShadowHiZEnabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_HIZ",
                            pointRayShadowHiZEnabled)) {
                settings.shadows.pointRayShadowHiZEnabled =
                    pointRayShadowHiZEnabled != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointRayShadowHiZEnabled=%d\n",
                    settings.shadows.pointRayShadowHiZEnabled ? 1 : 0);
            }

            int pointRayShadowMaxLights = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS",
                            pointRayShadowMaxLights)) {
                settings.shadows.pointRayShadowMaxLights =
                    static_cast<uint32_t>(
                        std::clamp(pointRayShadowMaxLights, 1, 2));
            }

            int pointRayShadowSteps = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_STEPS",
                            pointRayShadowSteps)) {
                settings.shadows.pointRayShadowSteps =
                    static_cast<uint32_t>(
                        std::clamp(pointRayShadowSteps, 4, 32));
            }

            int pointRayShadowHiZVisits = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_HIZ_VISITS",
                            pointRayShadowHiZVisits)) {
                settings.shadows.pointRayShadowHiZMaxVisits =
                    static_cast<uint32_t>(
                        std::clamp(pointRayShadowHiZVisits, 8, 64));
            }

            float pointRayShadowMaxDistance = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_MAX_DISTANCE",
                              pointRayShadowMaxDistance)) {
                settings.shadows.pointRayShadowMaxDistance =
                    std::clamp(pointRayShadowMaxDistance, 32.0f, 2400.0f);
            }

            float pointRayShadowThickness = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_THICKNESS",
                              pointRayShadowThickness)) {
                settings.shadows.pointRayShadowThickness =
                    std::clamp(pointRayShadowThickness, 1.0f, 160.0f);
            }

            float pointRayShadowStartOffset = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_START_OFFSET",
                              pointRayShadowStartOffset)) {
                settings.shadows.pointRayShadowStartOffset =
                    std::clamp(pointRayShadowStartOffset, 1.0f, 96.0f);
            }

            float pointRayShadowStrength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_STRENGTH",
                              pointRayShadowStrength)) {
                settings.shadows.pointRayShadowStrength =
                    std::clamp(pointRayShadowStrength, 0.0f, 1.0f);
            }

            int pointShadowMaxFaces = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW_MAX_FACES",
                            pointShadowMaxFaces)) {
                // 0 = 全更；1..6 = 每帧 face budget。
                pointShadowMaxFaces = std::clamp(pointShadowMaxFaces, 0, 6);
                settings.shadows.pointShadowMaxFacesPerFrame =
                    static_cast<uint32_t>(pointShadowMaxFaces);
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointShadowMaxFacesPerFrame=%d\n",
                    pointShadowMaxFaces);
            }

            int clearTestLights = 0;
            if (ParseEnvInt("DXVK_WAR3_TEST_POINT_LIGHT_CLEAR",
                            clearTestLights) &&
                clearTestLights != 0) {
                War3LightManager::Instance().ClearLights();
            }

            int testLight = 0;
            if (ParseEnvInt("DXVK_WAR3_TEST_POINT_LIGHT", testLight) &&
                testLight != 0) {
                float x = 0.0f;
                float y = 0.0f;
                float z = 420.0f;
                float range = 1800.0f;
                float r = 1.0f;
                float g = 0.86f;
                float b = 0.58f;
                float intensity = 2.4f;
                float shadowIntensity = 0.65f;
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_X", x);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_Y", y);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_Z", z);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_RANGE", range);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_R", r);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_G", g);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_B", b);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY", intensity);
                ParseEnvFloat("DXVK_WAR3_TEST_POINT_LIGHT_SHADOW",
                              shadowIntensity);
                if (!War3LightManager::Instance().HasActiveLights()) {
                    const int32_t id = War3LightManager::Instance().AddPointLight(
                        x, y, z, range, r, g, b, intensity, shadowIntensity);
                    WAR3_RENDER_LOG(
                        "DXVK War3PointLight: test light id=%d pos=(%.1f,%.1f,%.1f) range=%.1f color=(%.2f,%.2f,%.2f) intensity=%.2f shadow=%.2f\n",
                        id, static_cast<double>(x), static_cast<double>(y),
                        static_cast<double>(z), static_cast<double>(range),
                        static_cast<double>(r), static_cast<double>(g),
                        static_cast<double>(b), static_cast<double>(intensity),
                        static_cast<double>(shadowIntensity));
                }
                settings.shadows.pointLightsEnabled = true;
                if (shadowIntensity > 0.0f)
                    settings.shadows.pointShadowEnabled = true;
            }
        }

        // 体积光实验入口：默认关闭，只在显式环境变量打开时参与 BeforeUi。
        {
            int enabled = -1;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_LIGHT", enabled)) {
                settings.postFx.volumetricLight.enabled = enabled != 0;
                WAR3_RENDER_LOG("DXVK War3Volumetric: DXVK_WAR3_VOLUMETRIC_LIGHT=%d\n",
                               settings.postFx.volumetricLight.enabled ? 1 : 0);
            }

            float intensity = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_INTENSITY", intensity)) {
                settings.postFx.volumetricLight.intensity =
                    std::max(0.0f, intensity);
                WAR3_RENDER_LOG("DXVK War3Volumetric: intensity=%.3f\n",
                               static_cast<double>(
                                   settings.postFx.volumetricLight.intensity));
            }

            float decay = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_DECAY", decay)) {
                settings.postFx.volumetricLight.decay =
                    std::clamp(decay, 0.70f, 0.999f);
                WAR3_RENDER_LOG("DXVK War3Volumetric: decay=%.3f\n",
                               static_cast<double>(
                                   settings.postFx.volumetricLight.decay));
            }

            float density = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_DENSITY", density)) {
                settings.postFx.volumetricLight.density =
                    std::max(0.0f, density);
                WAR3_RENDER_LOG("DXVK War3Volumetric: density=%.3f\n",
                               static_cast<double>(
                                   settings.postFx.volumetricLight.density));
            }

            float weight = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_WEIGHT", weight)) {
                settings.postFx.volumetricLight.weight =
                    std::max(0.0f, weight);
                WAR3_RENDER_LOG("DXVK War3Volumetric: weight=%.3f\n",
                               static_cast<double>(
                                   settings.postFx.volumetricLight.weight));
            }

            float skyThreshold = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_SKY_THRESHOLD",
                              skyThreshold)) {
                settings.postFx.volumetricLight.skyThreshold =
                    std::clamp(skyThreshold, 0.55f, 0.99f);
                WAR3_RENDER_LOG("DXVK War3Volumetric: skyThreshold=%.3f\n",
                               static_cast<double>(settings.postFx
                                                       .volumetricLight
                                                       .skyThreshold));
            }

            int sampleCount = 0;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_SAMPLES", sampleCount)) {
                settings.postFx.volumetricLight.sampleCount =
                    std::clamp(sampleCount, 4, 16);
                WAR3_RENDER_LOG("DXVK War3Volumetric: samples=%d\n",
                               settings.postFx.volumetricLight.sampleCount);
            }

            int backend = -1;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_BACKEND", backend)) {
                backend = std::clamp(backend, 0, 2);
                settings.postFx.volumetricLight.quality =
                    static_cast<War3VolumetricQuality>(backend);
                WAR3_RENDER_LOG("DXVK War3Volumetric: backend=%d\n", backend);
            }

            float froxelFar = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_FROXEL_FAR",
                              froxelFar)) {
                settings.postFx.volumetricLight.froxelFar =
                    std::clamp(froxelFar, 1000.0f, 10000.0f);
                WAR3_RENDER_LOG("DXVK War3Volumetric: froxelFar=%.1f\n",
                               static_cast<double>(settings.postFx
                                                       .volumetricLight
                                                       .froxelFar));
            }

            int pointMaxLights = 0;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_POINT_MAX_LIGHTS",
                            pointMaxLights)) {
                settings.postFx.volumetricLight.maxPointLights =
                    static_cast<uint32_t>(std::clamp(pointMaxLights, 0, 2));
                WAR3_RENDER_LOG(
                    "DXVK War3Volumetric: pointMaxLights=%u\n",
                    static_cast<unsigned>(
                        settings.postFx.volumetricLight.maxPointLights));
            }

            int resDivisor = 0;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_RES_DIVISOR", resDivisor)) {
                settings.postFx.volumetricLight.resolutionDivisor =
                    static_cast<uint32_t>(std::clamp(resDivisor, 4, 8));
                WAR3_RENDER_LOG("DXVK War3Volumetric: resDivisor=%u\n",
                               static_cast<unsigned>(settings.postFx
                                                         .volumetricLight
                                                         .resolutionDivisor));
            }

            float fadeNear = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_FADE_NEAR", fadeNear)) {
                settings.postFx.volumetricLight.fadeNear =
                    std::clamp(fadeNear, 0.0f, 0.95f);
            }

            float fadeFar = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_FADE_FAR", fadeFar)) {
                const float nearValue =
                    settings.postFx.volumetricLight.fadeNear;
                settings.postFx.volumetricLight.fadeFar =
                    std::clamp(fadeFar, nearValue + 0.01f, 1.0f);
            }

            float maxRayDistance = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_MAX_RAY",
                              maxRayDistance)) {
                settings.postFx.volumetricLight.maxRayDistance =
                    std::max(0.05f, maxRayDistance);
            }

            float heightFogStrength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_HEIGHT_FOG",
                              heightFogStrength)) {
                settings.postFx.volumetricLight.heightFogStrength =
                    std::max(0.0f, heightFogStrength);
            }

            float extinctionStrength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_EXTINCTION",
                              extinctionStrength)) {
                settings.postFx.volumetricLight.extinctionStrength =
                    std::clamp(extinctionStrength, 0.0f, 1.0f);
            }

            float unshadowedScattering = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_UNSHADOWED",
                              unshadowedScattering)) {
                settings.postFx.volumetricLight.unshadowedScattering =
                    std::clamp(unshadowedScattering, 0.0f, 1.0f);
            }
        }
    }

    void War3SettingsFrameResolver::BeginFrame(War3RenderSettingsMailbox& mailbox,
                                               War3RenderSettings& settings) {
        m_authoredExposureChanged = false;
        {
            War3RenderSettingsMailbox* const mailboxPtr = &mailbox;
            std::lock_guard<std::mutex> lock(mailboxPtr->mutex);
            if (mailboxPtr->pendingRevision != mailboxPtr->appliedRevision) {
                settings = mailboxPtr->pending;
                mailboxPtr->appliedRevision = mailboxPtr->pendingRevision;
            }
            m_authoredExposureChanged =
                mailboxPtr->pendingExposureRevision !=
                mailboxPtr->appliedExposureRevision;
            mailboxPtr->appliedExposureRevision =
                mailboxPtr->pendingExposureRevision;
        }

        // 日夜色调在帧开始进行更新，保证主线程渲染状态可见且避免跨线程写入。

        auto& lightingClock = settings.dayNight;
        const auto lightingNow = std::chrono::steady_clock::now();
        if (m_lightingClockRevision != lightingClock.clockRevision) {
            m_lightingClockRevision = lightingClock.clockRevision;
            m_lightingClockTime01 =
                Wrap01(lightingClock.renderTimeHours / 24.0f);
            m_lightingClockLastUpdate = lightingNow;
        }

        if (lightingClock.clockMode == War3LightingClockMode::GameTime) {
            m_lightingClockTime01 = ResolvePipelineDayNightTime01(
                War3RenderState::GetGameTime(), lightingClock);
            m_lightingClockLastUpdate = lightingNow;
        } else if (lightingClock.clockMode ==
                   War3LightingClockMode::Independent) {
            float dt = std::chrono::duration<float>(
                lightingNow - m_lightingClockLastUpdate).count();
            m_lightingClockLastUpdate = lightingNow;
            if (!std::isfinite(dt) || dt < 0.0f)
                dt = 0.0f;
            dt = std::min(dt, 0.25f);
            const float dayLength = std::clamp(
                lightingClock.independentDayLengthSeconds,
                1.0f, 86400.0f);
            m_lightingClockTime01 = Wrap01(
                m_lightingClockTime01 + dt / dayLength);
        } else {
            m_lightingClockLastUpdate = lightingNow;
        }
        lightingClock.renderTimeHours = m_lightingClockTime01 * 24.0f;
    }

    void War3SettingsFrameResolver::FinishFrame(War3RenderSettingsMailbox& mailbox,
                                                War3RenderSettings& settings) {
        // 日夜色调：在帧开始应用到全局设置，保证主线程渲染状态与后处理能读取到
        const auto& dayNight = settings.dayNight;
        if (dayNight.enabled && dayNight.timeColorGradingEnabled) {
            float realGameTime = War3RenderState::GetGameTime();
            {
                // The sun's own time: a locked sun holds the ambient and the
                // exposure at its hour too (War3SunCycle::Resolve).
                float time01 = settings.shadows.lockSun
                    ? std::clamp(settings.shadows.lockSunTime, 0.0f, 1.0f)
                    : ResolvePipelineDayNightTime01(realGameTime, dayNight);
                float t = 0.0f;
                if (ComputeDayNightFactor(time01, dayNight, t)) {
                    if (dayNight.affectAmbient) {
                        if (!settings.ambient.overrideEnabled || settings.ambient.autoOverride) {
                            settings.ambient.overrideEnabled = true;
                            settings.ambient.autoOverride = true;
                            // The author's night and day colours, kept above 85% of
                            // the day's luminance, times the tint of the hour (the
                            // sky's blue, the purple light, the blue hour).
                            Vector4 ambient =
                                War3SunCycle::AmbientBlend(dayNight.nightAmbient, dayNight.dayAmbient, t) *
                                War3SunCycle::AmbientTint(settings.sunStyle, time01, t);
                            ambient.x = std::clamp(ambient.x, 0.0f, 1.0f);
                            ambient.y = std::clamp(ambient.y, 0.0f, 1.0f);
                            ambient.z = std::clamp(ambient.z, 0.0f, 1.0f);
                            ambient.w = 1.0f;
                            settings.ambient.color = ambient;
                        }
                    } else if (settings.ambient.autoOverride) {
                        settings.ambient.overrideEnabled = false;
                        settings.ambient.autoOverride = false;
                    }

                    if (dayNight.affectExposure) {
                        float autoExposure =
                            dayNight.nightExposure + (dayNight.dayExposure - dayNight.nightExposure) * t;
                        const float minExposure = dayNight.dayExposure * 0.85f;
                        if (autoExposure < minExposure)
                            autoExposure = minExposure;

                        // 若用户手动修改曝光，则自动关闭日夜曝光覆盖，避免“回弹到 1.0”
                        if (m_hasAutoExposure && m_authoredExposureChanged) {
                            settings.dayNight.affectExposure = false;
                            m_hasAutoExposure = false;
                            static bool s_logged = false;
                            if (!s_logged) {
                                s_logged = true;
                                WAR3_RENDER_LOG("DXVK War3Pipeline: 自动曝光被手动覆盖，已关闭日夜曝光\n");
                            }
                        } else {
                            settings.postFx.exposure = autoExposure;
                            m_lastAutoExposure = autoExposure;
                            m_hasAutoExposure = true;
                        }
                    } else {
                        m_hasAutoExposure = false;
                    }
                }
            }
        } else if (settings.ambient.autoOverride) {
            settings.ambient.overrideEnabled = false;
            settings.ambient.autoOverride = false;
            m_hasAutoExposure = false;
        }

        // Keep the next scoped authoring edit based on the latest resolved
        // state, but never overwrite a command that arrived while this frame
        // was being resolved.
        {
            War3RenderSettingsMailbox* const mailboxPtr = &mailbox;
            std::lock_guard<std::mutex> lock(mailboxPtr->mutex);
            if (mailboxPtr->pendingRevision == mailboxPtr->appliedRevision)
                mailboxPtr->pending = settings;
        }
    }

} // namespace dxvk
