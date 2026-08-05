// war3_renderer.cpp - War3 渲染逻辑入口实现

#include <windows.h>

#include "war3_renderer.h"
#include "../model/war3_model_resource_cache.h"
#include "../model/war3_model_registry.h"
#include "../core/war3_events.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_runtime_profile.h"
#include "../core/war3_semantic_shadow_gate.h"
#include "../platform/war3_runtime_bootstrap.h"
#include "../shadow/war3_shadow_native_runtime.h"
#include "../shadow/war3_shadow_renderer_core.h"
#include "../shadow/war3_shadow_runtime_contract.h"
#include "../war3.h"
#include "war3_render_objects.h"
#include "war3_visible_renderables.h"
#include "war3_upper_layer_shadow.h"
#include "../render/war3_shadow_object_registry.h"
#include "war3_shadow_runtime_bridge.h"
#include "war3_scene_collector.h"
#include "../tools/war3_perf_monitor.h"

#include <chrono>

namespace dxvk::war3::render {

namespace {
thread_local void* s_tlsWorldObjectEntry = nullptr;
thread_local void* s_tlsSceneNode = nullptr;

bool IsSemanticDataModuleEnabled() {
    return runtime::IsWar3RuntimeModuleEnabled(
        runtime::War3RuntimeModule::SemanticData);
}

bool IsSemanticModelProducerEnabled() {
    return IsSemanticDataModuleEnabled() &&
           internal::kWar3RuntimeConfigSemanticModelProducerEffective;
}

bool AreSemanticFrameRegistriesEnabled() {
    return IsSemanticModelProducerEnabled() &&
           internal::kWar3RuntimeConfigSemanticFrameRegistriesEffective;
}

bool IsSemanticContractCaptureEnabled() {
    return IsSemanticModelProducerEnabled() &&
           internal::kWar3RuntimeConfigSemanticContractCaptureEffective;
}

bool IsSemanticConsumerEnabled() {
    return IsSemanticModelProducerEnabled() &&
           internal::kWar3RuntimeConfigSemanticConsumerEffective;
}

bool ShouldTrackRenderObjects() {
    return AreSemanticFrameRegistriesEnabled() ||
           runtime::IsAnyWar3RuntimeModuleEnabled({
               runtime::War3RuntimeModule::HookRender,
               runtime::War3RuntimeModule::RenderQueue,
               runtime::War3RuntimeModule::ShadowCapture,
               runtime::War3RuntimeModule::ShadowMap,
               runtime::War3RuntimeModule::ShadowReceiver,
           });
}

bool ShouldPublishVisibleRenderableRegistry() {
    return AreSemanticFrameRegistriesEnabled() &&
           runtime::IsWar3RuntimeModuleEnabled(
               runtime::War3RuntimeModule::HookRender) &&
           runtime::IsWar3RuntimeModuleEnabled(
               runtime::War3RuntimeModule::RenderQueue) &&
           internal::kNativeVisibleRenderableRegistryEnabled &&
           !internal::kWar3RuntimeConfigDisableSemanticVisibleRenderableWrites;
}

bool ShouldPublishUpperLayerShadowRegistry() {
    return IsSemanticConsumerEnabled() &&
           internal::kUpperLayerShadowConsumerEnabled;
}

bool ShouldPublishPoseRegistry() {
    return IsSemanticDataModuleEnabled() &&
           (internal::kWar3RuntimeConfigSemanticPoseProducerEffective ||
            internal::kWar3RuntimeConfigSemanticMatrixPublisherPoseEffective);
}

bool ShouldPublishAttachmentRigidRegistry() {
    return IsSemanticDataModuleEnabled() &&
           internal::kWar3RuntimeConfigSemanticAttachmentProducerEffective;
}

bool HasVisibleRenderablePublishWork() {
    if (!ShouldPublishVisibleRenderableRegistry())
        return false;
    const auto& visibleRegistry = VisibleRenderableRegistry::instance();
    return visibleRegistry.getVisibleCount() != 0 ||
           visibleRegistry.getMainQueueCount() != 0 ||
           visibleRegistry.getTransparentCount() != 0;
}

bool ShouldCaptureSemanticLiveState() {
    return IsSemanticContractCaptureEnabled() &&
           IsSemanticConsumerEnabled() &&
           (internal::IsSemanticShadowPreviewEnabled() ||
            internal::IsSemanticCoreValidationRuntimeEnabled() ||
            internal::IsSemanticSceneSubmissionRuntimeEnabled() ||
            internal::IsNativeRendererHostExecuteValidationRuntimeEnabled());
}

class SemanticPerfScope {
public:
    explicit SemanticPerfScope(SemanticDataPerfTag tag, bool active = true)
        : m_tag(tag), m_active(active),
          m_start(std::chrono::steady_clock::now()) {
    }

