#define WAR3_SHADER_API_INTERNAL

#include "d3d9_war3_pipeline.h"
#include "war3_shader_api.h"
#include "war3_shaderpack.h"
#include "war3_shaderpack_internal.h"
#include "war3/render/war3_render_state.h"
#include "war3/render/war3_frame_graph.h"
#include "war3/core/war3_internal_test_config.h"
#include "war3/core/war3_runtime_profile.h"
#include "war3/core/war3_semantic_shadow_gate.h"
#include "d3d9_war3_light.h"
#include "d3d9_war3_shadow.h"
#include "d3d9_war3_debug.h"
#include "war3/tools/war3_perf_monitor.h"
#include "war3/tools/war3_diagnostics_hub.h"

#include "../util/util_env.h"
#include "../util/util_error.h"
#include "../util/log/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <exception>

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

        bool ParseEnvFlagOrDefault(const char* name, bool defaultValue) {
            const std::string v = env::getEnvVar(name);
            if (v.empty())
                return defaultValue;
            return v == "1" || v == "true" || v == "TRUE" ||
                   v == "on" || v == "ON" || v == "yes" || v == "YES";
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

        bool IsRuntimePipelinePassEnabled(const std::string& name) {
            using dxvk::war3::runtime::IsWar3RuntimeModuleEnabled;
            using dxvk::war3::runtime::War3RuntimeModule;
            if (name == "ShadowReceiver")
                return IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowReceiver);
            if (name == "SSAO")
                return IsWar3RuntimeModuleEnabled(War3RuntimeModule::Ssao);
            if (name == "AA")
                return IsWar3RuntimeModuleEnabled(War3RuntimeModule::Aa);
            return true;
        }

        war3shader::TextureFormat MapTextureFormat(VkFormat format) {
            switch (format) {
                case VK_FORMAT_R8G8B8A8_UNORM:
                    return war3shader::TextureFormat::R8G8B8A8_UNORM;
                case VK_FORMAT_R8G8B8A8_SRGB:
                    return war3shader::TextureFormat::R8G8B8A8_SRGB;
                case VK_FORMAT_B8G8R8A8_UNORM:
                    return war3shader::TextureFormat::B8G8R8A8_UNORM;
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                    return war3shader::TextureFormat::R16G16B16A16_FLOAT;
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                    return war3shader::TextureFormat::R32G32B32A32_FLOAT;
                case VK_FORMAT_D16_UNORM:
                    return war3shader::TextureFormat::D16_UNORM;
                case VK_FORMAT_D24_UNORM_S8_UINT:
                    return war3shader::TextureFormat::D24_UNORM_S8_UINT;
                case VK_FORMAT_D32_SFLOAT:
                    return war3shader::TextureFormat::D32_FLOAT;
                default:
                    return war3shader::TextureFormat::UNKNOWN;
            }
        }

        void UpdateShaderApiFrameBuffers(const War3PipelineInput& input) {
            const void* colorData = nullptr;
            uint32_t colorWidth = 0;
            uint32_t colorHeight = 0;
            war3shader::TextureFormat colorFormat = war3shader::TextureFormat::UNKNOWN;
            void* colorImage = nullptr;
            void* colorView = nullptr;
            uint32_t colorLayout = 0;

            const void* depthData = nullptr;
            uint32_t depthWidth = 0;
            uint32_t depthHeight = 0;
            war3shader::TextureFormat depthFormat = war3shader::TextureFormat::UNKNOWN;
            void* depthImage = nullptr;
            void* depthView = nullptr;
            uint32_t depthLayout = 0;

            if (input.colorView != nullptr) {
                const auto colorInfo = input.colorView->image()->info();
                colorData = input.colorView.ptr();
                colorWidth = static_cast<uint32_t>(colorInfo.extent.width);
                colorHeight = static_cast<uint32_t>(colorInfo.extent.height);
                colorFormat = MapTextureFormat(colorInfo.format);
                colorImage = reinterpret_cast<void*>(input.colorView->image()->handle());
                colorView = reinterpret_cast<void*>(input.colorView->handle());
                colorLayout = static_cast<uint32_t>(input.colorView->getLayout());
            }

            if (input.depthView != nullptr) {
                const auto depthInfo = input.depthView->image()->info();
                depthData = input.depthView.ptr();
                depthWidth = static_cast<uint32_t>(depthInfo.extent.width);
                depthHeight = static_cast<uint32_t>(depthInfo.extent.height);
                depthFormat = MapTextureFormat(depthInfo.format);
                depthImage = reinterpret_cast<void*>(input.depthView->image()->handle());
                depthView = reinterpret_cast<void*>(input.depthView->handle());
                depthLayout = static_cast<uint32_t>(input.depthView->getLayout());
            }

            war3shader::internal::UpdateFrameBuffers(
                colorData, colorWidth, colorHeight, colorFormat,
                depthData, depthWidth, depthHeight, depthFormat,
                colorImage, colorView, colorLayout,
                depthImage, depthView, depthLayout);
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

        float Luminance(const Vector4& color) {
            return color.x * 0.2126f + color.y * 0.7152f + color.z * 0.0722f;
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

        bool ShouldDispatchBeforeUiPreEvent(war3shader::RenderEventID eventId) {
            if constexpr (!dxvk::war3::internal::kNativeUseRenderSceneAsWorldEventAuthority)
                return true;

            switch (eventId) {
                case war3shader::RenderEventID::WORLD_RENDER_BEGIN:
                case war3shader::RenderEventID::WORLD_RENDER_END:
                case war3shader::RenderEventID::POST_PROCESS_BEGIN:
                    break;
                default:
                    return true;
            }

            const bool ready = War3RenderState::HasCompletedWorldRenderSceneThisFrame();
            if (!ready) {
                static uint32_t s_skipLogCount = 0;
                if (s_skipLogCount++ < 8) {
                    WAR3_RENDER_LOG(
                        "DXVK War3Pipeline: skip BeforeUi pre-event=%d (RenderScene not completed this frame)\n",
                        static_cast<int>(eventId));
                }
            }
            return ready;
        }
    }

    War3RenderPipeline::War3RenderPipeline(const Rc<DxvkDevice>& device)
        : m_device(device) {
// Note: War3RenderPipeline needs to be updated to accept/store D3D9DeviceEx*
    // For now, commenting out shadow pass registration - will fix in pipeline refactor
    // RegisterPass(std::make_unique<War3ShadowReceiverPass>(m_device));

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
                m_settings.shadows.debugMode = static_cast<War3ShadowDebugMode>(dbg);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_DEBUG=%d\n", dbg);
            }

            float strength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_STRENGTH", strength)) {
                m_settings.shadows.strength = std::clamp(strength, 0.0f, 1.0f);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_STRENGTH=%.3f\n",
                               static_cast<double>(m_settings.shadows.strength));
            }

            float bias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_BIAS", bias)) {
                m_settings.shadows.receiverBias = std::max(bias, 0.0f);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_BIAS=%.6f\n",
                               static_cast<double>(m_settings.shadows.receiverBias));
            }

            int receiverMode = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_RECEIVER_MODE", receiverMode)) {
                receiverMode = std::clamp(receiverMode, 0, 2);
                m_settings.shadows.receiverMode =
                    static_cast<War3ShadowReceiverMode>(receiverMode);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_RECEIVER_MODE=%d\n",
                    receiverMode);
            }

            float normalBiasScale = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_NORMAL_BIAS_SCALE",
                              normalBiasScale)) {
                m_settings.shadows.normalBiasScale =
                    std::max(normalBiasScale, 0.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_NORMAL_BIAS_SCALE=%.6f\n",
                    static_cast<double>(
                        m_settings.shadows.normalBiasScale));
            }

            float cascadeBlendRange = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_CASCADE_BLEND_RANGE",
                              cascadeBlendRange)) {
                m_settings.shadows.cascadeBlendRange =
                    std::max(cascadeBlendRange, 0.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_CASCADE_BLEND_RANGE=%.3f\n",
                    static_cast<double>(
                        m_settings.shadows.cascadeBlendRange));
            }

            int stableSnap = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_STABLE_SNAP", stableSnap)) {
                m_settings.shadows.csm.stableSnap =
                    stableSnap != 0 ? 1.0f : 0.0f;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_STABLE_SNAP=%d\n",
                    stableSnap != 0 ? 1 : 0);
            }

            float splitLambda = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_SPLIT_LAMBDA", splitLambda)) {
                m_settings.shadows.csm.splitLambda =
                    std::clamp(splitLambda, 0.0f, 1.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_SPLIT_LAMBDA=%.3f\n",
                    static_cast<double>(
                        m_settings.shadows.csm.splitLambda));
            }

            float pcf = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_PCF", pcf)) {
                m_settings.shadows.pcfRadius = std::max(pcf, 0.0f);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_PCF=%.3f\n",
                               static_cast<double>(m_settings.shadows.pcfRadius));
            }

            int cascades = 0;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_CASCADES", cascades)) {
                cascades = std::clamp(cascades, 1, 4);
                m_settings.shadows.csm.cascadeCount = static_cast<uint32_t>(cascades);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_CASCADES=%d\n",
                               cascades);
            }

            int shadowRes = 0;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_RES", shadowRes)) {
                shadowRes = std::clamp(shadowRes, 512, 4096);
                m_settings.shadows.csm.shadowResolution =
                    static_cast<uint32_t>(shadowRes);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_RES=%d\n",
                               shadowRes);
            }

            int pcfKernel = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_PCF_KERNEL", pcfKernel)) {
                pcfKernel = std::clamp(pcfKernel, 0, 3);
                m_settings.shadows.pcfKernel =
                    static_cast<War3ShadowPcfKernel>(pcfKernel);
                WAR3_RENDER_LOG("DXVK War3Shadow: DXVK_WAR3_SHADOW_PCF_KERNEL=%d\n",
                               pcfKernel);
            }

            War3ShadowTaaMode initialShadowTaaMode =
                m_settings.shadows.shadowTaaMode;
            if (ParseInitialShadowTaaMode(initialShadowTaaMode)) {
                m_settings.shadows.shadowTaaMode = initialShadowTaaMode;
                m_settings.shadows.shadowTaaEnabled =
                    initialShadowTaaMode == War3ShadowTaaMode::Temporal;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_TAA_MODE=%d\n",
                    static_cast<int>(initialShadowTaaMode));
            } else {
                // Compatibility fallback only. Parse it once during pipeline
                // construction so an in-game ImGui selection remains final.
                int shadowTaa = -1;
                if (ParseEnvInt("DXVK_WAR3_SHADOW_TAA", shadowTaa)) {
                    m_settings.shadows.shadowTaaEnabled = shadowTaa != 0;
                    m_settings.shadows.shadowTaaMode =
                        shadowTaa != 0
                            ? War3ShadowTaaMode::Temporal
                            : War3ShadowTaaMode::DirectInline;
                    WAR3_RENDER_LOG(
                        "DXVK War3Shadow: DXVK_WAR3_SHADOW_TAA=%d\n",
                        m_settings.shadows.shadowTaaEnabled ? 1 : 0);
                }
            }

            float shadowTaaWeight = 0.0f;
            if (ParseEnvFloat(
                    "DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT",
                    shadowTaaWeight)) {
                m_settings.shadows.shadowTaaBlendFactor =
                    std::clamp(shadowTaaWeight, 0.12f, 0.30f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: "
                    "DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT=%.3f\n",
                    static_cast<double>(
                        m_settings.shadows.shadowTaaBlendFactor));
            }

            int alphaHash = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_ALPHA_HASH", alphaHash)) {
                m_settings.shadows.alphaShadowHashed = alphaHash != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_ALPHA_HASH=%d\n",
                    m_settings.shadows.alphaShadowHashed ? 1 : 0);
            }

            int alphaMip = -1;
            if (ParseEnvInt("DXVK_WAR3_SHADOW_ALPHA_MIP", alphaMip)) {
                m_settings.shadows.alphaShadowUseMip = alphaMip != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_ALPHA_MIP=%d\n",
                    m_settings.shadows.alphaShadowUseMip ? 1 : 0);
            }

            float alphaMipBias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS",
                              alphaMipBias)) {
                m_settings.shadows.alphaShadowMipLodBias =
                    std::clamp(alphaMipBias, -4.0f, 4.0f);
                WAR3_RENDER_LOG(
                    "DXVK War3Shadow: DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS=%.3f\n",
                    static_cast<double>(
                        m_settings.shadows.alphaShadowMipLodBias));
            }
        }

        // 抗锯齿参数：可用环境变量快速切换 AA 模式
        // - DXVK_WAR3_AA=0..5: 0=None 1=FXAA 2=SMAA_Low 3=SMAA_Medium 4=SMAA_High 5=SMAA_Ultra
        {
            int aaMode = -1;
            if (ParseEnvInt("DXVK_WAR3_AA", aaMode)) {
                aaMode = std::clamp(aaMode, 0, 5);
                m_settings.postFx.aa.mode = static_cast<War3AAMode>(aaMode);
                WAR3_RENDER_LOG("DXVK War3AA: DXVK_WAR3_AA=%d\n", aaMode);
            }

            float fxaaSubpix = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_FXAA_SUBPIX", fxaaSubpix)) {
                m_settings.postFx.aa.fxaaQualitySubpix = std::clamp(fxaaSubpix, 0.0f, 1.0f);
                WAR3_RENDER_LOG("DXVK War3AA: DXVK_WAR3_FXAA_SUBPIX=%.3f\n",
                               static_cast<double>(m_settings.postFx.aa.fxaaQualitySubpix));
            }
        }

        // 点光源运行时入口：默认关闭；可由 JASS 命令或环境变量打开。
        {
            int enabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_LIGHTS", enabled)) {
                m_settings.shadows.pointLightsEnabled = enabled != 0;
                WAR3_RENDER_LOG("DXVK War3PointLight: DXVK_WAR3_POINT_LIGHTS=%d\n",
                               m_settings.shadows.pointLightsEnabled ? 1 : 0);
            }

            int shadowEnabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW", shadowEnabled)) {
                m_settings.shadows.pointShadowEnabled = shadowEnabled != 0;
                WAR3_RENDER_LOG("DXVK War3PointLight: DXVK_WAR3_POINT_SHADOW=%d\n",
                               m_settings.shadows.pointShadowEnabled ? 1 : 0);
            }

            int pointShadowMaxLights = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS",
                            pointShadowMaxLights)) {
                pointShadowMaxLights = std::clamp(pointShadowMaxLights, 1, 4);
                m_settings.shadows.pointShadowMaxLights =
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
                m_settings.shadows.pointShadowResolution =
                    static_cast<uint32_t>(pointShadowResolution);
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointShadowResolution=%d\n",
                    pointShadowResolution);
            }

            int pointShadowDebugLight = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW_DEBUG_LIGHT",
                            pointShadowDebugLight)) {
                pointShadowDebugLight = std::clamp(pointShadowDebugLight, 0, 3);
                m_settings.shadows.pointShadowDebugLightIndex =
                    static_cast<uint32_t>(pointShadowDebugLight);
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointShadowDebugLight=%d\n",
                    pointShadowDebugLight);
            }

            float pointShadowBias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_BIAS", pointShadowBias)) {
                m_settings.shadows.pointShadowBias = std::max(0.0f, pointShadowBias);
                WAR3_RENDER_LOG("DXVK War3PointLight: pointShadowBias=%.4f\n",
                               static_cast<double>(m_settings.shadows.pointShadowBias));
            }

            float pointShadowPcfNear = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_PCF_NEAR",
                              pointShadowPcfNear)) {
                m_settings.shadows.pointShadowPcfRadiusNear =
                    std::clamp(pointShadowPcfNear, 0.0f, 4.0f);
            }

            float pointShadowPcfFar = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_PCF_FAR",
                              pointShadowPcfFar)) {
                m_settings.shadows.pointShadowPcfRadiusFar =
                    std::clamp(pointShadowPcfFar,
                               m_settings.shadows.pointShadowPcfRadiusNear,
                               6.0f);
            }

            float pointShadowTexelBias = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_TEXEL_BIAS",
                              pointShadowTexelBias)) {
                m_settings.shadows.pointShadowTexelBiasScale =
                    std::clamp(pointShadowTexelBias, 0.0f, 1.0f);
            }

            float pointShadowRangeFade = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_SHADOW_RANGE_FADE",
                              pointShadowRangeFade)) {
                m_settings.shadows.pointShadowRangeFadeStart =
                    std::clamp(pointShadowRangeFade, 0.50f, 0.98f);
            }

            int pointRayShadowEnabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW",
                            pointRayShadowEnabled)) {
                m_settings.shadows.pointRayShadowEnabled =
                    pointRayShadowEnabled != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointRayShadowEnabled=%d\n",
                    m_settings.shadows.pointRayShadowEnabled ? 1 : 0);
            }

            int pointRayShadowHiZEnabled = -1;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_HIZ",
                            pointRayShadowHiZEnabled)) {
                m_settings.shadows.pointRayShadowHiZEnabled =
                    pointRayShadowHiZEnabled != 0;
                WAR3_RENDER_LOG(
                    "DXVK War3PointLight: pointRayShadowHiZEnabled=%d\n",
                    m_settings.shadows.pointRayShadowHiZEnabled ? 1 : 0);
            }

            int pointRayShadowMaxLights = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS",
                            pointRayShadowMaxLights)) {
                m_settings.shadows.pointRayShadowMaxLights =
                    static_cast<uint32_t>(
                        std::clamp(pointRayShadowMaxLights, 1, 2));
            }

            int pointRayShadowSteps = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_STEPS",
                            pointRayShadowSteps)) {
                m_settings.shadows.pointRayShadowSteps =
                    static_cast<uint32_t>(
                        std::clamp(pointRayShadowSteps, 4, 32));
            }

            int pointRayShadowHiZVisits = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_RAY_SHADOW_HIZ_VISITS",
                            pointRayShadowHiZVisits)) {
                m_settings.shadows.pointRayShadowHiZMaxVisits =
                    static_cast<uint32_t>(
                        std::clamp(pointRayShadowHiZVisits, 8, 64));
            }

            float pointRayShadowMaxDistance = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_MAX_DISTANCE",
                              pointRayShadowMaxDistance)) {
                m_settings.shadows.pointRayShadowMaxDistance =
                    std::clamp(pointRayShadowMaxDistance, 32.0f, 2400.0f);
            }

            float pointRayShadowThickness = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_THICKNESS",
                              pointRayShadowThickness)) {
                m_settings.shadows.pointRayShadowThickness =
                    std::clamp(pointRayShadowThickness, 1.0f, 160.0f);
            }

            float pointRayShadowStartOffset = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_START_OFFSET",
                              pointRayShadowStartOffset)) {
                m_settings.shadows.pointRayShadowStartOffset =
                    std::clamp(pointRayShadowStartOffset, 1.0f, 96.0f);
            }

            float pointRayShadowStrength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_POINT_RAY_SHADOW_STRENGTH",
                              pointRayShadowStrength)) {
                m_settings.shadows.pointRayShadowStrength =
                    std::clamp(pointRayShadowStrength, 0.0f, 1.0f);
            }

            int pointShadowMaxFaces = 0;
            if (ParseEnvInt("DXVK_WAR3_POINT_SHADOW_MAX_FACES",
                            pointShadowMaxFaces)) {
                // 0 = 全更；1..6 = 每帧 face budget。
                pointShadowMaxFaces = std::clamp(pointShadowMaxFaces, 0, 6);
                m_settings.shadows.pointShadowMaxFacesPerFrame =
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
                m_settings.shadows.pointLightsEnabled = true;
                if (shadowIntensity > 0.0f)
                    m_settings.shadows.pointShadowEnabled = true;
            }
        }

        // 体积光实验入口：默认关闭，只在显式环境变量打开时参与 BeforeUi。
        {
            int enabled = -1;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_LIGHT", enabled)) {
                m_settings.postFx.volumetricLight.enabled = enabled != 0;
                WAR3_RENDER_LOG("DXVK War3Volumetric: DXVK_WAR3_VOLUMETRIC_LIGHT=%d\n",
                               m_settings.postFx.volumetricLight.enabled ? 1 : 0);
            }

            float intensity = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_INTENSITY", intensity)) {
                m_settings.postFx.volumetricLight.intensity =
                    std::max(0.0f, intensity);
                WAR3_RENDER_LOG("DXVK War3Volumetric: intensity=%.3f\n",
                               static_cast<double>(
                                   m_settings.postFx.volumetricLight.intensity));
            }

            float decay = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_DECAY", decay)) {
                m_settings.postFx.volumetricLight.decay =
                    std::clamp(decay, 0.70f, 0.999f);
                WAR3_RENDER_LOG("DXVK War3Volumetric: decay=%.3f\n",
                               static_cast<double>(
                                   m_settings.postFx.volumetricLight.decay));
            }

            float density = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_DENSITY", density)) {
                m_settings.postFx.volumetricLight.density =
                    std::max(0.0f, density);
                WAR3_RENDER_LOG("DXVK War3Volumetric: density=%.3f\n",
                               static_cast<double>(
                                   m_settings.postFx.volumetricLight.density));
            }

            float weight = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_WEIGHT", weight)) {
                m_settings.postFx.volumetricLight.weight =
                    std::max(0.0f, weight);
                WAR3_RENDER_LOG("DXVK War3Volumetric: weight=%.3f\n",
                               static_cast<double>(
                                   m_settings.postFx.volumetricLight.weight));
            }

            float skyThreshold = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_SKY_THRESHOLD",
                              skyThreshold)) {
                m_settings.postFx.volumetricLight.skyThreshold =
                    std::clamp(skyThreshold, 0.55f, 0.99f);
                WAR3_RENDER_LOG("DXVK War3Volumetric: skyThreshold=%.3f\n",
                               static_cast<double>(m_settings.postFx
                                                       .volumetricLight
                                                       .skyThreshold));
            }

            int sampleCount = 0;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_SAMPLES", sampleCount)) {
                m_settings.postFx.volumetricLight.sampleCount =
                    std::clamp(sampleCount, 4, 16);
                WAR3_RENDER_LOG("DXVK War3Volumetric: samples=%d\n",
                               m_settings.postFx.volumetricLight.sampleCount);
            }

            int pointMaxLights = 0;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_POINT_MAX_LIGHTS",
                            pointMaxLights)) {
                m_settings.postFx.volumetricLight.maxPointLights =
                    static_cast<uint32_t>(std::clamp(pointMaxLights, 0, 2));
                WAR3_RENDER_LOG(
                    "DXVK War3Volumetric: pointMaxLights=%u\n",
                    static_cast<unsigned>(
                        m_settings.postFx.volumetricLight.maxPointLights));
            }

            int resDivisor = 0;
            if (ParseEnvInt("DXVK_WAR3_VOLUMETRIC_RES_DIVISOR", resDivisor)) {
                m_settings.postFx.volumetricLight.resolutionDivisor =
                    static_cast<uint32_t>(std::clamp(resDivisor, 4, 8));
                WAR3_RENDER_LOG("DXVK War3Volumetric: resDivisor=%u\n",
                               static_cast<unsigned>(m_settings.postFx
                                                         .volumetricLight
                                                         .resolutionDivisor));
            }

            float fadeNear = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_FADE_NEAR", fadeNear)) {
                m_settings.postFx.volumetricLight.fadeNear =
                    std::clamp(fadeNear, 0.0f, 0.95f);
            }

            float fadeFar = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_FADE_FAR", fadeFar)) {
                const float nearValue =
                    m_settings.postFx.volumetricLight.fadeNear;
                m_settings.postFx.volumetricLight.fadeFar =
                    std::clamp(fadeFar, nearValue + 0.01f, 1.0f);
            }

            float maxRayDistance = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_MAX_RAY",
                              maxRayDistance)) {
                m_settings.postFx.volumetricLight.maxRayDistance =
                    std::max(0.05f, maxRayDistance);
            }

            float heightFogStrength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_HEIGHT_FOG",
                              heightFogStrength)) {
                m_settings.postFx.volumetricLight.heightFogStrength =
                    std::max(0.0f, heightFogStrength);
            }

            float extinctionStrength = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_EXTINCTION",
                              extinctionStrength)) {
                m_settings.postFx.volumetricLight.extinctionStrength =
                    std::clamp(extinctionStrength, 0.0f, 1.0f);
            }

            float unshadowedScattering = 0.0f;
            if (ParseEnvFloat("DXVK_WAR3_VOLUMETRIC_UNSHADOWED",
                              unshadowedScattering)) {
                m_settings.postFx.volumetricLight.unshadowedScattering =
                    std::clamp(unshadowedScattering, 0.0f, 1.0f);
            }
        }

        // External authoring never mutates m_settings directly. Seed the
        // serialized pending copy only after all constructor/env overrides
        // have been applied.
        m_settingsMailbox->pending = m_settings;
        m_settingsMailbox->pendingRevision = 1u;
        m_settingsMailbox->appliedRevision = 1u;
    }

    War3RenderPipeline::~War3RenderPipeline() = default;

    void War3RenderPipeline::CaptureSettingsSnapshot(
            War3PipelineInput& input) const {
        const auto mailbox = m_settingsMailbox;
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        input.settings =
            std::make_shared<const War3RenderSettings>(m_settings);
        input.lighting = std::make_shared<War3FrameLightingState>();
        input.lighting->sunDirection = m_settings.sun.direction;
        input.lighting->sunColor = m_settings.sun.color;
        input.lighting->renderTimeHours =
            m_settings.dayNight.renderTimeHours;
        input.settingsMailbox = mailbox;
        input.settingsRevision = mailbox->appliedRevision;
    }

    War3RenderPipeline* CreateWar3RenderPipeline(
        const Rc<DxvkDevice>& device,
        War3RenderPipelineAbi) {
        return new War3RenderPipeline(device);
    }

    void DestroyWar3RenderPipeline(
        War3RenderPipeline* pipeline,
        War3RenderPipelineAbi) {
        delete pipeline;
    }

    void War3RenderPipeline::OnFrameStart() {
        bool authoredExposureChanged = false;
        {
            const auto mailbox = m_settingsMailbox;
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            if (mailbox->pendingRevision != mailbox->appliedRevision) {
                m_settings = mailbox->pending;
                mailbox->appliedRevision = mailbox->pendingRevision;
            }
            authoredExposureChanged =
                mailbox->pendingExposureRevision !=
                mailbox->appliedExposureRevision;
            mailbox->appliedExposureRevision =
                mailbox->pendingExposureRevision;
        }

        m_insertedBeforeUi = false;
        m_armedBeforeUi = false;
        m_hadWorldDraw = false;
        m_wantsBeforeUiInsertion = false;
        m_wantsShadowCapture = false;
        // 日夜色调在帧开始进行更新，保证主线程渲染状态可见且避免跨线程写入。

        auto& lightingClock = m_settings.dayNight;
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

        // ====================================================================
        // [性能] 关闭路径旁路判定
        // 说明：
        // - 即使所有效果都关闭，旧逻辑仍会每帧尝试插入 BeforeUi，从而触发
        //   beginExternalRendering + 大量 dirty 标记，导致 FPS 明显下降。
        // - 这里在帧开始计算一次“是否真的需要 BeforeUi/ShadowCapture”，供设备侧快速跳过。
        // - 目标：在“全部关闭”时尽可能接近原生渲染的 CPU/GPU 开销。
        // ====================================================================
        if constexpr (!dxvk::war3::internal::kWar3RenderModuleTakeoverEnabled) {
            // 诊断态：明确关闭我们自己的渲染接管链，只保留 Hook/桥接骨架做对照。
            m_wantsShadowCapture = false;
            m_wantsBeforeUiInsertion = false;
        } else {
            using dxvk::war3::runtime::IsWar3RuntimeModuleEnabled;
            using dxvk::war3::runtime::War3RuntimeModule;
            const bool moduleShadowCapture =
                IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowCapture);
            const bool moduleShadowMap =
                IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowMap);
            const bool moduleShadowReceiver =
                IsWar3RuntimeModuleEnabled(War3RuntimeModule::ShadowReceiver);
            const bool modulePostFx =
                IsWar3RuntimeModuleEnabled(War3RuntimeModule::PostFx);
            const bool moduleSemanticData =
                IsWar3RuntimeModuleEnabled(War3RuntimeModule::SemanticData);

            // ShaderPack 是否启用（需要 BeforeUi 执行）
            bool shaderPackEnabled = false;
            {
                war3shader::ShaderPackInfo info = {};
                war3shader::GetShaderPackInfo(&info);
                shaderPackEnabled = (info.flags & war3shader::PACK_FLAG_ENABLED) != 0;
            }
            shaderPackEnabled = shaderPackEnabled && modulePostFx;

            // ShadowReceiver/Outline 是否需要（需要 BeforeUi + ShadowCapture）
            bool wantsOutline =
                moduleShadowReceiver && m_settings.occludedOutline.enabled &&
                War3RenderState::HasOutlineHandles();
            const uint32_t nativeShadowMode =
                War3RenderState::GetNativeShadowMode();
            const bool disableWar3Shadow =
                dxvk::war3::internal::kNativeShadowDisableWar3ShadowReceiverWhenMode1 &&
                (nativeShadowMode >= 1u);
            const bool wantsShadows =
                moduleShadowMap && moduleShadowReceiver &&
                m_settings.shadows.enabled && !disableWar3Shadow;
            const bool wantsPointLights =
                moduleShadowReceiver && m_settings.shadows.pointLightsEnabled &&
                War3LightManager::Instance().HasActiveLights();
            const bool wantsPointShadow =
                wantsPointLights && moduleShadowMap &&
                m_settings.shadows.pointShadowEnabled &&
                m_settings.shadows.pointShadowMaxLights > 0;

            if (disableWar3Shadow && m_settings.shadows.enabled) {
                static uint32_t s_shadowDisableLog = 0;
                if (s_shadowDisableLog++ < 8) {
                    WAR3_RENDER_LOG(
                        "DXVK War3Shadow: disabled by NativeShadowMode=%u\n",
                        nativeShadowMode);
                }
            }

            if (disableWar3Shadow &&
                dxvk::war3::internal::kNativeShadowDisableOutlineWhenMode1 &&
                wantsOutline) {
                static uint32_t s_outlineDisableLog = 0;
                if (s_outlineDisableLog++ < 8) {
                    WAR3_RENDER_LOG(
                        "DXVK War3Shadow: outline disabled by NativeShadowMode=%u\n",
                        nativeShadowMode);
                }
                wantsOutline = false;
            }
            const bool wantsShadowReceiver =
                wantsShadows || wantsOutline || wantsPointLights;
            const bool wantsSemanticShadowScene =
                moduleSemanticData &&
                dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled();
            const bool semanticSceneDisablesLegacyCapture =
                wantsSemanticShadowScene &&
                dxvk::war3::internal::
                    IsSemanticSceneDisableLegacyShadowCaptureRuntimeEnabled();
            const bool effectiveModuleShadowCapture =
                moduleShadowCapture && !semanticSceneDisablesLegacyCapture;
            const bool semanticShadowForcesBeforeUi =
                wantsSemanticShadowScene &&
                ParseEnvFlagOrDefault(
                    "DXVK_WAR3_SEMANTIC_SHADOW_FORCE_BEFOREUI", true);

            // 后处理是否需要（需要 BeforeUi）
            const bool wantsPostFx =
                modulePostFx && m_settings.postFx.enabled &&
                !war3shader::internal::IsNativePostProcessDisabled();

            // Semantic preview should not change the frame graph by default:
            // it rides on frames where the normal receiver already runs. A
            // separate force flag exists only for narrow diagnostics.
            m_wantsShadowCapture =
                (effectiveModuleShadowCapture &&
                 (wantsShadows || wantsOutline || wantsPointShadow)) ||
                semanticShadowForcesBeforeUi;
            m_wantsBeforeUiInsertion =
                shaderPackEnabled || wantsShadowReceiver || wantsPostFx ||
                semanticShadowForcesBeforeUi;
        }

        // 日夜色调：在帧开始应用到全局设置，保证主线程渲染状态与后处理能读取到
        const auto& dayNight = m_settings.dayNight;
        if (dayNight.enabled && dayNight.timeColorGradingEnabled) {
            float realGameTime = War3RenderState::GetGameTime();
            {
                float time01 = ResolvePipelineDayNightTime01(
                    realGameTime, dayNight);
                float t = 0.0f;
                if (ComputeDayNightFactor(time01, dayNight, t)) {
                    if (dayNight.affectAmbient) {
                        if (!m_settings.ambient.overrideEnabled || m_settings.ambient.autoOverride) {
                            m_settings.ambient.overrideEnabled = true;
                            m_settings.ambient.autoOverride = true;
                            Vector4 ambient =
                                dayNight.nightAmbient * (1.0f - t) + dayNight.dayAmbient * t;
                            // 限制最低亮度为白天的 85%，避免夜晚过暗
                            const float minFactor = 0.85f;
                            const float dayLum = Luminance(dayNight.dayAmbient);
                            const float minLum = dayLum * minFactor;
                            const float curLum = Luminance(ambient);
                            if (curLum > 1e-4f && curLum < minLum) {
                                const float scale = minLum / curLum;
                                ambient.x *= scale;
                                ambient.y *= scale;
                                ambient.z *= scale;
                            }
                            ambient.w = 1.0f;
                            m_settings.ambient.color = ambient;
                        }
                    } else if (m_settings.ambient.autoOverride) {
                        m_settings.ambient.overrideEnabled = false;
                        m_settings.ambient.autoOverride = false;
                    }

                    if (dayNight.affectExposure) {
                        float autoExposure =
                            dayNight.nightExposure + (dayNight.dayExposure - dayNight.nightExposure) * t;
                        const float minExposure = dayNight.dayExposure * 0.85f;
                        if (autoExposure < minExposure)
                            autoExposure = minExposure;

                        // 若用户手动修改曝光，则自动关闭日夜曝光覆盖，避免“回弹到 1.0”
                        if (m_hasAutoExposure && authoredExposureChanged) {
                            m_settings.dayNight.affectExposure = false;
                            m_hasAutoExposure = false;
                            static bool s_logged = false;
                            if (!s_logged) {
                                s_logged = true;
                                WAR3_RENDER_LOG("DXVK War3Pipeline: 自动曝光被手动覆盖，已关闭日夜曝光\n");
                            }
                        } else {
                            m_settings.postFx.exposure = autoExposure;
                            m_lastAutoExposure = autoExposure;
                            m_hasAutoExposure = true;
                        }
                    } else {
                        m_hasAutoExposure = false;
                    }
                }
            }
        } else if (m_settings.ambient.autoOverride) {
            m_settings.ambient.overrideEnabled = false;
            m_settings.ambient.autoOverride = false;
            m_hasAutoExposure = false;
        }

        // Keep the next scoped authoring edit based on the latest resolved
        // state, but never overwrite a command that arrived while this frame
        // was being resolved.
        {
            const auto mailbox = m_settingsMailbox;
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            if (mailbox->pendingRevision == mailbox->appliedRevision)
                mailbox->pending = m_settings;
        }
    }

    bool War3RenderPipeline::NotifyDraw(War3RenderLayer layer,
                                        War3RenderState::StageCategory category,
                                        War3BatchTag batchTag,
                                        bool isUiBoundaryDraw) {
        // [性能] 当本帧不需要插入 BeforeUi 时，跳过全部分界检测逻辑（降低热路径开销）。
        if (!m_wantsBeforeUiInsertion)
            return false;

        const bool inWorldThisFrame = War3RenderState::HasWorldStageThisFrame();
        const int stage = War3RenderState::GetStage();
        const int dispStage = War3RenderState::GetDispatcherStage();

        const bool stateKnown =
            (category != War3RenderState::StageCategory::Unknown) ||
            (batchTag != War3BatchTag::Unknown);

        // Track whether we have seen any "real" world draws this frame.
        // We only want to inject shadows after the world has been rendered at least once.
        if (inWorldThisFrame && !isUiBoundaryDraw && stateKnown && layer != War3RenderLayer::UI) {
            m_hadWorldDraw = true;
            // If we armed too early (false-positive), but then see more world draws, cancel the arm.
            if (m_armedBeforeUi)
                m_armedBeforeUi = false;
        }

        const bool strongUiMarker =
            (layer == War3RenderLayer::UI) ||
            (category == War3RenderState::StageCategory::UI) ||
            (batchTag == War3BatchTag::UI) ||
            (dispStage == 67) ||
            War3RenderState::IsUiBatchStage();

        // Some paths emit a stable "unknown-signature" boundary (stage=-1, cat/tag unknown) right before UI begins.
        const bool unknownUiBoundary =
            isUiBoundaryDraw &&
            stage == -1 &&
            dispStage == -1 &&
            !stateKnown;

        if (!m_insertedBeforeUi && m_hadWorldDraw) {
            // Some machines expose the UI transition as an "unknown-signature"
            // boundary before the final world draws are truly done. Treat the
            // first unknown boundary as an arm signal and only commit after the
            // next matching boundary (or a later strong UI marker). Any real
            // world draw in between will clear m_armedBeforeUi above.
            if (unknownUiBoundary) {
                if (m_armedBeforeUi) {
                    m_insertedBeforeUi = true;
                    m_armedBeforeUi = false;

                    static uint32_t s_loggedCommitUnknown = 0;
                    if (s_loggedCommitUnknown < 8) {
                        s_loggedCommitUnknown++;
                        WAR3_RENDER_LOG(
                            "DXVK War3Pipeline: BeforeUi commit (unknown-sig armed) stage=%d disp=%d layer=%d cat=%d tag=%d\n",
                            stage,
                            dispStage,
                            static_cast<int>(layer),
                            static_cast<int>(category),
                            static_cast<int>(batchTag));
                    }
                    return true;
                }

                m_armedBeforeUi = true;

                static uint32_t s_loggedArmUnknown = 0;
                if (s_loggedArmUnknown < 8) {
                    s_loggedArmUnknown++;
                    WAR3_RENDER_LOG(
                        "DXVK War3Pipeline: BeforeUi armed (unknown-sig boundary) stage=%d disp=%d layer=%d cat=%d tag=%d\n",
                        stage,
                        dispStage,
                        static_cast<int>(layer),
                        static_cast<int>(category),
                        static_cast<int>(batchTag));
                }
                return false;
            }

            // Slow-path: if we did see a boundary candidate, arm and wait for a stronger UI marker.
            bool semanticTailBoundary = false;
            if constexpr (dxvk::war3::internal::
                              kShadowSemanticCoreSceneTailBoundaryFallbackEnabled) {
                semanticTailBoundary =
                    dxvk::war3::internal::
                        IsSemanticSceneTailBoundaryFallbackRuntimeEnabled() &&
                    isUiBoundaryDraw &&
                    (War3RenderState::HasMainWorldCompletedStageThisFrame(21) ||
                     War3RenderState::HasCompletedStageThisFrame(21) ||
                     War3RenderState::HasReachedStageThisFrame(21) ||
                     stage == 21 || dispStage == 21);
            }
            if (semanticTailBoundary && !strongUiMarker) {
                m_insertedBeforeUi = true;
                m_armedBeforeUi = false;

                static uint32_t s_loggedSemanticTailCommit = 0;
                if (s_loggedSemanticTailCommit < 8) {
                    s_loggedSemanticTailCommit++;
                    WAR3_RENDER_LOG(
                        "DXVK War3Pipeline: BeforeUi commit (semantic scene tail) stage=%d disp=%d layer=%d cat=%d tag=%d\n",
                        stage,
                        dispStage,
                        static_cast<int>(layer),
                        static_cast<int>(category),
                        static_cast<int>(batchTag));
                }
                return true;
            }

            // Semantic shadow has a stricter, explicit data path than the old
            // receiver/capture heuristic. Once the device-side boundary
            // classifier has accepted a non-unknown UI transition after the
            // world phase, commit it directly instead of waiting for a later
            // strong UI hook marker that may not exist on all launch paths.
            //
            // This is deliberately separate from the tail fallback above:
            // tail commits can happen on ambiguous stage21/unknown draws and
            // are useful diagnostics, but they are also the path that can
            // pollute the main image with a dark receiver overlay. The clean
            // weak commit still requires the normal device-side RT/DS/camera
            // and UI-boundary checks before NotifyDraw is called.
            static const bool s_semanticWeakBeforeUiCommit =
                ParseEnvFlagOrDefault(
                    "DXVK_WAR3_SEMANTIC_SHADOW_WEAK_BEFOREUI_COMMIT",
                    false);
            const bool semanticWeakBeforeUiCommit =
                s_semanticWeakBeforeUiCommit &&
                dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
                isUiBoundaryDraw &&
                !unknownUiBoundary &&
                !strongUiMarker &&
                (War3RenderState::HasReachedStageThisFrame(15) ||
                 War3RenderState::HasMainWorldCompletedStageThisFrame(21) ||
                 War3RenderState::HasCompletedStageThisFrame(21) ||
                 War3RenderState::HasReachedStageThisFrame(21) ||
                 stage == 15 ||
                 stage == 18 ||
                 dispStage == 67);
            if (semanticWeakBeforeUiCommit) {
                m_insertedBeforeUi = true;
                m_armedBeforeUi = false;

                static uint32_t s_loggedSemanticWeakCommit = 0;
                if (s_loggedSemanticWeakCommit < 8) {
                    s_loggedSemanticWeakCommit++;
                    WAR3_RENDER_LOG(
                        "DXVK War3Pipeline: BeforeUi commit (semantic weak boundary) stage=%d disp=%d layer=%d cat=%d tag=%d\n",
                        stage,
                        dispStage,
                        static_cast<int>(layer),
                        static_cast<int>(category),
                        static_cast<int>(batchTag));
                }
                return true;
            }

            if (m_armedBeforeUi) {
                if (strongUiMarker) {
                    m_insertedBeforeUi = true;
                    m_armedBeforeUi = false;

                    static uint32_t s_loggedCommit = 0;
                    if (s_loggedCommit < 8) {
                        s_loggedCommit++;
                        WAR3_RENDER_LOG(
                            "DXVK War3Pipeline: BeforeUi commit stage=%d disp=%d layer=%d cat=%d tag=%d\n",
                            stage,
                            dispStage,
                            static_cast<int>(layer),
                            static_cast<int>(category),
                            static_cast<int>(batchTag));
                    }
                    return true;
                }
            } else if (isUiBoundaryDraw) {
                m_armedBeforeUi = true;
                static uint32_t s_loggedArm = 0;
                if (s_loggedArm < 8) {
                    s_loggedArm++;
                    WAR3_RENDER_LOG(
                        "DXVK War3Pipeline: BeforeUi armed stage=%d disp=%d layer=%d cat=%d tag=%d\n",
                        stage,
                        dispStage,
                        static_cast<int>(layer),
                        static_cast<int>(category),
                        static_cast<int>(batchTag));
                }
            }
        }

        return false;
    }

    bool War3RenderPipeline::ForceBeforeUiInsertion() {
        if (!m_wantsBeforeUiInsertion || m_insertedBeforeUi || !m_hadWorldDraw)
            return false;

        m_insertedBeforeUi = true;
        m_armedBeforeUi = false;

        static uint32_t s_loggedForce = 0;
        if (s_loggedForce < 8) {
            s_loggedForce++;
            WAR3_RENDER_LOG(
                "DXVK War3Pipeline: BeforeUi commit (frame-end fallback)\n");
        }
        return true;
    }


