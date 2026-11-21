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
#include "war3/tools/war3_frame_evidence.h"
#include "war3/platform/war3_vram_preset.h"
#include "war3/platform/war3_player_settings.h"
#include "war3/ui/war3_imgui.h"

#include "../util/util_env.h"
#include "../util/util_error.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
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

        bool ParseEnvFlagOrDefault(const char* name, bool defaultValue) {
            const std::string v = env::getEnvVar(name);
            if (v.empty())
                return defaultValue;
            return v == "1" || v == "true" || v == "TRUE" ||
                   v == "on" || v == "ON" || v == "yes" || v == "YES";
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
        // 显存档位只改默认值；下面的环境变量覆盖仍然优先。
        if (const auto* preset = dxvk::war3::platform::CurrentVramPreset()) {
            const uint32_t before = m_settings.shadows.csm.shadowResolution;
            m_settings.shadows.csm.shadowResolution =
                (std::min)(before, preset->csmResolutionCap);
            Logger::info(str::format("WarVK VRAM preset: tier=",
                dxvk::war3::gpureq::VramTierName(preset->tier), " source=", preset->source,
                " csmResolution ", before, " -> ",
                m_settings.shadows.csm.shadowResolution));
        }

        // 玩家在 Ctrl+F1 面板里改过并保存的设置（WarVK\settings.ini）。在显存档位
        // 之后应用（玩家明确选的值优先于自动档位），在下面的诊断环境变量之前应用。
        // 地图脚本之后仍可通过设置邮箱修改，那些修改不会写回文件。
        {
            war3::player::Extras extras;
            extras.fpsUnlocked = war3::War3Imgui::isFpsUnlocked();
            extras.nativeShadowMode = War3RenderState::GetNativeShadowMode();
            const auto report = war3::player::ApplyAtStartup(m_settings, extras);
            war3::War3Imgui::setFpsUnlocked(extras.fpsUnlocked);
            War3RenderState::SetNativeShadowMode(extras.nativeShadowMode);
            if (report.applied != 0u || report.unknown != 0u ||
                !report.rejected.empty()) {
                std::string rejected;
                for (const auto& key : report.rejected)
                    rejected += (rejected.empty() ? "" : ",") + key;
                Logger::info(str::format("WarVK player settings: applied=",
                    report.applied, " unknown=", report.unknown,
                    " rejected=", report.rejected.size(),
                    rejected.empty() ? "" : " [", rejected,
                    rejected.empty() ? "" : "]"));
            }
        }
        // 调试/调参环境变量（DXVK_WAR3_SHADOW_*、DXVK_WAR3_AA、点光、体积光）：
        // 与代理模式共用 war3/render/war3_settings_resolver.cpp 的同一份实现。
        ApplyWar3SettingsEnvironmentOverrides(m_settings);

        // External authoring never mutates m_settings directly. Seed the
        // serialized pending copy only after all constructor/env overrides
        // have been applied.
        m_settingsMailbox->pending = m_settings;
        m_settingsMailbox->pendingRevision = 1u;
        m_settingsMailbox->appliedRevision = 1u;
    }

    War3RenderPipeline::~War3RenderPipeline() = default;

    void War3RenderPipeline::CaptureWeatherSnapshot(War3PipelineInput& input) {
        if (m_weatherSnapshot.frameSerial != input.frameSerial ||
            m_weatherSnapshot.mapEpoch != input.mapEpoch ||
            m_weatherSnapshot.deviceEpoch != input.deviceEpoch) {
            auto& runtime = war3::render::WeatherRuntime::instance();
            m_weatherSnapshot = runtime.hasEnabled()
                ? runtime.snapshot(input.frameSerial, input.mapEpoch, input.deviceEpoch)
                : war3::render::WeatherFrameSnapshot{};
            m_weatherSnapshot.frameSerial = input.frameSerial;
            m_weatherSnapshot.mapEpoch = input.mapEpoch;
            m_weatherSnapshot.deviceEpoch = input.deviceEpoch;
        }
        input.weather = m_weatherSnapshot;
    }

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
        m_settingsResolver.BeginFrame(*m_settingsMailbox, m_settings);

        m_insertedBeforeUi = false;
        m_armedBeforeUi = false;
        m_hadWorldDraw = false;
        m_wantsBeforeUiInsertion = false;
        m_wantsShadowCapture = false;

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
                semanticShadowForcesBeforeUi ||
                war3::render::WeatherRuntime::instance().hasEnabled() ||
                // Preserve one real world-frame drain after authored weather
                // is disabled/destroyed, even when no other post-FX needs us.
                // CaptureWeatherSnapshot replaces this cached enabled value
                // with the new off snapshot; queued prior frames keep theirs.
                m_weatherSnapshot.enabled;
        }

        m_settingsResolver.FinishFrame(*m_settingsMailbox, m_settings);
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
        namespace ev = war3::tools::evidence;
        const ev::Key evidenceKey{uint64_t(reinterpret_cast<uintptr_t>(m_device.ptr())),
          input.frameSerial,input.mapEpoch,input.deviceEpoch};
        ev::Scope evidencePipeline(ev::Kind::PipelineBegin,evidenceKey,"PipelineExecute");
        if (unlikely(m_device == nullptr ||
                     m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST)) {
            // Device loss is irreversible. In particular, do not let optional
            // pass exception handling turn it into a per-pass disable while
            // later passes continue recording against the same lost device.
            return;
        }

        if (evidencePipeline.id()) {
            ev::Event event{};
            event.kind=ev::Kind::Camera; event.key=evidenceKey; event.parent=evidencePipeline.id();
            event.data[0]=input.scene.worldCamera.valid?1:0;
            event.data[1]=input.settingsRevision;
            event.data[2]=input.scene.shadowCasters.size();
            event.data[3]=input.scene.shadowFallbacks.size();
            event.data[4]=uint32_t(point);
            event.data[5]=input.frameIndex; // ring slot, never a frame number
            if (input.settings) {
                event.data[6]=uint32_t(input.settings->shadows.debugMode);
                event.data[7]=uint32_t(input.settings->shadows.shadowTaaMode);
                event.data[8]=input.settings->postFx.volumetricLight.enabled?1:0;
            }
            const Matrix4* matrices[]={&input.scene.worldCamera.view,&input.scene.worldCamera.proj,
                                      &input.scene.worldCamera.viewProj};
            for (uint32_t matrix=0;matrix<3;++matrix) for (uint32_t row=0;row<4;++row) {
                const float v[]={(*matrices[matrix])[row].x,(*matrices[matrix])[row].y,
                                 (*matrices[matrix])[row].z,(*matrices[matrix])[row].w};
                for (uint32_t col=0;col<4;++col)
                    std::memcpy(&event.bits[matrix*16+row*4+col],&v[col],sizeof(float));
            }
            ev::Record(evidencePipeline.session(),event);
            // Finalized caster inputs are copied as bounded metadata while the
            // frame owner is still valid. This is not a replay authorization.
            static const bool recordCasters = env::getEnvVar("DXVK_WAR3_FRAME_EVIDENCE_CASTERS") == "1";
            const size_t casterLimit = recordCasters
                ? (std::min)(input.scene.shadowCasters.size(), size_t(512)) : 0;
            for (size_t i=0; i<casterLimit; ++i) {
                const auto& draw=input.scene.shadowCasters[i];
                ev::Event caster{}; caster.kind=ev::Kind::CasterInput; caster.key=evidenceKey;
                caster.parent=evidencePipeline.id();
                const char* label="ShadowCasterInput";
                for (size_t n=0;n+1<caster.label.size() && label[n];++n) caster.label[n]=label[n];
                caster.data[0]=i; caster.data[1]=draw.rawcode; caster.data[2]=draw.jHandle;
                caster.data[3]=uint64_t(reinterpret_cast<uintptr_t>(draw.shadowRenderablePart));
                caster.data[4]=draw.shadowLayerIndex; caster.data[5]=draw.shadowMetadataKeyHash;
                caster.data[6]=draw.shadowExactGeometryKeyHash; caster.data[7]=draw.boundsSourceGeneration;
                caster.data[8]=draw.alphaMetadataFrameSerial; caster.data[9]=draw.alphaPayloadComplete?1:0;
                caster.data[10]=draw.mapEpoch; caster.data[11]=draw.deviceEpoch;
                caster.bits[0]=draw.indexCount; caster.bits[1]=draw.firstIndex;
                caster.bits[2]=uint32_t(draw.vertexOffset); // exact two's-complement int32 bits
                caster.bits[3]=draw.vertexCount; caster.bits[4]=draw.firstVertex; caster.bits[5]=draw.numVertices;
                caster.bits[6]=uint32_t(draw.alphaTestEnabled); caster.bits[7]=uint32_t(draw.alphaBlendEnabled);
                caster.bits[8]=draw.objectKind; caster.bits[9]=uint32_t(draw.stage<0?0xFFFFFFFFu:uint32_t(draw.stage));
                caster.bits[10]=draw.shadowActualIndexMin; caster.bits[11]=draw.shadowActualIndexMax;
                std::memcpy(&caster.bits[12],&draw.alphaRef,sizeof(float));
                std::memcpy(&caster.bits[13],&draw.boundsRadius,sizeof(float));
                for (uint32_t row=0;row<4;++row) for (uint32_t col=0;col<4;++col) {
                    const float v[]={draw.worldMatrix[row].x,draw.worldMatrix[row].y,
                                     draw.worldMatrix[row].z,draw.worldMatrix[row].w};
                    std::memcpy(&caster.bits[16+row*4+col],&v[col],sizeof(float));
                }
                auto casterId=ev::Record(evidencePipeline.session(),caster);
                ev::Event backing{}; backing.kind=ev::Kind::CasterBinding; backing.key=evidenceKey;
                backing.parent=casterId; backing.data={i,uint64_t(draw.positionInfo.buffer),
                  draw.positionInfo.offset,draw.positionInfo.size,uint64_t(draw.indexInfo.buffer),
                  draw.indexInfo.offset,draw.indexInfo.size,uint64_t(draw.uvInfo.buffer),
                  draw.uvInfo.offset,draw.uvInfo.size,uint64_t(draw.blendInfo.buffer),draw.blendInfo.offset};
                backing.bits[0]=draw.positionStride;backing.bits[1]=draw.positionOffset;
                backing.bits[2]=uint32_t(draw.positionFormat);backing.bits[3]=uint32_t(draw.indexType);
                backing.bits[4]=draw.uvStride;backing.bits[5]=draw.uvOffset;backing.bits[6]=uint32_t(draw.uvFormat);
                backing.bits[7]=draw.uvBinding;backing.bits[8]=draw.diffuseSamplerIndex;
                backing.bits[9]=draw.vertexBlendEnabled?1:0;backing.bits[10]=draw.paletteIndex;
                backing.bits[11]=draw.replayBindingsResolved?1:0;
                ev::Record(evidencePipeline.session(),backing);
            }
            if (input.scene.shadowCasters.size()>casterLimit) {
                ev::Event omitted{}; omitted.kind=ev::Kind::CasterOmitted; omitted.key=evidenceKey;
                omitted.parent=evidencePipeline.id(); omitted.data[0]=input.scene.shadowCasters.size()-casterLimit;
                omitted.data[1]=recordCasters?1:0; // 0=provider disabled, 1=bounded truncation
                ev::Record(evidencePipeline.session(),omitted);
            }
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
        if (input.nativeLightBaselines[0] && input.lighting)
            input.lighting->nativeColorExternalWriteHazard =
                war3shader::internal::HasNativeColorWriteListeners();
        const auto& frameGraphPlan = dxvk::war3::render::War3FrameGraphPlan::Default();
        if (hasListeners) {
            // Callback draw-call handles borrow this frame's geometry. Keep
            // slices until the exposed command buffer retires, even if CSM
            // or outlines are disabled. No authority for external async queues.
            if (ctx) for (const auto& draw : input.scene.shadowCasters)
                war3::memory::TrackSnapshotSlices(*ctx, draw);
            UpdateShaderApiFrameBuffers(input);
            auto contextScope = perf.cpuScope("UpdateRenderContext");
            war3shader::internal::ValidateWar3ShaderContextAbi(
                War3ShaderContextAbi { });
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
                ev::Scope evidencePass(ev::Kind::PassBegin,evidenceKey,entry.name.c_str(),evidencePipeline.id());
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
                    evidencePass.outcome(1); // CPU command recording returned
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
                ev::Scope evidencePack(ev::Kind::PassBegin,evidenceKey,"ShaderPack",evidencePipeline.id());
                try {
                    dxvk::war3::tools::SetGpuFlightBreadcrumb(
                        dxvk::war3::tools::GpuFlightBreadcrumb::ShaderPack);
                    auto packScope = perf.cpuScope("ShaderPack");
                    war3shader::internal::RunShaderPackPasses(ctx, input);
                    evidencePack.outcome(1);
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
        evidencePipeline.outcome(1);
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