    ~SemanticPerfScope() {
        if (!m_active)
            return;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - m_start)
                .count();
        NoteSemanticDataPerf(m_tag,
                             elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0u);
    }

private:
    SemanticDataPerfTag m_tag;
    bool m_active = true;
    std::chrono::steady_clock::time_point m_start;
};
} // namespace

War3Renderer& War3Renderer::instance() {
    static War3Renderer* s_instance = new War3Renderer();
    return *s_instance;
}

void War3Renderer::BeginFrame() {
    ++m_rendererFrameSerial;
    m_semanticEndFrameBuildAttemptsThisFrame = 0;
    m_semanticEndFrameSawSkinnedThisFrame = false;
    const bool semanticData = AreSemanticFrameRegistriesEnabled();
    if (IsSemanticContractCaptureEnabled())
        shadow::ShadowRuntimeContractCache::instance().beginFrame();
    if (ShouldTrackRenderObjects())
        RenderObjectRegistry::instance().beginFrame();
    if (!semanticData)
        return;

    if (ShouldPublishVisibleRenderableRegistry())
        VisibleRenderableRegistry::instance().beginFrame();
    if (ShouldPublishUpperLayerShadowRegistry())
        UpperLayerShadowRegistry::instance().beginFrame();
    model::ShadowModelResourceCache::instance().beginFrame();
    model::ModelRegistry::instance().beginFrame();
    model::ModelInstanceRegistry::instance().beginFrame();
    if (ShouldPublishPoseRegistry())
        model::PoseRegistry::instance().beginFrame();
    if (ShouldPublishAttachmentRigidRegistry())
        model::AttachmentRigidRegistry::instance().beginFrame();
    ShadowObjectRegistry::instance().beginFrame();
}

void War3Renderer::PublishSemanticRegistriesForScene() {
    const bool semanticData = AreSemanticFrameRegistriesEnabled();
    SemanticPerfScope semanticPerf(
        SemanticDataPerfTag::FrameRegistryPublish, semanticData);
    if (ShouldTrackRenderObjects()) {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/RenderObject");
        RenderObjectRegistry::instance().endFrame();
    }
    if (!semanticData)
        return;

    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/Visible");
        if (ShouldPublishVisibleRenderableRegistry())
            VisibleRenderableRegistry::instance().endFrame();
    }
    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/UpperLayer");
        if (ShouldPublishUpperLayerShadowRegistry())
            UpperLayerShadowRegistry::instance().endFrame();
    }
    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/ModelResource");
        model::ShadowModelResourceCache::instance().endFrame();
    }
    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/Model");
        model::ModelRegistry::instance().endFrame();
    }
    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/ModelInstance");
        model::ModelInstanceRegistry::instance().endFrame();
    }
    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/Pose");
        if (ShouldPublishPoseRegistry())
            model::PoseRegistry::instance().endFrame();
    }
    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/Attachment");
        if (ShouldPublishAttachmentRigidRegistry())
            model::AttachmentRigidRegistry::instance().endFrame();
    }
    {
        auto scope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/Registries/ShadowObject");
        ShadowObjectRegistry::instance().endFrame();
    }
}