void War3RenderPipeline::Execute(War3InsertionPoint point,
                                     const Rc<DxvkCommandList>& ctx,
                                     const War3PipelineInput& input) {
        if (unlikely(m_device == nullptr ||
                     m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)) {
            // Device loss is irreversible. In particular, do not let optional
            // pass exception handling turn it into a per-pass disable while
            // later passes continue recording against the same lost device.
            return;
        }

        if (point == War3InsertionPoint::BeforeUi) {
            dxvk::war3::tools::SetGpuFlightBreadcrumb(
                dxvk::war3::tools::GpuFlightBreadcrumb::PipelineBeforeUi);
        }
        auto& perf = war3::War3PerfMonitor::instance();
        const char* rootScope = (point == War3InsertionPoint::BeforeUi) ? "Draw" : "Present";
        auto rootPerfScope = perf.cpuScope(rootScope);
        auto runPerfScope = perf.cpuScope("Run");

        if (ctx) {
            war3shader::internal::SetVulkanCommandBuffer(
                reinterpret_cast<void*>(ctx->getExecCommandBuffer()));
        }
        const bool hasListeners = war3shader::internal::HasAnyRenderListeners();
        const auto& frameGraphPlan = dxvk::war3::render::War3FrameGraphPlan::Default();
        if (hasListeners) {
            UpdateShaderApiFrameBuffers(input);
            auto contextScope = perf.cpuScope("UpdateRenderContext");
            war3shader::internal::UpdateRenderContext(input);

            if (point == War3InsertionPoint::BeforeUi) {
                const auto& preEvents = frameGraphPlan.Events(
                    dxvk::war3::render::FrameGraphDispatchStage::BeforeUiPrePass);
                for (auto eventId : preEvents) {
                    if (!ShouldDispatchBeforeUiPreEvent(eventId))
                        continue;
                    war3shader::internal::DispatchRenderEvent(eventId);
                }
            }
        }

        for (auto& entry : m_passes) {
            if (!entry.enabled || !entry.pass)
                continue;
            if (!IsRuntimePipelinePassEnabled(entry.name))
                continue;
            if (entry.pass->Point() == point) {
                try {
                    if (entry.name == "ShadowReceiver") {
                        dxvk::war3::tools::SetGpuFlightBreadcrumb(
                            dxvk::war3::tools::GpuFlightBreadcrumb::ShadowReceiverEntry);
                    } else if (entry.name == "VolumetricLight") {
                        dxvk::war3::tools::SetGpuFlightBreadcrumb(
                            dxvk::war3::tools::GpuFlightBreadcrumb::VolumetricLight);
                    } else if (entry.name == "SSAO") {
                        dxvk::war3::tools::SetGpuFlightBreadcrumb(
                            dxvk::war3::tools::GpuFlightBreadcrumb::Ssao);
                    } else if (entry.name == "AA") {
                        dxvk::war3::tools::SetGpuFlightBreadcrumb(
                            dxvk::war3::tools::GpuFlightBreadcrumb::Aa);
                    }
                    auto passScope = perf.cpuScope(entry.name.c_str());
                    entry.pass->Run(ctx, input);
                } catch (const dxvk::DxvkError& e) {
                    entry.enabled = false;
                    Logger::err(dxvk::str::format(
                        "War3Pipeline: Pass crashed, disabled: ", entry.name, " err=", e.message()));
                } catch (const std::exception& e) {
                    entry.enabled = false;
                    Logger::err(dxvk::str::format(
                        "War3Pipeline: Pass crashed, disabled: ", entry.name, " err=", e.what()));
                } catch (...) {
                    entry.enabled = false;
                    Logger::err(dxvk::str::format(
                        "War3Pipeline: Pass crashed, disabled: ", entry.name, " err=unknown"));
                }
            }
        }

        if (point == War3InsertionPoint::BeforeUi) {
            // [调试开关] 通过环境变量 DXVK_WAR3_DISABLE_SHADERPACK=1 可禁用 ShaderPack
            static bool s_disableShaderPack = false;
            static bool s_checkedEnv = false;
            if (!s_checkedEnv) {
                s_checkedEnv = true;
                int disable = 0;
                if (ParseEnvInt("DXVK_WAR3_DISABLE_SHADERPACK", disable) && disable != 0) {
                    s_disableShaderPack = true;
                    WAR3_RENDER_LOG("DXVK War3Pipeline: ShaderPack DISABLED by env var\n");
                }
            }
            if (!s_disableShaderPack &&
                dxvk::war3::runtime::IsWar3RuntimeModuleEnabled(
                    dxvk::war3::runtime::War3RuntimeModule::PostFx)) {
                try {
                    dxvk::war3::tools::SetGpuFlightBreadcrumb(
                        dxvk::war3::tools::GpuFlightBreadcrumb::ShaderPack);
                    auto packScope = perf.cpuScope("ShaderPack");
                    war3shader::internal::RunShaderPackPasses(ctx, input);
                } catch (const dxvk::DxvkError& e) {
                    war3shader::EnableShaderPack(false);
                    Logger::err(dxvk::str::format("ShaderPack: 异常中止，已禁用: ", e.message()));
                } catch (const std::exception& e) {
                    war3shader::EnableShaderPack(false);
                    Logger::err(dxvk::str::format("ShaderPack: 异常中止，已禁用: ", e.what()));
                } catch (...) {
                    war3shader::EnableShaderPack(false);
                    Logger::err("ShaderPack: 异常中止，已禁用: unknown");
                }

                war3shader::ShaderPackInfo info = {};
                if (war3shader::GetShaderPackInfo(&info) == war3shader::ShaderPackError::OK) {
                    if ((info.flags & war3shader::PACK_FLAG_HAS_ERROR) != 0) {
                        war3shader::EnableShaderPack(false);
                        static bool s_loggedPackError = false;
                        if (!s_loggedPackError) {
                            s_loggedPackError = true;
                            Logger::err(dxvk::str::format(
                                "ShaderPack: 检测到错误，已回退到内置渲染 pack=", info.name));
                        }
                    }
                }
            }
        }

        if (hasListeners) {
            if (point == War3InsertionPoint::BeforeUi) {
                dxvk::war3::tools::SetGpuFlightBreadcrumb(
                    dxvk::war3::tools::GpuFlightBreadcrumb::BeforeUiPostEvents);
                const auto& postEvents = frameGraphPlan.Events(
                    dxvk::war3::render::FrameGraphDispatchStage::BeforeUiPostPass);
                for (auto eventId : postEvents) {
                    war3shader::internal::DispatchRenderEvent(eventId);
                }
            } else if (point == War3InsertionPoint::BeforePresent) {
                const auto& presentEvents = frameGraphPlan.Events(
                    dxvk::war3::render::FrameGraphDispatchStage::BeforePresentPostPass);
                for (auto eventId : presentEvents) {
                    war3shader::internal::DispatchRenderEvent(eventId);
                }
            }
        }

        if (point == War3InsertionPoint::BeforeUi) {
            dxvk::war3::tools::SetGpuFlightBreadcrumb(
                dxvk::war3::tools::GpuFlightBreadcrumb::BeforeUiComplete);
        }

        // 低频运行时健康日志（默认每 1200 帧）。
        dxvk::war3::tools::LogRuntimeHealthPeriodic(input.frameSerial);
    }

    void War3RenderPipeline::RegisterPass(const char* name, std::unique_ptr<War3RenderPass> pass, bool enabled) {
        if (!pass || !name || !name[0])
            return;

        for (const auto& entry : m_passes) {
            if (entry.name == name)
                return;
        }

        PassEntry entry = { };
        entry.name = name;
        entry.enabled = enabled;
        entry.pass = std::move(pass);
        m_passes.emplace_back(std::move(entry));
    }

    bool War3RenderPipeline::SetPassEnabled(const char* name, bool enabled) {
        if (!name || !name[0])
            return false;
        for (auto& entry : m_passes) {
            if (entry.name == name) {
                entry.enabled = enabled;
                return true;
            }
        }
        return false;
    }

    bool War3RenderPipeline::IsPassEnabled(const char* name) const {
        if (!name || !name[0])
            return false;
        for (const auto& entry : m_passes) {
            if (entry.name == name)
                return entry.enabled;
        }
        return false;
    }

} // namespace dxvk