void War3Renderer::EndFrame() {
    const bool semanticData = AreSemanticFrameRegistriesEnabled();
    const bool renderTracking = ShouldTrackRenderObjects();
    if (!semanticData && !renderTracking)
      return;

    const bool semanticLiveState = semanticData && ShouldCaptureSemanticLiveState();
    if (semanticData && !semanticLiveState &&
        !HasVisibleRenderablePublishWork() &&
        !ShouldPublishUpperLayerShadowRegistry() &&
        !ShouldPublishPoseRegistry() &&
        !ShouldPublishAttachmentRigidRegistry()) {
      return;
    }

    // Even in semantic preview mode we should not republish/capture the same
    // frame-local registries over and over. The render thread can continue to
    // advance pending semantic builds from the last published contract, while
    // duplicate same-frame publish/capture just recreates the previous stall.
    constexpr bool allowMultiPublishThisFrame = false;
    if (!allowMultiPublishThisFrame && m_rendererFrameSerial != 0 &&
        m_lastSemanticRegistryPublishFrameSerial == m_rendererFrameSerial) {
      return;
    }

    {
      auto scope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Renderer/EndFrame/Registries");
      PublishSemanticRegistriesForScene();
    }
    if (!allowMultiPublishThisFrame)
      m_lastSemanticRegistryPublishFrameSerial = m_rendererFrameSerial;
    if (!semanticLiveState)
      return;

    const auto& visibleRegistry = VisibleRenderableRegistry::instance();
    const bool hasVisibleWork =
        visibleRegistry.getVisibleCount() != 0 ||
        visibleRegistry.getMainQueueCount() != 0 ||
        visibleRegistry.getTransparentCount() != 0;

    bool semanticSceneBuildRuntimeReady = false;
    bool semanticBuildAlreadyActiveAtEntry = false;
    if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled) {
      semanticSceneBuildRuntimeReady =
          dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled() &&
          dxvk::war3::internal::IsSemanticSceneEndFrameBuildRuntimeEnabled() &&
          (dxvk::war3::War3Events::get().isGameStarted() ||
           (dxvk::war3::War3Events::get().isJassReady() &&
            dxvk::war3::internal::
                IsSemanticShadowPreReadyValidationRuntimeEnabled()));
      if (semanticSceneBuildRuntimeReady) {
        const auto buildState =
            shadow::ShadowValidationRuntime::instance().buildStateSnapshot();
        semanticBuildAlreadyActiveAtEntry =
            buildState.buildInProgress || buildState.buildRequestPending;
      }
    }

    if (!hasVisibleWork && !semanticBuildAlreadyActiveAtEntry)
      return;
    constexpr uint32_t kSemanticEndFrameMaxBuildAttemptsPerFrame = 3u;
    if (!semanticBuildAlreadyActiveAtEntry &&
        m_semanticEndFrameBuildAttemptsThisFrame >=
            kSemanticEndFrameMaxBuildAttemptsPerFrame)
      return;
    if (hasVisibleWork)
      ++m_semanticEndFrameBuildAttemptsThisFrame;

    if (hasVisibleWork) {
      auto scope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Renderer/EndFrame/CaptureLiveState");
      shadow::ShadowRuntimeContractCache::instance().captureLiveState();
    }

    if constexpr (dxvk::war3::internal::kShadowSemanticCoreSceneSubmissionEnabled) {
      if (!dxvk::war3::internal::IsSemanticSceneSubmissionRuntimeEnabled())
        return;
      if (!semanticSceneBuildRuntimeReady)
        return;
      if (!dxvk::war3::internal::IsSemanticSceneEndFrameBuildRuntimeEnabled())
        return;

      auto scope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Renderer/EndFrame/EnsureSemanticFrame");
      auto& validationRuntime = shadow::ShadowValidationRuntime::instance();
      const auto preBuildState = validationRuntime.buildStateSnapshot();
      const bool semanticBuildAlreadyActive =
          preBuildState.buildInProgress || preBuildState.buildRequestPending;
      if (m_semanticEndFrameSawSkinnedThisFrame &&
          !semanticBuildAlreadyActive) {
        return;
      }
      validationRuntime.requestLatestFrameBuild();
      const auto postRequestBuildState =
          validationRuntime.buildStateSnapshot();
      const bool semanticBuildPendingAfterRequest =
          postRequestBuildState.buildInProgress ||
          postRequestBuildState.buildRequestPending;
      const bool needsBootstrapSemanticFrame =
          m_lastSemanticEndFrameFlushFrameSerial == 0u;

      // Consuming semantic build chunks on the render thread.
      // We process a limited number of chunks per frame to avoid stalling the game loop.
      // This allows the build to advance "in small steps" naturally, rather than forcing
      // a complete synchronous build which was causing pipe timeouts.
      constexpr uint32_t kRenderThreadMaxChunksWithVisibleWork = 2u;
      constexpr uint32_t kRenderThreadMaxChunksForPendingOnly = 2u;
      const uint32_t maxBuildChunksThisEndFrame =
          (semanticBuildAlreadyActiveAtEntry ||
           (needsBootstrapSemanticFrame && semanticBuildPendingAfterRequest))
              ? kRenderThreadMaxChunksForPendingOnly
              : kRenderThreadMaxChunksWithVisibleWork;
      for (uint32_t i = 0u; i < maxBuildChunksThisEndFrame; ++i) {
        const auto buildState = validationRuntime.buildStateSnapshot();
        if (!buildState.buildInProgress && !buildState.buildRequestPending)
          break;
        validationRuntime.runObserveValidation();
      }

      if constexpr (dxvk::war3::internal::
                        kShadowSemanticCoreSceneEndFrameFlushEnabled) {
        if (dxvk::war3::internal::IsSemanticSceneEndFrameFlushRuntimeEnabled()) {
          const bool shouldSkipEndFrameFlush =
              runtime::IsWar3RuntimeModuleEnabled(
                  runtime::War3RuntimeModule::HookRender) &&
              runtime::IsWar3RuntimeModuleEnabled(
                  runtime::War3RuntimeModule::RenderQueue) &&
              !dxvk::war3::internal::
                  IsNativeRendererHostExecuteValidationRuntimeEnabled();
          if (!shouldSkipEndFrameFlush) {
            const auto stats = validationRuntime.snapshot();
            const auto buildState = validationRuntime.buildStateSnapshot();
            if (stats.resolve.skinnedResolved != 0u &&
                !buildState.buildInProgress &&
                !buildState.buildRequestPending) {
              m_semanticEndFrameSawSkinnedThisFrame = true;
            }
            const bool semanticFrameAlreadyFlushed =
                stats.frameSerial != 0u &&
                stats.sourcePublishRevision != 0u &&
                stats.drawPacketCount != 0u &&
                !buildState.buildInProgress &&
                !buildState.buildRequestPending &&
                m_lastSemanticEndFrameFlushFrameSerial == stats.frameSerial &&
                m_lastSemanticEndFrameFlushPublishRevision ==
                    stats.sourcePublishRevision;
            if (!semanticFrameAlreadyFlushed) {
              auto flushScope = war3::War3PerfMonitor::instance().cpuScope(
                  "War3Renderer/EndFrame/SemanticShadowSceneFlush");
              const bool flushed =
                  dxvk::war3::ExecuteSemanticShadowSceneForValidation(
                      dxvk::war3::internal::kShadowSemanticCoreSceneUnitsOnly,
                      dxvk::war3::internal::
                          IsNativeRendererHostExecuteValidationRuntimeEnabled());
              if (flushed && stats.frameSerial != 0u &&
                  stats.sourcePublishRevision != 0u) {
                m_lastSemanticEndFrameFlushFrameSerial = stats.frameSerial;
                m_lastSemanticEndFrameFlushPublishRevision =
                    stats.sourcePublishRevision;
              }
            }
          }
        }
      }

      if constexpr (dxvk::war3::internal::kNativeRendererHostExecuteValidationEnabled &&
                    dxvk::war3::internal::
                        kNativeSemanticShadowWorldStageValidationEnabled) {
        if (!dxvk::war3::internal::
                IsNativeRendererHostExecuteValidationRuntimeEnabled() ||
            !dxvk::war3::internal::
                IsNativeSemanticShadowWorldStageValidationRuntimeEnabled())
          return;

        auto catchupScope = war3::War3PerfMonitor::instance().cpuScope(
            "War3Renderer/EndFrame/NativeShadowCatchupExecute");
        const bool nativePrepared =
            dxvk::war3::platform::DriveNativeShadowBackend(false, 0u);
        if (nativePrepared) {
          const auto nativeSummary =
              shadow::NativeD3D9BackendRuntime::instance().snapshot();
          const bool preparedFrameNotExecuted =
              nativeSummary.frameSerial != 0u &&
              nativeSummary.submittedDrawCount != 0u &&
              nativeSummary.frameSerial !=
                  nativeSummary.lastSuccessfulExecutedFrameSerial;
          if (preparedFrameNotExecuted)
            dxvk::war3::platform::ExecuteNativeShadowBackendPreparedFrame();
        }
      }
    }

    if constexpr (dxvk::war3::internal::kNativeRendererHostExecuteValidationEnabled &&
                  !dxvk::war3::internal::
                      kNativeSemanticShadowWorldStageValidationEnabled) {
      if (!dxvk::war3::internal::
              IsNativeRendererHostExecuteValidationRuntimeEnabled())
        return;

      auto scope = war3::War3PerfMonitor::instance().cpuScope(
          "War3Renderer/EndFrame/NativeShadowExecute");
      const bool nativePrepared = dxvk::war3::platform::DriveNativeShadowBackend();
      if (nativePrepared)
        dxvk::war3::platform::ExecuteNativeShadowBackendPreparedFrame();
    }
}

void War3Renderer::OnWorldObjectsGroup(void* worldPtr, int groupIdx) {
    if (!ShouldTrackRenderObjects())
        return;
    SceneCollector::CollectWorldObjects(worldPtr, groupIdx);
}

void War3Renderer::OnWorldObjectEntry(void* worldObjectEntry, void* sceneNode) {
    if (!ShouldTrackRenderObjects())
        return;
    RenderObjectRegistry::instance().mapSceneNode(worldObjectEntry, sceneNode);
}

void War3Renderer::OnVisibleRenderables(
    void* batchArray,
    uint32_t before,
    uint32_t after,
    const RenderObjectIdentitySnapshot& identity) {
    if (!ShouldPublishVisibleRenderableRegistry())
        return;
    VisibleRenderableRegistry::instance().registerMainQueueRange(
        batchArray, before, after, identity);
}

void War3Renderer::OnTransparentRenderable(
    void* payload,
    uint32_t transparentType,
    uint32_t queueSlot,
    uint32_t sortKey,
    float distanceSq,
    const RenderObjectIdentitySnapshot& identity) {
    if (!ShouldPublishVisibleRenderableRegistry())
        return;
    VisibleRenderableRegistry::instance().registerTransparentEntry(
        payload, transparentType, queueSlot, sortKey, distanceSq, identity);
}

void War3Renderer::SetCurrentWorldObjectContext(void* worldObjectEntry, void* sceneNode) {
    s_tlsWorldObjectEntry = worldObjectEntry;
    s_tlsSceneNode = sceneNode;
}

void War3Renderer::ClearCurrentWorldObjectContext() {
    s_tlsWorldObjectEntry = nullptr;
    s_tlsSceneNode = nullptr;
}

void* War3Renderer::GetCurrentWorldObjectEntry() const {
    return s_tlsWorldObjectEntry;
}

void* War3Renderer::GetCurrentSceneNode() const {
    return s_tlsSceneNode;
}

} // namespace dxvk::war3::render
